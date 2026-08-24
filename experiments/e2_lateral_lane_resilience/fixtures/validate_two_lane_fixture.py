#!/usr/bin/env python3
"""Validate the N=16 full-intersection two-lane fixture before protocol wiring.

This checkpoint validates only checked-in route/lane/declaration intent and
actual SUMO ingress/egress behavior. It does not classify conflicts, modify
kSafe, or exercise Byzantine behavior.
"""

from __future__ import annotations

import csv
import json
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
NET = HERE / "bft_intersection_2lane.net.xml"
ROUTES = HERE / "bft_16veh_2lane.rou.xml"
SUMOCFG = HERE / "bft_16veh_2lane.sumo.cfg"
MANIFEST = HERE / "two_lane_route_manifest.csv"
RESULT = HERE / "two_lane_calibration/results/two_lane_fixture_validation.json"

LANE_POLICY = {
    0: {"logical": "T", "directions": {"S", "R"}},
    1: {"logical": "L", "directions": {"L"}},
}


def ensure_sumo_tools() -> str:
    candidates = []
    if os.environ.get("SUMO_HOME"):
        candidates.append(Path(os.environ["SUMO_HOME"]))
    candidates.append(Path("/Users/yashmalegaonkar/Documents/omnet/sumo-1.22.0"))
    for root in candidates:
        tools = root / "tools"
        binary = root / "bin/sumo"
        if tools.is_dir() and binary.is_file():
            if str(tools) not in sys.path:
                sys.path.insert(0, str(tools))
            os.environ.setdefault("SUMO_HOME", str(root))
            return str(binary)
    raise RuntimeError("SUMO not found; set SUMO_HOME")


def load_manifest() -> list[dict[str, Any]]:
    with MANIFEST.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        row["node_index"] = int(row["node_index"])
        row["sumo_lane_index"] = int(row["sumo_lane_index"])
        row["depart_position_m"] = float(row["depart_position_m"])
    return rows


def static_validation(rows: list[dict[str, Any]]) -> dict[str, Any]:
    route_root = ET.parse(ROUTES).getroot()
    route_edges = {
        route.attrib["id"]: tuple(route.attrib["edges"].split())
        for route in route_root.findall("route")
    }
    vehicles = {vehicle.attrib["id"]: vehicle for vehicle in route_root.findall("vehicle")}
    net_root = ET.parse(NET).getroot()
    connections = list(net_root.findall("connection"))

    errors: list[str] = []
    if len(rows) != 16:
        errors.append(f"manifest has {len(rows)} rows, expected 16")
    if len(vehicles) != 16:
        errors.append(f"route file has {len(vehicles)} vehicles, expected 16")
    if {row["node_index"] for row in rows} != set(range(16)):
        errors.append("node_index must be a permutation of 0..15")
    if {row["vehicle_id"] for row in rows} != set(vehicles):
        errors.append("manifest and route-file vehicle IDs differ")

    movements = set()
    lane_counts = {0: 0, 1: 0}
    for row in rows:
        vid = row["vehicle_id"]
        vehicle = vehicles.get(vid)
        if vehicle is None:
            continue
        route_id = vehicle.attrib["route"]
        actual_edges = route_edges.get(route_id)
        expected_edges = (row["ingress_edge"], row["egress_edge"])
        if actual_edges != expected_edges:
            errors.append(f"{vid}: route {actual_edges} != manifest {expected_edges}")
        lane_index = row["sumo_lane_index"]
        if int(vehicle.attrib.get("departLane", "-1")) != lane_index:
            errors.append(f"{vid}: departLane mismatch")
        if abs(float(vehicle.attrib.get("departPos", "nan")) - row["depart_position_m"]) > 1e-9:
            errors.append(f"{vid}: departPos mismatch")
        policy = LANE_POLICY.get(lane_index)
        if policy is None:
            errors.append(f"{vid}: invalid lane index {lane_index}")
        else:
            lane_counts[lane_index] += 1
            if row["logical_lane"] != policy["logical"]:
                errors.append(f"{vid}: logical lane does not match lane {lane_index}")
            if row["intended_direction"] not in policy["directions"]:
                errors.append(
                    f"{vid}: direction {row['intended_direction']} unauthorized on lane {lane_index}"
                )
        connection = next(
            (
                item for item in connections
                if item.get("from") == row["ingress_edge"]
                and item.get("to") == row["egress_edge"]
            ),
            None,
        )
        if connection is None:
            errors.append(f"{vid}: no SUMO connection for {expected_edges}")
        elif int(connection.get("fromLane", "-1")) != lane_index:
            errors.append(
                f"{vid}: connection uses lane {connection.get('fromLane')}, expected {lane_index}"
            )
        movements.add((row["intended_lane"], row["intended_direction"]))

    if len(movements) != 12:
        errors.append(f"fixture covers {len(movements)} unique movements, expected 12")
    if lane_counts != {0: 8, 1: 8}:
        errors.append(f"physical lane counts {lane_counts}, expected 8/8")

    return {
        "pass": not errors,
        "errors": errors,
        "vehicle_count": len(rows),
        "movement_count": len(movements),
        "lane_counts": lane_counts,
        "lane_policy": {
            "0": "T/outer: STRAIGHT or RIGHT",
            "1": "L/inner: LEFT only",
        },
    }


def tls_width() -> int:
    root = ET.parse(NET).getroot()
    phase = root.find("./tlLogic/phase")
    if phase is None:
        raise RuntimeError("missing traffic-light phase")
    return len(phase.attrib["state"])


