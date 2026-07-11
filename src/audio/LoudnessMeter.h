#pragma once

#include <cstddef>
#include <deque>
#include <vector>

namespace neuracoust::daw {

// ITU-R BS.1770-4 loudness meter: momentary (400 ms), short-term (3 s) and gated
// integrated loudness in LUFS, plus loudness range (LRA) and true-peak (dBTP).
//
// Self-contained and framework-free so it is unit-testable off the render thread. The
// K-weighting coefficients are recomputed for the working sample rate.
class LoudnessMeter {
public:
    void prepare(double sampleRate);
    void reset();

    // Feed interleaved audio (any channel count; only L/R are weighted, per BS.1770 for
    // stereo). Safe to call every render block.
    void process(const float* interleaved, int64_t frames, int channels);

    float momentaryLufs() const { return momentaryLufs_; }
    float shortTermLufs() const { return shortTermLufs_; }
    float integratedLufs() const { return integratedLufs_; }
    float loudnessRange() const { return loudnessRange_; }
    float truePeakDb() const { return truePeakDb_; }

    // Feeds a synthetic tone and checks the meter is sane. 0 on success.
    static int runSelfTest();

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1L = 0, z2L = 0, z1R = 0, z2R = 0;
        double process(double x, double& z1, double& z2) const {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void resetState() { z1L = z2L = z1R = z2R = 0; }
    };

    void pushBlock(double meanSquare);

    // 4× polyphase oversampler for true-peak: reveals inter-sample peaks the raw
    // samples miss. `tpProto_` is a windowed-sinc prototype, each phase normalized.
    static constexpr int kTpPhases = 4;
    static constexpr int kTpTaps = 12;
    std::vector<float> tpProto_;        // length kTpPhases * kTpTaps
    std::vector<float> tpDelayL_;       // last kTpTaps input samples
    std::vector<float> tpDelayR_;
    int tpWrite_ = 0;
    float truePeakLinear_ = 0.0f;
    void buildTruePeakFilter();
    void processTruePeak(double l, double r);

    double sampleRate_ = 48000.0;
    Biquad stage1_;  // high-shelf
    Biquad stage2_;  // high-pass (RLB)

    // 100 ms sub-block accumulation of K-weighted mean square.
    int64_t blockSamples_ = 4800;
    int64_t sampleCounter_ = 0;
    double sumL_ = 0.0;
    double sumR_ = 0.0;

    // Rings of 100 ms block mean-squares. Momentary = 4 blocks, short-term = 30.
    std::deque<double> blocks_;
    // Gating blocks (400 ms, computed every 100 ms) for the integrated measurement.
    std::vector<double> gatingBlockLoudness_;

    float momentaryLufs_ = -70.0f;
    float shortTermLufs_ = -70.0f;
    float integratedLufs_ = -70.0f;
    float loudnessRange_ = 0.0f;
    float truePeakDb_ = -120.0f;
};

} // namespace neuracoust::daw
