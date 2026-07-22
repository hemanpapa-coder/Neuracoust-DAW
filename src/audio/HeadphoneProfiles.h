#pragma once

#include "audio/MonitorCorrection.h"   // ResponseCurve

#include <string>
#include <vector>

namespace neuracoust::daw {

// Measured headphone frequency-response curves, baked from the handoff package
// (docs/headphone-frequency-response-handoff, AutoEq-sourced) by tools/gen_headphone_profiles.py.
// Each curve is midband-normalized (300 Hz–3 kHz mean = 0 dB), i.e. the headphone's deviation from
// its own midband — the same convention as the speaker profiles, so it feeds fitCurveToEqBands the
// same way to model that headphone's character on the monitor path.

// The curve for a catalog name (e.g. "Sennheiser HD 600"), or empty if none is measured.
ResponseCurve headphoneProfileCurve(const std::string& catalogName);

// Every headphone catalog name that has a measured curve.
std::vector<std::string> headphoneProfilesWithCurve();

} // namespace neuracoust::daw
