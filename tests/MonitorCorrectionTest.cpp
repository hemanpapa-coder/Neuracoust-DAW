// Roadmap ③/④ core: a response curve fits to EQ bands the ParametricEq can render, plus the
// Harman target and curve helpers. Round-trips a known curve through fit → ParametricEq and
// checks the rendered response matches.
#include "audio/MonitorCorrection.h"
#include "audio/MonitorDspProcessor.h"
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
    // Matched-Z peaking: a high-frequency bell must render its actual gain at its centre, where the
    // RBJ cookbook peak cramps low. A +6 dB bell at 15 kHz (48 k) should measure ~+6, not the
    // ~+3–4 the cramped RBJ filter would give.
    {
        const double sr = 48000.0;
        for (double f0 : {12000.0, 15000.0, 18000.0}) {
            ParametricEq eq;
            eq.configure(sr, {{true, EqBandType::Peaking, f0, 6.0, 2.0}});
            const double atCentre = eq.magnitudeDb(f0);
            char msg[96];
            snprintf(msg, sizeof msg, "matched-Z bell renders +6 dB at %.0f Hz (got %.2f)", f0, atCentre);
            check(std::abs(atCentre - 6.0) < 1.0, msg);
        }
        // And a cut is not cramped into a deeper notch than asked.
        ParametricEq eq;
        eq.configure(sr, {{true, EqBandType::Peaking, 15000.0, -6.0, 3.0}});
        check(eq.magnitudeDb(15000.0) > -8.0, "matched-Z cut at 15 kHz stays near -6 dB (no deep ring)");
    }

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

    // Dense-grid fit + fractional-octave smoothing: track the broad tonal shape but do NOT chase
    // fine measurement noise. Feed a broad base plus a narrow ripple; the rendered EQ must follow
    // the base (ripple smoothed out), which is the wobble the user saw the EQ reproducing.
    {
        auto base = [](double f) { return 4.0 * std::sin(std::log10(f) * 3.0); };
        auto noise = [](double f) { return 3.0 * std::sin(std::log10(f) * 80.0); };
        ResponseCurve noisy;
        for (double f = 20; f <= 20000; f *= 1.02) noisy.push_back({f, base(f) + noise(f)});
        auto bands = fitCurveToEqBands(noisy, 64, 20.0, 20000.0, 9.0, 9.0);   // default 1/6-oct smoothing
        ParametricEq eq;
        eq.configure(48000.0, bands);
        double worst = 0.0, worstF = 0.0, ripple = 0.0;
        for (double f = 40; f <= 16000; f *= 1.02) {
            const double err = std::abs(eq.magnitudeDb(f) - base(f));
            if (err > worst) { worst = err; worstF = f; }
        }
        // The EQ itself should be smooth: small bin-to-bin change across a fine grid.
        double prev = eq.magnitudeDb(40.0);
        for (double f = 40 * 1.01; f <= 16000; f *= 1.01) {
            const double cur = eq.magnitudeDb(f);
            ripple = std::max(ripple, std::abs(cur - prev));
            prev = cur;
        }
        char msg[128];
        snprintf(msg, sizeof msg, "tracks smooth base, rejects ripple (worst %.2f dB @ %.0f Hz, step %.3f dB)", worst, worstF, ripple);
        check(worst < 1.6, msg);
        check(ripple < 0.6, "rendered EQ is smooth (no per-step jag)");
    }

    // Near-Nyquist ringing: a treble rolloff (like a real speaker) must not make the fit swing
    // adjacent cramped bands into a deep notch — the sharp -20 dB+ treble spike the user saw.
    // The rendered EQ must stay smooth and bounded up to 19 kHz.
    {
        ResponseCurve roll;
        for (double f = 20; f <= 20000; f *= 1.02) {
            const double db = f < 2000 ? 0.0 : -5.0 * (std::log10(f) - std::log10(2000.0));
            roll.push_back({f, db});
        }
        auto bands = fitCurveToEqBands(roll, 64, 20.0, 20000.0, 9.0, 15.0);
        ParametricEq eq;
        eq.configure(48000.0, bands);
        double minDb = 100.0, maxStep = 0.0, prev = eq.magnitudeDb(2000.0);
        for (double f = 2000.0 * 1.01; f <= 19000.0; f *= 1.01) {
            const double cur = eq.magnitudeDb(f);
            maxStep = std::max(maxStep, std::abs(cur - prev));
            prev = cur;
            minDb = std::min(minDb, cur);
        }
        char msg[128];
        snprintf(msg, sizeof msg, "no near-Nyquist ring (min %.1f dB, step %.3f dB)", minDb, maxStep);
        check(maxStep < 0.8 && minDb > -12.0, msg);
    }

    // Sample-rate-correct fitting (Codex #2): the same target, fit at each engine rate, must render
    // back accurately in the treble. A 48 kHz-only fit rendered at 96 kHz drifted near the top octave.
    {
        for (double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
            ResponseCurve target;
            for (double f = 20; f <= 20000; f *= 1.03) {
                const double db = f < 1000.0 ? 0.0 : -3.0 * (std::log10(f) - 3.0);   // gentle treble tilt
                target.push_back({f, db});
            }
            auto bands = fitCurveToEqBands(target, 64, 20.0, 20000.0, 9.0, 15.0, 1.0 / 6.0, fs);
            ParametricEq eq;
            eq.configure(fs, bands);
            double worst = 0.0, worstF = 0.0;
            const double top = std::min(18000.0, fs * 0.4);
            for (double f = 1000.0; f <= top; f *= 1.03) {
                const double err = std::abs(eq.magnitudeDb(f) - interpolateCurveDb(target, f));
                if (err > worst) { worst = err; worstF = f; }
            }
            char msg[128];
            snprintf(msg, sizeof msg, "fit@%.0fk renders target to <1.2 dB (worst %.2f dB @ %.0f Hz)", fs / 1000.0, worst, worstF);
            check(worst < 1.2, msg);
        }
    }

    // Perceptual level match (Codex #3): a monitor-sim curve with a treble PRESENCE boost and a flat
    // midband must be level-matched on the MIDBAND, not shifted down by its peak. Peak normalization
    // (old behaviour) dropped the whole curve by the boost, darkening the low/mid — the "답답/어두움".
    {
        ResponseCurve sim;
        for (double f = 20; f <= 20000; f *= 1.03) {
            // Flat everywhere except a +6 dB presence bump centred at 3.5 kHz.
            const double bump = 6.0 * std::exp(-std::pow(std::log(f / 3500.0), 2.0) / (2.0 * 0.09));
            sim.push_back({f, bump});
        }
        const ResponseCurve matched = normalizeCurveMidband(sim);

        // Midband (300 Hz–3 kHz) mean sits at 0 dB after matching.
        double sum = 0.0; int n = 0;
        for (const auto& [f, db] : matched) if (f >= 300.0 && f <= 3000.0) { sum += db; ++n; }
        const double midMean = n > 0 ? sum / n : 99.0;
        check(std::abs(midMean) < 0.5, "level-match: midband mean pinned to 0 dB");

        // The low band stays near 0 dB (NOT pulled down ~6 dB the way peak-normalization would).
        check(interpolateCurveDb(matched, 100.0) > -1.5, "level-match: low band not darkened");

        // The presence bump survives as a real boost relative to the midband (the sim's intent).
        double peak = -100.0;
        for (const auto& [f, db] : matched) peak = std::max(peak, db);
        check(peak > 4.5, "level-match: presence boost preserved as a real boost");

        // Contrast: the OLD peak-normalization would have driven the low band to ≈ -6 dB.
        double peakRaw = -100.0;
        for (const auto& [f, db] : sim) peakRaw = std::max(peakRaw, db);
        const double oldLowBand = interpolateCurveDb(sim, 100.0) - peakRaw;
        check(oldLowBand < -4.0, "level-match: old peak-normalization WOULD have darkened low band");
    }

    // Monitor safety soft-clip (Codex #3, part b): transparent below the knee, bounded above full
    // scale, odd-symmetric, monotone — so it catches only the residual sim overshoot, never colours
    // ordinary program level.
    {
        check(monitorSafetySoftClip(0.5f) == 0.5f, "soft-clip transparent at 0.5");
        check(monitorSafetySoftClip(-0.7f) == -0.7f, "soft-clip transparent at -0.7");
        check(monitorSafetySoftClip(0.85f) == 0.85f, "soft-clip transparent below -1 dBFS knee");
        check(monitorSafetySoftClip(2.0f) < 1.0f && monitorSafetySoftClip(2.0f) > 0.9f,
              "soft-clip bounds a +6 dBFS overshoot below full scale");
        check(std::abs(monitorSafetySoftClip(1.5f) + monitorSafetySoftClip(-1.5f)) < 1e-6f,
              "soft-clip is odd-symmetric");
        check(monitorSafetySoftClip(1.2f) < monitorSafetySoftClip(1.4f),
              "soft-clip stays monotone above the knee");
    }

    // Headphone-sim fixed tilt bypass (Codex #5): when the active target is a MEASURED headphone,
    // its curve voices the single EQ, so the module's generic 6.8 kHz tilt must be skipped (else the
    // treble is double-attenuated). A non-measured target keeps the generic tilt. Differential test:
    // identical module, only the target differs, so crossfeed cancels and only the tilt shows up.
    {
        const double sr = 48000.0;
        auto trebleRms = [&](const std::string& target) {
            MonitorDspModule m;
            m.id = "headphone-simulation";
            m.enabled = true;
            m.activeTargetSlot = 0;
            m.targetModelA = target;
            MonitorDspProcessor proc;
            proc.configure(sr, {m});
            double acc = 0.0; int n = 0;
            for (int i = 0; i < 4096; ++i) {
                const float x = std::sin(2.0 * M_PI * 12000.0 * i / sr);
                const StereoFrame out = proc.process({x, x});
                if (i >= 2048) { acc += out.left * out.left; ++n; }   // skip filter transient
            }
            return std::sqrt(acc / std::max(1, n));
        };
        const double measured = trebleRms("Sennheiser HD 600");   // has a measured curve
        const double generic = trebleRms("Generic Test Cans");    // no curve → keeps the tilt
        char msg[128];
        snprintf(msg, sizeof msg, "measured headphone bypasses fixed tilt (meas %.4f > generic %.4f)", measured, generic);
        check(measured > generic * 1.05, msg);
    }

    // Room correction L/R averaging (Codex #4): with both channels measured, the single-EQ correction
    // must be the AVERAGE of the two, not one channel applied to both. Give L and R opposite 100 Hz
    // deviations; the averaged correction there is ~0 (they cancel), and it sits between the two
    // channels' individual corrections.
    {
        ResponseCurve measuredL, measuredR;
        for (double f = 20; f <= 20000; f *= 1.05) {
            const double lBump = (f > 80 && f < 125) ? 4.0 : 0.0;    // L: +4 dB room mode at 100 Hz
            const double rBump = (f > 80 && f < 125) ? -4.0 : 0.0;   // R: -4 dB null at 100 Hz
            measuredL.push_back({f, lBump});
            measuredR.push_back({f, rBump});
        }
        const ResponseCurve avg = roomCorrectionCurve(measuredL, measuredR);
        const ResponseCurve lOnly = roomCorrectionCurve(measuredL, {});
        const ResponseCurve rOnly = roomCorrectionCurve({}, measuredR);
        const double avg100 = interpolateCurveDb(avg, 100.0);
        const double l100 = interpolateCurveDb(lOnly, 100.0);
        const double r100 = interpolateCurveDb(rOnly, 100.0);
        char msg[128];
        snprintf(msg, sizeof msg, "room L/R average cancels opposite modes (avg %.2f, L %.2f, R %.2f)", avg100, l100, r100);
        check(std::abs(avg100) < 0.5 && avg100 > std::min(l100, r100) && avg100 < std::max(l100, r100), msg);

        // Single-channel fallback still corrects that channel; empty stays empty.
        check(!lOnly.empty() && std::abs(interpolateCurveDb(lOnly, 100.0) + 4.0) < 0.5,
              "room single-channel corrects that channel (-4 dB for a +4 dB mode)");
        check(roomCorrectionCurve({}, {}).empty(), "room correction empty when nothing measured");
    }

    printf(failures == 0 ? "\nMonitorCorrection test passed\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
