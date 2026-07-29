#pragma once

#include "core/DawState.h"
#include "plugins/MonitorDspModules.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct TempoMarkerState {
    double timeSeconds = 0.0;
    double bpm = 120.0;
};

struct TimeSignatureMarkerState {
    double timeSeconds = 0.0;
    int numerator = 4;
    int denominator = 4;
};

// One band of the monitor parametric EQ (0–64, added on demand). `type` is one of
// "peaking" / "low_shelf" / "high_shelf" / "high_pass" / "low_pass" / "notch".
struct MonitorEqBandState {
    bool enabled = true;
    std::string type = "peaking";
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
};

struct ChordEventState {
    std::string id;
    std::string name;
    double timeSeconds = 0.0;
};

struct LyricEventState {
    std::string id;
    std::string text;
    double timeSeconds = 0.0;
};

struct MidiNoteState {
    std::string id;
    int pitch = 60;
    double startBeats = 0.0;
    double durationBeats = 1.0;
    int velocity = 96;
    int channel = 1;
    bool muted = false;
    std::string colorHex;
};

struct MidiControllerEventState {
    std::string id;
    double beat = 0.0;
    int controller = 0;
    int value = 0;
    int channel = 1;
};

struct MidiPitchBendEventState {
    std::string id;
    double beat = 0.0;
    int value = 8192;
    int channel = 1;
};

struct MidiProgramChangeEventState {
    std::string id;
    double beat = 0.0;
    int program = 0;
    int channel = 1;
};

struct MidiRegionState {
    std::string id;
    std::string trackName;
    std::string name = "MIDI Region";
    double startSeconds = 0.0;
    double durationSeconds = 4.0;
    int ticksPerQuarter = 960;
    bool loopEnabled = false;
    bool muted = false;
    bool locked = false;
    std::string colorHex;
    std::vector<MidiNoteState> notes;
    std::vector<MidiControllerEventState> controllerEvents;
    std::vector<MidiPitchBendEventState> pitchBendEvents;
    std::vector<MidiProgramChangeEventState> programChangeEvents;
};

struct MediaSourceState {
    std::string id;
    std::string path;
    std::string displayName;
    int channels = 0;
    double sampleRate = 0.0;
    int bitsPerSample = 0;
    bool floatingPoint = false;
    bool hasBroadcastTimeReference = false;
    uint64_t timeReferenceSamples = 0;
    double timeReferenceSeconds = 0.0;
};

struct ClipDefinitionState {
    std::string id;
    std::string sourceId;
    std::string name;
    double sourceOffsetSeconds = 0.0;
    double durationSeconds = 0.0;
    double sourceTempoBpm = 0.0;
    int sourceTimeSignatureNumerator = 0;
    int sourceTimeSignatureDenominator = 0;
    std::string sourceGrooveFeel;
    double sourceGrooveSwingAmount = 0.0;
};

struct VideoSourceState {
    std::string id;
    std::string path;
    std::string displayName;
    double frameRate = 30.0;
    double durationSeconds = 0.0;
    int width = 0;
    int height = 0;
    bool hasAudio = false;
};

struct VideoClipState {
    std::string id;
    std::string sourceId;
    std::string name;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
    double sourceTimecodeStartSeconds = 0.0;
    bool muted = false;
    bool locked = false;
};

struct PlaylistClipPlacementState {
    std::string id;
    std::string clipDefinitionId;
    double startSeconds = 0.0;
    int layer = 0;
    float gainDb = 0.0f;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    std::string fadeInCurve = "equal_power";
    std::string fadeOutCurve = "equal_power";
    double fadeInCurvature = 0.0;
    double fadeOutCurvature = 0.0;
    bool muted = false;
    bool polarityInverted = false;
    bool reversed = false;
    bool locked = false;
    std::string colorHex;
    double timeScale = 1.0;
    std::string tempoSyncPolicy = "project-tempo";
    bool pendingTimeStretchToProject = false;
    std::string legacyClipId;
    /// The clip's original timeline position (see ClipState.originalStartSeconds).
    /// Carried here too because the playlist model is the render source of truth
    /// and clips are rebuilt from placements. Declared LAST: aggregate
    /// initializers index fields by position.
    double originalStartSeconds = -1.0;
    /// ARA (Melodyne) state, per clip instance — two copies of the same audio can carry different
    /// edits, so this rides the PLACEMENT rather than the shared clip definition.
    /// See ClipState::araArchiveBase64 for what these mean.
    std::string araPluginName;
    std::string araPluginPath;
    std::string araSourcePath;
    std::string araArchiveBase64;
};

