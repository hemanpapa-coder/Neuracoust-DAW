#include "bridge/NeuracoustEngineBridge.h"

#include "audio/AudioDeviceModel.h"
#include "audio/ListenRoom.h"
#include "audio/MidiInputRecorder.h"
#include "audio/OfflineBounce.h"
#include "audio/RealtimeAudioEngine.h"
#include "audio/RemoteDspServerClient.h"
#include "audio/WavFile.h"
#include "plugins/InsertDspPolicy.h"
#include "plugins/MonitorDspModules.h"
#include "plugins/PluginScanner.h"
#include "plugins/Vst3RealtimeBridgeProtocol.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"
#include "project/AudioImport.h"
#include "project/ProjectHistory.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using neuracoust::daw::AudioEngineSettings;
using neuracoust::daw::AudioEngineStatus;
using neuracoust::daw::MonitorDspModule;
using neuracoust::daw::ProjectDocument;
using neuracoust::daw::RealtimeAudioEngine;

namespace {

void copyText(char* dst, size_t dstLen, const std::string& src) {
    if (dstLen == 0) {
        return;
    }
    const size_t n = std::min(src.size(), dstLen - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // namespace

struct NCEngine {
    RealtimeAudioEngine engine;
    ProjectDocument project = neuracoust::daw::defaultProject();
    bool monitorDspEnabled = true;
    bool delayCompensationEnabled = true;
    std::string monitorDspPathMode = "internal";
    /// Empty means the system default output device.
    std::string outputDeviceId;
    /// The reference/monitor input device (e.g. BlackHole for reference music).
    /// Empty means the system default input device.
    std::string inputDeviceId;
    /// Latest FFT spectrum bins, cached each status poll so the analyzer reads them
    /// without a second full status snapshot.
    std::vector<float> lastSpectrumBins;
    /// Latest goniometer L/R sample pairs, cached the same way.
    std::vector<float> lastGoniometerSamples;
    /// Monitor the computer's input source instead of the DAW master. Default: master.
    bool monitorListenSource = false;
    /// Insert tail on stop: <0 always on (default), 0 cut, >0 ring out N seconds.
    double insertTailOnStopSeconds = -1.0;

    /// Live MIDI input for monitoring a keyboard through an instrument track.
    neuracoust::daw::MidiInputRecorder midiInputRecorder;
    /// Peak activity (0..1) of the MIDI seen since the last meter read; consumed by
    /// nc_midi_input_activity so the UI can decay it.
    float midiInputActivity = 0.0f;

    std::vector<neuracoust::daw::PluginCandidate> plugins;         // full scan
    std::vector<neuracoust::daw::PluginCandidate> filteredPlugins; // current browser view
    neuracoust::daw::PluginCandidateFilterOptions facets;

    /// Peaks keyed by source path. Decoding a WAV is not cheap and the timeline
    /// asks for the same file on every redraw.
    struct WaveformPeaks {
        // Mono sum, kept for the single-envelope draw and any mono source.
        std::vector<float> mins;
        std::vector<float> maxs;
        // Per-channel peaks (up to 2: L, R) so a stereo clip draws two envelopes.
        int channels = 1;
        std::vector<float> minsL;
        std::vector<float> maxsL;
        std::vector<float> minsR;
        std::vector<float> maxsR;
        double durationSeconds = 0.0;
    };
    std::map<std::string, WaveformPeaks> waveformCache;

    /// One clip, the way the old UI's clipClipboard_ held one.
    /// A whole selection, with start times relative to the earliest clip — that is
    /// the shape pasteClipRange wants.
    std::vector<neuracoust::daw::ClipState> clipboard;
    /// Ids created by the last batch edit, for the caller to reselect.
    std::vector<std::string> lastResultIds;

    neuracoust::daw::ProjectHistory history;
    std::string projectPath;     // empty until the document has a home on disk
    std::string autosaveError;

    /// Records a step for a discrete edit, and autosaves if the document moved.
    /// Continuous edits (fader, pan) must not call this on every frame — the caller
    /// records once when the gesture ends.
    void recordStep(const std::string& stepName) {
        if (!history.recordEdit(project, stepName)) {
            return;
        }
        autosave();
    }

    void autosave() {
        if (projectPath.empty()) {
            return;
        }
        std::string error;
        if (history.isDirty()) {
            if (!neuracoust::daw::writeProjectAutosaveFile(project, projectPath, error)) {
                autosaveError = error.empty() ? "autosave failed" : error;
                return;
            }
        } else {
            neuracoust::daw::removeProjectAutosaveFile(projectPath, error);
        }
        autosaveError.clear();
    }

    /// Every insert edit reconciles into the engine the cheap way first.
    void reconcileProject() {
        neuracoust::daw::normalizeProjectRouting(project);
        std::string error;
        if (!engine.updateProject(project, error)) {
            engine.loadProject(project, error);
        }
    }

    MonitorDspModule* speakerSimulation() {
        for (auto& module : project.monitorModules) {
            if (module.id == "speaker-simulation") {
                return &module;
            }
        }
        return nullptr;
    }

    // The engine has no "apply this monitor state" entry point; controls go in as
    // one call and the module chain as another.
    void pushStationControls() {
        engine.setMonitorStationControls(project.monitorStationMono,
                                         project.monitorStationListenMode,
                                         project.monitorStationSwapLeftRight,
                                         project.monitorStationInvertLeft,
                                         project.monitorStationInvertRight,
                                         project.monitorStationMute,
                                         project.monitorStationDim,
                                         project.monitorStationTalkback,
                                         project.monitorInputTrimDb,
                                         project.monitorVolumeDb,
                                         project.monitorStationDimDb,
                                         project.monitorStationTalkbackRoute);
    }

    void pushModules() {
        engine.setMonitorDspModules(project.monitorModules, monitorDspEnabled);
    }

    neuracoust::daw::ListenRoomSettings listenSettings() const {
        neuracoust::daw::ListenRoomSettings settings;
        settings.enabled = project.listenRoomEnabled;
        settings.sessionName = project.listenRoomSessionName.empty() ? "mix" : project.listenRoomSessionName;
        settings.source = project.listenRoomSource.empty() ? "monitor" : project.listenRoomSource;
        settings.quality = project.listenRoomQuality.empty() ? "opus_high" : project.listenRoomQuality;
        settings.latencyMode = project.listenRoomLatencyMode.empty() ? "stable" : project.listenRoomLatencyMode;
        settings.transportMode = project.listenRoomTransportMode.empty() ? "direct_fallback" : project.listenRoomTransportMode;
        settings.relayHost = project.listenRoomRelayHost.empty() ? "127.0.0.1" : project.listenRoomRelayHost;
        settings.accessToken = project.listenRoomAccessToken;
        settings.relayHttpPort = project.listenRoomRelayHttpPort;
        settings.relayTcpIngestPort = project.listenRoomRelayTcpIngestPort;
        return neuracoust::daw::normalizedListenRoomSettings(settings);
    }

    void pushListenSettings() {
        engine.setListenRoomSettings(listenSettings());
    }
};

NCEngine* nc_engine_create(void) {
    NCEngine* engine = new NCEngine();
    if (engine->project.monitorModules.empty()) {
        engine->project.monitorModules = neuracoust::daw::defaultMonitorDspModules();
    }
    engine->history.reset(engine->project);
    return engine;
}

void nc_engine_destroy(NCEngine* engine) {
    delete engine;
}

namespace {

// The external DSP Manager (NuclustDspManager) config DW hands the engine. Only the
// requested core reserve is user-facing here; the rest stays at the shipped defaults.
// A connected node's reported core_count still wins inside makeRemoteDspCorePlan — this
// is the hint used before/without a report and the count DW asks the manager to hold.
neuracoust::daw::RemoteDspServerSettings buildRemoteDspSettings(NCEngine* engine) {
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    settings.totalCoreHint =
        static_cast<uint16_t>(std::max(1, std::min(16, engine->project.externalDspCoreCount)));
    // Point the engine at the user's node. Clearing the default node list makes the
    // top-level host the effective target, so External/NDS reach a real server instead
    // of the hardcoded "studio.local" default.
    settings.host = engine->project.remoteDspHost.empty()
                        ? std::string("studio.local") : engine->project.remoteDspHost;
    settings.nodes.clear();
    return settings;
}

AudioEngineSettings buildEngineSettings(NCEngine* engine) {
    AudioEngineSettings settings;
    settings.sampleRate = engine->project.sampleRate;
    settings.bufferSize = engine->project.defaultBufferSize;
    settings.tempoBpm = engine->project.tempoBpm;
    settings.timeSignatureNumerator = engine->project.timeSignatureNumerator;
    settings.timeSignatureDenominator = engine->project.timeSignatureDenominator;
    settings.transportRunning = false;
    settings.metronomeEnabled = false;
    settings.monitorDspEnabled = engine->monitorDspEnabled;
    settings.delayCompensationEnabled = engine->delayCompensationEnabled;
    settings.monitorDspPathMode = engine->monitorDspPathMode;
    settings.monitorModules = engine->project.monitorModules;
    settings.monitorStationMono = engine->project.monitorStationMono;
    settings.monitorStationListenMode = engine->project.monitorStationListenMode;
    settings.monitorStationMute = engine->project.monitorStationMute;
    settings.monitorStationDim = engine->project.monitorStationDim;
    settings.monitorStationDimDb = engine->project.monitorStationDimDb;
    settings.monitorInputTrimDb = engine->project.monitorInputTrimDb;
    settings.monitorVolumeDb = engine->project.monitorVolumeDb;
    // The core isolation QoS hint the engine applies to its realtime thread.
    settings.performanceCoreIsolationEnabled = engine->project.appleSiliconCoreIsolationEnabled;
    settings.requestedPerformanceCoreCount =
        std::max(1, std::min(16, engine->project.requestedDspCoreCount));
    settings.remoteDspServer = buildRemoteDspSettings(engine);
    settings.outputDeviceId = engine->outputDeviceId;
    settings.inputDeviceId = engine->inputDeviceId;
    return settings;
}

} // namespace

bool nc_engine_start(NCEngine* engine, char* error, size_t errorLen) {
    if (engine == nullptr) {
        copyText(error, errorLen, "engine is null");
        return false;
    }

    std::string loadError;
    neuracoust::daw::normalizeProjectRouting(engine->project);
    if (!engine->engine.loadProject(engine->project, loadError)) {
        copyText(error, errorLen, loadError.empty() ? "loadProject failed" : loadError);
        return false;
    }

    const AudioEngineSettings settings = buildEngineSettings(engine);
    if (!engine->engine.start(settings)) {
        const AudioEngineStatus status = engine->engine.status();
        copyText(error, errorLen, status.message.empty() ? "audio device did not open" : status.message);
        return false;
    }

    // start() builds the DSP engine, and only loadProject seeds its per-track meter
    // arrays. The load above ran before that engine existed, so its meters would stay
    // empty until the next load. Push the project in once more now that it is there.
    engine->engine.loadProject(engine->project, loadError);

    copyText(error, errorLen, "");
    return true;
}

void nc_engine_stop(NCEngine* engine) {
    if (engine != nullptr) {
        engine->engine.stop();
    }
}

void nc_engine_status(NCEngine* engine, NCEngineStatus* out) {
    if (out == nullptr) {
        return;
    }
    std::memset(out, 0, sizeof(*out));
    if (engine == nullptr) {
        return;
    }

    const AudioEngineStatus s = engine->engine.status();

    out->running = s.running;
    out->transportRunning = s.transportRunning;
    out->sampleRate = s.sampleRate;
    out->outputChannels = s.outputChannels;
    out->requestedBufferSize = s.requestedBufferSize;
    out->outputPeakLeft = s.outputPeakLeft;
    out->outputPeakRight = s.outputPeakRight;
    out->phaseCorrelation = s.phaseCorrelation;
    out->spectrumLow = s.spectrumLow;
    out->spectrumMid = s.spectrumMid;
    out->spectrumHigh = s.spectrumHigh;
    out->momentaryLufs = s.momentaryLufs;
    out->shortTermLufs = s.shortTermLufs;
    out->integratedLufs = s.integratedLufs;
    out->loudnessRange = s.loudnessRange;
    out->truePeakDb = s.truePeakDb;
    out->playbackSeconds = s.playbackSeconds;
    out->delayCompensationEnabled = s.delayCompensationEnabled;
    out->delayCompensationMs = s.delayCompensationMs;
    out->inputChannels = s.inputChannels;
    out->inputPeak = s.inputPeak;
    out->recordArmedTrackCount = s.recordArmedTrackCount;
    out->realtimeCallbackCount = s.realtimeCallbackCount;
    out->realtimeAverageWakeJitterUs = s.realtimeAverageWakeJitterUs;
    out->realtimeMaxWakeJitterUs = s.realtimeMaxWakeJitterUs;
    out->realtimeMaxRenderDurationUs = s.realtimeMaxRenderDurationUs;
    out->realtimeLateWakeCount = s.realtimeLateWakeCount;
    out->remoteDspMonitorActive = s.remoteDspMonitorActive;
    out->remoteDspRoundTripMs = s.remoteDspRoundTripMs;
    out->activeRealtimeVst3TrackInserts = s.activeRealtimeVst3TrackInsertCount;
    out->activeRealtimeVst3MasterInserts = s.activeRealtimeVst3MasterInsertCount;
    out->activeRemoteDspTrackInserts = s.activeRemoteDspTrackInsertCount;
    out->activeOfflineVst3TrackInserts = s.activeOfflineVst3TrackInsertCount;

    const size_t meterCount = std::min({s.trackMeterNames.size(),
                                        s.trackPeakLeft.size(),
                                        s.trackPeakRight.size(),
                                        static_cast<size_t>(NC_MAX_TRACK_METERS)});
    out->trackMeterCount = static_cast<int>(meterCount);
    for (size_t i = 0; i < meterCount; ++i) {
        copyText(out->trackMeterNames[i], NC_NAME_LEN, s.trackMeterNames[i]);
        out->trackPeakLeft[i] = s.trackPeakLeft[i];
        out->trackPeakRight[i] = s.trackPeakRight[i];
    }

    copyText(out->deviceName, NC_TEXT_LEN, s.deviceName);
    copyText(out->dspEngineName, NC_TEXT_LEN, s.dspEngineName);
    copyText(out->monitorDspPathMode, NC_TEXT_LEN, s.monitorDspPathMode);
    copyText(out->message, NC_TEXT_LEN, s.message);

    engine->lastSpectrumBins = s.spectrumBins;
    engine->lastGoniometerSamples = s.goniometerSamples;
}

int nc_spectrum_bin_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->lastSpectrumBins.size()) : 0;
}

bool nc_spectrum_bins(NCEngine* engine, float* out, int count) {
    if (engine == nullptr || out == nullptr || count <= 0) return false;
    const auto& bins = engine->lastSpectrumBins;
    const int n = std::min(count, static_cast<int>(bins.size()));
    for (int i = 0; i < n; ++i) out[i] = bins[static_cast<size_t>(i)];
    for (int i = n; i < count; ++i) out[i] = 0.0f;
    return n > 0;
}

// Goniometer L/R pairs (interleaved), cached on each status poll like the spectrum.
int nc_goniometer_sample_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->lastGoniometerSamples.size()) : 0;
}

bool nc_goniometer_samples(NCEngine* engine, float* out, int count) {
    if (engine == nullptr || out == nullptr || count <= 0) return false;
    const auto& s = engine->lastGoniometerSamples;
    const int n = std::min(count, static_cast<int>(s.size()));
    for (int i = 0; i < n; ++i) out[i] = s[static_cast<size_t>(i)];
    for (int i = n; i < count; ++i) out[i] = 0.0f;
    return n > 0;
}

void nc_engine_set_transport_running(NCEngine* engine, bool running) {
    if (engine != nullptr) {
        engine->engine.setTransportRunning(running);
    }
}

void nc_engine_set_recording(NCEngine* engine, bool active) {
    if (engine != nullptr) {
        engine->engine.setTransportRecordingActive(active);
    }
}

void nc_engine_seek(NCEngine* engine, double seconds) {
    if (engine != nullptr) {
        engine->engine.seek(std::max(0.0, seconds));
    }
}

void nc_engine_rewind(NCEngine* engine) {
    if (engine != nullptr) {
        engine->engine.rewind();
    }
}

void nc_engine_set_metronome_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr) {
        return;
    }
    engine->engine.setMetronomeEnabled(enabled,
                                       engine->project.tempoBpm,
                                       engine->project.tempoMap,
                                       engine->project.timeSignatureNumerator,
                                       engine->project.timeSignatureDenominator,
                                       engine->project.grooveFeel,
                                       engine->project.grooveSwingAmount,
                                       engine->project.timeSignatureMap,
                                       engine->project.metronomeSubdivision);
}

void nc_engine_set_test_tone_enabled(NCEngine* engine, bool enabled) {
    if (engine != nullptr) {
        engine->engine.setTestToneEnabled(enabled);
    }
}

void nc_project_name(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.name : std::string{});
}

void nc_project_timecode(NCEngine* engine, double seconds, char* out, size_t outLen) {
    if (engine == nullptr) {
        copyText(out, outLen, "");
        return;
    }
    copyText(out, outLen, neuracoust::daw::projectTimecodeString(engine->project, seconds));
}

int nc_project_tempo_bpm(NCEngine* engine) {
    return engine != nullptr ? engine->project.tempoBpm : 120;
}

int nc_project_time_signature_numerator(NCEngine* engine) {
    return engine != nullptr ? engine->project.timeSignatureNumerator : 4;
}

int nc_project_time_signature_denominator(NCEngine* engine) {
    return engine != nullptr ? engine->project.timeSignatureDenominator : 4;
}

void nc_project_bars_beats(NCEngine* engine, double seconds, int* bar, int* beat, int* tick) {
    // Constant-tempo conversion. A project carrying a tempo map will drift here;
    // the legacy UI integrated across markers and this must too before tempo
    // changes are exposed in the UI.
    const int fallbackBar = 1;
    const int fallbackBeat = 1;
    if (engine == nullptr) {
        if (bar) *bar = fallbackBar;
        if (beat) *beat = fallbackBeat;
        if (tick) *tick = 0;
        return;
    }

    const double bpm = std::max(1.0, neuracoust::daw::projectTempoAtSeconds(engine->project, seconds));
    const int beatsPerBar = std::max(1, engine->project.timeSignatureNumerator);
    const double secondsPerBeat = 60.0 / bpm;
    const double totalBeats = std::max(0.0, seconds) / secondsPerBeat;

    const int wholeBeats = static_cast<int>(std::floor(totalBeats));
    const double beatFraction = totalBeats - static_cast<double>(wholeBeats);

    if (bar) *bar = wholeBeats / beatsPerBar + 1;
    if (beat) *beat = wholeBeats % beatsPerBar + 1;
    if (tick) *tick = static_cast<int>(std::floor(beatFraction * 960.0));
}

bool nc_project_loop_enabled(NCEngine* engine) {
    return engine != nullptr && engine->project.loopEnabled;
}

void nc_project_set_loop_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->project.loopEnabled == enabled) {
        return;
    }
    engine->project.loopEnabled = enabled;

    // updateProject first, loadProject only as fallback — never rebuild the whole
    // graph on an edit (docs/legacy-ui-contract.md §1).
    std::string error;
    if (!engine->engine.updateProject(engine->project, error)) {
        engine->engine.loadProject(engine->project, error);
    }
    engine->recordStep(enabled ? "Enable loop" : "Disable loop");
}

double nc_project_loop_start(NCEngine* engine) {
    return engine != nullptr ? engine->project.loopStartSeconds : 0.0;
}

double nc_project_loop_end(NCEngine* engine) {
    return engine != nullptr ? engine->project.loopEndSeconds : 0.0;
}

// ---------------------------------------------------------------------------
// Tracks / mixer
// ---------------------------------------------------------------------------

namespace {

neuracoust::daw::TrackState* trackAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.tracks.size()) {
        return nullptr;
    }
    return &engine->project.tracks[static_cast<size_t>(index)];
}

/// Volume and pan travel together through updateTrackMix.
void pushTrackMix(NCEngine* engine, const neuracoust::daw::TrackState& track) {
    engine->engine.updateTrackMix(track.name, track.volumeDb, track.pan);
}

/// Arm, input monitoring, mute and solo travel together.
void pushTrackRealtimeState(NCEngine* engine, const neuracoust::daw::TrackState& track) {
    engine->engine.updateTrackRealtimeState(track.name,
                                            track.recordArmed,
                                            track.inputMonitoring,
                                            track.muted,
                                            track.solo);
}

} // namespace

int nc_track_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->project.tracks.size()) : 0;
}

void nc_track_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->name : std::string{});
}

void nc_track_type(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->trackType : std::string{});
}

void nc_track_color(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->colorHex : std::string{});
}

void nc_track_folder(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->folderName : std::string{});
}

