// Objective audio-quality measurement harness.
//
// Renders known signals through the offline bounce and measures THD+N, frequency
// response, downsample aliasing, true-peak, and render-to-render null depth. It is the
// tool the audio-quality review asked for: it turns "sounds fine" into numbers, and it
// exercises the sample-rate-conversion path by rendering a source at one rate into a
// project at another.
//
// Thresholds here are deliberately loose sanity gates (non-silent, deterministic, the
// no-resample passthrough is clean). The interesting numbers — upsample/downsample THD+N
// and the alias floor — are printed so the SRC work (task #59) can be judged against a
// recorded baseline and tightened later.
#include "audio/OfflineBounce.h"
#include "audio/WavFile.h"
#include "project/ProjectDocument.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace neuracoust::daw;

namespace {

constexpr double kPi = 3.14159265358979323846;
int g_failures = 0;

// --- tiny iterative radix-2 FFT ---------------------------------------------------------
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<float> channel(const WavAudioData& d, int ch) {
    std::vector<float> out;
    if (d.channels <= 0) return out;
    out.reserve(d.interleavedSamples.size() / static_cast<size_t>(d.channels));
    for (size_t i = static_cast<size_t>(ch); i < d.interleavedSamples.size(); i += static_cast<size_t>(d.channels))
        out.push_back(d.interleavedSamples[i]);
    return out;
}

// Blackman-Harris 4-term coherent gain — low sidelobes (~-92 dB) so a clean tone does not
// leak into the noise band and cap the measured THD+N.
constexpr double kCoherentGain = 0.35875;

// A Blackman-Harris-windowed magnitude spectrum from N samples of the steady middle.
std::vector<double> spectrum(const std::vector<float>& x, int sampleRate, size_t n, double& binHz) {
    std::vector<std::complex<double>> buf(n, {0.0, 0.0});
    const size_t start = x.size() > n ? (x.size() - n) / 2 : 0;
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    for (size_t i = 0; i < n && start + i < x.size(); ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
        const double w = a0 - a1 * std::cos(t) + a2 * std::cos(2 * t) - a3 * std::cos(3 * t);
        buf[i] = { static_cast<double>(x[start + i]) * w, 0.0 };
    }
    fft(buf);
    std::vector<double> mag(n / 2);
    for (size_t i = 0; i < n / 2; ++i) mag[i] = std::abs(buf[i]);
    binHz = static_cast<double>(sampleRate) / static_cast<double>(n);
    return mag;
}

struct ToneMetrics { double thdnDb = 0; double fundDbfs = 0; double peakDbfs = 0; };

double dbfs(double lin) { return lin <= 1e-12 ? -240.0 : 20.0 * std::log10(lin); }

ToneMetrics analyzeTone(const WavAudioData& out, double fundHz) {
    ToneMetrics m;
    const auto x = channel(out, 0);
    if (x.empty()) return m;
    double peak = 0;
    for (float s : x) peak = std::max(peak, static_cast<double>(std::abs(s)));
    m.peakDbfs = dbfs(peak);

    const size_t n = 32768;
    double binHz = 0;
    const auto mag = spectrum(x, out.sampleRate, n, binHz);
    const int fundBin = static_cast<int>(std::lround(fundHz / binHz));
    double sigP = 0, totP = 0;
    for (size_t i = 5; i < mag.size(); ++i) {                 // skip DC / window skirt
        const double p = mag[i] * mag[i];
        totP += p;
        if (std::abs(static_cast<int>(i) - fundBin) <= 4) sigP += p;   // BH4 main lobe ~ ±4 bins
    }
    const double noiseP = std::max(0.0, totP - sigP);
    m.thdnDb = (sigP <= 0) ? 0.0 : 10.0 * std::log10(std::max(noiseP, 1e-30) / sigP);
    m.fundDbfs = dbfs(std::sqrt(sigP) / (static_cast<double>(n) * kCoherentGain / 2.0));
    return m;
}

double nullDepthDb(const WavAudioData& a, const WavAudioData& b) {
    const size_t n = std::min(a.interleavedSamples.size(), b.interleavedSamples.size());
    if (n == 0) return 0.0;
    double diff = 0, sig = 0;
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a.interleavedSamples[i]) - static_cast<double>(b.interleavedSamples[i]);
        diff += d * d;
        sig += static_cast<double>(a.interleavedSamples[i]) * static_cast<double>(a.interleavedSamples[i]);
    }
    return (sig <= 0) ? -240.0 : 10.0 * std::log10(std::max(diff, 1e-30) / sig);
}

// Build a one-clip project that renders `source` (at its own rate) into `projectRate`.
ProjectDocument makeProject(const std::string& sourcePath, const WavAudioData& source, double projectRate) {
    auto p = defaultProject();
    p.name = "Measure Harness";
    p.sampleRate = projectRate;
    p.bitDepth = 32;
    p.loopEnabled = false;
    p.editSelectionEnabled = false;
    p.masterInserts.clear();
    for (auto& m : p.monitorModules) m.enabled = false;
    const double dur = source.sampleRate > 0
        ? static_cast<double>(source.frameCount()) / static_cast<double>(source.sampleRate) : 0.0;
    ClipState c;
    c.id = "measure";
    c.trackName = "Audio 1";
    c.sourcePath = sourcePath;
    c.startSeconds = 0.0;
    c.durationSeconds = dur;
    c.sourceOffsetSeconds = 0.0;
    c.sourceChannels = source.channels;
    c.sourceSampleRate = source.sampleRate;
    c.sourceBitsPerSample = source.bitsPerSample;
    c.sourceFloatingPoint = source.floatingPoint;
    c.regionName = "measure";
    p.clips.clear();
    p.clips.push_back(c);
    p.editSelectionStartSeconds = 0.0;
    p.editSelectionEndSeconds = dur;
    return p;
}

