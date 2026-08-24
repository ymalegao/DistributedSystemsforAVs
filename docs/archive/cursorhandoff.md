# Handoff - Scenario 15 Late-Emergency Rollback

**Status as of the latest `/tmp/resdb-simulation.log`: not stable.**

The most recent run did **not** reach epoch 1 commit. It reached:

```text
ORDER(0) committed
CANCEL(0) committed
rollback discovery started
ORDER(1) was proposed as normal seq=3 consensus
epoch 1 active view was installed
seq=3 did not commit
```

Important correction: older notes in this file said the latest run succeeded with epoch 1 `N=16`. That was true for one earlier run, but it is not true for the current fresh log. The current log shows the replacement order was proposed with `N=15`, missing `veh16`, and then PBFT seq=3 stalled.

---

## Current Latest Log Facts

Fresh log path:

```bash
/tmp/resdb-simulation.log
```

### ORDER(0) worked

Epoch 0 order committed with the original 16 vehicles:

```text
38361:[EXECUTOR] OrderDecision: epoch=0 n_vehicles=16 n_batches=8 decisions=[
  veh=0 batch=0
  veh=1 batch=4
  veh=10 batch=5
  veh=11 batch=4
  veh=12 batch=6
  veh=13 batch=7
  veh=14 batch=6
  veh=15 batch=7
  veh=2 batch=1
  veh=3 batch=5
  veh=4 batch=2
  veh=5 batch=1
  veh=6 batch=0
  veh=7 batch=2
  veh=8 batch=3
  veh=9 batch=3
]
38412:[METRICS 15] Order_Decided_Time: 18.48 n_batches=8
...
39865:[METRICS 9] Order_Decided_Time: 18.505 n_batches=8
```

Batch 0 in that schedule is `veh0` and `veh6`.

### CANCEL(0) worked

Cancel consensus reached PBFT seq=2:

```text
48421:[COLLECTOR-COMMIT] seq=2 hash=omnet-tx-1 data_bytes=1398 certs=0
...
48899:[COLLECTOR-COMMIT] seq=2 hash=omnet-tx-1 data_bytes=1398 certs=0
```

Many old-epoch replicas committed the cancel directly:

```text
48928:[CANCEL-COMMIT] r6 cancelled_epoch=0 ... quorum=11 source=commit
48951:[CANCEL-COMMIT] r7 cancelled_epoch=0 ... quorum=11 source=commit
48998:[CANCEL-COMMIT] r8 cancelled_epoch=0 ... quorum=11 source=commit
49009:[CANCEL-COMMIT] r9 cancelled_epoch=0 ... quorum=11 source=commit
49023:[CANCEL-COMMIT] r0 cancelled_epoch=0 ... quorum=11 source=commit
49031:[CANCEL-COMMIT] r1 cancelled_epoch=0 ... quorum=11 source=commit
49076:[CANCEL-COMMIT] r10 cancelled_epoch=0 ... quorum=11 source=commit
49087:[CANCEL-COMMIT] r11 cancelled_epoch=0 ... quorum=11 source=commit
49098:[CANCEL-COMMIT] r14 cancelled_epoch=0 ... quorum=11 source=commit
49108:[CANCEL-COMMIT] r15 cancelled_epoch=0 ... quorum=11 source=commit
49119:[CANCEL-COMMIT] r2 cancelled_epoch=0 ... quorum=11 source=commit
49130:[CANCEL-COMMIT] r4 cancelled_epoch=0 ... quorum=11 source=commit
49141:[CANCEL-COMMIT] r5 cancelled_epoch=0 ... quorum=11 source=commit
49511:[CANCEL-COMMIT] r3 cancelled_epoch=0 ... quorum=11 source=commit
```

Some replicas learned/adopted the tombstone by gossip:

```text
49727:[CANCEL-COMMIT] r12 cancelled_epoch=0 ... source=gossip-adopted
49972:[CANCEL-COMMIT] r17 cancelled_epoch=0 ... source=gossip-adopted
50068:[CANCEL-COMMIT] r16 cancelled_epoch=0 ... source=gossip-adopted
50716:[CANCEL-COMMIT] r13 cancelled_epoch=0 ... source=gossip-adopted
```

