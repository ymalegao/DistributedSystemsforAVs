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
could not distinguish the two. The fourth checks it is not paid for in
messages.
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
         ("prio", "treatment", "ambulance priority"))


def load(runs):
    ns = discover.ns(runs, "prio", STUDY)
    if not ns:
        return {}
    cells = {arm: [discover.cell(runs, STUDY, arm, None, n) for n in ns]
             for arm, _, _ in _ARMS}
    return dict(
        ns=ns,
        # The runner designates the last replica in both arms.
        absolute={arm: [aggregate.vehicle_clearance_wait(c, n - 1)
                        for c, n in zip(cells[arm], ns)] for arm, _, _ in _ARMS},
        relative={arm: [aggregate.relative_wait(c, n - 1)
                        for c, n in zip(cells[arm], ns)] for arm, _, _ in _ARMS},
        others={arm: [aggregate.clearance_wait(c) for c in cells[arm]]
                for arm, _, _ in _ARMS},
        cost={arm: [aggregate.msgs_per_vehicle(c) for c in cells[arm]]
              for arm, _, _ in _ARMS},
    )


def _panel(data, key, ax, *, title, ylabel, unit_line=False, legend=False):
    for arm, role, label in _ARMS:
        style.errorbar(ax, data["ns"], data[key][arm], style.series(role, label),
                       dashed=(role == "control"))
    if unit_line:
        ax.axhline(1.0, color=style.INK_SECONDARY, lw=0.9, ls="--", zorder=0)
        ax.annotate("same as an average vehicle", xy=(data["ns"][0], 1.0),
                    xytext=(0, 5), textcoords="offset points",
                    fontsize=8, color=style.INK_SECONDARY)
    ax.set_xticks(data["ns"])
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel, legend=legend)


def build(data, axes):
    flat = list(axes.flat)
    _panel(data, "absolute", flat[0], title="Designated vehicle's wait",
           ylabel="stop to departure (s)", legend=True)
    _panel(data, "relative", flat[1], title="Relative to the rest of the traffic",
           ylabel="wait / mean wait in the same run", unit_line=True)
    _panel(data, "others", flat[2], title="Cost to everyone else",
           ylabel="mean wait, all vehicles (s)")
    _panel(data, "cost", flat[3], title="Coordination cost",
           ylabel="messages per vehicle served")
    flat[0].figure.suptitle(
        "Ablation 4 — priority is worth more the longer the queue",
        fontsize=12, color=style.INK_PRIMARY)
