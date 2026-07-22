// Pins the denoiser's band-split DSP (neuracoust_denoiser_dsp.h) — the only part that must be correct
// independent of the neural model. Needs no LibTorch, so it always builds and runs in ctest.
#include "neuracoust_denoiser_dsp.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace neuracoust::denoiser;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    else std::printf("ok: %s\n", what);
}

std::vector<float> sine(double hz, double rate, int n) {
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = std::sin(2.0 * M_PI * hz * i / rate);
    return v;
}

// Correlation of the middle 80% (avoid the resampler's edge taper) after amplitude-normalizing.
double midCorr(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    const size_t lo = n / 10, hi = n - n / 10;
    double sa = 0, sb = 0, sab = 0;
    for (size_t i = lo; i < hi; ++i) { sa += a[i] * a[i]; sb += b[i] * b[i]; sab += a[i] * b[i]; }
    return (sa > 1e-12 && sb > 1e-12) ? sab / std::sqrt(sa * sb) : 0.0;
}
double rms(const std::vector<float>& v) {
    double s = 0; for (float x : v) s += x * x; return v.empty() ? 0 : std::sqrt(s / v.size());
}
}  // namespace

int main() {
    const double sr = 44100.0;
    const int n = 44100;  // 1 s

    // 1) A 1 kHz tone survives a 44.1k → 16k → 44.1k round trip (it is well below the 8 kHz cutoff).
    {
        auto x = sine(1000.0, sr, n);
        auto down = resampleSinc(x, sr, 16000.0);
        auto back = resampleSinc(down, 16000.0, sr);
        check(midCorr(x, back) > 0.99, "1kHz survives 44.1k->16k->44.1k round trip (corr>0.99)");
        check(std::abs(rms(back) / rms(x) - 1.0) < 0.05, "1kHz round-trip amplitude preserved (±5%)");
    }

    // 2) A 12 kHz tone (above the 8 kHz cutoff) is strongly attenuated by the anti-alias downsample —
    //    and does NOT alias back as a strong low tone. Round-trip RMS should collapse.
    {
        auto x = sine(12000.0, sr, n);
        auto down = resampleSinc(x, sr, 16000.0);
        auto back = resampleSinc(down, 16000.0, sr);
        check(rms(back) / rms(x) < 0.15, "12kHz tone attenuated >16 dB by anti-alias downsample");
    }

    // 3) combineBand at mix=0 reproduces the original EXACTLY (identity), whatever the delta.
    {
        auto x = sine(500.0, sr, n);
        auto delta16 = sine(300.0, 16000.0, 16000);          // arbitrary non-zero change
        auto out = combineBand(x, delta16, sr, 16000.0, 0.0f);
        bool identical = out.size() == x.size();
        for (size_t i = 0; identical && i < x.size(); ++i) identical = (out[i] == x[i]);
        check(identical, "combineBand mix=0 is exact identity");
    }

    // 4) combineBand at mix>0 changes the signal, stays the original length, and stays finite.
    {
        auto x = sine(500.0, sr, n);
        auto delta16 = sine(300.0, 16000.0, 16000);
        auto out = combineBand(x, delta16, sr, 16000.0, 1.0f);
        bool finite = out.size() == x.size();
        double diff = 0;
        for (size_t i = 0; finite && i < out.size(); ++i) { finite = std::isfinite(out[i]); diff += std::abs(out[i] - x[i]); }
        check(finite, "combineBand mix=1 output finite and length-preserving");
        check(diff > 1.0, "combineBand mix=1 actually changed the signal");
    }

    std::printf(failures ? "\n%d FAILURES\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
