"""Typed records shared by every figure.

One RunRecord per simulation log. Every field a figure could want is
extracted in a single pass (see logparse), so adding a figure never means
adding another parser.
"""
from dataclasses import dataclass, field
from typing import Dict, Optional


@dataclass(frozen=True)
class RunKey:
    """Identifies one cell of an ablation matrix.

    arm  - "OFF" (no RSU units) or "ON" (with units)
    k    - number of PBFT-silent vehicles
    v    - vehicles at the intersection; None for studies that fix it
    rep  - repetition index
    """
    arm: str
    k: int
    v: Optional[int] = None
    rep: int = 0

    def cell(self) -> tuple:
        """The key runs are grouped by — everything but the repetition."""
        return (self.arm, self.v, self.k)


@dataclass
class RunRecord:
    """Everything extracted from one run log."""
    key: RunKey

    # consensus outcome
    orders: int = 0                 # count of Order_Decided_Time lines
    committed: bool = False         # did an epoch-0 ORDER commit at all

    # safety-relevant degradation
    fallbacks: int = 0              # vehicles that crossed via stop-sign timeout

    # active view actually used
    quorum: Optional[int] = None
    vote_n: Optional[int] = None
    f: Optional[int] = None

    # message cost. Messages_Sent is cumulative and finish()-only, so we keep
    # the LAST value seen per replica and sum across replicas.
    msgs_by_replica: Dict[int, int] = field(default_factory=dict)

    # did the late-ambulance rollback actually fire? Message totals are bimodal
    # on this (fired => two consensus rounds), so cost figures must condition on
    # it rather than averaging across both modes.
    rollback_fired: bool = False

    @property
    def msgs(self) -> int:
        return sum(self.msgs_by_replica.values())

    @property
    def msgs_emitted(self) -> bool:
        """False when the run was killed before finish() — no metric to average."""
        return bool(self.msgs_by_replica)
