# Stage C Prime: Crash -> WAIT -> CLEAR -> ORDER (Scenario 16)

**Status:** Revised implementation plan, repository-audited against the current Scenario 15 code.

**Core decision:** WAIT is a signed advisory heartbeat from the ordinary next-epoch certificate primary after discovery completes. It is not PBFT consensus, not an `f+1` certificate, not a voter-set change, and not an ORDER validation or proposer-authorization rule. BLOCKED and CLEAR remain `f+1` physical-evidence certificates. CANCEL and ORDER remain the only committed decisions.

## 1. Protocol Summary

```text
ORDER(e)
  -> crash perceived in the executing batch
  -> f+1 BLOCKED incident certificate
  -> CANCEL(e) commits
  -> ordinary discovery for e+1 begins
  -> recovery leader sends advisory WAIT heartbeats while BLOCKING
  -> f+1 CLEAR certificate for the blocked incident
  -> ordinary ORDER(e+1), carrying CLEAR evidence
```

There are three deliberately separate mechanisms:

- **Committed decisions:** ORDER grants crossing authority; CANCEL tombstones a committed ORDER epoch.
- **Certified physical evidence:** BLOCKED proves the conflict box is obstructed; CLEAR proves the obstruction is gone.
- **Advisory liveness traffic:** WAIT says the ordinary next-epoch certificate primary is alive and recovery is still pending. It only delays local leader-suspicion timers.

Only ORDER grants crossing authority. WAIT cannot make an ORDER valid or invalid.

## 2. Scenario 16

1. Sixteen vehicles commit `ORDER(0)`.
2. Two vehicles in the executing batch are force-stopped in the conflict box and become communication-dead.
3. The remaining vehicles observe the blocked batch. Six trusted witnesses form one batch-scoped BLOCKED certificate.
4. CANCEL(0) commits under the 16-member committed view.
5. Fresh epoch-1 discovery starts among the 14 responsive vehicles.
6. After epoch-1 discovery completes, its ordinary certificate primary periodically broadcasts WAIT heartbeats while the conflict box remains blocked, so followers do not time it out merely because ORDER cannot yet be proposed.
7. The scenario removes both wrecks. Six trusted witnesses form one CLEAR certificate after the conflict box remains empty for the clearance dwell.
8. CLEAR immediately terminates local WAIT deferral.
9. The ordinary epoch-1 primary proposes the same ORDER payload used by ordinary discovery, plus a CLEAR evidence trailer.
10. `ORDER(1)` commits for the 14 remaining vehicles.

## 3. Non-Goals

- Do not use `ResdbRollbackHdr` for `ORDER(e+1)`.
- Do not add a rollback ORDER consensus mode.
- Do not add WAIT to the ResDB bridge, executor, ledger, or active-view registry.
- Do not create WAIT_ECHO or WAIT_CERT messages.
- Do not use WAIT to elect or authorize an ORDER proposer.
- Do not reject an otherwise valid ORDER because of local WAIT state.
- Do not add `ResdbConsensusTag` or infer epoch from PBFT sequence arithmetic.
- Do not create separate rollback discovery. Reuse the existing discovery state machine.
- Do not use per-vehicle crash certificates as the authoritative completeness boundary.
- Do not apply exponential backoff to PBFT PRE_PREPARE/PREPARE/COMMIT retries in this stage.

## 4. Identity Universe, Active Views, and Quorums

The 18 entries in `server.config` are the provisioned identity universe because 18 distinct vehicles may exist over the lifetime of the scenario. Provisioning does not make all 18 simultaneous voters.

For Scenario 16:

- `K = {r0..r17}` is the provisioned identity universe.
- `M_e` is the 16-member active view committed by `ORDER(e)`.
- The two wrecked vehicles remain members of `M_e` but are communication-dead.
- BLOCKED and CLEAR require `f_e + 1 = 6` distinct trusted witnesses from `M_e`, where `f_e = 5`.
- CANCEL uses committed `M_e`: `N=16`, `f=5`, quorum `11`.
- Fourteen responsive replicas remain, giving CANCEL a three-vote liveness margin.
- After tow, epoch-1 discovery contains 14 signed intents. Under the current per-epoch mode, ordinary `ORDER(1)` installs `N=14`, `f=4`, quorum `10`.

