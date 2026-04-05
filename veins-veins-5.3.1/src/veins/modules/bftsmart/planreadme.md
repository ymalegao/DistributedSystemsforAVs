**

# BFT-V2V: A Byzantine Fault Tolerant Cyber-Physical Intersection Protocol

A Cyber-Physical System (CPS) for distributed, Byzantine Fault Tolerant (BFT) intersection scheduling for Connected and Autonomous Vehicles (CAVs). 

This project implements a decentralized consensus mechanism that allows autonomous vehicles to securely agree on intersection crossing orders using 802.11p WAVE networks. It specifically addresses CSMA/CA channel congestion, malicious (Byzantine) actors, emergency vehicle preemption, and the strict boundary between cryptographic agreement and physical reality.

---

## 🏗️ System Architecture & Communication Model

The system operates on two strictly isolated planes, synchronized via a custom JNI (Java Native Interface) bridge:

1. **The Physical/Network Plane (C++ / OMNeT++ / SUMO):** Handles physical vehicle movement, sensor detection, and simulated 802.11p radio transmissions subject to physical constraints (CSMA/CA collisions, 300m range limits).
2. **The Consensus Plane (Java / BFT-SMaRt):** Handles cryptographic validation, state-machine replication, and Byzantine fault tolerance.

**Communication Assumption (The Hybrid Network):** To overcome the physical limitations of 802.11p packet collisions during heavy cryptographic traffic, this architecture utilizes a hybrid network approach. The initial Discovery and View phase is conducted strictly over the ad-hoc 802.11p V2V channel to prove physical presence. Once the View is established, Replica-to-Replica consensus (PRE-PREPARE, PREPARE, COMMIT) is assumed to utilize a high-bandwidth, reliable backend (e.g., 5G C-V2X or localized RSU-assisted TCP) to achieve millisecond-level Total Order without exhausting the ad-hoc radio channel.

---

## 🚦 The Single-Phase Batched Protocol

This protocol maximizes throughput by executing a single broadcast phase over the radio channel, subsequently relying on internal backend consensus and parallel batch scheduling.

### Phase 1: Arrival & State Declaration (V2V Radio)
As vehicles enter the boundary, they broadcast an `ARRIVAL_ANNOUNCE` containing their complete state: `[Vehicle_ID, Lane, Position_In_Lane, Intended_Direction (Straight/Left/Right), isAmbulance]`. 
* **Priority Cryptography:** If a vehicle claims `isAmbulance`, it must attach a valid ECDSA certificate signed by a trusted `Emergency_CA`.

### Phase 2: Witness Collection & View Consensus (BFT-SMaRt)
To prevent location spoofing, a vehicle must gather $f+1$ witness signatures from neighboring vehicles confirming its physical presence. The BFT network agrees on the global **View** (a tamper-proof snapshot of all vehicles currently waiting).

### Phase 3: Leader Batch Scheduling
With the View secured, the designated Leader dynamically calculates the crossing order (`OrderBag`):
1. **Priority Flush:** The Leader locates the ambulance and identifies any "blocker" cars in its lane.
2. **Batch Formation:** Using a geometric `ConflictMatrix`, the Leader builds parallel batches. 
   * *Batch 1:* Blocker cars + non-conflicting parallel traffic.
   * *Batch 2:* The Ambulance + non-conflicting traffic.
   * *Batch 3+:* Remaining vehicles via standard fairness.

### Phase 4: The Byzantine Firewall (Follower Verification)
Standard BFT protocols guarantee *Total Order* but do not understand physical safety; a standard BFT network will happily agree to crash four cars together if the Leader proposes it. To bridge this gap, the `RequestVerifier` intercepts the Leader's `OrderBag` before Followers vote `PRE-PREPARE`. 

The proposal is instantly rejected (triggering a Leader rotation / View Change) if the Leader attempts to:
* **Drop Cars:** The bag must contain every vehicle present in the current View.
* **Cause Crashes:** The `ConflictMatrix` must confirm that no two cars in the same Batch cross paths.
* **Ignore Emergencies:** The ambulance and its blockers must be placed in the earliest possible batches.

