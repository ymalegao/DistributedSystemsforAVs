#!/usr/bin/env python3

import math
import os
import unittest
from pathlib import Path

import numpy as np

from .run_calibration import (
    false_claim_probability,
    load_manifest,
    quantize_cm,
    quantize_cm_array,
    raw_inversion_probability,
    collect_stopped_queue,
)


class FormulaTests(unittest.TestCase):
    def test_honest_gate_probability(self):
        from scipy.stats import norm
        self.assertAlmostEqual(false_claim_probability(0.0, 0.5, 3.0), 2 * norm.cdf(3) - 1, places=12)

    def test_false_accept_decreases_with_offset(self):
        values = [false_claim_probability(d, 0.5, 3.0) for d in (0, 0.5, 1, 2, 3)]
        self.assertEqual(values, sorted(values, reverse=True))

    def test_pair_inversion_uses_reference_separation(self):
        self.assertGreater(raw_inversion_probability(5.0, 2.0), 0.0)
        self.assertLess(raw_inversion_probability(5.0, 1.0), raw_inversion_probability(5.0, 2.0))

    def test_pair_inversion_zero_separation_is_half(self):
        for sigma in (0.25, 0.5, 1.0, 2.0):
            self.assertAlmostEqual(raw_inversion_probability(0.0, sigma), 0.5, places=12)

    def test_pair_inversion_falls_with_reference_separation(self):
        values = [raw_inversion_probability(s, 1.0) for s in (0.0, 5.0, 5.5, 6.5, 7.5, 9.5, 12.5)]
        self.assertEqual(values, sorted(values, reverse=True))

    def test_pair_inversion_large_separation_tends_to_zero(self):
        for sigma in (0.25, 0.5, 1.0, 2.0):
            self.assertLess(raw_inversion_probability(100.0, sigma), 1e-100)


class QuantizationTests(unittest.TestCase):
    def test_half_away_from_zero(self):
        self.assertEqual(quantize_cm(1.235), 1.24)
        self.assertEqual(quantize_cm(-1.235), -1.24)
        np.testing.assert_array_equal(quantize_cm_array(np.array([1.235, -1.235])), np.array([1.24, -1.24]))


@unittest.skipUnless(
    os.environ.get("SUMO_HOME") or Path("/Users/yashmalegaonkar/Documents/omnet/sumo-1.22.0/bin/sumo").is_file(),
    "SUMO unavailable",
)
class SumoFixtureTests(unittest.TestCase):
    def test_stopped_queue_geometry(self):
        manifest = load_manifest()
        trace = collect_stopped_queue(manifest)
        self.assertTrue(trace["all_expected_lane"])
        self.assertLessEqual(trace["max_abs_speed_mps"], 1e-6)
        self.assertLessEqual(trace["max_position_span_m"], 1e-6)
        for actual, expected in zip(trace["front_reference_separations_m"], manifest["geometry"]["expected_front_reference_separations_m"]):
            self.assertAlmostEqual(actual, expected, places=2)


if __name__ == "__main__":
    unittest.main()
