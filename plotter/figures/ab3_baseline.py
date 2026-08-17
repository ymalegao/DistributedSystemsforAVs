"""Ablation 3 — consensus control vs an all-way stop, as traffic grows.

Two corrections to the previous version.

It compared Resume - Stop, which is not the same quantity in the two arms: the
all-way-stop baseline resumes a vehicle when it clears, while consensus
releases a vehicle seconds before it physically crosses. That flattered the BFT
arm by roughly 3x. Both arms emit [CAR-METRICS] with a departure time, so this
figure uses stop -> departure, which means the same thing on both sides.

And it reported a single load. The claim is that batching non-conflicting
movements beats serialising every vehicle, which is a claim about how the gap
GROWS with traffic; at four vehicles there is barely a queue to win back.

Caveat that belongs in the talk: the arms use different net/route files
(baseline_intersection vs bft_intersection), so this compares two control
approaches rather than one scenario with the controller swapped.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab3_baseline"
TITLE = "All-way stop vs consensus control, across load"
STUDY = 3
SUBPLOTS = (1, 2)
FIGSIZE = (11.5, 4.6)

_ARMS = (("baseline", "control", "SUMO all-way stop"),
         ("ours", "treatment", "our BFT protocol"))


def load(runs):
    ns = discover.ns(runs, "ours")
    if not ns:
        return {}
    wait, thru = {}, {}
    for arm, _, _ in _ARMS:
        cells = [discover.cell(runs, STUDY, arm, None, n) for n in ns]
        wait[arm] = [aggregate.clearance_wait(c) for c in cells]
        thru[arm] = [aggregate.throughput(c) for c in cells]
    return dict(ns=ns, wait=wait, thru=thru)


def _line_panel(data, key, ax, *, title, ylabel):
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        ys = [s.mean if s.mean is not None else float("nan") for s in data[key][arm]]
        errs = [[(s.mean - s.lo) if s.mean is not None else 0 for s in data[key][arm]],
                [(s.hi - s.mean) if s.mean is not None else 0 for s in data[key][arm]]]
        ax.errorbar(data["ns"], ys, yerr=errs, marker=spec["marker"],
                    color=spec["color"], linewidth=style.LINEWIDTH,
                    markersize=style.MARKERSIZE, capsize=4,
                    ecolor=style.INK_SECONDARY, label=spec["label"])
    ax.set_xticks(data["ns"])
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel)


def build(data, axes):
    _line_panel(data, "wait", axes[0],
                title="Time from stopping to clearing",
                ylabel="mean seconds per vehicle")
    _line_panel(data, "thru", axes[1],
                title="Intersection throughput",
                ylabel="vehicles cleared per second")
    axes[0].figure.suptitle(
        "Ablation 3 — measured stop-to-departure, the metric both arms share",
        fontsize=12, color=style.INK_PRIMARY)
