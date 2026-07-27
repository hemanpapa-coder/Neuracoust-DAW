#include "audio/MonitorDspProcessor.h"
#include "audio/HeadphoneProfiles.h"
#include "audio/SpeakerProfiles.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <string>

namespace neuracoust::daw {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSqrtHalf = 0.7071067811865476f;
constexpr float kSpeakerModelDeltaGain = 9.2f;
constexpr float kSpeakerModelDeltaMin = -2.0f;
constexpr float kSpeakerModelDeltaMax = 2.9f;
constexpr float kStateSilenceThreshold = 1.0e-20f;

float realtimeSafeSample(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

float flushTinySample(float value) {
    return std::abs(value) < kStateSilenceThreshold ? 0.0f : value;
}

bool moduleEnabled(const MonitorDspModule& module, const char* id) {
    return module.enabled && module.id == id;
}

bool routeIsNone(const std::string& route) {
    return route.empty() || route == "None" || route == "없음";
}

const std::string& speakerOutputForSlot(const MonitorDspModule& module, int slot) {
    if (slot == 1) {
        return module.speakerOutputB;
    }
    if (slot == 2) {
        return module.speakerOutputC;
    }
    return module.speakerOutputA;
}

bool speakerRoomEqForSlot(const MonitorDspModule& module, int slot) {
    if (slot == 1) {
        return module.speakerRoomEqB;
    }
    if (slot == 2) {
        return module.speakerRoomEqC;
    }
    return module.speakerRoomEqA;
}

bool speakerRoomEqForActivePhysicalOutput(const MonitorDspModule& module) {
    const int slot = std::max(0, std::min(2, module.activeTargetSlot));
    return speakerRoomEqForSlot(module, slot);
}

float speakerSimulationWeightForActiveSlot(const MonitorDspModule& module) {
    if (module.activeTargetSlot == 1) {
        return std::max(-0.5f, std::min(1.0f, module.speakerSimulationWeightB));
    }
    if (module.activeTargetSlot == 2) {
        return std::max(-0.5f, std::min(1.0f, module.speakerSimulationWeightC));
    }
    return std::max(-0.5f, std::min(1.0f, module.speakerSimulationWeightA));
}

bool audibleModuleEnabled(const MonitorDspModule& module) {
    const bool hasStreamingPreview =
        module.streamingPreview == "YouTube" ||
        module.streamingPreview == "Spotify" ||
        module.streamingPreview == "Tidal" ||
        module.streamingPreview == "Melon" ||
        module.streamingPreview == "Bugs";
    return module.enabled &&
           (hasStreamingPreview ||
            module.id == "speaker-simulation" ||
            module.id == "headphone-simulation" ||
            module.id == "graphic-eq" ||
            module.id == "room-correction" ||
            module.id == "crossfeed");
}

float modelAmount(const std::string& model, const std::string& token, float amount) {
    return model.find(token) != std::string::npos ? amount : 0.0f;
}

float speakerToneScore(const std::string& model) {
    if (model.find("Flat") != std::string::npos || model.find("Off") != std::string::npos) {
        return 0.0f;
    }
    float score = 0.0f;
    score += modelAmount(model, "NS-10", 0.24f);
    score += modelAmount(model, "Auratone", 0.42f);
    score += modelAmount(model, "Avantone", 0.38f);
    score += modelAmount(model, "MixCube", 0.36f);
    score += modelAmount(model, "Laptop", 0.48f);
    score += modelAmount(model, "Phone", 0.46f);
    score += modelAmount(model, "TV", 0.30f);
    score += modelAmount(model, "Bluetooth", 0.32f);
    score += modelAmount(model, "YouTube", 0.18f);
    score += modelAmount(model, "Spotify", 0.14f);
    score += modelAmount(model, "Apple Music", 0.10f);
    score += modelAmount(model, "Tidal", 0.04f);
    score += modelAmount(model, "Melon", 0.13f);
    score += modelAmount(model, "Bugs", 0.11f);
    score += modelAmount(model, "Broadcast", 0.16f);
    score += modelAmount(model, "HS5", 0.10f);
    score += modelAmount(model, "HS7", 0.07f);
    score += modelAmount(model, "HS8", 0.04f);
    score += modelAmount(model, "MSP", 0.06f);
    score += modelAmount(model, "KRK", 0.09f);
    score += modelAmount(model, "Rokit", 0.10f);
    score += modelAmount(model, "JBL", 0.06f);
    score += modelAmount(model, "Mackie", 0.07f);
    score += modelAmount(model, "PreSonus", 0.08f);
    score += modelAmount(model, "Genelec 80", 0.02f);
    score += modelAmount(model, "Genelec 83", -0.02f);
    score += modelAmount(model, "Genelec 10", -0.03f);
    score += modelAmount(model, "Neumann KH", -0.04f);
    score += modelAmount(model, "Focal", -0.05f);
    score += modelAmount(model, "Adam", -0.03f);
    score += modelAmount(model, "ADAM", -0.03f);
    score += modelAmount(model, "Dynaudio", -0.035f);
    score += modelAmount(model, "Barefoot", -0.07f);
    score += modelAmount(model, "ATC", -0.06f);
    score += modelAmount(model, "PMC", -0.055f);
    score += modelAmount(model, "Quested", -0.05f);
    score += modelAmount(model, "Ocean Way", -0.065f);
    score += modelAmount(model, "Meyer", -0.06f);
    score += modelAmount(model, "Augspurger", -0.08f);
    score += modelAmount(model, "M2", -0.07f);
    score += modelAmount(model, "Large", -0.04f);
    score += modelAmount(model, "(LF)", -0.045f);
    score += modelAmount(model, "(MF)", -0.025f);
    score += modelAmount(model, "(NF)", 0.015f);
    return std::clamp(score, -0.28f, 0.55f);
}

std::string activeTargetModel(const MonitorDspModule& module) {
    if (module.activeTargetSlot == 1) {
        return module.targetModelB;
    }
    if (module.activeTargetSlot == 2) {
        return module.targetModelC;
    }
    return module.targetModelA;
}

// The REAL speaker for the active slot; falls back to the single realModel (old projects).
std::string activeRealModel(const MonitorDspModule& module) {
    const std::string& slotReal = module.activeTargetSlot == 1 ? module.realModelB
                                 : module.activeTargetSlot == 2 ? module.realModelC
                                 : module.realModelA;
    return slotReal.empty() ? module.realModel : slotReal;
}

std::string canonicalSpeakerModelName(const std::string& model) {
    std::string value = model;
    const char* prefixes[] = {
        "Real Speaker:", "Speaker A:", "Speaker B:", "Speaker C:",
        "Real:", "A:", "B:", "C:", "실제:", "스피커 A:", "스피커 B:", "스피커 C:"
    };
    for (const char* prefix : prefixes) {
        const auto pos = value.find(prefix);
        if (pos != std::string::npos) {
            value.erase(pos, std::strlen(prefix));
        }
    }
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            normalized.push_back(static_cast<char>(std::toupper(ch)));
        }
    }
    for (const char* suffix : {"NF", "MF", "LF"}) {
        const auto suffixLength = std::strlen(suffix);
        if (normalized.size() > suffixLength && normalized.compare(normalized.size() - suffixLength, suffixLength, suffix) == 0) {
            normalized.erase(normalized.size() - suffixLength);
        }
    }
    if (normalized == "CLA10A" ||
        normalized == "AVANTONECLA10A" ||
        normalized == "AVANTONECLA10AACTIVE" ||
        normalized == "AVANTONEPROCLA10A" ||
        normalized == "AVANTONEPROCLA10AACTIVE" ||
        normalized == "AVANTONEPROCLA10ALIMITEDEDITION" ||
        normalized == "CLA10AACTIVE") {
        return "AVANTONECLA10A";
    }
    if (normalized == "CLA10" || normalized == "AVANTONECLA10" ||
        normalized == "AVANTONEPROCLA10" ||
        normalized == "AVANTONEPROCLA10PASSIVE") {
        return "AVANTONECLA10";
    }
    if (normalized == "AVANTONEPROCLA10ACTIVE") {
        return "AVANTONECLA10ACTIVE";
    }
    if (normalized == "AVANTONEPROMIXCUBEACTIVE") {
        return "AVANTONEMIXCUBEACTIVE";
    }
    if (normalized == "AVANTONEPROMIXCUBEPASSIVE" ||
        normalized == "AVANTONEMIXCUBE") {
        return "AVANTONEMIXCUBEPASSIVE";
    }
    if (normalized == "AVANTONEPROGAUSS7") {
        return "AVANTONEGAUSS7";
    }
    if (normalized == "NS10" || normalized == "YAMAHANS10") {
        return "YAMAHANS10";
    }
    if (normalized == "NS10M" || normalized == "YAMAHANS10M") {
        return "YAMAHANS10M";
    }
    if (normalized == "NS10MPRO" || normalized == "YAMAHANS10MPRO") {
        return "YAMAHANS10MPRO";
    }
    if (normalized == "NS10MSTUDIO" || normalized == "YAMAHANS10MSTUDIO" ||
        normalized == "NS10MMONITOR" || normalized == "YAMAHANS10MMONITOR") {
        return "YAMAHANS10MSTUDIO";
    }
    return normalized;
}

