# PicaBFT Rollback Protocol v2 — Totally Ordered Cancellation

**Status:** Design spec for implementation. Supersedes the single-shot forced-M rollback path.
**Core change vs v1:** Cancellation becomes its own committed consensus decision, voted by the cancelled epoch's membership, totally ordered against the order it cancels. Re-coordination remains a separate forced-M decision.

---

## 1. Design principles

1. **Halting is unilateral; resuming is consensual.** Any replica with valid cancel evidence halts locally, immediately, with no consensus in the loop. Nothing may begin or resume crossing without a committed decision. Entering the degraded fallback (§7) is a form of halting-plus-caution and is likewise unilateral.
2. **Standing rule.** Every decision is voted by the set with standing over it: epoch e's membership E decides e's cancellation; the rediscovered membership M decides the replacement order. Newcomers learn committed facts via gossip proof; they do not vote on decisions about epochs they were never part of.
3. **Anchored adversary.** The Byzantine budget `f_anchored` is a property of the deployment assumption (provisioned keys), not of who is currently present. It is never recomputed downward because members departed.
4. **Evidence justifies; consensus decides.** Witnessed physical evidence (cancel certs) is the admissible justification for a decision, never the decision itself.
5. **Decision ledger.** The intersection maintains a totally ordered sequence `ORDER(e) → CANCEL(e) → ORDER(e+1) → …`. Every transition carries verifiable physical evidence and is committed by the electorate with standing over it.

---

## 2. Sets, parameters, notation

| Symbol | Definition |
|---|---|
| `K` | All provisioned keyholders (static `server.config`; includes pre-keyed late arrivals and future RSUs). |
| `E` | Epoch e's membership: the active view under which `ORDER(e)` committed (e.g., {0..15}). |
| `D ⊆ E` | Members of E that have physically departed the intersection. Departure removes a vehicle from **M**, not from **E**; a departed-but-radio-reachable member remains a legitimate Phase-2 voter. |
| `M` | Rediscovered epoch-(e+1) membership: present, responsive, recallable survivors of E, plus certified newcomers. `M ⊆ K`, generally `M ≠ E`. |
| `f_anchored` | Declared fault bound for the run (`toleratedFaults`, else `⌊(N_static−1)/3⌋`). One value per run; all thresholds derive from it. |
| `q(N, f)` | `⌈(N + f + 1)/2⌉`. Sound for arbitrary N, f with `f ≤ ⌊(N−1)/3⌋`; guarantees any two quorums intersect in ≥ f+1 members (`2q − N ≥ f+1`). |

**Threshold summary:**

| Artifact | Threshold | Counted over |
|---|---|---|
| ARRIVAL_CERT / CANCEL_CERT echoes | `f_anchored + 1` distinct signers | any valid keyholders in `K` |
| Decision / cancel-commit gossip adoption | `f_anchored + 1` matching signed attestations | any valid keyholders in `K` |
| CANCEL(e) consensus quorum | `q(|E|, f_anchored)` | votes from `E` only |
| ORDER(e+1) consensus quorum | `q(|M|, f_anchored)` | votes from `M` only |

**Signer policy (decided):** cert and gossip *signers* may be any valid keyholder, not only E-members. Rationale: `f_anchored` is a global budget over provisioned keys, so f+1 distinct valid signatures guarantee at least one honest signer regardless of epoch membership; keyholder-signing also lowers evidence latency. *Votes* remain electorate-restricted per the standing rule — signing evidence and voting on decisions are different authorities.

---

## 3. Wire formats

### CANCEL_ECHO (broadcast, ECDSA-signed)
```
signed string:  cancelledEpoch:reason:reasonRef:echoingReplicaId
reason:         0 = CRASH, 1 = EMERGENCY   (2 = DEVIATION reserved, ICRA roadmap)
reasonRef:      EMERGENCY → "amb:<carId>:<cancelledEpoch>"
                CRASH     → "unsafe_batch:<epoch>:<batch>:vehA+vehB"
```
`reasonRef` must be byte-deterministic across honest witnesses: derived only from committed data (the cancelled epoch number, committed batch indices) and the offender's stable ID — never from per-witness observation classifications.

