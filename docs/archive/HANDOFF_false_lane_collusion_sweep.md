# Handoff: coordinated FALSE_LANE harness and cross-scale F sweep

## Scope of this session

This session implemented the planned defensive experiment harness for coordinated
Type-1 `FALSE_LANE` witnesses. The repository was already heavily modified before
this session, so **do not use the full working-tree diff as the boundary of this
work and do not revert unrelated changes**. The list below identifies the edits
made during this session.

The implementation is not finished: the first N=16 sweep exposed two harness
defects described under **Known defects / next fixes**.

## Changes made

### Coordinated Type-1 witness behavior

Files:

- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.ned`
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.h`
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc`
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBArrivalProtocol.cc`
- `veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBDecision.cc`

Added NED parameters:

- `falseLaneColluderIds`: comma-separated replica IDs selected by
  `--randomize N F`.
- `enableArrivalPositionGate`: defaults true; false is the D3 physical-witness
  ablation.

At initialization, each application parses `falseLaneColluderIds` into
`false_lane_colluder_ids_` and logs:

```text
[FALSE-LANE-COLLUDER-SET] r... N=... F=... ids=... position_gate=...
```

Type-1 behavior still emits the existing exact claim:

```text
laneId=BYZANTINE_FAKE_LANE lane=X
```

New helpers in `ResDBArrivalProtocol.cc`:

- `isExactFalseLaneClaim()`
- `shouldColludeOnFalseLane()`
- `isArrivalSignerEligible()`
- `collectArrivalEcho()`

A selected Type-1 replica endorses an exact `X` claim from another selected
Type-1 replica. A claimant also creates a local self-echo because radio broadcast
does not self-deliver. Received and local echoes use the same collector and
certificate assembly path.

The D3 toggle bypasses `verifyCarPosition()` only. Signature verification,
identity checks, certificate structure, PBFT, and bridge PreVerify remain active.

Added diagnostics:

- `FALSE-LANE-COLLUSION-ECHO`
- `FALSE-LANE-COLLUSION-CERT`
- `FALSE-LANE-COLLUSION-BLOCK`
- `FALSE-LANE-COLLUSION-COMMIT`

`BYZANTINE_BAD_PROPOSAL` recovery outcome was renamed from the misleading
`ORDER_COMMITTED_AFTER_MALFORMED_PROPOSAL` to:

```text
MALFORMED_PROPOSAL_REJECTED_AND_RECOVERED
```

### ARRIVAL_CERT validation hardening

`validateArrivalCert()` now additionally requires:

- signer ID in the configured discovery electorate (`0 <= id < totalVehicles`);
- distinct signer IDs;
- echo target/epoch/lane/position/direction/ambulance fields exactly match the
  enclosing certificate;
- `WitnessKeyRegistry` binds the embedded public key to `echoingReplicaId`;
- the signature verifies over those exact certificate fields.

`collectArrivalEcho()` performs the same semantic, key-binding, membership, and
signature checks before a claimant counts an echo locally.

### Wrapper and D3 plumbing

File: `fourway/run-resdb-simulation.sh`

Added:

- `--disable-arrival-position-gate`
- generation of `*.node[*].appl.falseLaneColluderIds = "..."`
- generation of `*.node[*].appl.enableArrivalPositionGate = false` for D3

Type-1 followers no longer get `byzantinePbftSilent=true`; other follower types
retain the old PBFT-silent setting. This isolates false witness data from PBFT
omission behavior.

### Orchestrator sweep support

File: `experiment_orchestrator.py`

Added:

- `--inject-f F`: decouples injected Byzantine count `F` from tolerated `f`.
- `--sweep-f`: expands each selected scale to `F=0..f+1`.
- result directories `F_<F>/run_<rep>`.
- a shared `FALSE_LANE_PAIRED` seed namespace for guarded scenario 3 and D3
  scenario 7.

Scenario 7 now uses `--disable-arrival-position-gate`, not the global
`--no-firewall` switch.

Smoke-sweep command:

```bash
python3 experiment_orchestrator.py \
  --config 4 8 12 16 20 \
  --scenario 3 7 \
  --sweep-f \
  --reps 1
```

### Physical ground-truth metrics

Files:

- `veins-veins-5.3.1/src/veins/modules/mobility/traci/TraCIScenarioManager.h`
- `veins-veins-5.3.1/src/veins/modules/mobility/traci/TraCIScenarioManager.cc`

Added an always-on TraCI observer that caches each vehicle's incoming physical
approach and checks concurrent internal-junction occupants. All current routes
are straight, so perpendicular approach pairs are reported as unsafe while
opposite approaches and same-approach following are not.

Diagnostics:

- `UNSAFE-CONFLICT-COOCCUPANCY`
- `PHYSICAL-COLLISION`

### Analyzer metrics and malformed-proposal classification

Files:

- `fourway/analyze_log.py`
- `incubator-resilientdb/integration/omnet/resdb_omnet_bridge.cc`

The bridge now emits `MALFORMED-PROPOSAL-REJECT` when the claimed vehicle count
requires more entry bytes than the request contains.

The analyzer no longer treats the old
`ORDER_COMMITTED_AFTER_MALFORMED_PROPOSAL` label as success. It parses the new
collusion, forged-certificate, commit, co-occupancy, collision, and malformed
proposal diagnostics.

Partner JSON now includes:

- `bft_stats.injected_f`
- `bft_stats.false_lane_colluder_ids`
- `bft_stats.arrival_position_gate_enabled`
- forged certificate and committed target counts
- unsafe co-occupancy pairs
- physical collision counts
- malformed proposal PreVerify rejection counts

## Verification completed

Passed:

```bash
python3 -m py_compile experiment_orchestrator.py fourway/analyze_log.py
bash -n fourway/run-resdb-simulation.sh
python3 experiment_orchestrator.py \
  --config 4 8 16 20 --scenario 3 7 --reps 1 --sweep-f --dry-run
```

The user built the native components successfully and ran the N=16 one-seed
sweep.

The supplied D3 result
`benchmarks/Priority16cars/no_fw_false_lane/F_6/run_0` behaved as expected:

- `tolerated_f=5`, threshold 6, injected `F=6`;
- arrival position gate disabled;
- six forged certificates assembled;
- six forged claims were committed;
- all 16 vehicles departed;
- no fallback was used;
- two ground-truth unsafe co-occupancy pairs were recorded;
- no SUMO physical collision event was recorded.

D3 `F=1` also admitted one forged certificate/claim and recorded one unsafe
co-occupancy pair, which is the intended gate-off ablation result.

Guarded `F=0..5` produced zero forged certificates, zero forged commits, zero
unsafe pairs, and all 16 vehicles departed. This is the expected defended region.

## Known defects / next fixes

### 1. The paired/nested selections are not actually deterministic

Although the orchestrator prints the same `RANDOM` seed for every paired cell,
the selected IDs differ across `F` and between scenario 3 and scenario 7. Example
from the N=16 run:

```text
guarded F=5: 6,7,9,10,12
guarded F=6: 0,1,3,10,11,14
D3 F=6:      2,4,9,11,12,14
```

Cause: the orchestrator executes:

```bash
export RANDOM=<seed> && run-resdb-simulation.sh ...
```

`RANDOM` is a Bash special variable and its inherited environment value is not
a reliable seed inside the separately executed wrapper shell.

Recommended fix:

1. Export a normal variable such as `SCENARIO_RANDOM_SEED` from the orchestrator.
2. After wrapper argument parsing and immediately before
   `generate_random_scenario`, assign:

   ```bash
   RANDOM="${SCENARIO_RANDOM_SEED}"
   ```

3. Verify that a fixed seed produces one permutation and `F=k` is the prefix of
   `F=k+1`, identically in guarded and D3 arms.

### 2. Guarded F=f+1 did not produce the required negative-control certificate

N=16 guarded `F=6` produced zero forged certificates. The likely cause is this
dedup in `sendArrivalEcho()`:

```cpp
if (collusionEcho &&
    !false_lane_collusion_echoes_sent_.insert({ann.epoch, ann.carId}).second) {
    return;
}
```

It permits only one radio transmission per colluder/claim. With packet loss,
the claimant may never receive all six signatures. D3 still succeeds because
honest replicas also endorse when the position gate is disabled.

Recommended fix:

- Deduplicate the claimant's **local insertion**, not radio retransmission.
- Let repeated/gossiped forged announcements cause a selected colluder to
  retransmit the same authenticated echo through the existing discovery retry
  behavior.
- The claimant collector already deduplicates by signer, so retransmission does
  not inflate the distinct signer count.
- Re-run guarded `F=6`; require at least one forged certificate and commit.
- Re-run guarded `F=5`; require zero forged certificates and commits.

### 3. Saved `.log` files are analyzer summaries, not complete raw logs

The detailed diagnostics were visible only in `/tmp/resdb-simulation.log` for
the final sweep cell. Per-cell files such as `16veh_0.log` contain the analyzer's
64-line summary. This prevents post-hoc debugging of earlier cells.

Recommended fix: before the next simulation overwrites `/tmp/resdb-simulation.log`,
copy the raw log into the cell directory as `raw_simulation.log`, then run
`analyze_log.py` against that copy. Keep the summary `.log` separately.

### 4. Analyzer outcome lines can be corrupted by native thread interleaving

The existing stdout corruption issue appeared again. Some
`CONSENSUS_ATTACK_OUTCOME` lines were interleaved with ResDB logs, so the generic
parser counted only four of six replica-local success outcomes. The new dedicated
`FALSE-LANE-COLLUSION-COMMIT` parser correctly recorded all six committed targets
in `run_safety`.

Do not use `attack_success_replica` counts as the cliff metric. Use:

- `false_lane_forged_certificate_count`
- `false_lane_committed_claim_count`
- `false_lane_committed_targets`
- unsafe co-occupancy counts

Longer-term, route structured metrics through a non-interleaved sink or emit
atomic preformatted lines.

## Immediate acceptance check after fixes

Run only the three critical N=16 cells first:

```bash
python3 experiment_orchestrator.py --config 16 --scenario 3 --inject-f 5 --reps 1
python3 experiment_orchestrator.py --config 16 --scenario 3 --inject-f 6 --reps 1
python3 experiment_orchestrator.py --config 16 --scenario 7 --inject-f 1 --reps 1
```

Required results:

- guarded `F=5`: 0 forged certs, 0 forged commits, 0 unsafe pairs;
- guarded `F=6`: forged cert/commit present (beyond threshold);
- D3 `F=1`: forged cert/commit present;
- identical seeded attacker prefixes across paired cells;
- every cell commits and all vehicles terminate.

