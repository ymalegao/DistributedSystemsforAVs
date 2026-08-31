# PicaBFT experiment command catalog

This file is the command-level index for implemented, retired, and planned
experiments. Run commands from the repository root:

```bash
cd /Users/yashmalegaonkar/Documents/v2v
```

The commands below assume the terminal is already inside the OMNeT++/opp_env
environment. `ORCHESTRATOR_SKIP_OMNET_SOURCE=1` prevents the orchestrator from
trying to enter the environment again:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --help
```

From a fresh terminal, activate the environment first:

```bash
/Users/yashmalegaonkar/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh
```

General rules:

- Run simulations sequentially. Do not launch two orchestrators at once.
- Add `--dry-run` to any orchestrator command to inspect its matrix first.
- Resumable matrices reuse a run only when its metadata and required artifacts
  match exactly.
- Use `--start-rep N` only for generic scenario runs. Fixed matrices own their
  repetition indices and implement their own resume policy.
- `b=f` is shoulder evidence. `b=f+1` is a Byzantine cliff/control.
- Analysis uses Byzantine support actually available or present in the relevant
  certificate; configured `b` is not substituted for measured `b_sig`.
- The current locked joint operating point is `sigma_lat=0.5 m`,
  `sigma_long=1.0 m`, `k=2`, a `0.25 s` certificate collection window, and
  `signal_error=0.20` where maneuver-cue noise is active.

---

## 1. Legacy system and capstone scenarios

### Legacy six-scenario scale suite — original honest/Byzantine and ambulance matrix

Smoke, one repetition at N=4:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 4 --reps 1
```

Full default suite, five repetitions at N=4/8/12/16/20:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 4 8 12 16 20 --reps 5
```

The six default rows are honest/no-ambulance, Byzantine follower/no-ambulance,
Byzantine leader/no-ambulance, honest ambulance, Byzantine follower/ambulance,
and Byzantine leader/ambulance.

### Honest-only system run — basic protocol sanity

Smoke:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 1 --reps 1
```

Twenty repetitions:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 1 --reps 20
```

### Legacy fault sweep — injected FALSE_LANE followers versus provisioned f

Smoke at N=16; runs injected faults `0..f+1` with paired nested selections:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 6 --sweep-f --reps 1
```

Statistical version:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 6 --sweep-f --reps 20
```

To separate provisioned tolerance from injected faults, use `--tolerate F` and
`--inject-f B` on scenario 3, 6, or 7.

### Existing all-way-stop baseline — SUMO throughput/delay anchor

Smoke:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --baseline --config 4 --scenario 1 2 --reps 1
```

Scale run:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --baseline --config 4 8 16 20 --scenario 1 2 --reps 20
```

This is a throughput/delay anchor only; it does not have a Byzantine-security
claim. A fixed-time traffic-light baseline is still planned and has no CLI yet.

### Firewall and certificate-gate comparisons — legacy integrity rows

Firewall-off versus firewall-on tampered-lane leader, one smoke each:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 12 14 --reps 1
```

Fake-ambulance evidence gate off versus on:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 10 11 --reps 1
```

Firewall-off versus firewall-on Byzantine-leader fake ambulance:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 9 13 --reps 1
```

---

## 2. Phase 1 and categorical Phase 2

### Phase 1 validation — zero-error regression and nonzero wiring smokes

Runs E0 at N=4/N=16, signal error 1 at N=4, and categorical approach error at
N=4:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase1-validation
```

Dry run:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase1-validation --dry-run
```

### Phase 2A fixture validation — mixed maneuvers and cue characterization

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase2-fixture-validation
```

This validates the N=16 mixed route fixture and characterizes native/controlled
maneuver cues during the echo window.

### Phase 2B self-attestation validation — self echo plus Phase 1 closure

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase2-self-attestation-validation
```

### Phase 2C authenticated attack validation — categorical E2/E4 accounting

Fixed validation over `b=0..f+1`, zero error, and representative nonzero error:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase2-attack-validation
```

Focused wrong-approach E2 example:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --reps 1 --experiment e2 --inject-b 5 \
  --attack-target 0 --approach-sigma 1
```

