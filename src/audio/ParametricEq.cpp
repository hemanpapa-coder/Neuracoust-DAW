#include "audio/ParametricEq.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace neuracoust::daw {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Solve a 3x3 linear system by Gaussian elimination with partial pivoting. Returns false if
// singular.
bool solve3x3(double A[3][3], const double b[3], double x[3]) {
    double m[3][4];
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) m[r][c] = A[r][c]; m[r][3] = b[r]; }
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r) if (std::abs(m[r][col]) > std::abs(m[piv][col])) piv = r;
        if (std::abs(m[piv][col]) < 1e-18) return false;
        for (int c = 0; c < 4; ++c) std::swap(m[col][c], m[piv][c]);
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double f = m[r][col] / m[col][col];
            for (int c = 0; c < 4; ++c) m[r][c] -= f * m[col][c];
        }
    }
    for (int r = 0; r < 3; ++r) x[r] = m[r][3] / m[r][r];
    return true;
}

// Matched-Z peaking (bell) biquad after Vicanek, "Matched Second Order Digital Filters" (2016).
// The RBJ cookbook peak cramps near Nyquist — its magnitude droops/rings versus the analog
// prototype, which is what made the fitted monitor EQ spike in the top octave. Here the poles are
// matched to the analog resonance and the numerator is fitted so the digital |H|² tracks the
// analog bell all the way to Nyquist. Returns false (caller falls back to RBJ) if the recovery is
// ill-conditioned. bOut/aOut are the biquad coefficients with a0 == 1.
bool designMatchedPeaking(double w0, double Q, double gainDb, double bOut[3], double aOut[3]) {
    const double G = std::pow(10.0, gainDb / 20.0);   // linear gain at the peak
    if (std::abs(gainDb) < 1e-6) { bOut[0] = 1; bOut[1] = 0; bOut[2] = 0; aOut[0] = 1; aOut[1] = 0; aOut[2] = 0; return true; }
    // RBJ is already accurate below ~0.16·Nyquist and the matched-Z numerator recovery is
    // ill-conditioned there (near-DC double pole), so only take over up top where RBJ cramps.
    if (w0 < 0.5) return false;
    const double qp = 1.0 / (2.0 * std::max(0.05, Q));
    const double a2 = std::exp(-2.0 * qp * w0);
    double a1;
    if (qp <= 1.0) a1 = -2.0 * std::exp(-qp * w0) * std::cos(w0 * std::sqrt(1.0 - qp * qp));
    else           a1 = -2.0 * std::exp(-qp * w0) * std::cosh(w0 * std::sqrt(qp * qp - 1.0));

    // Least-squares fit N(φ) = c0 + c1 φ + c2 φ² (φ = sin²(ω/2)) to the analog target
    // |H_a(jω)|²·|D(e^jω)|² across ω ∈ [0, π].
    const int M = 96;
    double S[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    double T[3] = {0, 0, 0};
    for (int i = 0; i < M; ++i) {
        const double w = kPi * static_cast<double>(i) / (M - 1);
        const double sh = std::sin(w / 2.0);
        const double phi = sh * sh;
        const double d = (w0 * w0 - w * w);
        const double numA = d * d + std::pow(G * w0 * w / std::max(0.05, Q), 2.0);
        const double denA = d * d + std::pow(w0 * w / std::max(0.05, Q), 2.0);
        const double Ha2 = denA > 1e-30 ? numA / denA : G * G;
        const double dr = 1.0 + a1 * std::cos(w) + a2 * std::cos(2.0 * w);
        const double di = -(a1 * std::sin(w) + a2 * std::sin(2.0 * w));
        const double D2 = dr * dr + di * di;
        const double Nt = Ha2 * D2;
        const double basis[3] = {1.0, phi, phi * phi};
        for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) S[r][c] += basis[r] * basis[c]; T[r] += basis[r] * Nt; }
    }
    double c[3];
    if (!solve3x3(S, T, c)) return false;
    // Recover b0,b1,b2 from c0=(b0+b1+b2)², c1=-4(b0b1+4b0b2+b1b2), c2=16 b0 b2.
    if (c[0] <= 0.0) return false;
    const double W = std::sqrt(c[0]);         // b0+b1+b2
    const double p = c[2] / 16.0;             // b0·b2
    const double K = -c[1] / 4.0 - 4.0 * p;   // b1·(b0+b2)
    const double sDisc = W * W - 4.0 * K;
    if (sDisc < 0.0) return false;
    const double s = 0.5 * (W + std::sqrt(sDisc));   // b0+b2
    const double bDisc = s * s - 4.0 * p;
    if (bDisc < 0.0) return false;
    const double root = std::sqrt(bDisc);
    const double b0 = 0.5 * (s + root);
    const double b2 = 0.5 * (s - root);
    const double b1 = W - s;
    if (!std::isfinite(b0) || !std::isfinite(b1) || !std::isfinite(b2)) return false;
    bOut[0] = b0; bOut[1] = b1; bOut[2] = b2;
    aOut[0] = 1.0; aOut[1] = a1; aOut[2] = a2;
    // Post-verify against the two anchors: ~0 dB at DC and ~gainDb at the centre. If the recovery
    // drifted (a bad sign choice or conditioning), reject so the caller keeps the safe RBJ design.
    auto magDb = [&](double w) {
        const double br = b0 + b1 * std::cos(w) + b2 * std::cos(2.0 * w);
        const double bi = -(b1 * std::sin(w) + b2 * std::sin(2.0 * w));
        const double ar = 1.0 + a1 * std::cos(w) + a2 * std::cos(2.0 * w);
        const double ai = -(a1 * std::sin(w) + a2 * std::sin(2.0 * w));
        const double denom = ar * ar + ai * ai;
        return 20.0 * std::log10(std::sqrt(std::max(1e-18, (br * br + bi * bi) / std::max(1e-18, denom))));
    };
    if (std::abs(magDb(0.0)) > 1.0 || std::abs(magDb(w0) - gainDb) > 2.0) return false;
    return true;
}
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
    case EqBandType::Peaking: {
        // Matched-Z peaking keeps the magnitude accurate to Nyquist (no RBJ cramping). Fall back
        // to the RBJ cookbook if the coefficient recovery is ill-conditioned.
        double bm[3], am[3];
        if (designMatchedPeaking(w0, q, band.gainDb, bm, am)) {
            bq.b0 = bm[0]; bq.b1 = bm[1]; bq.b2 = bm[2];
            bq.a1 = am[1]; bq.a2 = am[2];
            return bq;
        }
        b0 = 1.0 + alpha * A;   b1 = -2.0 * cosw0;      b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;   a1 = -2.0 * cosw0;      a2 = 1.0 - alpha / A;
        break;
    }
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
