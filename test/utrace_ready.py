#!/usr/bin/env python3
"""Synchronize finite test captures with the owned wprof BPF recording window."""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'test/utrace_ready.c'
LIBBPF = ROOT / 'src/.output/libbpf.a'
PID_WRAPPER = 'printf "%s\\n" "$$" > "$1"; shift; exec "$@"'


def build_ready(output):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'utrace-ready'
    if not LIBBPF.is_file():
        raise RuntimeError('build wprof before running capture tests; missing\n' + str(LIBBPF))
    if not binary.exists() or binary.stat().st_mtime_ns < max(SOURCE.stat().st_mtime_ns, LIBBPF.stat().st_mtime_ns):
        temporary = output / ('utrace-ready.%d.tmp' % os.getpid())
        try:
            subprocess.run(['cc', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'libbpf/src'), '-I' + str(ROOT / 'libbpf/include/uapi'),
                            str(SOURCE), str(LIBBPF), '-l:libelf.so.1', '-l:libz.so.1', '-o', str(temporary)], check=True)
            temporary.replace(binary)
        finally:
            temporary.unlink(missing_ok=True)
    return binary


def start_wprof(wprof, arguments, output, *, prefix=('sudo', '-n'), **popen_options):
    output = Path(output).resolve()
    build_ready(output)
    pid_file = output / 'wprof.pid'
    pid_file.unlink(missing_ok=True)
    command = [*prefix, 'sh', '-c', PID_WRAPPER, 'wprof-test', str(pid_file), str(Path(wprof).resolve()),
               *map(str, arguments)]
    return subprocess.Popen(command, **popen_options)


def wait_wprof_ready(tracer, output, timeout=30):
    output = Path(output).resolve()
    binary = output / 'utrace-ready'
    command = [*(['sudo', '-n'] if os.geteuid() else []), str(binary), str(output / 'wprof.pid'),
               str(tracer.args[tracer.args.index('wprof-test') + 2]),
               str(int(timeout * 1000)), str(tracer.pid)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout + 5)
    assert result.returncode == 0, 'wprof recording readiness failed (launcher status %s): %s' % (
        tracer.poll(), result.stderr.strip())
    return json.loads(result.stdout)


def check_wprof_nofile(window, soft, hard):
    result = subprocess.run([*(['sudo', '-n'] if os.geteuid() else []), 'cat',
                             '/proc/%d/limits' % window['pid']], check=True, capture_output=True, text=True)
    fields = next(line.split() for line in result.stdout.splitlines() if line.startswith('Max open files'))
    actual = int(fields[3]), int(fields[4])
    assert actual == (soft, hard), (actual, (soft, hard))
    return actual