### CANCEL_CERT (broadcast; relay-once per validator)
Bundle of ≥ `f_anchored + 1` echoes: distinct signer IDs, all signatures verify against provisioned keys, all signed strings identical in `cancelledEpoch:reason:reasonRef`. For EMERGENCY, the cert carries the ambulance's Emergency-CA credential bytes.

### CANCEL proposal (Phase 2 payload)
```
ResdbCancelHdr {
    uint32_t cancelled_epoch;    // e
    uint8_t  reason;
    uint8_t  _pad[3];
    uint32_t justification_len;
}
justification[justification_len]  // serialized CANCEL_CERT
```

### ROLLBACK proposal (Phase 3 payload)
```
ResdbRollbackHdr { new_epoch=e+1, cancelled_epoch=e, reason, justification_len }
justification[]                   // committed-cancel reference (§6), NOT the raw CANCEL_CERT
ResdbProposeHdr { epoch=e+1, leader_id, n_vehicles=|M| }
ResdbVehicleEntry[|M|]
```

Committed-cancel reference = { cancel instance sequence number, payload digest, commit proof (the f+1 signed commit attestations used for cancel-commit gossip) }.

---

## 4. Phase 1 — Evidence and halt (no consensus)

**Triggers (existing):**
- `maybeTriggerEmergencyRollbackFromAnnouncement()` / `FromCert()`: post-commit, credential-valid ambulance, absent from `committed_order_vehicle_ids_`, physically corroborated → send CANCEL_ECHO.
- `maybeTriggerCrashRollback()`: unsafe committed batch pair → send CANCEL_ECHO.

**On validating a CANCEL_CERT:**
1. Recallable vehicles (braking-distance predicate) halt immediately via TraCI stop; timers invert to *stay stopped* while `cancel_pending_`.
2. Set `cancel_pending_(e)`; suppress normal `proposeAll()` and normal app-level VC triggers.
3. Provisional (local, non-authoritative) tombstone intent on e.
4. **Do not** begin rollback discovery. Optional latency optimization: pre-warm discovery traffic (announces/echoes for e+1 are harmless before cancel commits), but no rollback proposal may be constructed.

Non-recallable vehicles are not force-stopped. Vehicles already inside the conflict box finish crossing.

---

## 5. Phase 2 — Cancel consensus (electorate = E)

**Proposer:** deterministic — smallest replica ID among the halted current batch, rotated by `rollback_rotation_index_` on retry. Proposer must be in E.

**Instance:** normal PBFT under **epoch e's present-set active view**: electorate E, `f = f_anchored`, quorum `q(|E|, f_anchored)`. Votes from non-E keyholders (newcomers) are dropped (`[ACTIVE-VOTE-DROP]`). Departed-but-reachable E members are legitimate voters. **A faulty cancel proposer is handled by ordinary ResDB view change** — the electorate is a normal membership, so no forced-M VC machinery is needed for this phase.

**Pre-verify — Check 13:**

*Structural:*
1. Payload parses as `ResdbCancelHdr + justification`; `reason` valid; `justification_len` in bounds.
2. `cancelled_epoch` is locally committed.
3. If `cancelled_epoch` already tombstoned → **accept and commit as idempotent no-op** (deterministic; this is how concurrent/duplicate cancel proposals serialize harmlessly under total order — do not reject, rejection causes VC churn).

*Semantic (justification):*
4. Parses as CANCEL_CERT; ≥ `f_anchored+1` distinct signers; all keys in static config; all signatures verify; all signed strings agree and match the header fields.
5. EMERGENCY: embedded credential verifies against Emergency-CA **and** ambulance ID ∉ epoch e's committed order (follower checks against its own committed copy).
6. CRASH: `reasonRef`'s batch pair re-derives as genuinely co-batched by re-running the deterministic scheduler over the committed epoch-e bytes (fabricated pairings fail recomputation).

