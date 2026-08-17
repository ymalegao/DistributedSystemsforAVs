"""Ablation 5 — rollback admits a late ambulance, and what that costs.

The previous version was one pair of bars, 100% against 0%, which is the same
degenerate shape ablation 2 had: it states the mechanism works without showing
what happens instead, or what is given up.

The left panel decomposes what actually became of each run, because "did not
re-order" is not one outcome. A run can commit an order and simply never admit
the ambulance, or fail to commit and let every vehicle out on the stop-sign
timeout; those are different failures and the reader should see which occurred.

The remaining panels are the price. Re-ordering pauses traffic to revise a
committed schedule, so the ON arm clears fewer vehicles and spends more
messages doing it. A figure showing only the capability would be advocacy.

Runs go to 90s of simulated time so the ambulance physically clears. At the
earlier 30s cap it could only ever be shown re-ordered into the schedule, which
is a materially weaker claim than crossing.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab5_rollback"
TITLE = "Rollback: admitting a late ambulance, and its cost"
STUDY = 5
SUBPLOTS = (1, 3)
FIGSIZE = (13.0, 4.6)

_ARMS = (("rollback_off", "control", "rollback OFF"),
         ("rollback_on", "treatment", "rollback ON"))

# Ordered best-to-worst so the stack reads downward into failure.
_OUTCOMES = (
    ("re-ordered for the ambulance", style.TREATMENT,
     lambda r: r.rollback_fired),
    ("committed, ambulance not admitted", style.CONTROL,
     lambda r: r.committed and not r.rollback_fired),
    ("no commit — stop-sign fallback", style.INK_SECONDARY,
     lambda r: not r.committed),
)


def load(runs):
    cells = {arm: discover.cell(runs, STUDY, arm) for arm, _, _ in _ARMS}
    if not any(cells.values()):
        return {}
    return dict(
        arms=[label for _, _, label in _ARMS],
        outcomes=[[aggregate.rate(cells[arm], pred) for arm, _, _ in _ARMS]
                  for _, _, pred in _OUTCOMES],
        cleared=[aggregate.cleared(cells[arm]) for arm, _, _ in _ARMS],
        cost=[aggregate.msgs_per_vehicle(cells[arm], committed_only=False)
              for arm, _, _ in _ARMS],
    )


def _stacked_outcomes(data, ax):
    xs = list(range(len(data["arms"])))
    bottoms = [0.0] * len(xs)
    for (label, color, _), stats in zip(_OUTCOMES, data["outcomes"]):
        ys = [s.mean if s.mean is not None else 0.0 for s in stats]
        if not any(ys):
            continue
        ax.bar(xs, ys, 0.55, bottom=bottoms, color=color, label=label,
               edgecolor=style.BAR_EDGE, linewidth=style.BAR_EDGEWIDTH)
        bottoms = [b + y for b, y in zip(bottoms, ys)]
    ax.set_xticks(xs)
    ax.set_xticklabels(data["arms"])
    ax.set_ylim(0, 105)
    style.finish(ax, title="What became of each run",
                 ylabel="% of runs", legend_above=True)


def _value_panel(data, key, ax, *, title, ylabel, fmt):
    entries = [(label, role, stat)
               for (_, role, label), stat in zip(_ARMS, data[key])]
    style.paired_bars(ax, entries, value_fmt=fmt)
    style.finish(ax, title=title, ylabel=ylabel, legend=False)


def build(data, axes):
    _stacked_outcomes(data, axes[0])
    _value_panel(data, "cleared", axes[1], fmt="{:.1f}",
                 title="Traffic served in the window",
                 ylabel="vehicles cleared")
    _value_panel(data, "cost", axes[2], fmt="{:.0f}",
                 title="Cost of serving them",
                 ylabel="messages per vehicle")
    axes[0].figure.suptitle(
        "Ablation 5 — rollback admits the ambulance, and pays for it in throughput",
        fontsize=12, color=style.INK_PRIMARY)
