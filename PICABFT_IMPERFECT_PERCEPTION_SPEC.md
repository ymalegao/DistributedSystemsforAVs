# PicaBFT Probabilistic Physical-Evidence Gate

## Design and implementation specification

## 1. Purpose

This specification defines separate probabilistic arrival-certification and maneuver-eligibility stages for PicaBFT while preserving deterministic PBFT agreement and the existing hard-direction scheduler.

The experiment asks:

> As honest witnesses become imperfect at observing a vehicle's approach lane and maneuver cue, how do `f+1` arrival certification, evidence-gated co-batching, safety, and delay degrade?

The experiment SHALL produce a family of safety, certification, batching, and latency curves over physical-observation error.

Arrival certification operates over the fields used by the intersection protocol:

- an exact SUMO `laneId` string;
- a cardinal approach `lane` in `{N,S,E,W}`;
- a discrete `positionInLane` queue rank (`1` is the front vehicle);
- a declared `direction` in `{S,L,R}`; and
- the existing arrival time, epoch, ambulance, identity, and signature fields.

`positionInLane` refers exclusively to discrete queue rank. Continuous longitudinal position is not an ARRIVAL claim in this specification.

The initial experimental scope is:

1. noisy observation of the cardinal approach lane as the gate for arrival certification;
2. noisy observation of a turn-signal maneuver cue as evidence for co-batching eligibility only;
3. `f+1` lane-certificate formation and `f+1` positive maneuver-cue support;
4. three trust tiers: QUIET, SIGNED-UNKNOWN, and SIGNED with an eligible scalar direction;
5. the existing hard-direction scheduler, extended only by the `direction=3` UNKNOWN sentinel; and
6. Byzantine lane and direction claims under noisy honest witnesses.

Continuous pose, noisy queue-rank reconstruction, occlusion, observation latency, BLOCKED/CLEAR perception, Bayesian `P(D)` aggregation, credible-set scheduling, and probability-distribution scheduling are outside the initial scope.

V2X intersection protocols necessarily operate on declared maneuver intent; perception cannot certify a future actuation choice. PicaBFT therefore certifies observable arrival evidence, aggregates signed maneuver cues using `f+1` positive support, and permits concurrent crossing only when that evidence unlocks the declared direction for `kSafe`. PBFT agrees on both the certificate evidence and the deterministically derived eligibility outcome. Execution conformance is measured separately.

### 1.1 Experimental-axis taxonomy

Every experiment and figure SHALL distinguish three kinds of variables:

| Category | Variables | Meaning |
|---|---|---|
| Environment axes | approach-observation error `sigma_approach`, signal-observation error `epsilon_signal`, packet conditions | Properties imposed by the sensing environment; not chosen by the protocol operator |
| Adversary axes | actual colluder count `b`, attack type, claim/route/signal consistency | Threat conditions; not operator controls |
| Design knobs | provisioned tolerance `f`, sample count `K`, support threshold `tau` | Parameters selected by the system designer |

The safety-throughput operating envelope MUST be parameterized by a design knob. The preferred figure grammar is:

```text
x-axis       = environment severity
line family  = K/tau design settings
panel or row = fixed adversary condition b and attack type
```

The existing `f` sweep remains a provisioning-cost experiment. A `b` sweep is a threat experiment. They must not be described as the same kind of knob.

## 2. Implemented baseline and certification path

### 2.1 Announcement fields

`ArrivalAnnouncement` currently contains:

```text
carId
laneId
cardinal lane
positionInLane
direction
isAmbulance
claimedArrivalTime
epoch
ambulance credential fields
self-signature
```

The announcing vehicle obtains `laneId` from TraCI. Its cardinal `lane` normally comes from the explicit `intendedLane` NED parameter, with lane-ID parsing as a fallback.

`positionInLane` is not a meter position. `discoverLane()` builds a list of vehicles sharing the exact lane ID, sorts them by TraCI lane position, and assigns rank `1` to the front vehicle. `broadcastArrivalAnnouncement()` copies that rank into the announcement.

The commented-out continuous-position encoding is inactive.

### 2.2 Implemented witness check

The honest arrival path obtains one cached `ArrivalPerceptionSample` from
`ResDBPerception` for each `(witness,target,epoch)`. It compares the corrupted
cardinal `observedApproach` with `ann.lane`. Existing ambulance checks remain in
force, but signal evidence never vetoes an echo.

```text
echo = laneAccept && ambulanceChecks
```

The complete signed echo is cached by `(witness,target,epoch,claimHash)`.
Periodic or gossiped re-announcements resend the same logical verdict and do
not resample perception. `verifyCarPosition()` is no longer the arrival-gate
decision.

### 2.3 Implemented direction semantics

The declaration remains `ann.direction in {STRAIGHT,LEFT,RIGHT}`. Each echo
retains that declaration and separately carries signed
`observedCue in {STRAIGHT,LEFT,RIGHT,UNKNOWN}`. Direction code `3` is the
explicit UNKNOWN scheduler sentinel; unknown input never falls through to
STRAIGHT.

After a lane certificate forms, replicas count distinct valid echoes whose
`observedCue` equals the declaration. At least `f+1` positive cues unlock the
declared direction. Otherwise the entry remains SIGNED with `direction=3` and
is scheduled as a singleton.

### 2.4 Implemented authenticated claim binding

The originating vehicle signs, and every receiver verifies, a canonical
announcement payload containing:

```text
carId : epoch : laneId : lane : positionInLane : direction : isAmbulance : claimedArrivalTime
```

Each echo signs the declared fields, `observedCue`, epoch, signer identity, and
`claimHash=SHA-256(authenticated serialized announcement)`. Type-5 certificate
serialization preserves the common claim hash and every echo's cue.

Periodic broadcasts reuse a cached, byte-identical signed announcement for the
entire epoch. ECDSA values are serialized in fixed-width padded slots together
with their real signature length. This prevents nondeterministic DER signature
lengths from changing simulated packet airtime and perturbing seeded perception
draws.

### 2.5 Implemented batching semantics

`proposeAll()` converts each valid certificate into a `ResdbVehicleEntry` containing scalar lane, queue rank, and direction codes. PBFT agrees on those bytes.

After commitment, `BuildIntersectionSchedule()` uses the fixed `kSafe` table. A candidate joins a batch only if its exact `(lane,direction)` is safe with every member already in the batch.

QUIET entries are already forced into singleton batches.

The implemented code path is:

```text
ARRIVAL_ANNOUNCE
  -> cached noisy cardinal-lane observation
  -> lane-only echo admission with signed observedCue
  -> collect at least f+1 echoes plus a short passive collection window
  -> ARRIVAL_CERT with all collected qualifying echoes
  -> deterministic f+1 cue-support calculation
  -> SIGNED direction or SIGNED-UNKNOWN ResdbVehicleEntry
  -> unchanged PBFT
  -> Check 10 exact proposal/snapshot match
  -> exact kSafe lookup
  -> batch assignment
```

## 3. Experiment state and terminology

For each vehicle `v`, keep the following concepts separate.

### 3.1 True approach lane

```text
L_true(v) in {N,S,E,W}
```

This is derived from the vehicle's actual incoming SUMO edge and is hidden simulator truth.

It is used to generate witness observations and evaluate attacks. The protocol must not compare claims directly with `L_true` after the perception adapter is enabled.

### 3.2 Claimed approach lane

```text
L_claim(v) = ann.lane
```

This is the cardinal approach placed in `ARRIVAL_ANNOUNCE` and eventually used by the scheduler.

For honest vehicles:

```text
L_claim = L_true
```

For a Byzantine lane attack:

```text
L_claim != L_true
```

The probabilistic attack must claim another **valid cardinal approach**. The existing `lane="X"` / `laneId="BYZANTINE_FAKE_LANE"` injection remains useful as a deterministic malformed-value regression, but it is not the primary perception experiment because an honest cardinal-lane classifier should never naturally output `X`.

### 3.3 Queue rank

