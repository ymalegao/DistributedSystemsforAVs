#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "veins/veins.h"

namespace omnetpp {
class cRNG;
}

namespace veins {

class TraCIMobility;

enum class ObservedCue : uint8_t {
    STRAIGHT = 0,
    LEFT = 1,
    RIGHT = 2,
    UNKNOWN = 3,
};

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

struct StoppedDistancePerceptionSample {
    bool detected = false;
    bool stationary = false;
    bool distanceValid = false;
    double trueDistanceToStopM = 0.0;
    double observedDistanceToStopM = 0.0;
    omnetpp::simtime_t observedAt{};
};

// One noisy longitudinal classification of a target relative to the conflict
// zone.  signedMarginM is positive inside an internal junction lane and
// negative on either adjoining external edge.  This gives BLOCKED and CLEAR a
// physical observation channel without treating TraCI's lane-id bit as a
// perfect sensor.
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
    void configure(TraCIMobility* mobility,
                   omnetpp::cRNG* rng,
                   const std::string& matrixSpec,
                   double approachSigmaM,
                   double signalError,
                   double lateralObservationSigmaM,
                   double longitudinalObservationSigmaM,
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
    ConflictBoxPerceptionSample observeAnyConflictBoxOccupancy(
        omnetpp::simtime_t now) const;

    uint64_t randomDrawCount() const { return random_draw_count_; }

    static ObservedCue cueFromCode(uint8_t code);
    static const char* cueName(ObservedCue cue);
    static ObservedCue readNativeCue(TraCIMobility* target);
    int32_t ownLateralClaimCm(omnetpp::simtime_t now) const;
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
    ConflictBoxPerceptionSample measureConflictBoxTruth(
        const std::string& targetCarId, omnetpp::simtime_t now) const;

    TraCIMobility* mobility_ = nullptr;
    omnetpp::cRNG* rng_ = nullptr;
    std::array<double, 16> matrix_{};
    double approach_sigma_m_ = 0.0;
    double signal_error_ = 0.0;
    double lateral_observation_sigma_m_ = 0.0;
    double longitudinal_observation_sigma_m_ = 0.0;
    bool adjacent_lateral_enabled_ = false;
    double lateral_origin_x_ = 0.0;
    double lateral_origin_y_ = 0.0;
    double lateral_normal_x_ = 0.0;
    double lateral_normal_y_ = 1.0;
    double adjacent_lane_separation_m_ = 3.2;
    mutable uint64_t random_draw_count_ = 0;
};

} // namespace veins
