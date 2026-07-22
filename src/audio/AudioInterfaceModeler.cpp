#include "audio/AudioInterfaceModeler.h"

#include <algorithm>
#include <cmath>

namespace neuracoust::daw {

namespace {
// Chebyshev polynomial Tₙ(x) via the stable recurrence T₀=1, T₁=x, Tₙ=2x·Tₙ₋₁−Tₙ₋₂.
double chebyshev(int n, double x) {
    if (n == 0) return 1.0;
    if (n == 1) return x;
    double tm2 = 1.0, tm1 = x, t = x;
    for (int k = 2; k <= n; ++k) {
        t = 2.0 * x * tm1 - tm2;
        tm2 = tm1;
        tm1 = t;
    }
    return t;
}
} // namespace

void AudioInterfaceModeler::configure(const std::vector<double>& harmonics, double mix) {
    mix = std::clamp(mix, 0.0, 1.0);
    bool any = false;
    for (double c : harmonics) any = any || (c != 0.0);
    if (!any || mix <= 0.0) { clear(); return; }

    // f(x) = x + Σ_{n=2}^{7} (mix·cₙ)·Tₙ(x). cₙ = harmonics[n-2].
    for (int i = 0; i < kLutSize; ++i) {
        const double x = -1.0 + 2.0 * static_cast<double>(i) / (kLutSize - 1);
        double y = x;
        for (size_t k = 0; k < harmonics.size() && (k + 2) <= 7; ++k) {
            const double c = mix * harmonics[k];
            if (c != 0.0) y += c * chebyshev(static_cast<int>(k) + 2, x);
        }
        lut_[static_cast<size_t>(i)] = static_cast<float>(y);
    }
    active_ = true;
}

void AudioInterfaceModeler::clear() {
    active_ = false;
}

float AudioInterfaceModeler::shape(float x) const {
    // Clamp into the table domain, then linearly interpolate the transfer LUT.
    const float xc = std::clamp(x, -1.0f, 1.0f);
    const float pos = (xc + 1.0f) * 0.5f * (kLutSize - 1);
    const int i0 = static_cast<int>(pos);
    const int i1 = std::min(i0 + 1, kLutSize - 1);
    const float frac = pos - static_cast<float>(i0);
    return lut_[static_cast<size_t>(i0)] * (1.0f - frac) + lut_[static_cast<size_t>(i1)] * frac;
}

void AudioInterfaceModeler::processInterleavedStereo(float* interleaved, int frames) {
    if (!active_ || interleaved == nullptr || frames <= 0) return;
    for (int i = 0; i < frames; ++i) {
        interleaved[i * 2] = shape(interleaved[i * 2]);
        interleaved[i * 2 + 1] = shape(interleaved[i * 2 + 1]);
    }
}

} // namespace neuracoust::daw
