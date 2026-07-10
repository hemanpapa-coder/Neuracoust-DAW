#include "audio/MetronomeClick.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace neuracoust::daw {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

struct TempoPosition {
    double bpm = 120.0;
    double beatPosition = 0.0;
};

std::vector<TempoMarkerState> normalizedTempoMap(const AudioEngineSettings& settings) {
    std::vector<TempoMarkerState> tempoMap;
    tempoMap.reserve(settings.tempoMap.size());
    for (const auto& marker : settings.tempoMap) {
        if (std::isfinite(marker.timeSeconds) && std::isfinite(marker.bpm) &&
            marker.timeSeconds >= 0.0 && marker.bpm > 0.0) {
            tempoMap.push_back(marker);
        }
    }
    std::sort(tempoMap.begin(), tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    return tempoMap;
}

TempoPosition tempoPositionAtSeconds(double seconds, const AudioEngineSettings& settings, const std::vector<TempoMarkerState>& tempoMap) {
    const double fallbackBpm = std::max(1.0, static_cast<double>(settings.tempoBpm));
    TempoPosition position;
    position.bpm = fallbackBpm;
    if (tempoMap.empty()) {
        position.beatPosition = seconds * fallbackBpm / 60.0;
        return position;
    }
    if (seconds <= tempoMap.front().timeSeconds) {
        position.bpm = std::max(1.0, tempoMap.front().bpm);
        position.beatPosition = seconds * position.bpm / 60.0;
        return position;
    }

    position.beatPosition = tempoMap.front().timeSeconds * tempoMap.front().bpm / 60.0;
    bool resolved = false;
    for (size_t index = 1; index < tempoMap.size(); ++index) {
        const auto& left = tempoMap[index - 1];
        const auto& right = tempoMap[index];
        const double segmentDuration = std::max(0.0, right.timeSeconds - left.timeSeconds);
        if (segmentDuration <= 0.0) {
            continue;
        }
        const double segmentEnd = right.timeSeconds;
        const double localDuration = std::max(0.0, std::min(seconds, segmentEnd) - left.timeSeconds);
        const double slope = (right.bpm - left.bpm) / segmentDuration;
        position.beatPosition += (left.bpm * localDuration + 0.5 * slope * localDuration * localDuration) / 60.0;
        if (seconds <= segmentEnd) {
            position.bpm = std::max(1.0, left.bpm + slope * localDuration);
            resolved = true;
            break;
        }
    }
    if (!resolved) {
        const auto& last = tempoMap.back();
        position.bpm = std::max(1.0, last.bpm);
        position.beatPosition += std::max(0.0, seconds - last.timeSeconds) * position.bpm / 60.0;
    }
    return position;
}

std::vector<TimeSignatureMarkerState> normalizedTimeSignatureMap(const AudioEngineSettings& settings) {
    std::vector<TimeSignatureMarkerState> markers;
    markers.reserve(settings.timeSignatureMap.size() + 1);
    for (const auto& marker : settings.timeSignatureMap) {
        const int numerator = std::max(1, std::min(16, marker.numerator));
        const int denominator = std::max(1, std::min(32, marker.denominator));
        if (std::isfinite(marker.timeSeconds) && marker.timeSeconds >= 0.0) {
            markers.push_back({marker.timeSeconds, numerator, denominator});
        }
    }
    if (markers.empty()) {
        markers.push_back({0.0,
            std::max(1, std::min(16, settings.timeSignatureNumerator)),
            std::max(1, std::min(32, settings.timeSignatureDenominator))});
    }
    std::sort(markers.begin(), markers.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    if (markers.front().timeSeconds > 0.0) {
        markers.insert(markers.begin(), {0.0,
            std::max(1, std::min(16, settings.timeSignatureNumerator)),
            std::max(1, std::min(32, settings.timeSignatureDenominator))});
    } else {
        markers.front().timeSeconds = 0.0;
    }
    return markers;
}

TimeSignatureMarkerState activeTimeSignatureAtSeconds(double seconds, const std::vector<TimeSignatureMarkerState>& markers) {
    TimeSignatureMarkerState active = markers.empty() ? TimeSignatureMarkerState{} : markers.front();
    for (const auto& marker : markers) {
        if (marker.timeSeconds > seconds + 1.0e-9) {
            break;
        }
        active = marker;
    }
    return active;
}

double clickUnitQuarterNotesForSettings(const AudioEngineSettings& settings, int denominator) {
    if (settings.metronomeSubdivision == "quarter") {
        return 1.0;
    }
    if (settings.metronomeSubdivision == "eighth") {
        return 0.5;
    }
    if (settings.metronomeSubdivision == "sixteenth") {
        return 0.25;
    }
    return std::max(0.125, 4.0 / static_cast<double>(denominator));
}

} // namespace

float renderMetronomeClickSampleAtFrame(int64_t frame, const AudioEngineSettings& settings) {
    if (!settings.metronomeEnabled || settings.tempoBpm <= 0 || settings.sampleRate <= 0.0 || frame < 0) {
        return 0.0f;
    }
    const double seconds = static_cast<double>(frame) / settings.sampleRate;
    const auto tempoMap = normalizedTempoMap(settings);
    const TempoPosition tempoPosition = tempoPositionAtSeconds(seconds, settings, tempoMap);
    const double bpm = tempoPosition.bpm;
    const double beatPosition = tempoPosition.beatPosition;
    if (!std::isfinite(beatPosition)) {
        return 0.0f;
    }
    const auto signatureMap = normalizedTimeSignatureMap(settings);
    const TimeSignatureMarkerState signature = activeTimeSignatureAtSeconds(seconds, signatureMap);
    const int numerator = std::max(1, std::min(16, signature.numerator));
    const int denominator = std::max(1, std::min(32, signature.denominator));
    const double signatureOriginBeat = tempoPositionAtSeconds(signature.timeSeconds, settings, tempoMap).beatPosition;
    const double clickUnitQuarterNotes = clickUnitQuarterNotesForSettings(settings, denominator);
    const double clickPosition = (beatPosition - signatureOriginBeat) / clickUnitQuarterNotes;
    auto clickIndex = static_cast<int64_t>(std::floor(clickPosition + 1.0e-9));
    double clickFraction = std::max(0.0, clickPosition - static_cast<double>(clickIndex));
    if (clickUnitQuarterNotes <= 0.5001 &&
        (settings.grooveFeel == "shuffle" || settings.grooveFeel == "triplet")) {
        const double swing = settings.grooveFeel == "triplet"
            ? (2.0 / 3.0)
            : std::max(0.5001, std::min(0.90, settings.grooveSwingAmount));
        const double swungOffset = swing * 2.0;
        const auto pairIndex = static_cast<int64_t>(std::floor(clickPosition / 2.0 + 1.0e-9));
        const double pairBase = static_cast<double>(pairIndex) * 2.0;
        double previousClickPosition = pairBase;
        clickIndex = pairIndex * 2;
        if (clickPosition + 1.0e-9 >= pairBase + swungOffset) {
            previousClickPosition = pairBase + swungOffset;
            clickIndex = pairIndex * 2 + 1;
        } else if (pairIndex > 0) {
            previousClickPosition = static_cast<double>(pairIndex - 1) * 2.0 + swungOffset;
            clickIndex = (pairIndex - 1) * 2 + 1;
        }
        clickFraction = std::max(0.0, clickPosition - previousClickPosition);
    }
    const double beatPhaseSeconds = clickFraction * clickUnitQuarterNotes * 60.0 / std::max(1.0, bpm);
    const int64_t clickFrames = std::max<int64_t>(1, static_cast<int64_t>(settings.sampleRate * 0.025));
    if (beatPhaseSeconds * settings.sampleRate >= static_cast<double>(clickFrames)) {
        return 0.0f;
    }
    const int pulseInBar = static_cast<int>(clickIndex % numerator);
    const bool compoundEighthMeter = denominator == 8 && numerator >= 6 && (numerator % 3) == 0;
    const bool strongAccent = pulseInBar == 0;
    const bool mediumAccent = compoundEighthMeter && pulseInBar > 0 && (pulseInBar % 3) == 0;
    const double frequency = strongAccent ? 1760.0 : (mediumAccent ? 1396.91 : 1174.66);
    const double phase = kTwoPi * frequency * beatPhaseSeconds;
    const float envelope = static_cast<float>(1.0 - (beatPhaseSeconds * settings.sampleRate) / static_cast<double>(clickFrames));
    const float level = strongAccent ? 0.22f : (mediumAccent ? 0.18f : 0.10f);
    return static_cast<float>(std::sin(phase)) * level * envelope;
}

} // namespace neuracoust::daw