struct TrackPlaylistState {
    std::string id;
    std::string trackName;
    std::string name = "Playlist 1";
    bool active = true;
    std::vector<PlaylistClipPlacementState> placements;
};

struct ProjectDocument {
    std::string name = "Untitled";
    double sampleRate = 48000.0;
    int defaultBufferSize = 256;
    int bitDepth = 24;
    int tempoBpm = 120;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    std::string grooveFeel = "straight";
    // Pan law for mono tracks: "-3dB" / "-4.5dB" / "-6dB" constant-power, or "legacy" (the
    // old linear balance). New projects default to constant-power; projects saved before
    // this field existed load as "legacy" so their balance is preserved.
    std::string panLaw = "-4.5dB";
    double grooveSwingAmount = 0.0;
    std::string metronomeSubdivision = "auto";
    // Click level (linear gain over the built-in click, 0..2) and timbre.
    double metronomeGain = 1.0;
    std::string metronomeSound = "beep";   // beep / wood / rim / cowbell
    bool metronomeAccentFirst = true;      // accent the bar's downbeat
    std::string metronomeGenre = "straight";     // named groove preset (UI catalog)
    std::vector<float> metronomeAccentPattern;   // per-step gains over a bar (empty = default)
    // Which continuous controllers a MIDI take captures. Recording everything a keyboard
    // sends buries a part in aftertouch and unused CC lanes, so this is a whitelist:
    // CC numbers here are written into the region, everything else is heard live and
    // dropped. Pitch bend is not a CC and carries its own flag. Defaults are the three a
    // player expects to survive a take — sustain, modulation, bend.
    std::vector<int> midiRecordControllers {1, 64};
    bool midiRecordPitchBend = true;
    std::string detectedKey = "C";
    std::string detectedKeyMode = "major";
    std::string chordKeyModePreference = "auto";
    std::string tempoMasterTrackName;
    std::string audioImportTempoPolicy = "preserve-project";
    double timecodeStartSeconds = 0.0;
    double videoFrameRate = 30.0;
    bool timecodeDropFrame = false;
    bool beatSnapEnabled = false;
    std::string editMode = "Slip";
    std::string gridUnit = "1s";
    std::string playbackStartMode = "Return to Start";
    bool delayCompensationEnabled = true;
    bool directMonitoringEnabled = true;
    double timelineZoomFactor = 1.0;
    int timelineFollowMode = 1;
    double trackHeightScale = 1.0;
    double tempoLaneHeightScale = 0.50;
    double waveformGainScale = 1.0;
    bool loopEnabled = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 4.0;
    double preRollSeconds = 0.0;
    double postRollSeconds = 0.0;
    bool editSelectionEnabled = false;
    double editSelectionStartSeconds = 0.0;
    double editSelectionEndSeconds = 0.0;
    bool appleSiliconCoreIsolationEnabled = true;
    int requestedDspCoreCount = 4;
    // Cores DW asks the external DSP Manager (NuclustDspManager) to reserve. It is a
    // hint: when a connected node reports its own core_count that report wins, so this
    // is the value used before/without a report and the count the DAW requests.
    int externalDspCoreCount = 4;
    // Waves-style server options on the NDS card. The network buffer trades latency for wire
    // resilience exactly like SoundGrid's "Server Network Buffer" (frames; ~2.7 ms at 128/48k).
    // The mixer channel capacity is what the remote-mixer sessions (M1+) size themselves for —
    // 8/16/32/64, the SoundGrid configuration ladder.
    int remoteNetworkBufferFrames = 128;
    int remoteMixerChannels = 32;
    // SoundGrid's two performance modes, split the way the user works: MIXING rides a roomy
    // buffer (DSP optimized), TRACKING runs the wire tight (latency optimized — 40 frames is
    // 0.83 ms at 48k, the Waves floor). "auto" follows the session: any record-armed or
    // input-monitoring track switches the streams to the tracking buffer, the same trigger the
    // monitor EQ already uses for its minimum-phase fallback.
    std::string remoteLatencyMode = "auto";   // auto | mixing | tracking
    int remoteTrackingBufferFrames = 48;
    // The "use this node" master switch — whether the external DSP node participates at all.
    // Off gates it out of the monitor/DAW/plugin core plan regardless of the reserve above.
    // Default on preserves the prior always-available behaviour.
    bool externalDspEnabled = true;
    // The remote DSP node the engine streams monitor audio to (host or IPv4). Empty
    // falls back to "studio.local". This is what makes External/NDS reach a real node
    // instead of the hardcoded default.
    std::string remoteDspHost = "studio.local";
    // The dedicated NDS appliance, addressed separately from the general-purpose node above.
    // They speak the same protocol but are not the same class of machine: the appliance runs its
    // own RT kernel and measures tighter in the tail, which is what decides quality for live work.
    // :20002 is the APPLIANCE engine (user-owned, crontab-kept); the old root service
    // squats 20000 harmlessly until someone with its password retires it.
    std::string ndsHost = "192.168.0.198:20002";
    bool ndsEnabled = false;
    // Which machine handles each job. "internal" (this Mac), "nds", or "external" (node computer).
    // Explicit assignment is the default because a path that changes under you mid-take is worse
    // than one that runs out of headroom predictably.
    std::string dspRoleMonitor = "internal";
    // The strip / master / insert assignments are PER-SLOT now (each plug-in's DSP 실행 위치
    // menu, each channel's DSP chip) — these globals stay only so older projects that saved a
    // machine here keep resolving; the UI no longer edits them.
    std::string dspRoleChannelStrip = "internal";
    std::string dspRoleMaster = "internal";
    std::string dspRoleInserts = "internal";
    // The DAW's own core functions, assignable ahead of the paths existing (재생/녹음/믹서/
    // 트랙·버스). Stored intent: nothing routes by these yet, and the table says so in amber.
    std::string dspRolePlayback = "internal";
    std::string dspRoleRecording = "internal";
    std::string dspRoleMixer = "internal";
    std::string dspRoleBuses = "internal";
    // When set, the assignments above become starting points and work spills to the next machine
    // as the one before it runs short: internal -> NDS -> external node.
    bool dspAutoOverflow = false;
    // The **physical** speaker / headphone the user actually monitors on — a definition
    // of the real hardware, not a simulation. Empty = unspecified. When
    // monitorSpeakerHeadphoneExclusive is set, switching to one output deactivates the
    // other, so only speaker or only headphone monitors at a time.
    std::string physicalSpeakerModel;
    std::string physicalHeadphoneModel;
    // The power amp and speaker cable driving a PASSIVE speaker; ignored for active monitors.
    std::string physicalPowerAmpModel;
    std::string physicalSpeakerCableModel;
    // AC feed and termination definitions. They are catalog/status metadata and intentionally
    // have no tone model: competent power leads/connectors are not audio transfer elements.
    std::string physicalPowerCableModel;
    std::string physicalConnectorModel;
    // The audio interface's D/A output-stage model (catalog/definition only — no audio effect yet).
    std::string physicalAudioInterfaceModel;
    // Purpose 2: render the physical interface AS this different model (A→B transform). Stored
    // intent only — the transform stays inert until BOTH source and target have raw measured
    // profiles (never synthesised from summary specs). Empty = no target (render as itself).
    std::string physicalAudioInterfaceTargetModel;
    // Optional 2단계: apply the measured nonlinear harmonic character (Chebyshev waveshaper) of the
    // modeled interface. Off by default — a clean interface's harmonics are inaudible.
    bool monitorInterfaceModelingEnabled = false;
    // Measurement microphone used for room measurement; drives whether absolute tone correction
    // is trustworthy (mic has a calibration file) or only L/R matching + relative correction.
    std::string measurementMicModel;
    bool monitorSpeakerHeadphoneExclusive = true;
    // Auto fade-out: a fade written into the Master track's volume automation over the
    // last N seconds. 0 = off. Curve is one of linear/equal_power/exponential/logarithmic.
    double autoFadeOutSeconds = 0.0;
    std::string autoFadeOutCurve = "equal_power";
    bool windowsProcessorAffinityEnabled = false;
    std::string windowsProcessorAffinityMode = "p_core_preferred";
    bool monitorStationMono = false;
    std::string monitorStationListenMode = "LR";
    bool monitorStationSwapLeftRight = false;
    bool monitorStationInvertLeft = false;
    bool monitorStationInvertRight = false;
    bool monitorStationMute = false;
    bool monitorStationDim = false;
    bool monitorStationTalkback = false;
    float monitorStationDimDb = -20.0f;
    std::string monitorStationTalkbackRoute = "listen_room";
    int monitorStationTalkbackChannel = 1;   // input channel (1-based) the talkback mic is on
    float monitorInputTrimDb = -9.0f;
    float monitorVolumeDb = -30.0f;
    bool listenRoomEnabled = false;
    std::string listenRoomSessionName = "mix";
    std::string listenRoomSource = "monitor";
    std::string listenRoomQuality = "opus_high";
    std::string listenRoomLatencyMode = "stable";
    std::string listenRoomTransportMode = "direct_fallback";
    std::string listenRoomRelayHost = "127.0.0.1";
    std::string listenRoomAccessToken;
    int listenRoomRelayHttpPort = 8787;
    int listenRoomRelayTcpIngestPort = 8791;
    std::vector<TrackState> tracks;
    std::vector<ClipState> clips;
    std::vector<MediaSourceState> mediaSources;
    std::vector<ClipDefinitionState> clipDefinitions;
    std::vector<VideoSourceState> videoSources;
    std::vector<VideoClipState> videoClips;
    std::vector<TrackPlaylistState> trackPlaylists;
    std::vector<TempoMarkerState> tempoMap;
    std::vector<TimeSignatureMarkerState> timeSignatureMap;
    std::vector<MarkerState> markers;
    std::vector<ChordEventState> chordEvents;
    std::vector<LyricEventState> lyricEvents;
    // Song-form / arrangement sections (Intro, Verse, Chorus…). Per-project so they don't carry
    // across projects; each entry's timeSeconds starts a section that runs until the next.
    std::vector<ChordEventState> songSections;
    std::vector<MidiRegionState> midiRegions;
    std::vector<InsertState> masterInserts;
    std::vector<MonitorDspModule> monitorModules;
    // Monitor parametric EQ — the user-built 0–64 band correction/tone EQ (monitor path only).
    std::vector<MonitorEqBandState> monitorEqBands;
};