```text
Q_claim(v) = ann.positionInLane
```

This is the discrete queue rank used by same-lane ordering.

A false queue rank can change same-lane work-queue priority or release order, but it cannot induce a conflicting co-batch: `IsSafeToBatch()` rejects same-lane pairs before consulting `kSafe`.

The perception gate SHALL NOT apply Gaussian meter noise to `Q_claim`. Queue-order behavior remains unchanged and is outside the initial perception scope.

A future extension may make witnesses independently reconstruct a noisy queue rank from detected vehicles. Missed vehicles can change multiple ranks simultaneously, so that extension requires a separate observation model and evaluation.

### 3.4 True SUMO route maneuver

```text
D_route(v) in {LEFT, STRAIGHT, RIGHT}
```

This is determined by the incoming and outgoing edges of the vehicle's assigned SUMO route. It is evaluation truth, not protocol input.

The current network already contains left, straight, and right junction connections. The existing route files simply choose straight routes only, so new route definitions are required but the intersection network does not need to be redesigned from scratch.

### 3.5 Claimed maneuver

```text
D_claim(v) = ann.direction
```

This remains the vehicle's declared maneuver and is sourced from `intendedDirection` in the current app.

For honest scenarios, the generated `intendedDirection` must match the SUMO route. The current wildcard `*.node[*].appl.intendedDirection = "S"` must be replaced with per-node values in mixed-maneuver configurations.

### 3.6 True and observed turn signals

```text
S_true(v) in {LEFT, OFF, RIGHT, OTHER}
S_obs(i,v) in {LEFT, OFF, RIGHT, UNKNOWN}
```

Veins subscribes to `VAR_SIGNALS`, stores the result in each target's
`TraCIMobility`, and exposes `TraCIMobility::getSignals()`. `ResDBPerception`
uses this state only as corruption input for the witness's `observedCue`.

The new perception layer may read the target module's signal state as hidden truth and corrupt it independently for witness `i`. It must not pass the uncorrupted value directly to the witness gate.

Do not assume SUMO's automatic signal timing or semantics without a pilot trace. Log the actual signal state across the approach for each new turn route first. Restrict the experiment to a controlled intent-observation zone where lane changes, parking, hazards, and U-turns are disabled.

Within that controlled zone, use the experimental mapping:

```text
left only   -> LEFT cue
right only  -> RIGHT cue
neither     -> STRAIGHT cue
both/other  -> UNKNOWN cue
```

This is a simulator assumption, not a claim that turn signals perfectly reveal real driver intent.

### 3.7 Witness observation

```text
O_i(v) = (L_obs(i,v), S_obs(i,v), sample_time)
```

The lane component controls whether the witness emits an arrival echo. The signal component is recorded as a signed cue on that echo but never vetoes the echo and never grants co-batching authority by itself.

### 3.8 Certified and eligible values

```text
L_cert(v)
D_claim(v)
D_eligible(v) in {LEFT, STRAIGHT, RIGHT, UNKNOWN}
```

An `ArrivalCert` certifies the lane-qualified arrival claim and carries the declared maneuver plus each signer's observed cue. The scheduler-facing direction is derived deterministically from those certificate bytes:

```text
D_eligible = D_claim   if at least f+1 certificate echoes report observedCue == D_claim
D_eligible = UNKNOWN   otherwise
```

The correct interpretation is:

> At least `f+1` distinct eligible witnesses accepted the imperfect lane/arrival evidence. Co-batching is unlocked only if at least `f+1` signed echoes also contain a cue supporting the declared maneuver.

Neither result proves future physical execution. A turn signal is a noisy, target-controlled cue: it is useful for honest mismatch and inconsistent-claim detection, but a consistent liar can claim RIGHT, signal RIGHT, and later drive STRAIGHT.

### 3.9 Executed maneuver

```text
D_exec(v)
```

This is derived for evaluation from the vehicle's actual internal/outgoing movement.

Normally, when cue support is sufficient:

```text
D_exec = D_route = D_claim = D_eligible
```

A post-certification actuation attack may instead produce:

```text
D_claim = D_eligible = RIGHT
D_exec = STRAIGHT
```

This attack is separate from noisy pre-entry direction verification.

## 4. Perception adapter

The implementation uses a single code boundary:

```text
ResDBPerception.h/.cc
```

Each `ResDBIntersectionApp` owns a perception object representing that replica's sensor.

A minimal interface is:

```cpp
struct ArrivalPerceptionSample {
    bool detected;
    char trueApproach;           // experiment metrology only
    char observedApproach;       // N/S/E/W, or ?
    ObservedCue trueCue;         // experiment metrology only
    ObservedCue observedCue;
    simtime_t observedAt;
    int knownCueSamples;
};

ArrivalPerceptionSample observeArrival(const std::string& targetCarId,
                                        simtime_t now);
```

The adapter may use exact TraCI/Veins state internally to generate a sample, but `handleArrivalAnnouncement()` receives only the corrupted sample.

Keep three roles separate:

```text
SUMO / TraCI
  |
  +-- actuation: stop, resume, injected route deviation
  +-- metrology: actual collision and executed movement
  +-- perception truth: input to witness-specific corruption only
```

Exact actuation and evaluation are allowed. Exact truth must not be used to accept an arrival claim.

### Determinism

Perception MUST use a dedicated OMNeT++ RNG stream index configured explicitly in the experiment INI. It MUST NOT draw from an RNG stream already consumed by radio jitter, protocol timing, vehicle selection, or another baseline mechanism.

At zero observation error, the perception adapter MUST consume no random draws. This prevents the adapter from shifting the random sequences used elsewhere in the simulation.

Noise must be reproducible for a fixed run seed and separated logically per
witness-target pair. Authenticated arrival messages use fixed-width signature
slots so OpenSSL signature-length variation cannot change simulated airtime and
therefore cannot change which module consumes the next perception draw.

Do not use wall-clock randomness or a process-global unseeded C++ RNG.

Every result file SHALL record:

```text
run seed
perception RNG stream index
approach confusion-matrix identifier
signal-channel parameters
lane K and tau
cue-sample aggregation rule
direction-eligibility collection-close policy
```

## 5. First observation model

The baseline observation model uses categorical error over approach-lane and maneuver-cue state.

### 5.1 Lane observation error

Runtime perception uses a categorical `4 x 4` confusion matrix over `{N,S,E,W}`. The matrix SHALL be derived offline from the actual incoming-lane geometry in `bft_intersection.net.xml`; it SHALL NOT assign equal probability to every wrong approach.

For each configured observation standard deviation `sigma_approach`:

```text
sample representative target poses in the intent-observation zone
add zero-mean 2D Gaussian observation error with sigma_approach meters
project the corrupted point to the nearest incoming approach geometry
record true approach -> classified approach
normalize counts into a 4 x 4 confusion matrix
```

The generator MUST use the real lane shapes and stop-zone sampling distribution. A half-lane lateral-error formula alone is insufficient because the certified state is a cardinal approach, not an index among parallel lane strips.

Sweep:

```text
sigma_approach in {0, 0.25, 0.5, 1.0, 2.0} meters
```

Check the generating script and generated matrices into the repository. Runtime code draws only from the selected matrix. The matrices should naturally make opposite-approach confusion much rarer than geometrically adjacent confusion.

Noisy `(x,y)` exists only inside the offline model and evaluation tooling. It is not added to ARRIVAL claims.

A symmetric `epsilon_lane/3` channel MAY remain as a debug/test mode, but it is not a paper experiment.

### 5.2 Signal observation error

Let `epsilon_signal` corrupt the cue in `{LEFT, OFF, RIGHT}`.

A simple symmetric channel is:

```text
P(S_obs = S_true) = 1 - epsilon_signal
P(S_obs = either other state) = epsilon_signal / 2
```

Treat an unavailable, both-blinkers, or invalid reading as `UNKNOWN`, which contributes no positive direction evidence.

Sweep, for example:

```text
epsilon_signal in {0, 0.02, 0.05, 0.10, 0.20, 0.30}
```

Later experiments may split this into missed signal, false-on, left/right flip, and unknown-detection probabilities.

### 5.3 Repeated-observation confidence

Each witness maintains a bounded sample buffer for each `(target, epoch)`. The initial implementation runs with `K=1`. The tunable-gate experiment uses:

```text
K in {1,3,5}
```

Over the fixed window, calculate lane support:

```text
C_lane(i,v) = matching lane samples / valid lane samples
```

Lane endorsement requires:

```text
C_lane >= tau_lane
```

Cue sampling does not create an echo-acceptance threshold. It produces exactly one deterministic `observedCue` for the witness echo:

```text
known = all non-UNKNOWN cue samples in the fixed buffer

if known is empty:
    observedCue = UNKNOWN
else:
    observedCue = majority value in known
```

Break an exact majority tie using a fixed total ordering over cue codes, recorded in the specification and implementation; use `STRAIGHT < LEFT < RIGHT` for the baseline. This rule is deterministic for fixed samples. `UNKNOWN` is silence: it contributes neither positive support nor contradiction.

The baseline does not average witness cues into a maneuver probability. It counts certificate echoes whose signed `observedCue` equals the declared maneuver and requires at least `f+1` such echoes to unlock co-batching.

P2 MUST build the per-target buffer/cache even though P3 and P4 initially run with `K=1`. E5 then sweeps lane `K/tau_lane` and, if desired, cue sample count without changing the cache architecture. There is no `tau_direction` echo gate in the baseline design.

## 6. Witness rule

Define:

```text
claimHash = H(canonical authenticated ARRIVAL_ANNOUNCE bytes)
```

Each witness maintains two related records:

```text
physical sample buffer: (witness, target, epoch)
decision cache:          (witness, target, epoch, claimHash) -> PENDING | ACCEPT | REJECT
```

The physical sample buffer is shared across claim variants from the same target in the same epoch. A retransmission or equivocation variant therefore cannot obtain a fresh set of noisy observations. The decision cache binds the resulting verdict to the exact authenticated claim.

On the first valid announcement for a cache key, honest witness `i` performs:

```text
collect up to K samples in the fixed observation window

if no valid lane sample is available:
    cache REJECT

laneAccept = C_lane >= tau_lane

derive observedCue from the fixed sample buffer using Section 5.3

if laneAccept and existing ambulance checks pass:
    cache ACCEPT
    send ARRIVAL_ECHO(declaredDirection=ann.direction,
                      observedCue=observedCue,
                      claimHash=claimHash)
else:
    cache REJECT
```

Every re-announcement with the same cache key returns the cached result and consumes no new perception draw. An accepted retransmission may resend the same logical echo for reliability; a rejected retransmission remains rejected. A different `claimHash` is evaluated against the already collected physical sample buffer, not a newly sampled world state.

This invariant prevents repeated announcements from turning a single-shot false-accept probability `q0` into `1-(1-q0)^r` after `r` retries. It also prevents an equivocator from obtaining independent noise lottery tickets by sending several claim variants.

The arrival gate remains binary and lane-based: an honest witness either emits an echo or does not. Direction evidence never vetoes arrival certification. A cue is useful only after it is carried in a lane-qualifying echo and counted with other positive support.

`observedCue` MUST be a new signed field. It cannot reuse the current echo `direction` field: that field continues to carry the declared `ann.direction` and remains part of arrival-certificate semantic equality. `ARRIVAL_ECHO` and the echo material carried by `ARRIVAL_CERT` must include both `observedCue` and `claimHash`; the echo signature must cover both fields.

Turn-signal evidence is a consistency cue controlled by the target vehicle, not an independent proof of future maneuver. A vehicle that claims RIGHT and displays a RIGHT signal can obtain positive cue support even if it later drives straight. Later execution is evaluated separately.

The existing Byzantine modes may bypass the honest perception decision where the scenario intentionally models collusion.

## 7. Certificate and PBFT behavior

Keep the current certificate threshold:

```text
required echoes = toleratedFaults + 1
```

or the currently derived `f+1` when `toleratedFaults` is unset.

The threshold establishes lane-qualified arrival certification. It is not a direction threshold on whether an echo may exist.

### 7.1 Echo collection policy

Certificate validation accepts at least `f+1` valid distinct echoes. Assembly
now uses the same semantics: the first `f+1` arms a one-shot collection timer,
collection continues passively until that timer or discovery close, and the
certificate contains all collected qualifying echoes up to `N-1`. Reaching
`f+1` marks the claim certifiable; it does not immediately freeze the
certificate. The collection window is recorded in run metadata.

### 7.2 Deterministic maneuver eligibility

Let `d_hat = ann.direction` and let `E(cert)` be the valid distinct echoes serialized in the certificate. Compute:

```text
support(cert, d_hat)
    = |{ e in E(cert) : e.observedCue == d_hat }|

EligibleDirection(cert) =
    code(d_hat)   if support(cert, d_hat) >= f+1
    3             otherwise
```

Direction codes are:

```text
0 = STRAIGHT
1 = LEFT
2 = RIGHT
3 = UNKNOWN
```

`UNKNOWN` cues never count toward support. Negative or contradictory cues are logged but do not subtract support. Requiring `f+1` positive supporting signatures ensures that, when at most `f` certificate signers are Byzantine, Byzantine signers alone cannot unlock co-batching and at least one honest witness contributed a matching cue.

The split eligibility calculation is always enabled on this branch. Phase 1
contains no legacy runtime path or compatibility toggle. A later paper
ablation may add an isolated experiment-only switch after the baseline is
frozen; it is not part of the Phase 1 protocol.

### 7.3 Replica recomputation invariant

Maneuver eligibility is a deterministic function of authenticated certificate bytes. Every replica MUST recompute the identical value at all boundaries:

- the target when assembling the certificate and packing a proposal;
- validators when receiving or relaying the certificate; and
- `certSnapshotCallback` when constructing `ResdbCertEntry.direction` for Check 10.

Neither the proposal path nor the certificate snapshot copies the declared
direction directly. Both set their scheduler-facing direction to
`EligibleDirection(cert)`. Check 10 remains mechanically unchanged: its exact
direction comparison rejects a Byzantine leader's attempted
`UNKNOWN -> d_hat` upgrade. If the proposal and snapshot derive direction
differently, Check 10 becomes a correctness and security footgun.

Keep:

- distinct signer checks;
- registry-bound witness keys;
- ECDSA signature validation;
- exact equality of the declared claim fields, including `claimHash` and declared `direction`, among counted echoes;
- per-witness `observedCue` values that may legitimately differ and are individually signature-verified;
- ARRIVAL_CERT retry and relay behavior;
- cert-primary selection;
- proposal construction;
- PBFT quorum behavior;
- Check 9 omission handling; and
- Check 10 proposal-to-certificate field matching.

For signed entries, preverification accepts only
`direction in {0,1,2,3}`. Bridge headers and comments document
`0=S,1=L,2=R,3=UNKNOWN`.

Check 10 prevents the PBFT leader from changing the lane or the derived scheduler-facing direction. It does not prove that either value matches future physical execution.

PBFT remains unchanged.

## 8. Low-evidence fallback and batching

The split design has three trust tiers:

| Tier | Evidence | Entry encoding | Scheduling result |
|---|---|---|---|
| QUIET | No valid `f+1` lane/arrival certificate | Existing QUIET encoding; `IsQuietEntry()==true` | Singleton |
| SIGNED-UNKNOWN | Valid lane certificate, but fewer than `f+1` matching maneuver cues | `cyber_status=1`, `direction=3` | Singleton through `kSafe` fallthrough |
| SIGNED-`d_hat` | Valid lane certificate and at least `f+1` matching maneuver cues | `cyber_status=1`, `direction=code(d_hat)` | Eligible for existing `kSafe` batching |

QUIET must not be overloaded to represent direction uncertainty. It continues to mean that no arrival certificate was available:

```text
no valid f+1 lane/arrival cert
  -> QUIET ResdbVehicleEntry
  -> IsQuietEntry == true
  -> singleton batch
```

SIGNED-UNKNOWN is a valid certified arrival whose co-batching authority was not unlocked. It remains `cyber_status=SIGNED`; only its scheduler-facing direction is the UNKNOWN sentinel.

The current scheduler uses:

```cpp
IsSafeToBatch(laneA, directionA, laneB, directionB)
```

No executor algorithm change is required for `direction=3`. `IsSafeToBatch()` first rejects same-lane pairs, and its `kSafe` table contains only direction codes `0` and `2`. Therefore:

- if a SIGNED-UNKNOWN entry is the head of a new batch, it starts that batch and no later candidate can join it;
- if it is considered as a candidate for an existing batch, `SafeWithWholeBatch()` fails because no `kSafe` pair contains direction `3`; and
- the result is a singleton without invoking the QUIET predicate.

Likewise, direction code `1` for LEFT has no `kSafe` pair and is already singleton by fallthrough. Right-turn and opposite-straight combinations retain the concurrency encoded in `kSafe` only after `f+1` positive cue support unlocks the declared direction.

Missing, late, or noisy cues therefore reduce throughput by increasing SIGNED-UNKNOWN singletons; they do not block lane-based arrival certification. False positive cue support can incorrectly unlock a declared direction and create an unsafe co-batch, which is the direction-safety outcome measured in E4.

Metrics SHALL split singleton outcomes into at least QUIET, SIGNED-UNKNOWN, and certified-LEFT/table-forced singletons. Direction masks, Bayesian `P(D)`, and probability-of-conflict scheduling remain outside the baseline design.

## 9. Probabilistic interpretation

Define:

```text
f        configured tolerated Byzantine bound
b_sig    Byzantine signatures supporting a particular false certificate
h        honest witnesses that actually perform one cached evaluation of the claim
q0       probability an honest evaluating witness accepts the false claim
q1       probability an honest evaluating witness accepts the true claim
q0_dir   probability an honest lane-qualifying echo reports a cue matching a false declared maneuver
q1_dir   probability an honest lane-qualifying echo reports a cue matching a true declared maneuver
```

The network and discovery protocol mean `h` is an observed opportunity count, not automatically `N-f` or `N-1`. Packet loss, announcement gossip, intent-zone timing, discovery deadlines, departed/zombie filtering, and detection range affect which witnesses evaluate the claim. The analyzer SHALL derive `h` from `[PERC-EVAL]` records for each `(target, run)` and analytic overlays SHALL use this measured value.

Define `b_sig` from the actual distinct Byzantine witness signer IDs contained
in the certificate. Do not infer it from configured `b`. The claimant never
echoes its own arrival claim, so the claimant is excluded from `b_sig` by
construction.

Assuming conditionally independent honest decisions for the analytic baseline:

```text
P(false cert)
  = P[Binomial(h, q0) >= max(0, f+1-b_sig)]
```

At the important boundary where the attacker supplies `f` valid signatures:

```text
P(false cert | b_sig=f) = 1 - (1-q0)^h
```

Only one honest false acceptance is then required.

This is the expected resilience shoulder:

- at zero observation error, `q0` should approach zero and reproduce the current deterministic result;
- as perception error grows, false certification becomes possible at `b_sig=f`;
- at `b_sig=f+1`, the certificate threshold can be crossed without honest physical acceptance.

Retransmissions do not increase `h` and do not create additional Bernoulli trials because verdicts are cached. The analytic binomial curve is a baseline, not an unconditional theorem. Radio loss and common-mode perception error violate its simplest assumptions and must be measured separately.

Use two levels of statistical validation:

1. Estimate per-witness `q0` and `q1` from many cached decisions across seeds.
2. At selected parameter cells, compare observed certificate rates with the composed Binomial prediction using measured `h` and `b_sig`.

Every first evaluation SHALL emit one machine-readable record such as:

```text
[PERC-EVAL] witness= target= epoch= claimHash= laneVerdict= trueLane= claimedLane= observedLane= observedCue= knownCueSamples=
```

Direction eligibility uses the same threshold form but is conditioned on lane-qualifying echoes. For each certificate, let `h` be the measured number of honest certificate echoes, including echoes whose cue is UNKNOWN, and let `b_sig` be the number of Byzantine certificate echoes whose `observedCue` supports the false declared maneuver. Then:

```text
P(false eligibility)
  = P[Binomial(h, q0_dir) >= max(0, f+1-b_sig)]

P(false eligibility | b_sig=f)
  = 1 - (1-q0_dir)^h
```

For direction plots, compute `h` and `b_sig` from the actual certificate echo set rather than the configured replica count. A false lane certificate and false maneuver eligibility are distinct outcomes: the arrival certificate may validly form while its scheduler-facing direction remains UNKNOWN.

## 10. Attack definitions

### 10.1 Plausible false-lane claim

The current `BYZANTINE_FALSE_LANE` path uses:

```text
laneId = BYZANTINE_FAKE_LANE
lane   = X
```

Keep this as the deterministic malformed-lane regression.

Add a probabilistic lane attack with a valid but incorrect cardinal claim, for example:

```text
actual approach: E
claimed lane:    S
claimed laneId:  a valid S2C lane ID, if laneId remains in the signed claim
```

Byzantine colluders endorse the false claim. An honest witness endorses only if its noisy cardinal classifier outputs `S`.

This gives a meaningful `q0_lane(sigma_approach)`.

### 10.2 False maneuver eligibility from an inconsistent declaration

Example:

```text
actual SUMO route: STRAIGHT
true signal cue:   OFF
claimed direction: RIGHT
```

All lane-qualifying witnesses still echo, regardless of the direction mismatch. Byzantine colluders place `RIGHT` in `observedCue`; an honest echo supports RIGHT only when its corrupted cue classifier outputs RIGHT.

The measured attack outcome is whether `EligibleDirection(cert)` becomes RIGHT, not whether an arrival certificate forms. The expected shoulder at `b_sig=f` is:

```text
P(false eligibility | b_sig=f) = 1 - (1-q0_dir)^h
```

using measured honest cue opportunities `h` from the actual certificate.

This experiment measures resistance to an inconsistent declaration, not recovery of a malicious driver's true intention. A competent adversary can claim RIGHT and display a RIGHT signal. Such a consistent lie can obtain eligibility and is handled by the execution-deviation experiment.

### 10.3 Direction equivocation

The current equivocator sends LEFT to one peer subset and RIGHT to another. Retain this attack, but bind echoes to the original announcement bytes or an announcement hash before treating it as strong evidence against equivocation.

The current echo signature covers the semantic fields but not the complete announcement or an announcement hash. The experiment should log which announced variant each witness evaluated.

### 10.4 Claim-signal-execution deviation

Treat this as a separate post-certification measurement:

```text
claim RIGHT
signal RIGHT
form lane cert and derive eligible direction RIGHT
execute STRAIGHT
```

No pre-entry perception gate can reliably reject evidence that is consistent until after certification. A conformance monitor may detect and log the deviation, but the initial experiment SHALL NOT add CANCEL/tow recovery or claim that already released vehicles can be recalled.

Do not combine this attack with the headline false-certification plot. It tests a different boundary: whether agreement on a declared maneuver can constrain later physical execution.

Live multi-round recovery is future work. The current code contains a `ResdbOmnetRemoveReplica` membership stub and incomplete multi-epoch reset behavior, so reconfiguration is not part of this experiment.

## 11. Turning routes and physical evaluation

### 11.1 Routes

Add route definitions for all approach/maneuver combinations used in the experiment. The network file already has left, straight, and right connections for N, S, E, and W.

For each honest vehicle, configure:

```text
SUMO route maneuver == intendedDirection
```

Do not leave the current wildcard straight direction active in a mixed-route scenario.

### 11.2 Current evaluator limitation

