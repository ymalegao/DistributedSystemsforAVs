# Stage C Prime: Crash -> WAIT(BLOCKED) -> CLEAR -> ORDER (Scenario 16)

**Status:** Implementation plan, repository-audited against the current Scenario 15 code.

This specification extends the working Scenario 15 architecture. It deliberately does not add a rollback ORDER type, `ResdbRollbackHdr` usage, or `ResdbConsensusTag`. Rollback is the committed CANCEL decision. Discovery after CANCEL and the resulting `ORDER(e+1)` use the same ordinary discovery and ORDER path as epoch 0.

## 1. Protocol Summary

```text
ORDER(e)
  -> crash perceived in the executing batch
  -> f+1 BLOCKED incident certificate
  -> CANCEL(e) commits
  -> WAIT(term, renewal, ttl) decisions while the incident remains blocked
  -> f+1 CLEAR certificate for the blocked incident
  -> ordinary ORDER(e+1), carrying CLEAR evidence
```

The decisions use the same ResDB PBFT machinery:

- `ORDER` grants crossing authority according to a committed schedule.
- `CANCEL` tombstones a previously committed ORDER epoch.
- `WAIT` grants the current recovery leader a bounded lease to keep recovery pending.

The payloads differ because the application decisions differ. This does not create separate consensus algorithms.

## 2. Non-Goals and Removed Assumptions

- Do not use `ResdbRollbackHdr` for `ORDER(e+1)`.
- Do not infer decision type or epoch from PBFT sequence arithmetic.
- Do not add `ResdbConsensusTag`; discovery state already controls ANN/ECHO/CERT transmission.
- Do not create a separate rollback discovery protocol. The existing view-based discovery state machine is reused.
- Do not use per-vehicle crash certificates as the authoritative completeness boundary.
- Do not make all 18 provisioned identities mandatory active voters.
- Do not let WAIT expiry authorize crossing. Only ORDER grants crossing authority.

## 3. Identity Universe, Active Views, and Quorums

The repository provisions 18 ResDB identities because 18 distinct vehicles may exist over the lifetime of Scenario 15/16. Provisioning is an identity/address/key registry, not a claim that all 18 vehicles are simultaneously active.

For Scenario 16:

- `K = {r0..r17}` is the provisioned identity universe.
- `M_e` is the 16-member active view committed by `ORDER(e)`.
- The two wrecked vehicles remain members of `M_e` but become communication-dead.
- BLOCKED/CLEAR certificate threshold is `f_e + 1 = 6`, where `f_e = 5` for `M_e`.
- CANCEL and WAIT use the committed `M_e` view: `N=16`, `f=5`, quorum `11`.
- Fourteen responsive vehicles remain, so CANCEL and WAIT retain a three-vote liveness margin over quorum.
- After both wrecks are removed, fresh discovery for `e+1` contains 14 eligible intents. Under the current per-epoch fault mode, ordinary `ORDER(e+1)` installs `N=14`, `f=4`, quorum `10`.

Every active view must come from a committed decision or a proposal validated under the existing proposal-defined active-view rules. Live TraCI perception never directly defines a PBFT voter set.

## 4. Trusted Witness Identity

The current CANCEL wire carries `echoingReplicaId`, signer public key, and signature, but validation trusts the public key embedded by the sender. Before reusing this mechanism for CLEAR, bind witness keys to provisioned replica identities.

### 4.1 Simulation key registry

Add an immutable simulation bootstrap registry:

```cpp
class WitnessKeyRegistry {
public:
    bool registerKey(int replicaId,
                     const uint8_t pubKey[CRYPTO_PUBKEY_BYTES]);
    bool matches(int replicaId,
                 const uint8_t pubKey[CRYPTO_PUBKEY_BYTES]) const;
};
```

Rules:

