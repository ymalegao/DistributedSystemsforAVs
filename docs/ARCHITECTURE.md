# System Architecture: ResDB-over-Veins V2V Intersection Coordination

This is the current architecture handoff for the V2V intersection coordination system. The runnable codebase now uses the C++ ResilientDB integration; the old Java/BFT-SMaRt/JNI implementation has been removed from the active source tree.

The canonical runtime path is:

```text
OMNeT++ / Veins vehicle app
  -> ResDBIntersectionApp C++ arrival-cert protocol
  -> ResilientDB PBFT through resdb_omnet_bridge
  -> 802.11p radio frames for all inter-vehicle traffic
  -> direct C++ order callback
  -> TraCI vehicle control
```

There is no Java or JNI consensus path on the current hot path. Archived migration docs may still mention BFT-SMaRt/JNI as historical reference material, especially for the original arrival protocol, conflict matrix, verifier semantics, and leader-change research, but those files are not part of the build or runtime.

---

## 1. Core Invariants

1. **Each vehicle is one PBFT replica.** `veh0` maps to ResDB replica `0`, `veh1` to replica `1`, and so on. ResDB's internal config still uses 1-based node ids, so the bridge translates between ResDB node id `N+1` and OMNeT replica id `N`.

2. **All inter-vehicle protocol traffic uses the Veins 802.11p radio model.** Arrival messages, ResDB PBFT bytes, view-change traffic, and decision gossip are carried as `BFTMessage` frames. ResDB is socketless in simulation mode.

3. **OMNeT++ owns simulated time.** `ResDBIntersectionApp` periodically calls `ResdbOmnetUpdateSimTimeUs()`. ResDB worker threads read `SimTimeProvider::NowUs()` and wait through `SimTimeProvider::SleepForUs()` / `SleepUntilUs()`.

4. **OMNeT++ simulation APIs are used only on the simulation thread.** ResDB worker threads enqueue outbound packets and committed orders. `ResDBIntersectionApp` drains those queues from self-messages.

5. **Discovery completes locally before ORDER consensus.** Every replica runs the same view-based discovery state machine. Noisy lane observation, lane-qualified echo collection, maneuver-cue aggregation, and `ARRIVAL_CERT` validation happen in Veins C++ before the elected discovery primary submits `ResdbProposeHdr + ResdbVehicleEntry[]` to PBFT.

6. **Consensus decides an order, not movement directly.** ResDB commits binary order bytes. `ResDBIntersectionApp::processOrders()` applies the order, waits for preceding batches to clear through TraCI, and then resumes the vehicle.

7. **Type 9 is now decision gossip.** In the legacy Java path, type 9 carried Java client-request broadcasts. In the current ResDB path, type 9 carries signed post-consensus order gossip so stragglers can catch up after missing the PBFT storm.

8. **Type 10 is arrival-announce gossip.** Replicas that verify an `ARRIVAL_ANNOUNCE` may relay the original announcement bytes through the existing signed gossip wrapper. The relayer signs only the outer carrier frame; the inner announcement remains the originating vehicle's byte-for-byte payload.

9. **Crash recovery separates decisions, evidence, and liveness advice.** ORDER and CANCEL are committed PBFT decisions. BLOCKED and CLEAR are `f+1` physical-evidence certificates. WAIT is a signed, leader-only advisory heartbeat that can delay a local suspicion timer but cannot authorize motion, change membership, or validate an ORDER.

10. **Propagation is state-aware where implemented.** Decision/CANCEL gossip and CANCEL/CLEAR certificate paths stop on peer propagation or owning state transitions. CLEAR and TYPE11 count registry-bound carriers; TYPE11 uses deterministic, cancellable suppression rather than immediate all-replica flooding. Remaining blind paths are listed in Section 22.

11. **Arrival evidence and co-batching authority are separate.** Honest external witnesses emit an echo when their noisy observed approach matches the declared approach; maneuver cues never veto that echo. A valid lane certificate with fewer than `f+1` matching cues is SIGNED-UNKNOWN and remains a singleton. Only `f+1` positive cue signatures unlock the declared direction for the existing `kSafe` scheduler.

12. **PBFT agrees on a deterministic certificate-derived value.** Proposal packing, received-certificate validation, and `certSnapshotCallback()` all derive the scheduler-facing direction from the same authenticated certificate bytes. Check 10 rejects a leader that upgrades UNKNOWN to the declaration. PBFT does not turn the declaration or cue evidence into physical ground truth.

---

## 2. High-Level Node Model

Each simulated vehicle runs one `ResDBIntersectionApp` module. That module owns the vehicle's interaction with SUMO/TraCI, V2V arrival certificates, ResDB bridge lifecycle, radio transport, order delivery, and post-consensus gossip.

```text
Vehicle node i
|
+-- ResDBIntersectionApp
|   |
|   +-- TraCI helpers
|   |   +-- hidden lane/signal truth supplied to the sensor model
|   |   +-- stop-line distance
|   |   +-- stop / resume vehicle
|   |   +-- intersection-clearance checks
|   |
|   +-- ResDBPerception (witness-local)
|   |   +-- 4x4 approach confusion matrix
|   |   +-- noisy turn-signal cue
|   |   +-- dedicated RNG stream and zero-error no-draw path
|   |
|   +-- Arrival certificate protocol
|   |   +-- ARRIVAL_ANNOUNCE type 1
|   |   +-- ARRIVAL_ANNOUNCE_GOSSIP type 10
|   |   +-- lane-qualified ARRIVAL_ECHO type 4 with signed observedCue
|   |   +-- local claimant self-attestation
|   |   +-- ARRIVAL_CERT type 5 and derived direction eligibility
|   |
|   +-- ResDB bridge handle
|   |   +-- socketless ServiceNetwork
|   |   +-- OmnetConsensusManagerPBFT
|   |   +-- IntersectionExecutor
|   |   +-- order callback
|   |
|   +-- Radio transport
|   |   +-- outbound ResDB queue from worker threads
|   |   +-- signed type 8 PBFT radio frames
|   |   +-- inbound type 8 delivery to ResDB
|   |
|   +-- Decision gossip
|       +-- signed type 9 order broadcast
|       +-- f+1 matching gossip votes to apply missed order
|
+-- Veins NIC / 802.11p channel
```

The ResDB library still runs internal worker threads, but all interaction with Veins is mediated by the C bridge and callback queues. The app never includes internal ResDB C++ headers directly; it includes only `integration/omnet/resdb_omnet_bridge.h`.

---

## 3. Main Component Inventory

| File | Role |
|------|------|
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.h` | Main Veins app declaration. Defines phases, Byzantine modes, arrival-cert structs, transport queues, timers, gossip state, TraCI state, and ResDB callback hooks. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc` | App lifecycle, self-message dispatch, radio type dispatch, shared gossip timers, metrics, and fault-injection coordination. Protocol bodies are split into the files below. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.ned` | NED parameters for replica identity, ResDB paths, radio transport, jitter, cert timeout, gossip, view-change timeout, Byzantine injection, and TraCI behavior. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/IV2VTransport.h` | Minimal abstract transport interface. Provides C-compatible adapters for the bridge callback table. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBArrivalProtocol.cc` | Arrival ANN/ECHO/CERT handling, discovery-round closure/drain, announce gossip, cert retries, and stop-zone cert gossip. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBDistanceProtocol.cc` | Stopped-distance Type-18/19/20 authentication, cached witness evaluation, bounded attestation retransmission, distance-certificate assembly/validation, and deterministic queue-rank derivation. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBDecision.cc` | Normal and post-CANCEL ORDER construction, optional CLEAR evidence trailer, order callback processing, and movement scheduling. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBTransport.cc` | ResDB outbound/inbound radio transport, bounded PBFT retries, and deterministic cancellable TYPE11 propagation. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResdbV2VWire.h` | Shared signed-envelope helper used by types 8–11, 14, 16, and 17 where an authenticated outer carrier is required. Layout is pubkey, signature length, DER ECDSA signature, then inner bytes. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBDecisionGossip.h/.cc` | Pure relay-dedup logic for three independent mechanisms: (1) decision gossip — serializes `epoch \|\| order_bytes`, parses TYPE9 payloads, counts matching votes per sender via `GossipAccumulator`; (2) cert relay — `CertRelayTracker` deduplicates per-carId ARRIVAL_CERT re-floods so each node relays each validated cert exactly once; (3) announce gossip — serializes `epoch \|\| original_announce_bytes` and deduplicates per `(epoch, carId)` through `AnnouncementRelayTracker`. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBRollbackProtocol.cc` | BLOCKED/CANCEL/CLEAR/WAIT protocol module. Owns types 12–17, local halt, CANCEL consensus, incident state, CLEAR propagation, post-CANCEL round setup, tombstones, and rollback proposal gating. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBWitnessCert.h/.cc` | Shared `f+1` witness statement/certificate validation and immutable replica-id-to-P-256-key registry. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBPropagationTracker.h` | Generic semantic-keyed distinct-carrier tracker. Authentication and membership checks remain at the caller; currently used by CLEAR propagation. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBTraCI.cc` | TraCI helpers extracted from the legacy V2V module: distance-to-lane-end, lane queue discovery, vehicle stop/resume helpers, and clearance detection. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBPerception.h/.cc` | Witness-local imperfect-perception adapter. Reads target approach, signal state, world pose, and stop-line distance as hidden simulator truth; applies categorical and continuous corruption on the dedicated perception RNG stream; and returns early-arrival or stopped-distance samples without exposing uncorrupted truth to the admission rule. |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/crypto/CryptoAuth.h/.cc` | OpenSSL ECDSA P-256 helper. Generates per-vehicle EC keys, signs arbitrary byte buffers, verifies signatures, and contains CA certificate helpers. |
| `incubator-resilientdb/integration/omnet/resdb_omnet_bridge.h` | C ABI between Veins and ResDB. Defines lifecycle, transport callback registration, sim-time update, consensus trigger, order callback, cert-primary/PBFT primary alignment, view-change hooks, and shared packed structs. |
| `incubator-resilientdb/integration/omnet/resdb_omnet_bridge.cc` | ResDB-side integration. Builds socketless PBFT service, installs OMNeT communicator, registers pre-verify function, hosts `IntersectionExecutor`, injects inbound packets, and exposes the C API. |
| `incubator-resilientdb/platform/consensus/ordering/pbft/omnet_forced_view.h` | Header-only request-scoped active-view registry. Stores proposal-defined epoch membership `M`, quorum `2f+1`, primary, and sender-admission helpers without rewriting static `server.config`. |
| `incubator-resilientdb/common/utils/sim_time_provider.h/.cpp` | Global simulated-time provider used by ResDB worker threads. Updated by the OMNeT simulation thread. |

---

## 4. End-to-End Flow

```mermaid
sequenceDiagram
    participant App as ResDBIntersectionApp
    participant Radio as IEEE80211p_Radio
    participant Bridge as resdb_omnet_bridge
    participant PBFT as ResDB_PBFT
    participant Exec as IntersectionExecutor
    participant TraCI as SUMO_TraCI

    App->>App: sign declaration and add one local self-attestation
    App->>Radio: ARRIVAL_ANNOUNCE_type1
    Radio->>App: ARRIVAL_ANNOUNCE_GOSSIP_type10
    App->>TraCI: read target approach and signal as hidden truth
    App->>App: corrupt perception; echo iff observed lane matches claim
    App->>Radio: ARRIVAL_ECHO_type4 with observedCue and claimHash
    Radio->>App: collect distinct signed echoes through finalize window
    App->>Radio: ARRIVAL_CERT_type5
    App->>App: validate cert and derive direction or UNKNOWN

    App->>TraCI: stopVehicle_at_stop_zone
    App->>App: all_replicas_close_stable_certified_intent_view
    App->>App: drain_queued_CERT_frames
    App->>Bridge: ResdbOmnetTriggerConsensus
    Bridge->>PBFT: inject_TYPE_NEW_TXNS

    PBFT->>Bridge: outbound_PRE_PREPARE_PREPARE_COMMIT_VIEW_CHANGE
    Bridge->>App: transport_callback_enqueueOutbound
    App->>Radio: signed_type8_PBFT_frame
    Radio->>App: signed_type8_PBFT_frame
    App->>Bridge: ResdbOmnetDeliverPacket
    Bridge->>PBFT: ServiceNetwork_InjectInboundPacket

    PBFT->>Bridge: PRE_PREPARE_preverify
    PBFT->>Exec: committed_request_ExecuteData
    Exec->>App: ResdbOrderDecidedFn_callback
    App->>App: pending_orders_queue
    App->>App: processOrders_on_sim_thread
    App->>Radio: signed_type9_decision_gossip
    App->>TraCI: resume_when_batch_clear
```

The key split is between ResDB worker threads and the OMNeT++ simulation thread. Worker threads may call transport callbacks and order callbacks. Those callbacks only enqueue data. Self-messages (`transport_poll_msg_`, `time_tick_msg_`, `preceding_batch_poll_msg_`, and others) do the actual simulation work.

---

## 5. Runtime Lifecycle

### Stage 0 initialization

`ResDBIntersectionApp::initialize(stage == 0)` performs the system-level setup:

1. Reads NED parameters: replica id, total vehicles, ResDB config paths, radio settings, timeout settings, Byzantine mode, gossip settings, and TraCI behavior.
2. Binds `replicaId_` to the SUMO external id when possible. This keeps the PBFT replica id aligned with the SUMO vehicle id (`vehN`).
3. Generates the vehicle's ECDSA P-256 keypair through `CryptoAuth::generateKeyPair()`.
4. Creates either `VeinsTransport` or `LoggingTransport`.
5. Creates a real ResDB server handle with `ResdbOmnetCreateKvServer()` when config paths are present; otherwise creates a null handle.
6. Registers transport callbacks through `ResdbOmnetSetTransport()`.
7. Registers the order callback through `ResdbOmnetSetOrderCallback()`.
8. Registers the cert-snapshot callback through `ResdbOmnetSetCertSnapshotFn()` — enables pre-verify Checks 9 and 10.
9. Configures the PBFT view-change timeout through `ResdbOmnetSetVcTimeoutUs()`.
10. Starts sim-time ticking through `ResdbOmnetUpdateSimTimeUs()`.
11. Runs the socketless ResDB server thread through `ResdbOmnetRunServer()`.
12. Applies full PBFT communicator muting only when the explicit
    `byzantinePbftSilent` test flag is enabled. `BYZANTINE_SILENT_PRIMARY`
    suppresses proposal submission but leaves view-change traffic enabled.

### Stage 1 initialization

`initialize(stage == 1)` schedules the simulation behavior:

1. Optional smoke test for transport callbacks.
2. Staggered initial arrival announcement at `triggerJoinTimeSec + replicaId * arrivalSlotSec`.
3. Periodic arrival announcements until the car has broadcast its certificate or consensus starts.
4. Optional channel/SINR CSV metrics sampling.

### Teardown

`finish()` advances ResDB sim-time to a very large value before stopping the server. This wakes ResDB threads blocked in simulated sleeps. It then stops global stats, cancels VC timers, stops and destroys the ResDB handle, and tears down channel metrics.

---

## 6. V2V Message Type Registry

All inter-vehicle traffic is carried in `BFTMessage` packets over the Veins 802.11p channel.

| Type | Name | Current meaning | Payload |
|------|------|-----------------|---------|
| `1` | `ARRIVAL_ANNOUNCE` | Vehicle declares arrival, approach, queue-rank field, maneuver, ambulance flag, time, and epoch under an origin signature. The declaration is not itself independent physical evidence. | Text/pipe encoded authenticated arrival announcement. |
| `4` | `ARRIVAL_ECHO` | External witness's lane-qualified evidence for one authenticated claim. Carries the declared maneuver separately from the witness's noisy maneuver cue. Re-announcements may retransmit the same cached logical echo but never create a new perception trial. | Text/pipe encoded echo with `observedCue`, `claimHash`, signer id, registered P-256 pubkey, and ECDSA signature. |
| `5` | `ARRIVAL_CERT` | Claimant broadcasts at least `f+1` and at most `N` distinct valid signatures collected through the post-threshold window. Certificate bytes deterministically yield SIGNED-UNKNOWN or an eligible declared direction. | Text/pipe encoded claim plus per-echo signer ids, cues, claim hash, pubkeys, and signatures. |
| `8` | ResDB PBFT bytes | ResDB PRE_PREPARE, PREPARE, COMMIT, VIEW_CHANGE, NEW_VIEW, and related PBFT traffic. | `resdbwire` signed wrapper around serialized ResDB bytes. |
| `9` | Decision gossip | Post-consensus order dissemination for stragglers that missed PBFT delivery. | `resdbwire` signed wrapper around `epoch || order_bytes`. |
| `10` | Arrival announce gossip | Relay of an already-signed `ARRIVAL_ANNOUNCE` by a witness or carrier replica. | `resdbwire` signed wrapper around `epoch || original ARRIVAL_ANNOUNCE bytes`. |
| `11` | ResDB consensus relay | Re-flood of selected raw ResDB PBFT bytes through the existing signed carrier. | `resdbwire` signed wrapper around `epoch || raw ResDB bytes`. |
| `12` | `CANCEL_ECHO` | Witness attests that epoch `e` should be cancelled for a verified emergency or crash reason. | Text/pipe encoded cancel echo with signer's compressed P-256 pubkey and ECDSA signature. |
| `13` | `CANCEL_CERT` | f+1 collected `CANCEL_ECHO`s for epoch `e`; valid receivers halt locally and enter the CANCEL drain/consensus state machine. Discovery for `e+1` starts only after CANCEL commits. | Text/pipe encoded cancel cert carrying echo signer ids, pubkeys, and signatures. |
| `14` | CANCEL-commit gossip | Post-CANCEL attestation used to disseminate the committed tombstone and let stragglers begin recovery discovery. | `resdbwire` signed wrapper around `cancelled_epoch || ResdbCancelDecisionHdr`. |
| `15` | `CLEAR_ECHO` | One-shot witness statement that the blocked executing batch's conflict box remained empty for `clearDwellSec`. | CLEAR statement plus trusted witness id, bound public key, and signature. |
| `16` | `CLEAR_CERT` | `f+1` CLEAR evidence carried in a carrier-signed envelope. Candidate senders and relays are ranked and cancellable. | `resdbwire` signed wrapper around the unchanged embedded CLEAR certificate. |
| `17` | WAIT heartbeat | Signed advisory lease from the ordinary epoch-`e+1` cert-primary while a crash incident remains BLOCKING. No quorum and no bridge/PBFT meaning. | `resdbwire` signed wrapper around fixed-layout `WaitHeartbeatPayload`. |

