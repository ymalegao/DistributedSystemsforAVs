"""Aggregating repetitions into the numbers figures plot.

The conditioning rules here are research decisions, not implementation detail,
so each carries the reason it exists. Getting them wrong produces a plot that
looks fine and says something false.
"""
import statistics
from dataclasses import dataclass
from typing import List, Optional, Sequence

from ..io.schema import RunRecord


@dataclass
class Cost:
    """Message cost for one cell, with the spread across the runs used."""
    mean: Optional[float]
    n_used: int       # runs that passed the conditioning filter
    n_total: int      # runs that emitted the metric at all
    lo: Optional[int]
    hi: Optional[int]


def commit_rate(recs: Sequence[RunRecord]) -> Optional[float]:
    """Fraction of runs that reached consensus.

    Near the quorum edge this is probabilistic, not a step function, so it must
    be reported as a rate over several repetitions. A single run cannot locate
    the fault frontier.
    """
    if not recs:
        return None
    return statistics.mean(1 if r.committed else 0 for r in recs)


def mean_of(recs: Sequence[RunRecord], attr: str) -> Optional[float]:
    """Mean of a numeric attribute, ignoring runs where it was never observed."""
    vals = [getattr(r, attr) for r in recs if getattr(r, attr) is not None]
    return statistics.mean(vals) if vals else None


def _emitted(recs: Sequence[RunRecord]) -> List[RunRecord]:
    """Runs that reached finish() and so reported Messages_Sent at all."""
    return [r for r in recs if r.msgs_emitted]


def msg_cost(recs: Sequence[RunRecord], *, require_rollback_fired: bool = False) -> Cost:
    """Message cost per run for one cell.

    Always conditioned on the run having COMMITTED: a collapsed run sends fewer
    messages precisely because it died, so averaging failures into the cost
    reads as efficiency when it is the opposite.

    require_rollback_fired additionally restricts to runs where the late-ambulance
    rollback actually ran. Totals are bimodal on that (fired => two consensus
    rounds, roughly double), so blending the two modes is what produced wide,
    meaningless error bars in the 18-vehicle study.
    """
    emitted = _emitted(recs)
    used = [r for r in emitted if r.committed and (r.rollback_fired or not require_rollback_fired)]
    if not used:
        return Cost(None, 0, len(emitted), None, None)
    vals = [r.msgs for r in used]
    return Cost(statistics.mean(vals), len(used), len(emitted), min(vals), max(vals))


def tolerated_faults(n: int) -> int:
    """Silent replicas a view of size n survives: n - quorum, quorum = 2f+1.

    Exact and analytic. It replaced an empirical "consensus rate at each arm's
    own frontier" plot, which compared each arm at its own marginal quorum --
    a coin flip per arm, at a different k each, so the lines were noise and not
    comparable. More repetitions could not have fixed that design flaw.
    """
    f = (n - 1) // 3
    return n - (2 * f + 1)
