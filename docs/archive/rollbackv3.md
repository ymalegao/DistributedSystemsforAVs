# PicaBFT Rollback Protocol v3 — Totally Ordered Cancellation + Per-Epoch Reconfiguration

**Status:** Design spec for implementation. Supersedes the anchored-f Phase-3 rollback rule from v2.

**Core change vs v2:** Cancellation remains a committed consensus decision voted by the cancelled epoch’s membership. Re-coordination is now explicit reconfiguration: after `CANCEL(e)` commits, the protocol discovers a new certified active view `V_{e+1}` and installs epoch `e+1` with its own membership size, fault budget, and quorum.

**Protocol shape:**

```text
ORDER(e) under V_e
→ CANCEL(e) under V_e
→ tombstone epoch e
→ discover V_{e+1}
→ ORDER(e+1) under V_{e+1}
```

The old epoch’s budget governs the stop/handoff. The new epoch’s budget governs new operation.

---

## 1. Design principles

1. **Halting is unilateral; resuming is consensual.**
   Any replica with valid cancel evidence may halt locally and immediately. No vehicle may begin or resume crossing without a committed decision or degraded fallback rule.

2. **Cancellation is the committed stop command.**
   `CANCEL(e)` is voted by epoch `e`’s membership `V_e`. It is totally ordered against `ORDER(e)` and acts as the wedge command that ends epoch `e`’s authority.

3. **Reconfiguration happens only after stop.**
   The replacement epoch does not vote itself into existence. It is only valid after a committed or gossip-adopted `CANCEL(e)`.

4. **Standing rule.**
   The set with authority over a decision votes on that decision:

   * `V_e` votes on `CANCEL(e)`.
   * `V_{e+1}` votes on `ORDER(e+1)`.
   * Newcomers may help provide evidence, but they do not vote on cancelling an epoch they were not part of.

5. **Per-epoch fault budget.**
   Each epoch has its own certified active view `V_e` and its own fault budget:

   ```text
   f_e = floor((|V_e| - 1) / 3)
   ```

   unless the experiment explicitly declares a smaller `toleratedFaults`.

6. **Bounded adversary density assumption.**
   The default model assumes that every certified epoch view `V_e` contains at most `f_e` Byzantine members. If the view shrinks, the tolerated Byzantine count may shrink too. This is intentional and must be stated in the paper.

7. **Anchored mode remains available as a conservative mode.**
   A separate `rollbackFaultMode=anchored` may preserve the old epoch’s `f` through rollback. This is stricter and may halt when membership shrinks. It is useful for fallback demonstrations, but it is not the default recovery path.

8. **Evidence justifies; consensus decides.**
   Physical witness evidence authorizes proposing a cancel or admitting a vehicle to a view. Evidence alone is not a committed decision.

9. **Membership construction is safety-critical.**
   Because per-epoch `f` depends on the proposed view size, a Byzantine proposer must not be able to shrink the view arbitrarily. Followers must reject rollback proposals that omit vehicles they locally know are fresh, present, certified, and not departed/non-recallable.

10. **Decision ledger.**
    The intersection maintains a linear sequence:

    ```text
    ORDER(0), CANCEL(0), ORDER(1), CANCEL(1), ORDER(2), ...
    ```

    Every transition carries verifiable evidence and is committed by the electorate with standing over that transition.

---

## 2. Sets, parameters, notation

