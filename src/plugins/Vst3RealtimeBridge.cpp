#include "plugins/Vst3RealtimeBridge.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
// The bridge is POSIX-only for now. The client reports "unsupported" so callers
// transparently fall back to the in-process path, and the worker entry point is
// a no-op.
namespace neuracoust::daw {

struct Vst3RealtimeBridgeClient::Impl {};
Vst3RealtimeBridgeClient::Vst3RealtimeBridgeClient() : impl_(nullptr) {}
Vst3RealtimeBridgeClient::~Vst3RealtimeBridgeClient() = default;
bool Vst3RealtimeBridgeClient::prepare(const Vst3PluginDescriptor&, double, int, const std::string&, std::string& message, const std::string&) {
    message = "Out-of-process VST3 hosting is not supported on this platform.";
    return false;
}
void Vst3RealtimeBridgeClient::reset() {}
bool Vst3RealtimeBridgeClient::isReady() const { return false; }
bool Vst3RealtimeBridgeClient::process(float*, int, const std::vector<Vst3ParameterValueState>&, std::vector<Vst3ParameterValueState>&, std::string& message) {
    message = "Out-of-process VST3 hosting is not supported on this platform.";
    return false;
}
unsigned int Vst3RealtimeBridgeClient::latencySamples() const { return 0; }
unsigned int Vst3RealtimeBridgeClient::tailSamples() const { return 0; }
std::string Vst3RealtimeBridgeClient::className() const { return {}; }
bool vst3RealtimeBridgeSupported() { return false; }
int runVst3RealtimeBridgeWorker(int, char**) { return 70; }

} // namespace neuracoust::daw

#else // POSIX

#include "plugins/Vst3SdkAdapter.h"
#include "plugins/Vst3RealtimeBridgeProtocol.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace neuracoust::daw {

namespace {

void computeDeadline(struct timespec& deadline, long milliseconds) {
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
}

std::string argValue(int argc, char** argv, const std::string& key) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] != nullptr && key == argv[index]) {
            return argv[index + 1] != nullptr ? argv[index + 1] : "";
        }
    }
    return {};
}

long envLong(const char* name, long fallback, long minimum, long maximum) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (end == raw || value < minimum || value > maximum) {
        return fallback;
    }
    return value;
}

} // namespace

struct Vst3RealtimeBridgeClient::Impl {
    int shmFd = -1;
    void* mapping = nullptr;
    size_t mappingSize = 0;
    std::string shmName;
    pid_t workerPid = -1;
    Vst3BridgeLayout layout;
    int maxBlockSize = 0;
    bool ready = false;
    bool firstBlock = true;
    int consecutiveFailures = 0;
    int maxFailures = 12;
    long steadyTimeoutMs = 60;
    long warmupTimeoutMs = 1500;
    uint32_t lastResponseSeq = 0;
    int stallBlocks = 0;
    int maxStallBlocks = 4;
    unsigned int latency = 0;
    unsigned int tail = 0;
    std::string className;

    Vst3BridgeControlBlock* control() {
        return mapping != nullptr
            ? reinterpret_cast<Vst3BridgeControlBlock*>(static_cast<char*>(mapping) + layout.controlOffset)
            : nullptr;
    }

    void teardown() {
        if (mapping != nullptr && workerPid > 0) {
            Vst3BridgeControlBlock* cb = control();
            if (cb != nullptr) {
                pthread_mutex_lock(&cb->mutex);
                cb->command = kVst3BridgeCommandTerminate;
                ++cb->requestSeq;
                pthread_cond_signal(&cb->requestCond);
                pthread_mutex_unlock(&cb->mutex);
            }
        }
        if (workerPid > 0) {
            for (int waited = 0; waited < 20; ++waited) {
                int status = 0;
                const pid_t result = waitpid(workerPid, &status, WNOHANG);
                if (result == workerPid || result < 0) {
                    workerPid = -1;
                    break;
                }
                usleep(10000);
            }
            if (workerPid > 0) {
                kill(workerPid, SIGKILL);
                waitpid(workerPid, nullptr, 0);
                workerPid = -1;
            }
        }
        if (mapping != nullptr) {
            Vst3BridgeControlBlock* cb = control();
            if (cb != nullptr) {
                pthread_cond_destroy(&cb->requestCond);
                pthread_cond_destroy(&cb->responseCond);
                pthread_mutex_destroy(&cb->mutex);
            }
            munmap(mapping, mappingSize);
            mapping = nullptr;
            mappingSize = 0;
        }
        if (shmFd >= 0) {
            close(shmFd);
            shmFd = -1;
        }
        if (!shmName.empty()) {
            shm_unlink(shmName.c_str());
            shmName.clear();
        }
        ready = false;
    }

