"""Ablation 1 — what RSU units cost to run.

Fault tolerance is deliberately NOT in this figure. The k sweep behind it was
bounded at f+1, which is short of where quorum actually becomes unreachable
(N-(2f+1)), so 8, 12 and 20 vehicles were never run to their failure point.
Plotting those ranges drew a flat 100% line that reads as "tolerates
everything" when it only means "never tested far enough". The bound is fixed in
run_ablations.sh; the tolerance panels come back once the missing high-k cells
have actually been measured.

What remains is measured end to end: the quorum each configuration requires,
and the three costs of requiring it. All read at k=0, because with replicas
silenced the arms are not doing the same job and a difference between them is
not a like-for-like comparison.

Every point carries its observed min-max range across repetitions.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab1_rsu"
TITLE = "RSU units: quorum, throughput, latency and cost vs load"
STUDY = 1
SUBPLOTS = (1, 3)
FIGSIZE = (15.0, 5.0)

_ARMS = (("OFF", "control", "without RSU"),
         ("ON", "treatment", "with 4 RSU"))

_PANELS = (
    # Quorum is not plotted: it is a configuration constant per arm (2f+1 over
    # the replica count), not a measured outcome, so a panel of it restates the
    # x axis. The cost of raising it is what the remaining three panels show.
    ("Throughput", "vehicles cleared / s", aggregate.throughput),
    # Stop -> departure, not stop -> decision. With one decision instant per
    # run, averaging (decision - stop) over vehicles reduces to
    # decision - mean(stop), which measures when vehicles happened to arrive
    # relative to that instant -- a property of the route file, not of the
    # protocol. This spans the delay a vehicle actually experiences, and means
    # the same thing in both arms. Read it beside "cleared": a vehicle that
    # never departs leaves the average rather than worsening it.
    ("Latency", "mean seconds per vehicle",
     aggregate.clearance_wait),
    ("Message cost", "messages / vehicle served", aggregate.msgs_per_vehicle),
)


def load(runs):
    ns = discover.ns(runs, "OFF", STUDY)
    if not ns:
        return {}
    return dict(
        ns=ns,
        metrics={title: {arm: [agg(discover.cell(runs, STUDY, arm, 0, n)) for n in ns]
                         for arm, _, _ in _ARMS}
                 for title, _, agg in _PANELS},
    )


def build(data, axes):
    for ax, (title, ylabel, _) in zip(axes.flat, _PANELS):
        for arm, role, label in _ARMS:
            style.errorbar(ax, data["ns"], data["metrics"][title][arm],
                           style.series(role, label), dashed=(role == "control"))
        ax.set_xticks(data["ns"])
        style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel,
                     legend=(title == _PANELS[0][0]))
    axes.flat[0].figure.suptitle(
        "Ablation 1 — RSU units raise the quorum, and what that costs",
        fontsize=12, color=style.INK_PRIMARY)
