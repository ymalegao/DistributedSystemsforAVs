"""Structural invariants of a simulation run — the safety net for refactoring.

WHY these fields and not others
-------------------------------
Consensus timings are NOT reproducible. The bridge runs real ResilientDB PBFT
on worker threads which the app polls on a fixed tick, so thread scheduling
decides which tick delivers a decision: the same binary, with no code change,
was measured emitting Order_Decided_Time at 32.6 in one run and 33.1 in the
next. Anything comparing timings will cry wolf on every run and train you to
ignore the alarm.

What IS stable is the structure of the outcome -- whether every replica reached
a decision, how many batches the schedule produced, what quorum the view used,
how many vehicles fell back to the stop-sign timeout. Those are also exactly
the properties a refactor must not change: a decomposition that preserves the
timings but loses a commit is broken, and one that shifts a timing by a poll
tick is not.

Near the quorum edge even commit success is probabilistic rather than a step,
so a cell is compared as a RATE over repetitions, never as a single run.
"""
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# Reuse the one regex table rather than declaring a second that can drift.
from plotter.io.logparse import (
    RE_ORDER_BATCHES,
    RE_ORDER_DECIDED,
    RE_QUORUM_VOTE,
    RE_ROLLBACK_FIRED,
    RE_STOPSIGN_TIMEOUT,
)

RE_METRICS_REPLICA = re.compile(r"\[METRICS (\d+)\]")


@dataclass
class RunInvariants:
    """The structural facts about one run. No timings, by design."""
    committing_replicas: set = field(default_factory=set)
    reporting_replicas: set = field(default_factory=set)
    n_batches: Optional[int] = None
    quorum: Optional[int] = None
    vote_n: Optional[int] = None
    tolerated_f: Optional[int] = None
    fallbacks: int = 0
    rollback_fired: bool = False

    @property
    def committed(self) -> bool:
        return bool(self.committing_replicas)


def parse(path) -> RunInvariants:
    """Extract the invariants from one simulation log."""
    inv = RunInvariants()
    with open(path, errors="ignore") as fh:
        for line in fh:
            if m := RE_METRICS_REPLICA.search(line):
                inv.reporting_replicas.add(int(m.group(1)))

            if RE_ORDER_DECIDED.search(line):
                if m := RE_ORDER_BATCHES.search(line):
                    inv.committing_replicas.add(int(m.group(1)))
                    inv.n_batches = int(m.group(2))

            if inv.quorum is None:
                if m := RE_QUORUM_VOTE.search(line):
                    inv.vote_n, inv.tolerated_f, inv.quorum = (
                        int(m.group(1)), int(m.group(2)), int(m.group(3)))

            if RE_STOPSIGN_TIMEOUT.search(line):
                inv.fallbacks += 1

            if RE_ROLLBACK_FIRED.search(line):
                inv.rollback_fired = True
    return inv


@dataclass
class Cell:
    """One scenario, aggregated over its repetitions."""
    name: str
    runs: List[RunInvariants] = field(default_factory=list)

    def signature(self) -> Dict:
        """What is compared before vs after a refactor.

        commit_rate is a rate because commit success is genuinely probabilistic
        near the quorum edge. The structural fields are compared as the SET of
        values observed across reps, so a refactor that introduces a new
        schedule size or a different quorum is caught even when it does not
        change how often runs commit.
        """
        n = len(self.runs) or 1
        seen = lambda f: sorted({f(r) for r in self.runs if f(r) is not None})
        return {
            "reps": len(self.runs),
            "commit_rate": round(sum(r.committed for r in self.runs) / n, 3),
            "committing_replicas": seen(lambda r: len(r.committing_replicas)),
            "reporting_replicas": seen(lambda r: len(r.reporting_replicas)),
            "n_batches": seen(lambda r: r.n_batches),
            "quorum": seen(lambda r: r.quorum),
            "vote_n": seen(lambda r: r.vote_n),
            "fallbacks": seen(lambda r: r.fallbacks),
            "rollback_fired": seen(lambda r: r.rollback_fired),
        }


def compare(before: Dict[str, Dict], after: Dict[str, Dict]) -> List[str]:
    """Differences between two signature sets, as human-readable lines.

    Empty result means the refactor preserved every structural invariant.
    """
    diffs = []
    for name in sorted(set(before) | set(after)):
        if name not in before:
            diffs.append(f"{name}: NEW cell (absent before)")
            continue
        if name not in after:
            diffs.append(f"{name}: MISSING cell (present before)")
            continue
        for k in sorted(set(before[name]) | set(after[name])):
            b, a = before[name].get(k), after[name].get(k)
            # Repetition count differing is not a regression in itself; it only
            # weakens the comparison, so report it separately from a real diff.
            if b != a and k != "reps":
                diffs.append(f"{name}.{k}: {b!r} -> {a!r}")
    return diffs