WAIT has no quorum and creates no active view.

## 5. Trusted Witness Identity

The current CANCEL wire verifies a signature against the public key embedded by the same sender. Before CLEAR reuses that machinery, bind witness keys to replica IDs.

### 5.1 Simulation key registry

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

- Every module registers its existing `ec_pub_key_` for its own ID during initialization, before traffic begins.
- The first registration for an ID is immutable; conflicting registration fails loudly.
- BLOCKED, CLEAR, and WAIT signatures count only if the supplied key matches the registry entry for the claimed sender ID.
- Evidence signers must belong to the committed view associated with the incident.
- The current type-12/13 embedded key can remain wire-compatible, but it is no longer authoritative by itself.
- A deployment replaces this simulation registry with provisioned certificates or hardware identities.

The bridge uses one thread-safe, read-only evidence-validation callback for the ORDER CLEAR trailer. It must not implement a second weaker validator.

## 6. Blocked Incident

The authoritative subject is the obstruction of the executing committed batch, not each individual wreck.

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

The canonical existing CANCEL `reasonRef` is:

```text
blocked_batch:<cancelled_epoch>:<executing_batch>
```

Use one formatter/parser pair. Protocol checks consume `BlockedIncident`, never scattered string parsing.

Per-vehicle crash observations remain useful for injection, dwell tracking, and logs. Multiple wrecks in one executing batch strengthen the same incident statement rather than creating an unknowable set of independent crash subjects.

## 7. BLOCKED Evidence and CANCEL

### 7.1 Crash perception

An honest witness evaluates vehicles in the currently executing committed batch:

```text
crashedInExecutingBatch(v):
    v belongs to the currently executing committed batch
    AND v is still authorized/expected to move
    AND v is inside the conflict box
    AND speed(v) < crashSpeedEps
    continuously for crashDwellSec
```

The first qualifying vehicle causes that witness to emit one existing type-12 CANCEL_ECHO for the batch incident.

Local perception may defensively stop the observing vehicle immediately. It does not mutate the cert-driven incident registry; only a valid BLOCKED certificate records `BLOCKING`.

### 7.2 BLOCKED certificate

Reuse the existing type-12/13 CANCEL pipeline with `reason=CANCEL_CRASH` and the batch incident reference.

A valid certificate requires:

1. At least six distinct witness IDs.
2. Every signer belongs to committed `M_e`.
3. Every signer key matches the trusted key registry.
4. Every signature verifies.
5. Every signed statement matches epoch, reason, incident, and signer ID.
6. No duplicate signer IDs.

Every valid crash certificate registers the incident as BLOCKING before the application decides whether that certificate becomes the current singleton CANCEL justification. Registration must continue even when CANCEL for the epoch is already pending or committed.

### 7.3 CANCEL

The first valid BLOCKED certificate starts the existing path:

```text
CANCEL_WITNESSING
  -> CANCEL_DRAINING
  -> CANCEL_CONSENSUS
  -> CANCEL_COMMITTED
```

Reuse:

- Active-batch exclusion from CANCEL leadership.
- Frozen deterministic CANCEL proposer/electorate.
- CANCEL drain.
- Existing PBFT vote retry manager.
- Epoch tombstone.
- CANCEL-commit gossip/adoption.

CANCEL commits under the 16-member committed view. The two wrecked members are silent, leaving 14 possible voters for quorum 11.

Only committed CANCEL starts ordinary discovery for epoch `e+1`.

## 8. Advisory WAIT Heartbeat

WAIT exists only to avoid a false leader timeout after discovery has completed while the conflict box is legitimately blocked. It is one signed application message from the ordinary next-epoch certificate primary. Followers do not echo it and do not assemble a WAIT certificate.

