#include "bridge/NeuracoustEngineBridge.h"

#include "audio/ListenRoom.h"
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
#include <cmath>
#include <cstdlib>
#include <cstring>
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
    std::string monitorDspPathMode = "internal";

    std::vector<neuracoust::daw::PluginCandidate> plugins;         // full scan
    std::vector<neuracoust::daw::PluginCandidate> filteredPlugins; // current browser view
    neuracoust::daw::PluginCandidateFilterOptions facets;

    /// Peaks keyed by source path. Decoding a WAV is not cheap and the timeline
    /// asks for the same file on every redraw.
    struct WaveformPeaks {
        std::vector<float> mins;
        std::vector<float> maxs;
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

    AudioEngineSettings settings;
    settings.sampleRate = engine->project.sampleRate;
    settings.bufferSize = engine->project.defaultBufferSize;
    settings.tempoBpm = engine->project.tempoBpm;
    settings.timeSignatureNumerator = engine->project.timeSignatureNumerator;
    settings.timeSignatureDenominator = engine->project.timeSignatureDenominator;
    settings.transportRunning = false;
    settings.metronomeEnabled = false;
    settings.monitorDspEnabled = engine->monitorDspEnabled;
    settings.monitorDspPathMode = engine->monitorDspPathMode;
    settings.monitorModules = engine->project.monitorModules;
    settings.monitorStationMono = engine->project.monitorStationMono;
    settings.monitorStationListenMode = engine->project.monitorStationListenMode;
    settings.monitorStationMute = engine->project.monitorStationMute;
    settings.monitorStationDim = engine->project.monitorStationDim;
    settings.monitorStationDimDb = engine->project.monitorStationDimDb;
    settings.monitorInputTrimDb = engine->project.monitorInputTrimDb;
    settings.monitorVolumeDb = engine->project.monitorVolumeDb;

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

/// The points behind a parameter, wherever the track happens to keep them.
const std::vector<neuracoust::daw::AutomationPointState>* automationPoints(
    NCEngine* engine, int trackIndex, const char* parameterId) {
    const auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) {
        return nullptr;
    }
    if (isVolumeParameter(parameterId)) {
        return &track->volumeAutomation;
    }
    if (isPanParameter(parameterId)) {
        for (const auto& lane : track->automationLanes) {
            if (lane.parameterId == kPanParameterId) {
                return &lane.points;
            }
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

bool nc_automation_parameter_supported(const char* parameterId) {
    return isVolumeParameter(parameterId) || isPanParameter(parameterId);
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
            : neuracoust::daw::setTrackAutomationLanePoint(engine->project, trackName, kPanParameterId,
                                                           "Pan", timeSeconds, value);
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
            : neuracoust::daw::moveTrackAutomationLanePoint(engine->project, trackName, kPanParameterId,
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
                                                              kPanParameterId, index);
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
                                                                      kPanParameterId,
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

bool nc_waveform_peaks(NCEngine* engine, const char* path, float* mins, float* maxs) {
    if (engine == nullptr || path == nullptr || mins == nullptr || maxs == nullptr) {
        return false;
    }

    const std::string key(path);
    auto cached = engine->waveformCache.find(key);
    if (cached == engine->waveformCache.end()) {
        neuracoust::daw::WavAudioData audio;
        std::string error;
        if (!neuracoust::daw::readPcmWavFile(key, audio, error) ||
            audio.channels <= 0 || audio.interleavedSamples.empty()) {
            return false;
        }

        NCEngine::WaveformPeaks peaks;
        peaks.mins.assign(NC_WAVEFORM_BUCKETS, 0.0f);
        peaks.maxs.assign(NC_WAVEFORM_BUCKETS, 0.0f);

        const int64_t frames = audio.frameCount();
        const int channels = audio.channels;
        for (int bucket = 0; bucket < NC_WAVEFORM_BUCKETS; ++bucket) {
            const int64_t begin = frames * bucket / NC_WAVEFORM_BUCKETS;
            const int64_t end = std::max(begin + 1, frames * (bucket + 1) / NC_WAVEFORM_BUCKETS);

            float low = 0.0f;
            float high = 0.0f;
            for (int64_t frame = begin; frame < end && frame < frames; ++frame) {
                // Sum to mono: the timeline draws one envelope per clip.
                float sum = 0.0f;
                for (int channel = 0; channel < channels; ++channel) {
                    sum += audio.interleavedSamples[static_cast<size_t>(frame * channels + channel)];
                }
                const float value = sum / static_cast<float>(channels);
                low = std::min(low, value);
                high = std::max(high, value);
            }
            peaks.mins[static_cast<size_t>(bucket)] = std::max(-1.0f, low);
            peaks.maxs[static_cast<size_t>(bucket)] = std::min(1.0f, high);
        }

        cached = engine->waveformCache.emplace(key, std::move(peaks)).first;
    }

    std::memcpy(mins, cached->second.mins.data(), NC_WAVEFORM_BUCKETS * sizeof(float));
    std::memcpy(maxs, cached->second.maxs.data(), NC_WAVEFORM_BUCKETS * sizeof(float));
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

void nc_monitor_path_mode(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->monitorDspPathMode : std::string{});
}

void nc_monitor_set_path_mode(NCEngine* engine, const char* mode) {
    if (engine == nullptr || mode == nullptr) {
        return;
    }
    engine->monitorDspPathMode = mode;
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode,
                                         neuracoust::daw::defaultRemoteDspServerSettings());
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