    bool workerAlive() {
        if (workerPid <= 0) {
            return false;
        }
        int status = 0;
        const pid_t result = waitpid(workerPid, &status, WNOHANG);
        if (result == 0) {
            return true;
        }
        if (result == workerPid) {
            workerPid = -1;
        }
        return false;
    }
};

Vst3RealtimeBridgeClient::Vst3RealtimeBridgeClient() : impl_(std::make_unique<Impl>()) {}

Vst3RealtimeBridgeClient::~Vst3RealtimeBridgeClient() {
    if (impl_) {
        impl_->teardown();
    }
}

bool Vst3RealtimeBridgeClient::prepare(const Vst3PluginDescriptor& descriptor,
                                       double sampleRate,
                                       int maxBlockSize,
                                       const std::string& workerExecutablePath,
                                       std::string& message,
                                       const std::string& preferredShmName) {
    message.clear();
    impl_->teardown();

    if (workerExecutablePath.empty() || access(workerExecutablePath.c_str(), X_OK) != 0) {
        message = "Out-of-process VST3 worker executable is missing.";
        return false;
    }
    const std::string pluginPath = descriptor.bundlePath.empty()
        ? descriptor.executablePath
        : descriptor.bundlePath;
    if (pluginPath.empty()) {
        message = "VST3 plugin path is missing for out-of-process hosting.";
        return false;
    }

    impl_->maxBlockSize = std::max(1, maxBlockSize);
    impl_->layout = vst3BridgeComputeLayout(impl_->maxBlockSize);
    impl_->steadyTimeoutMs = envLong("NEURACOUST_VST3_BRIDGE_TIMEOUT_MS", 60, 5, 1000);
    impl_->warmupTimeoutMs = envLong("NEURACOUST_VST3_BRIDGE_WARMUP_MS", 1500, 100, 30000);
    impl_->maxFailures = static_cast<int>(envLong("NEURACOUST_VST3_BRIDGE_MAX_FAILURES", 12, 1, 100000));

    if (!preferredShmName.empty()) {
        // Deterministic name so the editor host can observe this exact insert's
        // bridge (read-only) to drive the plugin's own meters/graphics.
        impl_->shmName = preferredShmName;
    } else {
        static std::atomic<unsigned int> counter {0};
        const unsigned int unique = counter.fetch_add(1u, std::memory_order_relaxed);
        // macOS caps POSIX shared-memory names at 31 characters (including the slash).
        impl_->shmName = "/ncvb" + std::to_string(static_cast<int>(getpid()) % 100000) +
            "_" + std::to_string(unique % 1000);
    }

    shm_unlink(impl_->shmName.c_str());
    impl_->shmFd = shm_open(impl_->shmName.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (impl_->shmFd < 0) {
        message = "Could not create shared memory for VST3 bridge: " + std::string(std::strerror(errno));
        impl_->shmName.clear();
        return false;
    }
    impl_->mappingSize = impl_->layout.totalSize;
    if (ftruncate(impl_->shmFd, static_cast<off_t>(impl_->mappingSize)) != 0) {
        message = "Could not size shared memory for VST3 bridge: " + std::string(std::strerror(errno));
        impl_->teardown();
        return false;
    }
    impl_->mapping = mmap(nullptr, impl_->mappingSize, PROT_READ | PROT_WRITE, MAP_SHARED, impl_->shmFd, 0);
    if (impl_->mapping == MAP_FAILED) {
        impl_->mapping = nullptr;
        message = "Could not map shared memory for VST3 bridge: " + std::string(std::strerror(errno));
        impl_->teardown();
        return false;
    }
    std::memset(impl_->mapping, 0, impl_->mappingSize);

    Vst3BridgeControlBlock* cb = impl_->control();
    cb->magic = kVst3BridgeMagic;
    cb->abiVersion = kVst3BridgeAbiVersion;
    cb->maxBlockSize = impl_->maxBlockSize;
    cb->sampleRate = sampleRate;
    cb->requestSeq = 0;
    cb->responseSeq = 0;
    cb->command = kVst3BridgeCommandProcess;
    cb->workerReady = 0;
    cb->workerFailed = 0;

    pthread_mutexattr_t mutexAttr;
    pthread_mutexattr_init(&mutexAttr);
    pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&cb->mutex, &mutexAttr);
    pthread_mutexattr_destroy(&mutexAttr);

    pthread_condattr_t condAttr;
    pthread_condattr_init(&condAttr);
    pthread_condattr_setpshared(&condAttr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&cb->requestCond, &condAttr);
    pthread_cond_init(&cb->responseCond, &condAttr);
    pthread_condattr_destroy(&condAttr);

    const std::string sampleRateArg = std::to_string(sampleRate);
    const std::string maxBlockArg = std::to_string(impl_->maxBlockSize);
    std::vector<std::string> argStorage {
        workerExecutablePath,
        "--realtime-bridge",
        "--shm", impl_->shmName,
        "--plugin", pluginPath,
        "--name", descriptor.name,
        "--class-id", descriptor.componentClassCid,
        "--class-name", descriptor.componentClassName,
        "--sample-rate", sampleRateArg,
        "--max-block", maxBlockArg
    };
    std::vector<char*> args;
    args.reserve(argStorage.size() + 1);
    for (auto& arg : argStorage) {
        args.push_back(arg.data());
    }
    args.push_back(nullptr);

    const bool keepStderr = std::getenv("NEURACOUST_VST3_BRIDGE_DEBUG") != nullptr;
    const pid_t pid = fork();
    if (pid < 0) {
        message = "Could not fork VST3 bridge worker.";
        impl_->teardown();
        return false;
    }
    if (pid == 0) {
        setenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3", "1", 1);
        if (!keepStderr) {
            const int devNull = open("/dev/null", O_WRONLY);
            if (devNull >= 0) {
                dup2(devNull, STDERR_FILENO);
                dup2(devNull, STDOUT_FILENO);
                close(devNull);
            }
        }
        execv(workerExecutablePath.c_str(), args.data());
        _exit(127);
    }
    impl_->workerPid = pid;

    // prepare() runs while the engine mutex is held (adding/copying an insert
    // rebuilds the graph), and the realtime render blocks on that same mutex — so
    // every millisecond we wait here is a millisecond the audio thread is stalled,
    // i.e. an audible drop when copying a track mid-playback. Wait only a hair to
    // catch an instant spawn failure; if the worker is still loading, return
    // success optimistically — process() feeds the dry signal through until the
    // worker responds, so a newly added insert is briefly dry instead of dropping
    // the whole mix. The worker itself loads in the background at UTILITY priority.
    const long readyWaitMs = envLong("NEURACOUST_VST3_BRIDGE_READY_WAIT_MS", 8, 0, 20000);
    struct timespec overallDeadline;
    computeDeadline(overallDeadline, readyWaitMs);
    bool ready = false;
    bool failed = false;
    pthread_mutex_lock(&cb->mutex);
    while (cb->workerReady == 0 && cb->workerFailed == 0) {
        if (!impl_->workerAlive()) {
            failed = true;
            break;
        }
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > overallDeadline.tv_sec ||
            (now.tv_sec == overallDeadline.tv_sec && now.tv_nsec >= overallDeadline.tv_nsec)) {
            break;
        }
        struct timespec slice;
        computeDeadline(slice, 25);
        pthread_cond_timedwait(&cb->responseCond, &cb->mutex, &slice);
    }
    ready = cb->workerReady != 0;
    failed = failed || cb->workerFailed != 0;
    impl_->latency = cb->latencySamples;
    impl_->tail = cb->tailSamples;
    impl_->className = std::string(cb->className);
    const std::string workerMessage = std::string(cb->message);
    pthread_mutex_unlock(&cb->mutex);

    if (failed) {
        // Only an explicit failure / dead worker is fatal; a still-loading worker
        // is accepted and produces dry audio until it is ready.
        message = workerMessage.empty()
            ? "Out-of-process VST3 worker did not become ready."
            : workerMessage;
        impl_->teardown();
        return false;
    }
    (void)ready;

    impl_->ready = true;
    impl_->firstBlock = true;
    impl_->consecutiveFailures = 0;
    impl_->lastResponseSeq = 0;
    impl_->stallBlocks = 0;
    message = workerMessage.empty() ? "Out-of-process VST3 worker prepared." : workerMessage;
    return true;
}

