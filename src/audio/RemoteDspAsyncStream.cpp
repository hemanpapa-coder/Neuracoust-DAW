#include "audio/RemoteDspAsyncStream.h"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace neuracoust::daw {

namespace {
constexpr size_t kMaxInputQueueBlocks = 12;
constexpr size_t kMaxOutputQueueBlocks = 12;

std::string streamSettingsKey(const RemoteDspServerSettings& settings) {
    return settings.host + ":" + std::to_string(settings.rtEnginePort) + ":" +
        std::to_string(settings.channelCount) + ":" +
        std::to_string(settings.networkBufferFrames) + ":" +
        std::to_string(static_cast<int>(settings.sampleRate));
}
} // namespace

RemoteDspAsyncStream::RemoteDspAsyncStream() = default;

RemoteDspAsyncStream::~RemoteDspAsyncStream() {
    reset();
}

bool RemoteDspAsyncStream::settingsMatchLocked(const RemoteDspServerSettings& settings, size_t frameCount) const {
    return configured_ &&
        frameCount_ == frameCount &&
        streamSettingsKey(settings_) == streamSettingsKey(settings);
}

void RemoteDspAsyncStream::configureLocked(const RemoteDspServerSettings& settings,
                                           size_t frameCount,
                                           std::unique_lock<std::mutex>& lock) {
    stopLocked(lock);
    settings_ = settings;
    frameCount_ = frameCount;
    const size_t bufferFrames = std::max<size_t>(1, settings.networkBufferFrames);
    targetBufferedBlocks_ = std::max<size_t>(1, (bufferFrames + std::max<size_t>(1, frameCount_) - 1) / std::max<size_t>(1, frameCount_));
    targetBufferedBlocks_ = std::min<size_t>(targetBufferedBlocks_, 8);
    stopRequested_ = false;
    configured_ = true;
    // A different node (or a changed buffer) starts fresh: carrying the old node's failure count
    // across would leave the new one in backoff, probing once every few seconds for no reason.
    consecutiveFailures_ = 0;
    blocksSinceProbe_ = 0;
    inputQueue_.clear();
    outputQueue_.clear();
    previousRoundTripMs_ = 0.0;
    previousRoundTripValid_ = false;
    status_ = {};
    status_.running = true;
    status_.message = "Remote DSP async stream armed.";
    worker_ = std::thread(&RemoteDspAsyncStream::workerLoop, this);
}

void RemoteDspAsyncStream::stopLocked(std::unique_lock<std::mutex>& lock) {
    if (worker_.joinable()) {
        stopRequested_ = true;
        condition_.notify_all();
        auto worker = std::move(worker_);
        lock.unlock();
        worker.join();
        lock.lock();
    }
    inputQueue_.clear();
    outputQueue_.clear();
    processSession_.reset();
    configured_ = false;
    stopRequested_ = false;
    status_.running = false;
    status_.outputReady = false;
}

void RemoteDspAsyncStream::reset() {
    consecutiveFailures_ = 0;
    blocksSinceProbe_ = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    stopLocked(lock);
    status_ = {};
}

bool RemoteDspAsyncStream::process(const RemoteDspServerSettings& settings,
                                   const std::vector<float>& inputInterleavedStereo,
                                   std::vector<float>& outputInterleavedStereo) {
    static const std::vector<RemoteDspParameterValue> emptyParameters;
    return process(settings, inputInterleavedStereo, emptyParameters, outputInterleavedStereo);
}

