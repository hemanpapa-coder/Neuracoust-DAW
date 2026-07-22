// Bundled helper that runs the CREPE neural pitch detector on an audio file and prints a pitch track.
// Spawned as a subprocess by the DAW (like the stem separator) so LibTorch stays out of the audio
// process. CREPE is far more accurate than the built-in YIN on complex/noisy timbres.
//
// Usage: neuracoust_pitch_detector <input.wav> [--model <path>] [--hop-ms 10] [--confidence 0.5]
// Output (stdout), one line per analysis frame plus a terminator:
//   PITCH <timeSeconds> <hz> <confidence>     (hz 0 = unvoiced / below the confidence floor)
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
constexpr double kCrepeRate = 16000.0;   // CREPE runs at 16 kHz.
constexpr int kFrame = 1024;
constexpr int kBins = 360;

void emitError(const std::string& m) { std::printf("ERROR %s\n", m.c_str()); std::fflush(stdout); }

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

// CREPE bin → frequency: cents = 20*bin + 1997.3794…, freq = 10 * 2^(cents/1200).
double binToHz(double bin) { return 10.0 * std::pow(2.0, (20.0 * bin + 1997.3794084376191) / 1200.0); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { emitError("usage: neuracoust_pitch_detector <input.wav> [--model p] [--hop-ms n] [--confidence c]"); return 2; }
    const std::string inputPath = argv[1];
    std::string modelPath;
    double hopMs = 10.0, confFloor = 0.5;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string f = argv[i];
        if (f == "--model") modelPath = argv[i + 1];
        else if (f == "--hop-ms") hopMs = std::atof(argv[i + 1]);
        else if (f == "--confidence") confFloor = std::atof(argv[i + 1]);
    }
    if (modelPath.empty()) {
        const std::filesystem::path exe = std::filesystem::path(argv[0]);
        modelPath = (exe.parent_path().parent_path() / "Resources" / "crepe_full.pt").string();
    }

    neuracoust::daw::WavAudioData src;
    std::string err;
    if (!neuracoust::daw::readPcmWavFile(inputPath, src, err)) { emitError("read input: " + err); return 1; }
    if (src.channels < 1 || src.sampleRate <= 0.0) { emitError("unsupported input"); return 1; }

    // Mono mix → resample to 16 kHz.
    const int64_t frames = static_cast<int64_t>(src.interleavedSamples.size()) / src.channels;
    std::vector<float> mono(static_cast<size_t>(frames));
    for (int64_t i = 0; i < frames; ++i) {
        double s = 0.0;
        for (int c = 0; c < src.channels; ++c) s += src.interleavedSamples[static_cast<size_t>(i) * src.channels + c];
        mono[static_cast<size_t>(i)] = static_cast<float>(s / src.channels);
    }
    mono = resampleLinear(mono, src.sampleRate, kCrepeRate);
    const int64_t n16 = static_cast<int64_t>(mono.size());
    const int hop = std::max(1, static_cast<int>(hopMs * 0.001 * kCrepeRate));

    torch::jit::script::Module model;
    try { model = torch::jit::load(modelPath, torch::kCPU); model.eval(); }
    catch (const std::exception& e) { emitError(std::string("load model: ") + e.what()); return 1; }
    torch::NoGradGuard noGrad;

    // Frame (1024, centred) and per-frame normalize (zero-mean/unit-std), as CREPE expects.
    std::vector<int64_t> frameStarts;
    for (int64_t center = 0; center < n16; center += hop) frameStarts.push_back(center - kFrame / 2);
    const int64_t nf = static_cast<int64_t>(frameStarts.size());
    if (nf == 0) { std::printf("DONE\n"); return 0; }

    torch::Tensor input = torch::zeros({nf, kFrame}, torch::kFloat32);
    auto acc = input.accessor<float, 2>();
    for (int64_t f = 0; f < nf; ++f) {
        double sum = 0.0, sq = 0.0; int cnt = 0;
        for (int i = 0; i < kFrame; ++i) {
            const int64_t s = frameStarts[static_cast<size_t>(f)] + i;
            const float v = (s >= 0 && s < n16) ? mono[static_cast<size_t>(s)] : 0.0f;
            acc[f][i] = v; sum += v; sq += v * v; ++cnt;
        }
        const double mean = sum / cnt;
        const double var = sq / cnt - mean * mean;
        const double std = std::sqrt(std::max(1e-10, var));
        for (int i = 0; i < kFrame; ++i) acc[f][i] = static_cast<float>((acc[f][i] - mean) / std);
    }

    torch::Tensor act;
    try { act = model.forward({input}).toTensor().to(torch::kCPU).contiguous(); }   // [nf, 360]
    catch (const std::exception& e) { emitError(std::string("inference: ") + e.what()); return 1; }
    if (act.dim() != 2 || act.size(1) != kBins) { emitError("unexpected model output shape"); return 1; }
    auto a = act.accessor<float, 2>();

    for (int64_t f = 0; f < nf; ++f) {
        int best = 0; float peak = a[f][0];
        for (int b = 1; b < kBins; ++b) if (a[f][b] > peak) { peak = a[f][b]; best = b; }
        // Weighted local average of cents around the peak (CREPE's sub-bin decoding).
        double num = 0.0, den = 0.0;
        for (int b = std::max(0, best - 4); b <= std::min(kBins - 1, best + 4); ++b) { num += a[f][b] * b; den += a[f][b]; }
        const double bin = den > 1e-9 ? num / den : best;
        const double hz = binToHz(bin);
        const double time = static_cast<double>(f) * hop / kCrepeRate;
        const double conf = std::clamp(static_cast<double>(peak), 0.0, 1.0);
        std::printf("PITCH %.4f %.2f %.3f\n", time, conf >= confFloor ? hz : 0.0, conf);
    }
    std::printf("DONE\n");
    std::fflush(stdout);
    return 0;
}