Focused false-direction E4 example:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --reps 1 --experiment e4 --inject-b 5 \
  --attack-target 0 --signal-error 0.20
```

Focused false-distance/rank E2-LONG example:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --reps 1 --experiment e2long --inject-b 5 \
  --attack-target 0 --longitudinal-sigma 1 \
  --distance-claim-offset -5.1 --physical-gate-k 2
```

### Phase 2D metrology validation — actual movement and physical calibration

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase2-metrology-validation
```

This validates ingress/egress-derived actual movement, the checked-in conflict
table, pinned SUMO physics, and conflicting/non-conflicting calibration pairs.

### Frozen categorical E1–E4 pilot — original probabilistic gate result

One run per cell, 88 sequential runs:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase2-pilots --phase2-pilot-profile grid
```

Full statistical profile, 384 unique runs:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase2-pilots --phase2-pilot-profile full
```

These are frozen categorical-channel results. Do not relabel them as the
continuous adjacent-lane experiment.

---

## 3. Continuous lateral and longitudinal perception

### Continuous-coordinate calibration — standalone lane and stopped-distance math

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --continuous-coordinate-calibration
```

Direct two-lane calibration tests:

```bash
cd fourway/two_lane_calibration
python3 run_calibration.py
python3 -m unittest test_calibration.py -v
cd ../..
```

### Adjacent-lane wiring validation — retired straight-road mechanism smoke

Run the four validation cells:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --adjacent-lane-validation
```

Reanalyze existing artifacts only:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --adjacent-lane-validation-reanalyze
```

The old 310-run straight-road matrix is retired. `--adjacent-lane-grid` and
`--adjacent-lane-grid-reanalyze` intentionally return an error.

### Full two-lane fixture validation — lane authority, rank noise, and Check 10

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-validation
```

This runs the N=16 zero guard, shoulder, cliff, honest rank-noise, Check 10
mutation, and conflict-release validation cells.

### Conflict-release validation — complete evidence-to-physics chain

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-conflict-release-validation
```

Required chain:

```text
false lane evidence -> scheduling authority -> conflicting co-batch
-> conflict-zone co-occupancy
```

### Joint lateral plus longitudinal validation — locked operating point

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-combined-validation
```

Runs honest, `b=f`, and `b=f+1` cells at `sigma_lat=.5`, `sigma_long=1`, and
`k=2`. The lateral channel controls scheduling authority; longitudinal evidence
controls same-lane rank.

### Focused b=1 accounting smoke — attempt support versus finalized b_sig

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-b1-smoke
```

### Delta x Byzantine count — lane attack operating characteristic

Smoke, 18 runs: `delta={1.61,1.75,2.0} x b={1..6}`:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-delta-b-smoke
```

Full grid, 360 attack runs plus one honest control:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-delta-b-grid
```

Completion rerun, retaining all departures/wait/throughput metrics:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-delta-b-completion-grid
```

### k-knob sweep — safety/throughput operating point

Smoke, 18 runs: `k={1,2,3} x b={1..6}`:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-k-sweep smoke
```

Full, 360 runs:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-k-sweep full
```

Fixed values are `delta=1.75 m` and `sigma_lat=0.5 m`.

### Lateral-sigma sensitivity — environmental sensor-quality axis

Smoke, 42 runs: seven sigma values by `b={1..6}`:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-sigma-sweep smoke
```

Full, 840 runs:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-sigma-sweep full
```

The sigma axis is `{0.1,0.3,0.5,0.7,1.0,1.5,2.0}` with `delta=1.75 m` and
`k=3` for this sensitivity figure.

### Honest operating-point sweep — q1, departures, wait, and throughput

Smoke, 24 wiring cells:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-honest-operating-sweep smoke
```

k-only full run at `sigma_lat=0.5`, 60 runs:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-honest-operating-sweep k-full
```

Full honest `k x sigma` surface, 480 runs:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-honest-operating-sweep full
```

### Locked full two-lane matrix — historical 400-run matrix

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-grid
```

Reanalysis only:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-grid-reanalyze
```

