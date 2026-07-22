#include "audio/OutputChainModeling.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace neuracoust::daw;

int main() {
    // Unknown/None has no curve: the monitor combiner therefore remains bit-transparent.
    assert(powerAmpCatalogToneCurve("None").empty());
    assert(speakerCableCatalogToneCurve("None").empty());
    assert(powerAmpCatalogToneCurve("not in catalog").empty());

    // P2500S's published 20 Hz low boundary gives a small, deterministic LF tilt,
    // normalized at 1 kHz. No name-based "warmth" is involved.
    const auto amp = powerAmpCatalogToneCurve("Yamaha P2500S");
    assert(!amp.empty());
    const double amp20 = interpolateCurveDb(amp, 20.0);
    const double amp1k = interpolateCurveDb(amp, 1000.0);
    assert(amp20 < -1.4 && amp20 > -1.7);
    assert(std::abs(amp1k) < 1.0e-9);

    // Even a deliberately demanding 10 m 16-AWG run into 4 ohms is level-matched;
    // the calculated HF change stays small rather than inventing cable colour.
    const auto cable = speakerCableCatalogToneCurve("generic 16 AWG OFC", 10.0, 4.0);
    assert(!cable.empty());
    const double cable20k = interpolateCurveDb(cable, 20000.0);
    assert(cable20k < -0.05 && cable20k > -0.5);
    assert(std::abs(interpolateCurveDb(cable, 1000.0)) < 1.0e-9);

    std::cout << "output chain modeling tests passed\n";
    return 0;
}
