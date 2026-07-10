#include "bridge/NeuracoustEngineBridge.h"

#include "audio/ListenRoom.h"
#include "audio/RealtimeAudioEngine.h"
#include "audio/RemoteDspServerClient.h"
#include "plugins/InsertDspPolicy.h"
#include "plugins/MonitorDspModules.h"
#include "plugins/PluginScanner.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
}

void nc_track_set_pan(NCEngine* engine, int index, float pan) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackPan(engine->project, track->name, pan);
    pushTrackMix(engine, *track);
}

void nc_track_set_muted(NCEngine* engine, int index, bool muted) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackMuted(engine->project, track->name, muted);
    pushTrackRealtimeState(engine, *track);
}

void nc_track_set_solo(NCEngine* engine, int index, bool solo) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    // Additive, not exclusive: setTrackSolo touches only this track's flag, and
    // refuses outright on protected tracks (Master, Monitor).
    neuracoust::daw::setTrackSolo(engine->project, track->name, solo);
    pushTrackRealtimeState(engine, *track);
}

void nc_track_set_record_armed(NCEngine* engine, int index, bool armed) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackRecordArmed(engine->project, track->name, armed);
    pushTrackRealtimeState(engine, *track);
}

void nc_track_set_input_monitoring(NCEngine* engine, int index, bool monitoring) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    neuracoust::daw::setTrackInputMonitoring(engine->project, track->name, monitoring);
    pushTrackRealtimeState(engine, *track);
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
