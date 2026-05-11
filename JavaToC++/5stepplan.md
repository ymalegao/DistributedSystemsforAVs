# AI Handoff Document: OMNeT++ → ResilientDB Migration
**Updated:** 2026-05-10  
**Environment:** OMNeT++ 6.2.0, Veins 5.3.1, C++17, ResilientDB (NexRes), WSL2

---

## Core Architectural Directives (unchanged)

1. **Side-by-Side:** Do NOT touch `V2VProxyModule` / JNI / BFT-SMaRt. The ResDB path is built in parallel under `src/veins/modules/application/resDB/`.
2. **C-Bridge only:** OMNeT++ communicates with ResDB **exclusively** through `resdb_omnet_bridge.h`. Never `#include` internal ResDB C++ headers in the Veins codebase.
3. **OMNeT++ owns everything:** ResDB must not open real OS sockets or use wall-clock timers.

---

## What the Java Side Currently Does (source of truth for C++ equivalents)

Each vehicle runs:
- **C++ side (Veins):** radio, cert collection, TraCI control, cert snapshot
- **Java side (BFT-SMaRt):** consensus logic, verifier, scheduler, view change
- **JNI glue:** `V2VJNIBridge.cc` bridges them

### Java Responsibilities to Replicate in C++

| Java class | C++ replacement | Migration notes |
|------------|----------------|-----------------|
| `IntersectionServer` / `ServiceReplica` | `resdb::Replica` instantiated inside `ResDBIntersectionApp` | No `ServerRunner` thread; OMNeT++ module owns lifecycle |
| `OrderScheduler` | `IntersectionTxnProcessor::ProcessTxn()` | Deterministic schedule becomes the ResDB "smart contract" |
| `OrderRequestVerifier` | Override `ProcessPrePrepare()` / `ValidateMessage()` in `V2VPBFTReplica` | **Must run BEFORE voting**, not after commit — see §5 below |
| `V2VServersCommunicationLayer` | Custom `resdb::NetChannel` subclass (`VeinsResDbChannel`) | Wraps ResDB protobufs in `BFTMessage`, calls `sendDown()` |
| `ReliableV2VMessaging` | C++ queue inside `VeinsResDbChannel` | Retransmit scheduling driven by OMNeT++ self-messages |
| `SimulationClock.currentTimeMillis()` | `simTime().inUnit(SIMTIME_MS)` direct | Eliminates all wall-clock in consensus |
| `RequestsTimer` | `V2VViewChangeManager` + OMNeT++ self-messages | Dual-timer logic preserved; see §7 |
| JNI bridge (`V2VJNIBridge.cc`) | **Deleted** | `notifyOrderDecided` becomes a direct C++ callback |

### JNI Call Sites to Eliminate

These are the exact Java→C++ and C++→Java handoff points that must be replaced:

**C++ calls into Java (to replace with direct C++ calls):**
- `V2VProxyModule::triggerJoinViaJNI(request)` → `PROPOSE_ALL:<proposerId>:<states>:<certs>` — becomes `ResdbOmnetTriggerConsensus(handle, payload)`
- `SimulationClock.updateTime(double)` → eliminated; ResDB reads `simTime()` directly via Step-4 time callback
- `ReliableV2VMessaging.globalResetV2V(int[] departed)` → direct C++ call in `VeinsResDbChannel`

**Java calls into C++ (native methods in `V2VJNIBridge.cc` to replace):**
- `notifyOrderDecided(replicaId, orderDecision)` → direct C++ function registered via `ResdbOmnetSetOrderCallback()`
- `notifyWipeComplete(processId)` → direct C++ call into `ResDBIntersectionApp`
- `nativeGetCertSnapshot(replicaId) → Set<String>` → direct access to `collectedCerts` map (C++ already owns it)
- `nativeGetFreshProposePayload(replicaId) → String` → direct call to C++ cert-payload builder
- `nativeBroadcastClientRequest(fromReplicaId, tomBytes)` → **eliminated**; ResDB routes through `VeinsResDbChannel::SendMsg()` automatically

---

## File Map (as of Step 2 completion)

```
incubator-resilientdb/
  integration/omnet/
    resdb_omnet_bridge.h        ← C API (Steps 1+2 surface complete)
    resdb_omnet_bridge.cc       ← Real impl (rebuild Bazel after every change)
    BUILD                       ← cc_binary(linkshared=True) + :resdb_bridge

veins-veins-5.3.1/src/veins/modules/application/resDB/
  ResDBIntersectionApp.ned      ← params: replicaId, resdbCryptoDir,
                                   smokeTestBroadcast, resdbConfig/Key/Cert/LogDir
  ResDBIntersectionApp.h/.cc    ← LoggingTransport, ec_private_key_ (P-256), stages
  IV2VTransport.h               ← Pure C++ ABC + static C-bridge adapters
  crypto/CryptoAuth.h/.cc       ← OpenSSL ECDSA P-256 (same lib as partner)
  messages/intersection.proto   ← proto3: VehicleState, ReservationRequest,
                                   ArrivalCertificate, ProposeAllPayload, OrderDecision

veins-veins-5.3.1/src/makefrag  ← RESDB_ROOT include, -lresdb_omnet_bridge,
                                   -lcrypto -lssl, -std=c++17

fourway/
  omnetpp.ini                   ← [Config BFTOverV2VWithResilientDB] live
  resdb_crypto/
    server.config               ← 4-replica region (127.0.0.1:18881-18884)
    node{1-4}.key.pri/pub       ← ED25519 (ResDB BFT-internal auth) — ALREADY GENERATED
    cert_{1-4}.cert             ← Certs signed by admin key — ALREADY GENERATED
    gen_resdb_keys.sh           ← Regeneration script
    logs/                       ← ResDB log output
```

---

## Two Crypto Layers (keep separate forever)

| Layer | Algorithm | Purpose | Where |
|-------|-----------|---------|-------|
| V2V transport signing | ECDSA P-256 (OpenSSL) | Sign outbound radio packets; verify inbound | `CryptoAuth::instance()`, `ec_private_key_` per replica |
| ResDB BFT-internal auth | ED25519 (ResDB toolchain) | Authenticate replicas inside PBFT protocol | `resdb_crypto/node{N}.key.pri`, already generated |

---

## Step-by-Step Status

### ✅ Step 1 — Hollow Shell & Proto Definitions (DONE)

**What was built:**
- `ResDBIntersectionApp` extends `DemoBaseApplLayer`, owns opaque ResDB handle lifecycle
- `messages/intersection.proto` — proto3 equivalents of BFTMessage payloads
- `ResdbOmnetCreateNullHandle()` — safe fallback for missing config

**Gotchas discovered:**
- `libresdb_omnet_bridge.so` exports zero symbols when `cc_binary(linkshared=True, srcs=[])` — deps not pulled in without rebuild or `alwayslink`. Rebuild Bazel after every bridge change.
- `CreateResDBServer(nullptr, ...)` throws `basic_string::_M_construct null not valid` deep inside ResDB. Guard: always use `CreateNullHandle()` when config is empty.
- Exceptions in `ResDBIntersectionApp::initialize()` are attributed to `TraCIScenarioManagerLaunchd` by OMNeT++ because that module triggers vehicle creation. Misleading error context.
- `TraCIScenarioManagerLaunchd`: null guards needed for `getBaseDirectory()` and `getVariable(CFGVAR_RUNNUMBER)`. Patched.

---

### ✅ Step 2 — Abstract Transport Interface (DONE)

**What was built:**
- `IV2VTransport.h` — `sendTo(to, data, len)` / `broadcast(data, len)` ABC with C-bridge adapters
- `ResdbOmnetTransportCallbacks` C struct in bridge header
- `ResdbOmnetSetTransport()`, `ResdbOmnetTestBroadcast()` added to bridge
- `LoggingTransport` — prints every packet to `stderr` (thread-safe via `fprintf`)
- `smokeTestBroadcast` NED param — 50ms self-message fires synthetic broadcast to prove callback path
- Per-replica P-256 keypair at `initialize(stage==0)` via `CryptoAuth` (transport signing, Step 3+)
- `resdbCryptoDir` NED param — auto-computes `server.config/nodeN.key.pri/cert_N.cert`
- ResDB ED25519 keys already generated in `fourway/resdb_crypto/`

**Expected stderr per replica:**
```
[ResDB-TRANSPORT r0] broadcast  8 bytes
```

**Before running:** rebuild Bazel bridge, then `make` in Veins src.

---

### 🔲 Step 3 — Over-the-Air (OTA) Transmission

**Goal:** Replace `LoggingTransport` with a radio-backed transport. ResDB's PBFT packets travel over 802.11p; inbound radio bytes are injected back into the ResDB replica **without any OS sockets**.

**Java equivalent being replaced:**
- `V2VServersCommunicationLayer` (serialization, per-sender ordering, ACK/retransmit)
- `nativeBroadcastClientRequest` (leader → followers over radio)
- C++ type-9 dispatch → Java `deliverInjectedClientRequest`

