#pragma once

#include <array>
#include <vector>

namespace neuracoust::daw {

// A nonlinear waveshaper that reproduces an audio interface's MEASURED harmonic character
// (the "2단계 고조파" model). It is a Chebyshev-polynomial transfer function: feeding a sinusoid
// cos θ through Tₙ yields cos(nθ) — the n-th harmonic — so f(x) = x + Σ cₙ·Tₙ(x) synthesizes H2..H7
// at exactly the measured linear amplitudes. Magnitude only (phase-exact synthesis is out of scope
// and inaudible at real interface depths). Level-matched: with all cₙ = 0 it is a pure pass-through
// (null test passes), and a clean interface's tiny coefficients leave the sound essentially
// unchanged — which is itself the correct measured result.
class AudioInterfaceModeler {
public:
    // Build the transfer LUT from measured harmonic coefficients [c2..c7] (linear). Empty/all-zero
    // → identity (bypass). `mix` (0..1) scales the added harmonics for the optional-use control.
    void configure(const std::vector<double>& harmonics, double mix = 1.0);
    void clear();
    bool active() const { return active_; }

    // Shape an interleaved stereo block in place.
    void processInterleavedStereo(float* interleaved, int frames);

private:
    static constexpr int kLutSize = 2048;   // transfer table over x ∈ [-1, 1]
    std::array<float, kLutSize> lut_{};
    bool active_ = false;

    float shape(float x) const;
};

} // namespace neuracoust::daw
