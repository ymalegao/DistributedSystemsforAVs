# Probabilistic Direction and Batching in PicaBFT

## Short answer

No, imprecise direction does **not** mean that batching is impossible.

It means the scheduler can no longer interpret one announced direction as certain merely because `f+1` witnesses signed it. There are three reasonable policies:

1. **Threshold to one direction before PBFT.** If the physical-evidence gate has sufficiently high confidence in one direction, retain the current scheduler. If not, make the vehicle a singleton.
2. **Carry a set of plausible directions into scheduling.** Co-batch two vehicles only if every combination of their plausible directions is safe. This is the recommended design if direction uncertainty is meant to be a real part of the contribution.
3. **Carry a probability distribution and threshold collision risk.** This preserves the most throughput, but changes the scheduler more substantially and gives only model-conditional safety.

PBFT itself does not need to change in any of these designs. PBFT will still agree deterministically on proposal bytes. What may change is the content of those bytes and the deterministic executor applied after agreement.

The central distinction is:

> PBFT decides which certified evidence everyone will use. It cannot make uncertain physical evidence true.

## 1. What the current code actually does

### 1.1 Direction begins as a local configuration value

Each vehicle reads `intendedDirection` from its NED configuration and places it in its announcement. The current four-way configurations set every vehicle to straight, and the SUMO route files also contain only straight routes such as `N2C C2S` and `E2C C2W`.

Therefore, the current system has direction labels in its protocol and scheduler, but the principal traffic scenarios do not yet exercise honest left- and right-turning trajectories.

### 1.2 The arrival gate does not perceive direction

On receipt of an announcement, `handleArrivalAnnouncement()` calls:

```cpp
verifyCarPosition(ann.carId, ann.laneId, ann.positionInLane, 1e9)
```

The `1e9` tolerance makes the position comparison effectively irrelevant. `verifyCarPosition()` currently checks:

- that the vehicle exists;
- that its exact TraCI lane ID equals the claimed lane ID; and
- nominally, that lane position is within the supplied tolerance.

After this check succeeds, the code copies the announcement's direction directly into local state:

```cpp
vs.direction = ann.direction;
```

There is no independent direction observation in this path.

### 1.3 Every echo repeats the announced direction

`sendArrivalEcho()` signs a tuple containing the announcement's direction and then assigns:

```cpp
echo.direction = ann.direction;
```

The claimant collects matching echoes. `collectArrivalEcho()` requires each echo direction to equal the claimant's own `VehicleState.direction`, and an `f+1` certificate is assembled from those echoes.

`validateArrivalCert()` subsequently verifies that all counted signatures cover the same lane, queue position, direction, and ambulance flag.

Consequently, the current certificate proves:

> At least `f+1` identities signed the same direction string after the honest signers passed the lane gate.

It does **not** prove:

> At least `f+1` witnesses independently observed that the vehicle would execute that direction.

### 1.4 The certified direction is copied into the PBFT proposal

`proposeAll()` converts each certificate into a `ResdbVehicleEntry`. Its `direction` field is one byte:

```text
0 = straight
1 = left
2 = right
```

Check 10 in `resdb_omnet_bridge.cc` verifies that a PBFT proposal's direction exactly matches the locally held certificate. This prevents a Byzantine PBFT leader from changing the certified direction. It does not establish that the certified direction matches the future physical maneuver.

This is an important correction to the current Check 10 comment that calls the certificate “ground truth.” Check 10 establishes **proposal-to-certificate integrity**, not certificate-to-physical-world truth.

### 1.5 Direction directly controls co-batching

After PBFT commits the entries, `BuildIntersectionSchedule()` greedily constructs batches. `SafeWithWholeBatch()` calls `IsSafeToBatch()` for every pair already in the candidate batch.

`IsSafeToBatch()` accepts only exact tuples of:

```text
(lane A, direction A, lane B, direction B)
```

listed in the fixed `kSafe` table. If a vehicle is QUIET, the scheduler already isolates it in a singleton batch.

The current behavior is therefore:

```text
announced direction
  -> copied into f+1 echo signatures
  -> copied into ARRIVAL_CERT
  -> copied into ResdbVehicleEntry
  -> exact kSafe lookup
  -> co-batch or singleton
```

