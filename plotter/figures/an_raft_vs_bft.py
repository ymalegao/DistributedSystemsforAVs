"""Cross-protocol comparison: RAFT vs SETU on the same 1-lane WAVE fixture.

RAFT numbers come from ``v2vbenchmark-omnet6`` (Willem-Hendrik Thiart's RAFT C
lib driven by an OMNeT++6 / Veins 5.3.1 WAVE application). Per-vehicle JSON is
emitted by ``RaftMetrics::writeVehicleJSON`` — the schema we use is:

    timestamps_ms.stopped   (first-stop time, ms)
    timestamps_ms.passed    (intersection cleared time, ms)

BFT numbers come from our own ``ab1_OFF_n<N>_k0_rep<R>.log`` files (fault-free,
1-lane, no priority) — the same knobs RAFT is running under.

The plot is a single latency-vs-discharge-rate tradeoff panel; N labels annotate
the per-N means. Discharge rate is used because end-to-end busy-window
throughput includes the traffic-arrival spread and is not comparable across N.
Single-lane figure — not fanned by ``_lanes.expand``.
"""
from __future__ import annotations

import glob
import json
import os
import statistics
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

from ..io import discover, logparse
from .. import style
from . import _lanes, _paper

NAME = 'an_raft_vs_bft'
TITLE = 'RAFT vs SETU'
SUBPLOTS = (1, 1)
FIGSIZE = (6.5, 4.0)

# Default location where v2vbenchmark-omnet6 writes its raft_results.json files.
# ``results/simple_raftwave_<N>veh_allVehicles_nopriority/run_<i>/raft_results.json``.
RAFT_RESULTS_ROOT = Path.home() / "Documents/work/code/v2vbenchmark-omnet6/results"

# Ns we compare on. RAFT only has 1-lane scenarios, and BFT ab1_OFF has these.
N_VALUES = (8, 12, 16, 20)


@dataclass
class _RaftRun:
    """Minimal duck-type of ``RunRecord`` sufficient for ``_paper.tradeoff``.

    Only ``mean_clearance_wait`` and ``throughput`` are consumed by the helper.
    """
    mean_clearance_wait: Optional[float] = None
    throughput: Optional[float] = None
    n: int = 0
    rep: int = 0


def _parse_raft_json(path: Path) -> Optional[_RaftRun]:
    try:
        with open(path) as f:
            entries = json.load(f)
    except (json.JSONDecodeError, FileNotFoundError, OSError):
        return None
    if not entries:
        return None
    passes = []
    waits = []
    for e in entries:
        ts = e.get('timestamps_ms', {})
        p = ts.get('passed')
        wait_ms = e.get('durations_ms', {}).get('total_wait_time')
        if p is None:
            continue
        passes.append(p / 1000.0)
        if e.get('did_stop') and wait_ms is not None:
            waits.append(wait_ms / 1000.0)
    if not waits or len(passes) < 2:
        return None
    instants = sorted(set(round(t, 3) for t in passes))
    if len(instants) < 2:
        return None
    window = instants[-1] - instants[0]
    mean_batch = len(passes) / len(instants)
    thr = mean_batch * (len(instants) - 1) / window if window > 0 else None
    return _RaftRun(
        mean_clearance_wait=statistics.mean(waits),
        throughput=thr,
        n=len(entries),
    )


def _raft_cells(root: Path) -> Dict[int, List[_RaftRun]]:
    cells: Dict[int, List[_RaftRun]] = {n: [] for n in N_VALUES}
    for n in N_VALUES:
        pattern = str(
            root / f"simple_raftwave_{n}veh_allVehicles_nopriority" /
            "run_*/raft_results.json"
        )
        for path in sorted(glob.glob(pattern)):
            rec = _parse_raft_json(Path(path))
            if rec is not None:
                # basename of run_<i>
                m = os.path.basename(os.path.dirname(path))
                try:
                    rec.rep = int(m.rsplit('_', 1)[1])
                except (IndexError, ValueError):
                    rec.rep = 0
                cells[n].append(rec)
    return {n: recs for n, recs in cells.items() if recs}


def _bft_cells(runs) -> Dict[int, List]:
    """BFT baseline: ab1 OFF arm, 1-lane, k=0, at each N in N_VALUES."""
    cells: Dict[int, List] = {}
    for n in N_VALUES:
        recs = discover.cell(runs, study=1, arm='OFF', k=0, n=n)
        comparable = [
            _RaftRun(
                mean_clearance_wait=r.mean_clearance_wait,
                throughput=r.discharge_rate,
                n=n,
                rep=r.key.rep,
            )
            for r in recs
            if r.discharge_rate is not None and r.mean_clearance_wait is not None
        ]
        if comparable:
            cells[n] = comparable
    return cells


def load(runs):
    raft = _raft_cells(RAFT_RESULTS_ROOT)
    bft = _bft_cells(runs)
    if not raft or not bft:
        return {}
    return dict(raft=raft, bft=bft)


def build(data, axes):
    ax = axes  # single-axis figure
    ax.figure.suptitle(TITLE)

    # BFT first (control-style dashed baseline), RAFT on top so its color pops.
    _paper.tradeoff(ax, data['bft'], 'control', 'SETU',
                    label_offset=(5, -13))
    _paper.tradeoff(ax, data['raft'], 'treatment', 'RAFT',
                    label_offset=(5, 9))
    _paper.tradeoff_axes(ax, '')
    ax.set_ylabel('Discharge rate (vehicles/s)')