| Symbol    | Definition                                                                                                                                             |     |                                                                                                                       |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ | --- | --------------------------------------------------------------------------------------------------------------------- |
| `K`       | All provisioned keyholders in the static config. Includes original vehicles, late-arriving keyed vehicles, and future infrastructure nodes.            |     |                                                                                                                       |
| `V_e`     | Certified active voting view for epoch `e`. These replicas vote on `ORDER(e)` and, later, `CANCEL(e)`.                                                 |     |                                                                                                                       |
| `E`       | Alias for the cancelled epoch’s view, usually `E = V_e`.                                                                                               |     |                                                                                                                       |
| `D ⊆ E`   | Members of `E` that have physically departed or become non-recallable. They are removed from the replacement view, not retroactively removed from `E`. |     |                                                                                                                       |
| `V_{e+1}` | Certified active voting view for replacement epoch `e+1`. Includes recallable survivors of `E` and certified newcomers.                                |     |                                                                                                                       |
| `S_{e+1}` | Scene set for epoch `e+1`: physical vehicles/obstacles the scheduler must account for. This may include quiet/uncertified actors.                      |     |                                                                                                                       |
| `f_e`     | Fault budget for epoch `e`. Default: `floor((                                                                                                          | V_e | -1)/3)`. If `toleratedFaults >= 0`, then `f_e = toleratedFaults`, but only if it is no larger than the view capacity. |
| `q(N,f)`  | Quorum function: `ceil((N + f + 1)/2)`. Ensures two quorums over the same `N` intersect in at least `f+1` members.                                     |     |                                                                                                                       |
| `mode`    | Rollback fault mode: `per_epoch` by default, `anchored` for conservative experiments.                                                                  |     |                                                                                                                       |

Canonical quorum function:

```cpp
int QuorumForView(int n, int f) {
    return (n + f + 2) / 2;  // ceil((n + f + 1) / 2)
}
```

View capacity rule:

```cpp
int CapacityF(int n) {
    return (n - 1) / 3;
}
```

Default per-epoch fault budget:

```cpp
int FaultBudgetForView(int n, int toleratedFaults) {
    int cap = CapacityF(n);

    if (toleratedFaults < 0) {
        return cap;
    }

    // toleratedFaults is allowed only as a smaller declared budget.
    if (toleratedFaults > cap) {
        reject_config_or_view();
    }

    return toleratedFaults;
}
```

For scenario 15:

```text
Epoch 0:
|V_0| = 16
f_0 = floor(15/3) = 5
q_0 = q(16,5) = 11

Rollback discovers:
|V_1| = 14
f_1 = floor(13/3) = 4
q_1 = q(14,4) = 10
```

So `ORDER(1)` should proceed under `V_1` with quorum 10.

---

## 3. Threshold ownership

Different thresholds belong to different epochs.

| Artifact                                               |       Threshold owner |                 Threshold | Counted over                        |                   |                                         |
| ------------------------------------------------------ | --------------------: | ------------------------: | ----------------------------------- | ----------------- | --------------------------------------- |
| `CANCEL_ECHO` / `CANCEL_CERT` for cancelling epoch `e` |             Old epoch |                 `f_e + 1` | valid keyholders in `K`             |                   |                                         |
| `CANCEL(e)` consensus                                  |             Old epoch |                       `q( | V_e                                 | , f_e)`           | votes from `V_e` only                   |
| Cancel-commit gossip adoption                          |             Old epoch |                 `f_e + 1` | matching signed commit attestations |                   |                                         |
| Arrival certs used to construct `V_{e+1}`              | Old handoff threshold |                 `f_e + 1` | valid keyholders in `K`             |                   |                                         |
| `ORDER(e+1)` consensus                                 |             New epoch |                       `q( | V_{e+1}                             | , f_{e+1})`       | votes from `V_{e+1}` only               |
| Future cancellation of epoch `e+1`                     |             New epoch | `f_{e+1}+1` evidence, `q( | V_{e+1}                             | , f_{e+1})` votes | evidence over `K`, votes from `V_{e+1}` |

Rule:

```text
The old view’s budget governs the handoff.
The new view’s budget governs operation after the new order commits.
```

This avoids mixing thresholds from two live views.

---

## 4. Wire formats

### 4.1 CANCEL_ECHO

Broadcast, signed by the echoing replica.

```text
signed string: cancelledEpoch:reason:reasonRef:echoingReplicaId

reason:
  0 = CRASH
  1 = EMERGENCY
  2 = DEVIATION   // reserved / roadmap

reasonRef:
  EMERGENCY → "amb:<carId>:<cancelledEpoch>"
  CRASH     → "unsafe_batch:<epoch>:<batch>:vehA+vehB"
```

`reasonRef` must be byte-deterministic across honest witnesses. It must be derived from stable IDs and committed data, not from local observation labels that could differ across witnesses.

---

### 4.2 CANCEL_CERT

A bundle of at least `f_e + 1` matching `CANCEL_ECHO`s.