`TraCIScenarioManager::pollIntersectionCooccupancy()` currently records only each vehicle's incoming approach and explicitly assumes all configured routes are straight. It treats opposite approaches as nonconflicting.

That logic is invalid once turns are introduced.

### 11.3 Replacement metrology

The primary physical-safety metric is simultaneous conflict-zone occupancy by vehicles whose **actual movements conflict**. Actual movement is derived from ingress and egress edges, and conflict is determined by the movement-conflict table used for evaluation.

SUMO collision events are a secondary metric. They depend on geometry and simulator collision handling and may miss an unsafe crossing that avoids literal overlap by a small margin.

Add actual-movement metrology using incoming and outgoing edges:

```text
incoming N2C + outgoing C2S -> straight
incoming N2C + outgoing C2W -> right
incoming N2C + outgoing C2E -> left
```

and the corresponding mappings for other approaches.

For false-safe batch analysis, compare the scheduler's certified movement pair with the actual movement pair executed in SUMO. Record actual simultaneous internal-lane occupancy, conflicting-movement co-occupancy, and physical collisions.

This evaluator is oracle metrology only. It must not feed the witness gate or scheduler.

### 11.4 Simulator safety controls

Pin and document the controls that can prevent a released vehicle from following the committed schedule. For every run, record at least:

```text
TraCI speedMode applied on release
junction-model parameters
collision.check-junctions
collision.action
time-to-teleport
```

Audit the current `resumeVehicle()` use of `setSpeedMode(0)` against the installed SUMO semantics; the existing comment alone is not evidence of which checks are enabled. Apply one explicit, common configuration across the evaluated scenarios.

Before the main sweep, release a known conflicting pair and verify that the simulator permits the expected conflict-zone co-occupancy. This calibration ensures that measured safety comes from the committed PicaBFT schedule rather than an undocumented SUMO right-of-way or emergency-braking intervention. Store the complete control configuration in run metadata.

## 12. Experiment matrix

Use separate symbols for:

```text
f = configured tolerated Byzantine bound
b = actual Byzantine/colluding replicas injected in the run
```

The experiment SHALL retain the existing `f` sweep and add perception error as a second axis.

### E0: zero-error regression

```text
sigma_approach = 0
epsilon_signal = 0
all routes     = straight
```

Expected:

- honest arrivals certify as before;
- current batching is reproduced;
- deterministic fake lane `X` remains rejected by honest witnesses;
- PBFT and rollback behavior are unchanged.

The semantic regression invariants are:

```text
identical certificate sets
identical SIGNED/QUIET classifications
identical batch composition
identical committed-order bytes
identical [CONSENSUS_ATTACK_OUTCOME] lines
```

Timestamps may differ within a stated tolerance. New perception diagnostic records are the only expected log-content addition. Dedicated RNG isolation and a no-draw zero-error path are required so the adapter does not perturb unrelated simulation behavior.

### E1: honest approach-observation error

No Byzantine vehicles. Sweep `sigma_approach` with straight routes and `epsilon_signal=0`.

Measure:

- honest witness lane-accept rate;
- certificate completion rate;
- certificate latency;
- QUIET rate;
- singleton fraction;
- mean batch size;
- throughput; and
- mean/p95 delay.

This is primarily a liveness/efficiency experiment.

### E2: plausible false-lane claim

Use a vehicle on one real approach claiming another real approach. Sweep:

```text
b in {0,...,f,f+1}
sigma_approach
```

Measure:

- honest false-accept probability `q0_lane`;
- false ARRIVAL_CERT rate;
- committed false-lane rate;
- false-safe batch rate;
- unsafe co-occupancy; and
- physical collision rate.

This is the lane resilience-shoulder experiment.

### E3: maneuver-cue characterization

Use one fixed LEFT/STRAIGHT/RIGHT route mix across all cells. First run that mix at zero signal error and characterize signal availability during the actual echo window. Then use a small signal-error sweep only after the route and batching baselines are stable.

Measure:

- honest cue-support rate;
- direction-eligibility unlock rate;
- QUIET, SIGNED-UNKNOWN, and table-forced singleton fractions;
- batch-size distribution;
- throughput; and
- delay.

Because the current safe table isolates left turns, break results out by maneuver type and do not attribute the left-turn singleton cost to perception uncertainty.

### E4: false maneuver eligibility shoulder

Use the pre-entry detectable attack:

```text
route/signal = STRAIGHT/OFF
claim        = RIGHT
```

Sweep:

```text
b in {0,...,f,f+1}
epsilon_signal
```

Measure:

- honest false cue-support probability `q0_dir`;
- rate that eligibility sets `entry.direction=RIGHT`;
- valid lane-certificate rate, reported separately;
- measured `h` and supporting `b_sig` per certificate;
- false-safe batch rate;
- unsafe co-occupancy; and
- collision rate.

Compare the eligibility rate with `1-(1-q0_dir)^h` at `b_sig=f`. This is the inconsistent-declaration eligibility shoulder. It does not show that a signal reveals a malicious vehicle's future maneuver.

### E5: confidence/throughput knob

After E2 is stable, sweep lane `K in {1,3,5}` and `tau_lane` at fixed nonzero environment error. Cue `K` may also be swept while retaining the deterministic majority-to-`observedCue` rule and the fixed cross-witness `f+1` eligibility threshold. This experiment is required, not optional.

Measure:

- false certificate probability;
- honest certificate completion;
- singleton fraction;
- throughput; and
- delay.

This produces the safety-throughput operating curve for the probabilistic gate.

### E6: post-certification maneuver deviation

Use:

```text
claim RIGHT
signal RIGHT
form lane cert and derive eligible direction RIGHT
execute STRAIGHT
```

Run a perception-based conformance monitor in measurement-only mode.

Measure:

- detection rate;
- maneuver onset to local detection;
- maneuver onset to `f+1` violation evidence, if aggregation is implemented;
- unsafe co-occupancy; and
- physical collision.

Do not add CANCEL, tow, or live multi-round recovery. Report this experiment as detection latency and an architectural limitation, not mitigation or prevention.

### E7: correlated error

After independent-error experiments work, add one demonstration episode that biases several witnesses toward the same wrong lane or signal classification rather than a full factorial sweep.

This tests the limitation of the independent binomial model and shows that more witnesses do not automatically help when errors are correlated.

### 12.1 Baselines and ablations

Use exactly these comparison rows:

| Row | Definition | Purpose |
|---|---|---|
| Fixed-time traffic light | SUMO fixed-time TLS configuration | Throughput and delay anchor only; no Byzantine-security claim |
| All-way stop | SUMO all-way-stop/default junction control | Throughput and delay anchor only; no Byzantine-security claim |
| Perception gate off | PBFT arrival/scheduling path with physical-evidence admission disabled, so syntactically valid claims can certify | Isolates the contribution of the perception gate; this is the precisely defined “plain PBFT” row |
| Direction eligibility ablation (future) | A later experiment-only switch copies declared direction instead of deriving it; the Phase 1 branch itself has no compatibility toggle | Isolates the co-batching authority supplied by the maneuver-cue mechanism |
| Firewall off | Existing `RESDB_NO_FIREWALL` behavior | Isolates proposal validation Checks 9 and 10 |
| All singleton | Executor flag forcing every certified entry into its own batch | Isolates batching and provides a safety-ceiling/throughput-floor anchor |

Do not use the ambulance `NoCertGate` or `NoFW` policy names as an undocumented substitute for the plain-PBFT definition. Implement or alias a clearly named experiment toggle whose only effect is disabling arrival physical-evidence admission.

## 13. Metrics and plots

### Gate metrics

```text
honest evaluation opportunities
honest true accepts q1
honest false accepts q0
measured h per target/run
actual b_sig per certificate
echo count by target and signer
certificate completion and latency
false certificate count
QUIET count
SIGNED-UNKNOWN count
direction eligibility support count and unlock outcome
```

### Scheduling metrics

