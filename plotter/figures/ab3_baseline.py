"""Ablation 3 — consensus control vs an actuated traffic light, as traffic grows.

The comparison is against a signal, not an all-way stop. Surveys of autonomous
intersection management report fixed-time signalling as the most-used baseline
(46%) while explicitly calling for actuated or adaptive instead; all-way stop
control is used by only 8%. An unsignalised junction is also the wrong target
for a different reason -- it deadlocks. Under mixed turn demand it gridlocked
in 7 of 12 runs, resolved only by SUMO's 300s teleport timer, producing 300-600s
"waits" that are an artifact of jam resolution rather than a delay measurement
and that flatten every real arm to the floor of the axis.

The signal is actuated (netconvert --tls.default-type actuated): it extends
green while vehicles are present and ends the phase on a gap. Fixed-time would
have been unfair here in our favour, because this scenario is a single arrival
burst and a fixed cycle would burn green on approaches that emptied and never
refill.

Measured on stop -> departure, which means the same thing in both arms. An
earlier version used Resume - Stop, which does not: consensus releases a vehicle
seconds before it physically crosses, and that flattered this protocol roughly
threefold.

It also reported one load, at the one vehicle count with no queue. The claim is
that batching non-conflicting movements beats serialising vehicles -- a claim
about how the gap GROWS with traffic, which four simultaneous cars cannot show.
The sweep starts at 8.

Four panels: delay, service rate, the spread across individual vehicles (a mean
hides whether one car was starved to speed up the rest), and throughput.

Throughput starts its clock at the first stop, so it includes waiting as well as
discharging. Service rate is the cleaner comparison; both are shown because the
difference between them is itself the result -- the dead time consensus adds
before anything moves.

Both arms share the same route files and the same per-repetition seed, so they
see identical turn draws. The networks differ only in the junction type.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab3_baseline"
TITLE = "Actuated traffic light vs consensus control, across load"
STUDY = 3
SUBPLOTS = (2, 2)
FIGSIZE = (11.5, 8.0)

_ARMS = (("tl", "control", "actuated traffic light"),
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
        # information; a non-consensus arm exchanges no messages at all, which
        # the panel title states instead of drawing.
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
