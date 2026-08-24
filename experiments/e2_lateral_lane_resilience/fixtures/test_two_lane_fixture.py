from pathlib import Path
import sys
import unittest


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from validate_two_lane_fixture import load_manifest, static_validation  # noqa: E402


class TwoLaneFixtureStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.rows = load_manifest()
        cls.result = static_validation(cls.rows)

    def test_static_fixture_is_valid(self) -> None:
        self.assertTrue(self.result["pass"], self.result["errors"])

    def test_all_movements_and_both_lanes_are_covered(self) -> None:
        self.assertEqual(self.result["movement_count"], 12)
        self.assertEqual(self.result["lane_counts"], {0: 8, 1: 8})

    def test_target_and_counterpart_are_present(self) -> None:
        by_id = {row["vehicle_id"]: row for row in self.rows}
        self.assertEqual(
            (by_id["veh0"]["intended_lane"], by_id["veh0"]["intended_direction"],
             by_id["veh0"]["sumo_lane_index"]),
            ("N", "L", 1),
        )
        self.assertTrue(
            any(
                row["intended_lane"] == "S"
                and row["intended_direction"] == "S"
                and row["sumo_lane_index"] == 0
                for row in self.rows
            )
        )


if __name__ == "__main__":
    unittest.main()
