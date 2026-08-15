"""Typed records shared by every figure.

One RunRecord per simulation log. Every field any figure needs is extracted in
a single pass (see logparse), so adding a figure never means adding a parser.
"""
from dataclasses import dataclass, field
from statistics import mean
from typing import Dict, List, Optional


@dataclass(frozen=True)
class RunKey:
    """Identifies one run of one ablation.

    study - ablation number, 1..6
    arm   - the condition, e.g. "OFF"/"ON", "ours"/"vanilla", "rollback_on"
    k     - PBFT-silent replicas; None for ablations that do not sweep it
    rep   - repetition index
    """
    study: int
    arm: str
    k: Optional[int] = None
    rep: int = 0

    def cell(self) -> tuple:
        """The key runs are grouped by — everything but the repetition."""
        return (self.study, self.arm, self.k)


@dataclass
class RunRecord:
    """Everything extracted from one run log."""
    key: RunKey

    # ── consensus outcome ────────────────────────────────────────────────────
    committed: bool = False          # an epoch-0 ORDER decided at all

    # ── attack outcome ───────────────────────────────────────────────────────
    # A fake-ambulance transaction that got granted priority. Present => the
    # f+1 pre-verification firewall failed to reject the attack.
    false_priority_granted: int = 0

    # ── safety ───────────────────────────────────────────────────────────────
    # The late-ambulance cancel/rollback actually ran.
    rollback_fired: bool = False

    # ── cost ─────────────────────────────────────────────────────────────────
    # Messages_Sent is cumulative and finish()-only: keep the LAST value per
    # replica, then sum across replicas.
    msgs_by_replica: Dict[int, int] = field(default_factory=dict)

    # ── latency ──────────────────────────────────────────────────────────────
    bft_latency_s: List[float] = field(default_factory=list)    # PBFT ordering
    cert_latency_s: List[float] = field(default_factory=list)   # arrival-cert f+1

    # ── per-vehicle timing ───────────────────────────────────────────────────
    stop_time: Dict[int, float] = field(default_factory=dict)
    resume_time: Dict[int, float] = field(default_factory=dict)

    # -- derived ------------------------------------------------------------
    @property
    def msgs(self) -> int:
        return sum(self.msgs_by_replica.values())

    @property
    def msgs_emitted(self) -> bool:
        """False when the run was killed before finish() — no metric to average."""
        return bool(self.msgs_by_replica)

    @property
    def attack_committed(self) -> bool:
        return self.false_priority_granted > 0

    @property
    def mean_bft_latency(self) -> Optional[float]:
        return mean(self.bft_latency_s) if self.bft_latency_s else None

    @property
    def mean_cert_latency(self) -> Optional[float]:
        return mean(self.cert_latency_s) if self.cert_latency_s else None

    def wait_times(self) -> Dict[int, float]:
        """Per-vehicle intersection wait = Resume - Stop.

        Negative spans are dropped: a vehicle that logged a Resume without a
        matching Stop in the same epoch would otherwise contribute a nonsense
        negative wait. Both the baseline and BFT arms emit these, which is what
        makes ablation 3 a fair comparison.
        """
        return {
            r: self.resume_time[r] - self.stop_time[r]
            for r in self.resume_time
            if r in self.stop_time and self.resume_time[r] >= self.stop_time[r]
        }

    @property
    def mean_wait(self) -> Optional[float]:
        w = self.wait_times()
        return mean(w.values()) if w else None
