#!/usr/bin/env python3
"""Generate and query the checked-in approach confusion-matrix catalog.

The four incoming approaches are separated by the intersection's 3.2 m lane
width.  A Gaussian lateral error that crosses half a lane is assigned equally
to the two geometrically adjacent approaches; direct opposite-approach errors
are excluded by geometry.  Rows and columns use N,S,E,W order.
"""

from __future__ import annotations

import argparse
import csv
import math
import xml.etree.ElementTree as ET
from pathlib import Path

SIGMAS = (0.0, 0.25, 0.5, 1.0, 2.0)
NETWORK = Path(__file__).with_name("bft_intersection.net.xml")
APPROACHES = "NSEW"
OPPOSITE = {"N": "S", "S": "N", "E": "W", "W": "E"}
CATALOG = Path(__file__).with_name("perception_matrices.csv")


def lane_width_m(network: Path = NETWORK) -> float:
    """Read SUMO's lane width from an outer-junction boundary shape."""
    root = ET.parse(network).getroot()
    junction = root.find("./junction[@id='N']")
    if junction is None or not junction.get("shape"):
        raise ValueError(f"cannot derive lane width from {network}")
    points = [tuple(map(float, token.split(","))) for token in junction.get("shape").split()]
    distances = [math.hypot(x1 - x0, y1 - y0) for (x0, y0), (x1, y1) in zip(points, points[1:])]
    width = max(distances, default=0.0)
    if width <= 0.0:
        raise ValueError(f"invalid lane width derived from {network}")
    return width


def matrix(sigma: float, lane_width: float | None = None) -> list[float]:
    if lane_width is None:
        lane_width = lane_width_m()
    if sigma == 0:
        miss = 0.0
    else:
        miss = math.erfc((lane_width / 2.0) / (math.sqrt(2.0) * sigma))
    values: list[float] = []
    for truth in APPROACHES:
        adjacent = [a for a in APPROACHES if a not in (truth, OPPOSITE[truth])]
        for observed in APPROACHES:
            if observed == truth:
                values.append(1.0 - miss)
            elif observed in adjacent:
                values.append(miss / 2.0)
            else:
                values.append(0.0)
    return values


def fmt(values: list[float]) -> str:
    return " ".join(f"{value:.12g}" for value in values)


def read_catalog(path: Path = CATALOG) -> dict[float, list[float]]:
    with path.open(newline="") as stream:
        reader = csv.reader(stream)
        next(reader)
        return {float(row[0]): [float(value) for value in row[1:]] for row in reader}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lookup", type=float)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    rows = {sigma: matrix(sigma) for sigma in SIGMAS}
    if args.lookup is not None:
        catalog = read_catalog()
        if args.lookup not in catalog:
            parser.error(f"sigma must be one of: {', '.join(map(str, SIGMAS))}")
        generated = rows[args.lookup]
        checked_in = catalog[args.lookup]
        if len(checked_in) != 16 or any(
            not math.isclose(a, b, rel_tol=1e-11, abs_tol=1e-14)
            for a, b in zip(generated, checked_in)
        ):
            parser.error("checked-in matrix catalog is stale; run with --write")
        print(fmt(checked_in))
        return 0
    if args.write:
        with CATALOG.open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["sigma_m", *[f"{a}_to_{b}" for a in APPROACHES for b in APPROACHES]])
            for sigma, values in rows.items():
                writer.writerow([sigma, *[f"{value:.12g}" for value in values]])
        return 0
    for sigma, values in rows.items():
        print(f"{sigma}: {fmt(values)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