- Each module registers its existing `ec_pub_key_` for its own replica ID during initialization, before simulation traffic begins.
- The first registration for an ID is immutable. Conflicting registration fails loudly.
- Evidence validation checks the echo's public key against this registry before checking its signature.
- A signer ID must also belong to the statement's eligible committed view.
- The embedded key remains on the type-12/13 wire for compatibility, but it is no longer an authority by itself.
- A deployment replaces this simulation bootstrap with provisioned certificates or hardware identities; the validation interface stays the same.

The bridge must not implement a second, weaker certificate validator. Register a thread-safe, read-only evidence-validation callback, following the existing cert-snapshot callback pattern, so bridge pre-verify and the Veins application consume the same result.

## 5. Blocked Incident Model

The authoritative subject is the obstruction of the currently executing committed batch, not an individual wreck.

```cpp
struct BlockedIncident {
    uint32_t cancelledEpoch;
    uint32_t executingBatch;

    bool operator<(const BlockedIncident& other) const;
};

enum class IncidentState {
    BLOCKING,
    CLEARED,
};

struct IncidentRecord {
    IncidentState state = IncidentState::BLOCKING;
    std::vector<uint8_t> blockedCertBytes;
    std::vector<uint8_t> clearCertBytes;
};

std::map<BlockedIncident, IncidentRecord> incidentRegistry_;
```

Per-vehicle observations remain useful for injection, dwell tracking, and logs, but they do not define how many crash certificates must be discovered.

The canonical existing CANCEL `reasonRef` is:

```text
blocked_batch:<cancelled_epoch>:<executing_batch>
```

Provide one formatter/parser pair. Safety code consumes `BlockedIncident`, never ad hoc string parsing.

## 6. BLOCKED Evidence and CANCEL

### 6.1 Crash perception

On each world-state poll, an honest witness evaluates vehicles in the currently executing committed batch:

```text
crashedInExecutingBatch(v):
    v belongs to the currently executing committed batch
    AND v is still authorized/expected to move
    AND v is inside the conflict box
    AND speed(v) < crashSpeedEps
    continuously for crashDwellSec
```

The first qualifying vehicle causes that witness to emit one existing type-12 `CANCEL_ECHO` for the batch incident. Multiple wrecks in the same batch map to the same statement and therefore strengthen the same echo bucket instead of fragmenting evidence.

Local perception may defensively stop the observing vehicle immediately. It does not mark the incident cert-driven registry `BLOCKING`; only a validated certificate does that.

### 6.2 BLOCKED certificate

Reuse the existing type-12/13 CANCEL pipeline with `reason=CANCEL_CRASH` and the canonical batch reference.

A valid certificate requires:

1. At least six distinct witness IDs.
2. Every witness ID is in committed view `M_e`.
3. Every witness key matches the immutable key registry.
4. Every signature verifies.
5. Every signed statement matches epoch, reason, incident reference, and signer ID.
6. No duplicate signer IDs.

Validation registers `incidentRegistry_[incident] = BLOCKING` before selecting or suppressing a local CANCEL proposal. Registration is independent of the existing singleton "current cancel justification" state.

### 6.3 CANCEL behavior

The first valid BLOCKED certificate starts the existing path:

```text
CANCEL_WITNESSING
  -> CANCEL_DRAINING
  -> CANCEL_CONSENSUS
  -> CANCEL_COMMITTED
```

Reuse the current active-batch exclusion, deterministic CANCEL proposer, cancel drain, tombstone, vote retry, and cancel-commit gossip behavior.

CANCEL commits under `M_e`, including the two wrecked IDs as silent members. The remaining 14 replicas can satisfy quorum 11.

Only committed CANCEL starts ordinary discovery for epoch `e+1`.

## 7. WAIT Decision

WAIT is a committed application decision submitted through the same PBFT service as CANCEL and ORDER. It is not discovery state and it does not grant crossing authority.

### 7.1 Payload

Add an unambiguous WAIT payload magic:

```cpp
#define RESDB_WAIT_MAGIC 0x57414954u

#pragma pack(push, 1)
struct ResdbWaitHdr {
    uint32_t magic;              // RESDB_WAIT_MAGIC
    uint32_t pendingEpoch;       // e + 1
    uint32_t cancelledEpoch;     // e
    uint32_t leaderTerm;         // changes only after a lapsed lease
    uint32_t renewalIndex;       // increments for timely same-leader renewals
    int32_t  proposerId;
    uint64_t proposedAtSimUs;    // committed lease time base
    uint32_t ttlMs;
    uint32_t blockedCertLen;
    uint32_t nElectors;
};
#pragma pack(pop)

// payload:
// ResdbWaitHdr
// blockedCert[blockedCertLen]
// int32_t electors[nElectors]
```

The elector list must exactly equal the committed `M_e` membership, sorted and unique. It is included so the request-scoped active view remains explicit and independently checkable.

### 7.2 WAIT pre-verify

The bridge/application validation boundary checks, in order:

1. Magic, lengths, integer bounds, and `pendingEpoch == cancelledEpoch + 1`.
2. `ttlMs` is non-zero and no greater than `waitTtlMaxMs`; `proposedAtSimUs` is not in the future beyond the configured clock-skew allowance, and `proposedAtSimUs + ttlMs` has not already expired.
3. CANCEL for `cancelledEpoch` is committed/adopted and no ORDER for `pendingEpoch` is committed.
4. The embedded BLOCKED certificate is valid and names the registered incident for `cancelledEpoch`.
5. No valid matching CLEAR certificate is known.
6. Electors exactly match committed `M_e`; proposer belongs to that view.
7. `(leaderTerm, renewalIndex, proposerId)` follows the lease rules below.
8. Install a request-scoped active view `N=16`, `f=5`, quorum `11`.

A replica that already holds the matching CLEAR certificate rejects WAIT with `reason=incident-cleared`. Because CLEAR certificates are relayed and retried, all honest replicas should converge on this rejection before ORDER.

### 7.3 Leader lease and renewal

The initial WAIT leader is derived from the frozen recovery candidate list already used around CANCEL. The leader stays the same while it remains responsive.

```text
first WAIT:
    leaderTerm = 0
    renewalIndex = 0

timely renewal by the same leader:
    leaderTerm unchanged
    renewalIndex = previous renewalIndex + 1

lease expires without a committed successor:
    leaderTerm = previous leaderTerm + 1
    renewalIndex = 0
    proposer = candidates[leaderTerm % candidates.size()]
```

Only committed WAITs update the committed term/index. Rejected proposals do not advance rotation.

The active leader begins renewal when the remaining lease reaches `waitRenewalLeadSec`, provided the incident is still BLOCKING. A valid committed ORDER, CANCEL, or successor WAIT supersedes the current WAIT.

All replicas derive the same lease deadline from the committed `proposedAtSimUs + ttlMs`, rather than adding TTL to their locally delayed executor callback. OMNeT simulation time supplies the common clock for Scenario 16. A deployment requires a bounded synchronized clock or replaces deterministic timeout takeover with proper PBFT view change.

If the leader is alive and the accident remains, it should commit another WAIT before expiry. Rotation occurs only when no successor decision is committed by the expiry deadline.

The first implementation uses this deterministic lease rotation. A proper PBFT view-change/leader-election implementation may replace it later, but both mechanisms must not run concurrently for the same WAIT instance.

### 7.4 Lapse semantics

WAIT expiry is a real leader-lease lapse:

- The previous leader can no longer renew the expired term.
- The next deterministic leader becomes eligible to propose WAIT or ORDER.
- `[WAIT-LAPSED]` is a recovery event, not merely a metric.
- Vehicles remain stopped because CANCEL tombstoned `ORDER(e)` and no `ORDER(e+1)` exists.
- WAIT expiry never restores the cancelled schedule and never grants stop-sign crossing by itself.

This bounds an unresponsive WAIT leader without weakening the rule that only ORDER authorizes crossing.

