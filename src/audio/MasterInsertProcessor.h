#pragma once

#include "audio/WavFile.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/ProjectDocument.h"
#include <string>
#include <vector>

namespace neuracoust::daw {

struct ProjectAudioRenderPlan;

bool isVst3MasterInsert(const InsertState& insert);
bool applyProjectMasterInsertsToStereoMix(const ProjectDocument& project,
                                          WavAudioData& mix,
                                          std::string& error,
                                          bool applyMonitorDsp = true,
                                          bool applyVst3Inserts = true);

class RealtimeMasterInsertChain {
public:
    struct InsertProcessMeter {
        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        bool processed = false;
        bool bypassed = false;
    };

    bool prepare(const ProjectAudioRenderPlan& plan,
                 double sampleRate,
                 int maxBlockSize,
                 std::string& error,
                 const std::vector<std::string>& bridgeShmKeys = {});
    void reset();
    bool isPrepared() const;
    // True when every active (non-bypassed) insert produced its real output on the last block — i.e.
    // the whole chain is engaged, not still passing dry while an out-of-process worker warms up.
    bool producedWetLastBlock() const;
    size_t activeVst3Count() const;
    unsigned int totalLatencySamples() const;
    bool updateParameter(size_t processorIndex,
                         uint32_t parameterId,
                         const std::string& displayName,
                         double normalizedValue);
    void setBypassStates(const std::vector<bool>& bypassStates);
    unsigned int activeLatencySamples() const;
    bool processInterleavedStereo(std::vector<float>& interleavedStereo,
                                  int frameCount,
                                  std::string& error);
    const std::vector<InsertProcessMeter>& lastProcessMeters() const;
    std::vector<std::vector<Vst3ParameterValueState>> drainOutputParameterChanges();

private:
    std::vector<Vst3RealtimeProcessor> vst3Processors_;
    std::vector<std::vector<Vst3ParameterValueState>> vst3ParameterStates_;
    std::vector<std::vector<Vst3ParameterValueState>> vst3PendingParameterChanges_;
    std::vector<bool> vst3BypassStates_;
    std::vector<bool> vst3ResendParameterStates_;
    std::vector<Vst3ParameterValueState> emptyParameterStates_;
    std::vector<Vst3ParameterValueState> pendingParameterScratch_;
    std::vector<InsertProcessMeter> lastProcessMeters_;
    int maxBlockSize_ = 256;
};

} // namespace neuracoust::daw
