# PicaBFT Probabilistic Physical-Evidence Gate

## Design and implementation specification

## 1. Purpose

This specification defines separate probabilistic physical-evidence and maneuver-eligibility stages for PicaBFT while preserving deterministic PBFT agreement and the existing hard-direction scheduler.

The experiment asks:

> As honest witnesses become imperfect at observing a vehicle's lateral lane state, longitudinal distance to the stop line, and maneuver cue, how do `f+1` certification, evidence-gated scheduling, safety, and delay degrade?

The experiment SHALL produce a family of safety, certification, batching, and latency curves over physical-observation error.

The two certification stages operate over the fields used by the intersection
protocol:

- an exact SUMO `laneId` string;
- a cardinal `approach` in `{N,S,E,W}`;
- a `physicalLaneIndex` within that approach (`0` in the single-lane fixture);
- a canonical `distanceToStopCm` longitudinal claim, quantized to one
  centimetre and carried only by the stopped-distance attestation;
- a declared `direction` in `{S,L,R}`; and
- the existing arrival time, epoch, ambulance, identity, and signature fields.

The application orders vehicles sharing `(approach,physicalLaneIndex)` by
certified `distanceToStopCm`, with a smaller distance meaning physically closer
to the stop line. It deterministically derives the legacy one-byte queue rank
that the unchanged scheduler consumes. Integer queue rank is not sensed or
certified.

The initial experimental scope is:

1. noisy continuous lateral observation as the gate for lane certification;
2. noisy per-vehicle longitudinal distance observation as the gate for same-lane ordering authority;
3. noisy observation of a turn-signal maneuver cue as evidence for co-batching eligibility only;
4. `f+1` physical-evidence certificate formation and `f+1` positive maneuver-cue support;
5. three trust tiers: QUIET, SIGNED-UNKNOWN, and SIGNED with an eligible scalar direction;
6. the existing hard-direction scheduler, extended only by the `direction=3` UNKNOWN sentinel; and
7. Byzantine lateral-lane, longitudinal-distance, and direction claims under noisy honest witnesses.

Occlusion, observation latency, joint multi-car plausibility rejection,
BLOCKED/CLEAR perception, Bayesian `P(D)` aggregation, credible-set scheduling,
and probability-distribution scheduling are outside the initial scope.

V2X intersection protocols necessarily operate on declared maneuver intent; perception cannot certify a future actuation choice. PicaBFT therefore certifies observable arrival evidence, aggregates signed maneuver cues using `f+1` positive support, and permits concurrent crossing only when that evidence unlocks the declared direction for `kSafe`. PBFT agrees on both the certificate evidence and the deterministically derived eligibility outcome. Execution conformance is measured separately.

### 1.1 Experimental-axis taxonomy

Every experiment and figure SHALL distinguish three kinds of variables:

| Category | Variables | Meaning |
|---|---|---|
| Environment axes | ego-localization error `sigma_self`, witness lateral error `sigma_lat`, witness longitudinal error `sigma_long`, signal error `epsilon_signal`, physical queue spacing, packet conditions | Properties imposed by the sensing environment; not chosen by the protocol operator |
| Adversary axes | actual colluder count `b`, lateral offset `delta_lat`, longitudinal offset `Delta_long`, attack type | Threat conditions; not operator controls |
| Design knobs | provisioned tolerance `f`, sample count `K`, standardized gate radius `k` | Parameters selected by the system designer |

The safety-throughput operating envelope MUST be parameterized by a design knob. The preferred figure grammar is:

```text
x-axis       = environment severity
line family  = k (or later K/tau) design settings
panel or row = fixed adversary condition b and attack type
```

The existing `f` sweep remains a provisioning-cost experiment. A `b` sweep is a threat experiment. They must not be described as the same kind of knob.

## 2. Frozen baseline and target certification path

### 2.1 Announcement fields

The frozen categorical Phase 2 implementation contains:

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

The physical-coordinate implementation adds a canonical centimetre-quantized
lane-normal claim and derived `physicalLaneIndex` required by Section 5.1. The
old `positionInLane` byte remains serialized for now but is non-authoritative:
proposal packing and Check 10 use the rank derived from the separate stopped-
distance certificates. A separate authenticated
`STOPPED_DISTANCE_ATTEST` later carries `distanceToStopCm`. These are protocol
fields, not untrusted analyzer annotations.

The announcing vehicle obtains `laneId` from TraCI. Its cardinal `lane` normally comes from the explicit `intendedLane` NED parameter, with lane-ID parsing as a fallback.

The pre-physical implementation builds `positionInLane` by sorting exact
TraCI lane positions and copies the resulting integer rank into the
announcement. That path is the frozen categorical Phase 2 baseline only. The
physical-coordinate implementation ignores that value for scheduler authority
and uses a signed, centimetre-quantized distance-to-stop-line claim created only
after the target is stationary. It SHALL NOT add Gaussian noise to the old
integer rank.

### 2.2 Implemented witness check

The honest arrival path obtains one cached `ArrivalPerceptionSample` from
`ResDBPerception` for each `(witness,target,epoch)`. Cardinal configurations
compare corrupted `observedApproach` with `ann.lane`. The dedicated adjacent-
lane configuration additionally applies the scalar residual in Section 5.1.
Existing ambulance checks remain in force, but signal evidence never vetoes an
echo.

```text
echo = laneAccept && ambulanceChecks
```

The first origin-authenticated claim hash is locked by
`(witness,target,epoch)`. The complete signed echo for that accepted variant is
cached by `(witness,target,epoch,claimHash)`. Periodic or gossiped
re-announcements of the same hash resend the same logical verdict and do not
resample perception. A different hash is retained as signed equivocation
evidence and rejected before perception or echo generation. `verifyCarPosition()`
is no longer the arrival-gate decision.

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

The originating vehicle signs, and every receiver verifies, a canonical early
announcement payload containing:

```text
carId : epoch : laneId : lane : physicalLaneIndex : lateralClaimCm :
direction : isAmbulance :
claimedArrivalTime : ambulanceCertBytes : ambulanceSigBytes
```

Each echo signs the declared fields, `observedCue`, epoch, signer identity, and
`claimHash=SHA-256(authenticated serialized announcement)`. Type-5 certificate
serialization preserves the common claim hash and every echo's cue.

Periodic broadcasts reuse a cached, byte-identical signed announcement for the
entire epoch. ECDSA values are serialized in fixed-width padded slots together
with their real signature length. This prevents nondeterministic DER signature
lengths from changing simulated packet airtime and perturbing seeded perception
draws.

After the stop-zone and stationary predicate holds, the claimant signs exactly
one separate payload:

```text
targetCarId : epoch : earlyClaimHash : distanceToStopCm
```

External distance echoes bind to the hash of that authenticated attestation.
The stopped-distance certificate preserves the attestation, its binding to the
early claim, and every distinct supporting signature. Re-announcement of the
early claim never changes or implicitly creates the stopped-distance claim.

"Exactly one" means one sampled and signed logical attestation. The claimant
may retransmit the same cached bytes a bounded number of times while distance
collection remains active. Retries stop immediately when `f+1` signers are
present, when discovery drains, or when proposal, commitment, departure,
rollback, or crash muting closes the phase. A retry never samples again,
changes the signature, or creates a second attestation.

After assembly, the origin schedules one additional byte-identical Type-20
certificate transmission through the existing cancellable discovery queue.
This is a bounded reliability copy, not a new collection timer: it creates no
perception draw, echo, or certificate variant, and the discovery drain waits
for it to air or cancels it on the same terminal lifecycle transitions. The
initial Type-20 transmission and this one queued copy are the complete
origin-side policy for the stopped-distance certificate.

For the single-lane approach fixtures, cardinal approach uniquely determines
the physical lane and the protocol uses `physicalLaneIndex=0`. This is an
explicit model restriction, not a claim that cardinal approach is sufficient
for a multi-lane road. A later multi-lane fixture must add the claimant's
physical lane index to the authenticated early claim and certify that index
from lane geometry before using `(approach,physicalLaneIndex)` queue groups.

### 2.5 Target batching semantics

`proposeAll()` requires both certificates. It deterministically sorts certified
distances within each `(approach,physicalLaneIndex)` and packs the resulting
legacy one-byte queue rank, scalar approach, and eligible direction into each
`ResdbVehicleEntry`. PBFT agrees on those derived scheduling bytes. Check 10
continues to compare exact proposal bytes with the certificate snapshot; both
sides must derive the same rank and other fields from the same certificate
sets.

After commitment, `BuildIntersectionSchedule()` uses the fixed `kSafe` table. A candidate joins a batch only if its exact `(lane,direction)` is safe with every member already in the batch.

QUIET entries are already forced into singleton batches.

The target physical-evidence path is two-stage. Cardinal approach stays on the
geometry-derived categorical channel. The continuous lateral branch is enabled
only for a fixture with parallel adjacent physical lanes and a reviewed
lane-normal coordinate:

