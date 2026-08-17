"""Every figure the CLI can build.

Adding a figure is: write one module exposing NAME / TITLE / load() / build(),
then list it here. Nothing else changes and no existing figure is touched.

Naming convention
-----------------
A figure's NAME is its PNG filename and its CLI argument, so it has to stay
stable and sort usefully once there are dozens. Every name is
`<family>_<topic>`, lower snake case:

    ab<N>_<topic>   presents ablation study N and reads that study's logs
                    e.g. ab1_rsu, ab4_priority, ab6_ladder
    an_<topic>      analysis across or beneath the studies, not one study's
                    headline result          e.g. an_cost_decomposition
    ref_<topic>     derived from the protocol definition, needs no run data
                    e.g. ref_conflict_matrix

The family prefix is what keeps the set browsable: every ablation figure sorts
next to its study, and anything without a number is by construction not a
study's result. A variant of an existing figure extends the topic rather than
inventing a family, e.g. ab1_rsu_bandwidth.

Run logs follow the matching convention, produced by run_ablations.sh:
    ab<study>_<arm>_n<vehicles>_k<k>_rep<r>.log
with _n and _k omitted for studies that do not sweep them.

NAME_PATTERN is enforced at import, so a name that drifts from the scheme fails
immediately instead of quietly shipping an oddly named PNG.

Optional module attributes:
  STUDY       ablation number it reads (documentation; load() does the lookup)
  SUBPLOTS    (nrows, ncols) for multi-panel figures, default (1, 1)
  FIGSIZE     override the shared default
  NEEDS_RUNS  False for figures derived from the protocol rather than measured
"""
import re

from . import (
    ab1_rsu,
    ab2_attack,
    ab3_baseline,
    ab4_priority,
    ab5_rollback,
    ab6_ladder,
    an_cost_decomposition,
    ref_conflict_matrix,
)

FIGURE_MODULES = (
    ab1_rsu,
    ab2_attack,
    ab3_baseline,
    ab4_priority,
    ab5_rollback,
    ab6_ladder,
    an_cost_decomposition,
    ref_conflict_matrix,
)

NAME_PATTERN = re.compile(r"^(ab[1-9]\d*|an|ref)_[a-z0-9]+(_[a-z0-9]+)*$")

_offenders = [m.NAME for m in FIGURE_MODULES if not NAME_PATTERN.match(m.NAME)]
if _offenders:
    raise SystemExit(
        "figure name(s) do not follow <ab N|an|ref>_<topic>: "
        + ", ".join(_offenders)
    )

REGISTRY = {m.NAME: m for m in FIGURE_MODULES}


def get(name):
    try:
        return REGISTRY[name]
    except KeyError:
        raise SystemExit(
            f"unknown figure {name!r}\navailable: {', '.join(sorted(REGISTRY))}"
        )
