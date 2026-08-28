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
    # [CAR-METRICS] is emitted by the BFT app and by BaselineModule (the plain
    # non-protocol vehicle app the traffic-light arm runs) in the same format,
    # which is what makes throughput comparable across ablation 3's two arms.
    # Intersection units never emit it, so these dicts count vehicles only --
    # never replicas.
    stop_at: Dict[int, float] = field(default_factory=dict)
    depart_at: Dict[int, float] = field(default_factory=dict)
    role: Dict[int, str] = field(default_factory=dict)

    # ── latency ──────────────────────────────────────────────────────────────
    bft_latency_s: List[float] = field(default_factory=list)    # PBFT ordering
    cert_latency_s: List[float] = field(default_factory=list)   # arrival-cert f+1
    cert_collection_s: List[float] = field(default_factory=list)  # f+1 round
    # Stop -> decided. Includes waiting for the right to propose, so it is the
    # delay actually experienced, where bft_latency_s starts at proposal.
    stop_to_decision_s: List[float] = field(default_factory=list)

    # ── configuration the run actually used ──────────────────────────────────
    quorum: Optional[int] = None

    # ── per-vehicle timing ───────────────────────────────────────────────────
    stop_time: Dict[int, float] = field(default_factory=dict)
    resume_time: Dict[int, float] = field(default_factory=dict)

    # When each replica saw the order decided. Emitted by every participant,
    # unlike the PHASE_SUMMARY line, which only a replica that actually called
    # proposeAll() emits -- see mean_stop_to_decision below for why that
    # distinction decides whether the metric is comparable across scenarios.
    order_decided_at: Dict[int, float] = field(default_factory=dict)

    # ── log integrity ────────────────────────────────────────────────────────
    # Worker threads and the simulation thread share stdout, so lines splice
    # into each other. A spliced line is not merely missing -- it can present a
    # field with another line's value, which is how a quorum of 6 appeared in a
    # 16-vehicle run. Counted per reason and reported, never silently dropped.
    corrupt_lines: Dict[str, int] = field(default_factory=dict)

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
        """Vehicles cleared per second, counted from the first stop.

        Includes the spread over which traffic ARRIVES, so it is only
        comparable between arms of the same scenario. Across scenarios it moves
        with the route file's spawn pattern: the traffic-light arm's window
        is 6.1s at 4 vehicles but 16.3s at 8, which drags the rate down even
        though the intersection is serving traffic no more slowly. Use
        discharge_rate to compare across vehicle counts.
        """
        window = self.busy_window
        return self.cleared / window if window else None

    @property
    def discharge_rate(self) -> Optional[float]:
        """Vehicles per second across the departure sequence itself.

        Measured first departure to last, so it is the rate the intersection
        actually clears vehicles once it starts, independent of when they
        arrived. This is what the conflict-matrix batching claim is about:
        movements that may cross together depart together.

        Needs at least two departures to have a gap to measure.

        Counted over departure INSTANTS, not vehicles. Compatible movements
        cross together, so a batch leaves at one instant; dividing vehicles by
        the span assumes one departure per instant and overstates the rate
        whenever they batch. With pairs it reads (n-1)/((n/2-1)*gap), which at
        n=8 is 1.56 and at n=20 is 1.38 for an unchanged 1.31 -- a decline that
        is pure small-sample artifact. Batch size x instants per second is the
        rate the intersection actually clears vehicles, and it is stable in n.
        """
        deps = sorted(self.depart_at.values())
        if len(deps) < 2:
            return None
        instants = sorted(set(round(t, 3) for t in deps))
        if len(instants) < 2:
            return None
        span = instants[-1] - instants[0]
        if span <= 0:
            return None
        mean_batch = len(deps) / len(instants)
        return mean_batch * (len(instants) - 1) / span

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
        while in a non-consensus arm the two coincide. Comparing arms on
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

    @property
    def mean_cert_collection(self) -> Optional[float]:
        return mean(self.cert_collection_s) if self.cert_collection_s else None

    @property
    def corrupt_total(self) -> int:
        return sum(self.corrupt_lines.values())

    @property
    def mean_stop_to_decision_proposer(self) -> Optional[float]:
        """Stop -> decided, sampled only where PHASE_SUMMARY was emitted.

        Retained for audit against the corrected metric below, NOT for figures.
        ResDBDecision.cc guards that line with `if (propose_time_ >= 0)`, so it
        exists only on replicas that proposed. How many replicas propose varies
        with the scenario -- one at 16 vehicles, two at 8 and 12, three at 20 --
        and a proposer is systematically an early-stopping, front-of-queue
        vehicle. The sample is therefore neither fixed in size nor unbiased in
        composition, and comparing it across vehicle counts compares different
        populations.
        """
        return mean(self.stop_to_decision_s) if self.stop_to_decision_s else None

    @property
    def mean_stop_to_decision(self) -> Optional[float]:
        """Stop -> decided, over every vehicle that both stopped and decided.

        The delay a vehicle actually experiences before release, which is what
        this metric has always claimed to be. Derived from the per-replica
        Stop_Time and Order_Decided_Time lines that every participant emits,
        so the population is the whole stopped fleet rather than whichever
        replicas happened to propose.

        Intersection units drop out on their own: they never stop at a line and
        so never emit Stop_Time, leaving the average over vehicles only.
        """
        spans = [self.order_decided_at[r] - self.stop_time[r]
                 for r in self.order_decided_at
                 if r in self.stop_time
                 and self.order_decided_at[r] > self.stop_time[r]]
        return mean(spans) if spans else None

    @property
    def bytes_per_vehicle(self) -> Optional[float]:
        if not self.cleared or not self.bytes_by_replica:
            return None
        return self.bytes_sent / self.cleared

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