void nc_track_input_bus(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->inputBus : std::string{});
}

void nc_track_output_bus(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->outputBus : std::string{});
}

void nc_track_set_input_bus(NCEngine* engine, int index, const char* bus) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || bus == nullptr || track->inputBus == bus) return;
    track->inputBus = bus;
    engine->recordStep("Set track input");
    engine->reconcileProject();
}

void nc_track_set_output_bus(NCEngine* engine, int index, const char* bus) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || bus == nullptr || track->outputBus == bus) return;
    track->outputBus = bus;
    engine->recordStep("Set track output");
    engine->reconcileProject();
}

namespace {
// Output targets a track can route to: Master plus any aux/bus tracks (not itself,
// not master/monitor). Cached between the count and name queries.
std::vector<std::string>& outputOptionCache() {
    static std::vector<std::string> options;
    return options;
}
}

int nc_track_output_option_count(NCEngine* engine, int index) {
    auto& options = outputOptionCache();
    options.clear();
    if (engine == nullptr) return 0;
    const auto* self = trackAt(engine, index);
    options.push_back("Master");
    for (const auto& track : engine->project.tracks) {
        if (self != nullptr && track.name == self->name) continue;
        if (track.trackType == "aux" || track.trackType == "bus_folder" ||
            track.trackType == "routing_folder") {
            options.push_back(track.name);
        }
    }
    return static_cast<int>(options.size());
}

void nc_track_output_option(NCEngine* engine, int index, int i, char* out, size_t outLen) {
    (void)engine; (void)index;
    const auto& options = outputOptionCache();
    copyText(out, outLen, (i >= 0 && static_cast<size_t>(i) < options.size())
                              ? options[static_cast<size_t>(i)] : std::string{});
}

float nc_track_volume_db(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr ? track->volumeDb : 0.0f;
}

float nc_track_pan(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr ? track->pan : 0.0f;
}

bool nc_track_muted(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr && track->muted;
}

bool nc_track_solo(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr && track->solo;
}

bool nc_track_record_armed(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr && track->recordArmed;
}

bool nc_track_input_monitoring(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr && track->inputMonitoring;
}

void nc_track_set_volume_db(NCEngine* engine, int index, float db) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackVolumeDb(engine->project, track->name, db);
    pushTrackMix(engine, *track);
    // Continuous: the caller records one step when the drag ends.
}

void nc_track_set_pan(NCEngine* engine, int index, float pan) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackPan(engine->project, track->name, pan);
    pushTrackMix(engine, *track);
    // Continuous: the caller records one step when the drag ends.
}

void nc_track_set_muted(NCEngine* engine, int index, bool muted) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackMuted(engine->project, track->name, muted);
    pushTrackRealtimeState(engine, *track);
    engine->recordStep("Mute");
}

void nc_track_set_solo(NCEngine* engine, int index, bool solo) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    // Additive, not exclusive: setTrackSolo touches only this track's flag, and
    // refuses outright on protected tracks (Master, Monitor).
    neuracoust::daw::setTrackSolo(engine->project, track->name, solo);
    pushTrackRealtimeState(engine, *track);
    engine->recordStep("Solo");
}

void nc_track_set_record_armed(NCEngine* engine, int index, bool armed) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackRecordArmed(engine->project, track->name, armed);
    pushTrackRealtimeState(engine, *track);
    engine->recordStep("Record arm");
}

void nc_track_set_input_monitoring(NCEngine* engine, int index, bool monitoring) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackInputMonitoring(engine->project, track->name, monitoring);
    pushTrackRealtimeState(engine, *track);
    engine->recordStep("Input monitoring");
}

namespace {

/// A fresh track changes the render graph and the lane list; adopt it fully.
int adoptNewTrack(NCEngine* engine, const std::string& trackName, const char* stepName) {
    if (trackName.empty()) {
        return -1;
    }
    engine->reconcileProject();
    engine->recordStep(stepName);
    for (size_t index = 0; index < engine->project.tracks.size(); ++index) {
        if (engine->project.tracks[index].name == trackName) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace

int nc_track_add_audio(NCEngine* engine) {
    if (engine == nullptr) return -1;
    return adoptNewTrack(engine, neuracoust::daw::addAudioTrack(engine->project), "Add audio track");
}

int nc_track_add_instrument(NCEngine* engine) {
    if (engine == nullptr) return -1;
    return adoptNewTrack(engine, neuracoust::daw::addInstrumentTrack(engine->project), "Add instrument track");
}

int nc_track_add_midi(NCEngine* engine) {
    if (engine == nullptr) return -1;
    return adoptNewTrack(engine, neuracoust::daw::addMidiTrack(engine->project), "Add MIDI track");
}

bool nc_track_delete(NCEngine* engine, int index, bool removeClips) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return false;
    const std::string name = track->name;
    if (!neuracoust::daw::deleteTrack(engine->project, name, removeClips, removeClips)) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Delete track");
    return true;
}

bool nc_track_rename(NCEngine* engine, int index, const char* newName) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || newName == nullptr || *newName == '\0') return false;
    if (!neuracoust::daw::renameTrack(engine->project, track->name, newName)) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Rename track");
    return true;
}

int nc_track_insert_count(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr ? static_cast<int>(track->inserts.size()) : 0;
}

void nc_track_insert_name(NCEngine* engine, int index, int slot, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) {
        copyText(out, outLen, "");
        return;
    }
    copyText(out, outLen, track->inserts[static_cast<size_t>(slot)].pluginName);
}

bool nc_track_insert_bypassed(NCEngine* engine, int index, int slot) {
    const auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) {
        return false;
    }
    return track->inserts[static_cast<size_t>(slot)].bypassed;
}

void nc_track_set_insert_bypassed(NCEngine* engine, int index, int slot, bool bypassed) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) {
        return;
    }
    track->inserts[static_cast<size_t>(slot)].bypassed = bypassed;
    engine->engine.updateTrackInsertBypassState(track->name, static_cast<size_t>(slot), bypassed);
    engine->recordStep(bypassed ? "Bypass insert" : "Enable insert");
}

namespace {

neuracoust::daw::TrackInsertSlot* insertAt(NCEngine* engine, int index, int slot) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) {
        return nullptr;
    }
    return &track->inserts[static_cast<size_t>(slot)];
}

} // namespace

void nc_track_insert_plugin_path(NCEngine* engine, int index, int slot, char* out, size_t outLen) {
    const auto* insert = insertAt(engine, index, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginPath : std::string{});
}

void nc_track_insert_plugin_format(NCEngine* engine, int index, int slot, char* out, size_t outLen) {
    const auto* insert = insertAt(engine, index, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginFormat : std::string{});
}

void nc_track_insert_class_id(NCEngine* engine, int index, int slot, char* out, size_t outLen) {
    const auto* insert = insertAt(engine, index, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginClassId : std::string{});
}

void nc_track_insert_class_name(NCEngine* engine, int index, int slot, char* out, size_t outLen) {
    const auto* insert = insertAt(engine, index, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginClassName : std::string{});
}

int nc_track_insert_param_count(NCEngine* engine, int index, int slot) {
    const auto* insert = insertAt(engine, index, slot);
    return insert != nullptr ? static_cast<int>(insert->parameters.size()) : 0;
}

uint32_t nc_track_insert_param_id(NCEngine* engine, int index, int slot, int paramIndex) {
    const auto* insert = insertAt(engine, index, slot);
    if (insert == nullptr || paramIndex < 0 ||
        static_cast<size_t>(paramIndex) >= insert->parameters.size()) {
        return 0;
    }
    return insert->parameters[static_cast<size_t>(paramIndex)].parameterId;
}

double nc_track_insert_param_value(NCEngine* engine, int index, int slot, int paramIndex) {
    const auto* insert = insertAt(engine, index, slot);
    if (insert == nullptr || paramIndex < 0 ||
        static_cast<size_t>(paramIndex) >= insert->parameters.size()) {
        return 0.0;
    }
    return insert->parameters[static_cast<size_t>(paramIndex)].normalizedValue;
}

void nc_track_insert_param_name(NCEngine* engine, int index, int slot, int paramIndex,
                                char* out, size_t outLen) {
    const auto* insert = insertAt(engine, index, slot);
    if (insert == nullptr || paramIndex < 0 ||
        static_cast<size_t>(paramIndex) >= insert->parameters.size()) {
        copyText(out, outLen, std::string{});
        return;
    }
    copyText(out, outLen, insert->parameters[static_cast<size_t>(paramIndex)].displayName);
}

bool nc_track_insert_observer(NCEngine* engine, int index, int slot,
                              char* shmName, size_t shmNameLen,
                              int* maxBlock, double* sampleRate) {
    copyText(shmName, shmNameLen, std::string{});
    auto* track = trackAt(engine, index);
    const auto* insert = insertAt(engine, index, slot);
    if (track == nullptr || insert == nullptr || insert->pluginPath.empty()) {
        return false;
    }

    // The out-of-process rule keys on brand/vendor, which the insert slot does not
    // store; the scan does. Match the slot back to the plug-in it came from.
    const auto found = std::find_if(engine->plugins.begin(), engine->plugins.end(),
                                    [&](const neuracoust::daw::PluginCandidate& candidate) {
                                        return candidate.path == insert->pluginPath;
                                    });
    if (found == engine->plugins.end()) {
        return false;
    }

    neuracoust::daw::Vst3PluginDescriptor descriptor;
    descriptor.name = found->pluginName.empty() ? found->name : found->pluginName;
    descriptor.brand = found->brand;
    descriptor.vendor = found->brand;
    descriptor.bundlePath = found->path;
    if (!neuracoust::daw::isVst3HostedOutOfProcess(descriptor)) {
        return false;
    }

    // Derived identically on the engine side (activeLocalRouteInsertShmKeys).
    const std::string key = track->name + "\x1f" + std::to_string(static_cast<size_t>(slot));
    copyText(shmName, shmNameLen, neuracoust::daw::vst3BridgeObserverShmName(key));
    if (maxBlock != nullptr) {
        *maxBlock = engine->project.defaultBufferSize > 0 ? engine->project.defaultBufferSize : 256;
    }
    if (sampleRate != nullptr) {
        *sampleRate = engine->project.sampleRate > 0.0 ? engine->project.sampleRate : 48000.0;
    }
    return true;
}

bool nc_track_set_vst3_parameter(NCEngine* engine, int index, int slot,
                                 uint32_t parameterId, const char* displayName,
                                 double normalizedValue) {
    auto* track = trackAt(engine, index);
    auto* insert = insertAt(engine, index, slot);
    if (track == nullptr || insert == nullptr) {
        return false;
    }

    const double clamped = std::max(0.0, std::min(1.0, normalizedValue));
    const std::string name = displayName != nullptr ? displayName : "";

    bool found = false;
    for (auto& parameter : insert->parameters) {
        if (parameter.parameterId == parameterId) {
            parameter.normalizedValue = clamped;
            if (!name.empty()) {
                parameter.displayName = name;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        // The editor's PARAM lines carry no name; label it the way the engine does.
        insert->parameters.push_back(
            {parameterId, name.empty() ? "Param " + std::to_string(parameterId) : name, clamped});
    }

    // Fine-grained: never rebuild the graph for a knob turn.
    engine->engine.updateTrackVst3Parameter(track->name, static_cast<size_t>(slot),
                                            parameterId, name, clamped);
    return true;
}

namespace {

const neuracoust::daw::InstrumentSlotState* loadedInstrument(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    if (track == nullptr || track->instrument.pluginPath.empty() ||
        track->instrument.pluginName == "No Instrument") {
        return nullptr;
    }
    return &track->instrument;
}

} // namespace

void nc_track_instrument_plugin_path(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* instrument = loadedInstrument(engine, index);
    copyText(out, outLen, instrument != nullptr ? instrument->pluginPath : std::string{});
}

void nc_track_instrument_plugin_format(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* instrument = loadedInstrument(engine, index);
    copyText(out, outLen, instrument != nullptr ? instrument->pluginFormat : std::string{});
}

void nc_track_instrument_class_id(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* instrument = loadedInstrument(engine, index);
    copyText(out, outLen, instrument != nullptr ? instrument->pluginClassId : std::string{});
}

void nc_track_instrument_class_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* instrument = loadedInstrument(engine, index);
    copyText(out, outLen, instrument != nullptr ? instrument->pluginClassName : std::string{});
}

int nc_track_instrument_param_count(NCEngine* engine, int index) {
    const auto* instrument = loadedInstrument(engine, index);
    return instrument != nullptr ? static_cast<int>(instrument->parameters.size()) : 0;
}

uint32_t nc_track_instrument_param_id(NCEngine* engine, int index, int paramIndex) {
    const auto* instrument = loadedInstrument(engine, index);
    if (instrument == nullptr || paramIndex < 0 ||
        static_cast<size_t>(paramIndex) >= instrument->parameters.size()) {
        return 0;
    }
    return instrument->parameters[static_cast<size_t>(paramIndex)].parameterId;
}

double nc_track_instrument_param_value(NCEngine* engine, int index, int paramIndex) {
    const auto* instrument = loadedInstrument(engine, index);
    if (instrument == nullptr || paramIndex < 0 ||
        static_cast<size_t>(paramIndex) >= instrument->parameters.size()) {
        return 0.0;
    }
    return instrument->parameters[static_cast<size_t>(paramIndex)].normalizedValue;
}

bool nc_track_set_instrument_vst3_parameter(NCEngine* engine, int index, uint32_t parameterId,
                                            const char* displayName, double normalizedValue) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || loadedInstrument(engine, index) == nullptr) {
        return false;
    }

    const double clamped = std::max(0.0, std::min(1.0, normalizedValue));
    const std::string name = displayName != nullptr ? displayName : "";

    auto& parameters = track->instrument.parameters;
    auto found = std::find_if(parameters.begin(), parameters.end(),
                              [&](const neuracoust::daw::Vst3ParameterValueState& parameter) {
                                  return parameter.parameterId == parameterId;
                              });
    if (found != parameters.end()) {
        found->normalizedValue = clamped;
        if (!name.empty()) {
            found->displayName = name;
        }
    } else {
        parameters.push_back({parameterId,
                              name.empty() ? "Param " + std::to_string(parameterId) : name,
                              clamped});
    }

    // The instrument's parameters live in the render plan, not in a live insert
    // chain, so the graph has to be reconciled for a knob turn to be heard.
    engine->reconcileProject();
    return true;
}

int nc_track_send_count(NCEngine* engine, int index) {
    const auto* track = trackAt(engine, index);
    return track != nullptr ? static_cast<int>(track->sends.size()) : 0;
}

void nc_track_send_bus(NCEngine* engine, int index, int slot, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) {
        copyText(out, outLen, "");
        return;
    }
    copyText(out, outLen, track->sends[static_cast<size_t>(slot)].busName);
}

float nc_track_send_gain_db(NCEngine* engine, int index, int slot) {
    const auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) {
        return 0.0f;
    }
    return track->sends[static_cast<size_t>(slot)].gainDb;
}

// Reorder a mixer channel: move `sourceName` to sit before/after `targetName`.
bool nc_track_move_near(NCEngine* engine, const char* sourceName, const char* targetName, bool after) {
    if (engine == nullptr || sourceName == nullptr || targetName == nullptr) return false;
    if (!neuracoust::daw::moveTrackNearTrack(engine->project, sourceName, targetName, after)) return false;
    engine->reconcileProject();
    engine->recordStep("Reorder mixer channel");
    return true;
}

int nc_track_add_aux(NCEngine* engine) {
    if (engine == nullptr) return -1;
    auto& tracks = engine->project.tracks;
    const auto taken = [&](const std::string& nm) {
        return std::any_of(tracks.begin(), tracks.end(),
                           [&](const neuracoust::daw::TrackState& t) { return t.name == nm; });
    };
    std::string name;
    for (int n = 1; ; ++n) { name = "Aux " + std::to_string(n); if (!taken(name)) break; }
    neuracoust::daw::TrackState aux;
    aux.name = name;
    aux.trackType = "aux";
    aux.outputBus = "Master";
    aux.colorHex = "#7C8BA0";
    auto master = std::find_if(tracks.begin(), tracks.end(), [](const neuracoust::daw::TrackState& t) {
        return t.name == "Master" || t.trackType == "master";
    });
    if (master != tracks.end()) tracks.insert(master, aux); else tracks.push_back(aux);
    return adoptNewTrack(engine, name, "Add aux track");
}

namespace {
// Send targets = aux/bus tracks (not the track itself, not master/monitor). Cached
// between the count and name queries.
std::vector<std::string>& sendOptionCache() {
    static std::vector<std::string> options;
    return options;
}
}

int nc_track_send_option_count(NCEngine* engine, int index) {
    auto& options = sendOptionCache();
    options.clear();
    if (engine == nullptr) return 0;
    const auto* self = trackAt(engine, index);
    for (const auto& track : engine->project.tracks) {
        if (self != nullptr && track.name == self->name) continue;
        if (track.trackType == "aux" || track.trackType == "bus_folder" ||
            track.trackType == "routing_folder") {
            options.push_back(track.name);
        }
    }
    return static_cast<int>(options.size());
}

void nc_track_send_option(NCEngine* engine, int index, int i, char* out, size_t outLen) {
    (void)engine; (void)index;
    const auto& options = sendOptionCache();
    copyText(out, outLen, (i >= 0 && static_cast<size_t>(i) < options.size())
                              ? options[static_cast<size_t>(i)] : std::string{});
}

bool nc_track_add_send(NCEngine* engine, int index, const char* busName) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || busName == nullptr) return false;
    neuracoust::daw::TrackSendState send;
    send.busName = busName;
    send.gainDb = -12.0f;
    send.enabled = true;
    if (!neuracoust::daw::addTrackSendSlot(engine->project, track->name, send)) return false;
    engine->recordStep("Add send");
    engine->reconcileProject();
    return true;
}

void nc_track_set_send_gain_db(NCEngine* engine, int index, int slot, float db) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) return;
    neuracoust::daw::TrackSendState send = track->sends[static_cast<size_t>(slot)];
    send.gainDb = db;
    if (!neuracoust::daw::setTrackSendSlot(engine->project, track->name, static_cast<size_t>(slot), send)) return;
    engine->recordStep("Set send level");
    engine->reconcileProject();
}

float nc_track_send_pan(NCEngine* engine, int index, int slot) {
    const auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) return 0.0f;
    return track->sends[static_cast<size_t>(slot)].pan;
}

void nc_track_set_send_pan(NCEngine* engine, int index, int slot, float pan) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) return;
    neuracoust::daw::TrackSendState send = track->sends[static_cast<size_t>(slot)];
    send.pan = std::max(-1.0f, std::min(1.0f, pan));
    if (!neuracoust::daw::setTrackSendSlot(engine->project, track->name, static_cast<size_t>(slot), send)) return;
    engine->recordStep("Set send pan");
    engine->reconcileProject();
}

bool nc_track_send_pre_fader(NCEngine* engine, int index, int slot) {
    const auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) return false;
    return track->sends[static_cast<size_t>(slot)].preFader;
}

void nc_track_set_send_pre_fader(NCEngine* engine, int index, int slot, bool pre) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) return;
    neuracoust::daw::TrackSendState send = track->sends[static_cast<size_t>(slot)];
    send.preFader = pre;
    if (!neuracoust::daw::setTrackSendSlot(engine->project, track->name, static_cast<size_t>(slot), send)) return;
    engine->recordStep(pre ? "Send pre-fader" : "Send post-fader");
    engine->reconcileProject();
}

void nc_track_remove_send(NCEngine* engine, int index, int slot) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) return;
    neuracoust::daw::removeTrackSendSlot(engine->project, track->name, static_cast<size_t>(slot));
    engine->recordStep("Remove send");
    engine->reconcileProject();
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

bool nc_history_record_gesture(NCEngine* engine, const char* stepName) {
    if (engine == nullptr) {
        return false;
    }
    const bool recorded = engine->history.recordEdit(engine->project,
                                                    stepName != nullptr ? stepName : "");
    if (recorded) {
        engine->autosave();
    }
    return recorded;
}

bool nc_history_can_undo(NCEngine* engine) {
    return engine != nullptr && engine->history.canUndo();
}

bool nc_history_can_redo(NCEngine* engine) {
    return engine != nullptr && engine->history.canRedo();
}

int nc_history_undo_depth(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->history.undoDepth()) : 0;
}

void nc_history_undo_step_name(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->history.undoStepName() : std::string{});
}

void nc_history_redo_step_name(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->history.redoStepName() : std::string{});
}

namespace {

/// A restored document can differ structurally, so the monitor chain and station
/// controls have to be pushed again alongside the graph reconcile.
void applyRestoredProject(NCEngine* engine) {
    engine->reconcileProject();
    engine->pushModules();
    engine->pushStationControls();
    engine->pushListenSettings();
    engine->autosave();
}

} // namespace