bool RemoteDspAsyncStream::process(const RemoteDspServerSettings& settings,
                                   const std::vector<float>& inputInterleavedStereo,
                                   const std::vector<RemoteDspParameterValue>& parameters,
                                   std::vector<float>& outputInterleavedStereo) {
    outputInterleavedStereo.clear();
    if (inputInterleavedStereo.empty() || inputInterleavedStereo.size() % 2u != 0u) {
        return false;
    }
    const size_t frameCount = inputInterleavedStereo.size() / 2u;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!settingsMatchLocked(settings, frameCount)) {
        configureLocked(settings, frameCount, lock);
    }

    // No synchronous prime here. This runs on the audio render thread, and priming did a blocking
    // network round trip with an 8 ms timeout against a 5.33 ms buffer period — so an unreachable
    // node made every single block overrun, forever, because processedBlocks never left zero.
    // The worker primes instead; until it succeeds this returns false and the caller stays dry.

    // A node that has failed repeatedly is treated as absent: submit nothing, wait for nothing,
    // and tell the caller to stay dry. One block in every 256 still goes out so the stream can
    // recover on its own when the node comes back. Without this the render thread paid the queue
    // wait on every single block for as long as the node stayed unreachable.
    constexpr uint32_t kFailuresBeforeBackoff = 8;
    constexpr uint64_t kProbeInterval = 256;
    if (consecutiveFailures_ >= kFailuresBeforeBackoff) {
        const bool probe = (blocksSinceProbe_++ % kProbeInterval) == 0;
        if (!probe) {
            status_.outputReady = false;
            return false;
        }
    }

    if (inputQueue_.size() >= kMaxInputQueueBlocks) {
        inputQueue_.pop_front();
        ++status_.droppedInputBlocks;
    }
    inputQueue_.push_back({nextBlockId_++, inputInterleavedStereo, parameters});
    ++status_.submittedBlocks;
    status_.queuedInputBlocks = static_cast<uint32_t>(inputQueue_.size());
    condition_.notify_one();

    if (outputQueue_.size() < targetBufferedBlocks_) {
        const double blockMs = settings.sampleRate > 0.0
            ? (static_cast<double>(frameCount) * 1000.0 / settings.sampleRate)
            : 1.0;
        // Never wait a large fraction of the block, and never wait longer while cold: the old
        // cold-start branch parked the render thread for up to 50 ms waiting for a node that might
        // never answer. A short wait absorbs ordinary network wobble; anything worse must go dry.
        const double waitMs = std::clamp(blockMs * 0.20, 0.25, 1.0);
        const auto waitMicros = static_cast<int64_t>(waitMs * 1000.0);
        condition_.wait_for(lock,
                            std::chrono::microseconds(waitMicros),
                            [&] { return stopRequested_ || outputQueue_.size() >= targetBufferedBlocks_; });
    }

    if (outputQueue_.size() < targetBufferedBlocks_) {
        ++status_.underrunBlocks;
        status_.outputReady = false;
        status_.queuedOutputBlocks = static_cast<uint32_t>(outputQueue_.size());
        return false;
    }

    outputInterleavedStereo = std::move(outputQueue_.front().samples);
    outputQueue_.pop_front();
    status_.outputReady = outputInterleavedStereo.size() == inputInterleavedStereo.size();
    status_.queuedOutputBlocks = static_cast<uint32_t>(outputQueue_.size());
    return status_.outputReady;
}

RemoteDspAsyncStreamStatus RemoteDspAsyncStream::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto copy = status_;
    copy.queuedInputBlocks = static_cast<uint32_t>(inputQueue_.size());
    copy.queuedOutputBlocks = static_cast<uint32_t>(outputQueue_.size());
    copy.running = configured_ && !stopRequested_;
    return copy;
}

void RemoteDspAsyncStream::recordRoundTripLocked(double roundTripMs) {
    if (!std::isfinite(roundTripMs) || roundTripMs <= 0.0) {
        return;
    }
    status_.averageRoundTripMs = status_.averageRoundTripMs <= 0.0
        ? roundTripMs
        : status_.averageRoundTripMs + ((roundTripMs - status_.averageRoundTripMs) * 0.10);
    if (previousRoundTripValid_) {
        const double jitterUs = std::abs(roundTripMs - previousRoundTripMs_) * 1000.0;
        status_.lastRoundTripJitterUs = jitterUs;
        status_.averageRoundTripJitterUs = status_.averageRoundTripJitterUs <= 0.0
            ? jitterUs
            : status_.averageRoundTripJitterUs + ((jitterUs - status_.averageRoundTripJitterUs) * 0.08);
        status_.maxRoundTripJitterUs = std::max(jitterUs, status_.maxRoundTripJitterUs * 0.995);
    }
    previousRoundTripMs_ = roundTripMs;
    previousRoundTripValid_ = true;
}

void RemoteDspAsyncStream::workerLoop() {
    while (true) {
        Block block;
        RemoteDspServerSettings settings;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return stopRequested_ || !inputQueue_.empty(); });
            if (stopRequested_) {
                return;
            }
            block = std::move(inputQueue_.front());
            inputQueue_.pop_front();
            settings = settings_;
            settings.channelCount = 2;
            settings.frameCount = static_cast<uint16_t>(std::max<size_t>(1, frameCount_));
            settings.timeoutMs = std::max(2, settings.timeoutMs);
            status_.queuedInputBlocks = static_cast<uint32_t>(inputQueue_.size());
        }

        std::vector<float> processed;
        const auto result = processSession_.process(settings, block.samples, block.parameters, processed);
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopRequested_) {
            return;
        }
        if (!result.processed || processed.size() != block.samples.size()) {
            ++status_.failedBlocks;
            ++consecutiveFailures_;
            status_.message = "Remote DSP async block failed: " + result.message;
            continue;
        }
        consecutiveFailures_ = 0;
        status_.meterCount = result.meterCount;
        if (result.meterCount > 0) {
            std::memcpy(status_.meters, result.meters, sizeof(status_.meters));
        }
        recordRoundTripLocked(result.roundTripMs);
        if (outputQueue_.size() >= kMaxOutputQueueBlocks) {
            outputQueue_.pop_front();
        }
        outputQueue_.push_back({block.id, std::move(processed)});
        ++status_.processedBlocks;
        status_.queuedOutputBlocks = static_cast<uint32_t>(outputQueue_.size());
        status_.outputReady = outputQueue_.size() >= targetBufferedBlocks_;
        status_.message = "Remote DSP async stream active.";
        condition_.notify_all();
    }
}

} // namespace neuracoust::daw
