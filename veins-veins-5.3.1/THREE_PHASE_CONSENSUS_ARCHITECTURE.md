# Three-Phase Witnessed Consensus Architecture (CORRECTED)

## Overview

This document describes the **corrected** architecture where View consensus goes through BFT-SMaRt consensus to achieve Byzantine agreement on group membership.

## Phase Flow

```
Phase 1a: Local Detection (V2V layer)
  ↓
Phase 1b: V2V Agreement Collection (V2V layer)
  ↓  
Phase 1c: View Consensus (BFT layer) ← **CRITICAL: Byzantine agreement**
  ↓
Phase 2: ReadyQC Collection (V2V layer)
  ↓
Phase 3: Order Consensus (BFT layer)
```

---

## Phase 1: View Consensus (WHO is at the intersection?)

### Phase 1a: Local Detection (C++ - V2V layer)

**File**: `V2VProxyModule.cc::initiateViewProposal()`

Each car uses TraCI sensors to detect visible vehicles:

```cpp
std::set<std::string> visibleCars = getVisibleVehicles(300.0);  // 300m DSRC range
```

**Output**: Local view (e.g., `{veh0, veh1, veh2, veh3}`)

---

### Phase 1b: V2V Agreement Collection (C++ - V2V layer)

**Purpose**: Collect f+1 proximity-based signatures that the view is physically valid.

1. **Broadcast view** (messageType=4):
   ```cpp
   broadcastViewProposal();  // Sends to all neighbors via V2V
   ```

2. **Neighbors validate** via `handleViewProposal()`:
   - Check: Do I see the same cars via TraCI?
   - If YES: Sign the view and send back (messageType=5)
   - If NO: Don't sign

3. **Collect f+1 signatures** via `handleViewAgreement()`:
   - When f+1 neighbors agree: move to Phase 1c

**Output**: View + f+1 V2V signatures proving physical validity

---

### Phase 1c: BFT Consensus (Java - BFT layer)

**Purpose**: Achieve Byzantine agreement on THE SAME view across all replicas.

**File**: `IntersectionServer.java::appExecuteBatch()` case `VIEW_PROPOSE`

1. **Submit to BFT** (C++ → Java via JNI):
   ```cpp
   submitViewToBFTConsensus(view, v2vSignatures);
   ```
   Format: `"VIEW_PROPOSE:proposerId:veh0,veh1,veh2:sig1|sig2|sig3"`

2. **Java validates**:
   - Check: Does proposal have f+1 valid V2V signatures?
   - Verify each signature is hash(carList + replicaId)
   
3. **BFT agreement**:
   - All replicas submit their views
   - Views with matching car lists are grouped together
   - When **2f+1 replicas propose the SAME view** → Consensus!

**Output**: `agreedView` - Byzantine-agreed set of cars at intersection

**Java Response**: `"VIEW_AGREED:veh0,veh1,veh2,veh3"`

---

## Phase 2: ReadyQC Collection (WHEN did they arrive?)

**Triggered after**: View consensus completes

**File**: `V2VProxyModule.cc::startReadyQCCollection()`

### Step 1: Broadcast Arrival Announcement (messageType=1)

Each car in the agreed view broadcasts:
```cpp
ArrivalAnnouncement {
    carId: "veh0"
    laneId: "N2C_0"
    positionInLane: 285.551214
    claimedArrivalTime: 5.600000
    epoch: 0
    signature: self-signed
}
```

### Step 2: Witness Validation (messageType=2)

Neighbors receive announcement via `handleArrivalAnnouncement()`:

1. **Verify via TraCI**:
   ```cpp
   verifyCarPosition(carId, laneId, positionInLane);
   ```
   - Is the car really at that position?
   - Is it on the correct lane?
   
2. **If valid**: Sign witness claim and send back:
   ```cpp
   WitnessSignature {
       witnessReplicaId: 1
       witnessTimestamp: 5.605908
       signature: hash(carId:lane:pos:arrival:epoch:witnessTime:witnessId)
   }
   ```

### Step 3: Assemble ReadyQC

After collecting f+1 witness signatures:
```cpp
ReadyQC {
    carId: "veh0"
    laneId: "N2C_0"
    positionInLane: 285.551214
    verifiedArrival: 5.605908  // Earliest witness timestamp
    epoch: 0
    signatures: [wit1, wit2, wit3, ...]  // f+1 witnesses
}
```

**Key Point**: ReadyQC proves "Car X was at position Y at time T" with f+1 sensor-verified witnesses.

---

## Phase 3: Order Consensus (WHAT order to pass?)

**File**: `IntersectionServer.java::appExecuteBatch()` case `ORDER_PROPOSE`

Uses:
- `agreedView` (from Phase 1)
- `verifiedCars` (ReadyQCs from Phase 2)