### Longitudinal/rank channel — sensor and protocol two-panel result

Four-cell smoke at the selected `k=2`:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --longitudinal-grid --longitudinal-grid-profile smoke --physical-gate-k 2
```

Full 186-run grid at `k=2`:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --longitudinal-grid --longitudinal-grid-profile full --physical-gate-k 2
```

Reanalyze and regenerate the figure without simulation:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --longitudinal-grid-reanalyze --longitudinal-grid-profile full \
  --physical-gate-k 2
```

Focused distance-rank Check 10 mutation/recovery:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --distance-rank-check10-validation
```

---

## 4. Direction, co-batching, and scaling

### Left-heavy direction ablation — sensitivity fixture, 4S/8L/4R

Six-row smoke:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-ablation
```

Full 120-run sensitivity matrix:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-ablation-full
```

This remains a LEFT-heavy sensitivity result, not the headline throughput mix.

### Straight-heavy direction ablation — headline 8S/4L/4R fixture

First validate all four checked-in fixture rotations and meaningful batching:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-straight-heavy-prerequisite
```

Six-row smoke: honest/FALSE_DIRECTION crossed with eligibility on, eligibility
off, and all-singleton scheduling:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-straight-heavy
```

Full 120-run paired-seed ablation:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-straight-heavy-full
```

The full command is resumable. Eligibility ON converts insufficient cue support
to SIGNED-UNKNOWN; eligibility OFF trusts the authenticated declaration;
all-singleton disables all co-batching.

### Arm B perfect-cue concurrency — same 8S/4L/4R fixture, `signal_error=0`

The locked `signal_error=0.20` headline leaves most cars as SIGNED-UNKNOWN, so
eligibility-on stays near singleton even though `kSafe` already allows
STRAIGHT/RIGHT co-batches. Arm B keeps that scheduler and fixture, and sets
cue noise to zero so ON can actually form size-2 batches.

Six-row smoke (honest/FALSE_DIRECTION × on/off/all-singleton):

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-arm-b
```

Full 120-run paired-seed matrix after the smoke passes:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-arm-b-full
```

Success criterion: eligibility-on mean batch ≥ 1.3 with zero unsafe
co-occupancy; eligibility-off under FALSE_DIRECTION still shows the veh0/veh1
conflict. This is not Exp 1 and not a LEFT-in-`kSafe` change.

### Arm C cue-quality sweep — same fixture, `signal_error` ∈ {0, 0.05, 0.10, 0.20, 0.30}

Arm B is only the perfect-cue point. Arm C is the mechanism curve: as cues
worsen, eligibility-ON should see SIGNED-UNKNOWN rise and mean batch fall
toward singleton while staying safe; eligibility-OFF should keep co-batching
and still unlock the FALSE_DIRECTION conflict.

Smoke (20 runs: 5 ε × honest/FALSE_DIRECTION × on/off; no all-singleton):

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-arm-c
```

Full 200-run paired-seed sweep after the smoke passes (10 reps):

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-direction-arm-c-full
```

Curve pass checks: ON under attack stays unsafe=0 / false-eligibility=0 across
ε; OFF under attack stays unsafe=1; honest ON batch falls and UNKNOWN rises
from ε=0 to ε=0.30.

### Honest two-lane scaling — N=4/8/16/20 at the locked operating point

Four-run smoke:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-scale-smoke
```

Full 80-run honest scale experiment, 20 repetitions per N:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-scale-honest-full
```

### Adversarial two-lane scaling — 12-run validation smoke

Runs one paired seed at each `N={4,8,16,20}` for three rows: honest control,
false-physical-lane evidence at `b=f` (probabilistic shoulder), and at `b=f+1`
(cliff/control). The locked operating point is `sigma_lat=.5 m`,
`sigma_long=1 m`, `delta=1.75 m`, `k=2`, and signal error `.20`. The harness
uses actual finalized-certificate `b_sig`; configured `b=f+1` is not assumed to
mean all Byzantine signatures reached the collector.

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-scale-adversarial-smoke
```

Dry-run inspection (no simulation):