```text
EARLY ARRIVAL_ANNOUNCE while approaching
  -> cached approach / physical-lane / cue observation
  -> f+1 EARLY ARRIVAL_CERT

one-shot STOPPED_DISTANCE_ATTEST after stop-zone + stationary predicate
  -> cached stopped-window longitudinal observation
  -> f+1 STOPPED_DISTANCE_CERT bound to early claimHash and epoch

  -> proposal input requires early and stopped-distance evidence
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

### 3.2.1 Physical lane within an approach

The current `lane` field is historically overloaded: it identifies the
cardinal approach for `kSafe` and the queue used for same-lane ordering. The
continuous implementation separates these meanings:

```text
A_claim(v) = claimed approach in {N,S,E,W}
J_claim(v) = claimed physical-lane index within that approach
```

`kSafe` continues to consume `(approach,direction)`. Queue membership and
front-before-rear ordering consume `(approach,physicalLaneIndex)`. The initial
single-lane intersection has `physicalLaneIndex=0` on every approach and must
therefore reproduce the existing behavior. Adding a second lane is a later
topology checkpoint and does not authorize same-approach co-batching.

For an honest vehicle, the signed physical claim is produced by its ego
localization estimate. Byzantine vehicles may claim arbitrary authenticated
coordinates, approach, lane index, and distance. External witnesses derive
their observation independently from noisy position; they do not read the
claimant's hidden SUMO lane as proof.

### 3.2.2 Early lane evidence and post-stop distance evidence

The protocol deliberately uses two physical reference times.

Approach and physical-lane occupancy may be observed while the target is still
approaching because they classify the lane geometry containing the target; the
vehicle's changing distance along that lane does not invalidate that spatial
classification. The early certificate therefore covers approach,
`physicalLaneIndex`, declaration, and maneuver-cue evidence.

Longitudinal distance is time-varying and SHALL NOT be included in that early
verdict. A second one-shot signed distance attestation is created only after
the existing stop-zone and stationary-speed predicate holds. Its witness
echoes are bound to the same target, epoch, and authenticated early claim hash.
It introduces no periodic broadcast, polling, or extension of the early gossip
deadline; it uses a bounded collection/drain path with the same congestion
discipline as existing certificate traffic.

In the single-lane intersection, an incoming vehicle cannot change physical
lane, so the early lane classification remains valid through the stop. In a
future multi-lane approach, an early certificate proves only the lane occupied
at its observation time. A lane change before stopping requires a post-stop
lane re-observation and a newly bound final lane value; the scheduler must
never treat stale early evidence as authority for an unobserved lane change.

### 3.3 Longitudinal distance and derived queue order

```text
d_true(v,t)  = physical distance from the target's front reference point to the stop line
d_claim(v)   = stoppedAttestation.distanceToStopCm / 100
```

`d_claim` is a fixed signed claim created while the vehicle is stopped in the
arrival/echo window. A smaller value means closer to the intersection. The
protocol derives same-lane order by sorting certified distances and breaks an
exact centimetre tie by replica ID.

A false longitudinal claim can change same-lane work-queue priority or release
order, but it cannot induce a conflicting co-batch: `IsSafeToBatch()` rejects
same-lane pairs before consulting `kSafe`.

Witness `i` observes the target independently:

```text
d_obs(i,v) = d_true(v) + Normal(0, sigma_long^2)
```

It accepts the longitudinal claim iff:

```text
abs(d_obs(i,v) - d_claim(v)) <= k * sigma_long
```

The protocol does not reconstruct an integer rank by sorting a witness's noisy
set of vehicles. Each certificate remains per car. Ordering is derived only
after the per-car claims have been certified. This preserves the per-witness,
per-target Binomial model for false certification; the later pairwise ordering
outcome is measured separately.

The stopped-window invariant is mandatory for the second stage. A cached witness sample and the
fixed announcement claim must refer to the same stationary physical state.
Samples taken before the target is stopped or after it is released are invalid
for longitudinal endorsement.

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

## 5. Physical observation models

The baseline observation model uses categorical error over approach-lane and maneuver-cue state.

### 5.1 Lateral lane observation

The completed categorical Phase 2 baseline uses a geometry-derived `4 x 4`
confusion matrix over `{N,S,E,W}`. Its 384-run result set is immutable regression
evidence and SHALL remain in a separate result namespace.

The continuous lane branch does not replace cardinal classification. It is
scoped to physical lane index within the adjacent-lane fixture. Let `u` be the
signed scalar projection onto the reviewed unit normal from lane A to lane B:

```text
u_claim,honest = quantize_1cm(u_true)
u_claim,attack = quantize_1cm(u_true + delta_lat)
u_obs          = quantize_1cm(u_true + Normal(0, sigma_lat^2))
```

The claimant's own physical-lane coordinate is the authenticated declaration;
perception uncertainty is injected only into external witness observations.
This self/other split matches the longitudinal channel. The witness accepts iff:

```text
T_lat = k * sigma_lat
abs(u_obs - u_claim) <= T_lat
```

Physical lane index is projected from `u_claim` only after this plausibility
check passes. The implementation must not first quantize `u_obs` to a lane
label and compare labels. The signed claim and echo/certificate canonical bytes
include the one-centimetre scalar claim and its derived physical-lane index.
World coordinates remain analysis-only metrics, and longitudinal vehicle motion
does not enter the lane residual.

Main experiments use `k=3`. One operating-characteristic figure re-decides the
stored residual samples offline at `k in {2,2.5,3}`; it does not re-simulate
those runs. The adjacent-lane fixture and runtime use this same scalar gate.
Its analytic reference is the one-dimensional Gaussian interval with a
centimetre-quantization envelope, producing the smooth
`q0_lane(sigma_lat,delta_lat)` shoulder.

Its parallel A/B
lanes share one travel approach and therefore cannot silently be relabeled as
the scheduler's cardinal `N/S/E/W` field. Cardinal approach therefore remains
on the existing categorical model. A smooth A/B ROC on 3.2 m parallel lanes is
evidence for adjacent physical-lane substitution only, not for an N-to-E claim.
Categorical cardinal and continuous physical-lane modes share the same
perception adapter and verdict cache but intentionally use different geometry.

For historical comparison, the categorical matrix was generated as follows:

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

The categorical matrix path MAY remain as a frozen regression mode, but it is
not the primary continuous-coordinate paper experiment.

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

### 5.4 Longitudinal distance observation

Longitudinal perception certifies one physical coordinate per car; it never
adds noise directly to an integer queue rank. The coordinate is the distance
from a fixed vehicle reference point to the stop line, measured along the
incoming lane and recorded in canonical centimetres.

An honest claimant samples one ego-localization estimate per epoch:

```text
d_claim,honest = quantize_1cm(d_true + Normal(0,sigma_self_lon^2))
d_obs          = quantize_1cm(d_true + Normal(0,sigma_long^2))
T_lon          = k * sqrt(sigma_self_lon^2 + sigma_long^2)
```

The claim is then fixed, signed, and reused. It is not resampled for each
witness or re-announcement. E0 sets both self and witness errors to zero.

For a fixed realized claim offset `Delta_long = d_claim-d_true`, one honest
witness's false-accept probability is:

```text
q_dist(Delta_long, sigma_long, T_lon)
  = Phi((T_lon - abs(Delta_long))/sigma_long)
    - Phi((-T_lon - abs(Delta_long))/sigma_long)

q1_dist = 2*Phi(T_lon/sqrt(sigma_self_lon^2+sigma_long^2))-1
```

For a certificate attempt with `h` honest external evaluators and
`b_sig_available` Byzantine support actually available to that attempt
(the claimant's local self-attestation plus distinct supporting Byzantine
echoes collected before the attempt closes):

```text
r = max(0, f+1-b_sig_available)
P(false distance certificate) = P[Binomial(h,q_dist) >= r]
```

The analyzer also records `b_sig_cert`, the Byzantine signer count in a
finalized certificate, for forensic accounting. It MUST NOT substitute
`b_sig_cert=0` for a failed attempt: doing so conditions the theory input on
the outcome being predicted. A failed attempt can have substantial collected
Byzantine support without producing certificate bytes.

This is a per-car certification result. It must not be confused with the raw
pairwise probability that two independently noisy observations reverse order.
For true front-reference separation `s`:

```text
P(raw observed-order inversion) = Phi(-s/(sqrt(2)*sigma_long))
```

Nor is either expression, by itself, the probability of a post-certificate
ordering violation. That outcome depends on which two claims certify and their
canonical values, so it is computed from certificate bytes and physical truth
per run.

The paper and analyzer MUST keep three longitudinal quantities distinct:

1. per-witness false acceptance of one fixed distance claim (`q_dist`);
2. raw reversal of two independently noisy observations
   (`Phi(-s/(sqrt(2)*sigma_long))`); and
3. a committed per-pair ordering inversion, derived from finalized certificate
   bytes and actual stopped positions.

Only item 2 receives the direct `sqrt(2)` Gaussian overlay. At `s=0` its
prediction is exactly `0.5`, and it decreases monotonically with `s`. Item 3 is
reported per pair rather than only as whole-queue correctness, but its protocol
prediction composes item 1 with attempt-local `b_sig_available` and measured `h`; it
must not be relabeled as item 2 merely because both metrics use vehicle pairs.

The longitudinal calibration fixture uses physically realizable stopped
queues. If `g` is bumper-to-bumper clearance and `ell` is vehicle length, then
the reference-point separation is `s=ell+g`; plots and analytic calculations
must not substitute clearance `g` for `s`. To make a rear vehicle sort ahead of
the front vehicle by margin `eta`, the required false-claim magnitude is
`abs(Delta_long)=s+eta`.

Main experiments use `k=3`. The same recorded absolute residuals are
re-thresholded offline at `k in {2,2.5,3}`. Exact centimetre ties use replica ID
and are logged separately. Phase 1 does not add a joint rule rejecting two
individually plausible equal-position certificates.

### 5.5 Self declarations, witness perception, and model scope

Direction is an authenticated declaration of planned behavior, not a physical
sensor reading. Honest direction matches the configured route; a Byzantine
claimant may lie. Turn-signal evidence remains a noisy external cue and does
not prove future execution.

Physical self reports are localization estimates, not perfect ground truth.
Ego error is sampled once when the authenticated announcement is created.
Witness errors are sampled independently per `(witness,target,epoch)` and are
cached. The claimant's local self-attestation signs the fixed claim but is not
an independent perception evaluation.

Conditional on the realized fixed claim offset, per-witness false endorsements
use the measured-`h` Binomial model. Marginalizing over shared ego error, or
introducing common scene/geometry bias, correlates witnesses. The independent
Binomial is therefore reported as a model-conditional predictor, not exact
physical truth or a formally proven lower bound. Empirical deviations and the
correlated-error experiment must be reported rather than hidden.

The physical model follows established VANET misbehavior-detection grammar:
the attack is a constant offset added to a true position, matching the
constant-position-offset family in [VeReMi](https://arxiv.org/abs/1804.06701),
and witness localization error is Gaussian per coordinate, consistent with the
sensor-error extension evaluated by [VeReMi
Extension](https://veremi-dataset.github.io/veremi-extension) on an
OMNeT++/Veins/SUMO stack. The local residual check is a physical-plausibility
detector, aligned with data-centric position-consistency checks such as [Ruj et
al.](https://arxiv.org/abs/1103.2404). The `f+1` stage is a fixed counting rule
over local binary decisions; its independent-decision Binomial composition is
reported as a conditional model, not as a new sensor law.

Unlike the lateral and longitudinal channels, which are perceived and exhibit
a graded false-certification regime characterized via operating curves,
direction is a committed field gated by cue-support eligibility. Direction is
therefore evaluated by ablation (eligibility-on / eligibility-off /
co-batching-off), measuring the safe throughput the gate recovers.

For distance, the Binomial applies to one car's certificate. The
`Phi(-separation/(sqrt(2)*sigma_long))` expression applies only to one raw
pairwise comparison of two independently noisy positions. Adjacent queue-pair
events share vehicles, so a whole-queue permutation is evaluated empirically
and must not be modeled as a product of independent pair events.

## 6. Witness rule

Define:

```text
claimHash = H(canonical authenticated ARRIVAL_ANNOUNCE bytes)
```

Each witness maintains three related records:

```text
variant lock:           (witness, target, epoch) -> firstClaimHash
signed variant evidence:(witness, target, epoch) -> authenticated announcements
decision/echo cache:    (witness, target, epoch, firstClaimHash) -> ACCEPT | REJECT
```

The first origin-authenticated announcement fixes `firstClaimHash`. A replay of
that hash uses the existing cached verdict. A different authenticated hash for
the same target and epoch is retained alongside the first signed announcement,
logged as equivocation, and rejected without perception sampling or an echo.
The witness therefore signs at most one declaration variant per target and
epoch.

On the first valid early announcement for a cache key, honest witness `i`
performs:

```text
collect up to K early lateral/cue samples in the fixed observation window