All checks consume only data every honest E-member already holds (committed order, CA keys, static config). No new replica state.

**On commit — the authoritative acts:**
1. **Authoritative tombstone** on epoch e. This is the fact "e's unexecuted suffix is dead," now totally ordered against `ORDER(e)` itself: every quorum that could have extended e intersects the cancel quorum in ≥ f+1 honest members, so no partition of E can disagree about e's status.
2. **Apply rule:** no vehicle may *begin* crossing under epoch e after applying CANCEL(e); in-box vehicles finish. Recallability governs physical stopping, as in Phase 1.
3. **Cancel-commit gossip:** each committed replica rebroadcasts a signed commit attestation (new gossip class, same semantics as type-9). Adoption threshold `f_anchored+1` matching attestations. All tombstone checks (`handleDecisionGossip`, `applyGossipOrder`, `processOrders`) consult committed/gossip-adopted cancels — this is what defeats the stale-epoch-e-resurrection straggler.
4. `beginRollbackDiscovery()` is triggered **here** (not at cert validation).

**Availability:** Phase 2 needs `q(|E|, f_anchored)` responsive E-members (departed-but-reachable count). Benign bound at |E|=16, f=5: available through 5 physical departures *plus* however long departed members remain in radio range. If unavailable → Phase 2 retries under rotation until the fallback timer (§7) fires.

---

## 6. Phase 3 — Re-coordination consensus (electorate = M)

**Discovery:** as currently implemented — fresh announce/echo/cert for e+1; membership filter excludes departed (TraCI-cleared) and non-recallable; includes certified newcomers. Newcomers that missed Phase 2 adopt CANCEL(e) from cancel-commit gossip (f+1) or validate the commit proof embedded in the Phase 3 justification — learn, then vote.

