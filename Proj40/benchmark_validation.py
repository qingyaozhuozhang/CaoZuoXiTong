#!/usr/bin/env python3
"""Offline regression checks for benchmark evidence and submission gates."""

import copy
import unittest

from benchmark import MODES, assess_samples, parse_output


def valid_samples():
    samples = []
    for round_number in (1, 2):
        for mode in MODES:
            samples.append(dict(mode=mode, status='PASS', round=round_number,
                                iterations=1000, total_ns=200000, ns_per_call=200.0,
                                events=1000 * {'baseline': 0, 'inert': 0, 'entry': 1,
                                               'return': 2, 'uprobe': 1}[mode],
                                drops=0, missed=0, stops=0, attach_ms=1.0,
                                insert_ms=1.0, remove_ms=1.0, detach_ms=1.0,
                                cleanup_ms=2.1))
    return samples


class BenchmarkEvidenceValidation(unittest.TestCase):
    def test_complete_native_measurement_passes(self):
        result = assess_samples(valid_samples(), 2, 1000, require_kernel=True)
        self.assertTrue(result['passed'])
        self.assertTrue(all(result['checks'].values()))

    def test_missing_kernel_is_visible_and_strict_mode_fails(self):
        samples = [s for s in valid_samples() if s['mode'] != 'uprobe']
        optional = assess_samples(samples, 2, 1000)
        self.assertTrue(optional['passed'])
        self.assertFalse(optional['kernel_comparison_complete'])
        self.assertFalse(assess_samples(samples, 2, 1000, require_kernel=True)['passed'])

    def test_duplicate_round_cannot_replace_missing_round(self):
        samples = valid_samples()
        samples[5]['round'] = 1  # Same number of baseline rows, wrong round identities.
        result = assess_samples(samples, 2, 1000)
        self.assertFalse(result['checks']['sample_integrity'])
        self.assertFalse(result['passed'])

    def test_incomplete_kernel_rounds_are_not_a_complete_comparison(self):
        samples = valid_samples()[:-1]
        result = assess_samples(samples, 2, 1000, require_kernel=True)
        self.assertFalse(result['kernel_comparison_complete'])
        self.assertFalse(result['passed'])

    def test_cleanup_includes_final_session_release(self):
        samples = valid_samples()
        samples[2].update(remove_ms=3.0, detach_ms=8.0, cleanup_ms=11.1)
        result = assess_samples(samples, 2, 1000)
        self.assertTrue(result['checks']['removal_latency'])
        self.assertFalse(result['checks']['complete_cleanup_latency'])
        self.assertFalse(result['performance'])
        self.assertFalse(result['passed'])

    def test_thresholds_are_strict(self):
        for field, value in (('insert_ms', 10.0), ('remove_ms', 10.0),
                             ('cleanup_ms', 10.0), ('ns_per_call', 1200.0)):
            with self.subTest(field=field):
                samples = valid_samples()
                for sample in samples:
                    if sample['mode'] == 'entry':
                        sample[field] = value
                self.assertFalse(assess_samples(samples, 2, 1000)['performance'])

    def test_pass_label_cannot_hide_bad_event_counters(self):
        for field, value in (('events', 999), ('drops', 1), ('missed', 1), ('stops', 1)):
            with self.subTest(field=field):
                samples = valid_samples()
                samples[2][field] = value
                self.assertFalse(assess_samples(samples, 2, 1000)['correctness'])

    def test_wrong_iteration_count_or_process_failure_fails(self):
        samples = valid_samples()
        samples[0]['iterations'] = 999
        self.assertFalse(assess_samples(samples, 2, 1000)['passed'])
        self.assertFalse(assess_samples(valid_samples(), 2, 1000, returncode=1)['passed'])

    def test_negative_paired_delta_remains_measurement_noise(self):
        samples = valid_samples()
        for sample in samples:
            if sample['mode'] == 'inert':
                sample['ns_per_call'] -= 0.2
        result = assess_samples(samples, 2, 1000)
        self.assertTrue(result['passed'])
        self.assertAlmostEqual(result['paired_overhead_ns']['inert'], -0.2)

    def test_parser_rejects_missing_cleanup_and_nonfinite_or_negative_values(self):
        original = valid_samples()[0]
        mutations = [dict(cleanup_ms=None), dict(ns_per_call='nan'),
                     dict(cleanup_ms='inf'), dict(events=-1)]
        for changes in mutations:
            with self.subTest(changes=changes):
                sample = copy.copy(original)
                sample.update(changes)
                output = 'SAMPLE ' + ' '.join(f'{key}={value}' for key, value in sample.items()
                                             if value is not None)
                samples, _, errors = parse_output(output)
                self.assertEqual(samples, [])
                self.assertEqual(len(errors), 1)
                self.assertFalse(assess_samples([], 2, 1000, parse_errors=errors)['passed'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
