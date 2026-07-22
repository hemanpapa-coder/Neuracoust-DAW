#include "audio/TimePitchProcessor.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace neuracoust::daw {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// In-place iterative radix-2 Cooley-Tukey FFT. size is a power of two. sign = -1 forward, +1 inverse
// (unscaled). Same shape as MonitorFirEq's fftRadix2, kept local so this file stands alone.
void fftRadix2(std::vector<std::complex<float>>& a, int sign) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = sign * kTwoPi / static_cast<double>(len);
        const std::complex<float> wlen(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// Phase-vocoder stretch of one channel by `stretch` (synthesis/analysis hop ratio), then the caller
// resamples for pitch. Mirrors stretchAndPitch's inner loop exactly.
std::vector<float> stretchChannel(const std::vector<float>& in, double stretch) {
    constexpr int windowSize = 2048;                 // 1 << 11
    constexpr int analysisHop = 512;
    constexpr int bins = windowSize / 2 + 1;
    const int inputSamples = static_cast<int>(in.size());
    const int synthesisHop = std::max(32, static_cast<int>(std::lround(analysisHop * stretch)));
    const int frames = std::max(1, 1 + (inputSamples + analysisHop - 1) / analysisHop);
    const int stretchedSamples = synthesisHop * (frames - 1) + windowSize;

    std::vector<float> out(static_cast<size_t>(stretchedSamples), 0.0f);
    std::vector<float> norm(static_cast<size_t>(stretchedSamples), 0.0f);

    std::vector<float> window(windowSize);
    for (int i = 0; i < windowSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos(kTwoPi * i / (windowSize - 1)));   // periodic-ish Hann

    std::vector<float> previousPhase(bins, 0.0f), synthesisPhase(bins, 0.0f);
    std::vector<std::complex<float>> spec(windowSize);

    for (int frame = 0; frame < frames; ++frame) {
        const int inputPosition = frame * analysisHop;
        const int outputPosition = frame * synthesisHop;
        for (int i = 0; i < windowSize; ++i) {
            const int s = inputPosition + i;
            const float x = (s < inputSamples) ? in[static_cast<size_t>(s)] : 0.0f;
            spec[static_cast<size_t>(i)] = std::complex<float>(x * window[i], 0.0f);
        }
        fftRadix2(spec, -1);
        for (int bin = 0; bin < bins; ++bin) {
            const float mag = std::abs(spec[static_cast<size_t>(bin)]);
            const float phase = std::arg(spec[static_cast<size_t>(bin)]);
            const float expected = static_cast<float>(kTwoPi) * bin * analysisHop / windowSize;
            float delta = phase - previousPhase[static_cast<size_t>(bin)] - expected;
            delta -= static_cast<float>(kTwoPi) * std::round(delta / static_cast<float>(kTwoPi));
            const float trueAdvance = (expected + delta) * synthesisHop / static_cast<float>(analysisHop);
            if (frame == 0) synthesisPhase[static_cast<size_t>(bin)] = phase;
            else synthesisPhase[static_cast<size_t>(bin)] += trueAdvance;
            previousPhase[static_cast<size_t>(bin)] = phase;
            const float ph = synthesisPhase[static_cast<size_t>(bin)];
            spec[static_cast<size_t>(bin)] = std::complex<float>(mag * std::cos(ph), mag * std::sin(ph));
            if (bin > 0 && bin < bins - 1) {   // Hermitian mirror for the inverse real transform
                spec[static_cast<size_t>(windowSize - bin)] = std::conj(spec[static_cast<size_t>(bin)]);
            }
        }
        fftRadix2(spec, +1);
        const float invN = 1.0f / windowSize;   // unscaled inverse → divide by N
        for (int i = 0; i < windowSize; ++i) {
            const int o = outputPosition + i;
            if (o < 0 || o >= stretchedSamples) continue;
            const float weight = window[i];
            out[static_cast<size_t>(o)] += spec[static_cast<size_t>(i)].real() * invN * weight;
            norm[static_cast<size_t>(o)] += weight * weight;
        }
    }
    for (int i = 0; i < stretchedSamples; ++i)
        out[static_cast<size_t>(i)] /= std::max(0.0001f, norm[static_cast<size_t>(i)]);
    return out;
}
}  // namespace

