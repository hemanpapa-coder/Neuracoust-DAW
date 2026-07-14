#pragma once

#include <cstddef>
#include <vector>

namespace neuracoust::daw {

enum class EqBandType {
    Peaking,
    LowShelf,
    HighShelf,
    HighPass,
    LowPass,
    Notch,
};

/// One band of the monitor parametric EQ. A project starts with none; the user adds them one
/// frequency at a time (up to kMaxBands). A disabled band costs nothing — configure() only
/// builds biquads for the enabled ones, so an empty/idle EQ is free.
struct EqBandSpec {
    bool enabled = true;
    EqBandType type = EqBandType::Peaking;
    double frequencyHz = 1000.0;
    double gainDb = 0.0;   // peaking / shelves only
    double q = 1.0;        // peaking / notch / pass filters; shelf slope for shelves
};

/// A minimum-phase (IIR biquad) parametric EQ for the monitor path. Double-precision
/// coefficients and state (Direct Form II Transposed) for a clean, drift-free response even
/// with many stacked bands. Linear-phase (FIR) is a later mode; this is the low-latency core.
class ParametricEq {
public:
    static constexpr std::size_t kMaxBands = 64;

    /// Rebuild the filter chain from the enabled bands. Preserves nothing — call reset() to
    /// also clear the running state (e.g. on a sample-rate change or a seek).
    void configure(double sampleRate, const std::vector<EqBandSpec>& bands);
    void reset();

    /// True if at least one band is active (so the caller can skip the whole EQ otherwise).
    bool active() const { return !biquads_.empty(); }
    std::size_t activeBandCount() const { return biquads_.size(); }

    void processInterleavedStereo(float* interleaved, int frameCount);

    /// Combined magnitude response in dB at a frequency — for the UI curve, tests, and the
    /// room-correction/virtual-monitor maths built on top.
    double magnitudeDb(double frequencyHz) const;

private:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;   // normalized (a0 == 1)
        double zL1 = 0.0, zL2 = 0.0, zR1 = 0.0, zR2 = 0.0;         // per-channel state
    };
    static Biquad design(const EqBandSpec& band, double sampleRate);

    double sampleRate_ = 48000.0;
    std::vector<Biquad> biquads_;   // one per enabled band, applied in series
};

} // namespace neuracoust::daw
