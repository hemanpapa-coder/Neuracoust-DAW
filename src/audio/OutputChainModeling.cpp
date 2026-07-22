#include "audio/OutputChainModeling.h"

#include <algorithm>
#include <cmath>

namespace neuracoust::daw {
namespace {
constexpr double kPi = 3.14159265358979323846;

double highPassDb(double frequency, double cutoff) {
    const double ratio = cutoff / frequency;
    // Catalog bandwidth endpoints are commonly specified near -1.5 dB. This is a
    // deliberately conservative half-strength one-pole interpolation, not a measurement.
    return -5.0 * std::log10(1.0 + ratio * ratio);
}

double lowPassDb(double frequency, double cutoff) {
    const double ratio = frequency / cutoff;
    return -5.0 * std::log10(1.0 + ratio * ratio);
}
}

const std::vector<PowerAmpCatalogSpec>& powerAmpCatalogSpecs() {
    // Only fields backed by the catalog's cited manufacturer/spec documents are populated.
    static const std::vector<PowerAmpCatalogSpec> specs = {
        {"None", "solid_state", {}, {}, {}},
        {"Bryston 4B", "solid_state", {1.0}, {100000.0}, {500.0}},
        {"Bryston 3B", "solid_state", {1.0}, {100000.0}, {500.0}},
        {"Bryston 4B³", "solid_state", {1.0}, {100000.0}, {500.0}},
        {"Yamaha P2200", "solid_state", {10.0}, {50000.0}, {70.0}},
        {"Yamaha P2201", "solid_state", {10.0}, {50000.0}, {70.0}},
        {"Yamaha P2500S", "solid_state", {20.0}, {50000.0}, {200.0}},
        {"Crown DC-300A", "solid_state", {1.0}, {20000.0}, {200.0}},
        {"Crown D-75A", "solid_state", {20.0}, {20000.0}, {400.0}},
        {"Crown Macro-Tech 1200", "solid_state", {20.0}, {20000.0}, {1000.0}},
        {"Amcron DC-300A", "solid_state", {}, {}, {}},
        {"BGW 250", "solid_state", {}, {}, {}},
        {"BGW 750", "solid_state", {}, {}, {}},
        {"Hafler DH-200", "solid_state", {}, {}, {}},
        {"Hafler P3000", "solid_state", {}, {}, {}},
        {"Threshold S/500", "solid_state", {}, {}, {}},
        {"Pass Labs X150.8", "solid_state", {}, {}, {}},
        {"Benchmark AHB2", "solid_state", {0.1}, {200000.0}, {370.0}},
        {"McIntosh MC275", "tube", {10.0}, {100000.0}, {22.0}},
        {"Marantz 8B", "tube", {20.0}, {20000.0}, {}},
        {"Dynaco Stereo 70", "tube", {10.0}, {40000.0}, {}},
        {"Quad II", "tube", {10.0}, {20000.0}, {}},
        {"Quad 405", "solid_state", {20.0}, {20000.0}, {}},
        {"Quad 909", "solid_state", {10.0}, {20000.0}, {}},
        {"Hypex NC252MP", "class_d", {10.0}, {50000.0}, {}},
        {"Purifi 1ET400A", "class_d", {10.0}, {50000.0}, {}},
        {"ATI AT6002", "solid_state", {20.0}, {20000.0}, {}},
    };
    return specs;
}

const std::vector<SpeakerCableCatalogSpec>& speakerCableCatalogSpecs() {
    // DCR is the complete loop's effective series resistance after pairing star-quad conductors.
    static const std::vector<SpeakerCableCatalogSpec> specs = {
        {"None", {}, {}},
        {"Canare 4S8", {0.015}, {}},
        {"Canare 4S11", {0.009}, {}},
        {"Canare 4S8G", {0.015}, {}},
        {"Canare 4S11G", {0.009}, {}},
        {"Mogami 3082", {0.011}, {0.4}},
        {"Mogami 3103", {0.009}, {0.6}},
        {"Belden 5000UE", {0.0132}, {}},
        {"Belden 5T00UP", {0.0083}, {}},
        {"Belden 9497", {}, {}},
        {"Monster Cable XP", {}, {}},
        {"Kimber Kable 8TC", {}, {}},
        {"AudioQuest Rocket 33", {}, {}},
        {"generic 10 AWG OFC", {0.00656}, {0.6}},
        {"generic 12 AWG OFC", {0.0104}, {0.6}},
        {"generic 14 AWG OFC", {0.0165}, {0.6}},
        {"generic 16 AWG OFC", {0.0262}, {0.6}},
    };
    return specs;
}

ResponseCurve powerAmpCatalogToneCurve(const std::string& name) {
    const auto& all = powerAmpCatalogSpecs();
    const auto it = std::find_if(all.begin(), all.end(), [&](const auto& s) { return name == s.name; });
    if (it == all.end() || name == "None" || (!it->bandwidthLowHz && !it->bandwidthHighHz)) return {};
    ResponseCurve result;
    constexpr double frequencies[] = {20.0, 60.0, 200.0, 1000.0, 4000.0, 8000.0, 14000.0, 20000.0};
    const auto responseAt = [&](double f) {
        double db = 0.0;
        if (it->bandwidthLowHz) db += highPassDb(f, *it->bandwidthLowHz);
        if (it->bandwidthHighHz) db += lowPassDb(f, *it->bandwidthHighHz);
        return db;
    };
    const double reference = responseAt(1000.0);
    for (double f : frequencies) result.push_back({f, responseAt(f) - reference});
    const bool effectivelyFlat = std::all_of(result.begin(), result.end(), [](const auto& p) {
        return std::abs(p.second) < 0.01;
    });
    return effectivelyFlat ? ResponseCurve{} : result;
}

ResponseCurve speakerCableCatalogToneCurve(const std::string& name, double lengthM,
                                            double nominalLoadOhm) {
    const auto& all = speakerCableCatalogSpecs();
    const auto it = std::find_if(all.begin(), all.end(), [&](const auto& s) { return name == s.name; });
    if (it == all.end() || name == "None" || !it->resistanceOhmPerM || lengthM <= 0.0 || nominalLoadOhm <= 0.0)
        return {};
    const double r = *it->resistanceOhmPerM * lengthM;
    const double l = it->inductanceUhPerM.value_or(0.0) * 1.0e-6 * lengthM;
    const auto responseAt = [&](double f) {
        const double xl = 2.0 * kPi * f * l;
        const double denominator = std::hypot(nominalLoadOhm + r, xl);
        return 20.0 * std::log10(nominalLoadOhm / denominator);
    };
    const double reference = responseAt(1000.0); // level-match: retain only spectral tilt.
    ResponseCurve result;
    for (double f : {20.0, 1000.0, 8000.0, 15000.0, 20000.0})
        result.push_back({f, responseAt(f) - reference});
    const bool effectivelyFlat = std::all_of(result.begin(), result.end(), [](const auto& p) {
        return std::abs(p.second) < 0.01;
    });
    return effectivelyFlat ? ResponseCurve{} : result;
}

} // namespace neuracoust::daw
