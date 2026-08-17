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
    n     - vehicle count. None for the original single-operating-point runs,
            which encoded no N in the filename because every ablation ran at
            4 vehicles. Kept optional so those logs still parse.
    """
    study: int
    arm: str
    k: Optional[int] = None
    rep: int = 0
    n: Optional[int] = None

    def cell(self) -> tuple:
        """The key runs are grouped by — everything but the repetition."""
        return (self.study, self.arm, self.k, self.n)


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
    # replica, then sum across replicas. Bytes_Sent and Messages_Received come
    # from the same finish() block and follow the same rule.
    msgs_by_replica: Dict[int, int] = field(default_factory=dict)
    bytes_by_replica: Dict[int, int] = field(default_factory=dict)
    recv_by_replica: Dict[int, int] = field(default_factory=dict)

    # (replica, message type) -> (messages, bytes). Lets a protocol layer be
    # priced without a counterfactual build: the arrival-cert exchange and PBFT
    # ordering travel as distinct BFTMessage types.
    sent_by_type: Dict[tuple, tuple] = field(default_factory=dict)

    # ── degradation ──────────────────────────────────────────────────────────
    # A vehicle that crossed on the stop-sign timeout bypassed BFT entirely, so
    # a run can "commit" and still have degraded. Counted, not just flagged,
    # because how MANY cars fell back is the size of the degradation.
    stopsign_timeouts: int = 0
    consensus_timeouts: int = 0

    # ── clearance ────────────────────────────────────────────────────────────
    # [CAR-METRICS] is emitted by the BFT app and the all-way-stop baseline in
    # the same format, which is what makes throughput comparable across
    # ablation 3's two arms. Intersection units never emit it, so these dicts
    # count vehicles only -- never replicas.
    stop_at: Dict[int, float] = field(default_factory=dict)
    depart_at: Dict[int, float] = field(default_factory=dict)
    role: Dict[int, str] = field(default_factory=dict)

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
    def bytes_sent(self) -> int:
        return sum(self.bytes_by_replica.values())

    # BFTMessage types, from ResDBUtil.h and ResDBIntersectionApp.h. Grouped by
    # the protocol layer that owns them, which is the unit the ladder figure
    # reports: what each addition costs on top of plain PBFT ordering.
    ARRIVAL_CERT_TYPES = (1, 4, 5)      # announce, echo, cert
    PBFT_TYPES = (8, 11)                # consensus bytes, consensus relay
    GOSSIP_TYPES = (9, 10)              # order gossip, announce gossip
    ROLLBACK_TYPES = (12, 13, 14)       # cancel echo, cancel cert, cancel gossip

    def layer_msgs(self, types) -> int:
        """Messages sent across all replicas for the given message types."""
        return sum(v[0] for (_, t), v in self.sent_by_type.items() if t in types)

    def layer_bytes(self, types) -> int:
        return sum(v[1] for (_, t), v in self.sent_by_type.items() if t in types)

    def layer_share(self, types) -> Optional[float]:
        """This layer's share of all messages sent, as a percentage."""
        total = sum(v[0] for v in self.sent_by_type.values())
        return 100.0 * self.layer_msgs(types) / total if total else None

    @property
    def cleared(self) -> int:
        """Vehicles that physically crossed the intersection."""
        return len(self.depart_at)

    @property
    def busy_window(self) -> Optional[float]:
        """First vehicle stops -> last vehicle clears.

        The service window, not the simulated duration: sim-time-limit differs
        between scenarios (and the scenario's own [Config] silently overrides
        [General]), so anything divided by wall duration is not comparable
        across configs. This is.
        """
        if not self.depart_at or not self.stop_at:
            return None
        span = max(self.depart_at.values()) - min(self.stop_at.values())
        return span if span > 0 else None

    @property
    def throughput(self) -> Optional[float]:
        """Vehicles cleared per second of service window."""
        window = self.busy_window
        return self.cleared / window if window else None

    @property
    def msgs_per_vehicle(self) -> Optional[float]:
        """Message cost of serving one vehicle.

        Divided by vehicles SERVED rather than by replica count, so a
        configuration that adds replicas (RSU units) to move the same traffic
        is charged for them instead of being credited with a lower per-replica
        average.
        """
        if not self.cleared or not self.msgs_by_replica:
            return None
        return self.msgs / self.cleared

    def clearance_waits(self) -> Dict[int, float]:
        """Per-vehicle stop -> departure time.

        Distinct from wait_times(): Resume_Time is when consensus RELEASED the
        vehicle, which in the BFT arm precedes physically clearing by seconds,
        while in the all-way-stop baseline the two coincide. Comparing arms on
        release time therefore flatters the BFT arm; this is the metric that
        means the same thing in both.
        """
        return {
            v: self.depart_at[v] - self.stop_at[v]
            for v in self.depart_at
            if v in self.stop_at and self.depart_at[v] >= self.stop_at[v]
        }

    @property
    def mean_clearance_wait(self) -> Optional[float]:
        w = self.clearance_waits()
        return mean(w.values()) if w else None

    def ambulance_ids(self) -> List[int]:
        return [v for v, r in self.role.items() if r == "ambulance"]

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