### 7.5 WAIT executor result and gossip

Add a distinct executor result so the application callback cannot parse WAIT as ORDER:

```cpp
#define RESDB_WAIT_DECISION_MAGIC 0x57414944u

struct ResdbWaitDecisionHdr {
    uint32_t magic;
    uint32_t pendingEpoch;
    uint32_t cancelledEpoch;
    uint32_t leaderTerm;
    uint32_t renewalIndex;
    int32_t  proposerId;
    uint64_t leaseExpiresAtSimUs;
    uint32_t ttlMs;
    uint8_t  payloadDigest[32];
};
```

WAIT decisions need the same late-learner/gossip treatment as other committed application decisions. Adoption must be idempotent by cancelled epoch, term, renewal index, and payload digest.

## 8. CLEAR Evidence

Reserve the currently unused application message types:

```cpp
static constexpr int kClearEchoType = 15;
static constexpr int kClearCertType = 16;
```

### 8.1 Clearance predicate

An honest witness may emit CLEAR evidence only when it holds a valid matching BLOCKED certificate and observes:

```text
conflictBoxClear(incident):
    no vehicle occupies the conflict-box region
    continuously for clearDwellSec
```

This intentionally certifies the whole blocked incident, not merely the removal of one named wreck. If either wreck remains, honest witnesses cannot produce CLEAR echoes.

### 8.2 CLEAR statement and certificate

The signed statement is canonically derived from the same `BlockedIncident`:

```text
cancelledEpoch:clear:blocked_batch:<cancelledEpoch>:<batch>:signerId
```

Any replica may assemble CLEAR_CERT after six matching trusted echoes. Validation uses the same six rules as BLOCKED evidence. A valid CLEAR_CERT transitions only the matching incident:

```text
BLOCKING -> CLEARED
```

CLEAR_ECHO and CLEAR_CERT use per-statement bounded retries and relay-once behavior. A valid cert stops retries for that statement only.

## 9. Shared Witness-Certificate Machinery

Extract only the behavior shared by CANCEL and CLEAR; preserve the existing type-12/13 wire representation.

```cpp
enum class WitnessKind {
    CANCEL_CRASH,
    CANCEL_EMERGENCY,
    CLEAR,
};

struct WitnessStatement {
    uint32_t epoch;
    WitnessKind kind;
    std::string subjectRef;
};

class WitnessCertificateValidator;
class WitnessEchoCollector;
class WitnessRetryManager;
```

Requirements:

- Collector key is the full canonical statement.
- Distinct-signer handling is shared.
- Trusted-key and signature checks are shared.
- Retry state is keyed by statement; it is not a singleton certificate slot.
- OMNeT timer ownership remains in `ResDBIntersectionApp`; the manager owns data and retry decisions, not raw `cMessage` lifetimes.
- Type-12/13 serialization remains compatible except for validation becoming identity-bound.
- Types 15/16 have separate thin serializers over the shared internal model.

## 10. Ordinary ORDER(e+1) with CLEAR Evidence

`ORDER(e+1)` remains the existing ordinary payload and proposal-defined active-view path. Add an optional evidence trailer after the vehicle entries:

```cpp
#define RESDB_ORDER_EVIDENCE_MAGIC 0x4F455631u

#pragma pack(push, 1)
struct ResdbOrderEvidenceHdr {
    uint32_t magic;              // RESDB_ORDER_EVIDENCE_MAGIC
    uint16_t version;            // 1
    uint16_t nClearCerts;
};
#pragma pack(pop)

// existing ResdbProposeHdr
// existing ResdbVehicleEntry[nVehicles]
// optional ResdbOrderEvidenceHdr
// repeated { uint32_t certLen; uint8_t cert[certLen]; }
```

Rules:

