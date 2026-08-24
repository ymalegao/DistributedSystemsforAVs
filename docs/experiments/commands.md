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

## 8. Planned experiments — no runnable CLI yet

The following are part of the evaluation roadmap but are not implemented
orchestrator commands. The proposed flag names below are placeholders only. Do
not run or cite them as completed.

### Main adversarial operating-point scale — N=4/8/16/20

Planned rows: honest, false-evidence Byzantine followers, and an actual elected
Byzantine proposer, all at the locked joint operating point. This must log the
actual elected proposer and precommit every attack before perception draws.

```bash
# NOT IMPLEMENTED
# python3 experiment_orchestrator.py --two-lane-scale-adversarial-smoke
# python3 experiment_orchestrator.py --two-lane-scale-adversarial-full
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

### Recovery under noisy BLOCKED/CLEAR perception

Planned metrics: missed BLOCKED, false CLEAR, recovery latency, and safe halt.
Use separate crash-observation noise parameters; do not silently reuse lateral
or signal error as though they were the same sensor channel.

```bash
# NOT IMPLEMENTED
# python3 experiment_orchestrator.py --noisy-crash-recovery-smoke
# python3 experiment_orchestrator.py --noisy-crash-recovery-full
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
