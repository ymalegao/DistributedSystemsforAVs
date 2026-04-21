---
name: SimTime-based timeout bug diagnosis
description: Root cause of LC failure at N=16 with simTime-based timeout — two confirmed bugs
type: project
---

Root cause 1: **Long.MIN_VALUE / 2 sentinel creates astronomically false sim-gap on first STOP emit**.
`lastStopSimEmitMs.getOrDefault(reg, Long.MIN_VALUE / 2)` means `sinceSim = nowSimMs - (Long.MIN_VALUE/2)` ≈ 4.6×10^18 ms on first call, which always passes the `>= STOP_RETX_SIM_MS` guard — but the log shows this produces burst-of-one correct behavior, since the task only fires after `STOP_RETX_WALL_MS` wall ms. The enormous sim-gap is logged but the first blind emit still fires correctly.

Root cause 2 (primary): **STOPDATA "out of context" deadlock**. The new leader (replica 1 for regency 1) is receiving STOPDATA (distinct 1..8 logged at lines 492624–535376) but ALL are stamped "Keeping STOPDATA as out of context for regency 1". This happens because `Synchronizer.processLCMessage` line 863 checks `regency == lcManager.getLastReg()` — but replica 1's `lastReg` is still 0 when the STOPDATAs arrive. Replica 1 has NOT yet locally called `startSynchronization` (i.e., has not seen 2f+1=11 STOPs), so `lastReg` is 0, not 1. The condition fails and every STOPDATA goes to `outOfContextLC`. Since outOfContext LC messages are only re-examined on the NEXT LC event, they are never processed for regency 1.

Root cause 3 (consequence): regency 2 storm. Some replicas time out again before regency 1 SYNC arrives, escalating to regency 2, creating a mixed-regency environment where regency-2 STOPs arrive at replicas still trying to complete regency 1 — STOPDATA for reg=1 are then discarded (line 874: `else { discarding }`).

**Why:** Replica 0 is Byzantine and silent. Each follower fires STOP for regency 1 via RequestsTimer at different wall-clock times (their own `receptionTimestamp + timeout`). Because `receptionTimestamp` is stamped from `SimulationClock.currentTimeMillis()`, and the PROPOSE_ALL client request arrives to all followers at roughly the same sim-time (~7s sim), all 15 followers should have similar `receptionTimestamp` values. BUT the ByzLeader (replica 0) itself sends its own STOP for regency 1 only after its own RequestsTimer fires — which happens LATE because replica 0 is Byzantine and never called `forwardRequestToLeader`. The log shows replica 0 STOP for regency 1 arrives at line 642881 at wall 20:30:34, while followers started LC at 20:28:15 (line 140213). The Byzantine leader's STOP arrives very late — after many followers have already timed into regency 2.

**Fix required:** Replica 1 needs to see 2f+1 STOPs to advance `lastReg` to 1 BEFORE STOPDATAs arrive, so they are processed rather than parked as out-of-context. The STOP flood is taking 90+ sim-seconds to accumulate 11 distinct senders at any single receiver (the problem quantified in LC_INVESTIGATION.md). STOPDATA reliably fails because it arrives during the window where `lastReg < 1` or `lastReg >= 2` (after regency escalation).

**How to apply:** Any fix to the sim-time timeout must ensure replica 1 reaches 2f+1 STOPs BEFORE the first STOPDATA is sent. The STOP phase is the bottleneck; STOPDATA/SYNC transport is healthy.
