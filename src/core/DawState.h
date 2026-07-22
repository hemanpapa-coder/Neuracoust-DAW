#pragma once

#include "audio/AudioDeviceModel.h"
#include "audio/RemoteDspServerClient.h"
#include "core/AppIdentity.h"
#include "license/LicenseAgentClient.h"
#include "plugins/Vst3HostFoundation.h"
#include <cstdint>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct Vst3ParameterValueState {
    uint32_t parameterId = 0;
    std::string displayName;
    double normalizedValue = 0.0;
};

struct TrackInsertSlot {
    std::string pluginName = "No Insert";
    std::string pluginFormat = "None";
    std::string pluginPath;
    bool bypassed = false;
    bool enabled = true;
    std::string dspExecutionMode = "native";
    std::string assignedDspServerId;
    std::string serverModuleId;
    unsigned int reportedLatencySamples = 0;
    bool dspAvailable = true;
    std::string dspLastError;
    std::vector<Vst3ParameterValueState> parameters;
    std::string pluginClassId;
    std::string pluginClassName;
};

struct InstrumentSlotState {
    std::string pluginName = "No Instrument";
    std::string pluginFormat = "None";
    std::string pluginPath;
    bool bypassed = false;   // per-layer mute
    bool soloed = false;     // per-layer solo (transient; when any layer soloes, only soloed layers sound)
    bool enabled = false;
    std::string midiInput = "MIDI Input";
    int midiChannel = 0;
    unsigned int reportedLatencySamples = 0;
    std::vector<Vst3ParameterValueState> parameters;
    std::string pluginClassId;
    std::string pluginClassName;
};

struct TrackSendState {
    std::string busName;
    float gainDb = -12.0f;
    float pan = 0.0f;
    bool enabled = true;
    bool preFader = false;
    bool stereo = true;
};

struct AutomationPointState {
    double timeSeconds = 0.0;
    float value = 0.0f;
};

struct AutomationLaneState {
    std::string parameterId;
    std::string displayName;
    std::vector<AutomationPointState> points;
};

struct TrackState {
    std::string name;
    float volumeDb = 0.0f;
    float pan = 0.0f;
    double displayHeightScale = 1.0;
    bool muted = false;
    bool solo = false;
    bool recordArmed = false;
    bool inputMonitoring = false;
    std::string channelFormat = "stereo";
    std::string inputBus = "Input 1";
    std::string outputBus = "Master";
    std::vector<TrackInsertSlot> inserts;
    InstrumentSlotState instrument;
    std::vector<InstrumentSlotState> instrumentSlots;
    std::string instrumentRackMode = "parallel";
    std::vector<TrackSendState> sends;
    std::string colorHex;
    std::vector<AutomationPointState> volumeAutomation;
    std::vector<AutomationLaneState> automationLanes;
    std::string trackType = "audio";
    std::string folderName;
    bool folderCollapsed = false;
    bool mixerHidden = false;
    int mixerOrder = 0;
    std::string automationMode = "read";
    std::string trackViewMode = "waveform";
    std::string timebaseMode = "samples";
    std::string elasticAudioMode = "none";
    std::string mixGroupName;
    std::string controlMasterTrackName;
    std::string notes;   // free-text channel memo, shown in the mixer; no audio effect
};

