"""One figure module, two lane arms, two PNGs.

Every study is now run twice: once on the single-lane network and once on the
two-lane one. The two are NOT plotted together. A one-lane and a two-lane
junction are different physical worlds -- different approach lengths, different
junction radius, different movements per phase -- so a shared axis invites the
reader to subtract two numbers that were never measured under the same
conditions. Each lane arm gets its own figure and its own axis.

The lane rides in the log's arm field, which run_ablations.sh writes and
plotter.io.logparse already parses (the arm pattern is lazy, so "OFF_2lane"
survives the _n/_k split intact). One-lane runs keep the bare arm names:

    ab1_OFF_n4_k0_rep1.log          one lane,  arm "OFF"
    ab1_OFF_2lane_n4_k0_rep1.log    two lanes, arm "OFF_2lane"

That asymmetry is deliberate. run()'s cache keys on the log filename, so
renaming the one-lane arms would orphan every existing log and force a full
re-sweep of work that is already measured and unchanged.
"""

LANES = (
    ("1lane", "one lane per approach"),
    ("2lane", "two lanes per approach"),
)


def label(lane):
    """Human-readable lane arm, for a figure's suptitle."""
    return dict(LANES)[lane]


def arm(base, lane):
    """Arm name for `base` under `lane`. One lane keeps the bare name."""
    return base if lane == "1lane" else f"{base}_{lane}"


class LaneVariant:
    """One lane arm of a figure module, presented to the CLI as a module.

    The CLI only ever reaches a figure through attributes -- NAME, TITLE, load,
    build and the optional SUBPLOTS/FIGSIZE/NEEDS_RUNS -- so a plain object that
    forwards the rest to its base is enough, and keeps NAME the PNG filename.
    """

    def __init__(self, base, lane, label):
        self._base = base
        self._lane = lane
        self.NAME = f"{base.NAME}_{lane}"
        self.TITLE = f"{base.TITLE} ({label})"

    # STUDY, SUBPLOTS, FIGSIZE, NEEDS_RUNS and anything added later.
    def __getattr__(self, item):
        return getattr(self._base, item)

    def load(self, runs):
        return self._base.load(runs, self._lane)

    def build(self, data, axes):
        return self._base.build(data, axes, self._lane)


def expand(*modules):
    """Every module crossed with every lane, in registry order."""
    return tuple(LaneVariant(m, lane, label)
                 for m in modules for lane, label in LANES)