## 2. What Claude's response gets right

Claude correctly identifies that direction is not independently perceived today and that `f+1` cryptographic signatures do not make a noisy physical claim true.

It also correctly separates:

- a probabilistic physical-evidence gate;
- deterministic `f+1` certificate rules;
- unchanged deterministic PBFT; and
- deterministic scheduling over whatever evidence PBFT committed.

The proposed ATTESTED/COMMITTED distinction is useful:

- lane and position can be physically observed, although imperfectly;
- direction before entering the intersection is partly a declaration or commitment;
- turn signals, lane geometry, and approach motion provide evidence about direction, but not certainty.

The recommendation to keep SUMO/Veins and insert a noisy perception adapter is also consistent with the code. TraCI can remain hidden ground truth for evaluation while protocol decisions consume noisy per-witness observations.

## 3. Two corrections or cautions to Claude's proposal

### 3.1 The current position check is weaker than Claude assumed

Claude describes a perfect position-and-lane oracle. The code currently supplies a position tolerance of `1e9`, so the active arrival gate is essentially an exact **lane** oracle, not a meaningful position verifier.

The probabilistic work should therefore start by defining what “position” means:

- continuous longitudinal/lateral position;
- stop-zone membership;
- lane identity; and
- queue rank.

These should not remain conflated in one call.

### 3.2 Detecting a direction deviation after turning may be too late

Claude proposes detecting a direction violation after maneuver onset and triggering CANCEL. This is useful as defense in depth, but it is not automatically a preventive safety mechanism.

If two vehicles in the same batch have already been released, observing that one began the wrong turn may occur after both vehicles are non-recallable. CANCEL cannot undo simultaneous entry into the conflict zone.

Therefore, the primary batching rule must already be safe for the uncertainty the system admits. Post-commit conformance monitoring should be described as:

- a way to stop later batches;
- a way to exclude or penalize a deviating vehicle;
- a rollback trigger when enough stopping time remains; and
- an empirical mitigation, not a proof that an already released batch is safe.

If the fault model permits a Byzantine vehicle to choose an arbitrary maneuver after release, deterministic safe co-batching with that vehicle is impossible unless one of the following holds:

- its physical controller is trusted and enforces the committed route;
- the schedule reserves a conflict-free reachable set covering every maneuver it could execute; or
- it crosses alone.

This limitation should be explicit in the paper.

## 4. Batching design choices

### 4.1 Design A: confidence threshold, then use the current scheduler

For each witness `i` observing target vehicle `v`, compute a direction posterior from its local evidence:

```text
p_i(L), p_i(S), p_i(R)
```

The witness echoes the claimed direction `d_claim` only if:

```text
p_i(d_claim) >= tau_direction
```

The claimant still needs `f+1` distinct qualifying echoes. If it obtains them, the certificate contains one discrete direction and the rest of the pipeline remains unchanged:

```text
confidence-based witness accept/reject
  -> f+1 matching echoes
  -> one certified direction
  -> unchanged ResdbVehicleEntry
  -> unchanged PBFT
  -> unchanged kSafe scheduler
```

If the vehicle does not obtain a direction-qualified certificate by the discovery deadline, place it in the existing QUIET/singleton path.

This does not eliminate batching. It means:

- high-confidence vehicles can be co-batched exactly as today;
- low-confidence vehicles cross alone;
- increasing `tau_direction` generally reduces unsafe acceptance but also reduces batch size and throughput.

That safety-throughput curve is likely the simplest answer to the professor's requested “knob.”

#### Benefits

- Smallest implementation change.
- PBFT payload and executor remain byte-for-byte unchanged.
- Existing QUIET singleton behavior supplies the fallback.
- Easy to explain and sweep experimentally.

#### Limitations

- It discards the rest of the direction distribution.
- `f+1` may contain only one honest noisy accept when there are `f` Byzantine signers.
- A wrongly accepted hard label can still authorize an unsafe batch.
- A deliberate post-certification deviation remains outside the gate's guarantee.

### 4.2 Design B: certify a plausible-direction set and batch robustly

This is the strongest practical response to “how can we batch if direction is imprecise?”