Important legacy note: type `9` used to mean Java/BFT-SMaRt client-request broadcast in the JNI architecture. In the current ResDB architecture it is not a client request; it is post-consensus decision gossip.

---

## 7. Wire Formats

### Signed radio wrapper

Defined in `ResdbV2VWire.h`:

```text
[33 bytes sender_pub_key_compressed]
[1 byte  sig_len]
[sig_len bytes DER ECDSA_SHA256(signature over inner bytes)]
[inner bytes]
```

The wrapper is used for PBFT transport and for carrier-authenticated gossip/evidence paths. Important inner payloads are: raw ResDB bytes for type `8`; `epoch || order_bytes` for type `9`; `epoch || original announce` for type `10`; `epoch || raw ResDB bytes` for type `11`; CLEAR certificate bytes for type `16`; and `WaitHeartbeatPayload` for type `17`.

Inbound wrapped messages are dropped if:

1. The signed packet cannot be parsed.
2. The inner byte length is zero.
3. `CryptoAuth::verifyBytes(pubKey, innerBytes, sig)` fails.

Every module registers its generated P-256 key in `WitnessKeyRegistry`; the first binding for a replica id is immutable. Security-critical evidence paths bind the wrapper or embedded witness key to the claimed replica id: BLOCKED/CANCEL and CLEAR certificate validation, type `16` carrier counting, type `17` WAIT, and type `11` suppression carriers. Type `9`, type `10`, type `14`, and the direct type `8` outer wrapper currently verify the included key's signature but do not all apply the registry binding; that remaining hardening gap is documented in Section 22. ResDB's built-in node signature verifier remains disabled in the simulation bridge because the socketless path uses these P-256 radio envelopes.

For type `10`, this outer signature authenticates the carrier frame, not the original vehicle. The carried announcement bytes are not rewritten. They remain the exact `ARRIVAL_ANNOUNCE` payload produced by the origin vehicle, including its self-signature and claimed fields.

### Consensus proposal payload

Defined in `resdb_omnet_bridge.h` and passed to `ResdbOmnetTriggerConsensus()`:

```c
#pragma pack(push, 1)
typedef struct ResdbVehicleEntry {
    int32_t  replica_id;
    uint64_t sim_time_us;
    uint8_t  is_ambulance;
    uint8_t  lane;
    uint8_t  direction;
    uint8_t  position_in_lane;
    uint8_t  cyber_status;
    uint8_t  physical_lane_index;
    int32_t  lateral_claim_cm;
} ResdbVehicleEntry;  // 22 bytes

typedef struct ResdbProposeHdr {
    uint32_t epoch;
    int32_t  leader_id;
    uint64_t propose_sim_time_us;
    uint32_t n_vehicles;
} ResdbProposeHdr;  // 20 bytes
#pragma pack(pop)
```

The full proposal is:

```text
ResdbProposeHdr
ResdbVehicleEntry[hdr.n_vehicles]
```

Field semantics:

| Field | Meaning |
|-------|---------|
| `replica_id` | Vehicle/replica id, 0-based. |
| `sim_time_us` | Sim-time when the vehicle entered the stop zone or was observed. `UINT64_MAX` marks a QUIET synthetic entry. |
| `is_ambulance` | `1` for ambulance priority, `0` otherwise. Current announce path still trusts this flag from the arrival announcement. |
| `lane` | `0=N`, `1=S`, `2=E`, `3=W`. |
| `direction` | `0=Straight`, `1=Left`, `2=Right`. |
| `position_in_lane` | `1` is the front vehicle in its lane. Larger values are farther back. |
| `cyber_status` | `1=SIGNED` when an f+1 arrival cert exists. `0=QUIET` when no f+1 cert was available by proposal time. |

### Order decision payload

Defined in `resdb_omnet_bridge.h` and delivered through `ResdbOrderDecidedFn`:

```c
#pragma pack(push, 1)
typedef struct ResdbVehicleDecision {
    int32_t  replica_id;
    uint32_t batch_index;
} ResdbVehicleDecision;  // 8 bytes

typedef struct ResdbOrderHdr {
    uint32_t epoch;
    uint32_t n_vehicles;
    uint32_t n_batches;
} ResdbOrderHdr;  // 12 bytes
#pragma pack(pop)
```

The full order is:

```text
ResdbOrderHdr
ResdbVehicleDecision[hdr.n_vehicles]
```

`batch_index` is 0-based. Vehicles with the same `batch_index` may cross together. Vehicles in batch `k > 0` wait until all vehicles in batch `k - 1` have cleared the conflict region according to TraCI.

### Decision gossip payload

Defined in `ResDBDecisionGossip.h/.cc`:

```text
uint32_t epoch
uint8_t  order_bytes[]
```

The gossip payload is then wrapped in the same `resdbwire` signed wrapper used by type `8`.

### Arrival announce gossip payload

Defined in `ResDBDecisionGossip.h/.cc`:

```text
uint32_t epoch
uint8_t  original_arrival_announce_bytes[]
```

The original announce bytes are copied unchanged from the type `1` payload. `ResDBIntersectionApp::sendArrivalAnnouncementGossipPayload()` wraps these inner bytes in `resdbwire::packSignedPacket()` before sending type `10`.

---

## 8. Imperfect-Perception Arrival Certificate Protocol

The arrival protocol is an evidence gate before PBFT, not a proof of perfect
physical state or future maneuver. Noisy approach evidence controls whether an
external witness signs the arrival claim. Signed maneuver cues control only
whether the resulting certificate unlocks the declared direction for
co-batching. This creates three scheduler-visible trust tiers: QUIET,
SIGNED-UNKNOWN, and SIGNED with an eligible direction.

### Discovery round state machine

Initial discovery and discovery after a committed CANCEL use the same states and completion rule:

```text
INACTIVE
  -> COLLECTING
  -> DRAINING_CERTS
  -> COMPLETE
  -> INACTIVE when ORDER applies, the vehicle departs, or a newer round starts
```

- `COLLECTING` accepts and transmits ANN, ANN gossip, ECHO, and CERT traffic. Discovery begins while the vehicle approaches the intersection.
- The stop zone arms the hard `cert_collection_timeout_` deadline and the intent-settle timer. Post-CANCEL participants are already stopped, so both timers arm immediately.
- `observed_intent_cars_` contains validated network intent, not an independent physical census. A normal close therefore requires no new eligible intent for `discoveryIntentSettleSec`, at least one eligible intent, the recallable local vehicle's intent, and a valid cert for every eligible intent.
- The hard deadline closes an incomplete view when an announced vehicle is Byzantine, crashed, or unreachable. Only observed uncertified intents become QUIET; unobserved configured replica IDs are not synthesized into membership.
- `DRAINING_CERTS` stops ANN/ECHO production and removes queued non-CERT discovery frames. Queued CERTs remain. A replica cannot enter `COMPLETE` until its assembled local cert has actually reached the radio and all queued cert frames for the round have drained.
- A newly accepted eligible ANN before proposal submission reopens `COLLECTING`, restarts stabilization, and cancels the follower leader-timeout.
- `COMPLETE` lets the elected primary call `proposeAll()` and lets followers arm the ordinary VC trigger. During crash recovery, a still-BLOCKING incident gates ORDER submission and activates WAIT; CLEAR or an ORDER carrying valid CLEAR evidence ends WAIT. PBFT TYPE8/TYPE11 traffic does not itself close discovery.

The primary's PRE_PREPARE is therefore not the discovery-closure event. Every follower reaches closure from its own stabilized, certified intent view.

### Phase A: Arrival announcement

Each vehicle periodically broadcasts `ARRIVAL_ANNOUNCE` (`messageType = 1`) until it has broadcast its cert or consensus has started.

The announcement contains:

```text
carId
laneId
cardinal lane
positionInLane
direction
isAmbulance
claimedArrivalTime
epoch
ambulanceCertBytes
ambulanceSigBytes
self signature
```

The origin signature covers the canonical declaration:

```text
carId | epoch | laneId | cardinal lane | positionInLane | declared direction |
isAmbulance | claimedArrivalTime | ambulanceCertBytes | ambulanceSigBytes
```

The receiver verifies this signature using the origin replica's immutable
registered public key. `claimHash` is SHA-256 over the authenticated serialized
announcement and binds every witness signature to that exact claim variant.
The carried `positionInLane` is a queue-order field, separate from lane
verification.

`lane_observation_mode_` selects how the gate verifies the claimed lane:

- `CATEGORICAL_CARDINAL` — the gate does not verify a continuous position
  measurement; it only checks the witness's sampled cardinal approach against
  the declaration (Phase B step 3).
- `ADJACENT_LATERAL` (current default on the two-lane fixture) — the
  claimant also carries `lateralClaimCm` and `physicalLaneIndex`. In addition
  to the cardinal check, the witness compares its own noisy lateral
  observation against the claim: `lateralResidualCm = |observedLateralCm -
  ann.lateralClaimCm|` must fall within `lateralToleranceCm = physicalGateK *
  lateralObservationSigmaM * 100` (the τ = kσ tolerance from the operating
  point), and `ann.physicalLaneIndex` must match the lane the claimed
  lateral position itself projects to. This is the continuous-lateral gate
  that resolves the cardinal-only mode's inability to distinguish adjacent
  same-approach lanes. See `ResDBArrivalProtocol.cc` (`cardinalMatch` /
  `lateralMatch` / `laneMatch = cardinalMatch && lateralMatch`).

When the claimant creates the announcement, it also creates exactly one signed
local self-attestation with `observedCue=declaredDirection`. This signature is
inserted through the normal echo validation/collection path, consumes no
perception sample or RNG draw, and is never sent as Type 4 traffic. The origin
signature and self-attestation are distinct signatures with different roles.

### Phase B: Witness echo

When an honest external replica receives an authenticated announcement:

1. It rejects traffic outside `DiscoveryState::COLLECTING`, after proposal/order,
   after departure, or while crash communication is muted.
2. It uses `ResDBPerception` to take one witness-local sample. TraCI approach
   and signal state are hidden truth inputs to configured categorical corruption,
   not values compared directly with the declaration.
3. It accepts the lane claim only when the target is detected and
   `observedApproach == ann.lane` (`cardinalMatch`), plus the existing
   ambulance checks. Under `ADJACENT_LATERAL` mode this is necessary but not
   sufficient: the claim must also pass the continuous lateral-residual check
   described above (`lateralMatch`); the final verdict is
   `cardinalMatch && lateralMatch`.
4. Signal evidence never vetoes the lane echo. The sampled cue is carried as
   `observedCue in {STRAIGHT,LEFT,RIGHT,UNKNOWN}`.
5. On lane acceptance, it stores the intent and original announcement, signs
   and broadcasts an echo, and may gossip the original announcement. On lane
   rejection it records the verdict but sends no echo.

The echo signature covers:

```text
targetCarId : lane : positionInLane : declaredDirection : observedCue :
claimHash : isAmbulance : epoch : echoingReplicaId
```

Each echo includes the signer's compressed P-256 public key and DER ECDSA
signature. The key must match the immutable replica-key registry.

The first authenticated `claimHash` is locked by `(witness,target,epoch)`, and
the resulting perception verdict and signed echo are cached for that hash. A
re-announcement of the same hash reuses the cached verdict; an accepted replay
can retransmit the byte-identical logical echo for radio reliability but cannot
resample the sensor or add another distinct signer. A different authenticated
hash is retained as equivocation evidence and rejected before perception or
echo generation. Phase 1 uses `K=1`; the buffer/cache is the substrate for the
deferred repeated-sampling experiment.

The focused equivocation experiment constructs one signed LEFT announcement
and one signed RIGHT announcement and reuses those byte-identical variants
across the corresponding peer subsets. It does not re-sign per peer, because
randomized ECDSA encodings would otherwise create multiple wire hashes for the
same semantic variant.

### Announce gossip and custody relay

Announce gossip closes a topology gap where a late-arriving vehicle can directly reach only a small witness set. A witness that hears and verifies that vehicle can carry the original announcement outward so replicas outside the origin vehicle's current radio range can still echo it.

The flow is:

```text
veh18 -> ARRIVAL_ANNOUNCE type 1
replica 4 runs its cached noisy lane/cue evaluation
replica 4 -> ARRIVAL_ECHO for veh18
replica 4 -> ARRIVAL_ANNOUNCE_GOSSIP type 10 carrying veh18's original announce bytes
replica 9 receives type 10
replica 9 reconstructs a synthetic ARRIVAL_ANNOUNCE message
replica 9 runs the normal handleArrivalAnnouncement path
replica 9 runs its cached noisy lane/cue evaluation
replica 9 -> ARRIVAL_ECHO for veh18
```

Security boundary:

- The relayer signs only the outer type `10` carrier frame.
- The inner announcement bytes remain the exact type `1` payload from the origin vehicle.
- A Byzantine carrier can drop, delay, or replay within dedup limits, but cannot modify the original announcement and still preserve the origin vehicle's self-signature.
- Honest recipients still run the same cached imperfect-perception lane gate before echoing, so a relayed announce is not accepted merely because a carrier gossiped it.

Implementation:

- `gossipArrivalAnnouncement()` deduplicates immediate announce gossip with `AnnouncementRelayTracker::tryRelay(epoch, carId)`.
- `sendArrivalAnnouncementGossipPayload()` signs and broadcasts the type `10` carrier packet. It is used both by immediate verified gossip and delayed custody replay.
- `handleArrivalAnnouncementGossip()` verifies the carrier wrapper, parses `epoch || original_announce_bytes`, creates a temporary `BFTMessage`, and calls `handleArrivalAnnouncement(..., viaGossip=true, carrierReplicaId=...)`.
- `pending_relays_` is an app-local map keyed by `(epoch, carId)` that stores original announce bytes after successful verification.
- When a vehicle enters the stop zone, it flushes pending announce relays as proper signed type `10` packets with reason `stop-zone`, then clears the custody map.

This relay is intentionally not a new consensus rule. It only improves witness discovery before cert assembly. Echo dedup still prevents repeated echoes from the same replica for the same car.

### Phase C: Collection, certificate, and direction eligibility

The target collects echoes for itself, including its one local
self-attestation. At the first `f+1` distinct valid signatures, where
`f=(N-1)/3`, it arms the one-shot `arrivalCertFinalizeTimer` for
`directionEligibilityCollectionWindowSec` (default `0.25 s`). It continues
passively accepting distinct echoes up to `N`; the window adds no announcement,
gossip, polling, or periodic transmission source. If discovery drains first, a
threshold-satisfying certificate is finalized immediately and placed on the
existing pending-CERT drain path.

Every receiver validates the certificate before storing it:

1. The cert must contain at least `f + 1` echoes.
2. It may contain no more than `N` echoes, and echo signers must be distinct.
3. Each signer public key must match the registry and each signature must verify.
4. Every echo must match the cert's target, epoch, lane, queue-rank field,
   declared direction, ambulance flag, and common `claimHash`; `observedCue`
   values may differ.

If validation passes, the receiver stores `collected_certs_[carId] = cert`.

The scheduler-facing direction is a pure function of the validated certificate:

```text
support = number of distinct valid echoes with observedCue == cert.direction
eligibleDirection(cert) = cert.direction  if support >= f+1
                          UNKNOWN (3)     otherwise
```

UNKNOWN cues neither support nor contradict the declaration. A turn signal is
a noisy, target-controlled consistency cue; `f+1` matching cues do not prove
future execution. The same function is called during local/proposal packing,
received-cert handling, and `certSnapshotCallback()` so Check 10 compares two
independent derivations from identical certificate bytes.

### Certificate retries

If enabled by `enableArrivalCertRetries`, a vehicle rebroadcasts its assembled cert every `arrivalCertRetryIntervalSec` while discovery is collecting. During `DRAINING_CERTS`, future retries stop after the local cert's first successful air transmission; already queued CERT frames still flush. Retries also stop when:

1. `arrivalCertRetryMax` is reached, unless `0` means unlimited.
2. `proposeAll()` runs.
3. An order is applied.
4. the discovery round becomes inactive.

This improves visibility of type `5` certificates without adding TCP-like ACK machinery.

### Epidemic cert relay

Source-only cert retries can fail if a straggler misses every retry window from the original sender. To close this gap, each replica that receives and validates a cert for a given `carId` re-floods that cert **once**, giving epidemic propagation without source spam.

Implementation:

- `CertRelayTracker` in `ResDBDecisionGossip.h/.cc` tracks which `carId` values this node has already relayed. `tryRelay(carId)` returns `true` the first time and `false` on all subsequent calls.
- `handleArrivalCert()` calls `cert_relay_tracker_.tryRelay(cert.carId)` after validation and state reconstruction. On first receipt, it calls `sendBFTMessage(-1, serializeArrivalCert(cert), kArrivalCertType)` and logs `[CERT-RELAY]`.
- The relay fires immediately on the simulation thread — no additional timer. Natural topology-induced arrival stagger provides spread without synchronized floods.
- `cert_relay_tracker_.reset()` is called in `applyGossipOrder()` alongside `gossip_acc_.reset()`.

The acceptance rule differs from decision gossip. Decision gossip requires `f + 1` matching votes before acting. Cert relay fires as soon as `validateArrivalCert()` passes, because the cert already carries `f + 1` independent ECDSA echo signatures. Any node that validates it can safely relay it with no further quorum check.

Source retries (`enableArrivalCertRetries`) are kept. Relay is additive: it covers the case where the original sender's retry window closes before a straggler node gets coverage.

### Three trust tiers

The proposal/executor mapping is:

| Tier | Evidence | Proposal entry | Scheduling |
|------|----------|----------------|------------|
| QUIET | No valid `f+1` arrival certificate | `cyber_status=0`, `sim_time_us=UINT64_MAX` | Singleton through `IsQuietEntry()` |
| SIGNED-UNKNOWN | Valid arrival certificate, but cue support `< f+1` | `cyber_status=1`, `direction=3` | Singleton through `kSafe` fallthrough |
| SIGNED-direction | Valid arrival certificate and cue support `>= f+1` | `cyber_status=1`, `direction=0/1/2` | Existing `kSafe` behavior; LEFT remains table-forced singleton |

If the hard discovery deadline is reached with an observed intent lacking a
valid cert, the primary includes that observed vehicle as QUIET:

```text
sim_time_us = UINT64_MAX
cyber_status = 0
is_ambulance = 0
direction = 0
```

Lane and queue rank come from the locally observed `VehicleState`; unobserved
configured replicas are excluded. QUIET must not be overloaded to represent
direction uncertainty: a valid arrival certificate with insufficient cue
support is SIGNED-UNKNOWN.

### Cert-primary selection

Normal initial proposal leadership is derived from complete physical evidence, not from the static `leaderReplicaId` ini default. The frozen `OrderCandidate::voterIds` contains only static replicas present in both `collected_certs_` and `collected_distance_certs_`; its smallest id is the initial primary:

```text
InitialPrimary = min { rid | arrivalCert(rid) and stoppedDistanceCert(rid),
                            0 <= rid < totalVehicles }
```

If no jointly certified static candidate exists, no node proposes. The system
fails safe rather than assigning proposal authority to a QUIET or partially
certified vehicle.

This means a Byzantine replica 0 that lacks either stage is not selected as the
initial proposer. For the initial view, a node that knows a lower jointly
certified id behaves as a follower. After a valid PBFT view change, the current
PBFT primary may become the recovery proposer even when it is not the initial
minimum, but only if it is itself jointly certified in the validator's local
snapshot. An uncertified static successor is skipped through another ordinary
PBFT view change.

---

## 9. ResDB Bridge and Socketless PBFT

The C bridge is the only dependency boundary between Veins and ResDB. Veins includes `resdb_omnet_bridge.h` and calls C functions. Internal ResDB C++ headers stay on the ResDB side.

### Server creation

`ResdbOmnetCreateKvServer()`:

1. Loads ResDB config, private key, and cert paths.
2. Disables heartbeat.
3. Disables ResDB's built-in signature verifier.
4. Creates `OmnetConsensusManagerPBFT`.
5. Installs the PRE_PREPARE pre-verify lambda.
6. Creates `ServiceNetwork` with `enable_network_acceptor = false`.
7. Activates `SimTimeProvider` with a non-zero sentinel.
8. Stores pointers to the consensus manager and `IntersectionExecutor` in `ResdbOmnetServerHandle`.

Because `ServiceNetwork` has no acceptor, ResDB does not bind real sockets for consensus traffic in the simulation path.

### Transport callbacks

`ResDBIntersectionApp::registerTransport()` fills:

```c
ResdbOmnetTransportCallbacks {
    send_to,
    broadcast,
    ctx
}
```

The bridge's `OmnetReplicaCommunicator` calls those callbacks whenever ResDB wants to send PBFT messages. In the radio-backed path, the callback object is `VeinsTransport`, which calls `ResDBIntersectionApp::enqueueOutbound()`.

### Outbound radio path

ResDB worker thread:

```text
PBFT sends protobuf message
  -> OmnetReplicaCommunicator serializes ResDBMessage
  -> transport callback
  -> ResDBIntersectionApp::enqueueOutbound()
```

OMNeT simulation thread:

```text
transport_poll_msg_
  -> drainOutboundQueue()
  -> resdbwire::packSignedPacket()
  -> BFTMessage type 8
  -> sendDelayedDown()
```

`enqueueOutbound()` deduplicates identical outbound byte strings. This matters because some ResDB send paths call per-recipient send functions for the same logical broadcast. In the V2V model, identical per-recipient sends collapse to one broadcast frame.

Because the radio does not provide perfect links, each replica also keeps bounded retry state for its own PBFT phase packet:

- PRE_PREPARE retries stop after verified PREPARE progress is observed.
- PREPARE retries continue until that replica's local ResDB emits COMMIT. They may also stop after the local verified collector contains a matching `2f+1` COMMIT certificate; merely overhearing one COMMIT is insufficient.
- COMMIT retries continue until the decision is applied/adopted or `consensusRetryMax` is reached.

Before any locally triggered proposal enters `TYPE_NEW_TXNS`, the bridge calls `EnsureNextSeqAheadOfExecuted()`. This advances the new primary's allocator to at least `max(local executed sequence, checkpoint last commit) + 1`. It is necessary when deterministic application leadership moves from the previous PBFT primary to a follower; it does not infer epoch from sequence or assume ORDER/CANCEL alternation.

### Inbound radio path

```text
onWSM()
  -> filter messageType == 8
  -> parse signed wrapper
  -> verify P-256 signature
  -> ResdbOmnetDeliverPacket(handle, fromReplicaId, innerBytes)
  -> ServiceNetwork::InjectInboundPacket()
  -> normal ResDB ConsensusManager::Process() path
```

### Deterministic TYPE11 relay suppression

TYPE11 is a coverage relay for eligible PRE_PREPARE, PREPARE, and COMMIT bytes; it is no longer sent immediately by every first-time hearer. Each raw PBFT packet is keyed by type, view, sequence, original sender, and SHA-256 digest.

On first receipt, a non-origin replica arms one cancellable relay:

```text
carrier_target = max(1, min(type11RelayCarrierCap, f + 1))
rank = membership position rotated after the original PBFT sender
fire_time = first_receipt + type11RelayBaseDelaySec
            + rank * type11RelaySlotSec
```

The default carrier cap is `2`. This is a bounded forwarding-redundancy target, not a PBFT quorum. With `f >= 1`, the first two deterministic relays normally transmit and later ranked relays cancel after hearing two distinct authenticated TYPE11 carriers. A node across a coverage gap that hears fewer than the target retains its fallback timer.

Carrier observations count only after the outer signature verifies and its public key matches `fromReplicaId` in `WitnessKeyRegistry`. Pending relays also cancel when the relevant PRE_PREPARE/PREPARE progress has already reached the next PBFT-phase quorum, the packet becomes stale, the replica becomes inactive, or ORDER/CANCEL cleanup calls `clearConsensusRetries()`. Actual relay transmission uses `sendBFTMessageNow()` because the deterministic holdoff already supplied the scheduling slot; adding the generic random broadcast jitter again would defeat the ranking.

This is counter-based suppression with deterministic sender-relative ranking. It does not yet compute a perception-derived connected dominating set or geographic-progress relay priority.

### Self-injection

`OmnetReplicaCommunicator::SendMessage()` also self-injects broadcast bytes into the local `ServiceNetwork`.

This is required because real TCP deployments count a replica's own votes, while the Veins MAC path filters self-broadcasts. Without self-injection, a replica can miss its own PREPARE/COMMIT contribution and stall below the `2f + 1` quorum.

### PBFT silent mode

`ResdbOmnetSetPbftSilent()` toggles `OmnetReplicaCommunicator::SetPbftSilent()`.
When enabled, the node drops all outbound PBFT messages. This is an explicit
full-communicator fault; it is separate from proposal-only
`BYZANTINE_SILENT_PRIMARY`, which must retain complaint/view-change traffic so
followers can replace the silent proposer.

---

## 10. PRE_PREPARE Pre-Verify

The bridge registers a `SetPreVerifyFunc` lambda inside `ResdbOmnetCreateKvServer()`. This runs on every follower's ResDB worker thread for each incoming PRE_PREPARE, before the follower sends a PREPARE vote. If any check fails the PRE_PREPARE is rejected, which feeds into ResDB's built-in view-change machinery.

The ResDB consensus engine itself is **not modified**. `SetPreVerifyFunc` is an existing ResDB hook; all logic is in the bridge layer.

### Structural checks (1–8)

These verify the binary proposal is well-formed for this cluster:

1. Non-PRE_PREPARE requests pass through.
2. PRE_PREPARE data parses as `BatchUserRequest`.
3. Batch contains at least one user request.
4. Payload is large enough to hold `ResdbProposeHdr`.
5. For ordinary proposals, `hdr.n_vehicles` is in `(0, static_config_N]` so physically absent, departed, or late replicas can be omitted from the request-scoped epoch view. The legacy wrapped-rollback compatibility parser applies the same bound to its inner proposal.
6. Payload is large enough for all `ResdbVehicleEntry` records.
7. All `replica_id` values are unique.
8. All `replica_id` values are in `[0, expected)`.

Per-entry field sanity (part of the above gate): `sim_time_us ≠ 0` (UINT64_MAX is the QUIET sentinel), `is_ambulance ∈ {0,1}`, `cyber_status ∈ {0,1}`, `direction ∈ {0,1,2,3}`, and `leader_id ∈ [0, expected)`. Direction `3` is the signed UNKNOWN sentinel. The leader must also be a member of the request's proposed entry set.

For normal proposals, the bridge also enforces certified-candidate leadership before PBFT accepts the PRE_PREPARE:

1. The leader's own entry must be SIGNED, not QUIET.
2. In the initial view, `leader_id` must equal the smallest SIGNED static replica id in the proposal; if the follower's local snapshot contains a lower certified id, reject.
3. After authenticated view change, `leader_id` may instead equal the current PBFT primary.
4. In either case, the follower's local snapshot must contain validated arrival and stopped-distance evidence for `leader_id`. A proposal SIGNED bit alone cannot establish leader eligibility.

When those checks pass for an incoming normal PRE_PREPARE, the follower installs
`leader_id + 1` into PBFT `SystemInfo` for that request's view before ResDB's
primary-sender check runs. This keeps the frozen initial primary or authenticated
view-change successor, `ResdbProposeHdr.leader_id`, `ResdbOmnetGetPrimary()`, and
ResDB's 1-based PBFT primary aligned on every replica.

### Semantic checks (9–10) — cert-based

These run only when the app has registered a `ResdbCertSnapshotFn` via `ResdbOmnetSetCertSnapshotFn()`. The callback is called from the pre-verify thread (ResDB worker) under `certs_mutex_` and returns a snapshot of the follower's `collected_certs_` as `ResdbCertEntry` structs.

**Check 9 — cert-omission guard** (mirrors Java `OrderRequestVerifier` Check 7):

For every cert in the local snapshot, if the corresponding proposal entry has `cyber_status == 0` or `sim_time_us == UINT64_MAX` (QUIET), increment an omission counter. If `omitted > f` where `f = (N−1)/3`, reject. Up to `f` omissions are tolerated as plausible channel loss. A Byzantine leader must suppress at least `f+1` cars' certs to hide them; at least one honest follower in any quorum holds each of those certs and will see `omitted > f` and reject.

**Check 10 — state-field verification**:

For every SIGNED proposal entry (`cyber_status == 1`) whose cert the follower holds, the proposal's `lane`, `position_in_lane`, **derived scheduler-facing direction**, and `is_ambulance` must exactly match `ResdbCertEntry` from the local cert snapshot. The snapshot recomputes `eligibleDirection(cert)` rather than copying the declaration. Any mismatch—including a Byzantine leader's UNKNOWN-to-declared-direction upgrade—is rejected at threshold zero. The certificate is authenticated evidence, not perfect physical ground truth or proof of future execution.

Check 8 (deterministic schedule re-execution in Java) is not needed in C++ because the proposal contains only raw vehicle entries, no pre-computed schedule. The schedule is computed post-consensus by `IntersectionExecutor::ExecuteData()` identically on every replica. A Byzantine leader cannot submit a wrong schedule because the schedule is not part of the proposal.

### Pre-verify callback registration

In `initialize()`, after `ResdbOmnetSetOrderCallback()`:

```cpp
ResdbOmnetSetCertSnapshotFn(resdb_server_handle_,
                             &ResDBIntersectionApp::certSnapshotCallback, this);
```

`certSnapshotCallback` is a static method in `ResDBIntersectionApp`. It locks `certs_mutex_`, iterates `collected_certs_`, parses `"vehN"` → replica id, and fills the `ResdbCertEntry` buffer with cert-attested state. All writes to `collected_certs_` in `ResDBArrivalProtocol.cc` also lock `certs_mutex_` to protect against concurrent pre-verify reads from the ResDB worker thread.

### Request-scoped epoch-view install

Scenario 15 and Scenario 16 recovery ORDERs use the ordinary `ResdbProposeHdr + ResdbVehicleEntry[]` path. After structural, evidence, leader, and cert-backed state checks pass, the bridge builds the request's active view from the proposal entries and installs it in the PBFT active-view registry. Crash-recovery proposals additionally require a valid CLEAR evidence trailer before view installation.

Malformed or rejected proposals do not mutate the registry. A locally submitted proposal is first recorded as a pending view keyed by its request hash; when PRE_PREPARE assigns a PBFT sequence, the bridge promotes it to the full request identity. The installed view is scoped by epoch, sequence, and request hash where available, so it does not rewrite the static identity configuration or leak into unrelated requests.

The request membership is proposal-defined:

```text
M = { ResdbVehicleEntry.replica_id from the validated rollback payload }
```

ResDB does not infer `M` from PBFT traffic or whichever senders happen to respond. The application constructs it from the completed discovery round. `M` stores canonical OMNeT replica ids (`0..N-1`), matching `ResdbVehicleEntry.replica_id`. ResDB message sender ids are still 1-based node ids, so PBFT filtering converts `Request.sender_id()` to OMNeT id exactly once at the membership boundary.

The bridge still parses `ResdbRollbackHdr`/`ResdbRollbackEntry` for compatibility with older traces and callers. That wrapper is not the current Scenario 15/16 application hot path.

---

## 11. `proposeAll()` and Proposal Construction

Only the current cert-primary should submit a normal proposal:

```text
replicaId_ == CertPrimary()
```

`CertPrimary()` returns `-1` when no eligible cert exists; in that case `proposeAll()` returns without submitting. Before submitting, the app calls `ResdbOmnetSetPrimaryFromCert(handle, CertPrimary())` so the local PBFT primary matches the discovery primary. `proposeAll()` is gated on `DiscoveryState::COMPLETE`.

`proposeAll()`:

1. Stops cert rebroadcast retries.
2. Marks `propose_submitted_ = true`.
3. Records `ProposeAll_Submit_Time`.
4. Converts each collected eligible cert into a SIGNED `ResdbVehicleEntry`.
5. Adds QUIET entries only for locally observed eligible intents that lack certs at the hard deadline.
6. Submits the resulting view; it does not fabricate a missing arrival certificate or pad unobserved configured replica IDs. This is separate from the one permitted claimant self-attestation inside a real certificate.
7. Builds `ResdbProposeHdr + ResdbVehicleEntry[]`.
8. Applies Byzantine primary corruption if enabled.
9. Calls `ResdbOmnetTriggerConsensus()`.

For SIGNED entries, lane, queue-rank field, ambulance flag, and vehicle identity come from the stored cert and local vehicle state; direction is recomputed with `eligibleDirection(cert)` and may therefore be `3=UNKNOWN` even though the certificate retains a declared maneuver in `{0,1,2}`. For QUIET entries, the app uses locally observed lane/position if available and marks the cyber fields as unknown/quiet.

`ResdbOmnetTriggerConsensus()` wraps the proposal as a ResDB client request, then injects it into the local socketless `ServiceNetwork` as `TYPE_NEW_TXNS`.

---

## 12. `IntersectionExecutor` and Scheduling

`IntersectionExecutor` lives in `resdb_omnet_bridge.cc` and subclasses `resdb::TransactionManager`. Its `ExecuteData()` method is the ResDB-side smart contract for intersection scheduling.

Execution steps:

1. Parse `ResdbProposeHdr`.
2. Parse `ResdbVehicleEntry[]`.
3. Dump entries for diagnostics.
4. Build a work queue with ambulance-lane priority and lane-order constraints.
5. Repeatedly choose a schedulable head vehicle.
6. Grow the current batch with vehicles that are safe with every vehicle already in the batch.
7. Isolate QUIET entries as singleton batches; SIGNED-UNKNOWN and LEFT entries also become singletons through the unchanged safe-table lookup.
8. Emit `ResdbOrderHdr + ResdbVehicleDecision[]`.
9. Invoke the registered `ResdbOrderDecidedFn` callback.

