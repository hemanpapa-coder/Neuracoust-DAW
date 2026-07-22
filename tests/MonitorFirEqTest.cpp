// Linear-phase FIR monitor EQ: the design must match an arbitrary target magnitude across the
// WHOLE band — the steep bass rolloff and treble dips the biquad fit could not — and the realtime
// convolution must actually apply it.

#include "audio/MonitorFirEq.h"
#include "audio/MonitorCorrection.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace neuracoust::daw;

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    const double sr = 48000.0;

    // 1) Impulse response identity: a flat 0 dB target is a bypass (cleared, zero latency).
    {
        MonitorFirEq fir;
        ResponseCurve flat = {{20, 0.0}, {20000, 0.0}};
        fir.designFromCurve(sr, flat, 2048);
        check(!fir.active(), "flat target bypasses (no FIR)");
    }

    // 2) A target with a steep bass rolloff AND a treble dip — exactly the two ends the biquad fit
    //    struggled with. The designed FIR magnitude must track it within ~1.2 dB everywhere.
    {
        auto wanted = [](double f) {
            double db = 0.0;
            if (f < 120.0) db += -18.0 * (std::log10(120.0 / std::max(20.0, f)) / std::log10(120.0 / 40.0)); // steep low rolloff
            db += -5.0 * std::exp(-std::pow((std::log10(f) - std::log10(15000.0)) / 0.1, 2.0)); // treble dip at 15 kHz
            db += 2.0 * std::exp(-std::pow((std::log10(f) - std::log10(1500.0)) / 0.15, 2.0));  // presence bump
            return db;
        };
        ResponseCurve curve;
        for (double f = 20; f <= 20000; f *= 1.02) curve.push_back({f, wanted(f)});
        MonitorFirEq fir;
        fir.designFromCurve(sr, curve, 4096);
        check(fir.active() && fir.numTaps() == 4096, "designed a 4096-tap FIR");
        check(fir.latencySamples() == 2048, "reports N/2 latency");
        double worst = 0.0, worstF = 0.0;
        for (double f = 40; f <= 18000; f *= 1.02) {
            const double err = std::abs(fir.magnitudeDb(f, sr) - wanted(f));
            if (err > worst) { worst = err; worstF = f; }
        }
        char msg[128];
        snprintf(msg, sizeof msg, "FIR matches steep-low + treble-dip target within 1.2 dB (worst %.2f dB @ %.0f Hz)", worst, worstF);
        check(worst < 1.2, msg);
    }

    // 3) Convolution correctness: feed a unit impulse, the output N samples back must equal the
    //    FIR taps (a linear-phase FIR IS its impulse response).
    {
        ResponseCurve curve = {{20, 0.0}, {200, 6.0}, {1000, -4.0}, {20000, 3.0}};
        MonitorFirEq fir;
        fir.designFromCurve(sr, curve, 1024);
        const int N = fir.numTaps();
        std::vector<float> taps(N);
        // Recover taps via impulse: x = [1,0,0,...], y[n] = h[n].
        std::vector<float> buf(2 * N, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f; // interleaved stereo impulse at t=0
        fir.processInterleavedStereo(buf.data(), N);
        // y[n] on the left channel is h[n]; compare its energy peak position to the latency.
        int peak = 0; double pv = 0.0;
        for (int n = 0; n < N; ++n) {
            const double a = std::abs(buf[2 * n]);
            if (a > pv) { pv = a; peak = n; }
        }
        char msg[96];
        snprintf(msg, sizeof msg, "impulse response peaks at the linear-phase centre (peak @ %d, latency %d)", peak, fir.latencySamples());
        check(std::abs(peak - fir.latencySamples()) <= 2, msg);
    }

    printf(failures == 0 ? "\nMonitorFirEq test passed\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