### 8.1 Message type and payload

Reserve one unused application message type:

```cpp
static constexpr int kWaitHeartbeatType = 17;

#define RESDB_WAIT_HEARTBEAT_MAGIC 0x57414954u

#pragma pack(push, 1)
struct WaitHeartbeatPayload {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad;
    uint32_t cancelledEpoch;
    uint32_t executingBatch;
    int32_t  leaderId;
    uint32_t heartbeatIndex;
    uint64_t sentAtSimUs;
    uint64_t validUntilSimUs;
};
#pragma pack(pop)
```

The payload is signed by the leader using the existing signed application envelope. The claimed leader ID must match the trusted key registry.

WAIT does not carry BLOCKED_CERT bytes on every heartbeat. The follower already needs the matching locally validated BLOCKED certificate before accepting the heartbeat. A follower that lacks BLOCKED evidence simply does not defer its timeout.

### 8.2 Leader derivation and send rule

WAIT leadership is derived from the ordinary epoch-`e+1` discovery result. Once that discovery reaches `COMPLETE`, every replica computes the expected sender using the existing `CertPrimary()` rule over the finalized eligible certificate set. In the honest converged case this is the smallest eligible certified replica ID, and it is the same replica that may submit the ordinary ORDER proposal after CLEAR.

No WAIT heartbeat is sent and no follower leader-timeout is armed before local discovery reaches `COMPLETE`. WAIT does not create a second leader election, and `cancel_primary_` is not reused as the WAIT or ORDER leader.

After discovery completes, the ordinary certificate primary sends WAIT every `waitHeartbeatIntervalSec` while:

- The local discovery round is `COMPLETE` for `cancelledEpoch + 1`.
- The matching incident is locally BLOCKING.
- No matching CLEAR certificate is known.
- No `ORDER(e+1)` is committed.
- The leader remains the locally expected recovery leader.

WAIT uses a fixed heartbeat interval in Scenario 16. One leader frame per interval is already inexpensive and provides predictable failure detection. Exponential retry backoff applies to evidence/gossip dissemination, not the heartbeat cadence.

### 8.3 Follower acceptance

A follower accepts a WAIT heartbeat only when:

1. The signature is valid and bound to `leaderId` by the key registry.
2. CANCEL for `cancelledEpoch` is committed/adopted.
3. It holds a valid matching BLOCKED certificate and the incident is locally BLOCKING.
4. It holds no matching CLEAR certificate.
5. No `ORDER(cancelledEpoch+1)` is committed or being applied.
6. The sender equals the follower's locally computed ordinary `CertPrimary()` for the completed epoch-`e+1` discovery view.
7. `heartbeatIndex` is strictly greater than the last accepted index from that leader/incident.
8. `sentAtSimUs` is within the configured clock-skew allowance.
9. `validUntilSimUs > sentAtSimUs` and the advertised lease is no greater than `waitHeartbeatMaxDeferralSec`.
10. `validUntilSimUs` has not already expired.

Acceptance only reschedules the local recovery-leader suspicion timer to `validUntilSimUs`.

It does not:

- Change `rollback_rotation_index_`.
- Install a PBFT primary or active view.
- Authorize the sender to propose ORDER.
- Enter bridge pre-verify.
- Make an ORDER valid or invalid.
- Restart, close, or erase discovery.

### 8.4 Precedence

The precedence order is strict:

```text
valid ORDER(e+1) with matching CLEAR evidence
    > valid CLEAR certificate
    > advisory WAIT heartbeat
```

On valid CLEAR_CERT:

1. Transition the incident from BLOCKING to CLEARED.
2. Cancel the local WAIT deferral timer immediately.
3. Ignore all future WAIT heartbeats for that incident.
4. Reevaluate ordinary ORDER proposal readiness.

On an ORDER proposal carrying valid matching CLEAR evidence:

1. Validate and adopt CLEAR evidence first.
2. Clear local WAIT state before any ordinary proposal/primary checks.
3. Continue through the existing ordinary ORDER validation path.

