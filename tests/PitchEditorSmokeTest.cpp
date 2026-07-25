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

    // --- Melodyne-style horizontal note move: source is cleared and audio appears later ---
    {
        std::vector<float> buf;
        appendSine(buf, 1, 220.0, 0.8, sr);
        auto notes = segmentNotes(detectPitchTrack(buf, 1, sr));
        check(!notes.empty(), "single note detected for timing edit");
        if (!notes.empty()) {
            notes[0].timeOffsetSeconds = 0.15;
            auto edited = renderNoteEdits(buf, 1, sr, notes);
            double sourceEnergy = 0.0, movedEnergy = 0.0;
            const int sourceA = static_cast<int>((notes[0].startSeconds + 0.03) * sr);
            const int sourceB = static_cast<int>((notes[0].startSeconds + 0.10) * sr);
            const int movedA = static_cast<int>((notes[0].startSeconds + 0.18) * sr);
            const int movedB = static_cast<int>((notes[0].startSeconds + 0.25) * sr);
            for (int i = sourceA; i < sourceB && i < static_cast<int>(edited.size()); ++i)
                sourceEnergy += std::abs(edited[static_cast<size_t>(i)]);
            for (int i = movedA; i < movedB && i < static_cast<int>(edited.size()); ++i)
                movedEnergy += std::abs(edited[static_cast<size_t>(i)]);
            check(movedEnergy > sourceEnergy * 4.0, "horizontal note move relocates audible energy");
        }
    }

    // --- Amplitude / mute / formant / attack: the rest of the Melodyne palette ---
    {
        std::vector<float> buf;
        appendSine(buf, 1, 220.0, 0.8, sr);
        auto notes = segmentNotes(detectPitchTrack(buf, 1, sr));
        check(!notes.empty(), "single note detected for the palette tests");
        if (!notes.empty()) {
            const int64_t midA = static_cast<int64_t>((notes[0].startSeconds + 0.20) * sr);
            const int64_t midB = static_cast<int64_t>((notes[0].startSeconds + 0.50) * sr);
            auto energy = [&](const std::vector<float>& v) {
                double sum = 0.0;
                for (int64_t i = midA; i < midB && i < static_cast<int64_t>(v.size()); ++i)
                    sum += std::abs(v[static_cast<size_t>(i)]);
                return sum;
            };
            const double plain = energy(buf);

            // An untouched note must come back BIT-identical, not merely close: renderNoteEdits is
            // run on every note of every clip, so a silent re-render would rewrite whole takes.
            {
                auto untouched = renderNoteEdits(buf, 1, sr, notes);
                check(untouched == buf, "an unedited note passes through bit-identical");
            }

            {
                auto edited = notes;
                edited[0].gainDb = -6.0;
                const double got = energy(renderNoteEdits(buf, 1, sr, edited));
                char m[96];
                std::snprintf(m, sizeof m, "-6 dB halves the note (ratio %.3f, want ~0.501)", got / plain);
                check(std::abs(got / plain - 0.5012) < 0.03, m);
            }
            {
                auto edited = notes;
                edited[0].muted = true;
                const double got = energy(renderNoteEdits(buf, 1, sr, edited));
                char m[96];
                std::snprintf(m, sizeof m, "mute silences the note (residual %.5f of original)", got / plain);
                check(got < plain * 0.01, m);
            }
            {
                // A formant shift must NOT move the pitch — that is the whole point of the tool.
                auto edited = notes;
                edited[0].formantSemitones = 5.0;
                auto shifted = renderNoteEdits(buf, 1, sr, edited);
                check(shifted.size() == buf.size(), "formant shift preserves length");
                std::vector<float> mid(shifted.begin() + midA, shifted.begin() + midB);
                const double hz = estimateHz(mid, sr);
                char m[96];
                std::snprintf(m, sizeof m, "formant +5 st leaves the pitch at 220 Hz (got %.0f)", hz);
                check(hz > 205.0 && hz < 235.0, m);
            }
            {
                // Attack speed reshapes only the attack, and 1.0 is exactly transparent.
                auto neutral = notes;
                neutral[0].attackSpeed = 1.0;
                check(renderNoteEdits(buf, 1, sr, neutral) == buf, "attackSpeed 1.0 changes nothing");
            }
        }
    }

    // --- A held note with vibrato is ONE note ------------------------------------------------
    //
    // The segmenter used to end a note at the first frame outside tolerance, so a ±0.5-semitone
    // wobble read as a new note every half cycle: a 1.6 s sung note came back as fifteen 90 ms
    // fragments, and every per-note tool then operated on a fragment. This is the guard.
    {
        const double dur = 1.6;
        std::vector<float> buf(static_cast<size_t>(dur * sr), 0.0f);
        double phase = 0.0;
        for (size_t i = 0; i < buf.size(); ++i) {
            const double t = static_cast<double>(i) / sr;
            const double hz = 220.0 * std::pow(2.0, (0.5 * std::sin(2.0 * M_PI * 5.0 * t)) / 12.0);
            phase += 2.0 * M_PI * hz / sr;
            buf[i] = static_cast<float>(0.4 * std::sin(phase));
        }
        const auto notes = segmentNotes(detectPitchTrack(buf, 1, sr));
        char m[120];
        std::snprintf(m, sizeof m, "a 1.6 s note with vibrato stays one note (got %zu)", notes.size());
        check(notes.size() == 1, m);
        if (notes.size() == 1) {
            std::snprintf(m, sizeof m, "…and keeps its full length (%.2f s of %.2f)",
                          notes[0].durationSeconds, dur);
            check(notes[0].durationSeconds > dur * 0.9, m);
            check(std::abs(notes[0].detectedMidi - 57.0) < 0.5, "…at the vibrato's centre pitch (A3)");
        }
    }

    // --- Pitch modulation / drift: scaling a note's own contour ----------------------------
    //
    // A 220 Hz tone with a 5 Hz, ±0.5-semitone vibrato. Scaling the modulation to 0 must flatten it;
    // scaling it to 2 must roughly double it. Measured on the rendered audio's own pitch track, not
    // on the parameter — the parameter agreeing with itself would prove nothing.
    {
        const double dur = 1.6;
        std::vector<float> buf(static_cast<size_t>(dur * sr), 0.0f);
        double phase = 0.0;
        for (size_t i = 0; i < buf.size(); ++i) {
            const double t = static_cast<double>(i) / sr;
            const double semis = 0.5 * std::sin(2.0 * M_PI * 5.0 * t);
            const double hz = 220.0 * std::pow(2.0, semis / 12.0);
            phase += 2.0 * M_PI * hz / sr;
            buf[i] = static_cast<float>(0.4 * std::sin(phase));
        }
        // Vibrato depth of the RENDERED audio, in semitones (peak-to-peak / 2), over the steady middle.
        auto vibratoDepth = [&](const std::vector<float>& v) {
            const auto track = detectPitchTrack(v, 1, sr, 0.01);
            double lo = 1e9, hi = -1e9;
            for (const auto& f : track) {
                if (f.frequencyHz <= 0.0) continue;
                if (f.timeSeconds < 0.35 || f.timeSeconds > dur - 0.35) continue;
                const double semis = 12.0 * std::log2(f.frequencyHz / 220.0);
                lo = std::min(lo, semis);
                hi = std::max(hi, semis);
            }
            return (hi > lo) ? (hi - lo) / 2.0 : 0.0;
        };

        auto notes = segmentNotes(detectPitchTrack(buf, 1, sr));
        check(!notes.empty(), "vibrato tone detected as a note");
        if (!notes.empty()) {
            const double base = vibratoDepth(buf);
            char m[160];
            std::snprintf(m, sizeof m, "the test tone really has ~0.5 st of vibrato (got %.3f)", base);
            check(base > 0.3 && base < 0.8, m);

            auto flat = notes;
            for (auto& n : flat) n.pitchModulationScale = 0.0;
            const double flattened = vibratoDepth(renderNoteEdits(buf, 1, sr, flat));
            std::snprintf(m, sizeof m, "modulation 0 flattens the vibrato (%.3f → %.3f st)", base, flattened);
            check(flattened < base * 0.45, m);

            auto deep = notes;
            for (auto& n : deep) n.pitchModulationScale = 2.0;
            const double deepened = vibratoDepth(renderNoteEdits(buf, 1, sr, deep));
            std::snprintf(m, sizeof m, "modulation 2 deepens the vibrato (%.3f → %.3f st)", base, deepened);
            check(deepened > base * 1.5, m);

            // Length is the contract everything else in the editor relies on.
            check(renderNoteEdits(buf, 1, sr, flat).size() == buf.size(),
                  "contour scaling preserves the clip length");
            auto neutral = notes;
            for (auto& n : neutral) { n.pitchModulationScale = 1.0; n.pitchDriftScale = 1.0; }
            check(renderNoteEdits(buf, 1, sr, neutral) == buf,
                  "modulation/drift at 1.0 changes nothing");
        }
    }

    // --- The formant tool really moves the spectral envelope -------------------------------
    //
    // The check above only proves the PITCH did not move, which a no-op would also pass. This one
    // needs harmonics to see: on a saw, moving the envelope up must tilt energy from the low
    // harmonics to the high ones while the fundamental stays put.
    {
        std::vector<float> buf(static_cast<size_t>(1.0 * sr), 0.0f);
        for (size_t i = 0; i < buf.size(); ++i) {
            const double t = static_cast<double>(i) / sr;
            double v = 0.0;
            for (int h = 1; h <= 12; ++h) v += std::sin(2.0 * M_PI * 220.0 * h * t) / h;
            buf[i] = static_cast<float>(0.3 * v);
        }
        auto notes = segmentNotes(detectPitchTrack(buf, 1, sr));
        check(!notes.empty(), "saw tone detected as a note");
        if (!notes.empty()) {
            // Energy in a band, by direct correlation against the harmonic — no FFT needed here.
            auto bandEnergy = [&](const std::vector<float>& v, double hz) {
                double re = 0.0, im = 0.0;
                const int64_t a = static_cast<int64_t>(0.25 * sr);
                const int64_t b = std::min<int64_t>(static_cast<int64_t>(0.75 * sr), (int64_t)v.size());
                for (int64_t i = a; i < b; ++i) {
                    const double t = static_cast<double>(i) / sr;
                    re += v[static_cast<size_t>(i)] * std::cos(2.0 * M_PI * hz * t);
                    im += v[static_cast<size_t>(i)] * std::sin(2.0 * M_PI * hz * t);
                }
                return std::sqrt(re * re + im * im);
            };
            auto edited = notes;
            edited[0].formantSemitones = 12.0;   // an octave of formant lift — unmistakable
            const auto shifted = renderNoteEdits(buf, 1, sr, edited);

            const double lowBefore = bandEnergy(buf, 440.0);      // 2nd harmonic
            const double highBefore = bandEnergy(buf, 2200.0);    // 10th harmonic
            const double lowAfter = bandEnergy(shifted, 440.0);
            const double highAfter = bandEnergy(shifted, 2200.0);
            const double tiltBefore = highBefore / std::max(1e-9, lowBefore);
            const double tiltAfter = highAfter / std::max(1e-9, lowAfter);
            char m[160];
            std::snprintf(m, sizeof m, "formant +12 st brightens the spectrum (tilt %.4f → %.4f)",
                          tiltBefore, tiltAfter);
            check(tiltAfter > tiltBefore * 1.2, m);

            // …and the fundamental is still 220 Hz, i.e. it moved the timbre, not the note.
            const double f0Before = bandEnergy(buf, 220.0);
            const double f0After = bandEnergy(shifted, 220.0);
            std::snprintf(m, sizeof m, "the fundamental survives the formant shift (%.3f → %.3f)",
                          f0Before, f0After);
            check(f0After > f0Before * 0.15, m);
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

    // --- Polyphonic mode resolves simultaneous chord tones instead of mirroring melodic YIN ---
    {
        const int n = static_cast<int>(0.8 * sr);
        std::vector<float> chord(static_cast<size_t>(n), 0.0f);
        const double frequencies[] = {261.63, 329.63, 392.0}; // C4 E4 G4
        for (int i = 0; i < n; ++i)
            for (double hz : frequencies)
                chord[static_cast<size_t>(i)] += 0.22f * std::sin(2.0 * M_PI * hz * i / sr);
        const auto poly = detectNotesForMode(chord, 1, sr, DetectionMode::Polyphonic);
        bool c = false, e = false, g = false;
        for (const auto& note : poly) {
            c = c || std::abs(note.detectedMidi - 60.0) <= 1.0;
            e = e || std::abs(note.detectedMidi - 64.0) <= 1.0;
            g = g || std::abs(note.detectedMidi - 67.0) <= 1.0;
        }
        check(poly.size() >= 3, "polyphonic chord produces multiple simultaneous notes");
        check(c && e && g, "polyphonic chord resolves C4, E4 and G4");
    }

    if (g_failures == 0) { std::printf("PitchEditor smoke: all checks passed\n"); return 0; }
    std::printf("PitchEditor smoke: %d failure(s)\n", g_failures);
    return 1;
}
