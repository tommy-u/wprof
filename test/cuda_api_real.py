#!/usr/bin/env python3
"""Verify typed CUDA UTRACE calls and preserve repeated overhead/attach measurements."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import random
import signal
import statistics
import subprocess
import time

from cuda_api_check import check, has_recorded_loss
from cuda_api_manifest import build_manifest, compiled_evidence
from utrace_ready import start_wprof, wait_wprof_ready

ROOT = Path(__file__).resolve().parents[1]
CASES = ('baseline', 'plain', 'stacks', 'sealed')


def execute(binary, wprof, output, evidence, stacks=False, sealed=False, baseline=False, coverage=False, iterations=100):
    workload_log = output / 'workload.jsonl'
    stdout_file = workload_log.open('w')
    stderr_file = (output / 'workload.stderr').open('w')
    tracee = subprocess.Popen([str(binary), *([] if coverage else [str(iterations)])], stdin=subprocess.PIPE, stdout=stdout_file,
                              stderr=stderr_file, text=True, cwd=output)
    profiler, stream = None, None
    try:
        deadline = time.monotonic() + 60
        while '"ready":true' not in workload_log.read_text():
            assert tracee.poll() is None, (output / 'workload.stderr').read_text()
            assert time.monotonic() < deadline, 'workload preparation deadline'
            time.sleep(0.02)
        data, log = output / 'capture.data', output / 'capture.log'
        if not baseline:
            options = ['--stacks=utrace'] if stacks else ['--no-stacks']
            if sealed:
                options += ['--seal=uprobe-args']
            prefix = ['sudo', '-n', 'prlimit', '--nofile=1024:1048576', '--']
            command = ['-f', 'cuda-api=' + str(tracee.pid), '-f', 'no-sched',
                       '-f', 'no-wakeup', '-f', 'no-task-life', '-d10s', '-D', str(data), *options]
            stream = log.open('w')
            profiler = start_wprof(wprof, command, output, prefix=prefix, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
            wait_wprof_ready(profiler, output, timeout=60)
        tracee.stdin.write('\n')
        tracee.stdin.flush()
        deadline = time.monotonic() + 60
        while '"elapsed_ns"' not in workload_log.read_text():
            assert tracee.poll() is None, (output / 'workload.stderr').read_text() + workload_log.read_text()
            assert time.monotonic() < deadline, 'workload completion deadline'
            time.sleep(0.02)
        calls = [json.loads(line) for line in workload_log.read_text().splitlines() if '"ready"' not in line]
        measured = calls[-1]
        calls = [r for r in calls if r.get('pid', tracee.pid) == tracee.pid]
        assert measured['failures'] == 0 and measured['elapsed_ns'] > 0, measured
        assert measured['iterations'] == (1 if coverage else iterations), measured
        manifest = build_manifest(calls, evidence, caller='main' if coverage else 'coverage_run',
                                  iterations=measured['iterations'])
        manifest_path = output / 'expected-manifest.json'
        manifest_path.write_text(json.dumps(manifest, indent=2))
        measured.update({'manifest': str(manifest_path), 'pass': True})
        if not baseline:
            os.killpg(profiler.pid, signal.SIGINT)
            assert profiler.wait(timeout=60) == 0, log.read_text()
            stream.close()
        # Keep /proc mappings available until wprof finishes symbolizing the original callers.
        tracee.stdin.write('\n')
        tracee.stdin.flush()
        assert tracee.wait(timeout=60) == 0, (output / 'workload.stderr').read_text() + workload_log.read_text()
        stdout_file.close()
        stderr_file.close()
        if baseline:
            return measured
        subprocess.run(['sudo', '-n', 'chown', str(os.getuid()) + ':' + str(os.getgid()), str(data)], check=True)
        ndjson = output / 'capture.json'
        replay = subprocess.run([str(wprof), '-R', '-D', str(data), '-J', str(ndjson)], check=True,
                                text=True, capture_output=True)
        (output / 'replay.log').write_text(replay.stdout + replay.stderr)
        info = subprocess.run([str(wprof), '-RI', '--stats', '-D', str(data)], check=True, text=True, capture_output=True)
        info_text = info.stdout + info.stderr
        (output / 'capture.info').write_text(info_text)
        rows = [json.loads(line) for line in ndjson.read_text().splitlines()]
        metadata = rows[0].get('metadata', {})
        events = [r for r in rows if r.get('t', '').startswith('utrace_')]
        loss = has_recorded_loss(info_text + replay.stdout + replay.stderr)
        verification = check(ndjson, tracee.pid, manifest, status=not sealed, stacks=stacks, recorded_loss=loss)
        measured.update({'events': len(events), 'observed_names': len(set(r['name'] for r in events)),
                         'verified_names': [call['name'] for call in verification['expected']],
                         'compiler_wrappers': evidence['compiler_wrappers'],
                         'inline_helpers': sorted(evidence['inline_algorithms']), 'metadata': metadata,
                         'attach_ns': int(metadata['cuda-api.utrace_attach_ns']),
                         'data': str(data), 'recorded_loss': loss, 'verification': verification,
                         'pass': verification['pass'],
                         'scope': 'typed scenarios in this workload; other catalog entries are not claimed exercised'})
        return measured
    finally:
        if tracee.poll() is None:
            tracee.kill()
            tracee.wait()
        if profiler and profiler.poll() is None:
            os.killpg(profiler.pid, signal.SIGINT)
            profiler.wait(timeout=60)
        if stream:
            stream.close()
        stdout_file.close()
        stderr_file.close()
        tracee.stdin.close()


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError('must be a positive integer')
    return number


def case_schedule(repeat, seed):
    rng = random.Random(seed)
    schedule = []
    for phase, rounds in (('warmup', 1), ('sample', repeat)):
        for index in range(rounds):
            order = list(CASES)
            rng.shuffle(order)
            schedule.extend((phase, index + 1, case) for case in order)
    return schedule


def distribution(values):
    median = statistics.median(values)
    return median, {'min': min(values), 'max': max(values),
                    'mad': statistics.median(abs(value - median) for value in values),
                    'stdev': statistics.pstdev(values)}


def summarize_samples(results):
    summary = {}
    baseline = results['baseline']
    baseline_median = statistics.median(sample['elapsed_ns'] for sample in baseline)
    for case, samples in results.items():
        median, dispersion = distribution([sample['elapsed_ns'] for sample in samples])
        ratios = [sample['elapsed_ns'] / base['elapsed_ns'] for sample, base in zip(samples, baseline)]
        ratio_median, ratio_dispersion = distribution(ratios)
        for sample, ratio in zip(samples, ratios):
            sample['elapsed_ratio'] = ratio
        summary[case] = {'samples': samples, 'median_elapsed_ns': median, 'dispersion_ns': dispersion,
                         'elapsed_ratio': median / baseline_median, 'ratio_samples': ratios,
                         'median_elapsed_ratio': ratio_median, 'ratio_dispersion': ratio_dispersion,
                         'pass': all(sample['pass'] for sample in samples)}
        if case != 'baseline':
            attach_median, attach_dispersion = distribution([sample['attach_ns'] for sample in samples])
            summary[case].update({'median_attach_ns': attach_median, 'attach_dispersion_ns': attach_dispersion})
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wprof', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True, help='CUDA SDK root with include/ and lib/')
    parser.add_argument('--nvcc', type=Path, required=True, help='CUDA compiler executable')
    parser.add_argument('--gpu-lock', type=Path, required=True, help='Shared GPU coordination lock file')
    parser.add_argument('--coverage-dir', type=Path, help='Use an existing api-coverage demo.cu and its includes')
    parser.add_argument('--repeat', type=positive_int, default=5, help='Measured samples per case (default: 5)')
    parser.add_argument('--iterations', type=positive_int, default=100,
                        help='Iterations per native workload sample (default: 100; coverage demo uses one pass)')
    parser.add_argument('--seed', type=int, default=0, help='Seed for randomized case order (default: 0)')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    modes = [('default', []), ('per-thread', ['--default-stream', 'per-thread', '-DCUDA_API_PER_THREAD_DEFAULT_STREAM'])]
    summary = {'version': 1, 'configuration': {'repeat': args.repeat, 'iterations': args.iterations,
               'coverage_iterations': 1 if args.coverage_dir else None, 'seed': args.seed,
               'wprof': str(args.wprof.resolve()), 'sdk': str(args.sdk.resolve()), 'nvcc': str(args.nvcc.resolve()),
               'gpu_lock': str(args.gpu_lock.resolve()),
               'coverage_dir': str(args.coverage_dir.resolve()) if args.coverage_dir else None},
               'execution_order': [], 'modes': {}, 'pass': True}
    args.sdk = args.sdk.resolve()
    args.nvcc = args.nvcc.resolve()
    if args.coverage_dir:
        args.coverage_dir = args.coverage_dir.resolve()
    args.gpu_lock.parent.mkdir(parents=True, exist_ok=True)
    with args.gpu_lock.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for mode_index, (mode, flags) in enumerate(modes):
            source = ROOT / 'test/cuda_api_real.cu'
            if args.coverage_dir:
                text = (args.coverage_dir / 'demo.cu').read_text()
                text = '#include <chrono>\n#include <unistd.h>\n' + text
                marker = '    constexpr size_t bytes=16384;'
                assert marker in text
                text = text.replace(marker, '    puts("{\\"ready\\":true}");\n    fflush(stdout);\n    if (getchar() == EOF) return 1;\n    auto measured_start = std::chrono::steady_clock::now();\n' + marker, 1)
                text = text.replace('\\"status\\":%d,\\"ok\\":%s', '\\"status\\":%d,\\"pid\\":%d,\\"ok\\":%s')
                text = text.replace(r'\"cbid\":%u,\"api\":\"%s\",\"entries\":%llu', r'\"cbid\":%u,\"api\":\"%s\",\"entries\":%llu,\"pid\":%d')
                text = text.replace('domain==CUPTI_CB_DOMAIN_DRIVER_API?\"driver\":\"runtime\",id,name,count);', 'domain==CUPTI_CB_DOMAIN_DRIVER_API?\"driver\":\"runtime\",id,name,count,getpid());')
                text = text.replace('library, api, symbol, status, status == 0', 'library, api, symbol, status, getpid(), status == 0')
                end = '    return failures?1:0;'
                assert end in text
                text = text.replace(end, '    auto measured_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - measured_start).count();\n    printf("{\\"elapsed_ns\\":%lld,\\"failures\\":%d,\\"iterations\\":1}\\n", (long long)measured_ns, failures);\n    fflush(stdout);\n    if (getchar() == EOF) return 1;\n' + end, 1)
                source = out / 'coverage.cu'
                source.write_text(text)
            binary, obj = out / ('workload-' + mode), out / ('workload-' + mode + '.o')
            compiler_include = args.nvcc.parent.parent / 'targets/x86_64-linux/include'
            with (out / ('build-' + mode + '.log')).open('w') as log:
                subprocess.run([str(args.nvcc), '-std=c++17', '-arch=sm_80', '-Xcompiler=-fno-omit-frame-pointer',
                                '-Xcompiler=-g', '-I' + str(args.sdk / 'include'), '-I' + str(compiler_include),
                                *flags, *(['-I' + str(args.coverage_dir)] if args.coverage_dir else []), '-c', str(source), '-o', str(obj)], check=True,
                               stdout=log, stderr=log)
                subprocess.run(['g++', str(obj), str(args.sdk / 'lib/libcudart.so.13'), *([str(args.sdk / 'lib/libcupti.so.13')] if args.coverage_dir else []), '-lcuda', '-ldl', '-pthread',
                                '-rdynamic', '-Wl,-rpath,' + str(args.sdk / 'lib'), '-o', str(binary)], check=True,
                               stdout=log, stderr=log)
            evidence = compiled_evidence(subprocess.check_output(['objdump', '-d', '-C', str(binary)], text=True),
                                         subprocess.check_output(['nm', '-C', str(binary)], text=True))
            (out / ('binary-evidence-' + mode + '.json')).write_text(json.dumps(evidence, indent=2))
            results, warmups = {case: [] for case in CASES}, {}
            for phase, index, case in case_schedule(args.repeat, args.seed + mode_index):
                directory = out / (mode + '-' + case) / ('warmup' if phase == 'warmup' else 'sample-%02d' % index)
                directory.mkdir(parents=True, exist_ok=True)
                result = execute(binary, args.wprof.resolve(), directory, evidence, stacks=case == 'stacks',
                                 sealed=case == 'sealed', baseline=case == 'baseline', coverage=bool(args.coverage_dir),
                                 iterations=args.iterations)
                result.update({'phase': phase, 'sample': index})
                (directory / 'result.json').write_text(json.dumps(result, indent=2))
                summary['execution_order'].append({'mode': mode, 'case': case, 'phase': phase, 'sample': index,
                                                   'directory': str(directory)})
                summary['pass'] &= result['pass']
                if phase == 'warmup':
                    warmups[case] = result
                else:
                    results[case].append(result)
                print(mode, case, phase, index, {key: value for key, value in result.items() if key in
                      ('elapsed_ns', 'events', 'observed_names', 'attach_ns', 'recorded_loss', 'pass')}, flush=True)
            summary['modes'][mode] = {'seed': args.seed + mode_index, 'warmups': warmups,
                                      'cases': summarize_samples(results)}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2))
    print('Real CUDA UTRACE verification ' + ('PASS' if summary['pass'] else 'FAIL'), flush=True)
    return 0 if summary['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