### Safety rules in the executor

`IsSafeToBatch()` is a C++ port of the Java `ConflictMatrix` safe-pair table. Same-lane vehicles are never batched together. Specific opposite-straight, right-turn, and right-turn-plus-opposite-straight pairs are allowed.

`AllSameLaneFrontPlaced()` preserves physical queue order. If another vehicle is ahead of the candidate in the same lane and has not yet been placed in a batch, the candidate cannot be selected as the next head.

`IsQuietEntry()` returns true when:

```text
cyber_status == 0 || sim_time_us == UINT64_MAX
```

QUIET entries never join another vehicle's batch and never accept another vehicle into their own batch. They become singleton batches.

`IsSafeToBatch()` rejects same-approach pairs before consulting `kSafe`, so a
false `positionInLane` can affect same-lane work-queue order but cannot create a
conflicting co-batch. The safe table contains only STRAIGHT (`0`) and RIGHT
(`2`) entries. UNKNOWN (`3`) requires no executor change: as a batch head no
candidate can join it, and as a candidate `SafeWithWholeBatch()` fails. LEFT
(`1`) remains a conservative singleton for the same table-fallthrough reason.

### Ambulance behavior

The executor identifies ambulance vehicles and prioritizes vehicles blocking the ambulance's lane, followed by the ambulance itself. This flushes the ambulance lane while preserving front-of-lane ordering. Non-QUIET ambulance vehicles can still co-batch with safe partners from other lanes.

---

## 13. Order Callback and Vehicle Movement

The ResDB executor calls `ResdbOrderDecidedFn` from a ResDB worker thread. `ResDBIntersectionApp::onOrderDecided()` must not touch OMNeT APIs. It copies the bytes and pushes them into `pending_orders_`.

`transport_poll_msg_` runs every `transportPollInterval` and calls:

```text
drainOutboundQueue()
processOrders()
primary-change polling
```

`processOrders()`:

1. Swaps `pending_orders_` into a local deque.
2. Parses `ResdbOrderHdr`.
3. Parses `ResdbVehicleDecision[]`.
4. Logs `Order_Decided_Time`.
5. Cancels VC, stop-sign, and consensus timeout self-messages.
6. Finds this replica's `batch_index`.
7. Marks `order_applied_ = true`.
8. Triggers decision gossip if enabled.
9. Sets phase to `EXECUTING`.
10. If batch index is `0`, resumes immediately.
11. If batch index is greater than `0`, starts `preceding_batch_poll_msg_`.

### Clearance-gated batch movement

For batch `k > 0`, the app gathers all vehicles in batch `k - 1` into `preceding_batch_cars_`. The clearance poll checks:

```text
vehicleHasClearedIntersectionTraCI("veh" + precedingReplicaId)
```

The vehicle resumes when all preceding-batch cars have cleared. If `clearanceTimeoutSec` expires first, it logs a timeout and resumes as a safety fallback.

### Departure detection

During position updates, if the current phase is `EXECUTING` and the order has been applied, the app checks whether the local vehicle cleared the intersection. On success, it sets:

```text
current_phase_ = DEPARTED
cleared_time_ = simTime()
```

Departed vehicles ignore inbound V2V messages through the zombie filter in `onWSM()`.

---

## 14. Decision Gossip

Decision gossip is the new propagation layer for committed orders. Its job is to help a replica that missed enough type `8` PBFT traffic still learn the decided order from replicas that did commit.

### Sending

When `processOrders()` applies a committed order and `enableDecisionGossip` is true:

```text
triggerGossip(epoch, order_bytes)
```

`triggerGossip()`:

1. Stores `gossip_epoch_` and `gossip_order_bytes_`.
2. Serializes inner bytes as `epoch || order_bytes`.
3. Signs the inner bytes with `resdbwire::packSignedPacket()`.
4. Broadcasts a type `9` `BFTMessage`.
5. Schedules exponential retry through `gossip_timer_`.

Retries use:

```text
next_interval = decisionGossipInitialIntervalSec * (1 << gossip_retry_count_)
```

Retries stop when `decisionGossipMaxRetries` is reached or `stopGossip()` clears the order bytes.

### Receiving

`handleDecisionGossip()`:

1. Parses the signed wrapper.
2. Verifies the sender's P-256 signature over the inner gossip bytes.
3. Parses `epoch` and `order_bytes`.
4. If this replica already applied the same committed order and is not currently gossiping, it relays the order.
5. If no order is applied, it records the sender's vote in `GossipAccumulator`.
6. When `f + 1` distinct senders agree on identical `order_bytes` for the same epoch, it calls `applyGossipOrder()`.

`applyGossipOrder()` pushes the gossiped order bytes into the same `pending_orders_` queue used by the normal ResDB callback. This means gossiped decisions follow the same `processOrders()` path as PBFT-delivered decisions.

### Gossip threshold

The threshold is:

```text
f = (total_vehicles_ - 1) / 3
threshold = f + 1
```

This is intentionally lower than PBFT commit quorum. Gossip is not a replacement consensus protocol; it is a catch-up mechanism for a decision that has already been committed by at least one honest deciding replica. Requiring `f + 1` matching signed senders ensures at least one honest signer is included under the `N = 3f + 1` model.

---

## 15. CANCEL and Post-CANCEL Ordering

CANCEL is the committed invalidation path used when an already-committed order for epoch `e` becomes unsafe or stale before all scheduled vehicles have crossed. The first concrete triggers are:

1. a certified emergency vehicle arrives after epoch `e` committed and is absent from epoch `e`'s order; or
2. a crash / unsafe committed batch is detected after order delivery.

The design separates cancellation from the ordinary discovery that follows it:

```text
CANCEL_WITNESSING
  -> valid f+1 CANCEL certificate
  -> CANCEL_DRAINING
  -> CANCEL_CONSENSUS
  -> CANCEL_COMMITTED
  -> ordinary discovery for epoch e+1
  -> emergency: ordinary ORDER(e+1)
  -> crash: WAIT while BLOCKING -> f+1 CLEAR -> ordinary ORDER(e+1) with CLEAR evidence
```

### Ownership boundary

`ResDBRollbackProtocol.cc` owns the Veins-side CANCEL protocol and post-CANCEL proposal wrapper. It does not own discovery: `ResDBArrivalProtocol.cc` stores and validates arrival facts and runs the shared discovery state machine. CANCEL consumes those facts and decides when a committed order must be invalidated.

The bridge owns binary payload parsing, PRE_PREPARE pre-verify, executor dispatch, and rollback forced membership injection. It unwraps rollback proposals, validates the proposal-defined `M`, installs a request-scoped PBFT active view, and commits the same payload under quorum `N = |M|`. Static `server.config` remains an identity/address/key registry; it is not mutated or shrunk.

### Cancel echo and cancel cert

`CANCEL_ECHO` mirrors arrival echoes. A witness signs:

```text
cancelledEpoch:reason:reasonRef:echoingReplicaId
```

`reason` is:

```text
0 = CRASH
1 = EMERGENCY
```

`reasonRef` must be deterministic. For emergency rollback, it should identify the ambulance cert or ambulance vehicle/epoch. For crash rollback, it should identify the canonical unsafe condition, such as `unsafe_batch:e:b:vehA+vehB`.

`CANCEL_CERT` mirrors arrival certs:

1. at least `f + 1` distinct echo signers;
2. no duplicate signer ids;
3. every signer key matches its immutable `WitnessKeyRegistry` binding and belongs to the committed incident view;
4. every echo signature verifies;
5. every signed string matches the cert's `cancelledEpoch`, `reason`, `reasonRef`, and signer id.

Any replica that validates a `CANCEL_CERT` may relay it once. The cert already contains f+1 independent signatures, so relay does not need another gossip vote threshold.

The source's bounded CANCEL_CERT retry uses exponential evidence backoff and stops early after observing `f+1` distinct carrier IDs for the same semantic cancel key. Unlike CLEAR and TYPE11, type 13 does not currently wrap each relay in a carrier signature, so this carrier-count stop is transport-sender-aware but not yet cryptographically carrier-bound.

`CANCEL_CERT` closes witnessing, but it does not start the next discovery round. An emergency announcement/cert or crash observation may produce `CANCEL_ECHO`; a valid f+1 `CANCEL_CERT` moves replicas into `DRAINING`, and only a committed CANCEL starts discovery for `e+1`.

### CANCEL draining and deterministic primary

At `CANCEL_DRAINING`, every scheduled replica freezes the same schedule-derived election snapshot:

```text
active_batch = first committed batch containing a vehicle not yet clear
leader_candidates = sorted vehicles in the earliest waiting batch after active_batch
cancel_primary = leader_candidates[0]
cancel_electorate = all vehicles in the committed order
```

Clearance is polled through the centralized TraCI world view in simulation. A vehicle in the active executing batch is never eligible to lead CANCEL: it may continue crossing because it is non-recallable, or it may participate as a PBFT voter, but it cannot both execute the invalidated schedule and propose its cancellation. The frozen snapshot prevents the leader from changing merely because the active batch clears during the drain.

The drain duration covers the configured per-replica slot horizon plus jitter. Existing staggered witness traffic may reach the NIC, but new discovery traffic is blocked. At the drain deadline the frozen primary submits CANCEL. App-level proposer rotation exists as a bounded retry mechanism; full forced-membership CANCEL view-change/new-view remains unimplemented.

### Fast local halt

Once a replica receives valid cancel evidence, halt is local and immediate for recallable vehicles. A vehicle is recallable when it is still far enough from the conflict box to brake before entry:

```text
recallable = distanceToConflictBox >
             speed^2 / (2 * brakingDecelMps2) + processingLatencyMargin
```

Recallable vehicles cancel pending resume / clearance timers and call the existing TraCI stop helper. Non-recallable vehicles are already physically committed and are not force-stopped; post-CANCEL discovery excludes their local intent.

While `cancel_pending_` is true, the normal simulation fallback meaning inverts:

```text
stopSignTimeoutSec / consensusTimeoutSec / clearanceTimeoutSec
    means stay stopped, not resume
```

This keeps a vehicle that heard cancel evidence from crossing just because an app-level safety timer expired.

### Discovery and ORDER after CANCEL

After CANCEL commits, recallable vehicles start the same `COLLECTING -> DRAINING_CERTS -> COMPLETE` discovery state machine used initially, with timers armed immediately because they are already in the stop zone. Old arrival certs remain epoch-bound and do not authorize the new order. The fresh membership `M` is the responsive intent view:

```text
M = certified/observed responsive vehicles for epoch e+1
    excluding departed, wrecked, and non-recallable old-epoch vehicles
    including the new certified ambulance/emergency vehicle
```

The post-CANCEL ORDER uses the ordinary proposal format. The active epoch membership is the proposal's entry set; the already committed CANCEL/tombstone is not repeated in a `ResdbRollbackHdr` on the current application hot path:

```text
ResdbProposeHdr
ResdbVehicleEntry[ResdbProposeHdr.n_vehicles]
ResdbOrderEvidenceHdr + length-prefixed CLEAR_CERT bytes  // crash recovery only
```

The bridge derives the request-scoped epoch view from `ResdbVehicleEntry[].replica_id`, validates it, and installs PREPARE/COMMIT sender admission, quorum, and primary for that exact request. The legacy `ResdbRollbackHdr` parser remains in the bridge for compatibility, but Scenario 15 and Scenario 16 both log `normal_proposeAll=1` and do not use it.

Completion uses the normal stabilization rule from Section 8: the intent view must be stable for `discoveryIntentSettleSec` and fully certified, or the shared hard `cert_collection_timeout_` deadline closes it. There is no rollback-specific discovery timeout or expected epoch-0 membership count. At the deadline, only observed missing intents become QUIET.

This keeps post-CANCEL membership compatible with future perception-engine changes: if perception later changes who is visible, only the Veins-side discovery result changes, not the ResDB request-scoped epoch-view rules.

### Deterministic post-CANCEL ORDER proposer

The post-CANCEL ORDER proposer is derived from the epoch `e+1` discovery candidates. The same `shouldIncludeInRollbackMembership()` filter is used for proposer selection and proposal construction, so the app-level proposer is a member of the proposal-defined `M`:

```text
smallest replica id in the sorted rollback membership candidates,
rotated by rollback_rotation_index_ on retry
```

The selected proposer is encoded as `ResdbProposeHdr.leader_id`. When the bridge validates the rollback proposal, that leader becomes the PBFT primary for the rollback instance. `ResdbOmnetGetPrimary()`, PBFT `SystemInfo`, and the app-level proposer therefore agree on the same 0-based replica id for epoch `e+1`.

Byzantine-primary recovery for the proposal-defined epoch view is the next phase of work. The honest Scenario 15/16 paths are complete; do not infer that a deterministic app-level proposer rotation is equivalent to a fully implemented PBFT VIEW_CHANGE/NEW_VIEW for every dynamic epoch view.

### BLOCKED, WAIT, and CLEAR crash recovery

Crash recovery uses one incident per cancelled epoch and executing batch:

```text
BlockedIncident { cancelledEpoch, executingBatch }
IncidentState = BLOCKING | CLEARED
```

A vehicle continuously stationary inside the conflict box for `crashDwellSec` causes each surviving witness to emit at most one crash `CANCEL_ECHO` for the canonical `blocked_batch:e:b` statement. A valid `f+1` crash certificate both supplies the CANCEL justification and registers the batch incident as BLOCKING. The incident remains separate from the singleton CANCEL state so later CLEAR validation still has an authoritative subject.

After CANCEL commits, the existing clearance poll is explicitly rearmed. This is required because the halt path cancels that timer, while both crash dwell and empty-box dwell run on it. Recovery discovery can complete while the wreck remains, but `trySubmitRollbackProposal()` refuses to propose while any incident for the cancelled epoch is BLOCKING.

WAIT (type 17) is sent only by the ordinary epoch-`e+1` `CertPrimary()` after discovery is COMPLETE and the incident remains BLOCKING. Receivers require a registry-bound leader signature, committed CANCEL tombstone, matching local BLOCKING incident, locally matching cert-primary, increasing heartbeat index, bounded lease, and valid simulated timestamps. WAIT only reschedules the local leader-suspicion deferral. CLEAR and ORDER always supersede it; WAIT never enters the bridge or PBFT.

CLEAR uses type 15 witness echoes and type 16 certificates. A valid certificate requires `f+1` committed-view witnesses with registry-bound keys and matching incident statements. CLEAR transmission is deliberately quiet:

1. Once a node assembles a candidate certificate, it stops accumulating/logging further echoes for that incident.
2. Candidate broadcasters are ranked with `CertPrimary()` first and remaining active members in deterministic order; rank `i` waits `i * clearCertCandidateSlotSec`.
3. Receiving a valid earlier carrier cancels later candidates, marks the incident CLEARED, stops WAIT and CLEAR retries, and triggers the gated ORDER attempt.
4. Type 16's outer signature is rebound by every relay. `AuthenticatedPropagationTracker` counts distinct valid active-view carriers under `CLEAR:cancelledEpoch:executingBatch` and stops relaying at `f+1` or sooner when ORDER is proposed/applied.
5. `handleClearEcho()` hushes late echoes before accumulation when the incident is CLEARED, a valid cert is known, or a candidate is already assembled.

Crash-recovery ORDER appends a `ResdbOrderEvidenceHdr` and at least one length-prefixed raw CLEAR certificate. The bridge tracks epochs created by committed `CANCEL_CRASH` and rejects PRE_PREPARE for their recovery ORDER when the trailer is absent or invalid. Validation is delegated through `ResdbOmnetSetClearEvidenceCallback()` to the Veins certificate validator; the bridge does not implement a weaker second validator. A replica that missed type 16 can adopt the same CLEAR evidence atomically while applying ORDER.

In the latest successful honest 16-vehicle crash log, anchored counts show one initial CLEAR certificate, five follow-up CLEAR relays (the `f=5` fallback bound), no CLEAR-echo accumulation above the six-witness threshold, 11 hush events, 11 propagation-stop events, WAIT termination at all 14 survivors, and 14 epoch-1 commit callbacks. Deterministic TYPE11 suppression reduced TYPE11 sends from the earlier 978-frame baseline to 237, with 1,101 pending relays cancelled before transmission. Treat raw counts as lower bounds when log-line interleaving corruption is present (Section 22).

### Tombstones and gossip suppression

When CANCEL for `cancelled_epoch=e` commits, the app immediately records a tombstone for `e`; it does not wait for ORDER(e+1).

Tombstoned epochs must be refused by:

1. `handleDecisionGossip()` before counting type 9 votes;
2. `applyGossipOrder()` before enqueueing the gossiped order;
3. `processOrders()` before applying queued order bytes.

This prevents a missed or delayed type 9 gossip frame from resurrecting the cancelled order after rollback.

### Request-scoped epoch active view

Post-CANCEL ordering does not call runtime reconfiguration and does not rewrite ResDB `server.config`. `omnet_forced_view.h` provides a request-scoped active-view registry for ordinary epoch proposals:

