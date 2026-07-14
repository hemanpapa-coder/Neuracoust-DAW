// The monitor parametric EQ (0–64 bands, added on demand, only active bands processed).
// Pins the biquad response: a peaking band boosts its centre and nowhere else, disabled/0 dB
// bands cost nothing, a high-pass rolls off the lows, and the processed audio matches the
// analytic magnitude response.
#include "audio/ParametricEq.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace neuracoust::daw;

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// Measured gain (dB) the EQ applies to a steady sine at `freq`, past the filter warm-up.
static double measuredGainDb(ParametricEq& eq, double sampleRate, double freq) {
    const int n = 16384;
    std::vector<float> buf(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        const float s = static_cast<float>(std::sin(2.0 * M_PI * freq * i / sampleRate));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    eq.reset();
    eq.processInterleavedStereo(buf.data(), n);
    double inSq = 0.0, outSq = 0.0;
    for (int i = n / 2; i < n; ++i) {   // second half: steady state
        const double in = std::sin(2.0 * M_PI * freq * i / sampleRate);
        inSq += in * in;
        outSq += static_cast<double>(buf[i * 2]) * buf[i * 2];
    }
    return 10.0 * std::log10(outSq / inSq);
}

int main() {
    const double sr = 48000.0;

    // Empty EQ = passthrough, no active bands.
    {
        ParametricEq eq;
        eq.configure(sr, {});
        check(!eq.active() && eq.activeBandCount() == 0, "empty EQ is inactive");
        check(std::abs(eq.magnitudeDb(1000.0)) < 1e-9, "empty EQ is flat");
    }

    // A +6 dB peaking band at 1 kHz boosts 1 kHz and leaves 100 Hz / 10 kHz alone.
    {
        ParametricEq eq;
        eq.configure(sr, {{true, EqBandType::Peaking, 1000.0, 6.0, 1.0}});
        check(eq.activeBandCount() == 1, "one active band");
        check(std::abs(eq.magnitudeDb(1000.0) - 6.0) < 0.1, "analytic +6 dB at centre");
        check(std::abs(eq.magnitudeDb(100.0)) < 0.6, "far below centre ~flat");
        check(std::abs(eq.magnitudeDb(10000.0)) < 0.6, "far above centre ~flat");
        // Processed audio matches the analytic response.
        check(std::abs(measuredGainDb(eq, sr, 1000.0) - 6.0) < 0.2, "measured +6 dB at 1 kHz");
        check(std::abs(measuredGainDb(eq, sr, 100.0)) < 0.5, "measured ~0 dB at 100 Hz");
    }

    // A disabled band and a 0 dB peaking band both cost nothing.
    {
        ParametricEq eq;
        eq.configure(sr, {
            {false, EqBandType::Peaking, 500.0, 8.0, 1.0},   // disabled
            {true,  EqBandType::Peaking, 2000.0, 0.0, 1.0},  // 0 dB no-op
        });
        check(!eq.active(), "disabled + 0 dB bands are skipped");
    }

    // A high-pass at 100 Hz rolls off the lows but passes the mids.
    {
        ParametricEq eq;
        eq.configure(sr, {{true, EqBandType::HighPass, 100.0, 0.0, 0.707}});
        check(eq.magnitudeDb(20.0) < -18.0, "HP kills 20 Hz");
        check(std::abs(eq.magnitudeDb(1000.0)) < 0.5, "HP passes 1 kHz");
        check(std::abs(eq.magnitudeDb(100.0) + 3.0) < 1.0, "HP is ~-3 dB at corner");
    }

    // The band ceiling holds at 64.
    {
        ParametricEq eq;
        std::vector<EqBandSpec> many;
        for (int i = 0; i < 80; ++i) {
            many.push_back({true, EqBandType::Peaking, 50.0 + i * 200.0, 2.0, 1.0});
        }
        eq.configure(sr, many);
        check(eq.activeBandCount() == ParametricEq::kMaxBands, "clamped to 64 bands");
    }

    printf(failures == 0 ? "\nParametricEq test passed\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
