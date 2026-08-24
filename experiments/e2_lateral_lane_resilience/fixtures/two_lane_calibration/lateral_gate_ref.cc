// Standalone C++ reference for the ADJACENT_LATERAL residual gate.
// Must stay bit-compatible with ResDBArrivalProtocol.cc lateralMatch and
// ResDBPerception centimetre rounding (std::llround).
//
// Build+run:
//   c++ -std=c++17 -O0 -o lateral_gate_ref lateral_gate_ref.cc
//   ./lateral_gate_ref <obs_m> <claim_m> <sigma_m> <k>
// Prints: observed_cm claimed_cm residual_cm tolerance_cm accept={0|1}

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

static int32_t quantizeCm(double meters) {
    return static_cast<int32_t>(std::llround(meters * 100.0));
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: " << argv[0]
                  << " observed_m claimed_m sigma_m k\n";
        return 2;
    }
    const double observed_m = std::atof(argv[1]);
    const double claimed_m = std::atof(argv[2]);
    const double sigma_m = std::atof(argv[3]);
    const double k = std::atof(argv[4]);

    const int32_t observed_cm = quantizeCm(observed_m);
    const int32_t claimed_cm = quantizeCm(claimed_m);
    const int64_t residual_cm =
        std::llabs(static_cast<int64_t>(observed_cm) -
                   static_cast<int64_t>(claimed_cm));
    const double tolerance_cm = k * sigma_m * 100.0;
    const int accept =
        (static_cast<double>(residual_cm) <= tolerance_cm + 1e-9) ? 1 : 0;

    std::cout << observed_cm << " " << claimed_cm << " " << residual_cm << " "
              << tolerance_cm << " " << accept << "\n";
    return 0;
}
