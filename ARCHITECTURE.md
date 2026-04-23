# System Architecture: V2V BFT Intersection Coordination

Reference document for paper diagrams. Covers the full cross-language stack, the unified-car model, all inter-component interfaces, and the PROPOSE_ALL protocol flow including the architectural shift from TCP/Netty to 802.11p V2V for client-request delivery.

---

## 1. High-Level System Model

### Unified-Car Architecture

Each simulated vehicle is a **unified unit** containing two co-located subsystems bound by JNI:

```
┌─────────────────────────────────────────────────┐
│                  Vehicle Node i                 │
│                                                 │
│  ┌───────────────────┐   ┌───────────────────┐  │
│  │   C++ Layer       │   │   Java Layer      │  │
│  │  (OMNeT++/Veins)  │◄──►  (BFT-SMaRt)     │  │
│  │                   │JNI│                   │  │
│  │  V2VProxyModule   │   │ IntersectionServer│  │
│  │  V2VArrivalProto  │   │ OrderRequestVerif.│  │
│  │  V2VReliability   │   │ OrderScheduler    │  │
│  │  V2VEpochPreempt  │   │ ViewConsensusProto│  │
│  │  V2VTraCI         │   │ BFT-SMaRt Core    │  │
│  └───────────────────┘   └───────────────────┘  │
│                                                 │
│  ┌─────────────────────────────────────────┐    │
│  │         802.11p Radio (Veins NIC)       │    │
│  └─────────────────────────────────────────┘    │
└─────────────────────────────────────────────────┘
```

**Key invariant:** All inter-vehicle communication (BFT consensus messages, PROPOSE_ALL, ARRIVAL_ANNOUNCE, ARRIVAL_ECHO, ARRIVAL_CERT, EXECUTING) traverses the 802.11p radio model. Intra-vehicle communication between C++ and Java is via JNI — zero simulated latency, zero channel load.

---

## 2. Component Inventory

### C++ Components (`veins-veins-5.3.1/src/veins/modules/bftsmart/`)

| File | Role |
|------|------|
| `V2VProxyModule.cc/.h` | Central OMNeT++ module. Owns timers, message queue, BFT lifecycle. Entry point for all sim events. |
| `V2VReliability.cc` | Message send/receive logic. `sendBFTMessage()` wraps every outbound V2V frame. `handlepreConsensusMessages()` dispatches inbound frames by type. |
| `V2VArrivalProtocol.cc` | Phase 1 (cert collection): ARRIVAL_ANNOUNCE, ARRIVAL_ECHO, ARRIVAL_CERT handling. Maintains `collectedCerts`. |
| `V2VOrderProtocol.cc` | Triggers PROPOSE_ALL submission to Java; handles ORDER decision callback. |
| `V2VEpochPreemption.cc` | Epoch boundary logic: detects new arrivals mid-batch, triggers wipe/reinit. |
| `V2VTraCI.cc` | TraCI bridge: physical position queries, intersection geometry, departure detection. |
| `V2VJVMLifecycle.cc` | JVM creation/attach, `registerJNINativeMethods()`, `startBFTSmartReplica()`, `syncTimeToJava()`. |
| `V2VJNIBridge.cc` | All `JNIEXPORT` function bodies. Implements outbound JNI calls (notify callbacks) and inbound native methods (`nativeBroadcastClientRequest`, `nativeGetCertSnapshot`, etc.). |

### Java Components (`bftsmart/library/src/main/java/bftsmart/`)