bool speakerTargetMatchesReal(const MonitorDspModule& module) {
    const auto real = canonicalSpeakerModelName(activeRealModel(module));
    const auto target = canonicalSpeakerModelName(activeTargetModel(module));
    return !real.empty() && real == target;
}

bool speakerTargetMatchesPhysicalReal(const MonitorDspModule& module) {
    const auto real = canonicalSpeakerModelName(activeRealModel(module));
    const auto target = canonicalSpeakerModelName(activeTargetModel(module));
    return !real.empty() && real != "FLAT" && real != "OFF" && real == target;
}

float speakerTargetTone(const MonitorDspModule& module) {
    const std::string active = activeTargetModel(module);
    if (speakerTargetMatchesReal(module)) {
        return 0.0f;
    }
    const float targetScore = speakerToneScore(active);
    const float realScore = speakerToneScore(activeRealModel(module));
    return std::clamp((targetScore - realScore) * kSpeakerModelDeltaGain,
                      kSpeakerModelDeltaMin,
                      kSpeakerModelDeltaMax);
}

float realSpeakerCompensation(const MonitorDspModule& module) {
    return modelAmount(activeRealModel(module), "Custom", 0.02f);
}

float speakerLevelMatchGain(float targetTone) {
    const float darkerOrSmallerTarget = std::max(0.0f, targetTone);
    const float brighterOrLargerTarget = std::max(0.0f, -targetTone);
    return std::clamp(1.015f + darkerOrSmallerTarget * 0.085f + brighterOrLargerTarget * 0.025f,
                      0.98f,
                      1.22f);
}