**Concrete implementation plan:**

#### 3a. Step-3 transport implementation (current)

We are **not** subclassing `resdb::NetChannel` inside Veins. Instead, we use the Step-2 transport callback table to let ResDB request sends, and we translate those callbacks into OMNeT++ radio messages:

- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.h/.cc`
  - `class VeinsTransport : IV2VTransport` receives `sendTo/broadcast` from ResDB worker threads.
  - It enqueues outbound packets into a mutex-protected queue.
  - A periodic self-message (`transport_poll_msg_`) drains the queue on the simulation thread and transmits them via `sendDelayedDown()`.

This keeps OMNeT++ single-threaded while still allowing ResDB to call transport callbacks from its own threads.

On the ResDB side, PBFT outbound is now routed through the same callback table:
- `ResdbOmnetCreateKvServer()` instantiates an OMNeT++-aware PBFT service that overrides `ConsensusManager::GetReplicaClient()` to return an OMNeT communicator.
- That communicator serializes protobufs and calls `ResdbOmnetTransportCallbacks.send_to/broadcast` instead of `NetChannel` (TCP).

#### 3b. Packet format (current)

We reuse the existing `BFTMessage` envelope for radio carriage and reserve a new message type:

- `BFTMessage.messageType = 8` → “ResDB consensus bytes”
- `fromReplicaId` / `toReplicaId` keep the same semantics (`-1` = broadcast)
- `payload[]` is a **signed wrapper**:
  - `[33B sender_pub_key_compressed][1B sigLen][sigLen bytes DER ECDSA(sig over raw bytes)][raw ResDB bytes...]`

Signing and verification use the existing OpenSSL P-256 helpers in `crypto/CryptoAuth.*`.

Important: the “raw ResDB bytes” are currently a serialized `resdb::ResDBMessage` protobuf whose `data` field contains a serialized `resdb::Request`. (This matches what `ConsensusManager::Process()` expects on receive.)

#### 3c. Inbound path (current)

Inbound radio bytes are handled in `ResDBIntersectionApp::onWSM()`:

1. Filter to `BFTMessage.messageType == 8` and correct destination (`toReplicaId == -1 || toReplicaId == replicaId`).
2. Unpack the signed wrapper and verify the transport signature (P-256).
3. Call `ResdbOmnetDeliverPacket(handle, fromReplicaId, resdbBytes, resdbLen)`.

On the ResDB side, `ResdbOmnetDeliverPacket` injects bytes into `ServiceNetwork` via `ServiceNetwork::InjectInboundPacket()` so the normal `ConsensusManager::Process()` path consumes them.

#### 3d. Reliability layer (replacing `ReliableV2VMessaging`)

Java's reliability layer previously handled strict per-sender sequence ordering and explicit ACK piggybacking. To prevent 802.11p "ACK implosions" and broadcast storms as we scale up the number of vehicles, we are abandoning this TCP-like overhead.

In the new C++ (ResilientDB) stack:
* **The Unordered Path:** All messages take the unordered path for now. Do not implement strict `std::map` sequence tracking or dropping at the transport layer. 
* **Implicit ACKs (Let PBFT do the work):** We do not need explicit transport-level ACKs. The BFT phases naturally acknowledge each other. If Car A broadcasts a `Pre-Prepare`, Cars B, C, and D receive it and broadcast a `Prepare`. Hearing those `Prepare` broadcasts over the radio *is* the acknowledgment that they received the `Pre-Prepare`. 
* **Jittered Retransmission:** Retransmissions are handled via OMNeT++ self-message timers (`scheduleAt`). Always apply a random base jitter to transmissions to avoid MAC-layer collisions. If a node hears the next logical phase of the protocol (e.g., it was waiting for a `Prepare` and finally hears it), it simply stops/cancels its retransmission timer for the previous step. We had jitters defined here: veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.ned 
* **Reference the Arrival Certs:** For a template on how this should look, refer to the existing Arrival Cert logic in the C++ codebase. That part of the system was already running purely natively over the air (bypassing Java entirely) and serves as the exact model for how our new C++ ResilientDB messages should behave.

#### 3e. P-256 transport signing (wire in here)

Before `sendDown()`: `CryptoAuth::instance().signBytes(ec_private_key_, data, len, sig, sigLen)`  
Before `DeliverPacket`: `CryptoAuth::instance().verifyBytes(sender_pub_key, ...)` — need pubkey registry (`std::map<int, uint8_t[33]> replica_pub_keys_`). Populated by sharing `ec_pub_key_` in a beacon or ARRIVAL_ANNOUNCE equivalent.

#### 3f. Bridge additions needed

```c
// Inject inbound bytes into ResDB's receive path
int ResdbOmnetDeliverPacket(void* handle, int from_replica,
                            const uint8_t* data, uint32_t len);

// Pass the VeinsResDbChannel instance into the bridge (before RunServer)
int ResdbOmnetSetChannel(void* handle, void* channel_ptr);
```

#### 3g. Socketless ResDB server (current)

To satisfy “OMNeT++ owns everything”:
- `ServiceNetwork` now supports `enable_network_acceptor=false` (no `Acceptor` bind/listen).
- `ResdbOmnetCreateKvServer()` creates a socketless `ServiceNetwork` and disables heartbeat.
- `ResdbOmnetCreateKvServer()` also disables node-level signature verification (`SetSignatureVerifierEnabled(false)`); transport authenticity is provided by the P-256 V2V wrapper.
- `ResdbOmnetRunServer()` runs `ServiceNetwork::Run()` in a background `std::thread`.

If you see logs like `server call: N` but `server process: 0`, it usually means the server thread was never started. The OMNeT++ module must call `ResdbOmnetRunServer()` at init when a real handle exists.

---

### 🔲 Step 4 — Time Virtualization

**Goal:** All PBFT timeouts use `simTime()`, not wall clock. This is what makes the simulation deterministic and reproducible.

**Java equivalent being replaced:**
- `SimulationClock.currentTimeMillis()` → already abstracted in Java via `updateTime(double)`
- `RequestsTimer` dual-timer model (wall-clock wakeup + sim-time gate)

**Java `RequestsTimer` behaviors to preserve exactly:**
- `STOP_RETX_WALL_MS`: wall timer wakeup cadence (becomes OMNeT++ self-message interval)
- `STOP_RETX_SIM_MS`: sim-time gate on actual STOP emission
- `STOP_BLIND_EMITS`: first N STOPs are broadcast blind before switching to NACK bitmask
- `NACK_REPLIES_PER_PEER`: DoS cap on per-peer NACK replies
- `tryClaimLCEpoch()`: only one LC escalation in-flight; `LC_ESCALATION_GAP_SIM_MS` escape
- `recordHeardStop(sender)`: independent of LCManager purge timing
- `dropRegencyState(regency)`: clears NACK/emit state, re-arms escalation

**Concrete implementation plan:**

#### 4a. Bridge sim-time tick (current)

We use an **update-from-OMNeT** model (thread-safe) rather than calling `simTime()` from ResDB worker threads:

```c
// Called by OMNeT++ simulation thread.
int ResdbOmnetUpdateSimTimeUs(void* handle, int64_t now_us);
```

`ResDBIntersectionApp` schedules a periodic self-message (`resdbTimeTick`) and calls the bridge with:
`ResdbOmnetUpdateSimTimeUs(handle, simTime().inUnit(SIMTIME_US))`.

#### 4b. ResDB-side clock interception (current)

ResDB now has a `SimTimeProvider` singleton:

- `incubator-resilientdb/common/utils/sim_time_provider.*`
  - `SimTimeProvider::UpdateNowUs(now_us)` stores the current sim time and wakes any waiters.
  - `SimTimeProvider::NowUs()` returns the latest injected sim time.

Core changes:
- `GetCurrentTime()` now returns sim-time microseconds when sim-time is active (otherwise falls back to `gettimeofday`).
- `SleepForUs(delta_us)` now waits on sim-time (otherwise falls back to `usleep`).
- PBFT modules that previously used `usleep/sleep` now call `SleepForUs` so timeouts are driven by OMNeT++ ticks.

Do **not** use `LD_PRELOAD` to override `gettimeofday` — too fragile with Bazel's hermetic builds.

#### 4c. `V2VViewChangeManager` (replacing `RequestsTimer`)

```cpp
class V2VViewChangeManager {
    cMessage* lc_timer_msg_ = nullptr;
    simtime_t last_stop_emit_sim_time_;
    int       blind_emits_remaining_;
    std::map<int, int> nack_reply_count_;
    std::set<int>      heard_stops_;

