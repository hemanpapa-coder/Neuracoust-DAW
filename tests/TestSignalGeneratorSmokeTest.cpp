// Pins the native test-signal generator: sine frequency/level accuracy, channel routing, polarity,
// click-free ramp, deterministic noise, and that the band-limited oscillators stay bounded (no
// runaway aliasing/DC). If any of these drift the generator would lie during measurement.
#include "audio/TestSignalGenerator.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using neuracoust::daw::TestSignalChannel;
using neuracoust::daw::TestSignalGenerator;
using neuracoust::daw::TestSignalParams;
using neuracoust::daw::TestSignalWaveform;

namespace {
int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

// Peak magnitude of a chosen channel (0 = left, 1 = right) after skipping the ramp-in region.
double peakChannel(const std::vector<float>& buf, int channel, int skipFrames) {
    double peak = 0.0;
    for (int f = skipFrames; f * 2 + channel < (int)buf.size(); ++f)
        peak = std::max(peak, std::fabs((double)buf[f * 2 + channel]));
    return peak;
}

// Estimate frequency by counting positive-going zero crossings over the settled region.
double estimateHz(const std::vector<float>& buf, int channel, int skipFrames, double sr) {
    int crossings = 0;
    int firstCross = -1, lastCross = -1;
    for (int f = skipFrames + 1; f * 2 + channel < (int)buf.size(); ++f) {
        const float prev = buf[(f - 1) * 2 + channel];
        const float cur = buf[f * 2 + channel];
        if (prev <= 0.0f && cur > 0.0f) {
            if (firstCross < 0) firstCross = f;
            lastCross = f;
            ++crossings;
        }
    }
    if (crossings < 2 || lastCross <= firstCross) return 0.0;
    const double periods = crossings - 1;
    const double seconds = (lastCross - firstCross) / sr;
    return periods / seconds;
}
}  // namespace

