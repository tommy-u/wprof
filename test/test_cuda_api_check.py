#!/usr/bin/env python3
"""Fixed raw observations exercise the independent CUDA workload contract."""
import copy
import json
import unittest

from cuda_api_check import check_rows, has_recorded_loss


MANIFEST = json.loads('''{
    "version": 1, "caller": "coverage_run", "count_policy": "exact",
    "calls": [
        {"name": "cuDeviceGetAttribute", "count": 1, "status": "i32", "returns": {"-3": 1}},
        {"name": "cudaGetErrorString", "count": 1, "status": "none"}
    ]
}''')
ROWS = json.loads('''[
    {"stack_cnt": 1, "metadata": {}},
    {"id": 1, "frames": ["libcuda.so!cuDeviceGetAttribute", "fixture!coverage_run+0x12"]},
    {"t": "utrace_entry", "name": "cuDeviceGetAttribute", "ts": 1, "task": {"pid": 7, "tid": 8}, "stack_id": 1},
    {"t": "utrace_exit", "name": "cuDeviceGetAttribute", "ts": 2, "task": {"pid": 7, "tid": 8}, "args": {"status": -3}},
    {"t": "utrace_entry", "name": "cudaGetErrorString", "ts": 3, "task": {"pid": 7, "tid": 8}, "stack_id": 1},
    {"t": "utrace_exit", "name": "cudaGetErrorString", "ts": 4, "task": {"pid": 7, "tid": 8}}
]''')


class CheckTest(unittest.TestCase):
    def check(self, rows=ROWS, manifest=MANIFEST, **options):
        return check_rows(rows, 7, manifest, recorded_loss=False, **options)

    def test_no_status_return_does_not_require_a_scalar(self):
        result = self.check(stacks=True)
        self.assertTrue(result['pass'], result['errors'])
        self.assertEqual(result['records'], 4)
        self.assertEqual([pair['status'] for pair in result['pairs']], [-3, None])

    def test_missing_typed_call_fails(self):
        result = self.check(ROWS[:4])
        self.assertFalse(result['pass'])
        self.assertEqual(result['missing_calls'], [
            {'name': 'cudaGetErrorString', 'expected': 1, 'observed': 0}
        ])

    def test_distinct_configured_names_keep_independent_counts_and_pairs(self):
        manifest = json.loads('''{
            "version": 1, "count_policy": "exact", "caller": "coverage_run",
            "calls": [
                {"name": "cuDeviceGetAttribute", "count": 1, "status": "i32", "returns": {"-3": 1}},
                {"name": "cuDeviceGetAttribute_v2", "count": 1, "status": "i32", "returns": {"-3": 1}}
            ]
        }''')
        rows = copy.deepcopy(ROWS[:4])
        alias_entry, alias_exit = copy.deepcopy(rows[2:])
        alias_entry['name'] = alias_exit['name'] = 'cuDeviceGetAttribute_v2'
        rows = [*rows[:3], alias_entry, rows[3], alias_exit]
        result = self.check(rows, manifest)
        self.assertTrue(result['pass'], result['errors'])
        self.assertEqual([pair['name'] for pair in result['pairs']],
                         ['cuDeviceGetAttribute', 'cuDeviceGetAttribute_v2'])

    def test_status_on_a_no_status_return_fails(self):
        rows = copy.deepcopy(ROWS)
        rows[-1]['args'] = {'status': 0}
        result = self.check(rows)
        self.assertFalse(result['pass'])
        self.assertIn('status present on no-status return cudaGetErrorString', result['errors'])

    def test_wrong_status_fails_even_when_entry_and_return_counts_match(self):
        rows = copy.deepcopy(ROWS)
        rows[3]['args']['status'] = 0
        result = self.check(rows)
        self.assertFalse(result['pass'])
        self.assertIn('unexpected status 0 for cuDeviceGetAttribute', result['errors'])

    def test_per_thread_timestamp_order_is_checked_without_resorting(self):
        rows = copy.deepcopy(ROWS)
        rows[-1]['ts'] = 2
        result = self.check(rows)
        self.assertFalse(result['pass'])
        self.assertIn('per-thread timestamp order for TID 8', result['errors'])

    def test_balanced_counts_do_not_hide_an_unmatched_site_return(self):
        rows = [*ROWS[:2], {**ROWS[3], 'ts': 1}, {**ROWS[2], 'ts': 2}, *ROWS[4:]]
        result = self.check(rows)
        self.assertFalse(result['pass'])
        self.assertIn('unmatched return cuDeviceGetAttribute for TID 8', result['errors'])

    def test_recursive_entries_pair_with_returns_in_site_lifo_order(self):
        manifest = json.loads('''{
            "version": 1, "count_policy": "exact",
            "calls": [{"name": "cuDeviceGetAttribute", "count": 2,
                       "status": "i32", "returns": {"-3": 2}}]
        }''')
        rows = [*ROWS[:2], {**ROWS[2], 'ts': 1}, {**ROWS[2], 'ts': 2},
                {**ROWS[3], 'ts': 3}, {**ROWS[3], 'ts': 4}]
        result = self.check(rows, manifest)
        self.assertTrue(result['pass'], result['errors'])
        self.assertEqual([(pair['entry_ts'], pair['exit_ts']) for pair in result['pairs']], [(2, 3), (1, 4)])

    def test_missing_original_caller_fails(self):
        rows = copy.deepcopy(ROWS)
        rows[1]['frames'] = ['libcuda.so!cuDeviceGetAttribute']
        result = self.check(rows, stacks=True)
        self.assertFalse(result['pass'])
        self.assertIn('missing original coverage_run caller for cuDeviceGetAttribute', result['errors'])

    def test_recorded_loss_cannot_produce_a_pass(self):
        result = check_rows(ROWS, 7, MANIFEST, recorded_loss=True)
        self.assertFalse(result['pass'])
        self.assertTrue(result['recorded_loss'])
        self.assertIn('recorded loss makes pairing unreliable', result['errors'])

    def test_generic_replay_diagnostics_report_recorded_loss(self):
        for text in ('!!! Drops (TOTAL): 1', '!!! Drops (RB #0): 2', '!!! Drops (CPU #1): 3',
                     '!!! Ringbuf fetch misses: 1', '!!! Task state drops: 1',
                     '!!! utrace_exit: 1 recursion misses!'):
            with self.subTest(text=text):
                self.assertTrue(has_recorded_loss(text))
        self.assertFalse(has_recorded_loss('TOTAL 10 records processed, 0 rescued, 0 dropped.\n'
                                          'Task state drops: 0, storage fallbacks: 1\n'))


if __name__ == '__main__':
    unittest.main()
