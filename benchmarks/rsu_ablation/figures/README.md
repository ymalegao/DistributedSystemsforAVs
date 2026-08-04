# RSU Ablation — Results

All figures for the "static intersection units (RSU) as PBFT participants" study.
Vehicles run BFT consensus (ResilientDB PBFT over Veins/OMNeT++/SUMO) to negotiate
crossings; the ablation adds 4 static units and measures the effect. `k` = number of
PBFT-silent (omission-faulty) vehicles. **OFF** = no units, **ON** = +4 units.

This folder holds only final figures. Runs write scratch logs to transient
`results*/` working dirs (git-ignored); the analyzers write their plots here.

---

## 1. Four-vehicle fault frontier

The clearest single result: 4 vehicles, OFF (N=4, f=1) vs ON (N=8, f=2).

- **[4veh_frontier.png](4veh_frontier.png)** — consensus success vs k. OFF is 100% at
  k=0, drops to 67% at k=1, dies at k=2; ON holds 100% through k=2. **Units extend the
  survivable fault count.**
- **[4veh_overhead.png](4veh_overhead.png)** — total messages sent. At k=0 (fair
  comparison) units cost ~+24%. Bars shrink as k rises because failed runs stop
  transmitting — that is the system dying, not efficiency.
- **[4veh_latency.png](4veh_latency.png)** — consensus latency; units add ~25–40%.

## 2. Eighteen-vehicle fault pressure (with rollback)

Late-emergency scenario. OFF (N=18, quorum 11) vs ON (N=22, quorum 13), k=0–8.
**All three figures come from one 6-rep sim-capped sweep** (k=0–8 × both arms × 6 reps).

- **[18veh_availability.png](18veh_availability.png)** — consensus success vs k. Both
  collapse, offset by ~2: **OFF dies at k=6, ON at k=8.** With 6 reps the frontier is
  clean: OFF is 100% through k=4, marginal 50% at k=5 (16−5=11=quorum exactly), 0% from
  k=6; ON is 100% through k=6, marginal 83% at k=7, 0% at k=8. (The old "67% at k=4" was
  noise — k=4 is reliably 100%.) Units shift the frontier +2, they don't remove it.
- **[18veh_rollback_pipeline.png](18veh_rollback_pipeline.png)** — the emergency rollback
  as a **conditional funnel** (each stage = % of runs that reached the previous), at k=0
  vs k=6. The late-ambulance *trigger* is intermittent (the "rollback triggered" bar
  varies by luck), but **once triggered, cancel + re-order complete ~100%**. At k=6 the
  no-RSU arm can't even start (epoch-0 dead); the RSU arm triggers 83% and completes 100%.
- **[18veh_msg_cost.png](18veh_msg_cost.png)** — messages/run, now **conditioned on the
  rollback having fired** (removes the fired/not-fired bimodality that made this noisy).
  With that + 6 reps the bars are tight: OFF ~16.2k, ON ~17.3k → a clean **~+7% RSU
  premium**, holding until consensus fails (OFF at k≥6, ON at k=8, marked ✗). Still, for
  the headline cost number use the 4-veh overhead figure (no rollback, cleanest).

## 3. Scaling: 4 / 8 / 12 / 16 vehicles

OFF vs ON across intersection sizes, at k=0 (min) and k=frontier (max).

- **[scaling_msgs.png](scaling_msgs.png)** — the strong result. Message cost scales
  **super-linearly** with size (PBFT's O(N²)); the RSU premium is a modest ~6–23%.
- **[scaling_fault_tolerance.png](scaling_fault_tolerance.png)** — silent-fault
  tolerance (N − quorum) per size, OFF vs ON. Exact (from N and quorum), so no noise.
  RSU adds **+2** tolerated faults at V=4/8/16, but **+0 at V=12** — there the 4 units
  raise the quorum by exactly 4, cancelling the gain. Empirically, k=0 committed 100%
  for every config. (This replaced an earlier consensus-rate-vs-size line plot that put
  both arms at their own marginal quorum — a coin-flip that produced misleading noise;
  a re-run can't fix that design, so it was replaced with the exact quantity.)

---

## Caveats (read before citing)

- **Marginal-quorum cells are flaky by nature.** At a config's exact fault limit (honest
  voters = quorum), success is ~a coin flip — e.g. OFF k=5 (16−5=11) and ON k=7 (20−7=13)
  land near 50–83% no matter how many reps. That is real physics, not under-sampling; the
  18-veh study uses 6 reps so those rates are accurate, not that they become 100%.
- **Rollback trigger is intermittent by design**, so the "rollback triggered" stage in
  the pipeline funnel varies by luck. This is *isolated* now: the funnel is conditional,
  so cancel/re-order read as "% of triggered runs" (~100%), and the message cost is
  conditioned on the rollback having fired. An earlier "cancel-committed rate vs k" line
  plot was dropped as trigger-noise-dominated and redundant with availability.
- The **INVALID_SIG (active-Byzantine) study was dropped** — units out-transmit the
  faulty nodes so the corrupt echoes never enter a certificate; the attack is never
  exercised. Not a valid experiment; excluded on purpose.

## How to regenerate

Inside `opp_env shell -w /home/mathesh/Documents/vc/ omnetpp-6.2.0`, from
`benchmarks/rsu_ablation/`:

| Study | Runner | Analyzer |
|---|---|---|
| 4-veh frontier | `run_matrix.sh [reps] [timeout]` | `analyze_ablation.py` |
| 18-veh fault pressure | `run_rollback_matrix.sh "<ks>" [reps] [timeout]` | `analyze_rollback.py`, `analyze_rollback_pipeline.py` |
| 18-veh message cost | `run_msg_matrix.sh "<ks>" [reps] [timeout] [simlimit]` | `analyze_msg.py` |
| Scaling 4/8/12/16 | `run_scaling_matrix.sh [reps] [simlimit] [timeout]` | `analyze_scaling.py` |

Each analyzer reads logs from its working dir and writes its figure(s) into this folder.
