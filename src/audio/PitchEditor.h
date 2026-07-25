#pragma once

#include <vector>

namespace neuracoust::daw {

// Melodyne-style note-based pitch editing. Three stages, all offline:
//   1. detectPitchTrack — YIN fundamental-frequency estimate per analysis frame.
//   2. segmentNotes     — group the pitch track into note blobs (time range + detected pitch).
//   3. renderNoteEdits  — apply a per-note semitone offset (phase-vocoder pitch shift) → new audio.
// The Serato-style anchor time-remap lives in TimePitchProcessor; this is the complementary
// per-note mode. Both are selectable in the editor.

// One analysis frame's pitch estimate.
struct PitchFrame {
    double timeSeconds = 0.0;
    double frequencyHz = 0.0;   // 0 = unvoiced / no pitch found this frame
    double confidence = 0.0;    // 0..1 (1 − YIN's normalized difference minimum)
};

// A detected note: a run of frames at a stable pitch. detectedMidi is fractional (69 + 12·log2(f/440)).
struct DetectedNote {
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double detectedMidi = 0.0;      // fractional MIDI of the note's median pitch
    double medianFrequencyHz = 0.0;
    double confidence = 0.0;        // mean confidence across the note's frames
    double pitchOffsetSemitones = 0.0;   // the USER edit (0 = untouched). Set before renderNoteEdits.
    double timeOffsetSeconds = 0.0;      // Melodyne-style horizontal note move.
    double durationScale = 1.0;          // Melodyne-style left/right time stretch (0.25..4).

    // The rest of the Melodyne tool palette. All default to "untouched", and renderNoteEdits skips a
    // note entirely when every one of them is at its default — an unedited take must come back
    // sample-for-sample, not merely close.
    double gainDb = 0.0;                 // Amplitude tool: per-note level, ±24 dB.
    bool muted = false;                  // Amplitude/Mute tool: silence this note.
    double formantSemitones = 0.0;       // Formant tool: move the timbre without moving the pitch.
    /// Attack-speed tool. 1 = the note's own attack. >1 sharpens it, <1 softens it, by time-warping
    /// the note's OWN amplitude envelope over its attack — so 1.0 is exactly transparent.
    double attackSpeed = 1.0;
    /// Pitch-modulation tool: scales the note's vibrato depth. 1 = as sung, 0 = dead straight,
    /// 2 = twice the wobble. Operates on the fast part of the note's own pitch contour.
    double pitchModulationScale = 1.0;
    /// Pitch-drift tool: scales the note's slow pitch movement (the scoop into a note, the sag out
    /// of it) by the same rule, on the contour's slow part.
    double pitchDriftScale = 1.0;
};

// Detection modes, Melodyne-style. Melodic = monophonic pitch (YIN). Percussive = onset/transient
// events (no pitch — for timing/rhythm). Polyphonic = chords; best served by separating the source
// (Demucs) then detecting each part — until that pipeline lands it falls back to Melodic.
enum class DetectionMode { Melodic, Polyphonic, Percussive };

// One-call detection for a mode: returns notes (Melodic/Polyphonic) or transient events (Percussive,
// with detectedMidi = 0 and duration running to the next onset).
std::vector<DetectedNote> detectNotesForMode(const std::vector<float>& interleaved, int channels,
                                             double sampleRate, DetectionMode mode);

// Onset (transient) times in seconds via spectral flux + adaptive peak-picking. minIntervalSeconds
// suppresses double-triggers. The percussive mode's backbone.
std::vector<double> detectOnsets(const std::vector<float>& interleaved, int channels, double sampleRate,
                                 double minIntervalSeconds = 0.05);

// YIN pitch track over a mono mix of the input. hopSeconds ≈ 0.01, window auto-sized for the range.
// minHz/maxHz bound the search (default vocal/instrument range). Frames below the confidence floor
// are marked unvoiced (frequencyHz = 0).
std::vector<PitchFrame> detectPitchTrack(const std::vector<float>& interleaved, int channels,
                                         double sampleRate, double hopSeconds = 0.01,
                                         double minHz = 65.0, double maxHz = 1500.0);

// pYIN-style temporal cleanup of a raw YIN track: snap octave-jumped frames back to the octave of
// their neighbours, median-smooth single-frame frequency glitches, and fill 1-frame unvoiced gaps
// between voiced frames. Cuts the octave errors and jitter a per-frame detector leaves. detectPitchTrack
// runs this automatically; exposed for testing.
std::vector<PitchFrame> smoothPitchTrack(const std::vector<PitchFrame>& track);

// Group the pitch track into notes: consecutive voiced frames within ~`toleranceSemitones` of the
// running note pitch, at least `minDurationSeconds` long. Returns notes ascending in time.
std::vector<DetectedNote> segmentNotes(const std::vector<PitchFrame>& track,
                                       double minDurationSeconds = 0.06,
                                       double toleranceSemitones = 0.6);

// Rescale a note's own pitch CONTOUR in place: its deviation from the note's median pitch is split
// into a slow part (drift) and a fast part (vibrato) and each is multiplied by its scale, then the
// audio is resampled at a varying rate to follow the new contour. Both scales at 1 is a no-op.
//
// Variable-rate resampling is what makes this cheap and clean: the required correction is tiny (a
// ±0.5-semitone vibrato is a ±3 % rate change), so no phase vocoder is needed and none of its
// smearing is incurred. The rate curve is normalised to mean 1 so the note keeps its length.
// Exposed for testing; renderNoteEdits applies it before the constant pitch shift.
std::vector<float> scaleNotePitchContour(const std::vector<float>& interleaved, int channels,
                                         double sampleRate, double modulationScale, double driftScale);

// Render the input with each note pitch-shifted by its pitchOffsetSemitones (a phase-vocoder shift of
// just that note's audio span, length preserved, short crossfades at the note edges to hide seams).
// Notes with a zero offset are copied through untouched. Output is the same length as the input.
std::vector<float> renderNoteEdits(const std::vector<float>& interleaved, int channels,
                                   double sampleRate, const std::vector<DetectedNote>& notes);

} // namespace neuracoust::daw
