# Two-Phase Witnessed Consensus - Implementation Status

## 🔑 Key Design Points (Important!)

### ReadyQC is needed for BOTH VIEW and ORDER consensus!

**Common Confusion:** "Do we need ReadyQC for VIEW consensus, or just for ORDER (batch) consensus?"

**Answer:** ReadyQC is needed for **VIEW consensus**! Here's why:

1. **ReadyQC Collection (V2V gossip - no BFT yet):**
   - Car broadcasts arrival → collects f+1 witness signatures → forms ReadyQC
   - ReadyQC = **proof** that f+1 honest replicas verified this car is physically present
   - Without ReadyQC, Byzantine cars could claim "I'm here" without witnesses

2. **VIEW Consensus (BFT Round 1):**
   - **Purpose:** Agree on which cars have valid ReadyQCs (agree on membership)
   - Each replica proposes their ReadyQC: `"VIEW_PROPOSE:veh0:lane:pos:arrival:epoch:sig1|sig2|..."`
   - BFT validates all ReadyQCs (checks f+1 signatures, lane ordering invariant)
   - **Result:** `"VIEW_AGREED:veh0,veh1,...,veh15"` (16 cars with valid ReadyQCs)
   - **Why needed:** Without VIEW consensus, leader could propose a batch with cars others don't believe exist!

3. **ORDER Consensus (BFT Round 2):**
   - **Purpose:** Select 4 cars from the agreed view and decide crossing order
   - Uses the verifiedCars map (already validated in VIEW phase)
   - Sorts by verifiedArrival timestamp
   - **Result:** `"veh0:POS:0;veh1:POS:1;veh2:POS:2;veh3:POS:3"`

### Correct TPWC Flow:

```
1. Car approaches → broadcast arrival announcement
2. Collect f+1 witnesses → assembleAndBroadcastReadyQC()
3. Broadcast ReadyQC (type=3) to all cars → WAIT (don't trigger consensus yet!)
4. Collect ReadyQCs from other cars via handleReadyQCComplete()
5. When BATCH_SIZE (16) ReadyQCs collected → trigger VIEW_PROPOSE
6. Java validates all ReadyQCs → returns "VIEW_AGREED:veh0,veh1,..."
7. C++ receives VIEW_AGREED → triggers ORDER_PROPOSE
8. Java selects 4 cars → returns "veh0:POS:0;veh1:POS:1;..."
9. C++ schedules resumeVehicle() with calculated delays
```

---

## ✅ Completed - Java Side (IntersectionServer.java)

### 1. Data Structures
- ✅ `ReadyQCData` class with carId, lane, position, verifiedArrival, epoch, signatures
- ✅ `WitnessSignature` class with witnessReplicaId, signatureBytes, witnessTimestamp
- ✅ State variables: `verifiedCars`, `agreedView`, `viewPhaseComplete`, `orderPhaseComplete`

### 2. Request Handling
- ✅ Extended `Cmd.Type` enum with `VIEW_PROPOSE` and `ORDER_PROPOSE`
- ✅ **VIEW_PROPOSE handler**: Parses ReadyQCs, validates signatures, builds agreedView (BATCH_SIZE=16)
- ✅ **ORDER_PROPOSE handler**: Selects 4 cars from agreed view, sorts by verifiedArrival

### 3. Validation & Helper Methods
- ✅ `parseSignatures()` - Parses decimal-encoded signatures (no base64!)
- ✅ `validateReadyQC()` - Checks f+1 signatures and same-lane ordering invariant
- ✅ `verifyMockSignature()` - Hash verification with correct byte order (LITTLE_ENDIAN)
- ✅ `computeOrderBatch(4)` - Selects & sorts 4 cars by verifiedArrival for 4-way stop
- ✅ `resetForNextRound()` - Resets state for next consensus round

## ✅ Completed - C++ Side (V2VProxyModule.cc/h)

### 1. Serialization Functions (Raw Binary for V2V)
- ✅ `serializeArrivalAnnouncement()` / `deserializeArrivalAnnouncement()`
- ✅ `serializeWitnessResponse()` / `deserializeWitnessResponse()`
- ✅ `serializeReadyQC()` / `deserializeReadyQC()` - Binary format for V2V broadcast
- ✅ `serializeReadyQCToString()` - String format for JNI (decimal encoding, NO base64!)

