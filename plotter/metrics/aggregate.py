"""Aggregating repetitions into the numbers figures plot.

The conditioning rules here are research decisions, not implementation detail,
so each carries the reason it exists. Getting one wrong produces a plot that
looks fine and says something false.
"""
import statistics
from dataclasses import dataclass
from typing import Callable, Optional, Sequence

from ..io.schema import RunRecord


@dataclass
class Stat:
    """A cell's central value, its spread, and how many runs went into it."""
    mean: Optional[float]
    lo: Optional[float]
    hi: Optional[float]
    n: int

    @property
    def yerr(self):
        """[[down], [up]] for matplotlib error bars, or None if not computable."""
        if self.mean is None:
            return None
        return [[self.mean - self.lo], [self.hi - self.mean]]


EMPTY = Stat(None, None, None, 0)


def summarize(values: Sequence[float]) -> Stat:
    """Mean with min-max spread. Spread is min/max, not stdev: repetition counts
    here are small (3-6), where a standard deviation is not meaningful but the
    observed range is."""
    vals = [v for v in values if v is not None]
    if not vals:
        return EMPTY
    return Stat(statistics.mean(vals), min(vals), max(vals), len(vals))


def rate(recs: Sequence[RunRecord], predicate: Callable[[RunRecord], bool]) -> Stat:
    """Fraction of runs satisfying predicate, as a percentage.

    Near the quorum edge outcomes are probabilistic rather than a step, so this
    must be reported over several repetitions; a single run cannot locate a
    fault frontier.
    """
    if not recs:
        return EMPTY
    hits = [100.0 if predicate(r) else 0.0 for r in recs]
    return Stat(statistics.mean(hits), min(hits), max(hits), len(hits))


def commit_rate(recs: Sequence[RunRecord]) -> Stat:
    return rate(recs, lambda r: r.committed)


def attack_success_rate(recs: Sequence[RunRecord]) -> Stat:
    """Runs where the fake-ambulance attack COMMITTED. Lower is safer."""
    return rate(recs, lambda r: r.attack_committed)


def rollback_rate(recs: Sequence[RunRecord]) -> Stat:
    """Runs where the late ambulance was safely re-ordered."""
    return rate(recs, lambda r: r.rollback_fired)


def msgs(recs: Sequence[RunRecord], *, committed_only: bool = True) -> Stat:
    """Messages per run.

    committed_only because a collapsed run sends fewer messages precisely
    because it died -- averaging failures in makes a dying configuration look
    efficient. Runs killed before finish() emit no counter at all and are
    excluded either way.
    """
    usable = [r for r in recs if r.msgs_emitted and (r.committed or not committed_only)]
    return summarize([r.msgs for r in usable])


def bft_latency(recs: Sequence[RunRecord]) -> Stat:
    """Mean PBFT ordering latency in seconds."""
    return summarize([r.mean_bft_latency for r in recs])


def cert_latency(recs: Sequence[RunRecord]) -> Stat:
    """Mean arrival-certificate formation latency in seconds."""
    return summarize([r.mean_cert_latency for r in recs])


def wait_time(recs: Sequence[RunRecord]) -> Stat:
    """Mean per-vehicle intersection wait, averaged within a run then across runs."""
    return summarize([r.mean_wait for r in recs])


def vehicle_wait(recs: Sequence[RunRecord], vehicle_id: int) -> Stat:
    """Wait time for one specific vehicle, e.g. the ambulance."""
    return summarize([r.wait_times().get(vehicle_id) for r in recs])


def tolerated_faults(n: int) -> int:
    """Silent replicas a view of size n survives: n - quorum, quorum = 2f+1.

    Analytic, not measured. It replaced an empirical "commit rate at each arm's
    own frontier" plot, which put both arms at their own marginal quorum -- a
    coin flip each, at a different k per arm, so the lines were noise and not
    comparable. More repetitions could not have fixed that design flaw.
    """
    f = (n - 1) // 3
    return n - (2 * f + 1)
