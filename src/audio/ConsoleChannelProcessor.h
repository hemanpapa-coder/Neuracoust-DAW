#pragma once

#include "core/DawState.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace neuracoust::daw {

/// Stateful, host-independent built-in console processor. Model dispatch is
/// kept here so future console models can share the DAW/project/automation glue.
class ConsoleChannelProcessor {
public:
    void reset(double sampleRate);
    void processInterleavedStereo(std::vector<float>& audio,
                                  const ConsoleChannelState& parameters,
                                  double sampleRate);
    float compressorGainReductionDb() const {
        return std::max({0.0f, -compGainDb_[0], -compGainDb_[1]});
    }
    float gateGainReductionDb() const {
        return std::max({0.0f, -gateGainDb_[0], -gateGainDb_[1]});
    }

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process(float x);
        void peak(double sr, float hz, float q, float gainDb);
        void shelf(double sr, float hz, float gainDb, bool high);
        void highPass(double sr, float hz);
        void lowPass(double sr, float hz);
        void clear() { z1 = z2 = 0; }
    };
    std::array<std::array<Biquad, 6>, 2> eq_;
    // Smoothed copies of the coefficient-driving EQ/filter params, so a knob move
    // ramps the biquad coefficients instead of jumping them (which zippers/clicks).
    struct SmoothParams {
        bool init = false;
        float hpHz = 0, lpHz = 0, hfHz = 0, hfG = 0, hmfHz = 0, hmfQ = 0, hmfG = 0,
              lmfHz = 0, lmfQ = 0, lmfG = 0, lfHz = 0, lfG = 0;
    };
    SmoothParams sp_;
    std::array<float, 2> compDetector_ {0, 0};
    std::array<float, 2> gateDetector_ {0, 0};
    std::array<float, 2> compGainDb_ {0, 0};
    std::array<float, 2> gateGainDb_ {0, 0};
    std::array<int, 2> gateHold_ {0, 0};
    double sampleRate_ = 0;
};

} // namespace neuracoust::daw
