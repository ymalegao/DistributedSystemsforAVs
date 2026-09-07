#!/usr/bin/env python3
"""Generate approach confusion matrices from the network's real lane geometry.

The four incoming approaches are separated by the intersection's 3.2 m lane
width.  A Gaussian lateral error that crosses half a lane is assigned equally
to the two geometrically adjacent approaches; direct opposite-approach errors
are excluded by geometry.  Rows and columns use N,S,E,W order.

This is the documented provenance of the `approachConfusionMatrix` NED
parameter: a matrix pasted into a config should be reproducible from here, so
the parameter stays a physical quantity rather than a free knob.
"""

from __future__ import annotations

import math
import xml.etree.ElementTree as ET
from pathlib import Path

SIGMAS = (0.0, 0.25, 0.5, 1.0, 2.0)
NETWORK = Path(__file__).with_name("bft_intersection.net.xml")
APPROACHES = "NSEW"
OPPOSITE = {"N": "S", "S": "N", "E": "W", "W": "E"}


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


def main() -> int:
    for sigma in SIGMAS:
        print(f"{sigma}: {fmt(matrix(sigma))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
