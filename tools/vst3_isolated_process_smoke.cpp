#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3ModuleRuntime.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

neuracoust::daw::WavAudioData makeProbeAudio() {
    neuracoust::daw::WavAudioData audio;
    audio.channels = 2;
    audio.sampleRate = 48000;
    audio.interleavedSamples.resize(512 * 2, 0.0f);
    for (int frame = 0; frame < 512; ++frame) {
        const float value = (frame % 64) < 32 ? 0.05f : -0.05f;
        audio.interleavedSamples[static_cast<size_t>(frame) * 2] = value;
        audio.interleavedSamples[static_cast<size_t>(frame) * 2 + 1] = value;
    }
    return audio;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path self = argc > 0 && argv[0] != nullptr
        ? std::filesystem::absolute(argv[0])
        : std::filesystem::path();
    const auto worker = self.parent_path() / "neuracoust_vst3_process_worker";
    const std::filesystem::path pluginPath("/Library/Audio/Plug-Ins/VST3/Newacoust4001E.vst3");
    if (!std::filesystem::exists(pluginPath)) {
        std::cout << "Isolated VST3 process smoke skipped: plugin not installed at " << pluginPath.string() << "\n";
        return 0;
    }
    if (!std::filesystem::exists(worker)) {
        std::cerr << "Isolated VST3 process smoke failed: worker missing at " << worker.string() << "\n";
        return 2;
    }

    auto audio = makeProbeAudio();
    const auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert("Newacoust4001E", pluginPath.string());
    const auto result = neuracoust::daw::processStereoBufferWithIsolatedVst3(worker.string(), descriptor, audio, 256, 12);
    if (!result.processed) {
        std::cerr << "Isolated VST3 process smoke failed: " << result.message << "\n";
        return 3;
    }
    if (audio.channels != 2 || audio.sampleRate != 48000 || audio.interleavedSamples.size() < 512 * 2) {
        std::cerr << "Isolated VST3 process smoke failed: output shape changed unexpectedly.\n";
        return 4;
    }
    std::cout << result.message << "\n";
    return 0;
}