### Epoch 1 proposal was incomplete in the latest run

r1 proposed epoch 1 at `voteN=15`:

```text
56416:[ROLLBACK-DISCOVERY] r1 snapshot reason=cert-stored mode=per_epoch signed=15 quiet=0 voteN=15 sceneN=15 expectedN=4 missingCerts= cancelled_epoch=0 new_epoch=1
```

The proposal entries were:

```text
56421:[PROPOSE-PACK] r1 entry rid=1
56422:[PROPOSE-PACK] r1 entry rid=10
56423:[PROPOSE-PACK] r1 entry rid=11
56424:[PROPOSE-PACK] r1 entry rid=12
56425:[PROPOSE-PACK] r1 entry rid=13
56426:[PROPOSE-PACK] r1 entry rid=14
56427:[PROPOSE-PACK] r1 entry rid=15
56428:[PROPOSE-PACK] r1 entry rid=17
56429:[PROPOSE-PACK] r1 entry rid=2
56430:[PROPOSE-PACK] r1 entry rid=3
56431:[PROPOSE-PACK] r1 entry rid=4
56432:[PROPOSE-PACK] r1 entry rid=5
56433:[PROPOSE-PACK] r1 entry rid=7
56434:[PROPOSE-PACK] r1 entry rid=8
56435:[PROPOSE-PACK] r1 entry rid=9
```

So the active epoch 1 proposal was:

```text
1,2,3,4,5,7,8,9,10,11,12,13,14,15,17
```

Missing from that proposal:

```text
16
```

Excluded from that proposal:

```text
0,6
```

`veh0` and `veh6` were batch-0 vehicles and later departed:

```text
61962:[DEPARTED] Replica 0 cleared intersection t=21.1
62032:[DEPARTED] Replica 6 cleared intersection t=21.7
```

`veh16` was not in r1's proposal, but r1 later learned about it through gossip:

```text
61776:[ANN-RECV] Replica 1 received ARRIVAL_ANNOUNCE from veh16 frameFrom=8 via=gossip carrier=8 at t=20.8882577685
61935:[ANN-RECV] Replica 1 received ARRIVAL_ANNOUNCE from veh16 frameFrom=12 via=gossip carrier=12 at t=20.988891054119
61988:[ANN-RECV] Replica 1 received ARRIVAL_ANNOUNCE from veh16 frameFrom=14 via=gossip carrier=14 at t=21.151280682922
61996:[ANN-RECV] Replica 1 received ARRIVAL_ANNOUNCE from veh16 frameFrom=13 via=gossip carrier=13 at t=21.214339228165
```

Other replicas had already or later reached `signed=16`:

```text
56403:[ROLLBACK-DISCOVERY] r16 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61832:[ROLLBACK-DISCOVERY] r15 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61837:[ROLLBACK-DISCOVERY] r11 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61842:[ROLLBACK-DISCOVERY] r6 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61847:[ROLLBACK-DISCOVERY] r9 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61852:[ROLLBACK-DISCOVERY] r5 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61857:[ROLLBACK-DISCOVERY] r4 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61862:[ROLLBACK-DISCOVERY] r2 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61867:[ROLLBACK-DISCOVERY] r7 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61872:[ROLLBACK-DISCOVERY] r3 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61880:[ROLLBACK-DISCOVERY] r14 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61904:[ROLLBACK-DISCOVERY] r0 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61914:[ROLLBACK-DISCOVERY] r10 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
61922:[ROLLBACK-DISCOVERY] r17 snapshot ... signed=16 voteN=16 sceneN=16 expectedN=4
```

### Active view installation worked, but for the incomplete proposal

The bridge installed the epoch 1 active view from the proposal:

```text
56469:[PBFT-NEW-REQ] primary=2 broadcasting PRE_PREPARE seq=3 hash=omnet-tx-2 view=1
56473:[ACTIVE-VIEW] mode=promoted epoch=1 seq=3 hash=omnet-tx-2 N=15 f=4 quorum=10 primary=r1 members=1,2,3,4,5,7,8,9,10,11,12,13,14,15,17
```