void Vst3RealtimeBridgeClient::reset() {
    impl_->teardown();
}

bool Vst3RealtimeBridgeClient::isReady() const {
    return impl_->ready;
}

bool Vst3RealtimeBridgeClient::process(float* interleavedStereo,
                                       int frameCount,
                                       const std::vector<Vst3ParameterValueState>& parameters,
                                       std::vector<Vst3ParameterValueState>& outputParameters,
                                       std::string& message) {
    outputParameters.clear();
    message.clear();
    if (!impl_->ready) {
        message = "Out-of-process VST3 worker is not ready.";
        return false;
    }
    if (interleavedStereo == nullptr || frameCount <= 0 || frameCount > impl_->maxBlockSize) {
        message = "Out-of-process VST3 process block is invalid.";
        return false;
    }
    Vst3BridgeControlBlock* cb = impl_->control();
    void* base = impl_->mapping;
    const size_t sampleCount = static_cast<size_t>(frameCount) * 2u;

    // Realtime-safe, non-blocking exchange: publish this block's dry input for
    // the server, and return the output it produced for a PREVIOUS block (one
    // block of latency). The audio thread NEVER waits for the server, so a slow
    // or busy server can never stall the callback. While the server is warming
    // up or has fallen behind, we pass the dry signal through instead of a
    // frozen/garbage buffer.
    // Never BLOCK the realtime thread on the mutex: if a normal-priority worker is
    // descheduled while briefly holding it under CPU load, a plain lock would stall
    // the audio callback (priority inversion — audible as slowdown/jitter, worse
    // with several heavy out-of-process plug-ins). Use trylock and, on contention,
    // pass the dry signal through for this one block.
    if (pthread_mutex_trylock(&cb->mutex) != 0) {
        ++impl_->stallBlocks;
        message = "Out-of-process VST3 worker busy; dry for this block.";
        return true; // interleavedStereo left unchanged = dry passthrough
    }
    float* input = vst3BridgeAudioRegion(base, impl_->layout.inputOffset);
    std::memcpy(input, interleavedStereo, sampleCount * sizeof(float));
    cb->frameCount = frameCount;

    const int paramCount = std::min(static_cast<int>(parameters.size()), kVst3BridgeMaxParams);
    Vst3BridgeParam* inParams = vst3BridgeParamRegion(base, impl_->layout.inParamsOffset);
    for (int index = 0; index < paramCount; ++index) {
        inParams[index].id = parameters[static_cast<size_t>(index)].parameterId;
        inParams[index].value = static_cast<float>(parameters[static_cast<size_t>(index)].normalizedValue);
    }
    cb->numInParams = paramCount;
    cb->command = kVst3BridgeCommandProcess;
    cb->requestSeq += 1u;
    pthread_cond_signal(&cb->requestCond);

    const uint32_t responseSeq = cb->responseSeq;
    const bool fresh = responseSeq != impl_->lastResponseSeq;
    impl_->lastResponseSeq = responseSeq;
    if (fresh) {
        impl_->stallBlocks = 0;
    } else {
        ++impl_->stallBlocks;
    }
    const bool useOutput = responseSeq != 0 && cb->processedOk != 0 &&
        impl_->stallBlocks <= impl_->maxStallBlocks;
    if (useOutput) {
        const float* output = vst3BridgeAudioRegion(base, impl_->layout.outputOffset);
        std::memcpy(interleavedStereo, output, sampleCount * sizeof(float));
        const int outCount = std::min(cb->numOutParams, kVst3BridgeMaxParams);
        if (outCount > 0) {
            Vst3BridgeParam* outParams = vst3BridgeParamRegion(base, impl_->layout.outParamsOffset);
            outputParameters.reserve(static_cast<size_t>(outCount));
            for (int index = 0; index < outCount; ++index) {
                outputParameters.push_back({outParams[index].id, {}, static_cast<double>(outParams[index].value)});
            }
        }
        impl_->latency = cb->latencySamples;
        impl_->tail = cb->tailSamples;
    }
    pthread_mutex_unlock(&cb->mutex);

    // Either way we produced a valid block (wet when available, otherwise the
    // dry input we left in place), so the caller keeps our signal.
    return true;
}

