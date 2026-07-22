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
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr double kModelRate = 44100.0;     // htdemucs is trained at 44.1 kHz.
constexpr int kChunkSize = 343980;         // htdemucs training_segment (7.8 s @ 44.1 kHz).
const char* kStemNames[4] = {"Drums", "Bass", "Other", "Vocals"};

void emitError(const std::string& msg) { std::printf("ERROR %s\n", msg.c_str()); std::fflush(stdout); }

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
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        emitError("usage: neuracoust_stem_separator <input.wav> <outputDir> [--model <path>] [--stem-prefix <name>]");
        return 2;
    }
    const std::string inputPath = argv[1];
    const std::filesystem::path outputDir = argv[2];
    std::string modelPath;
    std::string stemPrefix;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--model") modelPath = argv[i + 1];
        else if (flag == "--stem-prefix") stemPrefix = argv[i + 1];
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
    std::vector<std::vector<float>> stems[4];
    for (auto& s : stems) s.assign(2, std::vector<float>(static_cast<size_t>(frames), 0.0f));
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
            output = module.forward(inputs).toTensor().to(torch::kCPU).contiguous();   // [1, 4, 2, chunk]
        } catch (const std::exception& e) {
            emitError(std::string("inference: ") + e.what());
            return 1;
        }
        auto out = output.accessor<float, 4>();

        for (int i = 0; i < n; ++i) weight[static_cast<size_t>(start + i)] += window[i];
        for (int s = 0; s < 4; ++s)
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) {
                    float v = out[0][s][c][i] * window[i];
                    if (!std::isfinite(v)) v = 0.0f;
                    stems[s][c][static_cast<size_t>(start + i)] += v;
                }

        std::printf("PROGRESS %.4f\n", std::min(1.0, static_cast<double>(start + jump) / static_cast<double>(frames)));
        std::fflush(stdout);
    }

    // Normalize by accumulated window weight, clamp.
    for (auto& stem : stems)
        for (int c = 0; c < 2; ++c)
            for (int64_t i = 0; i < frames; ++i) {
                const float w = weight[static_cast<size_t>(i)];
                stem[c][static_cast<size_t>(i)] = w > 1e-8f
                    ? std::clamp(stem[c][static_cast<size_t>(i)] / w, -1.0f, 1.0f) : 0.0f;
            }

    // --- Resample each stem back to the input rate, interleave, write WAV ---
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    for (int s = 0; s < 4; ++s) {
        std::vector<std::vector<float>> back(2);
        for (int c = 0; c < 2; ++c) back[c] = resampleLinear(stems[s][c], kModelRate, inRate);
        const int64_t outFrames = static_cast<int64_t>(back[0].size());

        neuracoust::daw::WavAudioData outData;
        outData.channels = outChannels;
        outData.sampleRate = inRate;
        outData.interleavedSamples.assign(static_cast<size_t>(outFrames) * outChannels, 0.0f);
        for (int64_t i = 0; i < outFrames; ++i)
            for (int c = 0; c < outChannels; ++c)
                outData.interleavedSamples[static_cast<size_t>(i) * outChannels + c] = back[c][static_cast<size_t>(i)];

        const std::filesystem::path outPath = outputDir / (stemPrefix + "_" + kStemNames[s] + ".wav");
        if (!neuracoust::daw::writePcm24WavFile(outPath, outData, err)) {
            emitError("write stem " + std::string(kStemNames[s]) + ": " + err);
            return 1;
        }
        std::printf("STEM %s %s\n", kStemNames[s], outPath.string().c_str());
        std::fflush(stdout);
    }

    std::printf("DONE\n");
    std::fflush(stdout);
    return 0;
}
