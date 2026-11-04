#!/usr/bin/env python3
"""Verify timestamp formatting and output ownership through worker callbacks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class JsonCallbackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="wprof-json-callback-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "callback"
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-O2", "-Wall", "-Wextra",
            str(ROOT / "test/json_callback_fixture.c"), "-o", str(cls.binary),
            str(ROOT / "src/wrust/target/release/libwrust_c.a"), str(ROOT / "src/json_uring.c"), "-lpthread", "-ldl", "-lm",
        ], check=True)

    def test_exact_timestamps_and_owned_output_on_workers_and_caller(self):
        expected = (
            b'{"ordinary":18446744073.709551615,"ts":18446744073.709551615}\n'
            b'{"ordinary":8388608.000000001,"ts":8388608.000000001}\n'
            b'{"ordinary":0.999999999,"ts":0.999999999}\n'
        )
        for flags in ([], ["--serial"]):
            with self.subTest(flags=flags):
                result = subprocess.run([str(self.binary), *flags], capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                self.assertEqual(result.stdout, expected)

    def test_explicit_timestamp_opt_out_on_workers_and_caller(self):
        expected = (
            b'{"ordinary":18446744073.709552765,"ts":18446744073.709552765}\n'
            b'{"ordinary":8388608.000000002,"ts":8388608.000000002}\n'
            b'{"ordinary":0.999999999,"ts":0.999999999}\n'
        )
        for flags in (["--legacy"], ["--legacy", "--serial"]):
            with self.subTest(flags=flags):
                result = subprocess.run([str(self.binary), *flags], capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                self.assertEqual(result.stdout, expected)


if __name__ == "__main__":
    unittest.main()
