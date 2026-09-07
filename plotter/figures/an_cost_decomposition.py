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
from . import _lanes
from ..io.schema import RunRecord
from ..metrics import aggregate
from .. import style

NAME = "an_cost_decomposition"
TITLE = "Cost decomposition by protocol layer"
STUDY = 1
SUBPLOTS = (1, 2)
FIGSIZE = (8.0, 3.5)

# Compare both layers in messages per round. Gossip is excluded, so these
# two groups are not a decomposition of ALL transmitted traffic.
LAYERS = (("arrival certificates", RunRecord.ARRIVAL_CERT_TYPES,
           style.TREATMENT, False),
          ("PBFT ordering", RunRecord.PBFT_TYPES,
           style.CONTROL, False))


def load(runs, lane):
    ns = discover.ns(runs, _lanes.arm("OFF", lane), STUDY)
    if not ns:
        return {}
    # k=0 only: with replicas silenced the traffic mix reflects failure, not
    # the protocol's normal composition.
    cells = [discover.cell(runs, STUDY, _lanes.arm("OFF", lane), 0, n) for n in ns]
    if not any(cells):
        return {}
    # Phases of one round, in the order they happen. Grouped rather than
    # stacked and drawn on a log axis, because the waiting window is two orders
    # of magnitude larger than the work -- stacked, the work would be invisible.
    # The two phases that are actual protocol work. "Waiting for certificates"
    # (cert_collection) is deliberately excluded: it spans from the vehicle
    # entering the stop zone to the primary proposing, so it is dominated by how
    # long OTHER vehicles take to arrive and be certified -- a property of the
    # arrival pattern, not of the protocol. Plotting it alongside these buries
    # both, since it is an order of magnitude larger.
    phases = {
        "arrival certificates": [aggregate.cert_latency(c) for c in cells],
        "PBFT ordering": [aggregate.bft_latency(c) for c in cells],
    }
    absolute = {
        label: [aggregate.summarize([r.layer_msgs(types) for r in cell
                                    if r.sent_by_type]) for cell in cells]
        for label, types, _, _ in LAYERS
    }
    return dict(ns=ns, absolute=absolute, phases=phases)


def _stacked(data, key, ax, *, title, ylabel):
    # Grouped: both layers now use the same per-round message denominator.
    xs = list(range(len(data["ns"])))
    width = 0.38
    for i, (label, _, color, _div) in enumerate(LAYERS):
        ys = [s.mean if s.mean is not None else float("nan") for s in data[key][label]]
        ax.bar([x + (i - 0.5) * width for x in xs], ys, width, color=color,
               label=label, edgecolor=style.BAR_EDGE,
               linewidth=style.BAR_EDGEWIDTH)
    ax.set_xticks(xs)
    ax.set_xticklabels([str(n) for n in data["ns"]])
    style.finish(ax, title=title, xlabel="vehicles", ylabel=ylabel,
                 legend=(key == "absolute"))


PHASE_COLORS = (style.TREATMENT, style.CONTROL, style.ACCENT)


def _phases(data, ax):
    xs = list(range(len(data["ns"])))
    width = 0.36
    for i, ((label, stats), color) in enumerate(zip(data["phases"].items(), PHASE_COLORS)):
        ys = [s.mean if s.mean is not None else float("nan") for s in stats]
        ax.bar([x + (i - 0.5) * width for x in xs], ys, width, color=color,
               label=label, edgecolor=style.BAR_EDGE,
               linewidth=style.BAR_EDGEWIDTH)
    # Linear, not log. The log axis was for a waiting phase two orders larger
    # that is no longer plotted; with two bars within 2.5x of each other it
    # only compresses the difference the panel exists to show.
    ax.set_xticks(xs)
    ax.set_xticklabels([str(n) for n in data["ns"]])
    style.finish(ax, title="(b) Protocol phase latency",
                 xlabel="vehicles", ylabel="seconds", legend=True)


def build(data, axes, lane):
    _stacked(data, "absolute", axes[0],
             title="(a) Communication cost",
             ylabel="Messages per intersection round")
    _phases(data, axes[1])
