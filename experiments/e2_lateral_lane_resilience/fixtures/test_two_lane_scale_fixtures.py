import unittest
from collections import Counter

from fourway.generate_two_lane_scale_fixtures import FIXTURES, manifest_rows


class TwoLaneScaleFixtureTests(unittest.TestCase):
    def test_locked_scales_and_counts(self):
        self.assertEqual(set(FIXTURES), {4, 8, 16, 20})
        for n, fixture in FIXTURES.items():
            self.assertEqual(len(fixture), n)
            self.assertEqual({item[0] for item in fixture}, set(range(n)))

    def test_balanced_approaches_and_proportional_maneuvers(self):
        for n in FIXTURES:
            rows = list(manifest_rows(n))
            self.assertEqual(
                Counter(row["intended_lane"] for row in rows),
                Counter({approach: n // 4 for approach in "NSEW"}),
            )
            self.assertEqual(
                Counter(row["intended_direction"] for row in rows),
                Counter({"L": n // 2, "S": n // 4, "R": n // 4}),
            )

    def test_physical_lane_policy(self):
        for n in FIXTURES:
            for row in manifest_rows(n):
                self.assertEqual(
                    row["sumo_lane_index"] == 1,
                    row["intended_direction"] == "L",
                )

    def test_reviewed_conflict_pair_is_scale_invariant(self):
        for n in FIXTURES:
            rows = {row["vehicle_id"]: row for row in manifest_rows(n)}
            self.assertEqual(
                (rows["veh0"]["intended_lane"], rows["veh0"]["intended_direction"]),
                ("N", "L"),
            )
            self.assertEqual(
                (rows["veh1"]["intended_lane"], rows["veh1"]["intended_direction"]),
                ("S", "S"),
            )

    def test_node_indices_follow_lexicographic_sumo_spawn_order(self):
        for n in FIXTURES:
            rows = list(manifest_rows(n))
            expected = {
                vehicle: index
                for index, vehicle in enumerate(sorted(row["vehicle_id"] for row in rows))
            }
            self.assertEqual(
                {row["vehicle_id"]: row["node_index"] for row in rows}, expected
            )


if __name__ == "__main__":
    unittest.main()
