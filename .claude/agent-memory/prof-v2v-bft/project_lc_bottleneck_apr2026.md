---
name: LC Phase 1 Bottleneck — N=16 Log Analysis April 2026
description: New run (22:24:15) with timeout=800ms, RETX=200ms confirms 200ms cadence works; replica 1 stalls at distinct=10/11; quorum=11 is reached by replicas 9/11/0/10 but they DISCARD because they already escalated to reg=2; regency-2 split by 6 replicas at sim t=7.27s
type: project
---

## Run 2 (Current): SixteenVehiclesAmbulanceBFT-0-20260420-22:24:15, N=16, f=5, quorum=11

**Params:** timeout=800ms sim, STOP_RETX_SIM_MS=200ms, STOP_BLIND_EMITS=Integer.MAX_VALUE, lastTriggeredForLastReg debounce

**Measured sim:wall ratio:**
- Sim ends at t=10.0s, run start 22:24:15, last event ~22:25:52 → 97 wall-s
- Ratio = 10s / 97s = **103 sim-ms per wall-second** (1 sim-s ≈ 9.7 wall-s)

**Timeline (all times in sim-s):**
- t=0.0: simulation starts
- t=2.326s (line 28590, Event #28590): CERT-COLLECTION TIMEOUT fires, PROPOSE_ALL submitted by replica 0 (leader; Byzantine)
- t=3.275s (C++ Reliability timer at 3275ms, line 81385): `RequestsTimer` fires expired request; first STOP for regency 1 sent (ALL 16 replicas send STOP, confirmed by VIEW-CHANGE logs)
- t=3.275–10s: blind STOP flooding active, 200ms cadence confirmed (sim-gap=200-277ms on blind 2+ lines)
- t=7.273s (line 256923, Event #94693 vicinity): **Regency-2 escalation begins** — 6 replicas (0, 1, 9, 10, 11, 4) send STOP for regency 2
- t=9.454s (Event #130274, line 357046): replica 1 sees distinct=9 (from replica 5)
- t=9.480s (Event #130270, line 358642): replica 1 hits **distinct=10** — last entry for reg=1 at replica 1
- t=9.480s (simultaneous): replicas 9, 11, 0, 10 receive STOP from replica 7 with distinct=11 — BUT all **DISCARD** because their `lastRegency=1, nextRegency=2` (they already escalated)
- t=10.0: simulation ends — no consensus, no startSynchronization

**Key metrics:**
- PROPOSE_ALL submitted at sim t=2.326s (not t=0.8s — cert collection adds ~1.5s)
- LC starts at sim t=3.275s (not t=0.8s — timeout fires ~0.95s after PROPOSE_ALL due to wall-sim ratio)
- BFT timeout of 800ms wall-time triggers at sim t=2.326 + (800ms_wall / 103 sim-ms_per_wall-ms) ≈ 2.326 + 7.77s_sim — NO, the 800ms is a wall-ms timeout not sim-ms. Wall time from PROPOSE_ALL to LC start = ~27.5 wall-s. At 103 sim-ms/wall-s → 27.5 × 0.103 = 2.83s sim. So LC starts at 2.326 + 0.949 = 3.275s sim. **The 800ms timeout IS in wall-ms, so it fires after 800 real-ms regardless of sim time. Wall-ms elapsed before PROPOSE_ALL delivery to Java ≈ 800ms → LC triggers correctly.**
- LC phase window: sim t=3.275s to t=10.0s = **6.725 sim-s available**
- Blind STOP emits: 16 replicas × 31 blind emits = confirmed working at 200ms cadence; 31 × 200ms = 6.2 sim-s of emitting (matches window)

**Blind STOP cadence (reg=1):**
- blind 1–31: all 16 replicas emit all 31 rounds (blind 1–14 all have 16 entries; blind 15–18 have 15; blind 19–28 have 12; blind 29–30 have 10; blind 31 has 1)
- 200ms cadence confirmed: sim-gap=200–277ms on blind 2+ lines
- **CRITICAL BUG STILL PRESENT:** blind=1 shows `sim-gap=4611686018427391203ms` (= 2^62 + 3299 ≈ Long.MIN_VALUE / 2) — the Long.MIN_VALUE sentinel bug from the previous run is STILL active for the first blind emit. This is a display issue only (the STOP is actually sent) but indicates the `lastSentSimTime` initialization is still wrong.

**Max distinct STOP counts at replica 1 (new leader) for reg=1:**
- distinct=1 (line 98664): from replica 2
- distinct=2 (line 121917): from replica 0
- distinct=3 (line 128168): from replica 6
- distinct=4 (line 133035): from replica 15
- distinct=5 (line 144387): from replica 13
- distinct=6 (line 155769): from replica 5
- distinct=7 (line 157853): from replica 3
- distinct=8 (line 211751): from replica 8
- distinct=9 (line 252271): from replica 14
- **distinct=10 (line 358642): from replica 7 — FINAL, at sim t=9.480s**
- replica 1 NEVER reaches distinct=11 before sim ends

**Regency-2 escalation (CRITICAL FAILURE):**
- 6 replicas escalate to reg=2: 0, 1, 4, 9, 10, 11
- First reg=2 STOP at line 256923 (sim t=7.273s, wall 22:25:27.878) — sent by replica 0
- 4 replicas that receive distinct=11 from replica 7 at sim t=9.480s (lines 358328, 358482, 358496, 358505) — me=9, 11, 0, 10 — ALL DISCARD with "Last regency: 1, next regency: 2" (Synchronizer.java discards because they are already in reg=2)
- Replica 1 is among the 6 that escalated to reg=2, so when it receives from replica 7 it sees distinct=10 (not 11) because its reg=1 counter is 10 — the 11th STOP arrives at sim t=9.480 but at that point replica 1 has already committed to reg=2 for its own internal counter

**Root cause of current run (CHANGED from previous):**
1. **200ms cadence is working correctly** — 31 blind emits per replica per 6.2s window
2. **Regency-2 split is still the fatal problem** — 6/16 replicas escalate to reg=2 at t=7.27s while replica 1 is at distinct=9. When the 11th STOP finally arrives at t=9.48s, those 6 replicas discard it because they are in reg=2.
3. **Replica 1 itself escalated to reg=2** (line 256923-260981 confirms replicas 0, 10, 9, 11 send reg=2; VIEW-CHANGE log shows replica 1 is also in the set). This means replica 1 never completes reg=1 synchronization — it transitions to reg=2 with only 10 distinct reg=1 STOPs.
4. **No startSynchronization is ever called** for any replica in reg=1 — confirmed by grep returning no hits.
5. Consensus does NOT complete — no "Deciding" or BFT delivery in the log.

**New root cause diagnosis:**
The debounce (`lastTriggeredForLastReg`) is still failing for 6 replicas. They escalate to reg=2 before replica 1 (new leader for reg=1) accumulates its 11th STOP. The 800ms timeout reduced the pre-LC dead time (t=4s→t=3.27s) and 200ms cadence increased STOP density, but the reg=2 escalation window opened at t=7.27s — only 4 sim-s into Phase 1 — and 6 replicas jumped before quorum was achieved.

**Critical question for next investigation:**
Why do 6 replicas escalate to reg=2 at t=7.27s (only 4 sim-s after LC start)?
- They sent their initial reg=1 STOP at t≈3.27s
- Their RequestsTimer fires again (with 800ms wall timeout) at wall-time = 3.27s + 800ms_wall/103 = 3.27 + 7.77s_sim ≈ 3.27 + 0.8s_wall_in_sim... NO. The 800ms is wall-ms. At wall ratio 9.7s_wall per sim-s: 7.27sim-s = 7.27 × 9.7 = 70.5 wall-s. LC started at wall ~22:24:47. Reg-2 at wall ~22:25:28 = 41 wall-s after LC start. That is >> 800ms. So the debounce IS preventing the immediate re-fire, but not the second one. The 800ms fires again at wall ~22:24:47 + 800ms = 22:24:48 (blocked by debounce), then again at 22:25:28 (41s later — multiple timeout cycles) → debounce only suppresses one cycle, not all.

**How to apply:** For any new N=16 run: check (1) which replicas escalate to reg=2 and at what sim-time; (2) whether debounce suppresses only one cycle or all; (3) whether replica 1 (new leader) itself escalates before accumulating 11 distinct.
