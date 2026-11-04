#!/usr/bin/env python3
"""Check cached UTF-8 fragments and source ownership through token capture."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CachedFragmentTest(unittest.TestCase):
    def test_utf8_fragments_are_owned_in_workers_and_serial_rendering(self):
        expected = ('{"task":{"comm":"é € 😀","pid":7},"literal":"é € 😀"}\n'
                    + '{"large":"' + '😀' * (1024 * 1024 // 4 + 1) + '"}\n').encode()
        with tempfile.TemporaryDirectory(prefix="wprof-cached-fragment-") as tmp:
            binary = Path(tmp) / "fixture"
            command = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-O2", "-Wall", "-Wextra",
                       str(ROOT / "test/json_fragment_fixture.c"), "-o", str(binary),
                       str(ROOT / "src/wrust/target/release/libwrust_c.a"), "-lpthread", "-ldl", "-lm"]
            backend = ROOT / "src/json_uring.c"
            if backend.exists():
                command.append(str(backend))
            subprocess.run(command, check=True)
            for flags in ([], ["--serial"]):
                with self.subTest(flags=flags):
                    run = subprocess.run([str(binary), *flags], capture_output=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stderr.decode(errors="replace"))
                    self.assertEqual(run.stdout, expected)


if __name__ == "__main__":
    unittest.main()
