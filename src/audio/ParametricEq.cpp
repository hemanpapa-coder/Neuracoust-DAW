#include "audio/ParametricEq.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace neuracoust::daw {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// RBJ Audio EQ Cookbook coefficients, normalized so a0 == 1. Double precision throughout.
ParametricEq::Biquad ParametricEq::design(const EqBandSpec& band, double sampleRate) {
    Biquad bq;
    const double sr = std::max(1.0, sampleRate);
    // Clamp the frequency below Nyquist so a band parked at the top can't blow up.
    const double f = std::clamp(band.frequencyHz, 1.0, sr * 0.49);
    const double q = std::max(0.05, band.q);
    const double w0 = 2.0 * kPi * f / sr;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    const double A = std::pow(10.0, band.gainDb / 40.0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (band.type) {
    case EqBandType::Peaking:
        b0 = 1.0 + alpha * A;   b1 = -2.0 * cosw0;      b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;   a1 = -2.0 * cosw0;      a2 = 1.0 - alpha / A;
        break;
    case EqBandType::LowShelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =      A * ((A + 1.0) - (A - 1.0) * cosw0 + s);
        b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
        b2 =      A * ((A + 1.0) - (A - 1.0) * cosw0 - s);
        a0 =           (A + 1.0) + (A - 1.0) * cosw0 + s;
        a1 = -2.0 *    ((A - 1.0) + (A + 1.0) * cosw0);
        a2 =           (A + 1.0) + (A - 1.0) * cosw0 - s;
        break;
    }
    case EqBandType::HighShelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =      A * ((A + 1.0) + (A - 1.0) * cosw0 + s);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
        b2 =      A * ((A + 1.0) + (A - 1.0) * cosw0 - s);
        a0 =           (A + 1.0) - (A - 1.0) * cosw0 + s;
        a1 =  2.0 *    ((A - 1.0) - (A + 1.0) * cosw0);
        a2 =           (A + 1.0) - (A - 1.0) * cosw0 - s;
        break;
    }
    case EqBandType::HighPass:
        b0 =  (1.0 + cosw0) / 2.0;   b1 = -(1.0 + cosw0);   b2 = (1.0 + cosw0) / 2.0;
        a0 =   1.0 + alpha;          a1 = -2.0 * cosw0;     a2 = 1.0 - alpha;
        break;
    case EqBandType::LowPass:
        b0 =  (1.0 - cosw0) / 2.0;   b1 =  1.0 - cosw0;     b2 = (1.0 - cosw0) / 2.0;
        a0 =   1.0 + alpha;          a1 = -2.0 * cosw0;     a2 = 1.0 - alpha;
        break;
    case EqBandType::Notch:
        b0 =  1.0;            b1 = -2.0 * cosw0;   b2 = 1.0;
        a0 =  1.0 + alpha;    a1 = -2.0 * cosw0;   a2 = 1.0 - alpha;
        break;
    }

    const double inv = (std::abs(a0) > 1e-20) ? 1.0 / a0 : 1.0;
    bq.b0 = b0 * inv; bq.b1 = b1 * inv; bq.b2 = b2 * inv;
    bq.a1 = a1 * inv; bq.a2 = a2 * inv;
    return bq;
}

void ParametricEq::configure(double sampleRate, const std::vector<EqBandSpec>& bands) {
    sampleRate_ = std::max(1.0, sampleRate);
    std::vector<Biquad> next;
    next.reserve(std::min(bands.size(), kMaxBands));
    for (const auto& band : bands) {
        if (next.size() >= kMaxBands) break;
        if (!band.enabled) continue;
        // A 0 dB peaking/shelf band is a no-op; skip it so it costs nothing.
        const bool gainBand = band.type == EqBandType::Peaking ||
                              band.type == EqBandType::LowShelf ||
                              band.type == EqBandType::HighShelf;
        if (gainBand && std::abs(band.gainDb) < 1e-4) continue;
        next.push_back(design(band, sampleRate_));
    }
    // Carry running state across a reconfigure where the band count is unchanged, so a knob
    // turn doesn't click; otherwise start clean.
    if (next.size() == biquads_.size()) {
        for (std::size_t i = 0; i < next.size(); ++i) {
            next[i].zL1 = biquads_[i].zL1; next[i].zL2 = biquads_[i].zL2;
            next[i].zR1 = biquads_[i].zR1; next[i].zR2 = biquads_[i].zR2;
        }
    }
    biquads_ = std::move(next);
}

void ParametricEq::reset() {
    for (auto& bq : biquads_) {
        bq.zL1 = bq.zL2 = bq.zR1 = bq.zR2 = 0.0;
    }
}

void ParametricEq::processInterleavedStereo(float* interleaved, int frameCount) {
    if (interleaved == nullptr || frameCount <= 0 || biquads_.empty()) {
        return;
    }
    for (int frame = 0; frame < frameCount; ++frame) {
        double l = static_cast<double>(interleaved[frame * 2]);
        double r = static_cast<double>(interleaved[frame * 2 + 1]);
        for (auto& bq : biquads_) {
            // Direct Form II Transposed, per channel.
            const double yL = bq.b0 * l + bq.zL1;
            bq.zL1 = bq.b1 * l - bq.a1 * yL + bq.zL2;
            bq.zL2 = bq.b2 * l - bq.a2 * yL;
            l = yL;
            const double yR = bq.b0 * r + bq.zR1;
            bq.zR1 = bq.b1 * r - bq.a1 * yR + bq.zR2;
            bq.zR2 = bq.b2 * r - bq.a2 * yR;
            r = yR;
        }
        interleaved[frame * 2] = static_cast<float>(l);
        interleaved[frame * 2 + 1] = static_cast<float>(r);
    }
}

double ParametricEq::magnitudeDb(double frequencyHz) const {
    if (biquads_.empty()) return 0.0;
    const double w = 2.0 * kPi * std::clamp(frequencyHz, 0.0, sampleRate_ * 0.5) / sampleRate_;
    const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -w));
    const std::complex<double> z2 = z1 * z1;
    double totalDb = 0.0;
    for (const auto& bq : biquads_) {
        const std::complex<double> num = bq.b0 + bq.b1 * z1 + bq.b2 * z2;
        const std::complex<double> den = 1.0 + bq.a1 * z1 + bq.a2 * z2;
        const double mag = std::abs(num) / std::max(1e-20, std::abs(den));
        totalDb += 20.0 * std::log10(std::max(1e-12, mag));
    }
    return totalDb;
}

} // namespace neuracoust::daw
