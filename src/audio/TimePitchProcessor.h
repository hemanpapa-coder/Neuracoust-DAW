#pragma once

#include <vector>

namespace neuracoust::daw {

// Offline time-stretch + pitch-shift, ported from the Neuracoust Time & Pitch editor's phase vocoder
// (Serato project, `stretchAndPitch`). Independent of tempo/key — timeRatio changes length without
// pitch, semitones change pitch without length. A phase-vocoder STFT (2048-pt, 512 hop) resynthesizes
// the stretch, then a resample applies the pitch. Offline only (allocates, not realtime-safe).
struct TimePitchParams {
    double timeRatio = 1.0;   // output length / input length (0.125 … 8). 2.0 = twice as long.
    double semitones = 0.0;   // pitch shift in semitones (± ~24 sensible).
};

// Interleaved in/out. `channels` ≥ 1. Output length ≈ inputFrames * timeRatio (per channel). An empty
// or degenerate input returns empty. timeRatio==1 && semitones==0 returns the input unchanged.
std::vector<float> processTimePitchInterleaved(const std::vector<float>& interleaved,
                                               int channels, const TimePitchParams& params);

// Restore the ORIGINAL spectral envelope (formants) onto a pitch-shifted signal of the same length,
// so a shifted vocal/instrument stays natural instead of "chipmunk"/"muddy". Per frame it divides out
// the shifted signal's cepstral envelope and multiplies in the original's — harmonics stay shifted,
// formants return to where they were. Both buffers same channels/layout; uses the shorter length.
std::vector<float> formantCorrect(const std::vector<float>& shifted, const std::vector<float>& original,
                                  int channels, double sampleRate);

// Shift the spectral envelope (the formants) WITHOUT moving the harmonics — the complement of
// formantCorrect. Positive semitones move the formants up (brighter/smaller-sounding source),
// negative down, at an unchanged pitch. This is Melodyne's formant tool: it changes who is singing,
// not what note they are singing.
//
// Same cepstral envelope as formantCorrect, warped along the frequency axis: the envelope wanted at
// bin b is the original envelope at b / 2^(semitones/12), so the gain applied is that ratio.
// 0 semitones returns the input unchanged.
std::vector<float> formantShift(const std::vector<float>& signal, int channels, double sampleRate,
                                double semitones);

// Piecewise time remap (ported from Serato's processWithTimeMap). sourceAnchors/destAnchors are
// matched NORMALIZED positions in [0,1]; each segment between anchors is stretched independently to
// its destination span, all at the global pitch (params.semitones) and overall length (timeRatio).
// Empty/mismatched anchors fall back to a uniform processTimePitchInterleaved.
std::vector<float> processTimeMapInterleaved(const std::vector<float>& interleaved, int channels,
                                             const TimePitchParams& params,
                                             const std::vector<double>& sourceAnchors,
                                             const std::vector<double>& destAnchors);

// Same as processTimeMapInterleaved, but preserves formants through a simultaneous time-stretch AND
// pitch shift (WORLD-style source/filter separation): the shifted result gets the ORIGINAL spectral
// envelope restored, taken from a pitch-neutral stretch of the same content at the same output length.
// A shifted vocal/instrument keeps its timbre instead of chipmunking. No pitch change → returns the
// plain time-remap unchanged. Offline only.
std::vector<float> processTimeMapFormantPreserving(const std::vector<float>& interleaved, int channels,
                                                   const TimePitchParams& params, double sampleRate,
                                                   const std::vector<double>& sourceAnchors,
                                                   const std::vector<double>& destAnchors);

} // namespace neuracoust::daw
