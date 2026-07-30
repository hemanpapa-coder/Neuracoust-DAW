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
    /// The plug-in's own component state, base64. A workstation instrument keeps its
    /// program/patch selection here and NOT in its parameters — KORG TRITON exposes
    /// 2,573 parameters and not one of them selects the program, so mirroring
    /// parameters alone loses the patch the moment the editor closes. Empty until an
    /// editor hands one over.
    std::string pluginStateBase64;
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

/// Built-in console channel processing. `model` is deliberately data, rather
/// than an enum, so additional Neuracoust console models can be registered
/// without changing the project format. The first implementation is 4000E.
struct ConsoleChannelState {
    std::string model = "4000e";
    std::string moduleOrder = "filter,eq,gate,comp,saturator";
    bool filterEnabled = false;
    bool eqEnabled = false;
    bool compEnabled = false;
    bool gateEnabled = false;
    bool saturatorEnabled = false;
    /// Linked stereo when false; independent L/R dynamics detectors when true. Per-module so each
    /// SSL block toggles independently (only comp/gate act on it — the detector linking; eq/filter/
    /// saturator already run independent L/R paths, so their flag is stored but tonally inert today).
    /// `dualMono` is the legacy shared field, kept only to migrate old projects into the per-module set.
    bool dualMono = false;
    bool filterDualMono = false;
    bool eqDualMono = false;
    bool compDualMono = false;
    bool gateDualMono = false;
    bool saturatorDualMono = false;
    bool filterCircuitMode = false;
    bool eqCircuitMode = false;
    bool compCircuitMode = false;
    bool gateCircuitMode = false;
    bool saturatorCircuitMode = false;
    float saturatorDriveDb = 6.0f;
    float saturatorMix = 1.0f;
    bool expanderMode = true;
    bool highPassEnabled = false;
    bool lowPassEnabled = false;
    float highPassHz = 20.0f;
    float lowPassHz = 12000.0f;
    float compThresholdDb = -18.0f;
    float compRatio = 4.0f;
    float compAttackMs = 30.0f;
    float compReleaseMs = 360.0f;
    float compMix = 1.0f;
    // 500-series style output stage: MAKE-UP is plain post-comp gain; CEILING is the API 525A's
    // coupled control — it lowers the effective threshold AND raises the make-up by the same dB,
    // so more compression never moves the output ceiling. 0/0 = inert for every other model.
    float compMakeupDb = 0.0f;
    float compCeilingDb = 0.0f;
    bool compFastAttack = false;
    bool compPeakMode = false;
    std::string compType = "ssl";
    float gateThresholdDb = -36.0f;
    float gateRangeDb = 20.0f;
    float gateAttackMs = 1.0f;
    float gateHoldMs = 0.0f;
    float gateReleaseMs = 360.0f;
    bool gateFastAttack = false;
    std::string gateType = "ssl";
    float eqHfGainDb = 0.0f;
    float eqHfHz = 8000.0f;
    bool eqHfBell = false;
    float eqHmfGainDb = 0.0f;
    float eqHmfHz = 3000.0f;
    float eqHmfQ = 1.0f;
    float eqLmfGainDb = 0.0f;
    float eqLmfHz = 1000.0f;
    float eqLmfQ = 1.0f;
    float eqLfGainDb = 0.0f;
    float eqLfHz = 200.0f;
    bool eqLfBell = false;
    bool eqEMode = true;
    std::string eqType = "ssl_4000e";
    /// Channel polarity (Ø), per side — invert L and/or R independently at the end of the console
    /// chain, surfaced as ØL/ØR on the saturator panel. `phaseInvert` is the legacy both-channels
    /// field, kept only to migrate old projects into the per-side pair.
    bool phaseInvert = false;
    bool phaseInvertL = false;
    bool phaseInvertR = false;
    /// Analog-console channel variation, digitally reproduced: each strip gets tiny, deterministic
    /// offsets (from the seed) to EQ frequency, saturation harmonic, comp/gate timing and output
    /// trim, so 512 channels are not bit-identical the way a pure digital console is. `seed` is
    /// normally the channel index (auto), user-overridable (manual); `depth` 0 = off (matched).
    int channelBiasSeed = 0;
    float channelBiasDepth = 0.0f;
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
    ConsoleChannelState consoleChannel;
    /// Which machine runs THIS channel's console strip: empty follows the project's 채널 스트립
    /// assignment, otherwise "internal" | "nds" | "external". Per-channel because a session is
    /// rarely uniform — a couple of heavy strips can go to the appliance while the rest stay home.
    std::string consoleDspMachine;
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
    /// ARA (Melodyne) edit state for this clip, and the audio those edits were made against.
    ///
    /// `araSourcePath` is the UNEDITED window written out when the ARA session first opened; once
    /// an edit is committed, `sourcePath` points at the rendered result, so re-opening the editor
    /// has to go back to this file or the archive would be applied a second time.
    /// `araArchiveBase64` is opaque plug-in state — see AraDocumentController::storeArchive.
    std::string araPluginName;
    std::string araPluginPath;
    std::string araSourcePath;
    std::string araArchiveBase64;

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

/// Resolve a per-item machine override against the project-wide assignment for that job.
/// Empty means "follow the assignment"; anything else is this one item's own answer.
inline std::string effectiveDspMachine(const std::string& itemOverride,
                                       const std::string& projectRole) {
    if (itemOverride == "internal" || itemOverride == "nds" || itemOverride == "external") {
        return itemOverride;
    }
    return projectRole.empty() ? std::string("internal") : projectRole;
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
