"""Ablation 4 — emergency priority, measured where it can actually matter.

At four vehicles there is no queue to jump, so the previous single-point
version measured almost nothing. The effect is supposed to grow with traffic.

Both arms designate the SAME vehicle (the last replica, which starts behind the
queue); only one grants it priority. They cannot be paired by role, because the
priority-off arm marks no ambulance at all.

Four panels, because "the ambulance got through faster" is not on its own a
result worth reporting. The second panel normalises by the run's own mean, so
the claim survives traffic growing in both arms. The third asks what priority
cost everyone else -- a scheme that clears the ambulance by stalling the other
vehicles is a different proposition, and a figure showing only the ambulance
could not distinguish the two. It averages the OTHER vehicles only: with the
ambulance folded in, its own saving cancelled part of the cost it caused, and
at N=8 that flipped the sign from +0.33s to -0.09s, i.e. from a cost to an
apparent free lunch. The fourth checks it is not paid for in
throughput.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab4_priority"
TITLE = "Emergency priority vs queue length"
STUDY = 4
SUBPLOTS = (2, 2)
FIGSIZE = (11.5, 8.0)

_ARMS = (("noprio", "control", "no priority (FIFO)"),
         ("prio", "treatment", "priority"))

# Excluded, for the reason that makes this ablation meaningful at all:
# priority is the right to jump a queue, and at 4 vehicles there is no
# queue. That route file puts all four cars at departPos=0, so they reach
# the stop line together (stop spread 0.0s) and the designated vehicle is
# not behind anyone. From 8 on there are 2-5 ranks per approach and being
# moved to the front actually means something.
MIN_N = 8


def load(runs):
    ns = [n for n in discover.ns(runs, "prio", STUDY) if n >= MIN_N]
    if not ns:
        return {}
    cells = {arm: [discover.cell(runs, STUDY, arm, None, n) for n in ns]
             for arm, _, _ in _ARMS}
    return dict(
        ns=ns,
        # The same vehicle (last replica) in both arms; only one grants it priority.
        absolute={arm: [aggregate.vehicle_clearance_wait(c, n - 1)
                        for c, n in zip(cells[arm], ns)] for arm, _, _ in _ARMS},
        relative={arm: [aggregate.relative_wait(c, n - 1)
                        for c, n in zip(cells[arm], ns)] for arm, _, _ in _ARMS},
        # Excludes the priority vehicle itself. Averaging it in let its own
        # speedup cancel part of the cost it imposed, which at N=8 reversed the
        # sign: -0.09s (priority reads as free) against +0.33s without it.
        others={arm: [aggregate.others_clearance_wait(c, n - 1)
                      for c, n in zip(cells[arm], ns)] for arm, _, _ in _ARMS},
        cost={arm: [aggregate.msgs_per_vehicle(c) for c in cells[arm]]
              for arm, _, _ in _ARMS},
        thru={arm: [aggregate.throughput(c) for c in cells[arm]]
              for arm, _, _ in _ARMS},
    )


def _panel(data, key, ax, *, title, ylabel, legend=False):
    for arm, role, label in _ARMS:
        style.errorbar(ax, data["ns"], data[key][arm], style.series(role, label),
                       dashed=(role == "control"))
    ax.set_xticks(data["ns"])
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel, legend=legend)


def build(data, axes):
    flat = list(axes.flat)
    _panel(data, "absolute", flat[0], title="Priority vehicle wait time",
           ylabel="stop to departure (s)", legend=True)
    _panel(data, "relative", flat[1], title="Relative to the rest of the traffic",
           ylabel="wait / mean wait in the same run")
    _panel(data, "others", flat[2],
           title="Cost to everyone else (priority vehicle excluded)",
           ylabel="mean wait, other vehicles (s)")
    _panel(data, "thru", flat[3], title="Throughput",
           ylabel="vehicles cleared / s")
    flat[0].figure.suptitle(
        "Ablation 4 — priority is worth more the longer the queue",
        fontsize=12, color=style.INK_PRIMARY)
