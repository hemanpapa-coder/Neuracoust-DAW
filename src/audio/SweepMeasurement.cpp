#include "audio/SweepMeasurement.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace neuracoust::daw {

namespace {
constexpr double kPi = 3.14159265358979323846;
using Complex = std::complex<double>;

std::size_t nextPow2(std::size_t n) {
    std::size_t p = 1;
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

std::vector<Complex> forwardFft(const std::vector<float>& x, std::size_t n) {
    std::vector<Complex> a(n, Complex(0.0, 0.0));
    for (std::size_t i = 0; i < x.size() && i < n; ++i) a[i] = Complex(x[i], 0.0);
    fftRadix2(a, -1);
    return a;
}
} // namespace

std::vector<float> generateLogSweep(const SweepParams& params) {
    const double sr = std::max(1.0, params.sampleRate);
    const double f1 = std::max(1.0, std::min(params.startHz, sr * 0.49));
    const double f2 = std::max(f1 + 1.0, std::min(params.endHz, sr * 0.5));
    const double T = std::max(0.05, params.durationSeconds);
    const double amp = std::max(0.0, std::min(1.0, params.amplitude));
    const std::size_t n = static_cast<std::size_t>(std::llround(T * sr));
    std::vector<float> out(n, 0.0f);
    // Farina ESS: instantaneous phase sweeps exponentially from f1 to f2.
    const double K = T * 2.0 * kPi * f1 / std::log(f2 / f1);
    const double L = T / std::log(f2 / f1);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sr;
        out[i] = static_cast<float>(amp * std::sin(K * (std::exp(t / L) - 1.0)));
    }
    // Short raised-cosine fades so the emitted sweep itself never clicks.
    const std::size_t fade = std::min<std::size_t>(n / 8, static_cast<std::size_t>(sr * 0.01));
    for (std::size_t i = 0; i < fade; ++i) {
        const double w = 0.5 * (1.0 - std::cos(kPi * static_cast<double>(i) / fade));
        out[i] *= static_cast<float>(w);
        out[n - 1 - i] *= static_cast<float>(w);
    }
    return out;
}

std::vector<float> deconvolveSweep(const std::vector<float>& recorded, const SweepParams& params) {
    const auto sweep = generateLogSweep(params);
    if (recorded.empty() || sweep.empty()) return {};
    const std::size_t n = nextPow2(recorded.size() + sweep.size());
    const auto X = forwardFft(sweep, n);     // excitation
    const auto Y = forwardFft(recorded, n);  // response

    // Regularized deconvolution: H = conj(X)·Y / (|X|² + ε). ε from the excitation's peak
    // energy keeps it stable where the sweep has little energy (out of band).
    double maxMag2 = 0.0;
    for (const auto& x : X) maxMag2 = std::max(maxMag2, std::norm(x));
    const double eps = std::max(1e-20, maxMag2 * 1e-6);
    std::vector<Complex> H(n);
    for (std::size_t k = 0; k < n; ++k) {
        H[k] = std::conj(X[k]) * Y[k] / (std::norm(X[k]) + eps);
    }
    fftRadix2(H, +1);   // inverse
    std::vector<float> ir(n);
    const double scale = 1.0 / static_cast<double>(n);
    for (std::size_t k = 0; k < n; ++k) ir[k] = static_cast<float>(H[k].real() * scale);
    return ir;
}

std::vector<double> impulseResponseMagnitudeDb(const std::vector<float>& impulseResponse,
                                               double sampleRate, int points,
                                               double minHz, double maxHz) {
    std::vector<double> out(std::max(0, points), 0.0);
    if (impulseResponse.empty() || points <= 0) return out;
    const double sr = std::max(1.0, sampleRate);
    const std::size_t n = nextPow2(impulseResponse.size());
    const auto H = forwardFft(impulseResponse, n);
    const double lo = std::max(1.0, minHz);
    const double hi = std::max(lo + 1.0, std::min(maxHz, sr * 0.5));
    const double ratio = std::log(hi / lo);
    for (int i = 0; i < points; ++i) {
        const double f = lo * std::exp(ratio * (points > 1 ? static_cast<double>(i) / (points - 1) : 0.0));
        const double binF = f / sr * static_cast<double>(n);
        const auto k = static_cast<std::size_t>(std::llround(binF));
        const double mag = (k < n) ? std::abs(H[k]) : 0.0;
        out[i] = 20.0 * std::log10(std::max(1e-9, mag));
    }
    return out;
}

