"""The capability ladder — what each protocol layer adds, and what it costs.

Six stages, each adding one layer to the one before. The order is by
dependency, not by importance, so each stage's effect is attributable:

  S0  ordering with pre-verification off (NOT vanilla PBFT -- see below)
  S1  + f+1 firewall     the admission gate every later layer assumes
  S2  + decision gossip  propagates what S1 admitted; changes no membership
  S3  + priority         a policy over an already-delivered schedule
  S4  + RSU units        the first stage that changes N, f and quorum
  S5  + rollback         revises an order that is already committed

Four panels rather than one bar, because no single metric improves across all
six: each layer fixes a different failure mode, and a chart that averaged them
would imply a progress that does not exist. Panels a-c step up as capability is
added; panel d steps right as it is paid for.

A limit to state plainly: S0 is PBFT ordering with pre-verification switched
off, not textbook PBFT. Arrival-certificate formation supplies membership and
primary election, so a build without it cannot order at all -- there is no
meaningful "no certificates" run to compare against. The certificate layer's
cost is therefore measured by attribution instead: its messages travel as
distinct types, and panel d prices them directly from a normal run.
"""
from ..io import discover
from ..io.schema import RunRecord
from ..metrics import aggregate
from .. import style

NAME = "ladder"
TITLE = "Capability ladder: what each layer adds"
STUDY = 6
SUBPLOTS = (2, 3)
FIGSIZE = (14.5, 8.2)

STAGES = ((0, "S0\nno firewall"), (1, "S1\n+firewall"), (2, "S2\n+gossip"),
          (3, "S3\n+priority"), (4, "S4\n+RSU"), (5, "S5\n+rollback"))


def _cells(runs, scenario):
    """Repetitions per stage for one scenario arm, e.g. "s3_attack"."""
    return [discover.cell(runs, STUDY, f"s{s}_{scenario}") for s, _ in STAGES]


def load(runs):
    if not any(discover.cell(runs, STUDY, f"s{s}_normal") for s, _ in STAGES):
        return {}
    normal, attack, faults = (_cells(runs, s) for s in ("normal", "attack", "faults"))
    return dict(
        # Blocked is the complement of the attack succeeding, so the panel reads
        # as capability gained rather than harm avoided.
        blocked=[aggregate.rate(c, lambda r: not r.attack_committed) for c in attack],
        commit=[aggregate.commit_rate(c) for c in faults],
        throughput=[aggregate.throughput(c) for c in normal],
        cost=[aggregate.msgs_per_vehicle(c) for c in normal],
        latency=[aggregate.stop_to_decision(c) for c in normal],
        bytes=[aggregate.bytes_per_vehicle(c) for c in normal],
    )


def _bar_panel(stats, ax, *, title, ylabel, fmt="{:.0f}", pct=False):
    xs, ys, labels, errs = [], [], [], ([], [])
    for i, ((_, label), stat) in enumerate(zip(STAGES, stats)):
        if stat.mean is None:
            continue
        xs.append(i)
        ys.append(stat.mean)
        labels.append(label)
        errs[0].append(stat.mean - stat.lo)
        errs[1].append(stat.hi - stat.mean)
    if not xs:
        return
    # One hue across the ladder: the bars are stages of one system, not rival
    # conditions, so the control/treatment pairing would misread here.
    ax.bar(xs, ys, 0.62, color=style.TREATMENT, yerr=errs, capsize=3,
           ecolor=style.INK_SECONDARY,
           edgecolor=style.BAR_EDGE, linewidth=style.BAR_EDGEWIDTH)
    for x, y, hi in zip(xs, ys, errs[1]):
        ax.annotate(fmt.format(y), xy=(x, y + hi), xytext=(0, 4),
                    textcoords="offset points", ha="center",
                    fontsize=9, color=style.INK_PRIMARY)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, fontsize=8)
    if pct:
        ax.set_ylim(0, 112)
    style.finish(ax, title=title, ylabel=ylabel, legend=False)


def build(data, axes):
    flat = list(axes.flat)
    _bar_panel(data["blocked"], flat[0], pct=True,
               title="Attack blocked (fake ambulance)",
               ylabel="runs denying false priority (%)")
    _bar_panel(data["commit"], flat[1], pct=True,
               title="Consensus under f silent replicas",
               ylabel="runs reaching consensus (%)")
    _bar_panel(data["throughput"], flat[2], fmt="{:.2f}",
               title="Throughput, no faults",
               ylabel="vehicles cleared per second")
    _bar_panel(data["latency"], flat[3], fmt="{:.1f}",
               title="Delay to consensus",
               ylabel="stop to decision (s)")
    _bar_panel(data["cost"], flat[4],
               title="Message cost per vehicle served",
               ylabel="messages per vehicle")
    _bar_panel(data["bytes"], flat[5], fmt="{:.0f}",
               title="Bandwidth per vehicle served",
               ylabel="payload bytes per vehicle")
    flat[0].figure.suptitle(
        "Each layer buys a different guarantee — and is paid for on the right",
        fontsize=12, color=style.INK_PRIMARY)
