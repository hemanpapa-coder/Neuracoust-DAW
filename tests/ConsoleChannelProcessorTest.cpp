#include "audio/ConsoleChannelProcessor.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

using namespace neuracoust::daw;

static float peak(const std::vector<float>& audio) {
    float p = 0; for (float x : audio) p = std::max(p, std::abs(x)); return p;
}
static float tailPeak(const std::vector<float>& audio) {
    float p = 0; for (size_t i = audio.size() / 2; i < audio.size(); ++i) p = std::max(p, std::abs(audio[i])); return p;
}

int main() {
    constexpr double sr = 48000.0;
    std::vector<float> tone(48000 * 2);
    for (size_t f = 0; f < tone.size() / 2; ++f) {
        const float x = 0.25f * std::sin(2.0 * 3.141592653589793 * 8000.0 * double(f) / sr);
        tone[f * 2] = tone[f * 2 + 1] = x;
    }
    ConsoleChannelProcessor processor;
    ConsoleChannelState eq;
    eq.eqEnabled = true; eq.eqHfGainDb = -12.0f; eq.eqHfHz = 8000.0f;
    auto filtered = tone; processor.processInterleavedStereo(filtered, eq, sr);
    assert(tailPeak(filtered) < peak(tone) * 0.65f);

    processor.reset(sr);
    auto bellEq = eq;
    bellEq.eqHfBell = true;
    auto bellFiltered = tone;
    processor.processInterleavedStereo(bellFiltered, bellEq, sr);
    assert(std::abs(tailPeak(bellFiltered) - tailPeak(filtered)) > 0.001f);

    processor.reset(sr);
    ConsoleChannelState cuts;
    cuts.filterEnabled = true; cuts.lowPassEnabled = true; cuts.lowPassHz = 3000.0f;
    auto cut = tone; processor.processInterleavedStereo(cut, cuts, sr);
    assert(tailPeak(cut) < peak(tone) * 0.15f);

    processor.reset(sr);
    ConsoleChannelState comp;
    comp.compEnabled = true; comp.compThresholdDb = -24; comp.compRatio = 10;
    auto compressed = tone; processor.processInterleavedStereo(compressed, comp, sr);
    assert(tailPeak(compressed) < peak(tone) * 0.8f);
    assert(processor.compressorGainReductionDb() > 1.0f);

    processor.reset(sr);
    ConsoleChannelState dryMix = comp;
    dryMix.compMix = 0.0f;
    auto parallelDry = tone;
    processor.processInterleavedStereo(parallelDry, dryMix, sr);
    assert(std::abs(tailPeak(parallelDry) - peak(tone)) < 0.001f);

    processor.reset(sr);
    ConsoleChannelState fastPeak = comp;
    fastPeak.compFastAttack = true;
    fastPeak.compPeakMode = true;
    auto fastPeakCompressed = tone;
    processor.processInterleavedStereo(fastPeakCompressed, fastPeak, sr);
    assert(processor.compressorGainReductionDb() > 1.0f);

    processor.reset(sr);
    ConsoleChannelState gate;
    gate.gateEnabled = true; gate.expanderMode = false;
    gate.gateThresholdDb = -20; gate.gateRangeDb = 40;
    std::vector<float> quiet(48000 * 2, 0.01f);
    processor.processInterleavedStereo(quiet, gate, sr);
    assert(tailPeak(quiet) < 0.005f);
    assert(processor.gateGainReductionDb() > 1.0f);

    processor.reset(sr);
    gate.gateFastAttack = true;
    auto fastGated = quiet;
    processor.processInterleavedStereo(fastGated, gate, sr);
    assert(processor.gateGainReductionDb() > 1.0f);

    processor.reset(sr);
    ConsoleChannelState sat;
    sat.saturatorEnabled = true;
    sat.saturatorDriveDb = 12.0f;
    auto saturated = tone;
    processor.processInterleavedStereo(saturated, sat, sr);
    assert(std::abs(tailPeak(saturated) - peak(tone)) > 0.01f);

    processor.reset(sr);
    sat.saturatorCircuitMode = true;
    auto circuitSaturated = tone;
    processor.processInterleavedStereo(circuitSaturated, sat, sr);
    float difference = 0.0f;
    for (size_t i = circuitSaturated.size() / 2; i < circuitSaturated.size(); ++i)
        difference += std::abs(circuitSaturated[i] - saturated[i]);
    assert(difference > 1.0f);
}
