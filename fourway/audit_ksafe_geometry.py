#!/usr/bin/env python3
"""Audit the executor kSafe allowlist against SUMO internal connection paths.

This is an offline review tool. It never edits kSafe and is not used by the
runtime scheduler or safety metrology.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import xml.etree.ElementTree as ET
from itertools import combinations
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

Point = Tuple[float, float]
Movement = Tuple[str, str]

LANE_CODE = {"N": 0, "S": 1, "E": 2, "W": 3}
DIRECTION_CODE = {"S": 0, "L": 1, "R": 2}


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    repo = here.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--net", type=Path, default=here / "bft_intersection.net.xml")
    parser.add_argument(
        "--scheduler",
        type=Path,
        default=repo / "incubator-resilientdb/integration/omnet/resdb_intersection_scheduler.cc",
    )
    parser.add_argument("--manifest", type=Path, default=here / "phase2_mixed_route_manifest.csv")
    parser.add_argument("--csv-out", type=Path, default=here / "phase2_ksafe_geometry_audit.csv")
    parser.add_argument("--json-out", type=Path, default=here / "phase2_ksafe_geometry_audit.json")
    return parser.parse_args()


def load_movements(path: Path) -> Dict[Movement, Tuple[str, str]]:
    result: Dict[Movement, Tuple[str, str]] = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            key = (row["intended_lane"], row["intended_direction"])
            edges = (row["ingress_edge"], row["egress_edge"])
            if key in result and result[key] != edges:
                raise ValueError(f"movement {key} has inconsistent routes")
            result[key] = edges
    if len(result) != 12:
        raise ValueError(f"expected 12 unique movements, got {len(result)}")
    return result


def load_ksafe(path: Path) -> set[Tuple[int, int, int, int]]:
    text = path.read_text()
    match = re.search(
        r"static const uint8_t kSafe\[12\]\[4\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    if not match:
        raise ValueError("could not locate kSafe[12][4]")
    rows = {
        tuple(int(value) for value in row)
        for row in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", match.group(1))
    }
    if len(rows) != 12:
        raise ValueError(f"expected 12 kSafe rows, got {len(rows)}")
    return rows


def parse_shape(value: str) -> List[Point]:
    return [tuple(float(part) for part in token.split(",")) for token in value.split()]


def load_paths(net_path: Path, movements: Dict[Movement, Tuple[str, str]]) -> Tuple[Dict[Movement, List[Point]], Dict[Movement, List[str]]]:
    root = ET.parse(net_path).getroot()
    lane_shapes = {
        lane.get("id"): parse_shape(lane.get("shape", ""))
        for lane in root.iter("lane")
        if lane.get("id") and lane.get("shape")
    }
    connections = list(root.iter("connection"))
    paths: Dict[Movement, List[Point]] = {}
    via_lanes: Dict[Movement, List[str]] = {}
    for movement, (ingress, egress) in movements.items():
        first = next(
            (c for c in connections if c.get("from") == ingress and c.get("to") == egress),
            None,
        )
        if first is None or not first.get("via"):
            raise ValueError(f"missing SUMO connection for {movement}: {ingress}->{egress}")
        vias = [first.get("via")]
        internal_edge = vias[-1].rsplit("_", 1)[0]
        while True:
            continuation = next(
                (c for c in connections if c.get("from") == internal_edge and c.get("to") == egress),
                None,
            )
            if continuation is None or not continuation.get("via"):
                break
            vias.append(continuation.get("via"))
            internal_edge = vias[-1].rsplit("_", 1)[0]
        points: List[Point] = []
        for lane_id in vias:
            if lane_id not in lane_shapes:
                raise ValueError(f"missing shape for internal lane {lane_id}")
            for point in lane_shapes[lane_id]:
                if not points or point != points[-1]:
                    points.append(point)
        paths[movement] = points
        via_lanes[movement] = vias
    return paths, via_lanes


def orientation(a: Point, b: Point, c: Point) -> float:
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def on_segment(a: Point, b: Point, point: Point, epsilon: float = 1e-9) -> bool:
    return (
        min(a[0], b[0]) - epsilon <= point[0] <= max(a[0], b[0]) + epsilon
        and min(a[1], b[1]) - epsilon <= point[1] <= max(a[1], b[1]) + epsilon
    )


def segments_intersect(a: Point, b: Point, c: Point, d: Point) -> bool:
    values = (
        orientation(a, b, c), orientation(a, b, d),
        orientation(c, d, a), orientation(c, d, b),
    )
    if values[0] * values[1] < 0 and values[2] * values[3] < 0:
        return True
    triples = ((a, b, c), (a, b, d), (c, d, a), (c, d, b))
    return any(abs(value) <= 1e-9 and on_segment(*triple) for value, triple in zip(values, triples))


def paths_intersect(first: Sequence[Point], second: Sequence[Point]) -> bool:
    return any(
        segments_intersect(a, b, c, d)
        for a, b in zip(first, first[1:])
        for c, d in zip(second, second[1:])
    )


def ksafe_allows(first: Movement, second: Movement, rows: set[Tuple[int, int, int, int]]) -> bool:
    encoded = (
        LANE_CODE[first[0]], DIRECTION_CODE[first[1]],
        LANE_CODE[second[0]], DIRECTION_CODE[second[1]],
    )
    reverse = (encoded[2], encoded[3], encoded[0], encoded[1])
    return encoded in rows or reverse in rows


def movement_name(movement: Movement) -> str:
    return f"{movement[0]}-{movement[1]}"


def main() -> int:
    args = parse_args()
    movements = load_movements(args.manifest)
    ksafe = load_ksafe(args.scheduler)
    paths, via_lanes = load_paths(args.net, movements)
    rows = []
    for first, second in combinations(sorted(movements), 2):
        same_approach = first[0] == second[0]
        intersects = paths_intersect(paths[first], paths[second])
        allows = ksafe_allows(first, second, ksafe) and not same_approach
        geometry_safe = not intersects and not same_approach
        if allows and not geometry_safe:
            classification = "UNSAFE_KSAFE_ENTRY"
        elif allows and geometry_safe:
            classification = "MATCH_SAFE"
        elif not allows and geometry_safe:
            classification = "CONSERVATIVE_OMISSION"
        else:
            classification = "MATCH_CONFLICT"
        rows.append({
            "movement_a": movement_name(first),
            "movement_b": movement_name(second),
            "via_lanes_a": ";".join(via_lanes[first]),
            "via_lanes_b": ";".join(via_lanes[second]),
            "same_approach": int(same_approach),
            "centerlines_intersect": int(intersects),
            "geometry_safe": int(geometry_safe),
            "ksafe_allows": int(allows),
            "classification": classification,
        })

    args.csv_out.parent.mkdir(parents=True, exist_ok=True)
    with args.csv_out.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    counts = {name: sum(row["classification"] == name for row in rows) for name in (
        "MATCH_SAFE", "MATCH_CONFLICT", "CONSERVATIVE_OMISSION", "UNSAFE_KSAFE_ENTRY"
    )}
    output = {
        "passed": counts["UNSAFE_KSAFE_ENTRY"] == 0,
        "method": "exact intersection of SUMO internal-lane connection centerlines",
        "runtime_classifier_modified": False,
        "ksafe_modified": False,
        "movement_count": len(movements),
        "pair_count": len(rows),
        "counts": counts,
        "unsafe_ksafe_entries": [row for row in rows if row["classification"] == "UNSAFE_KSAFE_ENTRY"],
        "conservative_omissions": [row for row in rows if row["classification"] == "CONSERVATIVE_OMISSION"],
    }
    args.json_out.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n")
    print(json.dumps(output["counts"], sort_keys=True))
    print(f"kSafe geometry soundness: {'PASS' if output['passed'] else 'FAIL'}")
    print(f"CSV:  {args.csv_out}")
    print(f"JSON: {args.json_out}")
    return 0 if output["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
