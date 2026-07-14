#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/ProjectAudioRenderer.h"
#include "plugins/Vst3ModuleRuntime.h"
#include "plugins/Vst3SdkAdapter.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace neuracoust::daw {

namespace {

constexpr double kMaxVst3TailSeconds = 30.0;

bool insertNeedsPersistentParameterQueue(const InsertState& insert) {
    const std::string identity = insert.pluginName + " " +
        insert.pluginPath + " " +
        insert.pluginClassName;
    std::string lower;
    lower.reserve(identity.size());
    for (const unsigned char ch : identity) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lower.find("waves") != std::string::npos ||
        lower.find("waveshell") != std::string::npos;
}

Vst3PluginDescriptor descriptorForInsert(const InsertState& insert) {
    return resolveVst3PluginDescriptorForInsert(insert.pluginName,
                                               insert.pluginPath,
                                               insert.pluginClassId,
                                               insert.pluginClassName);
}

void appendSilenceFrames(WavAudioData& audio, int64_t frames) {
    if (frames <= 0 || audio.channels <= 0) {
        return;
    }
    audio.interleavedSamples.resize(
        audio.interleavedSamples.size() + static_cast<size_t>(frames * audio.channels),
        0.0f);
}

int64_t boundedTailFrames(unsigned int tailSamples, int sampleRate) {
    if (tailSamples == 0 || sampleRate <= 0) {
        return 0;
    }
    const int64_t maxFrames = static_cast<int64_t>(kMaxVst3TailSeconds * sampleRate);
    return std::min<int64_t>(static_cast<int64_t>(tailSamples), maxFrames);
}

float interleavedStereoPeak(const std::vector<float>& interleavedStereo, int frameCount) {
    const size_t sampleCount = std::min(interleavedStereo.size(), static_cast<size_t>(std::max(0, frameCount)) * 2u);
    float peak = 0.0f;
    for (size_t index = 0; index < sampleCount; ++index) {
        peak = std::max(peak, std::abs(interleavedStereo[index]));
    }
    return peak;
}

} // namespace

bool isVst3MasterInsert(const InsertState& insert) {
    return insert.pluginFormat == "VST3" || insert.pluginFormat == "VST3/AU";
}

bool applyProjectMasterInsertsToStereoMix(const ProjectDocument& project,
                                          WavAudioData& mix,
                                          std::string& error,
                                          bool applyMonitorDsp,
                                          bool applyVst3Inserts) {
    error.clear();
    bool monitorDspConfigured = false;
    bool monitorDspApplied = false;
    for (const auto& insert : project.masterInserts) {
        if (insert.pluginAppId == "neuracoust-monitor-dsp") {
            monitorDspConfigured = true;
        }
        if (insert.bypassed) {
            continue;
        }
        if (insert.pluginAppId == "neuracoust-monitor-dsp") {
            if (applyMonitorDsp) {
                applyMonitorDspToInterleavedStereo(mix, project.monitorModules);
                monitorDspApplied = true;
            }
            continue;
        }
        if (applyVst3Inserts && isVst3MasterInsert(insert) && !insert.pluginPath.empty()) {
            auto descriptor = descriptorForInsert(insert);
            const auto probe = probeVst3ProcessorWithSdk(descriptor, mix.sampleRate);
            appendSilenceFrames(mix, boundedTailFrames(probe.tailSamples, mix.sampleRate));
            auto result = processStereoBufferWithVst3(descriptor, mix, 256, insert.parameters);
            if (!result.processed) {
                const auto worker = defaultVst3ProcessWorkerPath();
                if (!worker.empty()) {
                    auto isolated = processStereoBufferWithIsolatedVst3(worker, descriptor, mix, 256, 20);
                    if (isolated.processed) {
                        continue;
                    }
                    error = "VST3 insert failed: " + result.message + "; isolated worker failed: " + isolated.message;
                    return false;
                }
                error = "VST3 insert failed: " + result.message;
                return false;
            }
        }
    }

    if (applyMonitorDsp && !monitorDspConfigured && !monitorDspApplied) {
        applyMonitorDspToInterleavedStereo(mix, project.monitorModules);
    }
    return true;
}

