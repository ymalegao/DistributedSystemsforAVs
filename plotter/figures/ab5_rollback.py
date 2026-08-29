"""Ablation 5 — rollback admits a late ambulance, and what that costs.

The previous version was one pair of bars, 100% against 0%, which is the same
degenerate shape ablation 2 had: it states the mechanism works without showing
what happens instead, or what is given up.

Decomposing the outcome into a stacked bar did not fix it: with the ambulance
either admitted or not, the stack is one full-height block per arm, which is
the degenerate shape again in a second costume.

The left panel plots individual departures instead, because the measured
result is a substitution. Both arms clear the same number of vehicles -- every
count-based summary therefore shows them as identical -- and what changes is
which vehicles: the ambulance takes a slot an ordinary vehicle would have had,
and the remaining traffic is pushed later. That is invisible to any statistic
over counts, so the panel shows vehicle identity directly.

The remaining panels are the price. Re-ordering pauses traffic to revise a
committed schedule, so the ON arm serves its vehicles more slowly and spends
more messages doing it. A figure showing only the capability would be advocacy.

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
SUBPLOTS = (2, 4)
FIGSIZE = (16.0, 8.4)

_ARMS = (("rollback_off", "control", "rollback OFF"),
         ("rollback_on", "treatment", "rollback ON"))


def _timeline(cells):
    """Which vehicles cleared and when, for one run of each arm.

    This panel exists because the aggregate hides the result. Both arms clear
    the same NUMBER of vehicles, so every count-based summary shows them as
    identical; what differs is WHICH vehicles. Rollback does not reduce how
    many are served, it substitutes the ambulance for an ordinary vehicle and
    pushes the rest later. Only vehicle identity shows that, so the panel plots
    departures individually rather than summarising them.

    One run per arm, not a mean: averaging departure times across runs would
    dissolve exactly the per-vehicle detail the panel is here to show.
    """
    rows = []
    for arm, _, label in _ARMS:
        recs = cells[arm]
        rows.append(None if not recs else
                    (label, dict(recs[0].depart_at), set(recs[0].ambulance_ids())))
    return rows


def load(runs):
    cells = {arm: discover.cell(runs, STUDY, arm) for arm, _, _ in _ARMS}
    if not any(cells.values()):
        return {}
    return dict(
        arms=[label for _, _, label in _ARMS],
        timeline=_timeline(cells),
        cost=[aggregate.msgs_per_vehicle(cells[arm], committed_only=False)
              for arm, _, _ in _ARMS],
        thru=[aggregate.discharge_rate(cells[arm]) for arm, _, _ in _ARMS],
        wait=[aggregate.clearance_wait(cells[arm]) for arm, _, _ in _ARMS],
        latency=[aggregate.stop_to_decision(cells[arm]) for arm, _, _ in _ARMS],
    )


def _departures(data, ax):
    rows = [r for r in data["timeline"] if r is not None]
    if not rows:
        return
    served = [set(dep) for _, dep, _ in rows]
    # A vehicle one arm serves and the other does not. With two arms this is
    # the substitution itself; it is annotated rather than left to the reader
    # to find by comparing two rows of dots.
    only = [s - set().union(*(o for j, o in enumerate(served) if j != i))
            for i, s in enumerate(served)]

    for y, ((label, dep, amb), unique) in enumerate(zip(rows, only)):
        role = _ARMS[y][1]
        # Compatible movements cross together, so a batch departs at one
        # instant and its markers would coincide exactly -- 16 vehicles drawn
        # as 8 dots, contradicting the count in the title. Stacking within the
        # row separates them and makes batch size the thing the eye reads.
        batches = {}
        for v, t in dep.items():
            batches.setdefault(t, []).append(v)
        for t, members in batches.items():
            spread = (len(members) - 1) / 2 * 0.16
            for i, v in enumerate(sorted(members)):
                off = (i - (len(members) - 1) / 2) * 0.16
                if v in unique:
                    marker, color = (("*", style.ACCENT) if v in amb
                                     else ("X", style.INK_SECONDARY))
                    ax.scatter([t], [y + off], s=230 if v in amb else 95,
                               marker=marker, color=color, zorder=4)
                    # Anchor on the batch, not the marker: the label is offset
                    # clear of the row, and a marker stacked above its own
                    # batch-mate would otherwise be written over.
                    ax.annotate(f"veh{v}" + (" (ambulance)" if v in amb else ""),
                                xy=(t, y - spread if y == 0 else y + spread),
                                xytext=(0, 14 if y == 0 else -22),
                                textcoords="offset points", ha="center",
                                fontsize=8.5, color=color)
                else:
                    ax.scatter([t], [y + off], s=46,
                               color=style.series(role)["color"], zorder=3,
                               edgecolor=style.BAR_EDGE, linewidth=1.0)

    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels([label for label, _, _ in rows])
    ax.set_ylim(-0.75, len(rows) - 0.25)
    ax.invert_yaxis()
    style.finish(ax, title=f"Who cleared, and when ({len(served[0])} vehicles either way)",
                 xlabel="simulated time (s)", legend=False)


def _value_panel(data, key, ax, *, title, ylabel, fmt):
    entries = [(label, role, stat)
               for (_, role, label), stat in zip(_ARMS, data[key])]
    style.paired_bars(ax, entries, value_fmt=fmt)
    style.finish(ax, title=title, ylabel=ylabel, legend=False)


def build(data, axes):
    # "Traffic served in the window" is gone: both arms clear 16 either way, so
    # the bar pair was two equal columns stating that the count-based view sees
    # nothing -- which is the premise of the departures panel, not a finding of
    # its own. The four remaining bars are the price of the substitution.
    fig = axes[0][0].figure
    # The departures panel is the result; give it the whole top row. At one cell
    # of a 2x3 grid the 16 markers and their labels overlapped illegibly.
    for ax in axes[0]:
        fig.delaxes(ax)
    top = style.theme(fig.add_subplot(2, 1, 1))
    _departures(data, top)

    flat = list(axes[1])
    _value_panel(data, "thru", flat[0], fmt="{:.2f}",
                 title="Service rate once clearing starts",
                 ylabel="vehicles / s across departures")
    _value_panel(data, "wait", flat[1], fmt="{:.1f}",
                 title="Time from stopping to clearing",
                 ylabel="mean seconds per vehicle")
    _value_panel(data, "latency", flat[2], fmt="{:.1f}",
                 title="Delay to consensus",
                 ylabel="stop to decision (s)")
    _value_panel(data, "cost", flat[3], fmt="{:.0f}",
                 title="Cost of serving them",
                 ylabel="messages per vehicle")
    fig.suptitle(
        "Ablation 5 — rollback admits the ambulance by displacing a vehicle, not by serving fewer",
        fontsize=12, color=style.INK_PRIMARY)