float softLimit(float value) {
    return std::tanh(value * 1.08f) / std::tanh(1.08f);
}

float streamingPreviewTone(const MonitorDspModule& module) {
    if (module.streamingPreview == "YouTube") {
        return 0.26f;
    }
    if (module.streamingPreview == "Spotify") {
        return 0.18f;
    }
    if (module.streamingPreview == "Tidal") {
        return 0.055f;
    }
    if (module.streamingPreview == "Melon") {
        return 0.16f;
    }
    if (module.streamingPreview == "Bugs") {
        return 0.13f;
    }
    return 0.0f;
}

StereoFrame applyStreamingPreview(StereoFrame frame, const MonitorDspModule& module) {
    if (module.streamingPreview == "Off" || module.streamingPreview.empty()) {
        return frame;
    }
    const float tone = streamingPreviewTone(module);
    const float mono = (frame.left + frame.right) * 0.5f;
    float sideScale = 0.90f;
    if (module.streamingPreview == "Tidal") {
        sideScale = 0.985f;
    } else if (module.streamingPreview == "Spotify") {
        sideScale = 0.94f;
    } else if (module.streamingPreview == "Melon") {
        sideScale = 0.925f;
    } else if (module.streamingPreview == "Bugs") {
        sideScale = 0.935f;
    }
    frame.left = mono + (frame.left - mono) * sideScale;
    frame.right = mono + (frame.right - mono) * sideScale;
    frame.left = softLimit(frame.left * (1.0f + tone * 0.08f)) * (1.0f - tone * 0.025f);
    frame.right = softLimit(frame.right * (1.0f + tone * 0.08f)) * (1.0f - tone * 0.025f);
    return frame;
}