| Concern | Current behavior |
|---------|------------------|
| Active membership storage | `OmnetForcedViewRegistry` is injected into PBFT managers. It stores epoch views keyed by request identity: epoch, sequence, and request hash when available. |
| Membership source | The only source is the validated ordinary proposal's `ResdbVehicleEntry[].replica_id` list. ResDB does not infer active membership from traffic or live perception. |
| Id basis | `M` stores OMNeT 0-based replica ids. PBFT `Request.sender_id()` is converted from ResDB 1-based node id at the filter/counting boundary. |
| Quorum | PREPARE/COMMIT use `floor((|M| + f) / 2) + 1`, the smallest quorum whose two intersections contain more than `f` replicas. Per-epoch mode clamps configured `f` to `floor((|M|-1)/3)`. This equals `2f+1` when `|M|=3f+1`; Scenario 16 uses `|M|=14`, `f=4`, quorum `10`. |
| Primary | The validated `leader_id` is installed in PBFT `SystemInfo`; `ResdbOmnetGetPrimary()` returns that epoch primary. |
| Sender admission | PRE_PREPARE/PREPARE/COMMIT senders outside `M` are non-voting and dropped/logged before collector vote bits are counted. |
| Non-members | Non-members are passive/non-voting. They may still learn tombstones or final recovery knowledge so stale decision gossip cannot resurrect epoch `e`. |
| Scope | The dynamic `N` applies only to the request identity. Static config remains the provisioned identity/key universe. |
| Executor consistency | `IntersectionExecutor` schedules exactly the same `M` that PBFT committed under the request-scoped quorum. |

Checkpoint/recovery and complete dynamic-view VIEW_CHANGE/NEW_VIEW support remain follow-up work. The Veins ordinary VC trigger is suppressed while `cancel_pending_` is waiting on BLOCKING/CLEAR so a static-view change cannot be mistaken for recovery progress; WAIT provides bounded local deferral during that interval.

### Scenario 15: late emergency rollback test

The first orchestrated rollback experiment is scenario code `15`, named `Emergency_Preempt_DynamicN`.

It is currently hard-scoped to `N=18`:

```bash
python3 experiment_orchestrator.py --config 18 --scenario 15 --reps 1
```

The orchestrator expands scenario `15` to:

```bash
fourway/run-resdb-simulation.sh ... --rollback-late-emergency
```

with OMNeT++ config `EighteenVehiclesResDB`.

The first Byzantine protocol-surface pair reuses this exact physical harness:

```bash
# Unguarded: valid emergency evidence exists, but the selected CANCEL leader
# withholds the proposal and no follower owns a replacement lease.
python3 experiment_orchestrator.py --config 18 --scenario 17 --reps 1

# Guarded: the same round-0 leader withholds; all honest replicas' leases
# advance to the next deterministic proposer.
python3 experiment_orchestrator.py --config 18 --scenario 18 --reps 1
```

Both rows inject the fault by protocol role after the committed schedule identifies the round-0 CANCEL proposer. This avoids assuming that a fixed replica id will always occupy the next recallable batch. Scenario 17 disables `enableCancelLeaderFailover`; Scenario 18 leaves it enabled. The analyzer records the attacked replica, rotation-timer firings, maximum logical rotation index, staged outcome, and attack-to-CANCEL-commit latency. The expected contrast is `suppressed_cancel_stalled` versus a completed epoch-1 recovery.

`--rollback-late-emergency` writes `fourway/rollback_late_emergency.ini` at run time and selects:

```text
*.manager.launchConfig = xmldoc("resdb_bft_18veh_rollback_late.launchd.xml")
*.manager.intersectionBatchSize = 16
*.manager.enableR0Supervisor = true
*.manager.r0SpawnAfterCleared = 1
*.node[*].appl.totalVehicles = 16
*.node[*].appl.toleratedFaults = 5
*.node[*].appl.ambulanceReplicaId = 17
*.node[*].appl.enableRollback = true
*.node[*].appl.enableAmbulanceCertGate = true
*.node[*].appl.rollbackFaultMode = "per_epoch"
```

The scenario manager—not a fixed late route departure—injects `veh16` as the late normal car and `veh17` as the ambulance after one committed vehicle has cleared. Both identities are pre-provisioned in the 18-entry ResDB configuration; this is physical late spawn, not runtime key enrollment.

`totalVehicles = 16` is the critical parameter, even though 18 replicas exist.
`proposeAll()` uses `totalVehicles` as the initial eligibility boundary. If it were 18, the late identities could be treated as epoch-0 candidates, and
`maybeTriggerEmergencyRollbackFromCert()` (which skips when the ambulance id is
already in `committed_order_vehicle_ids_`) would never fire. Keeping it at 16
means epoch 0 commits `{0..15}`, so the late ambulance is genuinely unscheduled and triggers rollback. The late arrivals `veh16`/`veh17` still
receive correct replica ids: the SUMO identity binding only overrides ids
`< totalVehicles`, so they fall back to the NED `node[16]/node[17].replicaId`
values (valid because they spawn last, so node index equals veh number).
`intersectionBatchSize = 16` lets epoch 0 commit the 16 present vehicles.

The ResDB bridge pre-verify (`resdb_omnet_bridge.cc`) uses `expected =
server.config` replica count (18). Normal (non-rollback) proposals are accepted
when `0 < hdr.n_vehicles <= expected`, so epoch 0 with 16 signed entries passes
even though 18 keys exist. Rebuild the bridge after changing this check:
`bazel build //integration/omnet:resdb_omnet_bridge`.

The intended timeline is:

1. Static ResDB starts with config `N=18` (18 pre-generated keysets).
2. The first physical wave has 16 vehicles (`veh0..veh15`) at the intersection.
3. The initial epoch `e` commits a normal order over `M0 = {0..15}`; replicas 16 and 17 stay silent (16 present >= quorum `2f+1 = 11`).
4. Some vehicles begin crossing and at least two leave or become non-recallable.
5. The manager injects `veh16` as a late normal vehicle and `veh17` as the emergency vehicle.
6. Vehicles that can verify the emergency reason send `CANCEL_ECHO`.
7. `f+1` cancel echoes form a `CANCEL_CERT`.
8. Recallable vehicles halt, tombstone epoch `e`, restart discovery for epoch `e+1`, and propose rollback membership `M`.
9. Ordinary epoch-`e+1` consensus commits under request-scoped `N=|M|`, not static `18`.

This scenario was added because a tiny `N=4` rollback case is structurally weak: after vehicles leave, too few may remain to demonstrate a meaningful recovery quorum. The `N=18` identity universe commits 16 vehicles first, then admits two pre-keyed late arrivals under manager control. This is not runtime ResDB identity enrollment.

If scenario `15` does not trigger rollback:

- Inspect the manager's `[R0-*]` injection/supervisor logs and `r0SpawnAfterCleared` condition rather than editing a late route departure time.
- Confirm `totalVehicles=16` and `ambulanceReplicaId=17`; configurations using ambulance 16 describe an older harness.
- `fourway/analyze_log.py --scenario 15` has dedicated CANCEL/rollback/view parsing and reports a staged failure reason such as `no_echo`, `no_cert`, `rollback_propose_no_commit`, or `ok_epoch1_commit`. Some marker names retain the legacy `ROLLBACK`/`ACTIVE-VIEW` terminology.

### Scenario 16: crash, WAIT, CLEAR, and recovery ORDER

Scenario code `16`, `Crash_Wait_Clear`, is hard-scoped to config 16:

```bash
python3 experiment_orchestrator.py --config 16 --scenario 16 --reps 1
```

The runner expands this to `--crash-wait-clear` and generates overrides under `[Config SixteenVehiclesResDB]`: crash supervision enabled, two wrecks, a 0.2-second post-freeze MAC grace, a 20-second tow delay, `totalVehicles=16`, `toleratedFaults=5`, no ambulance, and rollback enabled.

The intended membership/quorum sequence is:

```text
ORDER(0): N=16, f=5, quorum=11
BLOCKED/CLEAR evidence: f+1=6 committed-view witnesses
CANCEL(0): committed 16-member view, quorum=11
ORDER(1): 14 responsive vehicles, f=4, quorum=10
```

The manager freezes two executing batch-0 vehicles and disables their app/PBFT communications after the bounded MAC grace. Survivors form one batch-scoped BLOCKED certificate and commit CANCEL. Epoch-1 discovery completes among the survivors; WAIT keeps the recovery leader from being falsely suspected while the wrecks remain. After tow plus empty-box dwell, ranked CLEAR dissemination ends WAIT and unlocks ordinary ORDER(1) with embedded CLEAR evidence.

### Departed replica lifecycle

Departure has both physical and logical effects:

1. `recordIntersectionDeparture()` marks the vehicle `DEPARTED` / `is_departed_`.
2. It calls `ResdbOmnetMarkReplicaInactive(handle, replicaId, current_epoch_ + 1)`.
3. The bridge sets PBFT silent mode for that local replica and logs `[ACTIVE-DEPART]`.
4. `enqueueOutbound()` drops late ResDB worker-thread sends after departure, and pending outbound packets are cleared.
5. `finish()` calls the same inactive marker before final server stop/destroy, acting as the cleanup safety net.

The static config may still contain the departed id, but future rollback active memberships must treat that vehicle as gone unless a later proposal explicitly includes it and it is physically responsive.

---

## 16. Simulated Time

OMNeT++ simulated time is the source of truth. The bridge provides:

```c
int ResdbOmnetUpdateSimTimeUs(void* handle, int64_t now_us);
```

`ResDBIntersectionApp` schedules `time_tick_msg_` every `timeTickInterval` and calls:

```text
ResdbOmnetUpdateSimTimeUs(handle, simTime().inUnit(SIMTIME_US))
```

ResDB uses `SimTimeProvider`:

```cpp
SimTimeProvider::NowUs()
SimTimeProvider::UpdateNowUs(now_us)
SimTimeProvider::SleepUntilUs(deadline_us)
SimTimeProvider::SleepForUs(delta_us)
```

`UpdateNowUs()` wakes condition-variable waiters even when the supplied time does not advance, which helps worker threads waiting for "any tick" behavior.

During teardown, the app updates sim time to `INT64_MAX` / max uint time to unblock any ResDB worker sleeping on simulated deadlines before stopping the server thread.

---

## 17. View Change and Primary Failure

ResDB's built-in PBFT view-change is the intended replacement for the old BFT-SMaRt `RequestsTimer` / STOP / STOP_NACK stack.

Normal cert-primary selection, request-scoped recovery membership, and PBFT view-change are related but distinct. At proposal time, the app installs the selected proposal leader into local PBFT state with `ResdbOmnetSetPrimaryFromCert()`. On followers, bridge pre-verify repeats the leader check and installs the incoming proposal's `leader_id` into PBFT `SystemInfo` for that PRE_PREPARE view before ResDB verifies that PRE_PREPARE came from the current primary.

The bridge exposes:

```c
int ResdbOmnetSetVcTimeoutUs(void* handle, int64_t timeout_us);
int ResdbOmnetForceViewChange(void* handle);
int ResdbOmnetGetPrimary(void* handle);
int ResdbOmnetSetPrimaryFromCert(void* handle, int primary_omnet);
int ResdbOmnetSetPbftSilent(void* handle, int silent);
```

### App-level VC trigger

A follower does not time out the leader merely because it entered the stop zone. It arms `vc_trigger_msg_` for `pbftVcTimeoutSec` only after its own discovery reaches `COMPLETE`. A new eligible ANN received before proposal submission reopens discovery and cancels that timer.

When this fires, the app calls `ResdbOmnetForceViewChange()`. The bridge calls `ViewChangeManager::TriggerViewChangeNow()` through `OmnetConsensusManagerPBFT::TriggerViewChange()`.

This app-level trigger is a safety valve for simulation progress and Byzantine-primary experiments. It does not replace PBFT correctness; it pushes the ResDB view-change machinery to run.

Before forcing view change, the handler calls `processOrders()`. This closes the race where ResDB has already executed and enqueued an order through `onOrderDecided()`, but the simulation thread has not yet applied it and set `order_applied_`.

### VC timer cancellation on delivery

Once an order is either queued or applied, the app cancels and deletes the follower VC trigger instead of rearming it. Merely hearing PRE_PREPARE, PREPARE, or COMMIT does not complete discovery or cancel discovery traffic.

The cancellation rule is:

```text
if order_applied_ == true or pending_orders_ is non-empty:
    cancel vc_trigger_msg_
else:
    rearm vc_trigger_msg_ for pbftVcTimeoutSec
```

This prevents late PBFT/control traffic after an `[EXECUTOR] OrderDecision` from keeping a stale app-level VC timer alive and forcing a view change after consensus is already over.

After a primary change, every non-primary replica that still has complete
discovery evidence and no pending/applied order rearms the one-shot suspicion
timer. This is necessary for consecutive faulty primaries: the timer consumed
while replacing `r0` cannot also detect a silent `r1`. The next timeout advances
the view again; the validated corner case reaches honest `r2` after
`r0 -> r1 -> r2`.

### Primary change polling

Every transport poll, the app checks:

```text
current_primary = ResdbOmnetGetPrimary(handle)
```

If the primary changed, the app first checks the frozen jointly certified
candidate set. An uncertified successor logs `[CERT-PRIMARY-SKIP]` and triggers
the next authenticated PBFT view change; it does not propose. If the new primary
is this replica, is in the certified candidate set, and no order has been
applied/submitted, the app calls `proposeAll()` under PBFT-view-change authority.
Bridge preverification repeats the local certificate-backed leader check before
any follower votes.

### Silent primary mode

`BYZANTINE_SILENT_PRIMARY` suppresses `proposeAll()` while leaving PBFT
complaint and view-change traffic enabled. Full communicator silence is a
separate, explicit `byzantinePbftSilent` fault used only when the experiment
intends to mute every outbound PBFT message. Keeping these controls separate
lets the primary-silence experiment test recovery rather than accidentally
preventing the faulty primary from participating in its own replacement.

### Bad proposal mode

`BYZANTINE_BAD_PROPOSAL` corrupts `hdr.n_vehicles` before calling `ResdbOmnetTriggerConsensus()`. Followers reject the malformed PRE_PREPARE in the bridge pre-verify path.

---

## 18. Byzantine Fault Model

The system assumes:

```text
N = 3f + 1
quorum = floor((N + f) / 2) + 1
arrival cert threshold = f + 1
decision gossip threshold = f + 1
```

For the common `N=3f+1` case, the quorum formula reduces to `2f+1`. Keeping the intersection formula explicit matters for recovery views whose responsive membership is not exactly `3f+1`; for example, Scenario 16's 14-member epoch-1 view uses `f=4`, quorum `10`, while BLOCKED/CLEAR evidence remains anchored to the cancelled 16-member view and therefore requires 6 witnesses.

Examples:

| N | f | PBFT quorum | Cert/gossip threshold |
|---|---|-------------|-----------------------|
| 4 | 1 | 3 | 2 |
| 12 | 3 | 7 | 4 |
| 14 | 4 | 10 | 5 |
| 16 | 5 | 11 | 6 |

### Current fault injection modes

| `byzantineType` | Name | Behavior | Defense / expected effect |
|-----------------|------|----------|---------------------------|
| `0` | Honest | Normal behavior. | Not a fault. |
| `1` | `FALSE_LANE` | Vehicle claims a fake lane in `ARRIVAL_ANNOUNCE`. | Honest witnesses fail TraCI verification and refuse to echo. Vehicle becomes QUIET if it cannot assemble f+1 echoes. |
| `2` | `INVALID_SIG` | Vehicle corrupts echo signatures with garbage bytes. | `validateArrivalCert()` rejects those echoes. With enough honest witnesses, other cars still collect f+1 valid echoes. |
| `3` | `EQUIVOCATOR` | Vehicle sends one signed LEFT byte variant to one peer subset and one signed RIGHT byte variant to the other. | Each honest witness locks its first `(target,epoch)` claim hash, rejects later variants before perception/echo, and signatures from different hashes cannot be merged. Either variant may still independently reach f+1. |
| `4` | `SILENT_PRIMARY` | Primary suppresses app proposal but retains PBFT/view-change communication. | Followers' VC triggers force ResDB view-change; normal reproposal still must pass the cert-primary rule. Full communicator muting is configured separately. |
| `5` | `BAD_PROPOSAL` | Primary corrupts proposal shape. | Bridge pre-verify rejects PRE_PREPARE; view-change path should recover. |
| `6` | `FAKE_AMBULANCE` | Primary flips `is_ambulance` 0→1 for the first non-ambulance entry in the proposal. | Pre-verify Check 10 catches the `is_ambulance` mismatch vs cert. Without the firewall (`RESDB_NO_FIREWALL=1`), the fake car receives ambulance crossing priority. |
| `7` | `FAKE_AMBULANCE_FOLLOWER` | Follower injects `isAmbulance=true` with empty cert bytes into its own `ARRIVAL_ANNOUNCE`. | When `enableAmbulanceCertGate=true`, the echo path rejects uncertified ambulance claims. With the cert gate off, honest echoes accept the claim and the wrong car gets priority. |
| `8` | `TAMPER_LANE` | Primary disguises the front E-lane car as S-lane (`position=0`) in the proposal so the scheduler sees N-STRAIGHT + "S"-STRAIGHT as safe to co-batch. The N car (going south) and the E car (going west) are released simultaneously and cross in the intersection center. | Pre-verify Check 10 catches the cert-lane vs proposal-lane mismatch. Without the firewall, the unsafe order is committed and `[CRASH_DETECTED]` is logged post-consensus by each replica. |
| `9` | `UPGRADE_UNKNOWN_DIRECTION` | Certified proposer changes a derived `UNKNOWN` direction to `STRAIGHT`. | Check 10 compares against the locally recomputed certificate snapshot, rejects the upgrade, and recovery commits `UNKNOWN`. |
| `10` | `TAMPER_DISTANCE_RANK` | Certified proposer changes a certificate-derived queue rank. | Check 10 rejects the position mismatch and recovery commits the certified rank. |
| `11` | `TAMPER_PHYSICAL_LANE` | Certified proposer changes `physical_lane_index` and `lateral_claim_cm`. | Check 10 rejects both certified-state mismatches and recovery commits the original physical authority. |
| `12` | `SUPPRESS_CERTS` | Certified proposer marks exactly `f+1` other certified entries QUIET while leaving its own entry SIGNED. | Keeping the proposer certified isolates Check 9: it rejects the `f+1` omissions, then recovery commits the locally certified entries as SIGNED. |

