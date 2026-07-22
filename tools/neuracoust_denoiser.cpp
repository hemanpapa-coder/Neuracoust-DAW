// Bundled helper executable that removes the broadband noise floor (hiss/hum/room tone) from an audio
// file with a TorchScript neural denoiser (Facebook's DNS "denoiser", MIT). Run as a SUBPROCESS by the
// DAW so LibTorch stays out of the main audio process, like neuracoust_stem_separator.
//
// The model runs at 16 kHz. To keep full bandwidth on music we BAND-SPLIT: the model's *change* is
// computed at 16 kHz (band-limited to 8 kHz) and added back to the original, so content above 8 kHz is
// preserved untouched and mix=0 reproduces the input exactly. See neuracoust_denoiser_dsp.h.
//
// Usage: neuracoust_denoiser <input.wav> <output.wav> [--model <path>] [--mix <0..1>]
// stdout line records the DAW parses:  PROGRESS <0..1>   then   DONE  |  ERROR <message>
#include "audio/WavFile.h"
#include "neuracoust_denoiser_dsp.h"

#include <torch/script.h>
#include <torch/torch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {
constexpr int kModelRate = 16000;   // the DNS denoiser's fixed rate (traced at 16000-sample chunks).
constexpr int kChunk = 16000;       // 1 s per model call (the traced length; longer would mis-shape).
constexpr int kHop = kChunk / 2;    // 50 % overlap; Hann meets COLA here, so interior weight == 1.0.

void emitError(const std::string& msg) { std::printf("ERROR %s\n", msg.c_str()); std::fflush(stdout); }

