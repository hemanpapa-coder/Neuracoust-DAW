#include "audio/AsyncInsertChainPreparer.h"

#include "audio/ProjectAudioRenderer.h"   // ProjectAudioRenderPlan

namespace neuracoust::daw {

AsyncInsertChainPreparer::AsyncInsertChainPreparer() {
    thread_ = std::thread([this] { run(); });
}

AsyncInsertChainPreparer::~AsyncInsertChainPreparer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void AsyncInsertChainPreparer::request(const std::string& key, const std::vector<InsertState>& inserts,
                                       double sampleRate, int maxBlock,
                                       const std::vector<std::string>& shmKeys) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;   // busy — retry next block
    if (ready_.count(key) || inFlight_.count(key)) return;
    inFlight_.insert(key);
    queue_.push_back(Job{key, inserts, sampleRate, std::max(1, maxBlock), shmKeys});
    lock.unlock();
    cv_.notify_one();
}

std::unique_ptr<RealtimeMasterInsertChain> AsyncInsertChainPreparer::tryTake(const std::string& key) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return nullptr;
    auto it = ready_.find(key);
    if (it == ready_.end()) return nullptr;
    auto chain = std::move(it->second);
    ready_.erase(it);
    return chain;
}

void AsyncInsertChainPreparer::retire(std::unique_ptr<RealtimeMasterInsertChain> chain) {
    if (chain == nullptr) return;
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            retired_.push_back(std::move(chain));
            lock.unlock();
            cv_.notify_one();
            return;
        }
    }
    // Contended: don't block the audio thread — destroy inline as a last resort. Rare, and
    // only when the background thread is mid-prepare.
    chain.reset();
}

void AsyncInsertChainPreparer::discard(const std::string& key) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;   // busy — the caller re-runs every block, so it'll land soon
    auto readyIt = ready_.find(key);
    if (readyIt != ready_.end()) {
        retired_.push_back(std::move(readyIt->second));   // destroy its worker off-thread
        ready_.erase(readyIt);
    }
    inFlight_.erase(key);   // a prepare still running for it will be dropped in run() (not in inFlight_)
    errors_.erase(key);
    lock.unlock();
    cv_.notify_one();
}

std::string AsyncInsertChainPreparer::lastError(const std::string& key) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    const auto it = errors_.find(key);
    return it != errors_.end() ? it->second : std::string{};
}

void AsyncInsertChainPreparer::run() {
    for (;;) {
        Job job;
        bool haveJob = false;
        std::deque<std::unique_ptr<RealtimeMasterInsertChain>> toDestroy;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty() || !retired_.empty(); });
            if (stop_ && queue_.empty() && retired_.empty()) return;
            toDestroy.swap(retired_);
            if (!queue_.empty()) {
                job = std::move(queue_.front());
                queue_.pop_front();
                haveJob = true;
            }
        }
        toDestroy.clear();   // destroy retired chains outside the lock (worker teardown blocks)
        if (!haveJob) continue;

        // Prepare the requested chain outside the lock.
        ProjectAudioRenderPlan plan;
        plan.sampleRate = job.sampleRate;
        plan.hasActiveVst3Inserts = true;
        plan.activeVst3Inserts = job.inserts;
        auto chain = std::make_unique<RealtimeMasterInsertChain>();
        std::string error;
        const bool ok = chain->prepare(plan, job.sampleRate, job.maxBlock, error, job.shmKeys);
        if (ok) {
            // Warm the process path off the audio thread: the FIRST process() otherwise pays the
            // out-of-process worker's lazy first-block cost — a shared-memory round-trip stall that
            // lands as a click the instant the insert engages. Pushing a few silent blocks through
            // here (still off-thread, safe to block) means the audio thread's first real block is
            // already warm. Best-effort — a warm-up failure doesn't fail the chain.
            std::vector<float> silence(static_cast<size_t>(job.maxBlock) * 2u, 0.0f);
            std::string warmError;
            for (int i = 0; i < 8; ++i) {
                std::fill(silence.begin(), silence.end(), 0.0f);
                chain->processInterleavedStereo(silence, job.maxBlock, warmError);
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        // If the key was discarded while we were preparing (user moved past this signature), it is
        // no longer in inFlight_ — drop the freshly-built chain for background destruction instead
        // of stashing it in ready_ where nothing would ever collect it (worker leak).
        if (inFlight_.erase(job.key) == 0) {
            if (chain != nullptr) retired_.push_back(std::move(chain));
            continue;
        }
        if (ok) {
            ready_[job.key] = std::move(chain);
            errors_.erase(job.key);
        } else {
            errors_[job.key] = error;
        }
    }
}

} // namespace neuracoust::daw
