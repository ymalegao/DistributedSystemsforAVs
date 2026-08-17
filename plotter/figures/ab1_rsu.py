"""Ablation 1 — what RSU units buy, and everything they cost.

Six panels, because "units help" is four separate claims and a reader has to
be able to check each: they raise the fault frontier, and they change
throughput, latency, and message cost. Reporting only the first would be
advocacy; reporting overhead as a single percentage (as the previous version
did) hid that the without-RSU arm sends FEWER messages as k rises, precisely
because its replicas have gone silent.

The top row splits the two arms rather than overlaying them, with one line per
vehicle count, because the claim is about a frontier that MOVES with the
replica set. Overlaying ten lines in one panel would make the movement
unreadable; separating the arms lets the reader compare the same shape twice.

Every point carries its observed min-max range. Near a quorum edge the outcome
is probabilistic rather than a step, so a bare mean over three runs cannot be
distinguished from one lucky run.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab1_rsu"
TITLE = "RSU units: fault tolerance, throughput, latency and cost vs load"
STUDY = 1
SUBPLOTS = (2, 3)
FIGSIZE = (14.5, 8.2)

_ARMS = (("OFF", "control", "without RSU"),
         ("ON", "treatment", "with 4 RSU"))

# (panel title, y label, aggregator) for the cost/performance row, all read at
# k=0: with replicas silenced the arms are not doing the same job, so a
# difference there is not a like-for-like comparison.
_METRICS = (
    ("Throughput", "vehicles cleared / s", aggregate.throughput),
    ("Delay to consensus", "stop to decision (s)", aggregate.stop_to_decision),
    ("Message cost", "messages / vehicle served", aggregate.msgs_per_vehicle),
)


def load(runs):
    ns = discover.ns(runs, "OFF", STUDY)
    if not ns:
        return {}
    ks = discover.ks(runs, study=STUDY)
    tolerance = {arm: {n: [aggregate.commit_rate(discover.cell(runs, STUDY, arm, k, n))
                           for k in ks] for n in ns}
                 for arm, _, _ in _ARMS}
    metrics = {title: {arm: [agg(discover.cell(runs, STUDY, arm, 0, n)) for n in ns]
                       for arm, _, _ in _ARMS}
               for title, _, agg in _METRICS}
    return dict(ns=ns, ks=ks, tolerance=tolerance, metrics=metrics,
                quorum={arm: [aggregate.quorum(discover.cell(runs, STUDY, arm, 0, n))
                              for n in ns] for arm, _, _ in _ARMS})


def _frontier_panel(data, ax, arm, title, show_ylabel):
    ns = data["ns"]
    for i, n in enumerate(ns):
        spec = style.ordered(i, len(ns), f"{n} veh")
        style.errorbar(ax, data["ks"], data["tolerance"][arm][n], spec)
    ax.set_xticks(data["ks"])
    ax.set_ylim(-5, 105)
    style.finish(ax, title=title, xlabel="silent replicas (k)",
                 ylabel="runs reaching consensus (%)" if show_ylabel else None,
                 legend=show_ylabel)


def _metric_panel(data, ax, title, ylabel, show_legend):
    ns = data["ns"]
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        style.errorbar(ax, ns, data["metrics"][title][arm], spec,
                       dashed=(role == "control"))
    ax.set_xticks(ns)
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel,
                 legend=show_legend)


def build(data, axes):
    flat = list(axes.flat)
    _frontier_panel(data, flat[0], "OFF", "Fault tolerance — without RSU", True)
    _frontier_panel(data, flat[1], "ON", "Fault tolerance — with 4 RSU", False)

    # Quorum is what the fault frontier is set by, so stating the measured
    # value keeps the left-hand panels from looking like an unexplained result.
    ns = data["ns"]
    ax = flat[2]
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        style.errorbar(ax, ns, data["quorum"][arm], spec, dashed=(role == "control"))
    ax.set_xticks(ns)
    style.finish(ax, title="Quorum required", xlabel="vehicles",
                 ylabel="replica votes to commit", legend=True)

    for i, (title, ylabel, _) in enumerate(_METRICS):
        _metric_panel(data, flat[3 + i], title, ylabel, show_legend=(i == 0))

    flat[0].figure.suptitle(
        "Ablation 1 — RSU units raise the fault frontier, and what that costs",
        fontsize=12, color=style.INK_PRIMARY)