### Proposal-integrity recovery and certificate availability

Checks 9 and 10 are detection mechanisms, not in-place repair mechanisms. A
proposal that fails either check receives no honest PREPARE quorum. Progress
therefore requires PBFT view change, followed by a new proposal assembled from
the successor's own authenticated certificate snapshot. The successor logs an
atomic `[PROPOSER-CERT-STATE]` record containing its authority, certificate
count, and certificate IDs; the E--H validation requires a non-Byzantine
successor whose snapshot covers the deposed proposer's certified set. It also
requires the correct certificate-derived values to commit after recovery.

Certificate-relay withholding is independent of proposal mutation. The J
experiment configures exactly `f` replicas to store valid certificates but
withhold both certificate-forwarding paths. Origin broadcasts, PBFT, and honest
relays remain enabled. Success requires every honest replica to reach the full
16-certificate set through honest-to-honest paths, then commit and depart.

The consecutive-primary corner configures proposal-only silence at `r0` and
`r1`. Honest replicas rearm suspicion after the first primary change, advance
through `r0 -> r1 -> r2`, and accept only an `r2` proposal backed by all 16
certificates. This demonstrates recovery within `f+1` primary attempts for the
tested two-fault prefix; it is not a dynamic-membership claim.

Equivocation uses a one-variant-per-`(witness,target,epoch)` invariant. An
honest witness may support its first authenticated variant but never evaluates
or signs a second one. Consequently, one variant may independently certify;
the validated claim is that honest witnesses do not double-sign and two
conflicting variants do not both produce an unsafe committed schedule.

The full binary-integrity suite runs D, E, F, G, H, J, and the consecutive-
primary corner strictly sequentially over multiple seed indices. F deliberately
uses `signal_error=1.0` to force the `SIGNED-UNKNOWN` state targeted by the
UNKNOWN-upgrade mutation; other rows use the locked `0.2` operating point.
The consistent-liar case (declare RIGHT, cue RIGHT, execute STRAIGHT) remains a
post-commit conformance-monitoring limitation rather than a certification-gate
defense claim.

### Experiment scenario wrappers

The orchestrator scenario names combine a traffic condition, a Byzantine role, and a targeted defense ablation:

| Orchestrator scenario | Fault mode | Targeted ablation | Failure marker |
|-----------------------|------------|-------------------|----------------|
| `NoFW_ByzFollower_FalseLane` | `FALSE_LANE` follower input lie | Firewall-off output path for comparison; TraCI input verification is the actual defense. | `[CONSENSUS_ATTACK_OUTCOME] fault=FALSE_LANE ...`; success is `MALICIOUS_INPUT_COMMITTED`, blocked cases are `BLOCKED_NO_VALID_CERT` or `BLOCKED_OR_CANONICALIZED`. |
| `NoFW_ByzLeader_BadProposal` | `BAD_PROPOSAL` leader structural corruption | `RESDB_NO_FIREWALL=1` disables bridge pre-verify checks. | `[CONSENSUS_ATTACK_OUTCOME] fault=BAD_PROPOSAL outcome=ORDER_COMMITTED_AFTER_MALFORMED_PROPOSAL`. |
| `NoFW_ByzLeader_FakeAmbulance` | `FAKE_AMBULANCE` leader state-field tampering | `RESDB_NO_FIREWALL=1` disables Check 10. | `[CONSENSUS_ATTACK_OUTCOME] fault=FAKE_AMBULANCE outcome=FALSE_PRIORITY_GRANTED`; also logs `[FALSE_PRIORITY_GRANTED]`. |
| `NoCertGate_ByzFollower_FakeAmbu` | `FAKE_AMBULANCE_FOLLOWER` input lie | `enableAmbulanceCertGate=false`. | `[CONSENSUS_ATTACK_OUTCOME] fault=FAKE_AMBULANCE_FOLLOWER outcome=UNCERTIFIED_PRIORITY_CLAIM_COMMITTED`. |
| `CertGate_ByzFollower_FakeAmbu` | `FAKE_AMBULANCE_FOLLOWER` input lie | `enableAmbulanceCertGate=true`. | `[CONSENSUS_ATTACK_OUTCOME] fault=FAKE_AMBULANCE_FOLLOWER outcome=CERT_GATE_BLOCKED_OR_NOT_CERTIFIED`; echo path also logs `[CERT-GATE]`. |
| `NoFW_ByzLeader_TamperLane` | `TAMPER_LANE` leader state-field tampering | `RESDB_NO_FIREWALL=1` disables Check 10. | `[CONSENSUS_ATTACK_OUTCOME] fault=TAMPER_LANE outcome=UNSAFE_ORDER_COMMITTED`; also logs `[CRASH_DETECTED]`. |

### No-firewall ablation

Setting `RESDB_NO_FIREWALL=1` at run time replaces all 10 pre-verify checks with a trivially-true lambda. This allows Byzantine proposals that Check 10 would normally reject to be committed. Scenarios 7–9 and 12 use this mode to demonstrate what each check prevents.

### Current defenses

| Threat | Current defense |
|--------|-----------------|
| False physical lane | TraCI verification before echo. |
| Forged arrival cert | f+1 distinct ECDSA echo signatures checked in `validateArrivalCert()`. |
| Malformed binary proposal | Bridge pre-verify structural checks 1–8. |
| Byzantine leader QUIET suppression | Pre-verify Check 9: reject if > f certs held by follower are marked QUIET. |
| Byzantine leader state-field tampering | Pre-verify Check 10: proposal entry fields must match local cert for every SIGNED car. |
| Unsafe ambulance claim by follower | `enableAmbulanceCertGate` NED flag: echo path rejects uncertified `isAmbulance=true` announce (cert gate ablation toggle). |
| Unsafe co-batching | Executor `IsSafeToBatch()` conflict matrix port. |
| Same-lane rear crossing before front | Executor `AllSameLaneFrontPlaced()`. |
| Missing cert at proposal deadline | QUIET padding and singleton batch isolation. |
| Isolated announce source | Type 10 announce gossip carries the original signed announce through verified witnesses. |
| Missed PBFT decision | Type 9 decision gossip with f+1 matching signed senders. |
| Primary silence | PBFT silent fault plus app-level forced view-change, canceled once an order is pending or applied. |
| Post-commit unsafe batch (firewall-off ablation) | `detectUnsafeBatch()` checks every committed batch against cert lanes using the same kSafe table as the executor. Logs `[CRASH_DETECTED]` when any batch pair is unsafe under true cert lanes. |

---

## 19. Important NED Parameters

Defined in `ResDBIntersectionApp.ned`.

### Identity and ResDB config

| Parameter | Meaning |
|-----------|---------|
| `replicaId` | Initial replica id. May be overridden from SUMO external id. |
| `resdbConfigFile` | Explicit ResDB `server.config`. |
| `resdbPrivateKeyFile` | Explicit ResDB private key path. |
| `resdbCertFile` | Explicit ResDB cert path. |
| `resdbLogDir` | Log directory. |
| `resdbCryptoDir` | If set, auto-fills config/key/cert/log paths. |

### Radio and timing

| Parameter | Meaning |
|-----------|---------|
| `useRadioTransport` | Use Veins radio-backed transport instead of logging transport. |
| `transportPollInterval` | Self-message interval for draining outbound PBFT and processing orders. |
| `timeTickInterval` | Sim-time update cadence for ResDB. |
| `viewJitterMin`, `viewJitterMax` | Jitter for arrival echo/cert style traffic. |
| `viewAgreementSlotSec` | Per-replica echo slot. |
| `broadcastJitterMin`, `broadcastJitterMax` | Broadcast jitter. |
| `broadcastSlotSec` | Per-replica slot for generic broadcasts, including type 8. |
| `type11RelayCarrierCap` | Maximum authenticated TYPE11 carriers required to suppress a pending relay. Live target is `min(cap, f+1)`; default `2`. |
| `type11RelayBaseDelaySec` | Deterministic minimum TYPE11 holdoff before the earliest ranked fallback; default `0.02s`. |
| `type11RelaySlotSec` | Deterministic spacing between sender-relative TYPE11 relay ranks; default `0.02s`. |

### Cert collection and proposal

| Parameter | Meaning |
|-----------|---------|
| `triggerJoinTimeSec` | Initial arrival announcement start time. |
| `arrivalSlotSec` | Per-replica stagger for announcements and some cert sends. |
| `broadcastArrivalAnnouncementIntervalSec` | Periodic re-announcement interval. |
| `certCollectionTimeoutSec` | Hard discovery deadline, armed on stop-zone entry. Observed intents still missing certs become QUIET when the deadline closes the view. |
| `discoveryIntentSettleSec` | Required interval with no newly accepted eligible intent before normal discovery completion; default `1.5s`. |
| `enableArrivalCertRetries` | Enables repeated type 5 cert broadcasts. |
| `arrivalCertRetryIntervalSec` | Cert retry interval. |
| `arrivalCertRetryMax` | Number of extra cert retries; `0` means unlimited until stopped by another condition. |
| `directionEligibilityCollectionWindowSec` | One-shot passive collection window after the first `f+1` echoes; default `0.25s`. Discovery drain finalizes immediately when the threshold is already met. |

### Imperfect perception and Phase 2 evidence

| Parameter | Meaning |
|-----------|---------|
| `approachSigmaM` | Geometry-calibrated approach-noise operating point. The runner maps it to a checked-in 4x4 categorical confusion matrix. |
| `approachConfusionMatrix` | Row-major approach confusion matrix over `N,S,E,W`. Runtime sampling uses this matrix rather than a symmetric unitless lane-error channel. |
| `signalObservationError` | Categorical maneuver-cue corruption probability in `[0,1]`. It changes signed cue support but never vetoes lane-qualified echo admission. |
| `perceptionRngIndex` | Module-local RNG stream used only by `ResDBPerception`; generated overrides map stream 1 to global RNG 1. Zero-error observations consume no draw. |
| `enablePhase2ControlledCue` | Mixed-fixture-only deterministic TraCI signal control during the ingress/discovery window. Native SUMO blinkers remain characterization data because they appeared too late for the one-verdict echo window. |
| `enablePhase2CueTrace` | Emits observation-only route, native signal, derived cue, cue-source, and echo-window characterization records. |
| `phase2AttackKind` | Experiment-only `NONE`, `WRONG_APPROACH` (E2), or `FALSE_DIRECTION` (E4). The legacy `X` fault scenario is unchanged. |
| `phase2AttackTargetReplicaId` | Claimant whose authenticated declaration is changed for E2/E4; pilots use `veh0`. |
| `phase2EvidenceColluderIds` | Deterministic comma-separated external colluder set. Colluders remain honest for their own claims and sign false support only for the target. |
| `phase2ActualByzantineCount` | Total configured Byzantine evidence identities, including the target. Analysis uses certificate-local `b_sig`, not this nominal count, in probability predictions. |

### Crossing behavior

| Parameter | Meaning |
|-----------|---------|
| `stopDistance` | Stop-zone trigger distance to lane end. Code multiplies this by `totalVehicles / 2`. |
| `totalVehicles` | Number of vehicles/replicas expected in the scenario. |
| `cruiseSpeedMps` | Speed applied on resume. |
| `precedingBatchPollPeriodSec` | Poll interval while waiting for previous batch to clear (also drives Scenario 16 crash-dwell perception). |
| `clearanceTimeoutSec` | Safety timeout for clearance wait. |
| `stopSignTimeoutSec` | App-level safety release if no order arrives. |
| `consensusTimeoutSec` | Longer app-level fallback timeout. |

### Faults, gossip, and view change

| Parameter | Meaning |
|-----------|---------|
| `isByzantine` | Enables Byzantine behavior for this vehicle. |
| `byzantineType` | Fault type `0` through `8`. See Byzantine fault model table. |
| `enableAmbulanceCertGate` | When true, the arrival-echo path rejects any `isAmbulance=true` announcement without a valid Emergency_CA `VehicleCert` and payload signature. Legitimate ambulances (`ambulanceReplicaId`) auto-issue the cert at init and attach it in `broadcastArrivalAnnouncement()` using the ported legacy ambulance-cert helper. Ablation toggle for cert-gate scenarios (types 10 and 11). |
| `enableDecisionGossip` | Enables shared gossip/relay machinery, including types 9–11. |
| `decisionGossipInitialIntervalSec` | First retry interval for gossip. |
| `decisionGossipMaxRetries` | Maximum gossip retries. |
| `pbftVcTimeoutSec` | Extra wait before app-level forced view-change. |
| `intendedDirection` | `S`, `L`, or `R`, used in announcements. |
| `intendedLane` | Optional `N/S/E/W` override to avoid SUMO lane-name ambiguity. |

### CANCEL and post-CANCEL ordering

| Parameter | Meaning |
|-----------|---------|
| `enableRollback` | Enables CANCEL echo/cert handling, local halt, CANCEL consensus, post-CANCEL discovery, and tombstone filtering. |
| `cancelCertRetryIntervalSec` | Legacy compatibility setting; evidence retry timing now uses the exponential-backoff parameters below. |
| `cancelCertRetryMax` | Maximum cancel cert retry count; `0` means unlimited while cancel remains pending. |
| `evidenceRetryBaseSec`, `evidenceRetryFactor`, `evidenceRetryCapSec` | Fast-start capped exponential backoff for CANCEL/CLEAR evidence retry; defaults `0.1s`, `2.0`, `2.0s`. |
| `cancelGossipRetryBaseSec`, `cancelGossipRetryCapSec` | Capped exponential retry timing for CANCEL-commit gossip. |
| `consensusRetryIntervalSec` | Interval for bounded PRE_PREPARE/PREPARE/COMMIT radio retransmission. |
| `consensusRetryMax` | Maximum retransmissions retained for each local PBFT phase packet. |
| `clearDwellSec` | Required continuously empty conflict-box interval before a witness emits CLEAR. |
| `clearCertCandidateSlotSec` | Deterministic candidate/relay fallback slot for type 16; default `0.1s`. |
| `waitHeartbeatIntervalSec` | Leader WAIT heartbeat cadence while recovery discovery is COMPLETE but the incident remains BLOCKING. |
| `waitHeartbeatMaxDeferralSec` | Maximum lease extension accepted from one WAIT heartbeat. |
| `waitClockSkewSec` | Allowed simulated timestamp skew for WAIT validation. |
| `brakingDecelMps2` | Deceleration used for recallable / committed horizon calculation. |
| `processingLatencyMargin` | Extra distance margin added to the braking horizon. |
| `rollbackVcTimeoutSec` | App-level timeout before rotating to the next deterministic rollback proposer. |
| `injectSuppressInitialCancelLeader` | Experiment-only fault: the deterministic round-0 CANCEL proposer withholds the proposal after accepting valid evidence. |
| `enableCancelLeaderFailover` | Guard/ablation switch. When enabled, every replica accepting the CANCEL certificate arms the same non-extending proposer lease; expiry advances the deterministic candidate index. |

---

## 20. Metrics and Logs

Common log markers used by benchmark scripts and debugging:

