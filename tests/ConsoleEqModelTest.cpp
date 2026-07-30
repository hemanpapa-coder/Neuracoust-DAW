// Pins the per-model EQ CURVE behaviour of the console strip: the same knob position must
// produce a family-true response, not the same biquad for every nameplate.
//
//   - API (proportional Q): a +12 dB bell is NARROWER than Neve's at the same dialled Q.
//   - Neve (inductor shelf): boosting the LF shelf dips the region above the corner.
//   - SSL G (shelf overshoot): same signature, from the published G-series curve.
//   - Neuracoust NC: delivers exactly the dialled gain at the bell centre.
//
// Gains are measured by pushing a sine through the real processor and comparing RMS in/out —
// the response that is heard, not the coefficients that were intended.

#include "audio/ConsoleChannelProcessor.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace neuracoust::daw;

namespace {

constexpr double kSampleRate = 48000.0;

// Gain in dB the strip applies to a sine at `hz`, settled (coefficients ramp per sample, so the
// first half is warm-up and only the tail is measured).
double measuredGainDb(const ConsoleChannelState& parameters, double hz) {
    ConsoleChannelProcessor processor;
    const int frames = 48000;
    std::vector<float> audio(static_cast<size_t>(frames) * 2u);
    for (int i = 0; i < frames; ++i) {
        const float sample = 0.1f * std::sin(2.0 * M_PI * hz * i / kSampleRate);
        audio[static_cast<size_t>(i) * 2u] = sample;
        audio[static_cast<size_t>(i) * 2u + 1u] = sample;
    }
    // Feed in blocks the way the renderer does; identical maths, but keeps the test honest
    // about the per-block coefficient update path.
    std::vector<float> processed;
    processed.reserve(audio.size());
    for (size_t start = 0; start < audio.size(); start += 512) {
        std::vector<float> block(audio.begin() + start,
                                 audio.begin() + std::min(audio.size(), start + 512));
        processor.processInterleavedStereo(block, parameters, kSampleRate);
        processed.insert(processed.end(), block.begin(), block.end());
    }
    double dryEnergy = 0.0;
    double wetEnergy = 0.0;
    for (size_t i = processed.size() / 2; i < processed.size(); ++i) {
        dryEnergy += static_cast<double>(audio[i]) * audio[i];
        wetEnergy += static_cast<double>(processed[i]) * processed[i];
    }
    if (dryEnergy <= 0.0 || wetEnergy <= 0.0) return -160.0;
    return 10.0 * std::log10(wetEnergy / dryEnergy);
}

ConsoleChannelState bellBoost(const std::string& model) {
    ConsoleChannelState p;
    p.model = model;
    p.channelBiasDepth = 0.0f;   // no per-strip drift in a measurement
    p.eqEnabled = true;
    p.eqEMode = true;            // Q exactly as dialled, so the family shaping is what differs
    p.eqHmfHz = 1000.0f;
    p.eqHmfQ = 1.0f;
    p.eqHmfGainDb = 12.0f;
    return p;
}

ConsoleChannelState lfShelfBoost(const std::string& model) {
    ConsoleChannelState p;
    p.model = model;
    p.channelBiasDepth = 0.0f;
    p.eqEnabled = true;
    p.eqLfBell = false;          // shelf — where the overshoot/inductor behaviour lives
    p.eqLfHz = 100.0f;
    p.eqLfGainDb = 12.0f;
    return p;
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

} // namespace

int main() {
    // Proportional Q: at +12 dB, API's bell must be clearly narrower than Neve's — less gain
    // one half-octave off centre — while both still deliver the boost at the centre.
    const double apiCenter = measuredGainDb(bellBoost("API Vision"), 1000.0);
    const double apiOff = measuredGainDb(bellBoost("API Vision"), 1500.0);
    const double neveCenter = measuredGainDb(bellBoost("Neve 8078"), 1000.0);
    const double neveOff = measuredGainDb(bellBoost("Neve 8078"), 1500.0);
    std::printf("bell +12 @1k: API centre %.2f off %.2f | Neve centre %.2f off %.2f\n",
                apiCenter, apiOff, neveCenter, neveOff);
    check(apiCenter > 10.0, "API bell delivers its boost at the centre");
    check(neveCenter > 10.0, "Neve bell delivers its boost at the centre");
    check(neveOff > apiOff + 1.5, "Neve bell is clearly wider than API's (proportional Q)");

    // NC: what the knob says is what the sine gets.
    const double ncCenter = measuredGainDb(bellBoost("Neuracoust NC"), 1000.0);
    std::printf("bell +12 @1k: NC centre %.2f\n", ncCenter);
    check(std::abs(ncCenter - 12.0) < 1.0, "NC bell centre is exactly the dialled gain");

    // Shelf companions: boosting the LF shelf on SSL G / Neve must dip the region above the
    // corner relative to the SSL E baseline (overshoot / inductor dip); NC must not.
    const double eAbove = measuredGainDb(lfShelfBoost("SSL 4000E"), 250.0);
    const double gAbove = measuredGainDb(lfShelfBoost("SSL 4000G"), 250.0);
    const double neveAbove = measuredGainDb(lfShelfBoost("Neve 8078"), 250.0);
    const double ncAbove = measuredGainDb(lfShelfBoost("Neuracoust NC"), 250.0);
    std::printf("LF shelf +12 @100, read at 250: E %.2f G %.2f Neve %.2f NC %.2f\n",
                eAbove, gAbove, neveAbove, ncAbove);
    check(gAbove < eAbove - 1.0, "SSL G shelf overshoots (dip above the corner)");
    check(neveAbove < eAbove - 0.7, "Neve inductor shelf dips above the corner");
    check(std::abs(ncAbove - eAbove) < 0.3, "NC shelf stays textbook (no companion)");

    // The shelf still boosts BELOW the corner for every family — the dip must not eat the shelf.
    const double gBelow = measuredGainDb(lfShelfBoost("SSL 4000G"), 60.0);
    const double neveBelow = measuredGainDb(lfShelfBoost("Neve 8078"), 60.0);
    std::printf("LF shelf +12 @100, read at 60: G %.2f Neve %.2f\n", gBelow, neveBelow);
    check(gBelow > 8.0, "SSL G shelf body survives its overshoot");
    check(neveBelow > 8.0, "Neve shelf body survives its inductor dip");

    // API 525A CEILING: threshold down AND make-up up by the same dB — more gain reduction,
    // (near-)unchanged output level. That coupling is the whole point of the knob.
    {
        ConsoleChannelState p;
        p.compType = "API 525A";
        p.channelBiasDepth = 0.0f;
        p.compEnabled = true;
        // The neutrality holds when the signal already sits ABOVE the threshold in limit (20:1)
        // mode — the hardware's own claim ("maintains a constant output ceiling"). The test tone
        // is a 0.1-amp sine (−23 dB RMS), so −26 puts it 3 dB over.
        p.compThresholdDb = -26.0f;
        p.compRatio = 20.0f;
        p.compMix = 1.0f;
        const double flat = measuredGainDb(p, 1000.0);
        p.compCeilingDb = 12.0f;
        const double ceiling = measuredGainDb(p, 1000.0);
        std::printf("525A ceiling: 0 -> %.2f dB, 12 -> %.2f dB\n", flat, ceiling);
        check(std::abs(ceiling - flat) < 3.0, "CEILING compresses without moving the output level");
        p.compCeilingDb = 0.0f;
        p.compMakeupDb = 6.0f;
        const double madeUp = measuredGainDb(p, 1000.0);
        std::printf("525A make-up: +6 -> %.2f dB (vs %.2f)\n", madeUp, flat);
        check(madeUp > flat + 4.0, "MAKE-UP raises the wet path");
    }

    if (failures == 0) std::printf("console EQ model curves: all pinned\n");
    return failures == 0 ? 0 : 1;
}