struct ClipState {
    std::string id;
    std::string trackName;
    std::string sourcePath;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
    float gainDb = 0.0f;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    // Crossfade amounts DERIVED from same-track overlap (not user-set, not serialized). The render
    // recomputes them from the current overlaps every plan build, so pulling clips apart removes the
    // crossfade automatically — like Pro Tools / Cubase / Logic. Manual fadeIn/Out stay separate.
    double crossfadeInSeconds = 0.0;
    double crossfadeOutSeconds = 0.0;
    bool muted = false;
    bool polarityInverted = false;
    // Non-destructive clip edits the renderer honors: reversed reads the clip's source window
    // back-to-front (like Logic/Cubase "Reverse"), no new file. Mute/polarity already above.
    bool reversed = false;
    std::string colorHex;
    std::string regionName;
    std::string sourceFileUid;
    int sourceChannels = 0;
    double sourceSampleRate = 0.0;
    int sourceBitsPerSample = 0;
    bool sourceFloatingPoint = false;
    bool sourceHasBroadcastTimeReference = false;
    uint64_t sourceTimeReferenceSamples = 0;
    double sourceTimeReferenceSeconds = 0.0;
    double sourceTempoBpm = 0.0;
    int sourceTimeSignatureNumerator = 0;
    int sourceTimeSignatureDenominator = 0;
    std::string sourceGrooveFeel;
    double sourceGrooveSwingAmount = 0.0;
    double timeScale = 1.0;
    std::string tempoSyncPolicy = "project-tempo";
    bool pendingTimeStretchToProject = false;
    bool locked = false;
    std::string fadeInCurve = "equal_power";
    std::string fadeOutCurve = "equal_power";
    // Continuous shape bend on top of the named curve, [-1, 1], 0 = the named curve unchanged. The
    // fade editor's middle handle drives it (Pro Tools fade tension); the render warps the fade
    // position by it so the drawn shape and the sound stay identical.
    double fadeInCurvature = 0.0;
    double fadeOutCurvature = 0.0;
    /// Where this clip FIRST landed on the timeline (import), the Pro-Tools
    /// "original time stamp". Moves and trims never touch it; splitting offsets
    /// the right half so spotting both re-forms the original layout.
    /// < 0 = unknown (clips from projects saved before the field existed).
    /// Declared LAST: aggregate initializers index fields by position.
    double originalStartSeconds = -1.0;
};

struct MarkerState {
    std::string id;
    std::string name;
    double timeSeconds = 0.0;
    std::string memoryType = "marker";
    double selectionStartSeconds = 0.0;
    double selectionEndSeconds = 0.0;
    std::string referenceMode = "absolute";
    bool recallZoom = false;
    bool recallPrePostRoll = false;
    bool recallTrackVisibility = false;
    bool recallTrackHeights = false;
    bool recallGroups = false;
    bool recallWindowConfiguration = false;
    double storedTimelineZoomFactor = 1.0;
    double storedTrackHeightScale = 1.0;
    double storedPreRollSeconds = 0.0;
    double storedPostRollSeconds = 0.0;
    std::string windowConfigurationName;
    std::string comment;
};

struct InsertState {
    std::string pluginName;
    std::string pluginAppId;
    std::string pluginFormat;
    std::string pluginPath;
    bool bypassed = false;
    bool available = false;
    std::string dspExecutionMode = "native";
    std::string assignedDspServerId;
    std::string serverModuleId;
    unsigned int reportedLatencySamples = 0;
    bool dspAvailable = true;
    std::string dspLastError;
    std::vector<Vst3ParameterValueState> parameters;
    std::string pluginClassId;
    std::string pluginClassName;
};

inline bool isRemoteInternalDspExecutionMode(const std::string& mode) {
    return mode == "remote_internal" || mode == "external";
}

inline bool isAnyInternalDspExecutionMode(const std::string& mode) {
    return mode == "internal" || isRemoteInternalDspExecutionMode(mode);
}

struct CoreAffinityOptions {
    bool enabled = false;
    int requestedPerformanceCores = 0;
    std::string mode = "balanced";
};

struct DawState {
    AppIdentity identity;
    LicenseStatus license;
    std::vector<AudioDeviceInfo> devices;
    std::vector<TrackState> tracks;
    std::vector<ClipState> clips;
    std::vector<MarkerState> markers;
    std::vector<InsertState> masterInserts;
    std::vector<Vst3PluginDescriptor> vst3Plugins;
    CoreAffinityOptions coreAffinity;
    RemoteDspServerSettings remoteDspServer;
    bool monitorDspEnabled = true;
    std::string monitorDspPathMode = "internal";
    bool externalDspEnabled = false;
    bool ndsDspEnabled = false;
};

DawState makeInitialDawState();

} // namespace neuracoust::daw
