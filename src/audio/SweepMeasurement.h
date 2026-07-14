#pragma once

#include <vector>

namespace neuracoust::daw {

// Roadmap ②: acoustic measurement by exponential sine sweep (ESS) + deconvolution.
// Play generateLogSweep() out a monitor, capture it back through the mic, and
// deconvolveSweep() recovers the system's impulse response (amp + cable + speaker + room).
// impulseResponseMagnitudeDb() turns that into the frequency-response curve the correction
// and virtual-monitor maths consume. Self-contained (its own FFT), so it is unit-testable.
struct SweepParams {
    double sampleRate = 48000.0;
    double startHz = 20.0;
    double endHz = 20000.0;
    double durationSeconds = 3.0;
    double amplitude = 0.5;   // peak of the emitted sweep, 0..1
};

/// The exponential sine sweep to emit. Length ≈ durationSeconds · sampleRate.
std::vector<float> generateLogSweep(const SweepParams& params);

/// Recover the system impulse response from a mono recording of the emitted sweep.
/// Regularized frequency-domain deconvolution — robust where the sweep has little energy.
std::vector<float> deconvolveSweep(const std::vector<float>& recorded, const SweepParams& params);

/// Log-spaced magnitude response (dB) of an impulse response, [minHz, maxHz].
std::vector<double> impulseResponseMagnitudeDb(const std::vector<float>& impulseResponse,
                                               double sampleRate, int points,
                                               double minHz, double maxHz);

} // namespace neuracoust::daw
