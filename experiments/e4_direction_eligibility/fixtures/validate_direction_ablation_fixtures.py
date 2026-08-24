#!/usr/bin/env python3
"""Validate all four straight-heavy direction fixtures with SUMO/TraCI."""

from __future__ import annotations

import csv
import json
import os
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

from generate_direction_ablation_fixtures import FIXTURE_COUNT


HERE = Path(__file__).resolve().parent
NET = HERE / "bft_intersection_2lane.net.xml"
RESULT = (
    HERE / "two_lane_calibration/results/"
    "direction_ablation_straight_heavy_fixture_validation.json"
)


def paths(fixture_id: int) -> tuple[Path, Path, Path]:
    stem = f"bft_16veh_direction_ablation_{fixture_id}"
    return (
        HERE / f"{stem}.rou.xml",
        HERE / f"{stem}.sumo.cfg",
        HERE / f"direction_ablation_straight_heavy_{fixture_id}_manifest.csv",
    )


def load_manifest(fixture_id: int) -> list[dict]:
    _, _, manifest = paths(fixture_id)
    with manifest.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        row["node_index"] = int(row["node_index"])
        row["sumo_lane_index"] = int(row["sumo_lane_index"])
        row["depart_position_m"] = float(row["depart_position_m"])
    return rows


def static_check(fixture_id: int, rows: list[dict]) -> dict:
    route_path, _, _ = paths(fixture_id)
    root = ET.parse(route_path).getroot()
    route_edges = {
        item.attrib["id"]: tuple(item.attrib["edges"].split())
        for item in root.findall("route")
    }
    vehicles = {item.attrib["id"]: item for item in root.findall("vehicle")}
    connections = list(ET.parse(NET).getroot().findall("connection"))
    errors: list[str] = []
    directions = Counter(row["intended_direction"] for row in rows)
    approaches = Counter(row["intended_lane"] for row in rows)
    if len(rows) != 16 or len(vehicles) != 16:
        errors.append(f"manifest={len(rows)} route={len(vehicles)}")
    if directions != Counter({"S": 8, "L": 4, "R": 4}):
        errors.append(f"wrong maneuver mix: {dict(directions)}")
    if approaches != Counter({value: 4 for value in "NSEW"}):
        errors.append(f"wrong approach mix: {dict(approaches)}")
    for approach in "NSEW":
        per_approach = Counter(
            row["intended_direction"] for row in rows
            if row["intended_lane"] == approach
        )
        if per_approach != Counter({"S": 2, "L": 1, "R": 1}):
            errors.append(f"{approach}: {dict(per_approach)}")
    queue_slots = set()
    for row in rows:
        vehicle = vehicles.get(row["vehicle_id"])
        if vehicle is None:
            errors.append(f"missing {row['vehicle_id']}")
            continue
        expected_edges = (row["ingress_edge"], row["egress_edge"])
        if route_edges.get(vehicle.attrib["route"]) != expected_edges:
            errors.append(f"{row['vehicle_id']}: route/manifest mismatch")
        lane = row["sumo_lane_index"]
        if int(vehicle.attrib["departLane"]) != lane:
            errors.append(f"{row['vehicle_id']}: depart lane mismatch")
        if (lane == 1) != (row["intended_direction"] == "L"):
            errors.append(f"{row['vehicle_id']}: lane policy violation")
        slot = (row["ingress_edge"], lane, row["depart_position_m"])
        if slot in queue_slots:
            errors.append(f"{row['vehicle_id']}: duplicate queue slot {slot}")
        queue_slots.add(slot)
        connection = next((item for item in connections
                           if item.get("from") == row["ingress_edge"]
                           and item.get("to") == row["egress_edge"]), None)
        if connection is None or int(connection.get("fromLane", -1)) != lane:
            errors.append(f"{row['vehicle_id']}: invalid connection/lane")
    by_vehicle = {row["vehicle_id"]: row for row in rows}
    if (by_vehicle.get("veh0", {}).get("intended_lane"),
            by_vehicle.get("veh0", {}).get("intended_direction")) != ("N", "L"):
        errors.append("veh0 is not N-LEFT")
    if (by_vehicle.get("veh1", {}).get("intended_lane"),
            by_vehicle.get("veh1", {}).get("intended_direction")) != ("S", "S"):
        errors.append("veh1 is not S-STRAIGHT")
    return {"passed": not errors, "errors": errors}


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


def traci_check(fixture_id: int, rows: list[dict], binary: str) -> dict:
    import traci

    _, cfg, _ = paths(fixture_id)
    label = f"direction_fixture_{fixture_id}"
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
            vehicle = row["vehicle_id"]
            if vehicle in conn.vehicle.getIDList():
                conn.vehicle.setSpeedMode(vehicle, 0)
                conn.vehicle.setSpeed(vehicle, 0)
        phase = ET.parse(NET).getroot().find("./tlLogic/phase")
        conn.trafficlight.setRedYellowGreenState("C", "G" * len(phase.attrib["state"]))
        for row in sorted(rows, key=lambda item: (-item["depart_position_m"], item["vehicle_id"])):
            vehicle = row["vehicle_id"]
            conn.vehicle.setSpeed(vehicle, 13.9)
            approach_lane = None
            for _ in range(5000):
                conn.simulationStep()
                teleports.extend(conn.simulation.getStartingTeleportIDList())
                collisions.update(conn.simulation.getCollidingVehiclesIDList())
                if vehicle not in conn.vehicle.getIDList():
                    break
                road = conn.vehicle.getRoadID(vehicle)
                if road == row["ingress_edge"]:
                    approach_lane = conn.vehicle.getLaneIndex(vehicle)
                if road == row["egress_edge"]:
                    outcomes[vehicle] = {
                        "egress": road,
                        "approach_lane_index": approach_lane,
                    }
                    conn.vehicle.remove(vehicle)
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
    return {
        "passed": not errors,
        "errors": errors,
        "outcomes": outcomes,
        "teleports": teleports,
        "collisions": sorted(collisions),
    }


def main() -> int:
    binary = sumo_binary()
    results = {}
    for fixture_id in range(FIXTURE_COUNT):
        rows = load_manifest(fixture_id)
        static = static_check(fixture_id, rows)
        dynamic = traci_check(fixture_id, rows, binary) if static["passed"] else {"passed": False}
        passed = static["passed"] and dynamic["passed"]
        results[str(fixture_id)] = {
            "static": static, "traci": dynamic, "passed": passed,
        }
        print(f"fixture={fixture_id}: {'PASS' if passed else 'FAIL'}")
    payload = {
        "checkpoint": "straight-heavy-direction-fixture-validation",
        "maneuver_mix": {"straight": 8, "left": 4, "right": 4},
        "per_approach": {"straight": 2, "left": 1, "right": 1},
        "reviewed_conflict_pair": "veh0=N-L; veh1=S-S",
        "selection_policy": "fixture_id=rep_mod_4 before perception",
        "results": results,
        "passed": all(item["passed"] for item in results.values()),
    }
    RESULT.parent.mkdir(parents=True, exist_ok=True)
    RESULT.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"Summary: {RESULT}")
    return 0 if payload["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
