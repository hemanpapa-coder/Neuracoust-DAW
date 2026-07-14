// Roadmap ③/④ core: a response curve fits to EQ bands the ParametricEq can render, plus the
// Harman target and curve helpers. Round-trips a known curve through fit → ParametricEq and
// checks the rendered response matches.
#include "audio/MonitorCorrection.h"
#include "audio/ParametricEq.h"

#include <cmath>
#include <cstdio>

using namespace neuracoust::daw;

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    // Harman target: flat below the corner, tilting down above it, monotonically.
    check(std::abs(harmanTargetDb(100.0)) < 1e-9, "Harman flat at 100 Hz");
    check(std::abs(harmanTargetDb(1000.0)) < 1e-9, "Harman 0 dB at corner");
    check(harmanTargetDb(2000.0) < -0.5, "Harman tilts down at 2 kHz");
    check(harmanTargetDb(16000.0) < harmanTargetDb(4000.0), "Harman keeps falling to the top");
    check(std::abs(harmanTargetDb(16000.0) - (-0.9 * 4.0)) < 0.05, "Harman ≈ -0.9 dB/oct (16k = 4 oct)");

    // Curve interpolation.
    ResponseCurve curve = {{100, 0}, {1000, 6}, {10000, 0}};
    check(std::abs(interpolateCurveDb(curve, 1000) - 6.0) < 1e-9, "interp exact at a point");
    check(std::abs(interpolateCurveDb(curve, 20) - 0.0) < 1e-9, "interp clamps low");
    check(interpolateCurveDb(curve, 316) > 2.0 && interpolateCurveDb(curve, 316) < 4.0, "interp between points");

    // Midband normalization: a curve offset by +5 dB comes back centred on 0.
    {
        ResponseCurve flat5;
        for (double f = 20; f <= 20000; f *= 1.1) flat5.push_back({f, 5.0});
        auto norm = normalizeCurveMidband(flat5);
        double mid = 0; int n = 0;
        for (auto& [f, db] : norm) if (f >= 300 && f <= 3000) { mid += db; ++n; }
        check(std::abs(mid / n) < 1e-6, "midband normalized to 0 dB");
    }

    // Round-trip: fit a +6 dB bell at 1 kHz, render through ParametricEq, expect the boost back
    // at 1 kHz and ~flat at 100 Hz / 10 kHz.
    {
        ResponseCurve bell;
        for (double f = 20; f <= 20000; f *= 1.05) {
            const double g = 6.0 * std::exp(-std::pow(std::log(f / 1000.0) / std::log(2.0), 2.0) / 2.0);
            bell.push_back({f, g});
        }
        auto bands = fitCurveToEqBands(bell, 31, 20.0, 20000.0);
        check(!bands.empty(), "fit produced bands");
        ParametricEq eq;
        eq.configure(48000.0, bands);
        check(std::abs(eq.magnitudeDb(1000.0) - 6.0) < 1.5, "rendered ~+6 dB at 1 kHz");
        check(std::abs(eq.magnitudeDb(100.0)) < 1.5, "rendered ~flat at 100 Hz");
        check(std::abs(eq.magnitudeDb(10000.0)) < 1.5, "rendered ~flat at 10 kHz");
    }

    // Boost/cut clamps: a huge deep null can't be filled, a huge peak is cut freely.
    {
        ResponseCurve extreme = {{20, 0}, {1000, 40}, {2000, -40}, {20000, 0}};
        auto bands = fitCurveToEqBands(extreme, 31, 20.0, 20000.0, 6.0, 12.0);
        double maxG = -100, minG = 100;
        for (auto& b : bands) { maxG = std::max(maxG, b.gainDb); minG = std::min(minG, b.gainDb); }
        check(maxG <= 6.0 + 1e-6, "boost clamped to +6 dB");
        check(minG >= -12.0 - 1e-6, "cut clamped to -12 dB");
    }

    printf(failures == 0 ? "\nMonitorCorrection test passed\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
