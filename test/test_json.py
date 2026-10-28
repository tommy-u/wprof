#!/usr/bin/env python3
"""Compare default JSON bytes against the frozen original C serializer."""

from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = Path(os.environ.get("WPROF_SOURCE_DIR", ROOT / "src")).resolve()
WPROF = Path(os.environ.get("WPROF_BINARY", ROOT / "src/wprof"))
REFERENCE_BYTES = 7219740
REFERENCE_RECORDS = 12002
REFERENCE_SHA256 = "baab684d42a1af585fe1ce21628b248c682b7567594d8ceca22f136b4a2d28cf"


JSON_OPTIMIZATIONS = (
    'buffering', 'integers',
)


JSON_DEBUG_FEATURES = (
    'json-no-batching', 'json-no-int-format',
)


class JsonOptionsTest(unittest.TestCase):
    def test_json_debug_features_are_accepted(self):
        for feature in JSON_DEBUG_FEATURES:
            with self.subTest(feature=feature):
                result = subprocess.run(
                    [str(WPROF), "-R", "-D", "/missing-json-test.data", "-J", "-", "--debug", feature],
                    capture_output=True,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(b"Unrecognized debug feature", result.stderr)
                self.assertIn(b"/missing-json-test.data", result.stderr)

    def test_unknown_json_optimization_is_rejected(self):
        for flag in ("--json-opt", "--no-json-opt"):
            with self.subTest(flag=flag):
                result = subprocess.run([str(WPROF), flag, "unknown"], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"Unknown JSON optimization 'unknown'", result.stderr)
                self.assertIn(flag.encode(), result.stderr)

    def test_json_optimization_controls_are_accepted(self):
        for flag in ("--json-opt", "--no-json-opt"):
            for name in (*JSON_OPTIMIZATIONS, "all"):
                with self.subTest(flag=flag, name=name):
                    result = subprocess.run(
                        [str(WPROF), "-R", "-D", "/missing-json-test.data", "-J", "-", flag, name],
                        capture_output=True,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn(b"Unknown JSON optimization", result.stderr)
                    self.assertIn(b"/missing-json-test.data", result.stderr)

    def test_removed_mode_and_tuning_flags_are_rejected(self):
        for flags in (("--fast-json",), ("--json-workers", "4"), ("--json-batch-size", "1mb")):
            with self.subTest(flags=flags):
                result = subprocess.run(
                    [str(WPROF), "-R", "-D", "/missing-json-test.data", "-J", "-", *flags],
                    capture_output=True,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"unrecognized option", result.stderr)
                self.assertIn(flags[0].encode(), result.stderr)

    def test_help_omits_removed_json_flags(self):
        result = subprocess.run([str(WPROF), "--help"], capture_output=True, check=True)
        for flag in (b"--fast-json", b"--json-workers", b"--json-batch-size"):
            self.assertNotIn(flag, result.stdout)


class JsonWriterTest(unittest.TestCase):
    def test_debug_combinations_match_reference_bytes(self):
        checked = 0
        for mask in range(1 << len(JSON_DEBUG_FEATURES)):
            features = [feature for bit, feature in enumerate(JSON_DEBUG_FEATURES) if mask & (1 << bit)]
            if "json-no-parallel" in JSON_DEBUG_FEATURES and "json-no-batching" in features and "json-no-parallel" not in features:
                continue
            flags = [arg for feature in features for arg in ("--debug", feature)]
            with self.subTest(features=features):
                reference = subprocess.check_output([str(self.fixtures["reference"]), *flags])
                output = subprocess.check_output([str(self.fixtures["default"]), *flags])
                self.assertEqual(reference, self.reference)
                self.assertEqual(output, reference)
                self.assertEqual(hashlib.sha256(output).hexdigest(), REFERENCE_SHA256)
                checked += 1
        self.assertGreater(checked, 0)
        print(f"Verified {checked} debug combinations, SHA-256 {REFERENCE_SHA256}")

    def test_named_optimization_controls_match_reference_bytes(self):
        settings = [["--no-json-opt", name] for name in JSON_OPTIMIZATIONS]
        settings.extend((
            ["--no-json-opt", "all"],
            ["--no-json-opt", "all", "--json-opt", "all"],
            ["--json-opt", "all", "--no-json-opt", "all"],
            ["--debug", "json-no-batching", "--json-opt", "buffering"],
            ["--json-opt", "buffering", "--debug", "json-no-batching"],
        ))
        for flags in settings:
            with self.subTest(flags=flags):
                reference = subprocess.check_output([str(self.fixtures["reference"]), *flags], timeout=30)
                output = subprocess.check_output([str(self.fixtures["default"]), *flags], timeout=30)
                self.assertEqual(reference, self.reference)
                self.assertEqual(output, reference)
                self.assertEqual(hashlib.sha256(output).hexdigest(), REFERENCE_SHA256)

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="wprof-json-reference-test-")
        cls.addClassCleanup(cls.tmp.cleanup)
        build_dir = Path(cls.tmp.name)
        fixture_dir = build_dir / "test"
        fixture_dir.mkdir()
        for name in ("json_fixture.c", "json_reference.h"):
            shutil.copyfile(ROOT / "test" / name, fixture_dir / name)
        (build_dir / "src").symlink_to(SOURCE_DIR, target_is_directory=True)

        cls.fixtures = {}
        for mode in ("reference", "default"):
            fixture = build_dir / mode
            command = [
                *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-O2", "-Wall", "-Wextra",
                str(fixture_dir / "json_fixture.c"), "-o", str(fixture),
            ]
            if mode == "reference":
                command.append("-DJSON_REFERENCE")
            else:
                library = SOURCE_DIR / "wrust/target/release/libwrust_c.a"
                if library.is_file():
                    command.extend([str(library), "-lpthread", "-ldl", "-lm"])
            subprocess.run(command, check=True)
            cls.fixtures[mode] = fixture

        cls.reference = subprocess.check_output([str(cls.fixtures["reference"])])
        cls.output = subprocess.check_output([str(cls.fixtures["default"])])
        cls.lines = cls.output.splitlines()
        cls.records = [json.loads(line, parse_float=Decimal) for line in cls.lines]

    def test_default_matches_reference_bytes(self):
        digest = hashlib.sha256(self.reference).hexdigest()
        self.assertEqual(len(self.reference), REFERENCE_BYTES)
        self.assertEqual(len(self.reference.splitlines()), REFERENCE_RECORDS)
        self.assertEqual(digest, REFERENCE_SHA256)
        self.assertEqual(len(self.output), len(self.reference))
        self.assertEqual(hashlib.sha256(self.output).hexdigest(), digest)
        self.assertEqual(self.output, self.reference)
        print(f"Verified {len(self.reference)} bytes, {len(self.lines)} records, SHA-256 {digest}")

    def test_control_bytes_utf8_and_owned_strings(self):
        first = self.records[0]
        self.assertEqual(
            first["text"],
            'quote" slash\\\n\t\r\b\f\x01\x1f é € 😀 '
            '\x80\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xe2\x82',
        )
        self.assertEqual(first["controls"], "".join(chr(i) for i in range(1, 32)))
        self.assertEqual(first["utf8_boundaries"], "\u0080\u07ff\u0800\ud7ff\uffff\U00010000\U0010ffff")
        self.assertEqual(first["malformed_utf8"], "\xc1\xbf\xe0\x9f\xbf\xf0\x8f\xbf\xbf\xf5\x80\x80\x80\xff")
        self.assertEqual(first["empty"], "")
        self.assertEqual(first[""], "empty key")
        self.assertEqual(first['key"\\\n€'], "escaped key")
        self.assertEqual(first["reused"], "original")
        self.assertEqual(first["original-key"], "stack key")
        self.assertNotIn("overwritten-key", first)
        self.assertEqual(first["formatted"], "original/7/\n")
        self.assertEqual(first["long_format"], "0" * 299 + "7")
        self.assertEqual(first["frames"], ["frame_0", "frame_1", "frame_2", "frame_3", "frame_3/9/\n", None])

    def test_integer_and_float_formats(self):
        first = self.records[0]
        self.assertEqual(first["signed"], -(2**63))
        self.assertEqual(first["signed_max"], 2**63 - 1)
        self.assertEqual(first["unsigned"], 2**64 - 1)
        self.assertEqual(first["unsigned_zero"], 0)
        self.assertEqual(first["signed_min_next"], -9223372036854775807)
        self.assertEqual(first["signed_max_prev"], 9223372036854775806)
        self.assertEqual(first["unsigned_max_prev"], 18446744073709551614)
        for key, value in (
            ("zero6", "-0.000000"), ("zero9", "-0.000000000"),
            ("positive_zero6", "0.000000"), ("positive_zero9", "0.000000000"),
            ("smallest6", "0.000000"), ("smallest9", "0.000000000"),
            ("rounding6", "1.234568"), ("rounding9", "0.000000002"),
            ("negative6", "-1.234568"), ("negative9", "-0.000000002"),
            ("other_format", "1.234"),
        ):
            with self.subTest(key=key):
                self.assertIn(f'"{key}":{value},'.encode(), self.lines[0])
                self.assertEqual(first[key], Decimal(value))
        largest = (
            "179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558"
            "632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245"
            "490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168"
            "738177180919299881250404026184124858368"
        )
        for key, places in (("largest6", 6), ("largest9", 9)):
            with self.subTest(key=key):
                value = largest + "." + "0" * places
                self.assertIn(f'"{key}":{value},'.encode(), self.lines[0])
                self.assertEqual(first[key], Decimal(value))

    def test_integer_decimal_boundaries_in_objects_and_arrays(self):
        values = [
            0, 1, 2, 9, 10, 11, 99, 100, 101, 999, 1000, 1001, 9999, 10000, 10001,
            99999, 100000, 100001, 999999, 1000000, 1000001,
            9999999, 10000000, 10000001, 99999999, 100000000, 100000001,
            999999999, 1000000000, 1000000001, 9999999999, 10000000000, 10000000001,
            99999999999, 100000000000, 100000000001,
            999999999999, 1000000000000, 1000000000001,
            9999999999999, 10000000000000, 10000000000001,
            99999999999999, 100000000000000, 100000000000001,
            999999999999999, 1000000000000000, 1000000000000001,
            9999999999999999, 10000000000000000, 10000000000000001,
            99999999999999999, 100000000000000000, 100000000000000001,
            999999999999999999, 1000000000000000000, 1000000000000000001,
            9999999999999999999, 10000000000000000000, 10000000000000000001,
        ]
        boundaries = self.records[0]["decimal_boundaries"]
        self.assertEqual([row["unsigned"] for row in boundaries], values)
        for value, row in zip(values, boundaries):
            with self.subTest(value=value):
                expected = {"unsigned": value}
                if value <= 9223372036854775807:
                    expected.update(positive=value, negative=-value, signed_array=[value, -value])
                self.assertEqual(row, expected)

    def test_timestamp_rollover_and_float_fallback_boundaries(self):
        expected = {
            "zero": "0.000000000",
            "one_ns": "0.000000001",
            "second_before": "0.999999999",
            "second": "1.000000000",
            "second_after": "1.000000001",
            "guard_before": "4194303.999999999",
            "guard": "4194304.000000000",
            "guard_after": "4194304.000000001",
            "seconds_2p23_before": "8388607.999999999",
            "seconds_2p23": "8388608.000000000",
            "seconds_2p23_after": "8388608.000000002",
            "ns_2p53_before": "9007199.254740991",
            "ns_2p53": "9007199.254740993",
            "ns_2p53_after": "9007199.254740993",
            "maximum_before": "18446744073.709552765",
            "maximum": "18446744073.709552765",
            "wrapped_minus_one": "18446744073.709552765",
            "wrapped_before_second": "18446744072.709552765",
            "wrapped_second": "18446744072.709552765",
            "wrapped_after_second": "18446744072.709552765",
            "wrapped_signed_min": "9223372036.854776382",
        }
        self.assertEqual(self.records[0]["timestamps"], {key: Decimal(value) for key, value in expected.items()})
        for key, value in expected.items():
            with self.subTest(key=key):
                self.assertIn(f'"{key}":{value}'.encode(), self.lines[0])

    def test_nested_arrays_null_and_booleans(self):
        first = self.records[0]
        self.assertEqual(first["nested"], {
            "missing": None,
            "enabled": True,
            "disabled": False,
            "tree": [{"leaves": [-7, Decimal("1.250000"), None]}],
        })
        self.assertEqual(first["empty_object"], {})
        self.assertEqual(first["empty_array"], [])

    def test_record_order_and_values(self):
        self.assertEqual(len(self.records), 12002)
        records = self.records[1:-1]
        self.assertEqual([record["id"] for record in records], list(range(12000)))
        for index, timestamp, value in (
            (0, "0.123456789", "0.000000"),
            (1, "0.123456806", "0.142857"),
            (7, "0.123456908", "1.000000"),
            (11999, "0.123660772", "1714.142857"),
        ):
            with self.subTest(index=index):
                self.assertEqual(records[index], {
                    "id": index,
                    "ts": Decimal(timestamp),
                    "values": [-index, Decimal(value)],
                    "name": f"task-{index}",
                })

    def test_large_formatted_string_survives_source_reuse(self):
        self.assertEqual(self.records[-1]["large"], "😀" * (1024 * 1024 // 4 + 1))
        self.assertEqual(self.records[-1]["after_large"], "tail")
        self.assertTrue(self.output.endswith(b"}\n"))

    def test_large_ascii_keys_and_escapes_preserve_tail_order(self):
        last = self.records[-1]
        large_len = 1024 * 1024 + 4
        large_key = "k" * large_len
        self.assertEqual(list(last), [
            "large", "after_large", "large_ascii", "after_ascii", large_key,
            "after_key", "large_escaped", "after_escaped",
        ])
        self.assertEqual(last["large_ascii"], "A" * large_len)
        self.assertEqual(last[large_key], "large key")
        self.assertEqual(last["large_escaped"], '"\\\n\x01' * (large_len // 4))
        for key in ("after_ascii", "after_key", "after_escaped"):
            with self.subTest(key=key):
                self.assertEqual(last[key], "tail")

    def test_empty_stream(self):
        for mode, fixture in self.fixtures.items():
            with self.subTest(mode=mode):
                self.assertEqual(subprocess.check_output([str(fixture), "--empty"]), b"")

    @unittest.skipUnless(Path("/dev/full").exists(), "/dev/full is required")
    def test_output_failure(self):
        for mode, fixture in self.fixtures.items():
            with self.subTest(mode=mode), open("/dev/full", "wb") as sink:
                result = subprocess.run([str(fixture)], stdout=sink, stderr=subprocess.PIPE)
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