```text
batch index
batch-size distribution
singleton fraction
singleton fraction split by QUIET / SIGNED-UNKNOWN / certified LEFT
false-safe batch count
throughput
mean and p95 stop-to-departure delay
```

### Physical metrics

```text
actual conflicting movement co-occupancy
SUMO physical collisions
actual versus certified movement
minimum separation, if added reliably
```

### Byzantine boundary metrics

```text
attack success versus actual b
configured f
explicit b=f and b=f+1 results
```

### Recommended plots

1. **False-certificate resilience shoulder:** x=`b`, y=false-cert rate, line per `sigma_approach`.
2. **Lane gate curve:** x=`sigma_approach` in meters, y=false-cert and honest-QUIET rates.
3. **Direction eligibility curve:** x=`epsilon_signal`, y=false eligibility and SIGNED-UNKNOWN singleton rates.
4. **Safety-throughput frontier:** x=throughput, y=false-safe batch or collision rate, point per threshold.
5. **Batching retention:** batch-size distribution versus direction error, split by actual maneuver.
6. **Deviation timing:** maneuver onset to detection for E6.

Use multiple seeded runs and confidence intervals. A measured zero should not be presented as mathematical zero; report the run count and interval.

Every figure SHALL label its variables according to the taxonomy in Section 1.1: environment axis, adversary axis, and design knob. The main figure grammar is an environment parameter on the x-axis, a line family over `K/tau`, and fixed or faceted adversary conditions. Use measured `h` and `b_sig` for theory overlays.

## 14. Concrete implementation map

### New `ResDBPerception.h/.cc`

Responsibilities:

- locate the target's current Veins/TraCI mobility state;
- derive true cardinal approach for corruption only;
- read the target's stored `VehicleSignalSet` for corruption only;
- generate witness-specific categorical errors;
- return only the noisy observation;
- return a single `K=1` sample to the application-owned per-target sample and
  verdict caches; and
- emit one `[PERC-EVAL]` record per first cached evaluation, with truth fields enabled only for experiment analysis.

### `ResDBIntersectionApp.h`

Add:

- perception model ownership;
- observation enums/structs;
- per-target sample buffers and per-claim verdict-cache state;
- `DIR_UNKNOWN=3` in the `Direction` enum and an explicit `directionCode()` mapping that never maps unknown input to STRAIGHT;
- a stable cue enum/wire mapping over LEFT, STRAIGHT, RIGHT, and UNKNOWN;
- a signed `observedCue` field on `ArrivalEcho`, serialized inside `ArrivalCert`;
- new Byzantine types for plausible cardinal-lane and false-direction claims, unless implemented through scenario parameters; and
- metrics counters.

Add the announcement-hash field required to bind echoes and certificates to the claim that was evaluated.

### `ResDBIntersectionApp.ned`

The implemented parameters are:

```text
approachSigmaM
approachConfusionMatrix
signalObservationError
perceptionRngIndex
directionEligibilityCollectionWindowSec
```

The new branch has no legacy imperfect-perception or direction-eligibility
toggle. The split path is the only runtime behavior. Phase 1 fixes `K=1` and
uses the existing `f+1` threshold.

### `ResDBArrivalProtocol.cc`

Update the witness, echo-collection, and certificate-derivation paths:

- extend the announcement self-signature to cover cardinal lane and direction;
- verify the origin signature on direct and gossiped announcements;
- bind each echo to the exact announcement with `claimHash`;
- replace direct `verifyCarPosition()` admission with the perception result;
- compare noisy cardinal approach to `ann.lane`;
- emit an honest echo when lane acceptance and the existing ambulance checks pass, regardless of cue value;
- derive `observedCue` deterministically from the fixed signal-sample buffer and add it as a new signed echo field;
- retain the existing echo `direction` field as the declared `ann.direction`; do not reuse it for the observation;
- keep ambulance credential checking;
- use the deterministic collect-until policy in Section 7.1 instead of freezing
  at the first `f+1`;
- assemble certificates with all collected distinct lane-qualifying echoes, capped at `N-1`;
- compute `EligibleDirection(cert)` when packing the proposal;
- cache the verdict for every `(witness,target,epoch,claimHash)` while reusing a single physical sample buffer per `(witness,target,epoch)`; and
- log one evaluation record with accept/reject reason.

`verifyCarPosition()` is not used as the imperfect arrival-gate decision.

### Route and INI generation

- add turning route IDs to the `.rou.xml` files used by the new scenarios;
- set each node's `intendedDirection` consistently with its honest route;
- retain explicit `intendedLane` per approach;
- add separate attack parameters for claimed direction and executed route; and
- configure the dedicated perception RNG stream explicitly;
- record the selected approach confusion-matrix ID and SUMO safety-control settings; and
- do not overwrite the established straight-only baseline configs.

Add an offline generator that reads the actual network lane shapes and produces the checked-in `4 x 4` approach confusion matrices for each `sigma_approach` value.

### `TraCIScenarioManager.cc`

- retain `[PHYSICAL-COLLISION]` metrology;
- replace or supplement the straight-only `pollIntersectionCooccupancy()` logic for turn scenarios;
- record ingress and egress edges so actual maneuver can be derived; and
- report conflicting actual-movement co-occupancy as the primary safety metric;
- optionally expose a read-only target-mobility lookup used internally by `ResDBPerception`.

### `experiment_orchestrator.py`

The Phase 1 runner arguments are:

```text
--approach-sigma
--signal-error
--direction-collection-window
```

The orchestrator derives and passes `--simulation-seed`, includes sigma and
signal error in result-directory names, writes
`perception_run_metadata.json`, and displays the complete generated command in
`--dry-run`. `run-resdb-simulation.sh` generates a named-section
`perception_override.ini` after validating the active `-c` configuration,
matrix catalog entry, seed, and numeric ranges.

Later phases add separate scenario codes/names for:

- plausible wrong cardinal lane;
- false direction with contradictory signal;
- honest mixed maneuvers; and
- post-certification maneuver deviation.

Add explicit baseline/ablation configuration switches for perception-gate-off, direction-eligibility-off, and all-singleton execution. Preserve the existing `RESDB_NO_FIREWALL` ablation.

### `fourway/analyze_log.py`

Parse:

- perception opportunities and outcomes;
- measured `h` from unique `[PERC-EVAL]` records;
- actual `b_sig` from certificate signer identities;
- cue support per signer, total support, and derived eligible direction;
- honest/Byzantine echo classifications;
- true/false certs using evaluator truth;
- declared, eligible, and actual maneuver;
- false-safe batches;
- singleton and batch-size statistics; and
- E6 detection timing.

Existing false-lane metrics should distinguish malformed `X` claims from plausible cardinal misclaims.

### ResDB bridge, certificate snapshot, and Check 10

- `directionCode()` maps `3=UNKNOWN`; unknown input never falls through to
  `0=STRAIGHT`;
- `resdb_omnet_bridge.h` documents `0=S,1=L,2=R,3=UNKNOWN`;
- bridge preverification enforces `direction in {0,1,2,3}`;
- received-certificate validation recomputes `EligibleDirection(cert)`;
- `certSnapshotCallback` sets `ResdbCertEntry.direction` to the same recomputed
  result used by proposal packing; and
- Check 10 retains its exact proposal-to-snapshot comparison, preventing a
  leader from upgrading UNKNOWN to the declaration.

### PBFT and executor files

No PBFT changes are required for the baseline probabilistic-gate design.

No scheduler-algorithm change is required. `direction=3` safely falls through the existing `kSafe` lookup and produces a singleton while retaining `cyber_status=1`. `BuildIntersectionSchedule()`, `IsSafeToBatch()`, and QUIET handling remain intact. The all-singleton ablation requires only an executor experiment flag.

Only comments that describe the certificate as physical “ground truth” should be corrected to say that it is canonical certified evidence whose physical correctness is probabilistic.

## 15. Implementation order and acceptance tests

### Phase 1 implementation record (2026-08-08)

Phase 1 is implemented on the `perception-error` branch as the sole protocol
path. It includes:

