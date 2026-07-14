#pragma once

#include "audio/MixerGraph.h"
#include "audio/MixerProcessorChain.h"
#include "audio/AsyncInsertChainPreparer.h"
#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/ProjectRenderTypes.h"
#include "audio/WavFile.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/ProjectDocument.h"
#include <cstdint>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace neuracoust::daw {

enum class ProjectRenderMidiEventKind {
    Note,
    Controller,
    PitchBend,
    ProgramChange
};

struct ProjectRenderClip {
    ClipState clip;
    WavAudioData source;
    bool missingSource = false;
};

struct ProjectRenderMidiEvent {
    std::string trackName;
    std::string regionId;
    std::string noteId;
    int64_t frameOffset = 0;
    int pitch = 60;
    int velocity = 0;
    int channel = 1;
    bool noteOn = false;
    ProjectRenderMidiEventKind kind = ProjectRenderMidiEventKind::Note;
    int controller = 0;
    int value = 0;
    int program = 0;
};

struct ProjectAudioRenderPlan {
    double sampleRate = 48000.0;
    double tempoBpm = 120.0;
    bool loopEnabled = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
    bool hasActiveVst3Inserts = false;
    bool hasActiveTrackVst3Inserts = false;
    bool renderTrackVst3Inserts = true;
    bool renderMonitorDsp = false;
    float monitorInputTrimDb = -9.0f;
    bool delayCompensationEnabled = true;
    unsigned int delayCompensationSamples = 0;
    std::string panLaw = "legacy";
    bool transportRecordingActive = false;
    bool hasMissingMedia = false;
    std::vector<InsertState> activeVst3Inserts;
    std::vector<std::string> activeTrackVst3InsertLabels;
    std::vector<TrackState> tracks;
    std::vector<ProjectRenderClip> clips;
    std::vector<ProjectExternalSidechainBus> externalSidechainBuses;
    std::vector<MidiRegionState> midiRegions;
    std::vector<TempoMarkerState> tempoMap;
    std::vector<MonitorDspModule> monitorModules;
    std::vector<std::string> missingMediaClipIds;
    MixerGraph mixerGraph;
    std::map<std::string, unsigned int> routeDelayCompensationSamples;
};

struct ProjectAudioBlockMeters {
    std::vector<std::string> trackNames;
    std::vector<float> trackPeakLeft;
    std::vector<float> trackPeakRight;
    std::vector<std::string> trackInsertMeterTrackNames;
    std::vector<int> trackInsertMeterSlotIndices;
    std::vector<float> trackInsertInputPeak;
    std::vector<float> trackInsertOutputPeak;
};

struct ProjectAudioRenderState {
    std::map<std::string, std::deque<MixerStereoFrame>> routeDelayLines;
    MonitorDspProcessor monitorDspProcessor;
    RealtimeMasterInsertChain masterInsertChain;
    int masterInsertChainMaxBlockSize = 0;
    bool monitorDspConfigured = false;
    bool masterInsertChainPrepared = false;
    bool masterInsertProcessingFailed = false;
    std::string masterInsertLastError;
    std::map<std::string, Vst3RealtimeProcessor> instrumentProcessors;
    std::map<std::string, std::string> instrumentProcessorKeys;
    std::map<std::string, int> instrumentProcessorMaxBlock;
    std::map<std::string, std::string> instrumentLastErrors;
    // Chains as unique_ptr so a retired one can be moved off the audio thread for destruction.
    std::map<std::string, std::unique_ptr<RealtimeMasterInsertChain>> routeInsertChains;
    std::map<std::string, std::string> routeInsertChainKeys;
    std::map<std::string, int> routeInsertChainMaxBlock;
    std::map<std::string, std::string> routeInsertLastErrors;
    // Prepares/destroys insert chains off the audio thread; created lazily on first use.
    std::unique_ptr<AsyncInsertChainPreparer> insertPreparer;
    // Declick: ramp a route's output up when its insert set changes (add/remove/reorder), so
    // the dry↔wet jump doesn't click.
    std::map<std::string, std::string> routeInsertSignatures;
    std::map<std::string, int> routeInsertDeclickRemaining;
    std::map<std::string, int> routeInsertDeclickTotal;
    std::map<std::string, double> sourceGeneratorPhases;
    std::map<std::string, std::vector<Vst3MidiEvent>> liveMidiEvents;
    std::vector<float> masterInsertDryFallback;

    void reset();
    void resetForSeek();
};

double projectDurationSeconds(const ProjectDocument& project);
size_t activeVst3MasterInsertCount(const ProjectDocument& project);
size_t activeVst3TrackInsertCount(const ProjectDocument& project);
bool hasActiveVst3MasterInserts(const ProjectDocument& project);
bool hasActiveVst3TrackInserts(const ProjectDocument& project);
bool makeProjectAudioRenderPlan(const ProjectDocument& project, ProjectAudioRenderPlan& plan, std::string& error);
void renderProjectAudioBlock(const ProjectAudioRenderPlan& plan,
                             int64_t startFrame,
                             int64_t frameCount,
                             std::vector<float>& interleavedStereo);
void renderProjectAudioBlockWithMeters(const ProjectAudioRenderPlan& plan,
                                       int64_t startFrame,
                                       int64_t frameCount,
                                       std::vector<float>& interleavedStereo,
                                       ProjectAudioBlockMeters* meters);
void renderProjectAudioBlockWithStateAndMeters(const ProjectAudioRenderPlan& plan,
                                               ProjectAudioRenderState& state,
                                               int64_t startFrame,
                                               int64_t frameCount,
                                               std::vector<float>& interleavedStereo,
                                               ProjectAudioBlockMeters* meters,
                                               bool offline = false,
                                               bool transportRunning = true);
bool renderTrackPreFaderStereoBlock(const ProjectAudioRenderPlan& plan,
                                    const std::string& trackName,
                                    int64_t startFrame,
                                    int64_t frameCount,
                                    std::vector<float>& interleavedStereo);
bool renderExternalSidechainBusStereoBlock(const ProjectAudioRenderPlan& plan,
                                           const std::string& busName,
                                           int64_t startFrame,
                                           int64_t frameCount,
                                           std::vector<float>& interleavedStereo);
bool printRecordedTakeThroughTrackDsp(const ProjectDocument& project,
                                      const std::string& trackName,
                                      const std::string& wavPath,
                                      std::string& error);
std::vector<ProjectRenderMidiEvent> collectMidiEventsForRenderBlock(const ProjectAudioRenderPlan& plan,
                                                                    const std::string& trackName,
                                                                    int64_t startFrame,
                                                                    int64_t frameCount);

} // namespace neuracoust::daw