A local WAIT heartbeat must never cause rejection of an otherwise valid ORDER.

### 8.5 Expiry and leader election

When WAIT expires without CLEAR or a successor heartbeat:

- Log `[WAIT-EXPIRED]`.
- Re-enable the ordinary follower leader-suspicion/view-change path.
- Keep every vehicle stopped because CANCEL remains committed and no new ORDER exists.

Scenario 16 assumes this ordinary certificate primary remains responsive. WAIT does not itself solve Byzantine leader election, and this stage does not claim that the current post-CANCEL application path can safely rotate to a replacement ORDER primary. Byzantine recovery-primary takeover remains deferred to a proper view-change or timeout-certificate design.

## 9. CLEAR Evidence

Reserve the unused message types:

```cpp
static constexpr int kClearEchoType = 15;
static constexpr int kClearCertType = 16;
```

### 9.1 Clearance predicate

An honest witness may emit CLEAR evidence only when it holds the matching valid BLOCKED certificate and observes:

```text
conflictBoxClear(incident):
    no vehicle occupies the conflict-box region
    continuously for clearDwellSec
```

This certifies the whole blocked incident. Removing only one of two wrecks cannot satisfy it.

### 9.2 CLEAR statement and certificate

The canonical signed statement is:

```text
cancelledEpoch:clear:blocked_batch:<cancelledEpoch>:<batch>:signerId
```

A valid CLEAR certificate requires six matching trusted echoes under the same identity and distinct-signer rules as BLOCKED.

```text
BLOCKING -> CLEARED
```

CLEARED is terminal for that incident. A new crash belongs to a later committed epoch/incident.

Any valid CLEAR certificate immediately stops WAIT acceptance and triggers ORDER-readiness evaluation.

## 10. Shared Witness Certificate Machinery

Extract only the behavior shared by CANCEL/BLOCKED and CLEAR. Preserve the existing type-12/13 wire representation.

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

- The collector key is the full canonical statement.
- Signers are distinct and identity-bound.
- Retry state is keyed by statement, not one singleton certificate slot.
- OMNeT timer ownership remains in `ResDBIntersectionApp`; the retry manager owns retry data/policy, not raw `cMessage` lifetime.
- Type-12/13 serializers remain wire-compatible.
- Types 15/16 use separate thin serializers over the same internal validation model.
- WAIT is not a client of this certificate machinery.

## 11. Exponential Backoff and Congestion

The repository already uses exponential spacing for ordinary decision gossip. CANCEL_CERT retry, CANCEL-commit gossip, and arrival certificate retries are currently fixed interval. Scenario 16 adds backoff only where it does not weaken PBFT phase progress.

### 11.1 Backoff function

```text
interval(k) = min(base * factor^k, cap) + full_jitter(0, jitterMax)
```

Use saturating arithmetic rather than unchecked bit shifting.

### 11.2 BLOCKED/CANCEL certificate retry

Cancellation evidence is time-sensitive, so use fast-start exponential backoff:

```text
initial broadcast immediately
retry 1: 0.10 s + jitter
retry 2: 0.20 s + jitter
retry 3: 0.40 s + jitter
retry 4: 0.80 s + jitter
later: capped at cancelEvidenceRetryCapSec
```

Rules:

- Reset the backoff for a statement when a new distinct echo is accepted.
- Stop certificate retry after CANCEL commits/adopts, the retry bound is reached, or the statement becomes obsolete.
- Relay-once by validators remains immediate and is not delayed by the assembler's retry backoff.
- Do not back off the first CANCEL_ECHO or first assembled BLOCKED certificate transmission.

### 11.3 CANCEL-commit gossip

CANCEL-commit gossip may use a slower capped exponential schedule:

```text
initial gossip immediately
then 0.25 s, 0.50 s, 1.0 s, 2.0 s, cap 4.0 s, each with jitter
```

Stop when the configured retry bound is reached or the run advances past the relevant recovery state. Gossip adoption still requires the existing matching-attestation threshold.

