// Guards the metronome click generator: the swung on-beats must not vanish (a subtle pairing
// bug once handed each on-beat's onset to the previous pair's off-beat, so only the off-beats
// sounded — "박이 사라지고 엇박만"), and the downbeat-accent toggle must actually flatten beat 1.

#include "audio/MetronomeClick.h"

#include <cmath>
#include <cstdio>
#include <vector>

using neuracoust::daw::AudioEngineSettings;
using neuracoust::daw::renderMetronomeClickSampleAtFrame;

namespace {

struct Onset {
    double seconds = 0.0;
    float peak = 0.0f;
};

// Render one bar and collect click onsets (silence → sound transitions) with their peak level.
std::vector<Onset> collectOnsets(const AudioEngineSettings& settings, double seconds) {
    const auto total = static_cast<int64_t>(settings.sampleRate * seconds);
    std::vector<Onset> onsets;
    bool inClick = false;
    for (int64_t frame = 0; frame < total; ++frame) {
        const float sample = std::abs(renderMetronomeClickSampleAtFrame(frame, settings));
        if (sample > 1.0e-4f) {
            if (!inClick) {
                onsets.push_back({static_cast<double>(frame) / settings.sampleRate, sample});
                inClick = true;
            } else {
                onsets.back().peak = std::max(onsets.back().peak, sample);
            }
        } else {
            inClick = false;
        }
    }
    return onsets;
}

bool hasOnsetNear(const std::vector<Onset>& onsets, double seconds, double tolerance) {
    for (const auto& onset : onsets) {
        if (std::abs(onset.seconds - seconds) <= tolerance) return true;
    }
    return false;
}

float peakNear(const std::vector<Onset>& onsets, double seconds, double tolerance) {
    for (const auto& onset : onsets) {
        if (std::abs(onset.seconds - seconds) <= tolerance) return onset.peak;
    }
    return 0.0f;
}

AudioEngineSettings baseSettings() {
    AudioEngineSettings s;
    s.sampleRate = 48000.0;
    s.tempoBpm = 120;                 // a quarter note = 0.5 s; a 4/4 bar = 2 s
    s.timeSignatureNumerator = 4;
    s.timeSignatureDenominator = 4;
    s.metronomeEnabled = true;
    return s;
}

} // namespace

int main() {
    int failures = 0;
    const double tol = 0.03;   // 30 ms

    // Shuffle eighths: every quarter-note ON-beat must fire (this is the regression), plus swung
    // off-beats between them.
    {
        AudioEngineSettings s = baseSettings();
        s.grooveFeel = "shuffle";
        s.grooveSwingAmount = 0.6;
        s.metronomeSubdivision = "eighth";
        const auto onsets = collectOnsets(s, 2.0);
        for (double beat : {0.0, 0.5, 1.0, 1.5}) {
            if (!hasOnsetNear(onsets, beat, tol)) {
                fprintf(stderr, "FAIL: shuffle on-beat missing at %.2fs (박이 사라짐)\n", beat);
                failures++;
            }
        }
        // A swung off-beat lands after the straight midpoint (0.25 s) — later, not at 0.25.
        bool swungOff = false;
        for (const auto& o : onsets) {
            if (o.seconds > 0.27 && o.seconds < 0.45) { swungOff = true; break; }
        }
        if (!swungOff) { fprintf(stderr, "FAIL: no swung off-beat between beats 1 and 2\n"); failures++; }
        printf("shuffle keeps on-beats + swung off-beats OK (%zu onsets/bar)\n", onsets.size());
    }

    // Downbeat accent on: beat 1 is louder than beat 2. Off: they are equal.
    {
        AudioEngineSettings on = baseSettings();
        on.metronomeSubdivision = "quarter";
        on.metronomeAccentFirst = true;
        const auto onsetsOn = collectOnsets(on, 2.0);
        const float down = peakNear(onsetsOn, 0.0, tol);
        const float beat2 = peakNear(onsetsOn, 0.5, tol);
        if (!(down > beat2 * 1.15f)) {
            fprintf(stderr, "FAIL: downbeat not accented (down=%.3f beat2=%.3f)\n", down, beat2);
            failures++;
        }

        AudioEngineSettings off = on;
        off.metronomeAccentFirst = false;
        const auto onsetsOff = collectOnsets(off, 2.0);
        const float downFlat = peakNear(onsetsOff, 0.0, tol);
        const float beat2Flat = peakNear(onsetsOff, 0.5, tol);
        if (std::abs(downFlat - beat2Flat) > 0.02f) {
            fprintf(stderr, "FAIL: accent-off did not flatten beat 1 (down=%.3f beat2=%.3f)\n",
                    downFlat, beat2Flat);
            failures++;
        }
        printf("downbeat accent toggle OK\n");
    }

    // Genre accent pattern: gains define the accents, and a 0 is a rest (no click on that step).
    {
        AudioEngineSettings s = baseSettings();
        s.metronomeSubdivision = "eighth";
        // Beats loud, the "and" steps silent: onsets only at 0.0/0.5/1.0/1.5 s.
        s.metronomeAccentPattern = {1.0f, 0.0f, 0.6f, 0.0f, 0.8f, 0.0f, 0.6f, 0.0f};
        const auto onsets = collectOnsets(s, 2.0);
        for (double beat : {0.0, 0.5, 1.0, 1.5}) {
            if (!hasOnsetNear(onsets, beat, tol)) {
                fprintf(stderr, "FAIL: pattern beat click missing at %.2fs\n", beat); failures++;
            }
        }
        // The rest steps (the eighth-note "and"s at 0.25/0.75/...) must be silent.
        for (double rest : {0.25, 0.75, 1.25, 1.75}) {
            if (hasOnsetNear(onsets, rest, tol)) {
                fprintf(stderr, "FAIL: rest step sounded at %.2fs\n", rest); failures++;
            }
        }
        // The downbeat (gain 1.0) is louder than beat 2 (gain 0.6).
        if (!(peakNear(onsets, 0.0, tol) > peakNear(onsets, 0.5, tol))) {
            fprintf(stderr, "FAIL: pattern gain did not scale the accent level\n"); failures++;
        }
        printf("genre accent pattern (rests + gains) OK\n");
    }

    if (failures > 0) {
        fprintf(stderr, "%d metronome check(s) failed\n", failures);
        return 1;
    }
    printf("metronome smoke OK\n");
    return 0;
}