bool nc_history_undo(NCEngine* engine) {
    if (engine == nullptr) {
        return false;
    }
    std::string error;
    if (!engine->history.undo(engine->project, error)) {
        return false;
    }
    applyRestoredProject(engine);
    return true;
}

bool nc_history_redo(NCEngine* engine) {
    if (engine == nullptr) {
        return false;
    }
    std::string error;
    if (!engine->history.redo(engine->project, error)) {
        return false;
    }
    applyRestoredProject(engine);
    return true;
}

void nc_history_reset(NCEngine* engine) {
    if (engine != nullptr) {
        engine->history.reset(engine->project);
        engine->autosave();
    }
}

void nc_history_mark_saved(NCEngine* engine) {
    if (engine != nullptr) {
        engine->history.markSaved(engine->project);
        engine->autosave();
    }
}

bool nc_project_dirty(NCEngine* engine) {
    return engine != nullptr && engine->history.isDirty();
}

void nc_project_set_path(NCEngine* engine, const char* path) {
    if (engine == nullptr) {
        return;
    }
    engine->projectPath = path != nullptr ? path : "";
}

void nc_project_path(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->projectPath : std::string{});
}

void nc_project_autosave_error(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->autosaveError : std::string{});
}

// ---------------------------------------------------------------------------
// Project file I/O and audio import
// ---------------------------------------------------------------------------

namespace {

/// A freshly loaded document replaces everything, so the graph, the monitor chain
/// and the station controls all have to be pushed again.
void adoptProject(NCEngine* engine) {
    if (engine->project.monitorModules.empty()) {
        engine->project.monitorModules = neuracoust::daw::defaultMonitorDspModules();
    }
    engine->reconcileProject();
    engine->pushModules();
    engine->pushStationControls();
    engine->pushListenSettings();
    engine->history.reset(engine->project);
}

const neuracoust::daw::ClipState* clipAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.clips.size()) {
        return nullptr;
    }
    return &engine->project.clips[static_cast<size_t>(index)];
}

} // namespace

void nc_project_new(NCEngine* engine) {
    if (engine == nullptr) {
        return;
    }
    engine->project = neuracoust::daw::defaultProject();
    engine->projectPath.clear();
    engine->autosaveError.clear();
    adoptProject(engine);
}

bool nc_project_autosave_is_newer(const char* path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    return neuracoust::daw::projectAutosaveIsNewerThanProject(std::filesystem::path(path));
}

bool nc_project_open(NCEngine* engine, const char* path, bool preferAutosave,
                     char* error, size_t errorLen) {
    if (engine == nullptr || path == nullptr || *path == '\0') {
        copyText(error, errorLen, "no project path");
        return false;
    }

    const std::filesystem::path projectPath(path);
    neuracoust::daw::ProjectDocument loaded;
    std::string loadError;

    const bool haveAutosave = neuracoust::daw::projectAutosaveIsNewerThanProject(projectPath);
    bool ok = false;
    if (preferAutosave && haveAutosave) {
        ok = neuracoust::daw::loadProjectAutosaveFile(projectPath, loaded, loadError);
    }
    if (!ok) {
        std::error_code fsError;
        if (!std::filesystem::exists(projectPath, fsError)) {
            copyText(error, errorLen, "project file not found");
            return false;
        }
        FILE* file = fopen(path, "rb");
        if (file == nullptr) {
            copyText(error, errorLen, "could not read the project file");
            return false;
        }
        std::string text;
        char buffer[8192];
        size_t read = 0;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, read);
        }
        fclose(file);

        if (!neuracoust::daw::deserializeProjectForPath(text, projectPath, loaded, loadError)) {
            copyText(error, errorLen, loadError.empty() ? "could not parse the project" : loadError);
            return false;
        }
    }

    engine->project = std::move(loaded);
    engine->projectPath = path;
    engine->autosaveError.clear();
    adoptProject(engine);

    // Recovered or declined, the autosave has served its purpose.
    std::string removeError;
    neuracoust::daw::removeProjectAutosaveFile(projectPath, removeError);

    copyText(error, errorLen, "");
    return true;
}

bool nc_project_save(NCEngine* engine, char* error, size_t errorLen) {
    if (engine == nullptr) {
        copyText(error, errorLen, "engine is null");
        return false;
    }
    if (engine->projectPath.empty()) {
        copyText(error, errorLen, "the project has no path yet");
        return false;
    }
    return nc_project_save_as(engine, engine->projectPath.c_str(), error, errorLen);
}

bool nc_project_save_as(NCEngine* engine, const char* path, char* error, size_t errorLen) {
    if (engine == nullptr || path == nullptr || *path == '\0') {
        copyText(error, errorLen, "no project path");
        return false;
    }

    const std::filesystem::path projectPath(path);
    std::string saveError;
    if (!neuracoust::daw::saveProjectFileWithBackup(engine->project, projectPath, saveError)) {
        copyText(error, errorLen, saveError.empty() ? "could not save the project" : saveError);
        return false;
    }

    engine->projectPath = path;
    engine->history.markSaved(engine->project);

    // The document now matches disk; drop the autosave.
    std::string removeError;
    neuracoust::daw::removeProjectAutosaveFile(projectPath, removeError);
    engine->autosaveError.clear();

    copyText(error, errorLen, "");
    return true;
}

bool nc_audio_import_supported(const char* path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    return neuracoust::daw::isSupportedImportAudioExtension(std::filesystem::path(path));
}

bool nc_audio_import(NCEngine* engine, int trackIndex, const char* path, double startSeconds,
                     char* error, size_t errorLen) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || path == nullptr || *path == '\0') {
        copyText(error, errorLen, "no track or no file");
        return false;
    }

    neuracoust::daw::AudioImportResult result;
    std::string importError;
    if (!neuracoust::daw::importAudioFile(engine->project,
                                          std::filesystem::path(engine->projectPath),
                                          track->name,
                                          std::filesystem::path(path),
                                          startSeconds,
                                          result,
                                          importError)) {
        copyText(error, errorLen, importError.empty() ? "import failed" : importError);
        return false;
    }

    engine->reconcileProject();
    engine->recordStep("Import " + std::filesystem::path(path).filename().string());
    copyText(error, errorLen, "");
    return true;
}

int nc_clip_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->project.clips.size()) : 0;
}

void nc_clip_id(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* clip = clipAt(engine, index);
    copyText(out, outLen, clip != nullptr ? clip->id : std::string{});
}

void nc_clip_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* clip = clipAt(engine, index);
    if (clip == nullptr) {
        copyText(out, outLen, "");
        return;
    }
    // Clips carry a region name only once renamed; fall back to the file.
    copyText(out, outLen, clip->regionName.empty()
                              ? std::filesystem::path(clip->sourcePath).filename().string()
                              : clip->regionName);
}

void nc_clip_track(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* clip = clipAt(engine, index);
    copyText(out, outLen, clip != nullptr ? clip->trackName : std::string{});
}

double nc_clip_source_offset_seconds(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr ? clip->sourceOffsetSeconds : 0.0;
}

void nc_clip_source_path(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* clip = clipAt(engine, index);
    copyText(out, outLen, clip != nullptr ? clip->sourcePath : std::string{});
}

double nc_clip_start_seconds(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr ? clip->startSeconds : 0.0;
}

double nc_clip_duration_seconds(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr ? clip->durationSeconds : 0.0;
}

void nc_clip_color(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* clip = clipAt(engine, index);
    copyText(out, outLen, clip != nullptr ? clip->colorHex : std::string{});
}

// ---------------------------------------------------------------------------
// Clip editing
// ---------------------------------------------------------------------------

double nc_project_snap_time(NCEngine* engine, double seconds) {
    if (engine == nullptr) {
        return seconds;
    }
    return neuracoust::daw::snapProjectTime(engine->project, seconds);
}

void nc_project_set_edit_mode(NCEngine* engine, const char* mode) {
    if (engine == nullptr || mode == nullptr) return;
    engine->project.editMode = mode;
}
void nc_project_set_grid_unit(NCEngine* engine, const char* unit) {
    if (engine == nullptr || unit == nullptr) return;
    engine->project.gridUnit = unit;
}
void nc_project_grid_unit(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.gridUnit.c_str() : "");
}

void nc_project_pan_law(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.panLaw.c_str() : "");
}
void nc_project_set_pan_law(NCEngine* engine, const char* law) {
    if (engine == nullptr || law == nullptr) return;
    const std::string value = law;
    if (value != "-3dB" && value != "-4.5dB" && value != "-6dB" && value != "legacy") return;
    if (engine->project.panLaw == value) return;
    engine->project.panLaw = value;
    engine->reconcileProject();
    engine->recordStep("Set pan law");
}
double nc_project_grid_quantum_seconds(NCEngine* engine) {
    return engine == nullptr ? 0.0 : neuracoust::daw::projectTimelineQuantumSeconds(engine->project);
}

namespace {

/// The renderer does not read project.clips. It rebuilds them from trackPlaylists
/// (makeProjectAudioRenderPlan calls rebuildProjectClipsFromActivePlaylists), so a
/// clip edit that only touches project.clips changes the picture and nothing else —
/// the audio keeps playing from the old placement. Only appendAudioClipAt rebuilds
/// the placements for us; every other edit operation leaves them stale.
///
/// Push the clips back into the playlists after each edit, then reconcile.
bool applyClipEdit(NCEngine* engine, bool changed) {
    if (!changed) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    return true;
}

} // namespace

bool nc_clip_move(NCEngine* engine, const char* clipId, double newStartSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::moveClip(engine->project, clipId,
                                                           std::max(0.0, newStartSeconds)));
}

bool nc_clip_trim_start(NCEngine* engine, const char* clipId, double newStartSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::trimClipStart(engine->project, clipId,
                                                                std::max(0.0, newStartSeconds)));
}

bool nc_clip_trim_end(NCEngine* engine, const char* clipId, double newEndSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::trimClipEnd(engine->project, clipId, newEndSeconds));
}

bool nc_clip_split(NCEngine* engine, const char* clipId, double seconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    std::string newClipId;
    if (!neuracoust::daw::splitClip(engine->project, clipId, seconds, newClipId)) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Split clip");
    return true;
}

int nc_clip_glue_range(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr) return 0;
    std::vector<std::string> glued;
    if (!neuracoust::daw::glueClipRange(engine->project, startSeconds, endSeconds, glued) || glued.empty()) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Heal clips");
    return static_cast<int>(glued.size());
}

bool nc_clip_shuffle_move(NCEngine* engine, const char* clipId, double newStartSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    if (!neuracoust::daw::shuffleMoveClip(engine->project, clipId, std::max(0.0, newStartSeconds))) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Shuffle move");
    return true;
}

int nc_clip_shuffle_delete_range(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr) return 0;
    if (!neuracoust::daw::shuffleDeleteClipRange(engine->project, startSeconds, endSeconds)) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Shuffle delete");
    return 1;
}

bool nc_track_clear_instrument(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return false;
    if (!neuracoust::daw::clearTrackInstrumentSlot(engine->project, track->name)) return false;
    engine->reconcileProject();
    engine->recordStep("Remove instrument");
    return true;
}

// Duplicates a track with all its settings, optionally excluding clips/inserts/sends.
// Returns the new track's index, or -1 on failure.
int nc_track_duplicate(NCEngine* engine, int trackIndex,
                       bool includeClips, bool includeInserts, bool includeSends) {
    const auto* source = trackAt(engine, trackIndex);
    if (source == nullptr) return -1;
    const std::string sourceName = source->name;

    std::string newTrackName;
    std::vector<std::string> newClipIds;
    if (!neuracoust::daw::duplicateTrackWithClips(engine->project, sourceName,
                                                  newTrackName, newClipIds)) {
        return -1;
    }

    // Strip the excluded parts from the fresh duplicate before it reaches the engine.
    if (!includeClips) {
        for (const auto& clipId : newClipIds) {
            neuracoust::daw::deleteClip(engine->project, clipId);
        }
    }
    for (auto& track : engine->project.tracks) {
        if (track.name != newTrackName) continue;
        if (!includeInserts) track.inserts.clear();
        if (!includeSends) track.sends.clear();
        break;
    }

    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Duplicate track");

    for (size_t i = 0; i < engine->project.tracks.size(); ++i) {
        if (engine->project.tracks[i].name == newTrackName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool nc_clip_delete(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return false;
    if (!neuracoust::daw::deleteClip(engine->project, clipId)) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Delete clip");
    return true;
}

float nc_clip_gain_db(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr ? clip->gainDb : 0.0f;
}

bool nc_clip_set_gain_db(NCEngine* engine, const char* clipId, float gainDb) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::setClipGainDb(engine->project, clipId, gainDb));
}

// Continuous: sets the field only, no graph rebuild — so dragging clip gain stays
// smooth. The heavy reconcile happens once on commit via nc_clip_set_gain_db.
bool nc_clip_set_gain_db_preview(NCEngine* engine, const char* clipId, float gainDb) {
    if (engine == nullptr || clipId == nullptr) return false;
    return neuracoust::daw::setClipGainDb(engine->project, clipId, gainDb);
}

double nc_clip_fade_in(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr ? clip->fadeInSeconds : 0.0;
}

double nc_clip_fade_out(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr ? clip->fadeOutSeconds : 0.0;
}

bool nc_clip_set_fades(NCEngine* engine, const char* clipId, double fadeIn, double fadeOut) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::setClipFades(engine->project, clipId,
                                                               std::max(0.0, fadeIn),
                                                               std::max(0.0, fadeOut)));
}

// Turn any same-track overlap around `clipId` into a crossfade (fade-out on the earlier
// clip, fade-in on the later, which the renderer sums). No history step — the caller folds
// it into the move gesture. Returns true if it changed anything.
bool nc_clip_apply_crossfades(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::applyAutomaticClipCrossfades(engine->project, clipId));
}

bool nc_clip_set_fade_curves(NCEngine* engine, const char* clipId,
                             const char* inCurve, const char* outCurve) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::setClipFadeCurves(
        engine->project, clipId,
        inCurve != nullptr ? inCurve : "equal_power",
        outCurve != nullptr ? outCurve : "equal_power"));
}

static void clipFadeCurve(NCEngine* engine, int index, bool wantIn, char* out, size_t outLen) {
    const auto* clip = clipAt(engine, index);
    copyText(out, outLen, clip == nullptr ? "" : (wantIn ? clip->fadeInCurve : clip->fadeOutCurve).c_str());
}
void nc_clip_fade_in_curve(NCEngine* engine, int index, char* out, size_t outLen) {
    clipFadeCurve(engine, index, true, out, outLen);
}
void nc_clip_fade_out_curve(NCEngine* engine, int index, char* out, size_t outLen) {
    clipFadeCurve(engine, index, false, out, outLen);
}

namespace {

const neuracoust::daw::ClipState* findClipById(NCEngine* engine, const std::string& clipId) {
    for (const auto& clip : engine->project.clips) {
        if (clip.id == clipId) {
            return &clip;
        }
    }
    return nullptr;
}

} // namespace

bool nc_clip_move_to_track(NCEngine* engine, const char* clipId, int trackIndex,
                           double startSeconds, char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (engine == nullptr || clipId == nullptr) return false;

    const auto* track = trackAt(engine, trackIndex);
    const auto* clip = findClipById(engine, clipId);
    if (track == nullptr || clip == nullptr) return false;
    if (track->name == clip->trackName) {
        // Same lane: an ordinary move, and the id survives.
        if (!applyClipEdit(engine, neuracoust::daw::moveClip(engine->project, clipId,
                                                             std::max(0.0, startSeconds)))) {
            return false;
        }
        copyText(out, outLen, clipId);
        return true;
    }

    // Re-place rather than mutate: pasteClip validates the target track for us,
    // and deleteClip keeps the neighbours where they are.
    neuracoust::daw::ClipState relocated = *clip;
    relocated.trackName = track->name;

    std::string newClipId;
    if (!neuracoust::daw::pasteClip(engine->project, relocated,
                                    std::max(0.0, startSeconds), newClipId)) {
        return false;
    }
    if (!neuracoust::daw::deleteClip(engine->project, clipId)) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Move clip to " + track->name);
    copyText(out, outLen, newClipId);
    return true;
}

namespace {

/// Resolves ids to clips, dropping ones that name nothing.
std::vector<const neuracoust::daw::ClipState*> resolveClips(NCEngine* engine,
                                                            const char* const* clipIds, int count) {
    std::vector<const neuracoust::daw::ClipState*> clips;
    if (engine == nullptr || clipIds == nullptr || count <= 0) {
        return clips;
    }
    for (int index = 0; index < count; ++index) {
        if (clipIds[index] == nullptr) continue;
        if (const auto* clip = findClipById(engine, clipIds[index])) {
            clips.push_back(clip);
        }
    }
    return clips;
}

/// Ids first, because every batch edit invalidates the pointers into project.clips.
std::vector<std::string> resolveClipIds(NCEngine* engine, const char* const* clipIds, int count) {
    std::vector<std::string> ids;
    for (const auto* clip : resolveClips(engine, clipIds, count)) {
        ids.push_back(clip->id);
    }
    return ids;
}

double earliestStart(const std::vector<const neuracoust::daw::ClipState*>& clips) {
    double earliest = std::numeric_limits<double>::max();
    for (const auto* clip : clips) {
        earliest = std::min(earliest, clip->startSeconds);
    }
    return clips.empty() ? 0.0 : earliest;
}

} // namespace

int nc_result_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->lastResultIds.size());
}

void nc_result_id(NCEngine* engine, int index, char* out, size_t outLen) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->lastResultIds.size()) {
        copyText(out, outLen, "");
        return;
    }
    copyText(out, outLen, engine->lastResultIds[static_cast<size_t>(index)]);
}

bool nc_clip_copy_many(NCEngine* engine, const char* const* clipIds, int count) {
    const auto clips = resolveClips(engine, clipIds, count);
    if (clips.empty()) {
        return false;
    }
    // pasteClipRange adds startSeconds to whatever it finds, so store offsets.
    const double anchor = earliestStart(clips);
    engine->clipboard.clear();
    for (const auto* clip : clips) {
        neuracoust::daw::ClipState copy = *clip;
        copy.startSeconds = clip->startSeconds - anchor;
        engine->clipboard.push_back(copy);
    }
    return true;
}

bool nc_clip_copy(NCEngine* engine, const char* clipId) {
    return nc_clip_copy_many(engine, &clipId, 1);
}

int nc_clip_cut_many(NCEngine* engine, const char* const* clipIds, int count) {
    if (!nc_clip_copy_many(engine, clipIds, count)) {
        return 0;
    }
    int cut = 0;
    for (const auto& id : resolveClipIds(engine, clipIds, count)) {
        if (neuracoust::daw::deleteClip(engine->project, id)) {
            ++cut;
        }
    }
    if (cut == 0) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep(cut == 1 ? "Cut clip" : "Cut " + std::to_string(cut) + " clips");
    return cut;
}

bool nc_clip_cut(NCEngine* engine, const char* clipId) {
    return nc_clip_cut_many(engine, &clipId, 1) == 1;
}

bool nc_clipboard_has_clip(NCEngine* engine) {
    return engine != nullptr && !engine->clipboard.empty();
}

int nc_clipboard_clip_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->clipboard.size());
}

void nc_clipboard_clip_name(NCEngine* engine, char* out, size_t outLen) {
    if (engine == nullptr || engine->clipboard.empty()) {
        copyText(out, outLen, "");
        return;
    }
    const auto& clip = engine->clipboard.front();
    copyText(out, outLen, clip.regionName.empty()
                              ? std::filesystem::path(clip.sourcePath).filename().string()
                              : clip.regionName);
}

int nc_clip_paste_all(NCEngine* engine, double startSeconds) {
    if (engine == nullptr) return 0;
    engine->lastResultIds.clear();
    if (engine->clipboard.empty()) {
        return 0;
    }

    std::vector<std::string> newClipIds;
    if (!neuracoust::daw::pasteClipRange(engine->project, engine->clipboard,
                                         std::max(0.0, startSeconds), newClipIds)) {
        return 0;
    }
    // pasteClipRange pushes onto project.clips and stops there — the placements
    // would stay stale and the pasted clips would play silent.
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep(newClipIds.size() == 1
                           ? "Paste clip"
                           : "Paste " + std::to_string(newClipIds.size()) + " clips");
    engine->lastResultIds = newClipIds;
    return static_cast<int>(newClipIds.size());
}

bool nc_clip_paste(NCEngine* engine, double startSeconds, char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (nc_clip_paste_all(engine, startSeconds) == 0) {
        return false;
    }
    copyText(out, outLen, engine->lastResultIds.front());
    return true;
}