| Class | Role |
|-------|------|
| `demo/intersection/IntersectionServer` | BFT application replica. Receives C++ trigger, builds PROPOSE_ALL, self-injects TOMMessage, broadcasts via V2V. Delivers decisions to C++ via JNI. |
| `demo/intersection/OrderRequestVerifier` | BFT firewall. 8-check validator run by every follower before casting WRITE vote. Rejects malformed/Byzantine proposals. |
| `demo/intersection/OrderScheduler` | Deterministic schedule builder. Pure function: same cert set → same `OrderBag`. Used by leader (compute) and followers (re-execute for Check 8). |
| `demo/intersection/ViewConsensusProtocol` | Parses `vehicleStatesStr` and `perCarCerts`. Verifies f+1 ECDSA P-256 echo signatures per SIGNED car (Check 1). |
| `demo/intersection/ServerRunner` | Wraps `IntersectionServer` in a background daemon thread; provides `isReplicaReady()` / `triggerJoinForReplica()` static API for C++ polling. |
| `tom/ServiceReplica` | BFT-SMaRt replica shell. Owns `TOMLayer`, `ClientsManager`, `Synchronizer`. Exposes `getTOMLayer()` for direct TOMMessage injection. |
| `tom/core/TOMLayer` | Core BFT engine. Batches client requests, drives Paxos-like consensus rounds. Batch timeout gated on `SimulationClock.currentTimeMillis()`. |
| `tom/leaderchange/RequestsTimer` | Leader-change timer. Emits STOP on timeout; handles STOP_NACK for targeted resend. Dual-timer: wall-clock poll + sim-time gate. |
| `tom/core/Synchronizer` | Coordinates STOP → STOPDATA → SYNC leader-change protocol. |
| `communication/V2V/V2VServersCommunicationLayer` | Replaces TCP between replicas with V2V JNI calls. All BFT consensus messages (PROPOSE, WRITE, ACCEPT, COMMIT, STOP, STOPDATA, SYNC) route here. |
| `communication/V2V/SimulationClock` | Shared sim-time source. Updated every ~20ms wall-time via `syncTimeToJava()` JNI. `currentTimeMillis()` returns OMNeT++ sim time. |
| `communication/V2V/ReliableV2VMessaging` | Reliability layer over V2V: sequence numbers, ACKs, retransmissions for BFT consensus messages. |

---

## 3. JNI Interface

### C++ → Java calls (outbound from simulation)