```bash
python3 experiment_orchestrator.py \
  --two-lane-scale-adversarial-smoke --dry-run
```

Full statistical matrix after the smoke passes:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-scale-adversarial-full
```

The full command executes 160 new attack runs (`4 N × 2 attack roles × 20
repetitions`) and reuses the completed 80 honest runs, producing one 240-run
summary. Attack seeds are byte-identical to the corresponding honest `(N,rep)`
seed. The command is resumable and stores compact analyzer JSON while retaining
all per-vehicle wait samples and one complete run-wide metrics map. A
resume-aware disk preflight reserves 4 MiB per incomplete cell plus 128 MiB of
working headroom and refuses to launch if that minimum is unavailable.

---

## 5. Attack-defense integrity demonstrations

### D: announcement equivocation — one variant per witness/target/epoch

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --attack-defense-equivocation-validation
```

### E–H: Byzantine proposer attacks — Check 9/10 and view-change recovery

Runs physical-lane mutation, UNKNOWN-direction upgrade, certificate suppression,
and silent-primary recovery:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --attack-defense-leader-validation
```

### J: certificate-gossip suppression

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --attack-defense-gossip-validation
```

### K: consecutive Byzantine primaries

Silent r0, silent successor r1, then honest recovery through r2:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --attack-defense-chain-validation
```

### Complete D–H/J/K multiseed suite

Smoke/default, three seed groups:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --attack-defense-full-validation
```

Five-seed result used in the current evaluation:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --attack-defense-full-validation --reps 5
```

Every Byzantine-proposer result must show that the mutation actually fired,
which check rejected it, the view change, and the correct honest value that
eventually committed.

---

## 6. Rollback and crash recovery

### Late emergency rollback — scenario 15, N=18 identity universe

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 18 --scenario 15 --reps 1
```

### CANCEL proposer suppression — unguarded versus guarded

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 18 --scenario 17 18 --reps 1
```

Scenario 17 is the stalled ablation; scenario 18 enables deterministic proposer
failover and should recover.

### Crash, BLOCKED/WAIT/CLEAR, and recovery ORDER — scenario 16

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 16 --reps 1
```

### Fabricated CLEAR evidence — unguarded versus guarded

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --config 16 --scenario 19 20 --reps 1
```

Scenario 19 disables the recovery CLEAR evidence gate; scenario 20 retains the
gate and must reject the fabricated CLEAR before honest recovery.

---

## 7. Plot and reanalysis commands

Generate the current two-lane ICRA plots from completed summaries:

```bash
python3 fourway/plot_two_lane_icra.py
```

Regenerate the selected-k longitudinal two-panel figure explicitly:

```bash
python3 fourway/plot_longitudinal_two_panel.py \
  --grid-summary benchmarks/Phase2DistanceGridK2/longitudinal_grid_summary.json
```

Reanalysis commands are preferable to rerunning simulations when only a parser,
aggregate, confidence interval, table, caption, or plot changes.

---

## 8. E7 noisy crash rollback and recovery

One persistent wreck is injected only after the selected committed vehicle is
verified as the physical front of its exact SUMO lane. Fixed `T_clear=1 s` and
`T_tow=15 s`; the only timer sweep is `T_blocked={.5,2,5} s`. Both profiles use
the frozen operating point (`sigma_lat=.5`, `sigma_lon=1`, cue error `.2`,
`k=2`) and execute strictly sequentially.

Nine-run smoke (three crash/control pairs and three integrity rows):

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --rollback-recovery smoke
```

Full profile: 20 repetitions per statistical crash/control row and five per
binary integrity row, 135 runs total:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --rollback-recovery full
```

Override every row's repetition count only for focused debugging:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --rollback-recovery smoke --reps 1 --start-rep 0
```

Artifacts are written under:

```text
experiments/e7_rollback_recovery/results/{smoke,full}/
```

## 9. E8 emergency-vehicle priority

Paired N=16 pre-decision rows compare the same `veh15` as a normal vehicle and
as an authenticated ambulance. A post-decision row injects a late normal
vehicle plus an authenticated ambulance after `ORDER(0)` and requires witnessed
CANCEL, a recovery order with signed ambulance authority, earliest-feasible
service after vehicles physically ahead in the same lane, all departures, and
safe physical execution.

