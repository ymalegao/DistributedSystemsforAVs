#include "veins/modules/application/resDB/ResDBPerception.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <omnetpp/crandom.h>

#include "veins/base/utils/FindModule.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include "veins/modules/mobility/traci/VehicleSignal.h"

using namespace omnetpp;

namespace veins {

namespace {

std::vector<double> parseMatrix(const std::string& spec)
{
    std::string normalized = spec;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::stringstream ss(normalized);
    std::vector<double> values;
    double value = 0;
    while (ss >> value) values.push_back(value);
    return values;
}

} // namespace

ObservedCue ResDBPerception::readNativeCue(TraCIMobility* target)
{
    if (!target) return ObservedCue::UNKNOWN;
    try {
        const VehicleSignalSet signals = target->getSignals();
        const bool left = signals.test(VehicleSignal::blinker_left);
        const bool right = signals.test(VehicleSignal::blinker_right);
        const bool emergency = signals.test(VehicleSignal::blinker_emergency);
        if (emergency || (left && right)) return ObservedCue::UNKNOWN;
        if (left) return ObservedCue::LEFT;
        if (right) return ObservedCue::RIGHT;
        return ObservedCue::STRAIGHT;
    } catch (...) {
        return ObservedCue::UNKNOWN;
    }
}

void ResDBPerception::configure(TraCIMobility* mobility,
                                cRNG* rng,
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
                                double adjacentLaneSeparationM)
{
    mobility_ = mobility;
    rng_ = rng;
    approach_sigma_m_ = approachSigmaM;
    signal_error_ = signalError;
    lateral_observation_sigma_m_ = lateralObservationSigmaM;
    longitudinal_observation_sigma_m_ = longitudinalObservationSigmaM;
    adjacent_lateral_enabled_ = adjacentLateralEnabled;
    lateral_origin_x_ = lateralOriginX;
    lateral_origin_y_ = lateralOriginY;
    double configuredNormalX = lateralNormalX;
    double configuredNormalY = lateralNormalY;
    if (adjacent_lateral_enabled_) {
        // Validate the caller's own inbound pair now. Observations derive the
        // same frame from the target's lane, so a four-way fixture never uses
        // the witness approach's normal to project another approach.
        try {
            auto vehicle = mobility_->getCommandInterface()->vehicle(mobility_->getExternalId());
            const std::string ownLaneId = vehicle.getLaneId();
            double derivedSeparation = 0.0;
            if (!deriveAdjacentLaneFrame(ownLaneId, lateral_origin_x_, lateral_origin_y_,
                    configuredNormalX, configuredNormalY, derivedSeparation))
                throw cRuntimeError("adjacent lane frame unavailable for %s", ownLaneId.c_str());
            if (std::abs(derivedSeparation - adjacentLaneSeparationM) > 0.01)
                throw cRuntimeError(
                    "adjacent lane separation mismatch: geometry=%g configured=%g",
                    derivedSeparation, adjacentLaneSeparationM);
            adjacent_lane_separation_m_ = derivedSeparation;
        } catch (const cRuntimeError&) {
            throw;
        } catch (...) {
            throw cRuntimeError("failed to derive adjacent-lane geometry from TraCI");
        }
    }
    const double normalLength = std::hypot(configuredNormalX, configuredNormalY);
    if (adjacent_lateral_enabled_ && normalLength <= 0.0)
        throw cRuntimeError("adjacent lateral normal must be non-zero");
    lateral_normal_x_ = normalLength > 0.0 ? configuredNormalX / normalLength : 0.0;
    lateral_normal_y_ = normalLength > 0.0 ? configuredNormalY / normalLength : 1.0;
    if (!adjacent_lateral_enabled_)
        adjacent_lane_separation_m_ = adjacentLaneSeparationM;
    random_draw_count_ = 0;
    if (approach_sigma_m_ < 0.0)
        throw cRuntimeError("approachSigmaM must be non-negative");
    if (signal_error_ < 0.0 || signal_error_ > 1.0)
        throw cRuntimeError("signalObservationError must be in [0,1]");
    if (lateral_observation_sigma_m_ < 0.0)
        throw cRuntimeError("lateralObservationSigmaM must be non-negative");
    if (longitudinal_observation_sigma_m_ < 0.0)
        throw cRuntimeError("longitudinalObservationSigmaM must be non-negative");
    if (adjacent_lateral_enabled_ && adjacent_lane_separation_m_ <= 0.0)
        throw cRuntimeError("adjacentLaneSeparationM must be positive");

    const std::vector<double> values = parseMatrix(matrixSpec);
    if (values.size() != matrix_.size())
        throw cRuntimeError("approachConfusionMatrix must contain exactly 16 probabilities");
    for (size_t row = 0; row < 4; ++row) {
        double sum = 0.0;
        for (size_t col = 0; col < 4; ++col) {
            const double p = values[row * 4 + col];
            if (!std::isfinite(p) || p < 0.0 || p > 1.0)
                throw cRuntimeError("approachConfusionMatrix entries must be in [0,1]");
            matrix_[row * 4 + col] = p;
            sum += p;
        }
        if (std::abs(sum - 1.0) > 1e-9)
            throw cRuntimeError("each approachConfusionMatrix row must sum to 1");
    }
}

ArrivalPerceptionSample ResDBPerception::observeArrival(const std::string& targetCarId,
                                                         simtime_t now) const
{
    ArrivalPerceptionSample sample;
    sample.observedAt = now;
    if (!mobility_ || !mobility_->getManager()) return sample;
    const auto& managedHosts = mobility_->getManager()->getManagedHosts();
    auto hostIt = managedHosts.find(targetCarId);
    if (hostIt == managedHosts.end()) return sample;
    cModule* targetHost = hostIt->second;
    TraCIMobility* targetMobility = FindModule<TraCIMobility*>::findSubModule(targetHost);
    if (!targetMobility) return sample;

    try {
        std::string road = targetMobility->getRoadId();
        if (road.empty()) return sample;
        char approach = static_cast<char>(std::toupper(static_cast<unsigned char>(road[0])));
        if (approachIndex(approach) < 0) {
            const std::string lane = targetMobility->getCommandInterface()
                ->vehicle(targetCarId).getLaneId();
            if (lane.empty()) return sample;
            approach = static_cast<char>(std::toupper(static_cast<unsigned char>(lane[0])));
        }
        if (approachIndex(approach) < 0) return sample;
        sample.detected = true;
        sample.trueApproach = approach;
        sample.observedApproach = sampleApproach(approach);
        auto targetVehicle = targetMobility->getCommandInterface()->vehicle(targetCarId);
        const std::string physicalLaneId = targetVehicle.getLaneId();
        const Coord truePosition = targetMobility->getPositionAt(now);
        sample.continuousPositionValid = true;
        sample.trueX = truePosition.x;
        sample.trueY = truePosition.y;
        if (adjacent_lateral_enabled_) {
            double targetOriginX = 0.0;
            double targetOriginY = 0.0;
            double targetNormalX = 0.0;
            double targetNormalY = 0.0;
            double targetSeparationM = 0.0;
            if (!deriveAdjacentLaneFrame(physicalLaneId, targetOriginX, targetOriginY,
                    targetNormalX, targetNormalY, targetSeparationM) ||
                    std::abs(targetSeparationM - adjacent_lane_separation_m_) > 0.01)
                return sample;
            const double trueLateral =
                (sample.trueX - targetOriginX) * targetNormalX +
                (sample.trueY - targetOriginY) * targetNormalY;
            const double observedLateral =
                trueLateral + sampleGaussian(lateral_observation_sigma_m_);
            sample.trueLateralCm = static_cast<int32_t>(std::llround(trueLateral * 100.0));
            sample.observedLateralCm =
                static_cast<int32_t>(std::llround(observedLateral * 100.0));
            sample.lateralCoordinateValid = true;
            const double observedDelta = observedLateral - trueLateral;
            sample.observedX = sample.trueX + observedDelta * targetNormalX;
            sample.observedY = sample.trueY + observedDelta * targetNormalY;
        } else {
            // Cardinal mode is driven by the checked-in categorical matrix.
            // It intentionally does not consume continuous-lateral draws.
            sample.observedX = sample.trueX;
            sample.observedY = sample.trueY;
        }
        sample.truePhysicalLaneIndex = targetVehicle.getLaneIndex();
        // The single-lane checkpoint has exactly one lane per approach, so
        // projection is identity. Multi-lane projection is added with its
        // reviewed lane geometry; never infer it from the cardinal code.
        sample.observedPhysicalLaneIndex = sample.truePhysicalLaneIndex;
        if (!physicalLaneId.empty() && physicalLaneId.front() != ':') {
            const double laneLength = targetMobility->getCommandInterface()
                ->lane(physicalLaneId).getLength();
            sample.trueDistanceToStopM = std::max(0.0, laneLength - targetVehicle.getLanePosition());
            // Early arrival evidence does not observe or certify longitudinal
            // distance. Keep truth for analysis-only logging, but do not draw
            // longitudinal noise or mark an early distance observation valid.
            sample.observedDistanceToStopM = sample.trueDistanceToStopM;
            sample.longitudinalDistanceValid = false;
        }
        sample.trueCue = readNativeCue(targetMobility);
        sample.observedCue = sampleCue(sample.trueCue);
        sample.knownCueSamples = sample.observedCue == ObservedCue::UNKNOWN ? 0 : 1;
        return sample;
    } catch (...) {
        return sample;
    }
}

int32_t ResDBPerception::ownLateralClaimCm(simtime_t now) const
{
    if (!adjacent_lateral_enabled_ || !mobility_) return 0;
    double originX = 0.0;
    double originY = 0.0;
    double normalX = 0.0;
    double normalY = 0.0;
    double separationM = 0.0;
    try {
        auto ownVehicle = mobility_->getCommandInterface()->vehicle(
            mobility_->getExternalId());
        if (!deriveAdjacentLaneFrame(ownVehicle.getLaneId(), originX, originY,
                normalX, normalY, separationM)) return 0;
    } catch (...) {
        return 0;
    }
    const Coord p = mobility_->getPositionAt(now);
    const double u = (p.x - originX) * normalX + (p.y - originY) * normalY;
    return static_cast<int32_t>(std::llround(u * 100.0));
}

bool ResDBPerception::deriveAdjacentLaneFrame(const std::string& physicalLaneId,
                                               double& originX,
                                               double& originY,
                                               double& normalX,
                                               double& normalY,
                                               double& separationM) const
{
    if (!mobility_ || !mobility_->getCommandInterface() || physicalLaneId.empty() ||
            physicalLaneId.front() == ':') return false;
    const size_t suffix = physicalLaneId.rfind('_');
    if (suffix == std::string::npos) return false;
    const std::string laneBase = physicalLaneId.substr(0, suffix + 1);
    try {
        const auto lane0Shape = mobility_->getCommandInterface()->lane(laneBase + "0").getShape();
        const auto lane1Shape = mobility_->getCommandInterface()->lane(laneBase + "1").getShape();
        if (lane0Shape.empty() || lane1Shape.empty()) return false;
        const Coord lane0 = lane0Shape.front();
        const Coord lane1 = lane1Shape.front();
        const double dx = lane1.x - lane0.x;
        const double dy = lane1.y - lane0.y;
        separationM = std::hypot(dx, dy);
        if (separationM <= 0.0) return false;
        originX = lane0.x;
        originY = lane0.y;
        normalX = dx / separationM;
        normalY = dy / separationM;
        return true;
    } catch (...) {
        return false;
    }
}

int ResDBPerception::projectPhysicalLaneIndex(int32_t lateralCm) const
{
    if (!adjacent_lateral_enabled_) return 0;
    const double boundaryCm = adjacent_lane_separation_m_ * 50.0;
    if (std::abs(static_cast<double>(lateralCm) - boundaryCm) < 0.5) return -1;
    return static_cast<double>(lateralCm) < boundaryCm ? 0 : 1;
}

StoppedDistancePerceptionSample ResDBPerception::observeStoppedDistance(
    const std::string& targetCarId, simtime_t now, double stationarySpeedMps) const
{
    StoppedDistancePerceptionSample sample;
    sample.observedAt = now;
    if (!mobility_ || !mobility_->getManager()) return sample;
    const auto& managedHosts = mobility_->getManager()->getManagedHosts();
    auto hostIt = managedHosts.find(targetCarId);
    if (hostIt == managedHosts.end()) return sample;
    TraCIMobility* targetMobility =
        FindModule<TraCIMobility*>::findSubModule(hostIt->second);
    if (!targetMobility) return sample;

    try {
        auto targetVehicle = targetMobility->getCommandInterface()->vehicle(targetCarId);
        const std::string laneId = targetVehicle.getLaneId();
        sample.detected = true;
        sample.stationary = targetVehicle.getSpeed() <= stationarySpeedMps;
        if (laneId.empty() || laneId.front() == ':') return sample;
        const double laneLength = targetMobility->getCommandInterface()
            ->lane(laneId).getLength();
        sample.trueDistanceToStopM =
            std::max(0.0, laneLength - targetVehicle.getLanePosition());
        sample.observedDistanceToStopM = std::max(
            0.0, sample.trueDistanceToStopM
                + sampleGaussian(longitudinal_observation_sigma_m_));
        sample.distanceValid = true;
    } catch (...) {
    }
    return sample;
}

ConflictBoxPerceptionSample ResDBPerception::observeConflictBoxOccupancy(
    const std::string& targetCarId, simtime_t now) const
{
    auto sample = measureConflictBoxTruth(targetCarId, now);
    if (!sample.valid) return sample;
    sample.observedSignedMarginM = sample.trueSignedMarginM +
        sampleGaussian(longitudinal_observation_sigma_m_);
    sample.observedOccupied = sample.observedSignedMarginM >= 0.0;
    return sample;
}

ConflictBoxPerceptionSample ResDBPerception::measureConflictBoxTruth(
    const std::string& targetCarId, simtime_t now) const
{
    ConflictBoxPerceptionSample sample;
    sample.observedAt = now;
    if (!mobility_ || !mobility_->getManager()) return sample;
    const auto& managedHosts = mobility_->getManager()->getManagedHosts();
    auto hostIt = managedHosts.find(targetCarId);
    if (hostIt == managedHosts.end()) return sample;
    TraCIMobility* targetMobility =
        FindModule<TraCIMobility*>::findSubModule(hostIt->second);
    if (!targetMobility) return sample;

    try {
        auto targetVehicle = targetMobility->getCommandInterface()->vehicle(targetCarId);
        const std::string laneId = targetVehicle.getLaneId();
        const std::string roadId = targetVehicle.getRoadId();
        if (laneId.empty()) return sample;
        sample.detected = true;

        const double lanePosition = targetVehicle.getLanePosition();
        const double laneLength = targetMobility->getCommandInterface()
            ->lane(laneId).getLength();
        if (!std::isfinite(lanePosition) || !std::isfinite(laneLength) ||
                laneLength <= 0.0) return sample;

        if (laneId.front() == ':') {
            sample.trueOccupied = true;
            sample.trueSignedMarginM =
                std::max(0.0, std::min(lanePosition, laneLength - lanePosition));
        } else if (roadId.size() >= 2 && roadId[0] == 'C' && roadId[1] == '2') {
            // Outbound: distance past the internal-lane exit.
            sample.trueOccupied = false;
            sample.trueSignedMarginM = -std::max(0.0, lanePosition);
        } else if (roadId.size() >= 2 &&
                roadId[roadId.size() - 2] == '2' && roadId.back() == 'C') {
            // Inbound: distance remaining to the internal-lane entrance.
            sample.trueOccupied = false;
            sample.trueSignedMarginM =
                -std::max(0.0, laneLength - lanePosition);
        } else {
            return sample;
        }

        sample.observedSignedMarginM = sample.trueSignedMarginM;
        sample.observedOccupied = sample.trueOccupied;
        sample.valid = true;
    } catch (...) {
    }
    return sample;
}

ConflictBoxPerceptionSample ResDBPerception::observeAnyConflictBoxOccupancy(
    simtime_t now) const
{
    ConflictBoxPerceptionSample aggregate;
    aggregate.observedAt = now;
    if (!mobility_ || !mobility_->getManager()) return aggregate;

    bool found = false;
    for (const auto& host : mobility_->getManager()->getManagedHosts()) {
        const auto candidate = measureConflictBoxTruth(host.first, now);
        if (!candidate.valid) continue;
        if (!found || candidate.trueSignedMarginM > aggregate.trueSignedMarginM) {
            aggregate = candidate;
            found = true;
        }
    }
    if (!found) return aggregate;

    // One box-level observation per witness/tick.  Applying independent noise
    // to every queued vehicle and OR-ing the results would make the false-
    // occupied rate grow artificially with traffic count.
    aggregate.trueOccupied = aggregate.trueSignedMarginM >= 0.0;
    aggregate.observedSignedMarginM = aggregate.trueSignedMarginM +
        sampleGaussian(longitudinal_observation_sigma_m_);
    aggregate.observedOccupied = aggregate.observedSignedMarginM >= 0.0;
    aggregate.valid = true;
    return aggregate;
}

double ResDBPerception::sampleGaussian(double sigma) const
{
    if (sigma == 0.0) return 0.0;
    if (!rng_) return 0.0;
    ++random_draw_count_;
    return omnetpp::normal(rng_, 0.0, sigma);
}

int ResDBPerception::approachIndex(char approach)
{
    switch (approach) {
    case 'N': return 0;
    case 'S': return 1;
    case 'E': return 2;
    case 'W': return 3;
    default: return -1;
    }
}

char ResDBPerception::approachChar(int index)
{
    static const char kApproaches[] = {'N', 'S', 'E', 'W'};
    return index >= 0 && index < 4 ? kApproaches[index] : '?';
}

char ResDBPerception::sampleApproach(char truth) const
{
    const int row = approachIndex(truth);
    if (row < 0) return '?';
    bool identity = true;
    for (int col = 0; col < 4; ++col) {
        const double expected = col == row ? 1.0 : 0.0;
        if (std::abs(matrix_[row * 4 + col] - expected) > 1e-12) identity = false;
    }
    if (approach_sigma_m_ == 0.0 && identity) return truth;
    if (!rng_) return '?';
    const double draw = rng_->doubleRand();
    ++random_draw_count_;
    double cumulative = 0.0;
    for (int col = 0; col < 4; ++col) {
        cumulative += matrix_[row * 4 + col];
        if (draw <= cumulative || col == 3) return approachChar(col);
    }
    return '?';
}

ObservedCue ResDBPerception::sampleCue(ObservedCue truth) const
{
    if (truth == ObservedCue::UNKNOWN || signal_error_ == 0.0) return truth;
    if (!rng_) return ObservedCue::UNKNOWN;
    const double corruptDraw = rng_->doubleRand();
    ++random_draw_count_;
    if (corruptDraw >= signal_error_) return truth;
    ObservedCue alternatives[2];
    int n = 0;
    for (ObservedCue cue : {ObservedCue::STRAIGHT, ObservedCue::LEFT, ObservedCue::RIGHT})
        if (cue != truth) alternatives[n++] = cue;
    const ObservedCue selected = alternatives[rng_->intRand(2)];
    ++random_draw_count_;
    return selected;
}

ObservedCue ResDBPerception::cueFromCode(uint8_t code)
{
    return code <= static_cast<uint8_t>(ObservedCue::UNKNOWN)
        ? static_cast<ObservedCue>(code) : ObservedCue::UNKNOWN;
}

const char* ResDBPerception::cueName(ObservedCue cue)
{
    switch (cue) {
    case ObservedCue::STRAIGHT: return "S";
    case ObservedCue::LEFT: return "L";
    case ObservedCue::RIGHT: return "R";
    default: return "UNKNOWN";
    }
}

} // namespace veins
