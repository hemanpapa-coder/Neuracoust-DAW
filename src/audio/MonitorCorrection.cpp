#include "audio/MonitorCorrection.h"

#include <algorithm>
#include <cmath>

namespace neuracoust::daw {

namespace {
constexpr double kLn2 = 0.6931471805599453;
double log2d(double x) { return std::log(std::max(1e-12, x)) / kLn2; }
} // namespace

double harmanTargetDb(double frequencyHz, double tiltDbPerOctave, double cornerHz) {
    if (frequencyHz <= cornerHz) return 0.0;
    return -tiltDbPerOctave * log2d(frequencyHz / cornerHz);
}

double harmanHeadphoneOeTargetDb(double frequencyHz) {
    // Harman OE 2018 anchor points (Hz, dB), midband-normalized near 500 Hz–1 kHz. Log-interpolated.
    static const ResponseCurve target = {
        {20, 6.1}, {30, 6.1}, {50, 5.5}, {70, 4.6}, {100, 3.6}, {150, 2.2}, {200, 1.4},
        {300, 0.5}, {400, 0.2}, {500, 0.0}, {700, -0.1}, {1000, 0.0}, {1200, 0.3},
        {1500, 1.0}, {2000, 2.7}, {2500, 4.0}, {3000, 3.8}, {3500, 2.5}, {4000, 1.0},
        {5000, -1.0}, {6000, -2.5}, {7000, -2.0}, {8000, -3.0}, {9000, -3.5},
        {10000, -3.0}, {12000, -4.5}, {15000, -6.0}, {20000, -8.0},
    };
    return interpolateCurveDb(target, frequencyHz);
}

double interpolateCurveDb(const ResponseCurve& curve, double frequencyHz) {
    if (curve.empty()) return 0.0;
    if (frequencyHz <= curve.front().first) return curve.front().second;
    if (frequencyHz >= curve.back().first) return curve.back().second;
    // Binary search for the bracketing points, interpolate in log-frequency.
    auto hi = std::lower_bound(curve.begin(), curve.end(), frequencyHz,
                               [](const std::pair<double, double>& p, double f) { return p.first < f; });
    const auto lo = hi - 1;
    const double lf = log2d(frequencyHz), l0 = log2d(lo->first), l1 = log2d(hi->first);
    const double t = (l1 > l0) ? (lf - l0) / (l1 - l0) : 0.0;
    return lo->second + t * (hi->second - lo->second);
}

ResponseCurve normalizeCurveMidband(const ResponseCurve& curve) {
    double sum = 0.0;
    int n = 0;
    for (const auto& [f, db] : curve) {
        if (f >= 300.0 && f <= 3000.0) { sum += db; ++n; }
    }
    const double mean = n > 0 ? sum / n : 0.0;
    ResponseCurve out = curve;
    for (auto& [f, db] : out) db -= mean;
    return out;
}

ResponseCurve roomCorrectionCurve(const ResponseCurve& measuredL, const ResponseCurve& measuredR,
                                  double minHz, double maxHz, int points) {
    ResponseCurve room;
    const bool haveL = !measuredL.empty();
    const bool haveR = !measuredR.empty();
    if ((!haveL && !haveR) || points <= 0) return room;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    room.reserve(static_cast<size_t>(points));
    for (int i = 0; i < points; ++i) {
        const double f = lo * std::pow(hi / lo, static_cast<double>(i) / (points - 1));
        double measured;
        if (haveL && haveR) {
            measured = 0.5 * (interpolateCurveDb(measuredL, f) + interpolateCurveDb(measuredR, f));
        } else {
            measured = interpolateCurveDb(haveL ? measuredL : measuredR, f);
        }
        room.push_back({f, harmanTargetDb(f) - measured});
    }
    return room;
}

ResponseCurve smoothCurveFractionalOctave(const ResponseCurve& curveDb, double octaveFraction,
                                          double minHz, double maxHz, int points) {
    ResponseCurve out;
    if (curveDb.empty() || points <= 0) return out;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    // Resample onto a log grid first, so a fixed bin window is a fixed octave fraction.
    std::vector<double> gf(points), src(points);
    for (int k = 0; k < points; ++k) {
        gf[k] = lo * std::pow(hi / lo, static_cast<double>(k) / (points - 1));
        src[k] = interpolateCurveDb(curveDb, gf[k]);
    }
    if (octaveFraction <= 0.0) {
        for (int k = 0; k < points; ++k) out.push_back({gf[k], src[k]});
        return out;
    }
    const double octaves = log2d(hi / lo);
    const double binsPerOct = (points - 1) / std::max(1e-9, octaves);
    // Gaussian sigma = half the smoothing window (in bins); ±3σ kernel.
    const double sigma = std::max(0.5, octaveFraction * binsPerOct * 0.5);
    const int half = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
    std::vector<double> kernel(2 * half + 1);
    double ksum = 0.0;
    for (int d = -half; d <= half; ++d) {
        const double w = std::exp(-0.5 * (d / sigma) * (d / sigma));
        kernel[d + half] = w;
        ksum += w;
    }
    out.reserve(points);
    for (int k = 0; k < points; ++k) {
        double acc = 0.0, wsum = 0.0;
        for (int d = -half; d <= half; ++d) {
            const int j = std::clamp(k + d, 0, points - 1);   // edge-clamp
            const double w = kernel[d + half];
            acc += w * src[j];
            wsum += w;
        }
        out.push_back({gf[k], acc / (wsum > 0 ? wsum : 1.0)});
    }
    (void)ksum;
    return out;
}

std::vector<EqBandSpec> fitCurveToEqBands(const ResponseCurve& curveDbRaw, int bandCount,
                                          double minHz, double maxHz,
                                          double maxBoostDb, double maxCutDb,
                                          double smoothOctave, double sampleRate) {
    std::vector<EqBandSpec> bands;
    if (curveDbRaw.empty() || bandCount <= 0) return bands;
    bandCount = std::min<int>(bandCount, static_cast<int>(ParametricEq::kMaxBands));
    const double fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
    // Keep every band centre safely below Nyquist so the biquad can render it (a band above Nyquist
    // has no meaning); 0.45·fs leaves a guard for the bell's upper skirt.
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, std::min(maxHz, fs * 0.45));
    // Smooth the target to a fraction of an octave before fitting, so the EQ tracks the tonal
    // balance and not the measurement noise / narrow ripple in the source curve. Without this the
    // dense fit faithfully reproduces every jag in the dataset, which is exactly the wobble we
    // don't want the monitor EQ to chase.
    const ResponseCurve curveDb = smoothOctave > 0.0
        ? smoothCurveFractionalOctave(curveDbRaw, smoothOctave, lo, hi, 512)
        : curveDbRaw;
    const double octaves = log2d(hi / lo);
    // Band centres span the full range now that ParametricEq renders peaking bands with matched-Z
    // coefficients — those keep the magnitude accurate to Nyquist, so a top-octave band no longer
    // cramps into a sharp ring and needs no cap or taper.
    const double octPerBand = bandCount > 1 ? octaves / (bandCount - 1) : octaves;
    // Peaking Q so adjacent bands cross near -3 dB: BW = octPerBand.
    const double bw = std::max(0.1, octPerBand);
    const double q = std::sqrt(std::pow(2.0, bw)) / (std::pow(2.0, bw) - 1.0);