const MonitorDspModule* activeStreamingPreviewModule(const std::vector<MonitorDspModule>& modules) {
    auto it = std::find_if(modules.begin(), modules.end(), [](const MonitorDspModule& module) {
        return module.enabled &&
               (module.streamingPreview == "YouTube" ||
               module.streamingPreview == "Spotify" ||
               module.streamingPreview == "Tidal" ||
               module.streamingPreview == "Melon" ||
               module.streamingPreview == "Bugs");
    });
    return it == modules.end() ? nullptr : &(*it);
}

float headphoneCrossfeedAmount(const MonitorDspModule& module) {
    const std::string combined = activeRealModel(module) + " " + activeTargetModel(module);
    float amount = 0.085f;
    amount += modelAmount(combined, "Open", -0.018f);
    amount += modelAmount(combined, "Closed", 0.018f);
    amount += modelAmount(combined, "Speaker A", 0.012f);
    amount += modelAmount(combined, "Speaker B", 0.018f);
    amount += modelAmount(combined, "Speaker C", 0.024f);
    return std::clamp(amount, 0.04f, 0.16f);
}

} // namespace

float MonitorDspProcessor::Biquad::process(float input) {
    input = realtimeSafeSample(input);
    float output = b0 * input + z1;
    if (!std::isfinite(output)) {
        reset();
        return 0.0f;
    }
    z1 = b1 * input - a1 * output + z2;
    z2 = b2 * input - a2 * output;
    if (!std::isfinite(z1) || !std::isfinite(z2)) {
        reset();
    } else {
        z1 = flushTinySample(z1);
        z2 = flushTinySample(z2);
    }
    output = flushTinySample(output);
    return output;
}

void MonitorDspProcessor::Biquad::reset() {
    z1 = 0.0f;
    z2 = 0.0f;
}

void MonitorDspProcessor::configure(double sampleRate, std::vector<MonitorDspModule> modules) {
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    modules_ = std::move(modules);
    hasAudibleProcessing_ = std::any_of(modules_.begin(), modules_.end(), audibleModuleEnabled);
    configureFilterState();
}

unsigned int MonitorDspProcessor::reportedLatencySamples() const {
    return 0;
}

MonitorDspProcessor::Biquad MonitorDspProcessor::makeLowPass(float frequencyHz, float q) const {
    const float frequency = std::clamp(frequencyHz, 10.0f, static_cast<float>(sampleRate_ * 0.45));
    const float omega = 2.0f * kPi * frequency / static_cast<float>(sampleRate_);
    const float alpha = std::sin(omega) / (2.0f * std::max(0.05f, q));
    const float cosOmega = std::cos(omega);
    const float a0 = 1.0f + alpha;
    return {
        ((1.0f - cosOmega) * 0.5f) / a0,
        (1.0f - cosOmega) / a0,
        ((1.0f - cosOmega) * 0.5f) / a0,
        (-2.0f * cosOmega) / a0,
        (1.0f - alpha) / a0
    };
}

MonitorDspProcessor::Biquad MonitorDspProcessor::makeHighPass(float frequencyHz, float q) const {
    const float frequency = std::clamp(frequencyHz, 10.0f, static_cast<float>(sampleRate_ * 0.45));
    const float omega = 2.0f * kPi * frequency / static_cast<float>(sampleRate_);
    const float alpha = std::sin(omega) / (2.0f * std::max(0.05f, q));
    const float cosOmega = std::cos(omega);
    const float a0 = 1.0f + alpha;
    return {
        ((1.0f + cosOmega) * 0.5f) / a0,
        (-(1.0f + cosOmega)) / a0,
        ((1.0f + cosOmega) * 0.5f) / a0,
        (-2.0f * cosOmega) / a0,
        (1.0f - alpha) / a0
    };
}

