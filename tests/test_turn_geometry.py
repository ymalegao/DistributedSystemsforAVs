"""Reject scheduler pairs SUMO marks as conflicting in either lane network."""
import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

class TurnGeometryTest(unittest.TestCase):
    def test_allowed_pairs_have_no_sumo_foes(self):
        text = (ROOT / "bridge/resdb_conflict_matrix.h").read_text()
        pairs = [tuple(map(int, p)) for p in re.findall(r"\{(\d), (\d), (\d), (\d)\}", text)]
        self.assertTrue(pairs)
        for name in ("bft_intersection.net.xml", "bft_intersection_2lane.net.xml"):
            net = ET.parse(ROOT / "scenarios/fourway" / name).getroot()
            junction = net.find("junction[@id='C']")
            foes = {int(q.get("index")): q.get("foes")[::-1] for q in junction.findall("request")}
            movements = {}
            for c in net.findall("connection"):
                if c.get("from") in ("N2C", "S2C", "E2C", "W2C") and c.get("dir") in ("s", "l", "r"):
                    movements[("NSEW".index(c.get("from")[0]), {"s": 0, "l": 1, "r": 2}[c.get("dir")])] = int(c.get("linkIndex") or c.get("via").split("_")[1])
            for a, d, b, e in pairs:
                with self.subTest(network=name, pair=(a,d,b,e)):
                    i, j = movements[a,d], movements[b,e]
                    self.assertEqual(foes[i][j], "0")
                    self.assertEqual(foes[j][i], "0")

    def test_n20_two_lane_queue_pattern_is_symmetric(self):
        root = ET.parse(ROOT / "scenarios/fourway/bft_20veh_2lane_scale.rou.xml").getroot()
        patterns = {}
        for approach in "NSEW":
            vehicles = [v for v in root.findall("vehicle")
                        if v.get("route").startswith(f"r{approach}_")]
            by_lane = {}
            for lane in ("0", "1"):
                ordered = sorted((v for v in vehicles if v.get("departLane") == lane),
                                 key=lambda v: -float(v.get("departPos")))
                by_lane[lane] = [v.get("route").split("_")[-1] for v in ordered]
            patterns[approach] = by_lane

        expected = {"0": ["straight", "right"],
                    "1": ["left", "left", "left"]}
        self.assertEqual(patterns, {approach: expected for approach in "NSEW"})

    def test_two_lane_protected_left_phase_keeps_its_nominal_green(self):
        # With a 4 s minimum, the actuated controller gaps out after two cars
        # and the third N=20 left turn waits a complete additional cycle.
        net = ET.parse(ROOT / "scenarios/fourway/tl_intersection_2lane.net.xml").getroot()
        phases = net.find("tlLogic[@id='C']").findall("phase")
        protected_lefts = [p for p in phases if p.get("state") in
                           ("rrGrrrrrGrrr", "rrrrrGrrrrrG")]
        self.assertEqual(len(protected_lefts), 2)
        for phase in protected_lefts:
            self.assertEqual(float(phase.get("minDur")), float(phase.get("duration")))

if __name__ == "__main__":
    unittest.main()