### 11.4 CLEAR evidence retry

CLEAR_ECHO/CLEAR_CERT use the generic capped backoff. The first transmission and relay-once remain immediate. New unique echo progress resets the assembler backoff so a nearly complete certificate is not needlessly delayed.

### 11.5 What remains fixed interval

Do not change these in Scenario 16:

- `ConsensusRetryManager` PRE_PREPARE/PREPARE/COMMIT retry cadence.
- Discovery's initial ANN/ECHO/CERT transmissions.
- Advisory WAIT heartbeat cadence.

PBFT retries are phase-liveness traffic. Backing them off could turn ordinary packet loss into long consensus stalls. Change them only after separate measurements and a dedicated acceptance test.

## 12. Ordinary ORDER(e+1) with CLEAR Evidence

The crash-recovery order remains ordinary ORDER consensus. It uses the same `ResdbProposeHdr + ResdbVehicleEntry[]` payload and proposal-defined active-view path as other epochs, with one optional evidence trailer.

```cpp
#define RESDB_ORDER_EVIDENCE_MAGIC 0x4F455631u

#pragma pack(push, 1)
struct ResdbOrderEvidenceHdr {
    uint32_t magic;
    uint16_t version;
    uint16_t nClearCerts;
};
#pragma pack(pop)

// existing ResdbProposeHdr
// existing ResdbVehicleEntry[nVehicles]
// optional ResdbOrderEvidenceHdr
// repeated { uint32_t certLen; uint8_t cert[certLen]; }
```

### 12.1 Why ORDER carries CLEAR

Include the matching CLEAR certificate even though it was already gossiped:

- Validation is self-contained for a follower that missed CLEAR gossip.
- Every voter validates the same physical-clearance proof.
- The follower can atomically adopt CLEAR and discard stale WAIT state before continuing ORDER checks.
- Packet reordering cannot make a valid crash-recovery ORDER depend on receiving CLEAR first.
- One batch-scoped certificate is modest compared with the ORDER and PBFT traffic it protects.

This evidence trailer does not make ORDER a special rollback consensus mode.

### 12.2 ORDER validation

For a crash-recovery ORDER:

1. Parse the ordinary proposal and optional evidence trailer.
2. Validate the matching CLEAR certificate through the shared evidence callback.
3. Adopt CLEAR and clear local WAIT state.
4. Reject if a known incident for the prior epoch remains BLOCKING.
5. Reject if a communication-dead wreck ID is scheduled.
6. Continue through ordinary epoch, entry, certificate, membership, active-view, and primary checks.

Epoch-0 ORDER and Scenario-15 emergency recovery ORDER do not require CLEAR evidence.

No committed-CANCEL reference is embedded in ORDER. The committed tombstone, epoch monotonicity, and CLEAR incident proof are the relevant state.

## 13. Discovery and Membership

Committed CANCEL starts the existing COLLECTING -> DRAINING_CERTS -> COMPLETE discovery round for `e+1`. Discovery runs while WAIT heartbeats are active.

- Wrecks do not re-announce and are absent from the stabilized intent view.
- Removed vehicles are not synthesized as QUIET members.
- The 14 responsive vehicles exchange fresh epoch-1 certificates.
- WAIT does not start, stop, reopen, or close discovery.
- WAIT begins only after discovery COMPLETE; before that point no follower leader-timeout is active.
- ORDER readiness requires discovery COMPLETE and incident CLEARED.
- The ordinary cert primary/proposal path selects the ORDER proposer. WAIT does not participate.
- CLEAR packet handlers update evidence state and schedule reevaluation; they do not call `proposeAll()` directly.

### 13.1 Certificate omission rule

For every locally held valid fresh arrival certificate:

- If ORDER omits the replica, reject.
- If ORDER encodes it as QUIET, reject.
- A vehicle for which the follower has no valid certificate is not a proven omission.

