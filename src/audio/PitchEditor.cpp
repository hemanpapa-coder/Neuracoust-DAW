#include "audio/PitchEditor.h"

#include "audio/TimePitchProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace neuracoust::daw {

namespace {
constexpr double kYinThreshold = 0.15;   // YIN absolute threshold on the normalized difference.
constexpr double kPi = 3.14159265358979323846;

double hzToMidi(double hz) { return hz > 0.0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0.0; }
double midiToHz(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

// pYIN-style Viterbi over octave candidates: within each voiced run, each frame may keep its detected
// pitch or shift by whole octaves; the min-cost path balances staying near the detection (emission)
// against pitch continuity (transition). Fixes octave errors GLOBALLY — even runs of them — while a
// genuine sustained octave leap is cheaper to follow than to fight, so real melody is preserved.
void viterbiOctaveSmooth(std::vector<PitchFrame>& t) {
    constexpr int K = 5;                 // octave candidates k = -2..+2
    constexpr double emitPen = 3.0;      // cost of shifting a frame one octave off its detection
    constexpr double transW = 1.0;       // cost per semitone of frame-to-frame pitch jump
    const int n = static_cast<int>(t.size());
    int i = 0;
    while (i < n) {
        if (t[static_cast<size_t>(i)].frequencyHz <= 0.0) { ++i; continue; }
        int j = i;
        while (j < n && t[static_cast<size_t>(j)].frequencyHz > 0.0) ++j;   // voiced run [i, j)
        const int len = j - i;
        if (len >= 2) {
            std::vector<double> base(static_cast<size_t>(len));
            for (int f = 0; f < len; ++f) base[static_cast<size_t>(f)] = hzToMidi(t[static_cast<size_t>(i + f)].frequencyHz);
            std::vector<std::array<double, K>> cost(static_cast<size_t>(len));
            std::vector<std::array<int, K>> prev(static_cast<size_t>(len));
            for (int k = 0; k < K; ++k) cost[0][static_cast<size_t>(k)] = emitPen * std::abs(k - 2);
            for (int f = 1; f < len; ++f) {
                for (int k = 0; k < K; ++k) {
                    const double curMidi = base[static_cast<size_t>(f)] + 12.0 * (k - 2);
                    double best = 1e18; int bp = 0;
                    for (int p = 0; p < K; ++p) {
                        const double prevMidi = base[static_cast<size_t>(f - 1)] + 12.0 * (p - 2);
                        const double c = cost[static_cast<size_t>(f - 1)][static_cast<size_t>(p)] + transW * std::abs(curMidi - prevMidi);
                        if (c < best) { best = c; bp = p; }
                    }
                    cost[static_cast<size_t>(f)][static_cast<size_t>(k)] = best + emitPen * std::abs(k - 2);
                    prev[static_cast<size_t>(f)][static_cast<size_t>(k)] = bp;
                }
            }
            int kbest = 0; double best = 1e18;
            for (int k = 0; k < K; ++k) if (cost[static_cast<size_t>(len - 1)][static_cast<size_t>(k)] < best) { best = cost[static_cast<size_t>(len - 1)][static_cast<size_t>(k)]; kbest = k; }
            std::vector<int> path(static_cast<size_t>(len));
            path[static_cast<size_t>(len - 1)] = kbest;
            for (int f = len - 1; f > 0; --f) path[static_cast<size_t>(f - 1)] = prev[static_cast<size_t>(f)][static_cast<size_t>(path[static_cast<size_t>(f)])];
            for (int f = 0; f < len; ++f)
                t[static_cast<size_t>(i + f)].frequencyHz = midiToHz(base[static_cast<size_t>(f)] + 12.0 * (path[static_cast<size_t>(f)] - 2));
        }
        i = j;
    }
}

// In-place radix-2 FFT (sign −1 forward), local so this file stands alone.
void fftRadix2(std::vector<std::complex<float>>& a, int sign) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = sign * 2.0 * kPi / static_cast<double>(len);
        const std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Mono mix (channel average) of an interleaved buffer.
std::vector<float> monoMix(const std::vector<float>& in, int channels, int64_t frames) {
    std::vector<float> mono(static_cast<size_t>(frames), 0.0f);
    for (int64_t i = 0; i < frames; ++i) {
        double sum = 0.0;
        for (int c = 0; c < channels; ++c) sum += in[static_cast<size_t>(i) * channels + c];
        mono[static_cast<size_t>(i)] = static_cast<float>(sum / channels);
    }
    return mono;
}
}  // namespace

std::vector<PitchFrame> detectPitchTrack(const std::vector<float>& interleaved, int channels,
                                         double sampleRate, double hopSeconds,
                                         double minHz, double maxHz) {
    std::vector<PitchFrame> track;
    if (channels < 1 || interleaved.empty() || sampleRate <= 1000.0) return track;
    const int64_t frames = static_cast<int64_t>(interleaved.size()) / channels;
    const std::vector<float> x = monoMix(interleaved, channels, frames);

    const int maxTau = std::min<int>(static_cast<int>(sampleRate / std::max(20.0, minHz)), static_cast<int>(frames) - 2);
    const int minTau = std::max(2, static_cast<int>(sampleRate / std::max(minHz + 1.0, maxHz)));
    if (maxTau <= minTau + 1) return track;
    const int W = maxTau;                                   // integration window
    const int hop = std::max(1, static_cast<int>(hopSeconds * sampleRate));

    std::vector<double> d(maxTau + 1, 0.0), dp(maxTau + 1, 0.0);
    for (int64_t base = 0; base + W + maxTau < frames; base += hop) {
        // Energy gate: silence → unvoiced.
        double energy = 0.0;
        for (int j = 0; j < W; ++j) { const double v = x[static_cast<size_t>(base + j)]; energy += v * v; }
        const double rms = std::sqrt(energy / W);

        PitchFrame frame;
        frame.timeSeconds = static_cast<double>(base + W / 2) / sampleRate;
        if (rms < 1.0e-4) { track.push_back(frame); continue; }

        // 1) difference function.
        for (int tau = 1; tau <= maxTau; ++tau) {
            double sum = 0.0;
            for (int j = 0; j < W; ++j) {
                const double diff = x[static_cast<size_t>(base + j)] - x[static_cast<size_t>(base + j + tau)];
                sum += diff * diff;
            }
            d[tau] = sum;
        }
        // 2) cumulative mean normalized difference.
        dp[0] = 1.0;
        double running = 0.0;
        for (int tau = 1; tau <= maxTau; ++tau) {
            running += d[tau];
            dp[tau] = d[tau] * tau / (running > 1e-12 ? running : 1e-12);
        }
        // 3) absolute threshold: first local min below threshold in range.
        int tauEst = -1;
        for (int tau = minTau; tau < maxTau; ++tau) {
            if (dp[tau] < kYinThreshold) {
                while (tau + 1 < maxTau && dp[tau + 1] < dp[tau]) ++tau;   // descend to the local min
                tauEst = tau;
                break;
            }
        }
        if (tauEst < 0) {   // no confident pitch — take the global min but mark low confidence.
            int best = minTau;
            for (int tau = minTau; tau < maxTau; ++tau) if (dp[tau] < dp[best]) best = tau;
            if (dp[best] > 0.6) { track.push_back(frame); continue; }   // too weak → unvoiced
            tauEst = best;
        }
        // 4) parabolic interpolation for sub-sample tau.
        double betterTau = tauEst;
        if (tauEst > 0 && tauEst < maxTau) {
            const double a = dp[tauEst - 1], b = dp[tauEst], c = dp[tauEst + 1];
            const double denom = 2.0 * (2.0 * b - a - c);
            if (std::abs(denom) > 1e-12) betterTau = tauEst + (a - c) / denom;
        }
        frame.frequencyHz = sampleRate / betterTau;
        frame.confidence = std::clamp(1.0 - dp[tauEst], 0.0, 1.0);
        if (frame.frequencyHz < minHz || frame.frequencyHz > maxHz) frame.frequencyHz = 0.0;
        track.push_back(frame);
    }
    return smoothPitchTrack(track);   // pYIN-style cleanup: octave correction, median smoothing, gap fill
}

std::vector<PitchFrame> smoothPitchTrack(const std::vector<PitchFrame>& in) {
    std::vector<PitchFrame> t = in;
    const int n = static_cast<int>(t.size());
    if (n < 3) return t;

    // 1) Octave correction — a global Viterbi over octave candidates (better than a local median: it
    //    fixes runs of octave errors and preserves genuine sustained octave leaps).
    viterbiOctaveSmooth(t);
    // 2) Median-3 smoothing of the frequency (voiced frames only) to remove single-frame jitter.
    std::vector<double> freq(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) freq[static_cast<size_t>(i)] = t[static_cast<size_t>(i)].frequencyHz;
    for (int i = 1; i < n - 1; ++i) {
        if (freq[static_cast<size_t>(i - 1)] > 0 && freq[static_cast<size_t>(i)] > 0 && freq[static_cast<size_t>(i + 1)] > 0) {
            double a = freq[static_cast<size_t>(i - 1)], b = freq[static_cast<size_t>(i)], c = freq[static_cast<size_t>(i + 1)];
            const double med = std::max(std::min(a, b), std::min(std::max(a, b), c));
            t[static_cast<size_t>(i)].frequencyHz = med;
        }
    }
    // 3) Voicing hysteresis: fill a lone unvoiced frame between two voiced frames of similar pitch.
    for (int i = 1; i < n - 1; ++i) {
        if (t[static_cast<size_t>(i)].frequencyHz <= 0.0 &&
            t[static_cast<size_t>(i - 1)].frequencyHz > 0.0 && t[static_cast<size_t>(i + 1)].frequencyHz > 0.0 &&
            std::abs(hzToMidi(t[static_cast<size_t>(i - 1)].frequencyHz) - hzToMidi(t[static_cast<size_t>(i + 1)].frequencyHz)) < 1.5) {
            t[static_cast<size_t>(i)].frequencyHz = 0.5 * (t[static_cast<size_t>(i - 1)].frequencyHz + t[static_cast<size_t>(i + 1)].frequencyHz);
            t[static_cast<size_t>(i)].confidence = 0.5 * (t[static_cast<size_t>(i - 1)].confidence + t[static_cast<size_t>(i + 1)].confidence);
        }
    }
    return t;
}

std::vector<DetectedNote> segmentNotes(const std::vector<PitchFrame>& track,
                                       double minDurationSeconds, double toleranceSemitones) {
    std::vector<DetectedNote> notes;
    size_t i = 0;
    const size_t n = track.size();
    while (i < n) {
        if (track[i].frequencyHz <= 0.0) { ++i; continue; }
        // Start a note; extend while pitch stays within tolerance of the running median.
        std::vector<double> midis, hzs, confs;
        const double startT = track[i].timeSeconds;
        double lastT = startT;
        size_t j = i;
        while (j < n && track[j].frequencyHz > 0.0) {
            const double midi = hzToMidi(track[j].frequencyHz);
            if (!midis.empty()) {
                std::vector<double> sorted = midis;
                std::sort(sorted.begin(), sorted.end());
                const double med = sorted[sorted.size() / 2];
                if (std::abs(midi - med) > toleranceSemitones) break;
            }
            midis.push_back(midi);
            hzs.push_back(track[j].frequencyHz);
            confs.push_back(track[j].confidence);
            lastT = track[j].timeSeconds;
            ++j;
        }
        const double dur = lastT - startT;
        if (dur >= minDurationSeconds && !midis.empty()) {
            std::sort(hzs.begin(), hzs.end());
            std::vector<double> sortedMidi = midis;
            std::sort(sortedMidi.begin(), sortedMidi.end());
            DetectedNote note;
            note.startSeconds = startT;
            note.durationSeconds = dur;
            note.medianFrequencyHz = hzs[hzs.size() / 2];
            note.detectedMidi = sortedMidi[sortedMidi.size() / 2];
            double cs = 0.0; for (double c : confs) cs += c;
            note.confidence = confs.empty() ? 0.0 : cs / confs.size();
            notes.push_back(note);
        }
        i = std::max(j, i + 1);
    }
    return notes;
}

std::vector<float> renderNoteEdits(const std::vector<float>& interleaved, int channels,
                                   double sampleRate, const std::vector<DetectedNote>& notes) {
    if (channels < 1 || interleaved.empty()) return interleaved;
    const int64_t frames = static_cast<int64_t>(interleaved.size()) / channels;
    std::vector<float> out = interleaved;   // untouched notes / gaps pass through
    const int fade = std::max(1, static_cast<int>(0.005 * sampleRate));   // 5 ms edge crossfade

    for (const auto& note : notes) {
        if (std::abs(note.pitchOffsetSemitones) < 0.01) continue;
        const int64_t startF = std::max<int64_t>(0, std::llround(note.startSeconds * sampleRate));
        const int64_t nF = std::min<int64_t>(std::llround(note.durationSeconds * sampleRate), frames - startF);
        if (nF <= fade * 2) continue;

        // Extract the note span, pitch-shift it in place (length preserved), crossfade the edges.
        std::vector<float> seg(static_cast<size_t>(nF) * channels);
        for (int64_t i = 0; i < nF; ++i)
            for (int c = 0; c < channels; ++c)
                seg[static_cast<size_t>(i) * channels + c] = interleaved[static_cast<size_t>(startF + i) * channels + c];

        TimePitchParams p;
        p.timeRatio = 1.0;
        p.semitones = note.pitchOffsetSemitones;
        std::vector<float> shifted = processTimePitchInterleaved(seg, channels, p);
        // Keep the note's formants where they were (natural voice/instrument, no "chipmunk").
        shifted = formantCorrect(shifted, seg, channels, sampleRate);
        const int64_t sF = std::min<int64_t>(static_cast<int64_t>(shifted.size()) / channels, nF);

        for (int64_t i = 0; i < sF; ++i) {
            float w = 1.0f;   // crossfade shifted (w) vs original (1-w) at the note edges
            if (i < fade) w = static_cast<float>(i) / fade;
            else if (i > sF - fade) w = static_cast<float>(sF - i) / fade;
            for (int c = 0; c < channels; ++c) {
                const size_t o = static_cast<size_t>(startF + i) * channels + c;
                out[o] = shifted[static_cast<size_t>(i) * channels + c] * w + out[o] * (1.0f - w);
            }
        }
    }
    return out;
}

std::vector<double> detectOnsets(const std::vector<float>& interleaved, int channels, double sampleRate,
                                 double minIntervalSeconds) {
    std::vector<double> onsets;
    if (channels < 1 || interleaved.empty() || sampleRate <= 1000.0) return onsets;
    const int64_t frames = static_cast<int64_t>(interleaved.size()) / channels;
    const std::vector<float> x = monoMix(interleaved, channels, frames);

    constexpr int W = 1024, hop = 512, bins = W / 2 + 1;
    if (frames < W) return onsets;
    std::vector<float> window(W);
    for (int i = 0; i < W; ++i) window[i] = 0.5f * (1.0f - std::cos(2.0 * kPi * i / (W - 1)));

    // Spectral flux per frame: sum of positive magnitude increases over the previous frame.
    std::vector<double> flux;
    std::vector<float> prevMag(bins, 0.0f);
    std::vector<std::complex<float>> spec(W);
    for (int64_t base = 0; base + W <= frames; base += hop) {
        for (int i = 0; i < W; ++i) spec[static_cast<size_t>(i)] = std::complex<float>(x[static_cast<size_t>(base + i)] * window[i], 0.0f);
        fftRadix2(spec, -1);
        double f = 0.0;
        for (int b = 0; b < bins; ++b) {
            const float mag = std::abs(spec[static_cast<size_t>(b)]);
            const float diff = mag - prevMag[static_cast<size_t>(b)];
            if (diff > 0.0f) f += diff;
            prevMag[static_cast<size_t>(b)] = mag;
        }
        flux.push_back(f);
    }
    if (flux.size() < 3) return onsets;

    // Adaptive peak-pick: a local maximum that clears a local-mean threshold, with a refractory gap.
    const int win = 8;             // ± frames for the local mean
    const double mult = 1.6, floorFrac = 0.08;
    double globalMax = 0.0; for (double v : flux) globalMax = std::max(globalMax, v);
    const double absFloor = globalMax * floorFrac;
    const double minGapFrames = minIntervalSeconds * sampleRate / hop;
    double lastOnsetFrame = -1e9;
    for (int i = 1; i + 1 < static_cast<int>(flux.size()); ++i) {
        if (flux[i] <= flux[i - 1] || flux[i] < flux[i + 1]) continue;   // local max
        if (flux[i] < absFloor) continue;
        double sum = 0.0; int cnt = 0;
        for (int j = std::max(0, i - win); j <= std::min<int>(flux.size() - 1, i + win); ++j) { sum += flux[j]; ++cnt; }
        const double localMean = cnt ? sum / cnt : 0.0;
        if (flux[i] < localMean * mult) continue;
        if (i - lastOnsetFrame < minGapFrames) continue;
        lastOnsetFrame = i;
        onsets.push_back(static_cast<double>(i) * hop / sampleRate);
    }
    return onsets;
}

std::vector<DetectedNote> detectNotesForMode(const std::vector<float>& interleaved, int channels,
                                             double sampleRate, DetectionMode mode) {
    if (mode == DetectionMode::Percussive) {
        const auto onsets = detectOnsets(interleaved, channels, sampleRate);
        std::vector<DetectedNote> events;
        const int64_t frames = channels > 0 ? static_cast<int64_t>(interleaved.size()) / channels : 0;
        const double endS = frames > 0 ? static_cast<double>(frames) / sampleRate : 0.0;
        for (size_t i = 0; i < onsets.size(); ++i) {
            DetectedNote e;
            e.startSeconds = onsets[i];
            e.durationSeconds = (i + 1 < onsets.size() ? onsets[i + 1] : endS) - onsets[i];
            e.detectedMidi = 0.0;      // percussive: no pitch, a rhythmic marker
            e.confidence = 1.0;
            events.push_back(e);
        }
        return events;
    }
    // Melodic (and, for now, Polyphonic — a Demucs-assisted per-part path is the planned upgrade).
    const auto track = detectPitchTrack(interleaved, channels, sampleRate);
    return segmentNotes(track);
}

} // namespace neuracoust::daw