std::vector<std::vector<float>> deinterleave(const std::vector<float>& in, int channels, int64_t frames) {
    const int ch = std::max(1, channels);
    std::vector<std::vector<float>> out(ch, std::vector<float>(static_cast<size_t>(frames), 0.0f));
    for (int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            out[static_cast<size_t>(c)][static_cast<size_t>(i)] = in[static_cast<size_t>(i * channels + c)];
    return out;
}

// Run the denoiser over one 16 kHz channel and return delta16 = denoised - original (same length).
// Where the OLA weight is ~0 (the leading/trailing half-window) delta is left at 0 = passthrough.
std::vector<float> denoiseDelta16(torch::jit::script::Module& model, const std::vector<float>& low16,
                                  int64_t chunkBase, int64_t chunkTotal, std::function<void(double)> onProgress) {
    const int64_t len = static_cast<int64_t>(low16.size());
    std::vector<float> clean(static_cast<size_t>(len), 0.0f);
    std::vector<float> weight(static_cast<size_t>(len), 0.0f);
    std::vector<float> window(kChunk);
    for (int i = 0; i < kChunk; ++i)
        window[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (kChunk - 1)));

    torch::NoGradGuard noGrad;
    int64_t done = 0;
    for (int64_t start = 0; start < len; start += kHop) {
        const int n = static_cast<int>(std::min<int64_t>(kChunk, len - start));
        torch::Tensor input = torch::zeros({1, 1, kChunk}, torch::kFloat32);
        auto acc = input.accessor<float, 3>();
        for (int i = 0; i < n; ++i) acc[0][0][i] = low16[static_cast<size_t>(start + i)];

        std::vector<torch::jit::IValue> inputs{input};
        torch::Tensor output = model.forward(inputs).toTensor().to(torch::kCPU).contiguous();  // [1,1,16000]
        auto out = output.accessor<float, 3>();
        for (int i = 0; i < n; ++i) {
            float v = out[0][0][i] * window[i];
            if (!std::isfinite(v)) v = 0.0f;
            clean[static_cast<size_t>(start + i)] += v;
            weight[static_cast<size_t>(start + i)] += window[i];
        }
        if (onProgress) onProgress(static_cast<double>(chunkBase + (++done)) / static_cast<double>(chunkTotal));
    }

    std::vector<float> delta(static_cast<size_t>(len), 0.0f);
    for (int64_t i = 0; i < len; ++i) {
        const float w = weight[static_cast<size_t>(i)];
        if (w > 1e-4f) delta[static_cast<size_t>(i)] = clean[static_cast<size_t>(i)] / w - low16[static_cast<size_t>(i)];
    }
    return delta;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { emitError("usage: neuracoust_denoiser <input.wav> <output.wav> [--model p] [--mix m]"); return 2; }
    const std::string inputPath = argv[1];
    const std::filesystem::path outputPath = argv[2];
    std::string modelPath;
    float mix = 1.0f;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string f = argv[i];
        if (f == "--model") modelPath = argv[i + 1];
        else if (f == "--mix") mix = std::clamp(static_cast<float>(std::atof(argv[i + 1])), 0.0f, 1.0f);
    }
    if (modelPath.empty()) {
        const std::filesystem::path exe = std::filesystem::path(argv[0]);
        modelPath = (exe.parent_path().parent_path() / "Resources" / "denoiser.pt").string();
    }

    neuracoust::daw::WavAudioData src;
    std::string err;
    if (!neuracoust::daw::readPcmWavFile(inputPath, src, err)) { emitError("read input: " + err); return 1; }
    if (src.channels < 1 || src.sampleRate <= 0) { emitError("unsupported input audio"); return 1; }
    const double inRate = static_cast<double>(src.sampleRate);
    const int64_t inFrames = static_cast<int64_t>(src.interleavedSamples.size()) / src.channels;

    torch::jit::script::Module model;
    try {
        model = torch::jit::load(modelPath, torch::kCPU);
        model.eval();
    } catch (const std::exception& e) { emitError(std::string("load model: ") + e.what()); return 1; }

    auto channels = deinterleave(src.interleavedSamples, src.channels, inFrames);

    // Total chunk count across all channels, for a single monotone progress bar.
    int64_t chunkTotal = 0;
    std::vector<std::vector<float>> low16(channels.size());
    for (size_t c = 0; c < channels.size(); ++c) {
        low16[c] = neuracoust::denoiser::resampleSinc(channels[c], inRate, kModelRate);
        const int64_t len = static_cast<int64_t>(low16[c].size());
        chunkTotal += (len > 0) ? ((len + kHop - 1) / kHop) : 0;
    }
    chunkTotal = std::max<int64_t>(1, chunkTotal);

    int64_t chunkBase = 0;
    for (size_t c = 0; c < channels.size(); ++c) {
        std::vector<float> delta16 = denoiseDelta16(model, low16[c], chunkBase, chunkTotal,
            [](double p) { std::printf("PROGRESS %.4f\n", std::min(1.0, p)); std::fflush(stdout); });
        const int64_t len = static_cast<int64_t>(low16[c].size());
        chunkBase += (len > 0) ? ((len + kHop - 1) / kHop) : 0;
        channels[c] = neuracoust::denoiser::combineBand(channels[c], delta16, inRate, kModelRate, mix);
    }

    // Re-interleave (lengths are preserved by combineBand) and write.
    neuracoust::daw::WavAudioData outData;
    outData.channels = src.channels;
    outData.sampleRate = src.sampleRate;
    outData.interleavedSamples.assign(static_cast<size_t>(inFrames) * src.channels, 0.0f);
    for (int64_t i = 0; i < inFrames; ++i)
        for (int c = 0; c < src.channels; ++c)
            outData.interleavedSamples[static_cast<size_t>(i * src.channels + c)] =
                (i < static_cast<int64_t>(channels[static_cast<size_t>(c)].size()))
                    ? channels[static_cast<size_t>(c)][static_cast<size_t>(i)] : 0.0f;

    if (!neuracoust::daw::writePcm24WavFileAtomically(outputPath, outData, err)) {
        emitError("write output: " + err); return 1;
    }
    std::printf("DONE\n"); std::fflush(stdout);
    return 0;
}