int main() {
    const double sr = 48000.0;
    const int frames = 48000;  // 1 s
    std::vector<float> buf(frames * 2, -123.0f);

    // --- Sine: frequency + level accuracy ---
    {
        TestSignalGenerator gen;
        TestSignalParams p;
        p.waveform = TestSignalWaveform::Sine;
        p.frequencyHz = 1000.0;
        p.levelDb = -20.0;  // → 0.1 peak
        p.channel = TestSignalChannel::Stereo;
        gen.setParams(p);
        gen.reset();
        gen.generateInterleavedStereo(buf.data(), frames, sr);

        const double hz = estimateHz(buf, 0, 2000, sr);
        check(std::fabs(hz - 1000.0) < 1.0, "sine 1kHz frequency within 1 Hz");

        const double peak = peakChannel(buf, 0, 2000);
        check(std::fabs(peak - 0.1) < 0.002, "sine -20 dBFS peak ~0.1");

        // Level scales: -6 dB ≈ 0.5012
        p.levelDb = -6.0;
        gen.setParams(p);
        gen.reset();
        gen.generateInterleavedStereo(buf.data(), frames, sr);
        const double peak6 = peakChannel(buf, 0, 2000);
        check(std::fabs(peak6 - 0.5012) < 0.01, "sine -6 dBFS peak ~0.5");
    }

    // --- Channel routing: Left silences right, Right silences left ---
    {
        TestSignalGenerator gen;
        TestSignalParams p;
        p.waveform = TestSignalWaveform::Sine;
        p.frequencyHz = 440.0;
        p.levelDb = -6.0;
        p.channel = TestSignalChannel::Left;
        gen.setParams(p);
        gen.reset();
        gen.generateInterleavedStereo(buf.data(), frames, sr);
        check(peakChannel(buf, 0, 2000) > 0.4, "left-only: left has signal");
        check(peakChannel(buf, 1, 2000) < 1e-6, "left-only: right silent");

        p.channel = TestSignalChannel::Right;
        gen.setParams(p);
        gen.reset();
        gen.generateInterleavedStereo(buf.data(), frames, sr);
        check(peakChannel(buf, 0, 2000) < 1e-6, "right-only: left silent");
        check(peakChannel(buf, 1, 2000) > 0.4, "right-only: right has signal");
    }

    // --- Polarity invert flips sign of every sample ---
    {
        TestSignalGenerator a, b;
        TestSignalParams p;
        p.waveform = TestSignalWaveform::Sine;
        p.frequencyHz = 500.0;
        p.levelDb = -6.0;
        p.polarityInvert = false;
        a.setParams(p);
        a.reset();
        std::vector<float> ba(frames * 2, 0.0f);
        a.generateInterleavedStereo(ba.data(), frames, sr);

        p.polarityInvert = true;
        b.setParams(p);
        b.reset();
        std::vector<float> bb(frames * 2, 0.0f);
        b.generateInterleavedStereo(bb.data(), frames, sr);

        double maxErr = 0.0;
        for (int i = 4000; i < frames * 2; ++i) maxErr = std::max(maxErr, std::fabs((double)(ba[i] + bb[i])));
        check(maxErr < 1e-5, "polarity invert == exact negation");
    }

    // --- Click-free: first sample near zero, ramps up over ~5 ms ---
    {
        TestSignalGenerator gen;
        TestSignalParams p;
        p.waveform = TestSignalWaveform::Sine;
        p.frequencyHz = 1000.0;
        p.levelDb = 0.0;
        gen.setParams(p);
        gen.reset();
        gen.generateInterleavedStereo(buf.data(), frames, sr);
        check(std::fabs(buf[0]) < 0.05, "ramp: first sample near zero (no click)");
        const double early = peakChannel(buf, 0, 0);
        const double settled = peakChannel(buf, 0, 2000);
        check(settled > 0.9 && early <= settled + 1e-3, "ramp: settles to full level");
    }

    // --- Deterministic noise: same seed → identical stream; and bounded ---
    {
        TestSignalGenerator a, b;
        TestSignalParams p;
        p.waveform = TestSignalWaveform::PinkNoise;
        p.levelDb = -12.0;
        a.setParams(p);
        b.setParams(p);
        a.reset();
        b.reset();
        std::vector<float> ba(frames * 2, 0.0f), bb(frames * 2, 0.0f);
        a.generateInterleavedStereo(ba.data(), frames, sr);
        b.generateInterleavedStereo(bb.data(), frames, sr);
        bool identical = true;
        double peak = 0.0;
        for (int i = 0; i < frames * 2; ++i) {
            if (ba[i] != bb[i]) identical = false;
            peak = std::max(peak, std::fabs((double)ba[i]));
        }
        check(identical, "pink noise deterministic for a fixed seed");
        check(peak < 1.0, "pink noise stays below full scale");
    }

    // --- Band-limited oscillators stay bounded (no aliasing blow-up / DC runaway) ---
    {
        const TestSignalWaveform waves[] = {TestSignalWaveform::Square, TestSignalWaveform::Saw,
                                            TestSignalWaveform::Triangle};
        const char* names[] = {"square", "saw", "triangle"};
        for (int w = 0; w < 3; ++w) {
            TestSignalGenerator gen;
            TestSignalParams p;
            p.waveform = waves[w];
            p.frequencyHz = 2000.0;
            p.levelDb = 0.0;
            gen.setParams(p);
            gen.reset();
            gen.generateInterleavedStereo(buf.data(), frames, sr);
            double peak = 0.0, mean = 0.0;
            int n = 0;
            for (int f = 4800; f < frames; ++f) {
                const double v = buf[f * 2];
                peak = std::max(peak, std::fabs(v));
                mean += v;
                ++n;
            }
            mean /= std::max(1, n);
            char msg[96];
            std::snprintf(msg, sizeof msg, "%s bounded (peak<1.5)", names[w]);
            check(peak < 1.5, msg);
            std::snprintf(msg, sizeof msg, "%s no DC (|mean|<0.05)", names[w]);
            check(std::fabs(mean) < 0.05, msg);
        }
    }

    // --- Sweep runs and its instantaneous frequency climbs ---
    {
        TestSignalGenerator gen;
        TestSignalParams p;
        p.waveform = TestSignalWaveform::Sweep;
        p.levelDb = -6.0;
        p.sweepStartHz = 100.0;
        p.sweepEndHz = 8000.0;
        p.sweepSeconds = 1.0;
        p.sweepLogarithmic = true;
        gen.setParams(p);
        gen.reset();
        gen.generateInterleavedStereo(buf.data(), frames, sr);
        // Compare zero-cross density in the first quarter vs the last quarter.
        std::vector<float> early(buf.begin(), buf.begin() + (frames / 4) * 2);
        std::vector<float> late(buf.end() - (frames / 4) * 2, buf.end());
        const double hzEarly = estimateHz(early, 0, 500, sr);
        const double hzLate = estimateHz(late, 0, 0, sr);
        check(hzLate > hzEarly * 2.0, "sweep frequency rises over time");
    }

    if (g_failures == 0) {
        std::printf("TestSignalGenerator smoke: all checks passed\n");
        return 0;
    }
    std::printf("TestSignalGenerator smoke: %d failure(s)\n", g_failures);
    return 1;
}