HarmonicSeparation separateHarmonics(const std::vector<float>& recorded,
                                     const SweepParams& params, int maxHarmonic) {
    HarmonicSeparation result;
    if (recorded.empty() || maxHarmonic < 2) return result;
    const std::vector<float> ir = deconvolveSweep(recorded, params);
    if (ir.empty()) return result;
    const std::ptrdiff_t N = static_cast<std::ptrdiff_t>(ir.size());
    const double sr = std::max(1.0, params.sampleRate);
    const double f1 = std::max(1.0, std::min(params.startHz, sr * 0.49));
    const double f2 = std::max(f1 + 1.0, std::min(params.endHz, sr * 0.5));
    const double L = std::max(0.05, params.durationSeconds) / std::log(f2 / f1);

    // Locate the LINEAR impulse: the largest peak in the causal half. Each harmonic order
    // lives in the wrapped tail, Δt_k = L·ln(k) seconds BEFORE this peak, so restricting the
    // search to the first half never mistakes a harmonic response for the fundamental.
    std::ptrdiff_t linIdx = 0;
    double linPeak = 0.0;
    const std::ptrdiff_t causalLimit = N / 2;
    for (std::ptrdiff_t i = 0; i < causalLimit; ++i) {
        const double a = std::abs(static_cast<double>(ir[static_cast<std::size_t>(i)]));
        if (a > linPeak) { linPeak = a; linIdx = i; }
    }
    if (linPeak < 1e-9) return result;   // no usable fundamental — treat as unmeasured

    // Window radius: comfortably under the tightest harmonic spacing (smallest at the top
    // order, Δt_{k+1}-Δt_k = L·ln(1+1/k)), capped at 10 ms so windows never overlap.
    const double minSpacing = L * std::log(1.0 + 1.0 / static_cast<double>(maxHarmonic));
    const auto radius = static_cast<std::ptrdiff_t>(
        std::max(4.0, std::min(minSpacing * 0.4, 0.010) * sr));

    // Each order's separated response is a near-delta at its Δt_k, so the peak in the window
    // (not the summed energy, which over-counts the deconvolution's time spread) is the honest
    // amplitude. Peak ratio harmonic/fundamental = the coefficient directly, no calibration.
    auto windowPeak = [&](std::ptrdiff_t center) {
        double peak = 0.0;
        for (std::ptrdiff_t d = -radius; d <= radius; ++d) {
            const std::ptrdiff_t idx = ((center + d) % N + N) % N;   // circular
            peak = std::max(peak, std::abs(static_cast<double>(ir[static_cast<std::size_t>(idx)])));
        }
        return peak;
    };

    const double linRef = windowPeak(linIdx);
    if (linRef < 1e-9) return result;

    double sumSq = 0.0;
    result.coefficients.reserve(static_cast<std::size_t>(maxHarmonic - 1));
    for (int k = 2; k <= maxHarmonic; ++k) {
        const double dt = L * std::log(static_cast<double>(k));   // advance before the linear IR
        const std::ptrdiff_t shift = static_cast<std::ptrdiff_t>(std::llround(dt * sr));
        const std::ptrdiff_t center = ((linIdx - shift) % N + N) % N;
        const double ratio = std::min(1.0, windowPeak(center) / linRef);
        result.coefficients.push_back(ratio);
        sumSq += ratio * ratio;
    }
    result.thdPercent = std::sqrt(sumSq) * 100.0;
    result.valid = true;
    return result;
}

} // namespace neuracoust::daw