int nc_clip_move_many(NCEngine* engine, const char* const* clipIds, int count, double deltaSeconds) {
    const auto clips = resolveClips(engine, clipIds, count);
    if (clips.empty() || !std::isfinite(deltaSeconds)) {
        return 0;
    }
    // Clamping each clip on its own would collapse the selection against zero.
    const double delta = std::max(deltaSeconds, -earliestStart(clips));

    std::vector<std::pair<std::string, double>> targets;
    targets.reserve(clips.size());
    for (const auto* clip : clips) {
        targets.emplace_back(clip->id, clip->startSeconds + delta);
    }

    int moved = 0;
    for (const auto& [id, start] : targets) {
        if (neuracoust::daw::moveClip(engine->project, id, start)) {
            ++moved;
        }
    }
    return applyClipEdit(engine, moved > 0) ? moved : 0;
}

namespace {

constexpr const char* kVolumeParameterId = "track.volume";
constexpr const char* kPanParameterId = "track.pan";

bool isVolumeParameter(const char* parameterId) {
    return parameterId != nullptr && std::strcmp(parameterId, kVolumeParameterId) == 0;
}

bool isPanParameter(const char* parameterId) {
    return parameterId != nullptr && std::strcmp(parameterId, kPanParameterId) == 0;
}

/// Plugin-insert automation lanes are keyed "insert.<slot>.<paramId>".
bool isPluginAutomationParameter(const char* parameterId) {
    return parameterId != nullptr && std::strncmp(parameterId, "insert.", 7) == 0;
}

/// The points behind a parameter, wherever the track happens to keep them. Volume has its
/// own vector; everything else — pan and plugin-insert lanes — is a generic lane keyed by id.
const std::vector<neuracoust::daw::AutomationPointState>* automationPoints(
    NCEngine* engine, int trackIndex, const char* parameterId) {
    const auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) {
        return nullptr;
    }
    if (isVolumeParameter(parameterId)) {
        return &track->volumeAutomation;
    }
    for (const auto& lane : track->automationLanes) {
        if (lane.parameterId == parameterId) {
            return &lane.points;
        }
    }
    return nullptr;
}

/// Automation changes what the mixer does, not what clips exist: no playlist rebuild.
bool applyAutomationEdit(NCEngine* engine, bool changed, const char* stepName) {
    if (!changed) {
        return false;
    }
    engine->reconcileProject();
    if (stepName != nullptr) {
        engine->recordStep(stepName);
    }
    return true;
}

} // namespace

namespace {

const neuracoust::daw::MidiRegionState* midiRegionAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.midiRegions.size()) {
        return nullptr;
    }
    return &engine->project.midiRegions[static_cast<size_t>(index)];
}

const neuracoust::daw::MidiRegionState* midiRegionById(NCEngine* engine, const char* regionId) {
    if (engine == nullptr || regionId == nullptr) {
        return nullptr;
    }
    for (const auto& region : engine->project.midiRegions) {
        if (region.id == regionId) {
            return &region;
        }
    }
    return nullptr;
}

const neuracoust::daw::MidiNoteState* midiNoteAt(NCEngine* engine, const char* regionId, int noteIndex) {
    const auto* region = midiRegionById(engine, regionId);
    if (region == nullptr || noteIndex < 0 ||
        static_cast<size_t>(noteIndex) >= region->notes.size()) {
        return nullptr;
    }
    return &region->notes[static_cast<size_t>(noteIndex)];
}

/// MIDI regions go into the render plan verbatim; there is no playlist to rebuild.
bool applyMidiEdit(NCEngine* engine, bool changed, const char* stepName) {
    if (!changed) {
        return false;
    }
    engine->reconcileProject();
    if (stepName != nullptr) {
        engine->recordStep(stepName);
    }
    return true;
}

} // namespace

int nc_midi_region_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.midiRegions.size());
}

void nc_midi_region_id(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* region = midiRegionAt(engine, index);
    copyText(out, outLen, region != nullptr ? region->id : std::string{});
}

void nc_midi_region_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* region = midiRegionAt(engine, index);
    copyText(out, outLen, region != nullptr ? region->name : std::string{});
}

void nc_midi_region_track(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* region = midiRegionAt(engine, index);
    copyText(out, outLen, region != nullptr ? region->trackName : std::string{});
}

double nc_midi_region_start_seconds(NCEngine* engine, int index) {
    const auto* region = midiRegionAt(engine, index);
    return region != nullptr ? region->startSeconds : 0.0;
}

double nc_midi_region_duration_seconds(NCEngine* engine, int index) {
    const auto* region = midiRegionAt(engine, index);
    return region != nullptr ? region->durationSeconds : 0.0;
}

bool nc_midi_region_muted(NCEngine* engine, int index) {
    const auto* region = midiRegionAt(engine, index);
    return region != nullptr && region->muted;
}

bool nc_midi_region_add(NCEngine* engine, int trackIndex, double startSeconds,
                        double durationSeconds, char* out, size_t outLen) {
    copyText(out, outLen, "");
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) {
        return false;
    }
    const std::string id = neuracoust::daw::addMidiRegion(engine->project, track->name,
                                                          startSeconds, durationSeconds);
    if (id.empty()) {
        return false;
    }
    applyMidiEdit(engine, true, "Add MIDI region");
    copyText(out, outLen, id);
    return true;
}

bool nc_midi_region_move(NCEngine* engine, const char* regionId, int trackIndex, double startSeconds) {
    const auto* region = midiRegionById(engine, regionId);
    if (region == nullptr) {
        return false;
    }
    // The engine wants a destination track name; a negative index means "same track".
    std::string trackName = region->trackName;
    if (trackIndex >= 0) {
        const auto* track = trackAt(engine, trackIndex);
        if (track == nullptr) {
            return false;
        }
        trackName = track->name;
    }
    return applyMidiEdit(engine,
                         neuracoust::daw::moveMidiRegion(engine->project, regionId, trackName,
                                                         std::max(0.0, startSeconds)),
                         nullptr);
}

bool nc_midi_region_resize(NCEngine* engine, const char* regionId, double durationSeconds) {
    if (engine == nullptr || regionId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::resizeMidiRegion(engine->project, regionId, durationSeconds),
                         nullptr);
}

bool nc_midi_region_delete(NCEngine* engine, const char* regionId) {
    if (engine == nullptr || regionId == nullptr) return false;
    return applyMidiEdit(engine, neuracoust::daw::deleteMidiRegion(engine->project, regionId),
                         "Delete MIDI region");
}

int nc_midi_region_quantize(NCEngine* engine, const char* regionId, double beatQuantum) {
    if (engine == nullptr || regionId == nullptr) return 0;
    std::vector<std::string> changed;
    if (!neuracoust::daw::quantizeMidiRegion(engine->project, regionId, beatQuantum, changed)) {
        return 0;
    }
    applyMidiEdit(engine, true, "Quantize region");
    return static_cast<int>(changed.size());
}

int nc_midi_region_transpose(NCEngine* engine, const char* regionId, int semitones) {
    if (engine == nullptr || regionId == nullptr) return 0;
    std::vector<std::string> changed;
    if (!neuracoust::daw::transposeMidiRegion(engine->project, regionId, semitones, changed)) {
        return 0;
    }
    applyMidiEdit(engine, true, "Transpose region");
    return static_cast<int>(changed.size());
}

int nc_midi_region_humanize(NCEngine* engine, const char* regionId, double maxTimingBeats,
                            int maxVelocityDelta, unsigned int seed) {
    if (engine == nullptr || regionId == nullptr) return 0;
    std::vector<std::string> changed;
    if (!neuracoust::daw::humanizeMidiRegion(engine->project, regionId, maxTimingBeats,
                                             maxVelocityDelta, seed, changed)) {
        return 0;
    }
    applyMidiEdit(engine, true, "Humanize region");
    return static_cast<int>(changed.size());
}

bool nc_midi_region_split(NCEngine* engine, const char* regionId, double splitSeconds,
                          char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (engine == nullptr || regionId == nullptr) return false;
    std::string newRegionId;
    if (!neuracoust::daw::splitMidiRegion(engine->project, regionId, splitSeconds, newRegionId)) {
        return false;
    }
    applyMidiEdit(engine, true, "Split MIDI region");
    copyText(out, outLen, newRegionId);
    return true;
}

bool nc_midi_region_duplicate(NCEngine* engine, const char* regionId, char* out, size_t outLen) {
    copyText(out, outLen, "");
    const auto* region = midiRegionById(engine, regionId);
    if (region == nullptr) return false;

    // Land the copy immediately after the original, the way a clip duplicate does.
    const double newStart = region->startSeconds + region->durationSeconds;
    std::string newRegionId;
    if (!neuracoust::daw::duplicateMidiRegion(engine->project, regionId, newStart, newRegionId)) {
        return false;
    }
    applyMidiEdit(engine, true, "Duplicate MIDI region");
    copyText(out, outLen, newRegionId);
    return true;
}

int nc_midi_note_count(NCEngine* engine, const char* regionId) {
    const auto* region = midiRegionById(engine, regionId);
    return region != nullptr ? static_cast<int>(region->notes.size()) : 0;
}

void nc_midi_note_id(NCEngine* engine, const char* regionId, int noteIndex, char* out, size_t outLen) {
    const auto* note = midiNoteAt(engine, regionId, noteIndex);
    copyText(out, outLen, note != nullptr ? note->id : std::string{});
}

int nc_midi_note_pitch(NCEngine* engine, const char* regionId, int noteIndex) {
    const auto* note = midiNoteAt(engine, regionId, noteIndex);
    return note != nullptr ? note->pitch : 0;
}

double nc_midi_note_start_beats(NCEngine* engine, const char* regionId, int noteIndex) {
    const auto* note = midiNoteAt(engine, regionId, noteIndex);
    return note != nullptr ? note->startBeats : 0.0;
}

double nc_midi_note_duration_beats(NCEngine* engine, const char* regionId, int noteIndex) {
    const auto* note = midiNoteAt(engine, regionId, noteIndex);
    return note != nullptr ? note->durationBeats : 0.0;
}

int nc_midi_note_velocity(NCEngine* engine, const char* regionId, int noteIndex) {
    const auto* note = midiNoteAt(engine, regionId, noteIndex);
    return note != nullptr ? note->velocity : 0;
}

bool nc_midi_note_add(NCEngine* engine, const char* regionId, int pitch, double startBeats,
                      double durationBeats, int velocity, char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (engine == nullptr || regionId == nullptr) return false;
    const std::string id = neuracoust::daw::addMidiNote(engine->project, regionId, pitch,
                                                        startBeats, durationBeats, velocity);
    if (id.empty()) {
        return false;
    }
    applyMidiEdit(engine, true, "Add note");
    copyText(out, outLen, id);
    return true;
}

bool nc_midi_note_move(NCEngine* engine, const char* regionId, const char* noteId,
                       int pitch, double startBeats) {
    if (engine == nullptr || regionId == nullptr || noteId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::moveMidiNote(engine->project, regionId, noteId,
                                                       pitch, startBeats),
                         nullptr);
}

bool nc_midi_note_resize(NCEngine* engine, const char* regionId, const char* noteId, double durationBeats) {
    if (engine == nullptr || regionId == nullptr || noteId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::resizeMidiNote(engine->project, regionId, noteId, durationBeats),
                         nullptr);
}

bool nc_midi_note_set_velocity(NCEngine* engine, const char* regionId, const char* noteId, int velocity) {
    if (engine == nullptr || regionId == nullptr || noteId == nullptr) return false;
    // Continuous: a velocity drag streams these, so it records nothing on its own.
    return applyMidiEdit(engine,
                         neuracoust::daw::setMidiNoteVelocity(engine->project, regionId, noteId, velocity),
                         nullptr);
}

bool nc_midi_note_delete(NCEngine* engine, const char* regionId, const char* noteId) {
    if (engine == nullptr || regionId == nullptr || noteId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::deleteMidiNote(engine->project, regionId, noteId),
                         "Delete note");
}

int nc_marker_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.markers.size());
}

namespace {

const neuracoust::daw::MarkerState* markerAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.markers.size()) {
        return nullptr;
    }
    return &engine->project.markers[static_cast<size_t>(index)];
}

} // namespace

double nc_marker_time(NCEngine* engine, int index) {
    const auto* marker = markerAt(engine, index);
    return marker != nullptr ? marker->timeSeconds : 0.0;
}

void nc_marker_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* marker = markerAt(engine, index);
    copyText(out, outLen, marker != nullptr ? marker->name : std::string{});
}

bool nc_marker_add(NCEngine* engine, double timeSeconds, char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (engine == nullptr) return false;
    const std::string id = neuracoust::daw::addMarkerAt(engine->project, timeSeconds);
    if (id.empty()) {
        return false;
    }
    // Markers touch no audio, so there is nothing to reconcile into the engine.
    engine->recordStep("Add marker");
    copyText(out, outLen, id);
    return true;
}

bool nc_marker_rename(NCEngine* engine, double timeSeconds, double toleranceSeconds, const char* name) {
    if (engine == nullptr || name == nullptr || *name == '\0') return false;
    if (!neuracoust::daw::renameNearestMarker(engine->project, timeSeconds, toleranceSeconds, name)) {
        return false;
    }
    engine->recordStep("Rename marker");
    return true;
}

bool nc_marker_move(NCEngine* engine, double fromSeconds, double toleranceSeconds, double toSeconds) {
    if (engine == nullptr) return false;
    return neuracoust::daw::moveNearestMarker(engine->project, fromSeconds, toleranceSeconds, toSeconds);
}

bool nc_marker_delete(NCEngine* engine, double timeSeconds, double toleranceSeconds) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::deleteNearestMarker(engine->project, timeSeconds, toleranceSeconds)) {
        return false;
    }
    engine->recordStep("Delete marker");
    return true;
}

bool nc_marker_surrounding_range(NCEngine* engine, double seconds, double* start, double* end) {
    if (engine == nullptr || start == nullptr || end == nullptr) return false;
    double rangeStart = 0.0;
    double rangeEnd = 0.0;
    if (!neuracoust::daw::setEditSelectionToSurroundingMarkers(engine->project, seconds,
                                                               rangeStart, rangeEnd)) {
        return false;
    }
    *start = rangeStart;
    *end = rangeEnd;
    return true;
}

// ---------------------------------------------------------------------------
// Conductor / global track: chords, lyrics, tempo markers. (Markers + tempo/sig
// values already have accessors above.) These touch no audio.
// ---------------------------------------------------------------------------

int nc_chord_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.chordEvents.size());
}
double nc_chord_time(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.chordEvents.size()) return 0.0;
    return engine->project.chordEvents[static_cast<size_t>(index)].timeSeconds;
}
void nc_chord_name(NCEngine* engine, int index, char* out, size_t outLen) {
    copyText(out, outLen, (engine != nullptr && index >= 0 && static_cast<size_t>(index) < engine->project.chordEvents.size())
             ? engine->project.chordEvents[static_cast<size_t>(index)].name : std::string{});
}
bool nc_chord_add(NCEngine* engine, double timeSeconds, const char* name) {
    if (engine == nullptr) return false;
    const std::string id = neuracoust::daw::addChordEventAt(engine->project, timeSeconds, name != nullptr ? name : "");
    if (id.empty()) return false;
    engine->recordStep("Add chord");
    return true;
}
bool nc_chord_rename(NCEngine* engine, double timeSeconds, double tol, const char* name) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::renameNearestChordEvent(engine->project, timeSeconds, tol, name != nullptr ? name : "")) return false;
    engine->recordStep("Rename chord");
    return true;
}
bool nc_chord_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::moveNearestChordEvent(engine->project, fromSeconds, tol, toSeconds)) return false;
    engine->recordStep("Move chord");
    return true;
}
bool nc_chord_delete(NCEngine* engine, double timeSeconds, double tol) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::deleteNearestChordEvent(engine->project, timeSeconds, tol)) return false;
    engine->recordStep("Delete chord");
    return true;
}

int nc_lyric_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.lyricEvents.size());
}
double nc_lyric_time(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.lyricEvents.size()) return 0.0;
    return engine->project.lyricEvents[static_cast<size_t>(index)].timeSeconds;
}
void nc_lyric_text(NCEngine* engine, int index, char* out, size_t outLen) {
    copyText(out, outLen, (engine != nullptr && index >= 0 && static_cast<size_t>(index) < engine->project.lyricEvents.size())
             ? engine->project.lyricEvents[static_cast<size_t>(index)].text : std::string{});
}
bool nc_lyric_add(NCEngine* engine, double timeSeconds, const char* text) {
    if (engine == nullptr) return false;
    const std::string id = neuracoust::daw::addLyricEventAt(engine->project, timeSeconds, text != nullptr ? text : "");
    if (id.empty()) return false;
    engine->recordStep("Add lyric");
    return true;
}
bool nc_lyric_rename(NCEngine* engine, double timeSeconds, double tol, const char* text) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::renameNearestLyricEvent(engine->project, timeSeconds, tol, text != nullptr ? text : "")) return false;
    engine->recordStep("Edit lyric");
    return true;
}
bool nc_lyric_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::moveNearestLyricEvent(engine->project, fromSeconds, tol, toSeconds)) return false;
    engine->recordStep("Move lyric");
    return true;
}
bool nc_lyric_delete(NCEngine* engine, double timeSeconds, double tol) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::deleteNearestLyricEvent(engine->project, timeSeconds, tol)) return false;
    engine->recordStep("Delete lyric");
    return true;
}

int nc_tempo_marker_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.tempoMap.size());
}
double nc_tempo_marker_time(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.tempoMap.size()) return 0.0;
    return engine->project.tempoMap[static_cast<size_t>(index)].timeSeconds;
}
double nc_tempo_marker_bpm(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.tempoMap.size()) return 0.0;
    return engine->project.tempoMap[static_cast<size_t>(index)].bpm;
}
bool nc_tempo_marker_add(NCEngine* engine, double timeSeconds, double bpm) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::addTempoMarkerAt(engine->project, timeSeconds, bpm)) return false;
    engine->reconcileProject();
    engine->recordStep("Add tempo");
    return true;
}
bool nc_tempo_marker_delete(NCEngine* engine, double timeSeconds, double tol) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::deleteNearestTempoMarker(engine->project, timeSeconds, tol)) return false;
    engine->reconcileProject();
    engine->recordStep("Delete tempo");
    return true;
}

// --- Time-signature (meter) changes: positional edits over project.timeSignatureMap. ---
int nc_time_sig_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.timeSignatureMap.size());
}
double nc_time_sig_time(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.timeSignatureMap.size()) return 0.0;
    return engine->project.timeSignatureMap[static_cast<size_t>(index)].timeSeconds;
}
int nc_time_sig_numerator(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.timeSignatureMap.size()) return 4;
    return engine->project.timeSignatureMap[static_cast<size_t>(index)].numerator;
}
int nc_time_sig_denominator(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.timeSignatureMap.size()) return 4;
    return engine->project.timeSignatureMap[static_cast<size_t>(index)].denominator;
}
bool nc_time_sig_add(NCEngine* engine, double timeSeconds, int numerator, int denominator) {
    if (engine == nullptr || numerator < 1 || denominator < 1) return false;
    auto& map = engine->project.timeSignatureMap;
    for (auto& m : map) {
        if (std::abs(m.timeSeconds - timeSeconds) < 1e-4) {   // replace one already at this time
            m.numerator = numerator; m.denominator = denominator;
            engine->reconcileProject(); engine->recordStep("Time signature"); return true;
        }
    }
    map.push_back({timeSeconds, numerator, denominator});
    std::sort(map.begin(), map.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
    engine->reconcileProject();
    engine->recordStep("Add time signature");
    return true;
}
bool nc_time_sig_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds) {
    if (engine == nullptr) return false;
    auto& map = engine->project.timeSignatureMap;
    for (auto& m : map) {
        if (m.timeSeconds > 1e-6 && std::abs(m.timeSeconds - fromSeconds) <= tol) {   // never move the anchor at 0
            m.timeSeconds = std::max(0.0, toSeconds);
            std::sort(map.begin(), map.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
            engine->reconcileProject(); engine->recordStep("Move time signature"); return true;
        }
    }
    return false;
}
bool nc_time_sig_delete(NCEngine* engine, double timeSeconds, double tol) {
    if (engine == nullptr) return false;
    auto& map = engine->project.timeSignatureMap;
    for (size_t i = 0; i < map.size(); ++i) {
        if (map[i].timeSeconds > 1e-6 && std::abs(map[i].timeSeconds - timeSeconds) <= tol) {   // keep the anchor at 0
            map.erase(map.begin() + static_cast<long>(i));
            engine->reconcileProject(); engine->recordStep("Delete time signature"); return true;
        }
    }
    return false;
}

bool nc_automation_parameter_supported(const char* parameterId) {
    return isVolumeParameter(parameterId) || isPanParameter(parameterId)
        || isPluginAutomationParameter(parameterId);
}

// Evaluate a lane's points (linear) at a time; returns fallback for an empty lane.
static float evalAutomationPoints(const std::vector<neuracoust::daw::AutomationPointState>& pts,
                                  double t, float fallback) {
    if (pts.empty()) return fallback;
    if (t <= pts.front().timeSeconds) return pts.front().value;
    for (size_t i = 1; i < pts.size(); ++i) {
        if (t <= pts[i].timeSeconds) {
            const auto& a = pts[i - 1]; const auto& b = pts[i];
            const double span = b.timeSeconds - a.timeSeconds;
            if (span <= 0.0) return b.value;
            const double u = (t - a.timeSeconds) / span;
            return static_cast<float>(a.value + (b.value - a.value) * u);
        }
    }
    return pts.back().value;
}

// Drive every plugin-insert automation lane to its value at `timeSeconds`, pushing the
// result into the live graph (fine-grained, no history, no rebuild). Called each UI tick
// while the transport runs, so a drawn plugin curve is actually heard.
void nc_apply_plugin_automation(NCEngine* engine, double timeSeconds) {
    if (engine == nullptr) return;
    for (const auto& track : engine->project.tracks) {
        for (const auto& lane : track.automationLanes) {
            if (lane.parameterId.rfind("insert.", 0) != 0 || lane.points.empty()) continue;
            int slot = -1; unsigned int pid = 0;
            if (std::sscanf(lane.parameterId.c_str(), "insert.%d.%u", &slot, &pid) != 2 || slot < 0) continue;
            const float v = std::max(0.0f, std::min(1.0f, evalAutomationPoints(lane.points, timeSeconds, 0.0f)));
            engine->engine.updateTrackVst3Parameter(track.name, static_cast<size_t>(slot),
                                                    static_cast<uint32_t>(pid), "", v);
        }
    }
}

// --- Automation modes (Off / Read / Touch / Latch / Write / Trim), per track. ---
void nc_track_automation_mode(NCEngine* engine, int trackIndex, char* out, size_t outLen) {
    const auto* track = trackAt(engine, trackIndex);
    copyText(out, outLen, track != nullptr ? track->automationMode.c_str() : "read");
}
void nc_track_set_automation_mode(NCEngine* engine, int trackIndex, const char* mode) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || mode == nullptr) return;
    const std::string value = mode;
    if (value != "off" && value != "read" && value != "touch" &&
        value != "latch" && value != "write" && value != "trim") return;
    if (track->automationMode == value) return;
    track->automationMode = value;
    engine->reconcileProject();
    engine->recordStep("Automation mode");
}

