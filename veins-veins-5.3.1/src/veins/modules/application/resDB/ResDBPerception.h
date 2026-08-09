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
};

class ResDBPerception {
public:
    void configure(TraCIMobility* mobility,
                   omnetpp::cRNG* rng,
                   const std::string& matrixSpec,
                   double approachSigmaM,
                   double signalError);

    ArrivalPerceptionSample observeArrival(const std::string& targetCarId,
                                            omnetpp::simtime_t now) const;

    uint64_t randomDrawCount() const { return random_draw_count_; }

    static ObservedCue cueFromCode(uint8_t code);
    static const char* cueName(ObservedCue cue);

private:
    static int approachIndex(char approach);
    static char approachChar(int index);
    char sampleApproach(char truth) const;
    ObservedCue sampleCue(ObservedCue truth) const;

    TraCIMobility* mobility_ = nullptr;
    omnetpp::cRNG* rng_ = nullptr;
    std::array<double, 16> matrix_{};
    double approach_sigma_m_ = 0.0;
    double signal_error_ = 0.0;
    mutable uint64_t random_draw_count_ = 0;
};

} // namespace veins
