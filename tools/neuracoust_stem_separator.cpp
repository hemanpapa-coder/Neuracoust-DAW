// Bundled helper executable that separates an audio file into 4 stems (Drums/Bass/Other/Vocals)
// with a TorchScript htdemucs model. Ported from the Neuracoust Stem Magic app's LibTorchStemMerger +
// StemAudioProcessor, with JUCE replaced by DW's WavFile and a plain resampler. Run as a SUBPROCESS by
// the DAW so LibTorch (~300 MB, and any OOM/crash) stays out of the main audio process.
//
// Usage: neuracoust_stem_separator <input.wav> <outputDir> [--model <path>] [--stem-prefix <name>]
// Progress + results are printed to stdout as line records the DAW parses:
//   PROGRESS <0..1>
//   STEM <Name> <absolute wav path>
//   DONE   |   ERROR <message>
#include "audio/WavFile.h"

#include <torch/script.h>
#include <torch/torch.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr double kModelRate = 44100.0;     // htdemucs is trained at 44.1 kHz.
constexpr int kChunkSize = 343980;         // htdemucs training_segment (7.8 s @ 44.1 kHz).

void emitError(const std::string& msg) { std::printf("ERROR %s\n", msg.c_str()); std::fflush(stdout); }

// Source names by (model kind, output count). The output count alone is ambiguous — htdemucs and a
// drumsep model both emit 4 sources but mean entirely different things — so the DAW tells the helper
// which family of model it bundled via --kind, and the names follow from that:
//   music     4 = htdemucs (Drums/Bass/Other/Vocals), 6 = htdemucs_6s (+Guitar/Piano)
//   drum      4 = kick/snare/toms/cymbals, 5 = kick/snare/toms/hihat/cymbals (Demucs-arch drumsep)
//   orchestra experimental family split — names by count, best-effort (Strings/Brass/Woodwinds/Other)
// An explicit --stem-names a,b,c wins over all of this (authoritative, trimmed/padded to nSources).
std::vector<std::string> stemNamesFor(const std::string& kind, int n) {
    if (kind == "drum") {
        if (n == 4) return {"Kick", "Snare", "Toms", "Cymbals"};
        if (n == 5) return {"Kick", "Snare", "Toms", "HiHat", "Cymbals"};
    } else if (kind == "orchestra") {
        if (n == 4) return {"Strings", "Brass", "Woodwinds", "Other"};
        if (n == 5) return {"Strings", "Brass", "Woodwinds", "Percussion", "Other"};
    } else {  // "music" (default)
        if (n == 4) return {"Drums", "Bass", "Other", "Vocals"};
        if (n == 6) return {"Drums", "Bass", "Other", "Vocals", "Guitar", "Piano"};
    }
    std::vector<std::string> names;
    for (int i = 0; i < n; ++i) names.push_back("Stem" + std::to_string(i + 1));
    return names;
}

