#pragma once

// ResDBPerception.h — the sensor model between TraCI truth and what a witness
// is allowed to believe.
//
// Every other part of this protocol reads TraCI directly and therefore sees
// ground truth, which quietly assumes perfect perception: a witness always knows
// exactly which lane a car is in and which way it is signalling. That assumption
// is what made verifyCarPosition() a sound admission gate, and it is not true of
// real vehicles.
//
// This class is the one place that assumption is broken, deliberately and
// measurably. It reads the true value from TraCI and returns a corrupted
// observation:
//
//   cardinal approach   through a row-stochastic 4x4 confusion matrix
//   lateral position    additive Gaussian noise on a TraCI-derived lane frame
//   maneuver cue        misread with probability signalObservationError
//
// Nothing here decides anything. It reports what was observed; the arrival
// protocol decides whether an observation is close enough to a claim to justify
// an echo. Keeping the two apart is what lets the gate be re-tuned (physicalGateK)
// without touching the sensor, and lets the sensor be validated against its own
// analytic model without a running protocol.
//
// Every random draw is counted. randomDrawCount() is reported at finish() as
// [PERCEPTION-RNG]; two runs at the same seed must draw the same number of times,
// and a zero-noise configuration must draw zero. That counter is the only cheap
// check that a refactor has not silently changed the noise stream.

#include <array>
#include <cstdint>
#include <string>

#include "v2vbft/v2vbft.h"
#include "v2vbft/protocol/Primitives.h"  // ObservedCue

namespace omnetpp {
class cRNG;
}

// Qualified deliberately. Declaring `class TraCIMobility;` inside namespace
// v2vbft would declare a *new* v2vbft::TraCIMobility that shadows the real Veins
// type, and the only symptom is an incomplete-type error far away in the .cc.
namespace veins {
class TraCIMobility;
}

namespace v2vbft {

// How much a lane claim has to say, and therefore what can be checked.
//
// CATEGORICAL_CARDINAL is the single-lane-per-approach checkpoint: a claim is
// just N/S/E/W, and the only question is whether the witness saw the same
// letter. ADJACENT_LATERAL is the two-lane network, where a claim also names a
// physical lane index and can be checked against a continuous lateral
// coordinate -- which is what makes a lane lie something a sensor can catch
// rather than something only a signature can.
enum class LaneObservationMode : uint8_t {
    CATEGORICAL_CARDINAL = 0,
    ADJACENT_LATERAL     = 1,
};

// One witness's observation of a vehicle arriving at the junction.
//
// Carries the true value beside the observed one throughout. That is for
// analysis only -- the protocol must read the observed fields, never the true
// ones -- but it is what makes a run self-describing: the measured accept rate
// can be recomputed from the log without re-running the simulation.
struct ArrivalPerceptionSample {
    bool detected = false;
    char trueApproach = '?';
    char observedApproach = '?';
    ObservedCue trueCue = ObservedCue::UNKNOWN;
    ObservedCue observedCue = ObservedCue::UNKNOWN;
    omnetpp::simtime_t observedAt{};
    int knownCueSamples = 0;
    bool continuousPositionValid = false;
    bool longitudinalDistanceValid = false;
    double trueX = 0.0;
    double trueY = 0.0;
    double observedX = 0.0;
    double observedY = 0.0;
    double trueDistanceToStopM = 0.0;
    double observedDistanceToStopM = 0.0;
    int truePhysicalLaneIndex = -1;
    int observedPhysicalLaneIndex = -1;
    bool lateralCoordinateValid = false;
    int32_t trueLateralCm = 0;
    int32_t observedLateralCm = 0;
};

// One witness's observation of an already-stopped vehicle's distance to the stop
// line. Separate from the arrival sample because it is taken later, under a
// different precondition (the target must actually be stationary), and gates a
// different certificate.
struct StoppedDistancePerceptionSample {
    bool detected = false;
    bool stationary = false;
    bool distanceValid = false;
    double trueDistanceToStopM = 0.0;
    double observedDistanceToStopM = 0.0;
    omnetpp::simtime_t observedAt{};
};

// One witness's observation of whether the conflict box is occupied.
//
// signedMarginM is positive inside an internal junction lane (distance to the
// nearer end of it) and negative on either adjoining external edge (distance
// still to travel, or already travelled past). That gives BLOCKED and CLEAR a
// continuous physical channel instead of treating TraCI's lane-id ':' prefix as
// a perfect sensor.
//
// The two consumers deliberately fail in OPPOSITE directions, and both call
// sites depend on it: BLOCKED fails open (an unobservable target never triggers
// a cancel) while CLEAR fails closed (an unobservable box is never declared
// clear). Read `valid` before either observed field.
struct ConflictBoxPerceptionSample {
    bool detected = false;
    bool valid = false;
    bool trueOccupied = false;
    bool observedOccupied = false;
    double trueSignedMarginM = 0.0;
    double observedSignedMarginM = 0.0;
    omnetpp::simtime_t observedAt{};
};

class ResDBPerception {
public:
    // Throws cRuntimeError on any inconsistent configuration -- negative sigma, a
    // matrix that is not 16 row-stochastic entries, a zero lane normal, or a
    // configured lane separation that disagrees with TraCI geometry. Failing at
    // init is the point: a silently mis-specified sensor produces plausible
    // numbers that mean nothing.
    void configure(veins::TraCIMobility* mobility,
                   omnetpp::cRNG* rng,
                   const std::string& matrixSpec,
                   double approachSigmaM,
                   double signalError,
                   double lateralObservationSigmaM,
                   double longitudinalObservationSigmaM,
                   double occupancyObservationSigmaM,
                   bool adjacentLateralEnabled,
                   double lateralOriginX,
                   double lateralOriginY,
                   double lateralNormalX,
                   double lateralNormalY,
                   double adjacentLaneSeparationM);

