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

std::vector<EqBandSpec> fitCurveToEqBands(const ResponseCurve& curveDb, int bandCount,
                                          double minHz, double maxHz,
                                          double maxBoostDb, double maxCutDb) {
    std::vector<EqBandSpec> bands;
    if (curveDb.empty() || bandCount <= 0) return bands;
    bandCount = std::min<int>(bandCount, static_cast<int>(ParametricEq::kMaxBands));
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    const double octaves = log2d(hi / lo);
    const double octPerBand = bandCount > 1 ? octaves / (bandCount - 1) : octaves;
    // Peaking Q so adjacent bands cross near -3 dB: BW = octPerBand.
    const double bw = std::max(0.1, octPerBand);
    const double q = std::sqrt(std::pow(2.0, bw)) / (std::pow(2.0, bw) - 1.0);

    auto clampGain = [&](double g) { return std::max(-maxCutDb, std::min(maxBoostDb, g)); };

    std::vector<double> centres(bandCount), desired(bandCount);
    for (int i = 0; i < bandCount; ++i) {
        centres[i] = lo * std::pow(2.0, octPerBand * i);
        desired[i] = interpolateCurveDb(curveDb, centres[i]);
        bands.push_back({true, EqBandType::Peaking, centres[i], clampGain(desired[i]), q});
    }

    // Residual passes: overlapping peaks over-boost, so measure what the chain actually does at
    // each centre and correct toward the target. A few damped passes converge the graphic-EQ
    // fit against the overlap.
    for (int pass = 0; pass < 6; ++pass) {
        ParametricEq eq;
        eq.configure(48000.0, bands);
        double worst = 0.0;
        for (int i = 0; i < bandCount; ++i) {
            const double achieved = eq.magnitudeDb(centres[i]);
            const double residual = desired[i] - achieved;
            worst = std::max(worst, std::abs(residual));
            bands[i].gainDb = clampGain(bands[i].gainDb + 0.7 * residual);
        }
        if (worst < 0.1) break;
    }
    return bands;
}

} // namespace neuracoust::daw
