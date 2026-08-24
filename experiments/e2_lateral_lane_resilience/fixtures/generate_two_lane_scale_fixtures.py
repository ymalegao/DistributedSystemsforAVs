#!/usr/bin/env python3
"""Generate the locked N=4/8/20 two-lane scaling fixtures and manifests.

N=16 reuses the reviewed conflict-release route.  Every size is balanced over
the four approaches and keeps a 50% LEFT / 25% STRAIGHT / 25% RIGHT maneuver
mix.  veh0=N-LEFT and veh1=S-STRAIGHT are fixed at every size so later attack
rows share one reviewed physical conflict pair.
"""

from __future__ import annotations

import csv
from pathlib import Path


HERE = Path(__file__).resolve().parent

# vehicle id, approach, direction, physical lane index, departure position
FIXTURES = {
    4: (
        (0, "N", "L", 1, 60),
        (1, "S", "S", 0, 60),
        (2, "E", "R", 0, 60),
        (3, "W", "L", 1, 60),
    ),
    8: (
        (0, "N", "L", 1, 60),
        (1, "S", "S", 0, 60),
        (2, "E", "R", 0, 60),
        (3, "W", "L", 1, 60),
        (4, "N", "R", 0, 60),
        (5, "S", "L", 1, 60),
        (6, "E", "L", 1, 60),
        (7, "W", "S", 0, 60),
    ),
    16: (
        (0, "N", "L", 1, 60), (1, "S", "S", 0, 60),
        (2, "N", "R", 0, 20), (3, "S", "L", 1, 60),
        (4, "N", "S", 0, 60), (5, "S", "R", 0, 20),
        (6, "E", "L", 1, 60), (7, "E", "S", 0, 60),
        (8, "E", "R", 0, 20), (9, "W", "L", 1, 60),
        (10, "W", "S", 0, 60), (11, "W", "R", 0, 20),
        (12, "N", "L", 1, 20), (13, "S", "L", 1, 20),
        (14, "E", "L", 1, 20), (15, "W", "L", 1, 20),
    ),
    20: (
        (0, "N", "L", 1, 100), (1, "S", "S", 0, 100),
        (2, "N", "R", 0, 60), (3, "N", "S", 0, 20),
        (4, "N", "L", 1, 60), (5, "N", "L", 1, 20),
        (6, "S", "R", 0, 60), (7, "S", "L", 1, 100),
        (8, "S", "L", 1, 60), (9, "S", "L", 1, 20),
        (10, "E", "L", 1, 60), (11, "E", "L", 1, 20),
        (12, "E", "S", 0, 100), (13, "E", "S", 0, 60),
        (14, "E", "R", 0, 20), (15, "W", "L", 1, 60),
        (16, "W", "L", 1, 20), (17, "W", "S", 0, 100),
        (18, "W", "R", 0, 60), (19, "W", "R", 0, 20),
    ),
}

EGRESS = {
    ("N", "L"): "C2E", ("N", "S"): "C2S", ("N", "R"): "C2W",
    ("S", "L"): "C2W", ("S", "S"): "C2N", ("S", "R"): "C2E",
    ("E", "L"): "C2S", ("E", "S"): "C2W", ("E", "R"): "C2N",
    ("W", "L"): "C2N", ("W", "S"): "C2E", ("W", "R"): "C2S",
}
SIGNAL = {"L": "LEFT", "S": "NONE", "R": "RIGHT"}


def route_id(approach: str, direction: str) -> str:
    physical = "L" if direction == "L" else "T"
    word = {"L": "left", "S": "straight", "R": "right"}[direction]
    return f"r{approach}_{physical}_{word}"


def manifest_rows(n: int):
    by_id = {f"veh{vid}": (vid, approach, direction, lane, pos)
             for vid, approach, direction, lane, pos in FIXTURES[n]}
    node_index = {vehicle: index for index, vehicle in enumerate(sorted(by_id))}
    for vehicle in sorted(by_id, key=lambda value: int(value[3:])):
        _, approach, direction, lane, pos = by_id[vehicle]
        yield {
            "vehicle_id": vehicle,
            "node_index": node_index[vehicle],
            "ingress_edge": f"{approach}2C",
            "egress_edge": EGRESS[(approach, direction)],
            "sumo_lane_index": lane,
            "logical_lane": "L" if lane == 1 else "T",
            "intended_lane": approach,
            "intended_direction": direction,
            "expected_signal": SIGNAL[direction],
            "depart_position_m": pos,
        }


def write_manifest(n: int) -> None:
    fields = list(next(iter(manifest_rows(n))).keys())
    with (HERE / f"two_lane_scale_{n}_manifest.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(manifest_rows(n))


def write_route(n: int) -> None:
    if n == 16:
        return
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<routes>',
        '    <vType id="car" accel="2.6" decel="4.5" sigma="0.0" length="4.5"',
        '           minGap="2.5" maxSpeed="13.90" color="1,1,0" speedFactor="1"',
        '           jmIgnoreFoeProb="1"/>',
        '',
    ]
    for approach in "NSEW":
        for direction in "SLR":
            rid = route_id(approach, direction)
            edges = f"{approach}2C {EGRESS[(approach, direction)]}"
            lines.append(f'    <route id="{rid}" edges="{edges}"/>')
    lines.append("")
    for vid, approach, direction, lane, pos in FIXTURES[n]:
        lines.append(
            f'    <vehicle id="veh{vid}" type="car" '
            f'route="{route_id(approach, direction)}" depart="0" '
            f'departLane="{lane}" departPos="{pos}" departSpeed="max"/>'
        )
    lines.append('</routes>')
    (HERE / f"bft_{n}veh_2lane_scale.rou.xml").write_text("\n".join(lines) + "\n")


def write_sumo_and_launch(n: int) -> None:
    if n == 16:
        return
    stem = f"bft_{n}veh_2lane_scale"
    (HERE / f"{stem}.sumo.cfg").write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<configuration xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://sumo.dlr.de/xsd/sumoConfiguration.xsd">
    <input>
        <net-file value="bft_intersection_2lane.net.xml"/>
        <route-files value="{stem}.rou.xml"/>
    </input>
    <time><begin value="0"/><end value="300"/><step-length value="0.1"/></time>
    <processing>
        <collision.check-junctions value="true"/>
        <collision.action value="none"/>
        <time-to-teleport value="-1"/>
    </processing>
    <report>
        <xml-validation value="never"/><xml-validation.net value="never"/>
        <no-step-log value="true"/>
    </report>
    <gui_only><start value="true"/><quit-on-end value="true"/></gui_only>
</configuration>
''')
    (HERE / f"resdb_{stem}.launchd.xml").write_text(f'''<?xml version="1.0"?>
<launch>
    <copy file="bft_intersection_2lane.net.xml" />
    <copy file="{stem}.rou.xml" />
    <copy file="{stem}.sumo.cfg" type="config" />
</launch>
''')


def main() -> int:
    for n in sorted(FIXTURES):
        write_manifest(n)
        write_route(n)
        write_sumo_and_launch(n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
