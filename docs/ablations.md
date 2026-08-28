# Ablation studies — what each figure shows, and why it looks that way

Generated from the run of **2026-08-27**. ab1's k=0 cells and ab3/ab4 use 9 repetitions; ab2 and ab5 use 3. Regenerate with, inside
`opp_env` and with `veins_launchd` on :9999:

```bash
benchmarks/ablations/run_ablations.sh <1..5|all> [reps]   # -> benchmarks/ablations/results/
python3 -m plotter build-all                              # -> figures/
python3 -m plotter summary                                # -> figures/results.md
```

## Scenario, in one paragraph

Four approaches, **one lane each**, no turn pockets. Every vehicle draws its exit
uniformly from its approach's three valid exits (`<routeDistribution>`), so
roughly a third turn left; the U-turn is absent from the distribution, so nobody
leaves the way they came. `*.manager.seed` is set per repetition, so each rep
samples a different turn assignment. All vehicles depart at `t=0` — a single
arrival burst, not continuous demand, because the protocol currently completes
**one consensus epoch** per run.

Min–max ranges are no longer drawn on any figure. The spread is still computed
and printed per cell in [`figures/results.md`](../figures/results.md).

---

## 1. RSU units — [ab1_rsu.png](../figures/ab1_rsu.png)

Three panels: throughput, stop-to-clearing delay, message cost. Vehicles 4–20,
with and without 4 static intersection units, **all at k=0 (no faults)**.

**Throughput and delay are identical between arms. That is the correct result.**
RSU units are non-vehicle replicas: they vote, echo and relay, but they never
queue at the junction and are never scheduled. The same vehicles cross in the
same conflict-matrix batches either way, so per-vehicle delay and throughput
cannot move. Only traffic changes — 63 → 135 msgs/vehicle at N=4, 326 → 459 at
N=20.

**The benefit is real but is not on this figure.** Every panel is pinned to
`k=0`, the one condition where units provably cannot help. The fault frontier,
measured in the same 186 logs, is where they earn their keep:

| | OFF | ON |
|---|---|---|
| N=4 | dies at k=2 | survives k=2 |
| N=8 | dies at k=4 | survives k=4 |
| N=16 | dies at k=6 | survives k=6 |
| N=20 | dies at k=7 | survives k=7, 2/3 at k=8 |

RSUs buy **1–2 additional tolerated silent replicas** at every vehicle count.
Quorum confirms they are genuine members: 3→5 at N=4, 13→15 at N=20, matching
`2f+1` over the enlarged membership.

**Open:** N=12 goes the wrong way (OFF `k5:2/3`, ON `k5:1/3`) where every other
count improves. One cell against four.

**To fix the figure:** add a commit-rate-vs-k panel. The data already exists; no
re-run needed.

**Earlier versions of this figure were wrong.** Before 2026-08-27 the ON arm
showed a large delay penalty (≈18s vs 6s at N=4). That was the `toleratedF`
defect below, not a property of RSUs — the arm was falling back to stop-sign
timeouts because consensus never committed.

---

## 2. Fake-ambulance attack — [ab2_attack.png](../figures/ab2_attack.png)

**This study currently measures nothing. Do not present it.**

Colluders take the *highest* replica ids, so `CertPrimary()` — which elects the
smallest certified id — always picks an honest proposer. That isolates the
certificate gate from leader succession, which is what we want. Arms are
`enableAmbulanceCertGate` on vs off.

The gate now fires correctly (90 rejections in the gate arm, 0 in the control).
But in the **control** arm, where the certless claim is deliberately allowed
through, the liars still gain nothing — they wait ~0.9s *longer* than honest
vehicles, and both arms grant zero false priority.

So the attack does not succeed even when undefended. The lie is accepted at the
announce layer and never becomes priority at the scheduler: `e.is_ambulance` is
set from the proposer's `local_vehicle_states_`
([ResDBDecision.cc:146](../src/v2vbft/app/ResDBDecision.cc)) and the scheduler
prioritises on `is_ambulance && cyber_status == 1`, but something between those
points drops it. Until that path is traced, both arms will be flat regardless of
the gate.

---

## 3. Actuated traffic light vs consensus — [ab3_baseline.png](../figures/ab3_baseline.png)

| N | signal wait | ours | delay | service rate |
|---|---|---|---|---|
| 8 | 14.40s | 7.28s | 1.98× | 3.42× |
| 12 | 18.81s | 9.91s | 1.90× | 2.51× |
| 16 | 18.22s | 11.26s | 1.62× | 2.77× |
| 20 | 20.97s | 13.49s | 1.55× | 2.71× |

**Why a signal and not an all-way stop.** AIM surveys report fixed-time
signalling as the most-used baseline (46%) while explicitly calling for actuated
or adaptive instead; all-way stop control is used by only 8%. The all-way stop
was also unusable for a second reason — it **deadlocked in 7 of 12 runs** under
mixed turn demand, resolved only by SUMO's 300s teleport timer, producing
300–600s "waits" that are jam-resolution artifacts and flatten every real arm to
the axis floor.

**Why actuated and not fixed-time.** This scenario is a single arrival burst. A
fixed cycle would burn green on approaches that emptied and never refill, losing
for a reason unrelated to signal control. Actuated extends green while vehicles
are present and ends the phase on a gap, so it serves a burst on its merits.

