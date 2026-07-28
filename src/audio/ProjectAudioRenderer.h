#pragma once

#include "audio/MixerGraph.h"
#include "audio/MixerProcessorChain.h"
#include "audio/ConsoleChannelProcessor.h"
#include "audio/AsyncInsertChainPreparer.h"
#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/ProjectRenderTypes.h"
#include "audio/TestSignalGenerator.h"
#include "audio/WavFile.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/ProjectDocument.h"
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
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
    double preRollSeconds = 0.0;
    double postRollSeconds = 0.0;
    bool hasActiveVst3Inserts = false;
    bool hasActiveTrackVst3Inserts = false;
    /// An enabled signal-generator source on some track. The realtime engine's "is there anything
    /// to render" gate counts VST3 inserts, clips, MIDI and instruments — a generator is none of
    /// those, so on an empty timeline the whole render was skipped and a freshly added generator
    /// played SILENCE (offline renders had no such gate, which is why the tests all passed).
    bool hasActiveSourceGenerator = false;
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
    std::vector<float> trackConsoleGainReductionDb;
    std::vector<float> trackConsoleGateGainReductionDb;
    std::vector<std::string> trackInsertMeterTrackNames;
    std::vector<int> trackInsertMeterSlotIndices;
    std::vector<float> trackInsertInputPeak;
    std::vector<float> trackInsertOutputPeak;
    /// The mix as it leaves the master fader — before anything the monitor path does (monitor EQ,
    /// speaker sim, monitor volume/dim/mute). The mixer's Master meter reads this, so turning the
    /// monitor knob no longer moves it.
    float masterPeakLeft = 0.0f;
    float masterPeakRight = 0.0f;
};

struct ProjectAudioRenderState {
    std::map<std::string, std::deque<MixerStereoFrame>> routeDelayLines;
    std::map<std::string, ConsoleChannelProcessor> consoleChannelProcessors;
    /// Optional detour for a track's console strip, set only by the realtime engine: return true
    /// having processed the block on a remote node, false to let the local processor run.
    ///
    /// It is a hook rather than a branch inside the renderer because the OFFLINE BOUNCE shares
    /// this code, and a bounce must never depend on a network node — it has to be deterministic
    /// and reproducible with the node switched off. The realtime engine installs the hook; the
    /// bounce leaves it null and always processes locally. Both sides run the same
    /// ConsoleChannelProcessor (the node links this library), so the sound matches.
    std::function<bool(const std::string& routeName, std::vector<float>& interleavedStereo)>
        remoteConsoleStrip;
    /// Same contract for the MASTER insert chain: return true having processed the block on a
    /// remote node, false to run the local chain. Null in every offline bounce, for the same
    /// reason as above — a bounce must be reproducible with the node switched off.
    std::function<bool(std::vector<float>& interleavedStereo)> remoteMasterInserts;
    /// A route's REMOTE-ASSIGNED track inserts. The local route graph rightly excludes them
    /// (activeLocalRouteInserts), and in a plain offline bounce nothing else runs them — they
    /// were silently dropped from the render. The strict remote bounce installs this to run
    /// them on the node, after the route's local inserts, in slot order.
    std::function<void(const std::string& routeName, std::vector<float>& interleavedStereo)>
        remoteRouteInserts;
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
    // Declick: crossfade a route's output whenever it flips between dry and wet (a chain becomes
    // ready, is retired, or is replaced), so the jump doesn't click. Held from the last emitted
    // sample so the seam is continuous; covers add, remove (even the last insert), and the async
    // dry↔wet swap.
    std::map<std::string, int> routeInsertWasWet;                     // -absent = first sight
    std::map<std::string, std::pair<float, float>> routeInsertDeclickHold;
    std::map<std::string, int> routeInsertDeclickRemaining;
    std::map<std::string, int> routeInsertDeclickTotal;
    // Set for one block whenever ANY route flips dry↔wet (an async insert chain engages, is
    // retired, or is replaced). The engine reads it to re-arm its 80 ms full-waveform output
    // crossfade at the real swap instant — the graph-signature arming happens at edit time, while
    // the chain is still preparing off-thread and the route is dry, so the window closes before the
    // wet audio arrives. Owned/reset by the engine each block.
    bool routeInsertEngagementChangedThisBlock = false;
    std::map<std::string, double> sourceGeneratorPhases;
    // High-accuracy per-route signal generators (band-limited, click-free). Keyed by route name so a
    // generator's phase/noise/ramp stay continuous across blocks. Preserved across a plain edit.
    std::map<std::string, TestSignalGenerator> sourceGenerators;
    std::map<std::string, std::vector<Vst3MidiEvent>> liveMidiEvents;
    std::vector<float> masterInsertDryFallback;

    void reset();
    void resetForSeek();
    // A plain clip edit during playback: keep the continuous-playback buffers (route delay lines,
    // generator phases, queued live MIDI) so the sound never gaps/clicks — only drop per-block error
    // state. The delay lines self-adapt if a delay amount changed, so keeping them is always safe.
    void resetForEdit();
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
/// Whether a track insert belongs to the LOCAL route graph (as opposed to the NDS path). The
/// strict remote bounce uses the same judgement, inverted, so a slot is never processed twice.
bool trackInsertShouldRunInLocalRouteGraph(const TrackInsertSlot& insert);

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