### 2. Helper Functions
- ✅ `extractReplicaIdFromCarId()` - "veh0" → 0
- ✅ `signatureBytesToString()` / `stringToSignatureBytes()` - Decimal encoding for mock hashes
- ✅ `split()` - String splitting utility

### 3. Two-Phase Consensus
- ✅ `triggerViewConsensus()` - Proposes own ReadyQC (or NONE) for VIEW_PROPOSE
- ✅ `triggerOrderConsensus()` - Sends ORDER_PROPOSE (no data needed)
- ✅ `handleReadyQCComplete()` - Receives ReadyQCs from other cars, triggers VIEW when BATCH_SIZE reached
- ✅ `triggerJoinViaJNI(string)` - Updated signature to accept request string

### 4. Message Dispatcher
- ✅ Fixed `handlepreConsensusMessages()` case 3 to call `handleReadyQCComplete()` (not `assembleAndBroadcastReadyQC()`)

## ⚠️ Known Issues & TODOs

### 1. ✅ **COMPLETED: ServerRunner.triggerJoinForReplica updated**
```java
// ✅ New signature (Java):
public static void triggerJoinForReplica(int replicaId, String request)

// ✅ Calls IntersectionServer.triggerConsensusRequest(request)
```

**✅ C++ now passes request string:**
```cpp
jstring jRequest = env->NewStringUTF(request.c_str());
env->CallStaticVoidMethod(serverRunnerClass, triggerMethod, replicaId, jRequest);
env->DeleteLocalRef(jRequest);
```

**✅ Java parses request and handles VIEW_PROPOSE and ORDER_PROPOSE:**
- sendConsensusRequest() method added
- Automatic ORDER trigger after VIEW_AGREED
- Parses final decision and calls notifyVehicleCanGo()

### 2. **Consensus Response Handling**
Need to handle responses from Java:
- When Java returns `"VIEW_AGREED:veh0,veh1,...,veh15"` → trigger ORDER consensus
- When Java returns `"veh0:POS:0;veh1:POS:1;veh2:POS:2;veh3:POS:3"` → schedule vehicle resume

**Where to add:**
- Probably in the JNI callback from Java or in message handling
- Parse response string and take action

### 3. **Multi-Round Support**
After 4 cars cross:
- Detect via TraCI that cars have left
- Remove their ReadyQCs from `verifiedPool`
- Trigger new VIEW consensus

**Need to add:**
- Detection logic for when cars have crossed (TraCI position check)
- Call `resetForNextRound()` in Java (via JNI or automatically)
- Trigger new `triggerViewConsensus()`

### 4. **Active Replica Tracking**
**Important design clarification:**
- All 32 cars start as BFT replicas (n=32)
- After 4 cars leave: **only 28 active cars participate**
- Departed cars should **ignore consensus messages** (application-level inactive)
- Dynamic n: 32 → 28 → 24 → 20... as cars leave
- Dynamic f: f = (n_active - 1) / 3

**Implementation options:**
- Option A: Departed cars set a flag and ignore all consensus messages
- Option B: Track "active" set in C++ and only active cars participate
- Option C: Java tracks who is active based on VIEW consensus results

### 5. **SendBFTMessage Signature**
Current calls use:
```cpp
sendBFTMessage(replicaId, -1, payload, messageType)
```

Check if `sendBFTMessage()` signature accepts 4 parameters (with messageType).

## 📊 Design Summary

### View Management (Application-Level, NOT BFT Reconfiguration)
- **BFT-SMaRt layer**: Fixed n=32 replicas throughout (or n=initial count)
- **Application layer**: Dynamic `agreedView` (cars with valid ReadyQCs)
- No BFT-SMaRt reconfiguration needed!
- Cars without ReadyQCs or that have left become **inactive** at application level

### Signature Encoding (Optimized)
- **V2V messages**: Raw binary (`uint8_t payload[]`) - no overhead
- **JNI string format**: Decimal encoding of 8-byte hash - lightweight
- **NO base64 encoding** - saves 33% overhead

### Constants
- **BATCH_SIZE = 16**: View consensus collects up to 16 cars
- **4 cars selected**: Order consensus picks 4 for 4-way stop
- **f = (n-1)/3**: Byzantine fault tolerance

