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
constexpr uint32_t kVst3BridgeAbiVersion = 2u;     // 2: added the observer block ring
constexpr int32_t kVst3BridgeCommandProcess = 0;
constexpr int32_t kVst3BridgeCommandTerminate = 1;
constexpr int kVst3BridgeMaxParams = 512;
// A passive metering reader (the editor's scope) reads a small ring of recent blocks rather
// than a single latest slot: the engine hands the bridge its audio in bursts (SoundGrid wakes
// several buffers at once), so a one-block slot dropped every block but the last in a burst —
// the scope was fed ~60% of realtime and combed into vertical stripes. The ring holds enough
// blocks (~85 ms at 256/48k) that a UTILITY-priority reader never loses one to a burst.
constexpr int kVst3BridgeObserverRingBlocks = 16;

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

// Name for an instrument editor's REVERSE audio ring (editor → DAW). The editor
// host publishes the audio its own instrument instance renders (GUI keyboard
// clicks, forwarded live MIDI) and the DAW mixes it into the monitor path — the
// only way a note born inside the plug-in's GUI can be heard, since the GUI's
// note messages are vendor-private and never visible to the host.
inline std::string vst3InstrumentMonitorShmName(const std::string& key) {
    uint32_t hash = 2166136261u;
    for (unsigned char ch : key) {
        hash ^= ch;
        hash *= 16777619u;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "/ncim%08x", hash);
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

    // Observer block ring (metering only; the worker never reads these). The publisher
    // appends every process block here and release-stores observerRingWrite last; a passive
    // reader acquire-loads observerRingWrite and drains slots [read, write) in order, so it
    // sees EVERY block even when several are published between its polls.
    uint64_t observerRingWrite;
    int32_t observerRingFrames[kVst3BridgeObserverRingBlocks];
};

struct Vst3BridgeLayout {
    size_t controlOffset = 0;
    size_t inputOffset = 0;
    size_t outputOffset = 0;
    size_t inParamsOffset = 0;
    size_t outParamsOffset = 0;
    size_t observerRingOffset = 0;
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
    layout.observerRingOffset = vst3BridgeAlignUp(layout.outParamsOffset + paramBytes, 64);
    const size_t ringBytes = audioBytes * static_cast<size_t>(kVst3BridgeObserverRingBlocks);
    layout.totalSize = vst3BridgeAlignUp(layout.observerRingOffset + ringBytes, 4096);
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

// ---------------------------------------------------------------------------
// Instrument editor monitor ring (editor host → DAW, audio only)
// ---------------------------------------------------------------------------
//
// Single writer (the editor host's paced instrument pump), single reader (a
// UTILITY thread in the DAW that feeds the engine's monitor FIFO). Lock-free:
// the writer fills a slot then release-stores ringWrite; the reader
// acquire-loads ringWrite and drains [read, write). Same discipline as the
// insert bridge's observer ring. The DAW creates and initializes the segment
// before spawning the editor host; the host only attaches.

constexpr uint32_t kVst3InstrumentMonitorMagic = 0x4E43494Du; // "NCIM"
constexpr uint32_t kVst3InstrumentMonitorAbiVersion = 1u;
constexpr int kVst3InstrumentMonitorRingBlocks = 16;

struct Vst3InstrumentMonitorControl {
    uint32_t magic;
    uint32_t abiVersion;
    int32_t maxBlockSize;
    double sampleRate;
    uint64_t ringWrite; // release-stored by the writer after filling a slot
    int32_t ringFrames[kVst3InstrumentMonitorRingBlocks];
};

inline size_t vst3InstrumentMonitorSegmentSize(int maxBlockSize) {
    const size_t block = static_cast<size_t>(std::max(1, maxBlockSize));
    const size_t audioBytes = block * 2u * sizeof(float) *
        static_cast<size_t>(kVst3InstrumentMonitorRingBlocks);
    return vst3BridgeAlignUp(vst3BridgeAlignUp(sizeof(Vst3InstrumentMonitorControl), 64) + audioBytes, 4096);
}

inline size_t vst3InstrumentMonitorRingOffset() {
    return vst3BridgeAlignUp(sizeof(Vst3InstrumentMonitorControl), 64);
}

// DAW side: create + initialize the segment. Returns true when the mapping is
// ready for a writer to attach. Safe to call again for the same name (recreates).
class Vst3InstrumentMonitorReader {
public:
    Vst3InstrumentMonitorReader() = default;
    ~Vst3InstrumentMonitorReader() { close(); }
    Vst3InstrumentMonitorReader(const Vst3InstrumentMonitorReader&) = delete;
    Vst3InstrumentMonitorReader& operator=(const Vst3InstrumentMonitorReader&) = delete;