// Evaluate an automation lane at a time (for fader-follow in Read). `fallback` is returned
// when the track has no points for that parameter.
float nc_track_automation_value_at(NCEngine* engine, int trackIndex,
                                   const char* parameterId, double timeSeconds, float fallback) {
    const auto* points = automationPoints(engine, trackIndex, parameterId);
    if (points == nullptr || points->empty()) return fallback;
    return evalAutomationPoints(*points, timeSeconds, fallback);
}

// Write one automation point during a live pass — no history step, so the whole
// touch/latch/write pass folds into a single undo the UI records when it ends.
bool nc_track_automation_write(NCEngine* engine, int trackIndex,
                               const char* parameterId, double timeSeconds, float value) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || !nc_automation_parameter_supported(parameterId)) return false;
    const std::string trackName = track->name;
    const bool changed = isVolumeParameter(parameterId)
        ? neuracoust::daw::setTrackVolumeAutomationPoint(engine->project, trackName, timeSeconds, value)
        : neuracoust::daw::setTrackAutomationLanePoint(engine->project, trackName, parameterId,
              isPanParameter(parameterId) ? "Pan" : parameterId, timeSeconds, value);
    if (changed) engine->reconcileProject();
    return changed;
}

int nc_track_automation_count(NCEngine* engine, int trackIndex, const char* parameterId) {
    const auto* points = automationPoints(engine, trackIndex, parameterId);
    return points != nullptr ? static_cast<int>(points->size()) : 0;
}

double nc_track_automation_time(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex) {
    const auto* points = automationPoints(engine, trackIndex, parameterId);
    if (points == nullptr || pointIndex < 0 || static_cast<size_t>(pointIndex) >= points->size()) {
        return 0.0;
    }
    return (*points)[static_cast<size_t>(pointIndex)].timeSeconds;
}

float nc_track_automation_value(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex) {
    const auto* points = automationPoints(engine, trackIndex, parameterId);
    if (points == nullptr || pointIndex < 0 || static_cast<size_t>(pointIndex) >= points->size()) {
        return 0.0f;
    }
    return (*points)[static_cast<size_t>(pointIndex)].value;
}

bool nc_track_automation_add(NCEngine* engine, int trackIndex, const char* parameterId,
                             double timeSeconds, float value) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || !nc_automation_parameter_supported(parameterId)) {
        return false;
    }
    const std::string trackName = track->name;
    const bool changed =
        isVolumeParameter(parameterId)
            ? neuracoust::daw::setTrackVolumeAutomationPoint(engine->project, trackName, timeSeconds, value)
            : neuracoust::daw::setTrackAutomationLanePoint(engine->project, trackName, parameterId,
                                                           isPanParameter(parameterId) ? "Pan" : parameterId,
                                                           timeSeconds, value);
    return applyAutomationEdit(engine, changed, "Automation point");
}

bool nc_track_automation_move(NCEngine* engine, int trackIndex, const char* parameterId,
                              int pointIndex, double timeSeconds, float value) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || pointIndex < 0 || !nc_automation_parameter_supported(parameterId)) {
        return false;
    }
    const std::string trackName = track->name;
    const auto index = static_cast<size_t>(pointIndex);
    const bool changed =
        isVolumeParameter(parameterId)
            ? neuracoust::daw::moveTrackVolumeAutomationPoint(engine->project, trackName, index,
                                                              timeSeconds, value)
            : neuracoust::daw::moveTrackAutomationLanePoint(engine->project, trackName, parameterId,
                                                            index, timeSeconds, value);
    return applyAutomationEdit(engine, changed, nullptr);
}

bool nc_track_automation_delete(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || pointIndex < 0 || !nc_automation_parameter_supported(parameterId)) {
        return false;
    }
    const std::string trackName = track->name;
    const auto index = static_cast<size_t>(pointIndex);
    const bool changed =
        isVolumeParameter(parameterId)
            ? neuracoust::daw::deleteTrackVolumeAutomationPoint(engine->project, trackName, index)
            : neuracoust::daw::deleteTrackAutomationLanePoint(engine->project, trackName,
                                                              parameterId, index);
    return applyAutomationEdit(engine, changed, "Delete automation point");
}

int nc_track_automation_clear_range(NCEngine* engine, int trackIndex, const char* parameterId,
                                    double startSeconds, double endSeconds) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || !nc_automation_parameter_supported(parameterId)) {
        return 0;
    }
    const std::string trackName = track->name;
    const size_t removed =
        isVolumeParameter(parameterId)
            ? neuracoust::daw::deleteTrackVolumeAutomationPointsInRange(engine->project, trackName,
                                                                        startSeconds, endSeconds)
            : neuracoust::daw::deleteTrackAutomationLanePointsInRange(engine->project, trackName,
                                                                      parameterId,
                                                                      startSeconds, endSeconds);
    applyAutomationEdit(engine, removed > 0, "Clear automation");
    return static_cast<int>(removed);
}

bool nc_project_set_loop_range(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr || !std::isfinite(startSeconds) || !std::isfinite(endSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(startSeconds, endSeconds));
    const double end = std::max(startSeconds, endSeconds);
    if (end <= start) {
        return false;
    }
    engine->project.loopStartSeconds = start;
    engine->project.loopEndSeconds = end;
    engine->reconcileProject();
    return true;
}

int nc_range_copy(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr) return 0;
    auto copied = neuracoust::daw::copyClipRange(engine->project, startSeconds, endSeconds);
    if (copied.empty()) {
        return 0;
    }
    // copyClipRange already anchors the slices to the range start, which is the
    // shape pasteClipRange wants.
    engine->clipboard = std::move(copied);
    return static_cast<int>(engine->clipboard.size());
}

int nc_range_cut(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr) return 0;
    std::vector<neuracoust::daw::ClipState> copied;
    if (!neuracoust::daw::cutClipRange(engine->project, startSeconds, endSeconds, copied) ||
        copied.empty()) {
        return 0;
    }
    engine->clipboard = std::move(copied);
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Cut range");
    return static_cast<int>(engine->clipboard.size());
}

bool nc_range_clear(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::clearClipRange(engine->project, startSeconds, endSeconds)) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Clear range");
    return true;
}

int nc_range_separate(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr) return 0;
    engine->lastResultIds.clear();
    std::vector<std::string> newClipIds;
    if (!neuracoust::daw::separateClipRange(engine->project, startSeconds, endSeconds, newClipIds)) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Separate range");
    engine->lastResultIds = newClipIds;
    return static_cast<int>(newClipIds.size());
}

int nc_range_duplicate(NCEngine* engine, double startSeconds, double endSeconds) {
    if (engine == nullptr) return 0;
    engine->lastResultIds.clear();
    std::vector<std::string> newClipIds;
    if (!neuracoust::daw::duplicateClipRange(engine->project, startSeconds, endSeconds, newClipIds)) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Duplicate range");
    engine->lastResultIds = newClipIds;
    return static_cast<int>(newClipIds.size());
}

int nc_clip_delete_many(NCEngine* engine, const char* const* clipIds, int count) {
    int deleted = 0;
    for (const auto& id : resolveClipIds(engine, clipIds, count)) {
        if (neuracoust::daw::deleteClip(engine->project, id)) {
            ++deleted;
        }
    }
    if (deleted == 0) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep(deleted == 1 ? "Delete clip"
                                    : "Delete " + std::to_string(deleted) + " clips");
    return deleted;
}

int nc_clip_split_many(NCEngine* engine, const char* const* clipIds, int count, double seconds) {
    if (engine == nullptr) return 0;
    engine->lastResultIds.clear();

    int split = 0;
    for (const auto& id : resolveClipIds(engine, clipIds, count)) {
        std::string newClipId;
        // A clip the playhead misses simply does not split.
        if (neuracoust::daw::splitClip(engine->project, id, seconds, newClipId)) {
            engine->lastResultIds.push_back(newClipId);
            ++split;
        }
    }
    if (split == 0) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep(split == 1 ? "Split clip" : "Split " + std::to_string(split) + " clips");
    return split;
}

int nc_clip_duplicate_many(NCEngine* engine, const char* const* clipIds, int count) {
    if (engine == nullptr) return 0;
    engine->lastResultIds.clear();

    const auto clips = resolveClips(engine, clipIds, count);
    if (clips.empty()) {
        return 0;
    }
    // Shift by the whole selection's width, or the copies land on the originals.
    double latestEnd = 0.0;
    for (const auto* clip : clips) {
        latestEnd = std::max(latestEnd, clip->startSeconds + clip->durationSeconds);
    }
    const double span = latestEnd - earliestStart(clips);

    std::vector<std::pair<std::string, double>> targets;
    targets.reserve(clips.size());
    for (const auto* clip : clips) {
        targets.emplace_back(clip->id, clip->startSeconds + span);
    }

    for (const auto& [id, start] : targets) {
        std::string newClipId;
        if (neuracoust::daw::duplicateClip(engine->project, id, start, newClipId)) {
            engine->lastResultIds.push_back(newClipId);
        }
    }
    if (engine->lastResultIds.empty()) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    const size_t made = engine->lastResultIds.size();
    engine->recordStep(made == 1 ? "Duplicate clip"
                                 : "Duplicate " + std::to_string(made) + " clips");
    return static_cast<int>(made);
}

bool nc_clip_duplicate(NCEngine* engine, const char* clipId, char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (engine == nullptr || clipId == nullptr) return false;

    const auto* clip = findClipById(engine, clipId);
    if (clip == nullptr) return false;
    const double newStart = clip->startSeconds + clip->durationSeconds;

    std::string newClipId;
    if (!neuracoust::daw::duplicateClip(engine->project, clipId, newStart, newClipId)) {
        return false;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Duplicate clip");
    copyText(out, outLen, newClipId);
    return true;
}

// ---------------------------------------------------------------------------
// Bounce
// ---------------------------------------------------------------------------

namespace {
void fillBounceResult(const neuracoust::daw::BounceResult& result, NCBounceResult* out);
} // namespace

bool nc_bounce_to_wav(NCEngine* engine, const char* path, NCBounceResult* out) {
    if (out != nullptr) {
        std::memset(out, 0, sizeof(*out));
    }
    if (engine == nullptr || path == nullptr || *path == '\0') {
        if (out != nullptr) copyText(out->message, NC_TEXT_LEN, "no output path");
        return false;
    }

    // Bounce the document as edited. The renderer rebuilds clips from the
    // placements, which every clip edit has already refreshed.
    const auto result = neuracoust::daw::bounceProjectToWav(engine->project, path);
    fillBounceResult(result, out);
    return result.ok;
}

namespace {

void fillBounceResult(const neuracoust::daw::BounceResult& result, NCBounceResult* out) {
    if (out == nullptr) {
        return;
    }
    out->ok = result.ok;
    out->durationSeconds = result.durationSeconds;
    out->peakLeft = result.levelStats.peakLeft;
    out->peakRight = result.levelStats.peakRight;
    out->rmsLeft = result.levelStats.rmsLeft;
    out->rmsRight = result.levelStats.rmsRight;
    out->clippingDetected = result.levelStats.clippingDetected;
    out->nearSilent = result.levelStats.nearSilent;
    out->missingMediaClipCount = static_cast<int>(result.missingMediaClipIds.size());
    copyText(out->message, NC_TEXT_LEN, result.message);
}

} // namespace

int nc_project_serialize(NCEngine* engine, char* out, size_t outLen) {
    if (engine == nullptr) {
        return 0;
    }
    const std::string text = neuracoust::daw::serializeProject(engine->project);
    if (out != nullptr && outLen > 0) {
        copyText(out, outLen, text);
    }
    return static_cast<int>(text.size());
}

bool nc_bounce_snapshot_to_wav(const char* projectText, const char* path, NCBounceResult* out) {
    if (out != nullptr) {
        std::memset(out, 0, sizeof(*out));
    }
    if (projectText == nullptr || path == nullptr || *path == '\0') {
        if (out != nullptr) copyText(out->message, NC_TEXT_LEN, "no project or no output path");
        return false;
    }

    neuracoust::daw::ProjectDocument project;
    std::string error;
    if (!neuracoust::daw::deserializeProject(projectText, project, error)) {
        if (out != nullptr) copyText(out->message, NC_TEXT_LEN, error.empty() ? "could not parse the project" : error);
        return false;
    }

    const auto result = neuracoust::daw::bounceProjectToWav(project, path);
    fillBounceResult(result, out);
    return result.ok;
}

namespace {

/// Peaks are cached at a fixed sample resolution, not a fixed bucket count, so a long
/// clip does not smear: 256 samples per peak is ~5 ms, finer than a pixel at any
/// zoom the timeline reaches. The view decimates these to columns at draw time. A
/// ceiling keeps a very long file from allocating without bound — past it the samples
/// per peak grows instead.
constexpr int64_t kWaveformSamplesPerPeak = 256;
constexpr int64_t kWaveformMaxPeaks = 2'000'000;  // ~3 hours at 256 samples/peak, 48 kHz

const NCEngine::WaveformPeaks* ensureWaveformPeaks(NCEngine* engine, const std::string& key) {
    auto cached = engine->waveformCache.find(key);
    if (cached != engine->waveformCache.end()) {
        return &cached->second;
    }

    neuracoust::daw::WavAudioData audio;
    std::string error;
    if (!neuracoust::daw::readPcmWavFile(key, audio, error) ||
        audio.channels <= 0 || audio.interleavedSamples.empty()) {
        return nullptr;
    }

    const int64_t frames = audio.frameCount();
    const int channels = audio.channels;

    int64_t samplesPerPeak = kWaveformSamplesPerPeak;
    if (frames / samplesPerPeak > kWaveformMaxPeaks) {
        samplesPerPeak = (frames + kWaveformMaxPeaks - 1) / kWaveformMaxPeaks;
    }
    const int64_t peakCount = std::max<int64_t>(1, (frames + samplesPerPeak - 1) / samplesPerPeak);

    NCEngine::WaveformPeaks peaks;
    peaks.channels = std::min(2, std::max(1, channels));
    peaks.mins.assign(static_cast<size_t>(peakCount), 0.0f);
    peaks.maxs.assign(static_cast<size_t>(peakCount), 0.0f);
    peaks.minsL.assign(static_cast<size_t>(peakCount), 0.0f);
    peaks.maxsL.assign(static_cast<size_t>(peakCount), 0.0f);
    if (peaks.channels > 1) {
        peaks.minsR.assign(static_cast<size_t>(peakCount), 0.0f);
        peaks.maxsR.assign(static_cast<size_t>(peakCount), 0.0f);
    }
    peaks.durationSeconds = audio.sampleRate > 0
        ? static_cast<double>(frames) / audio.sampleRate
        : 0.0;

    for (int64_t peak = 0; peak < peakCount; ++peak) {
        const int64_t begin = peak * samplesPerPeak;
        const int64_t end = std::min(frames, begin + samplesPerPeak);
        float low = 0.0f, high = 0.0f;     // mono sum
        float lowL = 0.0f, highL = 0.0f;   // left channel
        float lowR = 0.0f, highR = 0.0f;   // right channel
        for (int64_t frame = begin; frame < end; ++frame) {
            float sum = 0.0f;
            for (int channel = 0; channel < channels; ++channel) {
                const float sample = audio.interleavedSamples[static_cast<size_t>(frame * channels + channel)];
                sum += sample;
                if (channel == 0) { lowL = std::min(lowL, sample); highL = std::max(highL, sample); }
                else if (channel == 1) { lowR = std::min(lowR, sample); highR = std::max(highR, sample); }
            }
            const float value = sum / static_cast<float>(channels);
            low = std::min(low, value);
            high = std::max(high, value);
        }
        const auto p = static_cast<size_t>(peak);
        peaks.mins[p] = std::max(-1.0f, low);
        peaks.maxs[p] = std::min(1.0f, high);
        peaks.minsL[p] = std::max(-1.0f, lowL);
        peaks.maxsL[p] = std::min(1.0f, highL);
        if (peaks.channels > 1) {
            peaks.minsR[p] = std::max(-1.0f, lowR);
            peaks.maxsR[p] = std::min(1.0f, highR);
        }
    }

    return &engine->waveformCache.emplace(key, std::move(peaks)).first->second;
}

} // namespace

int nc_waveform_peak_count(NCEngine* engine, const char* path) {
    if (engine == nullptr || path == nullptr) {
        return 0;
    }
    const auto* peaks = ensureWaveformPeaks(engine, path);
    return peaks != nullptr ? static_cast<int>(peaks->mins.size()) : 0;
}

bool nc_waveform_peaks(NCEngine* engine, const char* path, float* mins, float* maxs, int count) {
    if (engine == nullptr || path == nullptr || mins == nullptr || maxs == nullptr || count <= 0) {
        return false;
    }
    const auto* peaks = ensureWaveformPeaks(engine, path);
    if (peaks == nullptr) {
        return false;
    }
    const size_t available = std::min<size_t>(static_cast<size_t>(count), peaks->mins.size());
    std::memcpy(mins, peaks->mins.data(), available * sizeof(float));
    std::memcpy(maxs, peaks->maxs.data(), available * sizeof(float));
    return true;
}

double nc_waveform_duration_seconds(NCEngine* engine, const char* path) {
    if (engine == nullptr || path == nullptr) {
        return 0.0;
    }
    const auto cached = engine->waveformCache.find(path);
    return cached != engine->waveformCache.end() ? cached->second.durationSeconds : 0.0;
}

// 1 for mono, 2 for stereo — how many envelopes the clip should draw.
int nc_waveform_channel_count(NCEngine* engine, const char* path) {
    if (engine == nullptr || path == nullptr) {
        return 0;
    }
    const auto* peaks = ensureWaveformPeaks(engine, path);
    return peaks != nullptr ? peaks->channels : 0;
}

// Per-channel peaks: channel 0 = L, 1 = R. Falls back to the mono envelope when the
// requested channel is absent (mono source asked for R).
bool nc_waveform_channel_peaks(NCEngine* engine, const char* path, int channel,
                               float* mins, float* maxs, int count) {
    if (engine == nullptr || path == nullptr || mins == nullptr || maxs == nullptr || count <= 0) {
        return false;
    }
    const auto* peaks = ensureWaveformPeaks(engine, path);
    if (peaks == nullptr) {
        return false;
    }
    const std::vector<float>* src = &peaks->mins;
    const std::vector<float>* srcMax = &peaks->maxs;
    if (channel == 0 && !peaks->minsL.empty()) {
        src = &peaks->minsL; srcMax = &peaks->maxsL;
    } else if (channel == 1 && !peaks->minsR.empty()) {
        src = &peaks->minsR; srcMax = &peaks->maxsR;
    }
    const size_t available = std::min<size_t>(static_cast<size_t>(count), src->size());
    std::memcpy(mins, src->data(), available * sizeof(float));
    std::memcpy(maxs, srcMax->data(), available * sizeof(float));
    return true;
}

// ---------------------------------------------------------------------------
// Plugin browser
// ---------------------------------------------------------------------------

namespace {

const neuracoust::daw::PluginCandidate* pluginAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->filteredPlugins.size()) {
        return nullptr;
    }
    return &engine->filteredPlugins[static_cast<size_t>(index)];
}

