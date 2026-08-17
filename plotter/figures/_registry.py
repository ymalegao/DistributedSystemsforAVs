"""Every figure the CLI can build.

Adding a figure is: write one module exposing NAME / TITLE / load() / build(),
then list it here. Nothing else changes and no existing figure is touched.

Optional module attributes:
  STUDY       ablation number it reads (documentation; load() does the lookup)
  SUBPLOTS    (nrows, ncols) for multi-panel figures, default (1, 1)
  FIGSIZE     override the shared default
  NEEDS_RUNS  False for figures derived from the protocol rather than measured
"""
from . import (
    ab1_rsu,
    ab2_attack,
    ab3_baseline,
    ab4_priority,
    ab5_rollback,
    conflict_matrix,
    decomposition,
    ladder,
)

FIGURE_MODULES = (
    ab1_rsu,
    ab2_attack,
    ab3_baseline,
    ab4_priority,
    ab5_rollback,
    ladder,
    decomposition,
    conflict_matrix,
)

REGISTRY = {m.NAME: m for m in FIGURE_MODULES}


def get(name):
    try:
        return REGISTRY[name]
    except KeyError:
        raise SystemExit(
            f"unknown figure {name!r}\navailable: {', '.join(sorted(REGISTRY))}"
        )
