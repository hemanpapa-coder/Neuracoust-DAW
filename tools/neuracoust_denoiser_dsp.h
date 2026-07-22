// Pure-DSP helpers for the neural denoiser helper, factored out of the torch executable so they can
// be unit-tested without LibTorch. The denoiser model runs at 16 kHz; DW keeps full bandwidth with a
// band split that is EXACT by construction:
//
//     out = orig + mix * upsample( model(down(orig)) - down(orig) )
//
// The bracket is the denoiser's *change*, computed at 16 kHz, hence band-limited to 8 kHz — so
// upsampling it adds no alias images, and mix=0 reproduces the original sample-for-sample regardless of
// resampler quality. The only DSP that must be correct is the anti-aliasing resampler below, which is
// why it lives here with a test (DenoiserDspTest).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace neuracoust {
namespace denoiser {

// Arbitrary-ratio windowed-sinc resampler (Hann-windowed sinc, ~2*halfTaps taps). Anti-aliases when
// downsampling (cutoff at the LOWER rate's Nyquist) and interpolates when upsampling. Each output is
// normalized to unity DC gain, which also handles the signal edges gracefully. Offline quality, O(N*taps).
inline std::vector<float> resampleSinc(const std::vector<float>& in, double srcRate, double dstRate,
                                       int halfTaps = 24) {
    if (in.empty() || srcRate <= 0.0 || dstRate <= 0.0) return {};
    if (std::abs(srcRate - dstRate) < 1e-6) return in;

    const double ratio = dstRate / srcRate;
    const int64_t inLen = static_cast<int64_t>(in.size());
    const int64_t outLen = std::max<int64_t>(1, static_cast<int64_t>(std::llround(inLen * ratio)));
    // Anti-alias cutoff = Nyquist of the lower of the two rates, expressed in input cycles/sample.
    const double fcNorm = std::min(srcRate, dstRate) / 2.0 / srcRate;   // <= 0.5
    const int taps = 2 * halfTaps;

    std::vector<float> out(static_cast<size_t>(outLen), 0.0f);
    for (int64_t i = 0; i < outLen; ++i) {
        const double center = static_cast<double>(i) / ratio;   // position in the input, in samples
        const int64_t first = static_cast<int64_t>(std::floor(center)) - halfTaps + 1;
        double acc = 0.0, wsum = 0.0;
        for (int j = 0; j < taps; ++j) {
            const int64_t n = first + j;
            if (n < 0 || n >= inLen) continue;
            const double x = center - static_cast<double>(n);            // distance in input samples
            const double a = 2.0 * fcNorm * x;
            const double sinc = std::abs(a) < 1e-9 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            // Hann window across the tap span, centered on `center`.
            const double win = 0.5 * (1.0 - std::cos(2.0 * M_PI * (j + 0.5) / taps));
            const double h = sinc * win;
            acc += static_cast<double>(in[static_cast<size_t>(n)]) * h;
            wsum += h;
        }
        out[static_cast<size_t>(i)] = wsum > 1e-9 ? static_cast<float>(acc / wsum) : 0.0f;
    }
    return out;
}

// Reconstruct the full-band output from the original and the denoiser's 16 kHz change.
//   delta16 = clean16 - low16   (same length, at 16 kHz)
// Upsample delta16 to the source rate and add: out = orig + mix * deltaFull. Length is clamped to orig.
inline std::vector<float> combineBand(const std::vector<float>& orig,
                                      const std::vector<float>& delta16,
                                      double srcRate, double modelRate, float mix) {
    std::vector<float> out = orig;
    if (mix <= 0.0f || delta16.empty()) return out;   // mix 0 = exact original
    std::vector<float> deltaFull = resampleSinc(delta16, modelRate, srcRate);
    const size_t n = std::min(out.size(), deltaFull.size());
    for (size_t i = 0; i < n; ++i) out[i] += mix * deltaFull[i];
    return out;
}

}  // namespace denoiser
}  // namespace neuracoust