### Phase 5: Physical Execution & Fallback (C++ / SUMO)
The committed `OrderBag` is sent back to C++ via JNI. C++ executes the batches sequentially. 
* **The Safety Net:** BFT only guarantees the *decision*, not physical compliance. If a car is scheduled to cross but stalls, C++ triggers a 3.0-second physical override, force-evicts the vehicle from the local state, and immediately triggers the next batch to prevent gridlock.

---

## 🛡️ Edge Cases & Threat Model Defenses

Peer-to-peer Cyber-Physical Systems face unique vulnerabilities where digital rules conflict with physical reality. Our protocol addresses these systematically:

### 1. Late Arrivals & Emergency Preemption
* **The Vulnerability:** An ambulance arrives *while* the Replicas are actively voting on an `OrderBag`.
* **The Defense:** We strictly **prohibit mid-round preemption or distributed aborts**, as they open the system to infinite Denial-of-Service (DoS) spam. A BFT consensus epoch executes on the scale of ~100 milliseconds. A late arrival simply buffers in the network layer. The current micro-batch clears, and the ambulance is immediately captured in the subsequent epoch. Delaying an emergency vehicle by 100ms introduces zero physical risk while eliminating complex rollback vulnerabilities.

### 2. The "Changed Mind" Vulnerability (Intent Forfeiture)
* **The Vulnerability:** A Byzantine or erratic human driver locks in a "Left Turn" during Phase 1 (influencing parallel batching), but physically decides to drive straight during Phase 5, risking a collision.
* **The Defense:** Directional intent is cryptographically bound to the initial `ARRIVAL_ANNOUNCE`. We do not allow mid-round trajectory updates or "intent caching." If a vehicle attempts a maneuver differing from its cryptographic promise, the physical collision avoidance systems (ADAS) intervene, and the vehicle **forfeits its execution window**. The 3.0-second timeout expires, the vehicle is evicted from the BFT state, and it must reverse, broadcast a new announcement, gather new witnesses, and re-enter the back of the queue. The protocol explicitly penalizes indecisive behavior to ensure determinism.

### 3. Localized Sybil Clusters
* **The Vulnerability:** A malicious actor surrounds themselves with $f+1$ colluding Byzantine vehicles to falsely vouch for an incorrect lane position.
* **The Defense / Assumption:** We assume Byzantine actors are randomly distributed. The system is secure as long as an attacker cannot form a localized physical majority ($f+1$ vehicles) within immediate sensor range of a specific intersection node.

---

## 🗄️ Core Data Structures

### `VehicleState`
The atomic unit of the View consensus.
```json
{
  "vehicleId": "veh_12",
  "lane": "N",
  "positionInLane": 1,
  "direction": "STRAIGHT",
  "isAmbulance": false
}
```

### `Batch` & `OrderBag`
The output of the consensus mechanism, designed for parallel execution.
```json
{
  "epoch": 42,
  "batches": [
    { "vehicleIds": ["veh_12", "veh_8"] },  // Cross simultaneously
    { "vehicleIds": ["veh_15"] }            // Crosses after Batch 0 clears
  ]
}
```

---

## 🛠️ File Structure & Refactor Map

* `/bftsmart/demo/intersection/IntersectionServer.java`: Core consensus logic, Leader scheduling algorithm, and View state management.
* `/bftsmart/demo/intersection/ConflictMatrix.java`: Static lookup utility for collision-free parallel batching.
* `/bftsmart/demo/intersection/OrderRequestVerifier.java`: The Byzantine Firewall implementation enforcing priority and safety rules.
* `/veins/modules/bftsmart/V2VProxyModule.cc`: Handles physical V2V radio broadcasts, $f+1$ witness gathering, JNI bridge triggers, and sequential batch execution via physical timers.