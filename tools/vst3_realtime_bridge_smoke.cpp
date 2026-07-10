// Smoke test for out-of-process (sandboxed) realtime VST3 hosting.
//
// Verifies that a plugin which is blocked from in-process hosting
// (isKnownUnsafeForInProcessVst3Host) is transparently hosted through the
// realtime bridge worker: prepare() succeeds and process() returns processed
// audio instead of failing. If no blocked plugin is installed, the test is
// skipped (exit 0) so it stays green on machines without such plugins.

#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3RealtimeBridge.h"
#include "plugins/Vst3SdkAdapter.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Candidate plugins that the in-process host blocks. First one installed wins.
const std::vector<std::pair<std::string, std::string>> kBlockedCandidates = {
    {"FabFilter Pro-Q 4", "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-Q 4.vst3"},
    {"FabFilter Pro-C 3", "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-C 3.vst3"},
    {"FabFilter Pro-G", "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-G.vst3"},
    {"FabFilter Pro-DS", "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-DS.vst3"},
};

} // namespace

int main() {
    if (!neuracoust::daw::vst3RealtimeBridgeSupported()) {
        std::cout << "Realtime VST3 bridge is not supported on this platform; skipping.\n";
        return 0;
    }

    std::string pluginName;
    std::string pluginPath;
    for (const auto& [name, path] : kBlockedCandidates) {
        if (std::filesystem::exists(path)) {
            pluginName = name;
            pluginPath = path;
            break;
        }
    }
    if (pluginPath.empty()) {
        std::cout << "No in-process-blocked VST3 plugin installed; skipping bridge smoke test.\n";
        return 0;
    }

    const int maxBlock = 128;
    const double sampleRate = 48000.0;
    auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(pluginName, pluginPath);

    neuracoust::daw::Vst3RealtimeProcessor processor;
    std::string message;
    if (!processor.prepare(descriptor, sampleRate, maxBlock, message)) {
        std::cerr << "Bridge prepare failed for " << pluginName << ": " << message << "\n";
        return 2;
    }
    if (!processor.isPrepared()) {
        std::cerr << "Processor reports not prepared after bridge prepare for " << pluginName << "\n";
        return 3;
    }
    // These candidates are all blocked in-process, so preparing successfully
    // must have gone through the sandboxed out-of-process bridge.
    if (message.find("out-of-process") == std::string::npos) {
        std::cerr << "Expected " << pluginName << " to prepare out-of-process, got: " << message << "\n";
        return 7;
    }
    std::cout << "Prepared out-of-process: " << message << "\n";

    // Feed a few blocks of a sine tone; confirm audio flows through and returns
    // finite, non-silent samples.
    double phase = 0.0;
    const double increment = 2.0 * 3.14159265358979323846 * 220.0 / sampleRate;
    double outputEnergy = 0.0;
    int processedBlocks = 0;
    for (int block = 0; block < 8; ++block) {
        std::vector<float> buffer(static_cast<size_t>(maxBlock) * 2u, 0.0f);
        for (int frame = 0; frame < maxBlock; ++frame) {
            const float sample = static_cast<float>(std::sin(phase) * 0.25);
            buffer[static_cast<size_t>(frame) * 2u] = sample;
            buffer[static_cast<size_t>(frame) * 2u + 1u] = sample;
            phase += increment;
        }
        std::string processMessage;
        const auto result = processor.processInterleavedStereo(buffer.data(), maxBlock, processMessage);
        if (!result.processed) {
            // The first couple of warmup blocks may drop while the plugin lazily
            // initializes; only fail if nothing ever processes.
            std::cout << "Block " << block << " not processed: " << processMessage << "\n";
            continue;
        }
        ++processedBlocks;
        for (const float value : buffer) {
            if (!std::isfinite(value)) {
                std::cerr << "Bridge returned non-finite audio for " << pluginName << "\n";
                return 4;
            }
            outputEnergy += std::abs(static_cast<double>(value));
        }
    }

    if (processedBlocks == 0) {
        std::cerr << "Bridge never processed a block for " << pluginName << "\n";
        return 5;
    }
    if (outputEnergy <= 0.0) {
        std::cerr << "Bridge processed audio but returned silence for " << pluginName << "\n";
        return 6;
    }

    processor.reset();
    std::cout << "Realtime VST3 bridge smoke test passed for " << pluginName
              << " (" << processedBlocks << "/8 blocks processed, energy " << outputEnergy << ").\n";
    return 0;
}
