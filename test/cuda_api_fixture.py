#!/usr/bin/env python3
"""Capture real uprobes on controlled CPU libraries; no GPU coverage is implied."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
import tempfile

from utrace_ready import check_wprof_nofile, start_wprof, wait_wprof_ready

ROOT = Path(__file__).resolve().parents[1]


def run(wprof, output, mode='plain'):
    output.mkdir(parents=True, exist_ok=True)
    libraries = []
    version_map = output / 'versions.map'
    version_map.write_text('FIRST {}; SECOND {};\n')
    for name in ('libcuda.so', 'libcudart.so'):
        path = output / name
        subprocess.run(['cc', '-shared', '-fPIC', '-O0', '-g', str(ROOT / 'test/cuda_api_fixture.c'),
                        *(['-Wl,--hash-style=sysv'] if mode in ('fallback', 'versions-fallback') else []),
                        *(['-DAMBIGUOUS_VERSION', '-Wl,--version-script=' + str(version_map)] if mode.startswith('versions') else []),
                        '-o', str(path)], check=True)
        libraries.append(path)
    binary = output / 'fixture'
    subprocess.run(['cc', '-O0', '-g', '-fno-omit-frame-pointer', '-rdynamic', '-pthread',
                    str(ROOT / 'test/cuda_api_fixture_main.c'), '-ldl', '-o', str(binary)], check=True)
    tracee = subprocess.Popen([str(binary), *map(str, libraries), *(['worker'] if mode == 'worker' else [])], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True)
    tracer = None
    try:
        assert tracee.stdout.readline().strip() == 'ready'
        data, log = output / 'capture.data', output / 'capture.log'
        prefix = ['sudo', '-n']
        command = ['-v', '-f', 'cuda-api=' + str(tracee.pid),
                   '-f', 'cuda-api=' + str(tracee.pid), '-f', 'no-sched', '-f', 'no-wakeup',
                   '-f', 'no-task-life', '-d200ms', '-D', str(data)]
        if mode == 'low-fds':
            prefix += ['prlimit', '--nofile=256:256', '--']
        if mode.startswith('multi-fail-'):
            failure = mode.removeprefix('multi-fail-')
            injector = output / 'libmulti-failure.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-I' + str(ROOT / 'libbpf/include/uapi'),
                            str(ROOT / 'test/utrace_multi_failure.c'), '-ldl', '-o', str(injector)], check=True)
            prefix += ['prlimit', '--nofile=256:1024', '--',
                       'env', 'LD_PRELOAD=' + str(injector), 'WPROF_TEST_FAIL_MULTI=' + failure]

        command += ['-U', 'u:cuDeviceGetAttribute (pid:%d, path:%s) | id:"instant-{env:tid}" |' % (tracee.pid, libraries[0])]
        command += ['-U', 'uspan:cuDeviceGetAttribute (pid:%d, path:%s%s) | id:"span-{env:tid}", name:"templated_attr" |' %
                    (tracee.pid, libraries[0], ', stack' if mode == 'stacks' else '')]
        if mode == 'worker':
            command += ['--thread-name=cuda-api-worker']
        if mode == 'stacks':
            command += ['--stacks=utrace']
        else:
            command += ['--no-stacks']
        if mode == 'sealed':
            command += ['--seal=uprobe-args']
        with log.open('w') as stream:
            tracer = start_wprof(wprof, command, output, prefix=prefix, stdout=stream, stderr=subprocess.STDOUT)
            window = wait_wprof_ready(tracer, output)
            if mode.startswith('multi-fail-'):
                check_wprof_nofile(window, 1024, 1024)
            elif mode == 'low-fds':
                check_wprof_nofile(window, 256, 256)
            tracee.stdin.write('\n')
            tracee.stdin.flush()
            assert tracee.stdout.readline().strip() == 'done'
            assert tracer.wait(timeout=30) == 0, log.read_text()
            tracee.stdin.write('\n')
            tracee.stdin.flush()
            assert tracee.wait(timeout=10) == 0
        subprocess.run(['sudo', '-n', 'chown', str(os.getuid()) + ':' + str(os.getgid()), str(data)], check=True)
        ndjson = output / 'capture.json'
        subprocess.run([str(wprof), '-R', '-D', str(data), '-J', str(ndjson)], check=True,
                       stdout=subprocess.DEVNULL)
        rows = [json.loads(line) for line in ndjson.read_text().splitlines()]
        events = [r for r in rows if r.get('t', '').startswith('utrace_')]
        counts = Counter((r['name'], r['t']) for r in events)
        assert counts['cuLaunchKernel', 'utrace_entry'] == 60, counts
        assert counts['cuLaunchKernel', 'utrace_exit'] == 60, counts
        assert counts['cuLaunchKernel_ptsz', 'utrace_entry'] == 60, counts
        assert counts['cuLaunchKernel_ptsz', 'utrace_exit'] == 60, counts
        assert counts['cudaLaunchKernel_ptsz', 'utrace_entry'] == 30, counts
        assert counts['cudaLaunchKernel_ptsz', 'utrace_exit'] == 30, counts
        capture_text = log.read_text()
        warnings = [line for line in capture_text.splitlines() if 'one call may appear under several names' in line]
        assert len(warnings) == 1 and 'libcuda.so' in warnings[0], capture_text
        assert any('cuLaunchKernel' in line and 'cuLaunchKernel_ptsz' in line and 'share file address' in line
                   for line in capture_text.splitlines()), capture_text
        if mode.startswith('multi-fail-'):
            expected_mode = 'individual_uprobes' if mode == 'multi-fail-load' else 'mixed'
            assert rows[0]['metadata']['cuda-api.attach_mode'] == expected_mode, rows[0]['metadata']
        if mode.startswith('versions'):
            key = 'cuda-api.site.%d.cudaSetDevice' % tracee.pid
            assert 'error:-17' in rows[0]['metadata'][key], rows[0]['metadata'][key]
        if mode == 'worker':
            assert all(r['task']['tid'] != tracee.pid for r in events)
        for r in events:
            assert r['task']['pid'] == tracee.pid
            if mode == 'sealed' or r['t'] != 'utrace_exit' or r['name'] == 'cudaGetErrorString':
                assert not r.get('args'), r
            elif r['name'] == 'cuDeviceGetAttribute':
                assert r['args']['status'] == -3, r
            elif r['name'] == 'cudaLaunchKernel_ptsz':
                assert r['args']['status'] == 4, r
        if mode == 'stacks':
            stacks = {r['id']: r for r in rows[1:1 + rows[0]['stack_cnt']]}
            entries = [r for r in events if r['t'] == 'utrace_entry']
            assert entries and all(r.get('stack_id') in stacks for r in entries)
            assert all(any('coverage_run' in frame for frame in stacks[r['stack_id']]['frames']) for r in entries)
        info = subprocess.run([str(wprof), '-RI', '-D', str(data)], check=True, capture_output=True, text=True)
        detail = subprocess.run([str(wprof), '-v', '-RI', '-D', str(data)], check=True, capture_output=True, text=True)
        info_text, detail_text = info.stdout + info.stderr, detail.stdout + detail.stderr
        assert any('CUDA host API: PID %d,' % tracee.pid in line and 'details: -v' in line
                   for line in info_text.splitlines()), info_text
        assert any('CUDA site diagnostics:' in line and 'missing/ambiguous (details: -v)' in line
                   for line in info_text.splitlines()), info_text
        assert info_text.count('--utrace ') < detail_text.count('--utrace '), (info_text, detail_text)
        assert 'cuda-api.site.' not in info_text, info_text
        assert 'cuLaunchKernel' in detail_text and 'cuLaunchKernel_ptsz' in detail_text, detail_text
        assert 'templated_attr' in info_text, 'manual span definition must remain visible'
        policy = output / 'policy'
        subprocess.run(['cc', '-I' + str(ROOT / 'src'), str(ROOT / 'test/cuda_api_policy.c'), '-o', str(policy)], check=True)
        subprocess.run([str(policy)], check=True)
        result = {'mode': mode, 'events': len(events), 'pass': True, 'data': str(data)}
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result))
        return data
    finally:
        if tracee.poll() is None:
            tracee.kill()
            tracee.wait()
        if tracer and tracer.poll() is None:
            tracer.terminate()
            tracer.wait(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wprof', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mode', choices=('plain', 'sealed', 'stacks', 'fallback', 'versions', 'versions-fallback', 'worker', 'low-fds', 'multi-fail-load', 'multi-fail-attach-entry', 'multi-fail-attach-return'), default='plain')
    args = parser.parse_args()
    if args.out:
        run(args.wprof.resolve(), args.out.resolve(), args.mode)
    else:
        with tempfile.TemporaryDirectory(prefix='wprof-fixture-') as directory:
            run(args.wprof.resolve(), Path(directory), args.mode)
