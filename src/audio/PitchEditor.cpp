#include "audio/PitchEditor.h"

#include "audio/TimePitchProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
        // A note ends where the pitch leaves the note's centre AND STAYS out. Breaking on the first
        // frame outside tolerance shreds any sung note with vibrato: a ±0.5-semitone wobble reads as
        // a new note every half cycle, and a 1.6 s held note came back as fifteen 90 ms fragments.
        // Vibrato returns within a fraction of a cycle; a real interval does not.
        const double hopSeconds = (n > 1) ? std::max(1e-4, track[1].timeSeconds - track[0].timeSeconds) : 0.01;
        const size_t warmupFrames = static_cast<size_t>(std::max(4.0, 0.12 / hopSeconds));
        const double sustainSeconds = 0.06;
        size_t breachStart = std::numeric_limits<size_t>::max();
        while (j < n && track[j].frequencyHz > 0.0) {
            const double midi = hzToMidi(track[j].frequencyHz);
            // Until the note has enough frames for its median to mean anything, an early sample can
            // sit at one extreme of the wobble and drag the centre with it — so don't judge yet.
            if (midis.size() >= warmupFrames) {
                std::vector<double> sorted = midis;
                std::sort(sorted.begin(), sorted.end());
                const double med = sorted[sorted.size() / 2];
                if (std::abs(midi - med) > toleranceSemitones) {
                    if (breachStart == std::numeric_limits<size_t>::max()) breachStart = j;
                    if (track[j].timeSeconds - track[breachStart].timeSeconds >= sustainSeconds) {
                        // The note really changed. It ended where the departure began, and the next
                        // note starts there — not here, or the transition would be swallowed.
                        const size_t keep = breachStart - i;
                        midis.resize(keep); hzs.resize(keep); confs.resize(keep);
                        lastT = track[breachStart].timeSeconds;
                        j = breachStart;
                        break;
                    }
                } else {
                    breachStart = std::numeric_limits<size_t>::max();
                }
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


namespace {

/// Melodyne's attack-speed handle: time-warp the note's OWN amplitude envelope over its attack.
///
/// The envelope is followed with a short RMS window; the attack is the run up to 90 % of the note's
/// peak. Inside it the gain applied at time t is env(t · speed) / env(t) — so speed 1 is exactly
/// unity everywhere, speed > 1 pulls the rise earlier (sharper), speed < 1 pushes it later (softer).
/// Working from the note's real envelope is what keeps 1.0 transparent; a fixed curve would not be.
void applyAttackSpeed(std::vector<float>& seg, int channels, double sampleRate, double speed) {
    if (channels < 1 || seg.empty() || sampleRate <= 1000.0) return;
    const double s = std::clamp(speed, 0.1, 8.0);
    if (std::abs(s - 1.0) < 0.01) return;
    const int64_t frames = static_cast<int64_t>(seg.size()) / channels;
    if (frames < 16) return;

    const int window = std::max(8, static_cast<int>(0.005 * sampleRate));   // 5 ms RMS follower
    std::vector<float> env(static_cast<size_t>(frames), 0.0f);
    double running = 0.0;
    for (int64_t i = 0; i < frames; ++i) {
        double mono = 0.0;
        for (int c = 0; c < channels; ++c) mono += seg[static_cast<size_t>(i) * channels + c];
        mono /= channels;
        running += mono * mono;
        if (i >= window) {
            double old = 0.0;
            for (int c = 0; c < channels; ++c) old += seg[static_cast<size_t>(i - window) * channels + c];
            old /= channels;
            running -= old * old;
        }
        env[static_cast<size_t>(i)] = static_cast<float>(std::sqrt(std::max(0.0, running) /
                                                                  std::min<int64_t>(window, i + 1)));
    }

    float peak = 0.0f;
    for (float v : env) peak = std::max(peak, v);
    if (peak <= 1e-6f) return;
    int64_t attackEnd = 0;
    while (attackEnd < frames && env[static_cast<size_t>(attackEnd)] < 0.9f * peak) ++attackEnd;
    // A note with no rise to speak of (a sustain fragment) has no attack to reshape.
    if (attackEnd < 8 || attackEnd >= frames) return;

    for (int64_t i = 0; i < attackEnd; ++i) {
        const double source = std::min<double>(attackEnd - 1, static_cast<double>(i) * s);
        const int64_t lo = static_cast<int64_t>(source);
        const float frac = static_cast<float>(source - lo);
        const float wanted = env[static_cast<size_t>(lo)] * (1.0f - frac) +
                             env[static_cast<size_t>(std::min<int64_t>(attackEnd - 1, lo + 1))] * frac;
        float g = wanted / (env[static_cast<size_t>(i)] + 1e-6f);
        g = std::clamp(g, 0.0f, 8.0f);
        for (int c = 0; c < channels; ++c) seg[static_cast<size_t>(i) * channels + c] *= g;
    }
}

} // namespace

std::vector<float> scaleNotePitchContour(const std::vector<float>& interleaved, int channels,
                                         double sampleRate, double modulationScale, double driftScale) {
    if (channels < 1 || interleaved.empty() || sampleRate <= 1000.0) return interleaved;
    const double mod = std::clamp(modulationScale, 0.0, 4.0);
    const double drift = std::clamp(driftScale, 0.0, 4.0);
    if (std::abs(mod - 1.0) < 0.01 && std::abs(drift - 1.0) < 0.01) return interleaved;
    const int64_t frames = static_cast<int64_t>(interleaved.size()) / channels;
    if (frames < static_cast<int64_t>(0.08 * sampleRate)) return interleaved;

    // The note's own contour, in semitones relative to its median. Unvoiced frames get the median,
    // so a breathy tail contributes no correction rather than a wild one.
    const auto track = detectPitchTrack(interleaved, channels, sampleRate, 0.01);
    std::vector<double> voiced;
    for (const auto& f : track) if (f.frequencyHz > 0.0) voiced.push_back(f.frequencyHz);
    if (voiced.size() < 8) return interleaved;
    std::vector<double> sorted = voiced;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double medianHz = sorted[sorted.size() / 2];
    if (medianHz <= 0.0) return interleaved;

    std::vector<double> deviation(track.size(), 0.0);
    for (size_t i = 0; i < track.size(); ++i) {
        if (track[i].frequencyHz > 0.0) {
            deviation[i] = 12.0 * std::log2(track[i].frequencyHz / medianHz);
        }
    }
    // Slow part = a centred moving average about 0.35 s wide; fast part = what is left. 0.35 s sits
    // safely below a vibrato period (4–7 Hz) and above the note-to-note movement we must not touch.
    const double hopSeconds = track.size() > 1
        ? std::max(1e-4, track[1].timeSeconds - track[0].timeSeconds)
        : 0.01;
    const int half = std::max(1, static_cast<int>(0.175 / hopSeconds));
    // TWO passes of the box, not one. A single box of length L leaves |sinc(f·L)| of a tone through
    // — 13 % of a 5 Hz vibrato at L = 0.35 s, which is exactly the vibrato this is meant to separate
    // out, so "modulation 0" only flattened it by half. Two passes square that to under 2 % while
    // barely touching the sub-1 Hz drift the other scale is for.
    auto boxSmooth = [half](const std::vector<double>& in) {
        std::vector<double> out(in.size(), 0.0);
        for (size_t i = 0; i < in.size(); ++i) {
            const int64_t a = std::max<int64_t>(0, static_cast<int64_t>(i) - half);
            const int64_t b = std::min<int64_t>(static_cast<int64_t>(in.size()) - 1,
                                                static_cast<int64_t>(i) + half);
            double sum = 0.0;
            for (int64_t k = a; k <= b; ++k) sum += in[static_cast<size_t>(k)];
            out[i] = sum / static_cast<double>(b - a + 1);
        }
        return out;
    };
    const std::vector<double> slow = boxSmooth(boxSmooth(deviation));

    // The correction we must ADD to the existing contour, per analysis frame.
    std::vector<double> correction(deviation.size(), 0.0);
    for (size_t i = 0; i < deviation.size(); ++i) {
        const double fast = deviation[i] - slow[i];
        correction[i] = (drift - 1.0) * slow[i] + (mod - 1.0) * fast;
    }

    // Per-sample playback rate. Reading the source faster raises the pitch, so rate = 2^(Δ/12).
    std::vector<double> rate(static_cast<size_t>(frames), 1.0);
    double rateSum = 0.0;
    for (int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double pos = t / hopSeconds;
        const size_t lo = static_cast<size_t>(std::max(0.0, pos));
        const size_t hi = std::min(correction.size() - 1, lo + 1);
        const double frac = pos - static_cast<double>(lo);
        const double delta = lo < correction.size()
            ? correction[lo] * (1.0 - frac) + correction[hi] * frac
            : 0.0;
        rate[static_cast<size_t>(i)] = std::pow(2.0, std::clamp(delta, -6.0, 6.0) / 12.0);
        rateSum += rate[static_cast<size_t>(i)];
    }
    // Normalise to mean 1 so the note comes out exactly as long as it went in — a vibrato deviation
    // is zero-mean in theory but never quite is in practice, and the drift part is not at all.
    const double meanRate = rateSum / static_cast<double>(frames);
    if (meanRate > 1e-6) {
        for (auto& r : rate) r /= meanRate;
    }

    // Variable-rate read with cubic (Catmull-Rom) interpolation.
    std::vector<float> out(interleaved.size(), 0.0f);
    double readPos = 0.0;
    for (int64_t i = 0; i < frames; ++i) {
        const int64_t base = static_cast<int64_t>(readPos);
        const double frac = readPos - static_cast<double>(base);
        for (int c = 0; c < channels; ++c) {
            auto at = [&](int64_t index) {
                const int64_t k = std::clamp<int64_t>(index, 0, frames - 1);
                return static_cast<double>(interleaved[static_cast<size_t>(k) * channels + c]);
            };
            const double p0 = at(base - 1), p1 = at(base), p2 = at(base + 1), p3 = at(base + 2);
            const double a = 0.5 * (-p0 + 3.0 * p1 - 3.0 * p2 + p3);
            const double b = 0.5 * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3);
            const double d = 0.5 * (-p0 + p2);
            const double v = ((a * frac + b) * frac + d) * frac + p1;
            out[static_cast<size_t>(i) * channels + c] = static_cast<float>(v);
        }
        readPos += rate[static_cast<size_t>(i)];
        if (readPos > static_cast<double>(frames - 1)) readPos = static_cast<double>(frames - 1);
    }
    return out;
}

std::vector<float> renderNoteEdits(const std::vector<float>& interleaved, int channels,
                                   double sampleRate, const std::vector<DetectedNote>& notes) {
    if (channels < 1 || interleaved.empty()) return interleaved;
    const int64_t frames = static_cast<int64_t>(interleaved.size()) / channels;
    std::vector<float> out = interleaved;   // untouched notes / gaps pass through
    const int fade = std::max(1, static_cast<int>(0.005 * sampleRate));   // 5 ms edge crossfade

    for (const auto& note : notes) {
        const bool pitchChanged = std::abs(note.pitchOffsetSemitones) >= 0.01;
        const bool timeChanged = std::abs(note.timeOffsetSeconds) >= 0.0001;
        const bool durationChanged = std::abs(note.durationScale - 1.0) >= 0.001;
        const bool formantChanged = std::abs(note.formantSemitones) >= 0.01;
        const bool levelChanged = note.muted || std::abs(note.gainDb) >= 0.01;
        const bool attackChanged = std::abs(note.attackSpeed - 1.0) >= 0.01;
        const bool contourChanged = std::abs(note.pitchModulationScale - 1.0) >= 0.01 ||
                                    std::abs(note.pitchDriftScale - 1.0) >= 0.01;
        if (!pitchChanged && !timeChanged && !durationChanged &&
            !formantChanged && !levelChanged && !attackChanged && !contourChanged) continue;
        const int64_t startF = std::max<int64_t>(0, std::llround(note.startSeconds * sampleRate));
        const int64_t nF = std::min<int64_t>(std::llround(note.durationSeconds * sampleRate), frames - startF);
        if (nF <= fade * 2) continue;

        // Extract the note span, pitch-shift it in place (length preserved), crossfade the edges.
        std::vector<float> seg(static_cast<size_t>(nF) * channels);
        for (int64_t i = 0; i < nF; ++i)
            for (int c = 0; c < channels; ++c)
                seg[static_cast<size_t>(i) * channels + c] = interleaved[static_cast<size_t>(startF + i) * channels + c];

        // The contour rescale runs on the note's ORIGINAL audio, before any constant shift: it is
        // measured against the note's own median pitch, and a shift would move that median.
        if (contourChanged) {
            seg = scaleNotePitchContour(seg, channels, sampleRate,
                                        note.pitchModulationScale, note.pitchDriftScale);
        }

        std::vector<float> shifted = seg;
        if (pitchChanged || durationChanged) {
            TimePitchParams p;
            p.timeRatio = note.durationScale;
            p.semitones = note.pitchOffsetSemitones;
            shifted = processTimePitchInterleaved(seg, channels, p);
            // Keep the note's formants where they were (natural voice/instrument, no "chipmunk").
            shifted = formantCorrect(shifted, seg, channels, sampleRate);
        }
        // The formant tool moves the timbre AFTER the pitch shift has been formant-corrected back to
        // the original — so its number means what it says (move the formants by this much from where
        // they started) rather than fighting the correction.
        if (formantChanged) {
            shifted = formantShift(shifted, channels, sampleRate, note.formantSemitones);
        }
        if (attackChanged) {
            applyAttackSpeed(shifted, channels, sampleRate, note.attackSpeed);
        }
        if (levelChanged) {
            const float g = note.muted ? 0.0f
                                       : static_cast<float>(std::pow(10.0, std::clamp(note.gainDb, -24.0, 24.0) / 20.0));
            for (auto& sample : shifted) sample *= g;
        }
        const int64_t sF = std::min<int64_t>(static_cast<int64_t>(shifted.size()) / channels, nF);
        const int64_t destF = std::clamp<int64_t>(
            std::llround((note.startSeconds + note.timeOffsetSeconds) * sampleRate), 0, frames - 1);

        // A horizontal move removes the source note with the same edge taper before
        // mixing it at the destination. This preserves the clip length and permits
        // overlaps, like Melodyne's note tool.
        if (timeChanged || durationChanged) {
            for (int64_t i = 0; i < nF; ++i) {
                float w = 1.0f;
                if (i < fade) w = static_cast<float>(i) / fade;
                else if (i > nF - fade) w = static_cast<float>(nF - i) / fade;
                for (int c = 0; c < channels; ++c) {
                    const size_t o = static_cast<size_t>(startF + i) * channels + c;
                    out[o] *= (1.0f - std::max(0.0f, w));
                }
            }
        }

        const int64_t placeFrames = std::min<int64_t>(sF, frames - destF);
        for (int64_t i = 0; i < placeFrames; ++i) {
            float w = 1.0f;   // crossfade shifted (w) vs original (1-w) at the note edges
            if (i < fade) w = static_cast<float>(i) / fade;
            else if (i > placeFrames - fade) w = static_cast<float>(placeFrames - i) / fade;
            for (int c = 0; c < channels; ++c) {
                const size_t o = static_cast<size_t>(destF + i) * channels + c;
                if (timeChanged) out[o] += shifted[static_cast<size_t>(i) * channels + c] * w;
                else if (note.muted) out[o] = shifted[static_cast<size_t>(i) * channels + c] +
                                              out[o] * (1.0f - w);
                else out[o] = shifted[static_cast<size_t>(i) * channels + c] * w + out[o] * (1.0f - w);
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

// Native chord-capable fallback. A long-window STFT scores every chromatic pitch by its harmonic
// stack, then joins active pitch bins over time. It is intentionally independent of Python/models:
// Basic Pitch can replace it when bundled, but Polyphonic must never silently become Melodic.
std::vector<DetectedNote> detectPolyphonicNotes(const std::vector<float>& interleaved,
                                                int channels, double sampleRate) {
    std::vector<DetectedNote> notes;
    if (channels < 1 || interleaved.empty() || sampleRate <= 1000.0) return notes;
    const int64_t frames = static_cast<int64_t>(interleaved.size()) / channels;
    const auto mono = monoMix(interleaved, channels, frames);
    constexpr int W = 4096, hop = 512, midiLo = 28, midiHi = 108;
    if (frames < W) return notes;
    const int pitchCount = midiHi - midiLo + 1;
    std::vector<int> activeStart(static_cast<size_t>(pitchCount), -1);
    std::vector<double> confidenceSum(static_cast<size_t>(pitchCount), 0.0);
    std::vector<int> confidenceFrames(static_cast<size_t>(pitchCount), 0);
    std::vector<std::complex<float>> spectrum(W);
    std::vector<float> magnitudes(W / 2 + 1);
    const int frameCount = static_cast<int>((frames - W) / hop) + 1;

    auto finish = [&](int p, int endFrame) {
        const int start = activeStart[static_cast<size_t>(p)];
        if (start < 0) return;
        const double duration = static_cast<double>(std::max(1, endFrame - start) * hop) / sampleRate;
        if (duration >= 0.055) {
            DetectedNote note;
            note.startSeconds = static_cast<double>(start * hop) / sampleRate;
            note.durationSeconds = duration;
            note.detectedMidi = midiLo + p;
            note.medianFrequencyHz = midiToHz(note.detectedMidi);
            note.confidence = confidenceFrames[static_cast<size_t>(p)] > 0
                ? confidenceSum[static_cast<size_t>(p)] / confidenceFrames[static_cast<size_t>(p)]
                : 0.5;
            notes.push_back(note);
        }
        activeStart[static_cast<size_t>(p)] = -1;
        confidenceSum[static_cast<size_t>(p)] = 0.0;
        confidenceFrames[static_cast<size_t>(p)] = 0;
    };

    for (int frame = 0; frame < frameCount; ++frame) {
        const int64_t base = static_cast<int64_t>(frame) * hop;
        double energy = 0.0;
        for (int i = 0; i < W; ++i) {
            const float window = 0.5f * (1.0f - std::cos(2.0 * kPi * i / (W - 1)));
            const float sample = mono[static_cast<size_t>(base + i)] * window;
            spectrum[static_cast<size_t>(i)] = {sample, 0.0f};
            energy += sample * sample;
        }
        fftRadix2(spectrum, -1);
        for (size_t b = 0; b < magnitudes.size(); ++b) magnitudes[b] = std::abs(spectrum[b]);

        std::vector<double> score(static_cast<size_t>(pitchCount), 0.0);
        std::vector<double> fundamentalMagnitude(static_cast<size_t>(pitchCount), 0.0);
        double maxScore = 0.0;
        double maxFundamental = 0.0;
        for (int p = 0; p < pitchCount; ++p) {
            const double fundamental = midiToHz(midiLo + p);
            double sum = 0.0;
            for (int harmonic = 1; harmonic <= 6; ++harmonic) {
                const double hz = fundamental * harmonic;
                if (hz >= sampleRate * 0.48) break;
                const int bin = static_cast<int>(std::llround(hz * W / sampleRate));
                if (bin > 1 && bin + 1 < static_cast<int>(magnitudes.size())) {
                    const float local = std::max({magnitudes[static_cast<size_t>(bin - 1)],
                                                  magnitudes[static_cast<size_t>(bin)],
                                                  magnitudes[static_cast<size_t>(bin + 1)]});
                    if (harmonic == 1) {
                        fundamentalMagnitude[static_cast<size_t>(p)] = local;
                        maxFundamental = std::max(maxFundamental, static_cast<double>(local));
                    }
                    sum += local / std::sqrt(static_cast<double>(harmonic));
                }
            }
            score[static_cast<size_t>(p)] = sum;
            maxScore = std::max(maxScore, sum);
        }

        std::vector<bool> active(static_cast<size_t>(pitchCount), false);
        if (energy / W > 1.0e-8 && maxScore > 1.0e-6) {
            for (int p = 1; p + 1 < pitchCount; ++p) {
                const double s = score[static_cast<size_t>(p)];
                // Local pitch-salience peak. Suppress octave duplicates when the lower pitch
                // explains the same harmonic stack almost as strongly.
                const bool localPeak = s >= score[static_cast<size_t>(p - 1)] &&
                                       s > score[static_cast<size_t>(p + 1)];
                const bool strong = s >= maxScore * 0.20;
                const bool hasFundamental = fundamentalMagnitude[static_cast<size_t>(p)] >= maxFundamental * 0.06;
                active[static_cast<size_t>(p)] = localPeak && strong && hasFundamental;
            }
        }
        for (int p = 0; p < pitchCount; ++p) {
            if (active[static_cast<size_t>(p)]) {
                if (activeStart[static_cast<size_t>(p)] < 0) activeStart[static_cast<size_t>(p)] = frame;
                confidenceSum[static_cast<size_t>(p)] +=
                    std::min(1.0, score[static_cast<size_t>(p)] / std::max(1.0e-9, maxScore));
                confidenceFrames[static_cast<size_t>(p)] += 1;
            } else {
                finish(p, frame);
            }
        }
    }
    for (int p = 0; p < pitchCount; ++p) finish(p, frameCount);
    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
        return a.startSeconds == b.startSeconds ? a.detectedMidi < b.detectedMidi
                                                : a.startSeconds < b.startSeconds;
    });
    return notes;
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
    if (mode == DetectionMode::Polyphonic) {
        return detectPolyphonicNotes(interleaved, channels, sampleRate);
    }
    const auto track = detectPitchTrack(interleaved, channels, sampleRate);
    return segmentNotes(track);
}

} // namespace neuracoust::daw
