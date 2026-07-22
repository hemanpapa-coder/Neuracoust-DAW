// Pins the ported phase-vocoder time/pitch core: a stretch changes length but not pitch, a pitch
// shift changes pitch but not length, and a no-op returns the input.
#include "audio/TimePitchProcessor.h"

#include <cmath>
#include <cstdio>
#include <vector>

using neuracoust::daw::processTimePitchInterleaved;
using neuracoust::daw::processTimeMapInterleaved;
using neuracoust::daw::formantCorrect;
using neuracoust::daw::TimePitchParams;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL: %s\n", what); ++g_failures; }
}

// Dominant frequency of a mono signal by counting positive-going zero crossings.
double estimateHz(const std::vector<float>& mono, double sr) {
    int crossings = 0, first = -1, last = -1;
    for (size_t i = 1; i < mono.size(); ++i) {
        if (mono[i - 1] <= 0.0f && mono[i] > 0.0f) {
            if (first < 0) first = static_cast<int>(i);
            last = static_cast<int>(i);
            ++crossings;
        }
    }
    if (crossings < 2 || last <= first) return 0.0;
    return (crossings - 1) / ((last - first) / sr);
}

std::vector<float> sine(double hz, int frames, double sr) {
    std::vector<float> v(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) v[static_cast<size_t>(i)] = 0.6f * std::sin(2.0 * M_PI * hz * i / sr);
    return v;
}
}  // namespace