MonitorDspProcessor::Biquad MonitorDspProcessor::makeLowShelf(float frequencyHz, float gainDb, float slope) const {
    const float frequency = std::clamp(frequencyHz, 10.0f, static_cast<float>(sampleRate_ * 0.45));
    const float a = std::pow(10.0f, gainDb / 40.0f);
    const float omega = 2.0f * kPi * frequency / static_cast<float>(sampleRate_);
    const float sinOmega = std::sin(omega);
    const float cosOmega = std::cos(omega);
    const float safeSlope = std::max(0.05f, slope);
    const float alpha = sinOmega * 0.5f * std::sqrt((a + 1.0f / a) * (1.0f / safeSlope - 1.0f) + 2.0f);
    const float twoRootAAlpha = 2.0f * std::sqrt(a) * alpha;
    const float a0 = (a + 1.0f) + (a - 1.0f) * cosOmega + twoRootAAlpha;
    return {
        a * ((a + 1.0f) - (a - 1.0f) * cosOmega + twoRootAAlpha) / a0,
        2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosOmega) / a0,
        a * ((a + 1.0f) - (a - 1.0f) * cosOmega - twoRootAAlpha) / a0,
        (-2.0f * ((a - 1.0f) + (a + 1.0f) * cosOmega)) / a0,
        ((a + 1.0f) + (a - 1.0f) * cosOmega - twoRootAAlpha) / a0
    };
}

MonitorDspProcessor::Biquad MonitorDspProcessor::makeHighShelf(float frequencyHz, float gainDb, float slope) const {
    const float frequency = std::clamp(frequencyHz, 10.0f, static_cast<float>(sampleRate_ * 0.45));
    const float a = std::pow(10.0f, gainDb / 40.0f);
    const float omega = 2.0f * kPi * frequency / static_cast<float>(sampleRate_);
    const float sinOmega = std::sin(omega);
    const float cosOmega = std::cos(omega);
    const float safeSlope = std::max(0.05f, slope);
    const float alpha = sinOmega * 0.5f * std::sqrt((a + 1.0f / a) * (1.0f / safeSlope - 1.0f) + 2.0f);
    const float twoRootAAlpha = 2.0f * std::sqrt(a) * alpha;
    const float a0 = (a + 1.0f) - (a - 1.0f) * cosOmega + twoRootAAlpha;
    return {
        a * ((a + 1.0f) + (a - 1.0f) * cosOmega + twoRootAAlpha) / a0,
        -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cosOmega) / a0,
        a * ((a + 1.0f) + (a - 1.0f) * cosOmega - twoRootAAlpha) / a0,
        (2.0f * ((a - 1.0f) - (a + 1.0f) * cosOmega)) / a0,
        ((a + 1.0f) - (a - 1.0f) * cosOmega - twoRootAAlpha) / a0
    };
}

MonitorDspProcessor::Biquad MonitorDspProcessor::makePeaking(float frequencyHz, float gainDb, float q) const {
    const float frequency = std::clamp(frequencyHz, 10.0f, static_cast<float>(sampleRate_ * 0.45));
    const float a = std::pow(10.0f, gainDb / 40.0f);
    const float omega = 2.0f * kPi * frequency / static_cast<float>(sampleRate_);
    const float alpha = std::sin(omega) / (2.0f * std::max(0.05f, q));
    const float cosOmega = std::cos(omega);
    const float a0 = 1.0f + alpha / a;
    return {
        (1.0f + alpha * a) / a0,
        (-2.0f * cosOmega) / a0,
        (1.0f - alpha * a) / a0,
        (-2.0f * cosOmega) / a0,
        (1.0f - alpha / a) / a0
    };
}

void MonitorDspProcessor::configureFilterState() {
    left_ = {};
    right_ = {};

    left_.speakerHighPass = right_.speakerHighPass = makeHighPass(58.0f, kSqrtHalf);
    left_.speakerLowShelf = right_.speakerLowShelf = makeLowShelf(115.0f, -1.8f, 0.85f);
    left_.speakerPresence = right_.speakerPresence = makePeaking(2600.0f, 1.2f, 0.75f);
    left_.speakerAir = right_.speakerAir = makeHighShelf(9200.0f, -0.9f, 0.9f);

    left_.headphoneTilt = right_.headphoneTilt = makeHighShelf(6800.0f, -1.4f, 0.8f);

    left_.graphicLowShelf = right_.graphicLowShelf = makeLowShelf(95.0f, 1.0f, 0.75f);
    left_.graphicMid = right_.graphicMid = makePeaking(620.0f, -0.8f, 0.9f);
    left_.graphicHighShelf = right_.graphicHighShelf = makeHighShelf(7200.0f, -0.7f, 0.75f);

    left_.roomRumble = right_.roomRumble = makeHighPass(42.0f, kSqrtHalf);
    left_.roomDeskNotch = right_.roomDeskNotch = makePeaking(180.0f, -2.1f, 1.2f);

    headphoneCrossfeedLeft_ = makeLowPass(1150.0f, 0.62f);
    headphoneCrossfeedRight_ = makeLowPass(1150.0f, 0.62f);
    crossfeedLeft_ = makeLowPass(700.0f, 0.58f);
    crossfeedRight_ = makeLowPass(700.0f, 0.58f);
}