| Marker | Meaning |
|--------|---------|
| `[METRICS r] Stop_Time` | Vehicle entered stop zone. |
| `[CERT-PRIMARY]` | Cert-primary election or PBFT primary alignment. Includes no-cert waits, follower skips, and bridge/app primary installs. |
| `[METRICS r] ProposeAll_Submit_Time` | Cert-primary submitted binary proposal to ResDB. |
| `[METRICS r] Order_Decided_Time` | App processed a committed/gossiped order. |
| `[METRICS r] Batch_Assignment` | Local vehicle's decided batch index. |
| `[METRICS r] Resume_Time` | Vehicle resumed movement. |
| `[CAR-METRICS]` | Per-car wait/departure summary. |
| `[ANN-RECV]` | Arrival announcement received. Includes `via=direct` or `via=gossip`; gossiped messages also log the carrier replica. |
| `[PERC-EVAL]` | The single cached external perception evaluation for the first authenticated claim variant at `(witness,target,epoch)`, including its `claimHash`, true/claimed/observed approach, and derived cue. The analyzer rejects exact duplicates and cross-variant evaluations. |
| `[EQUIVOCATION-DETECTED]` | A witness received a second origin-authenticated hash for the same target and epoch. Records both hashes/directions and confirms the second variant received no perception evaluation or echo. |
| `[SELF-ATTEST]` | Claimant's one local signed echo. It consumes no perception draw and produces no Type-4 transmission. |
| `[CERT-COLLECT]`, `[CERT-ASSEMBLE]` | First-threshold/finalize timing and the final distinct echo count, including why the one-shot collection window closed. |
| `[CERT-EVIDENCE]` | Per-signature certificate accounting: signer, cue, self flag, Byzantine flag, and whether the cue supports the declaration. |
| `[DIR-ELIGIBILITY]` | Certificate-local positive support, threshold, echo count, self-attestation count, measured `b_sig`, and derived scheduler direction. |
| `[TRUST-TIER]` | Final QUIET, SIGNED-UNKNOWN, or SIGNED-DIRECTION classification used by experiment analysis. |
| `[PHASE2-ATTACK-CONFIG]`, `[PHASE2-ATTACK-DECLARE]`, `[PHASE2-COLLUSION-ECHO]`, `[PHASE2-ATTACK-OUTCOME]` | Authenticated E2/E4 setup, forged declaration, colluder support, and false-certificate/false-eligibility outcome. |
| `[MOVEMENT-GROUND-TRUTH]`, `[CONFLICT-ZONE-ENTER]`, `[CONFLICT-ZONE-EXIT]`, `[CONFLICT-COOCCUPANCY]` | Physical metrology derived from actual SUMO ingress/egress and the checked-in 12x12 movement table. Claimed lane/direction are not oracle inputs. |
| `[DISCOVERY-BEGIN]`, `[DISCOVERY-VIEW]` | Discovery round start and newly accepted intent. |
| `[DISCOVERY-DEADLINE]`, `[DISCOVERY-DRAIN]`, `[DISCOVERY-COMPLETE]` | Hard deadline, certificate drain, and local completion, including intent/cert counts, missing IDs, and local-CERT-air state. |
| `[TYPE8-DRAIN]` | Outbound signed PBFT bytes sent to radio. |
| `[TYPE8-RECV]` | Inbound signed PBFT bytes verified and delivered. |
| `[TYPE11-RELAY-ARM]` | Deterministic TYPE11 fallback armed with carrier target, sender-relative rank, and fire time. |
| `[TYPE11-CARRIER]` | Newly observed registry-bound TYPE11 carrier for one semantic raw PBFT packet. |
| `[TYPE11-RELAY-CANCEL]`, `[TYPE11-PROPAGATION-STOP]` | Pending TYPE11 relay canceled by enough carriers, phase quorum, stale/inactive state, or consensus cleanup. |
| `[TYPE11-SEND]` | A deterministic fallback actually reached its timer and relayed. |
| `[PBFT-RETRY-ARM]`, `[PBFT-RETRY]`, `[PBFT-RETRY-STOP]` | Bounded local phase retransmission lifecycle. |
| `[SEQ-SYNC]` | A newly selected primary advanced its PBFT sequence allocator past its locally executed prefix. |
| `[GOSSIP-SEND]` | Type 9 order gossip broadcast. |
| `[GOSSIP-RECV]` | Type 9 order gossip vote received. |
| `[GOSSIP-APPLY]` | Replica applied an order through gossip catch-up. |
| `[ANN-GOSSIP-SEND]` | Type 10 announce gossip broadcast; `reason=verified` for immediate relay, `reason=stop-zone` for delayed custody relay. |
| `[ANN-GOSSIP-RECV]` | Type 10 announce gossip received, parsed, and handed to the normal announcement path. |
| `[CANCEL-ECHO]` | Type 12 cancel echo sent or received. |
| `[CANCEL-CERT]` | Type 13 cancel cert assembled, broadcast, received, or rejected. |
| `[CANCEL-RELAY]` | Valid cancel cert relayed once by a receiver. |
| `[CANCEL-DRAIN]`, `[CANCEL-CONSENSUS]` | Frozen active batch/candidate election and transition into CANCEL PBFT. |
| `[HALT-LOCAL]` | Local halt decision on valid cancel evidence, including recallable classification. |
| `[ROLLBACK-BEGIN]` | Legacy-named marker showing that committed CANCEL started the ordinary discovery round for `new_epoch`. |
| `[ROLLBACK-PROPOSE]` | Deterministic rollback proposer submitted or skipped a rollback proposal. Includes `|M|` when submitted. |
| `[ROLLBACK-COMMIT]` | Rollback order committed and cancelled epoch tombstoned. |
| `[ROLLBACK-VC]` | App-level rollback proposer rotation timer fired. |
| `[CANCEL-LEADER-LEASE]` | A replica armed the shared CANCEL proposer lease. Repeated evidence never extends an already scheduled lease. |
| `[BYZANTINE-CANCEL-SUPPRESS]` | Experiment fault injected at the deterministic round-0 CANCEL proposer. Includes attack time and failover setting. |
| `[ACTIVE-VIEW]` | ResDB installed or promoted a request-scoped active view. Includes epoch, seq, hash, `N`, `f`, quorum, primary, and members. Older log text may call this a forced rollback view. |
| `[EPOCH-VIEW]`, `[EPOCH-VIEW-REJECT]` | Ordinary epoch proposal active-view candidate/install or rejection, including CLEAR-evidence rejection for crash recovery. |
| `[ACTIVE-VIEW-REJECT]` | A request-scoped membership was rejected, usually for conflicting membership or sender/leader mismatch. |
| `[ACTIVE-VOTE-DROP]` | PBFT ignored a PRE_PREPARE/PREPARE/COMMIT vote because the sender is outside the request's `M`. |
| `[ACTIVE-PASSIVE]` | A non-member observed request-scoped PBFT traffic but skipped voting or execution for that instance. |
| `[ACTIVE-DEPART]` | A local vehicle departed and its ResDB/PBFT participation was disabled for future epochs. |
| `[ROLLBACK-VC-UNSUPPORTED]` | A dynamic epoch-view view-change/new-view path was reached; the prototype rejects it instead of silently falling back to static `N`. |
| `[CLEAR-CERT-CANDIDATE]`, `[CLEAR-CERT-CANDIDATE-CANCEL]` | Ranked CLEAR broadcaster/relay fallback lifecycle. |
| `[CLEAR-CARRIER]`, `[CLEAR-PROPAGATION-STOP]` | Authenticated type-16 carrier progress and relay suppression. |
| `[CLEAR-ECHO-HUSH]` | Late CLEAR echo ignored before accumulation because a candidate/cert/CLEARED state already exists. |
| `[WAIT-SEND]`, `[WAIT-ACCEPT]`, `[WAIT-REJECT]`, `[WAIT-STOP]` | Advisory WAIT lease lifecycle; these are app-level logs, not PBFT decisions. |
| `[VC-DEBUG]`, `[VC-TRIGGER]`, `[APP-VC]` | View-change instrumentation. |
| `[OMNET-PREVERIFY]` | Bridge PRE_PREPARE pre-verify result. |
| `[EXECUTOR]`, `[EXEC-CB]` | ResDB executor and order callback logs. |
| `[CRASH_DETECTED]` | Emitted by `detectUnsafeBatch()` after order delivery. Identifies the batch index, both vehicle IDs, and their cert lanes. Fires on every honest replica that processes the committed order. Indicates a Byzantine leader committed an order that the firewall (Check 10) would have rejected. |
| `[FALSE_PRIORITY_GRANTED]` | Emitted after order delivery when a leader-tampered fake ambulance proposal commits despite the local cert saying the vehicle is not an ambulance. |
| `[CONSENSUS_ATTACK_OUTCOME]` | Unified post-order detector emitted by `detectConsensusAttackOutcome()`. Includes `fault=...` and `outcome=...` so experiment scripts can mark attack success or blocked recovery across scenarios. |
| `[BYZANTINE]` | Byzantine primary or follower fault injection. Includes fault type name and affected replica IDs. |

---

## 21. Build and Run Handoff

When changing the bridge, rebuild ResDB first, then Veins. Use the checked-in
environment wrapper; do not build ResDB or Veins from a bare host shell.

Typical sequence:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh makeres
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh makeveins
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'cd fourway && make'
```

Use the scenario configs in `fourway/omnetpp.ini` for honest, ambulance, batch, Byzantine follower, and Byzantine primary experiments.

Phase 2 validation and pilots are orchestrator-owned presets. All current
Phase 2 experiments use `N=16` (`f=5`, evidence threshold `6`):

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-fixture-validation
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-self-attestation-validation
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-attack-validation
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-metrology-validation
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-pilots --phase2-pilot-profile grid
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-pilots --phase2-pilot-profile full
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --two-lane-validation
```

The grid profile is one repetition per E1-E4 parameter cell (88 sequential,
resumable runs). The full profile has 384 unique cells/repetitions and reuses
the completed grid `run_0` artifacts. Only the full profile supports shoulder
and prediction-versus-empirical claims.

### Imperfect-perception validation status (2026-08-15)

The checked summaries report PASS for Phase 1, the N=16 mixed fixture (2A),
self-attestation and Check 10 recovery (2B), authenticated E2/E4 attacks (2C),
actual-movement metrology/calibration (2D), and all 88 runs in the one-seed
grid. The 384-run `full` Phase 2 pilot profile now passes with all 384 runs
completed and `theory_consistent=true`. The separate N=16 longitudinal grid
also passes all 186 runs.

The paper-facing interpretation is prediction and calibration, not simply
"noise causes errors": empirical false-certification/false-eligibility
intervals are compared with a Binomial tail parameterized by honest
opportunities and support actually available to each attempt. For longitudinal
certificate formation the predictor uses `b_sig_available`: the claimant's
self-attestation plus distinct Byzantine supporting echoes actually collected.
`b_sig_cert` remains a separate forensic count from finalized certificate
bytes. Using `b_sig_cert=0` for an attempt that failed to finalize would
condition the theory input on the predicted outcome. Configured Byzantine count
also cannot substitute for `b_sig_available`, because the fixed collection
window may close before every configured colluder contributes.

### Orchestrator scenario codes (`experiment_orchestrator.py --scenario N`)

| Code | Name | Description |
|------|------|-------------|
| 1 | `No_Ambulance_Honest` | Baseline: no ambulance, all honest. |
| 2 | `Honest_Ambulance` | Ambulance present, all honest. |
| 3 | `ByzFollower_Ambulance` | f Byzantine followers, ambulance present. |
| 4 | `ByzLeader_Ambulance` | Byzantine primary (type 5), ambulance present. |
| 5 | `ByzLeader_NoAmbulance` | Byzantine primary (type 5), no ambulance. |
| 6 | `ByzFollower_NoAmbulance` | f Byzantine followers, no ambulance. |
| 7 | `NoFW_ByzFollower_FalseLane` | FALSE_LANE follower, firewall off — attack succeeds. |
| 8 | `NoFW_ByzLeader_BadProposal` | BAD_PROPOSAL primary, firewall off — attack succeeds. |
| 9 | `NoFW_ByzLeader_FakeAmbulance` | FAKE_AMBULANCE primary (type 6), firewall off — wrong priority committed. |
| 10 | `NoCertGate_ByzFollower_FakeAmbu` | FAKE_AMBULANCE_FOLLOWER (type 7), cert gate OFF — uncertified ambulance claim accepted. |
| 11 | `CertGate_ByzFollower_FakeAmbu` | FAKE_AMBULANCE_FOLLOWER (type 7), cert gate ON — claim rejected, correct priority. |
| 12 | `NoFW_ByzLeader_TamperLane` | TAMPER_LANE (type 8), firewall off — N+E co-batched, `[CRASH_DETECTED]` fires. Run with firewall on to confirm Check 10 prevents it. |
| 13 | `FW_ByzLeader_FakeAmbulance` | FAKE_AMBULANCE primary with the proposal firewall enabled. |
| 14 | `FW_ByzLeader_TamperLane` | TAMPER_LANE primary with the proposal firewall enabled. |
| 15 | `Emergency_Preempt_DynamicN` | Honest emergency rollback: committed CANCEL, fresh discovery, and epoch-1 ORDER over responsive membership. |
| 16 | `Crash_Wait_Clear` | Honest crash recovery: BLOCKING incident, advisory WAIT, signed CLEAR, and evidence-carrying epoch-1 ORDER. |
| 17 | `Emergency_SuppressCancel_Unguarded` | Scenario 15 with the round-0 CANCEL leader suppressing preemption and proposer failover disabled. Expected liveness harm: valid evidence exists but CANCEL does not commit. |
| 18 | `Emergency_SuppressCancel_Guarded` | Same suppression attack with all-replica deterministic proposer leases enabled. The next candidate should commit CANCEL and recovery ORDER. |

---

## 22. Known Limitations and Technical Debt

### Imperfect-perception experiment status

The completed Phase 1/2 cardinal baseline remains `K=1` with independent
categorical N/S/E/W errors. Continuous lateral evidence is deliberately not
forced onto cardinal approach substitution: realistic position noise around
orthogonal N/E roads does not represent an adjacent-lane boundary. A dedicated
`SixteenVehiclesAdjacentLaneResDB` fixture instead uses the same scalar
lane-normal residual rule as the standalone adjacent-lane calibration. The
runtime derives the normal from TraCI lane-0/lane-1 centerlines after coordinate
conversion, quantizes observations and signed claims to centimetres, and accepts
when `abs(u_obs-u_claim) <= k*sigma_lat`. Authenticated arrival evidence binds
both `lateralClaimCm` and the derived physical-lane index.

The active post-Phase-2 path is therefore a two-stage physical-evidence
protocol: an early cardinal-or-adjacent-lane/cue certificate while approaching,
followed by a separate stopped-distance certificate bound to the early claim
hash. Ordinary single-lane cardinal fixtures use physical lane index `0`.
The focused adjacent-lane validation passes 4/4: zero-noise lane centres are
preserved; `b=f` with zero noise is blocked; the fixed `b=f` shoulder requires
one honest noisy acceptance; and the `b=f+1` cliff is attributed to the six
Byzantine signatures actually present in the certificate. The statistical
adjacent-lane grid remains pending.

