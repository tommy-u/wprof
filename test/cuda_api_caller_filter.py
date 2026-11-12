#!/usr/bin/env python3
"""Check driver caller observations and optional runtime filtering without a GPU."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from utrace_ready import check_wprof_nofile, start_wprof, wait_wprof_ready

ROOT = Path(__file__).resolve().parents[1]
MODES = ('multi', 'individual', 'static-runtime')
ITERATIONS = 8
FILTERED_SEQUENCE = [
    ('cuDeviceGetAttribute', 'utrace_entry', None),
    ('cuDeviceGetAttribute', 'utrace_exit', -3),
    ('cudaDeviceSynchronize', 'utrace_entry', None),
    ('cudaDeviceSynchronize', 'utrace_exit', 4),
    ('cuCtxSynchronize', 'utrace_entry', None),
    ('cuCtxSynchronize', 'utrace_exit', 7),
]
UNFILTERED_SEQUENCE = [
    ('cuDeviceGetAttribute', 'utrace_entry', None),
    ('cuDeviceGetAttribute', 'utrace_exit', -3),
    ('cudaDeviceSynchronize', 'utrace_entry', None),
    ('cuDeviceGetAttribute', 'utrace_entry', None),
    ('cuDeviceGetAttribute', 'utrace_exit', -3),
    ('cuCtxSynchronize', 'utrace_entry', None),
    ('cuCtxSynchronize', 'utrace_exit', 7),
    ('cudaDeviceSynchronize', 'utrace_exit', 4),
    ('cuCtxSynchronize', 'utrace_entry', None),
    ('cuCtxSynchronize', 'utrace_exit', 7),
]
STATIC_SEQUENCE = [
    ('cuDeviceGetAttribute', 'utrace_entry', None),
    ('cuDeviceGetAttribute', 'utrace_exit', -3),
    ('cuDeviceGetAttribute', 'utrace_entry', None),
    ('cuDeviceGetAttribute', 'utrace_exit', -3),
    ('cuCtxSynchronize', 'utrace_entry', None),
    ('cuCtxSynchronize', 'utrace_exit', 7),
    ('cuCtxSynchronize', 'utrace_entry', None),
    ('cuCtxSynchronize', 'utrace_exit', 7),
]


def build(output):
    output.mkdir(parents=True, exist_ok=True)
    for name, source in (('libcuda.so', 'cuda_api_caller_filter_driver.c'),
                         ('libcudart.so', 'cuda_api_caller_filter_runtime.c')):
        subprocess.run(['cc', '-shared', '-fPIC', '-O0', '-g', '-fno-omit-frame-pointer',
                        str(ROOT / 'test' / source), '-o', str(output / name)], check=True)
    subprocess.run(['cc', '-O0', '-g', '-fno-omit-frame-pointer', '-rdynamic',
                    str(ROOT / 'test/cuda_api_caller_filter_main.c'), '-ldl', '-o', str(output / 'fixture')], check=True)
    subprocess.run(['cc', '-O0', '-g', '-fno-omit-frame-pointer', '-rdynamic', '-DSTATIC_RUNTIME',
                    str(ROOT / 'test/cuda_api_caller_filter_main.c'), str(ROOT / 'test/cuda_api_caller_filter_runtime.c'),
                    '-L' + str(output), '-Wl,-rpath,$ORIGIN', '-lcuda', '-ldl',
                    '-o', str(output / 'static-fixture')], check=True)
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-I' + str(ROOT / 'libbpf/include/uapi'),
                    str(ROOT / 'test/utrace_multi_failure.c'), '-ldl',
                    '-o', str(output / 'libmulti-failure.so')], check=True)


def signature(events):
    return [(row['name'], row['t'], row.get('args', {}).get('status')) for row in events]


def check_stacks(header, rows, events, filtered):
    stacks = {row['id']: row for row in rows[1:1 + header['stack_cnt']]}
    entries = [row for row in events if row['t'] == 'utrace_entry']
    assert entries and all(row.get('stack_id') in stacks for row in entries), entries
    drivers = [row for row in entries if row['name'] in ('cuDeviceGetAttribute', 'cuCtxSynchronize')]
    nested_drivers = [row for row in drivers if any('cudaDeviceSynchronize' in frame
                                                  for frame in stacks[row['stack_id']]['frames'])]
    assert len(nested_drivers) == (0 if filtered else 2 * ITERATIONS), nested_drivers
    direct_drivers = [row for row in drivers if row not in nested_drivers]
    assert len(direct_drivers) == 2 * ITERATIONS, direct_drivers
    for row in direct_drivers:
        frames = stacks[row['stack_id']]['frames']
        user_frames = [frame for frame in frames if frame.startswith('[U]')]
        assert 'coverage_run' in user_frames[-2], frames
        assert not any('cudaDeviceSynchronize' in frame for frame in frames), frames
    for row in nested_drivers:
        frames = stacks[row['stack_id']]['frames']
        user_frames = [frame for frame in frames if frame.startswith('[U]')]
        assert 'cudaDeviceSynchronize' in user_frames[-2], frames
    manual = [row for row in entries if row['name'] == 'manual_attr']
    nested = [row for row in manual if any('cudaDeviceSynchronize' in frame
                                         for frame in stacks[row['stack_id']]['frames'])]
    assert len(nested) == ITERATIONS, nested
    for row in nested:
        frames = stacks[row['stack_id']]['frames']
        user_frames = [frame for frame in frames if frame.startswith('[U]')]
        assert 'cudaDeviceSynchronize' in user_frames[-2], frames
    return {'retained_driver_entries': len(drivers), 'runtime_driver_entries': len(nested_drivers),
            'manual_runtime_callers': len(nested)}


def capture(wprof, fixture, output, mode):
    output.mkdir(parents=True, exist_ok=True)
    has_runtime = mode != 'static-runtime'
    stacks = mode == 'multi'
    driver = fixture / 'libcuda.so'
    tracee_command = [str(fixture / ('fixture' if has_runtime else 'static-fixture')), str(driver)]
    if has_runtime:
        tracee_command.append(str(fixture / 'libcudart.so'))
    tracee = subprocess.Popen(tracee_command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    tracer = None
    try:
        assert tracee.stdout.readline().strip() == 'ready', 'fixture did not initialize'
        maps = Path('/proc/%d/maps' % tracee.pid).read_text()
        assert ('libcudart.so' in maps) == has_runtime, maps
        data, log = output / 'capture.data', output / 'capture.log'
        prefix = ['sudo', '-n']
        if mode == 'individual':
            prefix += ['prlimit', '--nofile=256:1024', '--',
                       'env', 'LD_PRELOAD=' + str(fixture / 'libmulti-failure.so'), 'WPROF_TEST_FAIL_MULTI=load']
        command = ['-f', 'cuda-api=' + str(tracee.pid), '-f', 'no-sched', '-f', 'no-wakeup',
                   '-f', 'no-task-life', '-d200ms', '-D', str(data),
                   '--stacks=utrace' if stacks else '--no-stacks']
        command += ['-U', 'uspan:cuDeviceGetAttribute (pid:%d, path:%s, arg:ret:s32/name(status)%s) | name:"manual_attr" |' %
                    (tracee.pid, driver, ', stack' if stacks else '')]
        with log.open('w') as stream:
            tracer = start_wprof(wprof, command, output, prefix=prefix, stdout=stream, stderr=subprocess.STDOUT)
            window = wait_wprof_ready(tracer, output)
            if mode == 'individual':
                check_wprof_nofile(window, 1024, 1024)
            tracee.stdin.write('\n')
            tracee.stdin.flush()
            assert tracee.stdout.readline().strip() == 'done', 'fixture calls failed'
            assert tracer.wait(timeout=30) == 0, log.read_text()
            tracee.stdin.write('\n')
            tracee.stdin.flush()
            assert tracee.wait(timeout=10) == 0
        subprocess.run(['sudo', '-n', 'chown', '%d:%d' % (os.getuid(), os.getgid()), str(data)], check=True)
        with (output / 'capture.info').open('w') as stream:
            subprocess.run([str(wprof), '-RI', '--stats', '-D', str(data)], check=True,
                           stdout=stream, stderr=subprocess.STDOUT)
        ndjson = output / 'capture.json'
        subprocess.run([str(wprof), '-R', '-D', str(data), '-J', str(ndjson)], check=True,
                       stdout=subprocess.DEVNULL)
        rows = [json.loads(line) for line in ndjson.read_text().splitlines()]
        header = rows[0]
        events = [row for row in rows[1 + header['stack_cnt']:] if row.get('t', '').startswith('utrace_')]
        assert all(row['task']['pid'] == tracee.pid for row in events), events
        generated = [row for row in events if row['name'] != 'manual_attr']
        metadata = header['metadata']
        policy = metadata.get('cuda-api.nested_driver.' + str(tracee.pid), 'unavailable')
        assert policy in ('skipped', 'retained', 'unavailable'), metadata
        assert has_runtime or policy != 'skipped', metadata
        filtered = policy == 'skipped'
        sequence = FILTERED_SEQUENCE if filtered else UNFILTERED_SEQUENCE
        expected = (sequence if has_runtime else STATIC_SEQUENCE) * ITERATIONS
        actual = signature(generated)
        assert actual == expected, (mode, actual, expected)
        manual = [row for row in events if row['name'] == 'manual_attr']
        manual_pair = [('manual_attr', 'utrace_entry', None), ('manual_attr', 'utrace_exit', -3)]
        assert signature(manual) == manual_pair * ITERATIONS * 2, signature(manual)
        expected_mode = 'individual_uprobes' if mode == 'individual' else 'uprobe_multi'
        assert metadata['cuda-api.attach_mode'] == expected_mode, metadata
        result = {'mode': mode, 'events': len(events), 'attach_mode': metadata['cuda-api.attach_mode'],
                  'caller_policy': policy, 'pass': True}
        if stacks:
            result['stacks'] = check_stacks(header, rows, events, filtered)
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result))
        return actual
    finally:
        if tracee.poll() is None:
            tracee.kill()
            tracee.wait()
        if tracer and tracer.poll() is None:
            tracer.terminate()
            tracer.wait(timeout=10)


def run(wprof, output, mode='all'):
    fixture = output / 'fixture'
    build(fixture)
    sequences = {}
    for case in MODES if mode == 'all' else (mode,):
        sequences[case] = capture(wprof, fixture, output / case, case)
    if mode == 'all':
        assert sequences['multi'] == sequences['individual']
        print('Multi and individual name/type/status sequences match PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wprof', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mode', choices=('all', *MODES), default='all')
    args = parser.parse_args()
    if args.out:
        run(args.wprof.resolve(), args.out.resolve(), args.mode)
    else:
        with tempfile.TemporaryDirectory(prefix='wprof-caller-filter-') as directory:
            run(args.wprof.resolve(), Path(directory), args.mode)