std::vector<float> processTimePitchInterleaved(const std::vector<float>& interleaved,
                                               int channels, const TimePitchParams& params) {
    if (channels < 1 || interleaved.empty()) return {};
    const int inFrames = static_cast<int>(interleaved.size()) / channels;
    if (inFrames < 1) return {};

    const double timeRatio = std::clamp(params.timeRatio, 0.125, 8.0);
    const double pitchFactor = std::pow(2.0, params.semitones / 12.0);
    if (std::abs(timeRatio - 1.0) < 1e-9 && std::abs(params.semitones) < 1e-9) {
        return interleaved;   // no-op
    }
    // The vocoder stretches by timeRatio*pitchFactor; the pitch resample below divides length back by
    // pitchFactor, netting a timeRatio length change with a pitchFactor pitch change.
    const double stretch = std::clamp(timeRatio * pitchFactor, 0.125, 8.0);
    const int outFrames = std::max(1, static_cast<int>(std::lround(inFrames * timeRatio)));

    std::vector<float> out(static_cast<size_t>(outFrames) * channels, 0.0f);
    std::vector<float> chan(static_cast<size_t>(inFrames));
    for (int ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < inFrames; ++i)
            chan[static_cast<size_t>(i)] = interleaved[static_cast<size_t>(i) * channels + ch];
        const std::vector<float> stretched = stretchChannel(chan, stretch);
        const int stretchedSamples = static_cast<int>(stretched.size());
        for (int i = 0; i < outFrames; ++i) {
            const double position = i * pitchFactor;
            const int a = std::clamp(static_cast<int>(position), 0, stretchedSamples - 1);
            const int b = std::min(stretchedSamples - 1, a + 1);
            const float frac = static_cast<float>(position - a);
            out[static_cast<size_t>(i) * channels + ch] =
                stretched[static_cast<size_t>(a)] + (stretched[static_cast<size_t>(b)] - stretched[static_cast<size_t>(a)]) * frac;
        }
    }
    return out;
}

namespace {
// Smooth spectral envelope of one STFT frame via cepstral liftering: keep only the low-quefrency
// part of the log-magnitude spectrum, which is the formant structure (not the fine harmonic comb).
std::vector<float> cepstralEnvelope(const std::vector<std::complex<float>>& spectrum, int lifterQ) {
    const int N = static_cast<int>(spectrum.size());
    std::vector<std::complex<float>> c(N);
    for (int b = 0; b < N; ++b) c[b] = std::complex<float>(std::log(std::abs(spectrum[b]) + 1e-6f), 0.0f);
    fftRadix2(c, +1);                                   // → cepstrum (inverse, unscaled)
    for (auto& v : c) v /= static_cast<float>(N);
    for (int q = lifterQ + 1; q < N - lifterQ; ++q) c[static_cast<size_t>(q)] = std::complex<float>(0.0f, 0.0f);
    fftRadix2(c, -1);                                   // → smoothed log-envelope
    std::vector<float> env(N);
    for (int b = 0; b < N; ++b) env[b] = std::exp(c[static_cast<size_t>(b)].real());
    return env;
}
}  // namespace

std::vector<float> formantCorrect(const std::vector<float>& shifted, const std::vector<float>& original,
                                  int channels, double sampleRate) {
    if (channels < 1 || shifted.empty() || original.empty()) return shifted;
    const int64_t frames = std::min<int64_t>(shifted.size(), original.size()) / channels;
    if (frames < 4) return shifted;
    const double sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
    constexpr int N = 2048, hop = 512;
    if (frames < N) return shifted;
    // Lifter cutoff ≈ formant spacing (~600 Hz): keeps formants, drops the harmonic fine structure.
    const int lifterQ = std::max(16, static_cast<int>(N * 600.0 / sr));

    std::vector<float> window(N);
    for (int i = 0; i < N; ++i) window[i] = 0.5f * (1.0f - std::cos(kTwoPi * i / (N - 1)));

    std::vector<float> out(static_cast<size_t>(frames) * channels, 0.0f);
    std::vector<float> norm(static_cast<size_t>(frames), 0.0f);
    std::vector<std::complex<float>> so(N), sh(N);

    for (int ch = 0; ch < channels; ++ch) {
        std::fill(norm.begin(), norm.end(), 0.0f);
        for (int64_t base = 0; base + N <= frames; base += hop) {
            for (int i = 0; i < N; ++i) {
                const int64_t idx = (base + i) * channels + ch;
                so[static_cast<size_t>(i)] = std::complex<float>(original[static_cast<size_t>(idx)] * window[i], 0.0f);
                sh[static_cast<size_t>(i)] = std::complex<float>(shifted[static_cast<size_t>(idx)] * window[i], 0.0f);
            }
            fftRadix2(so, -1);
            fftRadix2(sh, -1);
            const std::vector<float> envOrig = cepstralEnvelope(so, lifterQ);
            const std::vector<float> envShift = cepstralEnvelope(sh, lifterQ);
            for (int b = 0; b < N; ++b) {
                float g = envOrig[static_cast<size_t>(b)] / (envShift[static_cast<size_t>(b)] + 1e-6f);
                g = std::clamp(g, 0.0625f, 16.0f);   // ±24 dB — enough to relocate a strong formant
                sh[static_cast<size_t>(b)] *= g;
            }
            fftRadix2(sh, +1);   // inverse, unscaled → divide by N
            const float invN = 1.0f / N;
            for (int i = 0; i < N; ++i) {
                const int64_t o = base + i;
                out[static_cast<size_t>(o) * channels + ch] += sh[static_cast<size_t>(i)].real() * invN * window[i];
                norm[static_cast<size_t>(o)] += window[i] * window[i];   // per-channel (reset each ch)
            }
        }
        for (int64_t i = 0; i < frames; ++i) {
            const float w = norm[static_cast<size_t>(i)];
            if (w > 1e-6f) out[static_cast<size_t>(i) * channels + ch] /= w;
            else out[static_cast<size_t>(i) * channels + ch] = shifted[static_cast<size_t>(i) * channels + ch];
        }
    }
    return out;
}

