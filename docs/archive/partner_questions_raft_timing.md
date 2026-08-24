# Raft vs BFT timing — answered from `partnersv2v`

Source code: **`/home/yash/partnersv2v`** (OMNeT++/Veins). Full detail with line-level references: **`/home/yash/partnersv2v/raft_timing_semantics_report.md`**.

BFT reference window you cited: **physical stop → 15 m past the intersection**. Raft JSON uses `timestamps_ms.stopped` / `passed` (implementation: `timeStopped_`, `timePassed_`).

---

## 1. When is stop time recorded? Distance?

- **Not** when consensus starts. Raft start is `timeRaftStarted_` at cluster formation; `raft_decision_time` runs from there until the pass schedule commits.
- **Usually not** when speed hits 0. The main rule is **distance**: the first tick where distance to end of the approach lane satisfies
  `0 ≤ dist < intersectionStopDistance_ * (totalVehicles_ / 2)`
  (`getDistanceToJunction()` = lane length minus lane position). The vehicle may still be moving.
- With **`intersectionStopDistance = 5` m** (sim ini): for 8 vehicles that ring is **20 m** from the stop line; for 4 vehicles, **10 m**.
- **Exception:** vehicles caught only on an internal junction edge set `timeStopped_` when that path runs (`handleClusterJunctionStop`).

---

## 2. When is depart time recorded?

- JSON **`passed`** is set when the code decides the vehicle has **left the intersection**: typically when `roadId` is in configured **`exitEdges`**, or after it has started moving and is no longer on an approach or internal (`:`) edge.
- **Not** the same event as **`resumeMovement()`** (batch go-ahead); departure for metrics is **exit detection**, not “first acceleration.”
- There is **no** built-in “15 m past” offset; end of window is **network/topology-based**, so it may differ from BFT’s 15 m past.

---

## 3. Does Raft wait include accordion / batch queue time?

- **Yes.** Follower **B** gets `timeStopped_` when **B** enters the distance ring, not when leader **A** clears. Queued cars may not get a hard TraCI stop, but the timestamp still starts at that approach-zone crossing.
- So **`total_wait_time` = `passed` − `stopped`** includes time **B** spent queued / accordion behind **A** (from ring entry until exit condition).

---

## Bottom line for comparing to BFT

Raft’s window can **start earlier** (distance ring while still moving vs strict physical stop) and **end at a different place** (exit edge / heuristic vs 15 m past). That alone can explain discrepancies without a BFT bug.