StereoFrame MonitorDspProcessor::process(StereoFrame frame) {
    frame.left = realtimeSafeSample(frame.left);
    frame.right = realtimeSafeSample(frame.right);
    if (!hasAudibleProcessing_) {
        return frame;
    }
    for (const auto& module : modules_) {
        if (moduleEnabled(module, "speaker-simulation")) {
            const bool targetMatchesReal = speakerTargetMatchesPhysicalReal(module);
            frame = applySpeakerSimulation(frame, module);
            if (!targetMatchesReal && speakerRoomEqForActivePhysicalOutput(module)) {
                frame = applyGraphicEq(frame);
            }
        } else if (moduleEnabled(module, "headphone-simulation")) {
            frame = applyHeadphoneSimulation(frame, module);
        } else if (moduleEnabled(module, "graphic-eq")) {
            frame = applyGraphicEq(frame);
        } else if (moduleEnabled(module, "room-correction")) {
            frame = applyRoomCorrection(frame);
        } else if (moduleEnabled(module, "crossfeed")) {
            frame = applyCrossfeed(frame);
        }
    }
    if (const auto* streamingModule = activeStreamingPreviewModule(modules_)) {
        frame = applyStreamingPreview(frame, *streamingModule);
    }
    frame.left = std::clamp(frame.left, -1.0f, 1.0f);
    frame.right = std::clamp(frame.right, -1.0f, 1.0f);
    return frame;
}

// The bare catalog name behind a slotted "Speaker A: <name>" string, for a profile lookup.
std::string speakerModelCatalogKey(const std::string& stored) {
    const auto pos = stored.find(": ");
    return pos == std::string::npos ? stored : stored.substr(pos + 2);
}

StereoFrame MonitorDspProcessor::applySpeakerSimulation(StereoFrame frame, const MonitorDspModule& module) {
    if (speakerTargetMatchesReal(module)) {
        return frame;
    }
    // Single-EQ design: a model with a MEASURED response curve is voiced entirely by the one
    // monitor parametric EQ (the bridge loads that curve into it), so pass the audio through
    // here untouched — otherwise this heuristic voicing chain would double-colour it. Models
    // without measured data keep the name-heuristic tone below.
    // A/B/C slots may hold a MEASURED HEADPHONE model too (EngineController allows a headphone with
    // a curve as a slot target, and nc_monitor_eq_sync loads its curve into the one EQ). Check both
    // catalogs so the guard is symmetric with the bridge — otherwise a headphone slot gets its
    // measured curve in the EQ AND this heuristic speaker voicing on top (the double-colour Codex flagged).
    const std::string key = speakerModelCatalogKey(activeTargetModel(module));
    // A model voiced by the single monitor EQ — measured OR spec-derived (the bridge loads that
    // curve) — passes through here untouched, so this heuristic chain never double-colours it. Only
    // Flat/Off/empty (no spec curve either) still take the name-heuristic voicing below.
    if (!speakerProfileCurve(key).empty() || !headphoneProfileCurve(key).empty() ||
        !speakerSpecCurve(key).empty()) {
        return frame;
    }
    const StereoFrame dry = frame;
    const float targetTone = speakerTargetTone(module) + realSpeakerCompensation(module);
    frame.left = left_.speakerAir.process(left_.speakerPresence.process(left_.speakerLowShelf.process(left_.speakerHighPass.process(frame.left))));
    frame.right = right_.speakerAir.process(right_.speakerPresence.process(right_.speakerLowShelf.process(right_.speakerHighPass.process(frame.right))));
    const float monoMid = (frame.left + frame.right) * 0.5f;
    frame.left = frame.left * (0.985f - targetTone * 0.10f) + monoMid * targetTone * 0.035f;
    frame.right = frame.right * (0.985f - targetTone * 0.10f) + monoMid * targetTone * 0.035f;
    const float levelMatch = speakerLevelMatchGain(targetTone);
    frame.left *= levelMatch;
    frame.right *= levelMatch;
    const float strength = std::max(0.25f, std::min(2.0f, 1.0f + speakerSimulationWeightForActiveSlot(module)));
    if (std::abs(strength - 1.0f) > 0.0001f) {
        frame.left = dry.left + (frame.left - dry.left) * strength;
        frame.right = dry.right + (frame.right - dry.right) * strength;
    }
    frame.left = softLimit(frame.left);
    frame.right = softLimit(frame.right);
    return frame;
}