- `ResDBPerception.h/.cc`, a checked-in geometry-derived matrix catalog for
  `sigma_approach in {0,0.25,0.5,1,2}` metres, and a deterministic generator;
- dedicated OMNeT local RNG stream `1`, mapped to global RNG `1`, with no
  perception draws at identity lane error and zero signal error;
- canonical origin signatures, verified origin identity, `claimHash`, signed
  `observedCue`, and Type-4/Type-5 serialization of the new evidence;
- fixed-width signature slots with explicit actual lengths, keeping packet
  lengths deterministic across repeated runs despite randomized ECDSA DER
  encodings;
- one cached physical verdict and one cached logical echo per
  `(witness,target,epoch,claimHash)`; replayed ACCEPT claims resend the identical
  echo, replayed REJECT claims remain rejected, and a genuinely different
  authenticated claim variant is evaluated independently;
- an explicit self-target guard prevents announcement gossip from turning the
  claimant into one of its own `f+1` witnesses;
- byte-identical cached local announcements within an epoch, including cached
  per-peer equivocation variants;
- lane-only echo admission and cue-independent arrival certification;
- one congestion-safe `arrivalCertFinalizeTimer`: first `f+1` arms a 0.25 s
  default passive collection window, additional distinct echoes are accepted
  without new polling/gossip traffic, and one certificate is finalized through
  the existing certificate retry/drain path;
- cancellation of that timer during normal finalization, discovery shutdown,
  proposal/order transitions, rollback, crash muting, departure, and teardown;
- `finish()` retakes and deletes the certificate-finalization and discovery
  flush timers while the TraCI manager context is still valid, including the
  QUIET path where manager-owned cancelled timers otherwise survive too long;
- deterministic `EligibleDirection(cert,f)` recomputation in proposal packing,
  received-certificate validation, and `certSnapshotCallback`;
- SIGNED proposal packing takes lane and queue rank from the validated arrival
  certificate, never from a witness's noisy `VehicleState`; perception gates
  the declaration but does not replace its authenticated scheduling fields;
- bridge direction allowlist `{0,1,2,3}` and unchanged Check 10 exact matching;
- after guarded normal PRE_PREPARE validation, followers install its authenticated
  SIGNED cert-primary even in epoch 0, where no forced/reconfiguration view
  exists; this wires a QUIET configured primary into the existing leader-failure
  handoff instead of leaving follower PBFT state pinned to that QUIET replica;
- when a completed ORDER candidate is frozen, every replica installs that
  candidate's cert-primary before any PRE_PREPARE is processed; follower
  preverify remains a guarded backstop, not the first point at which stale PBFT
  primary state is repaired;
- unchanged PBFT and unchanged `kSafe`, with SIGNED-UNKNOWN becoming singleton
  through direction-3 fallthrough;
- runner generation of `perception_override.ini`, deterministic simulation
  seeds, parameterized result paths, and run metadata, with the two-RNG mapping
  also present in base `omnetpp.ini` for direct runs without an override;
- atomic single-write `[CONSENSUS_ATTACK_OUTCOME]` records so merged ResDB
  stderr cannot split the analyzer's structured line; and
- analyzer support for lane evaluations, cue distribution, duplicate-evaluation
  detection, measured `h`, certificate echo counts, support, `b_sig`, derived
  direction, and all three trust tiers.

Phase 1 validation completed:

| Check | Result |
|---|---|
| ResDB bridge build | Pass |
| Veins release and debug builds | Pass |
| Matrix generator/unit tests | 5/5 pass |
| N=4 E0 | 4 certificates, 4 SIGNED-STRAIGHT, PBFT commit, 4 departures |
| N=16 E0 | 16 certificates, 16 SIGNED-STRAIGHT, PBFT commit, 16 departures |
| Collection beyond threshold | N=16 certificates contained 6-8 echoes at threshold 6 |
| Zero-error RNG isolation | Every replica reported zero perception draws |
| Single-evaluation invariant | Zero duplicate `[PERC-EVAL]` records |
| Signal-error smoke | `epsilon_signal=1`: lane certificates still formed; all four entries became SIGNED-UNKNOWN singletons |
| Lane-error smoke | `sigma_approach=2 m`: observed lanes changed and lane acceptance fell below 1 |
| Seed reproducibility | Same nonzero seed reproduced identical normalized perception decisions; a different seed changed them |
| Timer teardown | No undisposed arrival-finalize or ResDB discovery-flush timers |

Two verification tasks remain before declaring the full P2/P3 acceptance suite
closed: compare final committed-order bytes against an independently frozen
pre-change golden log, and run a dedicated Byzantine leader mutation that
changes a derived UNKNOWN entry to its declaration and confirms an explicit
Check 10 rejection. The implemented snapshot/proposal paths already recompute
the same direction and Check 10 compares the direction byte exactly.

### Phase 1 command reference

Run these commands from the repository root. The orchestrator is the preferred
simulation entry point because it regenerates ResDB membership and keys for
each requested `N` before launching the corresponding OMNeT configuration.

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh makeres
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh makeveins
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'cd fourway && make'
```

The simplest validation entry point is the orchestrator's fixed, self-checking
Phase 1 suite:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase1-validation'
```

It runs exactly four cells: E0 at `N=4`, signal error `1` at `N=4`, approach
sigma `2 m` at `N=4`, and E0 at `N=16`. It regenerates keys only when `N`
changes, analyzes each run immediately, prints a per-invariant PASS/FAIL
checklist (including no claimant self-witnesses, unsafe conflict-zone
co-occupancy, and SUMO collisions),
and exits nonzero if any required invariant fails. Each run writes
`raw_simulation.log` and `phase1_validation.json`; the raw log is preserved
immediately so that a later suite cell cannot overwrite a failed run's
diagnostics. The combined machine-readable result is
`benchmarks/Phase1Validation/phase1_validation_summary.json`.

Preview every generated simulation command without running it:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase1-validation --dry-run'
```

The preset defaults to one repetition. `--reps 3` repeats every cell three
times, `--start-rep <unused-index>` preserves prior run directories, and
`--direction-collection-window` remains configurable. The preset owns the
four configurations and therefore rejects scenario, baseline, fault-injection,
fault-tolerance, and randomized-leader options.

Individual parameter cells can still be run manually, for example:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --config 4 --reps 1 --scenario 1 --approach-sigma 0 --signal-error 1 --direction-collection-window 0.25'
```

Manual result paths include `sigma_<value>_signal_<value>`, and each run
contains `perception_run_metadata.json` plus the analyzer JSON.

### P0: freeze existing baselines

Capture fixed-seed results for:

- honest straight scheduling;
- existing false-lane `X` rejection/collusion sweep;
- direction equivocation;
- firewall-off tampered-lane collision; and
- rollback/crash recovery.

### P1: trace signals and add turn routes

Before changing the gate:

- add a small mixed-turn SUMO route file;
- log target signal states through the intent zone;
- verify the actual signal behavior for left/right/straight routes;
- verify that `intendedDirection` matches the route; and
- update evaluator metrology for turns.

Acceptance: all 12 approach/maneuver combinations traverse the intended outgoing edge and are classified correctly by the evaluator.

P1 is characterization, not a go/no-go test. In particular, record whether SUMO holds the blinker while a vehicle is stopped at the stop line during the echo window. If it does, use that as the primary controlled cue source. Missing or late cues become signed UNKNOWN observations and are measured as SIGNED-UNKNOWN singleton outcomes; they never prevent lane-based arrival certification.

### P2: perception adapter with zero error

Route arrival checks through `ResDBPerception` with `sigma_approach=0` and `epsilon_signal=0`.

Acceptance:

- identical certificate sets, SIGNED/QUIET classifications, batch composition, committed-order bytes, and `[CONSENSUS_ATTACK_OUTCOME]` records relative to the frozen fixed-seed baseline;
- timestamps differ only within a stated tolerance, and new perception records are the only permitted semantic log addition;
- mixed-turn honest claims receive matching zero-error cues;
- lane-valid echoes form independently of cue availability;
- cue-poor certificates derive `direction=3` while retaining SIGNED status;
- fixed simulation seed reproduces identical perception logs; and
- protocol code no longer compares the arrival claim directly with uncorrupted target state.

Before P3, also require valid origin signatures over the complete lane/direction declaration, deterministic echo-to-announcement binding, signed `observedCue`, the dedicated RNG mapping, the per-target buffer/per-claim verdict cache, and `directionCode(UNKNOWN)==3`.

### P3: split lane gate and cue eligibility

Add categorical lane and signal corruption, lane-only echo admission, deterministic per-echo cue derivation, collect-until certificate assembly, and certificate-derived maneuver eligibility.

Acceptance:

- empirical approach confusion matches the configured matrix;
- empirical signal misclassification approaches `epsilon_signal`;
- lane echo acceptance depends on lane evidence and not on signal evidence;
- signal error changes eligibility-unlock and SIGNED-UNKNOWN rates without changing lane-certificate completion, holding lane conditions fixed;
- fewer than `f+1` supporting cues always derive UNKNOWN while `f+1` or more derive the declared direction;
- proposal packing, received-cert validation, and `certSnapshotCallback` derive identical direction bytes;
- Check 10 rejects a proposed UNKNOWN-to-declared-direction upgrade;
- certificates can include more than `f+1` echoes when additional qualifying echoes arrive before collection close;
- an aggressive re-announcement run has per-witness accept rates matching the single-shot prediction rather than `1-(1-q0)^r`; and
- echo/cert cryptographic validation covers `claimHash`, declared direction, and `observedCue`.

### P4: plausible attacks and `f`/perception sweep

Implement plausible wrong-cardinal and inconsistent-direction attacks.

Acceptance:

- at zero perception error and `b<=f`, a false claim cannot obtain honest endorsement;
- at nonzero lane error and `b_sig=f`, false lane certificates appear at the predicted shoulder rate within sampling error using measured `h`;
- at nonzero cue error and supporting `b_sig=f`, false maneuver eligibility appears at `1-(1-q0_dir)^h` within sampling error;
- a lane certificate may form while the false maneuver remains SIGNED-UNKNOWN;
- at `b=f+1`, colluders can cross the classical certificate boundary; and
- PBFT continues to agree on whatever valid certificate reaches it.

### P5: repeated-sample design knob

After P4 is stable, run the defined lane `K in {1,3,5}` and `tau_lane` sweep using the buffer architecture installed in P2. If cue `K` is varied, retain majority-of-known cue derivation and the cross-witness `f+1` positive-support rule.

Acceptance: raising the lane threshold reduces false lane acceptance and honest certificate completion, while cue availability/error changes the split between SIGNED-UNKNOWN and direction-eligible entries.

### P6: post-certification conformance measurement

Implement only if E6 is in scope. Log detection timing and safety outcomes without wiring CANCEL, tow, or multi-round recovery.

## 16. Claims the resulting experiment supports

The paper may claim:

1. PBFT still gives deterministic agreement under the existing Byzantine and timing assumptions.
2. An `f+1` lane/arrival certificate contains at least one honest lane-qualifying signer when at most `f` certificate signers are Byzantine.
3. An eligible scalar direction supported by `f+1` signed matching cues contains at least one honest supporting observation under the same bound.
4. Co-batching authority scales with positive maneuver evidence: insufficient support produces a SIGNED-UNKNOWN singleton without discarding the valid arrival certificate.
5. PicaBFT exposes three explicit trust tiers—QUIET, SIGNED-UNKNOWN, and SIGNED with an eligible direction—whose safety and throughput effects can be measured separately.
6. Physical correctness remains probabilistic and depends on false-accept/support rates, sensor model, correlation, witness opportunities, and later actuation conformance.
7. A future declared-direction-only experiment can serve as the eligibility-off
   ablation, while zero perception error is the deterministic limit of the
   implemented evidence model.

The paper must not claim:

- that witnesses verify continuous position in meters;
- that `positionInLane` is continuous position;
- that `f+1` matching turn-signal cues prove future execution;
- that turn signals provide Byzantine-proof knowledge of intent;
- that maneuver cues can certify arrival or independently grant co-batching authority;
- that Check 10 converts a certificate into physical ground truth;
- that post-entry detection necessarily prevents a collision; or
- that independent sensor errors represent every real-world failure mode.

### 16.1 Paper positioning

The related-work discussion SHALL distinguish this contribution from F2MD and broader VANET misbehavior-detection systems. The contribution is a measured and analytically validated composition of imperfect lane evidence, an `f+1` arrival certificate, `f+1` signed positive maneuver-cue support, deterministic PBFT agreement, and evidence-gated conflict-aware scheduling. Any comparison must use the exact threat and sensing assumptions of the cited system rather than implying equivalence from a shared vehicular setting.

## 17. Scheduler safety table

The executor uses lane codes `0=N, 1=S, 2=E, 3=W` and scheduler-facing direction codes `0=STRAIGHT, 1=LEFT, 2=RIGHT, 3=UNKNOWN`. Its safe-pair relation is symmetric and contains exactly:

| Movement A | Movement B |
|---|---|
| N straight | S straight |
| E straight | W straight |
| Any right turn | Any right turn from a different approach |
| N right | S straight |
| S right | N straight |
| E right | W straight |
| W right | E straight |

Same-approach pairs are rejected before this lookup. No pair containing LEFT or UNKNOWN is present, so both directions become singleton by fallthrough. UNKNOWN requires no executor algorithm change: as a batch head nobody can join it, and as a candidate it fails `SafeWithWholeBatch()`.

The table is a conservative conflict matrix: two movements may be listed as safe only when their modeled paths do not intersect inside the conflict zone. The paper appendix SHALL print the expanded pair table and state this provenance. Before mixed-turn evaluation, validate every entry against the actual SUMO connection geometry; do not silently reinterpret or enlarge the table.

## 18. System architecture

```text
SUMO/Veins hidden state
  actual incoming edge
  actual route and signal state
  actual executed movement
          |
          +------------------------------+
          |                              |
          v                              v
  evaluation metrology           ResDBPerception owned by witness i
  conflicting co-occupancy       categorical lane + signal corruption
  collisions / actual movement
                                         |
                                         v
                         fixed lane/cue sample buffer
                                         |
                         +---------------+----------------+
                         |                                |
                         v                                v
              compare observed lane              derive observedCue
                  with ann.lane              majority known, else UNKNOWN
                         |                                |
          laneAccept + ambulance checks                   |
                  |              |                        |
                reject         accept                     |
                  |              |                        |
               no echo   signed ARRIVAL_ECHO <------------+
                         declared direction + observedCue + claimHash
                                        |
                     collect qualifying echoes until close
                                        |
                           at least f+1 distinct echoes?
                              |                    |
                             no                   yes
                              |                    |
                         QUIET entry         ARRIVAL_CERT
                         singleton                 |
                                         count observedCue == d_hat
                                                |
                              +-----------------+------------------+
                              |                                    |
                       support < f+1                      support >= f+1
                              |                                    |
                 SIGNED, direction=3 UNKNOWN          SIGNED, direction=d_hat
                         singleton                     existing kSafe eligibility
                              |                                    |
                              +-----------------+------------------+
                                                |
                                  deterministic proposal/snapshot
                                                |
                                         unchanged PBFT
                                                |
                                    Check 10 exact-match enforcement
                                                |
                                       scheduled physical execution
                                                |
                                      SUMO evaluation/conformance only
```

The core experiment is therefore:

> PicaBFT uses `f+1` imperfect lane evidence to certify arrival, `f+1` signed positive maneuver cues to unlock a declared direction for co-batching, and deterministic PBFT agreement over the certificate-derived outcome. Missing cue evidence degrades safely to a SIGNED-UNKNOWN singleton; it does not erase the arrival certificate or claim to prove future intent.