unsigned int Vst3RealtimeBridgeClient::latencySamples() const { return impl_->latency; }
unsigned int Vst3RealtimeBridgeClient::tailSamples() const { return impl_->tail; }
std::string Vst3RealtimeBridgeClient::className() const { return impl_->className; }

bool vst3RealtimeBridgeSupported() { return true; }

int runVst3RealtimeBridgeWorker(int argc, char** argv) {
    setenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3", "1", 1);

    // Load the plug-in at BACKGROUND priority. A heavy plug-in (a Waves shell) can
    // burn a core for seconds while it initialises, and when several tracks each
    // spawn a worker at once — e.g. pressing Play with 4 plug-ins — that load spike
    // saturates the cores and starves the realtime audio thread, causing the
    // start-of-playback dropouts that scale with plug-in count. QOS_CLASS_UTILITY
    // makes the OS schedule this initialisation on efficiency cores / below audio,
    // so the load costs a slightly longer warm-up instead of glitching playback.
    // Once the plug-in is ready we raise the QoS for the realtime processing loop.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    const std::string shmName = argValue(argc, argv, "--shm");
    const std::string pluginPath = argValue(argc, argv, "--plugin");
    const std::string pluginName = argValue(argc, argv, "--name");
    const std::string pluginClassId = argValue(argc, argv, "--class-id");
    const std::string pluginClassName = argValue(argc, argv, "--class-name");
    const std::string sampleRateText = argValue(argc, argv, "--sample-rate");
    const std::string maxBlockText = argValue(argc, argv, "--max-block");
    if (shmName.empty() || pluginPath.empty()) {
        return 64;
    }
    const double sampleRate = sampleRateText.empty() ? 48000.0 : std::strtod(sampleRateText.c_str(), nullptr);
    const int maxBlockSize = maxBlockText.empty() ? 256 : std::max(1, static_cast<int>(std::strtol(maxBlockText.c_str(), nullptr, 10)));

    Vst3BridgeServer server;
    if (!server.attach(shmName, maxBlockSize)) {
        return 65;
    }

    auto descriptor = resolveVst3PluginDescriptorForInsert(pluginName, pluginPath, pluginClassId, pluginClassName);
    Vst3RealtimeProcessor processor;
    std::string prepareMessage;
    const bool prepared = processor.prepare(descriptor, sampleRate, maxBlockSize, prepareMessage);
    server.announceReady(prepared,
                         prepared ? processor.probe().latencySamples : 0u,
                         prepared ? processor.probe().tailSamples : 0u,
                         descriptor.name,
                         prepareMessage);
    if (!prepared) {
        return 68;
    }

    // The worker's processing loop has a hard per-block deadline: the realtime
    // audio thread reads last block's output and moves on, so if this thread is
    // descheduled under CPU load the audio thread falls back to dry (trylock).
    // Promote it above the observer/GUI (USER_INITIATED) so the async pipeline
    // keeps up, but keep it strictly BELOW the DAW's realtime audio render thread
    // (which uses hard real-time time-constraint scheduling) — tying with the
    // audio thread would let the worker contend for the same cores and delay the
    // audio callback (wake jitter). Below it, the worker runs in the idle window
    // between callbacks and never steals the audio thread's slot.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);

    std::vector<float> scratch(static_cast<size_t>(maxBlockSize) * 2u, 0.0f);
    int frameCount = 0;
    std::vector<Vst3BridgeParam> inputParams;
    while (server.waitRequest(frameCount, inputParams)) {
        std::vector<Vst3ParameterValueState> parameters;
        parameters.reserve(inputParams.size());
        for (const auto& param : inputParams) {
            parameters.push_back({param.id, {}, static_cast<double>(param.value)});
        }

        const size_t sampleCount = static_cast<size_t>(frameCount) * 2u;
        std::memcpy(scratch.data(), server.input(), sampleCount * sizeof(float));

        std::string processMessage;
        const auto result = processor.processInterleavedStereo(scratch.data(), frameCount, parameters, processMessage);

        std::vector<Vst3BridgeParam> outputParams;
        if (result.processed) {
            for (const auto& change : processor.drainOutputParameterChanges()) {
                outputParams.push_back({change.parameterId, static_cast<float>(change.normalizedValue)});
            }
        }
        server.postResponse(result.processed, scratch.data(), result.latencySamples, result.tailSamples, outputParams);
    }

    processor.reset();
    server.detach();
    return 0;
}

} // namespace neuracoust::daw

#endif // POSIX
