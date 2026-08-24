# PicaBFT / ResDB-over-Veins — Experiment Handoff for Data Analysis

Purpose: describe each experiment, its methodology, and its expected outcome, so a coding agent can locate the relevant result files, check them against expectations, and flag anomalies.

**Grounding note — read this first.** Methodology below is grounded in `ARCHITECTURE.md` where cited. Two caveats the analyst must respect:
1. **Do not assume implementation details not present in the result data or ARCHITECTURE.md.** Where this doc says "[verify in code/data]", the claim is from design discussion and may not match the current build. Check the data, don't trust the description.
2. **ARCHITECTURE.md may lag the code.** In particular, ARCHITECTURE.md §8 describes the perception gate as a **categorical 4×4 approach confusion matrix** and states `positionInLane` is a discrete queue field the gate "does not claim to verify as a continuous position measurement." Several recent experiments (continuous-lateral lane gate, longitudinal/distance certificate) are **newer than that section**. Before analyzing those, confirm from the result files which gate variant actually produced the data (categorical vs continuous-lateral). Do not assume.

---

## Operating point (the fixed parameters most experiments hold)

Recent runs use, and the analyst should confirm per-file:
- `physical_gate_k = 2.0` (acceptance tolerance k in τ = kσ)
- `sigma_lat_m = 0.5` (lateral sensor noise)
- `sigma_long_m = 1.0` (longitudinal sensor noise)
- `signal_error = 0.2` (turn-signal cue flip rate)
- `f = 5`, `n = 16` for the main intersection (n = 3f+1)
- Maneuver ratio (recent honest scaling run): 50% LEFT / 25% STRAIGHT / 25% RIGHT — **[verify per experiment; this ratio starves co-batching, see G2 notes]**

The three axis roles (keep straight when reading any sweep):
- **σ (sigma_lat / sigma_long)** — environment. Not operator-controlled.
- **δ (claim displacement) / b (Byzantine count) / signal_error** — adversary.
- **k (gate tolerance)** — the one design knob the operator sets.
- **q0** — *derived*, not a knob: honest-witness false-accept probability. All of {δ, σ, k} act on breach only through q0.

---

## Core methodology (grounded in ARCHITECTURE.md)

**Two-stage pipeline.** Perception-gated certification (Stage 1, in Veins C++) feeds deterministic PBFT (Stage 2, ResilientDB via `resdb_omnet_bridge`). ARCHITECTURE.md §1.5, §1.12.

**Stage 1 — evidence gate.** Each vehicle is one PBFT replica (§1.1). A claimant broadcasts `ARRIVAL_ANNOUNCE` (type 1) with an origin signature over the canonical declaration (§8 Phase A). Honest external witnesses take one `ResDBPerception` sample and emit a lane-qualified `ARRIVAL_ECHO` (type 4) only when `observedApproach == ann.lane` (§8 Phase B step 3). The echo carries a signed `observedCue ∈ {STRAIGHT,LEFT,RIGHT,UNKNOWN}`; **signal evidence never vetoes the lane echo** (§1.11, §8 Phase B step 4). An `ARRIVAL_CERT` (type 5) forms from ≥ f+1 distinct-signer echoes.

**Three trust tiers (§8, §1.11):**
- **QUIET** — no valid lane cert → singleton.
- **SIGNED-UNKNOWN** — valid lane cert but < f+1 matching maneuver cues → singleton.
- **SIGNED-{direction}** — valid lane cert AND ≥ f+1 positive cue signatures → direction unlocked for `kSafe` co-batching.

**Per-witness single-evaluation invariant (§8 Phase B).** `(witness, target, epoch)` locks the first authenticated `claimHash`; the verdict/echo is cached. A re-announcement reuses the cached verdict (no resample, no second distinct signer). A *different* authenticated hash is retained as equivocation evidence and rejected before perception. This is the one-variant-per-witness rule.