- Epoch-0 ORDER and Scenario-15 emergency ORDER need no evidence trailer.
- A proposer recovering from a crash CANCEL includes the matching CLEAR_CERT.
- The trailer does not change PBFT consensus semantics or active-view derivation.
- The bridge validates the trailer before installing the active view.
- Validation adopts a valid embedded CLEAR_CERT, so a replica that missed the CLEAR flood can still vote.
- If a locally registered incident for `e` remains BLOCKING and the proposal lacks a valid matching CLEAR_CERT, reject.
- If the proposal schedules an ID known to be a communication-dead wreck, reject.
- The executor schedules only the existing vehicle entries and ignores the already-validated trailer.

No committed-CANCEL reference is required in ORDER. Epoch monotonicity, the committed tombstone, and the matching CLEAR incident evidence are the relevant checks.

## 11. Discovery and Membership After CLEAR

Committed CANCEL starts the existing shared discovery round for `e+1`. Discovery may collect while WAIT decisions are active.

- Wrecked communication-dead vehicles do not re-announce and are absent from the stabilized intent view.
- Towed/removed vehicles are not synthesized as QUIET members.
- The remaining 14 honest vehicles exchange fresh epoch-`e+1` arrival certificates.
- ORDER proposal submission is gated by both discovery COMPLETE and matching incident CLEARED.
- A committed WAIT does not restart or erase discovery.
- A CLEAR certificate reevaluates the ORDER gate but does not directly call `proposeAll()` from a packet handler; state transition code schedules the proposal attempt.

### 11.1 Certificate omission rule

For every locally held valid fresh arrival certificate:

- If the ORDER omits that replica, reject.
- If the ORDER encodes it as QUIET, reject.
- A vehicle for which the follower has no valid certificate is not counted as a proven omission.

Change the bridge gate from `omitted > f` to `omitted > 0` for these locally proven omissions. The protocol assumption is that honest discovery certificate gossip converges before honest replicas reach COMPLETE, so all honest replicas hold all honest certificates. Add an explicit convergence assertion to Scenario 16 rather than silently tolerating divergent snapshots.

## 12. Crash Injection and Tow Behavior

Scenario 16 starts from the Scenario 15 fleet shape without the ambulance:

- 16 vehicles commit `ORDER(0)`.
- Two vehicles in executing batch 0 are selected from the committed batch assignment.
- No normal departures or late arrivals occur before the crash.

### 12.1 Injection

Do not reuse `stopVehicle()` as the crash primitive. Add a dedicated helper that force-stops inside the box with TraCI speed control disabled from normal SUMO behavior.

At `[CRASH-INJECT]`:

1. Freeze the selected vehicle in the conflict box.
2. Set `crashCommsDisabled_`.
3. Clear queued application, discovery, gossip, and PBFT retry transmissions belonging to that module.
4. Reject all future TX enqueue/send paths and all RX dispatch for the wreck.
5. Mark ResDB PBFT silent if the bridge exposes that control.

Already handed-off MAC frames may still appear during one bounded radio grace interval. Acceptance checks zero wreck traffic after that interval rather than claiming instantaneous cancellation of frames already owned by the MAC.

### 12.2 Tow

At `crash_time + clearDelaySec`, remove both wrecks from TraCI. The current Veins vehicle wrapper has no `remove()` method, so add an explicit wrapper over the SUMO vehicle REMOVE command or implement removal in the scenario manager.

CLEAR is based on the observed empty conflict box after `clearDwellSec`, not directly on the tow timer.

Crash and clearance polling must run even after `order_applied_`; place it before the current early return in `handlePositionUpdate()` or use a dedicated world-state timer.

## 13. State Ownership

Keep the following boundaries:

- `ResDBArrivalProtocol.cc`: ANN/ECHO/CERT and shared discovery state.
- `ResDBRollbackProtocol.cc`: BLOCKED evidence, CANCEL, incident registry, CLEAR evidence, WAIT lease state.
- `ResDBDecision.cc`: ordinary ORDER construction, evidence trailer, executor callback dispatch.
- `ResDBTransport.cc`: PBFT byte transport and existing consensus retry manager.
- `ResDBIntersectionApp.cc::onWSM()`: application message-type dispatch, including types 15/16.
- ResDB bridge: payload parsing, pre-verify, request-scoped active view, executor decision kind.