Change the bridge threshold from `omitted > f` to `omitted > 0`. Scenario 16 explicitly asserts that honest discovery gossip converges before honest replicas reach COMPLETE, so every honest replica holds every honest certificate.

## 14. Crash Injection and Tow

### 14.1 Injection

Do not reuse `stopVehicle()` as the crash primitive. Add a dedicated TraCI force-stop helper.

At `[CRASH-INJECT]`:

1. Freeze the selected vehicle inside the conflict box.
2. Set `crashCommsDisabled_`.
3. Clear queued application, discovery, gossip, and PBFT retry transmissions for that module.
4. Reject future TX enqueue/send paths and RX dispatch.
5. Mark the local ResDB replica silent if the bridge exposes that control.

Frames already handed to the MAC may appear during one bounded grace interval. Test zero wreck traffic after that grace rather than claiming impossible cancellation of already-owned frames.

### 14.2 Tow

At `crash_time + clearDelaySec`, remove both wrecks from TraCI. The current Veins wrapper lacks `vehicle.remove()`, so add a SUMO REMOVE command wrapper or perform removal through the scenario manager.

CLEAR depends on observed empty conflict box plus `clearDwellSec`, not directly on the tow timer.

Crash and clearance polling must run after ORDER is applied. Place it before the current `order_applied_` early return or use a dedicated world-state timer.

## 15. State Ownership

Use explicit state rather than unrelated booleans:

```cpp
struct WaitHeartbeatState {
    uint32_t cancelledEpoch = 0;
    uint32_t executingBatch = 0;
    int leaderId = -1;
    uint32_t lastHeartbeatIndex = 0;
    simtime_t validUntil = SIMTIME_ZERO;
    bool active = false;
};
```

One transition helper owns WAIT timer replacement, expiration, CLEAR supersession, ORDER supersession, and logs.

Ownership boundaries:

- `ResDBArrivalProtocol.cc`: arrival evidence and discovery state.
- `ResDBRollbackProtocol.cc`: BLOCKED, CANCEL, incident registry, CLEAR, WAIT heartbeat state.
- `ResDBDecision.cc`: ordinary ORDER construction and CLEAR trailer.
- `ResDBTransport.cc`: PBFT transport and fixed-interval consensus retry manager.
- `ResDBIntersectionApp.cc::onWSM()`: application message dispatch for CLEAR and WAIT.
- ResDB bridge: ordinary ORDER parsing, CLEAR trailer validation, request-scoped active view. No WAIT handling.

## 16. Existing Machinery to Reuse

- Shared `DiscoveryRound` states and certificate-drain behavior.
- Full-statement-keyed CANCEL echo buckets.
- CANCEL drain and active-batch leader exclusion.
- `perceivedActiveBatch()` and committed batch assignments.
- CANCEL tombstones and cancel-commit gossip.
- Request-scoped active views for ordinary epoch proposals.
- `ConsensusRetryManager` and Stage A early-vote buffering/replay.
- Decision-gossip exponential backoff pattern.
- TraCI conflict-box/clearance helpers where semantics match.

Stage A in this repository means PBFT early-vote buffering, vote-drop accounting, and bounded phase retries. It does not include a consensus wire tag.

## 17. Parameters

Provisional defaults:

| Parameter | Default | Purpose |
|---|---:|---|
| `crashVehicle` | false | Scenario injection flag |
| `crashOnBoxEntrySec` | 0 s | Delay after selected batch enters box |
| `crashDwellSec` | 2.0 s | Stationary dwell before BLOCKED echo |
| `crashSpeedEps` | 0.1 m/s | Stationary threshold |
| `clearDelaySec` | 10.0 s | Scenario tow delay |
| `clearDwellSec` | 1.0 s | Empty-box debounce |
| `waitHeartbeatIntervalSec` | 1.0 s | Leader heartbeat period while blocked |
| `waitHeartbeatMaxDeferralSec` | 2.5 s | Maximum follower timeout extension |
| `waitClockSkewSec` | 0 s | Simulation allowance; deployment-specific |
| `evidenceRetryBaseSec` | 0.10 s | Fast-start evidence retry base |
| `evidenceRetryFactor` | 2.0 | Retry growth factor |
| `evidenceRetryCapSec` | 2.0 s | Evidence retry cap |
| `cancelGossipRetryBaseSec` | 0.25 s | CANCEL-commit gossip base |
| `cancelGossipRetryCapSec` | 4.0 s | CANCEL-commit gossip cap |
| `evidenceRetryMax` | 12 | Per-statement retry bound |

