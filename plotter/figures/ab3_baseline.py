"""Matched traffic-light and BFT scenarios; priority is a third condition."""
from ..io import discover
from . import _lanes, _paper

NAME = 'ab3_baseline'
TITLE = 'Intersection Control Performance'
STUDY = 3
SUBPLOTS = (1, 1)
FIGSIZE = (5.1, 3.9)
# ab4 uses the same cfg_for(..., base) and common_ini(seed) as ab3.
# Keep priority-off as the paired BFT control; do not pool duplicate experiments.
ARMS = ((3, 'tl', 'control', 'Traffic light (actuated)'),
        (4, 'noprio', 'treatment', 'BFT without priority'),
        (4, 'prio', 'accent', 'BFT with priority'))


def load(runs, lane):
    counts = [set(discover.ns(runs, _lanes.arm(a, lane), s)) for s,a,_,_ in ARMS]
    ns = sorted(n for n in set.intersection(*counts) if n >= 8)
    if not ns:
        return {}
    data = {}
    for study, arm, role, label in ARMS:
        data[arm] = {n: discover.cell(runs, study, _lanes.arm(arm, lane), None, n) for n in ns}
    # Compare only repetition IDs present in every condition at the same load.
    for n in ns:
        reps = set.intersection(*[{r.key.rep for r in data[a][n]} for _,a,_,_ in ARMS])
        for _,a,_,_ in ARMS:
            data[a][n] = [r for r in data[a][n] if r.key.rep in reps]
    return data


def build(data, ax, lane):
    for _, arm, role, label in ARMS:
        _paper.tradeoff(ax, data[arm], role, label,
                        label_offset=(0, -14) if role == 'accent' else (0, 10))
    _paper.tradeoff_axes(ax, TITLE)
