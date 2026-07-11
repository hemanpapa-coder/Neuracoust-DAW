#include "audio/LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace neuracoust::daw {

namespace {
constexpr double kAbsoluteGateLufs = -70.0;
constexpr double kRelativeGateLu = -10.0;
constexpr double kOffset = -0.691;  // BS.1770 loudness offset

double energyToLufs(double energy) {
    return energy > 0.0 ? kOffset + 10.0 * std::log10(energy) : -std::numeric_limits<double>::infinity();
}
} // namespace

void LoudnessMeter::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    blockSamples_ = static_cast<int64_t>(std::llround(sampleRate_ * 0.1));  // 100 ms
    if (blockSamples_ < 1) blockSamples_ = 1;

    // BS.1770-4 K-weighting, recomputed for this sample rate (pyloudnorm form).
    // Stage 1: high shelf.
    {
        const double db = 3.999843853973347;
        const double f0 = 1681.974450955533;
        const double Q = 0.7071752369554196;
        const double K = std::tan(M_PI * f0 / sampleRate_);
        const double Vh = std::pow(10.0, db / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        stage1_.b0 = (Vh + Vb * K / Q + K * K) / a0;
        stage1_.b1 = 2.0 * (K * K - Vh) / a0;
        stage1_.b2 = (Vh - Vb * K / Q + K * K) / a0;
        stage1_.a1 = 2.0 * (K * K - 1.0) / a0;
        stage1_.a2 = (1.0 - K / Q + K * K) / a0;
    }
    // Stage 2: RLB high-pass.
    {
        const double f0 = 38.13547087602444;
        const double Q = 0.5003270373238773;
        const double K = std::tan(M_PI * f0 / sampleRate_);
        const double a0 = 1.0 + K / Q + K * K;
        stage2_.b0 = 1.0;
        stage2_.b1 = -2.0;
        stage2_.b2 = 1.0;
        stage2_.a1 = 2.0 * (K * K - 1.0) / a0;
        stage2_.a2 = (1.0 - K / Q + K * K) / a0;
    }
    reset();
}

void LoudnessMeter::reset() {
    stage1_.resetState();
    stage2_.resetState();
    sampleCounter_ = 0;
    sumL_ = sumR_ = 0.0;
    blocks_.clear();
    gatingBlockLoudness_.clear();
    momentaryLufs_ = shortTermLufs_ = integratedLufs_ = -70.0f;
    loudnessRange_ = 0.0f;
    truePeakDb_ = -120.0f;
    peakHold_ = 0.0f;
}

void LoudnessMeter::pushBlock(double meanSquare) {
    blocks_.push_back(meanSquare);
    while (blocks_.size() > 30) blocks_.pop_front();  // 3 s of 100 ms blocks

    // Momentary: last 400 ms.
    auto averageLast = [&](size_t count) -> double {
        const size_t n = std::min(count, blocks_.size());
        if (n == 0) return 0.0;
        double sum = 0.0;
        for (size_t i = blocks_.size() - n; i < blocks_.size(); ++i) sum += blocks_[i];
        return sum / static_cast<double>(n);
    };
    const double momentaryEnergy = averageLast(4);
    const double shortTermEnergy = averageLast(30);
    momentaryLufs_ = static_cast<float>(std::max(-70.0, energyToLufs(momentaryEnergy)));
    shortTermLufs_ = static_cast<float>(std::max(-70.0, energyToLufs(shortTermEnergy)));

    // Integrated: a 400 ms gating block every 100 ms, gated absolutely then relatively.
    if (blocks_.size() >= 4) {
        gatingBlockLoudness_.push_back(momentaryEnergy);
    }
    if (!gatingBlockLoudness_.empty()) {
        double sumAbs = 0.0;
        int countAbs = 0;
        for (double e : gatingBlockLoudness_) {
            if (energyToLufs(e) >= kAbsoluteGateLufs) { sumAbs += e; ++countAbs; }
        }
        if (countAbs > 0) {
            const double relThreshold = energyToLufs(sumAbs / countAbs) + kRelativeGateLu;
            double sumRel = 0.0;
            int countRel = 0;
            for (double e : gatingBlockLoudness_) {
                if (energyToLufs(e) >= relThreshold) { sumRel += e; ++countRel; }
            }
            if (countRel > 0) {
                integratedLufs_ = static_cast<float>(std::max(-70.0, energyToLufs(sumRel / countRel)));
            }
        }

        // Loudness range: 10th–95th percentile of gated block loudness.
        std::vector<double> loud;
        loud.reserve(gatingBlockLoudness_.size());
        for (double e : gatingBlockLoudness_) {
            const double l = energyToLufs(e);
            if (l >= kAbsoluteGateLufs) loud.push_back(l);
        }
        if (loud.size() >= 2) {
            std::sort(loud.begin(), loud.end());
            auto pct = [&](double p) {
                const double idx = p * (loud.size() - 1);
                const size_t lo = static_cast<size_t>(std::floor(idx));
                const size_t hi = std::min(loud.size() - 1, lo + 1);
                const double frac = idx - lo;
                return loud[lo] * (1.0 - frac) + loud[hi] * frac;
            };
            loudnessRange_ = static_cast<float>(std::max(0.0, pct(0.95) - pct(0.10)));
        }
    }
}