**Stage 2 — PBFT.** The discovery primary submits `ResdbProposeHdr + ResdbVehicleEntry[]` (§8, §11). Pre-verify runs structural checks 1–8 and semantic checks 9–10 (§10). **Check 10** rejects a leader that mutates certificate-derived scheduler fields, including upgrading UNKNOWN→declared direction (§1.12, §10). **Check 9** guards certificate omission/suppression. Consensus commits an *order*, not movement (§1.6); `IntersectionExecutor` applies it, waits for preceding batches to clear via TraCI, then resumes vehicles (§12).

**Metrology vs trust separation.** TraCI approach/signal are *hidden truth inputs to the sensor model* (§8 Phase B step 2), corrupted before the witness sees them. TraCI is also used for actuation and for ground-truth safety measurement, but is **not** on the trust path for certification. [Analyst: physical-safety metrics like co-occupancy are ground-truth measurements, not protocol decisions.]

**Byzantine fault model (§18).** Fault injection modes and no-firewall ablation exist in-code; consult §18 for the exact enumerated modes rather than assuming.

---

## GROUP 1 — Main scaling / honest baseline

### Experiment G1: Honest control across scale
**Method.** N ∈ {4, 8, 16, 20}, all honest, 20 reps/N, mixed maneuvers. Operating point above. Measures throughput, wait, batch size, singleton causes, and safety with no adversary.
**Files.** `two_lane_scale_honest_full_{aggregates.csv, results.csv, summary.json, manifest.json}`.
**Expected outcome.**
- 20/20 pass at every N.
- **Zero unsafe co-occupancy and zero SUMO collisions at every N** (honest system is safe — this is the control that attacks are measured against).
- Throughput increases sub-linearly with N (intersection saturates).
**Known anomaly to check (do not treat as pass without explaining).**
- `mean_batch_size ≈ 1.0` at all N in the current run → co-batching is doing almost nothing. Likely cause: 50% LEFT maneuver ratio (left turns singleton under kSafe) plus SIGNED-UNKNOWN singletons rising with N (0.7 → 11.3 across N=4→20). **Analyst: split singleton count by cause (QUIET / SIGNED-UNKNOWN / left-turn-forced) and report which dominates.** If left-turns dominate, the ratio is starving batches; if SIGNED-UNKNOWN dominates, the eligibility gate is the bottleneck. This matters because the co-batching ablation (G4) needs a regime where batches actually form.

### Experiment G1b: Main attack scaling (planned)
**Method.** Same N sweep, with false-physical-lane claimant at **b=f** (shoulder) and **b=f+1** (cliff/control). Log the **actual elected proposer** every run — do not assume veh0.
**Expected.** b=f: breach rate = f(operating point) per the lane-channel model. b=f+1: breach ≈ 1.0 (Byzantine votes alone certify — this is a control demonstrating attack viability, NOT a system failure). Co-occupancy tracks false-cert. **[Verify these runs exist; may not be done yet.]**

---

## GROUP 2 — Perception-channel characterization (the model validation)

