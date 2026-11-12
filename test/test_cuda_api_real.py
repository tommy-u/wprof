#!/usr/bin/env python3
import unittest

from cuda_api_real import case_schedule, summarize_samples


class TimingTest(unittest.TestCase):
    def test_summary_keeps_samples_and_distinguishes_the_two_ratio_medians(self):
        summary = summarize_samples({
            'baseline': [{'elapsed_ns': 10, 'pass': True}, {'elapsed_ns': 20, 'pass': True}],
            'plain': [{'elapsed_ns': 20, 'attach_ns': 10, 'pass': True},
                      {'elapsed_ns': 60, 'attach_ns': 20, 'pass': False}],
        })
        plain = summary['plain']
        self.assertEqual([sample['elapsed_ns'] for sample in plain['samples']], [20, 60])
        self.assertEqual(plain['median_elapsed_ns'], 40)
        self.assertEqual(plain['dispersion_ns']['mad'], 20)
        self.assertEqual(plain['ratio_samples'], [2, 3])
        self.assertAlmostEqual(plain['elapsed_ratio'], 40 / 15)
        self.assertEqual(plain['median_elapsed_ratio'], 2.5)
        self.assertFalse(plain['pass'])

    def test_seed_replays_the_case_order_and_keeps_warmups_separate(self):
        schedule = case_schedule(2, 0)
        self.assertEqual(schedule, case_schedule(2, 0))
        self.assertEqual(schedule[:4], [
            ('warmup', 1, 'stacks'), ('warmup', 1, 'baseline'),
            ('warmup', 1, 'plain'), ('warmup', 1, 'sealed'),
        ])
        self.assertEqual(len([item for item in schedule if item[0] == 'sample']), 8)


if __name__ == '__main__':
    unittest.main()