def traci_validation(rows: list[dict[str, Any]]) -> dict[str, Any]:
    sumo = ensure_sumo_tools()
    import traci

    label = "two_lane_fixture_validation"
    traci.start(
        [
            sumo, "-c", str(SUMOCFG), "--start", "--quit-on-end",
            "--end", "1000", "--time-to-teleport", "-1",
            "--collision.action", "none", "--collision.check-junctions", "true",
        ],
        label=label,
    )
    conn = traci.getConnection(label)
    outcomes: dict[str, Any] = {}
    teleport_starts: list[str] = []
    collision_ids: set[str] = set()
    try:
        conn.simulationStep()
        active = set(conn.vehicle.getIDList())
        for row in rows:
            vid = row["vehicle_id"]
            if vid not in active:
                outcomes[vid] = {"error": "not active after departure"}
                continue
            conn.vehicle.setSpeedMode(vid, 0)
            conn.vehicle.setSpeed(vid, 0)

        conn.trafficlight.setRedYellowGreenState("C", "G" * tls_width())
        # Release the front car before the rear car in each physical queue.
        release_order = sorted(
            rows,
            key=lambda row: (-row["depart_position_m"], row["vehicle_id"]),
        )
        for row in release_order:
            vid = row["vehicle_id"]
            if vid not in conn.vehicle.getIDList():
                outcomes.setdefault(vid, {"error": "missing before release"})
                continue
            start_lane = conn.vehicle.getLaneID(vid)
            start_lane_index = conn.vehicle.getLaneIndex(vid)
            approach_lane = None
            approach_lane_index = None
            conn.vehicle.setSpeed(vid, 13.9)
            reached = False
            for _ in range(4000):
                conn.simulationStep()
                teleport_starts.extend(conn.simulation.getStartingTeleportIDList())
                collision_ids.update(conn.simulation.getCollidingVehiclesIDList())
                if vid not in conn.vehicle.getIDList():
                    break
                road = conn.vehicle.getRoadID(vid)
                if road == row["ingress_edge"]:
                    lane_id = conn.vehicle.getLaneID(vid)
                    lane_length = conn.lane.getLength(lane_id)
                    distance_to_stop = lane_length - conn.vehicle.getLanePosition(vid)
                    if distance_to_stop <= 100.0:
                        # Keep the last observation in the intent/echo region.
                        # Early spawn-time lane changes are not certificate truth.
                        approach_lane = lane_id
                        approach_lane_index = conn.vehicle.getLaneIndex(vid)
                if road == row["egress_edge"]:
                    outcomes[vid] = {
                        "start_lane": start_lane,
                        "start_lane_index": start_lane_index,
                        "approach_lane": approach_lane,
                        "approach_lane_index": approach_lane_index,
                        "observed_egress": road,
                        "expected_egress": row["egress_edge"],
                    }
                    reached = True
                    conn.vehicle.remove(vid)
                    break
            if not reached:
                outcomes.setdefault(
                    vid,
                    {
                        "start_lane": start_lane,
                        "start_lane_index": start_lane_index,
                        "approach_lane": approach_lane,
                        "approach_lane_index": approach_lane_index,
                        "error": "did not reach expected egress",
                    },
                )
    finally:
        conn.close()

    failures = []
    for row in rows:
        outcome = outcomes.get(row["vehicle_id"], {})
        if outcome.get("approach_lane_index") != row["sumo_lane_index"]:
            failures.append(f"{row['vehicle_id']}: echo-window approach lane mismatch")
        if outcome.get("observed_egress") != row["egress_edge"]:
            failures.append(f"{row['vehicle_id']}: observed egress mismatch")
    if teleport_starts:
        failures.append(f"teleports observed: {sorted(set(teleport_starts))}")
    if collision_ids:
        failures.append(f"collisions observed: {sorted(collision_ids)}")
    return {
        "pass": not failures,
        "failures": failures,
        "outcomes": outcomes,
        "release_order": [row["vehicle_id"] for row in release_order],
        "teleport_starts": sorted(set(teleport_starts)),
        "collision_ids": sorted(collision_ids),
        "physics_pins": {
            "time_to_teleport": -1,
            "collision_check_junctions": True,
            "collision_action": "none",
            "released_speed_mode": 0,
        },
    }


def main() -> int:
    rows = load_manifest()
    static = static_validation(rows)
    dynamic = traci_validation(rows) if static["pass"] else {
        "pass": False,
        "failures": ["dynamic validation skipped because static validation failed"],
    }
    summary = {
        "checkpoint": "two-lane-full-intersection-fixture",
        "pass": bool(static["pass"] and dynamic["pass"]),
        "static": static,
        "traci": dynamic,
        "attack_pair_for_authority_checkpoint": {
            "actual_target": "N-L on physical lane 1",
            "claimed_target": "N-R on physical lane 0",
            "counterpart": "S-S on physical lane 0",
            "ksafe_claimed_pair": True,
            "actual_paths_conflict": True,
        },
    }
    RESULT.parent.mkdir(parents=True, exist_ok=True)
    RESULT.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"Static fixture: {'PASS' if static['pass'] else 'FAIL'}")
    print(f"TraCI routes:   {'PASS' if dynamic['pass'] else 'FAIL'}")
    print(f"Overall:        {'PASS' if summary['pass'] else 'FAIL'}")
    print(f"Summary: {RESULT}")
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