Validity requirements:

```text
- at least f_e + 1 distinct signer IDs
- every signer is a valid keyholder in K
- every signature verifies
- every signed string agrees on cancelledEpoch, reason, and reasonRef
- EMERGENCY cert includes the Emergency-CA credential bytes
```

`CANCEL_CERT` justifies proposing `CANCEL(e)`. It does not itself tombstone the epoch.

---

### 4.3 CANCEL proposal

Phase-2 payload.

```cpp
struct ResdbCancelHdr {
    uint32_t cancelled_epoch;    // e
    uint8_t  reason;
    uint8_t  _pad[3];
    uint32_t justification_len;
};

uint8_t justification[justification_len]; // serialized CANCEL_CERT
```

---

### 4.4 ROLLBACK / RECONFIG proposal

Phase-3 payload.

```cpp
struct ResdbRollbackHdr {
    uint32_t cancelled_epoch;      // e
    uint32_t new_epoch;            // e + 1
    uint8_t  reason;
    uint8_t  mode;                 // per_epoch or anchored
    uint16_t _pad;
    uint32_t cancel_ref_len;
    uint32_t view_config_len;
};
```

The proposal carries:

```text
cancel_ref:
  committed-cancel reference:
    - cancel instance sequence number
    - cancel payload digest
    - commit proof or f_e+1 cancel-commit gossip attestations

view_config:
  - new epoch number
  - voting view V_{e+1}
  - scene set S_{e+1}
  - f_{e+1}
  - quorum q(|V_{e+1}|, f_{e+1})
  - leader/proposer ID
  - digest of all vehicle entries

vehicle entries:
  - voter entries: certified, vote-eligible members of V_{e+1}
  - quiet entries: scheduled-around but non-voting scene actors in S_{e+1}
```

A raw `CANCEL_CERT` is not enough for Phase 3. Phase 3 requires a committed-cancel reference.

---

## 5. Phase 1 — Evidence and halt

Phase 1 has no consensus.

### Triggers

Existing triggers remain:

```text
maybeTriggerEmergencyRollbackFromAnnouncement()
maybeTriggerEmergencyRollbackFromCert()
maybeTriggerCrashRollback()
```

A valid trigger causes a replica to broadcast `CANCEL_ECHO`.

### On validating a CANCEL_CERT

A replica that validates a `CANCEL_CERT` for epoch `e` does the following:

1. Halt recallable vehicles immediately using the existing braking-distance / recallability predicate.
2. Set `cancel_pending_(e)`.
3. Suppress normal `proposeAll()` and normal epoch-`e` application-level proposal triggers.
4. Mark local provisional tombstone intent.
5. Do **not** begin authoritative rollback discovery yet.
6. Optionally pre-warm discovery announces for `e+1`, but do not construct or submit a Phase-3 proposal before `CANCEL(e)` commits.

Non-recallable vehicles are not force-stopped. Vehicles already inside the conflict box finish crossing.

Important distinction:

```text
CANCEL_CERT = evidence.
CANCEL(e) commit = authoritative stop decision.
```

---

## 6. Phase 2 — Cancel consensus

Phase 2 commits the stop command.

### Electorate

```text
electorate = V_e
fault budget = f_e
quorum = q(|V_e|, f_e)
```

Only members of `V_e` may vote on `CANCEL(e)`.

Newcomers may have helped generate physical evidence, but they do not vote on cancellation of an epoch they were not part of.

Votes from outside `V_e` must be dropped:

```text
[ACTIVE-VOTE-DROP] epoch=e voter=<id> reason=not-in-cancel-electorate
```

### Proposer

The cancel proposer is deterministic:

```text
smallest halted replica ID in V_e
rotated by rollback_rotation_index_ on retry
```

The proposer must be in `V_e`.

Faulty or silent cancel proposers are handled by ordinary ResDB view change because Phase 2 uses a normal epoch view, not forced-M reconfiguration.

---

### Pre-verify — Check 13

Followers verify the cancel proposal before voting.

Structural checks:

1. Payload parses as `ResdbCancelHdr + justification`.
2. `reason` is valid.
3. `justification_len` is in bounds.
4. `cancelled_epoch` is locally committed.
5. If `cancelled_epoch` is already tombstoned, accept as deterministic idempotent no-op.

Semantic checks:

1. Justification parses as a `CANCEL_CERT`.
2. The cert contains at least `f_e + 1` distinct signers.
3. All signers are valid keyholders in `K`.
4. All signatures verify.
5. All signed strings agree and match the header fields.
6. For `EMERGENCY`, the Emergency-CA credential verifies and the ambulance ID is not already included in the committed order for epoch `e`.
7. For `CRASH`, the unsafe batch relation re-derives from the committed epoch-`e` order using the deterministic scheduler/checker.

All checks must use only data that honest `V_e` members already hold: committed order bytes, CA keys, static config, and locally stored evidence.

---

### On CANCEL(e) commit

When `CANCEL(e)` commits:

1. Install authoritative tombstone for epoch `e`.
2. Enforce the rule: no vehicle may begin crossing under epoch `e` after applying the tombstone.
3. Allow in-box vehicles to finish crossing.
4. Broadcast cancel-commit gossip attestation.
5. Allow replicas and newcomers to adopt the tombstone after receiving `f_e + 1` matching cancel-commit attestations.
6. Trigger `beginRollbackDiscovery(e+1)`.

This is the only point where Phase 3 becomes eligible.

---

## 7. Phase 3 — Reconfiguration and replacement order

Phase 3 constructs and commits the replacement epoch.

### Goal

Construct a new certified active voting view:

```text
V_{e+1}
```

and a scene set:

```text
S_{e+1}
```

Then commit:

```text
ORDER(e+1)
```

under `V_{e+1}`.

---

### Discovery

Discovery uses the existing announce/echo/cert mechanism, but the output must distinguish voting members from scene actors.

A discovered entry can be one of:

```text
VOTER_SIGNED:
  certified, recallable, responsive, vote-eligible member of V_{e+1}

SCENE_QUIET:
  physically visible or relevant to scheduling, but not vote-eligible
```

Voting view:

```text
V_{e+1} = all VOTER_SIGNED entries
```

Scene set:

```text
S_{e+1} = VOTER_SIGNED entries ∪ SCENE_QUIET entries
```

Quorum is computed only over `V_{e+1}`.

Scheduling must account for `S_{e+1}`.

Example:

```text
signed = 14
quiet = 2

|V_{e+1}| = 14
|S_{e+1}| = 16

f_{e+1} = floor((14 - 1) / 3) = 4
q_{e+1} = q(14,4) = 10
```

The two quiet entries are scheduled around, but they do not vote and do not increase quorum size.

---

### Newcomers

Newcomers are not retroactively part of epoch `e`.

They may enter `V_{e+1}` if:

```text
- they have a fresh e+1 arrival cert,
- they are recallable/present,
- they are included in the Phase-3 proposal,
- they have learned CANCEL(e) through gossip or embedded proof.
```

Newcomers can vote in `ORDER(e+1)` after validating the committed-cancel reference.

---

### Fault budget in per-epoch mode

Default mode:

```text
rollbackFaultMode = per_epoch
```

The replacement epoch computes:

```text
N_next = |V_{e+1}|
f_next = floor((N_next - 1) / 3)
q_next = q(N_next, f_next)
```

If the experiment declares a smaller `toleratedFaults`, then:

```text
f_next = toleratedFaults
```

but only if:

```text
toleratedFaults <= floor((N_next - 1) / 3)
```

Otherwise the view is invalid for that declared fault budget.

This is explicit reconfiguration. It is not silently preserving the old fault budget.

---

### Anchored mode

Conservative mode:

```text
rollbackFaultMode = anchored
```

Then Phase 3 uses:

```text
f_next = f_e
q_next = q(|V_{e+1}|, f_e)
available iff |V_{e+1}| >= 3*f_e + 1
```

If this condition fails, the replica logs:

```text
[ROLLBACK-UNAVAILABLE] mode=anchored oldF=<f_e> voteN=<|V_{e+1}|> need=<3*f_e+1>
```

and enters degraded fallback according to the fallback policy.