That means the active view code did install a forced epoch view. It installed `N=15` because that is what r1 proposed.

### Transport ordering fix helped

The latest log shows PRE-PREPARE went out before PREPARE for seq=3:

```text
56504:[TYPE8-DRAIN] r1 ... inner=TYPE_PRE_PREPARE(3) ... seq=3 ... phaseGap=0 delay=0.00568747
56505:[TYPE8-DRAIN] r1 ... inner=TYPE_PREPARE(4) ... seq=3 ... phaseGap=0.003 delay=0.00823941
```

Followers received seq=3 PRE-PREPARE:

```text
56510:[TYPE8-RECV] r0 ... TYPE_PRE_PREPARE(3) seq=3 ...
56513:[TYPE8-RECV] r8 ... TYPE_PRE_PREPARE(3) seq=3 ...
56606:[TYPE8-RECV] r2 ... TYPE_PRE_PREPARE(3) seq=3 ...
56609:[TYPE8-RECV] r7 ... TYPE_PRE_PREPARE(3) seq=3 ...
56612:[TYPE8-RECV] r5 ... TYPE_PRE_PREPARE(3) seq=3 ...
56615:[TYPE8-RECV] r4 ... TYPE_PRE_PREPARE(3) seq=3 ...
56618:[TYPE8-RECV] r3 ... TYPE_PRE_PREPARE(3) seq=3 ...
56621:[TYPE8-RECV] r9 ... TYPE_PRE_PREPARE(3) seq=3 ...
56624:[TYPE8-RECV] r6 ... TYPE_PRE_PREPARE(3) seq=3 ...
56627:[TYPE8-RECV] r10 ... TYPE_PRE_PREPARE(3) seq=3 ...
56630:[TYPE8-RECV] r12 ... TYPE_PRE_PREPARE(3) seq=3 ...
56634:[TYPE8-RECV] r11 ... TYPE_PRE_PREPARE(3) seq=3 ...
56637:[TYPE8-RECV] r14 ... TYPE_PRE_PREPARE(3) seq=3 ...
56640:[TYPE8-RECV] r15 ... TYPE_PRE_PREPARE(3) seq=3 ...
56643:[TYPE8-RECV] r17 ... TYPE_PRE_PREPARE(3) seq=3 ...
56646:[TYPE8-RECV] r16 ... TYPE_PRE_PREPARE(3) seq=3 ...
```

So the earlier race where PREPARE could arrive before PRE-PREPARE appears improved in this run.

### Seq=3 still did not commit

Only one replica reached PREPARE quorum and broadcast COMMIT:

```text
60310:[PBFT-COUNT] self=5 omnet_self=4 seq=3 type=PREPARE sender=8 omnet_sender=7 hash=omnet-tx-2 count=10 quorum=10 status_before=READY_PREPARE status_after=READY_COMMIT changed=1 forced=1
60312:[PBFT-BCAST-COMMIT] self=5 seq=3 hash=omnet-tx-2 sender=5 ret=1
```

There is no current-log evidence of:

```text
COLLECTOR-COMMIT seq=3
OrderDecision: epoch=1
```

The sim later enters unsupported rollback view-change logs:

```text
62076:[ROLLBACK-VC-UNSUPPORTED] r12 suppressed app forced view-change while cancel active ...
62077:[ROLLBACK-VC-UNSUPPORTED] r14 suppressed app forced view-change while cancel active ...
62247:[ROLLBACK-VC-UNSUPPORTED] r16 suppressed app forced view-change while cancel active ...
62254:[ROLLBACK-VC-UNSUPPORTED] r17 suppressed app forced view-change while cancel active ...
62255:[ROLLBACK-VC-UNSUPPORTED] r13 suppressed app forced view-change while cancel active ...
```

---

## What Was Changed During This Debugging Pass