Do not introduce another collection of unrelated booleans. Use explicit state objects:

```cpp
enum class WaitState { INACTIVE, PROPOSING, ACTIVE, LAPSED };

struct WaitLease {
    WaitState state = WaitState::INACTIVE;
    uint32_t cancelledEpoch = 0;
    uint32_t pendingEpoch = 0;
    uint32_t leaderTerm = 0;
    uint32_t renewalIndex = 0;
    int proposerId = -1;
    simtime_t expiresAt = SIMTIME_ZERO;
};
```

One transition function owns timer cancellation, replacement, and logs for each WAIT state change.

## 14. Existing Machinery to Reuse

- View-based `DiscoveryRound` and its COLLECTING/DRAINING_CERTS/COMPLETE states.
- Full-statement-keyed CANCEL echo buckets.
- CANCEL drain and active-batch leader exclusion.
- `perceivedActiveBatch()` and committed batch assignments.
- CANCEL tombstones and cancel-commit gossip.
- Request-scoped active views for ordinary epoch proposals.
- `ConsensusRetryManager`: retry PRE_PREPARE until local PREPARE progress, PREPARE until local COMMIT generation or verified COMMIT quorum, and COMMIT until decision/timeout.
- Stage A early-vote buffering/replay and loud vote-drop accounting.
- TraCI conflict-box and clearance polling helpers where their semantics match.

Stage A in this repository means PBFT vote buffering/replay, vote-drop accounting, and bounded phase retries. It does not include a consensus wire tag.

## 15. Implementation Inventory

### Veins application

- `ResDBIntersectionApp.h`
  - Add `BlockedIncident`, `IncidentRecord`, `WaitLease`, CLEAR structs, timers, and parameters.
  - Replace singleton CANCEL cert retry storage with statement-keyed retry state.
- `ResDBRollbackProtocol.cc`
  - Incident formatter/parser.
  - Cert-driven BLOCKING/CLEARED registry transitions.
  - WAIT proposal, renewal, lapse, and deterministic takeover.
  - Registration of every valid crash cert independently of CANCEL proposal selection.
- `ResDBDecision.cc`
  - Optional ORDER evidence trailer.
  - WAIT decision callback dispatch.
  - ORDER gate requiring discovery COMPLETE and incident CLEARED.
- `ResDBIntersectionApp.cc`
  - Parameters and timer ownership.
  - Crash/CLEAR world-state polling before the `order_applied_` return.
  - Type-15/16 receive dispatch.
- `ResDBTraCI.cc` and TraCI command interface
  - Dedicated crash freeze.
  - Vehicle removal support.
- `ResDBWitnessCert.h/.cc` (new)
  - Trusted validation, echo collection, keyed retry data.
- `ResdbV2VWire.h`
  - No consensus tag change.
- Veins generated/source Makefile
  - Add the new witness-cert object.

### ResDB bridge

- Define WAIT payload/result and ORDER evidence trailer structs in the shared bridge header.
- Parse WAIT before attempting to parse ordinary ORDER.
- Add evidence validation/adoption callback registration.
- Install WAIT active view from committed `M_e` electors.
- Validate crash ORDER evidence before installing the ordinary ORDER active view.
- Preserve ordinary ORDER active-view construction.

### Scenario and analysis

- Add Scenario 16 INI/orchestrator mapping.
- Add crash/tow parameters and select wrecks from committed batch 0.
- Extend analyzer for BLOCKED, CANCEL, WAIT leases, CLEAR, and ORDER evidence.
- Update `ARCHITECTURE.md` after implementation to remove stale claims that post-CANCEL ORDER uses `ResdbRollbackHdr`.

## 16. Parameters

Provisional defaults, subject to the latency gate below:

| Parameter | Default | Purpose |
|---|---:|---|
| `crashVehicle` | false | Scenario injection flag |
| `crashOnBoxEntrySec` | 0 s | Delay after selected batch enters box |
| `crashDwellSec` | 2.0 s | Stationary dwell before BLOCKED echo |
| `crashSpeedEps` | 0.1 m/s | Stationary threshold |
| `clearDelaySec` | 10.0 s | Scenario tow delay |
| `clearDwellSec` | 1.0 s | Empty-box debounce |
| `clearEchoRetryIntervalSec` | 0.5 s | CLEAR echo retry interval |
| `clearEchoRetryMax` | 20 | CLEAR echo retry bound |
| `waitTtlMs` | 4000 | WAIT leader lease |
| `waitTtlMaxMs` | 8000 | Pre-verify upper bound |
| `waitRenewalLeadSec` | 1.5 s | Timely renewal threshold |
| `waitClockSkewSec` | 0 s | Simulation-time proposal allowance; deployment-specific |

The constants are accepted only if:

```text
waitTtl > maximum observed WAIT propose-to-commit latency
          + radio retry margin
          + scheduling jitter margin
```

If the inequality fails in any calibration seed, increase TTL before counting acceptance runs.

## 17. Logging

Add concise transition logs:

- `[CRASH-INJECT] veh= batch= pos= t=`
- `[CRASH-PERCEIVED] witness= incident= stalled_vehicles= dwell=`
- `[INCIDENT-BLOCKING] epoch= batch= cert_signers=`
- `[WAIT-PROPOSE] epoch= term= renewal= leader= ttl_ms=`
- `[WAIT-COMMIT] epoch= term= renewal= leader= expires=`
- `[WAIT-RENEW] epoch= term= from= to=`
- `[WAIT-LAPSED] epoch= term= old_leader= next_leader=`
- `[CLEAR-PERCEIVED] witness= incident= dwell=`
- `[CLEAR-CERT] epoch= batch= signers=`
- `[INCIDENT-CLEARED] epoch= batch=`
- `[ORDER-EVIDENCE] epoch= clear_cert=valid|missing|invalid`
- `[ORDER-REJECT] reason=blocking-incident|wreck-scheduled|cert-omission`
- `[ACTIVE-VIEW] decision=WAIT|ORDER N= f= quorum= responsive= margin=`

## 18. Staged Implementation Plan

### Stage 0: Correct documentation and identity validation

1. Add immutable witness key registry.
2. Make existing CANCEL validation key-bound.
3. Add bridge evidence-validation callback.
4. Regression-test Scenario 15 with ordinary post-CANCEL ORDER and no rollback wrapper/tag.

Gate: Scenario 15 remains green; a forged signer ID with a valid signature under the wrong key is rejected.

### Stage 1: One batch-scoped BLOCKED incident

1. Add crash injection/freeze and communication kill.
2. Add execution-bound crash perception.
3. Emit existing CANCEL echoes using the batch incident reference.
4. Register the BLOCKED incident from a valid cert.
5. Commit CANCEL under the 16-member committed view.

Gate: exactly one incident cert forms even with two wrecks; both wrecks are silent; CANCEL commits with quorum 11.

### Stage 2: CLEAR evidence and ordinary recovery ORDER

1. Add types 15/16 and shared witness helpers.
2. Add tow/removal and empty-box dwell detection.
3. Add ORDER evidence trailer and crash-order pre-verify.
4. Gate ordinary ORDER on discovery COMPLETE plus incident CLEARED.

Gate: ORDER cannot commit after only one wreck is removed; after both are removed one CLEAR cert forms and ordinary 14-entry ORDER commits with valid evidence.

### Stage 3: WAIT lease

1. Add WAIT payload/parser/executor result.
2. Install WAIT over committed `M_e` with quorum 11.
3. Add same-leader renewal, lapse, and deterministic leader-term takeover.
4. Add WAIT gossip/adoption and analyzer continuity checks.

