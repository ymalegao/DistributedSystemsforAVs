"""Ablation 3 — consensus control vs an all-way stop, as traffic grows.

Two corrections to the previous version, both of which changed the result.

It compared Resume - Stop, which is not the same quantity in the two arms: the
all-way-stop baseline resumes a vehicle when it clears, while consensus
releases a vehicle seconds before it physically crosses. That flattered the BFT
arm roughly threefold. Both arms emit [CAR-METRICS] with a departure time, so
this figure uses stop -> departure, which means the same thing on both sides.

And it reported one load, at the one vehicle count that has no queue. The
claim is that batching non-conflicting movements beats serialising every
vehicle -- a claim about how the gap GROWS with traffic, which four
simultaneous cars cannot show at all. The sweep now starts at 8.

Four panels: delay, service rate, the spread across individual vehicles (a
mean hides whether one car was starved to speed up the rest), and throughput.

Throughput here starts its clock at the first stop, so it includes the time
cars spend waiting as well as the time they take to discharge. The arms run
different route files and therefore different arrival patterns, so part of the
gap in that panel is the scenario rather than the controller -- service rate is
the clean comparison. Both are shown because the difference between them is
itself the result: it is the dead time consensus adds before anything moves.

Caveat for the talk: the arms use different net/route files, so this compares
two control approaches rather than one scenario with the controller swapped.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab3_baseline"
TITLE = "All-way stop vs consensus control, across load"
STUDY = 3
SUBPLOTS = (2, 2)
FIGSIZE = (11.5, 8.0)

_ARMS = (("baseline", "control", "SUMO all-way stop"),
         ("ours", "treatment", "our BFT protocol"))

# The 4-vehicle scenario is excluded: it is the only one with no queue.
# Its route file puts every car at departPos=0, so all four reach the stop
# line together (stop spread 0.0s) and none ever waits for another to clear.
# From 8 vehicles on there are 2-5 ranks per approach at departPos 0/20/40/
# 60/80, and a trailing car cannot reach the line until its leader has gone.
# That one-off serialisation costs +10.2s of window between 4 and 8 against
# +3.4s for every rank after, which showed up as a throughput dip that is
# geometry, not control. Comparing across it measures the route file.
MIN_N = 8


def load(runs):
    ns = [n for n in discover.ns(runs, "ours", STUDY) if n >= MIN_N]
    if not ns:
        return {}
    cells = {arm: [discover.cell(runs, STUDY, arm, None, n) for n in ns]
             for arm, _, _ in _ARMS}
    # Every individual vehicle's wait, not just the per-run mean: the spread is
    # what shows whether throughput was bought by starving one movement.
    spread = {arm: [[w for c in col for r in c for w in r.clearance_waits().values()]
                    for col in [cells[arm][i:i + 1] for i in range(len(ns))]]
              for arm, _, _ in _ARMS}
    return dict(
        ns=ns,
        wait={arm: [aggregate.clearance_wait(c) for c in cells[arm]] for arm, _, _ in _ARMS},
        rate={arm: [aggregate.discharge_rate(c) for c in cells[arm]] for arm, _, _ in _ARMS},
        thru={arm: [aggregate.throughput(c) for c in cells[arm]]
              for arm, _, _ in _ARMS},
        spread=spread,
    )


def _line_panel(data, key, ax, *, title, ylabel, legend=False, skip_zero=False):
    drew = False
    for arm, role, label in _ARMS:
        stats = data[key][arm]
        if all(s.mean is None for s in stats):
            continue
        # A series that is identically zero is a flat line carrying no
        # information; the all-way stop exchanges no messages at all, which the
        # panel title states instead of drawing.
        if skip_zero and all((s.mean or 0) == 0 for s in stats):
            continue
        style.errorbar(ax, data["ns"], stats, style.series(role, label),
                       dashed=(role == "control"))
        drew = True
    ax.set_xticks(data["ns"])
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel, legend=legend and drew)


def _spread_panel(data, ax):
    """Per-vehicle waits as jittered points, so the distribution is visible."""
    import random
    rng = random.Random(0)          # fixed jitter: the figure must be stable
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        offset = -0.22 if role == "control" else 0.22
        xs, ys = [], []
        for i, n in enumerate(data["ns"]):
            for w in data["spread"][arm][i]:
                xs.append(i + offset + rng.uniform(-0.07, 0.07))
                ys.append(w)
        ax.scatter(xs, ys, s=16, color=spec["color"], alpha=0.55,
                   edgecolors="none", label=label)
    ax.set_xticks(range(len(data["ns"])))
    ax.set_xticklabels([str(n) for n in data["ns"]])
    style.finish(ax, title="Every vehicle, not just the mean",
                 xlabel="vehicles", ylabel="stop to departure (s)")


def build(data, axes):
    flat = list(axes.flat)
    _line_panel(data, "wait", flat[0], title="Time from stopping to clearing",
                ylabel="mean seconds per vehicle", legend=True)
    _line_panel(data, "rate", flat[1],
                title="Service rate once clearing starts",
                ylabel="vehicles / s across departures")
    _spread_panel(data, flat[2])
    _line_panel(data, "thru", flat[3],
                title="Throughput (stop to last departure)",
                ylabel="vehicles cleared / s")
    flat[0].figure.suptitle(
        "Ablation 3 — measured on stop-to-departure, the metric both arms share",
        fontsize=12, color=style.INK_PRIMARY)