if no valid lateral sample is available:
    cache REJECT

laneAccept = continuous lateral plausibility gate with T_lat

derive observedCue from the fixed sample buffer using Section 5.3

if laneAccept and existing ambulance checks pass:
    cache ACCEPT
    send ARRIVAL_ECHO(declaredDirection=ann.direction,
                      observedCue=observedCue,
                      claimHash=claimHash)
else:
    cache REJECT
```

Every re-announcement with the first claim hash returns the cached result and
consumes no new perception draw. An accepted retransmission may resend the same
logical echo for reliability; a rejected retransmission remains rejected. A
different `claimHash` is not evaluated and cannot receive an honest echo from
that witness.

The stopped-distance stage has a separate cache keyed by
`(witness,target,epoch,stoppedAttestationHash)`. Only after the target is
stationary does the witness take one longitudinal observation and evaluate:

```text
distanceAccept = abs(d_obs-d_claim) <= T_lon

if distanceAccept:
    send STOPPED_DISTANCE_ECHO(attestationHash, distanceToStopCm)
else:
    send nothing
```

Retransmissions of the same Type-18 attestation reuse that cached verdict and,
on acceptance, the same logical signed Type-19 echo. Early announcement retries
never trigger longitudinal sampling, and distance-attestation retries never
repeat the early lane/cue evaluation.

This invariant prevents repeated announcements from turning a single-shot
false-accept probability `q0` into `1-(1-q0)^r` after `r` retries. It also
prevents one honest witness from supporting multiple equivocation variants.
Because the arrival threshold is only `f+1`, this local invariant alone does
not prove that no independently supported variant can ever form; the
equivocation experiment reports the actual split support and does not claim a
global equivocation certificate without an additional evidence-propagation
mechanism.

Each physical-evidence stage remains binary, but the certificates are separate:
an early echo endorses lateral lane evidence and a stopped-distance echo
endorses longitudinal evidence. Proposal eligibility requires both
certificates. Direction evidence never vetoes early arrival certification. A
cue is useful only after it is carried in an early lane-qualifying echo and
counted with other positive support.

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
certificate contains all collected qualifying echoes up to `N`, including the
claimant's single local self-attestation. Reaching
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
- certified-candidate primary selection and PBFT view-change rotation;
- proposal construction;
- PBFT quorum behavior;
- Check 9 omission handling; and
- Check 10 proposal-to-certificate field matching.

For signed entries, preverification accepts only
`direction in {0,1,2,3}`. Bridge headers and comments document
`0=S,1=L,2=R,3=UNKNOWN`.

Check 10 prevents the PBFT leader from changing the lane or the derived scheduler-facing direction. It does not prove that either value matches future physical execution.

The PBFT protocol remains unchanged, but application leadership is restricted
to vehicles with complete physical evidence. The initial ORDER primary is the
lowest replica id for which the frozen local candidate contains both a valid
arrival certificate and a valid stopped-distance certificate. After an
authenticated PBFT view change, the current PBFT primary may propose even when
it is not that initial minimum, but only if the voting replica's local
certificate snapshot contains the same two-stage evidence for that leader. A
static PBFT successor without that evidence is not allowed to acquire proposal
authority: replicas trigger the next ordinary PBFT view change. If no certified
candidate exists, the system fails safe without submitting ORDER. The
proposal's SIGNED bit is not evidence by itself; bridge preverification checks
the leader against the validator's authenticated local snapshot.

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
b_sig    Byzantine supporting signatures, including a Byzantine claimant's self-attestation
h_lane   honest external witnesses that perform one cached lane evaluation
h_dir    honest external lane-qualifying echoes included in the certificate
q0       probability an honest evaluating witness accepts the false claim
q1       probability an honest evaluating witness accepts the true claim
q0_dir   probability an honest lane-qualifying echo reports a cue matching a false declared maneuver
q1_dir   probability an honest lane-qualifying echo reports a cue matching a true declared maneuver
```

The network and discovery protocol mean `h_lane` and `h_dir` are observed opportunity counts, not nominal replica counts. Packet loss, announcement gossip, intent-zone timing, discovery deadlines, departed/zombie filtering, and detection range affect which witnesses evaluate and which echoes enter a certificate. The analyzer SHALL derive `h_lane` from `[PERC-EVAL]` and `h_dir` from the external honest signers actually serialized in each certificate.

Define `b_sig` from the actual distinct Byzantine supporting signer IDs in the
certificate, including a Byzantine claimant's self-attestation. Do not infer
it from configured `b`. A claimant creates exactly one signed self-attestation
over its authenticated declaration, inserts it locally without Type-4 radio
traffic or perception sampling, and uses `observedCue=declaredDirection`.
Configured Byzantine identities are potential signers, not guaranteed
certificate participants. Radio reachability, gossip timing, lane
qualification, and the fixed collection window may leave some configured
colluders outside the finalized certificate. Consequently, `b=f+1` means an
attack can cross without honest support when the finalized certificate has
`b_sig=f+1`; it does not require every `b=f+1` run to succeed.

Assuming conditionally independent honest decisions for the analytic baseline:

```text
P(false cert)
  = P[Binomial(h_lane, q0) >= max(0, f+1-b_sig)]
```

At the important boundary where the attacker supplies `f` valid signatures:

```text
P(false cert | b_sig=f) = 1 - (1-q0)^h_lane
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

Direction eligibility uses the same threshold form but is conditioned on lane-qualifying echoes. For each certificate, let `h_dir` be the measured number of honest external certificate echoes, including echoes whose cue is UNKNOWN, and let `b_sig` be the number of Byzantine certificate echoes whose `observedCue` supports the false declared maneuver. Then:

```text
P(false eligibility)
  = P[Binomial(h_dir, q0_dir) >= max(0, f+1-b_sig)]

P(false eligibility | b_sig=f)
  = 1 - (1-q0_dir)^h_dir