Instead of forcing each witness to output one direction, it outputs a bit mask:

```text
LEFT     = 001
STRAIGHT = 010
RIGHT    = 100
```

For example:

```text
{straight}             -> 010
{straight, right}      -> 110
{left, straight, right}-> 111
```

Each honest witness constructs a credible set `M_i(v)` from its posterior. One possible deterministic rule is to choose the smallest set whose total probability is at least `1 - alpha_direction`.

Each echo signs its own direction mask. Once at least `f+1` valid echoes exist, the certificate uses the conservative union:

```text
M_cert(v) = M_1(v) union M_2(v) union ... union M_k(v)
```

or, in code, a bitwise OR of the included masks.

The union rule has a useful Byzantine property. Any `f+1`-signer certificate contains at least one honest witness when at most `f` witnesses are Byzantine. A Byzantine signer can widen the union and force conservative scheduling, which is a liveness/throughput attack, but it cannot remove possibilities contributed by the honest signer. The remaining physical error is the probability that the honest witness's credible set omitted the true maneuver.

The deterministic scheduling rule becomes:

```text
SafeSets(u, v) = true
iff
for every d_u in M_cert(u)
and every d_v in M_cert(v):
    IsSafeToBatch(lane_u, d_u, lane_v, d_v) == true
```

The batch-growing algorithm otherwise stays the same: a candidate may join only if `SafeSets(candidate, member)` holds for every existing member.

#### This still permits useful batching

Under the current `kSafe` table:

- Opposite approaches with exact straight directions may co-batch.
- Opposite approaches whose sets are both `{straight, right}` may also co-batch, because all four straight/right combinations for opposite approaches appear in the current safe table.
- Perpendicular vehicles generally cannot co-batch when either might go straight; exact right/right pairs can.
- If `left` remains possible for either vehicle, it cannot co-batch under the current table because the table contains no safe pair involving direction code `1` (left).

Thus uncertainty reduces batching selectively; it does not eliminate it globally.

#### Required code changes

- Add a `direction_mask` to `ArrivalEcho` and `ArrivalCert`.
- Sign the mask rather than copying one unqualified direction claim.
- Define deterministic certificate-mask aggregation and validation.
- Add a three-bit `direction_mask` to `ResdbVehicleEntry`, or reinterpret/expand the current direction representation.
- Update the cert snapshot used by Check 10 so the leader cannot narrow the certified mask.
- Replace exact `IsSafeToBatch()` calls with the universal `SafeSets()` rule.
- Keep the PBFT algorithm, quorum rules, and transport unchanged.

This changes the PBFT **application value**, not PBFT itself.

#### Confidence knob

`alpha_direction` controls the tradeoff:

- Smaller `alpha_direction` requests greater credible-set coverage, producing wider direction sets, fewer co-batches, and lower omission risk.
- Larger `alpha_direction` produces narrower sets, more co-batches, and greater risk that the true direction lies outside the certified set.

This gives a direct probabilistic safety-throughput curve.

### 4.3 Design C: probability-of-conflict scheduling

If the certificate carries a canonical distribution `P_v(d)`, calculate:

```text
P_conflict(u, v)
  = sum over d_u,d_v of
      P_u(d_u) * P_v(d_v) * Conflict(lane_u,d_u,lane_v,d_v)
```

Allow co-batching only when the risk is below `alpha_batch`.

This retains more throughput than the universal-set rule, but it requires:

- fixed-point probability encoding;
- a deterministic fusion rule that every replica reproduces;
- protection against Byzantine witnesses inventing confidence;
- a batch-level risk rule rather than only pairwise risk; and
- careful claims that safety is conditional on calibration and the assumed observation model.

It is a good stronger-version design, but not the safest first implementation.

## 5. Recommended direction design

The recommended plan is staged.

### Minimal implementation

Implement Design A first:

1. Each witness computes confidence in the claimed direction.
2. It echoes only above `tau_direction`.
3. `f+1` matching echoes retain the existing discrete certificate.
4. Certified vehicles use the existing scheduler.
5. Ambiguous or uncertified vehicles use the existing singleton path.

This directly produces the professor's requested curve without changing PBFT or the executor.

