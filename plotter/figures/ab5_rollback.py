"""Ablation 5 — rollback safely admits a late ambulance."""
from ..io import discover
from ..metrics import aggregate
from .. import style

NAME = "ab5_rollback"
TITLE = "Rollback admits a late ambulance"
STUDY = 5


def load(runs):
    return [
        ("rollback OFF", "control",
         aggregate.rollback_rate(discover.cell(runs, STUDY, "rollback_off"))),
        ("rollback ON", "treatment",
         aggregate.rollback_rate(discover.cell(runs, STUDY, "rollback_on"))),
    ]


def build(entries, ax):
    style.paired_bars(ax, entries, value_fmt="{:.0f}%")
    ax.set_ylim(0, 115)
    style.finish(
        ax,
        title="Ablation 5 — rollback safely admits a late ambulance\n"
              "(without it, the committed order cannot be undone)",
        ylabel="runs where the late ambulance was re-ordered (%)",
        legend=False,
    )
