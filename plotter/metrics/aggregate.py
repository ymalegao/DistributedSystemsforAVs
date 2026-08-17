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


def msgs_per_vehicle(recs: Sequence[RunRecord], *, committed_only: bool = True) -> Stat:
    """Messages spent per vehicle served — the scale-fair cost metric.

    Raw message count grows with the replica set, so comparing a 4-vehicle arm
    against a 4-vehicle + 4-unit arm on totals conflates "costs more" with "has
    more nodes". Per vehicle served, the two are on the same footing.
    """
    usable = [r for r in recs if r.msgs_emitted and (r.committed or not committed_only)]
    return summarize([r.msgs_per_vehicle for r in usable])


def bytes_sent(recs: Sequence[RunRecord], *, committed_only: bool = True) -> Stat:
    """Payload bytes per run. Same conditioning rule as msgs()."""
    usable = [r for r in recs if r.bytes_by_replica and (r.committed or not committed_only)]
    return summarize([r.bytes_sent for r in usable])


def throughput(recs: Sequence[RunRecord]) -> Stat:
    """Vehicles cleared per second of service window.

    Not conditioned on committing: a run that collapsed still moved whatever
    traffic the stop-sign fallback moved, and excluding those would hide the
    cost of collapse — which is the thing the figure exists to show.
    """
    return summarize([r.throughput for r in recs])


def cleared(recs: Sequence[RunRecord]) -> Stat:
    """Vehicles that physically crossed."""
    return summarize([float(r.cleared) for r in recs])


def clearance_wait(recs: Sequence[RunRecord]) -> Stat:
    """Mean stop -> departure time. See RunRecord.clearance_waits for why this
    rather than the release-time wait."""
    return summarize([r.mean_clearance_wait for r in recs])


def ambulance_clearance_wait(recs: Sequence[RunRecord]) -> Stat:
    """Stop -> departure for the ambulance, identified by its logged role
    rather than a hardcoded vehicle id (which changes with --randomize)."""
    values = []
    for r in recs:
        waits = r.clearance_waits()
        amb = [waits[v] for v in r.ambulance_ids() if v in waits]
        values.append(statistics.mean(amb) if amb else None)
    return summarize(values)


def vehicle_clearance_wait(recs: Sequence[RunRecord], vehicle_id: int) -> Stat:
    """Stop -> departure for one specific vehicle.

    The priority-off arm marks no ambulance, so the two arms cannot be paired by
    role. They are paired by replica id instead: the runner designates the same
    vehicle in both, and only one arm grants it priority.
    """
    return summarize([r.clearance_waits().get(vehicle_id) for r in recs])


def relative_wait(recs: Sequence[RunRecord], vehicle_id: int) -> Stat:
    """One vehicle's wait as a fraction of that run's mean wait.

    Absolute waits grow with traffic in both arms, so the raw number conflates
    "priority helped" with "the queue got longer". The ratio isolates whether
    the vehicle is served ahead of the pack: 1.0 is exactly average.
    """
    values = []
    for r in recs:
        waits = r.clearance_waits()
        mine, mean_wait = waits.get(vehicle_id), r.mean_clearance_wait
        values.append(mine / mean_wait if mine is not None and mean_wait else None)
    return summarize(values)


def fallback_rate(recs: Sequence[RunRecord]) -> Stat:
    """Runs where at least one vehicle crossed on the stop-sign timeout.

    A run can decide an order and still degrade, so this is reported alongside
    commit_rate rather than folded into it.
    """
    return rate(recs, lambda r: r.stopsign_timeouts > 0)


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
