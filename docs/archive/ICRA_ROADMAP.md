# ICRA Roadmap: PicaBFT Rollback, Resilience, and Evaluation

This document organizes the current paper-design discussion into concrete fixes,
experiments, and priorities for an ICRA submission built from the PicaBFT
capstone and the current ResDB-over-Veins architecture.

## Target Paper Positioning

The strongest paper is not "BFT for intersections." Prior work already studies
BFT-style V2V coordination. The stronger claim is:

> PicaBFT-R provides Byzantine-resilient intersection coordination that keeps
> safety grounded in physical evidence before consensus and recovers from
> post-commit hazards through cancel certificates, local halting, epoch
> tombstones, and dynamic rollback membership.

The ICRA extension should make rollback the headline contribution. The capstone
firewall, arrival certificates, pre-verify checks, priority certification, and
Raft baseline become the foundation. The new paper should show what happens
when the physical world changes after consensus has already decided.

## Core Contributions to Claim

1. Physical-to-digital admission firewall:
   Arrival certificates admit only witness-backed observable state into
   consensus.

2. Byzantine proposal firewall:
   Followers pre-verify proposal structure, cert omissions, and certified state
   fields before voting.

3. Post-commit rollback:
   Cancel certificates trigger immediate local halt for recallable vehicles,
   tombstone stale epochs, and restart discovery for a new epoch.

4. Dynamic rollback membership:
   Rollback PBFT commits over a proposal-defined membership `M` that excludes
   departed, wrecked, and non-recallable vehicles and includes new emergency
   participants.

5. Characterized resilience envelope:
   The paper should show where the protocol decides, where it conservatively
   halts, and where assumptions are exceeded.

## Claims to Avoid

- Do not claim to invent BFT intersection coordination.
- Do not imply the system can prevent a vehicle from physically disobeying a
  committed order.
- Do not present tunable `f` as a free performance knob.
- Do not claim stable tolerance after vehicles leave if quorums are recomputed
  downward.
- Do not make RSUs sound trusted; if included, they are ordinary replicas that
  improve quorum persistence.

## Highest Priority Design Fixes

### 1. Quorum Erosion and Fallback

Safety tolerance should remain tied to the configured assumption for the epoch.
If an epoch starts from `N = 20, f = 6`, departures should not silently shrink
the quorum to `N = 16, f = 5` if the adversary may not have departed. Honest
departures erode liveness headroom, not safety.

Paper sentence:

> Departures erode the protocol's ability to decide, never its ability to stay
> safe; when the remaining responsive set cannot satisfy the configured quorum,
> rollback falls back to conservative halt.

Implementation / evaluation needs:

- Define when rollback uses original epoch quorum vs forced `M` quorum.
- State the exact safety assumption for forced `M`.
- Add a fallback run where quorum erodes and rollback refuses to decide.
- Plot or table: responsive replicas, required quorum, decision/fallback result.

### 2. Rollback Concurrency and Re-Entrancy

Only one rollback instance should be active per cancelled epoch. Competing
cancel reasons should be merged into a single justification payload or
deterministically ordered.

Required rules:

- One rollback instance per cancelled epoch `e`.
- Multiple `CANCEL_CERT`s for `e` become one merged rollback justification.
- Rollback for `e + 1` follows the same rules if a new hazard occurs.
- Tombstones form an ordered chain and stale decision gossip for tombstoned
  epochs is ignored.

Required experiments:

- Dual-trigger: ambulance and crash trigger near-simultaneously.
- Re-entrant rollback: a hazard occurs during epoch `e + 1` recovery.

### 3. Direction Is Attested, Not Witnessed

Witnesses can verify lane, position, and priority credential. They cannot
directly verify intended direction. The paper should split fields into:

- Witnessed: lane, position, presence, priority credential.
- Attested: intended direction / maneuver commitment.

Strongest design:

- Keep direction in the signed commitment.
- During execution, detect deviation from the attested maneuver.
- Treat deviation as a canonical cancel reason.
- Trigger rollback and apply repeated-stall / repeated-deviation penalties.

Required scenario:

- A Byzantine vehicle attests straight.
- The scheduler co-batches it under the certified claim.
- The vehicle deviates toward an unsafe turn.
- Nearby vehicles detect trajectory deviation.
- `CANCEL_ECHO` messages form a `CANCEL_CERT`.
- Recallable vehicles halt and rollback commits a safe new epoch.

Reason reference format candidate:

```text
deviation:e:batch:b:vehX:attested=straight:observed=left
```

Also include a short design-space paragraph on geometric containment:
scheduling only movements safe under all lane-reachable directions. This is
safer but may reduce throughput; it can be discussed or lightly measured.

### 4. Griefing Stalls

A physically stalled vehicle really does require re-coordination whether it is
malicious or broken. The attack is repeated stall-induced rollback.

Mitigation:

- Rate-limit rollbacks per originating vehicle or reason reference.
- If a vehicle repeatedly causes crash-cancel / deviation-cancel reasons, stop
  co-batching it and schedule it as a singleton.
- Treat this as throughput degradation, not a safety failure.

