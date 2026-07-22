#pragma once

#include "audio/MonitorCorrection.h"   // ResponseCurve

#include <string>
#include <vector>

namespace neuracoust::daw {

// Measured audio-interface D/A frequency-response curves, baked from the in-house test reports
// (docs/audio-interface-da-modeling-research/measurements/*.html) by
// tools/gen_audio_interface_profiles.py. Each curve is midband-normalized (300 Hz–3 kHz mean = 0 dB)
// and is the interface's OWN measured deviation. The monitor path applies the INVERSE (compensation)
// when this interface is the selected output stage, so monitoring is flattened for that D/A.

// The measured FR curve for a catalog name (e.g. "Kurzweil UNiTE-2"), or empty if none is measured.
ResponseCurve audioInterfaceProfileCurve(const std::string& catalogName);

// Measured harmonic amplitudes [c2..c7] (linear, fundamental = 1) at the report's reference send
// level — seeds the interface's nonlinear waveshaper. Empty when no harmonic data is measured.
std::vector<double> audioInterfaceHarmonics(const std::string& catalogName);

// Every audio-interface catalog name that has a measured FR curve.
std::vector<std::string> audioInterfaceProfilesWithCurve();

} // namespace neuracoust::daw