std::string toLower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Deinterleave into per-channel float vectors (clamped to 2 channels for the stereo model).
std::vector<std::vector<float>> deinterleave(const std::vector<float>& in, int channels, int64_t frames) {
    const int ch = std::min(2, std::max(1, channels));
    std::vector<std::vector<float>> out(ch, std::vector<float>(static_cast<size_t>(frames), 0.0f));
    for (int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            out[c][static_cast<size_t>(i)] = in[static_cast<size_t>(i) * channels + c];
    return out;
}

// Linear-resample one channel from srcRate to dstRate. Good enough for a v1; a windowed-sinc is a
// future quality upgrade. Returns the resampled channel.
std::vector<float> resampleLinear(const std::vector<float>& in, double srcRate, double dstRate) {
    if (std::abs(srcRate - dstRate) < 1e-6 || in.empty()) return in;
    const double ratio = dstRate / srcRate;
    const int64_t outLen = std::max<int64_t>(1, static_cast<int64_t>(std::llround(in.size() * ratio)));
    std::vector<float> out(static_cast<size_t>(outLen));
    const int64_t inLen = static_cast<int64_t>(in.size());
    for (int64_t i = 0; i < outLen; ++i) {
        const double pos = i / ratio;
        const int64_t a = std::min(inLen - 1, static_cast<int64_t>(pos));
        const int64_t b = std::min(inLen - 1, a + 1);
        const float f = static_cast<float>(pos - a);
        out[static_cast<size_t>(i)] = in[static_cast<size_t>(a)] + (in[static_cast<size_t>(b)] - in[static_cast<size_t>(a)]) * f;
    }
    return out;
}

// --- Toms-by-pitch splitter --------------------------------------------------------------------
// The user asked that toms of different pitch land on different stems. A drum-separation model hands
// back ONE "Toms" stem with every tom hit in it; this splits that stem into `bands` pitch groups by:
//   1. onset detection on the mono energy envelope,
//   2. a fundamental (f0) estimate per hit via autocorrelation in the tom range (60–300 Hz),
//   3. 1-D k-means over log-f0 to cluster hits into `bands` groups,
//   4. routing each hit's audio (with short raised-cosine fades so edges don't click) into its band.
// Bands are returned low→high pitch. Quality is inherently source-dependent (bleed, overlapping hits);
// with too few distinct hits it degrades gracefully to fewer bands, and to the original stem at 1.
struct TomHit { int64_t start; int64_t end; double f0; };

// One tom hit's fundamental by autocorrelation over its attack+sustain window. 0 if none is confident.
double estimateHitF0(const std::vector<float>& mono, int64_t start, int64_t end, double rate) {
    const int64_t minLag = static_cast<int64_t>(rate / 300.0);   // 300 Hz ceiling
    const int64_t maxLag = static_cast<int64_t>(rate / 60.0);    // 60 Hz floor
    const int64_t len = end - start;
    if (len <= maxLag + 4) return 0.0;
    double best = 0.0; int64_t bestLag = 0; double zeroLagE = 0.0;
    for (int64_t i = start; i < end; ++i) zeroLagE += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i)];
    if (zeroLagE < 1e-9) return 0.0;
    for (int64_t lag = minLag; lag <= maxLag; ++lag) {
        double acc = 0.0;
        for (int64_t i = start; i + lag < end; ++i)
            acc += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i + lag)];
        if (acc > best) { best = acc; bestLag = lag; }
    }
    if (bestLag == 0 || best < 0.30 * zeroLagE) return 0.0;   // weak/aperiodic → not a pitched tom hit
    return rate / static_cast<double>(bestLag);
}

