# Java Side Handoff (BFT-SMaRt Integration)

This document is a migration handoff for replacing the Java/BFT-SMaRt side with a C++ library.

It focuses on:
- `bftsmart/demo/intersection`
- `bftsmart/communication/V2V`
- `bftsmart/tom/leaderchange/RequestsTimer`

## 1) What the Java side currently does

At runtime, each vehicle has:
- a C++ OMNeT++/Veins side (simulation, radio, cert collection, TraCI control),
- a Java BFT-SMaRt side (consensus logic and request verification),
- JNI glue between them.

Java responsibilities:
- run one BFT replica per vehicle (`IntersectionServer` inside `ServiceReplica`),
- validate leader proposals before followers vote (`OrderRequestVerifier`),
- compute deterministic schedules on the leader (`OrderScheduler`),
- inject and process BFT client requests (`TOMMessage`) through TOMLayer,
- notify C++ when an ORDER decision is committed,
- execute view-change timer and STOP/STOP_NACK behavior (`RequestsTimer`),
- maintain V2V reliability state in Java (`ReliableV2VMessaging`) used by C++ timers.

## 2) Exact C++ -> Java handoff points

### JVM + replica lifecycle

- C++ `V2VProxyModule::createOrAttachJVM()` creates/attaches the JVM and registers JNI methods.
- C++ `V2VProxyModule::startBFTSmartReplica()` instantiates `ServerRunner(replicaId, BATCH_SIZE)` and starts it in a Java thread.
- `ServerRunner.run()` creates `IntersectionServer`, which creates `ServiceReplica`.

### Triggering consensus from C++

- C++ `V2VProxyModule::triggerJoinViaJNI(request)` calls:
  - Java `ServerRunner.triggerJoinForReplica(int replicaId, String request)`
  - which calls `IntersectionServer.triggerConsensusRequest(request)`.

Request format passed from C++:
- `PROPOSE_ALL:<proposerId>:<vehicleStatesStr>:<perCarCerts>`

### Delivering leader broadcast to Java followers

- Java leader calls native `nativeBroadcastClientRequest(fromReplicaId, tomBytes)`.
- C++ enqueues/radio-broadcasts message type `9` (`CLIENT_REQUEST_V2V`).
- On follower receive, C++ calls Java static:
  - `IntersectionServer.deliverInjectedClientRequest(int toReplicaId, byte[] tomBytes)`
- Java deserializes `TOMMessage` and injects into follower TOMLayer:
  - `replica.getTOMLayer().requestReceived(tom, false)`.

### Other C++ -> Java calls

- C++ periodically syncs simulation time:
  - `SimulationClock.updateTime(double simTimeSeconds)`.
- C++ can trigger reliability cleanup:
  - `ReliableV2VMessaging.globalResetV2V(int[] departedReplicas)`.

## 3) Exact Java -> C++ handoff points (native methods)

Declared in `IntersectionServer` and implemented in `V2VJNIBridge.cc`:

- `notifyOrderDecided(int replicaId, String orderDecision)`
  - Java tells C++ consensus is committed; C++ resumes/moves vehicles.
- `notifyProposeAllConsensusMetric(int replicaId, int epoch, double wallSeconds)`
  - Java emits metric to C++.
- `notifyWipeComplete(int processId)`
  - Java reconfiguration/wipe completed; C++ proceeds with epoch flow.
- `nativeGetCertSnapshot(int replicaId) -> Set<String>`
  - Java pulls C++ `collectedCerts` key set (used by verifier Check 7).
- `nativeGetFreshProposePayload(int replicaId) -> String`
  - Java pulls fresh C++ cert-derived payload for rebuild after leader change.
- `nativeBroadcastClientRequest(int fromReplicaId, byte[] tomBytes)`
  - Java asks C++ to radio-broadcast serialized TOM request to followers.

Declared in `V2VNativeBridge` and implemented in C++:
- `nativeInit`, `nativeSendMessage`, `nativeShutdown`, `nativeIsRadioBusy`, `nativeWarmupPing`.

## 4) Core Java protocol flow (today)

1. C++ elects leader and builds cert payload from `collectedCerts`.
2. C++ calls Java trigger with `PROPOSE_ALL:<id>:<states>:<certs>`.
3. Java leader `IntersectionServer.sendConsensusRequest()`:
   - parses payload,
   - filters departed replicas,
   - computes schedule with `OrderScheduler.buildProposal(...)`,
   - forms full payload with `orderBag`,
   - builds `TOMMessage`,
   - self-injects into own TOMLayer,
   - serializes `TOMMessage` and calls `nativeBroadcastClientRequest(...)`.