    ArrivalPerceptionSample observeArrival(const std::string& targetCarId,
                                           omnetpp::simtime_t now) const;
    StoppedDistancePerceptionSample observeStoppedDistance(
        const std::string& targetCarId, omnetpp::simtime_t now,
        double stationarySpeedMps) const;
    ConflictBoxPerceptionSample observeConflictBoxOccupancy(
        const std::string& targetCarId, omnetpp::simtime_t now) const;
    // Whole-box occupancy, for the CLEAR predicate. Takes at most ONE noise draw
    // per call regardless of how many vehicles are queued: noising each vehicle
    // independently and OR-ing the results would make the false-occupied rate
    // grow with traffic count rather than with sigma.
    ConflictBoxPerceptionSample observeAnyConflictBoxOccupancy(
        omnetpp::simtime_t now) const;

    uint64_t randomDrawCount() const { return random_draw_count_; }

    static const char* cueName(ObservedCue cue);
    static ObservedCue readNativeCue(veins::TraCIMobility* target);
    int32_t ownLateralClaimCm(omnetpp::simtime_t now) const;
    // 0 or 1 in adjacent-lane mode; -1 when the coordinate sits on the boundary
    // and neither lane can be claimed.
    int projectPhysicalLaneIndex(int32_t lateralCm) const;

private:
    bool deriveAdjacentLaneFrame(const std::string& physicalLaneId,
                                 double& originX,
                                 double& originY,
                                 double& normalX,
                                 double& normalY,
                                 double& separationM) const;
    static int approachIndex(char approach);
    static char approachChar(int index);
    char sampleApproach(char truth) const;
    ObservedCue sampleCue(ObservedCue truth) const;
    double sampleGaussian(double sigma) const;
    // Noise-free classification of one vehicle against the conflict box. Depends
    // on the four-way net's edge naming (C2* outbound, *2C inbound) and yields
    // valid=false on any net that renames them -- the same assumption
    // vehicleHasClearedIntersectionTraCI() already makes in ResDBTraCI.cc.
    ConflictBoxPerceptionSample measureConflictBoxTruth(
        const std::string& targetCarId, omnetpp::simtime_t now) const;

    veins::TraCIMobility* mobility_ = nullptr;
    omnetpp::cRNG* rng_ = nullptr;
    std::array<double, 16> matrix_{};
    double approach_sigma_m_ = 0.0;
    double signal_error_ = 0.0;
    double lateral_observation_sigma_m_ = 0.0;
    double longitudinal_observation_sigma_m_ = 0.0;
    double occupancy_observation_sigma_m_ = 0.0;
    bool adjacent_lateral_enabled_ = false;
    double lateral_origin_x_ = 0.0;
    double lateral_origin_y_ = 0.0;
    double lateral_normal_x_ = 0.0;
    double lateral_normal_y_ = 1.0;
    double adjacent_lane_separation_m_ = 3.2;
    mutable uint64_t random_draw_count_ = 0;
};

} // namespace v2vbft