    void onLCTimerFired();       // called from ResDBIntersectionApp::handleSelfMsg
    void emitStop();             // broadcasts STOP via VeinsResDbChannel unordered path
    void emitStopNack(int peer); // targeted NACK after blind phase
    bool tryClaimLCEpoch(int epoch);
    void dropRegencyState(int regency);
};
```

`onLCTimerFired()` is the C++ equivalent of `RequestsTimer.run()`. It checks `simTime() - last_stop_emit > STOP_RETX_SIM_MS` before actually emitting, exactly mirroring the Java dual-timer gate.

---

### ✅ Step 5 — Smart Contract & Application Logic (DONE)

**Goal:** Full PBFT consensus produces deterministic `OrderDecision`. Vehicles cross in decided order. Metrics match existing `analyze_log.py` format.

**Java equivalents replaced:**
- `IntersectionServer.sendConsensusRequest()` → `proposeAll()` + `ResdbOmnetTriggerConsensus()`
- `OrderRequestVerifier` (8 checks) → `SetPreVerifyFunc` lambda (count + duplicate ID checks; full 8-check port is debt)
- `OrderScheduler.buildProposal()` → `IntersectionExecutor::ExecuteData()` sort
- `appExecuteBatch()` → `notifyOrderDecided()` → `onOrderDecided` C callback → `processOrders()`

---

#### Wire Format Decision — No Protobuf on Veins Side

**The Nix GCC toolchain used by OMNeT++ on WSL2 cannot safely add `/usr/include` to the include path** (the GCC's `<cstdio>` uses `#include_next <stdio.h>` and pulls in Ubuntu's stdio.h, which breaks `L_tmpnam`). Neither `-I` nor `-idirafter` was safe.

**Solution:** The C bridge API uses a simple packed-struct wire format defined in `resdb_omnet_bridge.h` (no external library required on the Veins side). The ResDB bridge side uses these same structs internally.

```c
// resdb_omnet_bridge.h — shared wire format (current as of 2026-05-10)
#pragma pack(push, 1)
typedef struct ResdbVehicleEntry {
    int32_t  replica_id;        // 4 bytes
    uint64_t sim_time_us;       // 8 bytes — simTime() at stop-zone entry; UINT64_MAX = QUIET
    uint8_t  is_ambulance;      // 1 byte
    uint8_t  lane;              // 1 byte  — 0=N,1=S,2=E,3=W
    uint8_t  direction;         // 1 byte  — 0=Straight,1=Left,2=Right
    uint8_t  position_in_lane;  // 1 byte  — 1=front,2=second,…
    uint8_t  cyber_status;      // 1 byte  — 0=QUIET,1=SIGNED
} ResdbVehicleEntry;            // 17 bytes total

typedef struct ResdbProposeHdr {
    uint32_t epoch;
    int32_t  leader_id;
    uint64_t propose_sim_time_us;
    uint32_t n_vehicles;
} ResdbProposeHdr;             // 20 bytes
// followed by n_vehicles × ResdbVehicleEntry (17 bytes each)

typedef struct ResdbVehicleDecision {
    int32_t  replica_id;
    uint32_t batch_index;    // 0-based; vehicles with same index cross simultaneously
} ResdbVehicleDecision;      // 8 bytes

typedef struct ResdbOrderHdr {
    uint32_t epoch;
    uint32_t n_vehicles;
    uint32_t n_batches;
} ResdbOrderHdr;               // 12 bytes
// followed by n_vehicles × ResdbVehicleDecision (8 bytes each)
#pragma pack(pop)
```

`intersection.proto` is kept for documentation only. `intersection.pb.h/.pb.cc` are **not** in the Veins build tree.

---

#### 5a. New BFT Message Type

- **Type 8** (existing): ResDB PBFT consensus bytes — signed, delivered via `ResdbOmnetDeliverPacket`
- **Type 9** (new): `kResdbStateAnnounceMsgType` — broadcasts a raw `ResdbVehicleEntry` (13 bytes, no crypto needed)

#### 5b. Pre-vote Verifier

`SetPreVerifyFunc` lambda wired into `OmnetConsensusManagerPBFT` at server creation. Current checks:
1. Payload parses as `ResdbProposeHdr` with correct size
2. `n_vehicles == expected_replicas` (from ResDB config)
3. No duplicate `replica_id` values

**Debt:** Checks 4–8 from Java `OrderRequestVerifier` (lane order, QUIET singletons, cert-omission guard, deterministic recomputation) are not yet ported.

#### 5c. `IntersectionExecutor` (replaces `KVExecutor`)

Lives in `resdb_omnet_bridge.cc`. Subclasses `resdb::TransactionManager`, overrides `ExecuteData`:
1. Parse `ResdbProposeHdr` + `ResdbVehicleEntry[]` (17 bytes each) from committed bytes
2. **Ambulance lane flush + greedy ConflictMatrix batching** (see §Batch Packer below)
3. Emit `ResdbOrderHdr` + `ResdbVehicleDecision[]` with `batch_index` per vehicle
4. Call registered `ResdbOrderDecidedFn` callback

**Batch packer algorithm** (port of `OrderScheduler.java::buildProposal` + `ConflictMatrix.java`):
- Sort work queue: ambulance-lane vehicles (by `position_in_lane`) first, then others (by `sim_time_us` ascending). This ensures ambulance-lane gets assigned to early batches (flush priority).
- Greedy packing loop: for each vehicle in work order, try to add to the current open batch. A vehicle **can** join the batch if `IsSafeToBatch(lane, dir, b.lane, b.dir)` is true for every vehicle `b` already in the batch. QUIET vehicles (`cyber_status=0` or `sim_time_us=UINT64_MAX`) always get singleton batches; non-QUIET vehicles (including ambulance) can co-batch with any safe non-QUIET partner.
- `IsSafeToBatch()`: C++ port of `ConflictMatrix.java::isSafeToBatch()` — 12 hardcoded safe pairs, same-lane always rejected.

**Veins-side batch crossing** (`processOrders()` in `ResDBIntersectionApp.cc`):
- Parses `ResdbVehicleDecision[]`, finds own `batch_index`.
- `batch_index == 0`: resume immediately.
- `batch_index > 0`: arm `clearance_poll_msg_` every `clearancePollPeriodSec`. Poll until **all** vehicles in `batch_index - 1` have cleared via TraCI (`vehicleHasClearedIntersectionTraCI`), then resume.

**QUIET marking** (`proposeAll()` in `ResDBIntersectionApp.cc`):
- Vehicles with collected `ARRIVAL_CERT` (f+1 echoes) → `cyber_status=1` (SIGNED).
- Vehicles absent from `collected_certs_` at propose time → `cyber_status=0` (QUIET), `sim_time_us=UINT64_MAX`.
- QUIET vehicles are isolated to singleton batches by the executor's greedy packer.

#### 5d. New Bridge API (Step 5)

```c
int ResdbOmnetTriggerConsensus(void* handle, const uint8_t* payload, uint32_t len);
int ResdbOmnetSetOrderCallback(void* handle, ResdbOrderDecidedFn cb, void* ctx);
int ResdbOmnetGetPrimary(void* handle);   // returns 0-based primary replica ID
int ResdbOmnetRemoveReplica(void* handle, int replica_id);  // stub, Step 5f debt
```

`TriggerConsensus` wraps the payload in `Request{TYPE_CLIENT_REQUEST}` → `ResDBMessage` → `InjectInboundPacket`. Only the primary should call it.

#### 5e. Veins-side flow

1. **Stage 1 init**: schedule `state_announce_msg_` every `stateAnnounceIntervalSec` (default 0.1 s)
2. **`broadcastStateAnnounce()`**: packs own `ResdbVehicleEntry` (13 bytes) → BFTMessage type 9 → `sendDown`; also stores own entry in `collected_states_`
3. **`onWSM` type 9**: unpacks `ResdbVehicleEntry` → stores in `collected_states_`
4. **`handlePositionUpdate`**: when `dist < stopDistance * (totalVehicles / 2.0)` and `!enteredStopZone`: calls `stopVehicle()`, records `stop_time_`; primary arms `propose_timeout_msg_` or calls `proposeAll()` immediately if all N states already collected
5. **`proposeAll()`**: packs `ResdbProposeHdr` + entries → `ResdbOmnetTriggerConsensus`
6. **`onOrderDecided` (C callback, ResDB worker thread)**: enqueues bytes into `pending_orders_` (mutex-protected)
7. **`processOrders()` (sim thread, polled by `transport_poll_msg_`)**: unpacks `ResdbOrderHdr` + `int32_t[]` → finds own position → calls `resumeVehicle(position)`
8. **`resumeVehicle(position)`**: `setSpeedMode(31)` + `setSpeed(cruiseSpeedMps)`; stagger = `position × safetyGapS`

#### 5e. Metrics emitted

```
[METRICS r] Arrival_Time: <simtime>
[METRICS r] Stop_Time: <simtime>
[METRICS r] Cert_Collection_Start: <simtime>
[METRICS r] ProposeAll_Submit_Time: <simtime>
[METRICS r] Order_Decided_Time: <simtime>
[METRICS r] Resume_Time: <simtime>
```

#### 5f. Epoch reset (`ResdbOmnetRemoveReplica`)

