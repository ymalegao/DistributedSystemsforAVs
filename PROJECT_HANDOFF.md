# Project Handoff: `v2v` (Distributed Systems for AVs)

This is the handoff for the overall repository at `/Users/yashmalegaonkar/Documents/v2v`.
It is written so a new agent can start being useful quickly: what the project is,
what is actually runnable today, where the important code lives, and what to do next.

## What This Repo Is

An OMNeT++ / Veins simulation of V2V intersection coordination with consensus.

There are multiple “eras” of implementation in this repo:

1. **Current hot path (recommended): ResilientDB PBFT over Veins (C++ only).**
2. **Legacy / reference: Java BFT-SMaRt + JNI integration.**
3. **Other research notes and experimental artifacts** (RAFT notes, protocol drafts, papers).

The project is ultimately about: vehicles reaching a stop zone, exchanging enough
authenticated state, agreeing on a safe crossing order, and then enforcing that
order via SUMO/TraCI movement control inside Veins.

## Quick Orientation (What’s Runnable)

The README is accurate for day-to-day usage and build order:
- `README.md`
- `ARCHITECTURE.md` (canonical system description for ResDB-over-Veins)

Core runnable pieces:
- `veins-veins-5.3.1/` (OMNeT++/Veins with custom apps)
- `fourway/` (scenario configs, launchd XMLs, run scripts, analysis scripts)
- `incubator-resilientdb/` (ResilientDB + the OMNeT bridge build via Bazel)

Legacy runnable pieces (not the hot path anymore, but still present):
- `bftsmart/` (Java BFT-SMaRt + older integration)
- `test-v2v-consensus.sh` (standalone BFT-SMaRt test harness; hard-coded paths)

## Current Architecture (ResDB-over-Veins)

The canonical runtime path (also in `ARCHITECTURE.md`) is:

```text
OMNeT++ / Veins vehicle app
  -> ResDBIntersectionApp C++ arrival-cert protocol
  -> ResilientDB PBFT through resdb_omnet_bridge
  -> 802.11p radio frames for all inter-vehicle traffic
  -> direct C++ order callback
  -> TraCI vehicle control
```

Key invariants to remember:
- **One vehicle = one PBFT replica.**
- **All inter-vehicle traffic goes through Veins 802.11p frames** (lossy, async).
- **Simulation thread boundary matters:** ResDB worker threads enqueue; OMNeT++ thread applies.
- **Physical-to-digital firewall:** “arrival certificates” (witness signatures after TraCI checks) happen before PBFT proposal.
- **Consensus decides bytes; movement is applied by the vehicle app** after batch-clearance checks via TraCI.

## Important Code + Config Locations