### Experiment G2a: Lane channel — adversary/defender/environment (DONE)
**Method.** Continuous-lateral gate **[verify gate variant in data]**. Fixed δ×b grid plus k-sweep and σ-sweep, 20 reps/cell.
- Adversary axis: sweep δ (lateral claim displacement) × b, fixed k=3, σ=0.5.
- Defender knob: sweep k ∈ {1,2,3}, fixed δ=1.75, σ=0.5, all b.
- Environment axis: sweep σ_lat, fixed δ=1.75, k=3, all b.
**Files.** `two_lane_delta_b_full_*`, `k_sweep_full_*`, `sigma_sweep_full_*`.
**Expected outcome.**
- **Coupling:** `false_certificate_rate == conflicting_cooccupancy_rate` in every cell, `background_conflicting_cooccupancy_rate == 0`. (Held 0/N mismatches in prior runs — this is the structural property that a fooled cert becomes a real breach.)
- **Shoulder/cliff:** breach flat-low for b < f, lifts at b=f, = 1.0 at b=f+1.
- **b-threshold slides with δ:** near-boundary lie (high q0) breaches at low b; blatant lie (low q0) needs b≈f.
- **Binomial prediction** `BinomialTail(h, q0, f+1−b)` tracks empirical within Wilson95, **using measured h** (not nominal). Mild conservative undershoot at high-q0 cells (weak positive witness correlation) is expected — flag if the *shape* diverges, not if it's slightly under.
- **k monotonic:** tighter k → lower q0 → lower breach. **σ monotonic:** higher σ → higher q0 → higher breach.
- **Anchor cell** (δ=1.75, k=3, σ=0.5) must agree across all three grids within seed noise. **[Prior check found δ×b grid gave 0.60 vs 0.85 in the k/σ sweeps at b=1 — RECONCILE: confirm whether seed variance or a δ/q0 identity drift between grids. Report pooled value.]**
- **Collapse:** pooling all cells, breach vs q0 falls on the binomial regardless of whether q0 came from δ, k, or σ. (This is the strongest single artifact — "everything reduces to q0.")
**Sanity checks.** `honest_lane_accepts / h_lane ≈ q0` and constant across b at fixed (δ,σ,k) — this ratio being b-dependent was a real bug previously; confirm it's flat.

### Experiment G2b: Longitudinal / distance-ordering channel (E3) [verify status]
**Method.** Per-car distance-to-intersection certificate; ordering derived, not sensed as a relation. Two-panel:
- Panel 1 (sensor calibration): raw per-witness pair-inversion vs separation s, overlaid `Φ(−s/(√2·σ_lon))`. The √2 is because two noisy observations are compared.
- Panel 2 (protocol, b=f): committed **per-pair** inversion vs s, overlaid with the certificate-composition prediction (q_dist + measured h + b_sig). **No √2 here** — the gate certifies absolute per-car distance, not pairwise comparison.
**Expected.** Same shoulder/cliff structure on the longitudinal axis; per-pair prediction tracks; whole-queue ordering reported **empirically only** (chained pairs share a car → does not factor into an independent binomial). Accident type is **ordering violation / same-lane hazard**, NOT conflicting co-occupancy. b=f+1 = cliff only. **[Verify the full longitudinal grid has been run; calibration + smoke passed previously.]**

---

## GROUP 3 — Attack-defense integrity table (N=16, few seeds each)

These are **binary integrity demonstrations**, not rate sweeps. Metric = pass/fail on the checks, plus recovery. **Do NOT expect throughput columns here** — a single forced-attack run's throughput is not meaningful; performance lives in Group 1.

Two non-negotiable checks per leader attack (both present in prior runs):
- `attack_executed_by_actual_certified_proposer` — the attack provably fired from the *actual elected* proposer (makes the result falsifiable).
- correct-value-committed — the system recovered to the correct certified value, not just rejected the attack.

| Row | Attack | Defense | Expected outcome |
|---|---|---|---|
| A | Honest control | — | 0 false cert, 0 co-occupancy (baseline) |
| B | False physical-lane evidence | evidence gate | breach = f(b,δ,σ) — the G2a grid |
| C | False longitudinal evidence | evidence gate | inversion = f(b,s) — the G2b grid |
| D | Announcement equivocation | one-variant-per-witness (§8) | no witness double-signs; no two conflicting variants both commit; safe |
| E | Proposer mutates physical lane/lateral | Check 10 | rejected; correct physical value committed; view-change to non-Byz successor |
| F | Proposer upgrades UNKNOWN→direction | Check 10 (§1.12) | rejected; UNKNOWN committed; view-change |
| G | Proposer suppresses > f certs | Check 9 | rejected; all suppressed certs committed signed; view-change |
| H | Silent proposer | view-change | timeout → non-Byz successor → full honest order committed |
| I | Consistent liar (declare+signal d, execute d′) | — (deferred) | **NOT prevented by certification gate; motivates conformance monitoring.** Report as limitation, not a catch. |
| J | Gossip suppression | honest-to-honest gossip | honest replicas achieve complete cert set despite f Byz non-forwarders; recovery unaffected |