Currently a no-op stub. Full implementation (reset per-sender sequence state, re-arm view-change manager) is deferred.

---

#### New NED params added to `ResDBIntersectionApp.ned`

| Param | Default | Notes |
|-------|---------|-------|
| `intersectionX/Y` | 300m | Matches `FourVehiclesBFTOverV2V` |
| `stopDistance` | 5m | Stop zone = `stopDistance × (totalVehicles/2)` |
| `totalVehicles` | 4 | N replicas in this batch |
| `certCollectionTimeoutSec` | 2s | Fallback propose timer |
| `stateAnnounceIntervalSec` | 0.1s | Type-9 broadcast cadence |
| `cruiseSpeedMps` | 14.0 | Speed after consensus |
| `safetyGapS` | 1.5 | Seconds between consecutive crossings |
| `isAmbulance` | false | Emergency priority |

---

---

## Session 2026-05-09 — Arrival Cert Protocol + TraCI Hardening

**Architecture reference:** [`RESDBARCH.md`](RESDBARCH.md) describes the **legacy Java flow vs current ResDB path**, parity with [`V2VProxyModule`](../veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.cc), bridge PreVerify vs Java `OrderRequestVerifier`, and known gaps cross-linked below.

### What We Fixed

#### Bug 1: `proposeAll()` crash — "payload too short for entries"

**Root cause (two bugs, same function):**

1. `hdr.n_vehicles = 4` was hard-coded in `proposeAll()`, but `buf` was sized using `entries.size()` (which equalled 1 — only own state). PreVerify saw 33 bytes but expected 72 (`20 + 4×13`).
2. In `onWSM` type-9 handler, `n = 4` was overriding the real collected count, forcing the primary to call `proposeAll()` after only receiving the first peer state announce.

**Fix:** `hdr.n_vehicles = n` (actual collected count). Removed the fake override. Added a self-state upsert in `proposeAll()` as a race guard.

---

#### Bug 2: Cars overshoot the intersection

**Root cause:** `getDistanceToIntersection()` used Euclidean distance to a fixed coordinate `(intersectionX, intersectionY)`. Two problems:
- The fixed coordinate may not match the actual stop line position in the SUMO network.
- At 14 m/s, a vehicle entering a 10m stop zone has under 0.7 s to react — not enough to stop cleanly.

**Fix:** Replaced with the V2VTraCI implementation:
```cpp
double laneLen = traci->lane(myLaneId).getLength();
double curPos  = traci->vehicle(myId).getLanePosition();
return std::max(0.0, laneLen - curPos);  // distance to end of lane = stop line
```
Stop zone trigger changed from `dist < stopDistance * (N/2)` to simply `dist < stopDistance && dist > 0`. Default `stopDistance` changed from 5 m → 20 m in the NED file and 15 m → 20 m in the .ini.

---

#### Bug 3: `stopVehicle()` wrong TraCI API

**Root cause:** Used `traciVehicle->setSpeedMode(0) + setSpeed(0)` via the old `traciVehicle` handle, bypassing SUMO's safety model. In practice, the vehicle didn't always stop.

**Fix:** Ported from `V2VTraCI.cc`:
```cpp
TraCICommandInterface::Vehicle* vc = mobility->getVehicleCommandInterface();
vc->setSpeedMode(31);   // re-enable all SUMO safety checks
vc->setSpeed(0);        // hard stop at 0 m/s
```

Same pattern applied to `resumeVehicle()`.

---

#### Bug 4: Bridge PreVerify — structural checks only (not Java OrderRequestVerifier)

The PRE_PREPARE verifier in [`resdb_omnet_bridge.cc`](../incubator-resilientdb/integration/omnet/resdb_omnet_bridge.cc) (~259–355) validates **binary propose payload shape** for ResDB PBFT. It is **not** a port of Java `OrderRequestVerifier` (no ConflictMatrix, QUIET batch rules, or schedule re-execution on the proposal string).

Original gap: only size + `n_vehicles` + duplicate IDs. **Extended** with: ID range, non-zero `sim_time_us`, sane `is_ambulance`, valid `leader_id`, plus logging as “all 8 checks ok” — these are **structural / sanity** checks aligned with `ResdbVehicleEntry`, not the eight **semantic** Java checks.

**Where cryptography applies:** **`validateArrivalCert`** on **type 5** (`ResDBIntersectionApp.cc`) verifies **f+1** ECDSA echoes per cert. Semantic omission/censorship guards on the **ordered proposal** remain **Gap 5** until the wire format carries lane / QUIET flags.

---

### What We Added / Ported

#### A. Full arrival cert protocol wired into ResDB path

`ResDBIntersectionApp` now runs the exact V2VArrivalProtocol cert exchange before triggering ResDB consensus. Message type map:

| Type | Name | Direction | Description |
|------|------|-----------|-------------|
| 1 | `ARRIVAL_ANNOUNCE` | broadcast | ECDSA P-256 self-signed claim with TraCI lane, position, direction, isAmbulance, epoch |
| 4 | `ARRIVAL_ECHO` | unicast | Witness echo — verifies lane via TraCI, ECDSA-signs over `(carId:lane:pos:dir:isAmb:echoingReplicaId)` |
| 5 | `ARRIVAL_CERT` | broadcast | f+1 collected echoes; validated before storing |
| 8 | ResDB PBFT | broadcast | Unchanged — signed `ResdbMessage` wrapper |

Type 9 (raw state announce) is **removed** from the main flow and replaced by this protocol.

**Wire serialization:** Identical text-pipe format from `V2VArrivalProtocol.cc` (`|`-delimited with hex-encoded crypto fields). `serializeArrivalAnnouncement`, `deserializeArrivalAnnouncement`, `serializeArrivalEcho`, `deserializeArrivalEcho`, `serializeArrivalCert`, `deserializeArrivalCert` all ported verbatim into `ResDBIntersectionApp.cc`.

**Cert-to-ResDB translation:** `proposeAll()` reads from `collected_certs_` (not the old `collected_states_`). For each `ArrivalCert` it packs a `ResdbVehicleEntry`:
- `replica_id` from `extractReplicaId("veh3")` → 3
- `sim_time_us` from `local_vehicle_states_[carId].arrival_time_us` (set at announce time)
- `is_ambulance` from `cert.isAmbulance` (cryptographically verified via echo signatures)

#### B. Phase state machine

```cpp
enum ConsensusPhase {
    IDLE,
    COLLECTING_CERTS,
    WAITING_FOR_CLEARANCE,
    PULLING_FORWARD,
    EXECUTING,
    DEPARTED,
};
```

Transitions (simplified): idle / collecting certs → waiting for clearance when proposing or at stop line → executing after `processOrders()` applies order; timeouts can force executing-like resume.

#### C. Staggered initial announcement (V2VProxyModule `triggerJoinTimer`)

```cpp
scheduleAt(simTime() + trigger_join_time_ + replicaId_ * arrival_slot_sec_, initial_announce_msg_);
```
Default: replica 0 announces at t=0.5s, replica 1 at t=0.6s, etc. **Periodic** `broadcastArrivalAnnouncement` uses `broadcastArrivalAnnouncementIntervalSec` until all vehicles are observed or order applied.

#### D. Stop-sign and consensus timeout fallbacks

Ported from `V2VProxyModule`:
- `stop_sign_timeout_msg_` (default 10 s): safety release if no order arrives
- `consensus_timeout_msg_` (default 30 s): backstop for all vehicles

Both are scheduled at stop-zone entry and cancelled when an order is processed (`processOrders`).

#### E. `discoverLane()` and `vehicleHasClearedIntersectionTraCI()`

Ported from `V2VTraCI.cc`. `discoverLane()` builds `lane_queue_` and `car_ahead_` from live TraCI positions at stop-zone entry. `vehicleHasClearedIntersectionTraCI()` uses road ID (`C2x` edges) to detect post-intersection departure.

#### F. NED params (`ResDBIntersectionApp.ned`)

| Param | Default | Notes |
|-------|---------|--------|
| `triggerJoinTimeSec` | 0.5s | Initial one-shot announce schedule |
| `arrivalSlotSec` | 0.025s | Stagger per replica (also scales announce **send delay** in `sendBFTMessage`) |
| `broadcastArrivalAnnouncementIntervalSec` | 0.1s | Periodic re-announce until all observed |
| `certCollectionTimeoutSec` | 2s | Leader cert-collection deadline length |
| `stopSignTimeoutSec` / `consensusTimeoutSec` | 10s / 30s | Fallbacks |
| `debugCertProtocol` | false | `[CERT-DEBUG]` traces |
| `viewJitterMin` / `viewJitterMax` | 1–5 ms | MAC stagger (echo/cert jitter component) |
| `viewAgreementSlotSec` | 25 ms | Per-replica echo slot (`× replicaId`) |
| `broadcastJitterMin` / `broadcastJitterMax` | 0.1–1 ms | Announce/cert broadcast jitter component |
| `broadcastSlotSec` | 5 ms | Fallback stagger for other types |
| `enableArrivalCertRetries` | true | After first type-5 send |
| `arrivalCertRetryIntervalSec` | 0.1s | Between cert rebroadcasts |
| `arrivalCertRetryMax` | 30 | Extra sends after first (0 = unlimited until stop) |
| `intendedDirection` | "S" | Same as V2VProxyModule |
| `intersectionX/Y` | 300m | .ini compat; TraCI drives geometry |

