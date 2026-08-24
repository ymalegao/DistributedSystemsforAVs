#!/usr/bin/env python3

import unittest
from pathlib import Path

from fourway.longitudinal_grid import (
    analyze_run,
    absolute_claim_accept_probability,
    binomial_tail,
    manifest,
    output_dir,
    run_dir,
)


class LongitudinalGridTests(unittest.TestCase):
    def test_full_manifest_is_deduplicated_and_has_186_runs(self):
        rows = manifest("full", 5)
        self.assertEqual(len(rows), 186)
        keys = {(r["reference_separation_m"], r["sigma_long_m"], r["b"], r["rep"]) for r in rows}
        self.assertEqual(len(keys), len(rows))

    def test_cliff_and_shoulder_are_separate_purposes(self):
        rows = manifest("smoke", 5)
        cliff = next(row for row in rows if row["b"] == 6)
        self.assertEqual(cliff["purposes"], ["cliff_plumbing"])
        self.assertTrue(any(row["b"] == 5 and "shoulder_plumbing" in row["purposes"] for row in rows))

    def test_k2_artifacts_are_isolated_from_legacy_k3(self):
        k2 = manifest("smoke", 5, physical_gate_k=2.0)
        self.assertTrue(all(row["physical_gate_k"] == 2.0 for row in k2))
        self.assertEqual(output_dir(Path("/repo"), 3.0), Path("/repo/benchmarks/Phase2DistanceGrid"))
        self.assertEqual(output_dir(Path("/repo"), 2.0), Path("/repo/benchmarks/Phase2DistanceGridK2"))
        self.assertIn("fixture_v3", str(run_dir(Path("/repo"), k2[0])))

    def test_absolute_gate_probability_and_binomial_composition(self):
        q = absolute_claim_accept_probability(-5.1, 1.0, 3.0)
        self.assertGreater(q, 0.0)
        self.assertLess(q, 0.1)
        self.assertAlmostEqual(binomial_tail(11, q, 1), 1.0 - (1.0 - q) ** 11)

    def test_prediction_uses_attempt_available_b_sig_when_no_cert_finalizes(self):
        evaluations = [
            {"target": "veh15", "witness": witness, "stationary": True,
             "residual_cm": 999}
            for witness in range(4, 15)
        ]
        evaluations.append({
            "target": "veh14", "witness": 4, "stationary": True, "residual_cm": 10,
        })
        records = [{"bft_stats": {"perception": {
            "phase2_attack": {
                "configured_byzantine_ids": [0, 1, 2, 3, 15],
                "h_distance": 11, "b_sig_distance": 0, "threshold": 6,
                "distance_certified": False,
                "distance_declaration": {"offset_m": -5.1},
                "distance_certificate_signers": [], "q0_distance": 0.0,
            },
            "stopped_distance": {
                "evaluations": evaluations,
                "collection_order": [
                    {"target": "veh15", "epoch": 0, "signer": signer,
                     "signer_count": index + 2, "threshold": 6}
                    for index, signer in enumerate((0, 1, 2, 3))
                ],
                "order_pairs": [], "order_inversion_count": 0,
                "single_evaluation_invariant_ok": True,
            },
        }}}]
        cell = {"n": 16, "target": 15, "predecessor": 11,
                "reference_separation_m": 5.0, "sigma_long_m": 1.0,
                "b": 5, "rep": 0, "purposes": ["test"]}
        row = analyze_run(cell, records, Path("/tmp/test"), 5)
        self.assertEqual(row["certificate_local_b_sig"], 0)
        self.assertEqual(row["attempt_available_b_sig"], 5)
        self.assertEqual(row["required_honest_support"], 1)
        self.assertAlmostEqual(
            row["protocol_binomial_tail_prediction"],
            1.0 - (1.0 - row["analytic_q_dist"]) ** 11,
        )


if __name__ == "__main__":
    unittest.main()