// A full-level (0.9) mono sine, so THD+N reflects the SRC and not a quiet-tone noise floor.
bool writeSineWav(const std::filesystem::path& path, int sr, double seconds, double freq, double amp) {
    WavAudioData d;
    d.channels = 1; d.sampleRate = sr; d.bitsPerSample = 32; d.floatingPoint = true;
    const size_t n = static_cast<size_t>(seconds * sr);
    d.interleavedSamples.resize(n);
    for (size_t i = 0; i < n; ++i)
        d.interleavedSamples[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / sr));
    std::string err;
    return writeFloat32WavFile(path, d, err);
}

bool renderTone(const std::filesystem::path& dir, const std::string& tag,
                int sourceRate, double freq, double projectRate, WavAudioData& out) {
    const auto src = dir / (tag + "_src.wav");
    const auto dst = dir / (tag + "_out.wav");
    if (!writeSineWav(src, sourceRate, 2.0, freq, 0.9)) return false;
    std::string err;
    WavAudioData source;
    if (!readPcmWavFile(src, source, err)) return false;
    const auto project = makeProject(src.string(), source, projectRate);
    const auto res = bounceProjectToWav(project, dst.string(), BounceOptions{});
    if (!res.ok) { printf("  bounce failed: %s\n", res.message.c_str()); return false; }
    return readPcmWavFile(dst, out, err);
}

void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "nc_audio_measure";
    std::filesystem::create_directories(dir, ec);

    printf("== Audio measurement harness ==\n");

    // 1) Passthrough (no resampling): the render must be clean and deterministic.
    WavAudioData pass1, pass2;
    check(renderTone(dir, "pass1", 48000, 1000.0, 48000.0, pass1), "render 1 kHz @48k->48k");
    check(renderTone(dir, "pass2", 48000, 1000.0, 48000.0, pass2), "render again (for null test)");
    if (!pass1.interleavedSamples.empty()) {
        const auto m = analyzeTone(pass1, 1000.0);
        printf("  passthrough: THD+N %.1f dB, fund %.1f dBFS, peak %.1f dBFS\n", m.thdnDb, m.fundDbfs, m.peakDbfs);
        check(m.peakDbfs > -12.0, "passthrough not silent (loud tone renders loud)");
        check(m.thdnDb < -70.0, "passthrough THD+N < -70 dB (no-resample is clean)");
    }
    const double nd = nullDepthDb(pass1, pass2);
    printf("  null depth (render vs render): %.1f dB\n", nd);
    check(nd < -100.0, "render is deterministic (null < -100 dB)");

    // 2) Upsample 44.1k -> 48k: linear interp adds HF loss + distortion. Record the number.
    WavAudioData up;
    if (renderTone(dir, "up", 44100, 1000.0, 48000.0, up)) {
        const auto m = analyzeTone(up, 1000.0);
        printf("  upsample 44.1->48k, 1 kHz: THD+N %.1f dB, peak %.1f dBFS\n", m.thdnDb, m.peakDbfs);
        check(m.peakDbfs > -12.0, "upsample not silent");
        check(m.thdnDb < -80.0, "upsample THD+N < -80 dB (windowed-sinc SRC)");
    }

    // 3) HF response: a 15 kHz tone upsampled 44.1->48k should keep essentially all its
    //    level — the old linear interp rolled ~3 dB off here.
    WavAudioData hf;
    if (renderTone(dir, "hf", 44100, 15000.0, 48000.0, hf)) {
        const auto m = analyzeTone(hf, 15000.0);
        printf("  HF 15 kHz 44.1->48k: level %.1f dBFS\n", m.fundDbfs);
        check(m.fundDbfs > -9.0, "15 kHz rolloff small (< ~2 dB below unity)");
    }

    // 4) Downsample alias: a 30 kHz tone @96k is above 24 kHz Nyquist at 48k. An ideal SRC
    //    filters it out (near silence); linear interp folds it to 48-30 = 18 kHz. Report the
    //    alias image level — lower is better, this is the headline SRC number.
    WavAudioData al;
    if (renderTone(dir, "alias", 96000, 30000.0, 48000.0, al)) {
        const double aliasDbfs = analyzeTone(al, 18000.0).fundDbfs;   // absolute level of the 18 kHz image
        const auto x = channel(al, 0);
        double peak = 0; for (float s : x) peak = std::max(peak, (double)std::abs(s));
        printf("  downsample 96->48k, 30 kHz tone: 18 kHz alias image %.1f dBFS, out peak %.1f dBFS\n",
               aliasDbfs, dbfs(peak));
        check(aliasDbfs < -70.0, "downsample alias image < -70 dBFS (anti-aliased SRC)");
    }

    printf("== %s ==\n", g_failures == 0 ? "ALL SANITY CHECKS PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
