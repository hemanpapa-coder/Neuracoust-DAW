#pragma once

#include "audio/MasterInsertProcessor.h"
#include "core/DawState.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace neuracoust::daw {

// Prepares (and destroys) realtime insert chains OFF the audio thread. Loading a VST3 — or
// spawning the out-of-process worker — takes tens of ms to seconds; doing it in the render
// callback froze the transport and stalled the editor. The audio thread instead REQUESTS a
// chain for a signature and plays dry until the background thread hands back a ready one;
// retired chains are destroyed here too (worker teardown also blocks). All audio-thread entry
// points are try_lock and never block.
class AsyncInsertChainPreparer {
public:
    AsyncInsertChainPreparer();
    ~AsyncInsertChainPreparer();

    // Ask for a chain for `key`. Deduped: a no-op if that key is already ready or in flight.
    // Cheap enough for the audio thread (try_lock; skips on contention, retries next block).
    void request(const std::string& key, const std::vector<InsertState>& inserts,
                 double sampleRate, int maxBlock, const std::vector<std::string>& shmKeys);

    // Take the ready chain for `key`, or nullptr if not prepared yet. Audio-thread safe.
    std::unique_ptr<RealtimeMasterInsertChain> tryTake(const std::string& key);

    // Hand a no-longer-needed chain back for background destruction (worker teardown blocks).
    void retire(std::unique_ptr<RealtimeMasterInsertChain> chain);

    // Last prepare error for a key ("" if none). Cheap; for surfacing a failed load.
    std::string lastError(const std::string& key);

private:
    struct Job {
        std::string key;
        std::vector<InsertState> inserts;
        double sampleRate = 48000.0;
        int maxBlock = 256;
        std::vector<std::string> shmKeys;
    };
    void run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::set<std::string> inFlight_;                                             // queued or preparing
    std::map<std::string, std::unique_ptr<RealtimeMasterInsertChain>> ready_;    // prepared, awaiting pickup
    std::map<std::string, std::string> errors_;
    std::deque<std::unique_ptr<RealtimeMasterInsertChain>> retired_;             // awaiting destruction
    bool stop_ = false;
};

} // namespace neuracoust::daw