void LoudnessMeter::process(const float* interleaved, int64_t frames, int channels) {
    if (interleaved == nullptr || frames <= 0 || channels <= 0) return;
    for (int64_t frame = 0; frame < frames; ++frame) {
        const double xL = interleaved[frame * channels];
        const double xR = channels > 1 ? interleaved[frame * channels + 1] : xL;
        peakHold_ = std::max(peakHold_, static_cast<float>(std::max(std::abs(xL), std::abs(xR))));

        const double s1L = stage1_.process(xL, stage1_.z1L, stage1_.z2L);
        const double yL = stage2_.process(s1L, stage2_.z1L, stage2_.z2L);
        const double s1R = stage1_.process(xR, stage1_.z1R, stage1_.z2R);
        const double yR = stage2_.process(s1R, stage2_.z1R, stage2_.z2R);
        sumL_ += yL * yL;
        sumR_ += yR * yR;

        if (++sampleCounter_ >= blockSamples_) {
            const double meanSquare = sumL_ / blockSamples_ + sumR_ / blockSamples_;
            pushBlock(meanSquare);
            sumL_ = sumR_ = 0.0;
            sampleCounter_ = 0;
        }
    }
    truePeakDb_ = peakHold_ > 0.0f ? 20.0f * std::log10(peakHold_) : -120.0f;
}

int LoudnessMeter::runSelfTest() {
    // A 1 kHz sine at -20 dBFS through the K-weighting reads near -20 LUFS (the filter
    // is ~unity at 1 kHz). Verify it lands in a sane window and that louder reads higher.
    const double sr = 48000.0;
    auto measure = [&](double amplitude) {
        LoudnessMeter m;
        m.prepare(sr);
        std::vector<float> block(2 * 4800);  // 100 ms stereo
        double phase = 0.0;
        const double inc = 2.0 * M_PI * 1000.0 / sr;
        for (int b = 0; b < 40; ++b) {  // 4 s
            for (int i = 0; i < 4800; ++i) {
                const float s = static_cast<float>(amplitude * std::sin(phase));
                phase += inc;
                block[static_cast<size_t>(i) * 2] = s;
                block[static_cast<size_t>(i) * 2 + 1] = s;
            }
            m.process(block.data(), 4800, 2);
        }
        return m;
    };

    const double ampMinus20 = std::pow(10.0, -20.0 / 20.0);  // -20 dBFS peak sine
    LoudnessMeter loud = measure(ampMinus20);
    const float integrated = loud.integratedLufs();
    if (!(integrated > -30.0f && integrated < -14.0f)) {
        std::cerr << "LOUDNESS_SELF_TEST failed: -20 dBFS sine integrated = "
                  << integrated << " LUFS, expected roughly -23..-17\n";
        return 61;
    }
    LoudnessMeter quiet = measure(std::pow(10.0, -40.0 / 20.0));
    if (!(quiet.integratedLufs() < integrated - 10.0f)) {
        std::cerr << "LOUDNESS_SELF_TEST failed: quieter signal did not read lower\n";
        return 62;
    }
    std::cout << "LOUDNESS_SELF_TEST ok (integrated " << integrated << " LUFS)\n";
    return 0;
}

} // namespace neuracoust::daw
