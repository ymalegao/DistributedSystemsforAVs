"""Ablation 4 — emergency priority, measured where it can actually matter.

At four vehicles there is no queue to jump, so the previous single-point
version was measuring almost nothing. The effect is supposed to grow with
traffic, so this sweeps load.

Both arms designate the SAME vehicle (the last replica, which starts behind the
queue); only one grants it priority. They cannot be paired by role, because the
priority-off arm marks no ambulance at all.

The right panel carries the claim that survives scaling. Absolute waits rise
with traffic in both arms, so a shrinking absolute gap would be ambiguous; the
ratio to the run's own mean wait asks whether the vehicle is served ahead of
the pack, where 1.0 is exactly average and lower is better.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab4_priority"
TITLE = "Emergency priority vs queue length"
STUDY = 4
SUBPLOTS = (1, 2)
FIGSIZE = (11.5, 4.6)

_ARMS = (("noprio", "control", "no priority (FIFO)"),
         ("prio", "treatment", "ambulance priority"))


def load(runs):
    ns = discover.ns(runs, "prio")
    if not ns:
        return {}
    absolute, relative = {}, {}
    for arm, _, _ in _ARMS:
        # The runner designates the last replica in both arms.
        absolute[arm] = [aggregate.vehicle_clearance_wait(
            discover.cell(runs, STUDY, arm, None, n), n - 1) for n in ns]
        relative[arm] = [aggregate.relative_wait(
            discover.cell(runs, STUDY, arm, None, n), n - 1) for n in ns]
    return dict(ns=ns, absolute=absolute, relative=relative)


def _panel(data, key, ax, *, title, ylabel, unit_line=False):
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        ys = [s.mean if s.mean is not None else float("nan") for s in data[key][arm]]
        ax.plot(data["ns"], ys, marker=spec["marker"], color=spec["color"],
                linewidth=style.LINEWIDTH, markersize=style.MARKERSIZE,
                label=spec["label"])
    if unit_line:
        ax.axhline(1.0, color=style.INK_SECONDARY, lw=0.9, ls="--", zorder=0)
        ax.annotate("same as an average vehicle", xy=(data["ns"][0], 1.0),
                    xytext=(0, 5), textcoords="offset points",
                    fontsize=8, color=style.INK_SECONDARY)
    ax.set_xticks(data["ns"])
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel)


def build(data, axes):
    _panel(data, "absolute", axes[0],
           title="Designated vehicle's wait",
           ylabel="stop to departure (s)")
    _panel(data, "relative", axes[1],
           title="Relative to the rest of the traffic",
           ylabel="wait / mean wait in the same run", unit_line=True)
    axes[0].figure.suptitle(
        "Ablation 4 — priority is worth more the longer the queue",
        fontsize=12, color=style.INK_PRIMARY)