bool RealtimeMasterInsertChain::prepare(const ProjectAudioRenderPlan& plan,
                                        double sampleRate,
                                        int maxBlockSize,
                                        std::string& error,
                                        const std::vector<std::string>& bridgeShmKeys) {
    reset();
    error.clear();
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 256;
    for (size_t insertIndex = 0; insertIndex < plan.activeVst3Inserts.size(); ++insertIndex) {
        const auto& insert = plan.activeVst3Inserts[insertIndex];
        if (insert.pluginPath.empty()) {
            error = "VST3 insert path is missing: " + insert.pluginName;
            reset();
            return false;
        }
        auto descriptor = descriptorForInsert(insert);
        Vst3RealtimeProcessor processor;
        std::string message;
        (void)bridgeShmKeys;   // out-of-process worker path retired for inserts (see below)
        // Host inserts IN-PROCESS (the path instruments already use successfully). The
        // "internal" / isolated-core out-of-process worker was gapping the audio: spawning
        // and tearing it down happens on the realtime chain-rebuild, so adding or removing
        // an insert during playback froze the playhead until the worker settled — and when
        // the worker failed to come up at all, every insert silently passed through dry.
        // In-process prepare is fast and reliable; the isolated-core path returns once its
        // teardown is off the audio thread.
        if (!processor.prepare(descriptor, sampleRate, maxBlockSize_, message, std::string{}, false)) {
            error = "VST3 realtime insert failed: " + insert.pluginName + ": " + message;
            reset();
            return false;
        }
        vst3Processors_.push_back(std::move(processor));
        vst3ParameterStates_.push_back(insert.parameters);
        vst3PendingParameterChanges_.push_back(insert.parameters);
        vst3BypassStates_.push_back(insert.bypassed || !insert.available);
        vst3ResendParameterStates_.push_back(insertNeedsPersistentParameterQueue(insert));
        lastProcessMeters_.push_back({});
    }
    return true;
}

void RealtimeMasterInsertChain::reset() {
    for (auto& processor : vst3Processors_) {
        processor.reset();
    }
    vst3Processors_.clear();
    vst3ParameterStates_.clear();
    vst3PendingParameterChanges_.clear();
    vst3BypassStates_.clear();
    vst3ResendParameterStates_.clear();
    pendingParameterScratch_.clear();
    lastProcessMeters_.clear();
}

bool RealtimeMasterInsertChain::isPrepared() const {
    return !vst3Processors_.empty();
}

size_t RealtimeMasterInsertChain::activeVst3Count() const {
    return vst3Processors_.size();
}

unsigned int RealtimeMasterInsertChain::totalLatencySamples() const {
    unsigned int total = 0;
    for (const auto& processor : vst3Processors_) {
        total += processor.probe().latencySamples;
    }
    return total;
}

bool RealtimeMasterInsertChain::updateParameter(size_t processorIndex,
                                                uint32_t parameterId,
                                                const std::string& displayName,
                                                double normalizedValue) {
    if (processorIndex >= vst3ParameterStates_.size()) {
        return false;
    }
    if (vst3PendingParameterChanges_.size() < vst3ParameterStates_.size()) {
        vst3PendingParameterChanges_.resize(vst3ParameterStates_.size());
    }
    auto& parameters = vst3ParameterStates_[processorIndex];
    const float value = static_cast<float>(std::clamp(normalizedValue, 0.0, 1.0));
    auto found = std::find_if(parameters.begin(), parameters.end(), [&](const Vst3ParameterValueState& parameter) {
        return parameter.parameterId == parameterId;
    });
    if (found == parameters.end()) {
        parameters.push_back({parameterId,
                              displayName.empty() ? "Param " + std::to_string(parameterId) : displayName,
                              value});
    } else {
        const bool valueUnchanged = std::abs(found->normalizedValue - value) <= 0.0000001f;
        if (!displayName.empty()) {
            found->displayName = displayName;
        } else if (found->displayName.empty()) {
            found->displayName = "Param " + std::to_string(parameterId);
        }
        if (valueUnchanged) {
            return false;
        }
        found->normalizedValue = value;
    }
    auto& pendingChanges = vst3PendingParameterChanges_[processorIndex];
    auto pendingFound = std::find_if(pendingChanges.begin(), pendingChanges.end(), [&](const Vst3ParameterValueState& parameter) {
        return parameter.parameterId == parameterId;
    });
    if (pendingFound == pendingChanges.end()) {
        pendingChanges.push_back({parameterId,
                                  displayName.empty() ? "Param " + std::to_string(parameterId) : displayName,
                                  value});
    } else {
        if (!displayName.empty()) {
            pendingFound->displayName = displayName;
        } else if (pendingFound->displayName.empty()) {
            pendingFound->displayName = "Param " + std::to_string(parameterId);
        }
        pendingFound->normalizedValue = value;
    }
    return true;
}