StereoFrame MonitorDspProcessor::applyHeadphoneSimulation(StereoFrame frame, const MonitorDspModule& module) {
    const float filteredLeft = headphoneCrossfeedLeft_.process(frame.left);
    const float filteredRight = headphoneCrossfeedRight_.process(frame.right);
    const float crossfeed = headphoneCrossfeedAmount(module);
    const float direct = std::max(0.82f, 1.0f - crossfeed * 0.82f);
    frame.left = frame.left * direct + filteredRight * crossfeed;
    frame.right = frame.right * direct + filteredLeft * crossfeed;
    // If the active target is a MEASURED model, its curve already voices the single monitor EQ, so the
    // generic fixed 6.8 kHz tilt (and its make-up) would double-attenuate the treble (Codex #5). Skip
    // the tilt in that case and leave the tone entirely to the measurement — crossfeed still applies.
    // A target with no measured data keeps the generic voicing tilt.
    const std::string key = speakerModelCatalogKey(activeTargetModel(module));
    const bool measured = !headphoneProfileCurve(key).empty() || !speakerProfileCurve(key).empty();
    if (measured) {
        return {std::clamp(frame.left, -1.0f, 1.0f), std::clamp(frame.right, -1.0f, 1.0f)};
    }
    frame.left = left_.headphoneTilt.process(frame.left);
    frame.right = right_.headphoneTilt.process(frame.right);
    return {
        std::clamp(frame.left * 1.035f, -1.0f, 1.0f),
        std::clamp(frame.right * 1.035f, -1.0f, 1.0f)
    };
}

StereoFrame MonitorDspProcessor::applyGraphicEq(StereoFrame frame) {
    return {
        left_.graphicHighShelf.process(left_.graphicMid.process(left_.graphicLowShelf.process(frame.left))),
        right_.graphicHighShelf.process(right_.graphicMid.process(right_.graphicLowShelf.process(frame.right)))
    };
}

StereoFrame MonitorDspProcessor::applyRoomCorrection(StereoFrame frame) {
    return {
        left_.roomDeskNotch.process(left_.roomRumble.process(frame.left)),
        right_.roomDeskNotch.process(right_.roomRumble.process(frame.right))
    };
}

StereoFrame MonitorDspProcessor::applyCrossfeed(StereoFrame frame) {
    const float filteredLeft = crossfeedLeft_.process(frame.left);
    const float filteredRight = crossfeedRight_.process(frame.right);
    return {
        frame.left * 0.955f + filteredRight * 0.07f,
        frame.right * 0.955f + filteredLeft * 0.07f
    };
}

void applyMonitorDspToInterleavedStereo(WavAudioData& audio, const std::vector<MonitorDspModule>& modules) {
    if (audio.channels != 2 || audio.sampleRate <= 0 || modules.empty()) {
        return;
    }
    MonitorDspProcessor processor;
    processor.configure(audio.sampleRate, modules);
    for (size_t index = 0; index + 1 < audio.interleavedSamples.size(); index += 2) {
        const auto processed = processor.process({audio.interleavedSamples[index], audio.interleavedSamples[index + 1]});
        audio.interleavedSamples[index] = processed.left;
        audio.interleavedSamples[index + 1] = processed.right;
    }
}

} // namespace neuracoust::daw
