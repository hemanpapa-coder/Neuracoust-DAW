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
    bool muted = false;
    bool polarityInverted = false;
    bool locked = false;
    std::string colorHex;
    double timeScale = 1.0;
    std::string tempoSyncPolicy = "project-tempo";
    bool pendingTimeStretchToProject = false;
    std::string legacyClipId;
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
    double grooveSwingAmount = 0.0;
    std::string metronomeSubdivision = "auto";
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
    float monitorInputTrimDb = -9.0f;
    float monitorVolumeDb = -6.0f;
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
    std::vector<MidiRegionState> midiRegions;
    std::vector<InsertState> masterInserts;
    std::vector<MonitorDspModule> monitorModules;
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
