

# Project: Two-Phase Witnessed Consensus (TPWC) for V2V

## 1. Problem Context & Baseline Results

The project implements a Byzantine Fault Tolerant (BFT) consensus protocol for autonomous vehicle intersection management using **Veins (C++)** and **BFT-SMaRt (Java)** bridged via **JNI**.

### **Current Bottlenecks Identified:**

* **Network Congestion (Physical Layer):** At  nodes, the simulation experiences **97% packet loss**. This is due to the  communication complexity of BFT-SMaRt saturating the 802.11p (DSRC) radio channel with high **SNIR-based interference**.
* **Arrival Time Vulnerability:** The current system relies on self-reported arrival times. A Byzantine node can "time-travel" by reporting a fake early arrival ( instead of ) to jump the line.
* **Topology Ignorance:** There is no physical verification of lane position. A Byzantine car at the back of a lane could be authorized to move by the consensus, causing a physical collision with the car in front.

---


## 2. The Solution: TPWC Architecture

The solution decouples **Physical Truth Verification** (local sensing) from **Logical Agreement** (global ordering).

### **Phase 0: Admission & Physical Witnessing (C++ Layer)**

Before a car is allowed to enter a consensus round, it must obtain a **ReadyQC**.

FOR CLAUDE: we are mainly focusing on the last 2, arrival time and topology by having these Ready QCs. 


* **Gossip Protocol:** The car broadcasts its `ID`, `Lane`, and `ArrivalTime`, `position` 
* **Neighborhood Verification:** Nearby "Honest" cars use their simulated sensors (LiDAR/Radar) to verify the claim. They only sign the message if the car is physically present and no car is in front of it in that lane.
* ** Threshold:** The car must collect ** signatures** to form a `ReadyQC`. Because  is the max number of faults,  signatures guarantee that at least one honest node has verified the physical truth of the car’s state.

FOR CLAUDE: I think in this simulation in veins im not sure how to use sensors, but I think the TRacii /home/yash/veins-veins-5.3.1/src/veins/modules/application/traci provides a ground truth. For example if we were using our senssors to see that a car has gone away, I think tracii knows and can be used as a source of truth/sensor. Just look into this for that part. 

### **Phase 1: Proposal & Digest Consensus (Java Layer)**

The "BFTsmart leader collects the `ReadyQCs` and initiates the consensus.

* **Batching:** The leader selects a batch (e.g., the front 4 cars) based on the oldest verified timestamps in the `ReadyQCs`.
* **Digest Consensus:** To solve the 97% packet loss, BFT-SMaRt replicas communicate using **Hashes (Digests)** of the batch rather than full data. This significantly reduces "airtime" and SNIR interference on the radio channel.

FOR CLAUDE: This is important but not a focus, we might want to do this in a seperate PR/change, not this one. Lets look how hard the ReadyQC stuff is and go from there. 

### **Phase 2: Global Decision (The DecisionQC)**

* **The Quorum:** A  quorum must agree on the batch order.
* **The DecisionQC:** The resulting certificate (DecisionQC) is broadcast to all  nodes. It serves as the "Green Light." Any car moving without being in a verified `DecisionQC` is treated as a fault. 

for CLAUDE: This is simuialr to waht we have now, I dont think it even needs to be a QC, lets just propagate the decision back. The reason im saying this is because our byznatine cars dont move without a decision. 

### **Two BFT Rounds Every Time: View Then Order**

Every time we run **view consensus first**, then **ordering consensus**. It’s not horrible: C++ does all message passing (gossip, witness, ReadyQC) until we need agreement; then we do two BFT rounds.

1. **Consensus on the View (Replica Set):** BFT-SMaRt agrees who the valid participants are before starting an ordering decision. This handles churn, stale cars, Byzantine nodes trying to sneak into rounds, etc.

2. **Consensus on the Order (Who Goes Next):** Once the view is fixed, BFT-SMaRt agrees on the ordered batch of cars (e.g., 1 car or 4). The proposal is built from cars with valid ReadyQC from within that agreed-upon view only.

---

## 3. Detailed Implementation Plan

### **Module 1: Veins Application Layer (C++)** THIS IS mainly /home/yash/veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.cc 

* **`ReadyQC` Struct:** Define fields for `carId`, `laneId`, `epoch`, `verifiedArrival`, and a signature buffer.
* **Witness Handler:**
* `sendWtsRequest()`: Periodically broadcast arrival data if `ReadyQC` is incomplete.
* `onWtsRequestReceived()`: If sensor data confirms the requester is at the front of the lane, sign and return.
For claude: so for cars that are behind a car
Field,Type,Purpose
carId,uint32_t,Unique identifier for the vehicle.
laneId,uint8_t,"The specific lane ID (e.g., Northbound-Left)."
positionInLane,uint8_t,The car's index in the queue (0 = front).
verifiedArrival,float,The f+1 witnessed timestamp of arrival.
epoch,uint32_t,The current global intersection state/round.
sigBuffer,byte[],The f+1 signatures proving physical truth.
Safety Invariant: The deterministic sort function in your Java BFT-SMaRt layer will reject any proposal where a car with positionInLane > 0 is cleared before the car with positionInLane == 0 in the same lane.

