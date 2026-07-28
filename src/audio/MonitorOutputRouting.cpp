#include "audio/MonitorOutputRouting.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace neuracoust::daw {
namespace {

int activeSpeakerSlot(const std::vector<MonitorDspModule>& modules) {
    if (modules.empty()) {
        return 0;
    }
    return std::max(0, std::min(2, modules.front().activeTargetSlot));
}

const std::string& routeForSlot(const MonitorDspModule& module, int slot) {
    if (slot == 1) {
        return module.speakerOutputB;
    }
    if (slot == 2) {
        return module.speakerOutputC;
    }
    return module.speakerOutputA;
}

bool routeIsNone(const std::string& route);

std::string effectiveRouteForSlot(const MonitorDspModule& module, int slot) {
    return routeForSlot(module, slot);
}

std::string slotName(int slot) {
    if (slot == 1) {
        return "Speaker B";
    }
    if (slot == 2) {
        return "Speaker C";
    }
    return "Speaker A";
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool routeIsNone(const std::string& route) {
    const auto trimmed = trim(route);
    return trimmed.empty() || trimmed == "None" || trimmed == "없음";
}

bool parseRoutePair(const std::string& route, int& leftChannel, int& rightChannel) {
    if (routeIsNone(route)) {
        return false;
    }
    const auto dash = route.find('-');
    if (dash == std::string::npos) {
        return false;
    }
    size_t firstDigit = 0;
    while (firstDigit < route.size() && !std::isdigit(static_cast<unsigned char>(route[firstDigit]))) {
        ++firstDigit;
    }
    if (firstDigit >= dash) {
        return false;
    }
    char* end = nullptr;
    const long first = std::strtol(route.c_str() + firstDigit, &end, 10);
    if (end == route.c_str() + firstDigit || first < 1) {
        return false;
    }
    size_t secondDigit = dash + 1;
    while (secondDigit < route.size() && !std::isdigit(static_cast<unsigned char>(route[secondDigit]))) {
        ++secondDigit;
    }
    if (secondDigit >= route.size()) {
        return false;
    }
    end = nullptr;
    const long second = std::strtol(route.c_str() + secondDigit, &end, 10);
    if (end == route.c_str() + secondDigit || second < first) {
        return false;
    }
    leftChannel = static_cast<int>(first - 1);
    rightChannel = static_cast<int>(second - 1);
    return true;
}

std::string compactRouteName(const std::string& route) {
    const auto trimmed = trim(route);
    if (trimmed == "Output 1-2") {
        return "Main 1-2";
    }
    return trimmed.empty() ? "None" : trimmed;
}

} // namespace

int monitorOutputRequiredChannels(const std::vector<MonitorDspModule>& modules) {
    if (modules.empty()) {
        return 2;
    }
    int required = 2;
    const auto& module = modules.front();
    for (int slot = 0; slot < 3; ++slot) {
        int left = 0;
        int right = 1;
        if (parseRoutePair(effectiveRouteForSlot(module, slot), left, right)) {
            required = std::max(required, right + 1);
        }
    }
    {
        int left = 0;
        int right = 1;
        if (parseRoutePair(module.headphoneOutput, left, right)) {
            required = std::max(required, right + 1);
        }
    }
    return required;
}

MonitorOutputRoute resolveMonitorOutputRoute(const std::vector<MonitorDspModule>& modules, int availableChannels) {
    MonitorOutputRoute result;
    result.activeSlot = activeSpeakerSlot(modules);
    if (modules.empty()) {
        result.available = availableChannels >= 2;
        return result;
    }

    const auto& module = modules.front();

    // Headphone destination: its OWN route, never the speaker slots'. The 스피커/헤드폰 tab
    // used to change only the DSP context while the audio kept leaving on the active speaker
    // slot's pair — with speakers and headphones on different interface outputs, "headphone"
    // still fed the speakers.
    if (module.monitorToHeadphone) {
        const auto route = compactRouteName(module.headphoneOutput);
        result.requestedRoute = route;
        result.displayRoute = route;
        result.description = "Headphone -> " + route;
        if (routeIsNone(route)) {
            result.assigned = true;
            result.available = availableChannels >= 2;
            result.leftChannel = 0;
            result.rightChannel = 1;
            result.displayRoute = "Main 1-2";
            result.description = "Headphone -> Main 1-2";
            return result;
        }
        int left = 0;
        int right = 1;
        if (!parseRoutePair(route, left, right)) {
            result.assigned = true;
            result.available = availableChannels >= 2;
            result.leftChannel = 0;
            result.rightChannel = 1;
            return result;
        }
        result.leftChannel = left;
        result.rightChannel = right;
        result.available = right < availableChannels;
        if (!result.available) {
            result.description += " (장치 채널 부족)";
        }
        return result;
    }

    const auto route = compactRouteName(effectiveRouteForSlot(module, result.activeSlot));
    result.requestedRoute = route;
    result.displayRoute = route;
    result.description = slotName(result.activeSlot) + " -> " + route;

    if (routeIsNone(route)) {
        // "None" is the modelled/virtual path, not a mute: the speaker-simulation DSP is
        // applied and its output still leaves on the main L/R pair. Only an explicit
        // physical route (Main 1-2, Output N-N) moves it onto other channels. Treating
        // None as silent black-holed every modelled speaker — selecting a speaker model
        // sets the slot's route to "None", so the monitor went completely dead the moment
        // a model was chosen.
        result.assigned = true;
        result.available = availableChannels >= 2;
        result.leftChannel = 0;
        result.rightChannel = 1;
        result.displayRoute = "Main 1-2";
        result.description = slotName(result.activeSlot) + " -> modelled (Main 1-2)";
        return result;
    }

    int left = 0;
    int right = 1;
    if (!parseRoutePair(route, left, right)) {
        result.available = availableChannels >= 2;
        result.displayRoute = "Main 1-2";
        result.description = slotName(result.activeSlot) + " -> " + route + " unavailable; using Main 1-2";
        return result;
    }

    result.leftChannel = left;
    result.rightChannel = right;
    result.available = right < availableChannels;
    if (!result.available) {
        result.leftChannel = 0;
        result.rightChannel = std::min(1, std::max(0, availableChannels - 1));
        result.displayRoute = "Main 1-2";
        result.description = slotName(result.activeSlot) + " -> " + route + " unavailable; using Main 1-2";
    }
    return result;
}

} // namespace neuracoust::daw
