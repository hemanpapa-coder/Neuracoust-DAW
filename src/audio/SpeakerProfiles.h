#pragma once

#include "audio/MonitorCorrection.h"   // ResponseCurve

#include <string>
#include <vector>

namespace neuracoust::daw {

// Measured (Spinorama Listening Window, midband-normalized) response curves for the speaker
// catalog, baked from resources/speaker_model_dataset.json by tools/gen_speaker_profiles.py.
// A curve IS the speaker's deviation from flat — feed it to fitCurveToEqBands to model that
// speaker on the physical monitor (the virtual-monitor transform, roadmap ④).

// The curve for a catalog name (e.g. "Yamaha NS-10 (NF)"), or empty if none is measured.
ResponseCurve speakerProfileCurve(const std::string& catalogName);

// Every catalog name that has a measured curve.
std::vector<std::string> speakerProfilesWithCurve();

} // namespace neuracoust::daw