Anchored mode is useful for a stronger threat model and for R6-style fallback experiments. It is not the default recovery path.

---

## 8. Check 12 — Membership corroboration

Check 12 is safety-critical in per-epoch mode.

Because `f_{e+1}` depends on `|V_{e+1}|`, a Byzantine proposer could try to omit honest present vehicles and shrink the view. Followers must reject such proposals.

### Follower-side omission rule

A follower rejects a rollback proposal if:

```text
The follower holds a fresh e+1 arrival cert for vehicle x,
and the follower has no departure/non-recallability evidence for x,
and x is omitted from V_{e+1} and S_{e+1}.
```

Log:

```text
[ROLLBACK-OMISSION-REJECT]
  omitted=<vehicle_id>
  hasFreshCert=1
  hasDepartureEvidence=0
  hasNonRecallableEvidence=0
```

### Admission rule for voters

A vehicle may be admitted to `V_{e+1}` only if:

```text
- it has a fresh e+1 arrival cert,
- it is recallable/present,
- it is keyed under K,
- it is included as VOTER_SIGNED.
```

The ambulance’s emergency credential does not substitute for an arrival cert. Cancel authority and membership admission are separate.

### Admission rule for quiet scene actors

A vehicle or obstacle may be included in `S_{e+1}` as `SCENE_QUIET` if it must be scheduled around but lacks voting eligibility.

Quiet entries:

```text
- affect scheduling,
- are included in the scene digest,
- do not vote,
- do not count toward N,
- do not affect f,
- do not affect quorum.
```

---

## 9. Phase-3 pre-verify — Check 14

Followers verify the rollback/reconfiguration proposal before voting.

Required checks:

1. Payload parses as `ResdbRollbackHdr + cancel_ref + view_config + entries`.
2. `new_epoch = cancelled_epoch + 1`.
3. `cancelled_epoch` is locally tombstoned or gossip-adopted as cancelled.
4. `cancel_ref` is a committed-cancel reference, not a raw `CANCEL_CERT`.
5. `cancel_ref` digest matches a locally committed or gossip-adopted `CANCEL(e)`.
6. Proposed `V_{e+1}` contains only valid `VOTER_SIGNED` entries.
7. Proposed `S_{e+1}` contains all `VOTER_SIGNED` entries and any `SCENE_QUIET` entries.
8. `f_next` equals the correct fault-budget rule for the selected mode.
9. `q_next` equals `q(|V_{e+1}|, f_next)`.
10. In per-epoch mode, `f_next <= floor((|V_{e+1}| - 1)/3)`.
11. In anchored mode, `|V_{e+1}| >= 3*f_e + 1`.
12. Check 12 membership corroboration passes.
13. Proposer/leader ID is in `V_{e+1}`.
14. Votes from non-`V_{e+1}` members are dropped.

Vote filtering:

```text
[ACTIVE-VOTE-DROP] epoch=e+1 voter=<id> reason=not-in-reconfig-view
```

---

## 10. Phase-3 commit

On `ORDER(e+1)` commit:

1. Install epoch `e+1` order.
2. Set active view to `V_{e+1}`.
3. Set scene set to `S_{e+1}` for execution/scheduling.
4. Set epoch fault budget to `f_{e+1}`.
5. Clear `cancel_pending_`.
6. Clear `rollback_cancel_initiated_`.
7. Resume execution according to the committed order.
8. Future hazards in epoch `e+1` are handled recursively:

```text
ORDER(e+1) → CANCEL(e+1) → ORDER(e+2)
```

with `V_{e+1}` voting on `CANCEL(e+1)`.

---

## 11. Fallback

Fallback remains necessary because both modes can still fail to recover.

Fallback is especially important when:

```text
- anchored mode rejects a too-small replacement view,
- per-epoch discovery cannot stabilize,
- the forced-M proposer is silent,
- partitions prevent ORDER(e+1) from committing,
- membership is too small to provide useful Byzantine tolerance.
```

### Entry conditions

A replica enters fallback if either:

1. Rollback is explicitly unavailable:

```text
[ROLLBACK-UNAVAILABLE]
```

2. Terminal timeout fires after `CANCEL(e)` commit and no `ORDER(e+1)` is committed or adopted.