Required experiment:

- Sustained stall adversary.
- Compare throughput and emergency delay with and without singleton penalty.
- Show bounded degraded service rather than repeated global collapse.

### 5. Rollback Leader Selection

Rollback proposer must be deterministic and must be a member of `M`.

Candidate rule:

```text
lowest-id responsive RSU in M, else lowest-id recallable vehicle in M,
rotated on timeout
```

If RSUs are cut from the paper, use:

```text
lowest-id recallable vehicle in M, rotated on timeout
```

Current architecture notes forced-M rollback view-change as unsupported. Do not
claim "vote them out" until retry / rotation is implemented and tested.

## RSU Scope

RSUs are useful but severable. They should not be allowed to consume the paper.

Best framing:

> RSUs are ordinary replicas that do not depart. They improve quorum persistence
> and sensing coverage, not authority.

Important rules:

- Four RSUs can help small-N and late-rollback liveness.
- No RSU is individually trusted.
- Failure budget is shared across vehicles and RSUs.
- State independent-compromise assumptions clearly.

Certificate diversity rule:

- At most `f` RSU echoes count toward an `f + 1` certificate, or
- When mobile vehicles are present, require at least one mobile-vehicle echo.

Required RSU tests if included:

- No RSU vs 4 RSUs under quorum erosion.
- Byzantine RSU plus Byzantine vehicle.
- Phantom vehicle injection with and without diversity rule.
- Phantom ambulance rollback trigger with and without diversity rule.

Cut rule:

- If schedule or page budget is tight, keep RSUs as a half-column discussion and
  one small matrix/table, or move them to future work.

## Experiment Plan

### A. Baseline 2x2

The capstone comparison must separate the certificate firewall from PBFT.

Run:

```text
{Raft, PBFT} x {firewall off, firewall on}
```

Expected story:

- Raft + firewall blocks follower input lies.
- Raft + firewall still cannot survive Byzantine leaders.
- PBFT + firewall pays extra cost to survive malicious proposal / leader cases.

This is the cleanest cost-benefit claim.

### B. Rollback Money Plot

Headline plot:

```text
delay saved for emergency vehicle vs late-arrival offset Delta
```

Where `Delta` is the time between epoch `e` commit and emergency arrival.

Report:

- Emergency wait time.
- Halt latency after cancel evidence.
- Cancel-cert formation latency.
- Rollback consensus latency.
- Time to new epoch execution.
- Number of recallable vehicles.
- Number of departed / non-recallable vehicles.
- Whether fallback engaged.

### C. N=16 or N=20 Late Ambulance Scenario

Base timeline:

1. Initial wave arrives and forms epoch `e`.
2. Epoch `e` commits.
3. Batch 0 begins crossing.
4. One or more vehicles clear or become non-recallable.
5. Ambulance arrives late and forms arrival cert.
6. Witnesses issue `CANCEL_ECHO`.
7. `CANCEL_CERT` forms.
8. Recallable vehicles halt locally.
9. Epoch `e` is tombstoned.
10. Rediscovery begins for epoch `e + 1`.
11. Rollback proposal commits under `M`.
12. Ambulance receives priority in the new schedule.

Sweep:

- Emergency arrival before commit: included in normal epoch.
- Emergency arrival shortly after commit: best rollback case.
- Emergency arrival after some vehicles leave: dynamic `M`.
- Emergency arrival after quorum erosion: conservative fallback.

### D. Crash / Unsafe Batch Scenario

Use either:

- Firewall-off lane-tamper crash detection from the capstone, or
- Deviation-triggered rollback where a vehicle violates attested direction.

Measure:

- Detection latency.
- Cancel-cert latency.
- Vehicles halted before conflict-box entry.
- Whether stale order is suppressed.
- Resulting safe order for epoch `e + 1`.

### E. Mis-Set-f / Accountable Degradation

Do not sell lower `f` as free speed.

Run:

- Assumed `f` lower than actual Byzantine count.
- Show which property fails first.
- Record whether there is cryptographic evidence: conflicting certs, equivocal
  signatures, incompatible cancel certs, or malformed proposal logs.

Claim only:

> Misconfigured resilience degrades visibly and accountably; it is not a safety
> guarantee under exceeded assumptions.

### F. Channel and Network Stress

Report:

- Channel busy ratio vs N.
- Frame loss vs N.
- SINR / interference.
- Background 10 Hz CAM/BSM traffic.
- Message bytes per vehicle.
- Decision-gossip recovery rate.

Add:

- Random interference in maps.
- Transport-independence paragraph for C-V2X.
- Optional small-N OpenCV2X / C-V2X-like run if feasible.

### G. Generalization

Minimum:

- Randomized Byzantine vehicle placement.
- Turn-ratio sweep at one N.
- T-intersection variant.
- inD-derived or realistic demand profile if available.

State clearly:

> This paper studies single-intersection coordination; multi-intersection
> networks are future work.

## Latency Reduction Plan

Legitimate optimizations that do not weaken safety:

- Start discovery during approach, not only at the stop zone. (We do this already)
- Keep stopping as timeout fallback, not mandatory if consensus already decided.
- Cache / relay arrival certs.
- Keep epidemic cert relay and decision gossip.
- Tune jitter and retry intervals from measured CBR.
- Use fast local halt before rollback consensus.
- Prioritize cancel evidence frames and emergency cert frames.
- Consider threshold-signature or aggregated-signature future work, but do not
  add it unless implementation time is realistic.

Latency numbers should include or explicitly model:

- ECDSA signing and verification costs.
- PBFT worker-thread scheduling.
- Radio contention.
- Background beaconing.

## CARLA and Perception Plan

CARLA should replace perfect TraCI as the witness observation source in a small
number of focused scenarios.

Use CARLA to provide:

- Vehicle presence near stop line.
- Lane occupancy.
- Approximate position / queue order.
- Emergency-vehicle detection from class, lights, siren proxy, or role signal.
- Occlusion / false-negative cases.

Do not try to rebuild the whole evaluation in CARLA. Use it to answer:

> Does the witness-certificate firewall still work when observations are noisy
> perception outputs rather than perfect simulator truth?

Suggested scenarios:

- Honest late ambulance with partial occlusion.
- False ambulance claim rejected by credential / perception mismatch.
- Phantom vehicle attempt rejected by lack of diverse witnesses.
- Direction deviation detected during execution.

## NAVSIM / Dataset Plan

Datasets such as inD / rounD are more useful for realistic demand and geometry
than for emergency preemption, because emergency vehicles are usually absent.

Use datasets for:

- Arrival timing distributions.
- Turn ratios.
- Queue lengths.
- Occlusion / visibility patterns.
- Scenario replay into SUMO demand.

Use ITS emergency vehicle preemption literature to anchor emergency-delay
numbers. The paper should compare conceptually against deployed signal
preemption, not claim the same infrastructure assumptions.

## Metrics and Plots

Safety:

- Unsafe co-batch commits.
- False priority grants.
- Stale tombstoned order applications.
- Uncertified vehicle admissions.
- Phantom vehicle / phantom ambulance acceptance.

Rollback:

- Cancel-cert formation latency.
- Halt latency.
- Rollback commit latency.
- Emergency delay saved.
- Recallable vs non-recallable count.
- Fallback rate.
- Tombstone suppression count.

Performance:

- Throughput.
- Mean and tail wait time.
- Priority-vehicle wait time.
- Message bytes per vehicle.
- Channel busy ratio.
- Frame loss.
- ECDSA verification cost.

Resilience envelope:

- Decide / rollback / fallback regions.
- Assumption-backed vs future-work regions.
- Mis-set-f failure shape.

## Page-Budget Discipline

Recommended 8-page shape:

- Intro + related work: 1.75 pages.
- Model + threat assumptions: 0.75 pages.
- Firewall + pre-verify protocol: 1.25 pages.
- Rollback protocol: 1.5 pages.
- Evaluation: 2 pages.
- Discussion / limitations / references: remaining space.

RSUs should be cuttable. If included, give them one compact subsection and one
small matrix/table.

## 4-6 Week Execution Order

### Week 1: Design Hardening

- Specify witnessed vs attested fields.
- Define deviation-triggered cancel reason.
- Specify one-rollback-per-epoch and merged justifications.
- Specify quorum erosion fallback.
- Decide whether RSUs are in or out for the main submission.

### Week 2: Implement Critical Rollback Fixes

- Add deviation-triggered rollback scenario.
- Add rollback concurrency / tombstone checks.
- Add conservative fallback when quorum is unavailable.
- Add rollback proposer rotation only if claiming Byzantine rollback-leader
  recovery.

### Week 3: Core Experiments

- Run late-ambulance rollback sweeps.
- Run crash / deviation rollback.
- Run stale-gossip tombstone tests.
- Run fallback under quorum erosion.

### Week 4: Comparative Claims

- Run `{Raft, PBFT} x {firewall off, firewall on}`.
- Run mis-set-f degradation.
- Randomize adversary placement.
- Add channel-load and background beaconing plots.

### Week 5: Robotics Grounding

- Add T-intersection and turn-ratio sweep.
- Add inD-derived demand if available.
- Add CARLA mini-scenarios if integration is stable.
- Add emergency preemption literature anchor.

### Week 6: Paper Assembly

- Build the resilience-envelope figure.
- Audit every figure against a claim.
- Compress related work to contrast-only.
- State assumptions and limitations directly.
- Keep RSUs only if they sharpen, not distract from, rollback.

## Strong Acceptance Checklist

- The rollback money plot is clear and compelling.
- The paper has a theorem or precise argument for quorum erosion and fallback.
- The paper includes one demo where rollback prevents a post-commit hazard from
  becoming stale unsafe execution.
- The 2x2 baseline separates firewall value from PBFT value.
- Direction is handled honestly as attested, not sensor-verified.
- Mis-set-f is characterized, not marketed.
- Channel load is reported with background traffic.
- Every envelope region is backed by an experiment, proof sketch, or explicit
  assumption.