**Files.** `leader_attack_validation_summary.json` (E–H), `equivocation_validation_summary.json` (D), `attack_run_metadata.json`, per-vehicle `16veh_*.json`.

**Recovery model (state correctly in analysis).** A corrupted proposal that fails Check 9/10 **cannot reach quorum**, so recovery is **view-change to a new honest leader who re-proposes from its own gossip-complete certs** — there is no "in-place field correction" path. Detection (Check 9/10) and recovery (view-change) are two steps, not alternatives. So E/F/G should each show a view-change record like H. **[Verify E/F/G actually record a view-change / successor_primaries; prior H showed successor=[1]. If E/F/G committed correct values WITHOUT a recorded view-change, flag it — it contradicts the recovery model.]**

**Checks to confirm added (from prior review):**
- D: explicit "no two conflicting variants both committed"; framing is "no witness double-signs," NOT "neither variant reaches f+1" (one variant got 8 echoes in prior run — that's fine, the point is no witness backs two).
- E/F/G/H: successor held ≥ deposed leader's certified set at view-change (gossip completeness). G is the sharpest test (leader suppressed f+1 certs; honest replicas must have had them via gossip anyway).
- Note operating-point differences: F ran at signal_error=1.0 (to deterministically force UNKNOWN) — intentional, label it.
- Multi-Byzantine successor (worst case): one scenario where the successor is also Byzantine, forcing consecutive view-changes, to show recovery reaches an honest leader within f+1 changes. **[Verify if run.]**

**Seeds.** Run 3–5 seeds per attack (not 1) — a single seed can pass by election/timing luck. Prior runs were rep 0 only; confirm multi-seed on the full batch.

---

## GROUP 4 — Direction / co-batching ablation [status: design]

**Method.** Three configurations at the operating point, mixed maneuvers:
- eligibility-ON (f+1 cue support required)
- eligibility-OFF (declared direction trusted)
- co-batching-OFF (all-singleton)
Fix the maneuver **ratio** across configs; vary only which cars get which maneuver by seed (isolate the gate effect from traffic composition).
**Expected.** all-singleton = safe, low throughput (ceiling/floor). eligibility-off = higher throughput but false-direction unlocks unsafe co-batches. eligibility-on = recovers most throughput without the unsafe batches. Metric = safe throughput (throughput + unsafe-co-batch rate), singletons split by cause.
**Prerequisite / caveat.** Needs a regime where batches actually form. If G1's `batch_size ≈ 1.0` persists (50% left ratio), this ablation shows ~no throughput difference and the direction-gate justification is not visible. **Resolve the batch-size issue (likely a straighter maneuver mix) before running.**

---

## GROUP 5 — Deferred / future work (state as such, do not fabricate results)
- **Rollback / cancellation** under perception noise (CANCEL machinery exists §15; evaluation deferred).
- **Crash recovery** under noisy BLOCKED/CLEAR (§15 scenario 16; evaluation scope [verify]).
- **RSU resilience** (Mathesh's component).
- **Conformance monitoring / CANCEL reason 2** (row I's defense; not implemented).
- **Combined leader attack** (suppression + mutation) — pre-scripted, non-adaptive; deferred until D and G solid solo.

---

## What the analyst should produce
For each experiment with data: (1) does the outcome match "Expected"? (2) any anomaly, quoted from the data with the file/field? (3) for G2a specifically, the collapse plot (breach vs q0, all cells, binomial overlay) and the anchor-cell reconciliation. (4) for Group 3, a filled attack×defense table with the actual rejecting check and recovery record per row, and explicit confirmation of the two non-negotiable checks. Do not claim a defense works if the check field is absent — report it as "not evidenced in data."