Smoke (one paired seed, three rows):

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --emergency-priority smoke
```

The completed pre-decision pair can be collected independently (40 runs):

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --emergency-priority full --emergency-priority-scope predecision
```

Reproduce only the currently blocked late-arrival smoke while repairing
recovery consistency:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --emergency-priority smoke --emergency-priority-scope postdecision
```

Planned full profile (20 paired seeds, 60 runs) is locked until the complete
post-decision smoke passes:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --emergency-priority full
```

Artifacts are written under:

```text
experiments/e8_emergency_priority/results/{smoke,full}/
```

## 10. Planned experiments — no runnable CLI yet

The following are part of the evaluation roadmap but are not implemented
orchestrator commands. The proposed flag names below are placeholders only. Do
not run or cite them as completed.

### Main adversarial operating-point scale — N=4/8/16/20

The 12-run smoke passed and the full statistical profile is implemented. Its
statistical rows are false physical-lane evidence at `b=f` and `b=f+1`; the
completed 80-run honest scale supplies the honest row. Proposal-byte mutation
remains in the deterministic D-H/J/K integrity suite rather than this
probability experiment.

```bash
# IMPLEMENTED SMOKE
python3 experiment_orchestrator.py --two-lane-scale-adversarial-smoke

# IMPLEMENTED FULL
python3 experiment_orchestrator.py --two-lane-scale-adversarial-full
```

### Full baseline/ablation table

Planned rows: full system, physical-evidence gate off, firewall off,
eligibility off, all-singleton, fixed-time traffic light, and all-way stop.
All-way stop exists through `--baseline`; the unified table and fixed-time TLS
runner do not.

```bash
# NOT IMPLEMENTED
# python3 experiment_orchestrator.py --icra-baseline-smoke
# python3 experiment_orchestrator.py --icra-baseline-full
```

### Repeated observation K/tau experiment

This is distinct from lowercase `k`, the continuous residual tolerance. The
planned experiment uses repeated perception samples `K={1,3,5}` and an
acceptance fraction `tau`. Phase 2 currently remains `K=1`.

```bash
# NOT IMPLEMENTED
# python3 experiment_orchestrator.py --repeated-observation-sweep
```

### Consistent maneuver liar / post-commit conformance

Planned attack: declare RIGHT, show RIGHT, execute STRAIGHT. The present cue gate
cannot prevent this because the pre-entry evidence is internally consistent.
The experiment should measure conformance-detection latency and physical
outcome; CANCEL/tow mitigation is separate work.

```bash
# NOT IMPLEMENTED
# python3 experiment_orchestrator.py --direction-conformance-validation
# python3 experiment_orchestrator.py --direction-conformance-full
```

### Correlated perception error

Planned small common-mode episode demonstrating where the independent-binomial
model stops fitting. This is a model-limit experiment, not a replacement for
the independent-error matrix.

```bash
# NOT IMPLEMENTED
# python3 experiment_orchestrator.py --correlated-perception-validation
```

### Combined fixed, non-adaptive attack selection

Planned fixed attack combinations may choose one lie type before the run, but
must never inspect honest RNG outcomes and then select whichever attack happens
to succeed. Any combined suppression-plus-mutation case must be scripted and
logged before perception begins.

```bash
# NOT IMPLEMENTED
# python3 experiment_orchestrator.py --combined-attack-defense-validation
```

### RSU resilience

Mathesh's RSU branch remains separate. Merge its exact CLI into this catalog
only after the branch, quorum semantics, and artifact paths are reviewed.

```bash
# NOT IMPLEMENTED ON perception-error
# Command pending integration from the RSU branch.
```

### Multi-round rollback, CANCEL/tow response, and dynamic membership

Repeated live rollback, response to conformance failure, and dynamic identity
membership remain future work. The existing recovery scenarios use a static,
pre-provisioned identity universe and request-scoped active membership.

```bash
# NOT IMPLEMENTED
# No command until membership/reset semantics and recovery safety are complete.
```
