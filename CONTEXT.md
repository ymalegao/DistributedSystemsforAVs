# Session Handoff — 2026-06-06

## Current Task
Adding no-firewall + cert-gate ablation experiments, and a crash scenario (`BYZANTINE_TAMPER_LANE`) to the ResDB V2V intersection system.

## What Was Done This Session

### New Byzantine types added (ResDBIntersectionApp.h):
- `BYZANTINE_FAKE_AMBULANCE = 6` — primary flips is_ambulance 0→1 in proposal (caught by PreVerify Check 10)
- `BYZANTINE_FAKE_AMBULANCE_FOLLOWER = 7` — follower claims ambulance without cert (caught by cert gate)
- `BYZANTINE_TAMPER_LANE = 8` — **PARTIALLY IMPLEMENTED, NOT YET IN CODE** — see below

### Files already modified:
- `incubator-resilientdb/integration/omnet/resdb_omnet_bridge.cc` — added `RESDB_NO_FIREWALL=1` env var that replaces all 10 PreVerify checks with a trivially-true lambda
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.h` — types 6,7,8 added to enum; `enableAmbulanceCertGate_` member added
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBDecision.cc` — types 6 (FAKE_AMBULANCE) injection added; **type 8 (TAMPER_LANE) NOT YET ADDED**
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBArrivalProtocol.cc` — type 7 follower injection + cert gate filter added
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.ned` — params: `enableAmbulanceCertGate`, `byzantineType` comment updated
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc` — reads `enableAmbulanceCertGate` par
- `fourway/run-resdb-simulation.sh` — flags: `--no-firewall`, `--leader-byz-type N`, `--follower-byz-type N`, `--cert-gate`; `BYZ_LEADER_TYPE`, `BYZ_FOLLOWER_TYPE`, `CERT_GATE_LINE` vars; 8th arg `follower_byz_type` added to `generate_random_scenario`
- `experiment_orchestrator.py` — scenarios 7-11 added (see below)

### Orchestrator scenarios now defined (codes 1-11):
- 1: No_Ambulance_Honest
- 2: Honest_Ambulance
- 3: ByzFollower_Ambulance
- 4: ByzLeader_Ambulance
- 5: ByzLeader_NoAmbulance
- 6: ByzFollower_NoAmbulance
- 7: NoFW_ByzFollower_FalseLane (FALSE_LANE + no firewall)
- 8: NoFW_ByzLeader_BadProposal (BAD_PROPOSAL + no firewall)
- 9: NoFW_ByzLeader_FakeAmbulance (FAKE_AMBULANCE primary type 6 + no firewall)
- 10: NoCertGate_ByzFollower_FakeAmbu (type 7 follower, cert gate OFF — attack succeeds)
- 11: CertGate_ByzFollower_FakeAmbu (type 7 follower, cert gate ON — attack caught)
- **12: NoFW_ByzLeader_TamperLane — NOT YET ADDED TO ORCHESTRATOR**

## What Still Needs To Be Done

### 1. Implement BYZANTINE_TAMPER_LANE (type 8) in ResDBDecision.cc

Add after the BYZANTINE_FAKE_AMBULANCE block (around line 225):

```cpp
if (is_byzantine_ && byzantine_type_ == BYZANTINE_TAMPER_LANE) {
    // Crash attack: quiet the real S-lane car (1 suppression, within f — PreVerify Check 9
    // tolerates it) and reassign the E-lane car's lane to S.
    // Scheduler then sees N-STRAIGHT + "S"-STRAIGHT → kSafe {0,0,1,0} → same batch.
    // N car (going south) and E car (going west) cross simultaneously → CRASH.
    // Caught by PreVerify Check 10 (cert says lane=E, proposal says lane=S).
    // Without firewall: CRASH. With firewall: Check 10 rejects → view change → no crash.
    uint8_t* base = buf.data() + sizeof(ResdbProposeHdr);

    // Pass 1: quiet the real S-lane car, push its position to 99 so fake-S sorts first
    for (uint32_t i = 0; i < n; ++i) {
        ResdbVehicleEntry e;
        std::memcpy(&e, base + i * sizeof(ResdbVehicleEntry), sizeof(e));
        if (e.lane == 1 && e.cyber_status == 1) {  // S lane, SIGNED
            e.cyber_status     = 0;
            e.sim_time_us      = UINT64_MAX;
            e.position_in_lane = 99;  // sorts after fake-S so it doesn't block batching
            std::memcpy(base + i * sizeof(ResdbVehicleEntry), &e, sizeof(e));
            std::cout << "[BYZANTINE] r" << replicaId_
                      << " TAMPER_LANE: quieted real S replica " << e.replica_id << "\n";
            break;
        }
    }
    // Pass 2: change first non-N non-S SIGNED car to lane=S, position=1
    for (uint32_t i = 0; i < n; ++i) {
        ResdbVehicleEntry e;
        std::memcpy(&e, base + i * sizeof(ResdbVehicleEntry), sizeof(e));
        if (e.lane != 0 && e.lane != 1 && e.cyber_status == 1) {
            uint8_t orig = e.lane;
            e.lane             = 1;  // fake S
            e.position_in_lane = 1;
            std::memcpy(base + i * sizeof(ResdbVehicleEntry), &e, sizeof(e));
            std::cout << "[BYZANTINE] r" << replicaId_
                      << " TAMPER_LANE: replica " << e.replica_id
                      << " lane " << (int)orig << "→S(1) — N+E batch → CRASH\n";
            break;
        }
    }
}
```

### 2. Update ResDBIntersectionApp.ned byzantineType comment
Add `8=tamper_lane` to the comment.

### 3. Add scenario 12 to experiment_orchestrator.py

In `SCENARIO_SUBDIR`:
```python
"NoFW_ByzLeader_TamperLane": "no_fw_tamper_lane",
```
In `ANALYZE_SCENARIO`:
```python
"NoFW_ByzLeader_TamperLane": 5,
```
In `SCENARIO_BY_CODE`:
```python
12: "NoFW_ByzLeader_TamperLane",
```
In `SCENARIO_ORDER` tuple: add `"NoFW_ByzLeader_TamperLane"`

In `randomize_args_for_scenario`:
```python
if scenario_name == "NoFW_ByzLeader_TamperLane":
    return ["--randomize", str(n), "0", "--byzleader", "0",
            "--leader-byz-type", "8", "--no-ambulance", "--no-firewall"]