ResDB app + protocol:
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc`
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.h`
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.ned`

Scenario / OMNeT configs:
- `fourway/omnetpp.ini` (contains multiple configs; includes legacy BFT configs too)
- `fourway/*.sumo.cfg`, `fourway/*.rou.xml`, `fourway/*.net.xml`, `fourway/*.launchd.xml`

Run wrapper for ResDB scenarios:
- `fourway/run-resdb-simulation.sh` (supports `--randomize`, `--byzleader`, `--leader`, `--channel-metrics-dir`)

Experiment orchestration + output structure:
- `experiment_orchestrator.py`
- Outputs under `benchmarks/Priority<N>cars/<scenario_subdir>/run_<rep>/`

ResDB bridge + build root:
- `incubator-resilientdb/` (Bazel build, `//integration/omnet:resdb_omnet_bridge`)

## Build + Run (Typical Flow)

Prereqs:
- OMNeT++ installed separately (README assumes 6.2.0) and its `setenv` sourced.
- Java 11 (still needed if you touch legacy BFT-SMaRt tools; not required for ResDB hot path).

Build (high level, see `README.md` for exact commands):
1. Build Veins: `veins-veins-5.3.1/`
2. Build `fourway/` if needed (often it’s linked against the Veins build)
3. Build ResDB OMNeT bridge (Bazel) if you changed ResDB or bridge code

Run a single ResDB simulation (example):
- Use `fourway/run-resdb-simulation.sh` with an OMNeT config like `FourVehiclesResDB`, `TwelveVehiclesResDB`, etc.

Run a batch of experiments:
- Use `experiment_orchestrator.py` which:
  - regenerates ResDB keys per N,
  - runs `fourway/run-resdb-simulation.sh` for scenario x repetition,
  - runs analysis (`fourway/analyze_log.py`).

## Fault Model + Scenarios (Current ResDB Path)

ResDB path assumes PBFT with `N = 3f + 1`, up to `f` Byzantine nodes per epoch.

The simulation supports scenario variants like:
- No ambulance / with ambulance
- Byzantine follower behavior
- Byzantine leader behavior (silent leader)

Randomization helpers (in `fourway/run-resdb-simulation.sh`):
- `--randomize <N> <F>` picks one ambulance and F byzantine followers
- `--byzleader <ID>` reserves a silent leader replica
- `--no-ambulance` disables ambulance selection
- `--leader <ID>` overrides initial leader

## Output Artifacts (Where to Look)

Experiments:
- `benchmarks/` is the main experiment output root (per README + orchestrator).

Fourway analysis / plots / debugging:
- `fourway/analyze_log.py`
- `fourway/diagnose_pbft_delivery.py`
- `fourway/analyze_collisions.sh`
- `fourway/results/` and `fourway/graphs/` (repo-local result directories)

Logs:
- `experiment_orchestrator.py` uses `/tmp/resdb-simulation.log` by default for the run log.

## Legacy / Reference Material (Still Useful)

Even though the ResDB path is the hot path, the legacy Java/BFT-SMaRt material is
still useful for protocol semantics and historical decisions:

- `JAVA_SIDE_HANDOFF.md` explains the old Java responsibilities and exact JNI handoff points.
- `bftsmart/` contains the old replica logic, verifier checks, scheduler logic, and leader-change behavior.
- `test-v2v-consensus.sh` is an older BFT-SMaRt harness (note: it hardcodes `/home/...` paths).

Research notes / planning:
- `newprotocol_plan.txt` (older “Genesis Protocol” walk-through; treat as historical context)
- `partner_questions_raft_timing.md`, `LC_PROTOCOL_RESEARCH_SUMMARY.md`, `LC_INVESTIGATION.md`
- `papers/` (reading material)

## Current “State of the Project”

Based on repo docs and configs, the current intended “real” system is:
- ResDB PBFT replication over Veins radio frames,
- arrival certification using TraCI checks plus witness ECDSA signatures,
- deterministic executor producing batch assignments,
- vehicles applying movement in batches with clearance polling.

The thesis writeup under `writing/` is documenting these systems, but the
implementation truth lives in `veins-veins-5.3.1/`, `fourway/`, and
`incubator-resilientdb/`.

## High-Leverage Next Steps (For Any Agent)

Pick one track:

1. **Repro / run track**
   - Run a small N scenario (4 or 8 vehicles) end-to-end with ResDB.
   - Confirm outputs land under `benchmarks/` and analysis scripts run.

2. **Correctness / protocol track**
   - Reconcile documentation vs code for message types (1/4/5/8/9) and thresholds (`f+1`, `2f+1`).
   - Audit timer constants and stop-zone logic in `ResDBIntersectionApp`.

3. **Performance / evaluation track**
   - Verify metric definitions against what the simulator actually logs.
   - Ensure experiment seeds and randomization are reproducible (orchestrator already pins a master seed).

4. **Cleanup / maintainability track**
   - Update or quarantine scripts that have machine-specific hard-coded paths (`test-v2v-consensus.sh`).
   - Keep `README.md` and `ARCHITECTURE.md` aligned with what is currently used.

