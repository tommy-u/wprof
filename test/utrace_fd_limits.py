#!/usr/bin/env python3
"""Verify ordinary UTRACE capture leaves descriptor limits unchanged."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from utrace_ready import start_wprof, wait_wprof_ready


def capture(wprof, soft, hard, output, cuda_metadata=False):
    libc_path = next(line.split(maxsplit=5)[5].strip() for line in Path('/proc/self/maps').read_text().splitlines()
                     if '/libc.so.6' in line)
    data, log = output / 'capture.data', output / 'capture.log'
    prefix = ['sudo', '-n', 'prlimit', '--nofile=%d:%d' % (soft, hard), '--']
    command = ['--no-stacks', '-f', 'no-sched', '-f', 'no-wakeup', '-f', 'no-task-life',
               '-d250ms', '-D', str(data), '-U', 'u:getpid(pid:%d,path:%s)' % (os.getpid(), libc_path)]
    if cuda_metadata:
        command += ['-f', 'cuda-api=' + str(os.getpid())]
    with log.open('w') as stream:
        tracer = start_wprof(wprof, command, output, prefix=prefix, stdout=stream, stderr=subprocess.STDOUT)
        limits_reader = None
        try:
            window = wait_wprof_ready(tracer, output)
            libc = ctypes.CDLL(None)
            target_pid = os.getpid()
            for _ in range(10):
                assert libc.getpid() == target_pid
            limits_reader = subprocess.Popen(['sudo', '-n', 'cat', '/proc/%d/limits' % window['pid']],
                                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            while time.monotonic_ns() < window['session_end_ts'] and tracer.poll() is None:
                assert libc.getpid() == target_pid
                try:
                    tracer.wait(timeout=0.001)
                except subprocess.TimeoutExpired:
                    pass
            limits, _ = limits_reader.communicate(timeout=30)
            assert limits_reader.returncode == 0, limits
            fields = next(line.split() for line in limits.splitlines() if line.startswith('Max open files'))
            actual_soft, actual_hard = int(fields[3]), int(fields[4])
            assert (actual_soft, actual_hard) == (soft, hard), (actual_soft, actual_hard)
            assert tracer.wait(timeout=30) == 0, log.read_text()
        finally:
            if limits_reader and limits_reader.poll() is None:
                limits_reader.terminate()
                limits_reader.wait(timeout=30)
            if tracer.poll() is None:
                tracer.terminate()
                tracer.wait(timeout=30)
    replay = subprocess.run(['sudo', '-n', str(wprof), '-R', '-D', str(data), '-J', '-', '--no-stacks'],
                            check=True, capture_output=True, text=True)
    def unique_object(items):
        result = {}
        for key, value in items:
            assert key not in result, 'duplicate JSON key ' + key
            result[key] = value
        return result
    records = [json.loads(line, object_pairs_hook=unique_object) for line in replay.stdout.splitlines()]
    if cuda_metadata:
        metadata = records[0]['metadata']
        for library in ('libcuda.so', 'libcudart.so'):
            assert metadata['cuda-api.library.%d.%s' % (os.getpid(), library)].startswith('unavailable_or_ambiguous,')
        assert metadata['cuda-api.target.%d' % os.getpid()] == 'no_available_sites,error:0'

    observed = [r for r in records if r.get('t') == 'utrace_instant' and r.get('name') == 'getpid']
    assert len(observed) >= 10, 'missing actual probe observations'
    assert all(r['task']['pid'] == os.getpid() for r in observed)
    print('FD limits %d/%d PASS: enforced %d/%d, %d observations' %
          (soft, hard, actual_soft, actual_hard, len(observed)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wprof', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wprof-fd-limits-') as directory:
        for soft, hard in ((256, 256), (256, 1024)):
            output = Path(directory) / str(hard)
            output.mkdir()
            capture(args.wprof.resolve(), soft, hard, output, cuda_metadata=soft == hard)
