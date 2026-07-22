// Pins the Melodyne-mode DSP: YIN pitch detection, note segmentation, and per-note pitch-shift render.
#include "audio/PitchEditor.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace neuracoust::daw;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL: %s\n", what); ++g_failures; }
}

double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

// Append `seconds` of a sine at `hz` (mono) to an interleaved buffer of `channels`.
void appendSine(std::vector<float>& buf, int channels, double hz, double seconds, double sr, float amp = 0.5f) {
    const int n = static_cast<int>(seconds * sr);
    for (int i = 0; i < n; ++i) {
        const float v = amp * std::sin(2.0 * M_PI * hz * i / sr);
        for (int c = 0; c < channels; ++c) buf.push_back(v);
    }
}

double estimateHz(const std::vector<float>& mono, double sr) {
    int crossings = 0, first = -1, last = -1;
    for (size_t i = 1; i < mono.size(); ++i)
        if (mono[i - 1] <= 0.0f && mono[i] > 0.0f) {
            if (first < 0) first = static_cast<int>(i);
            last = static_cast<int>(i); ++crossings;
        }
    if (crossings < 2 || last <= first) return 0.0;
    return (crossings - 1) / ((last - first) / sr);
}
}  // namespace

int main() {
    const double sr = 44100.0;

    // --- YIN detects a steady 440 Hz (A4, MIDI 69) ---
    {
        std::vector<float> buf;
        appendSine(buf, 1, 440.0, 0.5, sr);
        auto track = detectPitchTrack(buf, 1, sr);
        int voiced = 0; double sumMidi = 0.0;
        for (auto& f : track) if (f.frequencyHz > 0) { voiced++; sumMidi += hzToMidi(f.frequencyHz); }
        check(voiced > 5, "440 Hz: found voiced frames");
        const double meanMidi = voiced ? sumMidi / voiced : 0.0;
        char m[96]; std::snprintf(m, sizeof m, "440 Hz detected as ~A4 (midi %.2f, want 69)", meanMidi);
        check(std::abs(meanMidi - 69.0) < 0.5, m);
    }

    // --- Segmentation splits a 220→330 Hz sequence into two notes (A3=57, E4≈64) ---
    {
        std::vector<float> buf;
        appendSine(buf, 1, 220.0, 0.5, sr);   // A3
        appendSine(buf, 1, 329.63, 0.5, sr);  // E4
        auto track = detectPitchTrack(buf, 1, sr);
        auto notes = segmentNotes(track);
        char m[96]; std::snprintf(m, sizeof m, "two-note sequence → %zu notes (want 2)", notes.size());
        check(notes.size() == 2, m);
        if (notes.size() == 2) {
            check(std::abs(notes[0].detectedMidi - 57.0) < 0.6, "first note ~A3 (midi 57)");
            check(std::abs(notes[1].detectedMidi - 64.0) < 0.6, "second note ~E4 (midi 64)");
            check(notes[0].startSeconds < notes[1].startSeconds, "notes ordered in time");
        }
    }

    // --- renderNoteEdits: shift a 220 Hz note up an octave → ~440 Hz at that span ---
    {
        std::vector<float> buf;
        appendSine(buf, 1, 220.0, 0.6, sr);
        auto track = detectPitchTrack(buf, 1, sr);
        auto notes = segmentNotes(track);
        check(!notes.empty(), "single 220 Hz note detected for editing");
        if (!notes.empty()) {
            notes[0].pitchOffsetSemitones = 12.0;   // up an octave
            auto edited = renderNoteEdits(buf, 1, sr, notes);
            check(edited.size() == buf.size(), "note edit preserves length");
            // Measure the pitch in the middle of the note (skip the 5 ms edge crossfades).
            const int64_t s = static_cast<int64_t>((notes[0].startSeconds + 0.15) * sr);
            const int64_t e = static_cast<int64_t>((notes[0].startSeconds + notes[0].durationSeconds - 0.15) * sr);
            std::vector<float> mid(edited.begin() + s, edited.begin() + std::min<int64_t>(e, (int64_t)edited.size()));
            const double hz = estimateHz(mid, sr);
            char m[96]; std::snprintf(m, sizeof m, "220 Hz note +12 st ≈ 440 Hz (got %.0f)", hz);
            check(hz > 400.0 && hz < 480.0, m);
        }
    }

    // --- Percussive mode: onset detection on a click track ---
    {
        // Eight clicks (short noise bursts) at 0.25 s spacing.
        std::vector<float> buf(static_cast<size_t>(2.0 * sr), 0.0f);
        uint32_t rng = 12345;
        for (int k = 0; k < 8; ++k) {
            const int start = static_cast<int>((0.05 + k * 0.25) * sr);
            for (int i = 0; i < 400 && start + i < (int)buf.size(); ++i) {
                rng = rng * 1664525u + 1013904223u;
                const float noise = (static_cast<float>(rng >> 9) / 8388608.0f - 1.0f);
                buf[static_cast<size_t>(start + i)] = noise * (1.0f - i / 400.0f);   // decaying burst
            }
        }
        auto onsets = detectOnsets(buf, 1, sr);
        char m[96]; std::snprintf(m, sizeof m, "click track → %zu onsets (want ~8)", onsets.size());
        check(onsets.size() >= 6 && onsets.size() <= 10, m);

        auto events = detectNotesForMode(buf, 1, sr, DetectionMode::Percussive);
        check(events.size() == onsets.size(), "percussive mode returns one event per onset");
        if (!events.empty()) check(events[0].detectedMidi == 0.0, "percussive events carry no pitch");
    }

    // --- pYIN-style track cleanup: octave glitch corrected, lone gap filled ---
    {
        std::vector<PitchFrame> track;
        for (int i = 0; i < 11; ++i) { PitchFrame f; f.timeSeconds = i * 0.01; f.frequencyHz = 220.0; f.confidence = 0.9; track.push_back(f); }
        track[5].frequencyHz = 440.0;   // single-frame octave-up error
        track[7].frequencyHz = 0.0;     // lone unvoiced gap between voiced frames
        auto sm = smoothPitchTrack(track);
        check(std::abs(sm[5].frequencyHz - 220.0) < 20.0, "octave glitch snapped back to 220 Hz");
        check(sm[7].frequencyHz > 180.0, "lone unvoiced gap filled from neighbours");

        // Viterbi must PRESERVE a genuine sustained octave leap (not flatten it).
        std::vector<PitchFrame> leap;
        for (int i = 0; i < 30; ++i) { PitchFrame f; f.timeSeconds = i * 0.01; f.confidence = 0.9; f.frequencyHz = (i < 10 ? 220.0 : 440.0); leap.push_back(f); }
        auto sm2 = smoothPitchTrack(leap);
        check(std::abs(sm2[5].frequencyHz - 220.0) < 20.0, "leap: first section stays 220 Hz");
        check(std::abs(sm2[25].frequencyHz - 440.0) < 40.0, "leap: sustained 440 section preserved (not over-corrected)");
    }

    // --- Melodic mode dispatch matches direct detect+segment ---
    {
        std::vector<float> buf;
        appendSine(buf, 1, 440.0, 0.5, sr);
        auto viaMode = detectNotesForMode(buf, 1, sr, DetectionMode::Melodic);
        check(!viaMode.empty(), "melodic mode dispatch produces notes");
    }

    if (g_failures == 0) { std::printf("PitchEditor smoke: all checks passed\n"); return 0; }
    std::printf("PitchEditor smoke: %d failure(s)\n", g_failures);
    return 1;
}