struct ProjectMediaCollectReport {
    size_t copiedClips = 0;
    size_t alreadyInProjectClips = 0;
    size_t missingClips = 0;
    size_t failedClips = 0;
    std::vector<std::string> messages;
};

struct ProjectHealthReport {
    size_t clips = 0;
    size_t overlappingClipPairs = 0;
    size_t missingMediaClips = 0;
    size_t masterInserts = 0;
    size_t trackInserts = 0;
    size_t vst3MasterInserts = 0;
    size_t vst3TrackInserts = 0;
    size_t activeVst3TrackInserts = 0;
    std::vector<std::string> activeVst3TrackInsertLabels;
    size_t missingVst3Inserts = 0;
    size_t mutedAudioTracks = 0;
    size_t soloedAudioTracks = 0;
    size_t mutedClips = 0;
    size_t disabledMonitorModules = 0;
    std::vector<std::string> messages;
};

std::string serializeProject(const ProjectDocument& project);
std::string serializeProjectForPath(const ProjectDocument& project, const std::filesystem::path& projectPath);
bool deserializeProject(const std::string& text, ProjectDocument& project, std::string& error);
bool deserializeProjectForPath(const std::string& text,
                               const std::filesystem::path& projectPath,
                               ProjectDocument& project,
                               std::string& error);