| C++ call site | Java method | Purpose |
|---------------|-------------|---------|
| `V2VJVMLifecycle.cc` `triggerJoinViaJNI()` | `ServerRunner.triggerJoinForReplica(int, String)` | C++ hands PROPOSE_ALL payload to Java leader |
| `V2VJVMLifecycle.cc` `syncTimeToJava()` | `SimulationClock.updateTime(double)` | Sync OMNeT++ sim time into Java ~20ms wall |
| `V2VJVMLifecycle.cc` `triggerGlobalResetViaJNI()` | `ReliableV2VMessaging.globalResetV2V(int[])` | Notify Java of departed replicas at epoch boundary |
| `V2VJNIBridge.cc` `handleClientRequestBroadcast()` | `IntersectionServer.deliverInjectedClientRequest(int, byte[])` | Deliver type-9 V2V frame (leader's PROPOSE_ALL TOMMessage) to follower's TOMLayer |

### Java → C++ calls (inbound native methods)

| Java declaration | C++ implementation | Purpose |
|------------------|--------------------|---------|
| `native void notifyOrderDecided(int, String)` | `V2VJNIBridge.cc` | Java delivers ORDER decision; C++ resumes vehicle movement |
| `native void notifyWipeComplete(int)` | `V2VJNIBridge.cc` | Java signals epoch wipe done; C++ re-triggers ARRIVAL_ANNOUNCE |
| `native void notifyProposeAllConsensusMetric(int, int, double)` | `V2VJNIBridge.cc` | Wall-clock BFT latency metric per epoch |
| `native Set<String> nativeGetCertSnapshot(int)` | `V2VJNIBridge.cc` | Pull C++ `collectedCerts` key set into Java (Check 7 censor guard) |
| `native String nativeGetFreshProposePayload(int)` | `V2VJNIBridge.cc` | Pull fresh `vehicleStatesStr:perCarCerts` from C++ ground truth (EP5 rebuild path) |
| `native void nativeBroadcastClientRequest(int, byte[])` | `V2VJNIBridge.cc` | Leader broadcasts serialized TOMMessage via 802.11p V2V (type=9) |

---

## 4. V2V Message Type Registry

All inter-vehicle frames are `BFTMessage` OMNeT++ packets with a `messageType` field.

| Type | Name | Sender | Receiver | Layer |
|------|------|--------|----------|-------|
| 0 | `BFT_CONSENSUS` | Any replica | All replicas | Java (BFT-SMaRt): PROPOSE, WRITE, ACCEPT, COMMIT, STOP, STOPDATA, SYNC |
| 1 | `ARRIVAL_ANNOUNCE` | Arriving car | All replicas | C++: signals physical arrival at intersection |
| 4 | `ARRIVAL_ECHO` | Witnessing replica | Announcing car | C++: f+1 physically-verified echo signatures per car |
| 5 | `ARRIVAL_CERT` | Arriving car | All replicas | C++: assembled f+1-echo certificate broadcast |
| 7 | `EXECUTING` | Crossing car | All replicas | C++: signals batch-crossing started; locks new arrivals |
| 9 | `CLIENT_REQUEST_V2V` | BFT leader | All follower replicas | Java→C++→V2V→C++→Java: PROPOSE_ALL TOMMessage broadcast |

---

## 5. PROPOSE_ALL Protocol Flow (Single-Round BFT)

This is the core contribution. One BFT round = one intersection scheduling decision.

### Phase 1: Certificate Collection (C++ layer)

```
Car i arrives → ARRIVAL_ANNOUNCE broadcast (type=1)
     │
     ▼ (at each witnessing replica j)
ARRIVAL_ECHO sent back to car i (type=4)
  └─ contains: carId, lane, pos, dir, isAmb, j's ECDSA P-256 signature
     │
     ▼ (at car i, after collecting f+1 echoes)
ARRIVAL_CERT assembled and broadcast (type=5)
  └─ contains: carId metadata + f+1 echo signatures
     │
     ▼ (at all replicas)
collectedCerts[carId] = ArrivalCert stored in C++
```

Cert collection ends at `certCollectionTimeoutSec` (default 1.5s sim). Cars without f+1 echoes by timeout are marked **QUIET** (no physical witness quorum).

### Phase 2: PROPOSE_ALL Submission (C++ → Java)

```
Leader car (lowest-id active replica)
     │
     ▼
C++: buildFreshProposePayload()
  └─ serializes collectedCerts → "vehicleStatesStr:perCarCerts"
     vehicleStatesStr: "carId|lane|pos|dir|isAmb|cyberStatus;..."
     perCarCerts:      "carId~replicaId,pubkeyHex,sigHex|...;..."
     │
     ▼
C++: triggerJoinViaJNI()
  └─ calls ServerRunner.triggerJoinForReplica(leaderId, "PROPOSE_ALL:<id>:<states>:<certs>")
     │
     ▼
Java: IntersectionServer.triggerConsensusRequest(request)
  └─ daemon thread "propose-<id>" spawned
```

### Phase 3: Schedule Computation + BFT Submission (Java leader)

```
Java leader: sendConsensusRequest(request)
     │
     ├─ Parse vehicleStatesStr → List<VehicleState>
     ├─ Filter departed replicas
     ├─ OrderScheduler.buildProposal(viewMap, epoch, waitRegistry)
     │    └─ Deterministic schedule: priority queue by (isAmb, waited, positionInLane)
     │       Collision-safe batching via ConflictMatrix
     │       QUIET cars → exclusive singleton batch (Leader Rejection Rule)
     ├─ Serialize: "PROPOSE_ALL:<id>:<states>:<certs>:<orderBagStr>"
     │
     ├─ Step 1 — Self-delivery (intra-vehicle, instant, no radio):
     │    TOMMessage tom = new TOMMessage(clientId=leaderId+1000, session=epoch, ...)
     │    replica.getTOMLayer().requestReceived(tom, fromClient=false)
     │
     └─ Step 2 — V2V broadcast (inter-vehicle, traverses 802.11p):
          serialize(tom) → byte[]
          nativeBroadcastClientRequest(leaderId, tomBytes)
            └─ C++: enqueueBroadcastClientRequest() → PendingMessage{type=9}
                 └─ processQueueTimer dequeues → sendBFTMessage(..., type=9)
                      └─ OMNeT++ sendDelayed() → 802.11p channel
```

### Phase 4: Follower Receipt + OrderRequestVerifier (Java followers)

```
802.11p frame arrives at follower car j
     │
     ▼
C++: handlepreConsensusMessages() → case 9: handleClientRequestBroadcast()
  └─ JNI: IntersectionServer.deliverInjectedClientRequest(j, tomBytes)
       └─ deserialize TOMMessage → replica.getTOMLayer().requestReceived(tom, false)
     │
     ▼
BFT-SMaRt: OrderRequestVerifier.isValidRequest(TOMMessage)
  ├─ Check 1: f+1 valid ECDSA P-256 echo signatures per SIGNED car
  ├─ Check 2: No phantom vehicleIds in schedule
  ├─ Check 3: No duplicate vehicleIds across batches
  ├─ Check 4: ConflictMatrix.isSafeToBatch() for every pair within a batch
  ├─ Check 5: Same-lane queue order preserved (front car → earlier batch index)
  ├─ Check 6: Leader Rejection Rule — QUIET car must be in singleton batch
  ├─ Check 7: Cert-omission guard — omittedCount ≤ f (tolerates channel loss; rejects Byzantine censorship)
  └─ Check 8: Deterministic re-execution — follower rebuilds schedule from proposal's vehicleStates
               and compares with submitted orderBagStr (detects computation fraud)
     │
     ▼  (if all 8 checks pass)
Follower casts WRITE vote → BFT-SMaRt consensus proceeds
```

### Phase 5: Decision Delivery (Java → C++)

```
BFT-SMaRt delivers ordered batch to all replicas
     │
     ▼
IntersectionServer.appExecuteBatch()
  └─ Parse PROPOSE_ALL decision → OrderBag (epoch, batches)
  └─ Extract agreed VehicleState per car
  └─ JNI: notifyOrderDecided(replicaId, "epoch:batch0car0,batch0car1;batch1car0;...")
     │
     ▼
C++: handleOrderDecision() → parseAndNotifyDecision()
  └─ Build pendingBatches[][]
  └─ Emit EXECUTING message (type=7) → locks new arrivals on all replicas
  └─ Resume first batch of cars via TraCI
```

---

## 6. Leader-Change Protocol

Triggered when `RequestsTimer` detects no progress within `STOP_RETX_SIM_MS` sim-time.

```
Follower replica detects stall
     │
     ▼
RequestsTimer: dual-timer pattern
  ├─ Wall-clock java.util.Timer polls every STOP_RETX_WALL_MS (200ms wall)
  └─ Gate: only emit STOP if SimulationClock.currentTimeMillis() - lastEmit ≥ STOP_RETX_SIM_MS (200ms sim)

STOP message → V2VServersCommunicationLayer → type=0 V2V frame
     │
     ▼  (STOP_NACK if peer missing a STOP it needs)
STOP_NACK (type=10, isolated from BFT quorum in MessageHandler)
  └─ Triggers targeted STOP resend (not a new STOP)
  └─ DoS cap: NACK_REPLIES_PER_PEER=10 resends per peer per regency
  └─ Reply is broadcast to all (802.11p physical broadcast efficiency)

2f+1 STOPs collected → Synchronizer: STOPDATA → SYNC
     │
     ▼
New leader installed → rebuildPendingProposals() (EP5 rebuild hook)
  └─ New leader calls getFreshProposePayload() via JNI
  └─ Fresh PROPOSE_ALL built from current C++ collectedCerts
     (censored cars from Byzantine predecessor are included)
```

**LC escalation debounce:** `tryClaimLCEpoch()` (synchronized) prevents duplicate escalations. One LC per epoch; `dropRegencyState()` (synchronized) resets the epoch flag when SYNC installs.

---

## 7. Architectural Change: TCP/Netty → V2V for PROPOSE_ALL

### Before (TCP/Netty path — removed)

```
Java leader                          Java followers (same JVM process)
     │                                      │
     │  ServiceProxy(clientId=leaderId+1000)│
     │──TCP loopback (Netty)──────────────►│ ClientsManager.requestReceived()
     │  CommunicationSystemClientSideFactory│
     │  NettyClientServerCommunicationSystem│
     │                                      │
     └── Bypasses 802.11p entirely          └── Zero radio latency/loss modeled
```

Problems:
- PROPOSE_ALL delivery had zero simulated radio latency — BFT timing was not representative
- Netty connected to all N replicas point-to-point; in reality PROPOSE_ALL is one broadcast
- `Thread.sleep(delayMs)` introduced wall-clock stagger that distorted sim-time measurements

### After (V2V broadcast path — current)

```
Java leader (car i)                     802.11p channel        Java follower (car j)
     │                                        │                       │
     │ Step 1: self-inject (intra-vehicle)    │                       │
     │ getTOMLayer().requestReceived(tom)      │                       │
     │ [instant, no radio]                    │                       │
     │                                        │                       │
     │ Step 2: V2V broadcast                  │                       │
     │ nativeBroadcastClientRequest()         │                       │
     │──JNI──►C++:enqueueBroadcast()         │                       │
     │         processQueueTimer              │                       │
     │         sendBFTMessage(type=9)─────►sendDelayed()             │
     │                                     802.11p frame─────────────►│
     │                                        │         C++:handlepreConsensus()
     │                                        │         case 9: handleClientRequestBroadcast()
     │                                        │         JNI: deliverInjectedClientRequest(j, bytes)
     │                                        │         getTOMLayer().requestReceived(tom, false)
     │                                        │                       │
     │                                        │              OrderRequestVerifier (8 checks)
     │                                        │              WRITE vote cast
```

Benefits:
- PROPOSE_ALL delivery now experiences realistic 802.11p propagation delay, channel contention, and packet loss
- Single broadcast frame (not N point-to-point TCP connections)
- Leader does not wait for replies — fully async; decision arrives via `appExecuteBatch`
- `Thread.sleep` stagger eliminated; C++ `sendDelayed` slot stagger governs timing

---

## 8. Simulation Time Architecture

OMNeT++ sim time is authoritative. Wall-clock usage is restricted to performance instrumentation.

```
OMNeT++ simulation loop
     │
     ▼  (every ~20ms wall, from retxCheckTimer)
V2VProxyModule::syncTimeToJava()
  └─ JNI: SimulationClock.updateTime(simTime().dbl())
       └─ SimulationClock.currentSimTimeMillis (volatile long) updated

Java code that needs time:
  ├─ RequestsTimer STOP gate         → SimulationClock.currentTimeMillis() ✓
  ├─ RequestsTimer LC debounce       → SimulationClock.currentTimeMillis() ✓
  ├─ TOMLayer batch timeout          → SimulationClock.currentTimeMillis() ✓  [fixed]
  └─ ClientsManager request timestamp→ SimulationClock.currentTimeMillis() ✓

Wall-clock (System.currentTimeMillis()) allowed only for:
  └─ experimentStartWall: absolute start reference for log offset reporting
  └─ consensusStartWall: wall-time BFT latency metric (not protocol logic)

C++ wall-clock (std::chrono::high_resolution_clock) allowed only for:
  └─ realOrderConsensusStart/End: wall-time performance instrumentation
```

---

## 9. Byzantine Fault Model

`N = 3f + 1` replicas tolerate up to `f` Byzantine faults simultaneously.

| N | f | Quorum (2f+1) |
|---|---|----------------|
| 4 | 1 | 3 |
| 12 | 3 | 7 |
| 16 | 5 | 11 |

### Byzantine threat taxonomy

| Type | Behavior | Defense |
|------|----------|---------|
| `BYZANTINE_FALSE_LANE` | Announces wrong lane in ARRIVAL_ANNOUNCE | ECDSA P-256 witness echo: f+1 independent physical observations required |
| `BYZANTINE_INVALID_SIG` | Attaches garbage signature bytes | Check 1: signature verification per echo |
| `BYZANTINE_EQUIVOCATOR` | Sends different epochs to different peers | Epoch field in echo; mismatched epochs rejected by cert assembler |
| Byzantine Leader (BL) | Omits cars from vehicleStatesStr | Check 7: follower cert-omission guard (`omitted > f` → reject) |
| Byzantine Leader (BL) | Wrong schedule computation | Check 8: follower re-executes deterministic schedule |
| Byzantine Leader (BL) | QUIET car co-scheduled | Check 6: Leader Rejection Rule |
| Byzantine Leader (BL) | Phantom vehicleIds in schedule | Check 2: all scheduled IDs must appear in vehicleStates |
| Byzantine Leader (BL) | Stalls (no PROPOSE_ALL) | RequestsTimer STOP → leader change within epoch |

EP5 (Termination) after Byzantine leader: `rebuildPendingProposals()` hook on new leader fetches fresh `vehicleStatesStr:perCarCerts` from C++ `collectedCerts` ground truth, ensuring censored cars are included in the rebuilt proposal.

---

## 10. Key Data Structures

### PROPOSE_ALL wire format

```
"PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>:<orderBagStr>"

vehicleStatesStr  = "carId|lane|posInLane|dir|isAmb|cyberStatus;..."
                    cyberStatus ∈ {SIGNED, QUIET}

perCarCerts       = "carId~replicaId,pubkeyHex,sigHex|replicaId,pubkeyHex,sigHex;..."
                    (f+1 entries per SIGNED car)

orderBagStr       = "epoch:batch0car0,batch0car1;batch1car0;..."
                    (batches are semicolon-separated; cars within batch are comma-separated)
```

### ARRIVAL_CERT wire format (C++ serialization)

```
carId:lane:pos:dir:isAmb:epoch:
  replicaId0:pubkeyHex0:sigHex0|
  replicaId1:pubkeyHex1:sigHex1|
  ...
  (f+1 entries)
```

Signature covers: `SHA256withECDSA` over UTF-8(`carId:lane:pos:dir:isAmb:echoingReplicaId`). Each echo is self-contained — full 65-byte uncompressed P-256 pubkey included so any receiver can verify without a key registry.

---

## 11. Files Changed in This Implementation Pass

### New behavior / architectural changes

| File | Change |
|------|--------|
| `IntersectionServer.java` | Removed `ServiceProxy`/TCP/Netty path. Added `nativeBroadcastClientRequest` native method + `deliverInjectedClientRequest` static entry point. `sendConsensusRequest` now self-injects + V2V-broadcasts TOMMessage. |
| `ServiceReplica.java` | Added `getTOMLayer()` accessor for direct TOMMessage injection. |
| `V2VReliability.cc` | Added `enqueueBroadcastClientRequest()` (type=9 queue entry). Added `handleClientRequestBroadcast()` (type-9 frame → JNI → follower TOMLayer). Added `case 9` dispatch in `handlepreConsensusMessages()`. |
| `V2VJNIBridge.cc` | Added `nativeBroadcastClientRequest` JNI function. |
| `V2VJVMLifecycle.cc` | Caches `IntersectionServer.deliverInjectedClientRequest` method ID in both JVM-creator path and attach path (all replicas). |
| `V2VProxyModule.h` | Added `messageType` to `PendingMessage` struct. Added `enqueueBroadcastClientRequest` decl. Added `intersectionServerGlobalClass` + `deliverInjectedClientRequestMethod` fields. |
| `V2VProxyModule.cc` | processQueueTimer uses `pending.messageType` (not hardcoded 0). |

### Bug fixes

| File | Bug | Fix |
|------|-----|-----|
| `TOMLayer.java:216,391` | Batch timeout used `System.currentTimeMillis()` — wrong under sim time acceleration | Replaced with `SimulationClock.currentTimeMillis()` |
| `RequestsTimer.java:179,188` | `HashMap` for `nackReplyCount` and `heardByRegency` not thread-safe | Replaced with `ConcurrentHashMap`; inner set uses `ConcurrentHashMap.newKeySet()`; NACK cap uses atomic `merge()` |
| `RequestsTimer.java:427` | `dropRegencyState()` not synchronized — race with `tryClaimLCEpoch()` | Added `synchronized` |
| `Synchronizer.java:1119` | `syncSentRegencies` grew without bound under repeated view-changes | Added `removeIf(r -> r < regency - 10)` after every add |
| `V2VProxyModule.cc:~117` | Destructor deleted `javaCallbackObject` global ref but not `clockClass` or `intersectionServerGlobalClass` | Added `DeleteGlobalRef` for all three |
| `V2VJNIBridge.cc:298` | `nativeGetCertSnapshot` used `hsClass` without null check after `FindClass` | Added null check, `ExceptionClear`, `DeleteLocalRef(hsClass)` |
| `V2VReliability.cc:62` | `GetMethodID` failure in `registerJavaCallback` left pending exception un-cleared | Added `env->ExceptionClear()` in failure branch |
| `V2VJVMLifecycle.cc` (multiple) | `AttachCurrentThread` in temporary call sites never followed by `DetachCurrentThread` | Applied attach-track-detach pattern at all 4 temporary attach sites |
| `IntersectionServer.java:247` | `Thread.sleep(delayMs+10)` introduced wall-clock stagger in sim logic | Removed; daemon thread + name added |
| `IntersectionServer.java:74` | `consensusStartWall` not `volatile` — visibility gap between propose thread and delivery thread | Declared `volatile long` |

### Deleted

| File | Reason |
|------|--------|
| `IntersectionGateway.java` | Legacy TCP test harness (`ServerSocket` + two `invokeOrdered` calls). Never on any production call path. |