### Flow Per Round
```
1. Cars collect f+1 witness signatures → ReadyQC
2. Broadcast ReadyQC to all cars (V2V type=3)
3. Once 16 ReadyQCs collected → VIEW_PROPOSE consensus
4. Java validates, returns VIEW_AGREED with 16 car IDs
5. ORDER_PROPOSE consensus → Java selects 4 cars
6. Java returns final decision: "veh0:POS:0;veh1:POS:1;veh2:POS:2;veh3:POS:3"
7. 4 cars cross intersection
8. Detect 4 cars left → trigger new VIEW_PROPOSE (now ~12 remaining + new arrivals)
9. Repeat
```

## 🔧 Next Steps

### ✅ Just Completed (2026-02-04)
1. ✅ **Updated ServerRunner.java** to accept request string
2. ✅ **Added consensus response handler** in Java (sendConsensusRequest)
3. ✅ **Fixed C++ flow** - removed immediate ORDER trigger from assembleAndBroadcastReadyQC()
4. ✅ **Updated triggerJoinViaJNI()** to pass request string to Java

### 🚀 Next: Compile and Test!

**For testing with n=4 cars:**
1. **Change BATCH_SIZE to 4** in three files:
   - `V2VProxyModule.cc` line 19: `static const int BATCH_SIZE = 16;` → `4`
   - `IntersectionServer.java` line 63: `private static final int BATCH_SIZE =16;` → `4`
   - `ServerRunner.java` lines 17, 23, 25: `private static final int BATCH_SIZE = 16;` → `4`

2. **Compile:**
   ```bash
   cd /home/yash/veins-veins-5.3.1
   make -j8

   cd /home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library
   ./gradlew build
   ```

3. **Run test simulation (n=4):**
   - 4 cars approach intersection
   - Each broadcasts arrival → collects f+1=2 witnesses → forms ReadyQC
   - When 4 ReadyQCs collected → VIEW_PROPOSE consensus (all 4 participate)
   - VIEW_AGREED returns 4 cars → ORDER_PROPOSE consensus
   - ORDER selects all 4 cars in sorted order by verifiedArrival
   - Final decision: `"veh0:POS:0;veh1:POS:1;veh2:POS:2;veh3:POS:3"`
   - Cars resume with delays: 0s, ~2.5s, ~5s, ~7.5s

4. **What to check:**
   - [ ] Cars broadcast arrival announcements (type=1)
   - [ ] Neighbors send witness responses (type=2)
   - [ ] ReadyQCs broadcast after f+1 witnesses (type=3)
   - [ ] VIEW_PROPOSE triggered when 4 ReadyQCs collected
   - [ ] Java prints: `"[VIEW] ===== VIEW CONSENSUS COMPLETE ====="`
   - [ ] Java prints: `"[VIEW] Agreed view (4 cars): [veh0, veh1, veh2, veh3]"`
   - [ ] ORDER_PROPOSE automatically triggered
   - [ ] Java prints: `"[ORDER] ===== ORDER CONSENSUS COMPLETE ====="`
   - [ ] Java prints: `"[ORDER] Final decision: veh0:POS:0;veh1:POS:1;..."`
   - [ ] Cars resume with staggered delays
   - [ ] `notifyVehicleCanGo()` called with correct delays

### 🔮 After Testing (Future Work)
1. **Multi-round support** - detect when 4 leave, trigger new VIEW with remaining cars
2. **Inactive replica handling** - departed cars stop participating
3. **Increase to n=16** - test with BATCH_SIZE=16, ORDER selects 4 from 16
4. **Byzantine testing** - invalid ReadyQC rejection, same-lane ordering invariant

## 📝 Testing Checklist

- [ ] VIEW consensus with 16 cars (all get valid ReadyQCs)
- [ ] ORDER consensus selects 4 cars correctly
- [ ] 4 cars receive POS:0-3 and cross in order
- [ ] After 4 leave, new VIEW consensus with 12 remaining
- [ ] Byzantine car with invalid ReadyQC gets rejected
- [ ] Same-lane ordering invariant enforced
- [ ] Cars without ReadyQC can vote but don't get selected