This section records implementation changes that are currently visible in the working tree or were added during this debugging thread. Some files in the repo have unrelated dirty changes; this list focuses on rollback/PBFT/log-analyzer work.

### 1. Normal `proposeAll()` path for epoch 1 order

Goal:

```text
ORDER(0) -> CANCEL(0) -> ORDER(1)
```

with `ORDER(1)` using the same scheduling/proposal machinery as `ORDER(0)`.

Current tree evidence:

- `ResDBDecision.cc` has rollback order using normal proposal packing and logs `normal_proposeAll=1`.
- `trySubmitRollbackProposal(...)` in `ResDBRollbackProtocol.cc` drives the rollback proposal path.
- The epoch 1 proposal log uses `[PROPOSE-PACK]`, same as epoch 0.

Effect:

- The custom rollback scheduling path is no longer the intended active path for replacement ordering.
- The remaining rollback-specific part is installing the correct epoch view before/during normal PBFT consensus.

Spec alignment:

- This matches `rollbackv3.md` at the high level: after `CANCEL(e)`, the system discovers `V_{e+1}` and then runs `ORDER(e+1)`.
- It avoids treating rollback scheduling as a different algorithm.

Known issue:

- Normal `proposeAll()` will faithfully pack whatever the app currently believes the epoch 1 membership is. If discovery is incomplete at proposal time, the normal path will still propose an incomplete order.

### 2. Epoch-aware active view builder in the ResDB bridge

File:

```text
incubator-resilientdb/integration/omnet/resdb_omnet_bridge.cc
```

Main added function:

```cpp
BuildEpochOrderViewCandidate(...)
```

Behavior:

- For `hdr.epoch == 0`, it delegates to the existing anchored/static view behavior through `BuildToleratedFaultViewCandidate(...)`.
- For `hdr.epoch > 0`, it builds the active PBFT view from the proposal entries where:

```text
cyber_status == 1
sim_time_us != UINT64_MAX
```

- It sorts/checks duplicate voter ids.
- It requires the leader/proposer to be in the signed voter set.
- In per-epoch mode, it computes:

```text
f_epoch = min(configured_tolerated_f, floor((voteN - 1) / 3))
quorum = BftQuorumSize(voteN, f_epoch)
```

- In anchored mode, it requires:

```text
voteN >= 3 * old_f + 1
```

and logs `[ROLLBACK-UNAVAILABLE] mode=anchored ...` rather than silently falling back to static PBFT.

Logs added:

```text
[EPOCH-VIEW]
[EPOCH-VIEW-REJECT]
[ROLLBACK-UNAVAILABLE]
[ACTIVE-VIEW-REJECT]
```

Install paths:

- `InstallOmnetPendingForcedView(...)` for pending/new transaction requests.
- `InstallOmnetForcedViewForRequest(...)` for PRE-PREPARE requests.

Observed effect in latest log:

```text
56473:[ACTIVE-VIEW] mode=promoted epoch=1 seq=3 hash=omnet-tx-2 N=15 f=4 quorum=10 primary=r1 members=...
```

So the bridge view-install mechanism is active. It did not prevent the current failure because the proposal itself was missing `veh16`.

Spec alignment:

- This is the bridge-side implementation of `V_{e+1}` being a per-epoch active view with its own quorum.
- It preserves epoch 0 behavior and changes only epoch > 0 order view construction.

### 3. PBFT forced-view and quorum debug logs

Files:

```text
incubator-resilientdb/platform/consensus/ordering/pbft/omnet_forced_view.h
incubator-resilientdb/platform/consensus/ordering/pbft/message_manager.cpp
incubator-resilientdb/platform/consensus/ordering/pbft/commitment.cpp
incubator-resilientdb/platform/consensus/ordering/pbft/consensus_manager_pbft.cpp
```

Logs added or expanded:

```text
[ACTIVE-VIEW-INSTALL]
[ACTIVE-VIEW-FIND]
[PBFT-FORCED-CHECK]
[PBFT-QUORUM]
[PBFT-COUNT]
[PBFT-ADD-RESULT]
[PBFT-NEW-REQ]
[PBFT-NEW-REQ-PREVERIFY]
[PBFT-PREPREPARE-PREVERIFY]
[PBFT-PREPARE]
[PBFT-PREPARE-RESULT]
[PBFT-BCAST-COMMIT]
[PBFT-COMMIT]
[PBFT-COMMIT-RESULT]
```

