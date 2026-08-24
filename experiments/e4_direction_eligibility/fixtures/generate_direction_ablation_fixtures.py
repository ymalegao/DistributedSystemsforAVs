#!/usr/bin/env python3
"""Generate the locked straight-heavy N=16 direction-ablation fixtures.

Each checked-in variant contains 2 STRAIGHT, 1 LEFT, and 1 RIGHT movement per
approach (8S/4L/4R total).  veh0=N-LEFT and veh1=S-STRAIGHT never move: they
are the reviewed actual conflict pair used by the FALSE_DIRECTION ablation.
Only the remaining vehicle-to-maneuver assignment rotates.  The orchestrator
selects variant ``rep % len(VARIANTS)`` before any simulation randomness.
"""

from __future__ import annotations

import csv
from pathlib import Path


HERE = Path(__file__).resolve().parent
FIXTURE_COUNT = 4

EGRESS = {
    ("N", "L"): "C2E", ("N", "S"): "C2S", ("N", "R"): "C2W",
    ("S", "L"): "C2W", ("S", "S"): "C2N", ("S", "R"): "C2E",
    ("E", "L"): "C2S", ("E", "S"): "C2W", ("E", "R"): "C2N",
    ("W", "L"): "C2N", ("W", "S"): "C2E", ("W", "R"): "C2S",
}
SIGNAL = {"L": "LEFT", "S": "NONE", "R": "RIGHT"}
APPROACH_VEHICLES = {
    "N": (0, 2, 4, 12),
    "S": (1, 3, 5, 13),
    "E": (6, 7, 8, 14),
    "W": (9, 10, 11, 15),
}


def _rotate(values: tuple[str, ...], amount: int) -> tuple[str, ...]:
    amount %= len(values)
    return values[amount:] + values[:amount]


def assignments(fixture_id: int) -> dict[int, tuple[str, str]]:
    """Return ``vehicle -> (approach, direction)`` for one locked variant."""
    if fixture_id not in range(FIXTURE_COUNT):
        raise ValueError(f"fixture_id must be in [0,{FIXTURE_COUNT - 1}]")

    result = {0: ("N", "L"), 1: ("S", "S")}
    # North and south each have one fixed vehicle, so their three remaining
    # roles rotate over the other identities. East/west rotate all four roles
    # in opposite directions to avoid a single repeated global pattern.
    patterns = {
        "N": _rotate(("R", "S", "S"), fixture_id),
        "S": _rotate(("L", "R", "S"), fixture_id),
        "E": _rotate(("L", "S", "R", "S"), fixture_id),
        "W": _rotate(("L", "S", "R", "S"), -fixture_id),
    }
    for approach, vehicles in APPROACH_VEHICLES.items():
        flexible = tuple(vehicle for vehicle in vehicles if vehicle not in result)
        for vehicle, direction in zip(flexible, patterns[approach]):
            result[vehicle] = (approach, direction)
    return result


def route_id(approach: str, direction: str) -> str:
    physical = "L" if direction == "L" else "T"
    word = {"L": "left", "S": "straight", "R": "right"}[direction]
    return f"r{approach}_{physical}_{word}"


def fixture_rows(fixture_id: int) -> list[dict[str, object]]:
    assigned = assignments(fixture_id)
    node_index = {
        vehicle: index
        for index, vehicle in enumerate(sorted(assigned, key=lambda value: f"veh{value}"))
    }
    positions: dict[int, int] = {}
    for approach, vehicles in APPROACH_VEHICLES.items():
        left = [v for v in vehicles if assigned[v][1] == "L"]
        outer = sorted(v for v in vehicles if assigned[v][1] != "L")
        if len(left) != 1 or len(outer) != 3:
            raise AssertionError(f"invalid physical-lane split for {approach}")
        positions[left[0]] = 60
        if approach == "S":
            # Keep the reviewed counterpart veh1 at the head of S lane 0.
            outer.remove(1)
            positions[1] = 60
            for vehicle, position in zip(outer, (20, 5)):
                positions[vehicle] = position
        else:
            for vehicle, position in zip(outer, (60, 20, 5)):
                positions[vehicle] = position

    rows: list[dict[str, object]] = []
    for vehicle in sorted(assigned):
        approach, direction = assigned[vehicle]
        lane = 1 if direction == "L" else 0
        rows.append({
            "fixture_id": fixture_id,
            "vehicle_id": f"veh{vehicle}",
            "node_index": node_index[vehicle],
            "ingress_edge": f"{approach}2C",
            "egress_edge": EGRESS[(approach, direction)],
            "sumo_lane_index": lane,
            "logical_lane": "L" if lane == 1 else "T",
            "intended_lane": approach,
            "intended_direction": direction,
            "expected_signal": SIGNAL[direction],
            "depart_position_m": positions[vehicle],
        })
    return rows


def write_manifest(fixture_id: int) -> None:
    rows = fixture_rows(fixture_id)
    path = HERE / f"direction_ablation_straight_heavy_{fixture_id}_manifest.csv"
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_route(fixture_id: int) -> None:
    rows = fixture_rows(fixture_id)
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
            lines.append(
                f'    <route id="{route_id(approach, direction)}" '
                f'edges="{approach}2C {EGRESS[(approach, direction)]}"/>'
            )
    lines.extend((
        '',
        f'    <!-- Straight-heavy direction-ablation fixture {fixture_id}: '
        '2S/1L/1R per approach. -->',
    ))
    for row in rows:
        lines.append(
            f'    <vehicle id="{row["vehicle_id"]}" type="car" '
            f'route="{route_id(str(row["intended_lane"]), str(row["intended_direction"]))}" '
            f'depart="0" departLane="{row["sumo_lane_index"]}" '
            f'departPos="{row["depart_position_m"]}" departSpeed="max"/>'
        )
    lines.append('</routes>')
    (HERE / f"bft_16veh_direction_ablation_{fixture_id}.rou.xml").write_text(
        "\n".join(lines) + "\n"
    )


def write_sumo_and_launch(fixture_id: int) -> None:
    stem = f"bft_16veh_direction_ablation_{fixture_id}"
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
    for fixture_id in range(FIXTURE_COUNT):
        write_manifest(fixture_id)
        write_route(fixture_id)
        write_sumo_and_launch(fixture_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
