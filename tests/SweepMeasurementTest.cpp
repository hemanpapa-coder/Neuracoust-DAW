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

    // ---- Harmonic separation (roadmap ②c, Farina) --------------------------------------
    // Drive a KNOWN memoryless Chebyshev nonlinearity — the same basis the AudioInterfaceModeler
    // waveshaper uses — and check the separated coefficients come back. f(x)=x+a2·T2(x)+a3·T3(x)
    // with x=sin synthesizes a pure 2nd harmonic at amplitude a2 and a pure 3rd at a3.
    {
        SweepParams ph;
        ph.sampleRate = 48000.0; ph.startHz = 20.0; ph.endHz = 20000.0;
        ph.durationSeconds = 2.0; ph.amplitude = 1.0;   // Chebyshev maps amplitude cleanly at unity
        const auto drive = generateLogSweep(ph);

        const double a2 = 0.08, a3 = 0.04;   // ≈ −22 dBc, −28 dBc
        auto rec = drive;
        for (auto& s : rec) {
            const double x = s;
            const double t2 = 2.0 * x * x - 1.0;
            const double t3 = 4.0 * x * x * x - 3.0 * x;
            s = static_cast<float>(x + a2 * t2 + a3 * t3);
        }
        const auto h = separateHarmonics(rec, ph, 7);
        check(h.valid && h.coefficients.size() == 6, "harmonic separation returns 6 orders");
        if (h.valid && h.coefficients.size() == 6) {
            check(std::abs(h.coefficients[0] - a2) < 0.02, "recovered c2 ≈ a2 (2nd harmonic)");
            check(std::abs(h.coefficients[1] - a3) < 0.02, "recovered c3 ≈ a3 (3rd harmonic)");
            check(h.coefficients[3] < 0.02 && h.coefficients[4] < 0.02, "unused orders ≈ 0");
            check(h.thdPercent > 5.0 && h.thdPercent < 20.0, "THD in the expected range");
        }

        // Null test: a clean linear system reports essentially no harmonics.
        const auto hz = separateHarmonics(drive, ph, 7);
        bool clean = hz.valid;
        for (double c : hz.coefficients) clean = clean && (c < 0.02);
        check(clean, "linear system → ~0 harmonics (null-safe)");
    }

    printf(failures == 0 ? "\nSweepMeasurement test passed\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
