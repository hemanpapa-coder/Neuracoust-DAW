#pragma once

#include "audio/MonitorCorrection.h"   // ResponseCurve

#include <vector>

namespace neuracoust::daw {

// A linear-phase FIR monitor EQ. Its magnitude response is designed to match an arbitrary target
// curve (dB vs Hz) exactly across the WHOLE band — steep low-frequency rolloffs and treble dips
// alike — which is where the cascaded biquad fit rippled or cramped. The price is a fixed latency
// of numTaps/2 samples (the linear-phase group delay), reported so the engine can compensate.
//
// design*() runs offline (on a context change); process*() runs on the audio thread with no
// allocation. Not internally locked — the engine swaps a freshly designed instance under its own
// monitor lock, the same way it swaps the biquad EQ and the DSP processor.
class MonitorFirEq {
public:
    // Design a linear-phase FIR whose magnitude follows curveDb at sampleRate. numTaps is rounded
    // up to a power of two. A flat/empty curve clears to bypass. Safe to call repeatedly.
    void designFromCurve(double sampleRate, const ResponseCurve& curveDb, int numTaps);
    // Bypass (identity, zero latency).
    void clear();

    bool active() const { return !taps_.empty(); }
    int numTaps() const { return static_cast<int>(taps_.size()); }
    int latencySamples() const { return latency_; }

    // Process interleaved L/R in place. No-op when inactive.
    void processInterleavedStereo(float* interleaved, int frames);

    // Magnitude response (dB) at a frequency, for the UI curve. Returns 0 when inactive.
    double magnitudeDb(double frequencyHz, double sampleRate) const;

private:
    std::vector<float> taps_;      // symmetric, time order h[0..N-1]
    std::vector<float> bufL_;      // doubled ring (2N) so the window is contiguous
    std::vector<float> bufR_;
    int write_ = 0;
    int latency_ = 0;
};

} // namespace neuracoust::daw