### Stronger ICRA implementation

Implement Design B if direction uncertainty is intended to be a central contribution:

1. Witnesses sign plausible-direction masks.
2. The certificate conservatively unions at least `f+1` masks.
3. PBFT agrees on the certified masks.
4. The executor uses universal-safe set comparison.
5. Direction-mask width becomes a measurable explanation for lost or retained batching.

Design B gives a more honest answer than treating a posterior mode as certain, while remaining much simpler and more defensible than full probability-of-conflict scheduling.

## 6. What evidence can provide direction confidence?

The current repository does not simulate turn signals or infer direction from motion. These inputs must be added explicitly.

Useful evidence includes:

- the announced direction, treated as a declaration;
- the observed turn signal, generated through a configurable noisy channel;
- lane/map restrictions, such as turn-only lanes;
- lateral lane position;
- heading or yaw rate once turning begins;
- route priors for the approach; and
- recent trajectory history.

At the current stop-controlled intersection, heading, yaw, and braking are weak before release because every vehicle approaches in a straight lane and stops. Therefore, pre-entry direction may remain genuinely ambiguous.

The simulation should represent that ambiguity rather than manufacture certainty. For an honest vehicle, SUMO's assigned route is hidden ground truth. A witness sees only derived noisy cues. For example:

```text
P(observed left signal | actual left) = 0.90
P(observed left signal | actual straight) = 0.02
P(no signal observation) = function of distance/occlusion
```

The exact values should be swept and justified, not treated as universal facts.

## 7. Simulator work needed before a direction experiment is valid

### 7.1 Add real turning trajectories

The current route files use straight movements only. Add SUMO connections and route definitions for left and right turns from every approach. Honest `intendedDirection` must agree with the assigned hidden route.

Add explicit adversarial cases:

- claim left, execute straight;
- claim straight, execute left;
- signal left, execute straight;
- omit the signal;
- equivocate by announcing different directions to different witnesses; and
- change route after certification, if SUMO/TraCI supports the desired injection cleanly.

### 7.2 Replace the current straight-only physical safety monitor

`TraCIScenarioManager::pollIntersectionCooccupancy()` currently records only the approach and explicitly assumes all configured routes are straight. It treats same-approach and opposite-approach occupancy as safe.

That monitor is not valid after left/right routes are introduced. For turning experiments, safety metrology should use one of:

- SUMO's physical collision output as the primary collision metric;
- actual route/movement IDs and a complete movement conflict matrix;
- geometric conflict-zone occupancy for each movement; or
- trajectory intersection with temporal overlap.

The protocol must not use this oracle monitor to decide schedules. It is evaluation ground truth only.

### 7.3 Keep route truth out of the protocol path

SUMO knows the assigned route. That route must not be read directly by `handleArrivalAnnouncement()` or the certificate validator. It may be used only to:

- generate noisy observable cues;
- inject deliberate deviations; and
- calculate evaluation metrics.

Otherwise the new direction gate would reproduce the same perfect-oracle problem under a different name.

## 8. Concrete code plan

### Phase 0: tests for current behavior

Before changing formats, add scheduler unit tests covering all lane/direction pairs in `kSafe`, QUIET singleton behavior, and whole-batch pairwise compatibility.

Add an end-to-end test demonstrating the current weakness:

1. A vehicle announces direction `d_claim`.
2. Its SUMO route executes `d_actual != d_claim`.
3. Honest witnesses still echo `d_claim` because they only verify lane.
4. The certificate forms and Check 10 accepts it.
5. The scheduler uses `d_claim`.

### Phase 1: perception adapter

Create `ResDBPerception.h/.cc` with a per-witness API such as:

```cpp
PerceptionSample observeVehicle(const std::string& targetId);
DirectionBelief inferDirection(const PerceptionSample&, const ArrivalAnnouncement&);
```

Keep TraCI reads inside this adapter. Apply seeded noise separately for each witness/target pair. Preserve a `sigma=0` regression mode.

### Phase 2A: minimal hard-direction gate

Modify `handleArrivalAnnouncement()` to require both:

```text
state confidence >= tau_state
direction-claim confidence >= tau_direction
```

