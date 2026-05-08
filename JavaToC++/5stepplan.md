# AI Handoff Document: OMNeT++ → ResilientDB Migration
**Updated:** 2026-05-07  
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
// resdb_omnet_bridge.h — shared wire format
#pragma pack(push, 1)
typedef struct ResdbVehicleEntry {
    int32_t  replica_id;     // 4 bytes
    uint64_t sim_time_us;    // 8 bytes — simTime() at stop-zone entry
    uint8_t  is_ambulance;   // 1 byte
} ResdbVehicleEntry;          // 13 bytes total

typedef struct ResdbProposeHdr {
    uint32_t epoch;
    int32_t  leader_id;
    uint64_t propose_sim_time_us;
    uint32_t n_vehicles;
} ResdbProposeHdr;             // 20 bytes
// followed by n_vehicles × ResdbVehicleEntry

typedef struct ResdbOrderHdr {
    uint32_t epoch;
    uint32_t n_vehicles;
} ResdbOrderHdr;               // 8 bytes
// followed by n_vehicles × int32_t crossing_order
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
1. Parse `ResdbProposeHdr` + `ResdbVehicleEntry[]` from committed bytes
2. Sort: ambulances first, then `sim_time_us` ascending
3. Emit `ResdbOrderHdr` + `int32_t[]` crossing order
4. Call registered `ResdbOrderDecidedFn` callback

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

## Known Issues & Debt

| Issue | Severity | Notes |
|-------|----------|-------|
| `ResdbOmnetSetTransport` callbacks not yet used by real ResDB PBFT | Resolved | PBFT uses `OmnetReplicaCommunicator` (no TCP) via the callback table. |
| `ServiceNetwork::Run()` blocks the calling thread | Resolved | Bridge runs it in a background thread (`ResdbOmnetRunServer`). |
| Pre-vote verifier only checks count + duplicate IDs | Debt | Full 8-check Java `OrderRequestVerifier` port not yet done. |
| `ResdbOmnetRemoveReplica` is a no-op stub | Debt | Epoch reset / per-sender sequence state needs Step-5f implementation. |
| Cert collection uses direct `VehicleState` (no echo/f+1) | Debt | Full `ArrivalCertificate` echo mechanism not yet ported; currently collects states directly. |
| `smokeTestBroadcast` disabled in production config | Resolved | `smokeTestBroadcast = false` in `BFTOverV2VWithResilientDB`. |
| Protobuf cannot be used in Veins build on Nix+WSL2 | Resolved | Nix GCC `#include_next <stdio.h>` conflicts with `/usr/include`. Switched to packed C structs. `intersection.pb.h/.pb.cc` removed from Veins tree. |
| Ensure zero TCP usage | Resolved | Listener suppressed (socketless `ServiceNetwork`); PBFT outbound is callback-based. |

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

**All 5 steps are complete.** The simulation should run end-to-end with ResDB PBFT consensus.

**First run checklist:**
```bash
# 1. Build bridge
cd /home/yash/DistributedSystemsforAVs/incubator-resilientdb
bazel --output_user_root=/tmp/bazel build //integration/omnet:resdb_omnet_bridge

# 2. Build Veins
cd /home/yash/DistributedSystemsforAVs/veins-veins-5.3.1
make -j$(nproc)

# 3. Run
cd /home/yash/DistributedSystemsforAVs/fourway
runomnetnogui -c BFTOverV2VWithResilientDB
```

**What to look for in stdout:**
- `[METRICS r] Stop_Time:` — all 4 vehicles enter stop zone
- `[ResDB r0] TriggerConsensus rc=0` — primary (r0) submits consensus
- `[METRICS r] Order_Decided_Time:` — PBFT committed on all replicas
- `[METRICS r] Resume_Time:` — vehicles cross in decided order

**Next debugging priorities (if consensus doesn't fire):**
1. Check ResDB logs in `fourway/resdb_crypto/logs/` for PBFT activity (`server process: N`)
2. Verify type-9 state announces are reaching the primary (`collected_states_` size == 4 before propose)
3. If `TriggerConsensus` fires but `Order_Decided_Time` never appears: PBFT messages are being dropped — check type-8 radio delivery in `onWSM`

**Remaining debt (post-Step-5):**
- Full 8-check pre-vote verifier (5b)
- `ResdbOmnetRemoveReplica` implementation (5f)
- Full arrival cert echo mechanism (f+1 signatures)
- Multi-batch / multi-epoch support (currently epoch is always 0)
