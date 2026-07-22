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

// Harman over-ear headphone target (2018), the reference a raw headphone measurement is judged
// against: a bass shelf, flat mids, the diffuse-field ear-gain rise around 2.7 kHz, then a treble
// roll-off. Subtracting it from a raw coupler curve leaves the headphone's deviation from neutral
// (its signature), rather than the raw response that still carries the universal ear gain.
// Approximate anchor table; the treble is rig-dependent, so this is a reference, not a calibration.
double harmanHeadphoneOeTargetDb(double frequencyHz);

// Log-frequency interpolation of a curve; clamps to the endpoints outside its range.
double interpolateCurveDb(const ResponseCurve& curve, double frequencyHz);

// Normalize a curve so its 300 Hz–3 kHz mean sits at 0 dB (mic-independent reference band),
// matching how the speaker dataset is normalized.
ResponseCurve normalizeCurveMidband(const ResponseCurve& curve);

// Room-correction curve for the single (stereo) monitor EQ from per-channel measurements.
// Returns Harman_target − measured on a log grid. When BOTH channels are measured it uses their
// AVERAGE — a single EQ cannot correct L and R independently, so the average is the least-wrong
// shared correction for an asymmetric room (vs. applying one channel's correction to both). With
// one channel it uses that channel; with neither it returns empty.
ResponseCurve roomCorrectionCurve(const ResponseCurve& measuredL, const ResponseCurve& measuredR,
                                  double minHz = 20.0, double maxHz = 20000.0, int points = 96);

// Fractional-octave smoothing on a log grid: a Gaussian moving average whose width is
// `octaveFraction` of an octave (e.g. 1/6). Removes measurement noise / narrow ripple while
// keeping the broad tonal shape — the standard pre-correction step in acoustics. Returns the
// curve resampled onto `points` log-spaced frequencies across [minHz, maxHz].
ResponseCurve smoothCurveFractionalOctave(const ResponseCurve& curveDb, double octaveFraction,
                                          double minHz = 20.0, double maxHz = 20000.0, int points = 512);

// Fit a dB curve to `bandCount` peaking EQ bands at log-spaced centres — a graphic-EQ style
// approximation the ParametricEq then renders. Gains are clamped to ±maxBoostDb, and boosts
// are limited harder than cuts (you can't fill an acoustic null). The target is first smoothed
// to `smoothOctave` of an octave so the EQ tracks tonal balance, not measurement noise; the fit
// is a damped least-squares against the real cascade on a dense grid.
// `sampleRate` MUST match the rate the resulting bands will actually run at: the fit optimizes the
// band gains against the biquad magnitude response, which warps with sample rate near the top octave.
// Fitting at a fixed 48 kHz but rendering at 44.1/88.2/96 kHz left a treble error. maxHz is clamped
// just under Nyquist so no band is placed where the biquad can't render it.
std::vector<EqBandSpec> fitCurveToEqBands(const ResponseCurve& curveDb, int bandCount,
                                          double minHz = 20.0, double maxHz = 20000.0,
                                          double maxBoostDb = 6.0, double maxCutDb = 12.0,
                                          double smoothOctave = 1.0 / 6.0,
                                          double sampleRate = 48000.0);

} // namespace neuracoust::daw
