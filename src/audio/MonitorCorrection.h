#pragma once

#include "audio/ParametricEq.h"

#include <utility>
#include <vector>

namespace neuracoust::daw {

// Roadmap ③/④ shared core: turn a frequency-response curve into monitor EQ.
//   • Room correction (③): fit  Harman_target − measured_in_room  → correction bands.
//   • Virtual monitor (④):  fit  target_speaker − flat            → modelling bands.
// The output feeds the existing ParametricEq (0–64 bands), so no new render path is needed.

// A response curve: (Hz, dB) points ascending in frequency.
using ResponseCurve = std::vector<std::pair<double, double>>;

// Harman-style in-room target: flat below ~1 kHz, then a gentle downward tilt. dB relative to
// the low band. tiltDbPerOctave is the slope above the corner (Harman ≈ 0.8–1.0 dB/oct).
double harmanTargetDb(double frequencyHz, double tiltDbPerOctave = 0.9, double cornerHz = 1000.0);

// Log-frequency interpolation of a curve; clamps to the endpoints outside its range.
double interpolateCurveDb(const ResponseCurve& curve, double frequencyHz);

// Normalize a curve so its 300 Hz–3 kHz mean sits at 0 dB (mic-independent reference band),
// matching how the speaker dataset is normalized.
ResponseCurve normalizeCurveMidband(const ResponseCurve& curve);

// Fit a dB curve to `bandCount` peaking EQ bands at log-spaced centres — a graphic-EQ style
// approximation the ParametricEq then renders. Gains are clamped to ±maxBoostDb, and boosts
// are limited harder than cuts (you can't fill an acoustic null). One residual pass tightens
// the fit against band overlap.
std::vector<EqBandSpec> fitCurveToEqBands(const ResponseCurve& curveDb, int bandCount,
                                          double minHz = 20.0, double maxHz = 20000.0,
                                          double maxBoostDb = 6.0, double maxCutDb = 12.0);

} // namespace neuracoust::daw