const std::vector<std::string>* facetList(NCEngine* engine, int kind) {
    if (engine == nullptr) return nullptr;
    switch (kind) {
        case NC_FACET_BRAND: return &engine->facets.brands;
        case NC_FACET_CATEGORY: return &engine->facets.categories;
        case NC_FACET_FORMAT: return &engine->facets.formats;
        case NC_FACET_SCOPE: return &engine->facets.scopes;
        default: return nullptr;
    }
}

std::string facetValueOf(const neuracoust::daw::PluginCandidate& candidate, int kind) {
    switch (kind) {
        case NC_FACET_BRAND: return candidate.brand;
        case NC_FACET_CATEGORY: return candidate.category;
        case NC_FACET_FORMAT: return candidate.format;
        case NC_FACET_SCOPE: return candidate.scope;
        default: return {};
    }
}

} // namespace

int nc_plugin_scan(NCEngine* engine) {
    if (engine == nullptr) return 0;
    engine->plugins = neuracoust::daw::scanKnownPluginLocations();
    neuracoust::daw::sortPluginCandidatesForDisplay(engine->plugins);
    engine->facets = neuracoust::daw::pluginCandidateFilterOptions(engine->plugins);
    engine->filteredPlugins = engine->plugins;
    return static_cast<int>(engine->plugins.size());
}

int nc_plugin_apply_filter(NCEngine* engine,
                           const char* text,
                           const char* brand,
                           const char* category,
                           const char* format) {
    if (engine == nullptr) return 0;

    neuracoust::daw::PluginCandidateFilterCriteria criteria;
    criteria.text = text != nullptr ? text : "";
    criteria.brand = brand != nullptr ? brand : "";
    criteria.category = category != nullptr ? category : "";
    criteria.format = format != nullptr ? format : "";
    criteria.requireExisting = true;

    engine->filteredPlugins = neuracoust::daw::filterPluginCandidates(engine->plugins, criteria);
    return static_cast<int>(engine->filteredPlugins.size());
}

int nc_plugin_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->filteredPlugins.size()) : 0;
}

void nc_plugin_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* plugin = pluginAt(engine, index);
    copyText(out, outLen, plugin != nullptr ? plugin->name : std::string{});
}

void nc_plugin_brand(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* plugin = pluginAt(engine, index);
    copyText(out, outLen, plugin != nullptr ? plugin->brand : std::string{});
}

void nc_plugin_category(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* plugin = pluginAt(engine, index);
    copyText(out, outLen, plugin != nullptr ? plugin->category : std::string{});
}

void nc_plugin_format(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* plugin = pluginAt(engine, index);
    copyText(out, outLen, plugin != nullptr ? plugin->format : std::string{});
}

void nc_plugin_path(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* plugin = pluginAt(engine, index);
    copyText(out, outLen, plugin != nullptr ? plugin->path : std::string{});
}

bool nc_plugin_exists(NCEngine* engine, int index) {
    const auto* plugin = pluginAt(engine, index);
    return plugin != nullptr && plugin->exists;
}

int nc_plugin_facet_count(NCEngine* engine, int kind) {
    const auto* list = facetList(engine, kind);
    return list != nullptr ? static_cast<int>(list->size()) : 0;
}

void nc_plugin_facet_name(NCEngine* engine, int kind, int index, char* out, size_t outLen) {
    const auto* list = facetList(engine, kind);
    if (list == nullptr || index < 0 || static_cast<size_t>(index) >= list->size()) {
        copyText(out, outLen, "");
        return;
    }
    copyText(out, outLen, (*list)[static_cast<size_t>(index)]);
}

int nc_plugin_facet_tally(NCEngine* engine, int kind, int index) {
    const auto* list = facetList(engine, kind);
    if (list == nullptr || index < 0 || static_cast<size_t>(index) >= list->size()) {
        return 0;
    }
    const std::string& value = (*list)[static_cast<size_t>(index)];
    int tally = 0;
    for (const auto& candidate : engine->plugins) {
        if (facetValueOf(candidate, kind) == value) {
            ++tally;
        }
    }
    return tally;
}

namespace {

neuracoust::daw::InsertState* masterInsertAt(NCEngine* engine, int slot) {
    if (engine == nullptr || slot < 0 ||
        static_cast<size_t>(slot) >= engine->project.masterInserts.size()) {
        return nullptr;
    }
    return &engine->project.masterInserts[static_cast<size_t>(slot)];
}

} // namespace

int nc_master_insert_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.masterInserts.size());
}

void nc_master_insert_name(NCEngine* engine, int slot, char* out, size_t outLen) {
    const auto* insert = masterInsertAt(engine, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginName : std::string{});
}

bool nc_master_insert_bypassed(NCEngine* engine, int slot) {
    const auto* insert = masterInsertAt(engine, slot);
    return insert != nullptr && insert->bypassed;
}

void nc_master_insert_plugin_path(NCEngine* engine, int slot, char* out, size_t outLen) {
    const auto* insert = masterInsertAt(engine, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginPath : std::string{});
}

void nc_master_insert_plugin_format(NCEngine* engine, int slot, char* out, size_t outLen) {
    const auto* insert = masterInsertAt(engine, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginFormat : std::string{});
}

void nc_master_insert_class_id(NCEngine* engine, int slot, char* out, size_t outLen) {
    const auto* insert = masterInsertAt(engine, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginClassId : std::string{});
}

void nc_master_insert_class_name(NCEngine* engine, int slot, char* out, size_t outLen) {
    const auto* insert = masterInsertAt(engine, slot);
    copyText(out, outLen, insert != nullptr ? insert->pluginClassName : std::string{});
}

bool nc_master_add_insert(NCEngine* engine, int pluginIndex) {
    const auto* plugin = pluginAt(engine, pluginIndex);
    if (engine == nullptr || plugin == nullptr) {
        return false;
    }

    neuracoust::daw::InsertState insert;
    insert.pluginName = plugin->name;
    insert.pluginFormat = plugin->format.empty() ? "VST3" : plugin->format;
    insert.pluginPath = plugin->path;
    insert.pluginClassId = plugin->pluginClassId;
    insert.pluginClassName = plugin->pluginClassName;
    insert.available = plugin->exists;
    insert.dspAvailable = true;

    // addMasterVst3Insert refuses a duplicate of the same plug-in, which is the
    // engine's rule, not ours.
    if (!neuracoust::daw::addMasterVst3Insert(engine->project, insert)) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Add " + insert.pluginName + " to master");
    return true;
}

bool nc_master_remove_insert(NCEngine* engine, int slot) {
    if (masterInsertAt(engine, slot) == nullptr) {
        return false;
    }
    if (!neuracoust::daw::removeMasterVst3Insert(engine->project, static_cast<size_t>(slot))) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Remove master insert");
    return true;
}

bool nc_master_set_insert_bypassed(NCEngine* engine, int slot, bool bypassed) {
    auto* insert = masterInsertAt(engine, slot);
    if (insert == nullptr || insert->bypassed == bypassed) {
        return false;
    }
    if (!neuracoust::daw::toggleMasterVst3InsertBypass(engine->project, static_cast<size_t>(slot))) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep(bypassed ? "Bypass master insert" : "Enable master insert");
    return true;
}

int nc_master_move_insert(NCEngine* engine, int slot, int direction) {
    if (masterInsertAt(engine, slot) == nullptr) {
        return -1;
    }
    const int moved = neuracoust::daw::moveMasterInsert(engine->project, static_cast<size_t>(slot), direction);
    if (moved < 0) {
        return -1;
    }
    engine->reconcileProject();
    engine->recordStep("Reorder master inserts");
    return moved;
}

int nc_master_insert_param_count(NCEngine* engine, int slot) {
    const auto* insert = masterInsertAt(engine, slot);
    return insert != nullptr ? static_cast<int>(insert->parameters.size()) : 0;
}

uint32_t nc_master_insert_param_id(NCEngine* engine, int slot, int paramIndex) {
    const auto* insert = masterInsertAt(engine, slot);
    if (insert == nullptr || paramIndex < 0 ||
        static_cast<size_t>(paramIndex) >= insert->parameters.size()) {
        return 0;
    }
    return insert->parameters[static_cast<size_t>(paramIndex)].parameterId;
}

double nc_master_insert_param_value(NCEngine* engine, int slot, int paramIndex) {
    const auto* insert = masterInsertAt(engine, slot);
    if (insert == nullptr || paramIndex < 0 ||
        static_cast<size_t>(paramIndex) >= insert->parameters.size()) {
        return 0.0;
    }
    return insert->parameters[static_cast<size_t>(paramIndex)].normalizedValue;
}

bool nc_master_set_vst3_parameter(NCEngine* engine, int slot, uint32_t parameterId,
                                  const char* displayName, double normalizedValue) {
    auto* insert = masterInsertAt(engine, slot);
    if (insert == nullptr) {
        return false;
    }

    const double clamped = std::max(0.0, std::min(1.0, normalizedValue));
    const std::string name = displayName != nullptr ? displayName : "";

    auto found = std::find_if(insert->parameters.begin(), insert->parameters.end(),
                              [&](const neuracoust::daw::Vst3ParameterValueState& parameter) {
                                  return parameter.parameterId == parameterId;
                              });
    if (found != insert->parameters.end()) {
        found->normalizedValue = clamped;
        if (!name.empty()) {
            found->displayName = name;
        }
    } else {
        insert->parameters.push_back({parameterId,
                                      name.empty() ? "Param " + std::to_string(parameterId) : name,
                                      clamped});
    }

    // Fine-grained: never rebuild the graph for a knob turn.
    engine->engine.updateMasterVst3Parameter(static_cast<size_t>(slot), parameterId, name, clamped);
    return true;
}

bool nc_track_set_instrument(NCEngine* engine, int trackIndex, int pluginIndex) {
    auto* track = trackAt(engine, trackIndex);
    const auto* plugin = pluginAt(engine, pluginIndex);
    if (track == nullptr || plugin == nullptr) {
        return false;
    }

    neuracoust::daw::InstrumentSlotState instrument;
    instrument.pluginName = plugin->name;
    instrument.pluginFormat = plugin->format.empty() ? "VST3" : plugin->format;
    instrument.pluginPath = plugin->path;
    instrument.pluginClassId = plugin->pluginClassId;
    instrument.pluginClassName = plugin->pluginClassName;
    instrument.enabled = plugin->exists;
    instrument.bypassed = false;

    const std::string trackName = track->name;
    if (!neuracoust::daw::setTrackInstrumentSlot(engine->project, trackName, instrument)) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Load " + instrument.pluginName);
    return true;
}

void nc_track_instrument_name(NCEngine* engine, int trackIndex, char* out, size_t outLen) {
    const auto* track = trackAt(engine, trackIndex);
    copyText(out, outLen, track != nullptr ? track->instrument.pluginName : std::string{});
}

bool nc_track_add_insert(NCEngine* engine, int trackIndex, int pluginIndex) {
    auto* track = trackAt(engine, trackIndex);
    const auto* plugin = pluginAt(engine, pluginIndex);
    if (track == nullptr || plugin == nullptr) {
        return false;
    }

    const std::string trackName = track->name;

    // First free slot, else append one. addTrackInsertSlot enforces the ceiling.
    size_t slot = track->inserts.size();
    for (size_t index = 0; index < track->inserts.size(); ++index) {
        const auto& existing = track->inserts[index];
        if (!existing.enabled || existing.pluginName.empty() || existing.pluginName == "No Insert") {
            slot = index;
            break;
        }
    }
    if (slot >= track->inserts.size() &&
        !neuracoust::daw::addTrackInsertSlot(engine->project, trackName)) {
        return false;
    }

    neuracoust::daw::TrackInsertSlot insert;
    insert.pluginName = plugin->name;
    insert.pluginFormat = plugin->format.empty() ? "VST3" : plugin->format;
    insert.pluginPath = plugin->path;
    insert.pluginClassId = plugin->pluginClassId;
    insert.pluginClassName = plugin->pluginClassName;
    insert.bypassed = false;
    insert.enabled = plugin->exists;
    insert.dspAvailable = true;
    insert.dspExecutionMode = neuracoust::daw::defaultPluginInsertDspExecutionMode(
        engine->project, engine->monitorDspEnabled, insert);

    if (!neuracoust::daw::setTrackInsertSlot(engine->project, trackName, slot, insert)) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Add " + insert.pluginName);
    return true;
}

bool nc_track_remove_insert(NCEngine* engine, int trackIndex, int slot) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slot < 0) {
        return false;
    }
    if (!neuracoust::daw::removeTrackInsertSlot(engine->project, track->name,
                                                static_cast<size_t>(slot))) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Remove insert");
    return true;
}

int nc_track_move_insert(NCEngine* engine, int trackIndex, int slot, int direction) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slot < 0) {
        return -1;
    }
    const int moved = neuracoust::daw::moveTrackInsertSlot(engine->project, track->name,
                                                           static_cast<size_t>(slot), direction);
    if (moved < 0) {
        return -1;
    }
    engine->reconcileProject();
    engine->recordStep("Move insert");
    return moved;
}

// Moves a plugin to an arbitrary slot (drag-and-drop reorder), unlike the ±1 direction
// move. Returns the new index, or -1.
int nc_track_move_insert_to_index(NCEngine* engine, int trackIndex, int fromSlot, int toSlot) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || fromSlot < 0 || toSlot < 0) {
        return -1;
    }
    const int moved = neuracoust::daw::moveTrackInsertSlotToIndex(engine->project, track->name,
                                                                  static_cast<size_t>(fromSlot),
                                                                  static_cast<size_t>(toSlot));
    if (moved < 0) {
        return -1;
    }
    engine->reconcileProject();
    engine->recordStep("Move insert");
    return moved;
}

// Pro Tools-style positional move: place a plugin in an exact slot (A–E), leaving gaps.
// Inserts stay a packed vector, but empty slots (empty pluginPath) are padded in and the
// render skips them, so a plug-in can sit in slot C with A/B empty. Trailing empties are
// trimmed. Returns the target slot, or -1.
int nc_track_move_insert_to_slot(NCEngine* engine, int trackIndex, int fromSlot, int toSlot) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || fromSlot < 0 || toSlot < 0 || toSlot > 4 || fromSlot == toSlot) {
        return -1;
    }
    if (static_cast<size_t>(fromSlot) >= track->inserts.size() ||
        track->inserts[static_cast<size_t>(fromSlot)].pluginPath.empty()) {
        return -1;  // no plug-in at the source
    }
    const int need = std::max(fromSlot, toSlot) + 1;
    while (static_cast<int>(track->inserts.size()) < need) {
        track->inserts.push_back(neuracoust::daw::TrackInsertSlot{});
    }
    std::swap(track->inserts[static_cast<size_t>(fromSlot)], track->inserts[static_cast<size_t>(toSlot)]);
    while (!track->inserts.empty() && track->inserts.back().pluginPath.empty()) {
        track->inserts.pop_back();
    }
    engine->reconcileProject();
    engine->recordStep("Move insert");
    return toSlot;
}

void nc_track_insert_mode_badge(NCEngine* engine, int trackIndex, int slot, char* out, size_t outLen) {
    const auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) {
        copyText(out, outLen, "");
        return;
    }
    copyText(out, outLen,
             neuracoust::daw::effectiveInsertDspModeBadge(track->inserts[static_cast<size_t>(slot)],
                                                          engine->project));
}

// ---------------------------------------------------------------------------
// Monitor station
// ---------------------------------------------------------------------------

namespace {

MonitorDspModule* moduleAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.monitorModules.size()) {
        return nullptr;
    }
    return &engine->project.monitorModules[static_cast<size_t>(index)];
}

/// Per-slot accessors on the speaker-simulation module, which stores A/B/C as
/// three parallel fields rather than an array.
const std::string& speakerModelForSlot(const MonitorDspModule& module, int slot) {
    switch (slot) {
        case 1: return module.targetModelB;
        case 2: return module.targetModelC;
        default: return module.targetModelA;
    }
}

const std::string& speakerOutputForSlot(const MonitorDspModule& module, int slot) {
    switch (slot) {
        case 1: return module.speakerOutputB;
        case 2: return module.speakerOutputC;
        default: return module.speakerOutputA;
    }
}

} // namespace

int nc_monitor_module_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->project.monitorModules.size()) : 0;
}

void nc_monitor_module_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const MonitorDspModule* module = moduleAt(engine, index);
    copyText(out, outLen, module != nullptr ? module->displayName : std::string{});
}

void nc_monitor_module_detail(NCEngine* engine, int index, char* out, size_t outLen) {
    const MonitorDspModule* module = moduleAt(engine, index);
    if (module == nullptr) {
        copyText(out, outLen, "");
        return;
    }
    // Only the simulation modules carry a model string; the rest describe themselves
    // by stage.
    const std::string& model = speakerModelForSlot(*module, module->activeTargetSlot);
    copyText(out, outLen, model.empty() ? module->stage : model);
}

void nc_monitor_module_stage(NCEngine* engine, int index, char* out, size_t outLen) {
    const MonitorDspModule* module = moduleAt(engine, index);
    copyText(out, outLen, module != nullptr ? module->stage : std::string{});
}

bool nc_monitor_module_enabled(NCEngine* engine, int index) {
    const MonitorDspModule* module = moduleAt(engine, index);
    return module != nullptr && module->enabled;
}

void nc_monitor_set_module_enabled(NCEngine* engine, int index, bool enabled) {
    MonitorDspModule* module = moduleAt(engine, index);
    if (module == nullptr || module->enabled == enabled) {
        return;
    }
    module->enabled = enabled;
    engine->pushModules();
}

bool nc_monitor_dsp_enabled(NCEngine* engine) {
    return engine != nullptr && engine->monitorDspEnabled;
}

void nc_monitor_set_dsp_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->monitorDspEnabled == enabled) {
        return;
    }
    engine->monitorDspEnabled = enabled;
    engine->pushModules();
}

namespace {

/// Re-applies engine settings that only take effect at start() (the core hint) by
/// restarting the audio engine if it is running. A stopped engine just keeps the new
/// project values for its next start.
void restartEngineForSettings(NCEngine* engine) {
    if (!engine->engine.status().running) {
        return;
    }
    engine->engine.stop();
    std::string loadError;
    engine->engine.loadProject(engine->project, loadError);
    engine->engine.start(buildEngineSettings(engine));
    // start() builds a fresh DSP engine; only loadProject seeds its meter arrays.
    engine->engine.loadProject(engine->project, loadError);
}

} // namespace

// --- Plugin delay compensation (PDC). The engine computes per-path latency and aligns
//     tracks; the flag is read at start(), so toggling restarts the audio engine. ---
bool nc_delay_compensation_enabled(NCEngine* engine) {
    return engine != nullptr && engine->delayCompensationEnabled;
}
void nc_delay_compensation_set(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->delayCompensationEnabled == enabled) return;
    engine->delayCompensationEnabled = enabled;
    restartEngineForSettings(engine);
}
double nc_delay_compensation_ms(NCEngine* engine) {
    return engine == nullptr ? 0.0 : engine->engine.status().delayCompensationMs;
}
int nc_delay_compensation_samples(NCEngine* engine) {
    return engine == nullptr ? 0 : engine->engine.status().delayCompensationSamples;
}

bool nc_dsp_core_isolation(NCEngine* engine) {
    return engine != nullptr && engine->project.appleSiliconCoreIsolationEnabled;
}

void nc_dsp_set_core_isolation(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->project.appleSiliconCoreIsolationEnabled == enabled) {
        return;
    }
    engine->project.appleSiliconCoreIsolationEnabled = enabled;
    // The old UI keeps a floor of 4 cores whenever isolation is on.
    if (enabled && engine->project.requestedDspCoreCount < 4) {
        engine->project.requestedDspCoreCount = 4;
    }
    engine->recordStep(enabled ? "Enable core isolation" : "Disable core isolation");
    restartEngineForSettings(engine);
}

int nc_dsp_core_count(NCEngine* engine) {
    return engine != nullptr ? std::max(1, std::min(16, engine->project.requestedDspCoreCount)) : 0;
}

