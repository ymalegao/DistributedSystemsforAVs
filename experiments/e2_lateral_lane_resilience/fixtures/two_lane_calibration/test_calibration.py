#!/usr/bin/env python3
"""Acceptance tests for Checkpoint 1 two-lane fixture + lateral gate."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from lateral_gate import (
    analytic_q0,
    lateral_gate_accept,
    project_physical_lane_index,
    quantize_cm,
)
from run_calibration import (
    ATTACKER_DELTA_M,
    HERE,
    NET,
    SEPARATION_M,
    build_cpp_ref,
    derive_n2c_geometry,
    run_parity_cases,
    run_sigma0_exactness,
    validate_geometry_with_traci,
)

SUMMARY = HERE / "results" / "canonical_validation_summary.json"


class GateUnitTests(unittest.TestCase):
    def test_quantize_matches_half_away(self) -> None:
        self.assertEqual(quantize_cm(1.234), 123)
        self.assertEqual(quantize_cm(1.235), 124)
        self.assertEqual(quantize_cm(-1.235), -124)
        self.assertEqual(quantize_cm(0.005), 1)
        self.assertEqual(quantize_cm(0.004), 0)

    def test_sigma0_exactness_unit(self) -> None:
        self.assertTrue(lateral_gate_accept(0.0, 0.0, 0.0, 3.0)["accept"])
        self.assertFalse(lateral_gate_accept(0.0, 0.01, 0.0, 3.0)["accept"])

    def test_project_lane_indices(self) -> None:
        self.assertEqual(project_physical_lane_index(0), 0)
        self.assertEqual(project_physical_lane_index(159), 0)
        self.assertEqual(project_physical_lane_index(160), -1)
        self.assertEqual(project_physical_lane_index(161), 1)
        self.assertEqual(project_physical_lane_index(320), 1)

    def test_analytic_q1(self) -> None:
        self.assertAlmostEqual(analytic_q0(0.5, 0.0, 3.0), 0.9973002039367398, places=10)

    def test_analytic_monotonic_in_delta(self) -> None:
        prev = 1.0
        for d in ATTACKER_DELTA_M:
            p = analytic_q0(0.5, d, 3.0)
            self.assertLess(p, prev)
            prev = p


class ParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpp = build_cpp_ref()

    def test_python_cpp_identical(self) -> None:
        result = run_parity_cases(self.cpp)
        self.assertTrue(result["pass"], msg=json.dumps(result["cases"], indent=2))


class GeometryTests(unittest.TestCase):
    def test_net_exists(self) -> None:
        self.assertTrue(NET.is_file())

    def test_n2c_separation_and_mapping(self) -> None:
        geo = derive_n2c_geometry()
        self.assertAlmostEqual(geo["lane_center_separation_m"], SEPARATION_M, places=6)
        self.assertEqual(geo["lane_index_T"], 0)
        self.assertEqual(geo["lane_index_L"], 1)
        self.assertEqual(geo["lane_T_id"], "N2C_0")
        self.assertEqual(geo["lane_L_id"], "N2C_1")

    def test_traci_movements_and_no_teleport(self) -> None:
        result = validate_geometry_with_traci()
        self.assertTrue(result["outer_T_went_straight"], msg=result)
        self.assertTrue(result["inner_L_went_left"], msg=result)
        self.assertEqual(result["teleport_starts"], 0)
        self.assertEqual(result["pins"]["released_speed_mode"], 0)
        self.assertEqual(result["pins"]["time_to_teleport"], -1)


class Sigma0Tests(unittest.TestCase):
    def test_batch_exactness(self) -> None:
        r = run_sigma0_exactness()
        self.assertTrue(r["pass"], msg=r)


@unittest.skipUnless(SUMMARY.is_file(), "run run_calibration.py first for full acceptance")
class FullAcceptanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.summary = json.loads(SUMMARY.read_text())

    def test_overall_pass(self) -> None:
        self.assertTrue(self.summary["acceptance"]["pass"])

    def test_each_numbered_gate(self) -> None:
        for name, t in self.summary["acceptance"]["tests"].items():
            self.assertTrue(t["pass"], msg=f"{name}: {t}")


if __name__ == "__main__":
    unittest.main()