#### G. Cert collection deadline (leader) — align with V2V

- **`tryStartCertCollectionTimer()`** schedules **`cert_collection_timeout_`** when the **primary** first observes an arrival (`broadcastArrivalAnnouncement` self-path, **`handleArrivalAnnouncement`** valid path, and **FALSE_LANE** observation path).
- **Stop zone:** If all certs already collected → **`proposeAll()`** immediately. Else schedule **`propose_timeout_msg_`** **only if** no deadline timer is already pending (avoid stacking).
- **Timeout before stop line:** Set **`deferred_propose_after_cert_timeout_`**; run **`proposeAll()`** on stop-zone entry (no V2V **0.5 s** reschedule loop).

#### H. MAC stagger + type-5 retries

- **`sendBFTMessage`** uses **`sendDelayedDown`** with the same **knob semantics** as [`V2VReliability.cc`](../veins-veins-5.3.1/src/veins/modules/bftsmart/V2VReliability.cc) (echo: `viewAgreementSlotSec×replicaId + viewJitter`; announce: `arrivalSlotSec×replicaId + broadcastJitter`; cert: view jitter only).
- **`cert_retry_timer_`**: rebroadcasts the **same** assembled **ARRIVAL_CERT** until max retries, **`proposeAll()`**, **`order_applied_`**, or **first type-8 from current primary** (`onWSM`).

---

### What Is Still Not The Same — Remaining Gaps vs. V2VProxyModule

#### Gap 1: QUIET vehicle handling (HIGH PRIORITY)

**V2VProxyModule behavior:** When cert-collection timeout fires before all `BATCH_SIZE` certs arrive, the leader still calls `submitProposeAllToBFT()`. Missing vehicles are marked **QUIET** — they get a synthetic cert entry with `cyberStatus = QUIET` and are placed last in the order (after all SIGNED vehicles). They still cross the intersection, just last.

**Current ResDB behavior:** If only 2 of 4 certs are collected, `proposeAll()` sends `n_vehicles=2`. PreVerify check 2 (`n_vehicles == expected_replicas`) rejects it. The proposal silently fails. Consensus never fires.

**Required fix:**
- Change PreVerify check 2 to accept proposals where `n_vehicles >= 3f+1` (quorum minimum), not `== total_vehicles_`.
- Or: Add QUIET padding — vehicles absent from `collected_certs_` get a synthetic `ResdbVehicleEntry` with `sim_time_us = UINT64_MAX` (sorts last) and `is_ambulance = 0`.
- `IntersectionExecutor` sort already places `UINT64_MAX` entries last naturally.
- The ResDB config's `expected_replicas` should remain equal to `total_vehicles_` for BFT correctness; only the PreVerify threshold changes.

#### Gap 2: `resumeVehicle()` delay is not scheduled (MEDIUM)

**V2VProxyModule behavior:** `pendingResumeDelays.push(delaySeconds)` queues the delay. The main-thread `processQueueTimer` fires and executes `scheduleAt(simTime() + delay, resumeMsg)`. The actual speed change happens `position × safetyGapS` seconds later.

**Current ResDB behavior:** `resumeVehicle(position)` calls `setSpeed(cruiseSpeedMps)` immediately on the simulation thread. The `safetyGapS` delay is logged but never enforced. All vehicles get the same speed simultaneously.

**Required fix:** In `processOrders()`, after finding `position`, schedule a `cMessage* resume_msg_` at `simTime() + position * safetyGapS`. Handle it in `handleSelfMsg()` and call the actual TraCI speed change there.

#### Gap 3: Multi-epoch / multi-batch support (MEDIUM)

**V2VProxyModule behavior:** After a batch crosses, `resetForNextRound()` increments `currentEpoch`, clears all cert state, and restarts the cert collection for vehicles still waiting. Multi-batch intersections are handled.

**Current ResDB behavior:** `current_epoch_` never increments. `order_applied_ = true` is a one-shot gate — after the first order is processed, the app is permanently frozen. Subsequent arrivals at the intersection (in a longer simulation) will not trigger a new consensus round.

**Required fix:** Port `resetForNextRound()` — after `EXECUTING → DEPARTED` transition (detected via `vehicleHasClearedIntersectionTraCI()`), reset cert state, increment epoch, and re-enter `COLLECTING` phase.

#### Gap 4: Ambulance certificate verification not implemented

**V2VProxyModule behavior:** When `handleArrivalAnnouncement()` receives a car claiming `isAmbulance=true`, it runs full cert verification: parses `ambulanceCertBytes` as `VehicleCert`, calls `CryptoAuth::instance().verifyCert(cert)`, verifies the self-signature over the ambulance payload string.

**Current ResDB behavior:** `handleArrivalAnnouncement()` trusts `ann.isAmbulance` unconditionally (comment: "trust for now"). A Byzantine vehicle can claim ambulance priority and jump the queue.

**Required fix:** Include the `VehicleCert` struct and `CryptoAuth::verifyCert()` path in `handleArrivalAnnouncement()`. The struct is defined in `V2VProxyModule.h` — it should be extracted to a shared header rather than including V2VProxyModule.h directly (which pulls in JNI).

#### Gap 5: Java `OrderRequestVerifier` semantic checks 5–7 not ported (MEDIUM — security)

These are **not** the bridge’s structural PRE_PREPARE checks — they require **lane / QUIET / follower state** not exposed to **`SetPreVerifyFunc`**.

Java `OrderRequestVerifier` checks not yet replicated on the proposal path:
- **Check 5 (lane queue order):** `ResdbVehicleEntry` has no lane field, so this cannot be verified without extending the wire format.
- **Check 6 (QUIET singleton rule):** Requires QUIET/SIGNED distinction in the wire format — not present in `ResdbVehicleEntry`.
- **Check 7 (cert-omission guard):** Followers must reject proposals that list a vehicle as absent if the follower already has an `ARRIVAL_CERT` for that vehicle locally. Requires the follower's `collected_certs_` to be consulted inside the PreVerify lambda — currently PreVerify has no access to the per-replica app state.

