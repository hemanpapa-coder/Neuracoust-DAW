#pragma once

#include "audio/MonitorCorrection.h"
#include <optional>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct PowerAmpCatalogSpec {
    const char* name;
    const char* type;
    std::optional<double> bandwidthLowHz;
    std::optional<double> bandwidthHighHz;
    std::optional<double> dampingFactor;
};

struct SpeakerCableCatalogSpec {
    const char* name;
    std::optional<double> resistanceOhmPerM;
    std::optional<double> inductanceUhPerM;
};

const std::vector<PowerAmpCatalogSpec>& powerAmpCatalogSpecs();
const std::vector<SpeakerCableCatalogSpec>& speakerCableCatalogSpecs();
ResponseCurve powerAmpCatalogToneCurve(const std::string& name);
ResponseCurve speakerCableCatalogToneCurve(const std::string& name, double lengthM = 3.0,
                                            double nominalLoadOhm = 8.0);

} // namespace neuracoust::daw