void nc_dsp_set_core_count(NCEngine* engine, int count) {
    if (engine == nullptr) return;
    int clamped = std::max(1, std::min(16, count));
    if (engine->project.appleSiliconCoreIsolationEnabled) {
        clamped = std::max(4, clamped);
    }
    if (clamped == engine->project.requestedDspCoreCount) {
        return;
    }
    engine->project.requestedDspCoreCount = clamped;
    engine->recordStep("Set DSP core count");
    restartEngineForSettings(engine);
}

namespace {

// Output-capable devices, cached so a right-click menu does not re-scan CoreAudio on
// every open. The scan is cheap but the caller reads it item by item.
std::vector<neuracoust::daw::AudioDeviceInfo>& outputDeviceCache() {
    static std::vector<neuracoust::daw::AudioDeviceInfo> devices;
    return devices;
}

} // namespace

int nc_output_device_count(NCEngine* engine) {
    (void)engine;
    auto& cache = outputDeviceCache();
    cache.clear();
    for (const auto& device : neuracoust::daw::enumerateAudioDevices()) {
        if (device.outputChannels > 0) {
            cache.push_back(device);
        }
    }
    return static_cast<int>(cache.size());
}

void nc_output_device_id(NCEngine* engine, int index, char* out, size_t outLen) {
    (void)engine;
    const auto& cache = outputDeviceCache();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < cache.size())
                              ? cache[static_cast<size_t>(index)].id : std::string{});
}

void nc_output_device_name(NCEngine* engine, int index, char* out, size_t outLen) {
    (void)engine;
    const auto& cache = outputDeviceCache();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < cache.size())
                              ? cache[static_cast<size_t>(index)].name : std::string{});
}

void nc_current_output_device_id(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->outputDeviceId : std::string{});
}

/// The device the engine actually opened, so the UI can show the default's real name.
void nc_active_output_device_name(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->engine.status().deviceName : std::string{});
}

void nc_set_output_device(NCEngine* engine, const char* deviceId) {
    if (engine == nullptr) return;
    const std::string next = deviceId != nullptr ? deviceId : "";
    if (next == engine->outputDeviceId) {
        return;
    }
    engine->outputDeviceId = next;
    restartEngineForSettings(engine);
}

namespace {

// Input-capable devices — BlackHole and other loopbacks show up here so reference
// music can be monitored. Cached the same way as the output list.
std::vector<neuracoust::daw::AudioDeviceInfo>& inputDeviceCache() {
    static std::vector<neuracoust::daw::AudioDeviceInfo> devices;
    return devices;
}

} // namespace

int nc_input_device_count(NCEngine* engine) {
    (void)engine;
    auto& cache = inputDeviceCache();
    cache.clear();
    for (const auto& device : neuracoust::daw::enumerateAudioDevices()) {
        if (device.inputChannels > 0) {
            cache.push_back(device);
        }
    }
    return static_cast<int>(cache.size());
}

void nc_input_device_id(NCEngine* engine, int index, char* out, size_t outLen) {
    (void)engine;
    const auto& cache = inputDeviceCache();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < cache.size())
                              ? cache[static_cast<size_t>(index)].id : std::string{});
}

void nc_input_device_name(NCEngine* engine, int index, char* out, size_t outLen) {
    (void)engine;
    const auto& cache = inputDeviceCache();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < cache.size())
                              ? cache[static_cast<size_t>(index)].name : std::string{});
}

void nc_current_input_device_id(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->inputDeviceId : std::string{});
}

void nc_set_input_device(NCEngine* engine, const char* deviceId) {
    if (engine == nullptr) return;
    const std::string next = deviceId != nullptr ? deviceId : "";
    if (next == engine->inputDeviceId) {
        return;
    }
    engine->inputDeviceId = next;
    restartEngineForSettings(engine);
}

void nc_monitor_path_mode(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->monitorDspPathMode : std::string{});
}

void nc_monitor_set_path_mode(NCEngine* engine, const char* mode) {
    if (engine == nullptr || mode == nullptr) {
        return;
    }
    engine->monitorDspPathMode = mode;
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode,
                                         buildRemoteDspSettings(engine));
}

int nc_dsp_external_core_count(NCEngine* engine) {
    return engine != nullptr ? std::max(1, std::min(16, engine->project.externalDspCoreCount)) : 0;
}

void nc_dsp_set_external_core_count(NCEngine* engine, int count) {
    if (engine == nullptr) return;
    const int clamped = std::max(1, std::min(16, count));
    if (clamped == engine->project.externalDspCoreCount) {
        return;
    }
    engine->project.externalDspCoreCount = clamped;
    engine->recordStep("Set external DSP core count");
    // The remote reserve feeds through setMonitorDspPathMode, so re-apply the current
    // path to push the new hint live — no full audio restart needed.
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode,
                                         buildRemoteDspSettings(engine));
}

void nc_dsp_remote_host(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.remoteDspHost : std::string{});
}

void nc_dsp_set_remote_host(NCEngine* engine, const char* host) {
    if (engine == nullptr) return;
    std::string next = host != nullptr ? host : "";
    // Trim surrounding whitespace so a stray paste does not become the hostname.
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    next.erase(next.begin(), std::find_if(next.begin(), next.end(), notSpace));
    next.erase(std::find_if(next.rbegin(), next.rend(), notSpace).base(), next.end());
    if (next.empty()) next = "studio.local";
    if (next == engine->project.remoteDspHost) return;
    engine->project.remoteDspHost = next;
    engine->recordStep("Set remote DSP host");
    // Re-apply the monitor path live so the stream retargets without an audio restart.
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode,
                                         buildRemoteDspSettings(engine));
}

/// Broadcast-discover a node and return its address, or "" if none answered. Lets the
/// UI fill the host field without the user typing an IP.
void nc_dsp_discover_remote_host(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, std::string{});
    const auto settings = engine != nullptr ? buildRemoteDspSettings(engine)
                                            : neuracoust::daw::defaultRemoteDspServerSettings();
    for (const auto& found : neuracoust::daw::discoverRemoteDspServers(settings, {}, 600)) {
        if (!found.node.host.empty()) {
            copyText(out, outLen, found.node.host);
            return;
        }
    }
}

float nc_monitor_volume_db(NCEngine* engine) {
    return engine != nullptr ? engine->project.monitorVolumeDb : 0.0f;
}

void nc_monitor_set_volume_db(NCEngine* engine, float db) {
    if (engine == nullptr) {
        return;
    }
    engine->project.monitorVolumeDb = std::max(-60.0f, std::min(6.0f, db));
    engine->pushStationControls();
}

bool nc_monitor_mono(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorStationMono;
}

bool nc_monitor_mute(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorStationMute;
}

bool nc_monitor_dim(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorStationDim;
}

bool nc_monitor_talkback(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorStationTalkback;
}

void nc_monitor_set_mono(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->project.monitorStationMono = on;
    engine->pushStationControls();
}

void nc_monitor_set_mute(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->project.monitorStationMute = on;
    engine->pushStationControls();
}

void nc_monitor_set_dim(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->project.monitorStationDim = on;
    engine->pushStationControls();
}

// Master (false, default) vs the computer's input source (true) for the monitor bus.
bool nc_monitor_listen_source(NCEngine* engine) {
    return engine != nullptr && engine->monitorListenSource;
}

void nc_monitor_set_listen_source(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->monitorListenSource = on;
    engine->engine.setMonitorListenSource(on);
}

double nc_insert_tail_on_stop_seconds(NCEngine* engine) {
    return engine != nullptr ? engine->insertTailOnStopSeconds : 0.0;
}

void nc_set_insert_tail_on_stop_seconds(NCEngine* engine, double seconds) {
    if (engine == nullptr) return;
    engine->insertTailOnStopSeconds = seconds;  // <0 = always on
    engine->engine.setInsertTailOnStopSeconds(seconds);
}

void nc_monitor_set_talkback(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->project.monitorStationTalkback = on;
    engine->pushStationControls();
}

void nc_monitor_listen_mode(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.monitorStationListenMode : std::string{});
}

void nc_monitor_set_listen_mode(NCEngine* engine, const char* mode) {
    if (engine == nullptr || mode == nullptr) return;
    engine->project.monitorStationListenMode = mode;
    engine->pushStationControls();
}

namespace {

// The exact state machine from the old UI's monitorStationButtonChanged:, kept next
// to the engine because it manipulates the same project model.
bool isMidSide(const neuracoust::daw::ProjectDocument& p) {
    return p.monitorStationListenMode == "M" || p.monitorStationListenMode == "S";
}

void applyStationChange(NCEngine* engine) {
    neuracoust::daw::normalizeMonitorStationProjectState(engine->project);
    engine->pushStationControls();
}

} // namespace

void nc_monitor_cycle_stereo(NCEngine* engine) {
    if (engine == nullptr) return;
    auto& p = engine->project;
    if (isMidSide(p)) {
        p.monitorStationMono = false;
        p.monitorStationListenMode = "M";
        p.monitorStationSwapLeftRight = false;
    } else if (p.monitorStationMono) {
        p.monitorStationMono = false;
        p.monitorStationListenMode = "LR";
    } else if (p.monitorStationListenMode == "LR") {
        p.monitorStationListenMode = "L";
    } else if (p.monitorStationListenMode == "L") {
        p.monitorStationListenMode = "R";
    } else {
        p.monitorStationListenMode = "LR";
    }
    applyStationChange(engine);
}

void nc_monitor_cycle_mono(NCEngine* engine) {
    if (engine == nullptr) return;
    auto& p = engine->project;
    if (isMidSide(p)) {
        p.monitorStationMono = false;
        p.monitorStationListenMode = "S";
        p.monitorStationSwapLeftRight = false;
    } else if (p.monitorStationMono && p.monitorStationListenMode == "L") {
        p.monitorStationMono = true;
        p.monitorStationListenMode = "R";
    } else if (p.monitorStationMono && p.monitorStationListenMode == "R") {
        p.monitorStationMono = true;
        p.monitorStationListenMode = "LR";
    } else if (p.monitorStationMono) {
        p.monitorStationMono = true;
        p.monitorStationListenMode = "L";
    } else {
        p.monitorStationMono = true;
        p.monitorStationListenMode = "LR";
    }
    applyStationChange(engine);
}

void nc_monitor_toggle_mid_side(NCEngine* engine) {
    if (engine == nullptr) return;
    auto& p = engine->project;
    p.monitorStationMono = false;
    p.monitorStationSwapLeftRight = false;
    p.monitorStationListenMode = isMidSide(p) ? "LR" : "M";
    applyStationChange(engine);
}

void nc_monitor_cycle_phase(NCEngine* engine) {
    if (engine == nullptr) return;
    auto& p = engine->project;
    const bool l = p.monitorStationInvertLeft;
    const bool r = p.monitorStationInvertRight;
    if (!l && !r) {
        p.monitorStationInvertLeft = true;
    } else if (l && !r) {
        p.monitorStationInvertLeft = false;
        p.monitorStationInvertRight = true;
    } else if (!l && r) {
        p.monitorStationInvertLeft = true;
        p.monitorStationInvertRight = true;
    } else {
        p.monitorStationInvertLeft = false;
        p.monitorStationInvertRight = false;
    }
    applyStationChange(engine);
}

bool nc_monitor_mid_side(NCEngine* engine) {
    return engine != nullptr && isMidSide(engine->project);
}

bool nc_monitor_invert_left(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorStationInvertLeft;
}

bool nc_monitor_invert_right(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorStationInvertRight;
}

int nc_monitor_active_speaker_slot(NCEngine* engine) {
    if (engine == nullptr) return 0;
    const MonitorDspModule* module = engine->speakerSimulation();
    return module != nullptr ? module->activeTargetSlot : 0;
}

void nc_monitor_set_active_speaker_slot(NCEngine* engine, int slot) {
    if (engine == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr || module->activeTargetSlot == slot) return;
    module->activeTargetSlot = slot;
    engine->pushModules();
}

void nc_monitor_speaker_model(NCEngine* engine, int slot, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    const MonitorDspModule* module = engine->speakerSimulation();
    copyText(out, outLen, module != nullptr ? speakerModelForSlot(*module, slot) : std::string{});
}

void nc_monitor_speaker_output(NCEngine* engine, int slot, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    const MonitorDspModule* module = engine->speakerSimulation();
    copyText(out, outLen, module != nullptr ? speakerOutputForSlot(*module, slot) : std::string{});
}

float nc_monitor_speaker_sim_weight(NCEngine* engine, int slot) {
    if (engine == nullptr) return 0.0f;
    const MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return 0.0f;
    switch (slot) {
        case 1: return module->speakerSimulationWeightB;
        case 2: return module->speakerSimulationWeightC;
        default: return module->speakerSimulationWeightA;
    }
}

bool nc_monitor_speaker_room_eq(NCEngine* engine, int slot) {
    if (engine == nullptr) return false;
    const MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return false;
    switch (slot) {
        case 1: return module->speakerRoomEqB;
        case 2: return module->speakerRoomEqC;
        default: return module->speakerRoomEqA;
    }
}

namespace {

const char* slotLetter(int slot) { return slot == 1 ? "B" : (slot == 2 ? "C" : "A"); }

std::string* speakerModelFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.targetModelB : slot == 2 ? &m.targetModelC : &m.targetModelA;
}
std::string* speakerOutputFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.speakerOutputB : slot == 2 ? &m.speakerOutputC : &m.speakerOutputA;
}
bool* speakerRoomEqFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.speakerRoomEqB : slot == 2 ? &m.speakerRoomEqC : &m.speakerRoomEqA;
}

// The physical monitor output routes, ported verbatim from the old UI's
// monitorPhysicalOutputRoutes(). "None" means the modelled/virtual path; the rest send
// the slot straight to a hardware output pair (MonitorOutputRouting resolves them).
const std::vector<std::string>& speakerOutputRouteCatalog() {
    static const std::vector<std::string> routes = {
        "None", "Main 1-2",
        "Output 1-2", "Output 3-4", "Output 5-6", "Output 7-8",
        "Output 9-10", "Output 11-12", "Output 13-14", "Output 15-16",
        "Output 17-18", "Output 19-20", "Output 21-22", "Output 23-24",
        "Output 25-26", "Output 27-28", "Output 29-30", "Output 31-32",
    };
    return routes;
}

// The speaker-model catalog, ported from the old UI's speakerModelBaseCatalog(). The
// (NF/MF/LF) suffix is the field category. The name drives the monitor tone model.
const std::vector<std::string>& speakerModelCatalog() {
    static const std::vector<std::string> models = {
        "Flat",
        "Yamaha NS-10 (NF)", "Yamaha NS-10M (NF)", "Yamaha NS-10M Pro (NF)", "Yamaha NS-10M Studio (NF)", "Yamaha HS3 (NF)", "Yamaha HS4 (NF)", "Yamaha HS5 (NF)", "Yamaha HS7 (NF)", "Yamaha HS8 (NF)", "Yamaha MSP3A (NF)", "Yamaha MSP5 Studio (NF)", "Yamaha MSP7 Studio (NF)",
        "Auratone 5C Sound Cube (NF)", "Avantone Pro MixCube Active (NF)", "Avantone Pro MixCube Passive (NF)", "Avantone Pro CLA-10 Passive (NF)", "Avantone Pro CLA-10 Active (NF)", "Avantone Pro CLA-10A (NF)", "Avantone Pro CLA-10A Limited Edition (NF)", "Avantone Pro Gauss 7 (NF)",
        "Genelec 8010A (NF)", "Genelec 8020D (NF)", "Genelec 8030C (NF)", "Genelec 8040B (NF)", "Genelec 8050B (NF)", "Genelec 8320A (NF)", "Genelec 8330A (NF)", "Genelec 8331A (NF)", "Genelec 8341A (MF)", "Genelec 8351B (MF)", "Genelec 8361A (MF)", "Genelec S360A (MF)", "Genelec 1030A (NF)", "Genelec 1031A (MF)", "Genelec 1032A (MF)", "Genelec 1037C (LF)", "Genelec 1038C (LF)",
        "Neumann KH 80 DSP (NF)", "Neumann KH 120 II (NF)", "Neumann KH 150 (NF)", "Neumann KH 310 (MF)", "Neumann KH 420 (MF)",
        "ADAM T5V (NF)", "ADAM T7V (NF)", "ADAM T8V (NF)", "ADAM A3X (NF)", "ADAM A4V (NF)", "ADAM A44H (NF)", "ADAM A5X (NF)", "ADAM A7V (NF)", "ADAM A7X (NF)", "ADAM A77H (MF)", "ADAM A8H (MF)", "ADAM S2V (NF)", "ADAM S3V (MF)", "ADAM S3H (MF)", "ADAM S5V (LF)", "ADAM S5H (LF)", "ADAM S6X (LF)",
        "Focal Alpha 50 Evo (NF)", "Focal Alpha 65 Evo (NF)", "Focal Alpha Twin Evo (MF)", "Focal Shape 40 (NF)", "Focal Shape 50 (NF)", "Focal Shape 65 (NF)", "Focal Solo6 Be (NF)", "Focal Solo6 ST6 (NF)", "Focal Twin6 Be (MF)", "Focal Twin6 ST6 (MF)", "Focal Trio6 Be (MF)", "Focal Trio6 ST6 (MF)", "Focal Trio11 Be (MF)", "Focal SM9 (MF)", "Focal Grande Utopia EM (LF)",
        "Dynaudio BM5A (NF)", "Dynaudio BM6A (NF)", "Dynaudio BM15A (MF)", "Dynaudio LYD 5 (NF)", "Dynaudio LYD 7 (NF)", "Dynaudio LYD 8 (NF)", "Dynaudio LYD 48 (MF)", "Dynaudio Core 5 (NF)", "Dynaudio Core 7 (NF)", "Dynaudio Core 47 (MF)", "Dynaudio Core 59 (MF)", "Dynaudio M3VE (LF)",
        "KRK 9000B (NF)", "KRK Rokit 5 G4 (NF)", "KRK Rokit 7 G4 (NF)", "KRK Rokit 8 G4 (NF)", "KRK V4 (NF)", "KRK V6 (NF)", "KRK V8 (NF)", "KRK Expose E8B (MF)",
        "JBL 305P MkII (NF)", "JBL 306P MkII (NF)", "JBL 308P MkII (NF)", "JBL 705P (NF)", "JBL 708P (MF)", "JBL 4312 (MF)", "JBL 4329P (MF)", "JBL LSR6328P (MF)", "JBL M2 (LF)",
        "Mackie HR624 (NF)", "Mackie HR824 (NF)", "PreSonus Eris E5 (NF)", "PreSonus Eris E8 (NF)", "Kali LP-6 (NF)", "Kali LP-8 (NF)", "Kali IN-5 (NF)", "Kali IN-8 (MF)",
        "EVE Audio SC205 (NF)", "EVE Audio SC207 (NF)", "EVE Audio SC307 (MF)", "HEDD Type 05 MK2 (NF)", "HEDD Type 07 MK2 (NF)", "HEDD Type 20 MK2 (MF)", "HEDD Type 30 MK2 (MF)",
        "Amphion One12 (NF)", "Amphion One15 (NF)", "Amphion One18 (NF)", "Amphion One25A (MF)", "Amphion Two15 (MF)", "Amphion Two18 (MF)",
        "ATC SCM12 Pro (NF)", "ATC SCM20ASL Pro (NF)", "ATC SCM25A Pro (MF)", "ATC SCM25A (MF)", "ATC SCM45A Pro (MF)", "ATC SCM45A (MF)", "ATC SCM50ASL Pro (MF)", "ATC SCM50A (MF)", "ATC SCM100ASL Pro (LF)", "ATC SCM100A (LF)", "ATC SCM150ASL Pro (LF)",
        "PMC Result6 (NF)", "PMC twotwo.5 (NF)", "PMC twotwo.6 (NF)", "PMC twotwo.8 (MF)", "PMC 6 (NF)", "PMC 6-2 (MF)", "PMC 8-2 (MF)", "PMC IB1S-AIII (MF)", "PMC MB2S XBD (LF)", "PMC BB6 XBD (LF)",
        "Barefoot Footprint01 (MF)", "Barefoot Footprint02 (MF)", "Barefoot Footprint03 (NF)", "Barefoot MicroMain26 (MF)", "Barefoot MicroMain27 (MF)", "Barefoot MicroMain45 (MF)", "Barefoot MiniMain12 (LF)", "Barefoot MasterStack12 (LF)",
        "Quested S7R (NF)", "Quested V2108 (MF)", "Quested VH3208 (LF)", "Ocean Way HR5 (MF)", "Ocean Way HR4 (MF)", "Ocean Way HR3 (LF)", "Ocean Way HR2 (LF)", "Augspurger Duo 8 (MF)", "Augspurger Duo 12 (LF)", "Augspurger Duo 15 (LF)", "Meyer Sound Amie (MF)", "Meyer Sound Bluehorn (LF)",
        "Kii THREE (MF)", "Dutch & Dutch 8c (MF)", "GGNTKT M1 (MF)", "PSI Audio A17-M (NF)", "PSI Audio A21-M (MF)", "PSI Audio A25-M (MF)", "Manger P1 (MF)", "Unity Audio The Rock MkII (NF)", "Unity Audio Boulder MkIII (MF)",
        "Klein + Hummel O 300 (MF)", "Tannoy Gold 5 (NF)", "Tannoy Gold 8 (NF)", "Tannoy Reveal 502 (NF)", "Tannoy Reveal 802 (NF)", "Tannoy System 600 (NF)", "Tannoy System 800 (MF)", "Westlake BBSM-10 (MF)", "Westlake BBSM-15 (LF)",
        "Laptop", "Phone Speaker", "Small Bluetooth Speaker", "TV Speaker", "Car Stereo", "Club PA",
        "YouTube AAC Preview", "Spotify Ogg Preview", "Apple Music AAC Preview", "Tidal HiFi Preview", "Broadcast Loudness Preview",
    };
    return models;
}

} // namespace

