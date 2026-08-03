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

- **[18veh_availability.png](18veh_availability.png)** — consensus success vs k. Both
  collapse, offset by ~2: **OFF dies at k=6, ON at k=8.** Units shift the frontier, they
  don't remove it (as BFT theory requires).
- **[18veh_rollback_pipeline.png](18veh_rollback_pipeline.png)** — the emergency
  rollback traced stage-by-stage at k=0 vs k=6. At k=6 the no-RSU arm can't even *start*
  a rollback (epoch-0 never commits, so the late ambulance is never admitted); the RSU
  arm completes the whole cancel→re-order pipeline.
- **[18veh_msg_cost.png](18veh_msg_cost.png)** — messages/run at k=0,4,6,8. Traffic
  holds until consensus fails (OFF at k=6, ON at k=8), then dips. *Noisier* due to
  rollback intermittency; use the 4-veh overhead figure for the cost headline.

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

- **Marginal-quorum cells are flaky.** At a config's exact fault limit (honest voters =
  quorum), success is ~a coin flip. With 2 reps that reads as 0/50/100. The k=0 baselines
  and the message-cost figures are solid; the *frontier* rate lines need ~5 reps to smooth.
- **Rollback trigger is intermittent**, adding some noise to the rollback pipeline
  figure's ON stage bars (the 67% at k=6 is 2/3 = trigger not firing in one run), not
  to the availability figure. A "cancel-committed rate vs k" line plot was dropped for
  this reason — it was dominated by trigger noise and redundant with availability.
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