Gate: responsive leader renews while blockage persists; killing that leader causes one lease lapse and deterministic takeover; no vehicle crosses during the transition.

### Stage 4: Omission enforcement and adversarial runs

1. Reject every locally proven fresh-cert omission (`omitted > 0`).
2. Assert all honest replicas have all honest certificates before honest discovery COMPLETE.
3. Run forged evidence, lazy WAIT leader, bad ORDER, and staggered tow variants.

## 19. Acceptance Tests

Run at least 10 seeds after each stage that changes consensus behavior.

### Baseline

1. Scenario 15 remains green.
2. Post-CANCEL ORDER is logged as ordinary ORDER.
3. No `ResdbConsensusTag` or sequence-to-epoch inference is introduced.

### Crash and CANCEL

1. `ORDER(0)` commits with 16 entries and active view `N=16`, `f=5`, quorum 11.
2. Two batch-0 wrecks are injected and become silent after the bounded MAC grace interval.
3. Honest witnesses converge on one `BlockedIncident(e, batch0)`.
4. One BLOCKED certificate forms with at least six trusted distinct signers and no wreck signer.
5. CANCEL commits with 11-14 voters and tombstones epoch 0.

### WAIT

1. At least two WAIT decisions commit before clearance in the long-delay variant.
2. Timely renewals retain the same leader term and increment renewal index.
3. No `[WAIT-LAPSED]` occurs while the leader is responsive and latency remains inside the lease budget.
4. In the killed-leader variant, expiry emits one lapse, advances the term, and enables the next deterministic leader.
5. The old leader cannot renew an expired term.
6. Vehicles remain stopped during WAIT, renewal, lapse, and takeover.

### CLEAR and ORDER

1. Removing only one wreck does not produce CLEAR_CERT.
2. Removing both wrecks and observing an empty box for `clearDwellSec` produces one CLEAR_CERT with six trusted signers.
3. No `ORDER(1)` commits while the incident is BLOCKING.
4. `ORDER(1)` is ordinary ORDER with one valid CLEAR evidence trailer.
5. The 14 remaining honest vehicles have fresh epoch-1 certs at every honest replica before proposal.
6. `ORDER(1)` contains exactly 14 signed entries, excludes both wreck IDs, and commits under the current per-epoch active-view rule (`N=14`, `f=4`, quorum 10).
7. All 14 scheduled vehicles cross with zero conflict-box overlap violations.

### Adversarial variants

1. Wrong-key signer claiming an honest replica ID is rejected.
2. Underweight BLOCKED/CLEAR cert with five signers is rejected.
3. CLEAR cert for the wrong epoch or batch is rejected.
4. Lazy leader proposing WAIT after CLEAR convergence is rejected by honest replicas.
5. ORDER without CLEAR evidence is rejected while the incident is BLOCKING.
6. ORDER omitting or QUIET-encoding an honestly certified vehicle is rejected by every honest replica that holds that cert; the convergence assertion requires all honest replicas to hold it.
7. Staggered tow proves the intersection remains BLOCKING until the second wreck leaves.

## 20. Invariants

1. Only committed ORDER grants crossing authority.
2. Committed CANCEL permanently tombstones its cancelled epoch.
3. WAIT expiry changes recovery leadership, never crossing authority.
4. Local perception may halt and emit evidence but cannot create BLOCKING/CLEARED protocol state without a valid certificate.
5. One committed executing batch maps to at most one active blocked incident per cancelled epoch.
6. BLOCKING is monotonic until a matching valid CLEAR certificate transitions it to CLEARED.
7. A crash-recovery ORDER must carry valid matching CLEAR evidence.
8. Active PBFT membership comes from committed/proposal-validated membership, never a live perception snapshot.
9. A witness signature counts only when its key is bound to its claimed provisioned replica ID.
10. A fresh arrival certificate held by a follower cannot be silently converted to QUIET or omitted by the leader.