**Required fix:** Either extend `ResdbProposeHdr` / `ResdbVehicleEntry` with a `cyberStatus` byte (SIGNED=0/QUIET=1) and lane info, or implement check 7 alone first (it's the most safety-critical — prevents censorship attacks) by passing a pointer to `collected_certs_` into the lambda via capture.

#### Gap 6: PBFT primary vs V2V “lane leader” (not automatically a defect)

**Two different notions:**

| Layer | V2VProxyModule (Java path) | ResDB path today |
|--------|---------------------------|------------------|
| **Intersection / lane logic** | `amITheLeader(physicallyObservedCars)` — who may **submit** from the app’s perspective, topology-aware | Still only **primary** calls **`proposeAll()`** / `ResdbOmnetTriggerConsensus` — aligned with **one global PBFT primary** |
| **Consensus leadership** | BFT-SMaRt leader + Java LC integration | **ResDB PBFT primary** from **`ResdbOmnetGetPrimary`** (config / view) |

Using a **single global PBFT primary** is **normal** for PBFT: the protocol picks **one** sequencer per view. That is **not** the same bug class as “wrong lane leader” in the thesis sense — the open question is whether you **also** need **per-lane app scheduling** on top (multi-instance PBFT, or an app-layer delegate), which is a **product** choice for multi-lane deployments.

**Byzantine / slow primary:** surviving a bad primary is **view-change + timeouts**, not `amITheLeader` alone — see **Gap 7** and the LC research note **[`LC_PROTOCOL_RESEARCH_SUMMARY.md`](../LC_PROTOCOL_RESEARCH_SUMMARY.md)** (Java/BFT-SMaRt LC over lossy V2V). The **C++ ResDB** stack still needs **correct sim-time integration** for PBFT view timers and **reliable type-8 exchange** so VC can complete (same *class* of integration issues as in that doc, different codebase).

#### Gap 7: View-change / timeouts not OMNeT-simulation-time clean (Step 4c + PBFT LC debt)

ResDB’s internal **view-change** timers are not fully tied to **`simTime()`**. If a round stalls (slow or Byzantine primary), **wall-clock**–based timeouts can make **reproducibility** and **liveness** experiments hard to interpret — analogous to the timer/layering issues discussed for BFT-SMaRt LC in **[`LC_PROTOCOL_RESEARCH_SUMMARY.md`](../LC_PROTOCOL_RESEARCH_SUMMARY.md)**.

**Application fallbacks are not PBFT VC:** `stop_sign_timeout_msg_` and `consensus_timeout_msg_` **release vehicles** at the app; they do **not** replace **PBFT leader election** after a Byzantine primary.

**Required direction:** Full **Step 4 time virtualization** — consensus clocks driven by **`SimTimeProvider::NowUs()`** / OMNeT ticks — plus verification that **type-8** delivery allows replicas to complete **view-change** under RF (see **Success milestones** §4 below).

#### Gap 8: Retransmission — ARRIVAL_CERT vs announce/echo

**Implemented (type 5):** **`cert_retry_timer_`** rebroadcasts the assembled **ARRIVAL_CERT** on **`arrivalCertRetryIntervalSec`** until **`arrivalCertRetryMax`**, **`proposeAll()`**, order applied, or first **type 8** from current primary (`enableArrivalCertRetries` in NED). This addresses **leader missing a cert on the radio** without waiting for the full periodic announce cycle.

**Still optional / not done (types 1 / 4):** V2V **`retxCheckTimer`**-style **app-layer** retransmit for **ARRIVAL_ANNOUNCE** and **ARRIVAL_ECHO** (per-message ACK / reliability queue) is **not** ported. Mitigations today: **`broadcastArrivalAnnouncement`** periodic self-message, MAC stagger, and cert retries improving visibility of others’ type-5 payloads.

#### Gap 9: `zombieFilter()` / departed vehicle guard not fully implemented

**V2VProxyModule behavior:** `zombieFilter()` checks `isDeparted` and returns early from all message handlers and broadcasts if the vehicle has left the intersection. Prevents stale vehicles from polluting the cert protocol in multi-epoch runs.

**Current ResDB behavior:** `current_phase_ == ConsensusPhase::DEPARTED` check exists in `broadcastArrivalAnnouncement()` but is not checked in `handleArrivalAnnouncement`, `handleArrivalEcho`, `handleArrivalCert`, or `onWSM`. The `vehicleHasClearedIntersectionTraCI()` function exists but is never called — the transition to **`DEPARTED`** never happens.

**Required fix:** Call `vehicleHasClearedIntersectionTraCI()` in `handlePositionUpdate()` when `current_phase_ == ConsensusPhase::EXECUTING` and transition to **`ConsensusPhase::DEPARTED`**. Gate all cert handlers on **`current_phase_ != ConsensusPhase::DEPARTED`**.

---

## Known Issues & Debt

Single-place architecture narrative (legacy Java vs ResDB, bridge PreVerify, parity table): **[`RESDBARCH.md`](RESDBARCH.md)**.

| Issue | Severity | Status | Notes |
|-------|----------|--------|-------|
| `ResdbOmnetSetTransport` callbacks not used by real PBFT | — | ✅ Resolved | PBFT uses `OmnetReplicaCommunicator` (no TCP) via callback table |
| `ServiceNetwork::Run()` blocks calling thread | — | ✅ Resolved | Bridge runs it in `ResdbOmnetRunServer` background thread |
| `smokeTestBroadcast` disabled in production | — | ✅ Resolved | `smokeTestBroadcast = false` in BFTOverV2VWithResilientDB |
| Protobuf unusable in Veins build on Nix+WSL2 | — | ✅ Resolved | Switched to packed C structs; `.pb.h/.pb.cc` removed |
| Zero TCP usage | — | ✅ Resolved | Socketless `ServiceNetwork`; PBFT outbound is callback-based |
| `proposeAll()` crash — payload too short | Critical | ✅ Fixed | `hdr.n_vehicles = 4` hard-coded; fixed to `n`; self-state upsert added |
| Cars overshoot intersection | Critical | ✅ Fixed | `getDistanceToIntersection()` now uses TraCI lane-end distance |
| `stopVehicle()` wrong TraCI API | High | ✅ Fixed | Now uses `getVehicleCommandInterface()->setSpeedMode(31)+setSpeed(0)` |
| Type-9 state announce (no lane/cert info) | High | ✅ Fixed | Replaced by full cert protocol (Types 1/4/5) |
| Bridge PreVerify (structural “8 checks”) | High | ✅ Fixed | IDs, dupes, size, `sim_time_us`, `leader_id`, `is_ambulance`, `cyber_status` validated |
| Cert collection uses direct `VehicleState` (no echo/f+1) | High | ✅ Fixed | Full ARRIVAL_ANNOUNCE → ECHO → CERT protocol ported |
| QUIET vehicle handling (timeout propose with missing certs) | High | ✅ Fixed | `proposeAll()` pads missing vehicles as `cyber_status=0` / `UINT64_MAX`; executor isolates them to singleton batches |
| Multi-car batch crossing (ConflictMatrix) | High | ✅ Fixed | `IsSafeToBatch()` + greedy packer in `IntersectionExecutor`; Veins clears by batch; `BFTOverV2VWithResilientDBBatch` config works |
| Ambulance lane flush priority | High | ✅ Fixed | Ambulance-lane vehicles sorted to front of work queue; can co-batch with safe non-QUIET partners; only QUIET vehicles are singletons |
| Wire format missing lane/direction/cyber_status | High | ✅ Fixed | `ResdbVehicleEntry` extended to 17 bytes; `ResdbOrderHdr`+`ResdbVehicleDecision` replace flat `int32_t[]` |
| `resumeVehicle()` delay scheduling (batch sequencing) | High | ✅ Fixed | `processOrders()` now uses `clearance_poll_msg_` to gate on TraCI clearance of all preceding-batch vehicles |
| Departed vehicle guard (`zombieFilter`) incomplete | Medium | ⚠️ Debt | `vehicleHasClearedIntersectionTraCI()` called in `handlePositionUpdate` for own vehicle; `DEPARTED` phase set. Multi-epoch reset still unimplemented. |
| Multi-epoch / multi-batch support | Medium | ⚠️ Debt | `current_epoch_` never increments; no `resetForNextRound()` equivalent |
| Ambulance certificate verification | Medium | ⚠️ Debt | `is_ambulance` trusted from announce; no `VehicleCert` crypto verify (Gap 4) |
| Java semantic checks 5–7 (lane order, cert-omission guard) | Medium | ⚠️ Partial debt | Lane/cyber_status now in wire format; QUIET singleton enforced by packer; cert-omission guard (check 7) still needs `collected_certs_` access inside PreVerify |
| PBFT global primary vs V2V lane-leader semantics | Low | ⚠️ By design | Single primary per view is expected for PBFT — see Gap 6 |
| PBFT view-change / timers vs `simTime()` | Medium | ⚠️ Debt | Wall-clock VC hurts reproducibility; see Gap 7 / Step 4 |
| ARRIVAL_CERT retries (type 5) | — | ✅ Implemented | `cert_retry_timer_` + NED `enableArrivalCertRetries` |
| ANNOUNCE/ECHO app-layer retx (types 1/4) | Low | ⚠️ Optional debt | No per-message retx; stagger + periodic announce + cert retries mitigate |
| `ResdbOmnetRemoveReplica` is a no-op stub | Low | ⚠️ Debt | Epoch reset / departed replica removal not implemented |

---

## ResDB success milestones (ordered roadmap)

Use this list to **order experiments and PRs** (each step assumes the previous works in simulation):

| # | Milestone | What “done” means |
|---|-----------|-------------------|
| **1** | **✅ Honest consensus, N = 4** | Cert protocol → `TriggerConsensus vehicles=4` → `Order_Decided_Time` / `processOrders` / TraCI resume on all replicas. **Done 2026-05-10.** |
| **2** | **✅ Ambulance lane flush + multi-car batches** | ConflictMatrix greedy packer in `IntersectionExecutor`; ambulance-lane gets flush priority; non-QUIET vehicles co-batch; QUIET vehicles isolated; Veins clearance poll gated on preceding batch. **Done 2026-05-10.** Run config: `BFTOverV2VWithResilientDBAmb` / `BFTOverV2VWithResilientDBBatch`. |
| **3** | **✅ Byzantine replicas (non-primary faults)** | `isByzantine` / `byzantineType` NED params; FALSE_LANE (wrong lane in announce), INVALID_SIG (garbage echo sig), EQUIVOCATOR (split-direction unicast announces). Config: `BFTOverV2VWithResilientDBByz` (node[3] Byzantine). **Done 2026-05-11.** |
| **4** | **Byzantine primary + view-change + sim-time timers** | VC must fire when primary is silent/equivocating; ResDB PBFT timers must use `SimTimeProvider::NowUs()` (not wall clock); VC messages flow as type-8 over radio; new primary detected by Veins and triggers proposeAll. **This is next.** |

**Coding / file layout when extending Veins:** **[`RESDB_CODING_GUIDE.md`](RESDB_CODING_GUIDE.md)** (keep helpers out of `ResDBIntersectionApp.cc`; mirror [`V2VProxyModule`](../veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.cc) organization).

---

## Build Sequence (after any change)

```bash
# If resdb_omnet_bridge.cc or .h changed:
cd /home/yash/DistributedSystemsforAVs/incubator-resilientdb
# Note: in some WSL setups Bazel default cache may be unwritable; force /tmp.
bazel --output_user_root=/tmp/bazel build //integration/omnet:resdb_omnet_bridge

# Always (picks up Veins .cc + new .so):
cd /home/yash/DistributedSystemsforAVs/veins-veins-5.3.1
make -j$(nproc)

# Run:
cd /home/yash/DistributedSystemsforAVs/fourway
runomnetnogui -c BFTOverV2VWithResilientDB
```

Runtime note: `fourway/omnetpp.ini` now sets `*.node[*].appl.useRadioTransport = true` so the Step-2 probe traverses the radio path.

---

## Next Session Entry Point

### Handoff bundle

Use **`5stepplan.md`** (this file) together with **[`RESDBARCH.md`](RESDBARCH.md)** (architecture, phases, parity) and **[`RESDB_CODING_GUIDE.md`](RESDB_CODING_GUIDE.md)** (where to put new code). The **Success milestones** section defines **goal order**.

---

### Current Status (2026-05-11)

**Done:**
- Milestone 1: Honest N=4 consensus — cert protocol → TriggerConsensus → executor → TraCI resume, all 4 vehicles crossing.
- Milestone 2: Ambulance lane flush + ConflictMatrix multi-car batching.
  - Wire format extended: `ResdbVehicleEntry` is now 17 bytes (added `lane`, `direction`, `position_in_lane`, `cyber_status`). `ResdbOrderHdr` has `n_batches`. Output is `ResdbVehicleDecision[]` (batch_index per car).
  - QUIET vehicles (no f+1 echoes by deadline): `cyber_status=0` / `sim_time_us=UINT64_MAX` → isolated to singleton batches.
  - Ambulance lane flush: ambulance-lane vehicles sorted to front of greedy packer queue, so they land in the earliest batches; non-QUIET vehicles (including ambulance) can share a batch with safe ConflictMatrix partners.
  - Veins-side clearance: `processOrders()` now polls ALL preceding-batch vehicles (TraCI) before resuming, not just one predecessor.
  - New omnetpp.ini configs: `BFTOverV2VWithResilientDBAmb` (ambulance scenario), `BFTOverV2VWithResilientDBBatch` (batch crossing scenario).
  - `analyze_log.py` updated: `RE_BATCH_ASSIGN` regex, `replica_batch_index` store, `batch_throughput_veh_per_s`, `batch_index_distribution` in metrics JSON.
- Milestone 3: Byzantine follower injection — `isByzantine` / `byzantineType` NED params; `is_byzantine_` / `byzantine_type_` members in `ResDBIntersectionApp`. Three fault types wired:
  - **FALSE_LANE**: `broadcastArrivalAnnouncement()` sets `ann.laneId = "BYZANTINE_FAKE_LANE"` before signing. Honest nodes' `verifyCarPosition()` fails → no echo → Byzantine car padded as QUIET.
  - **INVALID_SIG**: `sendArrivalEcho()` overwrites signed echo with `0xDE×4` garbage. `validateArrivalCert` rejects it via `CryptoAuth::verifyBytes`. With f=1, each car still gets 3 valid echoes from honest nodes.
  - **EQUIVOCATOR**: `broadcastArrivalAnnouncement()` unicasts DIR_LEFT to peers 0..N/2-1 and DIR_RIGHT to N/2..N-1 instead of broadcasting. Direction is not in the announce signing string, so same sig; honest peers' echo signatures diverge by direction.
  - New config: `BFTOverV2VWithResilientDBByz` (node[3] Byzantine, `byzantineType=1` default; change to 2 or 3 to test other types).

---

### Current Status (2026-05-11 cont.) — Milestone 4 infrastructure wired

**Milestone 4 code complete (not yet run):**
- `viewchange_manager.h`: `SetTimeoutLength(uint64_t)` public setter added (1 line).
- `resdb_omnet_bridge.h/.cc`: `ResdbOmnetSetVcTimeoutUs` + `ResdbOmnetForceViewChange` added.
  - `OmnetConsensusManagerPBFT::TriggerViewChange()`: lowers `timeout_length_` to 1ms, calls
    `MayStart()` + `AddComplaintTimer("omnet_force_<ts>")`, restores configured timeout.
  - `OmnetConsensusManagerPBFT::SetVcTimeoutUs()`: sets `timeout_length_` on ViewChangeManager.
- `ResDBIntersectionApp.h`: `BYZANTINE_SILENT_PRIMARY=4`, `BYZANTINE_BAD_PROPOSAL=5` enum values;
  `last_known_primary_`, `pbft_vc_timeout_sec_`, `vc_trigger_msg_` members.
- `ResDBIntersectionApp.cc`:
  - `initialize()`: reads `pbftVcTimeoutSec` param, calls `ResdbOmnetSetVcTimeoutUs`, seeds `last_known_primary_`.
  - `transport_poll_msg_` loop: polls `ResdbOmnetGetPrimary()` every 1ms; on change → if new primary == self and still in COLLECTING_CERTS → reset `propose_submitted_` and call `proposeAll()`.
  - `propose_timeout_msg_` handler: follower (non-primary) schedules `vc_trigger_msg_` for `pbftVcTimeoutSec` later.
  - `vc_trigger_msg_` handler: calls `ResdbOmnetForceViewChange()` if no order yet.
  - `processOrders()` + `finish()`: cancel `vc_trigger_msg_` on order delivery or teardown.
  - `proposeAll()`: SILENT_PRIMARY returns early; BAD_PROPOSAL corrupts `hdr.n_vehicles += 1` before TriggerConsensus.
- `ResDBIntersectionApp.ned`: `pbftVcTimeoutSec` param (default 3s); byzantineType comment updated.
- `fourway/omnetpp.ini`: `BFTOverV2VWithResilientDBByzPrimary` (type=4 SILENT) + `BFTOverV2VWithResilientDBByzPrimaryBadProp` (type=5 BAD_PROPOSAL).

**Next: build + run + verify.** See "Next Goal" section below and "Debugging guide" in §4g.

---

### Next Goal: Milestone 4 — Byzantine Primary + View-Change + Sim-Time Timers

**What it means:** The PBFT primary (node[0]) is faulty. Honest followers must detect the fault, run view-change (VC), elect a new primary, and complete consensus. This is the hardest milestone because it requires three pieces to work simultaneously:
1. ResDB’s internal VC timers driven by `SimTimeProvider::NowUs()` (not wall clock)
2. VC messages flowing over the radio as type-8 packets
3. Veins detecting the new primary and re-triggering `proposeAll()`

The Java BFT-SMaRt path used a custom STOP/NACK LC layer (`RequestsTimer`) for this. **We do not port that layer.** ResDB’s built-in PBFT view-change is the replacement — we just need to ensure it fires correctly under simulated time.

---

#### 4a. Byzantine primary injection (new fault types)

Extend `ByzantineType` in `ResDBIntersectionApp.h` with:

```cpp
BYZANTINE_SILENT_PRIMARY   = 4,  // primary never calls proposeAll — triggers VC
BYZANTINE_BAD_PROPOSAL     = 5,  // primary sends a malformed proposal (wrong n_vehicles / bad sig)
```

In `ResDBIntersectionApp.cc`:
- **`proposeAll()`**: if `is_byzantine_ && byzantine_type_ == BYZANTINE_SILENT_PRIMARY`, skip the `ResdbOmnetTriggerConsensus` call entirely (drop the proposal). Log `[BYZANTINE] r<id> SILENT_PRIMARY: suppressing propose`. Followers will time out and trigger VC.
- **`BYZANTINE_BAD_PROPOSAL`**: call `TriggerConsensus` with a payload where `hdr.n_vehicles` is set to `total_vehicles_ + 1` (fails PreVerify). Primary looks "active" but every proposal is rejected.

Add to `.ned`:
```ned
int byzantineType = default(0);  // now accepts 0-5
// 4=silent_primary  5=bad_proposal
```

Add to `omnetpp.ini`:
```ini
[Config BFTOverV2VWithResilientDBByzPrimary]
extends = BFTOverV2VWithResilientDB
description = "ResDB 4-vehicle; node[0] (primary) is silent — VC must elect node[1]"
*.node[0].appl.isByzantine   = true
*.node[0].appl.byzantineType = 4
```

---

#### 4b. Sim-time alignment for PBFT VC timers (Step 4 debt — critical path)

**The root issue:** ResDB’s `ViewChangeManager` and `ConsensusManager` use `GetCurrentTime()` for their request-timeout and VC-trigger deadlines. `GetCurrentTime()` returns `SimTimeProvider::NowUs()` when the provider is active — but this only works if `SimTimeProvider` is actually advancing. If sim time advances faster than wall clock (common in OMNeT++), VC fires correctly. If it advances slower, VC never fires.

**Verification first:** Before writing code, add a log line to `SimTimeProvider::NowUs()` and `ViewChangeManager::run()` (or equivalent) to confirm that VC timer checks are reading simulated time. Run `BFTOverV2VWithResilientDBByzPrimary` and look for VC trigger logs.

**If VC does not fire:**
- Check `incubator-resilientdb/platform/consensus/ordering/pbft/viewchange_manager.h` — does `CheckTimeout()` call `GetCurrentTime()` or `gettimeofday` directly?
- If direct `gettimeofday`: patch it to call `SimTimeProvider::NowUs()` instead (same fix pattern as Step 4).
- The `SleepForUs` override in the bridge already redirects `usleep` to sim-time waits; the timeout check path needs the same.

**Key files:**
- `incubator-resilientdb/platform/consensus/ordering/pbft/viewchange_manager.h/.cpp`
- `incubator-resilientdb/common/utils/sim_time_provider.{h,cpp}` (already exists from Step 4)
- `incubator-resilientdb/platform/consensus/ordering/pbft/commitment.cpp` — `CheckTimeout` may live here

---

#### 4c. VC messages over radio (type-8 path)

ResDB’s VIEW-CHANGE and NEW-VIEW messages are sent via `OmnetReplicaCommunicator` → transport callbacks → `sendBFTMessage` type-8. Verify this end-to-end:

1. Run `BFTOverV2VWithResilientDBByzPrimary` with `debugCertProtocol = true`.
2. Grep for `[OmnetComm] send type=VIEW_CHANGE` or equivalent — should see these from followers after timeout.
3. Grep `[ANN-RECV]` / `onWSM type=8` on the new primary — it should receive the VC messages and call `ResdbOmnetGetPrimary` to discover it is now primary.

**If VC messages are not transmitted:** The `OmnetReplicaCommunicator` may only be routing consensus-phase messages (PRE-PREPARE / PREPARE / COMMIT). Check whether ResDB sends VC messages through the same `SendMessage`/`BroadcastMessage` path. If VC uses a separate code path, wire it to the same transport callbacks.

---

#### 4d. Veins-side new-primary detection

Currently `proposeAll()` is guarded by `ResdbOmnetGetPrimary(handle) == replicaId_`. After VC, the primary changes. The new primary must re-run `proposeAll()` if certs are already collected.

**Implementation:**
- In the `transport_poll_msg_` polling loop in `ResDBIntersectionApp::handleSelfMsg()`, add a periodic check:
  ```cpp
  int current_primary = ResdbOmnetGetPrimary(resdb_server_handle_);
  if (current_primary != last_known_primary_) {
      last_known_primary_ = current_primary;
      if (current_primary == replicaId_ && 
          current_phase_ == ConsensusPhase::COLLECTING_CERTS &&
          (int)collected_certs_.size() >= total_vehicles_) {
          proposeAll();
      }
  }
  ```
- Add `int last_known_primary_ = 0;` member.
- `ResdbOmnetGetPrimary` already exists in the bridge API (Step 5d).

**Caveat:** `ResdbOmnetGetPrimary` reads the primary from the ResDB config/view state. After VC completes, this should return the new primary ID. Confirm by checking `consensus_manager_pbft.cpp::GetPrimaryId()`.

---

#### 4e. Java `RequestsTimer` / NACK — do we need it?

**Short answer: No.** ResDB’s PBFT has its own view-change protocol (VIEW-CHANGE / NEW-VIEW messages). The Java `RequestsTimer` with STOP + NACK was BFT-SMaRt–specific overhead. It does not need to be ported.

**However**, the `V2VViewChangeManager` design in Step 4c (this doc) describes an *application-level* fallback — an OMNeT++ self-message that fires when the cert-collection timeout expires and no order has arrived. This is analogous to Java’s `stop_sign_timeout_msg_` but more aggressive: instead of just releasing the vehicle, it could call a new bridge API `ResdbOmnetForceViewChange(handle)` to manually trigger VC.

This is a safety valve for when ResDB’s internal VC is slow (wall-clock drift). It is **optional** for the first M4 pass — try with ResDB’s native VC first; add the app-level trigger only if VC doesn’t fire reliably in simulation.

---

#### 4f. Bridge API additions needed

```c
// Manually trigger a view-change from the OMNeT++ side (optional safety valve)
int ResdbOmnetForceViewChange(void* handle);

// Block until ResDB has completed a view-change round (useful for test assertions)
int ResdbOmnetWaitForViewChange(void* handle, int64_t timeout_us);
```

These are in `resdb_omnet_bridge.h/.cc`. Add only if 4e is needed.

---

#### 4g. Success criteria for Milestone 4

| Test | Expected outcome |
|------|-----------------|
| `BFTOverV2VWithResilientDBByzPrimary` (silent primary) | Followers time out after `certCollectionTimeoutSec`; VC messages observed in type-8 traffic; node[1] becomes new primary; node[1] calls `proposeAll()`; all 4 cars cross. |
| `BFTOverV2VWithResilientDBByzPrimary` (bad proposal, type 5) | PreVerify rejects all of node[0]’s proposals; VC fires after repeated failures; new primary completes consensus. |
| `[VC-DEBUG]` log from `ViewChangeManager` | VC triggered at a sim-time that matches `certCollectionTimeoutSec` threshold (not a wall-clock artifact). |

---

#### Debugging guide

1. **VC never fires:** `SimTimeProvider::NowUs()` is not advancing. Check that `time_tick_msg_` is scheduled and `ResdbOmnetUpdateSimTimeUs` is being called. Add `[SIM-TIME] now_us=<X>` log inside `UpdateSimTimeUs` to confirm.
2. **VC fires but new primary doesn’t propose:** `ResdbOmnetGetPrimary` is returning stale value. Check if `GetPrimaryId()` in ResDB reads from current view state (post-VC) or from static config.
3. **VC messages not seen on radio:** `OmnetReplicaCommunicator` may only handle `BroadcastBatch` / `SendBatchMessage` but not `SendViewChange`. Audit `incubator-resilientdb/platform/networkstrate/replica_communicator.h` for which send paths are overridden.
4. **Simulation hangs:** Deadlock in ResDB’s VC state machine waiting on a condition variable that `SleepForUs` never unblocks. Check `SleepForUs` sim-time implementation — the wakeup predicate must trigger on time-update, not just at a fixed wall interval.

---

#### Key reference files

| File | Why |
|------|-----|
| `incubator-resilientdb/platform/consensus/ordering/pbft/viewchange_manager.h/.cpp` | Where VC is triggered and state-machined |
| `incubator-resilientdb/platform/consensus/ordering/pbft/commitment.cpp` | CheckTimeout / request timer |
| `incubator-resilientdb/common/utils/sim_time_provider.{h,cpp}` | Sim-time injection into ResDB |
| `incubator-resilientdb/integration/omnet/resdb_omnet_bridge.{h,cc}` | Bridge API; OmnetReplicaCommunicator send paths |
| `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc` | `proposeAll()` primary guard; poll loop; new-primary detection |
| `LC_PROTOCOL_RESEARCH_SUMMARY.md` | Java BFT-SMaRt LC analysis — analogous timing/radio issues, different codebase |

---

### Build Sequence (no change)

```bash
# 1. Bridge (if resdb_omnet_bridge.cc/.h changed)
cd /home/yash/DistributedSystemsforAVs/incubator-resilientdb
bazel --output_user_root=/tmp/bazel build //integration/omnet:resdb_omnet_bridge

# 2. Veins (always)
cd /home/yash/DistributedSystemsforAVs/veins-veins-5.3.1
make -j$(nproc)

# 3. Run (Byzantine follower — done)
cd /home/yash/DistributedSystemsforAVs/fourway
runomnetnogui -c BFTOverV2VWithResilientDBByz

# 4. Run (Byzantine primary — next)
runomnetnogui -c BFTOverV2VWithResilientDBByzPrimary
```

**Happy path logs (Byzantine primary scenario):**
```
[BYZANTINE] r0 SILENT_PRIMARY: suppressing propose
[ViewChange] replica 1 triggered VC for view=1 at simtime=<T>
[OmnetComm] broadcast VIEW_CHANGE view=1 from r1
[ResDB] new primary = 1
[ANN-BROADCAST] Replica 1 proposeAll() as new primary (view=1)
[EXECUTOR] callback fired epoch=0 n=4 ...
[METRICS 1] Order_Decided_Time: <T+delta>
```

**Debugging:**
1. **No `[ViewChange]` log from ResDB:** VC timer path uses wall clock, not sim time. Start with `4b` — verify `SimTimeProvider` is being used by `viewchange_manager`.
2. **`ResdbOmnetGetPrimary` returns 0 after VC:** `GetPrimaryId()` reads static config. Need post-VC view update. Check `consensus_manager_pbft.cpp`.
3. **New primary calls `proposeAll()` but certs are empty:** Cert collection state was tied to old epoch. May need to reset `collected_certs_` and re-enter `COLLECTING_CERTS` phase after VC.
