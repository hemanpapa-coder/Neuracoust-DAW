#include "audio/WavFile.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3RealtimeBridge.h"
#include "plugins/Vst3SdkAdapter.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
std::string value(int argc, char** argv, const char* key) {
    for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == key) return argv[i + 1];
    return {};
}
}

int main(int argc, char** argv) {
    const auto pluginPath = value(argc, argv, "--plugin");
    const auto pluginName = value(argc, argv, "--name");
    const auto classId = value(argc, argv, "--class-id");
    const auto className = value(argc, argv, "--class-name");
    const auto inputPath = value(argc, argv, "--input");
    const auto workerPath = value(argc, argv, "--worker");
    const auto editorPath = value(argc, argv, "--editor");
    const int holdSeconds = std::max(0, std::atoi(value(argc, argv, "--hold-seconds").c_str()));
    if (pluginPath.empty() || inputPath.empty() || workerPath.empty() || editorPath.empty()) {
        std::cerr << "Usage: meter_plugin_fixture_host --plugin P --name N --class-id C --class-name CN"
                     " --input fixture.wav --worker process_worker --editor editor_host [--hold-seconds 5]\n";
        return 64;
    }

    neuracoust::daw::WavAudioData audio;
    std::string message;
    if (!neuracoust::daw::readPcmWavFile(inputPath, audio, message) || audio.channels < 1) {
        std::cerr << "FIXTURE_ERROR " << message << "\n";
        return 65;
    }
    std::vector<float> stereo(static_cast<size_t>(audio.frameCount()) * 2u, 0.0f);
    for (int64_t frame = 0; frame < audio.frameCount(); ++frame) {
        stereo[static_cast<size_t>(frame) * 2u] = audio.interleavedSamples[static_cast<size_t>(frame) * audio.channels];
        stereo[static_cast<size_t>(frame) * 2u + 1u] = audio.interleavedSamples[static_cast<size_t>(frame) * audio.channels + std::min(1, audio.channels - 1)];
    }

    const int blockSize = 256;
    const std::string shmName = "/ncml" + std::to_string(static_cast<int>(getpid()) % 100000);
    auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(pluginName, pluginPath, classId, className);
    neuracoust::daw::Vst3RealtimeBridgeClient bridge;
    setenv("NEURACOUST_VST3_BRIDGE_READY_WAIT_MS", "5000", 1);
    if (!bridge.prepare(descriptor, audio.sampleRate, blockSize, workerPath, message, shmName)) {
        std::cerr << "BRIDGE_ERROR " << message << "\n";
        return 66;
    }

    const pid_t editorPid = fork();
    if (editorPid == 0) {
        const auto block = std::to_string(blockSize);
        const auto rate = std::to_string(audio.sampleRate);
        execl(editorPath.c_str(), editorPath.c_str(), "--plugin", pluginPath.c_str(), "--name", pluginName.c_str(),
              "--title", ("Meter Alignment · " + pluginName).c_str(), "--class-id", classId.c_str(),
              "--class-name", className.c_str(), "--observe-shm", shmName.c_str(),
              "--observe-max-block", block.c_str(), "--observe-sample-rate", rate.c_str(),
              "--pulse-param", "5321", "--meter-overlay", nullptr);
        _exit(127);
    }
    if (editorPid < 0) {
        std::cerr << "EDITOR_ERROR could not fork editor host\n";
        return 67;
    }

    std::cout << "READY plugin=" << pluginName << " frames=" << audio.frameCount()
              << " sample_rate=" << audio.sampleRate << " shm=" << shmName << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::vector<neuracoust::daw::Vst3ParameterValueState> inputParameters, outputParameters;
    const auto start = std::chrono::steady_clock::now();
    for (int64_t position = 0; position < audio.frameCount(); position += blockSize) {
        const int frames = static_cast<int>(std::min<int64_t>(blockSize, audio.frameCount() - position));
        std::vector<float> block(static_cast<size_t>(frames) * 2u);
        std::copy_n(stereo.data() + static_cast<size_t>(position) * 2u, block.size(), block.data());
        std::string processMessage;
        bridge.process(block.data(), frames, inputParameters, outputParameters, processMessage);
        const auto target = start + std::chrono::duration<double>(static_cast<double>(position + frames) / audio.sampleRate);
        std::this_thread::sleep_until(target);
    }
    std::cout << "PLAYBACK_COMPLETE elapsed=" << (static_cast<double>(audio.frameCount()) / audio.sampleRate) << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(holdSeconds));
    kill(editorPid, SIGTERM);
    bridge.reset();
    return 0;
}
