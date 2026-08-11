# Ablation Studies — V2V BFT Intersection Protocol

Ablations + baselines for the project presentation. Run on the **current branch** (no
merge). All 4-vehicle (fast) except #5 (18-veh emergency). 3 reps each.

**Metric choice:** each uses the metric that actually *shows* its effect — latency/messages
don't move for all of them, so metrics are tailored (see each below).

Addresses the professor's 4 points: **stop-sign baseline** (#3), **vanilla-BFT baseline**
(#6), **per-mechanism ablations** (#1,#2,#4,#5), **conflict matrix defined**
([conflict_matrix.png](conflict_matrix.png) + `../conflict_matrix.md`).

Regenerate: inside `opp_env`, `benchmarks/ablations/run_ablations.sh <n|all> [reps]`,
then `python3 benchmarks/ablations/analyze_ablations.py` and `conflict_matrix.py`.

---

## 1. With / without RSU units — [ab1_rsu.png](ab1_rsu.png)
**Setup:** 4 vehicles, no units (N=4, f=1) vs +4 static units (N=8, f=2). `k` = PBFT-silent replicas.
**Metric:** commit-rate vs faults (tolerance) + messages/latency at k=0 (overhead).
**Result:** without RSU consensus **collapses at k=2**; with RSU it **holds**. Cost: ~**+27% messages**, ~**+30% latency**.
**Takeaway:** units buy an extra tolerated fault for a small, bounded overhead.

## 2. Vanilla BFT vs our BFT — [ab2_attack.png](ab2_attack.png)
**Setup:** Byzantine leader runs a **fake-ambulance** attack (byzType 6). "Our BFT" = full f+1 pre-verification firewall; "vanilla" = `--no-firewall` (no f+1 stage).
**Metric:** does the fake-ambulance proposal **commit** (`FALSE_PRIORITY_GRANTED`)?
**Result:** **vanilla commits the attack in 100% of runs** (fake car gets ambulance priority — unsafe); **ours blocks it in 100%** (0% committed).
**Takeaway:** the f+1 pre-verification stage is what makes the protocol *safe* — vanilla PBFT alone is not. This is the headline safety result.

## 3. All-way-stop baseline vs our protocol — [ab3_baseline.png](ab3_baseline.png)
**Setup:** SUMO all-way-stop (conventional control, no consensus) vs our BFT protocol, 4 vehicles.
**Metric:** mean per-car **wait time** at the intersection (Resume − Stop) — the metric both configs log.
**Result:** baseline **5.4 s** vs ours **1.2 s** — ours ~**4.6× lower** wait (non-conflicting cars cross in parallel vs one-at-a-time all-way-stop).
**Caveat (be precise in the talk):** this compares two *control approaches* — the two arms use **different net/route files** (`baseline_intersection` all-way-stop vs `bft_intersection`), so it is **not** the identical scenario with only the controller swapped. The baseline is SUMO's genuine all-way-stop logic (not an arbitrary timer), so the direction is sound; just don't present it as a perfectly controlled A/B.
**Takeaway:** our protocol yields lower intersection wait than conventional all-way-stop control.

## 4. With / without priority — [ab4_priority.png](ab4_priority.png)
**Setup:** 4 vehicles, one designated ambulance (`isAmbulance=true`, scheduled first) vs treated as normal (FIFO).
**Metric:** the ambulance's wait time at the intersection.
**Result:** ambulance waits **0.06 s** with priority vs **2.06 s** without — **~34× less**.
**Takeaway:** priority scheduling gets an emergency vehicle through almost immediately. (Effect grows with traffic — at 8–12 vehicles the ambulance skips a longer queue.)

## 5. With / without rollback — [ab5_rollback.png](ab5_rollback.png)
**Setup:** 18-vehicle late-emergency scenario. Rollback enabled vs disabled (`enableRollback=false` via a derived config so it actually takes effect).
**Metric:** was the late ambulance **re-ordered into a committed schedule**? (ON commits a real epoch-1 re-order; OFF cannot.)
**Result:** rollback ON: **3/3 (100%)** — an epoch-1 re-order commits, giving the ambulance a committed slot. rollback OFF: **0/3 (0%)** — the committed order is frozen; the ambulance is never admitted.
**Be precise in the talk — two honest limits:**
- The ambulance is injected at ~t=17 s and the sim caps at t=30 s, so **it does not physically clear the intersection in either arm** — the claim is "re-ordered into the committed schedule," *not* "safely crosses." (Re-run with `--sim-time-limit=90s` to show the physical crossing.)
- Rollback has a **cost**: re-ordering pauses traffic, so the ON arm cleared **fewer** cars in the 30 s window (10 vs 14). It trades throughput to correctly admit the ambulance.
**Takeaway:** rollback lets a committed order be safely revised to admit a late ambulance; without it the order is immutable. (Trigger is intermittent under faults — fired 3/3 here at k=0.)

---

## 6. Vanilla BFT vs our BFT (baseline) — [ab6_vanilla_vs_ours.png](ab6_vanilla_vs_ours.png)
**Setup:** vanilla = `--no-firewall` (no f+1 pre-verification, per our definition of vanilla BFT); ours = firewall on. Same 4-vehicle normal scenario; only difference is the firewall.
**Metric:** messages + consensus latency.
**Result:** **identical** (943 msgs, 61 ms both) — in normal operation the firewall passes everything, so it's **verification-only: zero message/latency overhead**.
**Takeaway:** our safety layer is **free in the common case**; its cost is paid only when it *rejects an attack* (Ablation 2, where vanilla commits it 100% and ours blocks it 100%). "Free safety."
**Note (Option A limit):** flags can only strip the firewall; the arrival-cert *formation* and conflict-matrix *batching* are baked in (can't be disabled without code). So this baseline still forms certs + batches — the true cost of those additions is shown by the decomposition below, not by this bar.

## Latency decomposition (for understanding) — [decomposition.png](decomposition.png)
Where our end-to-end consensus time goes: **vanilla PBFT ordering ≈ 61 ms** + **arrival-cert f+1 layer ≈ +54 ms** (our addition). This is what our per-car certificate layer costs on top of plain PBFT ordering — the honest "what we add extra from vanilla BFT."

## Conflict matrix (definition) — [conflict_matrix.png](conflict_matrix.png) + [../conflict_matrix.md](../conflict_matrix.md)
The scheduler's "smart contract": which movements may cross simultaneously. 12 safe-batch pairs (2 opposing-straight, 6 right-right, 4 right-straight); **left turns never batch**; same-lane never batches. Green ✓ = parallel-safe, red ✗ = must serialize. This parallelism is the mechanism behind #3's throughput win.

---

## Caveats (be ready for questions)
- **#1 fault tolerance** needs injected faults (`k` silent replicas) — that's the point; at k=0 there's only the overhead difference.
- **#2** only differs *under attack* — the firewall does nothing in normal operation; its value is catching the bad claim.
- **#5** uses the 18-veh scenario, and the late-ambulance **trigger is intermittent** — so the "rollback ON" rate is limited by how often the trigger fires, not by the rollback consensus (which is reliable once triggered).
- All runs use the fast channel + 5 ms bridge poll (speed; outcome-invariant).
