#!/usr/bin/env python3
"""Validate the N=4/8/16/20 mixed two-lane fixtures before OMNeT smokes."""

from __future__ import annotations

import csv
import json
import os
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

from generate_two_lane_scale_fixtures import FIXTURES


HERE = Path(__file__).resolve().parent
NET = HERE / "bft_intersection_2lane.net.xml"
RESULT = HERE / "two_lane_calibration/results/two_lane_scale_fixture_validation.json"


def paths(n: int) -> tuple[Path, Path, Path]:
    if n == 16:
        route = HERE / "bft_16veh_2lane_conflict_release.rou.xml"
        cfg = HERE / "bft_16veh_2lane_conflict_release.sumo.cfg"
    else:
        route = HERE / f"bft_{n}veh_2lane_scale.rou.xml"
        cfg = HERE / f"bft_{n}veh_2lane_scale.sumo.cfg"
    return route, cfg, HERE / f"two_lane_scale_{n}_manifest.csv"


def load_manifest(n: int) -> list[dict]:
    _, _, manifest = paths(n)
    with manifest.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        row["node_index"] = int(row["node_index"])
        row["sumo_lane_index"] = int(row["sumo_lane_index"])
        row["depart_position_m"] = float(row["depart_position_m"])
    return rows


def static_check(n: int, rows: list[dict]) -> dict:
    route_path, _, _ = paths(n)
    route_root = ET.parse(route_path).getroot()
    route_edges = {
        item.attrib["id"]: tuple(item.attrib["edges"].split())
        for item in route_root.findall("route")
    }
    vehicles = {item.attrib["id"]: item for item in route_root.findall("vehicle")}
    connections = list(ET.parse(NET).getroot().findall("connection"))
    errors: list[str] = []
    if len(rows) != n or len(vehicles) != n:
        errors.append(f"N={n}: manifest={len(rows)} route={len(vehicles)}")
    if {row["node_index"] for row in rows} != set(range(n)):
        errors.append("node indices are not a permutation")
    if {row["vehicle_id"] for row in rows} != set(vehicles):
        errors.append("manifest/route vehicle IDs differ")

    approaches = Counter(row["intended_lane"] for row in rows)
    directions = Counter(row["intended_direction"] for row in rows)
    expected_directions = {"L": n // 2, "S": n // 4, "R": n // 4}
    if approaches != Counter({value: n // 4 for value in "NSEW"}):
        errors.append(f"approach imbalance: {dict(approaches)}")
    if directions != Counter(expected_directions):
        errors.append(f"maneuver mix {dict(directions)} != {expected_directions}")

    queue_slots = set()
    for row in rows:
        vehicle = vehicles.get(row["vehicle_id"])
        if vehicle is None:
            continue
        actual_edges = route_edges.get(vehicle.attrib["route"])
        expected_edges = (row["ingress_edge"], row["egress_edge"])
        if actual_edges != expected_edges:
            errors.append(f"{row['vehicle_id']}: edges {actual_edges} != {expected_edges}")
        if int(vehicle.attrib["departLane"]) != row["sumo_lane_index"]:
            errors.append(f"{row['vehicle_id']}: depart lane mismatch")
        direction = row["intended_direction"]
        lane = row["sumo_lane_index"]
        if (lane == 1) != (direction == "L"):
            errors.append(f"{row['vehicle_id']}: physical-lane policy violation")
        slot = (row["ingress_edge"], lane, row["depart_position_m"])
        if slot in queue_slots:
            errors.append(f"{row['vehicle_id']}: duplicate queue slot {slot}")
        queue_slots.add(slot)
        connection = next((item for item in connections
                           if item.get("from") == row["ingress_edge"]
                           and item.get("to") == row["egress_edge"]), None)
        if connection is None or int(connection.get("fromLane", -1)) != lane:
            errors.append(f"{row['vehicle_id']}: invalid network connection/lane")

    target = next((row for row in rows if row["vehicle_id"] == "veh0"), {})
    counterpart = next((row for row in rows if row["vehicle_id"] == "veh1"), {})
    if (target.get("intended_lane"), target.get("intended_direction")) != ("N", "L"):
        errors.append("veh0 is not locked N-LEFT")
    if (counterpart.get("intended_lane"), counterpart.get("intended_direction")) != ("S", "S"):
        errors.append("veh1 is not locked S-STRAIGHT")
    return {
        "passed": not errors,
        "errors": errors,
        "approach_counts": dict(approaches),
        "maneuver_counts": dict(directions),
    }


def sumo_binary() -> str:
    candidates = []
    if os.environ.get("SUMO_HOME"):
        candidates.append(Path(os.environ["SUMO_HOME"]))
    candidates.append(Path("/Users/yashmalegaonkar/Documents/omnet/sumo-1.22.0"))
    for root in candidates:
        if (root / "bin/sumo").is_file() and (root / "tools").is_dir():
            sys.path.insert(0, str(root / "tools"))
            return str(root / "bin/sumo")
    raise RuntimeError("SUMO not found")


def traci_check(n: int, rows: list[dict], binary: str) -> dict:
    import traci

    _, cfg, _ = paths(n)
    label = f"scale_fixture_{n}"
    traci.start([
        binary, "-c", str(cfg), "--start", "--quit-on-end", "--end", "1000",
        "--time-to-teleport", "-1", "--collision.action", "none",
        "--collision.check-junctions", "true",
    ], label=label)
    conn = traci.getConnection(label)
    outcomes = {}
    teleports: list[str] = []
    collisions: set[str] = set()
    try:
        conn.simulationStep()
        for row in rows:
            vid = row["vehicle_id"]
            if vid in conn.vehicle.getIDList():
                conn.vehicle.setSpeedMode(vid, 0)
                conn.vehicle.setSpeed(vid, 0)
        phase = ET.parse(NET).getroot().find("./tlLogic/phase")
        conn.trafficlight.setRedYellowGreenState("C", "G" * len(phase.attrib["state"]))
        for row in sorted(rows, key=lambda item: (-item["depart_position_m"], item["vehicle_id"])):
            vid = row["vehicle_id"]
            conn.vehicle.setSpeed(vid, 13.9)
            approach_lane = None
            for _ in range(5000):
                conn.simulationStep()
                teleports.extend(conn.simulation.getStartingTeleportIDList())
                collisions.update(conn.simulation.getCollidingVehiclesIDList())
                if vid not in conn.vehicle.getIDList():
                    break
                road = conn.vehicle.getRoadID(vid)
                if road == row["ingress_edge"]:
                    approach_lane = conn.vehicle.getLaneIndex(vid)
                if road == row["egress_edge"]:
                    outcomes[vid] = {
                        "egress": road,
                        "approach_lane_index": approach_lane,
                    }
                    conn.vehicle.remove(vid)
                    break
    finally:
        conn.close()
    errors = []
    for row in rows:
        outcome = outcomes.get(row["vehicle_id"], {})
        if outcome.get("egress") != row["egress_edge"]:
            errors.append(f"{row['vehicle_id']}: egress mismatch/missing")
        if outcome.get("approach_lane_index") != row["sumo_lane_index"]:
            errors.append(f"{row['vehicle_id']}: approach lane mismatch")
    if teleports:
        errors.append(f"teleports={teleports}")
    if collisions:
        errors.append(f"collisions={sorted(collisions)}")
    return {"passed": not errors, "errors": errors, "outcomes": outcomes,
            "teleports": teleports, "collisions": sorted(collisions)}


def main() -> int:
    binary = sumo_binary()
    results = {}
    for n in sorted(FIXTURES):
        rows = load_manifest(n)
        static = static_check(n, rows)
        traci = traci_check(n, rows, binary) if static["passed"] else {"passed": False}
        results[str(n)] = {"static": static, "traci": traci,
                           "passed": static["passed"] and traci["passed"]}
        print(f"N={n}: {'PASS' if results[str(n)]['passed'] else 'FAIL'}")
    payload = {
        "checkpoint": "two-lane-scale-fixture-validation",
        "maneuver_ratio": "50% LEFT / 25% STRAIGHT / 25% RIGHT",
        "reviewed_conflict_pair": "veh0=N-L; veh1=S-S",
        "results": results,
        "passed": all(item["passed"] for item in results.values()),
    }
    RESULT.parent.mkdir(parents=True, exist_ok=True)
    RESULT.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"Summary: {RESULT}")
    return 0 if payload["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
