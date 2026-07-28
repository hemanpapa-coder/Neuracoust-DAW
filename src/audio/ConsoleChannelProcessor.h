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
        // Target coefficients; process() ramps the live ones toward these per sample so a
        // gain/freq change slides instead of stepping at the block boundary (no zipper).
        float tb0 = 1, tb1 = 0, tb2 = 0, ta1 = 0, ta2 = 0;
        bool primed = false;
        float process(float x);
        void set(float nb0, float nb1, float nb2, float na1, float na2);
        void peak(double sr, float hz, float q, float gainDb);
        void shelf(double sr, float hz, float gainDb, bool high);
        void highPass(double sr, float hz);
        void lowPass(double sr, float hz);
        void clear() { z1 = z2 = 0; }
    };
    std::array<std::array<Biquad, 6>, 2> eq_;
    std::array<float, 2> compDetector_ {0, 0};
    std::array<float, 2> gateDetector_ {0, 0};
    std::array<float, 2> compGainDb_ {0, 0};
    std::array<float, 2> gateGainDb_ {0, 0};
    std::array<int, 2> gateHold_ {0, 0};
    double sampleRate_ = 0;
};

/// The harmonic spectrum the saturator currently adds (harmonics 2..count+1), each normalised
/// 0..1 over a −60..0 dB window relative to the fundamental. Computed from the same saturate()
/// math the processor uses, so the display matches what is heard; zero-filled when disabled.
void consoleSaturatorHarmonics(const ConsoleChannelState& parameters, float* out, int count);

/// The console strip as a flat parameter list, for sending a channel to a remote DSP node.
///
/// Values are NORMALISED 0..1, because that is what the NART wire carries — it clamps every
/// parameter to that range, so a raw 8000 Hz would arrive as 1.0. Each index therefore has a
/// range, and both ends go through this one mapping: the DAW packs with
/// consoleChannelParameterValues() and the node unpacks with applyConsoleChannelParameter().
/// Numbering and ranges physically cannot drift apart, so a strip sounds the same either side.
struct ConsoleChannelParameter {
    int index = 0;
    float normalized = 0.0f;
};
std::vector<ConsoleChannelParameter> consoleChannelParameterValues(const ConsoleChannelState& parameters);
void applyConsoleChannelParameter(ConsoleChannelState& parameters, int index, float normalized);

} // namespace neuracoust::daw