**Quorum and availability — REQUIRED CHANGE from current code:**
```
f_rollback  = f_anchored                          // never recomputed from |M|
quorum      = q(|M|, f_anchored)
available   iff |M| ≥ 3·f_anchored + 1            // gate tests f_anchored
```
The current `f_dynamic = min(f_anchored, (|M|−1)/3)` with gate `|M| ≥ 3·f_dynamic+1` is **vacuous** — the gate is satisfied for every |M| by construction, so rollback always proceeds with a silently shrunken f, which is unsound (departures are biased toward honest members; the adversary's key count does not shrink). With the corrected gate, `min()` never diverges from `f_anchored` inside the available region, so the fix is the gate, and `f_dynamic` collapses to `f_anchored`.

**Pre-verify — Check 14 (extends existing Check 11):**
1. Structural rollback framing as currently implemented.
2. Justification is a **committed-cancel reference** whose digest matches a locally committed or gossip-adopted CANCEL(e); a proposal whose justification is a raw CANCEL_CERT with no committed cancel is **rejected** (no skipping Phase 2).
3. `new_epoch = cancelled_epoch + 1`; `cancelled_epoch` tombstoned locally.
4. Membership corroboration (Check 12, as planned): every exclusion of a present E-member is backed by departure/non-recallability evidence; every SIGNED newcomer entry carries a fresh e+1 arrival cert. The ambulance's cancel-evidence credential does **not** substitute for its e+1 arrival cert — cancel authority and membership admission are separate thresholds.
5. Proposer (`leader_id`) ∈ proposal-defined M; forced view installs it as primary.

**Commit:** executor schedules exactly the committed M; `cancel_pending_` and `rollback_cancel_initiated_` clear; ownership of epoch e+1 — including its own future cancellation — now rests with M (the electorate refreshes each epoch instead of eroding monotonically).

**Known limitation (unchanged):** forced-M view change unimplemented; silent forced-M primary is covered by app-level proposer rotation + resubmission, and terminally by the fallback (§7). `[ROLLBACK-VC-UNSUPPORTED]` remains the loud-failure marker.

---

## 7. Degraded serialization fallback (the Phase 3 liveness edge)

**The edge case:** CANCEL(e) commits and the fleet halts safely — but ORDER(e+1) can never commit: `|M| < 3·f_anchored + 1` (too many departed/non-recallable), or discovery/consensus stalls past all proposer rotations (silent forced-M primary, partition). Cancellation succeeded; recovery is unavailable. Without a defined behavior the fleet is halted indefinitely with the emergency vehicle idling inside it — a liveness deadlock manufactured by the safety mechanism.

**The solution — stop-sign fallback.** Unilateral entry (like halting, it only ever reduces concurrency, so it is monotone-safe and needs no quorum):

*Entry (either condition):*
- Local determination of unavailability: locally observed candidate membership `< 3·f_anchored + 1` after discovery timeout (`[ROLLBACK-UNAVAILABLE]`), **or**
- Terminal timeout: `T_fallback = rollbackDiscoveryTimeoutSec × maxProposerRotations` elapses after CANCEL(e) commit with no ORDER(e+1) committed or adopted.

*Rules while in fallback:*
1. **Never co-batch.** Vehicles cross strictly one at a time.
2. **Local clearance gate + dwell:** a vehicle may enter only after observing the conflict box empty for a continuous dwell `τ_dwell` (dwell gives gossip time to deliver any late-committed ORDER(e+1) and damps simultaneous-entry races).
3. **Deterministic right-of-way:** among mutually visible waiting vehicles, lowest replica ID (or fixed lane rotation) proceeds first — a tie-break heuristic for liveness, not a safety dependency (safety rests on rules 1–2).
4. **Emergency priority is local and verifiable:** while a credential-valid emergency vehicle is present and uncleared, all others yield; it crosses first. Credential verification is unilateral (CA signature), so this needs no agreement.
5. **Preemptible:** a replica that learns a committed ORDER(e+1) (directly or via f+1 gossip) before entering the box adopts it and exits fallback. Vehicles that already crossed in fallback are physically departed; any committed order that still lists them is satisfied vacuously through existing departure detection and clearance gating.
6. **Exit / re-formation:** once the halted set drains (or membership recovers), arrivals form a fresh epoch through normal discovery. If ORDER(e+1) never committed anywhere, epoch number e+1 is reused with fresh discovery; if it committed somewhere, gossip reconciles first (rule 5), so no two conflicting e+1 histories can both be extended — extension requires a quorum, and the committed one owns it.

*Safety argument:* singleton crossing under an observed-empty gate cannot create a protocol-authorized co-occupancy; the only residual race is simultaneous entry on stale observation, which is the physical stop-sign race, bounded by `τ_dwell` and rule 3, and explicitly outside the consensus safety claim (same scope boundary as post-commit physical misbehavior).

*Liveness argument:* the intersection drains at degraded throughput; the emergency vehicle is served by rule 4 without any consensus. Fallback throughput is a reportable metric, not a failure.

---

## 8. Edge-case matrix

| # | Edge case | Resolution | Mechanism |
|---|---|---|---|
| 1 | Straggler resurrects cancelled epoch e | Impossible after adoption | Cancel-commit gossip (f+1) + quorum intersection of CANCEL(e) with all epoch-e quorums; tombstone checks consult committed cancels |
| 2 | Two hazards / two cancel proposals race | One wins total order; second commits as no-op | Check 13.3 idempotent no-op |
| 3 | Byzantine / silent cancel proposer | Replaced | Ordinary ResDB view change (Phase 2 runs under E's normal view) |
| 4 | Newcomer legitimacy | Learn CANCEL(e) via f+1 gossip or embedded commit proof; vote only in Phase 3 | Standing rule |
| 5 | Proposer skips cancellation, submits ORDER(e+1) directly | Rejected | Check 14.2 requires committed-cancel reference |
| 6 | Fabricated crash reason | Rejected | Check 13.6 deterministic scheduler recomputation |
| 7 | Replayed/duplicate justification | No-op | Tombstone check |
| 8 | f colluders below threshold attempt spurious cancel | No CANCEL_CERT forms | f+1 evidence threshold over K |
| 9 | Adversarial M (proposer excludes honest present vehicle) | Rejected | Check 14.4 membership corroboration |
| 10 | Too many departures before Phase 2 | Cancel unavailable → halt persists → fallback timer | §7 entry condition 2 |
| 11 | Cancel commits, Phase 3 unavailable forever | Degraded serialization; ambulance served locally | §7 (the Phase 3 edge) |
| 12 | ORDER(e+1) commits concurrently with some replicas in fallback | Fallback preempted for un-crossed vehicles; crossed vehicles reconciled as departures | §7 rules 5–6 |
| 13 | Hazard during e+1 execution (re-entrant rollback) | Identical protocol, one level up: CANCEL(e+1) voted by M, tombstone chain {e, e+1} | Standing rule recursion |
| 14 | Byzantine ambulance field fragments echo matching | Prevented | reasonRef derived from committed data + stable IDs only (§3) |

---

## 9. Invariants (paper-facing)

**S1 (Cancellation agreement).** No two correct replicas disagree on whether epoch e's unexecuted suffix is authorized: CANCEL(e) quorums intersect all epoch-e quorums in ≥ f_anchored+1 members.
**S2 (No unjustified cancel).** A CANCEL(e) commit implies ≥ 1 honest witness observed the cancel reason (f+1 evidence threshold).
**S3 (No unjustified membership).** A committed ORDER(e+1) admits only cert-backed or corroborated-QUIET members and excludes only evidence-backed departures/non-recallables.
**S4 (Anchored safety).** No decision at any phase commits under a quorum sized for f < f_anchored.
**S5 (Monotone-safe degradation).** Every unilateral action (halt, fallback entry) strictly reduces authorized concurrency; unsafe co-occupancy is never authorized by any path, including fallback.
**L1 (Cancel liveness).** Under partial synchrony, if a valid CANCEL_CERT exists and ≥ q(|E|, f_anchored) honest E-members are reachable, CANCEL(e) commits.
**L2 (Recovery liveness).** If additionally |M| ≥ 3·f_anchored+1 with an honest reachable proposer in rotation, ORDER(e+1) commits.
**L3 (Fallback liveness).** Otherwise the intersection drains and the emergency vehicle is served under §7, at degraded throughput.

---

## 10. Analyzer hooks (delta over R0 predicates)

- `[CANCEL-PROPOSE]` / `[CANCEL-COMMIT]` with instance seq, quorum used (`must == q(|E|, f_anchored)`), voter set ⊆ E.
- Ordering: CANCEL(e) commit strictly precedes any `[ROLLBACK-PROPOSE]`.
- Check-14 rejection fires in the skip-Phase-2 adversarial run.
- Duplicate cancel run: second proposal logs no-op commit, no view change triggered.
- Tombstone consulted from committed cancel (log source: `commit` vs `gossip-adopted`), not only local flag.
- `[FALLBACK-ENTER]` with entry reason (`unavailable` | `timeout`); zero co-occupancies during fallback; ambulance clearance time under fallback.
- `[ROLLBACK-QUORUM]` line must satisfy `f == f_anchored` and gate `|M| ≥ 3·f_anchored+1` (regression test for the vacuous-gate fix).

---

## 11. Implementation deltas vs current repo

1. **New:** `ResdbCancelHdr`, Phase-2 proposal path, Check 13, cancel-commit gossip class, committed-cancel reference in rollback justification, Check 14.2.
2. **Moved:** `beginRollbackDiscovery()` trigger from CANCEL_CERT validation → CANCEL(e) commit. Authoritative tombstone from rollback commit → cancel commit.
3. **Fixed:** Phase-3 availability gate tests `f_anchored` (vacuous-gate bug); `f_dynamic` collapses to `f_anchored`.
4. **New:** fallback mode — entry conditions, dwell-gated singleton crossing, local emergency yield, preemption by committed order.
5. **Unchanged:** Phase-1 triggers, halt path, recallability, discovery mechanics, forced-M vote filtering, `[ROLLBACK-VC-UNSUPPORTED]` loud failure (now backstopped by fallback).