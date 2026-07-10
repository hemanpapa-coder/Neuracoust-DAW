#include "audio/WavFile.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3RealtimeBridge.h"
#include "plugins/Vst3SdkAdapter.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string argValue(int argc, char** argv, const std::string& key) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] != nullptr && key == argv[index]) {
            return argv[index + 1] != nullptr ? argv[index + 1] : "";
        }
    }
    return {};
}

bool hasArg(int argc, char** argv, const std::string& key) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && key == argv[index]) {
            return true;
        }
    }
    return false;
}

std::vector<neuracoust::daw::Vst3ParameterValueState> parameterArgs(int argc, char** argv) {
    std::vector<neuracoust::daw::Vst3ParameterValueState> parameters;
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == nullptr || std::string(argv[index]) != "--param" || argv[index + 1] == nullptr) {
            continue;
        }
        std::string text = argv[index + 1];
        const auto colon = text.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        char* end = nullptr;
        const auto idValue = std::strtoul(text.substr(0, colon).c_str(), &end, 10);
        if (end == text.substr(0, colon).c_str()) {
            continue;
        }
        end = nullptr;
        const auto normalized = std::strtod(text.substr(colon + 1).c_str(), &end);
        if (end == text.substr(colon + 1).c_str()) {
            continue;
        }
        parameters.push_back({static_cast<uint32_t>(idValue),
                              "CLI Param " + std::to_string(idValue),
                              static_cast<float>(std::clamp(normalized, 0.0, 1.0))});
    }
    return parameters;
}

int intArg(int argc, char** argv, const std::string& key, int fallback) {
    const std::string text = argValue(argc, argv, key);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value <= 0 || value > 65536) {
        return fallback;
    }
    return static_cast<int>(value);
}

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

int runSelfTest(int argc, char** argv) {
    const std::string pluginPath = argValue(argc, argv, "--plugin").empty()
        ? "/Library/Audio/Plug-Ins/VST3/Newacoust4001E.vst3"
        : argValue(argc, argv, "--plugin");
    const std::string pluginName = argValue(argc, argv, "--name").empty()
        ? "Newacoust4001E"
        : argValue(argc, argv, "--name");
    if (!std::filesystem::exists(pluginPath)) {
        std::cout << "VST3 process worker self-test skipped: plugin not installed at " << pluginPath << "\n";
        return 0;
    }
    auto audio = makeProbeAudio();
    auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(pluginName, pluginPath);
    auto result = neuracoust::daw::processStereoBufferWithVst3(descriptor, audio, 256);
    if (!result.processed) {
        std::cerr << "VST3 process worker self-test failed: " << result.message << "\n";
        return 2;
    }
    std::cout << "VST3 process worker self-test processed " << result.framesProcessed << " frames\n";
    return 0;
}

void setProcessEnvironmentValue(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main(int argc, char** argv) {
    setProcessEnvironmentValue("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3", "1");
    if (hasArg(argc, argv, "--realtime-bridge")) {
        return neuracoust::daw::runVst3RealtimeBridgeWorker(argc, argv);
    }
    if (hasArg(argc, argv, "--self-test")) {
        return runSelfTest(argc, argv);
    }

    const std::string pluginPath = argValue(argc, argv, "--plugin");
    const std::string pluginName = argValue(argc, argv, "--name");
    const std::string pluginClassId = argValue(argc, argv, "--class-id");
    const std::string pluginClassName = argValue(argc, argv, "--class-name");
    const std::string inputPath = argValue(argc, argv, "--input");
    const std::string outputPath = argValue(argc, argv, "--output");
    const int maxBlock = intArg(argc, argv, "--max-block", 256);
    if (pluginPath.empty() || inputPath.empty() || outputPath.empty()) {
        std::cerr << "Usage: " << (argc > 0 ? argv[0] : "neuracoust_vst3_process_worker")
                  << " --plugin /path/plugin.vst3 --name Plugin --input in.wav --output out.wav"
                  << " [--class-id CID] [--class-name NAME] [--max-block N] [--param id:normalized]\n";
        return 64;
    }

    std::string error;
    neuracoust::daw::WavAudioData audio;
    if (!neuracoust::daw::readPcmWavFile(inputPath, audio, error)) {
        std::cerr << "Could not read input WAV: " << error << "\n";
        return 65;
    }
    if (audio.channels != 2) {
        std::cerr << "VST3 process worker requires stereo input WAV.\n";
        return 66;
    }

    auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(pluginName,
                                                                            pluginPath,
                                                                            pluginClassId,
                                                                            pluginClassName);
    const auto parameters = parameterArgs(argc, argv);
    auto result = neuracoust::daw::processStereoBufferWithVst3(descriptor, audio, maxBlock, parameters);
    if (!result.processed) {
        std::cerr << "VST3 process worker failed: " << result.message << "\n";
        return 67;
    }
    if (!neuracoust::daw::writeFloat32WavFile(outputPath, audio, error)) {
        std::cerr << "Could not write output WAV: " << error << "\n";
        return 68;
    }

    std::cout << "PROCESSED\t" << result.framesProcessed
              << "\tlatency=" << result.latencySamples
              << "\ttail=" << result.tailSamples
              << "\tclass=" << result.className << "\n";
    return 0;
}