std::vector<std::vector<std::vector<float>>> splitTomsByPitch(const std::vector<std::vector<float>>& toms,
                                                              int bands, double rate,
                                                              std::vector<double>& outCenters) {
    outCenters.clear();
    const int64_t frames = static_cast<int64_t>(toms[0].size());
    // Mono analysis signal.
    std::vector<float> mono(static_cast<size_t>(frames), 0.0f);
    for (int c = 0; c < static_cast<int>(toms.size()); ++c)
        for (int64_t i = 0; i < frames; ++i) mono[static_cast<size_t>(i)] += toms[static_cast<size_t>(c)][static_cast<size_t>(i)];
    const float chScale = toms.empty() ? 1.0f : 1.0f / static_cast<float>(toms.size());
    for (float& v : mono) v *= chScale;

    // Energy envelope (10 ms window, 5 ms hop) → onset picking.
    const int64_t win = std::max<int64_t>(1, static_cast<int64_t>(0.010 * rate));
    const int64_t hop = std::max<int64_t>(1, static_cast<int64_t>(0.005 * rate));
    std::vector<double> env; env.reserve(static_cast<size_t>(frames / hop + 1));
    double peakEnv = 0.0;
    for (int64_t s = 0; s < frames; s += hop) {
        double e = 0.0; const int64_t e0 = std::min(frames, s + win);
        for (int64_t i = s; i < e0; ++i) e += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i)];
        e = std::sqrt(e / static_cast<double>(std::max<int64_t>(1, e0 - s)));
        env.push_back(e); peakEnv = std::max(peakEnv, e);
    }
    if (peakEnv < 1e-6) return {toms};   // silent — nothing to split

    // Onsets: a rising frame that clears a floor and sits a good margin above the previous frame,
    // with a ~50 ms refractory gap so one hit is not counted twice.
    const double floorE = 0.06 * peakEnv;
    const int64_t refractory = std::max<int64_t>(1, static_cast<int64_t>(0.050 * rate / hop));
    std::vector<int64_t> onsetFrames; int64_t lastOnset = -refractory * 2;
    for (size_t k = 1; k < env.size(); ++k) {
        if (env[k] > floorE && env[k] > 1.6 * env[k - 1] &&
            static_cast<int64_t>(k) - lastOnset >= refractory) {
            onsetFrames.push_back(static_cast<int64_t>(k)); lastOnset = static_cast<int64_t>(k);
        }
    }
    if (onsetFrames.size() < 2) return {toms};   // not enough hits to split meaningfully

    // Per-hit f0.
    std::vector<TomHit> hits;
    for (size_t o = 0; o < onsetFrames.size(); ++o) {
        const int64_t start = onsetFrames[o] * hop;
        const int64_t nextOnset = (o + 1 < onsetFrames.size()) ? onsetFrames[o + 1] * hop : frames;
        const int64_t end = std::min(frames, std::min(nextOnset, start + static_cast<int64_t>(0.30 * rate)));
        const double f0 = estimateHitF0(mono, start, end, rate);
        // A hit's audio spans onset → next onset (so the whole decaying tail travels with it).
        hits.push_back({start, nextOnset, f0});
    }
    // Drop unpitched hits from clustering but still route them (to the nearest band later).
    std::vector<double> logf;
    for (const auto& h : hits) if (h.f0 > 0.0) logf.push_back(std::log(h.f0));
    if (logf.size() < 2) return {toms};

    int k = std::min<int>(bands, static_cast<int>(logf.size()));
    if (k < 2) return {toms};

    // 1-D k-means, initialised by evenly splitting the sorted log-f0 values.
    std::vector<double> sorted = logf; std::sort(sorted.begin(), sorted.end());
    std::vector<double> centers(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c)
        centers[static_cast<size_t>(c)] = sorted[std::min(sorted.size() - 1, static_cast<size_t>((c * 2 + 1) * sorted.size() / (2 * k)))];
    for (int iter = 0; iter < 25; ++iter) {
        std::vector<double> sum(static_cast<size_t>(k), 0.0); std::vector<int> cnt(static_cast<size_t>(k), 0);
        for (double lf : logf) {
            int best = 0; double bd = 1e18;
            for (int c = 0; c < k; ++c) { double d = std::abs(lf - centers[static_cast<size_t>(c)]); if (d < bd) { bd = d; best = c; } }
            sum[static_cast<size_t>(best)] += lf; ++cnt[static_cast<size_t>(best)];
        }
        for (int c = 0; c < k; ++c) if (cnt[static_cast<size_t>(c)]) centers[static_cast<size_t>(c)] = sum[static_cast<size_t>(c)] / cnt[static_cast<size_t>(c)];
    }
    // Order bands low→high pitch.
    std::sort(centers.begin(), centers.end());
    for (double lf : centers) outCenters.push_back(std::exp(lf));

    // Route each hit into its nearest band (unpitched hits use the median pitch so they land somewhere).
    const double medianLog = sorted[sorted.size() / 2];
    std::vector<std::vector<std::vector<float>>> out(
        static_cast<size_t>(k), std::vector<std::vector<float>>(toms.size(), std::vector<float>(static_cast<size_t>(frames), 0.0f)));
    const int64_t fade = std::max<int64_t>(1, static_cast<int64_t>(0.003 * rate));   // 3 ms raised-cosine edges
    for (const auto& h : hits) {
        const double lf = (h.f0 > 0.0) ? std::log(h.f0) : medianLog;
        int band = 0; double bd = 1e18;
        for (int c = 0; c < k; ++c) { double d = std::abs(lf - centers[static_cast<size_t>(c)]); if (d < bd) { bd = d; band = c; } }
        for (int c = 0; c < static_cast<int>(toms.size()); ++c) {
            for (int64_t i = h.start; i < h.end && i < frames; ++i) {
                float g = 1.0f;
                const int64_t rel = i - h.start, tail = h.end - 1 - i;
                if (rel < fade) g = 0.5f * (1.0f - std::cos(static_cast<float>(M_PI) * rel / fade));
                if (tail < fade) g = std::min(g, 0.5f * (1.0f - std::cos(static_cast<float>(M_PI) * tail / fade)));
                out[static_cast<size_t>(band)][static_cast<size_t>(c)][static_cast<size_t>(i)] =
                    toms[static_cast<size_t>(c)][static_cast<size_t>(i)] * g;
            }
        }
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        emitError("usage: neuracoust_stem_separator <input.wav> <outputDir> [--model <path>] [--stem-prefix <name>] "
                  "[--kind music|drum|orchestra] [--stem-names <a,b,c>] [--split-toms <N>] "
                  "[--stems <a,b,accompaniment>] [--skip-silent]");
        return 2;
    }
    const std::string inputPath = argv[1];
    const std::filesystem::path outputDir = argv[2];
    std::string modelPath;
    std::string stemPrefix;
    std::string kind = "music";            // music | drum | orchestra — picks the naming scheme
    std::vector<std::string> stemNameOverride;   // authoritative names, if given (--stem-names)
    std::vector<std::string> wantStems;   // native names + optional "accompaniment"; empty = all
    bool skipSilent = false;
    int splitToms = 0;                     // >1 = split a "Toms" stem into that many pitch bands
    for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--skip-silent") skipSilent = true;
        else if (i + 1 < argc && flag == "--model") modelPath = argv[++i];
        else if (i + 1 < argc && flag == "--stem-prefix") stemPrefix = argv[++i];
        else if (i + 1 < argc && flag == "--kind") kind = toLower(argv[++i]);
        else if (i + 1 < argc && flag == "--stem-names") for (auto& s : splitCsv(argv[++i])) stemNameOverride.push_back(s);
        else if (i + 1 < argc && flag == "--split-toms") splitToms = std::atoi(argv[++i]);
        else if (i + 1 < argc && flag == "--stems") for (auto& s : splitCsv(toLower(argv[++i]))) wantStems.push_back(s);
    }
    // Default: a demucs model sitting next to this executable's Resources (…/Contents/Resources).
    if (modelPath.empty()) {
        const std::filesystem::path exe = std::filesystem::path(argv[0]);
        const std::filesystem::path resources = exe.parent_path().parent_path() / "Resources" / "demucs.pt";
        modelPath = resources.string();
    }
    if (stemPrefix.empty()) {
        stemPrefix = std::filesystem::path(inputPath).stem().string();
    }

    // --- Load the audio ---
    neuracoust::daw::WavAudioData src;
    std::string err;
    if (!neuracoust::daw::readPcmWavFile(inputPath, src, err)) { emitError("read input: " + err); return 1; }
    if (src.channels < 1 || src.sampleRate <= 0.0) { emitError("unsupported input audio"); return 1; }
    const double inRate = src.sampleRate;
    const int outChannels = std::min(2, src.channels);
    const int64_t inFrames = static_cast<int64_t>(src.interleavedSamples.size()) / src.channels;

    // Deinterleave → resample each channel to the model rate.
    auto channels = deinterleave(src.interleavedSamples, src.channels, inFrames);
    for (auto& c : channels) c = resampleLinear(c, inRate, kModelRate);
    const int64_t frames = static_cast<int64_t>(channels[0].size());
    const int modelCh = static_cast<int>(channels.size());   // 1 or 2

    // --- Load the model ---
    torch::jit::script::Module module;
    try {
        module = torch::jit::load(modelPath, torch::kCPU);
        module.eval();
    } catch (const std::exception& e) {
        emitError(std::string("load model: ") + e.what());
        return 1;
    }
    torch::NoGradGuard noGrad;

    // --- Weighted overlap-add across 343980-sample chunks (50 % hop, Hann window) ---
    // Source count comes from the model's output (4 = htdemucs, 6 = htdemucs_6s), so a bigger model
    // just works. Stems are allocated after the first chunk once we know that count.
    std::vector<std::vector<std::vector<float>>> stems;   // [source][channel][frame]
    int nSources = 0;
    std::vector<float> weight(static_cast<size_t>(frames), 0.0f);

    std::vector<float> window(kChunkSize);
    for (int i = 0; i < kChunkSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (kChunkSize - 1)));

    const int jump = kChunkSize / 2;
    for (int64_t start = 0; start < frames; start += jump) {
        const int n = static_cast<int>(std::min<int64_t>(kChunkSize, frames - start));

        // Build the [1, 2, chunk] input tensor (zero-padded; mono duplicated to both channels).
        torch::Tensor input = torch::zeros({1, 2, kChunkSize}, torch::kFloat32);
        auto acc = input.accessor<float, 3>();
        for (int c = 0; c < 2; ++c) {
            const auto& srcCh = channels[std::min(c, modelCh - 1)];
            for (int i = 0; i < n; ++i) acc[0][c][i] = srcCh[static_cast<size_t>(start + i)];
        }

        torch::Tensor output;
        try {
            std::vector<torch::jit::IValue> inputs;
            inputs.push_back(input);
            output = module.forward(inputs).toTensor().to(torch::kCPU).contiguous();   // [1, S, 2, chunk]
        } catch (const std::exception& e) {
            emitError(std::string("inference: ") + e.what());
            return 1;
        }
        if (output.dim() != 4) { emitError("unexpected model output rank"); return 1; }
        if (nSources == 0) {
            nSources = static_cast<int>(output.size(1));
            stems.assign(static_cast<size_t>(nSources), std::vector<std::vector<float>>(2, std::vector<float>(static_cast<size_t>(frames), 0.0f)));
        }
        auto out = output.accessor<float, 4>();

        for (int i = 0; i < n; ++i) weight[static_cast<size_t>(start + i)] += window[i];
        for (int s = 0; s < nSources; ++s)
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) {
                    float v = out[0][s][c][i] * window[i];
                    if (!std::isfinite(v)) v = 0.0f;
                    stems[static_cast<size_t>(s)][c][static_cast<size_t>(start + i)] += v;
                }

        std::printf("PROGRESS %.4f\n", std::min(1.0, static_cast<double>(start + jump) / static_cast<double>(frames)));
        std::fflush(stdout);
    }
    if (nSources == 0) { emitError("model produced no sources"); return 1; }

    // Normalize by accumulated window weight, clamp.
    for (auto& stem : stems)
        for (int c = 0; c < 2; ++c)
            for (int64_t i = 0; i < frames; ++i) {
                const float w = weight[static_cast<size_t>(i)];
                stem[c][static_cast<size_t>(i)] = w > 1e-8f
                    ? std::clamp(stem[c][static_cast<size_t>(i)] / w, -1.0f, 1.0f) : 0.0f;
            }

    // Names: an explicit --stem-names list wins (trimmed/padded to the model's actual source count);
    // otherwise derive from (kind, count). This is what lets one helper serve htdemucs, drumsep and an
    // orchestral model without guessing from the count alone.
    std::vector<std::string> names = stemNamesFor(kind, nSources);
    if (!stemNameOverride.empty()) {
        names = stemNameOverride;
        names.resize(static_cast<size_t>(nSources));
        for (int s = 0; s < nSources; ++s)
            if (names[static_cast<size_t>(s)].empty()) names[static_cast<size_t>(s)] = "Stem" + std::to_string(s + 1);
    }
    auto isVocal = [&](int s) { return toLower(names[static_cast<size_t>(s)]) == "vocals"; };

    // Build the list of stems to write: native sources selected by --stems (or all), plus a synthesized
    // "Accompaniment" (everything but vocals, summed) when requested — the karaoke split.
    struct OutStem { std::string name; std::vector<std::vector<float>> ch; };
    std::vector<OutStem> outputs;
    auto wants = [&](const std::string& lname) {
        return wantStems.empty() || std::find(wantStems.begin(), wantStems.end(), lname) != wantStems.end();
    };
    for (int s = 0; s < nSources; ++s)
        if (wants(toLower(names[static_cast<size_t>(s)]))) outputs.push_back({ names[static_cast<size_t>(s)], stems[static_cast<size_t>(s)] });
    if (std::find(wantStems.begin(), wantStems.end(), std::string("accompaniment")) != wantStems.end()) {
        std::vector<std::vector<float>> acc(2, std::vector<float>(static_cast<size_t>(frames), 0.0f));
        for (int s = 0; s < nSources; ++s) if (!isVocal(s))
            for (int c = 0; c < 2; ++c) for (int64_t i = 0; i < frames; ++i)
                acc[c][static_cast<size_t>(i)] = std::clamp(acc[c][static_cast<size_t>(i)] + stems[static_cast<size_t>(s)][c][static_cast<size_t>(i)], -1.0f, 1.0f);
        outputs.push_back({ "Accompaniment", std::move(acc) });
    }

    // Split a "Toms" stem into pitch bands (--split-toms N): different-pitch toms onto different stems.
    // Runs at the model rate, before the resample-back below. Falls back to the single stem if the DSP
    // can't find enough distinct pitched hits.
    if (splitToms > 1) {
        std::vector<OutStem> expanded;
        for (auto& os : outputs) {
            if (toLower(os.name).find("tom") != std::string::npos) {
                std::vector<double> centers;
                auto bandsCh = splitTomsByPitch(os.ch, splitToms, kModelRate, centers);
                if (bandsCh.size() <= 1) { expanded.push_back(std::move(os)); continue; }
                static const char* kLabels3[3] = {"Low", "Mid", "High"};
                for (size_t b = 0; b < bandsCh.size(); ++b) {
                    std::string suffix = (bandsCh.size() == 3) ? kLabels3[b]
                        : (bandsCh.size() == 2 ? (b == 0 ? "Low" : "High") : ("Band" + std::to_string(b + 1)));
                    const int hz = centers.size() > b ? static_cast<int>(std::lround(centers[b])) : 0;
                    std::printf("INFO toms band %s ~%d Hz\n", suffix.c_str(), hz); std::fflush(stdout);
                    expanded.push_back({ os.name + " " + suffix, std::move(bandsCh[b]) });
                }
            } else {
                expanded.push_back(std::move(os));
            }
        }
        outputs = std::move(expanded);
    }

    // --- Resample each selected stem back to the input rate, interleave, write WAV ---
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    for (const auto& os : outputs) {
        // --skip-silent: don't emit a stem that is essentially empty (the part isn't in this clip).
        if (skipSilent) {
            double sum = 0.0; int64_t cnt = 0;
            for (int c = 0; c < 2; ++c) for (float v : os.ch[static_cast<size_t>(c)]) { sum += static_cast<double>(v) * v; ++cnt; }
            const double rms = cnt ? std::sqrt(sum / cnt) : 0.0;
            if (rms < 0.0015) { std::printf("SKIP %s silent\n", os.name.c_str()); std::fflush(stdout); continue; }   // ~ -56 dBFS
        }
        std::vector<std::vector<float>> back(2);
        for (int c = 0; c < 2; ++c) back[c] = resampleLinear(os.ch[static_cast<size_t>(c)], kModelRate, inRate);
        const int64_t outFrames = static_cast<int64_t>(back[0].size());

        neuracoust::daw::WavAudioData outData;
        outData.channels = outChannels;
        outData.sampleRate = inRate;
        outData.interleavedSamples.assign(static_cast<size_t>(outFrames) * outChannels, 0.0f);
        for (int64_t i = 0; i < outFrames; ++i)
            for (int c = 0; c < outChannels; ++c)
                outData.interleavedSamples[static_cast<size_t>(i) * outChannels + c] = back[c][static_cast<size_t>(i)];

        const std::filesystem::path outPath = outputDir / (stemPrefix + "_" + os.name + ".wav");
        if (!neuracoust::daw::writePcm24WavFile(outPath, outData, err)) {
            emitError("write stem " + os.name + ": " + err);
            return 1;
        }
        std::printf("STEM %s %s\n", os.name.c_str(), outPath.string().c_str());
        std::fflush(stdout);
    }

    std::printf("DONE\n");
    std::fflush(stdout);
    return 0;
}
