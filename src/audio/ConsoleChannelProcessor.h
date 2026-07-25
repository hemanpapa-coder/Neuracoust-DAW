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
    float compressorGainReductionDb() const { return std::max(0.0f, -compGainDb_); }
    float gateGainReductionDb() const { return std::max(0.0f, -gateGainDb_); }

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
    float compDetector_ = 0, gateDetector_ = 0;
    float compGainDb_ = 0, gateGainDb_ = 0;
    int gateHold_ = 0;
    double sampleRate_ = 0;
};

} // namespace neuracoust::daw