* **Admission Control:** Maintain a `map<int, ReadyQC> verifiedPool`. Only cars in this pool are considered for consensus proposals.

### **Module 2: JNI Bridge (C++/Java)**

* **Invoke Consensus:** The Line Leader calls a JNI method `invokeConsensus(byte[][] batchHashes)`.
* **Update View:** When cars leave, C++ calculates the new  and  and sends them to Java to update the BFT-SMaRt replica configuration. (We have this done already)

### **Module 3: BFT-SMaRt Service (Java)**

* **`appExecuteOrdered`**:
* Verify the batch hashes against the global state - > THIS IS DONE IN /home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/src/main/java/bftsmart/demo/intersection/IntersectionServer.java
* Use a deterministic sort based on the `verifiedArrival` field within the ReadyQCs.


* **View Change Logic:** Trigger a view update after the authorized batch has physically cleared the intersection.
- For CLAUDE: a view change and the inital gossip is essentially the same, both need to get f+1 about who current is in the view, and position and then do consenus 
---

## 4. Maintenance: Churn & Dynamic Membership

* **Recalculation:** The system updates  as a function of  (e.g., ) to maintain a constant  Byzantine tolerance as the intersection clears.
* **Fairness:** The `Initial_Timestamp` in the `ReadyQC` persists across rounds. A car that arrived in Epoch 1 will always be prioritized over a car arriving in Epoch 5.

---

## Thoughts for claude 
Gossip and View change should basically be the same code. In the beginning, we have to do a view change f+1 messages to settle on a view. Then we need to do f+1 messages to validate positions and time, we have to do f+1 signatures Then we can do consensus using JNI bft. After consensus, we need to update the view and positions at the same time as well. So for example we would have cars say: This is the new view: 1,2,3,4. Get f+1 signs on the view, and then do consensus again.

**View change is done by consensus (BFT agrees on the new view) but driven by C++:** C++ gathers who has ReadyQC (same gossip/witness protocol), forms the proposed view (e.g. set of replica IDs with valid ReadyQCs). C++ then invokes BFT via JNI so that the *next* consensus round is “agree on this view.” The outcome of that consensus is the new round view; then the next consensus round is “order who goes” using that view. So: C++ proposes and drives (gossip → proposed view → trigger consensus); Java/BFT executes the view-change as an ordered decision.

Now they have decided on an order. Then (if there are cars remaining) they would once again do the same gossip thing—f+1 signatures—and decide on a going order.

We can assume that before we do our first consensus, we are doing a view change, just one where no one has QCs. 

So the way I imagine it, cars come to an intersection, They do message exchage and get f+1 signs. Once this happens, they can send join (they can join even before this, their proposals just wont get accepted) we can have a timeout thing like maybe after 5 seconds just propose. 

But the leader/deterministc function that we have (we used to sort by arrival time) can now sort by who has a readyQC that is valid and signed, and then arrival time or whoever has a QC that is the oldest etc. I know in BFTsmart we have to start the system with the total amount of nodes, 
but im not sure how to just include 4 of the nodes in consenuss and also how to do view change to remove people from the view...  We could do something like Admission Control: Maintain a map<int, ReadyQC> verifiedPool. Only cars in this pool are considered for consensus proposals.
 Simulate dynamic view membership in your own logic.

Start BFT-SMaRt with all nodes.

Only include eligible nodes (those with ReadyQCs) in the proposal and ordering logic.

Let others receive messages passively but never propose or vote unless “admitted”. Also the fact that we are passing cars in the intersection, we might be able to make them disapear or do something else... not *sure please look into this. 

---

## Thoughts/Notes for Claude 
All the message passing that isnt consensus can be done in c++.

What defines a "neighbor" for witnessing: I Was thinking just whoever is closest. I am working with the assumption that everyone can tell where everyone is? 

What happens if f+1 signatures can’t be obtained?
    I think at a certain time, eveyrone should just send a join request. If you dont have f+1 signigutres by that time, your proposal isnt getting considered

What if a car gains a ReadyQC after the batch is formed but before consensus ends?
    They would need to wait, they are not taking part of this round, and would have to get a new one once the view change is done 

How are stale ReadyQCs expired?
    Yeah these should have a TTL - we dont have to worry about this now, but its something to think about 


 Treat ReadyQC as a physical-layer admission certificate, not as a logical proposal.

Only BFT-SMaRt replicas holding active ReadyQCs participate in ordering, even though all nodes exist in the network.

Emphasize: ArrivalTime is only valid if verified, otherwise it's not used for ordering.












 Issue 1: parse data info fail (lines 258, 259, 264, 270, 273, 276)

  consensus_manager.cpp:171 fires for every delivered packet. The injection pipe works (rc=0), but ResDB's PBFT layer
  can't decode the payload as a valid BFT request. If the smoke test sends raw bytes (a "hello" probe), this is expected
   — it just means Step 3's wire format (BFTMessage type 8 wrapper) is not yet applied. Not a blocker for Step 2 itself,
   but worth noting so the handoff doc is clear: the smoke test proves the pipe exists, not that consensus flows.