#pragma once

#include "audio/WavFile.h"
#include "plugins/Vst3HostFoundation.h"
#include <string>

namespace neuracoust::daw {

struct Vst3ModuleProbeResult {
    bool opened = false;
    bool hasFactory = false;
    std::string factorySymbol = "GetPluginFactory";
    std::string message;
};

struct Vst3IsolatedProcessResult {
    bool processed = false;
    int exitStatus = 0;
    std::string message;
};

Vst3ModuleProbeResult probeVst3ModuleFactory(const Vst3PluginDescriptor& descriptor);
std::string defaultVst3ProcessWorkerPath();
Vst3IsolatedProcessResult processStereoBufferWithIsolatedVst3(const std::string& workerExecutablePath,
                                                              const Vst3PluginDescriptor& descriptor,
                                                              WavAudioData& audio,
                                                              int maxBlockSize = 256,
                                                              int timeoutSeconds = 20);

} // namespace neuracoust::daw
