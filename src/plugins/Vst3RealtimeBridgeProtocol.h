#pragma once

// Shared shared-memory protocol for the realtime out-of-process VST3 bridge.
//
// The client side lives in Vst3RealtimeBridge.cpp. The server (worker) side is
// used both by the headless worker (Vst3RealtimeBridge.cpp) and by the editor
// host process, so it lives here as a header-only helper: the editor host hosts
// ONE plugin instance that both shows the GUI and processes the real track
// audio pulled from this shared memory (replacing the old synthetic meter
// probe). POSIX only; the header compiles to nothing meaningful on Windows.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace neuracoust::daw {

constexpr uint32_t kVst3BridgeMagic = 0x4E435642u; // "NCVB"
constexpr uint32_t kVst3BridgeAbiVersion = 1u;
constexpr int32_t kVst3BridgeCommandProcess = 0;
constexpr int32_t kVst3BridgeCommandTerminate = 1;
constexpr int kVst3BridgeMaxParams = 512;

struct Vst3BridgeParam {
    uint32_t id = 0;
    float value = 0.0f;
};

// Deterministic shared-memory name for a track insert's realtime bridge, derived
// from a stable key (e.g. "<track>\x1f<slot>"). The engine (which creates the
// bridge) and the editor host / UI (which observe it to drive the plugin's own
// meters) both compute the same name from the same key without any handshake.
// FNV-1a keeps it identical across the separate DAW and editor-host binaries,
// and the result stays within the 31-char macOS POSIX shm-name limit.
inline std::string vst3BridgeObserverShmName(const std::string& key) {
    uint32_t hash = 2166136261u;
    for (unsigned char ch : key) {
        hash ^= ch;
        hash *= 16777619u;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "/ncob%08x", hash);
    return std::string(buffer);
}

} // namespace neuracoust::daw

