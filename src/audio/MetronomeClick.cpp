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
    double unit;
    if (settings.metronomeSubdivision == "quarter") {
        unit = 1.0;
    } else if (settings.metronomeSubdivision == "eighth") {
        unit = 0.5;
    } else if (settings.metronomeSubdivision == "sixteenth") {
        unit = 0.25;
    } else {
        unit = std::max(0.125, 4.0 / static_cast<double>(denominator));
    }
    // Swing is only *felt* when there are sub-beats to shift: a shuffle/triplet feel implies at
    // least an eighth-note grid, so enabling swing always lays down the swung sub-beats — the way
    // a Boss Dr. Beat or Logic does it — even if the visible subdivision is set to the beat.
    if ((settings.grooveFeel == "shuffle" || settings.grooveFeel == "triplet") && unit > 0.5) {
        unit = 0.5;
    }
    return unit;
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
        // Each beat-pair is [pairBase, pairBase+2) in eighth-note units: the ON-beat sits at
        // pairBase, the swung OFF-beat at pairBase+swungOffset. The region before the off-beat
        // belongs to the on-beat (its onset), the region after to the off-beat. (An earlier
        // version handed the on-beat region to the *previous* pair's off-beat, which silenced
        // every on-beat but the first — "박이 사라지고 엇박만" — so only the off-beats sounded.)
        const auto pairIndex = static_cast<int64_t>(std::floor(clickPosition / 2.0 + 1.0e-9));
        const double pairBase = static_cast<double>(pairIndex) * 2.0;
        double previousClickPosition = pairBase;
        clickIndex = pairIndex * 2;
        if (clickPosition + 1.0e-9 >= pairBase + swungOffset) {
            previousClickPosition = pairBase + swungOffset;
            clickIndex = pairIndex * 2 + 1;
        }
        clickFraction = std::max(0.0, clickPosition - previousClickPosition);
    }
    const double beatPhaseSeconds = clickFraction * clickUnitQuarterNotes * 60.0 / std::max(1.0, bpm);
    // The click's length is timbre-dependent: a rim/wood tick is short and dry, a cowbell rings.
    const std::string& sound = settings.metronomeSound;
    double clickSeconds = 0.025;
    if (sound == "rim") { clickSeconds = 0.010; }
    else if (sound == "wood") { clickSeconds = 0.016; }
    else if (sound == "cowbell") { clickSeconds = 0.070; }
    const int64_t clickFrames = std::max<int64_t>(1, static_cast<int64_t>(settings.sampleRate * clickSeconds));
    if (beatPhaseSeconds * settings.sampleRate >= static_cast<double>(clickFrames)) {
        return 0.0f;
    }
    // Drummer-metronome accent hierarchy over the subdivision grid (Boss Dr. Beat style):
    //   • bar downbeat  → STRONG
    //   • each main beat → MEDIUM
    //   • the sub-beats in between → WEAK (quiet fills)
    // This is what makes a subdivided or swung click musical instead of a wall of equal ticks:
    // "딴 따 따 따" per beat, not "딴 딴 딴 딴". clickIndex is the subdivision index (the swing
    // branch above keeps it 0-based), so position/beat math works for straight and swung alike.
    const double barQuarters = static_cast<double>(numerator) * 4.0 / static_cast<double>(denominator);
    const int clicksPerBar = std::max(1, static_cast<int>(std::lround(barQuarters / clickUnitQuarterNotes)));
    const bool compoundEighthMeter = denominator == 8 && numerator >= 6 && (numerator % 3) == 0;
    // Clicks between MEDIUM accents: a quarter note normally, a dotted quarter in compound meters.
    const double accentBeatQuarters = compoundEighthMeter ? 1.5 : 1.0;
    const int clicksPerBeat = std::max(1, static_cast<int>(std::lround(accentBeatQuarters / clickUnitQuarterNotes)));
    const int positionInBar = ((static_cast<int>(clickIndex % clicksPerBar)) + clicksPerBar) % clicksPerBar;

    // accent: 0/1/2 → weak/medium/strong (drives pitch). accentLevel: the amplitude.
    int accent;
    float accentLevel;
    const std::vector<float>& pattern = settings.metronomeAccentPattern;
    if (!pattern.empty()) {
        // A genre groove: the pattern's per-step gain fully defines the accents (a 0 is a rest).
        // This is what turns a flat click into Samba / Bossa / Rock / Jazz feel.
        const float gain = pattern[static_cast<size_t>(positionInBar) % pattern.size()];
        if (gain <= 0.001f) {
            return 0.0f;   // rest step — no click here
        }
        accent = gain > 0.80f ? 2 : (gain > 0.45f ? 1 : 0);
        accentLevel = std::max(0.05f, std::min(0.28f, gain * 0.26f));
    } else {
        // The default bar/beat/sub-beat hierarchy. With downbeat accent off, beat 1 is treated
        // like any other beat (a flat, even click); the sub-beats stay quiet either way.
        const bool strongAccent = settings.metronomeAccentFirst && positionInBar == 0;
        const bool mediumAccent = !strongAccent && (positionInBar % clicksPerBeat == 0);
        accent = strongAccent ? 2 : (mediumAccent ? 1 : 0);
        // A wider strong>medium>weak spread so the sub-beats sit clearly under the main beats.
        accentLevel = strongAccent ? 0.24f : (mediumAccent ? 0.15f : 0.075f);
    }
    const double linearPhase = (beatPhaseSeconds * settings.sampleRate) / static_cast<double>(clickFrames);  // 0..1
    const float envelope = static_cast<float>(std::max(0.0, 1.0 - linearPhase));

    float wave;
    if (sound == "cowbell") {
        // The classic 808 cowbell: two detuned square-ish tones. Accent nudges the pitch up.
        const double f1 = (accent == 2 ? 620.0 : 540.0);
        const double f2 = f1 * 1.5060;   // ~ perfect-fifth-ish beating ratio
        const auto sq = [](double p) { return std::sin(p) >= 0.0 ? 1.0 : -1.0; };
        wave = static_cast<float>(0.5 * (sq(kTwoPi * f1 * beatPhaseSeconds) + sq(kTwoPi * f2 * beatPhaseSeconds)));
        // A sharper attack decay so it reads as metallic, not a sustained buzz.
        wave *= static_cast<float>(std::exp(-linearPhase * 3.5));
    } else if (sound == "wood") {
        // Woodblock: a mid tone with a second harmonic and a fast exponential decay.
        const double freq = accent == 2 ? 1050.0 : (accent == 1 ? 900.0 : 800.0);
        wave = static_cast<float>(std::sin(kTwoPi * freq * beatPhaseSeconds)
                                  + 0.35 * std::sin(kTwoPi * 2.0 * freq * beatPhaseSeconds));
        wave *= static_cast<float>(std::exp(-linearPhase * 6.0));
    } else if (sound == "rim") {
        // Rim/side-stick: a very short bright click, near noise-like via a high partial mix.
        const double freq = accent == 2 ? 2600.0 : (accent == 1 ? 2300.0 : 2000.0);
        wave = static_cast<float>(0.7 * std::sin(kTwoPi * freq * beatPhaseSeconds)
                                  + 0.3 * std::sin(kTwoPi * 1.7 * freq * beatPhaseSeconds));
        wave *= static_cast<float>(std::exp(-linearPhase * 8.0));
    } else {
        // Default "beep": the original clean sine, pitched by accent.
        const double freq = accent == 2 ? 1760.0 : (accent == 1 ? 1396.91 : 1174.66);
        wave = static_cast<float>(std::sin(kTwoPi * freq * beatPhaseSeconds)) * envelope;
    }

    const float userGain = static_cast<float>(std::max(0.0, std::min(2.0, settings.metronomeGain)));
    return wave * accentLevel * userGain;
}

} // namespace neuracoust::daw
