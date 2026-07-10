#pragma once

#include "core/DawState.h"
#include "plugins/Vst3HostFoundation.h"
#include <memory>
#include <string>
#include <vector>

namespace neuracoust::daw {

// Out-of-process realtime VST3 hosting bridge.
//
// Some plugins crash when hosted inside the DAW process (see
// isKnownUnsafeForInProcessVst3Host). Instead of refusing to run them live,
// the bridge spawns a persistent worker process that hosts the plugin and
// exchanges one audio block at a time over shared memory with a process-shared
// pthread mutex/condition-variable handshake. If the worker is slow, hangs, or
// crashes, the client times out and reports failure so the caller can keep the
// dry signal instead of stalling or losing audio.
//
// Only POSIX platforms (macOS/Linux) are supported today. On other platforms
// the client always reports "unsupported" and the caller falls back to the
// in-process path.

class Vst3RealtimeBridgeClient {
public:
    Vst3RealtimeBridgeClient();
    ~Vst3RealtimeBridgeClient();
    Vst3RealtimeBridgeClient(const Vst3RealtimeBridgeClient&) = delete;
    Vst3RealtimeBridgeClient& operator=(const Vst3RealtimeBridgeClient&) = delete;

    // Spawns the worker and blocks (off the audio thread) until the plugin is
    // prepared or preparation fails/times out. workerExecutablePath is the path
    // to neuracoust_vst3_process_worker.
    bool prepare(const Vst3PluginDescriptor& descriptor,
                 double sampleRate,
                 int maxBlockSize,
                 const std::string& workerExecutablePath,
                 std::string& message,
                 const std::string& preferredShmName = {});
    void reset();
    bool isReady() const;

    // Realtime-safe (bounded wait). Processes one interleaved-stereo block in
    // place. Returns false and leaves the buffer untouched if the worker did
    // not respond in time or has become unhealthy.
    bool process(float* interleavedStereo,
                 int frameCount,
                 const std::vector<Vst3ParameterValueState>& parameters,
                 std::vector<Vst3ParameterValueState>& outputParameters,
                 std::string& message);

    unsigned int latencySamples() const;
    unsigned int tailSamples() const;
    std::string className() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// True when the current platform can host plugins through the bridge.
bool vst3RealtimeBridgeSupported();

// Worker-process entry point. Invoked by neuracoust_vst3_process_worker when
// launched with --realtime-bridge. Returns a process exit code.
int runVst3RealtimeBridgeWorker(int argc, char** argv);

} // namespace neuracoust::daw