int main() {
    const double sr = 48000.0;
    const int frames = 48000;   // 1 s, mono

    // No-op returns the input untouched.
    {
        const auto in = sine(1000.0, 1024, sr);
        const auto out = processTimePitchInterleaved(in, 1, {1.0, 0.0});
        check(out.size() == in.size(), "no-op preserves length");
    }

    // Time stretch 2x: ~twice as long, pitch unchanged (~1 kHz).
    {
        const auto in = sine(1000.0, frames, sr);
        const auto out = processTimePitchInterleaved(in, 1, {2.0, 0.0});
        check(std::abs(static_cast<int>(out.size()) - 2 * frames) < frames / 20, "stretch 2x ~doubles length");
        const double hz = estimateHz(out, sr);
        char m[96]; std::snprintf(m, sizeof m, "stretch keeps pitch ~1kHz (got %.0f)", hz);
        check(std::abs(hz - 1000.0) < 60.0, m);
    }

    // Time compress 0.5x: ~half as long.
    {
        const auto in = sine(1000.0, frames, sr);
        const auto out = processTimePitchInterleaved(in, 1, {0.5, 0.0});
        check(std::abs(static_cast<int>(out.size()) - frames / 2) < frames / 20, "compress 0.5x ~halves length");
        const double hz = estimateHz(out, sr);
        check(std::abs(hz - 1000.0) < 60.0, "compress keeps pitch ~1kHz");
    }

    // Pitch up +12 semitones (octave): length unchanged, frequency ~doubled (~2 kHz).
    {
        const auto in = sine(1000.0, frames, sr);
        const auto out = processTimePitchInterleaved(in, 1, {1.0, 12.0});
        check(std::abs(static_cast<int>(out.size()) - frames) < frames / 20, "pitch-only keeps length");
        const double hz = estimateHz(out, sr);
        char m[96]; std::snprintf(m, sizeof m, "pitch +12 semitones ~doubles freq (got %.0f)", hz);
        check(hz > 1850.0 && hz < 2150.0, m);
    }

    // Pitch down -12 semitones: ~half frequency (~500 Hz).
    {
        const auto in = sine(1000.0, frames, sr);
        const auto out = processTimePitchInterleaved(in, 1, {1.0, -12.0});
        const double hz = estimateHz(out, sr);
        char m[96]; std::snprintf(m, sizeof m, "pitch -12 semitones ~halves freq (got %.0f)", hz);
        check(hz > 460.0 && hz < 540.0, m);
    }

    // Stereo interleaving preserved (2 ch): length = frames per channel.
    {
        std::vector<float> stereo(static_cast<size_t>(frames) * 2);
        for (int i = 0; i < frames; ++i) {
            stereo[static_cast<size_t>(i) * 2] = 0.5f * std::sin(2.0 * M_PI * 440.0 * i / sr);
            stereo[static_cast<size_t>(i) * 2 + 1] = 0.5f * std::sin(2.0 * M_PI * 660.0 * i / sr);
        }
        const auto out = processTimePitchInterleaved(stereo, 2, {1.5, 0.0});
        check(out.size() % 2 == 0 && std::abs(static_cast<int>(out.size() / 2) - 3 * frames / 2) < frames / 20,
              "stereo stretch 1.5x length + interleave intact");
    }

    // Piecewise time remap: total length still follows timeRatio; empty anchors == uniform.
    {
        const auto in = sine(1000.0, frames, sr);
        // One anchor: source midpoint (0.5) maps to dest 0.75 — stretch the first half, compress the
        // second. Overall timeRatio 1.0, so output length ≈ input length.
        const auto mapped = processTimeMapInterleaved(in, 1, {1.0, 0.0}, {0.5}, {0.75});
        check(std::abs(static_cast<int>(mapped.size()) - frames) < frames / 20, "time-map keeps overall length at ratio 1.0");
        // Empty anchors fall back to the uniform path.
        const auto uniform = processTimeMapInterleaved(in, 1, {2.0, 0.0}, {}, {});
        const auto direct = processTimePitchInterleaved(in, 1, {2.0, 0.0});
        check(uniform.size() == direct.size(), "time-map with no anchors == uniform stretch");
        // A remapped clip is still audible (not silent).
        double peak = 0.0; for (float v : mapped) peak = std::max(peak, std::fabs((double)v));
        check(peak > 0.3, "time-map output is audible");
    }

    // Formant preservation: a "vowel" (harmonics of 200 Hz shaped by a formant peak at 1500 Hz),
    // shifted up an octave. Without correction the formant moves to ~3000 Hz; formantCorrect pulls it
    // back to ~1500 Hz. Measure energy in the 1500 Hz band vs the 3000 Hz band.
    {
        const int n = static_cast<int>(0.7 * sr);
        std::vector<float> vowel(n, 0.0f);
        for (int h = 1; h <= 24; ++h) {
            const double fh = 200.0 * h;
            if (fh >= sr / 2) break;
            const double amp = std::exp(-std::pow(fh - 1500.0, 2.0) / (2.0 * 300.0 * 300.0));   // formant @1500
            for (int i = 0; i < n; ++i) vowel[i] += static_cast<float>(0.3 * amp * std::sin(2.0 * M_PI * fh * i / sr));
        }
        auto bandEnergy = [&](const std::vector<float>& x, double flo, double fhi) {
            double e = 0.0;
            for (double f = flo; f <= fhi; f += 50.0) {
                double re = 0, im = 0;
                for (size_t k = 0; k < x.size(); ++k) { const double a = 2.0 * M_PI * f * k / sr; re += x[k] * std::cos(a); im -= x[k] * std::sin(a); }
                e += re * re + im * im;
            }
            return e;
        };
        const double origLo = bandEnergy(vowel, 1200, 1800), origHi = bandEnergy(vowel, 2700, 3300);
        check(origLo > origHi * 2.0, "vowel formant sits in the 1500 Hz band");

        auto shifted = processTimePitchInterleaved(vowel, 1, {1.0, 12.0});   // up an octave
        const double shLo = bandEnergy(shifted, 1200, 1800), shHi = bandEnergy(shifted, 2700, 3300);
        check(shHi > shLo, "without correction the formant moves up to ~3000 Hz");

        auto corrected = formantCorrect(shifted, vowel, 1, sr);
        const double coLo = bandEnergy(corrected, 1200, 1800), coHi = bandEnergy(corrected, 2700, 3300);
        char m[128]; std::snprintf(m, sizeof m, "formantCorrect pulls the formant back to 1500 Hz (lo %.2g > hi %.2g)", coLo, coHi);
        check(coLo > coHi, m);

        // The wrapper preserves the formant through a SIMULTANEOUS stretch (1.5x) AND octave shift:
        // the output is 1.5x long, pitch is up an octave, yet the formant still sits in the 1500 band.
        auto fp = neuracoust::daw::processTimeMapFormantPreserving(vowel, 1, {1.5, 12.0}, sr, {}, {});
        check(std::abs((int)fp.size() - (int)(n * 1.5)) < n / 5, "time+pitch+formant output is ~1.5x length");
        const double fpLo = bandEnergy(fp, 1200, 1800), fpHi = bandEnergy(fp, 2700, 3300);
        char m2[128]; std::snprintf(m2, sizeof m2, "formant preserved through simultaneous stretch+shift (lo %.2g > hi %.2g)", fpLo, fpHi);
        check(fpLo > fpHi, m2);
    }

    if (g_failures == 0) { std::printf("TimePitch smoke: all checks passed\n"); return 0; }
    std::printf("TimePitch smoke: %d failure(s)\n", g_failures);
    return 1;
}
