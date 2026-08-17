"""Ablation 2 — the firewall's guarantee, the bound on it, and its price.

The previous version was two bars: vanilla commits the attack every time, ours
never. One bar pinned at zero carries no information beyond a sentence, and it
implies the protocol has no breaking point.

Three rows, so each vehicle count is read as a column and the reader can follow
one configuration down through safety, liveness and degradation:

  attack succeeded   does a fabricated ambulance obtain priority
  reached consensus  did the honest system still decide an order
  stop-sign fallback did vehicles cross with BFT bypassed entirely

The second and third rows exist because refusing to commit a false proposal is
not the same as being unaffected. A configuration that blocks every attack by
never deciding anything has not defended the intersection, and the earlier
single-bar figure could not have shown the difference.

Colluders are swept to f+1, one past the threat model the f+1 pre-verification
assumes, so the figure locates the bound instead of asserting there is none.
"""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab2_attack"
TITLE = "Fake-ambulance attack: safety bound, liveness and degradation"
STUDY = 2
SUBPLOTS = (3, 3)
FIGSIZE = (14.0, 10.0)

_ARMS = (("vanilla", "control", "vanilla BFT (no f+1 firewall)"),
         ("ours", "treatment", "ours (f+1 pre-verification)"))

_ROWS = (
    ("attack succeeded", "runs granting false priority (%)",
     aggregate.attack_success_rate, True),
    ("reached consensus", "runs deciding an order (%)",
     aggregate.commit_rate, False),
    ("stop-sign fallback", "runs bypassing BFT (%)",
     aggregate.fallback_rate, False),
)


def _f_of(n):
    return (n - 1) // 3


def load(runs):
    ns = discover.ns(runs, "ours", STUDY)
    if not ns:
        return {}
    data = {}
    for title, _, agg, _ in _ROWS:
        data[title] = {
            arm: {n: [agg(discover.cell(runs, STUDY, arm, c, n))
                      for c in range(1, _f_of(n) + 2)] for n in ns}
            for arm, _, _ in _ARMS
        }
    return dict(ns=ns, rows=data)


def _panel(data, ax, row_title, n, *, ylabel, mark_bound, show_legend):
    cs = list(range(1, _f_of(n) + 2))
    for arm, role, label in _ARMS:
        spec = style.series(role, label)
        style.errorbar(ax, cs, data["rows"][row_title][arm][n], spec,
                       dashed=(role == "control"))
    if mark_bound:
        # Right of f colluders the threat model the firewall assumes is
        # violated, so a failure there is expected rather than a defect.
        ax.axvline(_f_of(n) + 0.5, color=style.INK_SECONDARY, lw=1.0, ls=":")
        ax.annotate("beyond f", xy=(_f_of(n) + 0.55, 50), fontsize=8,
                    color=style.INK_SECONDARY, rotation=90, va="center")
    ax.set_xticks(cs)
    ax.set_ylim(-5, 105)
    style.finish(ax, title=f"{n} veh — {row_title}", xlabel="colluding proposers",
                 ylabel=ylabel, legend=show_legend)


def build(data, axes):
    ns = data["ns"][:3]
    for r, (title, ylabel, _, mark) in enumerate(_ROWS):
        for c, n in enumerate(ns):
            _panel(data, axes[r][c], title, n,
                   ylabel=ylabel if c == 0 else None,
                   mark_bound=mark, show_legend=(r == 0 and c == 0))
        for c in range(len(ns), 3):
            axes[r][c].set_visible(False)
    axes[0][0].figure.suptitle(
        "Ablation 2 — the f+1 firewall holds to its assumed bound, and what it costs",
        fontsize=12, color=style.INK_PRIMARY)