**Why the gap narrows with load** (1.98× → 1.55×): the signal amortises its
fixed phase overhead better as the queue grows, while our per-round consensus
cost is roughly constant. Extrapolating past N=20 from four points is not
supported.

**Protected left turns are impossible here.** Each approach has a single lane, so
through and left share it and no phase can hold one red while the other runs.
`--tls.left-green.time` was verified to produce a byte-identical program. This
constrains the signal exactly as it constrains our protocol.

---

## 4. Ambulance priority — [ab4_priority.png](../figures/ab4_priority.png)

**9 repetitions**, not 3. The ambulance is *one vehicle*, so this study samples
n=1 per run where the others average 8–20; at 3 reps its wait swung 3.4s between
repetitions, wider than the whole N=8→20 trend.

The ambulance's route is also **pinned** to its approach's concrete straight
route while every other vehicle stays random. Leaving the measured vehicle's turn
to a coin flip let its own draw dominate the metric — a left-turning ambulance
batches only with the opposing left, a straight one batches freely.

| N | priority | FIFO | benefit |
|---|---|---|---|
| 8 | 7.29s | 10.34s | 3.05s |
| 12 | 7.36s | 13.93s | 6.57s |
| 16 | 8.97s | 15.72s | 6.75s |
| 20 | 9.87s | 18.52s | 8.65s |

Monotone in all four series, and **the benefit grows with load** — which is the
claim the title makes. Priority wait rises gently (7.29 → 9.87) while FIFO rises
steeply (10.34 → 18.52): the ambulance is inserted near the front regardless of
queue length, so what grows is what it skips.

The *"relative to the rest of the traffic"* panel is the honest headline — it
divides by the fleet mean, cancelling run-level variation.

---

## 5. Late-emergency rollback — [ab5_rollback.png](../figures/ab5_rollback.png)

| | cleared | wait | msgs/vehicle |
|---|---|---|---|
| rollback OFF | 16.0 | 10.40s | 357 |
| rollback ON | 16.0 | 12.30s | 554 |

**Both arms clear the same number of vehicles.** Rollback does not serve *fewer*;
it serves *different ones* — the ambulance takes a slot an ordinary vehicle would
have had, and the rest shift later. Every count-based summary shows the arms as
identical, which is why the left panel plots individual departures rather than a
total.

Cost is ~1.9s of extra wait and ~1.55× messages: revising a committed order means
a CANCEL round on top of the ORDER round.

**Open:** rollback fired in 2 of 3 reps in the latest run, 3 of 3 previously.
With 3 reps a regression cannot be separated from the turn draw. This is the
cheapest study (~5 min for 3 reps) — it should be run at 9–12.

---


## Cost decomposition — [an_cost_decomposition.png](../figures/an_cost_decomposition.png)

Honest operation only (`k=0`); with replicas silenced the traffic mix reflects
failure, not normal composition.

**Per occurrence, PBFT ordering is slower than certificate formation** — 213ms vs
165ms at N=16. Certificate formation is one round trip at an `f+1` threshold;
PBFT is two to three phases at `2f+1`.

**But certificates dominate total traffic ~15.6×** (3251 vs 209 messages at
N=16), because the cert layer runs once per *vehicle* and is O(N²) in echoes,
while PBFT runs once per *round*. Latency and volume point in opposite
directions, which is why the figure shows both.

Certificates are normalised **per vehicle** and PBFT **per round**, so the left
panel compares one occurrence against one occurrence. Comparing raw totals would
pit a layer that ran N times against one that ran once.

Two things changed the picture recently and are worth knowing:

- **Discovery no longer waits out a fixed timer.** Epoch 0 used to be barred from
  closing early, so every round burned the full `certCollectionTimeoutSec = 8s`
  even with every certificate already collected. It now closes once the roster is
  certified.
- **Announces back off exponentially** (0.1s → 3.2s, reset when a new peer is
  heard). A vehicle re-announces until `f+1` peers echo it, which takes the whole
  approach; at a fixed 100ms that was ~130 announces each, and every listener
  echoes every announce it hears.

---


## Defects found and fixed in this cycle

**`f` was computed two ways.** Certificate *formation* derived `f` from
`total_vehicles_`; *validation* used `toleratedF()`, i.e. `num_replicas_`
including RSU units. With units present these never agree, so every certificate
was assembled one to two echoes short of what every receiver demanded and **every
replica dropped every other replica's certificate**. Each vehicle kept only its
own → a proposal that is mostly QUIET → no commit → stop-sign fallback.

Intermittent rather than constant, because extra echoes sometimes arrive before
the certificate latches — which is why it presented as "N=4 instability". It
explained ab1's phantom RSU delay penalty *and* ab6's S4/S5 attack cells, which
decided nothing in every rep and now decide normally.

Three further sites used the same wrong formula and were unified on
`toleratedF()`: gossip propagation confirmation, consensus relay carrier
threshold, and a Byzantine-injection log line.

## Known-open issues

- **ab2 measures nothing** — attack does not succeed even undefended (above)
- **ab1 does not plot the fault frontier**, which is the RSU benefit
- **ab1 N=12** fault tolerance goes the wrong way with units
- **ab5 needs more repetitions** — rollback fired 2/3 vs 3/3 across runs
- **Single-lane approaches** preclude protected-left phases for every arm
- **Burst arrival, single epoch** — no steady-state comparison is possible until
  multi-epoch support lands
