import unittest
from pathlib import Path

from fourway import adjacent_lane_grid as grid


class AdjacentLaneGridTest(unittest.TestCase):
    def setUp(self):
        self.rows = grid.manifest()

    def test_locked_unique_count(self):
        self.assertEqual(len(self.rows), 400)
        keys = {
            (r["b"], r["sigma_lat_m"], r["delta_m"], r["k"], r["rep"])
            for r in self.rows
        }
        self.assertEqual(len(keys), 400)

    def test_locked_axes(self):
        sigma_rows = [r for r in self.rows if "sigma_sensitivity_b_f" in r["purposes"]]
        self.assertEqual({r["sigma_lat_m"] for r in sigma_rows}, set(grid.SIGMA_SENSITIVITY_M))
        self.assertEqual(len(sigma_rows), 8 * 20)

        delta_rows = [r for r in self.rows if "delta_shoulder_b_f" in r["purposes"]]
        self.assertEqual({r["delta_m"] for r in delta_rows}, set(grid.DELTA_SHOULDER_M))
        self.assertEqual(len(delta_rows), 5 * 20)

        boundary = [r for r in self.rows if "byzantine_boundary" in r["purposes"]]
        self.assertEqual({r["b"] for r in boundary}, set(grid.BYZANTINE_BOUNDARY_B))
        self.assertEqual(len(boundary), 6 * 20)

        k_rows = [r for r in self.rows if "k_tradeoff" in r["purposes"]]
        self.assertEqual({r["k"] for r in k_rows}, set(grid.K_TRADEOFF))
        self.assertEqual(len(k_rows), 3 * 20)

    def test_overlap_is_deduplicated(self):
        headline = [
            r for r in self.rows
            if r["b"] == 5 and r["sigma_lat_m"] == 0.5 and
               r["delta_m"] == 1.75 and r["k"] == 3.0
        ]
        self.assertEqual(len(headline), 20)
        self.assertEqual(
            set(headline[0]["purposes"]),
            {"delta_shoulder_b_f", "sigma_sensitivity_b_f", "k_tradeoff"},
        )

    def test_nested_colluders(self):
        self.assertEqual(grid.nested_colluders(0), [])
        self.assertEqual(grid.nested_colluders(1), [])
        self.assertEqual(grid.nested_colluders(5), [1, 2, 3, 4])
        self.assertEqual(grid.nested_colluders(6), [1, 2, 3, 4, 5])

    def test_model_reference(self):
        self.assertAlmostEqual(grid.q0_model(0.5, 1.75, 3.0), 0.308537538685827)
        self.assertEqual(grid.q0_model(0.0, 1.75, 3.0), 0.0)

    def test_delta_b_smoke_is_exactly_eighteen_attack_cells(self):
        rows = grid.delta_b_manifest("smoke")
        self.assertEqual(len(rows), 18)
        self.assertEqual({row["b"] for row in rows}, set(range(1, 7)))
        self.assertEqual({row["delta_m"] for row in rows}, {1.61, 1.75, 2.0})
        self.assertEqual({row["rep"] for row in rows}, {0})
        self.assertTrue(all(row["b"] > 0 for row in rows))

    def test_delta_b_full_has_360_attacks_and_one_control(self):
        rows = grid.delta_b_manifest("full")
        attacks = [row for row in rows if row["b"] > 0]
        controls = [row for row in rows if row["b"] == 0]
        self.assertEqual(len(attacks), 360)
        self.assertEqual(len(controls), 1)
        self.assertEqual(controls[0]["delta_m"], 0.0)
        self.assertEqual(
            len({(row["b"], row["delta_m"], row["rep"]) for row in attacks}),
            360,
        )

    def test_k_sweep_sizes_and_axes(self):
        smoke = grid.parameter_sweep_manifest("k", "smoke")
        full = grid.parameter_sweep_manifest("k", "full")
        self.assertEqual(len(smoke), 18)
        self.assertEqual(len(full), 360)
        self.assertEqual({row["k"] for row in full}, {1.0, 2.0, 3.0})
        self.assertEqual({row["sigma_lat_m"] for row in full}, {0.5})
        self.assertEqual({row["delta_m"] for row in full}, {1.75})
        self.assertEqual({row["b"] for row in full}, set(range(1, 7)))

    def test_sigma_sweep_sizes_and_axes(self):
        smoke = grid.parameter_sweep_manifest("sigma", "smoke")
        full = grid.parameter_sweep_manifest("sigma", "full")
        self.assertEqual(len(smoke), 42)
        self.assertEqual(len(full), 840)
        self.assertEqual(
            {row["sigma_lat_m"] for row in full},
            {0.1, 0.3, 0.5, 0.7, 1.0, 1.5, 2.0},
        )
        self.assertEqual({row["k"] for row in full}, {3.0})
        self.assertEqual({row["delta_m"] for row in full}, {1.75})

    def test_parameter_sweeps_share_identical_anchor_rows(self):
        k_rows = {
            grid._key(row): row
            for row in grid.parameter_sweep_manifest("k", "full")
            if "parameter_sweep_shared_anchor" in row["purposes"]
        }
        sigma_rows = {
            grid._key(row): row
            for row in grid.parameter_sweep_manifest("sigma", "full")
            if "parameter_sweep_shared_anchor" in row["purposes"]
        }
        self.assertEqual(k_rows, sigma_rows)
        self.assertEqual(len(k_rows), 6 * 20)
        for key in k_rows:
            self.assertEqual(
                grid.parameter_sweep_run_dir(Path("/repo"), k_rows[key], "k"),
                grid.parameter_sweep_run_dir(Path("/repo"), sigma_rows[key], "sigma"),
            )

    def test_honest_operating_sweep_has_zero_delta(self):
        smoke = grid.honest_operating_manifest("smoke")
        k_full = grid.honest_operating_manifest("k-full")
        full = grid.honest_operating_manifest("full")
        self.assertEqual(len(smoke), 24)
        self.assertEqual(len(k_full), 60)
        self.assertEqual(len(full), 480)
        self.assertEqual({row["sigma_lat_m"] for row in k_full}, {0.5})
        self.assertEqual({row["b"] for row in full}, {0})
        self.assertEqual({row["delta_m"] for row in full}, {0.0})
        self.assertEqual({row["k"] for row in full}, {1.0, 2.0, 3.0})
        self.assertEqual(
            {row["sigma_lat_m"] for row in full},
            {0.0, 0.1, 0.3, 0.5, 0.7, 1.0, 1.5, 2.0},
        )

    def test_q1_model_is_sigma_normalized(self):
        self.assertEqual(grid.q1_model(0.0, 1.0), 1.0)
        self.assertAlmostEqual(grid.q1_model(0.1, 1.0), 0.6826894921370859)
        self.assertAlmostEqual(grid.q1_model(2.0, 1.0), 0.6826894921370859)
        self.assertAlmostEqual(grid.q1_model(0.5, 3.0), 0.9973002039367398)

    def test_combined_validation_locks_joint_operating_point(self):
        rows = grid.combined_validation_manifest()
        self.assertEqual(len(rows), 3)
        self.assertEqual({row["b"] for row in rows}, {0, grid.F, grid.F + 1})
        self.assertEqual({row["sigma_lat_m"] for row in rows}, {0.5})
        self.assertEqual({row["sigma_long_m"] for row in rows}, {1.0})
        self.assertEqual({row["k"] for row in rows}, {2.0})
        self.assertEqual(
            {row["delta_m"] for row in rows if row["b"] > 0}, {1.75}
        )
        self.assertEqual(
            len({grid.combined_validation_run_dir(Path("/repo"), row) for row in rows}),
            3,
        )

    def test_direction_ablation_is_three_way_for_honest_and_attack(self):
        rows = grid.direction_ablation_manifest("smoke")
        self.assertEqual(len(rows), 6)
        self.assertEqual(
            {row["ablation_mode"] for row in rows},
            {"eligibility_on", "eligibility_off", "all_singleton"},
        )
        self.assertEqual({row["attack_kind"] for row in rows}, {"NONE", "FALSE_DIRECTION"})
        self.assertEqual({row["signal_error"] for row in rows}, {0.2})
        self.assertTrue(all(row["sigma_long_m"] == 1.0 for row in rows))

    def test_direction_ablation_full_has_twenty_paired_repetitions(self):
        rows = grid.direction_ablation_manifest("full")
        self.assertEqual(len(rows), 120)
        self.assertEqual({row["rep"] for row in rows}, set(range(20)))
        self.assertEqual(
            len({(row["attack_kind"], row["ablation_mode"], row["rep"]) for row in rows}),
            120,
        )
        for rep in range(20):
            paired = [row for row in rows if row["rep"] == rep]
            self.assertEqual(len(paired), 6)
            self.assertEqual({row["sigma_lat_m"] for row in paired}, {0.5})
            self.assertEqual({row["sigma_long_m"] for row in paired}, {1.0})
            self.assertEqual({row["k"] for row in paired}, {2.0})

    def test_aggregate_separates_honest_q1_from_attack_q0(self):
        common = {
            "sigma_lat_m": 0.5, "delta_m": 1.75, "k": 3.0,
            "false_lane_certificate": False,
            "conflicting_cooccupancy": False,
            "gate_induced_conflicting_cooccupancy": False,
            "background_conflicting_cooccupancy": False,
            "binomial_tail_prediction": None,
            "b_sig_cert": 0, "b_sig_attempt": None,
        }
        control = {**common, "b": 0, "honest_lane_accepts": 11, "h_lane": 11}
        attack = {
            **common, "b": 1, "honest_lane_accepts": 5, "h_lane": 15,
            "b_sig_cert": 1, "b_sig_attempt": 1,
            "binomial_tail_prediction": 0.5,
        }
        by_b = {row["b"]: row for row in grid.aggregate([control, attack])}
        self.assertEqual(by_b[0]["claim_kind"], "HONEST_CONTROL")
        self.assertEqual(by_b[0]["effective_delta_m"], 0.0)
        self.assertIsNone(by_b[0]["pooled_q0_empirical"])
        self.assertEqual(by_b[0]["pooled_q1_empirical"], 1.0)
        self.assertEqual(by_b[1]["claim_kind"], "FALSE_PHYSICAL_LANE")
        self.assertAlmostEqual(by_b[1]["pooled_q0_empirical"], 5 / 15)
        self.assertIsNone(by_b[1]["pooled_q1_empirical"])
        self.assertEqual(by_b[1]["mean_b_sig_attempt"], 1.0)


if __name__ == "__main__":
    unittest.main()