```

For direction plots, compute `h` and `b_sig` from the actual certificate echo set rather than the configured replica count. A false lane certificate and false maneuver eligibility are distinct outcomes: the arrival certificate may validly form while its scheduler-facing direction remains UNKNOWN.

## 10. Attack definitions

### 10.1 Graded false-lane claim

The current `BYZANTINE_FALSE_LANE` path uses:

```text
laneId = BYZANTINE_FAKE_LANE
lane   = X
```

Keep this as the deterministic malformed-lane regression.

The continuous attack uses the target's actual lane fixture and claims a fixed
lateral offset:

```text
p_claim      = quantize_1cm(p_true + delta_lat*n)
claimed lane = project_to_lane(p_claim)
```

Byzantine colluders endorse the false claim. An honest witness endorses only if
its continuous residual falls within `k*sigma_lat`. A boundary-crossing
`delta_lat` yields a valid adjacent-lane claim without inventing a fake lane.

This gives the calibrated `q0_lane(sigma_lat,delta_lat)` surface. The older
valid-cardinal E2 attack remains a frozen regression/coarse-offset comparison.

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

The equivocator creates exactly one signed LEFT byte variant and exactly one
signed RIGHT byte variant, then reuses each byte-identical variant for its peer
subset. This avoids treating randomized signature encodings of the same
semantic declaration as additional variants. Echoes are bound to the
authenticated announcement through `claimHash`, so signatures for different
variants cannot be merged into one certificate. In addition, an
honest witness locks the first authenticated hash for `(target,epoch)`, refuses
to evaluate or sign a second hash, retains both signed announcements, and logs
`[EQUIVOCATION-DETECTED]` with both hashes and directions.

The experiment SHALL verify that no honest witness evaluates or echoes two
variants. It SHALL report the support obtained by each variant and must not
claim that the one-variant invariant alone globally prevents either variant
from independently reaching the `f+1` arrival threshold.

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

### 10.5 Proposal-integrity recovery and availability attacks

Proposal-field and certificate-omission attacks use a two-step defense:

1. Check 10 rejects certificate-derived field mutation, or Check 9 rejects
   excessive certificate omission. Honest followers do not repair individual
   fields in place and do not vote for a partially corrected proposal.
2. Failure to obtain a quorum triggers PBFT view change. The non-Byzantine
   successor constructs a fresh proposal from its own authenticated local
   certificate snapshot and commits the correct value.

Every E--H validation SHALL log the actual attacking proposer, rejecting check,
primary transition, successor `[PROPOSER-CERT-STATE]`, correct committed value,
and recovery latency. The successor snapshot must cover at least the certified
set held by the deposed proposer. In the certificate-suppression row it must
also contain every certificate omitted from the malicious proposal.

The certificate-relay-withholding row configures exactly `f` replicas to retain
valid certificates while withholding both forwarding paths. It does not forge
certificates, silence origin broadcasts, or mute PBFT. The row passes only if
honest-to-honest relays let every honest replica obtain the full certified set
and the system commits and departs safely.

The consecutive-primary corner configures proposal-only silence for `r0` and
its Byzantine successor `r1`. Full PBFT communicator silence is a distinct
fault and is not used for this row. After each primary transition, honest
replicas with complete discovery and no pending/applied order must rearm their
one-shot suspicion timer. The required trace is `r0 -> r1 -> r2`, followed by
an honest `r2` proposal containing the complete certified set. This validates
the tested recovery prefix within `f+1` primary attempts; it does not extend
the paper's scope to dynamic membership.

Binary attack-defense rows D, E, F, G, H, relay withholding, and the
consecutive-primary corner run strictly sequentially for 3--5 seed indices.
F intentionally sets `signal_error=1.0` so the target certificate derives
`SIGNED-UNKNOWN`; the leader's UNKNOWN-to-STRAIGHT mutation is then exercised
deterministically. Other rows retain the locked system operating point.

For equivocation, one variant may independently reach `f+1`. The required
claim is narrower: no honest witness evaluates or signs two variants for the
same `(target,epoch)`, signatures from different hashes are not merged, and two
conflicting variants do not both produce an unsafe committed schedule. The
consistent-liar case in Section 10.4 remains deferred to post-commit
conformance monitoring.

## 11. Turning routes and physical evaluation

### 11.1 Routes

Add route definitions for all approach/maneuver combinations used in the experiment. The network file already has left, straight, and right connections for N, S, E, and W.

For each honest vehicle, configure:

```text
SUMO route maneuver == intendedDirection
```

Do not leave the current wildcard straight direction active in a mixed-route scenario.

### 11.2 Evaluator boundary

The evaluator must not classify safety from incoming approach alone. It records
the planned ingress/egress movement while the vehicle is present, then confirms
the observed outgoing edge after conflict-zone exit. A missing or mismatched
actual egress fails the run instead of silently assigning a movement.

### 11.3 Replacement metrology

The primary physical-safety metric is simultaneous conflict-zone occupancy by vehicles whose **actual movements conflict**. Actual movement is derived from ingress and egress edges, and conflict is determined by the movement-conflict table used for evaluation.

SUMO collision events are a secondary metric. They depend on geometry and simulator collision handling and may miss an unsafe crossing that avoids literal overlap by a small margin.

Actual-movement metrology uses incoming and outgoing edges:

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

### E1: honest lateral-observation error

No Byzantine vehicles. Sweep `sigma_lat` with straight routes and
`epsilon_signal=0`.

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

### E2-LAT: graded false-lane claim

Use the adjacent-lane fixture geometry. The target claims
`p_claim=p_true+delta_lat*n`, and the lane code is projected from that claim.
Sweep:

```text
b in {0,...,f,f+1}
sigma_lat
delta_lat
```

Measure:

- honest false-accept probability `q0_lane`;
- false ARRIVAL_CERT rate;
- committed false-lane rate;
- false-safe batch rate;
- unsafe co-occupancy; and
- physical collision rate.

This is the continuous lane resilience-shoulder experiment. The categorical
E2 artifacts remain a frozen coarse-channel baseline. At a full-lane offset,
the continuous implementation must reproduce the categorical attack's
certificate and scheduler semantics even though the calibrated observation
probabilities come from the continuous model.

### E2-LONG: false distance and same-lane ordering

Use a physically valid stopped same-lane queue. Select a rear target, keep the
front vehicle honest, and claim a smaller distance so the target sorts ahead:

```text
d_claim_target = d_true_target - Delta_long
Delta_long      = referencePointSeparation + eta
```

Sweep `b`, `sigma_long`, physical clearance, and `eta` only after the standalone
fixture verifies the true distances and spacing. Measure per-car false-distance
certification separately from post-certificate pairwise ordering violations.
The primary safety metric is release-order inversion relative to physical queue
order; rear-end collision is secondary. Do not look for this attack in
conflicting-movement co-occupancy, because same-lane pairs cannot co-batch.

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

### E4: maneuver-eligibility safe-throughput ablation

Use the pre-entry detectable attack:

```text
route/signal = STRAIGHT/OFF
claim        = RIGHT
```

The earlier `b x epsilon_signal` runs remain structural characterization, not a
headline shoulder. An inconsistent liar receives no honest false support at
zero cue error, while a consistent liar can set a matching cue and defeats this
pre-execution test; the latter belongs to E6 conformance measurement.

Evaluate direction's downstream contribution with three matched runs:

1. **eligibility ON:** the current `f+1` cue-support rule unlocks co-batching;
2. **eligibility OFF:** the declared maneuver is trusted for batching without
   cue support; and
3. **co-batching OFF:** every entry is scheduled as a singleton.

Report throughput and delay together with unsafe co-batches, actual conflicting
co-occupancy, and collisions. Split singletons into QUIET, SIGNED-UNKNOWN,
certified-LEFT/table-forced, and other scheduler-forced causes. The comparison
asks whether eligibility ON recovers batching throughput relative to
all-singleton scheduling without the unsafe co-batches admitted by trusting
declared direction.

The two experiment-only flags are implemented. `enableDirectionEligibility=false`
copies the authenticated declared direction into the scheduling entry, while
`RESDB_ALL_SINGLETON=1` forces one entry per batch in the scheduler. Neither
flag changes the default path, `kSafe`, PBFT, or Check 10.

The N=16 full-intersection fixture contains `8 LEFT / 4 STRAIGHT / 4 RIGHT`
movements. The statistical ablation crosses honest and `FALSE_DIRECTION` runs
with the three modes above, using 20 paired repetitions per cell (120 strictly
sequential runs). The selected attack kind, target, Byzantine set, and
colluders are logged during initialization before any perception sample is
drawn; an adversary may not choose an attack after observing sensor noise.

The scaling fixture extends the same two-lane geometry to N=`4,8,16,20`. Each
scale balances the four approaches and preserves the proportional maneuver mix
`50% LEFT / 25% STRAIGHT / 25% RIGHT`; `veh0=N-L` and `veh1=S-S` remain the
reviewed physical conflict pair. Before any Byzantine scale matrix, one honest
operating-point smoke per N must pass route/egress agreement, certification,
departure, lifecycle, metrology, and rank-noise checks. A Byzantine evidence
claimant is not automatically labeled a Byzantine PBFT leader: those roles
must be configured and reported separately in the later scale experiment.

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
| Direction eligibility off | `enableDirectionEligibility=false` copies the authenticated declared direction instead of deriving eligibility | Naive throughput baseline; isolates cue-gated co-batching authority |
| Firewall off | Existing `RESDB_NO_FIREWALL` behavior | Isolates proposal validation Checks 9 and 10 |
| All singleton | `RESDB_ALL_SINGLETON=1` forces every entry into its own batch | Isolates batching and provides a safety-ceiling/throughput-floor anchor |

Do not use the ambulance `NoCertGate` or `NoFW` policy names as an undocumented substitute for the plain-PBFT definition. Implement or alias a clearly named experiment toggle whose only effect is disabling arrival physical-evidence admission.

## 13. Metrics and plots

### Gate metrics

```text
honest evaluation opportunities
honest true accepts q1
honest false accepts q0
honest lateral residual and longitudinal residual
measured h per target/run
actual b_sig per certificate
echo count by target and signer
certificate completion and latency
false certificate count
QUIET count
SIGNED-UNKNOWN count
direction eligibility support count and unlock outcome
true/claimed/observed lateral coordinates and delta_lat
true/claimed/observed distance-to-stop and Delta_long
sigma_lat, sigma_long, k, and stopped-window validity
```

### Scheduling metrics

```text
batch index
batch-size distribution
singleton fraction
singleton fraction split by QUIET / SIGNED-UNKNOWN / certified LEFT
certified same-lane distance order
true same-lane physical order
exact-centimetre tie count and replica-ID tie-break outcome
ordering-violation count
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
same-lane release-order inversion
rear-end collision or forced-overlap event
```

### Byzantine boundary metrics

```text
attack success versus actual b
configured f
explicit b=f and b=f+1 results
```

### Recommended plots

1. **False-certificate resilience shoulder:** x=`b`, y=false-cert rate, line per `sigma_lat` and selected `delta_lat`.
2. **Continuous lane surface:** x=`delta_lat`, line per `sigma_lat`, y=`q0_lane` and false-certificate rate.
3. **Direction safe-throughput ablation:** eligibility ON vs eligibility OFF vs
   co-batching OFF; report throughput/delay, unsafe co-batches and physical
   co-occupancy, with QUIET/SIGNED-UNKNOWN/LEFT singleton causes separated.
4. **Safety-throughput frontier:** x=throughput, y=false-safe batch or collision rate, point per threshold.
5. **Batching retention:** batch-size distribution versus direction error, split by actual maneuver.
6. **Deviation timing:** maneuver onset to detection for E6.
7. **Longitudinal operating characteristic:** x=`Delta_long`, line per `sigma_long`, y=per-car false-cert rate and ordering-violation rate.
8. **Cheap gate-knob figure:** offline re-threshold the same residual traces for `k in {2,2.5,3}`.

Use multiple seeded runs and confidence intervals. A measured zero should not be presented as mathematical zero; report the run count and interval.

Every figure SHALL label its variables according to the taxonomy in Section 1.1: environment axis, adversary axis, and design knob. The main continuous-channel figure grammar uses an environment or attack parameter on the x-axis, a line family over `k`, and fixed or faceted Byzantine conditions. Later repeated-sampling figures may use `K/tau`. Use measured `h` and `b_sig` for theory overlays.

## 14. Concrete implementation map

### New `ResDBPerception.h/.cc`

Responsibilities:

- locate the target's current Veins/TraCI mobility state;
- read world position, lane geometry, and distance to the stop line for corruption only;
- derive the adjacent-lane normal from the two TraCI lane centerlines and
  generate one scalar Gaussian observation on that normal;
- read the target's stored `VehicleSignalSet` for corruption only;
- retain categorical cardinal errors for all N/S/E/W intersection executions;
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

Treat the protocol-facing integer `positionInLane` as non-authoritative and
derive the scheduler rank from canonical `distanceToStopCm`. Add the
centimetre-quantized scalar lateral claim and physical-lane index to the
authenticated announcement, echo binding, and certificate. Do not reuse
TraCI's `getLanePosition()` as a lateral coordinate; it is longitudinal.

Add the announcement-hash field required to bind echoes and certificates to the claim that was evaluated.

### `ResDBIntersectionApp.ned`

The implemented parameters are:

```text
approachSigmaM
approachConfusionMatrix
egoLateralSigmaM
egoLongitudinalSigmaM
lateralObservationSigmaM
laneObservationMode
adjacentLateralOriginX
adjacentLateralOriginY
adjacentLateralNormalX
adjacentLateralNormalY
adjacentLaneSeparationM
phase2LateralClaimOffsetM
longitudinalObservationSigmaM
physicalGateK
distanceStationarySpeedMps
stoppedDistanceAttestationRetryIntervalSec
stoppedDistanceAttestationRetryMax
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
- evaluate the continuous lateral residual before projecting the claimed lane;
- emit an honest early echo when the lateral check and existing ambulance
  checks pass, regardless of cue value;