### Rules in fallback

1. Never co-batch.
2. Vehicles cross one at a time.
3. A vehicle may enter only after observing the conflict box empty for dwell time `τ_dwell`.
4. Deterministic local right-of-way breaks ties.
5. Credential-valid emergency vehicles receive local priority.
6. Fallback is preemptible by a committed or gossip-adopted `ORDER(e+1)`.
7. Vehicles that crossed during fallback are treated as departed for later reconciliation.

Fallback is not a replacement for consensus. It is a monotone-safe degraded mode.

---

## 12. Operating modes

### 12.1 Default: per-epoch mode

Use for scenario 15 and the main paper story.

Fault assumption:

```text
For every certified epoch view V_e,
at most floor((|V_e|-1)/3) members are Byzantine.
```

Behavior:

```text
V_0 has 16 members → f_0 = 5
V_1 has 14 members → f_1 = 4
ORDER(1) can commit with q(14,4) = 10
```

Paper language:

```text
PicaBFT uses explicit reconfiguration: the cancelled epoch commits the stop command, then the replacement epoch is installed with a certified active view and a fault budget derived from that view size.
```

### 12.2 Conservative: anchored mode

Use for fallback and stress experiments.

Fault assumption:

```text
The original fault budget remains valid through rollback.
The adversary may keep all f Byzantine vehicles in the surviving set.
```

Behavior:

```text
V_0 has 16 members → f_0 = 5
V_1 has 14 members → anchored f remains 5
14 < 3*5 + 1
rollback unavailable
fallback enters
```

Paper language:

```text
In conservative mode, PicaBFT preserves the original fault budget across reconfiguration. This improves adversarial robustness against persistent Byzantine cliques but reduces liveness under churn.
```

### 12.3 Adversary-density violation experiment

Use for T3 or a dedicated stress test.

Setup:

```text
V_1 has 14 members
per-epoch f_1 = 4
but 5 Byzantine keys remain in V_1
```

Expected framing:

```text
This violates the declared per-epoch bounded-density assumption.
The experiment should show what fails and whether signed evidence/votes make the violation attributable.
```

This is stronger than hiding the limitation. It makes the assumption explicit and testable.

---

## 13. Edge-case matrix

| #  | Edge case                                                  | Resolution                                                       | Mechanism                                    |
| -- | ---------------------------------------------------------- | ---------------------------------------------------------------- | -------------------------------------------- |
| 1  | Straggler resurrects cancelled epoch `e`                   | Rejected after tombstone adoption                                | `CANCEL(e)` commit + cancel-commit gossip    |
| 2  | Two cancel proposals race                                  | One wins total order; duplicate becomes no-op                    | Check 13 idempotent tombstone                |
| 3  | Byzantine cancel proposer                                  | Replaced                                                         | Normal ResDB view change under `V_e`         |
| 4  | Newcomer tries to vote on `CANCEL(e)`                      | Vote dropped                                                     | Standing rule, `V_e` electorate only         |
| 5  | Newcomer votes on `ORDER(e+1)` after cert                  | Allowed                                                          | Member of `V_{e+1}`                          |
| 6  | Proposer skips Phase 2 and submits rollback directly       | Rejected                                                         | Check 14 requires committed-cancel reference |
| 7  | Raw `CANCEL_CERT` used as rollback justification           | Rejected                                                         | Check 14                                     |
| 8  | Byzantine proposer omits known present certified car       | Rejected                                                         | Check 12 omission rule                       |
| 9  | Quiet actor lacks voting cert but must be scheduled around | Included in scene, not voting view                               | `S_{e+1}` vs `V_{e+1}` split                 |
| 10 | Discovery finds 14 certified voters after 16-car epoch     | Per-epoch proceeds with `f=4`; anchored falls back               | Mode-specific rule                           |
| 11 | Too many departures before cancel consensus                | Cancel may stall; fallback timer eventually applies              | Phase-2 availability + fallback              |
| 12 | ORDER(e+1) commits while some replicas are in fallback     | Uncrossed vehicles adopt; crossed vehicles reconcile as departed | Fallback preemption                          |
| 13 | Hazard during epoch `e+1`                                  | Same protocol recursively                                        | `CANCEL(e+1)` voted by `V_{e+1}`             |
| 14 | Persistent 5-Byzantine clique remains in 14-member view    | Violates per-epoch assumption                                    | Stress test / attribution run                |

