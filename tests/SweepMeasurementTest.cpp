// Roadmap ② measurement core: log sweep + deconvolution recovers a system's response.
// Runs a sweep through a KNOWN biquad, deconvolves the "recording", and checks the recovered
// magnitude curve matches the biquad — a flat system deconvolves flat, a +6 dB bell at 1 kHz
// comes back as +6 dB there and flat at the neighbours.
#include "audio/SweepMeasurement.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace neuracoust::daw;

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// A mono RBJ peaking biquad applied in place — the "system under test".
static void applyPeaking(std::vector<float>& x, double sr, double fc, double gainDb, double q) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * M_PI * fc / sr;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cw = std::cos(w0);
    const double b0 = 1.0 + alpha * A, b1 = -2.0 * cw, b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A, a1 = -2.0 * cw, a2 = 1.0 - alpha / A;
    const double nb0 = b0 / a0, nb1 = b1 / a0, nb2 = b2 / a0, na1 = a1 / a0, na2 = a2 / a0;
    double z1 = 0.0, z2 = 0.0;
    for (auto& s : x) {
        const double in = s;
        const double y = nb0 * in + z1;
        z1 = nb1 * in - na1 * y + z2;
        z2 = nb2 * in - na2 * y;
        s = static_cast<float>(y);
    }
}

static double magAt(const std::vector<double>& curve, double minHz, double maxHz, double f) {
    const int n = static_cast<int>(curve.size());
    const double t = std::log(f / minHz) / std::log(maxHz / minHz);
    const int i = std::max(0, std::min(n - 1, static_cast<int>(std::llround(t * (n - 1)))));
    return curve[i];
}

int main() {
    SweepParams p;
    p.sampleRate = 48000.0; p.startHz = 20.0; p.endHz = 20000.0; p.durationSeconds = 2.0;

    // Sweep basics.
    const auto sweep = generateLogSweep(p);
    check(std::abs(static_cast<int>(sweep.size()) - 96000) < 4, "sweep length ≈ 2 s");
    check(std::abs(sweep.front()) < 1e-3 && std::abs(sweep.back()) < 1e-3, "sweep fades in/out (no click)");
    float peak = 0.0f; for (float s : sweep) peak = std::max(peak, std::abs(s));
    check(peak > 0.4f && peak <= 0.51f, "sweep amplitude ~0.5");

    const int pts = 240;

    // Flat system: recording == sweep → deconvolved response is flat.
    {
        const auto ir = deconvolveSweep(sweep, p);
        const auto curve = impulseResponseMagnitudeDb(ir, p.sampleRate, pts, 30.0, 16000.0);
        const double m100 = magAt(curve, 30, 16000, 100);
        const double m1k = magAt(curve, 30, 16000, 1000);
        const double m10k = magAt(curve, 30, 16000, 10000);
        check(std::abs(m1k - m100) < 1.0 && std::abs(m10k - m100) < 1.0, "flat system deconvolves flat");
    }

    // Known +6 dB bell at 1 kHz (Q 1): recovered response matches, relative to the neighbours.
    {
        auto recorded = sweep;
        applyPeaking(recorded, p.sampleRate, 1000.0, 6.0, 1.0);
        const auto ir = deconvolveSweep(recorded, p);
        const auto curve = impulseResponseMagnitudeDb(ir, p.sampleRate, pts, 30.0, 16000.0);
        const double m100 = magAt(curve, 30, 16000, 100);
        const double m1k = magAt(curve, 30, 16000, 1000);
        const double m10k = magAt(curve, 30, 16000, 10000);
        check(std::abs((m1k - m100) - 6.0) < 1.0, "recovered +6 dB at 1 kHz");
        check(std::abs(m10k - m100) < 1.0, "10 kHz ~ flat (unaffected by the bell)");
    }

    // Known -8 dB cut at 4 kHz: recovered as a dip.
    {
        auto recorded = sweep;
        applyPeaking(recorded, p.sampleRate, 4000.0, -8.0, 2.0);
        const auto ir = deconvolveSweep(recorded, p);
        const auto curve = impulseResponseMagnitudeDb(ir, p.sampleRate, pts, 30.0, 16000.0);
        const double m500 = magAt(curve, 30, 16000, 500);
        const double m4k = magAt(curve, 30, 16000, 4000);
        check((m4k - m500) < -5.0, "recovered dip at 4 kHz");
    }

    printf(failures == 0 ? "\nSweepMeasurement test passed\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
