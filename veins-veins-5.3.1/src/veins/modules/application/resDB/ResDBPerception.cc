#include "veins/modules/application/resDB/ResDBPerception.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

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

ObservedCue readCue(TraCIMobility* target)
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

} // namespace

void ResDBPerception::configure(TraCIMobility* mobility,
                                cRNG* rng,
                                const std::string& matrixSpec,
                                double approachSigmaM,
                                double signalError)
{
    mobility_ = mobility;
    rng_ = rng;
    approach_sigma_m_ = approachSigmaM;
    signal_error_ = signalError;
    random_draw_count_ = 0;
    if (approach_sigma_m_ < 0.0)
        throw cRuntimeError("approachSigmaM must be non-negative");
    if (signal_error_ < 0.0 || signal_error_ > 1.0)
        throw cRuntimeError("signalObservationError must be in [0,1]");

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
        sample.trueCue = readCue(targetMobility);
        sample.observedCue = sampleCue(sample.trueCue);
        sample.knownCueSamples = sample.observedCue == ObservedCue::UNKNOWN ? 0 : 1;
        return sample;
    } catch (...) {
        return sample;
    }
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
