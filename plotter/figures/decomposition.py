"""Where the protocol's cost actually goes.

The honest answer to "what do we add on top of vanilla BFT". The earlier
version of this comparison ran the protocol with the firewall switched off and
found the two identical, concluding the additions were free. That measured only
the pre-verification step, which is verification-only and so of course costs
nothing in normal operation. It said nothing about the layer that dominates.

Splitting sent frames by message type shows the real distribution: the
arrival-certificate exchange (announce, echo, cert) carries the overwhelming
majority of traffic, while PBFT ordering itself is a small fraction. That is
the price of establishing who is at the intersection, and it is the number a
reviewer asking "what does your protocol cost over plain PBFT" wants.

Reported as a share as well as an absolute, because both grow with N and the
share answers whether the certificate layer scales worse than ordering does.
"""
from ..io import discover
from ..io.schema import RunRecord
from ..metrics import aggregate
from .. import style

NAME = "decomposition"
TITLE = "Cost decomposition by protocol layer"
STUDY = 1
SUBPLOTS = (1, 2)
FIGSIZE = (12.0, 4.8)

# Ordered so the dominant layer sits at the bottom of the stack.
LAYERS = (("arrival certificates", RunRecord.ARRIVAL_CERT_TYPES, style.TREATMENT),
          ("PBFT ordering", RunRecord.PBFT_TYPES, style.CONTROL),
          ("gossip", RunRecord.GOSSIP_TYPES, style.ACCENT))


def load(runs):
    ns = discover.ns(runs, "OFF", STUDY)
    if not ns:
        return {}
    # k=0 only: with replicas silenced the traffic mix reflects failure, not
    # the protocol's normal composition.
    cells = [discover.cell(runs, STUDY, "OFF", 0, n) for n in ns]
    if not any(cells):
        return {}
    absolute, share = {}, {}
    for label, types, _ in LAYERS:
        absolute[label] = [aggregate.summarize([r.layer_msgs(types) for r in c])
                           for c in cells]
        share[label] = [aggregate.summarize([r.layer_share(types) for r in c])
                        for c in cells]
    return dict(ns=ns, absolute=absolute, share=share)


def _stacked(data, key, ax, *, title, ylabel):
    bottoms = [0.0] * len(data["ns"])
    xs = list(range(len(data["ns"])))
    for label, _, color in LAYERS:
        ys = [s.mean if s.mean is not None else 0.0 for s in data[key][label]]
        ax.bar(xs, ys, 0.6, bottom=bottoms, color=color, label=label,
               edgecolor=style.BAR_EDGE, linewidth=style.BAR_EDGEWIDTH)
        bottoms = [b + y for b, y in zip(bottoms, ys)]
    ax.set_xticks(xs)
    ax.set_xticklabels([str(n) for n in data["ns"]])
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel,
                 legend_above=(key == "absolute"))


def build(data, axes):
    _stacked(data, "absolute", axes[0],
             title="Messages sent, by layer",
             ylabel="messages per run")
    _stacked(data, "share", axes[1],
             title="Share of all traffic",
             ylabel="% of messages sent")
    axes[1].legend_ = None
    axes[0].figure.suptitle(
        "The certificate layer, not PBFT ordering, is what the protocol costs",
        fontsize=12, color=style.INK_PRIMARY)