```

Update CLI help string to include `12=NoFW_ByzLeader_TamperLane`.

## Key Design Decisions

### Why TAMPER_LANE causes a crash (scheduler analysis):
- Scheduler `IsSafeToBatch(0,0,1,0)` = N-STRAIGHT + S-STRAIGHT → TRUE (entry in kSafe)
- Quieting real S car (veh1, pos=99) and making E car (veh2) appear as S (pos=1):
  - veh2 sorts BEFORE veh1 in "S lane" (pos=1 < pos=99) → no S-lane predecessor blocking it
  - N car (veh0) batches with fake-S (veh2, actually E): N-STRAIGHT + S-STRAIGHT → same batch
  - veh0 goes south, veh2 goes west → paths cross in intersection center → CRASH
- PreVerify Check 9: 1 QUIET suppression ≤ f → tolerated (doesn't catch it alone!)
- PreVerify Check 10: veh2 cert=lane E, proposal=lane S → MISMATCH → rejects (WITH firewall)
- Key thesis point: Check 9 alone insufficient; Check 10 is the specific check that prevents the crash

### Ablation table (the thesis argument):
| Defense | Attack | Result |
|---|---|---|
| Firewall ON | TAMPER_LANE (type 8) | No crash (Check 10 catches lane mismatch) |
| Firewall OFF | TAMPER_LANE (type 8) | **CRASH** (N+E batched, cross simultaneously) |
| Cert gate ON | FakeAmbu follower (type 7) | No wrong priority (cert gate rejects uncertified claim) |
| Cert gate OFF | FakeAmbu follower (type 7) | Wrong priority (attack succeeds) |

### Run scenario 12:
```bash
python experiment_orchestrator.py --config 4 8 --scenario 12 --reps 5
```
or directly:
```bash
./fourway/run-resdb-simulation.sh --randomize 4 0 --byzleader 0 --leader-byz-type 8 --no-ambulance --no-firewall -u Cmdenv -c FourVehiclesResDB
```

## Next Steps
1. Implement type 8 injection in ResDBDecision.cc (code above)
2. Add scenario 12 to orchestrator (code above)
3. Rebuild: `cd veins-veins-5.3.1 && make` and rebuild ResDB bridge via bazel
4. Run `--scenario 12` and verify crash appears in log (`[EXECUTOR]` shows N+E in same batch)
5. Run WITH firewall to confirm no crash (view-change fires instead)