Purpose:

- Show whether seq=3 is using `source=forced` or static quorum.
- Show forced view membership and quorum.
- Show PRE-PREPARE/PREPARE/COMMIT counts.
- Show exactly which replica transitions to `READY_COMMIT` or `READY_EXECUTE`.
- Show when a sender is ignored as inactive for a request.

Observed effect:

- These logs proved the latest seq=3 run used forced quorum:

```text
N=15 f=4 quorum=10 primary=r1
```

- They also showed the liveness failure:

```text
only self=5 reached PREPARE count=10 and broadcast COMMIT
```

### 4. PBFT phase-gap transport ordering

File:

```text
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBTransport.cc
```

Constants added:

```cpp
constexpr double kPbftPreparePhaseGapSec = 0.003;
constexpr double kPbftCommitPhaseGapSec = 0.006;
```

Behavior:

- For Type 8 ResDB messages:
  - `TYPE_PREPARE(4)` gets an added `0.003s` phase gap.
  - `TYPE_COMMIT(5)` gets an added `0.006s` phase gap.
- Logs now print `phaseGap=...`.

Why it was added:

- A prior failure mode had PREPARE arriving before PRE-PREPARE, so followers did not have the request/forced view installed when the PREPARE arrived.

Observed effect in latest log:

```text
56504 TYPE_PRE_PREPARE seq=3 phaseGap=0
56505 TYPE_PREPARE seq=3 phaseGap=0.003
```

This appears to have helped message ordering for seq=3, but it did not make seq=3 commit.

Spec alignment:

- This is not protocol logic from `rollbackv3.md`; it is simulation/transport stabilization so PBFT phases arrive in a sane order under the OMNeT bridge.

### 5. Recallability changed for vehicles waiting on prior batch

File:

```text
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBRollbackProtocol.cc
```

Function:

```cpp
isRecallable()
```

Change:

- Vehicles that are not in/past the conflict box and are waiting for prior-batch clearance can be treated as recallable even if they are not fully stopped.

Added concept:

```cpp
waitingForPriorBatch =
    my_batch_index_ > 0 &&
    !preceding_batch_cars_.empty() &&
    clearance_poll_msg_ &&
    clearance_poll_msg_->isScheduled();
```

Recallable now includes:

```text
not in/past conflict AND waiting for prior batch
```

Logs now include:

```text
waiting_prior_batch=
batch=
```

Why it was added:

- r5 was being treated as not recallable even though it was a later-batch vehicle waiting behind earlier traffic and should still be able to halt/rejoin rollback.

Observed earlier:

```text
[HALT-LOCAL] r5 recallable=1 ... waiting_prior_batch=1 batch=1
```

Spec alignment:

- Matches the idea that vehicles not physically committed to crossing can halt on valid cancel evidence.
- This is a local physical-state predicate, not a consensus rule.

### 6. Cancel-path debug logs

File:

```text
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBRollbackProtocol.cc
```

Logs added:

```text
[CANCEL-ECHO-DUP]
[CANCEL-CERT-GATE]
[CANCEL-JUSTIFY-GATE]
[CANCEL-PROPOSER-CHECK]
[CANCEL-PROPOSE-GATE]
[CANCEL-PROPOSE]
```

Purpose:

- Show whether cancel cert generation is happening.
- Show whether valid cancel justification is being ignored.
- Show whether the local proposer election/rotation gate blocks proposal.
- Show certificate size, electorate size, rotation index, chosen proposer, and timestamps.

Observed effect:

- These logs helped establish that the latest run does reach cancel commit; the current blocker is after cancel, during replacement order.

Spec alignment:

- `CANCEL_CERT` remains evidence.
- `CANCEL(0)` remains the committed tombstone under old epoch `V_0`.
- These changes are mostly observability.

