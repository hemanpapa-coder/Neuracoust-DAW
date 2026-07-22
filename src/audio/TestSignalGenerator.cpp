#include "audio/TestSignalGenerator.h"

#include <algorithm>
#include <cmath>

namespace neuracoust::daw {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// PolyBLEP: band-limits a step discontinuity so saw/square don't alias. t is phase 0..1, dt the
// per-sample phase increment. Returns the correction to ADD/subtract at the discontinuity.
double polyBlep(double t, double dt) {
    if (dt <= 0.0) return 0.0;
    if (t < dt) {
        const double x = t / dt;
        return x + x - x * x - 1.0;
    }
    if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt;
        return x * x + x + x + 1.0;
    }
    return 0.0;
}
}  // namespace

void TestSignalGenerator::reset() {
    phase_ = 0.0;
    sweepPos_ = 0.0;
    triInt_ = 0.0;
    triDc_ = 0.0;
    for (float& s : pink_) s = 0.0f;
    rng_ = 0x2545F491u;
    ramp_ = 0.0f;
}

float TestSignalGenerator::nextWhite() {
    // xorshift32 → uniform [-1, 1). Deterministic (seeded) so tests repeat.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_) / 2147483648.0f - 1.0f;
}

float TestSignalGenerator::nextPink() {
    // Paul Kellett's economy pink filter: white → −3 dB/oct across the audio band.
    const float w = nextWhite();
    pink_[0] = 0.99886f * pink_[0] + w * 0.0555179f;
    pink_[1] = 0.99332f * pink_[1] + w * 0.0750759f;
    pink_[2] = 0.96900f * pink_[2] + w * 0.1538520f;
    pink_[3] = 0.86650f * pink_[3] + w * 0.3104856f;
    pink_[4] = 0.55000f * pink_[4] + w * 0.5329522f;
    pink_[5] = -0.7616f * pink_[5] - w * 0.0168980f;
    const float out = pink_[0] + pink_[1] + pink_[2] + pink_[3] + pink_[4] + pink_[5] + pink_[6] + w * 0.5362f;
    pink_[6] = w * 0.115926f;
    return out * 0.11f;   // ≈ unity peak
}

void TestSignalGenerator::generateInterleavedStereo(float* interleaved, int frames, double sampleRate) {
    if (interleaved == nullptr || frames <= 0) return;
    const double sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
    const TestSignalParams p = params_;
    const double gain = std::pow(10.0, p.levelDb / 20.0);
    const float rampTarget = p.enabled ? 1.0f : 0.0f;
    const float rampStep = static_cast<float>(1.0 / (0.005 * sr));   // ~5 ms click-free fade

    const double freq = std::clamp(p.frequencyHz, 0.0, sr * 0.5);
    const double dt = freq / sr;   // phase increment per sample

    for (int f = 0; f < frames; ++f) {
        double s = 0.0;
        switch (p.waveform) {
        case TestSignalWaveform::Sine:
            s = std::sin(kTwoPi * phase_);
            phase_ += dt;
            break;
        case TestSignalWaveform::Saw:
            s = 2.0 * phase_ - 1.0 - polyBlep(phase_, dt);
            phase_ += dt;
            break;
        case TestSignalWaveform::Square: {
            double sq = phase_ < 0.5 ? 1.0 : -1.0;
            sq += polyBlep(phase_, dt);
            sq -= polyBlep(std::fmod(phase_ + 0.5, 1.0), dt);
            s = sq;
            phase_ += dt;
            break;
        }
        case TestSignalWaveform::Triangle: {
            // Integral of the band-limited square is a triangle; a DC blocker removes the
            // integrator's slow drift, then scale to ±1.
            double sq = phase_ < 0.5 ? 1.0 : -1.0;
            sq += polyBlep(phase_, dt);
            sq -= polyBlep(std::fmod(phase_ + 0.5, 1.0), dt);
            triInt_ += sq * dt;                         // ∫ over half a period ≈ ±0.5
            const double blocked = triInt_ - triDc_;    // one-pole DC blocker
            triDc_ += 0.0008 * blocked;
            s = 4.0 * blocked;                          // 0.5 range → ±1
            phase_ += dt;
            break;
        }
        case TestSignalWaveform::WhiteNoise:
            s = nextWhite();
            break;
        case TestSignalWaveform::PinkNoise:
            s = nextPink();
            break;
        case TestSignalWaveform::Sweep: {
            const double t = sweepPos_ / std::max(1e-6, p.sweepSeconds);   // 0..1
            const double lo = std::max(1.0, std::min(p.sweepStartHz, p.sweepEndHz));
            const double hi = std::max(lo + 1.0, std::max(p.sweepStartHz, p.sweepEndHz));
            const double instHz = p.sweepLogarithmic ? lo * std::pow(hi / lo, t)
                                                      : lo + (hi - lo) * t;
            s = std::sin(kTwoPi * phase_);
            phase_ += std::clamp(instHz, 0.0, sr * 0.5) / sr;
            sweepPos_ += 1.0 / sr;
            if (sweepPos_ >= p.sweepSeconds) sweepPos_ = 0.0;
            break;
        }
        }
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);

        // Click-free enable/disable.
        if (ramp_ < rampTarget) ramp_ = std::min(rampTarget, ramp_ + rampStep);
        else if (ramp_ > rampTarget) ramp_ = std::max(rampTarget, ramp_ - rampStep);

        float value = static_cast<float>(s * gain) * ramp_;
        if (p.polarityInvert) value = -value;

        const float left = (p.channel == TestSignalChannel::Right) ? 0.0f : value;
        const float right = (p.channel == TestSignalChannel::Left) ? 0.0f : value;
        interleaved[2 * f] = left;
        interleaved[2 * f + 1] = right;
    }
}

}  // namespace neuracoust::daw