Sorts by `verifiedArrival` (witness timestamps, not self-reported):

```java
sortedCars.sort((a, b) -> {
    return Double.compare(a.verifiedArrival, b.verifiedArrival);
});
```

**Output**: `"veh0:POS:0;veh3:POS:1;veh2:POS:2;veh1:POS:3"`

---

## Key Architectural Differences

### ❌ Old (Broken) Approach:
```
ReadyQCs → accumulate until BATCH_SIZE → View = all with valid QCs
```
Problem: View membership wasn't Byzantine-agreed through consensus!

### ✅ New (Correct) Approach:
```
View (with f+1 V2V sigs) → BFT consensus → THEN collect ReadyQCs for agreed view
```
Benefit: View membership has Byzantine agreement before collecting timing data!

---

## Why Three Layers?

### Layer 1: V2V Signatures (Proximity-based)
- **Purpose**: Prove physical presence
- **Validates**: "I can see this car via sensors"
- **Tolerance**: Byzantine cars can't lie about positions (need f+1 honest witnesses)

### Layer 2: BFT Consensus (Agreement-based)
- **Purpose**: Achieve global agreement
- **Validates**: "We all agree on the same view/order"
- **Tolerance**: Up to f Byzantine replicas can propose wrong views, but 2f+1 honest replicas will outvote

### Layer 3: Deterministic Ordering
- **Purpose**: Fair, predictable traversal
- **Validates**: Same inputs → same outputs (no manipulation)
- **Tolerance**: Byzantine replicas can't change the sort order

---

## Byzantine Fault Tolerance Analysis

### View Phase (f=1, n=4):
- **Attack**: Byzantine replica proposes fake view `{veh0, veh99}` (veh99 doesn't exist)
- **Defense**: Honest replicas can't see veh99 via TraCI → won't sign → no f+1 V2V signatures → proposal rejected

### ReadyQC Phase:
- **Attack**: Byzantine car claims arrival time 0.0 (lie to go first)
- **Defense**: Need f+1 honest witnesses to sign the claim → witnesses verify via TraCI → if position doesn't match claimed time, they won't sign

### Order Phase:
- **Attack**: Byzantine replica proposes reversed order
- **Defense**: All honest replicas compute same deterministic order from ReadyQCs → 2f+1 honest > f Byzantine → correct order wins

---

## Message Types Summary

| Type | Name | Layer | Purpose |
|------|------|-------|---------|
| 0 | BFT_CONSENSUS | BFT | Java BFT-SMaRt protocol messages |
| 1 | ARRIVAL_ANNOUNCE | V2V | Car broadcasts arrival claim |
| 2 | WITNESS_RESPONSE | V2V | Neighbor sends witness signature |
| 3 | READYQC_COMPLETE | V2V | Car broadcasts completed ReadyQC (optional) |
| 4 | VIEW_PROPOSAL | V2V | Broadcast view to collect V2V signatures |
| 5 | VIEW_AGREEMENT | V2V | Neighbor sends V2V signature on view |

---

## Scalability (n=32)

### TraCI Visibility:
- `getVehicleIds()` returns ALL vehicles (omniscient)
- **Solution**: Filter by distance using `getVisibleVehicles(300.0)`
- 300m DSRC range can see ~30 cars (10m spacing)

### View Consensus:
- Need 2f+1 = 22 replicas to agree (for n=32, f=10)
- All must propose views within 300m proximity
- **Potential issue**: Cars at back of queue (>300m) won't be in view

### Recommendation:
- For n=32, use **rolling batches of 8-12 cars**
- Run multiple consensus rounds (batch 1, then batch 2, ...)

---

## Testing Checklist

- [ ] Phase 1a: Cars detect visible neighbors via TraCI
- [ ] Phase 1b: f+1 V2V signatures collected for matching views
- [ ] Phase 1c: BFT consensus agrees on same view across replicas
- [ ] Phase 2: ReadyQCs collected only for cars in agreed view
- [ ] Phase 3: Order consensus uses ReadyQC timestamps
- [ ] Byzantine car can't join view without physical presence
- [ ] Byzantine car can't manipulate arrival times (need f+1 witnesses)
- [ ] Byzantine replica can't change agreed order (outvoted by honest replicas)

---

## Next Steps

1. **Compile and test** the updated code
2. **Verify logs** show:
   - `"PHASE 1a: DETECTING VISIBLE CARS"`
   - `"PHASE 1b: COLLECTING V2V AGREEMENTS"`
   - `"PHASE 1c: SUBMITTING TO BFT CONSENSUS"`
   - `"[VIEW] ===== VIEW CONSENSUS COMPLETE ====="`
   - `"PHASE 2: STARTING READYQC COLLECTION"`
3. **Test Byzantine behavior**: Add fake cars to views, verify rejection
4. **Measure performance**: View consensus latency vs n

