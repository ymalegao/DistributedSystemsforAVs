import csv
import re
import unittest
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

from fourway import adjacent_lane_grid as grid
from fourway import generate_direction_ablation_fixtures as fixtures


HERE = Path(__file__).resolve().parent


class DirectionAblationFixtureTests(unittest.TestCase):
    def test_all_variants_have_locked_composition_and_pair(self):
        for fixture_id in range(fixtures.FIXTURE_COUNT):
            rows = fixtures.fixture_rows(fixture_id)
            self.assertEqual(len(rows), 16)
            self.assertEqual(
                Counter(row["intended_direction"] for row in rows),
                Counter({"S": 8, "L": 4, "R": 4}),
            )
            for approach in "NSEW":
                self.assertEqual(
                    Counter(
                        row["intended_direction"] for row in rows
                        if row["intended_lane"] == approach
                    ),
                    Counter({"S": 2, "L": 1, "R": 1}),
                )
            by_vehicle = {row["vehicle_id"]: row for row in rows}
            self.assertEqual(
                (by_vehicle["veh0"]["intended_lane"],
                 by_vehicle["veh0"]["intended_direction"]),
                ("N", "L"),
            )
            self.assertEqual(
                (by_vehicle["veh1"]["intended_lane"],
                 by_vehicle["veh1"]["intended_direction"]),
                ("S", "S"),
            )
            for row in rows:
                self.assertEqual(
                    row["sumo_lane_index"],
                    1 if row["intended_direction"] == "L" else 0,
                )

    def test_checked_in_route_and_manifest_match_generator(self):
        for fixture_id in range(fixtures.FIXTURE_COUNT):
            expected = {
                row["vehicle_id"]: row for row in fixtures.fixture_rows(fixture_id)
            }
            manifest_path = (
                HERE / f"direction_ablation_straight_heavy_{fixture_id}_manifest.csv"
            )
            with manifest_path.open() as handle:
                manifest = {row["vehicle_id"]: row for row in csv.DictReader(handle)}
            self.assertEqual(set(manifest), set(expected))
            route_root = ET.parse(
                HERE / f"bft_16veh_direction_ablation_{fixture_id}.rou.xml"
            ).getroot()
            vehicles = {item.attrib["id"]: item.attrib for item in route_root.findall("vehicle")}
            self.assertEqual(set(vehicles), set(expected))
            for vehicle, row in expected.items():
                self.assertEqual(
                    vehicles[vehicle]["route"],
                    fixtures.route_id(
                        str(row["intended_lane"]), str(row["intended_direction"])
                    ),
                )
                self.assertEqual(
                    int(vehicles[vehicle]["departLane"]), row["sumo_lane_index"]
                )
                self.assertEqual(
                    manifest[vehicle]["intended_direction"],
                    row["intended_direction"],
                )

    def test_omnet_configs_match_manifest_node_directions(self):
        ini = (HERE / "omnetpp.ini").read_text()
        for fixture_id in range(fixtures.FIXTURE_COUNT):
            name = f"SixteenVehiclesDirectionAblationFixture{fixture_id}ResDB"
            section_match = re.search(
                rf"\[Config {name}\](.*?)(?=\n\[Config |\Z)", ini, re.S
            )
            self.assertIsNotNone(section_match, name)
            section = section_match.group(1)
            configured = {
                int(node): direction
                for node, direction in re.findall(
                    r'\*\.node\[(\d+)\]\.appl\.intendedDirection = "([LSR])"',
                    section,
                )
            }
            expected = {
                int(row["node_index"]): str(row["intended_direction"])
                for row in fixtures.fixture_rows(fixture_id)
            }
            self.assertEqual(configured, expected)

    def test_manifest_pairs_fixture_and_seed_within_repetition(self):
        rows = grid.straight_heavy_direction_ablation_manifest("full")
        self.assertEqual(len(rows), 120)
        self.assertEqual({tuple(row["maneuver_mix"].values()) for row in rows}, {(4, 8, 4)})
        for rep in range(20):
            rep_rows = [row for row in rows if row["rep"] == rep]
            self.assertEqual(len(rep_rows), 6)
            self.assertEqual({row["fixture_id"] for row in rep_rows}, {rep % 4})
            self.assertEqual(len({row["config"] for row in rep_rows}), 1)

    def test_prerequisite_is_one_honest_eligibility_off_row(self):
        rows = grid.straight_heavy_direction_ablation_manifest("prerequisite")
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["attack_kind"], "NONE")
        self.assertFalse(row["direction_eligibility_enabled"])
        self.assertFalse(row["all_singleton_scheduling"])


if __name__ == "__main__":
    unittest.main()
