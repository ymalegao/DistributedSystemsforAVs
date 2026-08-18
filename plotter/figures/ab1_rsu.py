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
SUBPLOTS = (2, 2)
FIGSIZE = (11.5, 8.0)

_ARMS = (("OFF", "control", "without RSU"),
         ("ON", "treatment", "with 4 RSU"))

_PANELS = (
    ("Quorum required", "replica votes to commit", aggregate.quorum),
    ("Throughput", "vehicles cleared / s", aggregate.throughput),
    ("Delay to consensus", "stop to decision (s)", aggregate.stop_to_decision),
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
