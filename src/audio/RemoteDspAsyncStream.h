#pragma once

#include "audio/RemoteDspServerClient.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace neuracoust::daw {

struct RemoteDspAsyncStreamStatus {
    bool running = false;
    bool outputReady = false;
    uint32_t queuedInputBlocks = 0;
    uint32_t queuedOutputBlocks = 0;
    uint64_t submittedBlocks = 0;
    uint64_t processedBlocks = 0;
    uint64_t droppedInputBlocks = 0;
    uint64_t underrunBlocks = 0;
    uint64_t failedBlocks = 0;
    double averageRoundTripMs = 0.0;
    double averageRoundTripJitterUs = 0.0;
    double lastRoundTripJitterUs = 0.0;
    double maxRoundTripJitterUs = 0.0;
    std::string message;
};

class RemoteDspAsyncStream {
public:
    RemoteDspAsyncStream();
    ~RemoteDspAsyncStream();

    RemoteDspAsyncStream(const RemoteDspAsyncStream&) = delete;
    RemoteDspAsyncStream& operator=(const RemoteDspAsyncStream&) = delete;

    bool process(const RemoteDspServerSettings& settings,
                 const std::vector<float>& inputInterleavedStereo,
                 std::vector<float>& outputInterleavedStereo);
    bool process(const RemoteDspServerSettings& settings,
                 const std::vector<float>& inputInterleavedStereo,
                 const std::vector<RemoteDspParameterValue>& parameters,
                 std::vector<float>& outputInterleavedStereo);
    void reset();
    RemoteDspAsyncStreamStatus status() const;

private:
    struct Block {
        uint64_t id = 0;
        std::vector<float> samples;
        std::vector<RemoteDspParameterValue> parameters;
    };

    /// Consecutive worker failures. A node that is not answering must cost the render thread
    /// NOTHING — otherwise every block pays the queue wait forever — so past the threshold
    /// process() returns dry immediately and only lets one block through every so often to probe.
    uint32_t consecutiveFailures_ = 0;
    uint64_t blocksSinceProbe_ = 0;

    bool settingsMatchLocked(const RemoteDspServerSettings& settings, size_t frameCount) const;
    void configureLocked(const RemoteDspServerSettings& settings, size_t frameCount, std::unique_lock<std::mutex>& lock);
    void stopLocked(std::unique_lock<std::mutex>& lock);
    void workerLoop();
    void recordRoundTripLocked(double roundTripMs);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    RemoteDspProcessSession processSession_;
    RemoteDspServerSettings settings_;
    size_t frameCount_ = 0;
    size_t targetBufferedBlocks_ = 1;
    bool stopRequested_ = false;
    bool configured_ = false;
    std::deque<Block> inputQueue_;
    std::deque<Block> outputQueue_;
    RemoteDspAsyncStreamStatus status_;
    uint64_t nextBlockId_ = 1;
    double previousRoundTripMs_ = 0.0;
    bool previousRoundTripValid_ = false;
};

} // namespace neuracoust::daw