    bool create(const std::string& shmName, int maxBlockSize, double sampleRate) {
        close();
        maxBlockSize_ = std::max(1, maxBlockSize);
        const size_t total = vst3InstrumentMonitorSegmentSize(maxBlockSize_);
        shm_unlink(shmName.c_str()); // stale segment from a crashed run
        fd_ = shm_open(shmName.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) {
            return false;
        }
        if (ftruncate(fd_, static_cast<off_t>(total)) != 0) {
            close();
            return false;
        }
        mapping_ = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            close();
            return false;
        }
        totalSize_ = total;
        control_ = reinterpret_cast<Vst3InstrumentMonitorControl*>(mapping_);
        std::memset(control_, 0, sizeof(*control_));
        control_->maxBlockSize = maxBlockSize_;
        control_->sampleRate = sampleRate;
        control_->abiVersion = kVst3InstrumentMonitorAbiVersion;
        __atomic_store_n(&control_->ringWrite, 0ull, __ATOMIC_RELEASE);
        // Magic last: a writer that maps mid-initialization sees no magic and waits.
        __atomic_store_n(&control_->magic, kVst3InstrumentMonitorMagic, __ATOMIC_RELEASE);
        readCursor_ = 0;
        name_ = shmName;
        return true;
    }

    void close() {
        if (mapping_ != nullptr) {
            munmap(mapping_, totalSize_);
            mapping_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (!name_.empty()) {
            shm_unlink(name_.c_str());
            name_.clear();
        }
        control_ = nullptr;
    }

    bool ready() const { return control_ != nullptr; }

    // Drains every block published since the last call. `sink(samples, frames)`
    // receives interleaved stereo. Falls behind → skips ahead (drop-oldest).
    template <typename Sink>
    void drain(Sink&& sink) {
        if (control_ == nullptr) {
            return;
        }
        const uint64_t write = __atomic_load_n(&control_->ringWrite, __ATOMIC_ACQUIRE);
        if (write <= readCursor_) {
            return;
        }
        if (write - readCursor_ > static_cast<uint64_t>(kVst3InstrumentMonitorRingBlocks)) {
            readCursor_ = write - static_cast<uint64_t>(kVst3InstrumentMonitorRingBlocks);
        }
        const float* ring = reinterpret_cast<const float*>(
            static_cast<const char*>(mapping_) + vst3InstrumentMonitorRingOffset());
        const size_t slotSamples = static_cast<size_t>(maxBlockSize_) * 2u;
        while (readCursor_ < write) {
            const int slot = static_cast<int>(readCursor_ % kVst3InstrumentMonitorRingBlocks);
            const int frames = std::max(0, std::min(control_->ringFrames[slot], maxBlockSize_));
            if (frames > 0) {
                sink(ring + static_cast<size_t>(slot) * slotSamples, frames);
            }
            ++readCursor_;
        }
    }

private:
    int fd_ = -1;
    void* mapping_ = nullptr;
    size_t totalSize_ = 0;
    Vst3InstrumentMonitorControl* control_ = nullptr;
    int maxBlockSize_ = 0;
    uint64_t readCursor_ = 0;
    std::string name_;
};

// Editor-host side: attach to the DAW-created segment and publish blocks.
class Vst3InstrumentMonitorWriter {
public:
    Vst3InstrumentMonitorWriter() = default;
    ~Vst3InstrumentMonitorWriter() { detach(); }
    Vst3InstrumentMonitorWriter(const Vst3InstrumentMonitorWriter&) = delete;
    Vst3InstrumentMonitorWriter& operator=(const Vst3InstrumentMonitorWriter&) = delete;

    bool attach(const std::string& shmName, int maxBlockSize) {
        detach();
        maxBlockSize_ = std::max(1, maxBlockSize);
        const size_t total = vst3InstrumentMonitorSegmentSize(maxBlockSize_);
        fd_ = shm_open(shmName.c_str(), O_RDWR, 0600);
        if (fd_ < 0) {
            return false;
        }
        mapping_ = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            detach();
            return false;
        }
        totalSize_ = total;
        control_ = reinterpret_cast<Vst3InstrumentMonitorControl*>(mapping_);
        if (__atomic_load_n(&control_->magic, __ATOMIC_ACQUIRE) != kVst3InstrumentMonitorMagic ||
            control_->abiVersion != kVst3InstrumentMonitorAbiVersion ||
            control_->maxBlockSize != maxBlockSize_) {
            detach();
            return false;
        }
        return true;
    }

    void detach() {
        if (mapping_ != nullptr) {
            munmap(mapping_, totalSize_);
            mapping_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        control_ = nullptr;
    }

    bool attached() const { return control_ != nullptr; }

    void publish(const float* interleavedStereo, int frames) {
        if (control_ == nullptr || interleavedStereo == nullptr || frames <= 0) {
            return;
        }
        frames = std::min(frames, maxBlockSize_);
        const uint64_t write = __atomic_load_n(&control_->ringWrite, __ATOMIC_RELAXED);
        const int slot = static_cast<int>(write % kVst3InstrumentMonitorRingBlocks);
        float* ring = reinterpret_cast<float*>(
            static_cast<char*>(mapping_) + vst3InstrumentMonitorRingOffset());
        std::memcpy(ring + static_cast<size_t>(slot) * static_cast<size_t>(maxBlockSize_) * 2u,
                    interleavedStereo,
                    static_cast<size_t>(frames) * 2u * sizeof(float));
        control_->ringFrames[slot] = frames;
        __atomic_store_n(&control_->ringWrite, write + 1u, __ATOMIC_RELEASE);
    }

private:
    int fd_ = -1;
    void* mapping_ = nullptr;
    size_t totalSize_ = 0;
    Vst3InstrumentMonitorControl* control_ = nullptr;
    int maxBlockSize_ = 0;
};

} // namespace neuracoust::daw

#endif // !_WIN32