Only then call `sendArrivalEcho()`. Leave the current echo/cert/PBFT formats and scheduler unchanged.

Log:

- witness posterior;
- accepted/rejected direction claim;
- reason for rejection;
- resulting certificate status; and
- singleton versus co-batched outcome.

### Phase 2B: direction-mask gate

If adopting the stronger design:

- extend `ArrivalEcho` and `ArrivalCert` in `ResDBIntersectionApp.h`;
- update echo/cert serialization and signatures in `ResDBArrivalProtocol.cc`;
- aggregate and validate the mask deterministically;
- extend `ResdbVehicleEntry` and `ResdbCertEntry` in `resdb_omnet_bridge.h`;
- pack and snapshot the mask in `ResDBDecision.cc`;
- make Check 10 compare the exact mask in `resdb_omnet_bridge.cc`; and
- replace the scheduler's scalar-direction check in `resdb_intersection_scheduler.cc` with universal mask compatibility.

### Phase 3: conformance monitoring

Once a vehicle enters an internal junction lane, compare its actual observed movement with its certified direction or mask. A movement outside the certified set can trigger a new CANCEL reason.

Treat this as defense in depth. Measure whether the deviation is detected before conflicting vehicles become non-recallable; do not assume that rollback always prevents a collision.

### Phase 4: experiments

Sweep:

- position/lane noise;
- direction threshold or credible-set coverage;
- turn-signal reliability;
- number of Byzantine witnesses from `0` through `f`, plus `f+1` as the breach point;
- correlated observation error;
- packet loss and observation latency; and
- direction deviation attacks.

Report:

- physical collisions;
- unsafe conflict-zone co-occupancy;
- false-safe co-batches;
- fraction of singleton vehicles;
- mean and tail delay;
- throughput and batch-size distribution;
- direction-set coverage/calibration; and
- safety-throughput curves.

## 9. Recommended experiments specifically about batching

The batching experiments should separate three questions.

### Experiment B1: honest uncertainty

Vary `alpha_direction` or `tau_direction` with no Byzantine vehicles.

Expected result: conservative thresholds widen masks or reject hard labels, increasing singleton scheduling and delay while reducing false-safe batches.

### Experiment B2: Byzantine direction claim

Up to `f` Byzantine witnesses support a false direction. Measure false certificates and unsafe co-batches as honest sensor uncertainty increases.

This exposes the “resilience shoulder” instead of a perfect zero/one cliff.

### Experiment B3: declaration versus execution

Allow a vehicle to obtain a certificate for one direction and physically execute another.

Compare:

- current scalar-direction batching;
- scalar direction plus conformance monitoring;
- universal direction-mask batching; and
- all-singleton scheduling.

This experiment demonstrates the difference between certifying a claim and controlling future physical behavior.

### Experiment B4: retained batching under uncertainty

Construct cases where uncertainty does not force serialization:

- opposite approaches, each with `{straight, right}`;
- exact right/right movements;
- exact opposite straight movements.

Compare them with cases where uncertainty necessarily forces singleton treatment:

- any set containing left under the current `kSafe` table;
- perpendicular approaches where straight remains possible; and
- fully unknown `{left, straight, right}` sets.

This directly answers the concern that probabilistic direction destroys all concurrency: it does not. It removes only concurrency that is not safe for the admitted uncertainty.

## 10. Final recommendation

For the immediate implementation, use the existing scheduler with a direction-confidence gate and singleton fallback. This is enough to make the current binary physical gate probabilistic and produce a safety-throughput curve.

For the strongest direction story, use certified plausible-direction masks and universal-safe batching. It preserves deterministic PBFT, preserves deterministic execution, retains concurrency when uncertainty is compatible with the conflict matrix, and makes the safety tradeoff explicit.

The paper should not claim that the system knows a driver's future direction. It should claim that the system:

1. collects Byzantine-bounded, noisy evidence about a declared or plausible movement;
2. represents remaining uncertainty conservatively;
3. uses PBFT to agree on that certified representation; and
4. co-batches only when the certified uncertainty is compatible with safe concurrent movement.

That is a stronger and more honest result than either assuming perfect direction or abandoning batching entirely.