---

## 14. Invariants

### Safety invariants

**S1 — Cancel agreement.**
No two correct replicas disagree on whether epoch `e` remains authorized once `CANCEL(e)` commits or is gossip-adopted.

**S2 — No unjustified cancel.**
A committed `CANCEL(e)` implies at least one honest witness signed matching cancel evidence, because the evidence threshold is `f_e + 1`.

**S3 — Stop-before-reconfigure.**
No `ORDER(e+1)` proposal is valid unless it carries a committed-cancel reference for `CANCEL(e)`.

**S4 — Per-epoch quorum safety.**
Every consensus decision in epoch `e` uses `q(|V_e|, f_e)`, where `f_e <= floor((|V_e|-1)/3)`.

**S5 — No arbitrary membership shrink.**
A committed `ORDER(e+1)` cannot omit a vehicle that an honest follower knows is freshly certified, present, and not departed/non-recallable.

**S6 — Voter/scene separation.**
Only certified voters count toward `N`, `f`, and quorum. Quiet scene actors affect scheduling only.

**S7 — Monotone-safe degradation.**
Fallback and halt actions only reduce authorized concurrency. They never authorize co-batched crossing.

### Liveness invariants

**L1 — Cancel liveness.**
If a valid `CANCEL_CERT` exists and enough `V_e` members are reachable, then `CANCEL(e)` commits under partial synchrony.

**L2 — Per-epoch recovery liveness.**
If discovery forms a valid `V_{e+1}` with an honest reachable proposer in rotation, then `ORDER(e+1)` commits using `f_{e+1}`.

**L3 — Anchored fallback liveness.**
If anchored recovery is unavailable because `|V_{e+1}| < 3*f_e + 1`, replicas enter degraded fallback instead of silently stalling.

**L4 — Emergency fallback liveness.**
If consensus recovery fails, the emergency vehicle is still served through local credential-verifiable priority in fallback mode.

---

## 15. Analyzer hooks

Required logs:

```text
[CANCEL-PROPOSE]
  epoch=<e>
  proposer=<id>
  voterN=<|V_e|>
  f=<f_e>
  quorum=<q(|V_e|, f_e)>

[CANCEL-COMMIT]
  epoch=<e>
  quorum=<q>
  voters=<ids>

[TOMBSTONE]
  epoch=<e>
  source=commit|gossip-adopted

[ROLLBACK-BEGIN]
  cancelled=<e>
  newEpoch=<e+1>

[ROLLBACK-DISCOVERY]
  signed=<count>
  quiet=<count>
  voteN=<|V_{e+1}|>
  sceneN=<|S_{e+1}|>
  missingCerts=<ids>
  echoCounts=<per-id>

[ROLLBACK-RECONFIG]
  mode=per_epoch
  oldEpoch=<e>
  oldN=<|V_e|>
  oldF=<f_e>
  newEpoch=<e+1>
  newVoteN=<|V_{e+1}|>
  newSceneN=<|S_{e+1}|>
  newF=<f_{e+1}>
  quorum=<q>

[ROLLBACK-UNAVAILABLE]
  mode=anchored|per_epoch
  reason=<membership-too-small|timeout|invalid-view|no-proposer>
  voteN=<count>
  need=<count>

[ROLLBACK-OMISSION-REJECT]
  omitted=<vehicle_id>
  hasFreshCert=<0|1>
  hasDepartureEvidence=<0|1>
  hasNonRecallableEvidence=<0|1>

[ROLLBACK-PROPOSE]
  rc=<0|error>
  proposer=<id>
  mode=<per_epoch|anchored>
  voteN=<count>
  sceneN=<count>
  f=<f>
  quorum=<q>

[ROLLBACK-COMMIT]
  epoch=<e+1>
  voterN=<count>
  sceneN=<count>
  f=<f>
  quorum=<q>

[FALLBACK-ENTER]
  reason=<unavailable|timeout>
  lastCommittedCancel=<e>
```

