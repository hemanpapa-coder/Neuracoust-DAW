// Probe whether the in-process realtime VST3 host actually changes audio for a
// given plugin. Feeds an impulse + tone through Vst3RealtimeProcessor (the exact
// object the realtime track/master insert chain uses) and reports whether the
// output differs from the dry input.
//
// Usage: vst3_realtime_process_probe --plugin <bundle> --name <n> [--class-id <hex>] [--class-name <n>]

#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string arg(int argc, char** argv, const std::string& key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (key == argv[i]) return argv[i + 1];
    }
    return {};
}
}

int main(int argc, char** argv) {
    const std::string path = arg(argc, argv, "--plugin");
    const std::string name = arg(argc, argv, "--name");
    const std::string classId = arg(argc, argv, "--class-id");
    const std::string className = arg(argc, argv, "--class-name");
    if (path.empty()) {
        std::cerr << "need --plugin\n";
        return 2;
    }

    auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(name, path, classId, className);
    std::cout << "descriptor name=" << descriptor.name
              << " brand=" << descriptor.brand
              << " bundle=" << descriptor.bundlePath << "\n";

    const int maxBlock = 512;
    const double sampleRate = 48000.0;
    bool forceOop = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--force-oop") forceOop = true;
    }
    neuracoust::daw::Vst3RealtimeProcessor processor;
    std::string message;
    if (!processor.prepare(descriptor, sampleRate, maxBlock, message, std::string{}, forceOop)) {
        std::cerr << "PREPARE FAILED: " << message << "\n";
        return 3;
    }
    std::cout << "prepared: " << message << "\n";
    std::cout << "isPrepared=" << (processor.isPrepared() ? "yes" : "no") << "\n";

    double dryEnergy = 0.0;
    double wetEnergy = 0.0;
    double diffEnergy = 0.0;
    int processedBlocks = 0;
    double phase = 0.0;
    const double inc = 2.0 * 3.14159265358979323846 * 440.0 / sampleRate;
    // Steady -18 dBFS tone (0.125 amplitude), identical L/R, no impulse.
    const float kAmp = 0.125f; // -18.06 dBFS
    float inPeak = 0.0f;
    float outPeak = 0.0f;
    for (int block = 0; block < 120; ++block) {
        std::vector<float> buffer(static_cast<size_t>(maxBlock) * 2u, 0.0f);
        for (int f = 0; f < maxBlock; ++f) {
            const float s = static_cast<float>(std::sin(phase)) * kAmp;
            buffer[static_cast<size_t>(f) * 2u] = s;
            buffer[static_cast<size_t>(f) * 2u + 1u] = s;
            phase += inc;
        }
        std::vector<float> dry = buffer;
        std::string pm;
        const auto r = processor.processInterleavedStereo(buffer.data(), maxBlock, pm);
        if (!r.processed) {
            continue;
        }
        ++processedBlocks;
        // Ignore the first 40 blocks (worker warmup / plugin settling); measure steady state.
        if (block >= 40) {
            for (size_t i = 0; i < buffer.size(); ++i) {
                inPeak = std::max(inPeak, std::abs(dry[i]));
                outPeak = std::max(outPeak, std::abs(buffer[i]));
                dryEnergy += std::abs(static_cast<double>(dry[i]));
                wetEnergy += std::abs(static_cast<double>(buffer[i]));
                diffEnergy += std::abs(static_cast<double>(buffer[i] - dry[i]));
            }
        }
    }
    processor.reset();

    auto dbfs = [](float v) { return v > 0.0f ? 20.0 * std::log10(static_cast<double>(v)) : -144.0; };
    std::cout << "processedBlocks=" << processedBlocks << "\n";
    std::cout << "INPUT peak = " << dbfs(inPeak) << " dBFS, OUTPUT peak = " << dbfs(outPeak)
              << " dBFS, delta = " << (dbfs(outPeak) - dbfs(inPeak)) << " dB\n";
    std::cout << "dryEnergy=" << dryEnergy << " wetEnergy=" << wetEnergy << " diffEnergy=" << diffEnergy << "\n";
    if (processedBlocks == 0) {
        std::cout << "RESULT: plugin never processed a block (would be dry passthrough)\n";
        return 4;
    }
    if (diffEnergy <= 1e-4) {
        std::cout << "RESULT: output IDENTICAL to input -> plugin is NOT changing the audio\n";
        return 5;
    }
    std::cout << "RESULT: output DIFFERS from input -> plugin IS processing (diff/dry="
              << (dryEnergy > 0 ? diffEnergy / dryEnergy : 0.0) << ")\n";
    return 0;
}