Require:

```text
waitHeartbeatMaxDeferralSec
    > waitHeartbeatIntervalSec
      + maximum measured heartbeat delivery jitter
      + radio scheduling margin
```

## 18. Implementation Inventory

### Veins

- `ResDBIntersectionApp.h`
  - Add incident, CLEAR, WAIT heartbeat, key registry, and keyed retry state.
- `ResDBRollbackProtocol.cc`
  - Batch incident formatter/parser and registration.
  - Trusted BLOCKED/CLEAR validation.
  - Advisory WAIT send/receive/expiry/supersession.
  - Phased backoff for BLOCKED cert and CANCEL-commit gossip.
- `ResDBDecision.cc`
  - Optional CLEAR evidence trailer and crash-recovery ORDER gate.
- `ResDBIntersectionApp.cc`
  - Parameters/timers, world-state polling, message dispatch.
- `ResDBTraCI.cc` and TraCI command interface
  - Crash freeze and vehicle removal.
- `ResDBWitnessCert.h/.cc`
  - Shared validation, collection, identity binding, keyed backoff policy.
- `ResdbV2VWire.h`
  - No consensus tag change.
- Generated/source Makefile
  - Add new witness-cert object.

### Bridge

- Define and parse only the ORDER evidence trailer.
- Register the read-only evidence validation/adoption callback.
- Adopt CLEAR before ordinary ORDER checks.
- Preserve ordinary active-view construction.
- Add no WAIT payload, executor result, or active view.

### Scenario and analyzer

- Add Scenario 16 configuration/orchestrator mapping.
- Select wrecks from committed batch 0.
- Parse BLOCKED, CANCEL, WAIT heartbeat, CLEAR, ORDER evidence, backoff, and channel load.
- Update `ARCHITECTURE.md` after implementation to remove stale rollback-wrapper claims and document WAIT as advisory.

## 19. Staged Implementation

### Stage 0: Identity and Scenario 15 regression

1. Add immutable witness key binding.
2. Harden existing CANCEL validation.
3. Add the bridge evidence callback.
4. Run Scenario 15 with ordinary post-CANCEL ORDER.

Gate: Scenario 15 remains green; wrong-key signer ID forgery is rejected.

### Stage 1: BLOCKED and CANCEL

1. Add crash freeze/comms kill.
2. Add execution-bound crash perception.
3. Form one batch-scoped BLOCKED certificate.
4. Register BLOCKING independently of CANCEL singleton state.
5. Commit CANCEL under the 16-member view.

Gate: two wrecks produce one incident certificate; CANCEL commits with quorum 11.

### Stage 2: CLEAR and ordinary ORDER

1. Add CLEAR_ECHO/CLEAR_CERT.
2. Add tow/removal and empty-box dwell.
3. Add CLEAR evidence trailer.
4. Gate ORDER on discovery COMPLETE plus incident CLEARED.

Gate: one remaining wreck prevents CLEAR and ORDER; removing both allows a 14-entry ordinary ORDER with valid CLEAR evidence.

### Stage 3: Advisory WAIT

1. Add one signed WAIT heartbeat type.
2. Delay only local recovery-leader suspicion timers.
3. Add strict CLEAR and ORDER supersession.
4. Test heartbeat loss/expiry without claiming full Byzantine view change.

Gate: WAIT never reaches the bridge, never changes the ORDER proposer, and never blocks valid ORDER with CLEAR evidence.

### Stage 4: Backoff and omission enforcement