4. Followers receive via C++ type-9 dispatch -> Java `deliverInjectedClientRequest`.
5. Followers run `OrderRequestVerifier` before WRITE vote.
6. On commit, `appExecuteBatch()` runs and Java calls `notifyOrderDecided(...)` back to C++.

## 5) `OrderRequestVerifier` checks that gate follower votes

For `PROPOSE_ALL`, followers enforce:
1. per-car cert signatures valid (f+1 for SIGNED cars),
2. no phantom vehicle IDs,
3. no duplicate vehicle IDs across batches,
4. collision-safe batch composition,
5. lane-order preservation,
6. QUIET vehicles must be singleton,
7. cert-omission guard against leader censorship (`nativeGetCertSnapshot`),
8. deterministic recomputation matches submitted schedule.

If these fail, follower rejects and leader change is expected.

## 6) V2V Java communication layer responsibilities

`V2VServersCommunicationLayer` + `ReliableV2VMessaging` currently provide:
- serialization/deserialization of BFT `SystemMessage` inside `V2VMessageEnvelope`,
- per-sender sequence ordering and buffering,
- ACK/piggyback ACK handling,
- retransmission bookkeeping,
- special unordered path for LC messages (`STOP`, `STOP_NACK`, `STOPDATA`),
- epoch cleanup/reset functions used by C++ side.

Important: retransmission scheduling is intentionally driven by C++ simulation timers; Java mainly stores state and produces pending retransmit envelopes.

## 7) `RequestsTimer` leader-change optimizations you added

Key behavioral changes in `RequestsTimer`:

- Sim-time driven logic:
  - timeout decisions and STOP cadence are based on `SimulationClock.currentTimeMillis()`, not wall-clock.
- Dual-timer model:
  - wall timer wakes often (`STOP_RETX_WALL_MS`) but actual STOP emits are gated by sim-time gap (`STOP_RETX_SIM_MS`).
- STOP strategy:
  - first `STOP_BLIND_EMITS` are blind STOP broadcasts,
  - then switch to `STOP_NACK` bitmask mode for missing peers.
- DoS/misbehavior control:
  - per-peer NACK reply cap (`NACK_REPLIES_PER_PEER`).
- Escalation debounce:
  - `tryClaimLCEpoch()` allows only one LC escalation in-flight, with sim-time liveness escape (`LC_ESCALATION_GAP_SIM_MS`).
- Transport-level heard set:
  - `recordHeardStop()` tracks STOP senders independent of LCManager purge timing.
- Cleanup hook:
  - `dropRegencyState(regency)` clears regency-local NACK/emit state and re-arms escalation.

These are not just performance tweaks: they prevent over-escalation and channel saturation at larger N.

## 8) File map (Java classes to replicate or retire)

Primary:
- `bftsmart/demo/intersection/IntersectionServer`
- `bftsmart/demo/intersection/ServerRunner`
- `bftsmart/demo/intersection/OrderRequestVerifier`
- `bftsmart/demo/intersection/OrderScheduler`
- `bftsmart/demo/intersection/ViewConsensusProtocol`

V2V bridge:
- `bftsmart/communication/V2V/V2VNativeBridge`
- `bftsmart/communication/V2V/V2VServersCommunicationLayer`
- `bftsmart/communication/V2V/V2VNativeReplicaConnection`
- `bftsmart/communication/V2V/ReliableV2VMessaging`
- `bftsmart/communication/V2V/SimulationClock`

Leader change:
- `bftsmart/tom/leaderchange/RequestsTimer`

## 9) Migration checklist for new C++ consensus library

To fully replace Java side behavior, your C++ replacement must cover:
- consensus proposal/commit pipeline equivalent to `TOMMessage` injection and delivery,
- follower-side proposal verification semantics (the 8 checks),
- deterministic scheduler equivalence (`OrderScheduler` logic),
- leader-change protocol semantics (including STOP/STOP_NACK behavior and debounce),
- epoch reset/reconfiguration behavior,
- decision callback into vehicle control (`notifyOrderDecided` equivalent),
- cert snapshot + fresh payload rebuild path (currently JNI pulls from C++).

If any of these are omitted, you may get behavior drift versus current benchmark results.

