"""Ablation 2 — the firewall's guarantee, and the assumption it rests on.

The previous version was two bars: vanilla commits the attack 100% of the time,
ours 0%. One bar pinned at zero for every run carries no information beyond a
sentence, and it invites the reading that the protocol is simply immune.

Two changes fix that. Sweeping the number of colluding proposers to f+1 tests
the assumption the f+1 pre-verification actually rests on, so the figure shows
where the guarantee ends rather than asserting it has no end. And the lower row
reports whether the honest configuration still reaches consensus: rejecting a
false proposal is only a win if the intersection keeps running, and "refused to
commit anything" is a different outcome from "unaffected".
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab2_attack"
TITLE = "Fake-ambulance attack: safety bound and liveness cost"
STUDY = 2
SUBPLOTS = (2, 3)
FIGSIZE = (13.5, 7.4)

_ARMS = (("vanilla", "control", "vanilla BFT (no f+1 firewall)"),
         ("ours", "treatment", "ours (f+1 pre-verification)"))


def _f_of(n):
    """Faults a view of n tolerates. The sweep runs to f+1, i.e. one past the
    assumption, which is the point the figure exists to locate."""
    return (n - 1) // 3


def load(runs):
    ns = discover.ns(runs, "ours")
    if not ns:
        return {}
    # The swept value rides in the filename's k field; here it means colluders.
    attack, live = {}, {}
    for arm, _, _ in _ARMS:
        attack[arm] = {
            n: [aggregate.attack_success_rate(discover.cell(runs, STUDY, arm, c, n))
                for c in range(1, _f_of(n) + 2)]
            for n in ns
        }
        live[arm] = {
            n: [aggregate.commit_rate(discover.cell(runs, STUDY, arm, c, n))
                for c in range(1, _f_of(n) + 2)]
            for n in ns
        }
    return dict(ns=ns, attack=attack, live=live)


def _sweep_panel(series_by_arm, ax, n, *, title, ylabel, mark_bound):
    cs = list(range(1, _f_of(n) + 2))
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        ys = [s.mean if s.mean is not None else float("nan")
              for s in series_by_arm[arm][n]]
        ax.plot(cs, ys, marker=spec["marker"], color=spec["color"],
                linestyle="--" if role == "control" else "-",
                linewidth=style.LINEWIDTH, markersize=style.MARKERSIZE,
                label=spec["label"])
    if mark_bound:
        # Everything to the right of f colluders violates the threat model the
        # firewall assumes, so a failure there is expected rather than a defect.
        ax.axvline(_f_of(n) + 0.5, color=style.INK_SECONDARY, lw=1.0, ls=":")
        ax.annotate("beyond f", xy=(_f_of(n) + 0.55, 50), fontsize=8,
                    color=style.INK_SECONDARY, rotation=90, va="center")
    ax.set_xticks(cs)
    ax.set_ylim(-5, 105)
    style.finish(ax, title=title, xlabel="colluding proposers",
                 ylabel=ylabel, legend=False)


def build(data, axes):
    flat = list(axes.flat)
    ns = data["ns"][:3]
    for i, n in enumerate(ns):
        _sweep_panel(data["attack"], flat[i], n,
                     title=f"{n} vehicles — attack succeeds",
                     ylabel="runs granting false priority (%)" if i == 0 else None,
                     mark_bound=True)
        _sweep_panel(data["live"], flat[i + 3], n,
                     title=f"{n} vehicles — still reaches consensus",
                     ylabel="runs reaching consensus (%)" if i == 0 else None,
                     mark_bound=False)
    for ax in flat[len(ns):3]:
        ax.set_visible(False)
    for ax in flat[3 + len(ns):]:
        ax.set_visible(False)

    handles, labels = flat[0].get_legend_handles_labels()
    flat[0].figure.legend(handles, labels, fontsize=9, frameon=False,
                          labelcolor=style.INK_PRIMARY, ncol=2,
                          loc="upper right", bbox_to_anchor=(0.99, 0.99))
    flat[0].figure.suptitle(
        "Ablation 2 — the f+1 firewall blocks the attack up to its assumed bound",
        fontsize=12, color=style.INK_PRIMARY)