- derive `observedCue` deterministically from the fixed signal-sample buffer and add it as a new signed echo field;
- retain the existing echo `direction` field as the declared `ann.direction`; do not reuse it for the observation;
- keep ambulance credential checking;
- use the deterministic collect-until policy in Section 7.1 instead of freezing
  at the first `f+1`;
- create one signed local claimant self-attestation with the declared cue,
  without perception sampling or Type-4 transmission;
- assemble certificates with all collected distinct physical-evidence-qualifying signatures,
  capped at `N`;
- compute `EligibleDirection(cert)` when packing the proposal;
- lock the first authenticated hash per `(witness,target,epoch)`, cache its
  verdict/echo, and reject later hashes without perception or echo generation;
- log one evaluation record with accept/reject reason.

`verifyCarPosition()` is not used as the imperfect arrival-gate decision.

### `ResDBDistanceProtocol.cc`

Responsibilities:

- create one signed stopped-distance attestation after the stop-zone and
  stationary predicates hold;
- retransmit only its cached bytes with a bounded phase-aware retry;
- cache one longitudinal observation/verdict per
  `(witness,target,epoch,attestationHash)`;
- collect distinct valid Type-19 echoes through the existing `0.25 s`
  post-threshold window;
- serialize Type-20 as a compact length-prefixed binary envelope while leaving
  the signed nested encodings unchanged;
- validate and bind the distance certificate to the early `claimHash`; and
- deterministically derive same-lane queue ranks by certified distance, then
  vehicle ID for ties.

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

The implemented Phase 2 runner adds separate scenario codes/names for:

- plausible wrong cardinal lane;
- false direction with contradictory signal;
- honest mixed maneuvers.

Post-certification maneuver deviation and the explicit perception-gate-off
switch remain deferred. Direction-eligibility-off and all-singleton are
implemented experiment modes. Preserve the existing `RESDB_NO_FIREWALL`
ablation.

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
- one authenticated claim variant eligible for evaluation per
  `(witness,target,epoch)`; replayed ACCEPT claims resend the identical echo,
  replayed REJECT claims remain rejected, and a different authenticated hash is
  retained as equivocation evidence and rejected without perception or echo;
- an explicit self-target guard prevents announcement gossip from invoking
  perception or creating a second claimant signature; the one permitted
  self-attestation is created locally and validated through the normal echo
  collection path;
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
  candidate's lowest jointly arrival-and-distance-certified primary before any
  PRE_PREPARE is processed; follower
  preverify remains a guarded backstop, not the first point at which stale PBFT
  primary state is repaired;
- after a Byzantine-primary rejection, ordinary authenticated PBFT view changes
  rotate only across usable certified candidates: an uncertified static
  successor causes another view change, while a successor with locally verified
  arrival and stopped-distance evidence may submit as the current PBFT primary;
  preverify independently rejects a leader absent from the validator's local
  certificate snapshot;
- unchanged PBFT and unchanged `kSafe`, with SIGNED-UNKNOWN becoming singleton
  through direction-3 fallthrough;
- runner generation of `perception_override.ini`, deterministic simulation
  seeds, parameterized result paths, and run metadata, with the two-RNG mapping
  also present in base `omnetpp.ini` for direct runs without an override;
- atomic single-write `[CONSENSUS_ATTACK_OUTCOME]` records so merged ResDB
  stderr cannot split the analyzer's structured line; and
- analyzer support for lane evaluations, cue distribution, duplicate-evaluation
  detection, measured `h_lane`/`h_dir`, self-attestations, certificate evidence,
  support, `b_sig`, derived direction, and all three trust tiers.

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

