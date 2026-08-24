#!/usr/bin/env python3
"""Structural tests for the approved Phase 2 statistical pilot matrix."""

import unittest

import experiment_orchestrator as orchestrator


class Phase2PilotMatrixTest(unittest.TestCase):
    def setUp(self) -> None:
        self.rows = orchestrator._phase2_pilot_manifest(16)

    def test_unique_run_counts(self) -> None:
        self.assertEqual(len(self.rows), 384)
        self.assertEqual(
            {name: sum(row["experiment"] == name for row in self.rows)
             for name in ("e1", "e2", "e3", "e4")},
            {"e1": 50, "e2": 124, "e3": 60, "e4": 150},
        )

    def test_requested_submatrices_survive_deduplication(self) -> None:
        expected = {
            "e2": {"full_grid": 35, "shoulder_b_f": 80,
                   "boundary_b_f_plus_1": 5, "boundary_b_f_minus_1": 10},
            "e4": {"full_grid": 42, "shoulder_b_f": 100,
                   "boundary_b_f_plus_1": 5, "boundary_b_f_minus_1": 10},
        }
        for experiment, purposes in expected.items():
            for purpose, count in purposes.items():
                self.assertEqual(
                    sum(row["experiment"] == experiment and purpose in row["purposes"]
                        for row in self.rows),
                    count,
                )

    def test_grid_profile_is_one_run_per_n16_parameter_cell(self) -> None:
        rows = orchestrator._phase2_pilot_manifest(16, profile="grid")
        self.assertEqual(len(rows), 88)
        self.assertEqual(
            {name: sum(row["experiment"] == name for row in rows)
             for name in ("e1", "e2", "e3", "e4")},
            {"e1": 5, "e2": 35, "e3": 6, "e4": 42},
        )
        self.assertEqual({row["n"] for row in rows}, {16})
        self.assertEqual({row["rep"] for row in rows}, {0})
        self.assertEqual(
            {purpose for row in rows for purpose in row["purposes"]},
            {"honest_sigma", "full_grid", "honest_signal"},
        )

    def test_wilson_interval_contains_observed_rate(self) -> None:
        interval = orchestrator._wilson_interval(3, 10)
        self.assertLessEqual(interval["low"], interval["rate"])
        self.assertGreaterEqual(interval["high"], interval["rate"])
        self.assertEqual(orchestrator._wilson_interval(0, 0)["rate"], None)


if __name__ == "__main__":
    unittest.main()
