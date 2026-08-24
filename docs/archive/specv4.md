# PicaBFT Rollback Protocol v4 — Fixed-F, One Consensus Path, Staged Implementation

**Status:** Supersedes v3. Incorporates advisor decisions (fixed F, open cancel electorate, committee-mode f-dial, WAIT decisions). Restructured for implementability: minimal new machinery, maximal reuse of the epoch-0 path, five stages with acceptance tests.

**Guiding implementation rule:** `ORDER(e+1)` is semantically identical to `ORDER(0)`. Same `proposeAll()` packing, same QUIET-padding, same view style, same quorum arithmetic. The only difference is *which replicas are expected* and *that a committed CANCEL must precede it*.

---

## 0. Locked design decisions

| # | Decision | Consequence |
|---|----------|-------------|
| D1 | **F is fixed for the run** (`f_star` = declared `toleratedFaults`, else `⌊(N₀−1)/3⌋`). Never recomputed from view size. Adversary model: at most `f_star` of the provisioned keys `K` are Byzantine, globally. | Per-epoch mode deleted from protocol (discussion-only). Evidence signers may be any keyholders in `K` (global budget ⇒ `f_star+1` distinct signers contain an honest one). |
| D2 | **Cancel electorate is the static provisioned view `K`.** CANCEL (and WAIT) are voted under the static config — all provisioned keys. Departed-but-reachable members vote; certified newcomers vote; no membership computation happens at cancel time. Quorum `q(|K|, f_star)` (scenario 15: `q(18,5) = 12` — the current code's `quorum=11` is 2f+1 over 18, intersection 4 < f+1, and must be corrected). Out-of-range replicas learn via cancel-commit gossip (`f_star+1` matching attestations). | Delete electorate policing from Check 13. Sound under D1 (global key budget). **View-source rule (hard rule for implementation):** a voting view may only come from static config or a committed decision — never from a live perception snapshot. Perception feeds proposals; a committed proposal becomes a view. |
| D3 | **QUIET is a scheduling status, never a voting status.** All entries in a committed order — SIGNED or QUIET — are view members and may vote. (This is existing epoch-0 semantics; epoch e+1 inherits it.) | Delete VOTER_SIGNED/SCENE_QUIET voting split from v3. `BuildEpochOrderViewCandidate` must stop filtering to SIGNED-only. |
| D4 | **Tunable f ships in two steps.** Step 1 (now, per advisor): **low-quorum override** — quorum `2f+1` over the full static view. Valid for all benign / honest-leader latency experiments; unsound only against an equivocating primary (§5.2), so Byzantine-leader rows never run in this mode except the deliberate demonstration run. Step 2 (later, as the clustering optimization): **committee mode** — consensus over a deterministic `3f+1` committee with quorum `2f+1`, everyone else a learner via decision gossip — sound under Byzantine leaders. | Stage D implements the override + the equivocation demo; committee mode follows as an optimization stage. §5 specifies both and the boundary between them. |
| D5 | **WAIT is a third committed decision type.** Ledger: `ORDER | CANCEL | WAIT`, all normal PBFT instances at consecutive sequence numbers under the current active view, each with its own pre-verify. | New payload + Check 15. Stage C. |

**Quorum function (one implementation, used everywhere):**
```cpp
// safety: any two quorums of size q over the same N-member view
// intersect in >= f+1 members  (2q - N >= f + 1)
int QuorumForView(int n, int f) { return (n + f + 2) / 2; }  // ceil((n+f+1)/2)
```
With fixed `f_star`, this is **safe at every view size** (even `n < 3f_star+1`); what varies with `n` is liveness — a commit needs `q(n, f_star)` responsive honest members. This safety/liveness split is the paper's core statement about rollback under churn.

---

## 1. Decision ledger

```text
seq=1  ORDER(0)    view = expected₀ (16 entries)           q(16, f_star)
seq=2  CANCEL(0)   view = static K (all provisioned, 18)   q(18, f_star) = 12
seq=3  WAIT(1)     view = static K [optional, TTL-bounded] q(18, f_star) = 12
seq=4  ORDER(1)    view = its own entry list (16 entries)  q(16, f_star)
...
```

One dispatcher: payload type → pre-verify (Check 10/13/14/15) → commit handler. No decision type gets a bespoke consensus path.

**Expected set (the only new membership concept):**
```text
expected_{e+1} = K  ∖  Departed(evidence)
```
`Departed(evidence)` = replicas with departure evidence (TraCI-cleared detection, gossiped `[DEPARTED]`). In scenario 15: `18 − {0,6} = 16`. The proposal must contain exactly one entry per expected member: SIGNED if a fresh e+1 arrival cert exists, QUIET otherwise. The view is all entries; QUIET affects scheduling only (never co-batched; see §6 for QUIET emergency vehicles).

---

## 2. Phase summary

**Phase 1 — evidence & halt (no consensus, unchanged).** Witness triggers → CANCEL_ECHO (signed `cancelledEpoch:reason:reasonRef:replicaId`) → `f_star+1` distinct keyholder echoes form CANCEL_CERT → recallable vehicles halt unilaterally, `cancel_pending_` set, normal proposeAll suppressed. Recallability includes waiting-for-prior-batch vehicles (current `isRecallable()` behavior — keep).

**Phase 2 — CANCEL(e) consensus.** Deterministic proposer (smallest halted ID, rotation on retry) proposes `ResdbCancelHdr + CANCEL_CERT` as a normal PBFT instance under the **static provisioned view K** — no view computation at cancel time. Every keyholder may vote: departed-but-reachable members, certified newcomers, everyone. Quorum `q(|K|, f_star)` (= 12 for the 18-key config; replaces the current `quorum=11`). On commit: authoritative tombstone; no vehicle *begins* crossing under e; cancel-commit gossip attestation broadcast (adoption at `f_star+1`); gossip-adopters advance executor sequence past the cancel seq (`EXECUTOR-GOSSIP-SYNC` — keep); `beginRollbackDiscovery(e+1)` fires **here**.

**Phase 3 — discovery + ORDER(e+1).** Fresh announce/echo/cert for e+1 (echo threshold `f_star+1`). Ambulance certification is prioritized (§6). Proposal gate (§4). `proposeAll()` packs the expected set; bridge installs view over **all entries** with `q(|entries|, f_star)`; PBFT commits; executor schedules; `cancel_pending_` clears.

**WAIT (optional, Phase 3 stopgap).** If the proposer's discovery cannot yet cover the expected set with enough SIGNED entries, it may propose `WAIT(e+1, ttl)` instead — committed "everyone stays halted" (§5).

**Fallback (last resort, unchanged from v3 §11).** If neither ORDER nor WAIT can commit (quorum unreachable) past the terminal timeout: degraded stop-sign serialization — singleton crossing, dwell-gated on observed-empty conflict box, local credential-verified emergency priority, preemptible by any later committed/gossip-adopted order.

---

## 3. Payloads

Unchanged from v3: `CANCEL_ECHO`, `CANCEL_CERT`, `ResdbCancelHdr`, `ResdbRollbackHdr + committed-cancel-reference + ResdbProposeHdr + entries`. (`mode` byte in `ResdbRollbackHdr` is retired with per-epoch mode; keep the field, require it to equal the run configuration — reject otherwise.)

New:
```cpp
struct ResdbWaitHdr {
    uint32_t pending_epoch;     // e+1 being stalled
    uint32_t cancelled_epoch;   // e
    uint32_t ttl_ms;            // bounded validity
    uint32_t cancel_ref_len;    // committed-cancel reference follows
};
```

---

## 4. Pre-verify checks

**Check 13 — CANCEL (simplified per D2):**
1. Parses; reason valid; justification in bounds; `cancelled_epoch` locally committed.
2. Already tombstoned → **accept as idempotent no-op** (never reject; rejection causes VC churn on proposer races).
3. Justification is a valid CANCEL_CERT: ≥ `f_star+1` distinct signers, all provisioned keys, all signatures verify, all signed strings agree and match header.
4. EMERGENCY: Emergency-CA credential verifies ∧ ambulance ∉ committed epoch-e order. CRASH: reasonRef batch pair re-derives from the deterministic scheduler over committed epoch-e bytes.
5. *(deleted from v3: electorate membership policing)*

**Check 14 — ORDER(e+1):**
1. Committed-cancel reference required (raw CANCEL_CERT rejected). If the follower hasn't adopted CANCEL(e) yet, **validate and adopt from the embedded commit proof, then continue** (applies to any replica, not just newcomers — removes a partition liveness trap).
2. `new_epoch = cancelled_epoch + 1`.
3. **Expected-set coverage:** entries = exactly one per member of `expected_{e+1}` as the follower computes it. Follower rejects if a member for which it holds a fresh cert and no departure evidence is missing or marked departed (`[ROLLBACK-OMISSION-REJECT]`), or if a member with departure evidence known to the follower is included as SIGNED. Tolerance: an entry the follower would mark SIGNED appearing as QUIET is **accepted** (proposer may have missed the cert; QUIET is the conservative direction).
4. Proposer ∈ entries. `hdr.mode` == run configuration.
5. View/quorum consistency: bridge installs view over all entries, `q(|entries|, f_star)`.

**Check 15 — WAIT:**
1. `cancel_ref` matches a committed/adopted CANCEL(e) with no committed ORDER(e+1).
2. `ttl_ms ≤ waitTtlMaxMs` (config).
3. **Lazy-leader rejection:** if the follower's own discovery holds fresh certs covering ≥ `waitRejectSignedFrac` (default: all) of the expected set, reject as bad proposal → existing bad-proposal/view-change path removes the leader. (Deterministic over the follower's local evidence — this is the advisor's "vote the leader out," grounded in certs rather than road perception.)
4. On commit: halted state re-affirmed until TTL expiry or a later committed decision; consecutive WAITs beyond `waitMaxConsecutive` force proposer rotation.

---

## 5. Tunable f: low-quorum mode now, committee/clustering as the optimization

### 5.1 Low-quorum mode (Stage D — implement first, per advisor)

Declared `f < capacity` lowers the quorum directly, full electorate unchanged:

```text
view    = static K (or the epoch's entry list, unchanged)
quorum  = 2f + 1          // e.g. f=1 → quorum 3 over 16 voters
```

Latency benefit: a commit needs only the **fastest 2f+1 responders of the whole view** — this is the maximum speed the dial can ever reach (even faster than committee mode, which waits for 2f+1 of a fixed 4). Cert/gossip thresholds scale to `f+1` as before.

**Validity scope (must be enforced by the experiment harness):** sound whenever the primary does not equivocate — i.e., all benign runs and all Byzantine-*follower* runs. Every planned latency experiment (T1′) qualifies. Byzantine-**leader** scenarios must not run in this mode, with one exception: the demonstration run below.

### 5.2 The boundary, demonstrated (experiment T3′)

With quorum `2f+1 < q(N, f)`, two quorums need not intersect, and a single equivocating primary can split the honest set: at f=1, N=16, quorum 3, the primary sends valid schedule X to one radio pocket and valid schedule Y to another; {A, B, primary} commit X while {C, D, primary} commit Y — fifteen honest replicas, two conflicting committed schedules, two incompatible batches authorized into the conflict box. Intersection (`2q − N ≥ f+1`) is the mechanism that forces one honest replica into both quorums to refuse the second proposal; low-quorum mode trades it away.

**T3′:** run exactly this — equivocating primary, f=1, low-quorum mode — and capture the split commit and (detection-off) the unsafe co-authorization. The signed equivocating PRE-PREPAREs are attributable evidence. This run motivates §5.3 empirically and is the paper's bridge from the dial to clustering.

### 5.3 Committee mode (clustering optimization — later stage)

Restore soundness at the same declared f by restricting the *voter set* instead of just the quorum:

```text
committee(view, f) = the 3f+1 lowest replica IDs among the view's entries
consensus          = PBFT over the committee only; quorum 2f+1 (= q(3f+1, f))
everyone else      = learner; adopts via existing decision gossip (f+1 matching)
```

Classical soundness (committee is exactly N=3f+1, quorums intersect in f+1); non-committee votes dropped (`[ACTIVE-VOTE-DROP] reason=not-in-committee`); committee membership deterministic from the entry list, verified in pre-verify. Applies uniformly to ORDER/CANCEL/WAIT. Recovery availability: ≥ `3f+1` certified present members to seat a committee — the measured boundary in T2′.

Paper note: committee mode is the BFT analogue of the Raft cluster-mode baseline — revising the capstone's claim that clustering is structurally unavailable to the Byzantine protocol. Present as: dial (§5.1) → demonstrated boundary (§5.2) → clustering fix (§5.3).

---

## 6. Emergency vehicle handling in Phase 3

1. Discovery prioritizes certifying the emergency vehicle: its announces/echoes/cert relays get queue priority.
2. If at proposal time the ambulance is still QUIET: it remains in the entry list (expected set includes it), and the **executor schedules a QUIET entry bearing a valid Emergency-CA credential as the first singleton batch**. Credential verification is unilateral; QUIET-never-co-batch is preserved. Without this rule, a slow cert leaves the ambulance scheduled last and the scenario's purpose fails.
3. Report both paths (ambulance-as-SIGNED vs ambulance-as-QUIET recovery latency) as a measured contrast.

---

## 7. Staged implementation plan (each stage independently runnable + testable)

### Stage A — make seq=3 commit reliably (PBFT vote liveness)
The N=15 run stalled with 15–16 replicas receiving PRE-PREPARE but only one reaching PREPARE quorum. Root-cause hypothesis: votes arriving before the receiver has processed the PRE-PREPARE (radio jitter 1–5ms > 3ms phase gap; relay reordering) are silently dropped at forced-view/request lookup.
1. **Buffer-and-replay:** votes (PREPARE/COMMIT) for unknown `(seq, hash)` go to a bounded pending buffer; on PRE-PREPARE processing / view install, replay and count. Remove reliance on transmit-side phase gaps (keep them, but they are not the correctness mechanism).
2. **Loud drops:** every early-return in vote handling logs `[VOTE-DROP] seq= type= sender= reason=` with reason ∈ {no-request, no-view, inactive-sender, duplicate, hash-mismatch, buffered}.
3. **Instrumented acceptance:** per replica, `count(TYPE8-RECV PREPARE seq=S) − count(PBFT-COUNT increments seq=S) == count(VOTE-DROP seq=S)` — no silent losses.

**Acceptance test A:** re-run the *current* scenario (even with the 15-entry proposal): seq=3 either commits or every non-commit is fully explained by VOTE-DROP reasons. 10/10 runs, no unexplained stalls.

### Stage B — expected-set proposal + epoch-0-style view (fixes N=15)
1. `expected_{e+1} = K ∖ Departed(evidence)`; proposer builds one entry per expected member; stragglers QUIET. Proposal trigger: SIGNED set covers expected, or stability (no new certs for `rollbackStabilityWindowSec`, suggest 1.5s), or `rollbackDiscoveryTimeoutSec` — whichever first; QUIET-pad the remainder.
2. Bridge: `BuildEpochOrderViewCandidate` uses **all entries** (drop the `cyber_status==1` filter); quorum `q(|entries|, f_star)`; log `[ACTIVE-VIEW] ... N=16 f=5 quorum=11`.
3. Check 14.3 omission/inclusion rules follower-side.
4. Ambulance priority rules (§6).

**Acceptance test B:** scenario 15, 10 seeds: every run's epoch-1 proposal has exactly 16 entries (14–16 SIGNED, rest QUIET, veh16 SIGNED or QUIET-emergency-first); `[ACTIVE-VIEW] N=16 f=5 quorum=11`; `OrderDecision: epoch=1` present; ambulance crosses first; analyzer t₁<t₄≤t₅<t₇<t₈ ordering holds; zero unsafe entries.

### Stage C — WAIT
Payload, Check 15, TTL timer, consecutive-WAIT rotation. **Acceptance test C:** (i) forced-slow-discovery run: ≥1 WAIT commits between CANCEL(0) and ORDER(1), ledger shows no un-ordered halted gap; (ii) lazy-leader run: leader proposes WAIT while followers hold full certs → rejection → view change → ORDER(1) commits under new leader.

### Stage D — low-quorum f-dial (advisor's version) + boundary demo
Quorum override `2f+1` wired through the existing `toleratedFaults` path for all decision types; harness rule blocking Byzantine-leader scenarios in this mode. **Acceptance test D:** f ∈ {1,2,3,5} benign sweeps commit with quorum `2f+1`, latency monotonically decreasing in f, zero unsafe entries; T3′ equivocation run produces the split commit with attributable signed evidence (and is labeled as the demonstration, not a regression).

### Stage D2 — committee/clustering optimization (after D)
Deterministic committee from entries; vote filter; learner adoption via decision gossip. **Acceptance test D2:** same f sweep: committee = 3f+1 lowest IDs, quorum 2f+1 over the committee, all learners adopt within the gossip bound; T3′ rerun in committee mode shows no split; latency between low-quorum and full-quorum modes.

### Stage E — hardening
Adversarial batteries: skip-Phase-2 proposal (Check 14.1 reject), omission attack (14.3 reject), duplicate cancel no-op, stale type-9 post-tombstone, lazy-leader WAIT, fallback-entry run (quorum unreachable → stop-sign mode → ambulance served → zero co-occupancy).

---

## 8. Experiments this unlocks

- **R1/R2 (money plot):** unchanged; Stage B makes it a sweep.
- **T1′ (f-dial latency):** decision latency / bandwidth vs declared f ∈ {1..5} at N=16–20, low-quorum mode (honest leaders). Expect steep cert-phase gains (f+1: 6→2) and consensus gains (quorum 11→3, fastest-responders effect). Rerun key points in committee mode after Stage D2 for the three-way comparison (full / committee / low-quorum).
- **T2′ (rollback availability vs f and churn):** for each f, increase departures/Δ until recovery unavailable; empirical boundary vs predicted (full mode: responsive ≥ q(n, f_star); committee mode: certified-present ≥ 3f+1). Overlay predicted lines on measured crossovers.
- **T3′ (equivocation boundary demo):** equivocating primary at f=1 in low-quorum mode → split commit captured with attributable signed evidence; identical run in committee mode → no split. The figure that motivates clustering.
- **T3 (mis-set f):** actual Byzantine > declared f; document first property violated and that the evidence is attributable.
- **W1 (lazy leader):** WAIT-attack detection latency and recovery.

---

## 9. Invariants (updated)

- **S1 (agreement per instance):** every committed decision uses `q(n_view, f_star)` (or committee `2f+1` over `3f+1`); quorums over the same view intersect in ≥ f_star+1 (resp. f+1).
- **S2 (justified cancel):** committed CANCEL implies ≥1 honest witness signed matching evidence.
- **S3 (stop-before-replace):** no ORDER(e+1) without a committed-cancel reference for e.
- **S4 (safety under churn):** with fixed f_star and q(n,f_star), no view size makes a commit unsafe; churn affects liveness only.
- **S5 (bounded omission):** an ORDER(e+1) omitting a vehicle whose fresh cert reached enough honest followers to deny quorum cannot commit (epidemic cert relay is the propagation mechanism).
- **S6 (QUIET never co-batched):** including QUIET-emergency-first singleton scheduling.
- **S7 (monotone degradation):** halt, WAIT, and fallback only reduce authorized concurrency.
- **L1–L3:** cancel/recovery liveness under responsive-quorum conditions; fallback serves the emergency vehicle otherwise.

---

## 10. Analyzer predicate deltas

1. Epoch-1 `[ACTIVE-VIEW]`: `N == |expected|`, `f == f_star`, `quorum == q(N, f_star)`. CANCEL/WAIT instances: `N == |K|`, `quorum == q(|K|, f_star)` (18-key config: 12, not 11).
1b. Low-quorum-mode runs: quorum `== 2f+1` on every instance **and** scenario metadata confirms no Byzantine-leader attack is active (except tagged T3′ runs).
2. Entry-count predicate: epoch-1 entries ≡ expected set; SIGNED/QUIET counts logged.
3. `VOTE-DROP` accounting closes (Stage A identity) for every consensus instance.
4. WAIT instances: `ttl` bounded, cancel_ref valid, no ORDER committed during an accepted WAIT's scope other than its successor.
5. Committee runs: voter set == 3f+1 lowest IDs; no counted vote from a learner.
6. Ledger continuity: every inter-decision interval where vehicles are halted is covered by cancel_pending_, an active WAIT, or fallback — no unexplained halted gaps.
7. Existing R0 timeline/tombstone/safety predicates unchanged.