std::string copyAudioFileToProjectMedia(const std::filesystem::path& sourcePath,
                                        const std::filesystem::path& projectPath,
                                        std::string& error);
std::string copyVideoFileToProjectMedia(const std::filesystem::path& sourcePath,
                                        const std::filesystem::path& projectPath,
                                        std::string& error);
ProjectMediaCollectReport collectProjectMedia(ProjectDocument& project,
                                              const std::filesystem::path& projectPath);
ProjectHealthReport analyzeProjectHealth(const ProjectDocument& project);
ProjectHealthReport analyzeProjectHealth(const ProjectDocument& project, const std::filesystem::path& projectPath);
std::string summarizeProjectHealth(const ProjectHealthReport& report);
std::string summarizeProjectHealth(const ProjectDocument& project);
std::string summarizeProjectHealth(const ProjectDocument& project, const std::filesystem::path& projectPath);
std::filesystem::path projectBackupPath(const std::filesystem::path& projectPath);
bool backupExistingProjectFile(const std::filesystem::path& projectPath, std::string& error);
bool saveProjectFileWithBackup(const ProjectDocument& project,
                               const std::filesystem::path& projectPath,
                               std::string& error);
std::filesystem::path projectAutosavePath(const std::filesystem::path& projectPath);
bool projectAutosaveIsNewerThanProject(const std::filesystem::path& projectPath);
bool writeProjectAutosaveFile(const ProjectDocument& project, const std::filesystem::path& projectPath, std::string& error);
bool loadProjectAutosaveFile(const std::filesystem::path& projectPath, ProjectDocument& project, std::string& error);
bool removeProjectAutosaveFile(const std::filesystem::path& projectPath, std::string& error);
std::string nextProjectRecordingPath(const std::filesystem::path& projectPath, std::string& error);
std::filesystem::path importedMediaManifestPath(const std::filesystem::path& mediaWavPath);
bool writeImportedMediaManifest(const ProjectDocument& project,
                                const std::filesystem::path& mediaWavPath,
                                const std::string& originalSourcePath,
                                const std::string& clipId,
                                const std::string& trackName,
                                double startSeconds,
                                double durationSeconds,
                                int sourceBitsPerSample,
                                bool sourceFloatingPoint,
                                double sourceSampleRate,
                                int sourceChannels,
                                bool convertedToProjectSampleRate,
                                bool convertedToProjectBitDepth,
                                const std::string& sampleRateImportPolicy,
                                const std::string& bitDepthImportPolicy,
                                double sourceTempoBpm,
                                const std::string& tempoSyncPolicy,
                                bool pendingTimeStretchToProject,
                                std::string& error);
std::filesystem::path recordedTakeManifestPath(const std::filesystem::path& recordedWavPath);
bool writeRecordedTakeManifest(const ProjectDocument& project,
                               const std::filesystem::path& recordedWavPath,
                               const std::string& clipId,
                               const std::string& trackName,
                               double startSeconds,
                               double durationSeconds,
                               const std::string& inputDeviceId,
                               std::string& error);
bool applyDefaultProjectNameFromPath(ProjectDocument& project, const std::filesystem::path& projectPath);
std::filesystem::path normalizedProjectSavePath(const std::filesystem::path& requestedPath);
ProjectDocument defaultProject();
void normalizeProjectRouting(ProjectDocument& project);
void normalizeProjectEditModel(ProjectDocument& project);
void rebuildProjectEditModelFromClips(ProjectDocument& project);
bool rebuildProjectClipsFromActivePlaylists(ProjectDocument& project);

} // namespace neuracoust::daw