Analyzer predicates:

1. `CANCEL(e)` commit precedes any valid `ROLLBACK-PROPOSE(e+1)`.
2. Cancel quorum equals `q(|V_e|, f_e)`.
3. Cancel voters are a subset of `V_e`.
4. Rollback quorum equals `q(|V_{e+1}|, f_{e+1})`.
5. Rollback voters are a subset of `V_{e+1}`.
6. In per-epoch mode, `f_{e+1} == floor((|V_{e+1}|-1)/3)` unless a smaller `toleratedFaults` is declared.
7. In anchored mode, rollback proceeds only if `|V_{e+1}| >= 3*f_e + 1`.
8. Quiet entries do not count toward voterN, f, or quorum.
9. Any omitted fresh-certified vehicle without departure evidence triggers Check-12 rejection.
10. No silent `proposer=-1` skip is allowed. Every skip must produce either `[ROLLBACK-UNAVAILABLE]` or a concrete rejection log.
11. Fallback logs entry reason and produces zero co-batched crossings.
12. Scenario 15 per-epoch expected result: if discovery produces 14 certified voters, analyzer expects `newF=4`, `quorum=10`, and rollback proposal should not be skipped for needing 16.

---

## 16. Implementation deltas vs v2

1. **Replace anchored Phase-3 default with per-epoch reconfiguration.**
   Phase 3 computes `f_{e+1}` from `|V_{e+1}|` by default.

2. **Keep anchored mode as explicit option.**
   Add:

   ```text
   rollbackFaultMode = per_epoch | anchored
   ```

3. **Add canonical quorum function.**
   Replace all `2*f + 1` quorum computations with:

   ```cpp
   QuorumForView(n, f)
   ```

4. **Separate voting view from scene set.**
   Add explicit accounting for:

   ```text
   voteN = |V_{e+1}|
   sceneN = |S_{e+1}|
   signed = VOTER_SIGNED count
   quiet = SCENE_QUIET count
   ```

5. **Make Check 12 mandatory.**
   Followers reject proposals omitting fresh-certified, present, non-departed vehicles.

6. **Make rollback timeout loud.**
   Replace silent `proposer=-1` skip with:

   ```text
   [ROLLBACK-UNAVAILABLE]
   ```

   or a concrete rejection reason.

7. **Keep Phase 2 structure.**
   `CANCEL(e)` remains a normal PBFT decision under `V_e`.

8. **Keep committed-cancel reference requirement.**
   Phase 3 still rejects raw `CANCEL_CERT` justifications.

9. **Add per-candidate discovery diagnostics.**
   At timeout, log which vehicles lack certs and their echo counts.

10. **Update analyzer expected scenario-15 behavior.**
    In per-epoch mode, `|V_1|=14` is valid with `f_1=4`, `q=10`. In anchored mode, the same view triggers fallback because it cannot preserve `f_0=5`.

---

## 17. Paper-facing framing

Recommended text:

```text
PicaBFT treats rollback as explicit reconfiguration. A hazard certificate does not directly install a new schedule. Instead, the current epoch first commits CANCEL(e), a totally ordered stop command voted by the same active view that authorized ORDER(e). Only after this stop decision is committed do vehicles discover a replacement certified active view V_{e+1}. The replacement view then commits ORDER(e+1) under its own quorum and fault budget. Thus, cancellation safety is inherited from the old epoch, while recovery liveness follows the standard dynamic-BFT model in which each configuration carries its own Byzantine threshold.
```

Assumption sentence:

```text
The default per-epoch mode assumes bounded adversary density: each certified active view V_e contains at most floor((|V_e|-1)/3) Byzantine vehicles. This differs from a stronger anchored-adversary model in which a fixed Byzantine clique may persist while honest vehicles depart. PicaBFT supports the anchored model as a conservative mode, but uses per-epoch reconfiguration as the default operating point for dynamic traffic.
```

Contribution sentence:

```text
The novelty is not reconfiguration alone, but physically justified reconfiguration: the stop command is triggered by signed witness evidence about the road state, and the replacement view is constrained by arrival certificates and follower-side omission checks.
```