std::vector<float> processTimeMapInterleaved(const std::vector<float>& interleaved, int channels,
                                             const TimePitchParams& params,
                                             const std::vector<double>& sourceAnchors,
                                             const std::vector<double>& destAnchors) {
    if (channels < 1 || interleaved.empty()) return {};
    if (sourceAnchors.empty() || sourceAnchors.size() != destAnchors.size())
        return processTimePitchInterleaved(interleaved, channels, params);

    const int inFrames = static_cast<int>(interleaved.size()) / channels;
    if (inFrames < 1) return {};
    const double timeRatio = std::clamp(params.timeRatio, 0.125, 8.0);
    const int outFrames = std::max(1, static_cast<int>(std::lround(inFrames * timeRatio)));
    std::vector<float> out(static_cast<size_t>(outFrames) * channels, 0.0f);

    const int nSeg = static_cast<int>(sourceAnchors.size()) + 1;
    for (int seg = 0; seg < nSeg; ++seg) {
        const double sStartN = seg == 0 ? 0.0 : sourceAnchors[static_cast<size_t>(seg - 1)];
        const double sEndN = seg == nSeg - 1 ? 1.0 : sourceAnchors[static_cast<size_t>(seg)];
        const double tStartN = seg == 0 ? 0.0 : destAnchors[static_cast<size_t>(seg - 1)];
        const double tEndN = seg == nSeg - 1 ? 1.0 : destAnchors[static_cast<size_t>(seg)];

        const int sStart = std::clamp(static_cast<int>(std::lround(sStartN * inFrames)), 0, inFrames);
        const int sEnd = std::clamp(static_cast<int>(std::lround(sEndN * inFrames)), 0, inFrames);
        const int tStart = std::clamp(static_cast<int>(std::lround(tStartN * outFrames)), 0, outFrames);
        const int tEnd = std::clamp(static_cast<int>(std::lround(tEndN * outFrames)), 0, outFrames);
        const int sCount = std::max(1, sEnd - sStart);
        const int tCount = std::max(1, tEnd - tStart);

        // Slice the source segment (contiguous interleaved frames) and stretch it to the dest span.
        std::vector<float> segIn(static_cast<size_t>(sCount) * channels, 0.0f);
        for (int i = 0; i < sCount && (sStart + i) < inFrames; ++i)
            for (int c = 0; c < channels; ++c)
                segIn[static_cast<size_t>(i) * channels + c] =
                    interleaved[static_cast<size_t>(sStart + i) * channels + c];

        TimePitchParams segParams;
        segParams.timeRatio = static_cast<double>(tCount) / static_cast<double>(sCount);
        segParams.semitones = params.semitones;
        const std::vector<float> processed = processTimePitchInterleaved(segIn, channels, segParams);
        const int processedFrames = static_cast<int>(processed.size()) / channels;
        const int copyCount = std::min({tCount, processedFrames, outFrames - tStart});
        for (int i = 0; i < copyCount; ++i)
            for (int c = 0; c < channels; ++c)
                out[static_cast<size_t>(tStart + i) * channels + c] =
                    processed[static_cast<size_t>(i) * channels + c];
    }
    return out;
}

std::vector<float> processTimeMapFormantPreserving(const std::vector<float>& interleaved, int channels,
                                                   const TimePitchParams& params, double sampleRate,
                                                   const std::vector<double>& sourceAnchors,
                                                   const std::vector<double>& destAnchors) {
    std::vector<float> shifted = processTimeMapInterleaved(interleaved, channels, params, sourceAnchors, destAnchors);
    if (std::abs(params.semitones) < 1e-6 || shifted.empty() || sampleRate <= 0.0) return shifted;

    // A pitch-NEUTRAL stretch of the same content, at the same output timing: it carries the ORIGINAL
    // spectral envelope (time-stretch preserves formants) with the shifted signal's length. Restoring
    // that envelope onto `shifted` keeps formants put through BOTH the stretch and the pitch shift.
    TimePitchParams neutral = params;
    neutral.semitones = 0.0;
    std::vector<float> stretched = processTimeMapInterleaved(interleaved, channels, neutral, sourceAnchors, destAnchors);
    if (stretched.empty()) return shifted;
    return formantCorrect(shifted, stretched, channels, sampleRate);
}

} // namespace neuracoust::daw
