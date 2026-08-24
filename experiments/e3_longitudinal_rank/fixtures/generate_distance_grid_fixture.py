#!/usr/bin/env python3
"""Generate the N=16 distance-grid SUMO fixture at a calibrated spacing.

The requested value is the front-reference separation measured by TraCI.  For
the identical 4.5 m vehicles in bft_16veh.rou.xml this is length + minGap, so
the generated route file uses minGap = separation - 4.5 m.  Vehicle routes,
departures, IDs, and the network are otherwise unchanged.
"""

from __future__ import annotations

import argparse
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path


ALLOWED_SEPARATIONS_M = (5.0, 5.5, 6.5, 7.5, 9.5, 12.5)
VEHICLE_LENGTH_M = 4.5


def canonical_separation(value: float) -> float:
    if not math.isfinite(value):
        raise ValueError("reference separation must be finite")
    for candidate in ALLOWED_SEPARATIONS_M:
        if math.isclose(value, candidate, rel_tol=0.0, abs_tol=1e-9):
            return candidate
    allowed = ", ".join(format(v, "g") for v in ALLOWED_SEPARATIONS_M)
    raise ValueError(f"unsupported reference separation {value:g}; choose one of {allowed} m")


def generate(output_dir: Path, separation_m: float) -> dict[str, object]:
    separation_m = canonical_separation(separation_m)
    source_dir = Path(__file__).resolve().parent
    output_dir.mkdir(parents=True, exist_ok=True)

    route_name = "distance_grid_16veh.rou.xml"
    sumocfg_name = "distance_grid_16veh.sumo.cfg"
    launch_name = "distance_grid_16veh.launchd.xml"
    metadata_name = "distance_grid_fixture_metadata.json"

    route_tree = ET.parse(source_dir / "bft_16veh.rou.xml")
    route_root = route_tree.getroot()
    min_gap_m = separation_m - VEHICLE_LENGTH_M
    if min_gap_m < 0.0:
        raise ValueError("reference separation cannot be shorter than the vehicle")
    vehicle_types = route_root.findall("vType")
    if not vehicle_types:
        raise ValueError("source route file contains no vType")
    for vehicle_type in vehicle_types:
        length = float(vehicle_type.get("length", "nan"))
        if not math.isclose(length, VEHICLE_LENGTH_M, rel_tol=0.0, abs_tol=1e-9):
            raise ValueError(
                f"vType {vehicle_type.get('id')} length={length:g} m; "
                f"fixture assumes {VEHICLE_LENGTH_M:g} m"
            )
        vehicle_type.set("minGap", format(min_gap_m, ".12g"))
    ET.indent(route_tree, space="    ")
    route_tree.write(output_dir / route_name, encoding="utf-8", xml_declaration=True)

    cfg_tree = ET.parse(source_dir / "bft_16veh.sumo.cfg")
    route_files = cfg_tree.getroot().find("./input/route-files")
    if route_files is None:
        raise ValueError("source SUMO config has no input/route-files element")
    route_files.set("value", route_name)
    ET.indent(cfg_tree, space="    ")
    cfg_tree.write(output_dir / sumocfg_name, encoding="utf-8", xml_declaration=True)

    launch_root = ET.Element("launch")
    ET.SubElement(launch_root, "copy", {"file": "bft_intersection.net.xml"})
    ET.SubElement(launch_root, "copy", {"file": route_name})
    ET.SubElement(launch_root, "copy", {"file": sumocfg_name, "type": "config"})
    launch_tree = ET.ElementTree(launch_root)
    ET.indent(launch_tree, space="    ")
    launch_tree.write(output_dir / launch_name, encoding="utf-8", xml_declaration=True)

    metadata = {
        "fixture": "N16 calibrated stopped-reference separation",
        "source_route": "bft_16veh.rou.xml",
        "source_sumocfg": "bft_16veh.sumo.cfg",
        "reference_separation_m": separation_m,
        "vehicle_length_m": VEHICLE_LENGTH_M,
        "bumper_clearance_m": min_gap_m,
        "generated_files": [route_name, sumocfg_name, launch_name],
    }
    (output_dir / metadata_name).write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-separation", required=True, type=float)
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    try:
        metadata = generate(args.output_dir.resolve(), args.reference_separation)
    except (OSError, ValueError, ET.ParseError) as exc:
        parser.error(str(exc))
    print(json.dumps(metadata, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