void RealtimeMasterInsertChain::setBypassStates(const std::vector<bool>& bypassStates) {
    vst3BypassStates_.resize(vst3Processors_.size(), false);
    for (size_t index = 0; index < vst3BypassStates_.size() && index < bypassStates.size(); ++index) {
        vst3BypassStates_[index] = bypassStates[index];
    }
}

unsigned int RealtimeMasterInsertChain::activeLatencySamples() const {
    unsigned int total = 0;
    for (size_t index = 0; index < vst3Processors_.size(); ++index) {
        if (index < vst3BypassStates_.size() && vst3BypassStates_[index]) {
            continue;
        }
        total += vst3Processors_[index].probe().latencySamples;
    }
    return total;
}

bool RealtimeMasterInsertChain::processInterleavedStereo(std::vector<float>& interleavedStereo,
                                                         int frameCount,
                                                         std::string& error) {
    error.clear();
    if (vst3Processors_.empty() || frameCount <= 0) {
        return true;
    }
    if (interleavedStereo.size() < static_cast<size_t>(frameCount) * 2) {
        error = "Realtime VST3 block is smaller than the requested frame count.";
        return false;
    }
    const int blockSize = maxBlockSize_ > 0 ? maxBlockSize_ : 256;
    if (lastProcessMeters_.size() != vst3Processors_.size()) {
        lastProcessMeters_.assign(vst3Processors_.size(), {});
    } else {
        for (auto& meter : lastProcessMeters_) {
            meter = {};
        }
    }
    for (size_t processorIndex = 0; processorIndex < vst3Processors_.size(); ++processorIndex) {
        const float inputPeak = interleavedStereoPeak(interleavedStereo, frameCount);
        if (processorIndex < vst3BypassStates_.size() && vst3BypassStates_[processorIndex]) {
            lastProcessMeters_[processorIndex].inputPeak = inputPeak;
            lastProcessMeters_[processorIndex].outputPeak = inputPeak;
            lastProcessMeters_[processorIndex].processed = false;
            lastProcessMeters_[processorIndex].bypassed = true;
            continue;
        }
        auto& processor = vst3Processors_[processorIndex];
        const bool resendParameterState = processorIndex < vst3ResendParameterStates_.size() &&
            vst3ResendParameterStates_[processorIndex];
        pendingParameterScratch_.clear();
        if (processorIndex < vst3PendingParameterChanges_.size()) {
            pendingParameterScratch_.swap(vst3PendingParameterChanges_[processorIndex]);
        }
        bool sentPendingChanges = false;
        for (int frameStart = 0; frameStart < frameCount; frameStart += blockSize) {
            const int framesThisBlock = std::min(blockSize, frameCount - frameStart);
            std::string message;
            const auto& parameters = resendParameterState && processorIndex < vst3ParameterStates_.size()
                ? vst3ParameterStates_[processorIndex]
                : (!sentPendingChanges ? pendingParameterScratch_ : emptyParameterStates_);
            auto result = processor.processInterleavedStereo(
                interleavedStereo.data() + static_cast<size_t>(frameStart) * 2,
                framesThisBlock,
                parameters,
                message);
            if (!result.processed) {
                if (processorIndex < vst3PendingParameterChanges_.size() && !pendingParameterScratch_.empty()) {
                    vst3PendingParameterChanges_[processorIndex].insert(vst3PendingParameterChanges_[processorIndex].end(),
                                                                        pendingParameterScratch_.begin(),
                                                                        pendingParameterScratch_.end());
                }
                error = message;
                return false;
            }
            sentPendingChanges = true;
        }
        lastProcessMeters_[processorIndex].inputPeak = inputPeak;
        lastProcessMeters_[processorIndex].outputPeak = interleavedStereoPeak(interleavedStereo, frameCount);
        lastProcessMeters_[processorIndex].processed = true;
        lastProcessMeters_[processorIndex].bypassed = false;
    }
    return true;
}

const std::vector<RealtimeMasterInsertChain::InsertProcessMeter>&
RealtimeMasterInsertChain::lastProcessMeters() const {
    return lastProcessMeters_;
}

std::vector<std::vector<Vst3ParameterValueState>> RealtimeMasterInsertChain::drainOutputParameterChanges() {
    std::vector<std::vector<Vst3ParameterValueState>> changes;
    changes.reserve(vst3Processors_.size());
    for (auto& processor : vst3Processors_) {
        changes.push_back(processor.drainOutputParameterChanges());
    }
    return changes;
}

} // namespace neuracoust::daw
