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
static float tailChannelPeak(const std::vector<float>& audio, size_t channel) {
    float p = 0;
    for (size_t i = audio.size() / 2 + channel; i < audio.size(); i += 2)
        p = std::max(p, std::abs(audio[i]));
    return p;
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

    // Stereo-link follows the louder side; dual mono leaves a quiet opposite
    // channel substantially less compressed.
    std::vector<float> asymmetric = tone;
    for (size_t i = 1; i < asymmetric.size(); i += 2) asymmetric[i] *= 0.08f;
    processor.reset(sr);
    auto linkedCompressed = asymmetric;
    processor.processInterleavedStereo(linkedCompressed, comp, sr);
    processor.reset(sr);
    auto dualComp = comp; dualComp.dualMono = true;   // channel-level dual (comp + gate detectors)
    auto dualCompressed = asymmetric;
    processor.processInterleavedStereo(dualCompressed, dualComp, sr);
    assert(tailChannelPeak(dualCompressed, 1) > tailChannelPeak(linkedCompressed, 1) * 1.5f);

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

    // The wire format a channel travels over to a remote DSP node. The NART protocol clamps every
    // parameter to 0..1, so raw Hz and dB have to be normalised and put back — and both ends of
    // the wire share this one mapping. A silent mismatch here would not fail anything; it would
    // just make a remote strip sound different from the local one, which is the whole promise.
    ConsoleChannelState packed;
    packed.filterEnabled = true;
    packed.highPassEnabled = true;
    packed.highPassHz = 82.0f;
    packed.lowPassEnabled = true;
    packed.lowPassHz = 14500.0f;
    packed.eqEnabled = true;
    packed.eqHfGainDb = -4.5f;
    packed.eqHfHz = 11000.0f;
    packed.eqHmfGainDb = 3.25f;
    packed.eqHmfHz = 2400.0f;
    packed.eqHmfQ = 2.5f;
    packed.eqLmfGainDb = -2.0f;
    packed.eqLmfHz = 640.0f;
    packed.eqLmfQ = 0.8f;
    packed.eqLfGainDb = 5.0f;
    packed.eqLfHz = 90.0f;
    packed.compEnabled = true;
    packed.compThresholdDb = -22.5f;
    packed.compRatio = 6.0f;
    packed.compAttackMs = 12.0f;
    packed.compReleaseMs = 480.0f;
    packed.compMix = 0.75f;
    packed.gateEnabled = true;
    packed.gateThresholdDb = -44.0f;
    packed.gateRangeDb = 32.0f;
    packed.gateAttackMs = 2.5f;
    packed.gateReleaseMs = 250.0f;
    packed.saturatorEnabled = true;
    packed.saturatorDriveDb = 9.0f;
    packed.saturatorMix = 0.6f;
    packed.expanderMode = false;
    packed.compFastAttack = true;
    packed.compCircuitMode = true;
    packed.channelBiasSeed = 137;
    packed.channelBiasDepth = 0.4f;

    ConsoleChannelState unpacked;
    for (const auto& parameter : consoleChannelParameterValues(packed)) {
        // Everything on the wire must already be normalised — an out-of-range value would be
        // clamped in transit and arrive as something else entirely.
        assert(parameter.normalized >= 0.0f && parameter.normalized <= 1.0f);
        applyConsoleChannelParameter(unpacked, parameter.index, parameter.normalized);
    }
    const auto close = [](float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; };
    assert(unpacked.filterEnabled && unpacked.highPassEnabled && unpacked.lowPassEnabled);
    assert(close(unpacked.highPassHz, packed.highPassHz, 0.1f));
    assert(close(unpacked.lowPassHz, packed.lowPassHz, 1.0f));
    assert(unpacked.eqEnabled);
    assert(close(unpacked.eqHfGainDb, packed.eqHfGainDb, 0.01f));
    assert(close(unpacked.eqHfHz, packed.eqHfHz, 1.0f));
    assert(close(unpacked.eqHmfHz, packed.eqHmfHz, 0.5f));
    assert(close(unpacked.eqHmfQ, packed.eqHmfQ, 0.01f));
    assert(close(unpacked.eqLmfHz, packed.eqLmfHz, 0.2f));
    assert(close(unpacked.eqLfHz, packed.eqLfHz, 0.1f));
    assert(unpacked.compEnabled && unpacked.compFastAttack && unpacked.compCircuitMode);
    assert(close(unpacked.compThresholdDb, packed.compThresholdDb, 0.01f));
    assert(close(unpacked.compRatio, packed.compRatio, 0.01f));
    assert(close(unpacked.compAttackMs, packed.compAttackMs, 0.01f));
    assert(close(unpacked.compReleaseMs, packed.compReleaseMs, 0.5f));
    assert(close(unpacked.compMix, packed.compMix, 0.001f));
    assert(unpacked.gateEnabled && !unpacked.expanderMode);
    assert(close(unpacked.gateThresholdDb, packed.gateThresholdDb, 0.01f));
    assert(close(unpacked.gateRangeDb, packed.gateRangeDb, 0.01f));
    assert(close(unpacked.gateAttackMs, packed.gateAttackMs, 0.01f));
    assert(unpacked.saturatorEnabled);
    assert(close(unpacked.saturatorDriveDb, packed.saturatorDriveDb, 0.01f));
    assert(close(unpacked.saturatorMix, packed.saturatorMix, 0.001f));
    assert(unpacked.channelBiasSeed == packed.channelBiasSeed);
    assert(close(unpacked.channelBiasDepth, packed.channelBiasDepth, 0.001f));

    // And the round trip must be audibly identical, not merely close on paper.
    ConsoleChannelProcessor local, remoteSide;
    local.reset(sr);
    remoteSide.reset(sr);
    auto localOut = tone;
    auto remoteOut = tone;
    local.processInterleavedStereo(localOut, packed, sr);
    remoteSide.processInterleavedStereo(remoteOut, unpacked, sr);
    float worst = 0.0f;
    for (size_t i = localOut.size() / 2; i < localOut.size(); ++i) {
        worst = std::max(worst, std::abs(localOut[i] - remoteOut[i]));
    }
    assert(worst < 1e-4f);
}
