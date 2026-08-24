#!/usr/bin/env python3

import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from fourway.generate_distance_grid_fixture import (
    ALLOWED_SEPARATIONS_M,
    VEHICLE_LENGTH_M,
    canonical_separation,
    generate,
)


class DistanceGridFixtureTests(unittest.TestCase):
    def test_catalog_is_accepted(self):
        for value in ALLOWED_SEPARATIONS_M:
            self.assertEqual(canonical_separation(value), value)

    def test_unknown_spacing_is_rejected(self):
        with self.assertRaises(ValueError):
            canonical_separation(6.0)

    def test_generated_spacing_and_source_semantics(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            metadata = generate(output, 6.5)
            self.assertEqual(metadata["reference_separation_m"], 6.5)
            self.assertEqual(metadata["bumper_clearance_m"], 2.0)

            source = ET.parse(Path(__file__).resolve().parent / "bft_16veh.rou.xml").getroot()
            generated = ET.parse(output / "distance_grid_16veh.rou.xml").getroot()
            self.assertEqual(
                [(v.get("id"), v.get("route"), v.get("departPos")) for v in source.findall("vehicle")],
                [(v.get("id"), v.get("route"), v.get("departPos")) for v in generated.findall("vehicle")],
            )
            for vehicle_type in generated.findall("vType"):
                separation = float(vehicle_type.get("length")) + float(vehicle_type.get("minGap"))
                self.assertAlmostEqual(separation, 6.5, places=9)
                self.assertAlmostEqual(float(vehicle_type.get("length")), VEHICLE_LENGTH_M)

            cfg = ET.parse(output / "distance_grid_16veh.sumo.cfg").getroot()
            self.assertEqual(cfg.find("./input/route-files").get("value"), "distance_grid_16veh.rou.xml")
            self.assertEqual(cfg.find("./processing/time-to-teleport").get("value"), "-1")


if __name__ == "__main__":
    unittest.main()