The focused sequential regression command is:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --adjacent-lane-validation
```

The former 310-run straight-road matrix was stopped after three development
cells and is calibration-only. It is replaced by the full four-way two-lane net
and `two_lane_route_manifest.csv`. Lane 0 is outer/T and permits STRAIGHT or
RIGHT; lane 1 is inner/L and permits LEFT. The N=16 fixture covers all twelve
movements with two vehicles in each `(approach,physicalLaneIndex)` queue.

The runtime derives a lane-normal frame from the target vehicle's inbound lane
pair for every perception evaluation. This target-relative frame is necessary:
using the witness's own cached normal is valid on the old one-road fixture but
wrong across orthogonal approaches. Direction eligibility now additionally
requires that the certified physical lane authorize the declaration. Failure
produces SIGNED-UNKNOWN and the existing singleton behavior; the executor and
`kSafe` table are unchanged.

`ResdbVehicleEntry` and `ResdbCertEntry` carry the certified physical-lane index
and centimetre lateral claim. Proposal packing and `certSnapshotCallback`
populate the same values, and Check 10 rejects any mismatch as well as a derived
direction upgrade. The mechanically reviewed hazard pair is actual `N-L + S-S`
(conflicting) versus claimed `N-R + S-S` (allowed by `kSafe`).

Checkpoint 1 calibration and the standalone N=16 route validation pass. The
first honest zero-error protocol smoke also passes with 16/16 lane-authorized
certificates, zero perception RNG draws, no Check 10 rejection, no physical
collision/teleport, and all vehicles departed.

The sequential `--two-lane-validation` checkpoint now passes 5/5. Its `b=f`,
zero-noise guard leaves the attacked target QUIET; its `b=f`,
`sigma_lat=.5 m` shoulder forms with certificate-local `b_sig=5` plus honest
support; and its `b=f+1`, zero-noise cliff forms from the six Byzantine
signatures actually present. The honest `sigma_long=1 m` run partitions rank
comparisons by `(approach,physicalLaneIndex)`. Byzantine fault type `11`
mutates the certified physical-lane/lateral fields, is rejected by Check 10,
and recovers through an honest view. The report is
`benchmarks/Phase2TwoLaneValidation/two_lane_validation_summary.json`.

The deterministic conflict-release smoke now closes the physical chain. With
certificate-local `b_sig=6`, false physical-lane evidence authorizes the actual
North-left target as claimed North-right; the unchanged `kSafe` rule places it
in committed batch 0 with the South-straight counterpart; SUMO ground truth
classifies the actual movements as conflicting; and the pair overlaps in the
conflict zone. The run pins `speedMode=0`, `jmIgnoreFoeProb=1`, junction
collision checking, `collision.action=none`, and teleport disabled. Its report
is `benchmarks/Phase2TwoLaneValidation/conflict_release_cliff/run_0/two_lane_conflict_release_validation.json`.

The unlocked statistical replacement is launched with
`python3 experiment_orchestrator.py --two-lane-grid` (prefix
`ORCHESTRATOR_SKIP_OMNET_SOURCE=1` inside an already activated environment).
It contains 400 exact, resumable, strictly sequential cells on the full
two-lane intersection and uses the conflict-release route ordering in every
cell. Aggregation reports false-certification and actual conflicting
co-occupancy Wilson intervals. The `b=0` cell is explicitly an honest control
with effective offset zero and reports `q1`; only `b>=1` false-claim cells
report `q0`. Theory uses collection-time `b_sig_attempt` in
`r=max(0,f+1-b_sig_attempt)`, while finalized-byte `b_sig_cert` is retained for
forensics. `[ECHO-COLLECT]` makes the former observable even when no certificate
forms. The retired straight-road artifacts are excluded.
The controlled-fixture validation is strict: false certificate, reviewed
co-batch, and physical co-occupancy must agree end to end. Co-occupancy without
a false certificate, or a false certificate without the expected physical
consequence, fails the cell and remains separately classified for diagnosis.
The Byzantine-boundary slice uses 20 repetitions per off-headline `b` (same
seed budget as the δ / k / σ panels); the prior 5-rep scout is not a publishable
b-panel.
The replacement statistical matrix is therefore unlocked, with sigma axis
`{0,.25,.5,.75,1,1.5,2,3}`.

The stopped-distance stage uses one signed centimetre-quantized claim after the
target is stationary, one cached noisy longitudinal verdict per witness, a
bounded retransmission of identical Type-18 bytes, `f+1` distinct signers, and
a compact Type-20 certificate. The origin sends the finalized Type-20 once and
queues one byte-identical reliability copy in the existing cancellable discovery
queue; this adds no timer, sampling, or certificate variant, and discovery drain
waits for the copy. Proposal packing sorts certified distance within each
approach/lane group and maps that order to the scheduler's unchanged rank byte.
Missing distance evidence leaves the entry QUIET. PBFT, Check 10, kSafe,
direction eligibility, and executor behavior remain unchanged.

The generic authenticated longitudinal-offset attack is wired with separate
attempt-local `b_sig_available` prediction accounting, certificate-local
`b_sig_cert` forensic accounting, and actual-vs-certified same-lane ordering
metrology. Its N=16 `b=f+1` boundary smoke has 16/16 stopped-distance
certificates, zero QUIET entries, and the intended ordering inversion; the
complete Phase 1 regression suite remains green. Byzantine fault type `10`
mutates only the proposal's certificate-derived queue rank; the focused
`--distance-rank-check10-validation` regression confirms Check 10 rejects the
position mismatch, view change occurs, and an honest order commits. Continuous
lateral claim certification and the focused N=16 adjacent-lane fixture are now
wired and their four-cell validation preset passes. The full-intersection
two-lane five-cell protocol validation also passes, including boundary,
rank-noise, and physical-field Check 10 recovery. Conflict-release fixture
ordering and the statistical grid remain active work.
The full N=16 longitudinal measured-`h` grid and
nonzero statistical calibration are complete. Later work is deliberately
separated from this checkpoint:

Operating-point selection also has an attack-free, completion-capable N=16
two-lane preset. `--two-lane-honest-operating-sweep smoke|k-full|full` crosses
`k={1,2,3}` with `sigma_lat={0,.1,.3,.5,.7,1,1.5,2}` while fixing the honest
claim displacement to its only meaningful value, `delta=0`. It reports global
witness `q1`, lane-certification/QUIET outcomes, throughput, batch size, and
wait. The one-repetition 24-cell smoke is required first. `k-full` then runs
only the 60 operating-point-selection runs at `sigma_lat=.5`; the optional
480-run full surface reuses those artifacts and fills the other sigma cells.
Selection candidates are reported at `sigma_lat=.5`, but no runtime default is
changed automatically.

The `b=f+1` ordering inversion is treated only as an end-to-end Byzantine-cliff
smoke. Longitudinal shoulder claims use `b=f`, where certificate
success requires honest noisy support. Raw two-observation inversions use
`Phi(-s/(sqrt(2)*sigma_long))`; committed certificate-derived pair inversions
remain a separate metric governed by the per-target gate and attempt-local
`b_sig_available`/measured-`h` composition.

Unlike lateral and longitudinal evidence, direction is not modeled as a graded
physical state whose false-acceptance shoulder proves future maneuver intent.
It is a committed declaration whose cue evidence only unlocks co-batching.
Accordingly, direction is evaluated by a safe-throughput ablation: eligibility
ON, eligibility OFF, and co-batching OFF. The analyzer already separates QUIET,
SIGNED-UNKNOWN, and LEFT/table-forced singleton causes. The eligibility-off
mode is implemented with `enableDirectionEligibility=false`; co-batching-off is
implemented with `RESDB_ALL_SINGLETON=1`.

The headline fixture is N=16 and straight-heavy: every approach has `2S/1L/1R`
(`8S/4L/4R` overall). LEFT is not present in `kSafe` and therefore remains a
singleton by fallthrough. Four checked-in assignment variants keep
`veh0=N-L` and `veh1=S-S` fixed while rotating the remaining maneuver identities.
Repetition `r` selects fixture `r mod 4` before perception, then shares that
fixture and the paired seed across all six honest/`FALSE_DIRECTION` policy
rows. The old `4S/8L/4R` full result is retained as LEFT-heavy sensitivity data.

The honest eligibility-OFF prerequisite passes with 16 vehicles in 11 batches
(`mean_batch_size=1.45`), all departures, and no unsafe co-occupancy. The new
six-row straight-heavy smoke also passes. Its single-repetition observations
are: honest ON/OFF/singleton mean batch sizes `1.08/1.45/1.00`; false-direction
ON/OFF/singleton mean batch sizes `1.07/1.45/1.00`. Only the false-direction
eligibility-OFF row produces the reviewed conflicting co-occupancy. These are
wiring results, not final confidence estimates. The full profile runs 20 paired
repetitions per row, strictly sequentially:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-straight-heavy-prerequisite
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-straight-heavy
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-straight-heavy-full
```

The multiscale two-lane fixture checkpoint uses N=`4,8,16,20`, balances every
approach, and preserves a `50% LEFT / 25% STRAIGHT / 25% RIGHT` maneuver mix.
All scales use the same lane policy and reviewed `veh0=N-L`, `veh1=S-S`
conflict pair. The honest operating-point smoke runs strictly sequentially:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-scale-smoke
```

The four-cell smoke passes with all vehicles certified and departed, no unsafe
conflicting co-occupancy, no SUMO collision or teleport, and no committed rank
inversion. Byzantine consensus roles are intentionally not inferred from an
evidence-lying claimant; leader/follower attack rows are added only after their
threat-model roles are named explicitly.

The corrected longitudinal fixture uses generated config
`SixteenVehiclesDistanceGridResDB`, which extends `SixteenVehiclesResDB`. This
is required because another `launchConfig` assignment in the parent section
does not supersede the parent's first matching parameter. The 4-cell smoke
passes with requested `s=5 m` measured near `5.0021 m`, a committed inversion
at `b=f`, the expected `b=f+1` cliff, and a clean `s=12.5 m` tail. The full
186-run grid now passes. At `b=f`, `s=5 m`, `sigma_long=1 m`, the observed
committed inversion rate is `4/20=0.20` versus `0.1799` predicted; the
`s=5.5 m` point is `1/20=0.05` versus `0.0452`. Cached offline re-thresholding
at `k={2,2.5,3}` produces false-certificate outcomes `{0,2,4}/20` and honest
true-accept rates `{0.961,0.988,0.998}`. The run and analysis-only commands are:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --longitudinal-grid --longitudinal-grid-profile full

python3 experiment_orchestrator.py \
  --longitudinal-grid-reanalyze --longitudinal-grid-profile full
```

The longitudinal preset honors `--physical-gate-k`. Nonlegacy values use a
separate artifact root (`Phase2DistanceGridK<k>/fixture_v3`) and exact metadata
matching, so an executed `k=2` run cannot silently resume the prior `k=3`
grid.

- E5 repeated sampling over `K in {1,3,5}` and `tau_lane`, using the existing
  per-target sample cache;
- perception-gate-off, direction-eligibility-off, firewall-off, and
  all-singleton ablations, plus fixed-time TLS/all-way-stop performance anchors;
- E6 measurement-only conformance for a consistent claim/cue followed by a
  different executed maneuver;
- one correlated/common-mode error episode to expose the limits of the
  independent Binomial model;
- final paper-scale repetitions and plotting after pilot confidence intervals
  are inspected; and
- richer sensing such as occlusion, latency, correlated pose errors,
  asymmetric signal channels, Bayesian direction distributions, or
  CARLA/hardware-calibrated channels.

CANCEL/tow mitigation for E6, repeated live intersection rounds, and complete
dynamic-membership recovery are not part of this perception experiment. They
remain blocked on the multi-epoch reset and membership/view-change limitations
below.

### General repeated multi-epoch reset remains incomplete

Scenario 15 and 16 implement the intended CANCEL-to-epoch-`e+1` transition, including fresh discovery, tombstones, incident state, and request-scoped membership. A long-lived scenario with multiple independent cancellation/recovery cycles still lacks full `resetForNextRound()` parity and needs a dedicated soak test.

### `ResdbOmnetRemoveReplica()` is a stub; inactive marking is local

The bridge function currently returns success for non-null handles but does not remove or reconfigure a replica. The implemented departure path instead uses `ResdbOmnetMarkReplicaInactive(handle, replica_id, min_epoch)`, which disables local PBFT outbound participation and prevents late outbound radio sends after physical departure. This is not static-config reconfiguration; the original config remains an identity registry.

### Request-scoped dynamic epoch-view scope

Recovery PREPARE/COMMIT uses proposal-defined `M`: quorum is computed from `|M|`, non-members are non-voting, and the proposal `leader_id` is installed as the PBFT primary for that request. This covers the intended reduced-membership recovery without mutating `server.config`. Scenario 15 and 16 use ordinary proposals; the wrapped rollback format remains compatibility code only.

Remaining dynamic-membership limitations:

- View-change/new-view over a request-scoped dynamic epoch view is not implemented. Reaching that path logs `[ROLLBACK-VC-UNSUPPORTED]` and fails loud.
- Checkpoint/recovery dynamic membership is deferred; experiments should not rely on checkpoint/recovery during the recovery ORDER instance.
- Full emergency membership-corroboration Check 12 remains follow-up work. Emergency certificate validation itself is enforced when `enableAmbulanceCertGate=true`.

### Recovery ORDER leader replacement remains separate from CANCEL failover

CANCEL now has an all-replica deterministic proposer lease: every replica that accepts the same valid CANCEL certificate arms a non-extending timeout, advances the same candidate order on expiry, and lets the newly selected candidate submit. Scenarios 17/18 provide the unguarded/guarded suppression ablation. This fixes silence before CANCEL starts PBFT and applies to both emergency and crash reasons.

The later epoch-`e+1` recovery ORDER still uses its older proposer timer and does not yet have equivalent Byzantine-primary replacement across a request-scoped dynamic view. Complete dynamic-view VIEW_CHANGE/NEW_VIEW support remains the next liveness step before lazy-WAIT or suppressed-recovery-ORDER experiments.

### Ambulance certificate verification is configuration-gated

When `enableAmbulanceCertGate=true`, the echo path rejects an ambulance announcement lacking a valid Emergency_CA `VehicleCert` and payload signature; Scenario 15 enables this gate. Configurations with the gate disabled intentionally preserve the ablation behavior and may trust `ann.isAmbulance` after lane-qualified perception rather than independent ambulance-role authentication.

### Java `OrderRequestVerifier` port status

The key semantic checks from the old Java verifier are now ported to the bridge pre-verify:

- **Check 9** (cert-omission guard, Java Check 7): wired via `ResdbCertSnapshotFn` callback.
- **Check 10** (state-field verification): follower compares each SIGNED entry's lane/position/direction/ambulance against its own cert.
- **Java Check 8** (deterministic schedule re-execution): not needed in C++ — the leader does not embed a schedule in the proposal; `IntersectionExecutor` computes it deterministically post-consensus on every replica.

Remaining gaps:
- QUIET singleton isolation is enforced by the executor (post-commit), not by pre-vote rejection of a malformed schedule — because there is no schedule in the proposal to reject.
- Collision-safety and lane-queue-order checks (Java Checks 4 and 5) run in the executor post-commit, not as pre-vote firewall. A Byzantine leader that submits wrong lane/position values for cars it has no cert for (i.e., QUIET cars) can affect their batch placement, but cannot affect SIGNED cars whose state is checked by Check 10.

### App-level fallbacks are not PBFT proofs

On the ordinary non-incident path, `stopSignTimeoutSec` and `consensusTimeoutSec` can release vehicles for simulation safety. Once valid CANCEL/BLOCKING evidence exists, timeout handlers keep affected vehicles stopped until protocol resolution. In neither case are app timers substitutes for PBFT view-change; Byzantine-primary correctness experiments should use the VC logs and type `8` view-change traffic.

### Transport-level ACKs are intentionally absent

The current design does not port Java `ReliableV2VMessaging` sequence maps, ACKs, or retransmit queues. It relies on:

1. MAC staggering and jitter.
2. PBFT phase messages as implicit acknowledgments.
3. Type `5` cert retries.
4. Type `10` announce gossip for witness discovery across topology gaps.
5. Type `9` decision gossip for post-commit catch-up.
6. Deterministic authenticated-carrier suppression for type `11`.
7. Ranked authenticated CLEAR propagation and state-transition hush.

This avoids ACK implosion on broadcast V2V channels, but it means lossy-radio behavior must be evaluated at the PBFT and gossip layers.

### Carrier identity binding is not yet uniform

`WitnessKeyRegistry` now binds replica IDs to immutable generated P-256 keys. BLOCKED/CANCEL/CLEAR witness validation, CLEAR carriers, WAIT, and TYPE11 carrier counting use it. Direct type 8, decision gossip type 9, announce gossip type 10, and CANCEL-commit gossip type 14 still verify the key included in the envelope without uniformly checking it against `fromReplicaId`. Type 13 carrier-count suppression also records transport sender IDs without a separately signed carrier envelope. Those paths should be migrated to the same registry-bound carrier helper before adversarial propagation tests.

### ARRIVAL_CERT and stop-zone cert gossip remain blind while collecting

ARRIVAL_CERT source retry is phase/max gated, and each receiver relays a validated cert at most once. The primary's stop-zone cert gossip rebroadcasts every collected certificate until its deadline or discovery transition. Neither path currently stops per semantic certificate after authenticated propagation confirmation. This is the main remaining timer-driven propagation path that does not follow the CLEAR/TYPE11 evidence-driven suppression model.

### Geographic TYPE11 relay selection is not implemented

TYPE11 selection is deterministic and sender-relative, not random, but it does not compute a connected dominating set, multipoint relay set, or perception-derived geographic coverage gain. A future geographic layer should influence relay rank while retaining the counter-based fallback; nominal range alone must not hard-suppress the only radio path.

### ResDB log lines can be corrupted by thread interleaving

ResDB worker threads and the simulation thread write to the same stream without one shared line lock. Concurrent writes can splice numeric fields and produce plausible but false IDs. Use anchored full-line parsing and plausibility filters. `scripts/loglens.sh` provides categorized inspection and `scripts/pbft_matrix.py` provides phase/sequence delivery matrices, retry-arm, quorum, forced-view, and corruption views. In PBFT logs, `self=` and `sender=` are ResDB IDs (`OMNeT id + 1`); `omnet_self=` and `r<N>` are zero-based OMNeT/vehicle IDs.

---

## 23. Legacy BFT-SMaRt / JNI Mapping

The old architecture is useful for comparison but is not the current hot path.

| Legacy Java/BFT-SMaRt concept | Current ResDB/C++ equivalent |
|-------------------------------|------------------------------|
| `V2VProxyModule` | `ResDBIntersectionApp` |
| JNI JVM lifecycle | Removed from the active codebase |
| `IntersectionServer` / `ServiceReplica` | Socketless ResDB `ServiceNetwork` with `OmnetConsensusManagerPBFT` |
| `ServerRunner.triggerJoinForReplica()` | `ResdbOmnetTriggerConsensus()` |
| Java `TOMMessage` client request | Binary `ResdbProposeHdr + ResdbVehicleEntry[]` |
| Java type 9 client-request broadcast | Type 8 ResDB PBFT bytes; type 9 is decision gossip; type 10 is announce gossip |
| `V2VServersCommunicationLayer` | `OmnetReplicaCommunicator` + `VeinsTransport` callbacks |
| `ReliableV2VMessaging` | No direct port; jitter, PBFT phases, cert retries, and gossip provide current reliability strategy |
| `OrderScheduler` | `IntersectionExecutor::ExecuteData()` |
| `OrderRequestVerifier` | Bridge structural pre-verify plus executor invariants; not a full semantic pre-vote port |
| `notifyOrderDecided()` JNI callback | `ResdbOrderDecidedFn` C callback into `ResDBIntersectionApp::onOrderDecided()` |
| `SimulationClock.currentTimeMillis()` | `SimTimeProvider::NowUs()` updated by OMNeT++ |
| Java `RequestsTimer` STOP/STOP_NACK | ResDB PBFT view-change plus app-level forced VC trigger |
| Java `nativeGetFreshProposePayload()` after LC | New primary polling calls `proposeAll()` from current C++ cert state |

---

## 24. Agent Orientation Checklist

When continuing work on this system, first identify which layer owns the behavior:

1. **Physical arrival, lane truth, certs, and vehicle movement:** `ResDBArrivalProtocol.cc`, `ResDBDecision.cc`, and `ResDBTraCI.cc`.
2. **Radio carriage and signed V2V wrappers:** `ResDBTransport.cc`, `ResDBIntersectionApp.cc`, `ResdbV2VWire.h`, `WitnessKeyRegistry`, and `CryptoAuth`.
3. **PBFT integration and socketless ResDB behavior:** `resdb_omnet_bridge.cc`.
4. **Binary proposal/order ABI:** `resdb_omnet_bridge.h`.
5. **BLOCKED / CANCEL / CLEAR / WAIT behavior:** `ResDBRollbackProtocol.cc`, incident/rollback state in `ResDBIntersectionApp.h`, and recovery ORDER evidence/view validation in `resdb_omnet_bridge.cc`.
6. **Post-consensus catch-up:** `ResDBDecisionGossip.h/.cc` plus `handleDecisionGossip()`.
7. **Consensus time behavior:** `SimTimeProvider` and `ResdbOmnetUpdateSimTimeUs()`.
8. **Experiment knobs:** `ResDBIntersectionApp.ned` and `fourway/omnetpp.ini`.

Before changing behavior, archived Java migration docs can be consulted for historical invariants. Implement any still-relevant behavior in the current C++/ResDB ownership boundary rather than reintroducing Java/JNI assumptions.
