#!/usr/bin/env python3
"""Unit and fixture tests for adjacent-lane continuous perception calibration."""

from __future__ import annotations

import json
import math
import os
import unittest
from pathlib import Path

import numpy as np

from .run_calibration import (
    AMBIGUOUS,
    HERE,
    LANE_A,
    LANE_B,
    QUANT_ENVELOPE_M,
    VALIDATION_FAMILY_ALPHA,
    VALIDATION_N_CELLS,
    analytic_accept_prob,
    analytic_quantization_envelope,
    bonferroni_z,
    collect_traci_trace,
    derive_geometry_from_net,
    generate_delta_observations,
    load_manifest,
    project_claimed_lane,
    quantize_1cm,
    quantize_point,
    rethreshold_counts,
    run_calibration,
    validate_quantized_gate_cell,
    wilson_interval,
)

MANIFEST = load_manifest()


class QuantizationTests(unittest.TestCase):
    def test_cm_grid_is_deterministic(self) -> None:
        self.assertEqual(quantize_1cm(1.234), 1.23)
        self.assertEqual(quantize_1cm(1.235), 1.24)  # half away from zero
        self.assertEqual(quantize_1cm(-1.235), -1.24)
        self.assertEqual(quantize_1cm(1.6), 1.60)
        self.assertEqual(quantize_point((10.001, 1.604)), (10.00, 1.60))

    def test_repeated_quantize_is_idempotent(self) -> None:
        for v in (0.0, 1.6, -0.015, 3.199):
            q = quantize_1cm(v)
            self.assertEqual(quantize_1cm(q), q)


class LaneProjectionTests(unittest.TestCase):
    def test_boundary_is_ambiguous(self) -> None:
        self.assertEqual(project_claimed_lane((50.0, 1.6), 0.0, 3.2), AMBIGUOUS)
        self.assertEqual(project_claimed_lane((0.0, 1.60), 0.0, 3.2), AMBIGUOUS)

    def test_true_and_false_sides(self) -> None:
        self.assertEqual(project_claimed_lane((50.0, 1.59), 0.0, 3.2), LANE_A)
        self.assertEqual(project_claimed_lane((50.0, 1.61), 0.0, 3.2), LANE_B)
        self.assertEqual(project_claimed_lane((50.0, 0.0), 0.0, 3.2), LANE_A)
        self.assertEqual(project_claimed_lane((50.0, 3.2), 0.0, 3.2), LANE_B)


class AnalyticTests(unittest.TestCase):
    def test_q1_closed_form(self) -> None:
        from scipy.stats import norm

        for k in (2.0, 2.5, 3.0):
            got = analytic_accept_prob(0.5, 0.0, k)
            expect = 2.0 * norm.cdf(k) - 1.0
            self.assertAlmostEqual(got, expect, places=12)
        self.assertAlmostEqual(analytic_accept_prob(0.5, 0.0, 3.0), 0.99730020394, places=8)

    def test_closed_form_matches_normal_interval(self) -> None:
        from scipy.stats import norm

        sigma, delta, k = 0.5, 1.75, 3.0
        expected = norm.cdf(delta / sigma + k) - norm.cdf(delta / sigma - k)
        self.assertAlmostEqual(
            analytic_accept_prob(sigma, delta, k), expected, places=12
        )

    def test_acceptance_decreases_with_delta(self) -> None:
        prev = 1.0
        for delta in (0.0, 0.5, 1.0, 1.6, 2.0, 3.2):
            p = analytic_accept_prob(0.5, delta, 3.0)
            self.assertLessEqual(p, prev + 1e-12)
            prev = p


class WilsonTests(unittest.TestCase):
    def test_interval_contains_rate(self) -> None:
        iv = wilson_interval(3, 10)
        self.assertLessEqual(iv["low"], iv["rate"])
        self.assertGreaterEqual(iv["high"], iv["rate"])

    def test_bonferroni_is_wider_than_95(self) -> None:
        z_bonf = bonferroni_z()
        iv95 = wilson_interval(50000, 100000)
        ivbonf = wilson_interval(50000, 100000, z=z_bonf)
        self.assertLess(ivbonf["low"], iv95["low"])
        self.assertGreater(ivbonf["high"], iv95["high"])


class ValidationGateTests(unittest.TestCase):
    def test_quantization_envelope_ordering(self) -> None:
        env = analytic_quantization_envelope(0.5, 1.0, 2.5)
        nominal = analytic_accept_prob(0.5, 1.0, 2.5)
        self.assertLessEqual(env["p_low"], nominal)
        self.assertGreaterEqual(env["p_high"], nominal)
        self.assertAlmostEqual(
            env["threshold_high_m"] - env["threshold_low_m"],
            2.0 * QUANT_ENVELOPE_M,
            places=12,
        )

    def test_gate_validation_passes_for_tight_match(self) -> None:
        env = analytic_quantization_envelope(0.5, 0.0, 3.0)
        nominal = analytic_accept_prob(0.5, 0.0, 3.0)
        successes = int(round(nominal * 100000))
        pooled = {
            "successes": successes,
            "trials": 100000,
            "rate": successes / 100000,
        }
        result = validate_quantized_gate_cell(pooled, env, bonferroni_z())
        self.assertTrue(result["pass"])
        self.assertTrue(result["bonferroni_overlaps_envelope"])

    def test_predeclared_family_size(self) -> None:
        self.assertEqual(VALIDATION_N_CELLS, 12 * 3)
        self.assertAlmostEqual(
            VALIDATION_FAMILY_ALPHA / VALIDATION_N_CELLS,
            0.05 / 36.0,
            places=15,
        )


class GeometryFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.geometry = derive_geometry_from_net(
            HERE / MANIFEST["files"]["net"], MANIFEST
        )

    def test_centers_are_3p2_apart(self) -> None:
        self.assertAlmostEqual(self.geometry["lane_center_separation_m"], 3.2, places=12)
        self.assertAlmostEqual(self.geometry["lane_a_center_sample_xy"][1], 0.0, places=12)
        self.assertAlmostEqual(self.geometry["lane_b_center_sample_xy"][1], 3.2, places=12)

    def test_normal_and_boundary(self) -> None:
        self.assertEqual(self.geometry["unit_normal_a_to_b"], [0.0, 1.0])
        self.assertAlmostEqual(self.geometry["boundary_xy"][1], 1.6, places=12)


@unittest.skipUnless(
    os.environ.get("SUMO_HOME")
    or Path("/Users/yashmalegaonkar/Documents/omnet/sumo-1.22.0/bin/sumo").is_file(),
    "SUMO binary not available",
)
class TraCIFixtureTests(unittest.TestCase):
    def test_target_stays_on_lane_a(self) -> None:
        trace = collect_traci_trace(
            HERE / MANIFEST["files"]["sumocfg"],
            MANIFEST["target"]["vehicle_id"],
            MANIFEST["target"]["true_lane_id"],
        )
        self.assertTrue(trace["all_on_expected_lane"])
        self.assertEqual(trace["lanes_seen"], [MANIFEST["target"]["true_lane_id"]])
        self.assertAlmostEqual(trace["y_min"], 0.0, places=9)
        self.assertAlmostEqual(trace["y_max"], 0.0, places=9)


class SamplingTests(unittest.TestCase):
    def test_same_seed_is_identical(self) -> None:
        true_positions = [(10.0, 0.0), (20.0, 0.0)]
        kwargs = dict(
            true_positions=true_positions,
            delta=1.61,
            normal=(0.0, 1.0),
            sigma=0.5,
            seeds=[1000],
            samples_per_seed=500,
            center_a_y=0.0,
            center_b_y=3.2,
            boundary_y=1.6,
        )
        a = generate_delta_observations(**kwargs)
        b = generate_delta_observations(**kwargs)
        np.testing.assert_array_equal(a["distances_m"], b["distances_m"])
        np.testing.assert_array_equal(a["claimed_lanes"], b["claimed_lanes"])

    def test_longitudinal_component_does_not_affect_lateral_residual(self) -> None:
        from .run_calibration import _claim_and_obs

        claim, obs, residual = _claim_and_obs(
            (10.0, 0.0), 1.7, (0.0, 1.0), (25.0, 0.4)
        )
        self.assertEqual(claim, (10.0, 1.7))
        self.assertEqual(obs, (35.0, 0.4))
        self.assertAlmostEqual(residual, 1.3, places=12)

    def test_rethreshold_uses_same_distances(self) -> None:
        dists = np.array([0.4, 1.0, 1.2, 1.6, 2.0])
        pooled = rethreshold_counts(dists, sigma=0.5, k_values=[2.0, 2.5, 3.0])
        # thresholds: 1.0, 1.25, 1.5
        self.assertEqual(pooled["2"]["successes"], 2)   # 0.4, 1.0
        self.assertEqual(pooled["2.5"]["successes"], 3)  # +1.2
        self.assertEqual(pooled["3"]["successes"], 3)    # still <= 1.5; 1.6 excluded

    def test_delta_1p6_claims_are_ambiguous(self) -> None:
        block = generate_delta_observations(
            true_positions=[(25.0, 0.0)],
            delta=1.6,
            normal=(0.0, 1.0),
            sigma=0.5,
            seeds=[1000],
            samples_per_seed=200,
            center_a_y=0.0,
            center_b_y=3.2,
            boundary_y=1.6,
        )
        self.assertEqual(block["example_claimed_lane"], AMBIGUOUS)
        self.assertEqual(block["seeds"][0]["claimed_lane_counts"][AMBIGUOUS], 200)


@unittest.skipUnless(
    os.environ.get("RUN_FULL_ADJACENT_LANE_CAL") == "1",
    "Set RUN_FULL_ADJACENT_LANE_CAL=1 to run the full 100k×12 ROC (slow).",
)
class FullCalibrationTests(unittest.TestCase):
    def test_checkpoint_pass(self) -> None:
        results = HERE / "results"
        summary = run_calibration(results_dir=results, store_bulk=False)
        self.assertTrue(summary["pass_criteria"]["checkpoint_pass"])
        # Canonical file should already exist from the developer run; refresh is optional.
        path = HERE / "canonical_validation_summary.json"
        self.assertTrue(path.is_file())
        checked = json.loads(path.read_text())
        self.assertTrue(checked["pass_criteria"]["checkpoint_pass"])


if __name__ == "__main__":
    unittest.main()
