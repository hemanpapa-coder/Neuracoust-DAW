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

// Roadmap ②c: harmonic separation, the real reason to use an ESS over an MLS. Deconvolving
// the recorded sweep places the LINEAR impulse response at t≈0 and each harmonic-order
// response at a fixed time advance Δt_k = L·ln(k) (L = T/ln(f2/f1)) before it — so a single
// sweep yields both the frequency response AND the harmonic-distortion coefficients. The
// broadband amplitude ratio harmonic-k / fundamental is exactly the [c2..c7] the Chebyshev
// AudioInterfaceModeler consumes. (Farina 2000.)
struct HarmonicSeparation {
    std::vector<double> coefficients;   // [c2, c3, …] linear amplitude ratios vs the fundamental
    double thdPercent = 0.0;            // total harmonic distortion, % (sqrt(Σ cₖ²)·100)
    bool valid = false;                 // false when the fundamental is too weak to trust
};

/// Separate the harmonic orders 2..maxHarmonic from a mono recording of the emitted sweep.
/// Returns broadband amplitude ratios (linear), null-safe: a clean linear system gives ~0.
HarmonicSeparation separateHarmonics(const std::vector<float>& recorded,
                                     const SweepParams& params, int maxHarmonic = 7);

} // namespace neuracoust::daw