### 7. Cancel-commit gossip and executor sequence sync

Files visible in tree:

```text
incubator-resilientdb/integration/omnet/resdb_omnet_bridge.h
incubator-resilientdb/integration/omnet/resdb_omnet_bridge.cc
incubator-resilientdb/platform/consensus/execution/transaction_executor.cpp
incubator-resilientdb/platform/consensus/ordering/pbft/message_manager.h
incubator-resilientdb/platform/consensus/ordering/pbft/message_manager.cpp
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBRollbackProtocol.cc
```

Relevant symbols/logs:

```text
ResdbOmnetAdvanceExecutorAfterGossipCancel(...)
MessageManager::AdvanceExecutorAfterGossipCancel(...)
[EXECUTOR-GOSSIP-SYNC]
```

Purpose:

- A replica may learn `CANCEL(0)` through cancel-commit gossip instead of locally executing PBFT seq=2.
- If it adopts the tombstone, its executor/PBFT sequence state must not remain stuck expecting the missing seq=2 execution before seq=3 can be accepted.

Observed earlier:

```text
[EXECUTOR-GOSSIP-SYNC] advancing last_seq ...
[EXECUTOR-GOSSIP-SYNC] advanced next_execute_seq ... to 3 (gossip-adopted cancel)
```

Spec alignment:

- `rollbackv3.md` allows cancel-commit gossip adoption as a tombstone adoption path.
- A gossip-adopted tombstone should let the node participate in/observe the next epoch without requiring a local full replay of the cancel consensus instance.

Status:

- This solved/diagnosed earlier `ORDERMSG-GAPWAIT` and executor assertion style failures.
- It is not the current latest-log blocker.

### 8. Rollback discovery reset and membership minimum behavior

File:

```text
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBRollbackProtocol.cc
```

Current visible behavior:

- `beginRollbackDiscovery()` resets rollback discovery state.
- `rollback_expected_membership_size_` is set to `0`.
- `minRollbackMembershipSize()` returns the explicit expected override if present; otherwise it returns the protocol minimum from `minRollbackVoteN()`.
- Latest logs show:

```text
expectedN=4
```

Important caution:

- This was an intentional move away from a hard-coded "wait for 16" scenario guard.
- The current failure shows that the resulting completion/proposal gate can still allow r1 to propose before it has all currently relevant signed vehicles.

Spec alignment:

- Removing a fixed scenario-level 16 gate better matches dynamic discovery.
- But `rollbackv3.md` also says membership construction is safety-critical and followers should reject proposals that omit vehicles they locally know are fresh, present, certified, and not departed/non-recallable.
- That follower-side omission check is not cleanly implemented yet.

### 9. Analyzer updates/status

File:

```text
fourway/analyze_log.py
```

Current analyzer has scenario-15/R0 parsing for:

```text
CANCEL-WITNESS
CANCEL-ECHO
CANCEL-CERT
CANCEL-COMMIT
ROLLBACK-COMMIT
ROLLBACK-DISCOVERY snapshot
ACTIVE-VIEW epoch=1
OrderDecision: epoch=1
ROLLBACK-UNAVAILABLE
rollback_commit_extra_replica_ids
rollback_commit_missing_replica_ids
R0_Failure_Stage
R0_Epoch1_Order_N_Vehicles
R0_Epoch1_Order_Vehicles
```

Useful command:

```bash
python3 fourway/analyze_log.py /tmp/resdb-simulation.log --scenario 15 --cars 18 --save-to /tmp/analyze-r0 --no-scenario-subdir
```

Important:

- The analyzer was updated to make the one successful run clearer.
- It should now also report the current run as a failure before epoch 1 order commit.

---

## How This Lines Up With `rollbackv3.md`

### Intended protocol shape

Spec:

```text
ORDER(e) under V_e
-> CANCEL(e) under V_e
-> tombstone epoch e
-> discover V_{e+1}
-> ORDER(e+1) under V_{e+1}
```

Implementation direction after this debugging pass:

```text
ORDER(0)       PBFT seq=1   normal proposeAll / V_0
CANCEL(0)      PBFT seq=2   cancel payload / V_0
Discovery      no PBFT seq  app-level cert/announcement collection
ORDER(1)       PBFT seq=3   normal proposeAll / V_1 forced active view
```

This is the right high-level shape.

### What is aligned

- `CANCEL_CERT` is treated as evidence, not as the final tombstone.
- `CANCEL(0)` is a committed PBFT decision under the original epoch.
- Cancel-commit gossip can spread the tombstone.
- Replacement order is intended to use normal order proposal machinery.
- The bridge now installs a per-epoch active PBFT view for `hdr.epoch > 0`.
- Per-epoch quorum is derived from `voteN` and `f_epoch`.
- Anchored mode is stricter and should reject insufficient membership instead of falling back to static PBFT.

### What is not aligned or not finished

#### A. Membership closure is not clean

Spec principle 9:

```text
Followers must reject rollback proposals that omit vehicles they locally know are fresh, present, certified, and not departed/non-recallable.
```

Current state:

- The proposer can form an epoch 1 order from its local discovered set.
- The bridge installs a view from the proposal entries.
- There is no clearly audited, shared proposer/follower predicate that proves omitted vehicles are safe to omit.
- In the latest log, r1 proposed with `N=15` while other nodes reached `N=16`, and r1 later learned `veh16`.

Do not treat "wait for fixed 16" as the final design. The spec wants dynamic discovery, but the implementation still needs a correct closure/omission rule.

#### B. View-change/proposer rotation is still local/stuck

Known issue:

```text
[ROLLBACK-VC-UNSUPPORTED] ... suppressed app forced view-change while cancel active ...
```

The user explicitly said this is not the current focus because the scenario assumes honest nodes. Still, it is in the log and should not be mistaken for a solved part of the protocol.

#### C. Scenario-specific numbers in `rollbackv3.md` may be stale

`rollbackv3.md` says scenario 15 example:

```text
|V_1| = 14
f_1 = 4
q_1 = 10
```

But the intended experiment we converged toward appears to be:

```text
14 old recallable/sitting vehicles + 2 new certified vehicles = 16 active vehicles
```

The one successful run had epoch 1 `N=16`. The latest failed run had epoch 1 `N=15` because `veh16` was missing from the proposal, not because `16` was scene-only by design.

Decision to make:

- If both new vehicles should be voters, update the scenario example in the spec to `N=16`.
- If newcomers should be scene-only, then the implementation needs explicit `SCENE_QUIET` classification and should not count them in `V_1`.

#### D. PBFT seq=3 liveness still not stable

Latest log:

- Forced view installed.
- PRE-PREPARE delivered before PREPARE.
- No primary rejection.
- Only one replica broadcast COMMIT.
- No seq=3 collector commit.

So even after the active view/transport fixes, seq=3 liveness is not solved.

---

## Known Good/Bad Runs

### Earlier one-off good run

There was one run where epoch 1 appeared to commit with 16 vehicles:

```text
OrderDecision: epoch=1 n_vehicles=16
vehicles: 1,2,3,4,5,7,8,9,10,11,12,13,14,15,16,17
```

That run led to the note about `rollback_commit=17`, where an extra non-member observer/adopter like `r6` could log rollback commit after learning the epoch 1 decision by gossip. The important distinction was:

```text
order membership count != number of replicas that later learned/applied the order
```

### Latest bad run

Latest run:

```text
ORDER(0): yes
CANCEL(0): yes
ORDER(1) proposed: yes
epoch 1 active view installed: yes, but N=15
seq=3 commit: no
epoch 1 OrderDecision: no
```

---

## Useful Greps

Current broad rollback/PBFT path:

```bash
rg -n 'COLLECTOR-COMMIT.*seq=2|CANCEL-COMMIT|ROLLBACK-DISCOVERY.*snapshot|PROPOSE-PACK|EPOCH-VIEW|ACTIVE-VIEW.*epoch=1|PBFT-NEW-REQ.*seq=3|PBFT-FORCED-CHECK.*seq=3|PBFT-QUORUM.*seq=3|PBFT-COUNT.*seq=3|PBFT-BCAST-COMMIT.*seq=3|COLLECTOR-COMMIT.*seq=3|OrderDecision: epoch=1|the request is not from primary|ROLLBACK-VC-UNSUPPORTED' /tmp/resdb-simulation.log
```

Membership proposal:

```bash
rg -n 'ROLLBACK-DISCOVERY] r1 snapshot|PROPOSE-PACK] r1 entry|ACTIVE-VIEW] mode=promoted epoch=1 seq=3|ANN-RECV] Replica 1 received ARRIVAL_ANNOUNCE from veh16|ROLLBACK-DISCOVERY.*signed=16' /tmp/resdb-simulation.log
```

Seq=3 PBFT liveness:

```bash
rg -n 'PBFT-COUNT.*seq=3|PBFT-BCAST-COMMIT.*seq=3|PBFT-PREPARE-RESULT.*seq=3|PBFT-COMMIT-RESULT.*seq=3|COLLECTOR-COMMIT.*seq=3|OrderDecision: epoch=1' /tmp/resdb-simulation.log
```

Transport ordering:

```bash
rg -n 'TYPE8-DRAIN.*seq=3|TYPE8-RECV.*TYPE_PRE_PREPARE.*seq=3|TYPE8-RECV.*TYPE_PREPARE.*seq=3' /tmp/resdb-simulation.log
```

Cancel only:

```bash
rg -n 'CANCEL-CERT-GATE|CANCEL-JUSTIFY-GATE|CANCEL-PROPOSER-CHECK|CANCEL-PROPOSE-GATE|CANCEL-PROPOSE|PBFT-COUNT.*seq=2|PBFT-BCAST-COMMIT.*seq=2|COLLECTOR-COMMIT.*seq=2|CANCEL-COMMIT' /tmp/resdb-simulation.log
```

Analyzer:

```bash
python3 fourway/analyze_log.py /tmp/resdb-simulation.log --scenario 15 --cars 18 --save-to /tmp/analyze-r0 --no-scenario-subdir
```

---

## Files Touched / Relevant

Implementation files:

```text
incubator-resilientdb/integration/omnet/resdb_omnet_bridge.cc
incubator-resilientdb/integration/omnet/resdb_omnet_bridge.h
incubator-resilientdb/platform/consensus/execution/transaction_executor.cpp
incubator-resilientdb/platform/consensus/ordering/pbft/commitment.cpp
incubator-resilientdb/platform/consensus/ordering/pbft/consensus_manager_pbft.cpp
incubator-resilientdb/platform/consensus/ordering/pbft/message_manager.cpp
incubator-resilientdb/platform/consensus/ordering/pbft/message_manager.h
incubator-resilientdb/platform/consensus/ordering/pbft/omnet_forced_view.h
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBDecision.cc
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.h
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBRollbackProtocol.cc
veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBTransport.cc
fourway/analyze_log.py
```

Dirty worktree note:

- The repo has many unrelated dirty/generated files, including `.DS_Store`, docs, compiled binaries, generated cert/key files, and config files.
- Do not assume every dirty file is part of the rollback debugging work.

---

## Clean Mental Model To Keep

The protocol should be:

```text
1. Epoch 0 normal consensus decides the first schedule.
2. A late emergency produces signed evidence.
3. Original epoch voters commit CANCEL(0).
4. Everyone learns the tombstone, either by PBFT commit or cancel-commit gossip.
5. Discovery restarts from current perception/certs.
6. Certified, recallable vehicles form V_1.
7. The normal scheduler/proposeAll path decides ORDER(1).
8. Vehicles follow the committed epoch 1 order.
```

What is currently reliable from the latest log:

```text
1 through 4 work.
5 starts.
6/7 are not stable.
8 is not reached.
```

Main lesson from the last day:

```text
The active-view bridge can install epoch views, and normal proposeAll can produce epoch 1 proposals.
The unresolved problem is making the replacement view/proposal both complete and live every run.
```