    auto clampGain = [&](double g) { return std::max(-maxCutDb, std::min(maxBoostDb, g)); };

    std::vector<double> centres(bandCount);
    for (int i = 0; i < bandCount; ++i) centres[i] = lo * std::pow(2.0, octPerBand * i);

    // Dense evaluation grid. The old fit only checked error AT the band centres, so the response
    // between them was free to ripple — worst in the treble where the target has fine features.
    // Fitting the whole grid pins the curve everywhere.
    const int gridN = std::max(256, bandCount * 8);
    std::vector<double> gf(gridN), target(gridN);
    for (int k = 0; k < gridN; ++k) {
        gf[k] = lo * std::pow(hi / lo, static_cast<double>(k) / (gridN - 1));
        target[k] = interpolateCurveDb(curveDb, gf[k]);
    }
    // Each band's normalized bell shape (dB per dB of gain), so a band is only asked to correct
    // the region it actually affects — this is the sensitivity that turns the per-band update
    // into a damped least-squares step over the whole grid.
    const double probe = 12.0;
    std::vector<std::vector<double>> bell(bandCount, std::vector<double>(gridN));
    std::vector<double> bellNorm(bandCount, 1e-9);
    for (int i = 0; i < bandCount; ++i) {
        ParametricEq one;
        one.configure(fs, {{true, EqBandType::Peaking, centres[i], probe, q}});
        for (int k = 0; k < gridN; ++k) {
            const double s = one.magnitudeDb(gf[k]) / probe;
            bell[i][k] = s;
            bellNorm[i] += s * s;
        }
    }

    for (int i = 0; i < bandCount; ++i)
        bands.push_back({true, EqBandType::Peaking, centres[i], clampGain(interpolateCurveDb(curveDb, centres[i])), q});

    // Damped least-squares against the REAL cascade on the dense grid, PLUS a smoothness penalty
    // that pulls each band toward its neighbours' average gain. Near Nyquist the biquad bells are
    // cramped and the bare LS solution is ill-conditioned — adjacent bands swing in opposite
    // directions and sum into a deep ringing notch (the sharp treble spike the user saw). The
    // smoothness term couples neighbours so they can't alternate, killing the notch while the
    // already 1/6-octave-smoothed target keeps real (broad) features intact.
    const double lambdaSmooth = 0.30;
    std::vector<double> achieved(gridN);
    for (int pass = 0; pass < 60; ++pass) {
        ParametricEq eq;
        eq.configure(fs, bands);
        double worst = 0.0;
        for (int k = 0; k < gridN; ++k) {
            achieved[k] = eq.magnitudeDb(gf[k]);
            worst = std::max(worst, std::abs(target[k] - achieved[k]));
        }
        for (int i = 0; i < bandCount; ++i) {
            double num = 0.0;
            for (int k = 0; k < gridN; ++k) num += bell[i][k] * (target[k] - achieved[k]);
            const double grad = num / bellNorm[i];
            const double neighbour =
                (i > 0 && i < bandCount - 1) ? 0.5 * (bands[i - 1].gainDb + bands[i + 1].gainDb)
                : (i > 0) ? bands[i - 1].gainDb
                : (bandCount > 1) ? bands[i + 1].gainDb
                : bands[i].gainDb;
            const double reg = lambdaSmooth * (neighbour - bands[i].gainDb);
            bands[i].gainDb = clampGain(bands[i].gainDb + 0.5 * grad + reg);
        }
        if (worst < 0.05) break;
    }
    return bands;
}

} // namespace neuracoust::daw
