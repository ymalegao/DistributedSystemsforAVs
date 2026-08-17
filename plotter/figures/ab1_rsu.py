"""Ablation 1 — what RSU units buy, and what they cost, across traffic load.

Five small multiples rather than one panel: the claim is that the fault
frontier MOVES with the replica set, and a single operating point cannot show
a frontier moving. Each panel is one vehicle count, so "without RSU consensus
fails at k=2" is read directly off the leftmost panel, and the reader sees
whether that failure point shifts as N grows.

The sixth panel is throughput, because tolerance bought at the cost of moving
less traffic is not obviously a win, and the earlier version of this figure
reported message overhead as a percentage of the without-RSU arm -- which hid
that the without-RSU arm sends fewer messages as k rises precisely because its
replicas have gone silent.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab1_rsu"
TITLE = "RSU units: fault tolerance and throughput vs traffic load"
STUDY = 1
SUBPLOTS = (2, 3)
FIGSIZE = (13.5, 7.4)

_ARMS = (("OFF", "control", "without RSU"),
         ("ON", "treatment", "with 4 RSU"))


def load(runs):
    ns = discover.ns(runs, "OFF")
    if not ns:
        return {}
    ks = discover.ks(runs)
    tolerance, throughput, cost = {}, {}, {}
    for arm, _, _ in _ARMS:
        tolerance[arm] = {
            n: [aggregate.commit_rate(discover.cell(runs, STUDY, arm, k, n)) for k in ks]
            for n in ns
        }
        # Throughput and cost are read at k=0: with faults injected the arms are
        # not doing the same job, so a difference there is not a like-for-like
        # comparison of what the units cost.
        throughput[arm] = [aggregate.throughput(discover.cell(runs, STUDY, arm, 0, n)) for n in ns]
        cost[arm] = [aggregate.msgs_per_vehicle(discover.cell(runs, STUDY, arm, 0, n)) for n in ns]
    return dict(ns=ns, ks=ks, tolerance=tolerance, throughput=throughput, cost=cost)


def _panel_tolerance(data, ax, n, show_ylabel):
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        ys = [s.mean if s.mean is not None else float("nan")
              for s in data["tolerance"][arm][n]]
        # The arms sit on top of each other until the control collapses, so a
        # solid control line would be hidden and read as "not measured".
        ax.plot(data["ks"], ys, marker=spec["marker"], color=spec["color"],
                linestyle="--" if role == "control" else "-",
                linewidth=style.LINEWIDTH, markersize=style.MARKERSIZE,
                label=spec["label"])
    ax.set_xticks(data["ks"])
    ax.set_ylim(-5, 105)
    style.finish(ax, title=f"{n} vehicles",
                 xlabel="silent replicas (k)",
                 ylabel="runs reaching consensus (%)" if show_ylabel else None,
                 legend=False)


def _panel_throughput(data, ax):
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        ys = [s.mean if s.mean is not None else float("nan")
              for s in data["throughput"][arm]]
        ax.plot(data["ns"], ys, marker=spec["marker"], color=spec["color"],
                linewidth=style.LINEWIDTH, markersize=style.MARKERSIZE,
                label=spec["label"])
    ax.set_xticks(data["ns"])
    style.finish(ax, title="Throughput, no faults injected",
                 xlabel="vehicles", ylabel="vehicles cleared per second")


def build(data, axes):
    flat = list(axes.flat)
    ns = data["ns"]
    for i, n in enumerate(ns[:5]):
        _panel_tolerance(data, flat[i], n, show_ylabel=(i % 3 == 0))
    # Whatever panel is left after one per vehicle count carries throughput.
    _panel_throughput(data, flat[5])
    for ax in flat[len(ns):5]:
        ax.set_visible(False)

    handles, labels = flat[0].get_legend_handles_labels()
    flat[0].figure.legend(handles, labels, fontsize=9, frameon=False,
                          labelcolor=style.INK_PRIMARY, ncol=2,
                          loc="upper right", bbox_to_anchor=(0.99, 0.99))
    flat[0].figure.suptitle(
        "Ablation 1 — RSU units raise the fault frontier as traffic grows",
        fontsize=12, color=style.INK_PRIMARY)
