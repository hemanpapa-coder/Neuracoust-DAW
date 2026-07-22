#include "audio/MonitorFirEq.h"

#include <algorithm>
#include <cmath>
#include <complex>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>   // vDSP_dotpr — vectorised N-tap dot product on the render thread
#endif

namespace neuracoust::daw {

namespace {
constexpr double kPi = 3.14159265358979323846;
using Complex = std::complex<double>;

int pow2AtLeast(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// Iterative radix-2 Cooley-Tukey FFT (in place). sign = -1 forward, +1 inverse (unscaled).
void fftRadix2(std::vector<Complex>& a, int sign) {
    const std::size_t n = a.size();
    if (n < 2) return;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = sign * 2.0 * kPi / static_cast<double>(len);
        const Complex wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const Complex u = a[i + k];
                const Complex v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}
} // namespace

void MonitorFirEq::clear() {
    taps_.clear();
    bufL_.clear();
    bufR_.clear();
    write_ = 0;
    latency_ = 0;
}

void MonitorFirEq::designFromCurve(double sampleRate, const ResponseCurve& curveDb, int numTaps) {
    if (curveDb.empty() || numTaps < 4) { clear(); return; }
    const int N = pow2AtLeast(numTaps);
    const double sr = std::max(1.0, sampleRate);

    // Frequency-sample the desired magnitude with a linear-phase term (delay = N/2 → the phase
    // factor is exp(-jπk) = (-1)^k, which keeps the impulse real and symmetric). If the target is
    // already ~flat, bypass rather than spend a convolution doing nothing.
    double maxAbs = 0.0;
    std::vector<Complex> spec(N);
    for (int k = 0; k <= N / 2; ++k) {
        const double f = static_cast<double>(k) * sr / N;
        const double db = interpolateCurveDb(curveDb, f);
        maxAbs = std::max(maxAbs, std::abs(db));
        const double mag = std::pow(10.0, db / 20.0);
        const double sign = (k & 1) ? -1.0 : 1.0;
        spec[k] = Complex(mag * sign, 0.0);
    }
    if (maxAbs < 0.05) { clear(); return; }
    for (int k = N / 2 + 1; k < N; ++k) spec[k] = std::conj(spec[N - k]);

    fftRadix2(spec, +1);   // inverse, unscaled

    std::vector<float> h(N);
    for (int n = 0; n < N; ++n) h[n] = static_cast<float>(spec[n].real() / N);
    // Hann window centred on the N/2 delay — tames the frequency-sampling (Gibbs) ripple while
    // keeping the response steep enough to follow a bass rolloff.
    for (int n = 0; n < N; ++n) {
        const double wnd = 0.5 - 0.5 * std::cos(2.0 * kPi * n / (N - 1));
        h[n] *= static_cast<float>(wnd);
    }

    taps_ = h;
    bufL_.assign(static_cast<std::size_t>(2 * N), 0.0f);
    bufR_.assign(static_cast<std::size_t>(2 * N), 0.0f);
    write_ = 0;
    latency_ = N / 2;
}

void MonitorFirEq::processInterleavedStereo(float* interleaved, int frames) {
    if (taps_.empty() || interleaved == nullptr || frames <= 0) return;
    const int N = static_cast<int>(taps_.size());
    const float* h = taps_.data();
    // Doubled ring: writing the newest sample at both write_ and write_+N means the last N samples
    // sit contiguously at [write_ .. write_+N-1] in newest-first order (buf[write_+k] == x[n-k]),
    // so y[n] = Σ_k h[k]·x[n-k] is a single contiguous dot product. write_ decrements each sample.
    for (int f = 0; f < frames; ++f) {
        const float xl = interleaved[2 * f];
        const float xr = interleaved[2 * f + 1];
        bufL_[write_] = xl; bufL_[write_ + N] = xl;
        bufR_[write_] = xr; bufR_[write_ + N] = xr;
        const float* segL = &bufL_[write_];
        const float* segR = &bufR_[write_];
#if defined(__APPLE__)
        // Accelerate runs the N-tap dot product on NEON SIMD — ~30× the scalar double loop, which at
        // 4096 taps × stereo × 48 kHz otherwise pegs a core and starves the render (100% DSP load →
        // dropouts, exactly what the linear-phase monitor EQ showed). Same math, float accumulate
        // (inaudible for a monitor EQ), same latency.
        float yl = 0.0f, yr = 0.0f;
        vDSP_dotpr(h, 1, segL, 1, &yl, static_cast<vDSP_Length>(N));
        vDSP_dotpr(h, 1, segR, 1, &yr, static_cast<vDSP_Length>(N));
        interleaved[2 * f] = yl;
        interleaved[2 * f + 1] = yr;
#else
        double yl = 0.0, yr = 0.0;
        for (int k = 0; k < N; ++k) {
            yl += static_cast<double>(h[k]) * segL[k];
            yr += static_cast<double>(h[k]) * segR[k];
        }
        interleaved[2 * f] = static_cast<float>(yl);
        interleaved[2 * f + 1] = static_cast<float>(yr);
#endif
        if (--write_ < 0) write_ = N - 1;
    }
}

double MonitorFirEq::magnitudeDb(double frequencyHz, double sampleRate) const {
    if (taps_.empty()) return 0.0;
    const double sr = std::max(1.0, sampleRate);
    const double w = 2.0 * kPi * frequencyHz / sr;
    double re = 0.0, im = 0.0;
    for (int n = 0; n < static_cast<int>(taps_.size()); ++n) {
        re += taps_[n] * std::cos(-w * n);
        im += taps_[n] * std::sin(-w * n);
    }
    const double mag = std::sqrt(re * re + im * im);
    return 20.0 * std::log10(std::max(1e-9, mag));
}

} // namespace neuracoust::daw
