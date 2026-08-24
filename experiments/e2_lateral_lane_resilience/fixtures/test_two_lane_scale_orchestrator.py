import unittest
import json
import tempfile
from pathlib import Path

import experiment_orchestrator as orchestrator


class TwoLaneScaleOrchestratorTests(unittest.TestCase):
    def test_smoke_profile_remains_four_rows(self):
        rows = orchestrator._two_lane_scale_rows("smoke")
        self.assertEqual(len(rows), 4)
        self.assertEqual({row["n"] for row in rows}, {4, 8, 16, 20})
        self.assertEqual({row["rep"] for row in rows}, {0})

    def test_full_honest_profile_is_eighty_unique_runs(self):
        rows = orchestrator._two_lane_scale_rows("full")
        self.assertEqual(len(rows), 80)
        self.assertEqual({row["n"] for row in rows}, {4, 8, 16, 20})
        self.assertEqual({row["rep"] for row in rows}, set(range(20)))
        self.assertEqual({row["attack_kind"] for row in rows}, {"NONE"})
        self.assertEqual({row["b"] for row in rows}, {0})
        self.assertEqual({row["sigma_lat_m"] for row in rows}, {0.5})
        self.assertEqual({row["sigma_long_m"] for row in rows}, {1.0})
        self.assertEqual({row["signal_error"] for row in rows}, {0.2})
        self.assertEqual({row["k"] for row in rows}, {2.0})
        self.assertEqual(
            len({orchestrator._two_lane_scale_run_dir(row) for row in rows}),
            80,
        )
        self.assertEqual(
            len({
                orchestrator._two_lane_scale_metadata(row)["simulation_seed"]
                for row in rows
            }),
            80,
        )

    def test_direction_aggregate_keeps_failed_attempt_out_of_denominator(self):
        common = {
            "attack_kind": "FALSE_DIRECTION",
            "ablation_mode": "eligibility_on",
            "b": 5,
            "direction_eligibility_enabled": True,
            "all_singleton_scheduling": False,
        }
        valid = {
            **common,
            "passed": True,
            "target_pair_conflicting_cooccupancy": False,
            "false_eligibility": False,
            "target_direction": {"derived": "UNKNOWN", "support": 2, "b_sig": 1},
            "wait_samples_s": [1.0, 2.0],
            "physical_collision_vehicle_count": 0,
            "throughput_veh_per_min": 30.0,
            "mean_batch_size": 1.5,
        }
        failed = {**common, "passed": False, "error": "disk full"}

        aggregate = orchestrator._aggregate_direction_ablation([valid, failed])[0]

        self.assertEqual(aggregate["attempted_runs"], 2)
        self.assertEqual(aggregate["failed_runs"], 1)
        self.assertEqual(aggregate["repetitions"], 1)
        self.assertEqual(aggregate["passing_runs"], 1)
        self.assertEqual(aggregate["unsafe_cooccupancy_rate"], 0.0)
        self.assertEqual(aggregate["mean_throughput_veh_per_min"], 30.0)

    def test_direction_json_compaction_preserves_per_car_and_one_shared_copy(self):
        records = [
            {
                "vehicle_id": replica,
                "durations_ms": {"total_wait_time": replica * 1000},
                "run_metrics": {"throughput_veh_per_s": 0.5},
                "bft_stats": {"perception": {"quiet_count": 0}},
            }
            for replica in range(3)
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "records.json"
            path.write_text(json.dumps(records))

            orchestrator._compact_direction_analyzer_json(path)
            compact = json.loads(path.read_text())

        self.assertEqual(len(compact), 3)
        self.assertIn("run_metrics", compact[0])
        self.assertIn("bft_stats", compact[0])
        self.assertNotIn("run_metrics", compact[1])
        self.assertNotIn("bft_stats", compact[1])
        self.assertEqual(compact[2]["durations_ms"]["total_wait_time"], 2000)


if __name__ == "__main__":
    unittest.main()
