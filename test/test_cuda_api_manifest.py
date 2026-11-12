#!/usr/bin/env python3
import json
import unittest

from cuda_api_manifest import build_manifest, compiled_evidence


class ManifestTest(unittest.TestCase):
    def test_direct_export_is_not_rewritten_as_a_compiler_wrapper(self):
        calls = json.loads('''[
            {"name": "cudaGraphLaunch_ptsz", "status": 0, "call_kind": "api"},
            {"name": "cudaGraphLaunch_ptsz", "status": 0, "call_kind": "direct_export"}
        ]''')
        manifest = build_manifest(calls, {'compiler_wrappers': {'cudaGraphLaunch_ptsz': 'cudaGraphLaunch'}})
        self.assertEqual(manifest['calls'], [
            {'name': 'cudaGraphLaunch', 'count': 1, 'status': 'i32', 'returns': {'0': 1}},
            {'name': 'cudaGraphLaunch_ptsz', 'count': 1, 'status': 'i32', 'returns': {'0': 1}},
        ])

    def test_inline_occupancy_requires_the_underlying_export(self):
        calls = json.loads('''[
            {"name": "cudaOccupancyMaxPotentialBlockSize", "status": 0},
            {"name": "cudaOccupancyMaxPotentialBlockSize", "status": 0, "call_kind": "direct_export"}
        ]''')
        manifest = build_manifest(calls, {'inline_algorithms': {
            'cudaOccupancyMaxPotentialBlockSize': 'cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags'
        }})
        self.assertEqual([call['name'] for call in manifest['calls']], [
            'cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags', 'cudaOccupancyMaxPotentialBlockSize'
        ])

    def test_compiled_forwarding_is_resolved_without_observed_events(self):
        evidence = compiled_evidence('''0000000000001010 <cudaGraphLaunch@plt>:
 1010: jmp *0x12
0000000000002010 <cudaGraphLaunch_ptsz>:
 2010: call 1010 <cudaGraphLaunch@plt>
''', '')
        self.assertEqual(evidence['compiler_wrappers'], {'cudaGraphLaunch_ptsz': 'cudaGraphLaunch'})
        self.assertEqual(evidence['static_imports'], ['cudaGraphLaunch'])

    def test_excluded_getters_leave_retained_calls_required(self):
        calls = [{'name': name, 'status': 0} for name in (
            'cuCtxGetDevice', 'cuCtxGetDevice_v2', 'cudaStreamIsCapturing_ptsz',
            'cudaGetLastError', 'cuDeviceGetAttribute')]
        manifest = build_manifest(calls, {'compiler_wrappers': {'cuCtxGetDevice': 'cuCtxGetDevice_v2'}})
        self.assertEqual(manifest['calls'], [
            {'name': 'cuDeviceGetAttribute', 'count': 1, 'status': 'i32', 'returns': {'0': 1}},
        ])


if __name__ == '__main__':
    unittest.main()