1. Add keyed fast-start exponential backoff for BLOCKED/CLEAR cert retries.
2. Add capped exponential CANCEL-commit gossip.
3. Keep PBFT retry cadence unchanged.
4. Change locally proven omission rejection to `omitted > 0`.
5. Add channel-load and convergence assertions.

## 20. Acceptance Tests

Run at least 10 seeds after each stage affecting consensus or evidence behavior.

### Baseline

1. Scenario 15 remains green.
2. Post-CANCEL ORDER is ordinary ORDER.
3. No `ResdbConsensusTag` or sequence arithmetic is introduced.
4. No WAIT bytes enter bridge dispatch or the executor.

### Crash and CANCEL

1. `ORDER(0)` commits with 16 entries, `N=16`, `f=5`, quorum 11.
2. Both wrecks become silent after the MAC grace interval.
3. Witnesses converge on one batch incident.
4. BLOCKED_CERT has at least six trusted signers and no wreck signer.
5. CANCEL commits with 11-14 voters and tombstones epoch 0.

### WAIT

1. Only the ordinary next-epoch certificate primary emits WAIT after discovery completes.
2. Followers with BLOCKING defer their local suspicion timer.
3. A follower missing WAIT may suspect the leader but never moves.
4. WAIT expiry never restores ORDER(0).
5. WAIT never changes the ordinary ORDER proposer or bridge active view.
6. CLEAR immediately cancels WAIT state at every receiver.
7. A delayed WAIT arriving after CLEAR is ignored.
8. A valid ORDER with embedded CLEAR is accepted even when the follower had not received separate CLEAR gossip and still had WAIT active.
9. Forged, stale-index, wrong-leader, excessive-deferral, and wrong-incident WAIT messages are rejected.

### CLEAR and ORDER

1. Removing only one wreck does not produce CLEAR_CERT.
2. Empty box after both removals produces CLEAR_CERT with six trusted signers.
3. No ORDER commits while the incident remains BLOCKING.
4. Crash-recovery ORDER embeds matching CLEAR evidence.
5. All honest replicas hold all 14 honest epoch-1 arrival certificates before honest discovery COMPLETE.
6. `ORDER(1)` has 14 signed entries, excludes wreck IDs, and commits under `N=14`, `f=4`, quorum 10.
7. All scheduled vehicles cross without conflict-box overlap.

### Backoff and adversarial tests

1. First BLOCKED/CLEAR/CANCEL-gossip transmissions are immediate.
2. Retry intervals grow to their cap with jitter and no arithmetic overflow.
3. Accepting a new unique echo resets the statement's backoff.
4. PBFT retry intervals remain unchanged.
5. Channel frames per second fall after the fast-start evidence window.
6. Underweight or wrong-key evidence certificates are rejected.
7. ORDER without CLEAR evidence is rejected for crash recovery.
8. ORDER omitting or QUIET-encoding a locally certified honest vehicle is rejected.

## 21. Invariants

1. Only committed ORDER grants crossing authority.
2. Committed CANCEL permanently tombstones the cancelled epoch.
3. BLOCKED and CLEAR require `f+1` trusted physical witnesses.
4. WAIT is advisory and has no quorum, lock, vote, active view, or proposer authority.
5. WAIT may only delay a local leader-suspicion timer while the incident is locally BLOCKING.
6. CLEAR and valid crash-recovery ORDER always supersede WAIT.
7. Missing or expired WAIT never authorizes motion.
8. Local perception may halt and emit evidence but cannot create BLOCKING/CLEARED registry state without a valid certificate.
9. One executing batch maps to one blocked incident per cancelled epoch.
10. A crash-recovery ORDER carries matching CLEAR evidence.
11. Active PBFT membership comes from committed/proposal-validated membership, never a live perception snapshot.
12. A witness signature counts only when its key is bound to its claimed replica ID.
13. A fresh arrival certificate held by a follower cannot be silently omitted or converted to QUIET.
14. Exponential backoff applies to evidence/gossip retries, not PBFT phase retries or WAIT heartbeat cadence.