#if !defined(_WIN32)

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace neuracoust::daw {

struct Vst3BridgeControlBlock {
    uint32_t magic;
    uint32_t abiVersion;
    int32_t maxBlockSize;
    double sampleRate;

    pthread_mutex_t mutex;
    pthread_cond_t requestCond;
    pthread_cond_t responseCond;

    uint32_t requestSeq;
    uint32_t responseSeq;
    int32_t command;

    int32_t workerReady;
    int32_t workerFailed;
    uint32_t latencySamples;
    uint32_t tailSamples;
    char className[128];
    char message[256];

    int32_t frameCount;
    int32_t numInParams;
    int32_t processedOk;
    int32_t numOutParams;
};

struct Vst3BridgeLayout {
    size_t controlOffset = 0;
    size_t inputOffset = 0;
    size_t outputOffset = 0;
    size_t inParamsOffset = 0;
    size_t outParamsOffset = 0;
    size_t totalSize = 0;
};

inline size_t vst3BridgeAlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

inline Vst3BridgeLayout vst3BridgeComputeLayout(int maxBlockSize) {
    const size_t block = static_cast<size_t>(std::max(1, maxBlockSize));
    const size_t audioBytes = block * 2u * sizeof(float);
    const size_t paramBytes = static_cast<size_t>(kVst3BridgeMaxParams) * sizeof(Vst3BridgeParam);
    Vst3BridgeLayout layout;
    layout.controlOffset = 0;
    layout.inputOffset = vst3BridgeAlignUp(sizeof(Vst3BridgeControlBlock), 64);
    layout.outputOffset = vst3BridgeAlignUp(layout.inputOffset + audioBytes, 64);
    layout.inParamsOffset = vst3BridgeAlignUp(layout.outputOffset + audioBytes, 64);
    layout.outParamsOffset = vst3BridgeAlignUp(layout.inParamsOffset + paramBytes, 64);
    layout.totalSize = vst3BridgeAlignUp(layout.outParamsOffset + paramBytes, 4096);
    return layout;
}

inline float* vst3BridgeAudioRegion(void* base, size_t offset) {
    return reinterpret_cast<float*>(static_cast<char*>(base) + offset);
}

inline Vst3BridgeParam* vst3BridgeParamRegion(void* base, size_t offset) {
    return reinterpret_cast<Vst3BridgeParam*>(static_cast<char*>(base) + offset);
}

// Server-side view over an already-created shared-memory segment. The client
// (Vst3RealtimeBridgeClient) creates and initializes the segment (including the
// process-shared mutex/conds) before launching the server process; the server
// only attaches.
class Vst3BridgeServer {
public:
    Vst3BridgeServer() = default;
    ~Vst3BridgeServer() { detach(); }
    Vst3BridgeServer(const Vst3BridgeServer&) = delete;
    Vst3BridgeServer& operator=(const Vst3BridgeServer&) = delete;

    bool attach(const std::string& shmName, int maxBlockSize) {
        detach();
        maxBlockSize_ = std::max(1, maxBlockSize);
        inputScratch_.assign(static_cast<size_t>(maxBlockSize_) * 2u, 0.0f);
        layout_ = vst3BridgeComputeLayout(maxBlockSize_);
        fd_ = shm_open(shmName.c_str(), O_RDWR, 0600);
        if (fd_ < 0) {
            return false;
        }
        mapping_ = mmap(nullptr, layout_.totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            close(fd_);
            fd_ = -1;
            return false;
        }
        control_ = reinterpret_cast<Vst3BridgeControlBlock*>(
            static_cast<char*>(mapping_) + layout_.controlOffset);
        if (control_->magic != kVst3BridgeMagic || control_->abiVersion != kVst3BridgeAbiVersion) {
            detach();
            return false;
        }
        return true;
    }

    void detach() {
        if (mapping_ != nullptr) {
            munmap(mapping_, layout_.totalSize);
            mapping_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        control_ = nullptr;
    }

    bool attached() const { return control_ != nullptr; }
    int maxBlockSize() const { return maxBlockSize_; }

    void announceReady(bool ok,
                       unsigned int latencySamples,
                       unsigned int tailSamples,
                       const std::string& className,
                       const std::string& message) {
        if (control_ == nullptr) {
            return;
        }
        pthread_mutex_lock(&control_->mutex);
        control_->latencySamples = latencySamples;
        control_->tailSamples = tailSamples;
        copyString(control_->className, sizeof(control_->className), className);
        copyString(control_->message, sizeof(control_->message), message);
        if (ok) {
            control_->workerReady = 1;
        } else {
            control_->workerFailed = 1;
        }
        pthread_cond_signal(&control_->responseCond);
        pthread_mutex_unlock(&control_->mutex);
    }

    // Blocks until the client posts a request. Returns false when the client
    // asked the server to terminate. On success, `outFrameCount`, `inputParams`,
    // and the input audio (via inputAudio()) describe the request; the caller
    // fills a local output buffer and calls postResponse(). The mutex is held
    // ONLY while copying the request in and the response out — never during the
    // caller's DSP — so the audio-thread client never blocks behind processing.
    bool waitRequest(int& outFrameCount, std::vector<Vst3BridgeParam>& inputParams) {
        if (control_ == nullptr) {
            return false;
        }
        pthread_mutex_lock(&control_->mutex);
        while (control_->requestSeq == handledSeq_) {
            pthread_cond_wait(&control_->requestCond, &control_->mutex);
        }
        handledSeq_ = control_->requestSeq;
        if (control_->command == kVst3BridgeCommandTerminate) {
            pthread_mutex_unlock(&control_->mutex);
            return false;
        }
        outFrameCount = std::max(0, std::min(control_->frameCount, maxBlockSize_));
        const int count = std::max(0, std::min(control_->numInParams, kVst3BridgeMaxParams));
        inputParams.clear();
        inputParams.reserve(static_cast<size_t>(count));
        const Vst3BridgeParam* in = vst3BridgeParamRegion(mapping_, layout_.inParamsOffset);
        for (int index = 0; index < count; ++index) {
            inputParams.push_back(in[index]);
        }
        // Copy the input out of shared memory so the client can immediately
        // overwrite the shared input region for the next block while we process.
        const size_t sampleCount = static_cast<size_t>(outFrameCount) * 2u;
        if (inputScratch_.size() < sampleCount) {
            inputScratch_.assign(sampleCount, 0.0f);
        }
        std::memcpy(inputScratch_.data(), vst3BridgeAudioRegion(mapping_, layout_.inputOffset),
                    sampleCount * sizeof(float));
        lastFrameCount_ = outFrameCount;
        pthread_mutex_unlock(&control_->mutex);
        return true;
    }

    // Server-owned copy of the request's input audio (safe to read without the
    // lock; the shared input region may already hold the next block).
    const float* input() const { return inputScratch_.data(); }

    void postResponse(bool processed,
                      const float* outputData,
                      unsigned int latencySamples,
                      unsigned int tailSamples,
                      const std::vector<Vst3BridgeParam>& outputParams) {
        if (control_ == nullptr) {
            return;
        }
        pthread_mutex_lock(&control_->mutex);
        if (processed && outputData != nullptr) {
            const size_t sampleCount = static_cast<size_t>(lastFrameCount_) * 2u;
            std::memcpy(vst3BridgeAudioRegion(mapping_, layout_.outputOffset), outputData,
                        sampleCount * sizeof(float));
        }
        const int outCount = std::min(static_cast<int>(outputParams.size()), kVst3BridgeMaxParams);
        Vst3BridgeParam* out = vst3BridgeParamRegion(mapping_, layout_.outParamsOffset);
        for (int index = 0; index < outCount; ++index) {
            out[index] = outputParams[static_cast<size_t>(index)];
        }
        control_->numOutParams = outCount;
        control_->latencySamples = latencySamples;
        control_->tailSamples = tailSamples;
        control_->processedOk = processed ? 1 : 0;
        control_->responseSeq = handledSeq_;
        pthread_cond_signal(&control_->responseCond);
        pthread_mutex_unlock(&control_->mutex);
    }

private:
    static void copyString(char* destination, size_t capacity, const std::string& source) {
        if (capacity == 0) {
            return;
        }
        const size_t length = std::min(source.size(), capacity - 1u);
        std::memcpy(destination, source.data(), length);
        destination[length] = '\0';
    }

    int fd_ = -1;
    void* mapping_ = nullptr;
    Vst3BridgeControlBlock* control_ = nullptr;
    Vst3BridgeLayout layout_;
    int maxBlockSize_ = 0;
    uint32_t handledSeq_ = 0;
    int lastFrameCount_ = 0;
    std::vector<float> inputScratch_;
};

} // namespace neuracoust::daw

#endif // !_WIN32