Checkpoint 2B closes the remaining Phase 1 verification tasks. The N=4 and
N=16 serialized committed orders match the frozen pre-self-attestation bytes,
and a Byzantine primary mutation from derived UNKNOWN to declared STRAIGHT is
explicitly rejected by Check 10 before an honest view commits and all vehicles
depart.

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
checklist (including no claimant perception evaluations, exactly one local
self-attestation per claimant, unsafe conflict-zone
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

### Phase 2A implementation record (2026-08-09)

Checkpoint 2A is implemented as the isolated `SixteenVehiclesMixedResDB`
configuration. Its checked-in route manifest contains one vehicle for every
`N/S/E/W x STRAIGHT/LEFT/RIGHT` pair plus one additional straight vehicle per
approach. This produces a balanced four vehicles per approach and an
8-straight/4-left/4-right mix. The manifest records the SUMO vehicle ID,
dynamic OMNeT node index, ingress/egress edges, declaration, and expected
signal. The existing straight-only 16-vehicle configuration and the safety
classifier are unchanged.

The self-checking entry point is:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-fixture-validation'
```

The first native-signal smoke reached the required supervision gate: all 12
routes were correct, but SUMO enabled turn signals too late for the existing
one-verdict-per-witness evidence window. Correct native cues during actual
witness evaluations ranged from `0` to `0.818`, so native blinkers are retained
as characterization data only and SHALL NOT drive E3/E4.

The approved fixture now uses a deterministic TraCI signal override while the
vehicle is on its ingress edge and discovery is `COLLECTING`. SUMO applies the
override for one simulation step, so each vehicle refreshes its route-consistent
LEFT/RIGHT/none signal from its mobility update. The trace and run metadata
record `cueSource=controlled`; the control path is enabled only by the mixed
Phase 2 configuration.

| Check | Result |
|---|---|
| Route/declaration manifest | 16/16 pass |
| Vehicle departures | 16/16 pass |
| Honest/zero-error configuration | pass |
| Controlled cue source active | 16/16 pass |
| Controlled cue correctness during actual witness evaluations | 100% on all eight turning routes |
| Existing `kSafe` entries vs. SUMO connection centerlines | 12/12 safe entries pass; zero unsafe entries |

The separate offline `audit_ksafe_geometry.py` check reconstructs each of the
12 SUMO movements from its internal connection-lane shapes and compares exact
centerline intersections with the executor allowlist. It reports 12 matching
safe pairs, 40 matching conflicts, 14 conservative omissions, and zero unsafe
`kSafe` entries. It does not edit `kSafe` and is not a runtime safety classifier.

No self-attestation, attack role, or conflict-zone metrology change is included
in Checkpoint 2A.

### Phase 2B implementation record (2026-08-09)

Checkpoint 2B adds one authenticated claimant self-attestation per arrival
claim. It is inserted locally through normal echo validation, carries the
announcement's declared maneuver as `observedCue`, consumes no perception
sample or RNG draw, and is never transmitted as Type-4 traffic. Announcement
replays cannot create another self-attestation. Certificates reject duplicate
signers and accept at most `N` distinct signatures.

The analyzer records `[SELF-ATTEST]` and `[CERT-EVIDENCE]`, reports external
`h_lane`, certificate-local external `h_dir`, and supporting `b_sig`, and keeps
self-attestation separate from physical perception evaluations.

The self-checking entry point is:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-self-attestation-validation'
```

| Check | Result |
|---|---|
| Phase 1 four-cell revalidation | pass |
| Self-attestations | exactly one per active claimant; no duplicate or self-perception records |
| Type-4 behavior | no claimant self-echo transmission |
| N=4 committed-order bytes | match frozen reference (`fc3ffcb2...f89292a`) |
| N=16 committed-order bytes | match frozen reference (`2822713e...dfd018`) |
| Check 10 UNKNOWN upgrade | mutation injected, state mismatch rejected, view changed, honest order committed, 4/4 departed |
| Veins release/debug builds | pass |

Checkpoint 2B completed before the separately gated E2/E4 work below; it did
not itself include attack roles or physical conflict metrology.

### Phase 2C implementation record (2026-08-09)

Checkpoint 2C adds authenticated `WRONG_APPROACH` (E2) and
`FALSE_DIRECTION` (E4) evidence attacks without changing the legacy `X`
scenario. The pilot target is `veh0`; configured Byzantine sets are nested in
ascending replica-ID order, and only the target changes its declaration.
External colluders use the normal signed echo and claim-hash path. E4 retains
honest lane qualification and changes only its supporting cue.

Phase 2 uses `N=16`, hence `f=5` and the evidence threshold is `f+1=6`.
The runner exposes `--experiment e2|e4`, `--inject-b`, and
`--attack-target`. The fixed validation preset is:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-attack-validation'
```

It executes `b=0..6` for each attack at zero error and at the approved
representative nonzero point (`sigma_approach=1 m` for E2 and
`epsilon_signal=0.20` for E4). Seeds are paired across `b` within each attack
and repetition. The analyzer reports certificate-local signers, `h_lane`,
`h_dir`, supporting `b_sig`, the required honest support, the empirical
single-witness error rate, and the corresponding Binomial-tail prediction.

The 28-cell Checkpoint 2C validation passes. E2 demonstrates the no-honest-
support boundary in runs whose finalized false certificate contains
`b_sig=f+1`. E4 also demonstrates why configured `b` must not replace measured
`b_sig`: with configured `b=f+1`, the fixed `0.25 s` collection window
finalized the representative certificate with only `b_sig=2`; the remaining
two required signatures would therefore have to be honest false cue support,
and the attack correctly did not unlock RIGHT in that run.

Both 16-vehicle SUMO configurations pin `collision.action=none` and
`time-to-teleport=-1` while retaining `collision.check-junctions=true`.
Collided or blocked vehicles are therefore never removed and reinserted as new
OMNeT modules. This prevents teleportation from producing stale identity-key
registrations or falsely successful departure traces and preserves physical
outcomes for the later metrology checkpoint.

Checkpoint 2C records schedules and physical traces but makes no calibrated
safety or collision claim. Those claims depend on the separately validated
actual-movement metrology below.

For quick N=16 characterization, the pilot runner provides a `grid` profile:
one repetition for every E1/E2/E3/E4 parameter cell (88 sequential runs:
5 + 35 + 6 + 42). The `full` profile contains 384 unique runs and adds the
approved repetitions plus shoulder and boundary measurements. Grid runs use
the same result layout as the full profile, so a later full run resumes from
and reuses completed grid cells; the grid alone is not used for statistical
shoulder claims.

### Phase 2D metrology implementation record (2026-08-09)

Checkpoint 2D implements physical-safety metrology without changing the
executor's `kSafe` table. `generate_phase2_movement_conflicts.py` reconstructs
the 12 approved movements from the mixed-route manifest and SUMO internal
connection-lane centerlines. Two movements conflict when they share an ingress
or their paths intersect inside the junction. The checked-in 12-by-12 table has
92 conflict cells and 52 non-conflict cells; it is symmetric and has a
conflicting diagonal. The separate `kSafe` audit still reports 12 matching safe
pairs, 40 matching conflicts, 14 conservative omissions, and zero unsafe
executor entries.

At runtime, `TraCIScenarioManager` records the planned ingress and egress from
the SUMO route, emits conflict-zone entry and exit events, and confirms the
observed outgoing edge. The analyzer fails validation for a missing or
mismatched actual egress. Concurrent occupancy is unsafe only when the two
actual movement indices conflict in the checked-in table. Claimed lane and
declared direction are never inputs to this oracle classifier, and its result
does not feed the witness gate, PBFT, or scheduling.

The self-checking entry point is:

```bash
~/.codex/skills/v2v-opp-env/scripts/activate_v2v_env.sh run 'ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --phase2-metrology-validation'
```

The validator first runs the complete 12-movement fixture and then two
calibration-only simultaneous releases. Both calibration cases pin TraCI
`speedMode=0`, release speed `14 m/s`, `jmIgnoreFoeProb=1`,
`collision.check-junctions=true`, `collision.action=none`, and
`time-to-teleport=-1`. North-straight with east-straight produces one measured
conflicting co-occupancy; north-straight with south-straight enters
simultaneously but is not labeled unsafe. Both vehicles in both cases exit on
their planned edges, and neither run teleports a vehicle. SUMO collision events
remain a secondary metric.

| Check | Result |
|---|---|
| Generated movement table | 144/144 entries; symmetric; diagonal conflicting |
| Mixed fixture movement ground truth | 16/16 ingress/egress pairs observed and confirmed |
| Mixed fixture zone lifecycle | 16/16 enter and exit exactly once |
| Conflicting calibration | N-S + E-S concurrently occupied and classified conflicting |
| Non-conflicting calibration | N-S + S-S concurrently occupied and not classified unsafe |
| Simulator controls and anti-teleport policy | pinned and verified in both cases |

The command writes `phase2_movement_conflicts.csv`,
`phase2_metrology_validation.json`, and `phase2_calibration_trace.csv` under
`benchmarks/Phase2Metrology`. Statistical shoulder pilots are intentionally not
started by this validation preset and require separate approval.

### Phase 2 pilot execution status (2026-08-10)

All implementation and calibration gates preceding the statistical pilots are
green at `N=16`:

| Gate | Status |
|---|---|
| Phase 1 E0 and nonzero wiring smokes | PASS |
| 2A mixed-maneuver fixture and controlled-cue characterization | PASS |
| 2B self-attestation, golden-order comparison, and Check 10 mutation recovery | PASS |
| 2C authenticated E2/E4 attack accounting | PASS |
| 2D actual-movement metrology and simultaneous-release calibration | PASS |
| One-repetition E1-E4 grid | PASS, 88/88 N=16 runs |
| Full statistical pilot profile | PASS, 384/384 N=16 runs; `theory_consistent=true` |
| Full longitudinal grid | PASS, 186/186 N=16 runs; cached `k` re-thresholding complete |

The full profile is run with:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --phase2-pilots --phase2-pilot-profile full
```

`benchmarks/Phase2Pilots/phase2_pilot_summary.json` records `profile=full`,
`passed=true`, `planned_unique_runs=384`, `completed_unique_runs=384`, and
`theory_consistent=true`; therefore the original Phase 2 pilot exit gate is
closed. The decisive result
is not merely that injected noise creates failures. It is agreement between
the empirical false-outcome intervals and the Binomial-tail prediction computed
from measured `h`, measured `q0`, and attempt-local Byzantine support. For
longitudinal certificate formation this is `b_sig_available`, not the
outcome-conditioned finalized-certificate count `b_sig_cert`.
The zero-error result is the `q0 -> 0` deterministic limit of that family.

### Deferred work after Phase 2

The following work is deliberately outside the Phase 2 exit gate. Deferral
does not imply that every item is optional for the final paper; the first two
are the highest-priority follow-on experiments because they provide the design
knob and the direction-mechanism contrast.

1. **E5 repeated-sampling design knob.** Sweep lane `K in {1,3,5}` and
   `tau_lane` using the existing per-target sample buffer. If cue `K` is also
   varied, retain deterministic majority-of-known cue derivation and the
   cross-witness `f+1` positive-support threshold.
2. **Baselines and ablations.** Add clearly isolated perception-gate-off,
   direction-eligibility-off, and all-singleton experiment switches; retain the
   existing firewall-off row. Run fixed-time TLS and all-way stop only as
   throughput/delay anchors. The direction-eligibility-off row is especially
   important if E3/E4 otherwise produce a visually flat direction result.
3. **E6 post-certification conformance.** Measure a consistent lie such as
   claim RIGHT, cue RIGHT, execute STRAIGHT. Report deviation-detection latency
   and physical outcomes only; do not claim the cue predicts future actuation.
4. **E7 correlated perception error.** Add a small common-mode episode to show
   where the independent-witness Binomial model ceases to apply.
5. **Paper-scale statistics and artifacts.** Increase repetitions only where
   pilot confidence intervals are too wide, freeze plotting scripts, and
   generate the final prediction-versus-empirical, trust-tier, batching,
   throughput/delay, conflict-co-occupancy, and collision figures.
6. **Richer sensing models.** Occlusion, observation latency, joint multi-car
   physical-consistency rejection, asymmetric signal miss/flip channels,
   Bayesian `P(D)`, credible-set scheduling, and CARLA or hardware-calibrated
   sensor channels remain future extensions. Continuous lateral pose and
   per-car longitudinal distance are no longer deferred; they are the
   immediate post-Phase-2 checkpoint below.
7. **Recovery beyond measurement.** CANCEL/tow response to E6, repeated live
   intersection rounds, and complete dynamic-membership/view-change recovery
   remain separate work. `ResdbOmnetRemoveReplica()` and multi-epoch reset are
   not complete enough to support those claims in this experiment.

The P0-P6 sequence below is retained as the acceptance-test history. P0-P3
and the implementation/validation portion of P4 are complete; the full P4
statistical exit gate is described above. P5 and P6 remain deferred.

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
- exactly one valid claimant self-attestation may appear, while claimant
  perception evaluation and claimant Type-4 echo transmission remain absent;
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

### P7: continuous lateral and longitudinal coordinates (active checkpoint)

This checkpoint adds the adjacent-physical-lane channel; it does not supersede
the categorical cardinal-approach channel. N/S/E/W intersection runs retain the
completed categorical model and 384-run suite as immutable regression evidence.

Execution order is mandatory:

1. validate the standalone adjacent-lane continuous fixture;
2. validate a standalone physically spaced stopped-queue fixture;
3. keep cardinal scheduler approach categorical and scope the scalar lateral
   gate to the reviewed A/B adjacent-lane fixture;
4. add the centimetre scalar claim and derived physical-lane index to the
   authenticated arrival evidence;
5. rerun zero-error golden vectors and Check 10 mutation tests;
6. rerun the E2 grid into a new result namespace; and
7. run E2-LONG only after the queue fixture and protocol byte tests pass.

The stopped-queue fixture must log the actual reference-point spacing, vehicle
length, bumper clearance, stop-line distance, and stopped-window validity. It
must compare empirical per-car residual decisions with Section 5.4 and raw
pair inversions with the distinct pairwise formula. A small `k` sweep is
computed by offline re-thresholding of saved residuals.

Protocol acceptance requires:

- zero error consumes no perception RNG draw;
- one evaluated authenticated claim variant per `(witness,target,epoch)`, with
  same-hash replay caching and different-hash equivocation rejection;
- repeated announcements never create new lateral or longitudinal trials;
- announcement, echo, certificate, snapshot, and proposal bytes bind the same
  canonical centimetre claims;
- Check 10 rejects a leader mutation of lane, distance, or derived direction;
- the scheduler orders same-lane entries by certified distance, then replica
  ID for an exact tie;
- `kSafe`, direction eligibility, QUIET handling, PBFT, and the collection
  timer remain mechanically unchanged; and
- old and new experiment outputs cannot overwrite one another.

Current implementation status: the stopped-distance wire path, bounded cached
attestation retry, single-evaluation cache, compact Type-20 certificate,
one queued byte-identical Type-20 reliability copy, certificate-derived queue
rank, runner overrides, and analyzer metrics are implemented for the
single-lane fixtures. The authenticated `FALSE_DISTANCE` path supports a signed
`claim=true+offset`, normal-format colluder echoes, separate attempt-local
`b_sig_available` prediction accounting, certificate-local `b_sig_cert`
forensic accounting, and actual-vs-certified same-lane ordering metrology. The
N=16 `b=f+1` boundary smoke produces all 16 distance certificates, zero QUIET
entries, and the intended ordering inversion; the complete Phase 1 regression
suite remains green. A focused Byzantine-primary mutation of the
certificate-derived rank is rejected by Check 10 (`cert pos=1`, `proposal
pos=2`), triggers view change, and recovers to an honest committed order with
all vehicles departing.

The corrected N=16 longitudinal fixture uses a derived OMNeT configuration so
its generated SUMO launch file has unambiguous precedence. Its 4/4 smoke passes:
requested `s=5.0 m` is measured as approximately `5.0021 m`; the `b=f`,
`sigma_long=1 m` cell produces the intended committed pair inversion (one-seed
prediction `0.1799`); the `b=f+1` cliff and `sigma_long=2 m` plumbing cells
invert; and the `s=12.5 m`, `b=f`, `sigma_long=1 m` tail remains clean. The
full 186-run N=16 grid also passes. At the headline `b=f`, `s=5 m`,
`sigma_long=1 m` cell, committed inversion is `4/20=0.20` versus prediction
`0.1799`; at `s=5.5 m` it is `1/20=0.05` versus `0.0452`; and at `s=6.5 m`
it is `0/20` versus `0.00157`. The `sigma_long=2 m`, `s=5 m` cell is `20/20`
versus `0.99999`. Offline re-thresholding of cached observations at
`k={2,2.5,3}` yields false-certificate outcomes `{0,2,4}/20` and honest
true-accept rates `{0.961,0.988,0.998}`.

The adjacent-lane protocol path is now implemented in the dedicated
`SixteenVehiclesAdjacentLaneResDB` fixture. It uses the same scalar residual
rule as the standalone calibration: the runtime derives the lane-normal axis
from the two TraCI lane centerlines after SUMO-to-OMNeT coordinate conversion,
projects and centimetre-quantizes the observation and signed claim, and applies
`abs(u_obs-u_claim) <= k*sigma_lat`. The authenticated announcement, echo, and
certificate bind `lateralClaimCm` and the physical-lane index. A zero-noise
smoke certified lane-0 claims at `0 cm` and lane-1 claims at `320 cm` with zero
residual. At `N=16`, `f=5`, `sigma_lat=0.5 m`, and `delta=1.75 m`, the fixed-seed
shoulder smoke required one honest false acceptance (`147 cm <= 150 cm`) after
five Byzantine supporting signatures and produced the model prediction
`q0=0.30854`. Cardinal N/S/E/W configurations remain on the categorical gate;
the runner rejects adjacent-lateral mode outside the dedicated fixture.
Removing the now-nonauthoritative early `positionInLane` wire field is cleanup,
not a prerequisite for the adjacent-lane experiment.

Run the focused wiring checkpoint before defining or launching the statistical
delta/sigma grid:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --adjacent-lane-validation
```

This preset runs exactly four cells, strictly sequentially: honest zero error,
the fixed-seed `b=f`, `sigma_lat=0.5 m`, `delta=1.75 m` shoulder, a zero-error
`b=f` guard, and a `b=f+1` cliff/accounting check. The cliff assertion uses the
Byzantine support actually present in the finalized certificate; configured
Byzantine membership alone is not treated as participation.

Checkpoint result: **4/4 PASS**. The honest cell preserved both physical lane
centres `(0 cm,index 0)` and `(320 cm,index 1)`; every logged witness decision
used `ADJACENT_LATERAL`; all four cells satisfied the single-evaluation and
authenticated-evidence invariants. The zero-error `b=f` claim did not certify.
The shoulder certificate contained signers `{0,1,2,3,4,5}`, with `b_sig=5` and
one honest acceptance. The cliff certificate contained Byzantine signers
`{0,1,2,3,4,5}`, with `b_sig=6` and no required honest support. The canonical
summary is `benchmarks/Phase2AdjacentLane/adjacent_lane_validation_summary.json`.

#### Full-intersection two-lane replacement

The former 310-run straight-road declaration is retired as a paper experiment.
Only three development cells were executed before it was stopped. Those cells
remain calibration evidence, but they do not measure intersection scheduling or
physical safety and SHALL NOT be pooled with the final results.

The replacement uses `bft_intersection_2lane.net.xml` with the following locked
physical-lane policy on every approach:

```text
physical lane 0 (T / outer): STRAIGHT or RIGHT
physical lane 1 (L / inner): LEFT only
```

Checkpoint 1 is complete. `fourway/two_lane_calibration/run_calibration.py`
passes sigma-zero exactness, Gaussian q0/q1 agreement, monotonicity, smooth
shoulder, Python/C++ gate parity, full-intersection connection geometry, and
no-teleport validation. The N=16 route manifest then validates all twelve
movements, eight vehicles per physical lane, the echo-window lane assignment,
the intended egress, and zero collisions/teleports under sequential release.

Physical-lane evidence now controls scheduling authority by constraining the
derived direction:

```text
laneAuthorized = declared direction is permitted by certified physical lane
eligibleDirection = declared direction
                    iff laneAuthorized and cueSupport >= f+1
                    else UNKNOWN
```

`UNKNOWN` uses the existing singleton fallthrough; `kSafe` is unchanged. The
proposal and certificate snapshot both bind `physical_lane_index` and
`lateral_claim_cm`, and Check 10 compares those fields plus the derived
direction. The reviewed attack target is an actual North-inner LEFT vehicle
claiming North-outer RIGHT with a controlled RIGHT cue. Existing `kSafe` permits
the claimed `N-R + S-S` pair, while the actual two-lane paths `N-L + S-S`
conflict. Thus any unsafe scheduling authority must pass the probabilistic
lateral gate rather than arise from an unrelated direction-cue failure.

The first honest zero-error N=16 smoke has all sixteen claims at exactly
`(0 cm,lane 0)` or `(320 cm,lane 1)`, all sixteen lane-authorized directions,
zero perception RNG draws, no Check 10 rejection, and all vehicles departed.
The unified full-intersection wiring checkpoint is:

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-validation
```

Its original five cells run strictly sequentially and report **5/5 PASS**.
At `b=f=5`, `sigma_lat=0`, the false physical-lane claim does not form a
certificate and the target commits QUIET. At `b=f`, `sigma_lat=0.5 m`, the
finalized certificate contains `b_sig=5` and four honest supporting echoes, so
the false outer-lane/right-turn authority is unlocked. At `b=f+1=6`,
`sigma_lat=0`, the certificate contains exactly six Byzantine supporting
signatures and no honest support. The honest `sigma_long=1 m` cell partitions
queue-rank comparisons by `(approach,physicalLaneIndex)` and all sixteen cars
depart. Finally, Byzantine fault type `11` mutates the proposal's
`physical_lane_index`/`lateral_claim_cm`; Check 10 rejects it, view change
occurs, and an honest proposal commits. The canonical report is
`benchmarks/Phase2TwoLaneValidation/two_lane_validation_summary.json`.

The dedicated deterministic conflict-release validation additionally proves
the complete physical chain without changing `kSafe` or the executor:

```text
false physical-lane evidence
  -> direction/scheduling authority
  -> unchanged-kSafe committed co-batch
  -> actual conflicting movements
  -> conflict-zone co-occupancy
```

The target and reviewed South-straight counterpart commit in batch 0, while
SUMO ingress/egress ground truth identifies their actual movements as North-left
and South-straight. Runtime/config evidence pins `speedMode=0`,
`jmIgnoreFoeProb=1`, junction collision checking, `collision.action=none`, and
teleport disabled. The focused artifact reports **15/15 PASS** at
`benchmarks/Phase2TwoLaneValidation/conflict_release_cliff/run_0/two_lane_conflict_release_validation.json`.
This unlocks the statistical matrix. Its lateral sigma axis SHALL use
`{0,.25,.5,.75,1,1.5,2,3}`.

The locked replacement contains 400 unique parameter/seed cells, and every cell
now runs the full-intersection `SixteenVehiclesTwoLaneConflictReleaseResDB`
fixture rather than the retired straight road. It sweeps the reviewed delta,
sigma, Byzantine-boundary, and fresh-`k` slices; runs strictly sequentially;
resumes only exact metadata matches; and records both false lane certificates
and actual `N-L + S-S` conflicting co-occupancy. The signed runtime offset is
`-delta` because the North lane-normal coordinate maps inner lane 1 at 3.2 m to
outer lane 0 at 0 m. The `b=0` boundary cell is an honest no-attack control:
its effective offset is zero and its acceptance rate is `q1`, not `q0`; it is
not a false-certificate probability point.

Lane analysis uses the same attempt/certificate distinction as the longitudinal
channel. `b_sig_attempt` is the claimant self-attestation plus distinct
supporting Byzantine echoes actually collected before the attempt closes and
is used in `r=max(0,f+1-b_sig_attempt)`. `b_sig_cert` is read from finalized
certificate bytes and is retained for forensic accounting. A failed attempt
MUST NOT be assigned `b_sig_attempt=0` merely because no certificate bytes
exist. New runs log `[ECHO-COLLECT]`; older `b=1` logs are exactly recoverable
from the valid claimant self-attestation because no Byzantine colluder exists.
Configured Byzantine membership is never substituted for collected support.

#### Completion-capable `k` and sigma parameter sweeps

The early-stop conflict fixture proves the physical consequence but cannot
produce departure, wait, or throughput measurements after the first unsafe
overlap. Figure B (`k`) and Figure C (lateral sigma) therefore use
`SixteenVehiclesTwoLaneSweepResDB`, which extends the identical route/physics
fixture and changes only `endOnFirstConflictingCooccupancy=false`. Protocol,
gate, scheduler, `kSafe`, PBFT, Check 10, seeds, and attack semantics remain
unchanged. All 16 vehicles must depart in every parameter-sweep run.

Figure B sweeps `k={1,2,3}` at `delta=1.75 m`, `sigma_lat=.5 m`, and
`b=1..6`. Figure C sweeps
`sigma_lat={.1,.3,.5,.7,1,1.5,2} m` at `delta=1.75 m`, `k=3`, and `b=1..6`.
Both use one repetition per smoke cell and 20 paired repetitions per full
cell. Their `(delta=1.75,k=3,sigma=.5,b,rep)` rows resolve to one shared
completion-artifact path, so Figures B and C consume the exact same anchor
artifacts. The fixed parameter bytes are identical to the existing early-stop
delta-by-`b` anchor. Separate ResDB/TraCI process launches do not guarantee the
same echo-arrival order or certificate signer membership even under a paired
OMNeT seed; therefore cross-grid validation compares the 20-run false-certificate
and pooled-`q0` Wilson intervals, not per-run signer lists. Per-run signer and
honest-accept differences remain recorded as forensic observations.
For publication, the overlapping delta-by-`b` cells are replaced—not pooled—
with these same completion-capable shared artifacts, so Figures A, B, and C
report one canonical value at every shared `(b,rep)` anchor. The original
early-stop summary remains archived as a reproducibility/reconciliation
artifact and is not a second paper data point.

Before locking the publication operating point, an attack-free companion sweep
uses the same completion-capable N=16 two-lane fixture. It crosses
`k={1,2,3}` with
`sigma_lat={0,.1,.3,.5,.7,1,1.5,2}` and records pooled per-witness true-accept
rate `q1`, lane-certificate rate, QUIET/SIGNED-UNKNOWN outcomes, throughput,
batch size, and mean/p95 wait. Honest declarations always have `delta=0`; an
"honest delta sweep" would repeat identical claim bytes under misleading
labels and is therefore forbidden. The report presents two explicit candidate
criteria at `sigma_lat=.5`: a system criterion (at least 99% lane
certification, zero QUIET vehicles, and throughput within 5% of the best `k`)
and a strict sensor criterion that additionally requires pooled witness
`q1>=.99`. It reports candidates but does not silently rewrite protocol
configuration.

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-honest-operating-sweep smoke

ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-honest-operating-sweep k-full

# Optional full honest k x sigma surface; reuses the k-full artifacts.
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-honest-operating-sweep full
```

If throughput/delay is required for every Figure A cell, run the dedicated
completion grid. It reuses the 120 shared anchor artifacts and creates 241
completion-config artifacts (the 240 non-anchor attack runs plus the single
honest control). Every aggregate cell must then contain `16 × repetitions`
wait samples before the publication aliases are updated.

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-delta-b-completion-grid
```

Each aggregate includes Wilson intervals, measured `h`/`b_sig`, throughput,
pooled mean/p95 wait, final committed trust-tier singleton percentages, and
mean batch size. A post-departure QUIET lifecycle log cannot overwrite a
previously observed signed committed tier. Full profiles fail validation if
false-certificate/co-occupancy coupling breaks, background co-occupancy is
nonzero, empirical `q0` is non-monotonic for any `b`, the pooled acceptance
ratio is inconsistent, throughput/wait samples are incomplete, or the shared
anchor disagrees with the delta-by-`b` reference.

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --two-lane-k-sweep smoke
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --two-lane-k-sweep full
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --two-lane-sigma-sweep smoke
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py --two-lane-sigma-sweep full
```

The controlled fixture enforces the strict end-to-end invariant: a false
physical-lane certificate must unlock the reviewed co-batch and produce the
`N-L + S-S` co-occupancy, while a run without that false certificate must not
produce the reviewed co-occupancy. Any mismatch fails validation and is also
retained under `background_conflicting_cooccupancy` or the corresponding
false-certificate-without-consequence fields for diagnosis.

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --two-lane-grid

# Rebuild the JSON/CSV summary from exact existing artifacts only.
python3 experiment_orchestrator.py --two-lane-grid-reanalyze
```

```bash
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --longitudinal-grid --longitudinal-grid-profile smoke

# Fresh executed-k=2 smoke; it cannot reuse the earlier k=3 artifacts.
ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --longitudinal-grid --longitudinal-grid-profile smoke --physical-gate-k 2

ORCHESTRATOR_SKIP_OMNET_SOURCE=1 python3 experiment_orchestrator.py \
  --longitudinal-grid --longitudinal-grid-profile full

# Rebuild summaries and plots from the 186 existing JSON artifacts only.
python3 experiment_orchestrator.py \
  --longitudinal-grid-reanalyze --longitudinal-grid-profile full
```

The `b=f+1` inversion is a plumbing/cliff result: Byzantine signatures can meet
the threshold without honest support, so it is not evidence for a probabilistic
sensor shoulder. The headline certificate and committed-order shoulder is
measured at `b=f`, where at least one honest false acceptance is required. All
plots and prose SHALL label `b=f+1` points as the Byzantine cliff or boundary
check, never as shoulder evidence.

The focused security regression is reproducible with:

```bash
python3 experiment_orchestrator.py --distance-rank-check10-validation
```

## 16. Claims the resulting experiment supports

The paper may claim, after P7 validation:

1. PBFT still gives deterministic agreement under the existing Byzantine and timing assumptions.
2. An `f+1` lane/arrival certificate contains at least one honest lane-qualifying signer when at most `f` certificate signers are Byzantine.
3. An eligible scalar direction supported by `f+1` signed matching cues contains at least one honest supporting observation under the same bound.
4. Co-batching authority scales with positive maneuver evidence: insufficient support produces a SIGNED-UNKNOWN singleton without discarding the valid arrival certificate.
5. PicaBFT exposes three explicit trust tiers—QUIET, SIGNED-UNKNOWN, and SIGNED with an eligible direction—whose safety and throughput effects can be measured separately.
6. Physical correctness remains probabilistic and depends on false-accept/support rates, sensor model, correlation, witness opportunities, and later actuation conformance.
7. A future declared-direction-only experiment can serve as the eligibility-off
   ablation, while zero perception error is the deterministic limit of the
   implemented evidence model.
8. Lateral lane evidence and per-car longitudinal distance evidence are noisy
   continuous observations converted to deterministic certified scheduler
   inputs; neither field is taken directly from hidden SUMO truth by witnesses.
9. The per-car false-distance certificate follows the measured-`h` Binomial
   model under independent witness errors, while the resulting pairwise
   ordering-violation probability is a separate empirical outcome.

The paper must not claim:

- that an integer queue rank is itself a sensor measurement;
- that the raw pairwise inversion formula is the probability of a false
  distance certificate or a committed ordering violation;
- that independently valid equal-distance claims receive an unimplemented
  joint physical-consistency check;
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
  actual world position and distance to stop line
  actual route and signal state
  actual executed movement
          |
          +------------------------------+
          |                              |
          v                              v
  evaluation metrology           ResDBPerception owned by witness i
  conflicting co-occupancy       2D lateral + 1D longitudinal + cue corruption
  true same-lane order
  collisions / actual movement
                                         |
                                         v
                         fixed stopped-window sample buffer
                                         |
                   +-------------+-------------+
                   |             |             |
                   v             v             v
          lateral residual  distance residual  derive observedCue
          then project lane stopped sample only majority known/UNKNOWN
                   |             |             |
                   +-------+-----+-------------+
                           |
          both physical gates + ambulance checks
                  |              |
                reject         accept
                  |              |
               no echo   signed ARRIVAL_ECHO
                         canonical coordinate claims
                         declared direction + observedCue + claimHash
                                        ^
                                        |
                 one local claimant self-attestation
                 self coordinates + declared cue;
                 no perception or Type-4 transmission
                                        |
                     collect qualifying echoes until close
                                        |
                           at least f+1 distinct echoes?
                              |                    |
                             no                   yes
                              |                    |
                         QUIET entry         PHYSICAL-EVIDENCE ARRIVAL_CERT
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
                         lane + distanceToStopCm + eligible direction
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

> PicaBFT uses `f+1` imperfect continuous lateral and longitudinal evidence to
> certify deterministic lane and same-lane ordering inputs, `f+1` signed
> positive maneuver cues to unlock a declared direction for co-batching, and
> deterministic PBFT agreement over the certificate-derived outcome. Missing
> cue evidence degrades safely to a SIGNED-UNKNOWN singleton; it does not erase
> the arrival certificate or claim to prove future intent.
