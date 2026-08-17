"""The intersection conflict matrix — which movements may cross together.

Unlike every other figure this reads no logs: it is derived from the kSafe
table in incubator-resilientdb/integration/omnet/resdb_intersection_scheduler.cc
(IsSafeToBatch), so it documents the scheduler's rule rather than measuring a
run. The table below MIRRORS that C++ array and must be updated with it.

Encoding, from the scheduler: lane 0=N,1=S,2=E,3=W; direction 0=straight,
1=left, 2=right. Two movements may share a committed batch iff the pair appears
in kSafe -- opposing straights, any two rights, or a right plus the opposing
straight. Same-lane pairs and left turns never batch.
"""
import numpy as np
from matplotlib.colors import ListedColormap

from .. import style

NAME = "ref_conflict_matrix"
TITLE = "Intersection conflict matrix (scheduler batching rule)"
FIGSIZE = (8.2, 7.2)
NEEDS_RUNS = False

# The exact kSafe[12][4] table from the scheduler.
K_SAFE = [(0, 0, 1, 0), (2, 0, 3, 0), (0, 2, 1, 2), (0, 2, 2, 2),
          (0, 2, 3, 2), (1, 2, 2, 2), (1, 2, 3, 2), (2, 2, 3, 2),
          (0, 2, 1, 0), (1, 2, 0, 0), (2, 2, 3, 0), (3, 2, 2, 0)]

LANES = ["N", "S", "E", "W"]
DIR_GLYPHS = ["↑", "↰", "↱"]                      # straight, left, right
MOVES = [(lane, d) for lane in range(4) for d in range(3)]
LABELS = [f"{LANES[lane]}{DIR_GLYPHS[d]}" for lane, d in MOVES]

_SELF, _CONFLICT, _SAFE = -1, 0, 1

# Status encoding, not a series palette. "Safe" is the sparse, interesting class
# so it alone carries saturated color; conflicts recede into the surface rather
# than forming a wall of red. Every cell also carries a glyph, so the state is
# never conveyed by color alone.
_CMAP = ListedColormap(["#c9c8c0", "#f0efe9", style.CONTROL])
_GLYPHS = {_SELF: "–", _CONFLICT: "✗", _SAFE: "✓"}
_GLYPH_INK = {_SELF: style.INK_SECONDARY, _CONFLICT: style.INK_SECONDARY, _SAFE: "#ffffff"}


def is_safe(lane_a, dir_a, lane_b, dir_b) -> bool:
    if lane_a == lane_b:                 # same approach lane never batches
        return False
    return any(
        (lane_a, dir_a, lane_b, dir_b) == (p[0], p[1], p[2], p[3])
        or (lane_a, dir_a, lane_b, dir_b) == (p[2], p[3], p[0], p[1])
        for p in K_SAFE
    )


def safe_pairs():
    """Labels of every unordered movement pair that may batch."""
    return [
        f"{LABELS[i]} + {LABELS[j]}"
        for i, (la, da) in enumerate(MOVES)
        for j, (lb, db) in enumerate(MOVES)
        if j > i and is_safe(la, da, lb, db)
    ]


def load(_runs=None):
    n = len(MOVES)
    matrix = np.zeros((n, n))
    for i, (la, da) in enumerate(MOVES):
        for j, (lb, db) in enumerate(MOVES):
            matrix[i, j] = _SELF if i == j else (
                _SAFE if is_safe(la, da, lb, db) else _CONFLICT)
    return matrix


def build(matrix, ax):
    n = len(MOVES)
    ax.imshow(matrix, cmap=_CMAP, vmin=-1, vmax=1)
    ax.set_xticks(range(n))
    ax.set_xticklabels(LABELS, rotation=90, fontsize=9)
    ax.set_yticks(range(n))
    ax.set_yticklabels(LABELS, fontsize=9)

    for i in range(n):
        for j in range(n):
            state = matrix[i, j]
            ax.text(j, i, _GLYPHS[state], ha="center", va="center",
                    fontsize=9, color=_GLYPH_INK[state])

    # Cell separators; the data grid must not inherit the axes grid.
    ax.set_xticks([x - 0.5 for x in range(1, n)], minor=True)
    ax.set_yticks([y - 0.5 for y in range(1, n)], minor=True)
    ax.grid(False)
    ax.grid(which="minor", color=style.SURFACE, lw=1.2)
    ax.tick_params(which="minor", length=0)

    ax.set_title(
        "Intersection conflict matrix — which movements may cross together\n"
        "✓ = safe to batch (parallel)   ✗ = conflict (must serialize)   – = same movement\n"
        "lane N/S/E/W · ↑ straight ↰ left ↱ right",
        fontsize=10, color=style.INK_PRIMARY)