int nc_speaker_model_count() {
    return static_cast<int>(speakerModelCatalog().size());
}

void nc_speaker_model_name(int index, char* out, size_t outLen) {
    const auto& catalog = speakerModelCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < catalog.size())
                              ? catalog[static_cast<size_t>(index)] : std::string{});
}

namespace {
// Physical headphone models the user might monitor on, for the 헤드폰 model picker.
const std::vector<std::string>& headphoneModelCatalog() {
    static const std::vector<std::string> models = {
        "Flat",
        "Sennheiser HD 600", "Sennheiser HD 650", "Sennheiser HD 660S", "Sennheiser HD 800S", "Sennheiser HD 25", "Sennheiser HD 280 Pro",
        "Beyerdynamic DT 770 Pro", "Beyerdynamic DT 880 Pro", "Beyerdynamic DT 990 Pro", "Beyerdynamic DT 1990 Pro",
        "AKG K240 Studio", "AKG K271 MkII", "AKG K371", "AKG K702", "AKG K712 Pro",
        "Audio-Technica ATH-M50x", "Audio-Technica ATH-M40x", "Audio-Technica ATH-R70x",
        "Sony MDR-7506", "Sony MDR-CD900ST", "Sony MDR-M1ST",
        "Focal Clear Pro", "Focal Listen Pro", "Audeze LCD-X", "Audeze MM-500", "HIFIMAN Sundara", "HIFIMAN Arya",
        "Shure SRH840A", "Shure SRH1540", "Grado SR325x", "Neumann NDH 20", "Neumann NDH 30", "Slate VSX",
        "Apple AirPods Pro", "Apple AirPods Max", "Bose QC", "Sony WH-1000XM5", "Earbuds (generic)",
    };
    return models;
}
}

int nc_headphone_model_count() {
    return static_cast<int>(headphoneModelCatalog().size());
}

void nc_headphone_model_name(int index, char* out, size_t outLen) {
    const auto& catalog = headphoneModelCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < catalog.size())
                              ? catalog[static_cast<size_t>(index)] : std::string{});
}

void nc_monitor_physical_speaker_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalSpeakerModel : std::string{});
}
void nc_monitor_set_physical_speaker_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr) return;
    if (engine->project.physicalSpeakerModel == model) return;
    engine->project.physicalSpeakerModel = model;
    engine->recordStep("Set physical speaker");
}
void nc_monitor_physical_headphone_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalHeadphoneModel : std::string{});
}
void nc_monitor_set_physical_headphone_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr) return;
    if (engine->project.physicalHeadphoneModel == model) return;
    engine->project.physicalHeadphoneModel = model;
    engine->recordStep("Set physical headphone");
}
bool nc_monitor_output_exclusive(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorSpeakerHeadphoneExclusive;
}
void nc_monitor_set_output_exclusive(NCEngine* engine, bool exclusive) {
    if (engine == nullptr || engine->project.monitorSpeakerHeadphoneExclusive == exclusive) return;
    engine->project.monitorSpeakerHeadphoneExclusive = exclusive;
    engine->recordStep(exclusive ? "Enable speaker/headphone exclusive" : "Disable speaker/headphone exclusive");
}

namespace {
// Fade-out amplitude curve at normalized position t (0 = full, 1 = silent), as a dB
// offset. Mirrors the old UI's curve set.
float autoFadeGainDb(const std::string& curve, double t) {
    t = std::max(0.0, std::min(1.0, t));
    double amp;
    if (curve == "linear") amp = 1.0 - t;
    else if (curve == "exponential") amp = (1.0 - t) * (1.0 - t) * (1.0 - t);   // slow then fast
    else if (curve == "logarithmic") amp = 1.0 - t * t * t;                     // fast then slow
    else if (curve == "s_curve") amp = 0.5 * (1.0 + std::cos(t * 3.14159265358979323846)); // ease both ends
    else amp = std::cos(t * 3.14159265358979323846 / 2.0);                      // equal_power
    if (amp <= 1e-6) return -120.0f;
    return static_cast<float>(20.0 * std::log10(amp));
}

double projectContentEnd(const ProjectDocument& project) {
    double end = 0.0;
    for (const auto& clip : project.clips) end = std::max(end, clip.startSeconds + clip.durationSeconds);
    for (const auto& region : project.midiRegions) end = std::max(end, region.startSeconds + region.durationSeconds);
    return end;
}

neuracoust::daw::TrackState* masterTrackPtr(NCEngine* engine) {
    for (auto& track : engine->project.tracks) {
        if (track.trackType == "master" || track.name == "Master") return &track;
    }
    return nullptr;
}

// Auto-fade owns the Master track's volume automation: rebuild it from the fade setting.
void applyMasterAutoFade(NCEngine* engine) {
    auto* master = masterTrackPtr(engine);
    if (master == nullptr) return;
    master->volumeAutomation.clear();
    const double seconds = engine->project.autoFadeOutSeconds;
    const double end = projectContentEnd(engine->project);
    if (seconds > 0.0 && end > 0.0) {
        const double start = std::max(0.0, end - seconds);
        const std::string& curve = engine->project.autoFadeOutCurve;
        const int steps = 24;
        for (int i = 0; i <= steps; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(steps);
            neuracoust::daw::AutomationPointState point;
            point.timeSeconds = start + t * (end - start);
            point.value = std::max(-120.0f, std::min(12.0f, master->volumeDb + autoFadeGainDb(curve, t)));
            master->volumeAutomation.push_back(point);
        }
    }
    engine->reconcileProject();
}
} // namespace

double nc_master_auto_fade_seconds(NCEngine* engine) {
    return engine != nullptr ? engine->project.autoFadeOutSeconds : 0.0;
}

void nc_master_set_auto_fade_seconds(NCEngine* engine, double seconds) {
    if (engine == nullptr) return;
    engine->project.autoFadeOutSeconds = std::max(0.0, std::min(600.0, seconds));
    applyMasterAutoFade(engine);
    engine->recordStep("Set auto fade-out");
}

void nc_master_auto_fade_curve(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.autoFadeOutCurve : std::string{});
}

void nc_master_set_auto_fade_curve(NCEngine* engine, const char* curve) {
    if (engine == nullptr || curve == nullptr) return;
    engine->project.autoFadeOutCurve = curve;
    applyMasterAutoFade(engine);
    engine->recordStep("Set auto fade-out curve");
}

// The fade's gain (0..1 amplitude) at normalized position, for the UI curve preview.
float nc_auto_fade_amplitude(const char* curve, double t) {
    const std::string key = curve != nullptr ? curve : "equal_power";
    const float db = autoFadeGainDb(key, t);
    return db <= -119.0f ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
}

int nc_speaker_output_route_count() {
    return static_cast<int>(speakerOutputRouteCatalog().size());
}

void nc_speaker_output_route(int index, char* out, size_t outLen) {
    const auto& routes = speakerOutputRouteCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < routes.size())
                              ? routes[static_cast<size_t>(index)] : std::string{});
}

void nc_monitor_set_speaker_model(NCEngine* engine, int slot, const char* model) {
    if (engine == nullptr || model == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    const std::string stored = std::string("Speaker ") + slotLetter(slot) + ": " + model;
    std::string* field = speakerModelFieldForSlot(*module, slot);
    if (*field == stored) return;
    *field = stored;
    // A modelled speaker means the slot is not a raw physical passthrough.
    *speakerOutputFieldForSlot(*module, slot) = "None";
    engine->recordStep("Set speaker model");
    engine->pushModules();
}

void nc_monitor_set_speaker_output(NCEngine* engine, int slot, const char* route) {
    if (engine == nullptr || route == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    const std::string value = route;
    std::string* out = speakerOutputFieldForSlot(*module, slot);
    if (*out == value) return;
    *out = value;
    // Choosing a physical output bypasses the virtual model: force Flat + room EQ off,
    // the reference rule. "None" leaves the modelled path in place.
    if (value != "None") {
        *speakerModelFieldForSlot(*module, slot) = std::string("Speaker ") + slotLetter(slot) + ": Flat";
        *speakerRoomEqFieldForSlot(*module, slot) = false;
    }
    engine->recordStep("Set speaker output");
    engine->pushModules();
}

void nc_monitor_set_speaker_room_eq(NCEngine* engine, int slot, bool enabled) {
    if (engine == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    bool* field = speakerRoomEqFieldForSlot(*module, slot);
    if (*field == enabled) return;
    *field = enabled;
    engine->recordStep("Toggle speaker room EQ");
    engine->pushModules();
}

// ---------------------------------------------------------------------------
// Live MIDI input — monitor a keyboard through an armed instrument track
// ---------------------------------------------------------------------------

namespace {

bool recordedMidiEventToVst3Event(const neuracoust::daw::RecordedMidiEvent& recorded,
                                  neuracoust::daw::Vst3MidiEvent& vstEvent) {
    using neuracoust::daw::RecordedMidiEventKind;
    using neuracoust::daw::Vst3MidiEventKind;
    vstEvent = {};
    vstEvent.frameOffset = 0;
    vstEvent.channel = std::max(1, std::min(16, recorded.channel));
    switch (recorded.kind) {
    case RecordedMidiEventKind::NoteOn:
    case RecordedMidiEventKind::NoteOff:
        vstEvent.kind = Vst3MidiEventKind::Note;
        vstEvent.pitch = std::max(0, std::min(127, recorded.pitch));
        vstEvent.velocity = recorded.kind == RecordedMidiEventKind::NoteOn
            ? std::max(1, std::min(127, recorded.velocity)) : 0;
        vstEvent.noteOn = recorded.kind == RecordedMidiEventKind::NoteOn;
        return true;
    case RecordedMidiEventKind::Controller:
        vstEvent.kind = Vst3MidiEventKind::Controller;
        vstEvent.controller = std::max(0, std::min(127, recorded.controller));
        vstEvent.value = std::max(0, std::min(127, recorded.value));
        return true;
    case RecordedMidiEventKind::PitchBend:
        vstEvent.kind = Vst3MidiEventKind::PitchBend;
        vstEvent.value = std::max(0, std::min(16383, recorded.value));
        return true;
    case RecordedMidiEventKind::ProgramChange:
        vstEvent.kind = Vst3MidiEventKind::ProgramChange;
        vstEvent.program = std::max(0, std::min(127, recorded.program));
        return true;
    }
    return false;
}

} // namespace

int nc_midi_input_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->midiInputRecorder.availableInputs().size()) : 0;
}

void nc_midi_input_id(NCEngine* engine, int index, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    const auto inputs = engine->midiInputRecorder.availableInputs();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < inputs.size())
                              ? inputs[static_cast<size_t>(index)].id : std::string{});
}

void nc_midi_input_name(NCEngine* engine, int index, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    const auto inputs = engine->midiInputRecorder.availableInputs();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < inputs.size())
                              ? inputs[static_cast<size_t>(index)].name : std::string{});
}

bool nc_midi_live_start(NCEngine* engine, const char* sourceId) {
    if (engine == nullptr) return false;
    return engine->midiInputRecorder.start(sourceId != nullptr ? sourceId : "");
}

void nc_midi_live_stop(NCEngine* engine) {
    if (engine == nullptr) return;
    std::vector<neuracoust::daw::RecordedMidiEvent> ignored;
    std::string error;
    engine->midiInputRecorder.stop(ignored, error);
}

bool nc_midi_live_active(NCEngine* engine) {
    return engine != nullptr && engine->midiInputRecorder.status().recording;
}

float nc_midi_input_activity(NCEngine* engine) {
    if (engine == nullptr) return 0.0f;
    const float value = engine->midiInputActivity;
    engine->midiInputActivity = 0.0f;   // consume: the UI keeps its own decay
    return value;
}

void nc_midi_pump_live_input(NCEngine* engine) {
    if (engine == nullptr || !engine->midiInputRecorder.status().recording) return;
    const auto pending = engine->midiInputRecorder.consumePendingEvents();
    if (pending.empty()) return;
    // Track the loudest thing seen for the input meter (velocity / CC / bend magnitude).
    using neuracoust::daw::RecordedMidiEventKind;
    for (const auto& e : pending) {
        float level = 0.0f;
        switch (e.kind) {
        case RecordedMidiEventKind::NoteOn:     level = std::max(0, std::min(127, e.velocity)) / 127.0f; break;
        case RecordedMidiEventKind::Controller: level = std::max(0, std::min(127, e.value)) / 127.0f; break;
        case RecordedMidiEventKind::PitchBend:  level = std::abs(e.value - 8192) / 8192.0f; break;
        case RecordedMidiEventKind::ProgramChange: level = 0.45f; break;
        default: break;
        }
        engine->midiInputActivity = std::max(engine->midiInputActivity, level);
    }
    std::vector<neuracoust::daw::Vst3MidiEvent> liveEvents;
    liveEvents.reserve(pending.size());
    for (const auto& event : pending) {
        neuracoust::daw::Vst3MidiEvent v;
        if (recordedMidiEventToVst3Event(event, v)) liveEvents.push_back(v);
    }
    if (liveEvents.empty()) return;
    // Every armed / input-monitoring instrument track hears the keyboard. A track with
    // no instrument plug-in simply renders nothing, so no extra guard is needed.
    for (const auto& track : engine->project.tracks) {
        if (track.trackType != "instrument") continue;
        if (!(track.recordArmed || track.inputMonitoring)) continue;
        engine->engine.queueLiveMidiEvents(track.name, liveEvents);
    }
}

// ---------------------------------------------------------------------------
// Listen Room
// ---------------------------------------------------------------------------

namespace {

/// Ambiguity-free alphabet: no O/0, no I/l/1.
std::string generateListenAccessToken() {
    static const char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::string token;
    token.reserve(18);
    for (int index = 0; index < 18; ++index) {
        token.push_back(alphabet[arc4random_uniform(static_cast<uint32_t>(sizeof(alphabet) - 1))]);
    }
    return token;
}

} // namespace

void nc_listen_status(NCEngine* engine, NCListenStatus* out) {
    if (out == nullptr) {
        return;
    }
    std::memset(out, 0, sizeof(*out));
    if (engine == nullptr) {
        return;
    }

    const auto status = engine->engine.status().listenRoom;
    out->enabled = status.enabled;
    out->senderRunning = status.senderRunning;
    out->relayReachable = status.relayReachable;
    out->nativeWebRtcOfferReady = status.nativeWebRtcOfferReady;
    out->nativeWebRtcConnected = status.nativeWebRtcConnected;
    out->packetsQueued = status.packetsQueued;
    out->packetsSent = status.packetsSent;
    out->packetsDropped = status.packetsDropped;
    out->sendFailures = status.sendFailures;
    out->queuedBlocks = status.queuedBlocks;
    out->latencyTargetMs = status.latencyTargetMs;
    out->targetBitrateKbps = status.targetBitrateKbps;
    copyText(out->shareUrl, sizeof(out->shareUrl), status.shareUrl);
    copyText(out->activeCodec, NC_TEXT_LEN, status.activeCodec);
    copyText(out->qualityLabel, NC_TEXT_LEN, status.qualityLabel);
    copyText(out->transportMode, NC_TEXT_LEN, status.transportMode);
    copyText(out->message, NC_TEXT_LEN, status.message);
}

bool nc_listen_enabled(NCEngine* engine) {
    return engine != nullptr && engine->project.listenRoomEnabled;
}

void nc_listen_set_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr) {
        return;
    }
    engine->project.listenRoomEnabled = enabled;
    if (enabled && engine->project.listenRoomAccessToken.empty()) {
        engine->project.listenRoomAccessToken = generateListenAccessToken();
    }
    engine->pushListenSettings();
}

void nc_listen_reset_token(NCEngine* engine) {
    if (engine == nullptr) return;
    engine->project.listenRoomAccessToken = generateListenAccessToken();
    engine->pushListenSettings();
}

void nc_listen_session_name(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->listenSettings().sessionName : std::string{});
}

void nc_listen_access_token(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.listenRoomAccessToken : std::string{});
}

void nc_listen_relay_host(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->listenSettings().relayHost : std::string{});
}

int nc_listen_relay_http_port(NCEngine* engine) {
    return engine != nullptr ? engine->listenSettings().relayHttpPort : 0;
}

int nc_listen_relay_tcp_ingest_port(NCEngine* engine) {
    return engine != nullptr ? engine->listenSettings().relayTcpIngestPort : 0;
}

void nc_listen_quality(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->listenSettings().quality : std::string{});
}

void nc_listen_set_quality(NCEngine* engine, const char* quality) {
    if (engine == nullptr || quality == nullptr) return;
    engine->project.listenRoomQuality = quality;
    engine->pushListenSettings();
}

void nc_listen_latency_mode(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->listenSettings().latencyMode : std::string{});
}

void nc_listen_set_latency_mode(NCEngine* engine, const char* mode) {
    if (engine == nullptr || mode == nullptr) return;
    engine->project.listenRoomLatencyMode = mode;
    engine->pushListenSettings();
}

void nc_listen_share_url(NCEngine* engine, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    copyText(out, outLen, neuracoust::daw::listenRoomShareUrl(engine->listenSettings()));
}

void nc_listen_public_share_url(NCEngine* engine, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    copyText(out, outLen, neuracoust::daw::listenRoomPublicShareUrl(engine->listenSettings()));
}

namespace {

std::string trimmedText(std::string value) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

/// Where the external listener page lives. Matches the old UI: an env override, then
/// a dotfile, then the tplinkdns default. The base may already carry query items.
std::string externalListenPageBase() {
    if (const char* env = std::getenv("NEURACOUST_LISTEN_EXTERNAL_URL")) {
        const std::string value = trimmedText(env);
        if (!value.empty()) {
            return value;
        }
    }
    if (const char* home = std::getenv("HOME")) {
        std::ifstream file(std::string(home) + "/.neuracoust/listen_external_url");
        if (file) {
            std::string value((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
            value = trimmedText(value);
            if (!value.empty()) {
                return value;
            }
        }
    }
    return "https://neuracoust.tplinkdns.com/listen/index.html?external=1";
}

} // namespace

void nc_listen_external_share_url(NCEngine* engine, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    const auto settings = engine->listenSettings();

    const std::string base = externalListenPageBase();
    // Split the base into its path and any query it already carries.
    const auto queryPos = base.find('?');
    const std::string path = base.substr(0, queryPos);

    // Keep any non-reserved query items the base URL brought (e.g. a router hint),
    // drop the ones we set ourselves so they are never duplicated.
    static const std::set<std::string> reserved = {
        "external", "profile", "session", "quality", "latency", "transport", "connect", "token"};
    std::string kept;
    if (queryPos != std::string::npos) {
        std::string query = base.substr(queryPos + 1);
        size_t start = 0;
        while (start <= query.size()) {
            const size_t amp = query.find('&', start);
            const std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            if (!pair.empty()) {
                const std::string name = pair.substr(0, pair.find('='));
                if (reserved.find(name) == reserved.end()) {
                    kept += (kept.empty() ? "" : "&") + pair;
                }
            }
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
    }

    const std::string transport = settings.transportMode.empty() ? "direct_fallback" : settings.transportMode;
    const std::string connect = settings.transportMode == "relay" ? "server" : "direct";
    std::string query = kept.empty() ? "" : kept + "&";
    query += "external=1&profile=external";
    query += "&session=" + (settings.sessionName.empty() ? std::string("mix") : settings.sessionName);
    query += "&quality=" + (settings.quality.empty() ? std::string("opus_high") : settings.quality);
    query += "&latency=" + (settings.latencyMode.empty() ? std::string("stable") : settings.latencyMode);
    query += "&transport=" + transport;
    query += "&connect=" + connect;
    if (!settings.accessToken.empty()) {
        query += "&token=" + settings.accessToken;
    }
    copyText(out, outLen, path + "?" + query);
}
