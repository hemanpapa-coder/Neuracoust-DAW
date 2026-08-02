#include "bridge/NeuracoustEngineBridge.h"

#include "ai/AiAssistant.h"
#include "audio/ConsoleChannelProcessor.h"
#include "audio/ParametricEq.h"
#include "audio/MonitorCorrection.h"
#include "audio/OutputChainModeling.h"
#include "audio/SweepMeasurement.h"
#include "audio/SpeakerProfiles.h"
#include "audio/HeadphoneProfiles.h"
#include "audio/AudioInterfaceCatalog.h"
#include "audio/AudioInterfaceProfiles.h"
#include "core/Base64.h"
#include "core/Localization.h"
#include "audio/AudioDeviceModel.h"
#include "audio/ListenRoom.h"
#include "audio/MetronomeClick.h"
#include "audio/MidiInputRecorder.h"
#include "control/ControlSurfaceMidi.h"
#include "control/MackieHuiProtocol.h"
#include "audio/OfflineBounce.h"
#include "audio/RealtimeAudioEngine.h"
#include "audio/RemoteDspServerClient.h"
#include "audio/PitchEditor.h"
#include "audio/TimePitchProcessor.h"
#include "audio/VocalAlign.h"
#include "audio/WavFile.h"
#include "plugins/InsertDspPolicy.h"
#include "plugins/MonitorDspModules.h"
#include "plugins/AraHost.h"
#include "plugins/PluginScanner.h"
#include "plugins/Vst3RealtimeBridgeProtocol.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"
#include "project/AafImport.h"
#include "project/AudioImport.h"
#include "project/ProjectHistory.h"
#include "project/TimelineExport.h"

#include <pthread/qos.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
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

// Localization is global (no engine handle). tr() resolves the key in the current UI
// language, falls back to English, then to the key itself.
void nc_set_ui_language(const char* localeTag) {
    if (localeTag != nullptr) {
        neuracoust::daw::setUiLanguageFromLocaleTag(localeTag);
    }
}

void nc_tr(const char* key, char* out, size_t outLen) {
    copyText(out, outLen, neuracoust::daw::tr(key != nullptr ? key : ""));
}

struct NCEngine {
    RealtimeAudioEngine engine;
    /// Why the last plug-in action was refused (e.g. an ARA plug-in kept out of the realtime
    /// chain), so the UI can say more than "it didn't work".
    std::string lastPluginMessage;
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
    bool monitorReferenceArmed = false;
    /// A tap-input track's capture pass is running (begin→finish/discard) → keep the tap alive so it
    /// can be captured. Separate from the AUDIBLE punch monitor (engine tapInputMonitorActive_).
    bool tapCaptureActive = false;
    /// A tap-input track's Input-Monitor toggle is on → run the tap AND hear it (heard always).
    bool tapInputHold = false;
    /// Audio recording-to-disk (V1): the take's start position and target track, captured at
    /// record-start so the clip can be placed when recording stops.
    bool recordingAudio = false;
    double recordStartSeconds = 0.0;
    std::string recordTargetTrackName;
    /// Insert tail on stop: <0 always on (default), 0 cut, >0 ring out N seconds.
    double insertTailOnStopSeconds = -1.0;

    /// Live MIDI input for monitoring a keyboard through an instrument track.
    neuracoust::daw::MidiInputRecorder midiInputRecorder;
    neuracoust::daw::ControlSurfaceMidi huiMidi;
    neuracoust::daw::MackieHuiProtocol huiProtocol;
    std::deque<NCHuiEvent> huiEvents;
    int huiBankOffset = 0;
    std::array<float, 9> huiLastFaders {{-1,-1,-1,-1,-1,-1,-1,-1,-1}};
    std::array<int, 8> huiLastFlags {{-1,-1,-1,-1,-1,-1,-1,-1}};
    std::array<std::string, 8> huiLastNames;
    bool huiLastPlaying = false;
    bool huiLastRecording = false;
    int huiKeepAliveCounter = 0;
    /// Peak activity (0..1) of the MIDI seen since the last meter read; consumed by
    /// nc_midi_input_activity so the UI can decay it.
    float midiInputActivity = 0.0f;
    /// The selected instrument track hears the keyboard even when not record-armed — the
    /// Logic/Live convention. Transient (not project state, no undo), set from the UI on
    /// selection; the live-MIDI pump queues to this track in addition to armed ones.
    std::string liveMidiTargetTrack;

    /// Live MIDI recording take: while the transport records, the pumped keyboard events are
    /// accumulated here (note-ons paired with note-offs) and committed to a region on stop.
    /// Timing is per-feed (playhead at the tick), which is fine for a keyboard part.
    struct MidiRecordTake {
        bool active = false;
        std::string trackName;
        std::string regionId;   // the live region, created at begin so input draws in real time
        double startSeconds = 0.0;
        double tempoBpm = 120.0;
        double lastPlayheadSeconds = 0.0;
        struct Held { double startBeat = 0.0; int velocity = 0; bool on = false; };
        std::array<Held, 128> held {};
        int noteCount = 0;      // notes committed into the region so far (0 = empty take)
        int controllerCount = 0;  // whitelisted CC + pitch-bend events written into the region
    };
    MidiRecordTake midiRecordTake;

    /// Pitches an editor keyboard is currently holding down, per track. A mouse-up that
    /// lands outside the key, or a view torn down mid-drag, would otherwise leave a note
    /// sounding forever.
    std::map<std::string, std::set<int>> previewHeldNotes;

    /// Reverse audio ring from an open instrument editor: the editor's own instance
    /// renders GUI keyboard clicks (and the forwarded live MIDI), publishes into the
    /// shm ring, and this pump thread feeds it to the engine's monitor mix. One at a
    /// time — the most recently opened instrument editor owns the live path.
    struct InstrumentEditorMonitor {
        std::string trackName;
        std::unique_ptr<neuracoust::daw::Vst3InstrumentMonitorReader> reader;
        std::thread pumpThread;
        std::shared_ptr<std::atomic<bool>> stop;
    };
    InstrumentEditorMonitor instrumentEditorMonitor;

    void stopInstrumentEditorMonitor() {
        if (instrumentEditorMonitor.stop) {
            instrumentEditorMonitor.stop->store(true, std::memory_order_relaxed);
        }
        if (instrumentEditorMonitor.pumpThread.joinable()) {
            instrumentEditorMonitor.pumpThread.join();
        }
        if (!instrumentEditorMonitor.trackName.empty()) {
            engine.setEditorInstrumentMonitor(false, "");
        }
        instrumentEditorMonitor = {};
    }

    ~NCEngine() { stopInstrumentEditorMonitor(); }

    std::vector<neuracoust::daw::PluginCandidate> plugins;         // full scan
    std::vector<neuracoust::daw::PluginCandidate> filteredPlugins; // current browser view
    std::string pluginScanSignature;                              // .vst3 inventory at last scan
    neuracoust::daw::ResponseCurve measuredCurveL;                // room measurement, per channel
    neuracoust::daw::ResponseCurve measuredCurveR;
    // VR/헤드셋 착용 보정: monitoring on real speakers while wearing a VR headset (Meta Quest 3)
    // colours the sound because the headset mass + facepad reshape the acoustics at the ears. Capture
    // a reference measurement with the headset OFF (baseline), then one with it ON; the correction is
    // (baseline − worn), added to the monitor EQ to undo the worn coloring. Session state, the same
    // tier as the room measurement above (re-captured per session).
    neuracoust::daw::ResponseCurve vrHeadsetBaseline;
    neuracoust::daw::ResponseCurve vrHeadsetCorrection;
    bool vrHeadsetCorrectionEnabled = false;
    // Melodyne-mode pitch edit: notes detected for the clip most recently opened in the editor, with
    // the user's per-note offsets. Detect populates it; the editor sets offsets; apply consumes it.
    std::vector<neuracoust::daw::DetectedNote> pitchEditNotes;
    /// The one live ARA editing session, if any. In-process by necessity — ARA hands the plug-in
    /// pointers into our address space for its audio access, so it cannot be sandboxed the way the
    /// VST3 realtime hosts are. One at a time: two Melodyne documents over the same clip would each
    /// think they own it.
    std::unique_ptr<neuracoust::daw::AraDocumentController> araSession;
    std::string araSessionClipId;
    std::string pitchEditClipId;

    // Live loopback measurement of a physical audio interface (②d): the DAC→ADC path measured
    // by an ESS sweep, keyed by interface model name. When present it OVERRIDES the offline
    // baked profile — real measurement beats the HTML test-report bake. Persisted per-model to
    // ~/.neuracoust/measured_interfaces so a one-time measurement applies across projects.
    struct MeasuredInterfaceProfile {
        neuracoust::daw::ResponseCurve curve;   // midband-normalized FR (deviation from flat)
        std::vector<double> harmonics;          // [c2..c7] linear amplitude ratios
        double thdPercent = 0.0;
        // Multi-level (A단계): THD vs return level (dBFS, THD%), the level-dependence of the device.
        std::vector<std::pair<double, double>> thdVsLevel;
    };
    std::map<std::string, MeasuredInterfaceProfile> measuredInterfaces;
    int measureOutputChannel = 1;   // 1-based physical DAC channel for interface loopback measurement
    int measureInputChannel = 1;    // 1-based physical ADC channel the loopback is patched into
    double measureSweepAmplitude = 0.5;   // per-sweep drive level (auto-scaled for multi-level)
    // Accumulator for a multi-level auto run: one finished sweep's profile per drive level.
    struct LevelPoint { double dbfs; MeasuredInterfaceProfile prof; };
    std::vector<LevelPoint> pendingLevels;
    // A just-finished measurement held for review before the user commits it (save + apply), so a
    // clipped or too-low capture is not silently written over a good profile.
    MeasuredInterfaceProfile pendingInterfaceProfile;
    std::string pendingInterfaceName;
    bool pendingInterfaceValid = false;
    float pendingInterfacePeak = 0.0f;   // sweep peak, 0..1 (for the clip verdict)
    neuracoust::daw::PluginCandidateFilterOptions facets;
    bool monitorEqLinearPhase = false;   // FIR (linear-phase) monitor EQ vs the biquad fit
    // Low-latency monitoring. The linear-phase FIR costs numTaps/2 samples of pure delay —
    // 2048 samples, 42.7 ms at 48 kHz with the default 4096 taps — on EVERYTHING monitored.
    // That is right for judging a mix and unplayable for performing: a keyboard answers a
    // twentieth of a second late. So while any track is record-armed or input-monitoring,
    // the EQ falls back to the minimum-phase biquad fit of the same target curve, which adds
    // no delay at all. This is what every DAW calls low-latency monitoring; on here it needs
    // no manual switch because the arm state already says what the user is doing.
    bool monitorEqLowLatencyWhileMonitoring = true;
    bool monitorEqLowLatencyActive = false;   // whether the fallback is engaged right now
    int  monitorEqFirTaps = 4096;        // FIR length: resolution vs latency (numTaps/2 samples)
    bool monitorEqHeadphoneOeTarget = false;  // reference a headphone model to the Harman OE target

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
            // The fallback rebuild must not teleport the transport: an edit landing here during
            // playback reset the playhead to zero — and during a RECORDING pass that shrank the
            // live take's punch bookkeeping (the audio kept capturing; only the picture and the
            // final clip lengths lied).
            const double resume = std::max(0.0, engine.status().playbackSeconds);
            engine.loadProject(project, error);
            engine.seek(resume);
        }
    }

    // Reconcile through a monitor declick: for structural changes (add track/bus/send, send
    // pre-post) whose mixer-graph rebuild would otherwise click. Fades to silence, swaps, fades in.
    void reconcileProjectDeclicked() {
        engine.beginGraphChangeDeclick();
        reconcileProject();
        engine.endGraphChangeDeclick();
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
        engine.setTalkbackInputChannel(std::max(1, project.monitorStationTalkbackChannel));
    }

    void pushModules() {
        // The simultaneous-output flag is the project's speaker/headphone exclusive switch,
        // inverted, mirrored into the speaker-sim module so the realtime routing can read it
        // from the modules it already holds. Re-mirroring on every push means the two can
        // never drift, whatever set either of them.
        if (auto* sim = speakerSimulation(); sim != nullptr) {
            sim->simultaneousOutput = !project.monitorSpeakerHeadphoneExclusive;
        }
        engine.setMonitorDspModules(project.monitorModules, monitorDspEnabled);
    }

    // 2단계 nonlinear interface modeling: when enabled, feed the modeled interface's measured H2–H7
    // to the waveshaper (modeling target's if set, else the output-stage interface's). Off → bypass.
    // Prefer a live loopback measurement over the offline baked profile for this interface.
    neuracoust::daw::ResponseCurve interfaceCurveFor(const std::string& name) const {
        if (name.empty()) return {};
        const auto it = measuredInterfaces.find(name);
        if (it != measuredInterfaces.end() && !it->second.curve.empty()) return it->second.curve;
        auto profile = neuracoust::daw::audioInterfaceProfileCurve(name);
        if (!profile.empty()) return profile;
        // No measurement, no baked profile → spec-derived approximation, so a selected interface
        // still voices the monitor path (and real vs modeling interfaces read differently).
        return neuracoust::daw::audioInterfaceSpecCurve(name);
    }
    std::vector<double> interfaceHarmonicsFor(const std::string& name) const {
        if (name.empty()) return {};
        const auto it = measuredInterfaces.find(name);
        if (it != measuredInterfaces.end() && !it->second.harmonics.empty()) return it->second.harmonics;
        return neuracoust::daw::audioInterfaceHarmonics(name);
    }

    void pushInterfaceModeler() {
        std::vector<double> harmonics;
        if (project.monitorInterfaceModelingEnabled) {
            if (!project.physicalAudioInterfaceTargetModel.empty())
                harmonics = interfaceHarmonicsFor(project.physicalAudioInterfaceTargetModel);
            if (harmonics.empty() && !project.physicalAudioInterfaceModel.empty())
                harmonics = interfaceHarmonicsFor(project.physicalAudioInterfaceModel);
        }
        engine.updateInterfaceModeler(harmonics, 1.0);
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

namespace { void loadMeasuredInterfaces(NCEngine* engine); }  // defined with the measurement code below

NCEngine* nc_engine_create(void) {
    NCEngine* engine = new NCEngine();
    if (engine->project.monitorModules.empty()) {
        engine->project.monitorModules = neuracoust::daw::defaultMonitorDspModules();
    }
    loadMeasuredInterfaces(engine);   // a one-time loopback measurement applies across projects
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
// The wire buffer the current work calls for. Two modes, split the way the user works: MIXING
// rides a roomy buffer, TRACKING runs tight (40 frames = 0.83 ms at 48k, the SoundGrid floor).
// "auto" follows the session — any record-armed or input-monitoring track means someone is
// playing THROUGH the chain and latency is what matters; none means mixing, where resilience is.
// The same trigger the monitor EQ uses for its minimum-phase fallback.
uint16_t effectiveRemoteBufferFrames(const neuracoust::daw::ProjectDocument& project) {
    const int mixing = std::max(40, std::min(1024, project.remoteNetworkBufferFrames));
    const int tracking = std::max(40, std::min(1024, project.remoteTrackingBufferFrames));
    if (project.remoteLatencyMode == "mixing") {
        return static_cast<uint16_t>(mixing);
    }
    if (project.remoteLatencyMode == "tracking") {
        return static_cast<uint16_t>(tracking);
    }
    for (const auto& track : project.tracks) {
        if (track.recordArmed || track.inputMonitoring) {
            return static_cast<uint16_t>(tracking);
        }
    }
    return static_cast<uint16_t>(mixing);
}

// From the DOCUMENT, not the engine: the background bounce renders a serialized snapshot with no
// NCEngine anywhere near it, and its remote settings must come from the same fields.
neuracoust::daw::RemoteDspServerSettings buildRemoteDspSettingsFromProject(
        const neuracoust::daw::ProjectDocument& project) {
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    settings.totalCoreHint =
        static_cast<uint16_t>(std::max(1, std::min(16, project.externalDspCoreCount)));
    settings.networkBufferFrames = effectiveRemoteBufferFrames(project);
    settings.host = project.remoteDspHost.empty() ? std::string("studio.local") : project.remoteDspHost;
    settings.enabled = project.externalDspEnabled;
    settings.ndsHost = project.ndsHost.empty() ? std::string("192.168.0.198:20002") : project.ndsHost;
    settings.ndsEnabled = project.ndsEnabled;
    settings.roleMonitor = project.dspRoleMonitor;
    settings.roleChannelStrip = project.dspRoleChannelStrip;
    settings.roleMaster = project.dspRoleMaster;
    settings.roleMixer = project.dspRoleMixer;
    settings.roleInserts = project.dspRoleInserts;
    settings.autoOverflow = project.dspAutoOverflow;
    settings.mixerChannels = static_cast<uint16_t>(std::max(8, std::min(64, project.remoteMixerChannels)));
    settings.ndsPoolHosts = project.ndsPoolHosts;
    settings.preferredInterface = project.remoteDspInterface;
    settings.nodes.clear();
    return settings;
}

neuracoust::daw::RemoteDspServerSettings buildRemoteDspSettings(NCEngine* engine) {
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    settings.totalCoreHint =
        static_cast<uint16_t>(std::max(1, std::min(16, engine->project.externalDspCoreCount)));
    settings.networkBufferFrames = effectiveRemoteBufferFrames(engine->project);
    // Point the engine at the user's node. Clearing the default node list makes the
    // top-level host the effective target, so External/NDS reach a real server instead
    // of the hardcoded "studio.local" default.
    settings.host = engine->project.remoteDspHost.empty()
                        ? std::string("studio.local") : engine->project.remoteDspHost;
    // The "use this node" master switch. When off, makeRemoteDspCorePlan gates the node out of the
    // monitor/DAW/plugin core plan (settings.enabled already drives those flags).
    settings.enabled = engine->project.externalDspEnabled;
    // The NDS appliance is a second, separately addressed machine — not another name for the
    // external node. It carries its own switch so one can be off while the other works.
    settings.ndsHost = engine->project.ndsHost.empty()
                           ? std::string("192.168.0.198:20002") : engine->project.ndsHost;
    settings.ndsEnabled = engine->project.ndsEnabled;
    settings.roleMonitor = engine->project.dspRoleMonitor;
    settings.roleChannelStrip = engine->project.dspRoleChannelStrip;
    settings.roleMaster = engine->project.dspRoleMaster;
    settings.roleMixer = engine->project.dspRoleMixer;
    settings.roleInserts = engine->project.dspRoleInserts;
    settings.autoOverflow = engine->project.dspAutoOverflow;
    settings.mixerChannels = static_cast<uint16_t>(std::max(8, std::min(64, engine->project.remoteMixerChannels)));
    settings.ndsPoolHosts = engine->project.ndsPoolHosts;
    settings.preferredInterface = engine->project.remoteDspInterface;
    settings.nodes.clear();
    return settings;
}

// The 모니터 assignment and the monitor path mode are one decision under two names: the dock's
// DSP-source row writes the mode, the role table writes the role. Keeping them equal here means
// neither control can disagree with the other, whichever one the user touched.
std::string monitorPathModeForRole(const std::string& role) {
    if (role == "nds") return "nds";
    if (role == "external") return "remote_external";
    return "internal";
}

std::string monitorRoleForPathMode(const std::string& mode) {
    if (mode == "nds" || mode == "external") return "nds";
    if (mode == "remote_external") return "external";
    return "internal";
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
    settings.grooveFeel = engine->project.grooveFeel;
    settings.grooveSwingAmount = engine->project.grooveSwingAmount;
    settings.metronomeSubdivision = engine->project.metronomeSubdivision;
    settings.metronomeGain = engine->project.metronomeGain;
    settings.metronomeSound = engine->project.metronomeSound;
    settings.metronomeAccentFirst = engine->project.metronomeAccentFirst;
    settings.metronomeAccentPattern = engine->project.metronomeAccentPattern;
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
    // Allow the engine to open a physical input when an input feature actually needs it
    // (record-arm monitoring, talkback, or listening to a source like BlackHole). The
    // engine's own gates decide WHEN to open the queue, so plain playback never does —
    // and the macOS microphone prompt only appears when the user engages one of those.
    settings.physicalInputAccessAllowed = true;
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
    out->monitorPrePeakLeft = s.monitorPrePeakLeft;
    out->monitorPrePeakRight = s.monitorPrePeakRight;
    out->masterBusPeakLeft = s.masterBusPeakLeft;
    out->masterBusPeakRight = s.masterBusPeakRight;
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
    out->inputPeakLeft = s.inputPeakLeft;
    out->inputPeakRight = s.inputPeakRight;
    out->recordArmedTrackCount = s.recordArmedTrackCount;
    out->realtimeCallbackCount = s.realtimeCallbackCount;
    out->realtimeAverageWakeJitterUs = s.realtimeAverageWakeJitterUs;
    out->realtimeMaxWakeJitterUs = s.realtimeMaxWakeJitterUs;
    out->realtimeMaxRenderDurationUs = s.realtimeMaxRenderDurationUs;
    out->realtimeMaxRenderLoad = s.realtimeMaxRenderLoad;
    out->realtimeMaxLocalRenderLoad = s.realtimeMaxLocalRenderLoad;
    out->realtimeMaxRemoteWaitLoad = s.realtimeMaxRemoteWaitLoad;
    out->remoteDspRoundTripJitterUs = s.remoteDspAverageRoundTripJitterUs;
    out->realtimeLateWakeCount = s.realtimeLateWakeCount;
    out->referenceUnderrunBlocks = s.referenceUnderrunBlocks;
    out->referenceOverrunDrops = s.referenceOverrunDrops;
    out->remoteDspMonitorActive = s.remoteDspMonitorActive;
    out->remoteDspRoundTripMs = s.remoteDspRoundTripMs;
    out->remoteMixBusCount = s.remoteMixBusCount;
    out->remoteMixSums = s.remoteMixSums;
    out->remoteMixMisses = s.remoteMixMisses;
    out->activeRealtimeVst3TrackInserts = s.activeRealtimeVst3TrackInsertCount;
    out->activeRealtimeVst3MasterInserts = s.activeRealtimeVst3MasterInsertCount;
    out->activeRemoteDspTrackInserts = s.activeRemoteDspTrackInsertCount;
    out->activeOfflineVst3TrackInserts = s.activeOfflineVst3TrackInsertCount;

    const size_t meterCount = std::min({s.trackMeterNames.size(),
                                        s.trackPeakLeft.size(),
                                        s.trackPeakRight.size(),
                                        s.trackConsoleGainReductionDb.size(),
                                        s.trackConsoleGateGainReductionDb.size(),
                                        static_cast<size_t>(NC_MAX_TRACK_METERS)});
    out->trackMeterCount = static_cast<int>(meterCount);
    for (size_t i = 0; i < meterCount; ++i) {
        copyText(out->trackMeterNames[i], NC_NAME_LEN, s.trackMeterNames[i]);
        out->trackPeakLeft[i] = s.trackPeakLeft[i];
        out->trackPeakRight[i] = s.trackPeakRight[i];
        out->trackConsoleGainReductionDb[i] = s.trackConsoleGainReductionDb[i];
        out->trackConsoleGateGainReductionDb[i] = s.trackConsoleGateGainReductionDb[i];
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

// The sentinel inputBus value that routes a track's recording from the reference process tap
// ("다른 앱") instead of a physical input pair. Kept in sync with the Swift input menu.
static const char* kReferenceTapInputBus = "다른 앱";

// Pro-Tools-style INTERNAL BUSES: virtual names ("내부 버스 1"…) any track can output to and any
// audio track can take as its input — no aux track needed, the mixer graph resolves the name and
// orders feeders before the receiver. A record pass on a bus-input track captures the bus on the
// RENDER clock (sample-locked to the timeline). The prefix must not be "Bus ", which the graph
// reserves for classifying aux tracks.
static const char* kInternalBusPrefix = "내부 버스 ";
static bool isInternalBusName(const std::string& busName) {
    return busName.rfind(kInternalBusPrefix, 0) == 0;
}

// 0-based index of the first device channel a "Input N-M" bus name refers to (N-1). 0 if none.
static int firstInputChannelIndex(const std::string& busName) {
    for (size_t i = 0; i < busName.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(busName[i])) != 0) {
            size_t j = i;
            while (j < busName.size() && std::isdigit(static_cast<unsigned char>(busName[j])) != 0) ++j;
            const int n = std::stoi(busName.substr(i, j - i));
            return std::max(0, n - 1);
        }
    }
    return 0;
}

// Begin capturing the record-armed track's input to disk. Source is the reference tap when the
// track's input is the "다른 앱" sentinel, else the physical channel pair its bus names.
bool nc_engine_begin_audio_record(NCEngine* engine) {
    if (engine == nullptr || engine->recordingAudio) return false;
    const std::string trackName = neuracoust::daw::recordingTargetTrackName(engine->project);
    if (trackName.empty()) return false;
    auto it = std::find_if(engine->project.tracks.begin(), engine->project.tracks.end(),
                           [&](const neuracoust::daw::TrackState& t) { return t.name == trackName; });
    if (it == engine->project.tracks.end()) return false;

    int source = 1, offset = 0, channels = 2;
    if (it->inputBus == kReferenceTapInputBus) {
        source = 2; offset = 0; channels = 2;
        // Keep the tap alive during the capture pass so it can be recorded (apps muted). It is NOT
        // made audible here — the punch (nc_monitor_set_tap_input_monitor) decides when you hear it.
        engine->tapCaptureActive = true;
        engine->engine.setMonitorReferenceArmed(true);
    } else if (isInternalBusName(it->inputBus)) {
        // Internal bus: the render thread appends the track's received bus block to the take —
        // sample-locked to the timeline, so the punched clip lands exactly where the sound was.
        source = 3; offset = 0; channels = 2;
    } else {
        source = 1;
        channels = std::max(1, neuracoust::daw::inputChannelCountForBusName(it->inputBus));
        offset = firstInputChannelIndex(it->inputBus);
    }
    engine->recordStartSeconds = std::max(0.0, engine->engine.status().playbackSeconds);
    engine->recordTargetTrackName = trackName;
    engine->recordingAudio = true;
    engine->engine.beginInputRecording(source, offset, channels, trackName);
    // NOTE: transportRecordingActive is deliberately NOT set here. It marks an ACTIVE PUNCH (set by
    // nc_engine_set_recording on the Record button), which silences the armed track's tape under the
    // pass. Background capture must leave the tape audible, so it stays off until a punch-in.
    return true;
}

// Stop capturing, save the WAV, and drop an audio clip at the record-start position.
// Stop capturing and save the whole-pass WAV, returning its path. Add one clip per punch region
// afterward with nc_engine_add_take_clip. (Split so a single pass can leave multiple clips.)
bool nc_engine_finish_audio_record(NCEngine* engine, char* outPath, size_t pathLen,
                                   char* outError, size_t errLen) {
    if (engine == nullptr || !engine->recordingAudio) return false;
    engine->recordingAudio = false;
    engine->engine.setTransportRecordingActive(false);
    engine->tapCaptureActive = false;
    engine->engine.setTapInputMonitor(false);
    engine->engine.setMonitorReferenceArmed(engine->monitorReferenceArmed);   // release tap unless the button holds it

    std::string pathError;
    std::string path = neuracoust::daw::nextProjectRecordingPath(engine->projectPath, pathError);
    if (path.empty()) {
        // Untitled project: land the take in a temp recordings folder so recording still works.
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path() / "Neuracoust Recordings";
        std::filesystem::create_directories(dir, ec);
        for (int i = 0; i < 100000; ++i) {
            const auto cand = dir / ("Neuracoust DAW Recording " + std::to_string(i) + ".wav");
            if (!std::filesystem::exists(cand)) { path = cand.string(); break; }
        }
    }
    if (path.empty()) { copyText(outError, errLen, "녹음 파일 경로를 만들 수 없습니다."); return false; }

    std::string saveError; double durationSeconds = 0.0; int channels = 0;
    if (!engine->engine.endInputRecording(path, 24, saveError, durationSeconds, channels)) {
        copyText(outError, errLen, saveError.empty() ? "녹음된 오디오가 없습니다." : saveError);
        return false;
    }
    copyText(outPath, pathLen, path);
    return true;
}

// Add one clip from a finished take WAV: the clip sits at clipStartSeconds and references the file
// from sourceOffsetSeconds, so extending it reveals the pre/post-roll captured in the background.
bool nc_engine_add_take_clip(NCEngine* engine, const char* path, double clipStartSeconds,
                             double sourceOffsetSeconds, double durationSeconds,
                             char* outClipId, size_t outLen, char* outError, size_t errLen) {
    if (engine == nullptr || path == nullptr || *path == '\0') { copyText(outError, errLen, "no take"); return false; }
    const double dur = std::max(0.05, durationSeconds);
    const double clipStart = std::max(0.0, clipStartSeconds);
    // Overdub like analog tape: cut the target track's existing clips out of the punch range first,
    // so the new take replaces the tape underneath rather than doubling with it on playback.
    neuracoust::daw::clearTrackClipRange(engine->project, engine->recordTargetTrackName, clipStart, clipStart + dur);
    std::string clipId = neuracoust::daw::appendAudioClipAt(engine->project, engine->recordTargetTrackName,
                                                            std::string(path), clipStart, dur);
    if (clipId.empty()) { copyText(outError, errLen, "클립 생성 실패"); return false; }
    for (auto& c : engine->project.clips) {
        if (c.id == clipId) { c.sourceOffsetSeconds = std::max(0.0, sourceOffsetSeconds); break; }
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Record audio");
    copyText(outClipId, outLen, clipId);
    return true;
}

bool nc_engine_audio_recording_active(NCEngine* engine) {
    return engine != nullptr && engine->recordingAudio;
}

// Drop an uncommitted background pass (played over an armed track but never pressed Record).
void nc_engine_discard_audio_record(NCEngine* engine) {
    if (engine == nullptr || !engine->recordingAudio) return;
    engine->recordingAudio = false;
    engine->engine.setTransportRecordingActive(false);
    engine->tapCaptureActive = false;
    engine->engine.setTapInputMonitor(false);
    engine->engine.setMonitorReferenceArmed(engine->monitorReferenceArmed);   // release tap unless the button holds it
    engine->engine.cancelInputRecording();
}

// Live take metering for the timeline's growing waveform while recording.
double nc_recording_live_seconds(NCEngine* engine) {
    return engine == nullptr ? 0.0 : engine->engine.recordingLiveSeconds();
}
int nc_recording_live_peak_count(NCEngine* engine) {
    return engine == nullptr ? 0 : engine->engine.recordingLivePeakCount();
}
int nc_recording_live_peaks_since(NCEngine* engine, int fromBucket, float* outLR, int maxBuckets) {
    return engine == nullptr ? 0 : engine->engine.recordingLivePeaksSince(fromBucket, outLR, maxBuckets);
}
int nc_recording_channels(NCEngine* engine) {
    return engine == nullptr ? 2 : engine->engine.recordingChannels();
}
int nc_recording_peak_samples(NCEngine* engine) {
    return engine == nullptr ? 512 : engine->engine.recordingPeakSamples();
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
                                       engine->project.metronomeSubdivision,
                                       engine->project.metronomeGain,
                                       engine->project.metronomeSound,
                                       engine->project.metronomeAccentFirst);
}

// All of these store a project field; nc_engine_set_metronome_enabled re-applies them, so the
// Swift side calls one then re-asserts the enabled state to make the change audible immediately.

// The click resolution: "auto" (beat, accenting the bar), "quarter", "eighth", "sixteenth".
void nc_engine_set_metronome_subdivision(NCEngine* engine, const char* subdivision) {
    if (engine == nullptr || subdivision == nullptr) {
        return;
    }
    engine->project.metronomeSubdivision = subdivision;
}

// Click level (0..2 linear over the built-in click).
void nc_engine_set_metronome_gain(NCEngine* engine, float gain) {
    if (engine == nullptr) {
        return;
    }
    engine->project.metronomeGain = std::max(0.0, std::min(2.0, static_cast<double>(gain)));
}

// Click timbre: "beep" / "wood" / "rim" / "cowbell".
void nc_engine_set_metronome_sound(NCEngine* engine, const char* sound) {
    if (engine == nullptr || sound == nullptr) {
        return;
    }
    engine->project.metronomeSound = sound;
}

// Groove feel ("straight" / "shuffle" / "triplet") + swing amount (0.5..0.9 meaningful).
void nc_engine_set_groove(NCEngine* engine, const char* feel, float swingAmount) {
    if (engine == nullptr || feel == nullptr) {
        return;
    }
    engine->project.grooveFeel = feel;
    engine->project.grooveSwingAmount = std::max(0.0, std::min(1.0, static_cast<double>(swingAmount)));
}

// Whether the bar's first beat is accented (off = an even, flat click).
void nc_engine_set_metronome_accent_first(NCEngine* engine, bool accent) {
    if (engine == nullptr) {
        return;
    }
    engine->project.metronomeAccentFirst = accent;
}

// A genre groove's accent pattern: per-step gains (0..1) over one bar at the click subdivision.
// Pass count 0 (or gains null) to clear it and return to the default bar/beat hierarchy. Applied
// live via setMetronomeAccentPattern (no restart), and stored so an engine start keeps it.
void nc_engine_set_metronome_pattern(NCEngine* engine, const float* gains, int count) {
    if (engine == nullptr) {
        return;
    }
    std::vector<float> pattern;
    if (gains != nullptr && count > 0) {
        pattern.assign(gains, gains + count);
    }
    engine->project.metronomeAccentPattern = pattern;
    engine->engine.setMetronomeAccentPattern(pattern);
}

// The selected genre preset's id (the UI catalog label). Stored so the picker reloads with a project.
void nc_engine_set_metronome_genre(NCEngine* engine, const char* genre) {
    if (engine == nullptr || genre == nullptr) {
        return;
    }
    engine->project.metronomeGenre = genre;
}

// Getters so the UI can reload metronome settings from an opened project.
float nc_metronome_gain(NCEngine* engine) {
    return engine != nullptr ? static_cast<float>(engine->project.metronomeGain) : 1.0f;
}
void nc_metronome_sound(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.metronomeSound : std::string{"beep"});
}
bool nc_metronome_accent_first(NCEngine* engine) {
    return engine != nullptr ? engine->project.metronomeAccentFirst : true;
}
void nc_metronome_genre(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.metronomeGenre : std::string{"straight"});
}

bool nc_metronome_print_to_track(NCEngine* engine, bool loopRangeOnly,
                                 char* error, size_t errorLen) {
    if (engine == nullptr) {
        copyText(error, errorLen, "engine is null");
        return false;
    }

    double startSeconds = 0.0;
    double endSeconds = 0.0;
    if (loopRangeOnly) {
        if (!engine->project.loopEnabled ||
            engine->project.loopEndSeconds <= engine->project.loopStartSeconds) {
            copyText(error, errorLen, "먼저 유효한 루프/편집 범위를 지정하세요.");
            return false;
        }
        startSeconds = std::max(0.0, engine->project.loopStartSeconds);
        endSeconds = engine->project.loopEndSeconds;
    } else {
        for (const auto& clip : engine->project.clips) {
            endSeconds = std::max(endSeconds, clip.startSeconds + clip.durationSeconds);
        }
        for (const auto& region : engine->project.midiRegions) {
            endSeconds = std::max(endSeconds, region.startSeconds + region.durationSeconds);
        }
        for (const auto& clip : engine->project.videoClips) {
            endSeconds = std::max(endSeconds, clip.startSeconds + clip.durationSeconds);
        }
        // An empty session still gets four bars, which makes the command useful while
        // preparing a count/cue track before importing or recording anything.
        if (endSeconds <= 0.0) {
            const double quarterSeconds = 60.0 / std::max(1, engine->project.tempoBpm);
            const double barQuarters =
                std::max(1, engine->project.timeSignatureNumerator) * 4.0 /
                std::max(1, engine->project.timeSignatureDenominator);
            endSeconds = quarterSeconds * barQuarters * 4.0;
        }
    }

    const int sampleRate = std::max(8000, static_cast<int>(std::lround(engine->project.sampleRate)));
    const int64_t frameCount = std::max<int64_t>(
        1, static_cast<int64_t>(std::ceil((endSeconds - startSeconds) * sampleRate)));
    neuracoust::daw::WavAudioData audio;
    audio.channels = 1;
    audio.sampleRate = sampleRate;
    audio.bitsPerSample = 24;
    audio.interleavedSamples.resize(static_cast<size_t>(frameCount));

    auto settings = buildEngineSettings(engine);
    settings.sampleRate = sampleRate;
    settings.metronomeEnabled = true;
    settings.tempoMap = engine->project.tempoMap;
    settings.timeSignatureMap = engine->project.timeSignatureMap;
    const int64_t timelineStartFrame =
        static_cast<int64_t>(std::llround(startSeconds * sampleRate));
    for (int64_t frame = 0; frame < frameCount; ++frame) {
        audio.interleavedSamples[static_cast<size_t>(frame)] =
            neuracoust::daw::renderMetronomeClickSampleAtFrame(timelineStartFrame + frame, settings);
    }

    const auto mediaDirectory = engine->projectPath.empty()
        ? neuracoust::daw::temporaryImportAudioFilesDirectory()
        : neuracoust::daw::projectAudioFilesDirectory(std::filesystem::path(engine->projectPath));
    std::error_code fsError;
    std::filesystem::create_directories(mediaDirectory, fsError);
    if (fsError) {
        copyText(error, errorLen, "Audio Files 폴더를 만들 수 없습니다: " + fsError.message());
        return false;
    }
    std::filesystem::path wavPath = mediaDirectory / "Metronome Print.wav";
    for (int suffix = 2; std::filesystem::exists(wavPath) && suffix < 10000; ++suffix) {
        wavPath = mediaDirectory / ("Metronome Print " + std::to_string(suffix) + ".wav");
    }
    std::string writeError;
    if (!neuracoust::daw::writePcm24WavFileAtomically(wavPath, audio, writeError)) {
        copyText(error, errorLen, writeError.empty() ? "메트로놈 WAV를 만들 수 없습니다." : writeError);
        return false;
    }

    const std::string freshName = neuracoust::daw::addAudioTrack(engine->project);
    if (freshName.empty()) {
        std::filesystem::remove(wavPath, fsError);
        copyText(error, errorLen, "오디오 트랙을 추가할 수 없습니다.");
        return false;
    }
    std::string clickName = "Metronome";
    int suffix = 2;
    const auto nameExists = [&](const std::string& candidate) {
        return std::any_of(engine->project.tracks.begin(), engine->project.tracks.end(),
                           [&](const auto& track) {
                               return track.name == candidate && track.name != freshName;
                           });
    };
    while (nameExists(clickName)) clickName = "Metronome " + std::to_string(suffix++);
    neuracoust::daw::renameTrack(engine->project, freshName, clickName);
    neuracoust::daw::appendAudioClipAt(engine->project, clickName, wavPath.string(),
                                      startSeconds, endSeconds - startSeconds);
    engine->reconcileProjectDeclicked();
    engine->recordStep(loopRangeOnly ? "Print metronome range" : "Print metronome track");
    copyText(error, errorLen, clickName + " 트랙을 만들었습니다.");
    return true;
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

double nc_project_pre_roll(NCEngine* engine) {
    return engine != nullptr ? engine->project.preRollSeconds : 0.0;
}

double nc_project_post_roll(NCEngine* engine) {
    return engine != nullptr ? engine->project.postRollSeconds : 0.0;
}

void nc_project_set_pre_post_roll(NCEngine* engine, double preRollSeconds, double postRollSeconds) {
    if (engine == nullptr) return;
    const double pre = std::clamp(std::isfinite(preRollSeconds) ? preRollSeconds : 0.0, 0.0, 3600.0);
    const double post = std::clamp(std::isfinite(postRollSeconds) ? postRollSeconds : 0.0, 0.0, 3600.0);
    if (engine->project.preRollSeconds == pre && engine->project.postRollSeconds == post) return;
    engine->project.preRollSeconds = pre;
    engine->project.postRollSeconds = post;
    std::string error;
    if (!engine->engine.updateProject(engine->project, error)) {
        engine->engine.loadProject(engine->project, error);
    }
    engine->recordStep("Set loop pre/post-roll");
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

void nc_track_notes(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->notes : std::string{});
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

void nc_track_channel_format(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->channelFormat : std::string{"stereo"});
}

void nc_track_set_channel_format(NCEngine* engine, int index, const char* format) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || format == nullptr) return;
    const std::string value = std::string(format) == "mono" ? "mono" : "stereo";
    if (track->channelFormat == value) return;
    track->channelFormat = value;
    engine->recordStep("Set track channel format");
    engine->reconcileProject();   // mono/stereo changes how the renderer sums the track
}

// A channel memo — free text, no audio effect, so it records an undo step (and
// autosaves) but does not touch the render graph.
void nc_track_set_notes(NCEngine* engine, int index, const char* notes) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || notes == nullptr || track->notes == notes) return;
    track->notes = notes;
    engine->recordStep("Edit channel memo");
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
    // Pro-Tools-style internal buses: always offered — no aux track needed. A bus nobody takes
    // as an input is silent (collected, never consumed), same as an unassigned PT bus.
    for (int bus = 1; bus <= 8; ++bus) {
        options.push_back(std::string(kInternalBusPrefix) + std::to_string(bus));
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

void nc_track_console_model(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* track = trackAt(engine, index);
    copyText(out, outLen, track != nullptr ? track->consoleChannel.model : std::string{});
}
// The strip's overall console model (voices the saturator + filter/EQ circuit colour). Comp and
// gate carry their own models; this is the shared one behind their plates.
void nc_track_set_console_model(NCEngine* engine, int index, const char* name) {
    auto* t = trackAt(engine, index); if (t == nullptr || name == nullptr) return;
    if (t->consoleChannel.model == name) return;
    t->consoleChannel.model = name;
    engine->engine.updateTrackConsoleChannel(t->name, t->consoleChannel);   // live, no rebuild
    engine->recordStep("Console model");
}
// Harmonic spectrum the saturator adds right now (2nd..count+1), each 0..1. Drives the strip's
// harmonics visualiser; computed from the live console params so it tracks Drive/Mix/model.
void nc_track_console_harmonics(NCEngine* engine, int index, float* out, int count) {
    for (int i = 0; i < count; ++i) if (out != nullptr) out[i] = 0.0f;
    const auto* t = trackAt(engine, index);
    if (t == nullptr || out == nullptr || count <= 0) return;
    neuracoust::daw::consoleSaturatorHarmonics(t->consoleChannel, out, count);
}
// --- Analog channel bias: per-strip variation reproduced digitally, up to 512 tracks -------------
int nc_track_console_bias_seed(NCEngine* engine, int index) {
    const auto* t = trackAt(engine, index); return t != nullptr ? t->consoleChannel.channelBiasSeed : 0;
}
float nc_track_console_bias_depth(NCEngine* engine, int index) {
    const auto* t = trackAt(engine, index); return t != nullptr ? t->consoleChannel.channelBiasDepth : 0.0f;
}
// Auto: seed every strip from its channel index at the given depth (0..1). One undo step.
void nc_console_bias_auto(NCEngine* engine, float depth) {
    if (engine == nullptr) return;
    const float d = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    for (size_t i = 0; i < engine->project.tracks.size(); ++i) {
        auto& t = engine->project.tracks[i];
        t.consoleChannel.channelBiasSeed = static_cast<int>(i) + 1;
        t.consoleChannel.channelBiasDepth = d;
        engine->engine.updateTrackConsoleChannel(t.name, t.consoleChannel);
    }
    engine->recordStep("Channel bias (auto)");
}
// Off: matched channels (depth 0) — a pure digital console.
void nc_console_bias_off(NCEngine* engine) {
    if (engine == nullptr) return;
    for (auto& t : engine->project.tracks) {
        t.consoleChannel.channelBiasDepth = 0.0f;
        engine->engine.updateTrackConsoleChannel(t.name, t.consoleChannel);
    }
    engine->recordStep("Channel bias (off)");
}
// Manual: re-roll one strip's character (keeps its current depth).
void nc_track_set_console_bias_seed(NCEngine* engine, int index, int seed) {
    auto* t = trackAt(engine, index); if (t == nullptr) return;
    t->consoleChannel.channelBiasSeed = seed;
    engine->engine.updateTrackConsoleChannel(t->name, t->consoleChannel);
    engine->recordStep("Channel bias (manual)");
}
// Per-module model (comp / gate) — the model library. Each voices the DSP as a named classic.
void nc_track_console_comp_type(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* t = trackAt(engine, index);
    copyText(out, outLen, t != nullptr ? t->consoleChannel.compType : std::string{});
}
void nc_track_set_console_comp_type(NCEngine* engine, int index, const char* name) {
    auto* t = trackAt(engine, index); if (t == nullptr || name == nullptr) return;
    if (t->consoleChannel.compType == name) return;
    t->consoleChannel.compType = name;
    engine->engine.updateTrackConsoleChannel(t->name, t->consoleChannel);   // live, no rebuild
    engine->recordStep("Compressor model");
}
void nc_track_console_gate_type(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* t = trackAt(engine, index);
    copyText(out, outLen, t != nullptr ? t->consoleChannel.gateType : std::string{});
}
void nc_track_set_console_gate_type(NCEngine* engine, int index, const char* name) {
    auto* t = trackAt(engine, index); if (t == nullptr || name == nullptr) return;
    if (t->consoleChannel.gateType == name) return;
    t->consoleChannel.gateType = name;
    engine->engine.updateTrackConsoleChannel(t->name, t->consoleChannel);
    engine->recordStep("Gate model");
}
void nc_track_console_module_order(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* t=trackAt(engine,index); copyText(out,outLen,t?t->consoleChannel.moduleOrder:std::string{});
}
void nc_track_set_console_module_order(NCEngine* engine, int index, const char* order) {
    auto* t=trackAt(engine,index); if(!t||!order)return; t->consoleChannel.moduleOrder=order;
    engine->reconcileProject(); engine->recordStep("Console module order");
}

bool nc_track_console_bool(NCEngine* engine, int index, const char* parameter) {
    const auto* t = trackAt(engine, index); if (t == nullptr || parameter == nullptr) return false;
    const std::string p(parameter);
    if (p == "filterEnabled") return t->consoleChannel.filterEnabled;
    if (p == "filterCircuitMode") return t->consoleChannel.filterCircuitMode;
    if (p == "highPassEnabled") return t->consoleChannel.highPassEnabled;
    if (p == "lowPassEnabled") return t->consoleChannel.lowPassEnabled;
    if (p == "eqEnabled") return t->consoleChannel.eqEnabled;
    if (p == "eqCircuitMode") return t->consoleChannel.eqCircuitMode;
    if (p == "eqHfBell") return t->consoleChannel.eqHfBell;
    if (p == "eqLfBell") return t->consoleChannel.eqLfBell;
    if (p == "eqEMode") return t->consoleChannel.eqEMode;
    if (p == "compEnabled") return t->consoleChannel.compEnabled;
    if (p == "compCircuitMode") return t->consoleChannel.compCircuitMode;
    if (p == "compFastAttack") return t->consoleChannel.compFastAttack;
    if (p == "compPeakMode") return t->consoleChannel.compPeakMode;
    if (p == "gateEnabled") return t->consoleChannel.gateEnabled;
    if (p == "gateCircuitMode") return t->consoleChannel.gateCircuitMode;
    if (p == "saturatorEnabled") return t->consoleChannel.saturatorEnabled;
    if (p == "dualMono") return t->consoleChannel.dualMono;
    if (p == "filterDualMono") return t->consoleChannel.filterDualMono;
    if (p == "eqDualMono") return t->consoleChannel.eqDualMono;
    if (p == "compDualMono") return t->consoleChannel.compDualMono;
    if (p == "gateDualMono") return t->consoleChannel.gateDualMono;
    if (p == "saturatorDualMono") return t->consoleChannel.saturatorDualMono;
    if (p == "saturatorCircuitMode") return t->consoleChannel.saturatorCircuitMode;
    if (p == "gateFastAttack") return t->consoleChannel.gateFastAttack;
    if (p == "expanderMode") return t->consoleChannel.expanderMode;
    if (p == "phaseInvert") return t->consoleChannel.phaseInvert;
    if (p == "phaseInvertL") return t->consoleChannel.phaseInvertL;
    if (p == "phaseInvertR") return t->consoleChannel.phaseInvertR;
    return false;
}

float nc_track_console_value(NCEngine* engine, int index, const char* parameter) {
    const auto* t = trackAt(engine, index); if (t == nullptr || parameter == nullptr) return 0;
    const auto& c=t->consoleChannel; const std::string p(parameter);
#define NC_GET(name, field) if (p == name) return c.field
    NC_GET("highPassHz", highPassHz); NC_GET("lowPassHz", lowPassHz);
    NC_GET("compThresholdDb", compThresholdDb); NC_GET("compRatio", compRatio);
    NC_GET("compAttackMs", compAttackMs); NC_GET("compReleaseMs", compReleaseMs);
    NC_GET("compMix", compMix);
    NC_GET("compMakeupDb", compMakeupDb); NC_GET("compCeilingDb", compCeilingDb);
    NC_GET("saturatorDriveDb", saturatorDriveDb); NC_GET("saturatorMix", saturatorMix);
    NC_GET("gateThresholdDb", gateThresholdDb); NC_GET("gateRangeDb", gateRangeDb);
    NC_GET("gateAttackMs", gateAttackMs); NC_GET("gateHoldMs", gateHoldMs); NC_GET("gateReleaseMs", gateReleaseMs);
    NC_GET("eqHfGainDb", eqHfGainDb); NC_GET("eqHfHz", eqHfHz);
    NC_GET("eqHmfGainDb", eqHmfGainDb); NC_GET("eqHmfHz", eqHmfHz); NC_GET("eqHmfQ", eqHmfQ);
    NC_GET("eqLmfGainDb", eqLmfGainDb); NC_GET("eqLmfHz", eqLmfHz); NC_GET("eqLmfQ", eqLmfQ);
    NC_GET("eqLfGainDb", eqLfGainDb); NC_GET("eqLfHz", eqLfHz);
#undef NC_GET
    return 0;
}

void nc_track_set_console_bool(NCEngine* engine, int index, const char* parameter, bool value) {
    auto* t=trackAt(engine,index); if(t==nullptr||parameter==nullptr)return; const std::string p(parameter);
    if(p=="filterEnabled") {
        t->consoleChannel.filterEnabled=value;
        t->consoleChannel.highPassEnabled=value;
        t->consoleChannel.lowPassEnabled=value;
    } else if(p=="highPassEnabled") {
        t->consoleChannel.highPassEnabled=value;
        t->consoleChannel.filterEnabled=t->consoleChannel.highPassEnabled || t->consoleChannel.lowPassEnabled;
    } else if(p=="lowPassEnabled") {
        t->consoleChannel.lowPassEnabled=value;
        t->consoleChannel.filterEnabled=t->consoleChannel.highPassEnabled || t->consoleChannel.lowPassEnabled;
    } else if(p=="filterCircuitMode")t->consoleChannel.filterCircuitMode=value;
    else if(p=="eqEnabled")t->consoleChannel.eqEnabled=value;
    else if(p=="eqCircuitMode")t->consoleChannel.eqCircuitMode=value;
    else if(p=="eqHfBell")t->consoleChannel.eqHfBell=value; else if(p=="eqLfBell")t->consoleChannel.eqLfBell=value;
    else if(p=="eqEMode")t->consoleChannel.eqEMode=value; else if(p=="compEnabled")t->consoleChannel.compEnabled=value;
    else if(p=="compCircuitMode")t->consoleChannel.compCircuitMode=value;
    else if(p=="compFastAttack")t->consoleChannel.compFastAttack=value; else if(p=="compPeakMode")t->consoleChannel.compPeakMode=value;
    else if(p=="gateEnabled")t->consoleChannel.gateEnabled=value; else if(p=="gateCircuitMode")t->consoleChannel.gateCircuitMode=value;
    else if(p=="saturatorEnabled")t->consoleChannel.saturatorEnabled=value;
    else if(p=="dualMono")t->consoleChannel.dualMono=value;
    else if(p=="filterDualMono")t->consoleChannel.filterDualMono=value;
    else if(p=="eqDualMono")t->consoleChannel.eqDualMono=value;
    else if(p=="compDualMono")t->consoleChannel.compDualMono=value;
    else if(p=="gateDualMono")t->consoleChannel.gateDualMono=value;
    else if(p=="saturatorDualMono")t->consoleChannel.saturatorDualMono=value;
    else if(p=="saturatorCircuitMode")t->consoleChannel.saturatorCircuitMode=value;
    else if(p=="expanderMode")t->consoleChannel.expanderMode=value;
    else if(p=="gateFastAttack")t->consoleChannel.gateFastAttack=value;
    else if(p=="phaseInvert")t->consoleChannel.phaseInvert=value;
    else if(p=="phaseInvertL")t->consoleChannel.phaseInvertL=value;
    else if(p=="phaseInvertR")t->consoleChannel.phaseInvertR=value;
    else return;
    // Live push, NOT a reconcile: a full updateProject per lamp press stalled the main thread
    // for a beat (the plate buttons made it obvious). The full rebuild was only ever needed
    // when remote-strip membership followed the lamps — membership is assignment-only now, and
    // the processor's engage crossfades handle the audible transition.
    engine->engine.updateTrackConsoleChannel(t->name, t->consoleChannel);
    engine->recordStep("Console channel");
}

void nc_track_set_console_value(NCEngine* engine, int index, const char* parameter, float value) {
    auto* t=trackAt(engine,index); if(t==nullptr||parameter==nullptr)return; auto& c=t->consoleChannel; const std::string p(parameter);
#define NC_SET(name, field, lo, hi) if(p==name)c.field=std::max(lo,std::min(hi,value));else
    NC_SET("highPassHz",highPassHz,20.0f,350.0f) NC_SET("lowPassHz",lowPassHz,3000.0f,20000.0f)
    NC_SET("compThresholdDb",compThresholdDb,-20.0f,10.0f) NC_SET("compRatio",compRatio,1.0f,25.0f)
    NC_SET("compAttackMs",compAttackMs,0.1f,100.0f) NC_SET("compReleaseMs",compReleaseMs,40.0f,4000.0f)
    NC_SET("compMix",compMix,0.0f,1.0f)
    NC_SET("compMakeupDb",compMakeupDb,0.0f,24.0f) NC_SET("compCeilingDb",compCeilingDb,0.0f,24.0f)
    NC_SET("saturatorDriveDb",saturatorDriveDb,0.0f,24.0f) NC_SET("saturatorMix",saturatorMix,0.0f,1.0f)
    NC_SET("gateThresholdDb",gateThresholdDb,-30.0f,5.0f) NC_SET("gateRangeDb",gateRangeDb,0.0f,40.0f)
    NC_SET("gateAttackMs",gateAttackMs,0.05f,20.0f) NC_SET("gateHoldMs",gateHoldMs,0.0f,800.0f)
    NC_SET("gateReleaseMs",gateReleaseMs,40.0f,4000.0f)
    NC_SET("eqHfGainDb",eqHfGainDb,-18.0f,18.0f) NC_SET("eqHfHz",eqHfHz,1000.0f,20000.0f)
    NC_SET("eqHmfGainDb",eqHmfGainDb,-18.0f,18.0f) NC_SET("eqHmfHz",eqHmfHz,600.0f,7000.0f)
    NC_SET("eqHmfQ",eqHmfQ,0.2f,10.0f) NC_SET("eqLmfGainDb",eqLmfGainDb,-18.0f,18.0f)
    NC_SET("eqLmfHz",eqLmfHz,100.0f,2500.0f) NC_SET("eqLmfQ",eqLmfQ,0.2f,10.0f)
    NC_SET("eqLfGainDb",eqLfGainDb,-18.0f,18.0f) NC_SET("eqLfHz",eqLfHz,20.0f,600.0f) { return; }
#undef NC_SET
    // Continuous knob/wheel changes: push the params straight into the render plan (the processor
    // ramps to them per sample). Calling reconcileProject() here rebuilt the whole render plan on
    // every value — 60×/s during a drag — which is what zippered the EQ. The caller records one
    // undo step when the gesture ends.
    engine->engine.updateTrackConsoleChannel(t->name, t->consoleChannel);
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

namespace {
// Auto latency mode: when arming flips the session between "someone plays through the chain"
// and "nobody does", the remote streams re-buffer (tracking <-> mixing). Compare the effective
// buffer before/after rather than counting armed tracks — fixed modes then never re-push.
template <typename Fn>
void withAutoLatencyRepush(NCEngine* engine, Fn&& change) {
    const auto before = effectiveRemoteBufferFrames(engine->project);
    change();
    if (effectiveRemoteBufferFrames(engine->project) != before) {
        engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    }
}
}  // namespace

void nc_track_set_record_armed(NCEngine* engine, int index, bool armed) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    withAutoLatencyRepush(engine, [&] {
        neuracoust::daw::setTrackRecordArmed(engine->project, track->name, armed);
    });
    pushTrackRealtimeState(engine, *track);
    engine->recordStep("Record arm");
}

void nc_track_set_input_monitoring(NCEngine* engine, int index, bool monitoring) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return;
    withAutoLatencyRepush(engine, [&] {
        neuracoust::daw::setTrackInputMonitoring(engine->project, track->name, monitoring);
    });
    pushTrackRealtimeState(engine, *track);
    engine->recordStep("Input monitoring");
}

// Apply one flag to several tracks in a single undo step (Pro Tools: a mute/solo/arm on a track
// that is part of the selection hits the whole selection). flag: 0=mute 1=solo 2=armed 3=inputMon.
// No add/remove happens, so the resolved pointers stay valid across the loop.
bool nc_track_set_flag_many(NCEngine* engine, const int* indices, int count, int flag, bool value) {
    if (engine == nullptr || indices == nullptr || count <= 0) return false;
    bool any = false;
    for (int i = 0; i < count; ++i) {
        auto* track = trackAt(engine, indices[i]);
        if (track == nullptr) continue;
        switch (flag) {
            case 0: neuracoust::daw::setTrackMuted(engine->project, track->name, value); break;
            case 1: neuracoust::daw::setTrackSolo(engine->project, track->name, value); break;
            case 2: neuracoust::daw::setTrackRecordArmed(engine->project, track->name, value); break;
            case 3: neuracoust::daw::setTrackInputMonitoring(engine->project, track->name, value); break;
            default: return false;
        }
        pushTrackRealtimeState(engine, *track);
        any = true;
    }
    if (!any) return false;
    engine->recordStep(flag == 0 ? "Mute" : flag == 1 ? "Solo" : flag == 2 ? "Record arm" : "Input monitoring");
    return true;
}

namespace {

/// A fresh track changes the render graph and the lane list; adopt it fully.
int adoptNewTrack(NCEngine* engine, const std::string& trackName, const char* stepName) {
    if (trackName.empty()) {
        return -1;
    }
    engine->reconcileProjectDeclicked();   // adding a track/bus rebuilds the mixer graph → declick
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

// Delete several tracks in one undo step (Pro Tools: select many, one Delete removes them all).
// Track ids are indices, and deleting shifts them — so resolve every index to its stable name
// FIRST, then delete by name. Rebuild/reconcile/record just once.
bool nc_track_delete_many(NCEngine* engine, const int* indices, int count, bool removeClips) {
    if (engine == nullptr || indices == nullptr || count <= 0) return false;
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const auto* track = trackAt(engine, indices[i]);
        if (track != nullptr) names.push_back(track->name);
    }
    if (names.empty()) return false;
    bool any = false;
    for (const auto& name : names) {
        if (neuracoust::daw::deleteTrack(engine->project, name, removeClips, removeClips)) any = true;
    }
    if (!any) return false;
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep(names.size() > 1 ? "Delete tracks" : "Delete track");
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

    // Report the editor observer whenever this insert ACTUALLY runs out-of-process in the render,
    // because only then does a worker exist to publish the observer shm. This MUST match the
    // render's rule (MasterInsertProcessor: preferOutOfProcess = dspExecutionMode == "internal",
    // OR the plug-in's brand is force-sandboxed). Keying this purely on the brand policy — as it
    // did — meant an Internal-DSP insert whose brand isn't on the sandbox list got an observer shm
    // the editor was never told about, so its meters sat dark. FabFilter set to Internal DSP was
    // exactly that case: audio hosted out-of-process, but no observer name handed to the editor.
    bool outOfProcess = insert->dspExecutionMode == "internal";
    if (!outOfProcess) {
        // Brand-forced sandbox (e.g. an "unsafe in-process" vendor). The slot doesn't store the
        // brand; the scan does, so match the slot back to the plug-in it came from.
        const auto found = std::find_if(engine->plugins.begin(), engine->plugins.end(),
                                        [&](const neuracoust::daw::PluginCandidate& candidate) {
                                            return candidate.path == insert->pluginPath;
                                        });
        if (found != engine->plugins.end()) {
            neuracoust::daw::Vst3PluginDescriptor descriptor;
            descriptor.name = found->pluginName.empty() ? found->name : found->pluginName;
            descriptor.brand = found->brand;
            descriptor.vendor = found->brand;
            descriptor.bundlePath = found->path;
            outOfProcess = neuracoust::daw::isVst3HostedOutOfProcess(descriptor);
        }
    }
    if (!outOfProcess) {
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

// The renderer plays track.instrumentSlots, not the legacy track.instrument mirror, so
// parameter and editor access must target the actual rack slot — otherwise an editor knob or
// preset writes to a copy the sound never reads. Resolves slot `slotIndex`, materializing the
// legacy single instrument into slot 0 so access is uniform. Returns nullptr if empty.
neuracoust::daw::InstrumentSlotState* instrumentSlotMutable(NCEngine* engine, int trackIndex, size_t slotIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return nullptr;
    if (track->instrumentSlots.empty() && !track->instrument.pluginPath.empty() &&
        track->instrument.pluginName != "No Instrument") {
        track->instrumentSlots.push_back(track->instrument);
    }
    if (slotIndex >= track->instrumentSlots.size()) return nullptr;
    auto& slot = track->instrumentSlots[slotIndex];
    if (slot.pluginPath.empty() || slot.pluginName.empty() || slot.pluginName == "No Instrument") {
        return nullptr;
    }
    return &slot;
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

// Per-slot plug-in descriptor, so a layer's editor can be opened and addressed individually.
void nc_track_instrument_slot_plugin_path(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen) {
    const auto* slot = slotIndex < 0 ? nullptr : instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    copyText(out, outLen, slot != nullptr ? slot->pluginPath : std::string{});
}
void nc_track_instrument_slot_plugin_format(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen) {
    const auto* slot = slotIndex < 0 ? nullptr : instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    copyText(out, outLen, slot != nullptr ? slot->pluginFormat : std::string{});
}
void nc_track_instrument_slot_class_id(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen) {
    const auto* slot = slotIndex < 0 ? nullptr : instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    copyText(out, outLen, slot != nullptr ? slot->pluginClassId : std::string{});
}
void nc_track_instrument_slot_class_name(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen) {
    const auto* slot = slotIndex < 0 ? nullptr : instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    copyText(out, outLen, slot != nullptr ? slot->pluginClassName : std::string{});
}

// Read/write instrument parameters on a specific rack slot (slot 0 = the primary instrument).
// These are what the editor host talks to; they target instrumentSlots so a knob turn is heard.
int nc_track_instrument_slot_param_count(NCEngine* engine, int index, int slotIndex) {
    if (slotIndex < 0) return 0;
    const auto* slot = instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    return slot != nullptr ? static_cast<int>(slot->parameters.size()) : 0;
}

uint32_t nc_track_instrument_slot_param_id(NCEngine* engine, int index, int slotIndex, int paramIndex) {
    if (slotIndex < 0) return 0;
    const auto* slot = instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    if (slot == nullptr || paramIndex < 0 || static_cast<size_t>(paramIndex) >= slot->parameters.size()) {
        return 0;
    }
    return slot->parameters[static_cast<size_t>(paramIndex)].parameterId;
}

double nc_track_instrument_slot_param_value(NCEngine* engine, int index, int slotIndex, int paramIndex) {
    if (slotIndex < 0) return 0.0;
    const auto* slot = instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    if (slot == nullptr || paramIndex < 0 || static_cast<size_t>(paramIndex) >= slot->parameters.size()) {
        return 0.0;
    }
    return slot->parameters[static_cast<size_t>(paramIndex)].normalizedValue;
}

bool nc_track_set_instrument_slot_vst3_parameter(NCEngine* engine, int index, int slotIndex,
                                                 uint32_t parameterId, const char* displayName,
                                                 double normalizedValue) {
    if (slotIndex < 0) return false;
    auto* slot = instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    if (slot == nullptr) return false;

    const double clamped = std::max(0.0, std::min(1.0, normalizedValue));
    const std::string name = displayName != nullptr ? displayName : "";
    auto& parameters = slot->parameters;
    auto found = std::find_if(parameters.begin(), parameters.end(),
                              [&](const neuracoust::daw::Vst3ParameterValueState& parameter) {
                                  return parameter.parameterId == parameterId;
                              });
    if (found != parameters.end()) {
        found->normalizedValue = clamped;
        if (!name.empty()) found->displayName = name;
    } else {
        parameters.push_back({parameterId,
                              name.empty() ? "Param " + std::to_string(parameterId) : name,
                              clamped});
    }
    // Keep the legacy mirror in step so display/name reads stay correct for slot 0.
    auto* track = trackAt(engine, index);
    if (track != nullptr && slotIndex == 0 && !track->instrumentSlots.empty()) {
        track->instrument = track->instrumentSlots.front();
    }
    // Push straight into the live render plan instead of reconciling: the renderer reads instrument
    // parameters from the plan every block, so a knob turn is heard on the next block with no plugin
    // rebuild — no more sound dropping out and re-appearing while adjusting an instrument. The project
    // model above is still the source of truth for save / undo / a later full reconcile.
    if (track != nullptr) {
        engine->engine.updateInstrumentVst3Parameter(track->name, static_cast<size_t>(slotIndex),
                                                      parameterId, name, clamped);
    }
    return true;
}

// Slot 0 (primary instrument) accessors — the existing API, now routed through the rack slot
// the renderer actually plays rather than the stale track.instrument mirror.
int nc_track_instrument_param_count(NCEngine* engine, int index) {
    return nc_track_instrument_slot_param_count(engine, index, 0);
}
uint32_t nc_track_instrument_param_id(NCEngine* engine, int index, int paramIndex) {
    return nc_track_instrument_slot_param_id(engine, index, 0, paramIndex);
}
double nc_track_instrument_param_value(NCEngine* engine, int index, int paramIndex) {
    return nc_track_instrument_slot_param_value(engine, index, 0, paramIndex);
}
bool nc_track_set_instrument_vst3_parameter(NCEngine* engine, int index, uint32_t parameterId,
                                            const char* displayName, double normalizedValue) {
    return nc_track_set_instrument_slot_vst3_parameter(engine, index, 0, parameterId,
                                                       displayName, normalizedValue);
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
    // A bus receives from sends addressed to its own name, not a hardware input. Leaving
    // inputBus at the "Input 1" default made the renderer receive on "Input 1" instead of
    // "Aux N", so send signal never reached the aux. Clear it, like the Master bus.
    aux.inputBus.clear();
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
    engine->reconcileProjectDeclicked();
    return true;
}

void nc_track_set_send_gain_db(NCEngine* engine, int index, int slot, float db) {
    auto* track = trackAt(engine, index);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->sends.size()) return;
    neuracoust::daw::TrackSendState send = track->sends[static_cast<size_t>(slot)];
    send.gainDb = db;
    if (!neuracoust::daw::setTrackSendSlot(engine->project, track->name, static_cast<size_t>(slot), send)) return;
    // Continuous: push only the live send gain (no graph reconcile → no dropout). The view
    // records one history step when the drag ends, like the fader.
    engine->engine.updateTrackSendGain(track->name, slot, db);
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
    engine->reconcileProjectDeclicked();
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
    // Undo rebuilds the whole graph — declick it (fade → swap → fade) like the edit ops, or the
    // rebuild clicks during playback.
    engine->engine.beginGraphChangeDeclick();
    applyRestoredProject(engine);
    engine->engine.endGraphChangeDeclick();
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
    engine->engine.beginGraphChangeDeclick();
    applyRestoredProject(engine);
    engine->engine.endGraphChangeDeclick();
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

namespace {
// Copy every media source that lives OUTSIDE this project's Audio Files folder into it, rewrite the
// references, and re-save. Operates on a given project+path so it serves both the live document
// (consolidate) and a throwaway value-copy (save-a-copy). Audio AND video travel — both reference
// mediaSources. Returns the number of files copied (0 = already self-contained), -1 on error.
int consolidateProjectMedia(neuracoust::daw::ProjectDocument& project, const std::string& projectPath,
                            std::string& error) {
    if (projectPath.empty()) { error = "save the project before consolidating media"; return -1; }
    const std::filesystem::path audioDir = neuracoust::daw::projectAudioFilesDirectory(projectPath);
    std::error_code ec;
    std::filesystem::create_directories(audioDir, ec);
    if (ec) { error = std::string("could not create the Audio Files folder: ") + ec.message(); return -1; }
    const auto canonicalAudioDir = std::filesystem::weakly_canonical(audioDir, ec);

    std::map<std::string, std::string> remap;
    int copied = 0;
    auto consolidatePath = [&](std::string& path) {
        if (path.empty()) return;
        const auto existing = remap.find(path);
        if (existing != remap.end()) { path = existing->second; return; }
        std::error_code fileEc;
        const std::filesystem::path src(path);
        if (!std::filesystem::is_regular_file(src, fileEc)) return;
        if (std::filesystem::weakly_canonical(src.parent_path(), fileEc) == canonicalAudioDir) return;  // already inside
        std::filesystem::path dest = audioDir / src.filename();
        for (int suffix = 1; std::filesystem::exists(dest, fileEc); ++suffix) {
            dest = audioDir / (src.stem().string() + "-" + std::to_string(suffix) + src.extension().string());
        }
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::none, fileEc);
        if (fileEc) return;
        remap[path] = dest.string();
        path = dest.string();
        ++copied;
    };
    for (auto& source : project.mediaSources) consolidatePath(source.path);
    for (auto& clip : project.clips) consolidatePath(clip.sourcePath);

    if (copied > 0) {
        std::string saveError;
        neuracoust::daw::saveProjectFileWithBackup(project, projectPath, saveError);
    }
    error.clear();
    return copied;
}
}  // namespace

int nc_project_consolidate_media(NCEngine* engine, char* error, size_t errorLen) {
    if (engine == nullptr) { copyText(error, errorLen, "no engine"); return -1; }
    if (engine->projectPath.empty()) {
        copyText(error, errorLen, "save the project before consolidating media");
        return -1;
    }
    std::string e;
    const int copied = consolidateProjectMedia(engine->project, engine->projectPath, e);
    if (copied > 0) engine->reconcileProject();   // paths changed on the live render source
    copyText(error, errorLen, e);
    return copied;
}

// Save a fully self-contained COPY to `path` (collecting external media into its Audio Files) while
// leaving the working session untouched — a value copy of the project is saved, so the live
// document's paths, path binding, and dirty state never change.
int nc_project_save_copy(NCEngine* engine, const char* path, char* error, size_t errorLen) {
    if (engine == nullptr || path == nullptr || *path == '\0') {
        copyText(error, errorLen, "no project path");
        return -1;
    }
    neuracoust::daw::ProjectDocument copy = engine->project;   // value copy — session is untouched
    std::string saveError;
    if (!neuracoust::daw::saveProjectFileWithBackup(copy, std::filesystem::path(path), saveError)) {
        copyText(error, errorLen, saveError.empty() ? "could not save the copy" : saveError);
        return -1;
    }
    std::string e;
    const int copied = consolidateProjectMedia(copy, path, e);   // pulls external media into the copy + re-saves
    copyText(error, errorLen, e);
    return copied < 0 ? 0 : copied;
}

bool nc_audio_import_supported(const char* path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    return neuracoust::daw::isSupportedImportAudioExtension(std::filesystem::path(path));
}

bool nc_audio_import_analyzed(NCEngine* engine, int trackIndex, const char* path, double startSeconds,
                             bool analyze, bool applyToTimeline, char* error, size_t errorLen) {
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
                                          importError,
                                          analyze,
                                          applyToTimeline)) {
        copyText(error, errorLen, importError.empty() ? "import failed" : importError);
        return false;
    }

    engine->reconcileProject();
    engine->recordStep("Import " + std::filesystem::path(path).filename().string());
    // Hand the analysis summary back so the UI can report what was detected.
    copyText(error, errorLen, result.message);
    return true;
}

bool nc_audio_import(NCEngine* engine, int trackIndex, const char* path, double startSeconds,
                     char* error, size_t errorLen) {
    return nc_audio_import_analyzed(engine, trackIndex, path, startSeconds, true, true, error, errorLen);
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

// Defined below (same anonymous namespace) — forward-declared so the lightweight-drag helpers here
// can use them.
namespace {
std::vector<const neuracoust::daw::ClipState*> resolveClips(NCEngine* engine, const char* const* clipIds, int count);
double earliestStart(const std::vector<const neuracoust::daw::ClipState*>& clips);
}

bool nc_clip_move(NCEngine* engine, const char* clipId, double newStartSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::moveClip(engine->project, clipId,
                                                           std::max(0.0, newStartSeconds)));
}

// Lightweight clip move for a LIVE DRAG: update the model + slide the clip in the render plan IN
// PLACE. No rebuild, no reconcile — so the music never stops/gaps while dragging (the per-frame full
// reconcile was the cause). commit once on drop via nc_project_reconcile.
bool nc_clip_update_start(NCEngine* engine, const char* clipId, double startSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    if (!neuracoust::daw::moveClip(engine->project, clipId, std::max(0.0, startSeconds))) return false;
    for (const auto& c : engine->project.clips) {
        if (c.id == clipId) { engine->engine.updateClipStart(clipId, c.startSeconds); break; }
    }
    return true;
}

int nc_clip_update_start_many(NCEngine* engine, const char* const* clipIds, int count, double deltaSeconds) {
    const auto clips = resolveClips(engine, clipIds, count);
    if (clips.empty() || !std::isfinite(deltaSeconds)) return 0;
    const double delta = std::max(deltaSeconds, -earliestStart(clips));
    std::vector<std::pair<std::string, double>> targets;
    targets.reserve(clips.size());
    for (const auto* clip : clips) targets.emplace_back(clip->id, clip->startSeconds + delta);
    int moved = 0;
    for (const auto& target : targets) {
        if (neuracoust::daw::moveClip(engine->project, target.first, target.second)) {
            for (const auto& c : engine->project.clips) {
                if (c.id == target.first) { engine->engine.updateClipStart(target.first, c.startSeconds); break; }
            }
            ++moved;
        }
    }
    return moved;
}

// Canonicalize the project after a lightweight drag: rebuild the playlist from clips and reconcile
// once (on drop). Seamless — updateProject preserves the continuous-playback buffers (resetForEdit).
void nc_project_reconcile(NCEngine* engine) {
    if (engine == nullptr) return;
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
}

bool nc_clip_trim_start(NCEngine* engine, const char* clipId, double newStartSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    // Extending a trim reveals fresh source audio mid-playback → declick the reconcile (a shortening
    // trim just gets a harmless brief dip).
    engine->engine.beginGraphChangeDeclick();
    const bool changed = applyClipEdit(engine, neuracoust::daw::trimClipStart(engine->project, clipId,
                                                                              std::max(0.0, newStartSeconds)));
    engine->engine.endGraphChangeDeclick();
    return changed;
}

bool nc_clip_trim_end(NCEngine* engine, const char* clipId, double newEndSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    // Clamp the end to the source file — a clip can't be dragged out past the file's audio (doing so
    // played trailing silence). Max end = start + (file length − where in the file the clip begins).
    for (const auto& clip : engine->project.clips) {
        if (clip.id != clipId) continue;
        const auto cached = engine->waveformCache.find(clip.sourcePath);
        if (cached != engine->waveformCache.end() && cached->second.durationSeconds > 0.0) {
            const double maxEnd = clip.startSeconds + (cached->second.durationSeconds - clip.sourceOffsetSeconds);
            if (newEndSeconds > maxEnd) newEndSeconds = maxEnd;
        }
        break;
    }
    engine->engine.beginGraphChangeDeclick();
    const bool changed = applyClipEdit(engine, neuracoust::daw::trimClipEnd(engine->project, clipId, newEndSeconds));
    engine->engine.endGraphChangeDeclick();
    return changed;
}

// LIVE-DRAG trim (start/end): update the model + patch the clip's bounds in the render plan IN PLACE.
// No rebuild, no reconcile, no declick — the music keeps playing while the clip is stretched. commit
// once on drop via nc_project_reconcile (same as the lightweight move). Mirrors nc_clip_update_start.
bool nc_clip_update_trim_start(NCEngine* engine, const char* clipId, double newStartSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    if (!neuracoust::daw::trimClipStart(engine->project, clipId, std::max(0.0, newStartSeconds))) return false;
    for (const auto& c : engine->project.clips) {
        if (c.id == clipId) {
            engine->engine.updateClipBounds(clipId, c.startSeconds, c.durationSeconds, c.sourceOffsetSeconds);
            break;
        }
    }
    return true;
}

bool nc_clip_update_trim_end(NCEngine* engine, const char* clipId, double newEndSeconds) {
    if (engine == nullptr || clipId == nullptr) return false;
    // Same source-length clamp as nc_clip_trim_end — a clip cannot be dragged past the file's audio.
    for (const auto& clip : engine->project.clips) {
        if (clip.id != clipId) continue;
        const auto cached = engine->waveformCache.find(clip.sourcePath);
        if (cached != engine->waveformCache.end() && cached->second.durationSeconds > 0.0) {
            const double maxEnd = clip.startSeconds + (cached->second.durationSeconds - clip.sourceOffsetSeconds);
            if (newEndSeconds > maxEnd) newEndSeconds = maxEnd;
        }
        break;
    }
    if (!neuracoust::daw::trimClipEnd(engine->project, clipId, newEndSeconds)) return false;
    for (const auto& c : engine->project.clips) {
        if (c.id == clipId) {
            engine->engine.updateClipBounds(clipId, c.startSeconds, c.durationSeconds, c.sourceOffsetSeconds);
            break;
        }
    }
    return true;
}

// Offline time-stretch + pitch-shift PRINT (Serato Time & Pitch phase vocoder). Renders the clip's
// played window to a new WAV in the project's Audio Files folder and repoints the clip at it — offset
// 0, duration scaled by timeRatio, fades scaled to match. timeRatio (0.125..8) changes length,
// semitones (±24) change pitch, independently. Anchors (normalized [0,1], matched source→dest) drive
// a piecewise time remap when present; empty anchors = a uniform stretch.
static bool applyClipTimeTransform(NCEngine* engine, const char* clipId,
                                   double timeRatio, double semitones,
                                   const std::vector<double>& srcAnchors,
                                   const std::vector<double>& destAnchors,
                                   bool formantPreserve,
                                   char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || clipId == nullptr) { copyText(error, errorLen, "invalid arguments"); return false; }
    neuracoust::daw::ClipState* clip = nullptr;
    for (auto& c : engine->project.clips) { if (c.id == clipId) { clip = &c; break; } }
    if (clip == nullptr) { copyText(error, errorLen, "clip not found"); return false; }

    const double ratio = std::clamp(timeRatio, 0.125, 8.0);
    const double semis = std::clamp(semitones, -24.0, 24.0);
    if (srcAnchors.empty() && std::abs(ratio - 1.0) < 1e-6 && std::abs(semis) < 1e-6) return true;   // no-op

    neuracoust::daw::WavAudioData src;
    std::string e;
    if (!neuracoust::daw::readPcmWavFile(clip->sourcePath, src, e)) {
        copyText(error, errorLen, "could not read clip audio: " + e); return false;
    }
    if (src.channels < 1 || src.sampleRate <= 0.0) { copyText(error, errorLen, "unsupported clip audio"); return false; }

    // Extract the clip's played window (sourceOffset .. +duration) at the source rate.
    const int64_t total = static_cast<int64_t>(src.interleavedSamples.size()) / src.channels;
    const int64_t startFrame = std::max<int64_t>(0, std::llround(clip->sourceOffsetSeconds * src.sampleRate));
    int64_t winFrames = std::llround(clip->durationSeconds * src.sampleRate);
    if (winFrames <= 0) winFrames = std::max<int64_t>(1, total - startFrame);
    std::vector<float> window(static_cast<size_t>(winFrames) * src.channels, 0.0f);
    for (int64_t i = 0; i < winFrames; ++i) {
        const int64_t s = startFrame + i;
        if (s < 0 || s >= total) continue;
        for (int ch = 0; ch < src.channels; ++ch)
            window[static_cast<size_t>(i * src.channels + ch)] =
                src.interleavedSamples[static_cast<size_t>(s * src.channels + ch)];
    }

    neuracoust::daw::TimePitchParams p;
    p.timeRatio = ratio;
    p.semitones = semis;
    // Formant preservation only matters when the pitch actually changes; the wrapper no-ops otherwise.
    std::vector<float> rendered = (formantPreserve && std::abs(semis) > 1e-6)
        ? neuracoust::daw::processTimeMapFormantPreserving(window, src.channels, p, src.sampleRate, srcAnchors, destAnchors)
        : neuracoust::daw::processTimeMapInterleaved(window, src.channels, p, srcAnchors, destAnchors);
    if (rendered.empty()) { copyText(error, errorLen, "time/pitch produced no audio"); return false; }

    // Write to the project's Audio Files folder (a temp folder when the project is unsaved).
    std::error_code ec;
    const std::filesystem::path dir = engine->projectPath.empty()
        ? neuracoust::daw::temporaryImportAudioFilesDirectory()
        : neuracoust::daw::projectAudioFilesDirectory(engine->projectPath);
    std::filesystem::create_directories(dir, ec);
    const std::string stem = std::filesystem::path(clip->sourcePath).stem().string();
    char suffix[80];
    std::snprintf(suffix, sizeof suffix, "_tp%dr%dst", static_cast<int>(std::lround(ratio * 1000)),
                  static_cast<int>(std::lround(semis * 100)));
    const std::filesystem::path outPath = dir / (stem + suffix + "_" + clip->id + ".wav");

    neuracoust::daw::WavAudioData outData;
    outData.channels = src.channels;
    outData.sampleRate = src.sampleRate;
    outData.interleavedSamples = std::move(rendered);
    if (!neuracoust::daw::writePcm24WavFileAtomically(outPath, outData, e)) {
        copyText(error, errorLen, "could not write rendered clip: " + e); return false;
    }

    clip->sourcePath = outPath.string();
    clip->sourceOffsetSeconds = 0.0;
    clip->durationSeconds *= ratio;
    clip->fadeInSeconds *= ratio;
    clip->fadeOutSeconds *= ratio;
    clip->sourceSampleRate = src.sampleRate;
    clip->sourceChannels = src.channels;
    clip->timeScale = 1.0;
    clip->sourceFileUid.clear();
    engine->waveformCache.erase(outPath.string());

    const bool changed = applyClipEdit(engine, true);
    if (changed) engine->recordStep("Apply time/pitch");
    return changed;
}

bool nc_clip_apply_time_pitch(NCEngine* engine, const char* clipId,
                              double timeRatio, double semitones, int formantPreserve,
                              char* error, size_t errorLen) {
    return applyClipTimeTransform(engine, clipId, timeRatio, semitones, {}, {}, formantPreserve != 0, error, errorLen);
}

// Piecewise time remap (Serato anchor time map): sourceAnchors/destAnchors are matched normalized
// [0,1] positions across the clip; each segment stretches independently to its dest span, at the
// global pitch. anchorCount 0 = a uniform stretch (same as nc_clip_apply_time_pitch).
bool nc_clip_apply_time_map(NCEngine* engine, const char* clipId, double timeRatio, double semitones,
                            const double* sourceAnchors, const double* destAnchors, int anchorCount,
                            int formantPreserve, char* error, size_t errorLen) {
    std::vector<double> src, dst;
    if (sourceAnchors != nullptr && destAnchors != nullptr && anchorCount > 0) {
        src.assign(sourceAnchors, sourceAnchors + anchorCount);
        dst.assign(destAnchors, destAnchors + anchorCount);
    }
    return applyClipTimeTransform(engine, clipId, timeRatio, semitones, src, dst, formantPreserve != 0, error, errorLen);
}

namespace {
// Read a clip's played window (sourceOffset .. +duration) as interleaved float + its rate/channels.
neuracoust::daw::ClipState* readClipWindow(NCEngine* engine, const char* clipId,
                                           std::vector<float>& window, int& channels, double& rate,
                                           std::string& err) {
    neuracoust::daw::ClipState* clip = nullptr;
    for (auto& c : engine->project.clips) if (c.id == clipId) { clip = &c; break; }
    if (clip == nullptr) { err = "clip not found"; return nullptr; }
    neuracoust::daw::WavAudioData src;
    if (!neuracoust::daw::readPcmWavFile(clip->sourcePath, src, err)) return nullptr;
    if (src.channels < 1 || src.sampleRate <= 0.0) { err = "unsupported clip audio"; return nullptr; }
    const int64_t total = static_cast<int64_t>(src.interleavedSamples.size()) / src.channels;
    const int64_t startF = std::max<int64_t>(0, std::llround(clip->sourceOffsetSeconds * src.sampleRate));
    int64_t nF = std::llround(clip->durationSeconds * src.sampleRate);
    if (nF <= 0) nF = std::max<int64_t>(1, total - startF);
    window.assign(static_cast<size_t>(nF) * src.channels, 0.0f);
    for (int64_t i = 0; i < nF; ++i) {
        const int64_t s = startF + i;
        if (s < 0 || s >= total) continue;
        for (int c = 0; c < src.channels; ++c)
            window[static_cast<size_t>(i * src.channels + c)] = src.interleavedSamples[static_cast<size_t>(s * src.channels + c)];
    }
    channels = src.channels;
    rate = src.sampleRate;
    return clip;
}
}  // namespace

// --- Melodyne-mode pitch editing -----------------------------------------------------------------
// Detect the notes in a clip's window (YIN + segmentation), cache them on the engine, and return the
// count. Getters expose each note; nc_clip_note_set_offset records a per-note edit; apply renders.
int nc_clip_detect_notes(NCEngine* engine, const char* clipId, int mode) {
    if (engine == nullptr || clipId == nullptr) return 0;
    std::vector<float> window; int channels = 0; double rate = 0.0; std::string err;
    if (readClipWindow(engine, clipId, window, channels, rate, err) == nullptr) return 0;
    const auto detectionMode = mode == 2 ? neuracoust::daw::DetectionMode::Percussive
                             : mode == 1 ? neuracoust::daw::DetectionMode::Polyphonic
                                         : neuracoust::daw::DetectionMode::Melodic;
    engine->pitchEditNotes = neuracoust::daw::detectNotesForMode(window, channels, rate, detectionMode);
    engine->pitchEditClipId = clipId;
    return static_cast<int>(engine->pitchEditNotes.size());
}
int nc_clip_note_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->pitchEditNotes.size()) : 0;
}
static const neuracoust::daw::DetectedNote* noteAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || index >= static_cast<int>(engine->pitchEditNotes.size())) return nullptr;
    return &engine->pitchEditNotes[static_cast<size_t>(index)];
}
static neuracoust::daw::DetectedNote* mutableNoteAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || index >= static_cast<int>(engine->pitchEditNotes.size())) return nullptr;
    return &engine->pitchEditNotes[static_cast<size_t>(index)];
}
double nc_clip_note_start_seconds(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->startSeconds : 0.0; }
double nc_clip_note_duration_seconds(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->durationSeconds : 0.0; }
double nc_clip_note_detected_midi(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->detectedMidi : 0.0; }
double nc_clip_note_offset_semitones(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->pitchOffsetSemitones : 0.0; }
double nc_clip_note_time_offset_seconds(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->timeOffsetSeconds : 0.0; }
double nc_clip_note_duration_scale(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->durationScale : 1.0; }
double nc_clip_note_confidence(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->confidence : 0.0; }

// The rest of the Melodyne palette. Each clamps to the range renderNoteEdits honours, so a UI that
// drags past the end stops rather than producing something the render would reinterpret.
double nc_clip_note_gain_db(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->gainDb : 0.0; }
bool nc_clip_note_muted(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n != nullptr && n->muted; }
double nc_clip_note_formant_semitones(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->formantSemitones : 0.0; }
double nc_clip_note_attack_speed(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->attackSpeed : 1.0; }
void nc_clip_note_set_gain_db(NCEngine* engine, int index, double gainDb) {
    if (auto* n = mutableNoteAt(engine, index)) n->gainDb = std::clamp(gainDb, -24.0, 24.0);
}
void nc_clip_note_set_muted(NCEngine* engine, int index, bool muted) {
    if (auto* n = mutableNoteAt(engine, index)) n->muted = muted;
}
void nc_clip_note_set_formant_semitones(NCEngine* engine, int index, double semitones) {
    if (auto* n = mutableNoteAt(engine, index)) n->formantSemitones = std::clamp(semitones, -24.0, 24.0);
}
void nc_clip_note_set_attack_speed(NCEngine* engine, int index, double speed) {
    if (auto* n = mutableNoteAt(engine, index)) n->attackSpeed = std::clamp(speed, 0.25, 4.0);
}
double nc_clip_note_modulation_scale(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->pitchModulationScale : 1.0; }
double nc_clip_note_drift_scale(NCEngine* engine, int index) { auto* n = noteAt(engine, index); return n ? n->pitchDriftScale : 1.0; }
void nc_clip_note_set_modulation_scale(NCEngine* engine, int index, double scale) {
    if (auto* n = mutableNoteAt(engine, index)) n->pitchModulationScale = std::clamp(scale, 0.0, 4.0);
}
void nc_clip_note_set_drift_scale(NCEngine* engine, int index, double scale) {
    if (auto* n = mutableNoteAt(engine, index)) n->pitchDriftScale = std::clamp(scale, 0.0, 4.0);
}
/// Puts one note back to untouched — Melodyne's "reset" on a blob.
void nc_clip_note_reset(NCEngine* engine, int index) {
    auto* n = mutableNoteAt(engine, index);
    if (n == nullptr) return;
    n->pitchOffsetSemitones = 0.0;
    n->timeOffsetSeconds = 0.0;
    n->durationScale = 1.0;
    n->gainDb = 0.0;
    n->muted = false;
    n->formantSemitones = 0.0;
    n->attackSpeed = 1.0;
    n->pitchModulationScale = 1.0;
    n->pitchDriftScale = 1.0;
}

void nc_clip_note_set_offset(NCEngine* engine, int index, double semitones) {
    if (engine == nullptr || index < 0 || index >= static_cast<int>(engine->pitchEditNotes.size())) return;
    engine->pitchEditNotes[static_cast<size_t>(index)].pitchOffsetSemitones = std::clamp(semitones, -24.0, 24.0);
}
void nc_clip_note_set_time_offset(NCEngine* engine, int index, double seconds) {
    if (engine == nullptr || index < 0 || index >= static_cast<int>(engine->pitchEditNotes.size())) return;
    auto& note = engine->pitchEditNotes[static_cast<size_t>(index)];
    note.timeOffsetSeconds = std::clamp(seconds, -note.startSeconds,
                                        std::max(0.0, 86400.0 - note.startSeconds));
}
void nc_clip_note_set_duration_scale(NCEngine* engine, int index, double scale) {
    if (engine == nullptr || index < 0 || index >= static_cast<int>(engine->pitchEditNotes.size())) return;
    engine->pitchEditNotes[static_cast<size_t>(index)].durationScale = std::clamp(scale, 0.25, 4.0);
}
bool nc_clip_note_split(NCEngine* engine, int index, double localSeconds) {
    if (engine == nullptr || index < 0 || index >= static_cast<int>(engine->pitchEditNotes.size())) return false;
    auto& notes = engine->pitchEditNotes;
    const auto original = notes[static_cast<size_t>(index)];
    const double relative = localSeconds - original.startSeconds;
    if (relative < 0.02 || relative > original.durationSeconds - 0.02) return false;
    auto left = original;
    auto right = original;
    left.durationSeconds = relative;
    left.durationScale = 1.0;
    right.startSeconds = localSeconds;
    right.durationSeconds = original.durationSeconds - relative;
    right.timeOffsetSeconds = original.timeOffsetSeconds;
    right.durationScale = 1.0;
    notes[static_cast<size_t>(index)] = left;
    notes.insert(notes.begin() + index + 1, right);
    return true;
}

// Render the cached per-note offsets into a new WAV and repoint the clip (length preserved, so start/
// duration are unchanged — only the source is swapped). No-op if nothing was moved.
bool nc_clip_apply_note_edits(NCEngine* engine, const char* clipId, char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || clipId == nullptr) { copyText(error, errorLen, "invalid arguments"); return false; }
    if (engine->pitchEditClipId != clipId || engine->pitchEditNotes.empty()) {
        copyText(error, errorLen, "no detected notes for this clip"); return false;
    }
    bool anyEdit = false;
    for (const auto& n : engine->pitchEditNotes)
        if (std::abs(n.pitchOffsetSemitones) >= 0.01 || std::abs(n.timeOffsetSeconds) >= 0.0001 ||
            std::abs(n.durationScale - 1.0) >= 0.001) {
            anyEdit = true; break;
        }
    if (!anyEdit) return true;   // nothing to do

    std::vector<float> window; int channels = 0; double rate = 0.0; std::string err;
    neuracoust::daw::ClipState* clip = readClipWindow(engine, clipId, window, channels, rate, err);
    if (clip == nullptr) { copyText(error, errorLen, "read clip audio: " + err); return false; }

    std::vector<float> rendered = neuracoust::daw::renderNoteEdits(window, channels, rate, engine->pitchEditNotes);
    if (rendered.empty()) { copyText(error, errorLen, "pitch edit produced no audio"); return false; }

    std::error_code ec;
    const std::filesystem::path dir = engine->projectPath.empty()
        ? neuracoust::daw::temporaryImportAudioFilesDirectory()
        : neuracoust::daw::projectAudioFilesDirectory(engine->projectPath);
    std::filesystem::create_directories(dir, ec);
    const std::string stem = std::filesystem::path(clip->sourcePath).stem().string();
    const std::filesystem::path outPath = dir / (stem + "_pitch_" + clip->id + ".wav");

    neuracoust::daw::WavAudioData outData;
    outData.channels = channels;
    outData.sampleRate = rate;
    outData.interleavedSamples = std::move(rendered);
    if (!neuracoust::daw::writePcm24WavFileAtomically(outPath, outData, err)) {
        copyText(error, errorLen, "write pitch-edited clip: " + err); return false;
    }
    clip->sourcePath = outPath.string();
    clip->sourceOffsetSeconds = 0.0;   // the rendered window IS the clip
    clip->sourceSampleRate = rate;
    clip->sourceChannels = channels;
    clip->sourceFileUid.clear();
    engine->waveformCache.erase(outPath.string());

    const bool changed = applyClipEdit(engine, true);
    if (changed) engine->recordStep("Apply pitch edit");
    return changed;
}

// Write a clip's played window to `outPath` AS-IS (no processing). Used to feed the stem separator a
// clip-aligned file for polyphonic detection (separate → detect each part).
bool nc_clip_export_raw_window(NCEngine* engine, const char* clipId, const char* outPath,
                               char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || clipId == nullptr || outPath == nullptr) { copyText(error, errorLen, "invalid arguments"); return false; }
    std::vector<float> window; int channels = 0; double rate = 0.0; std::string err;
    if (readClipWindow(engine, clipId, window, channels, rate, err) == nullptr) {
        copyText(error, errorLen, "read clip audio: " + err); return false;
    }
    neuracoust::daw::WavAudioData out;
    out.channels = channels; out.sampleRate = rate; out.interleavedSamples = std::move(window);
    if (!neuracoust::daw::writePcm24WavFileAtomically(std::filesystem::path(outPath), out, err)) {
        copyText(error, errorLen, "write file: " + err); return false;
    }
    return true;
}

bool nc_clip_align_to_reference(NCEngine* engine, const char* dubClipId, const char* refClipId,
                                double strength, int formantPreserve, char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || dubClipId == nullptr || refClipId == nullptr) { copyText(error, errorLen, "invalid arguments"); return false; }
    if (std::string(dubClipId) == refClipId) { copyText(error, errorLen, "리드와 대상이 같은 클립입니다"); return false; }

    std::vector<float> dubWin, refWin; int dubCh = 0, refCh = 0; double dubRate = 0.0, refRate = 0.0; std::string e;
    if (readClipWindow(engine, dubClipId, dubWin, dubCh, dubRate, e) == nullptr) { copyText(error, errorLen, "대상 클립 읽기: " + e); return false; }
    if (readClipWindow(engine, refClipId, refWin, refCh, refRate, e) == nullptr) { copyText(error, errorLen, "리드 클립 읽기: " + e); return false; }

    const auto anchors = neuracoust::daw::alignVocals(refWin, refCh, refRate, dubWin, dubCh, dubRate);
    if (!anchors.ok) { copyText(error, errorLen, "정렬을 계산할 수 없습니다 (클립이 너무 짧거나 무음)"); return false; }

    // Blend each matched anchor between the dub's own timing (strength 0) and the reference's (1). The
    // output duration blends the same way; anchors are handed to the formant-preserving time-map print.
    const double s = std::clamp(strength, 0.0, 1.0);
    const double dubDur = static_cast<double>(dubWin.size()) / std::max(1, dubCh) / dubRate;
    const double refDur = static_cast<double>(refWin.size()) / std::max(1, refCh) / refRate;
    const double outDur = (1.0 - s) * dubDur + s * refDur;
    const double timeRatio = (dubDur > 1e-9) ? outDur / dubDur : 1.0;

    std::vector<double> src, dst;
    src.reserve(anchors.dub.size()); dst.reserve(anchors.dub.size());
    for (size_t k = 0; k < anchors.dub.size(); ++k) {
        const double t = (1.0 - s) * (anchors.dub[k] * dubDur) + s * (anchors.ref[k] * refDur);
        const double d = (outDur > 1e-9) ? t / outDur : anchors.dub[k];
        if (d <= 0.0 || d >= 1.0) continue;
        src.push_back(anchors.dub[k]);
        dst.push_back(d);
    }
    return applyClipTimeTransform(engine, dubClipId, timeRatio, 0.0, src, dst, formantPreserve != 0, error, errorLen);
}

// Repoint a clip at an externally-processed WAV of its played window (same length): copy the file into
// the project's Audio Files folder, swap the clip's source to it (offset 0, duration/fades unchanged),
// record one undo step. The offline-print counterpart to nc_clip_export_raw_window, used by the denoiser.
bool nc_clip_repoint_to_window_wav(NCEngine* engine, const char* clipId, const char* wavPath,
                                   const char* label, char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || clipId == nullptr || wavPath == nullptr) { copyText(error, errorLen, "invalid arguments"); return false; }
    neuracoust::daw::ClipState* clip = nullptr;
    for (auto& c : engine->project.clips) { if (c.id == clipId) { clip = &c; break; } }
    if (clip == nullptr) { copyText(error, errorLen, "clip not found"); return false; }

    // Load the produced WAV so we can both validate it and stamp the clip's source format.
    neuracoust::daw::WavAudioData src;
    std::string err;
    if (!neuracoust::daw::readPcmWavFile(std::string(wavPath), src, err)) {
        copyText(error, errorLen, "read processed audio: " + err); return false;
    }
    if (src.channels < 1 || src.sampleRate <= 0) { copyText(error, errorLen, "unsupported processed audio"); return false; }

    std::error_code ec;
    const std::filesystem::path dir = engine->projectPath.empty()
        ? neuracoust::daw::temporaryImportAudioFilesDirectory()
        : neuracoust::daw::projectAudioFilesDirectory(engine->projectPath);
    std::filesystem::create_directories(dir, ec);
    const std::string stem = std::filesystem::path(clip->sourcePath).stem().string();
    const std::filesystem::path outPath = dir / (stem + "_dn_" + clip->id + ".wav");
    // Re-write into Audio Files (rather than move) so the source can live anywhere (a temp dir).
    if (!neuracoust::daw::writePcm24WavFileAtomically(outPath, src, err)) {
        copyText(error, errorLen, "store processed clip: " + err); return false;
    }

    clip->sourcePath = outPath.string();
    clip->sourceOffsetSeconds = 0.0;   // the processed window IS the clip
    clip->sourceSampleRate = src.sampleRate;
    clip->sourceChannels = src.channels;
    clip->sourceFileUid.clear();
    engine->waveformCache.erase(outPath.string());

    const bool changed = applyClipEdit(engine, true);
    if (changed) engine->recordStep(label != nullptr && *label ? label : "Denoise clip");
    return changed;
}


// --- ARA editing sessions -------------------------------------------------------------------
//
// One clip at a time. The clip's played window is written out ONCE (araSourcePath) and every ARA
// session works from that file, never from the clip's current source: after a commit the clip points
// at the rendered result, so re-opening against it would apply the archive on top of audio that
// already carries it.

namespace {

/// The clip's unedited ARA source window, creating it on first use. Empty on failure.
std::string ensureAraSourceWindow(NCEngine* engine, neuracoust::daw::ClipState* clip,
                                  std::string& error) {
    if (!clip->araSourcePath.empty() && std::filesystem::exists(clip->araSourcePath)) {
        return clip->araSourcePath;
    }
    std::vector<float> window;
    int channels = 0;
    double rate = 0.0;
    std::string err;
    if (readClipWindow(engine, clip->id.c_str(), window, channels, rate, err) == nullptr) {
        error = "클립 오디오를 읽지 못했습니다: " + err;
        return {};
    }
    neuracoust::daw::WavAudioData out;
    out.channels = channels;
    out.sampleRate = rate;
    out.interleavedSamples = std::move(window);

    std::error_code ec;
    const std::filesystem::path dir = engine->projectPath.empty()
        ? neuracoust::daw::temporaryImportAudioFilesDirectory()
        : neuracoust::daw::projectAudioFilesDirectory(engine->projectPath);
    std::filesystem::create_directories(dir, ec);
    const std::string stem = std::filesystem::path(clip->sourcePath).stem().string();
    const std::filesystem::path path = dir / (stem + "_arasrc_" + clip->id + ".wav");
    if (!neuracoust::daw::writePcm24WavFileAtomically(path, out, err)) {
        error = "ARA 소스를 저장하지 못했습니다: " + err;
        return {};
    }
    clip->araSourcePath = path.string();
    return clip->araSourcePath;
}

neuracoust::daw::ClipState* findClip(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return nullptr;
    for (auto& c : engine->project.clips) {
        if (c.id == clipId) return &c;
    }
    return nullptr;
}

} // namespace


// The ARA-capable plug-ins, straight from the full catalog — deliberately not through the browser's
// filter, which the plug-in browser owns and would be clobbered by a query from a clip menu.
int nc_ara_plugin_count(NCEngine* engine) {
    if (engine == nullptr) return 0;
    int count = 0;
    for (const auto& plugin : engine->plugins) {
        if (plugin.araCapable) ++count;
    }
    return count;
}

namespace {
const neuracoust::daw::PluginCandidate* araPluginAt(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0) return nullptr;
    int seen = 0;
    for (const auto& plugin : engine->plugins) {
        if (!plugin.araCapable) continue;
        if (seen == index) return &plugin;
        ++seen;
    }
    return nullptr;
}
} // namespace

void nc_ara_plugin_name(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* plugin = araPluginAt(engine, index);
    copyText(out, outLen, plugin != nullptr ? plugin->name : std::string{});
}

void nc_ara_plugin_path(NCEngine* engine, int index, char* out, size_t outLen) {
    const auto* plugin = araPluginAt(engine, index);
    copyText(out, outLen, plugin != nullptr ? plugin->path : std::string{});
}

bool nc_ara_open(NCEngine* engine, const char* clipId, const char* pluginName, const char* pluginPath,
                 char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || clipId == nullptr || pluginName == nullptr || pluginPath == nullptr) {
        copyText(error, errorLen, "invalid arguments");
        return false;
    }
    if (!neuracoust::daw::araHostingCompiledIn()) {
        copyText(error, errorLen, "이 빌드에는 ARA 지원이 없습니다.");
        return false;
    }
    if (engine->araSession != nullptr) {
        copyText(error, errorLen, "이미 다른 클립의 ARA 편집이 열려 있습니다.");
        return false;
    }
    auto* clip = findClip(engine, clipId);
    if (clip == nullptr) {
        copyText(error, errorLen, "클립을 찾을 수 없습니다.");
        return false;
    }

    std::string err;
    const std::string sourceWav = ensureAraSourceWindow(engine, clip, err);
    if (sourceWav.empty()) {
        copyText(error, errorLen, err);
        return false;
    }
    // A previous session's plug-in and this one must agree, or the archive is meaningless.
    if (!clip->araArchiveBase64.empty() && !clip->araPluginName.empty() &&
            clip->araPluginName != pluginName) {
        copyText(error, errorLen, "이 클립은 " + clip->araPluginName + "(으)로 편집되었습니다.");
        return false;
    }

    const auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(pluginName, pluginPath);
    auto session = std::make_unique<neuracoust::daw::AraDocumentController>();
    std::string message;
    const std::string documentName = engine->project.name.empty() ? "Neuracoust" : engine->project.name;
    if (!session->create(descriptor, documentName, message)) {
        copyText(error, errorLen, message);
        return false;
    }
    const std::string sourceName = clip->regionName.empty()
        ? std::filesystem::path(clip->sourcePath).stem().string()
        : clip->regionName;
    if (!session->addAudioFile(sourceWav, clip->id, sourceName, message)) {
        copyText(error, errorLen, message);
        return false;
    }
    // Restore BEFORE binding: the archive describes the document graph, not the instance.
    if (!clip->araArchiveBase64.empty() && !session->restoreArchive(clip->araArchiveBase64, message)) {
        copyText(error, errorLen, "이전 편집을 복원하지 못했습니다: " + message);
        return false;
    }
    if (!session->bindPlugInInstance(message)) {
        copyText(error, errorLen, message);
        return false;
    }

    clip->araPluginName = pluginName;
    clip->araPluginPath = pluginPath;
    engine->araSession = std::move(session);
    engine->araSessionClipId = clip->id;
    return true;
}

bool nc_ara_is_open(NCEngine* engine) {
    return engine != nullptr && engine->araSession != nullptr;
}

void nc_ara_open_clip_id(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->araSessionClipId : std::string{});
}

bool nc_ara_attach_editor(NCEngine* engine, void* nsView, int* widthOut, int* heightOut,
                          char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (widthOut != nullptr) *widthOut = 0;
    if (heightOut != nullptr) *heightOut = 0;
    if (engine == nullptr || engine->araSession == nullptr) {
        copyText(error, errorLen, "열린 ARA 편집이 없습니다.");
        return false;
    }
    int width = 0;
    int height = 0;
    std::string message;
    if (!engine->araSession->createEditorView(nsView, width, height, message)) {
        copyText(error, errorLen, message);
        return false;
    }
    if (widthOut != nullptr) *widthOut = width;
    if (heightOut != nullptr) *heightOut = height;
    return true;
}

void nc_ara_detach_editor(NCEngine* engine) {
    if (engine != nullptr && engine->araSession != nullptr) {
        engine->araSession->destroyEditorView();
    }
}

bool nc_ara_commit(NCEngine* engine, char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || engine->araSession == nullptr) {
        copyText(error, errorLen, "열린 ARA 편집이 없습니다.");
        return false;
    }
    auto* clip = findClip(engine, engine->araSessionClipId.c_str());
    if (clip == nullptr) {
        copyText(error, errorLen, "클립이 사라졌습니다.");
        return false;
    }

    // The editor view holds the plug-in's UI state; take it down before archiving so what is stored
    // is what the user sees, and so the plug-in is not drawing while its document is read.
    engine->araSession->destroyEditorView();

    std::string message;
    std::string archive;
    if (!engine->araSession->storeArchive(archive, message)) {
        copyText(error, errorLen, "편집을 저장하지 못했습니다: " + message);
        return false;
    }

    std::error_code ec;
    const std::filesystem::path dir = engine->projectPath.empty()
        ? neuracoust::daw::temporaryImportAudioFilesDirectory()
        : neuracoust::daw::projectAudioFilesDirectory(engine->projectPath);
    std::filesystem::create_directories(dir, ec);
    const std::string stem = std::filesystem::path(clip->araSourcePath).stem().string();
    const std::filesystem::path printPath = dir / (stem + "_print.wav");
    if (!engine->araSession->renderToWavFile(printPath.string(), message)) {
        copyText(error, errorLen, "렌더링하지 못했습니다: " + message);
        return false;
    }

    neuracoust::daw::WavAudioData printed;
    std::string err;
    if (!neuracoust::daw::readPcmWavFile(printPath.string(), printed, err) ||
            printed.channels < 1 || printed.sampleRate <= 0) {
        copyText(error, errorLen, "렌더링 결과를 읽지 못했습니다: " + err);
        return false;
    }

    clip->araArchiveBase64 = archive;
    clip->sourcePath = printPath.string();
    clip->sourceOffsetSeconds = 0.0;   // the printed window IS the clip
    clip->sourceSampleRate = printed.sampleRate;
    clip->sourceChannels = printed.channels;
    clip->sourceFileUid.clear();
    engine->waveformCache.erase(printPath.string());

    const bool changed = applyClipEdit(engine, true);
    if (changed) {
        engine->recordStep("ARA 편집 적용");
    } else {
        copyText(error, errorLen, "프로젝트를 갱신하지 못했습니다.");
    }
    return changed;
}

void nc_ara_close(NCEngine* engine) {
    if (engine == nullptr || engine->araSession == nullptr) return;
    engine->araSession->destroy();
    engine->araSession.reset();
    engine->araSessionClipId.clear();
}

bool nc_clip_has_ara_edits(NCEngine* engine, const char* clipId) {
    const auto* clip = findClip(engine, clipId);
    return clip != nullptr && !clip->araArchiveBase64.empty();
}

bool nc_clip_clear_ara_edits(NCEngine* engine, const char* clipId, char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    auto* clip = findClip(engine, clipId);
    if (clip == nullptr) {
        copyText(error, errorLen, "클립을 찾을 수 없습니다.");
        return false;
    }
    if (engine->araSessionClipId == clip->id) {
        copyText(error, errorLen, "먼저 ARA 편집 창을 닫으세요.");
        return false;
    }
    if (clip->araSourcePath.empty() || !std::filesystem::exists(clip->araSourcePath)) {
        copyText(error, errorLen, "되돌릴 원본이 없습니다.");
        return false;
    }
    // Back to the window the first session started from — that file was never edited.
    clip->sourcePath = clip->araSourcePath;
    clip->sourceOffsetSeconds = 0.0;
    clip->araArchiveBase64.clear();
    clip->sourceFileUid.clear();
    const bool changed = applyClipEdit(engine, true);
    if (changed) engine->recordStep("ARA 편집 제거");
    return changed;
}

// Polyphonic detection is built by separating the clip (Demucs) then detecting each part. These let
// Swift orchestrate that: reset the note cache, then append the notes found in each stem file. Notes
// accumulate and are sorted by time, so the editor shows every part's pitch at once (true polyphony).
void nc_detect_notes_reset(NCEngine* engine) {
    if (engine != nullptr) engine->pitchEditNotes.clear();
}
int nc_detect_notes_add_from_file(NCEngine* engine, const char* wavPath, int mode) {
    if (engine == nullptr || wavPath == nullptr) return 0;
    neuracoust::daw::WavAudioData src;
    std::string err;
    if (!neuracoust::daw::readPcmWavFile(std::string(wavPath), src, err)) return 0;
    if (src.channels < 1 || src.sampleRate <= 0.0) return 0;
    const auto detectionMode = mode == 2 ? neuracoust::daw::DetectionMode::Percussive
                             : mode == 1 ? neuracoust::daw::DetectionMode::Polyphonic
                                         : neuracoust::daw::DetectionMode::Melodic;
    const auto found = neuracoust::daw::detectNotesForMode(src.interleavedSamples, src.channels, src.sampleRate, detectionMode);
    engine->pitchEditNotes.insert(engine->pitchEditNotes.end(), found.begin(), found.end());
    std::sort(engine->pitchEditNotes.begin(), engine->pitchEditNotes.end(),
              [](const neuracoust::daw::DetectedNote& a, const neuracoust::daw::DetectedNote& b) { return a.startSeconds < b.startSeconds; });
    return static_cast<int>(found.size());
}
void nc_detect_notes_add_note(NCEngine* engine, double startSeconds, double durationSeconds,
                              double midiPitch, double confidence) {
    if (engine == nullptr || !std::isfinite(startSeconds) || !std::isfinite(durationSeconds) ||
        !std::isfinite(midiPitch)) return;
    neuracoust::daw::DetectedNote note;
    note.startSeconds = std::max(0.0, startSeconds);
    note.durationSeconds = std::max(0.02, durationSeconds);
    note.detectedMidi = std::clamp(midiPitch, 0.0, 127.0);
    note.confidence = std::clamp(confidence, 0.0, 1.0);
    engine->pitchEditNotes.push_back(note);
}
// Bind the accumulated notes to a clip so apply/export know their target.
void nc_detect_notes_bind_clip(NCEngine* engine, const char* clipId) {
    if (engine != nullptr && clipId != nullptr) {
        std::sort(engine->pitchEditNotes.begin(), engine->pitchEditNotes.end(),
                  [](const auto& a, const auto& b) {
                      return a.startSeconds == b.startSeconds
                          ? a.detectedMidi < b.detectedMidi
                          : a.startSeconds < b.startSeconds;
                  });
        engine->pitchEditClipId = clipId;
    }
}

// Segment an externally-produced pitch track (e.g. from the CREPE neural detector helper) into notes
// and cache them. times/hzs are per-frame; hz 0 = unvoiced. Runs the same Viterbi cleanup + segmenter
// as the built-in YIN path, so downstream editing/apply is identical.
int nc_segment_pitch_track(NCEngine* engine, const double* times, const double* hzs,
                           const double* confs, int count) {
    if (engine == nullptr || times == nullptr || hzs == nullptr || count <= 0) return 0;
    std::vector<neuracoust::daw::PitchFrame> track;
    track.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        neuracoust::daw::PitchFrame f;
        f.timeSeconds = times[i];
        f.frequencyHz = hzs[i];
        f.confidence = confs != nullptr ? confs[i] : 0.9;
        track.push_back(f);
    }
    track = neuracoust::daw::smoothPitchTrack(track);
    engine->pitchEditNotes = neuracoust::daw::segmentNotes(track);
    return static_cast<int>(engine->pitchEditNotes.size());
}

// --- Export the processed result to a standalone WAV (the clip/project is NOT modified) -----------
// Renders the current Melodyne per-note edits to `outPath`. The user picks the path (a save panel);
// this just writes the file so they can keep or reuse it outside the project.
bool nc_clip_export_note_edits(NCEngine* engine, const char* clipId, const char* outPath,
                               char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || clipId == nullptr || outPath == nullptr) { copyText(error, errorLen, "invalid arguments"); return false; }
    if (engine->pitchEditClipId != clipId || engine->pitchEditNotes.empty()) {
        copyText(error, errorLen, "no detected notes for this clip"); return false;
    }
    std::vector<float> window; int channels = 0; double rate = 0.0; std::string err;
    if (readClipWindow(engine, clipId, window, channels, rate, err) == nullptr) {
        copyText(error, errorLen, "read clip audio: " + err); return false;
    }
    std::vector<float> rendered = neuracoust::daw::renderNoteEdits(window, channels, rate, engine->pitchEditNotes);
    if (rendered.empty()) { copyText(error, errorLen, "pitch edit produced no audio"); return false; }
    // DIAGNOSTIC: did the render actually alter the audio of the first pitch-edited note?
    // Sum of absolute differences over that note's region — near-zero means the render was a
    // no-op there (render bug); large means the shift happened (look at audition/perception).
    for (size_t ni = 0; ni < engine->pitchEditNotes.size(); ++ni) {
        const auto& n = engine->pitchEditNotes[ni];
        if (std::abs(n.pitchOffsetSemitones) < 0.01) continue;
        const int64_t frames = static_cast<int64_t>(window.size()) / std::max(1, channels);
        const int64_t s = std::max<int64_t>(0, std::llround(n.startSeconds * rate));
        const int64_t e = std::min<int64_t>(frames, s + std::llround(n.durationSeconds * rate));
        double sad = 0.0; int64_t cnt = 0;
        for (int64_t i = s * channels; i < e * channels && i < static_cast<int64_t>(rendered.size()); ++i) {
            sad += std::abs(static_cast<double>(rendered[static_cast<size_t>(i)]) - window[static_cast<size_t>(i)]);
            ++cnt;
        }
        fprintf(stderr, "[pitch-render] note %zu offset=%.2fst region=[%.2f,%.2f]s meanAbsDiff=%.6f frames=%lld\n",
                ni, n.pitchOffsetSemitones, n.startSeconds, n.startSeconds + n.durationSeconds,
                cnt > 0 ? sad / cnt : 0.0, static_cast<long long>(cnt));
        break;
    }
    neuracoust::daw::WavAudioData out;
    out.channels = channels; out.sampleRate = rate; out.interleavedSamples = std::move(rendered);
    if (!neuracoust::daw::writePcm24WavFileAtomically(std::filesystem::path(outPath), out, err)) {
        copyText(error, errorLen, "write file: " + err); return false;
    }
    return true;
}

// Renders the Serato-mode anchor time-remap (+ global ratio/pitch) to `outPath`. Clip untouched.
bool nc_clip_export_time_map(NCEngine* engine, const char* clipId, double timeRatio, double semitones,
                             const double* sourceAnchors, const double* destAnchors, int anchorCount,
                             const char* outPath, char* error, size_t errorLen) {
    copyText(error, errorLen, std::string{});
    if (engine == nullptr || clipId == nullptr || outPath == nullptr) { copyText(error, errorLen, "invalid arguments"); return false; }
    std::vector<float> window; int channels = 0; double rate = 0.0; std::string err;
    if (readClipWindow(engine, clipId, window, channels, rate, err) == nullptr) {
        copyText(error, errorLen, "read clip audio: " + err); return false;
    }
    std::vector<double> src, dst;
    if (sourceAnchors != nullptr && destAnchors != nullptr && anchorCount > 0) {
        src.assign(sourceAnchors, sourceAnchors + anchorCount);
        dst.assign(destAnchors, destAnchors + anchorCount);
    }
    neuracoust::daw::TimePitchParams p;
    p.timeRatio = std::clamp(timeRatio, 0.125, 8.0);
    p.semitones = std::clamp(semitones, -24.0, 24.0);
    std::vector<float> rendered = neuracoust::daw::processTimeMapInterleaved(window, channels, p, src, dst);
    if (rendered.empty()) { copyText(error, errorLen, "time map produced no audio"); return false; }
    neuracoust::daw::WavAudioData out;
    out.channels = channels; out.sampleRate = rate; out.interleavedSamples = std::move(rendered);
    if (!neuracoust::daw::writePcm24WavFileAtomically(std::filesystem::path(outPath), out, err)) {
        copyText(error, errorLen, "write file: " + err); return false;
    }
    return true;
}

// LIVE-DRAG roll: move the shared boundary of two abutting clips together — trim the left clip's end
// and the right clip's start to ONE clamped boundary so they never gap or overlap. In-place render
// patch, no rebuild (seamless during playback); commit once on drop via nc_project_reconcile.
bool nc_clip_roll_boundary(NCEngine* engine, const char* leftId, const char* rightId, double boundarySeconds) {
    if (engine == nullptr || leftId == nullptr || rightId == nullptr) return false;
    const neuracoust::daw::ClipState* left = nullptr;
    const neuracoust::daw::ClipState* right = nullptr;
    for (const auto& c : engine->project.clips) {
        if (c.id == leftId) left = &c;
        else if (c.id == rightId) right = &c;
    }
    if (left == nullptr || right == nullptr) return false;

    // Valid range for the boundary so neither clip inverts or runs past its source file.
    double lo = std::max(left->startSeconds, right->startSeconds - right->sourceOffsetSeconds);
    double hi = right->startSeconds + right->durationSeconds;  // right can't lose all length
    const auto lcached = engine->waveformCache.find(left->sourcePath);
    if (lcached != engine->waveformCache.end() && lcached->second.durationSeconds > 0.0) {
        hi = std::min(hi, left->startSeconds + (lcached->second.durationSeconds - left->sourceOffsetSeconds));
    }
    if (hi < lo) return false;
    const double b = std::max(lo, std::min(hi, boundarySeconds));

    bool changed = false;
    if (neuracoust::daw::trimClipEnd(engine->project, leftId, b)) changed = true;
    if (neuracoust::daw::trimClipStart(engine->project, rightId, b)) changed = true;
    if (!changed) return false;
    for (const auto& c : engine->project.clips) {
        if (c.id == leftId || c.id == rightId) {
            engine->engine.updateClipBounds(c.id, c.startSeconds, c.durationSeconds, c.sourceOffsetSeconds);
        }
    }
    return true;
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

int nc_clip_glue_selection(NCEngine* engine, const char* const* clipIds, int count) {
    if (engine == nullptr || clipIds == nullptr || count <= 0) return 0;
    std::vector<std::string> ids;
    ids.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (clipIds[i] != nullptr) ids.emplace_back(clipIds[i]);
    }
    std::vector<std::string> glued;
    if (!neuracoust::daw::glueSelectedClips(engine->project, ids, glued) || glued.empty()) {
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

namespace { std::vector<std::string> resolveClipIds(NCEngine* engine, const char* const* clipIds, int count); }  // defined below

// Shuffle-delete the given clips, rippling only each clip's own track (Pro Tools default), not all.
int nc_clip_shuffle_delete_many(NCEngine* engine, const char* const* clipIds, int count) {
    if (engine == nullptr) return 0;
    const auto ids = resolveClipIds(engine, clipIds, count);
    if (!neuracoust::daw::shuffleDeleteClips(engine->project, ids)) {
        return 0;
    }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep(ids.size() <= 1 ? "Shuffle delete" : "Shuffle delete " + std::to_string(ids.size()) + " clips");
    return static_cast<int>(ids.size());
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

// Non-destructive processing state, for the timeline to reflect it in the waveform (dimmed when
// muted, mirrored when reversed, flipped when polarity-inverted).
bool nc_clip_muted(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr && clip->muted;
}
bool nc_clip_reversed(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr && clip->reversed;
}
bool nc_clip_polarity(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip != nullptr && clip->polarityInverted;
}

bool nc_clip_set_gain_db(NCEngine* engine, const char* clipId, float gainDb) {
    if (engine == nullptr || clipId == nullptr) return false;
    // Committing clip gain reconciles the render plan; declick so the baked-gain swap doesn't click
    // (the drag itself uses the live preview path, which never reconciles).
    engine->engine.beginGraphChangeDeclick();
    const bool changed = applyClipEdit(engine, neuracoust::daw::setClipGainDb(engine->project, clipId, gainDb));
    engine->engine.endGraphChangeDeclick();
    return changed;
}

// Non-destructive clip processing (Logic/Cubase style): the renderer honours these flags directly,
// so nothing writes a new file and every one undoes cleanly. Each is a discrete step.
bool nc_clip_toggle_reversed(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return false;
    engine->engine.beginGraphChangeDeclick();
    const bool changed = applyClipEdit(engine, neuracoust::daw::toggleClipReversed(engine->project, clipId));
    engine->engine.endGraphChangeDeclick();
    if (changed) engine->recordStep("Reverse clip");
    return changed;
}

bool nc_clip_toggle_muted(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return false;
    engine->engine.beginGraphChangeDeclick();
    const bool changed = applyClipEdit(engine, neuracoust::daw::toggleClipMuted(engine->project, clipId));
    engine->engine.endGraphChangeDeclick();
    if (changed) engine->recordStep("Mute clip");
    return changed;
}

bool nc_clip_toggle_polarity(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return false;
    engine->engine.beginGraphChangeDeclick();
    const bool changed = applyClipEdit(engine, neuracoust::daw::toggleClipPolarityInverted(engine->project, clipId));
    engine->engine.endGraphChangeDeclick();
    if (changed) engine->recordStep("Invert clip polarity");
    return changed;
}

// Normalize: scan the clip's source WAV for its peak and bake a clip gain that brings it to
// targetPeakDb (a hair under 0 dBFS). Non-destructive — it only sets the clip's gain.
bool nc_clip_normalize(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return false;
    std::string message;
    engine->engine.beginGraphChangeDeclick();
    const bool ok = neuracoust::daw::normalizeClipGainToPeak(engine->project, clipId, -0.3f, message);
    const bool changed = applyClipEdit(engine, ok);
    engine->engine.endGraphChangeDeclick();
    if (changed) engine->recordStep("Normalize clip");
    return changed;
}

// Continuous: sets the field only, no graph rebuild — so dragging clip gain stays
// smooth. The heavy reconcile happens once on commit via nc_clip_set_gain_db.
bool nc_clip_set_gain_db_preview(NCEngine* engine, const char* clipId, float gainDb) {
    if (engine == nullptr || clipId == nullptr) return false;
    // Two updates, no reconcile (so the drag stays smooth and can't click): the project model drives
    // the waveform redraw, and updateClipGain patches the SAME clip in the live render plan in place,
    // so the new gain is heard in real time while dragging. Commit later reconciles for undo.
    const bool changed = neuracoust::daw::setClipGainDb(engine->project, clipId, gainDb);
    engine->engine.updateClipGain(clipId, gainDb);
    return changed;
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

// Consolidate (Pro Tools ⌥⇧3): render the selected clips on each of their tracks — clip gain, fades
// and crossfades baked, at unity track/master with no inserts — into ONE new WAV per track, and
// replace them with a single clip. Works for overlapping/crossfaded clips that glue can't join.
bool nc_clip_consolidate(NCEngine* engine, const char* const* clipIds, int count,
                         char* outError, size_t errLen) {
    if (engine == nullptr) { copyText(outError, errLen, "no engine"); return false; }
    const auto ids = resolveClipIds(engine, clipIds, count);
    if (ids.empty()) { copyText(outError, errLen, "선택된 클립이 없습니다."); return false; }

    auto findClipLocal = [&](const std::string& id) -> const neuracoust::daw::ClipState* {
        for (const auto& c : engine->project.clips) { if (c.id == id) return &c; }
        return nullptr;
    };
    auto clipEndLocal = [](const neuracoust::daw::ClipState& c) { return c.startSeconds + c.durationSeconds; };

    // Group the selected clips by track — each track's group becomes one consolidated file.
    std::map<std::string, std::vector<std::string>> byTrack;
    for (const auto& id : ids) {
        const auto* c = findClipLocal(id);
        if (c != nullptr && !c->locked) byTrack[c->trackName].push_back(id);
    }
    if (byTrack.empty()) { copyText(outError, errLen, "합칠 클립이 없습니다."); return false; }

    auto makeWavPath = [&]() -> std::string {
        std::string err;
        std::string p = neuracoust::daw::nextProjectRecordingPath(engine->projectPath, err);
        if (!p.empty()) return p;
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path() / "Neuracoust Recordings";
        std::filesystem::create_directories(dir, ec);
        for (int i = 0; i < 100000; ++i) {
            const auto cand = dir / ("Neuracoust Consolidated " + std::to_string(i) + ".wav");
            if (!std::filesystem::exists(cand)) return cand.string();
        }
        return {};
    };

    int consolidated = 0;
    for (auto& entry : byTrack) {
        const std::string& trackName = entry.first;
        const auto& trackClipIds = entry.second;
        double minStart = std::numeric_limits<double>::max();
        double maxEnd = 0.0;
        for (const auto& id : trackClipIds) {
            const auto* c = findClipLocal(id);
            if (c == nullptr) continue;
            minStart = std::min(minStart, c->startSeconds);
            maxEnd = std::max(maxEnd, clipEndLocal(*c));
        }
        if (!(maxEnd > minStart)) continue;

        // Temp project: only these clips, target track + master at unity with no processing, so the
        // print is exactly the clip-level audio (gain + fades + crossfades) — Pro Tools clip level.
        neuracoust::daw::ProjectDocument temp = engine->project;
        std::set<std::string> keep(trackClipIds.begin(), trackClipIds.end());
        temp.clips.erase(std::remove_if(temp.clips.begin(), temp.clips.end(),
            [&](const neuracoust::daw::ClipState& c) { return keep.find(c.id) == keep.end(); }),
            temp.clips.end());
        for (auto& t : temp.tracks) {
            if (t.name == trackName) {
                t.volumeDb = 0.0f; t.pan = 0.0f; t.inserts.clear(); t.sends.clear();
                t.outputBus = "Master"; t.muted = false; t.solo = false;
                t.recordArmed = false; t.inputMonitoring = false; t.volumeAutomation.clear();
            } else if (t.name == "Master") {
                t.volumeDb = 0.0f; t.pan = 0.0f; t.inserts.clear(); t.volumeAutomation.clear();
            }
        }
        temp.masterInserts.clear();
        temp.monitorModules.clear();
        temp.autoFadeOutSeconds = 0.0;
        temp.editSelectionEnabled = true;
        temp.editSelectionStartSeconds = minStart;
        temp.editSelectionEndSeconds = maxEnd;
        neuracoust::daw::normalizeProjectRouting(temp);
        neuracoust::daw::rebuildProjectEditModelFromClips(temp);

        const std::string wav = makeWavPath();
        if (wav.empty()) { copyText(outError, errLen, "통합 파일 경로를 만들 수 없습니다."); continue; }
        neuracoust::daw::BounceOptions opt;
        opt.rangeMode = neuracoust::daw::BounceRangeMode::EditSelection;
        const auto result = neuracoust::daw::bounceProjectToWav(temp, wav, opt);
        if (!result.ok) {
            copyText(outError, errLen, result.message.empty() ? "통합 렌더 실패" : result.message);
            continue;
        }

        for (const auto& id : trackClipIds) neuracoust::daw::deleteClip(engine->project, id);
        const std::string newId = neuracoust::daw::appendAudioClipAt(engine->project, trackName, wav,
                                                                     minStart, maxEnd - minStart);
        if (!newId.empty()) ++consolidated;
    }

    if (consolidated == 0) { copyText(outError, errLen, "통합할 수 없습니다."); return false; }
    neuracoust::daw::rebuildProjectEditModelFromClips(engine->project);
    engine->reconcileProject();
    engine->recordStep("Consolidate");
    return true;
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

double nc_clip_fade_in_curvature(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip == nullptr ? 0.0 : clip->fadeInCurvature;
}
double nc_clip_fade_out_curvature(NCEngine* engine, int index) {
    const auto* clip = clipAt(engine, index);
    return clip == nullptr ? 0.0 : clip->fadeOutCurvature;
}
bool nc_clip_set_fade_curvature(NCEngine* engine, const char* clipId,
                                double inCurvature, double outCurvature) {
    if (engine == nullptr || clipId == nullptr) return false;
    return applyClipEdit(engine, neuracoust::daw::setClipFadeCurvature(
        engine->project, clipId, inCurvature, outCurvature));
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

double nc_clip_original_start_seconds(NCEngine* engine, const char* clipId) {
    if (engine == nullptr || clipId == nullptr) return -1.0;
    const auto* clip = findClipById(engine, clipId);
    return clip != nullptr ? clip->originalStartSeconds : -1.0;
}

int nc_clip_spot_to_original_many(NCEngine* engine, const char* const* clipIds, int count) {
    const auto clips = resolveClips(engine, clipIds, count);
    if (clips.empty()) {
        return 0;
    }
    // Each clip goes to ITS OWN original position — not a common delta — so a
    // scattered selection re-forms the originally imported layout.
    std::vector<std::pair<std::string, double>> targets;
    targets.reserve(clips.size());
    for (const auto* clip : clips) {
        if (clip->originalStartSeconds >= 0.0 &&
            std::abs(clip->originalStartSeconds - clip->startSeconds) > 1.0e-9) {
            targets.emplace_back(clip->id, clip->originalStartSeconds);
        }
    }
    int moved = 0;
    for (const auto& [id, start] : targets) {
        if (neuracoust::daw::moveClip(engine->project, id, start)) {
            ++moved;
        }
    }
    if (!applyClipEdit(engine, moved > 0)) {
        return 0;
    }
    // A discrete action, unlike a drag: record its own single undo step.
    engine->recordStep("Spot to original");
    return moved;
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

bool isGenericMixerAutomationParameter(const char* parameterId) {
    return parameterId != nullptr &&
        (std::strcmp(parameterId, "track.mute") == 0 ||
         std::strcmp(parameterId, "track.volume.trim") == 0 ||
         std::strncmp(parameterId, "send.", 5) == 0 ||
         std::strncmp(parameterId, "instrument.", 11) == 0);
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

bool nc_midi_import_file_to_track(NCEngine* engine, const char* midiPath, int trackIndex,
                                  double startSeconds, char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (engine == nullptr || midiPath == nullptr) {
        return false;
    }
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) {
        return false;
    }
    neuracoust::daw::MidiImportResult imported =
        neuracoust::daw::readProjectMidiFile(std::filesystem::path(midiPath));
    if (!imported.ok) {
        return false;
    }
    // A MIDI file's note times are tempo-independent beats; flatten every imported region's notes into
    // one region at the drop point, preserving relative beat timing. The region then plays at the host
    // project's tempo (standard MIDI behaviour), so a 113-BPM drum loop follows the session tempo.
    const double importBps =
        (imported.project.tempoBpm > 0 ? static_cast<double>(imported.project.tempoBpm) : 120.0) / 60.0;
    const double projBps =
        (engine->project.tempoBpm > 0 ? static_cast<double>(engine->project.tempoBpm) : 120.0) / 60.0;
    struct FlatNote { int pitch; double startBeats; double durBeats; int velocity; };
    std::vector<FlatNote> flat;
    double maxBeatEnd = 0.0;
    for (const auto& region : imported.project.midiRegions) {
        const double regionStartBeats = region.startSeconds * importBps;
        for (const auto& note : region.notes) {
            const double sb = regionStartBeats + note.startBeats;
            const double db = std::max(0.03125, note.durationBeats);
            flat.push_back({note.pitch, sb, db, note.velocity});
            maxBeatEnd = std::max(maxBeatEnd, sb + db);
        }
    }
    if (flat.empty()) {
        return false;
    }
    const double durationSeconds = std::max(0.25, maxBeatEnd / std::max(0.01, projBps));
    const std::string regionId = neuracoust::daw::addMidiRegion(engine->project, track->name,
                                                                std::max(0.0, startSeconds), durationSeconds);
    if (regionId.empty()) {
        return false;
    }
    for (const auto& n : flat) {
        neuracoust::daw::addMidiNote(engine->project, regionId, n.pitch, n.startBeats, n.durBeats, n.velocity);
    }
    applyMidiEdit(engine, true, "Import MIDI file");
    copyText(out, outLen, regionId);
    return true;
}

int nc_midi_import_file_auto(NCEngine* engine, const char* midiPath, int preferredTrackIndex,
                             double startSeconds, char* error, size_t errorLen) {
    if (error != nullptr && errorLen > 0) error[0] = '\0';
    if (engine == nullptr || midiPath == nullptr) return 0;
    neuracoust::daw::MidiImportResult imported =
        neuracoust::daw::readProjectMidiFile(std::filesystem::path(midiPath));
    if (!imported.ok) { copyText(error, errorLen, "read failed"); return 0; }

    const double importBps =
        (imported.project.tempoBpm > 0 ? static_cast<double>(imported.project.tempoBpm) : 120.0) / 60.0;
    const double projBps =
        (engine->project.tempoBpm > 0 ? static_cast<double>(engine->project.tempoBpm) : 120.0) / 60.0;

    // Source tracks that actually carry notes (a full song is many; a loop is one).
    std::vector<const neuracoust::daw::MidiRegionState*> regions;
    for (const auto& r : imported.project.midiRegions) if (!r.notes.empty()) regions.push_back(&r);
    if (regions.empty()) { copyText(error, errorLen, "no notes"); return 0; }

    // Flatten a set of source regions into ONE region on `trackName` at the drop point.
    auto addRegion = [&](const std::string& trackName,
                         const std::vector<const neuracoust::daw::MidiRegionState*>& src) -> bool {
        struct FN { int pitch; double startBeats; double durBeats; int velocity; };
        std::vector<FN> flat; double maxEnd = 0.0;
        for (const auto* R : src) {
            const double base = R->startSeconds * importBps;
            for (const auto& n : R->notes) {
                const double s = base + n.startBeats, d = std::max(0.03125, n.durationBeats);
                flat.push_back({n.pitch, s, d, n.velocity}); maxEnd = std::max(maxEnd, s + d);
            }
        }
        if (flat.empty()) return false;
        const double durSec = std::max(0.25, maxEnd / std::max(0.01, projBps));
        const std::string rid = neuracoust::daw::addMidiRegion(engine->project, trackName,
                                                               std::max(0.0, startSeconds), durSec);
        if (rid.empty()) return false;
        for (const auto& n : flat) neuracoust::daw::addMidiNote(engine->project, rid, n.pitch, n.startBeats, n.durBeats, n.velocity);
        return true;
    };

    // Single source track → one region (reuse the preferred lane if given, else a new instrument track).
    if (regions.size() <= 1) {
        auto* track = trackAt(engine, preferredTrackIndex);
        std::string trackName;
        if (track != nullptr) trackName = track->name;
        else trackName = neuracoust::daw::addInstrumentTrack(engine->project);
        if (!addRegion(trackName, regions)) { copyText(error, errorLen, "add failed"); return 0; }
        applyMidiEdit(engine, true, "Import MIDI file");
        return 1;
    }

    // Multi-track song → one new instrument track PER source track, named from the source track name so
    // the parts land labelled (Piano / Bass / Drums / …). All under one undo step.
    auto uniqueName = [&](std::string wanted) {
        if (wanted.empty()) return std::string();
        std::string base = wanted, candidate = wanted; int suffix = 2;
        auto taken = [&](const std::string& n) {
            for (const auto& t : engine->project.tracks) if (t.name == n) return true; return false;
        };
        while (taken(candidate)) candidate = base + " " + std::to_string(suffix++);
        return candidate;
    };
    int made = 0;
    for (const auto* R : regions) {
        const std::string created = neuracoust::daw::addInstrumentTrack(engine->project);
        if (created.empty()) continue;
        std::string finalName = created;
        const std::string wanted = uniqueName(R->trackName);
        if (!wanted.empty() && wanted != created) {
            for (auto& t : engine->project.tracks) if (t.name == created) { t.name = wanted; finalName = wanted; break; }
        }
        if (addRegion(finalName, { R })) ++made;
    }
    if (made == 0) { copyText(error, errorLen, "add failed"); return 0; }
    applyMidiEdit(engine, true, "Import MIDI file (multi-track)");
    return made;
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

bool nc_midi_regions_merge(NCEngine* engine, const char* const* regionIds, int count,
                           char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (engine == nullptr || regionIds == nullptr || count < 2) {
        return false;
    }
    std::vector<std::string> ids;
    ids.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (regionIds[index] != nullptr) {
            ids.emplace_back(regionIds[index]);
        }
    }
    const std::string mergedId = neuracoust::daw::mergeMidiRegions(engine->project, ids);
    if (mergedId.empty()) {
        return false;
    }
    applyMidiEdit(engine, true, "Merge MIDI regions");
    copyText(out, outLen, mergedId);
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

bool nc_midi_notes_merge(NCEngine* engine, const char* regionId,
                         const char* const* noteIds, int count) {
    if (engine == nullptr || regionId == nullptr || noteIds == nullptr || count < 2) {
        return false;
    }
    std::vector<std::string> ids;
    ids.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (noteIds[index] != nullptr) {
            ids.emplace_back(noteIds[index]);
        }
    }
    std::vector<std::string> surviving;
    return applyMidiEdit(engine,
                         neuracoust::daw::mergeMidiNotes(engine->project, regionId, ids, surviving),
                         "Glue notes");
}

namespace {
/// Shared marshalling for the note-list editor functions: a C string array to std::vector.
std::vector<std::string> noteIdVector(const char* const* noteIds, int count) {
    std::vector<std::string> ids;
    if (noteIds == nullptr || count <= 0) return ids;
    ids.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (noteIds[index] != nullptr) ids.emplace_back(noteIds[index]);
    }
    return ids;
}
} // namespace

bool nc_midi_notes_legato(NCEngine* engine, const char* regionId,
                          const char* const* noteIds, int count, double gapBeats) {
    if (engine == nullptr || regionId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::applyMidiLegato(engine->project, regionId,
                                                          noteIdVector(noteIds, count), gapBeats),
                         "Legato");
}

bool nc_midi_notes_delete_overlaps(NCEngine* engine, const char* regionId,
                                   const char* const* noteIds, int count) {
    if (engine == nullptr || regionId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::deleteMidiNoteOverlaps(engine->project, regionId,
                                                                 noteIdVector(noteIds, count)),
                         "Delete overlaps");
}

bool nc_midi_notes_set_length(NCEngine* engine, const char* regionId,
                              const char* const* noteIds, int count, double lengthBeats) {
    if (engine == nullptr || regionId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::setMidiNoteLengths(engine->project, regionId,
                                                             noteIdVector(noteIds, count), lengthBeats),
                         "Fixed lengths");
}

// --- Controller (CC) lanes -------------------------------------------------
// Reads are filtered to one controller number so the UI paints exactly one lane at a time.

int nc_midi_cc_count(NCEngine* engine, const char* regionId, int controller) {
    const auto* region = midiRegionById(engine, regionId);
    if (region == nullptr) return 0;
    int count = 0;
    for (const auto& event : region->controllerEvents) {
        if (event.controller == controller) ++count;
    }
    return count;
}

bool nc_midi_cc_get(NCEngine* engine, const char* regionId, int controller, int index,
                    char* outId, size_t idLen, double* outBeat, int* outValue) {
    copyText(outId, idLen, "");
    const auto* region = midiRegionById(engine, regionId);
    if (region == nullptr || index < 0) return false;
    int match = 0;
    for (const auto& event : region->controllerEvents) {
        if (event.controller != controller) continue;
        if (match == index) {
            copyText(outId, idLen, event.id);
            if (outBeat != nullptr) *outBeat = event.beat;
            if (outValue != nullptr) *outValue = event.value;
            return true;
        }
        ++match;
    }
    return false;
}

bool nc_midi_cc_add(NCEngine* engine, const char* regionId, int controller, double beat, int value,
                    char* outId, size_t idLen) {
    copyText(outId, idLen, "");
    if (engine == nullptr || regionId == nullptr) return false;
    const std::string id = neuracoust::daw::addMidiControllerEvent(engine->project, regionId,
                                                                   beat, controller, value);
    if (id.empty()) return false;
    applyMidiEdit(engine, true, "Add controller");
    copyText(outId, idLen, id);
    return true;
}

bool nc_midi_cc_move(NCEngine* engine, const char* regionId, const char* eventId, double beat, int value) {
    if (engine == nullptr || regionId == nullptr || eventId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::moveMidiControllerEvent(engine->project, regionId,
                                                                  eventId, beat, value),
                         nullptr);
}

bool nc_midi_cc_delete(NCEngine* engine, const char* regionId, const char* eventId) {
    if (engine == nullptr || regionId == nullptr || eventId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::deleteMidiControllerEvent(engine->project, regionId, eventId),
                         "Delete controller");
}

// --- Pitch-bend lane -------------------------------------------------------

int nc_midi_pb_count(NCEngine* engine, const char* regionId) {
    const auto* region = midiRegionById(engine, regionId);
    return region != nullptr ? static_cast<int>(region->pitchBendEvents.size()) : 0;
}

bool nc_midi_pb_get(NCEngine* engine, const char* regionId, int index,
                    char* outId, size_t idLen, double* outBeat, int* outValue) {
    copyText(outId, idLen, "");
    const auto* region = midiRegionById(engine, regionId);
    if (region == nullptr || index < 0 ||
        static_cast<size_t>(index) >= region->pitchBendEvents.size()) {
        return false;
    }
    const auto& event = region->pitchBendEvents[static_cast<size_t>(index)];
    copyText(outId, idLen, event.id);
    if (outBeat != nullptr) *outBeat = event.beat;
    if (outValue != nullptr) *outValue = event.value;
    return true;
}

bool nc_midi_pb_add(NCEngine* engine, const char* regionId, double beat, int value,
                    char* outId, size_t idLen) {
    copyText(outId, idLen, "");
    if (engine == nullptr || regionId == nullptr) return false;
    const std::string id = neuracoust::daw::addMidiPitchBendEvent(engine->project, regionId, beat, value);
    if (id.empty()) return false;
    applyMidiEdit(engine, true, "Add pitch bend");
    copyText(outId, idLen, id);
    return true;
}

bool nc_midi_pb_move(NCEngine* engine, const char* regionId, const char* eventId, double beat, int value) {
    if (engine == nullptr || regionId == nullptr || eventId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::moveMidiPitchBendEvent(engine->project, regionId,
                                                                 eventId, beat, value),
                         nullptr);
}

bool nc_midi_pb_delete(NCEngine* engine, const char* regionId, const char* eventId) {
    if (engine == nullptr || regionId == nullptr || eventId == nullptr) return false;
    return applyMidiEdit(engine,
                         neuracoust::daw::deleteMidiPitchBendEvent(engine->project, regionId, eventId),
                         "Delete pitch bend");
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

// --- Song-form / arrangement sections (per-project; kept in project.songSections) ---
namespace {
void sortSongSections(std::vector<neuracoust::daw::ChordEventState>& s) {
    std::sort(s.begin(), s.end(), [](const neuracoust::daw::ChordEventState& a, const neuracoust::daw::ChordEventState& b) {
        return a.timeSeconds < b.timeSeconds;
    });
}
}
int nc_song_section_count(NCEngine* engine) {
    return engine == nullptr ? 0 : static_cast<int>(engine->project.songSections.size());
}
double nc_song_section_time(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.songSections.size()) return 0.0;
    return engine->project.songSections[static_cast<size_t>(index)].timeSeconds;
}
void nc_song_section_name(NCEngine* engine, int index, char* out, size_t outLen) {
    if (engine == nullptr || index < 0 || static_cast<size_t>(index) >= engine->project.songSections.size()) {
        copyText(out, outLen, std::string{});
        return;
    }
    copyText(out, outLen, engine->project.songSections[static_cast<size_t>(index)].name);
}
bool nc_song_section_add(NCEngine* engine, double timeSeconds, const char* name) {
    if (engine == nullptr) return false;
    auto& sections = engine->project.songSections;
    sections.erase(std::remove_if(sections.begin(), sections.end(),
        [&](const neuracoust::daw::ChordEventState& s) { return std::abs(s.timeSeconds - timeSeconds) < 0.05; }), sections.end());
    neuracoust::daw::ChordEventState section;
    section.id = "section-" + std::to_string(static_cast<long long>(timeSeconds * 1000.0)) + "-" + std::to_string(sections.size());
    section.name = (name != nullptr && *name != '\0') ? name : "Section";
    section.timeSeconds = std::max(0.0, timeSeconds);
    sections.push_back(section);
    sortSongSections(sections);
    engine->recordStep("Add song section");
    return true;
}
bool nc_song_section_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds) {
    if (engine == nullptr) return false;
    auto& sections = engine->project.songSections;
    for (auto& s : sections) {
        if (std::abs(s.timeSeconds - fromSeconds) <= tol) {
            s.timeSeconds = std::max(0.0, toSeconds);
            sortSongSections(sections);
            engine->recordStep("Move song section");
            return true;
        }
    }
    return false;
}
bool nc_song_section_delete(NCEngine* engine, double timeSeconds, double tol) {
    if (engine == nullptr) return false;
    auto& sections = engine->project.songSections;
    const size_t before = sections.size();
    sections.erase(std::remove_if(sections.begin(), sections.end(),
        [&](const neuracoust::daw::ChordEventState& s) { return std::abs(s.timeSeconds - timeSeconds) <= tol; }), sections.end());
    if (sections.size() == before) return false;
    engine->recordStep("Delete song section");
    return true;
}

// Pro Tools range edit on the conductor lanes: delete every conductor event (marker, chord, lyric,
// song section, tempo, meter) whose time falls inside [start, end], in ONE undo step. The tempo and
// meter maps keep their t=0 anchor. Returns the number removed. Key events are app-side (Swift).
int nc_conductor_clear_range(NCEngine* engine, double start, double end) {
    if (engine == nullptr || !(end > start)) return 0;
    auto& p = engine->project;
    const double lo = start, hi = end;
    auto inRange = [lo, hi](double t) { return t >= lo - 1e-6 && t <= hi + 1e-6; };
    auto clearVec = [&](auto& vec, bool keepAnchor) -> int {
        const size_t before = vec.size();
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const auto& e) {
            if (keepAnchor && e.timeSeconds <= 1e-6) return false;   // never drop the t=0 anchor
            return inRange(e.timeSeconds);
        }), vec.end());
        return static_cast<int>(before - vec.size());
    };
    int removed = 0;
    removed += clearVec(p.markers, false);
    removed += clearVec(p.chordEvents, false);
    removed += clearVec(p.lyricEvents, false);
    removed += clearVec(p.songSections, false);
    removed += clearVec(p.tempoMap, true);
    removed += clearVec(p.timeSignatureMap, true);
    if (removed == 0) return 0;
    engine->reconcileProject();   // tempo/meter edits reshape the musical timeline
    engine->recordStep("Clear conductor range");
    return removed;
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
// Set the project's base tempo (the transport TEMPO field), keeping the t=0 anchor in the
// tempo map in sync so the conductor lane and the transport always agree. Creates the anchor
// if the map has none.
bool nc_project_set_tempo_bpm(NCEngine* engine, int bpm) {
    if (engine == nullptr || bpm < 1 || bpm > 999) return false;
    engine->project.tempoBpm = bpm;
    auto& map = engine->project.tempoMap;
    bool anchored = false;
    for (auto& m : map) {
        if (m.timeSeconds <= 1e-6) { m.bpm = static_cast<double>(bpm); anchored = true; break; }
    }
    if (!anchored) {
        map.insert(map.begin(), neuracoust::daw::TempoMarkerState{0.0, static_cast<double>(bpm)});
    }
    engine->reconcileProject();
    engine->recordStep("Set tempo");
    return true;
}
// Set the project's base time signature (the transport SIG field), keeping the t=0 meter
// anchor in sync the same way.
bool nc_project_set_time_signature(NCEngine* engine, int numerator, int denominator) {
    if (engine == nullptr || numerator < 1 || numerator > 32 || denominator < 1 || denominator > 32) {
        return false;
    }
    engine->project.timeSignatureNumerator = numerator;
    engine->project.timeSignatureDenominator = denominator;
    auto& map = engine->project.timeSignatureMap;
    bool anchored = false;
    for (auto& m : map) {
        if (m.timeSeconds <= 1e-6) { m.numerator = numerator; m.denominator = denominator; anchored = true; break; }
    }
    if (!anchored) {
        map.insert(map.begin(), neuracoust::daw::TimeSignatureMarkerState{0.0, numerator, denominator});
    }
    engine->reconcileProject();
    engine->recordStep("Set time signature");
    return true;
}
bool nc_tempo_marker_delete(NCEngine* engine, double timeSeconds, double tol) {
    if (engine == nullptr) return false;
    if (!neuracoust::daw::deleteNearestTempoMarker(engine->project, timeSeconds, tol)) return false;
    engine->reconcileProject();
    engine->recordStep("Delete tempo");
    return true;
}
bool nc_tempo_marker_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds) {
    if (engine == nullptr) return false;
    auto& map = engine->project.tempoMap;
    for (auto& m : map) {
        if (std::abs(m.timeSeconds - fromSeconds) <= tol) {
            m.timeSeconds = std::max(0.0, toSeconds);
            std::sort(map.begin(), map.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
            engine->reconcileProject(); engine->recordStep("Move tempo"); return true;
        }
    }
    return false;
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
        if (std::abs(m.timeSeconds - fromSeconds) <= tol) {   // the start event may move too
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
        || isPluginAutomationParameter(parameterId)
        || isGenericMixerAutomationParameter(parameterId);
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
        for (const auto& lane : track.automationLanes) {
            if (lane.parameterId.rfind("instrument.", 0) != 0 || lane.points.empty()) continue;
            int slot = -1; unsigned int pid = 0;
            if (std::sscanf(lane.parameterId.c_str(), "instrument.%d.%u", &slot, &pid) != 2 || slot < 0) continue;
            const float v = std::max(0.0f, std::min(1.0f, evalAutomationPoints(lane.points, timeSeconds, 0.0f)));
            engine->engine.updateInstrumentVst3Parameter(track.name, static_cast<size_t>(slot),
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

// Write mode / a moving Touch pass: erase existing points the playhead has just swept over
// (fromExclusive, toInclusive] and drop the new value at `toInclusive`. No history step.
bool nc_track_automation_write_sweep(NCEngine* engine, int trackIndex, const char* parameterId,
                                     double fromExclusive, double toInclusive, float value) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || !nc_automation_parameter_supported(parameterId)) return false;
    const std::string trackName = track->name;
    if (toInclusive > fromExclusive + 1e-6) {
        const double eraseStart = fromExclusive + 1e-4;        // keep the previous tick's point
        if (isVolumeParameter(parameterId))
            neuracoust::daw::deleteTrackVolumeAutomationPointsInRange(engine->project, trackName, eraseStart, toInclusive);
        else
            neuracoust::daw::deleteTrackAutomationLanePointsInRange(engine->project, trackName, parameterId, eraseStart, toInclusive);
    }
    const bool added = isVolumeParameter(parameterId)
        ? neuracoust::daw::setTrackVolumeAutomationPoint(engine->project, trackName, toInclusive, value)
        : neuracoust::daw::setTrackAutomationLanePoint(engine->project, trackName, parameterId,
              isPanParameter(parameterId) ? "Pan" : parameterId, toInclusive, value);
    engine->reconcileProject();
    return added;
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

// Serialize ONE clip as its own minimal project: the clip at 0 s on the first audio track of a
// default project, everything else empty and flat, so a bounce of it is exactly the clip as it
// sounds at clip level — window, gain, fades, reverse/normalize/Ø, prints — with no console,
// inserts, fader or monitor colouring. Main-thread (reads the live project); the render itself
// happens in nc_clip_snapshot_export, off-thread, the same split as the background bounce.
int nc_clip_export_serialize(NCEngine* engine, const char* clip_id, char* out, size_t outLen) {
    if (engine == nullptr || clip_id == nullptr) {
        return 0;
    }
    const auto* clip = findClip(engine, clip_id);
    if (clip == nullptr) {
        return 0;
    }
    neuracoust::daw::ProjectDocument mini = neuracoust::daw::defaultProject();
    mini.sampleRate = engine->project.sampleRate;   // render at the session rate; afconvert resamples
    const auto audioTrack = std::find_if(mini.tracks.begin(), mini.tracks.end(),
                                         [](const auto& track) { return track.trackType == "audio"; });
    if (audioTrack == mini.tracks.end()) {
        return 0;
    }
    neuracoust::daw::ClipState exported = *clip;
    exported.trackName = audioTrack->name;
    exported.startSeconds = 0.0;
    exported.muted = false;         // exporting a muted clip should export its audio, not silence
    exported.crossfadeInSeconds = 0.0;   // derived from neighbours that are not coming along
    exported.crossfadeOutSeconds = 0.0;
    mini.clips.clear();
    mini.clips.push_back(std::move(exported));
    neuracoust::daw::rebuildProjectEditModelFromClips(mini);
    const std::string text = neuracoust::daw::serializeProject(mini);
    if (out != nullptr && outLen > 0) {
        copyText(out, outLen, text);
    }
    return static_cast<int>(text.size());
}

// Render a clip snapshot (from nc_clip_export_serialize) and convert it to `spec` at `path`.
// Engine-free, so it can run off the main thread. Specs are AudioImport's convertAudioFileToSpec
// strings ("wav24:48000", "flac:0", ...).
bool nc_clip_snapshot_export(const char* projectText, const char* spec, const char* path,
                             char* message, size_t messageLen) {
    const auto say = [&](const std::string& text) {
        if (message != nullptr && messageLen > 0) copyText(message, messageLen, text);
    };
    if (projectText == nullptr || spec == nullptr || path == nullptr || *path == '\0') {
        say("내보낼 클립/규격/경로가 없습니다");
        return false;
    }
    neuracoust::daw::ProjectDocument project;
    std::string error;
    if (!neuracoust::daw::deserializeProject(projectText, project, error)) {
        say(error.empty() ? "클립 스냅샷을 읽을 수 없습니다" : error);
        return false;
    }
    std::error_code fsError;
    const auto tempWav = std::filesystem::temp_directory_path() /
        ("nc-clip-export-" + std::to_string(::getpid()) + "-" +
         std::to_string(reinterpret_cast<uintptr_t>(&project)) + ".wav");
    const auto bounce = neuracoust::daw::bounceProjectToWav(project, tempWav.string());
    if (!bounce.ok) {
        std::filesystem::remove(tempWav, fsError);
        say(bounce.message.empty() ? "클립 렌더에 실패했습니다" : bounce.message);
        return false;
    }
    if (!bounce.missingMediaClipIds.empty()) {
        std::filesystem::remove(tempWav, fsError);
        say("클립의 소스 파일을 찾을 수 없습니다");
        return false;
    }
    const bool converted = neuracoust::daw::convertAudioFileToSpec(tempWav, path, spec, error);
    std::filesystem::remove(tempWav, fsError);
    if (!converted) {
        say(error);
        return false;
    }
    say(std::string("클립 내보내기 완료: ") + path);
    return true;
}

// Apply just the monitor-station configuration from a serialized project (used as the global
// "전체 설정 저장" template) onto the current project, so a new/blank session inherits the
// saved monitor setup — listen mode, mono/swap/phase, dim/talkback, input trim, volume, and
// the whole DSP-module chain incl. the A/B/C speaker slots. Everything else is left alone.
bool nc_apply_monitor_template(NCEngine* engine, const char* serialized) {
    if (engine == nullptr || serialized == nullptr || *serialized == '\0') return false;
    neuracoust::daw::ProjectDocument tmpl;
    std::string err;
    if (!neuracoust::daw::deserializeProject(serialized, tmpl, err)) return false;
    auto& p = engine->project;
    p.monitorStationMono = tmpl.monitorStationMono;
    p.monitorStationListenMode = tmpl.monitorStationListenMode;
    p.monitorStationSwapLeftRight = tmpl.monitorStationSwapLeftRight;
    p.monitorStationInvertLeft = tmpl.monitorStationInvertLeft;
    p.monitorStationInvertRight = tmpl.monitorStationInvertRight;
    p.monitorStationMute = tmpl.monitorStationMute;
    p.monitorStationDim = tmpl.monitorStationDim;
    p.monitorStationTalkback = tmpl.monitorStationTalkback;
    p.monitorStationDimDb = tmpl.monitorStationDimDb;
    p.monitorStationTalkbackRoute = tmpl.monitorStationTalkbackRoute;
    p.monitorStationTalkbackChannel = tmpl.monitorStationTalkbackChannel;
    p.monitorInputTrimDb = tmpl.monitorInputTrimDb;
    p.monitorVolumeDb = tmpl.monitorVolumeDb;
    // Physical monitoring-chain models are part of the saved monitor station — restore them too, or
    // the "전체 설정 저장" template drops them (the audio-interface MODELING target model in
    // particular had no other carrier, so the picker came back empty).
    p.physicalSpeakerModel = tmpl.physicalSpeakerModel;
    p.physicalHeadphoneModel = tmpl.physicalHeadphoneModel;
    p.physicalPowerAmpModel = tmpl.physicalPowerAmpModel;
    p.physicalSpeakerCableModel = tmpl.physicalSpeakerCableModel;
    p.physicalPowerCableModel = tmpl.physicalPowerCableModel;
    p.physicalConnectorModel = tmpl.physicalConnectorModel;
    p.physicalAudioInterfaceModel = tmpl.physicalAudioInterfaceModel;
    p.physicalAudioInterfaceTargetModel = tmpl.physicalAudioInterfaceTargetModel;
    p.monitorInterfaceModelingEnabled = tmpl.monitorInterfaceModelingEnabled;
    if (!tmpl.monitorModules.empty()) {
        p.monitorModules = tmpl.monitorModules;
    }
    p.monitorEqBands = tmpl.monitorEqBands;
    engine->reconcileProject();
    engine->pushInterfaceModeler();   // re-apply the restored interface modeling to the live monitor path
    return true;
}

bool nc_import_aaf(NCEngine* engine, const char* path, char* msgOut, size_t msgLen) {
    copyText(msgOut, msgLen, "");
    if (engine == nullptr || path == nullptr || *path == '\0') {
        copyText(msgOut, msgLen, "AAF 경로가 없습니다.");
        return false;
    }
    neuracoust::daw::ProjectDocument imported;
    const auto result = neuracoust::daw::importAafSession(path, imported);
    copyText(msgOut, msgLen, result.message);
    if (!result.ok) {
        return false;
    }
    // Replaces the open document, like opening a project — the import is a whole session.
    // Replaces the open document, like opening a project — an AAF import IS a whole session. No
    // path yet: it came from an .aaf, so the first save must ask where to put the .ndaw.
    engine->project = std::move(imported);
    engine->projectPath.clear();
    engine->autosaveError.clear();
    adoptProject(engine);
    return true;
}

bool nc_aaf_import_available(void) {
    return neuracoust::daw::aafImportAvailable();
}

int nc_bounce_stems(NCEngine* engine, const char* folderPath, char* errOut, size_t errLen) {
    copyText(errOut, errLen, "");
    if (engine == nullptr || folderPath == nullptr || *folderPath == '\0') {
        copyText(errOut, errLen, "출력 폴더가 없습니다.");
        return 0;
    }
    std::error_code ec;
    std::filesystem::create_directories(folderPath, ec);
    if (ec) {
        copyText(errOut, errLen, std::string("폴더를 만들 수 없습니다: ") + ec.message());
        return 0;
    }

    // Every stem is rendered from the SAME document with one track soloed, so each file starts at
    // 00:00 and runs the full session length. Dropping them onto tracks in another DAW at zero then
    // lines them up exactly — which is the whole point of stems as an interchange path.
    int written = 0;
    std::string lastError;
    for (const auto& track : engine->project.tracks) {
        if (track.trackType == "master" || track.trackType == "monitor" || track.trackType == "folder") {
            continue;
        }
        neuracoust::daw::ProjectDocument stemProject = engine->project;
        bool soloedAnything = false;
        for (auto& candidate : stemProject.tracks) {
            const bool isTarget = candidate.name == track.name;
            if (candidate.trackType == "master" || candidate.trackType == "monitor") {
                continue;
            }
            candidate.solo = isTarget;
            candidate.muted = false;   // solo decides; a pre-existing mute must not silence the stem
            if (isTarget) soloedAnything = true;
        }
        if (!soloedAnything) {
            continue;
        }
        // A filename from the track name, with anything path-hostile replaced.
        std::string safeName;
        for (const char ch : track.name) {
            safeName.push_back((ch == '/' || ch == '\\' || ch == ':') ? '_' : ch);
        }
        if (safeName.empty()) safeName = "Track";
        const auto outPath = (std::filesystem::path(folderPath) / (safeName + ".wav")).string();
        const auto result = neuracoust::daw::bounceProjectToWav(stemProject, outPath);
        if (result.ok) {
            ++written;
        } else {
            lastError = result.message;
        }
    }
    if (written == 0) {
        copyText(errOut, errLen, lastError.empty() ? "내보낼 트랙이 없습니다." : lastError);
    }
    return written;
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

bool nc_bounce_snapshot_to_wav_remote(const char* projectText, const char* path, NCBounceResult* out) {
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
    neuracoust::daw::BounceOptions options;
    options.useAssignedRemoteDsp = true;
    options.remoteDsp = buildRemoteDspSettingsFromProject(project);
    const auto result = neuracoust::daw::bounceProjectToWav(project, path, options);
    fillBounceResult(result, out);
    return result.ok;
}

namespace {

/// Peaks are cached at a fixed sample resolution, not a fixed bucket count, so a long clip does
/// not smear. 32 samples per peak is ~0.7 ms at 48 kHz — fine enough that the waveform stays
/// detailed even when you zoom to a few dozen samples per pixel (256 was ~5 ms and went blocky
/// on deep zooms). The view decimates these to columns at draw time. The ceiling keeps a very
/// long file from allocating without bound — past it the samples per peak grows instead.
constexpr int64_t kWaveformSamplesPerPeak = 32;
constexpr int64_t kWaveformMaxPeaks = 4'000'000;  // ~44 min at 32 samples/peak, 48 kHz (then coarsens)

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
    engine->pluginScanSignature = neuracoust::daw::vst3BundleInventorySignature();
    return static_cast<int>(engine->plugins.size());
}

// Force a fresh scan from disk (ignores the persistent cache) so a newly-installed plug-in
// appears without restarting the app.
int nc_plugin_rescan(NCEngine* engine) {
    if (engine == nullptr) return 0;
    engine->plugins = neuracoust::daw::scanKnownPluginLocations(true);
    neuracoust::daw::sortPluginCandidatesForDisplay(engine->plugins);
    engine->facets = neuracoust::daw::pluginCandidateFilterOptions(engine->plugins);
    engine->filteredPlugins = engine->plugins;
    engine->pluginScanSignature = neuracoust::daw::vst3BundleInventorySignature();
    return static_cast<int>(engine->plugins.size());
}

// True if the installed .vst3 set changed since the last scan — the browser rescans on open
// so a plug-in installed mid-session appears without the user hunting for the ↻ button.
bool nc_plugin_locations_changed(NCEngine* engine) {
    if (engine == nullptr) return false;
    return neuracoust::daw::vst3BundleInventorySignature() != engine->pluginScanSignature;
}

int nc_plugin_apply_filter(NCEngine* engine,
                           const char* text,
                           const char* brand,
                           const char* category,
                           const char* format,
                           const char* excludeCategory) {
    if (engine == nullptr) return 0;

    neuracoust::daw::PluginCandidateFilterCriteria criteria;
    criteria.text = text != nullptr ? text : "";
    criteria.brand = brand != nullptr ? brand : "";
    criteria.category = category != nullptr ? category : "";
    criteria.format = format != nullptr ? format : "";
    criteria.excludeCategory = excludeCategory != nullptr ? excludeCategory : "";
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

void nc_plugin_ara_info(NCEngine* engine, int index, char* out, size_t outLen) {
    copyText(out, outLen, "");
    const auto* plugin = pluginAt(engine, index);
    if (plugin == nullptr) {
        return;
    }
    const auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
        plugin->name, plugin->path, plugin->pluginClassId, plugin->pluginClassName);
    const auto info = neuracoust::daw::inspectAraFactory(descriptor);
    if (!info.available) {
        copyText(out, outLen, info.message);
        return;
    }
    // What the plug-in itself reports — not a guess from its name.
    std::string text = info.plugInName + " " + info.versionString + " · " + info.manufacturerName +
                       " · ARA " + std::to_string(info.lowestSupportedApiGeneration) + "–" +
                       std::to_string(info.highestSupportedApiGeneration);
    if (!info.compatibleWithHost) {
        text += " · 호스트와 세대 불일치";
    }
    copyText(out, outLen, text);
}

bool nc_plugin_is_ara(NCEngine* engine, int index) {
    const auto* plugin = pluginAt(engine, index);
    return plugin != nullptr && plugin->araCapable;
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
    // Instruments render as stereo, so an instrument track is defined stereo automatically
    // (a mono track picking up an instrument follows the instrument). The user can override.
    track->channelFormat = "stereo";
    engine->reconcileProject();
    engine->recordStep("Load " + instrument.pluginName);
    return true;
}

// --- Instrument rack (layering): several instruments on one track, all fed the same MIDI
// and summed. The engine already supports up to 8 slots; these expose them. ---

static bool instrumentSlotFilled(const neuracoust::daw::InstrumentSlotState& slot) {
    return !slot.pluginPath.empty() && slot.pluginName != "No Instrument" && !slot.pluginName.empty();
}

int nc_track_instrument_slot_count(NCEngine* engine, int trackIndex) {
    const auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return 0;
    int count = 0;
    for (const auto& slot : track->instrumentSlots) {
        if (instrumentSlotFilled(slot)) ++count;
    }
    if (count == 0 && instrumentSlotFilled(track->instrument)) count = 1;   // legacy single slot
    return count;
}

void nc_track_instrument_slot_name(NCEngine* engine, int trackIndex, int slotIndex, char* out, size_t outLen) {
    const auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slotIndex < 0) { copyText(out, outLen, ""); return; }
    if (static_cast<size_t>(slotIndex) < track->instrumentSlots.size()) {
        copyText(out, outLen, track->instrumentSlots[static_cast<size_t>(slotIndex)].pluginName);
    } else if (slotIndex == 0) {
        copyText(out, outLen, track->instrument.pluginName);
    } else {
        copyText(out, outLen, "");
    }
}

bool nc_track_set_instrument_slot(NCEngine* engine, int trackIndex, int slotIndex, int pluginIndex) {
    auto* track = trackAt(engine, trackIndex);
    const auto* plugin = pluginAt(engine, pluginIndex);
    if (track == nullptr || plugin == nullptr || slotIndex < 0) return false;
    neuracoust::daw::InstrumentSlotState instrument;
    instrument.pluginName = plugin->name;
    instrument.pluginFormat = plugin->format.empty() ? "VST3" : plugin->format;
    instrument.pluginPath = plugin->path;
    instrument.pluginClassId = plugin->pluginClassId;
    instrument.pluginClassName = plugin->pluginClassName;
    instrument.enabled = plugin->exists;
    instrument.bypassed = false;
    const std::string trackName = track->name;
    if (!neuracoust::daw::setTrackInstrumentSlot(engine->project, trackName, static_cast<size_t>(slotIndex), instrument)) {
        return false;
    }
    track->channelFormat = "stereo";
    engine->reconcileProject();
    engine->recordStep(slotIndex == 0 ? ("Load " + instrument.pluginName) : "Add instrument layer");
    return true;
}

// Per-layer mute (bypass) and solo in the instrument rack.
bool nc_track_instrument_slot_bypassed(NCEngine* engine, int trackIndex, int slotIndex) {
    const auto* slot = slotIndex < 0 ? nullptr : instrumentSlotMutable(engine, trackIndex, static_cast<size_t>(slotIndex));
    return slot != nullptr && slot->bypassed;
}
bool nc_track_instrument_slot_soloed(NCEngine* engine, int trackIndex, int slotIndex) {
    const auto* slot = slotIndex < 0 ? nullptr : instrumentSlotMutable(engine, trackIndex, static_cast<size_t>(slotIndex));
    return slot != nullptr && slot->soloed;
}
bool nc_track_toggle_instrument_slot_bypass(NCEngine* engine, int trackIndex, int slotIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slotIndex < 0) return false;
    if (!neuracoust::daw::toggleTrackInstrumentBypass(engine->project, track->name, static_cast<size_t>(slotIndex))) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Instrument layer mute");
    return true;
}
bool nc_track_toggle_instrument_slot_solo(NCEngine* engine, int trackIndex, int slotIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slotIndex < 0) return false;
    if (!neuracoust::daw::toggleTrackInstrumentSlotSolo(engine->project, track->name, static_cast<size_t>(slotIndex))) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Instrument layer solo");
    return true;
}

bool nc_track_remove_instrument_slot(NCEngine* engine, int trackIndex, int slotIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slotIndex < 0) return false;
    const std::string trackName = track->name;
    if (!neuracoust::daw::clearTrackInstrumentSlot(engine->project, trackName, static_cast<size_t>(slotIndex))) {
        return false;
    }
    engine->reconcileProject();
    engine->recordStep("Remove instrument layer");
    return true;
}

void nc_track_instrument_name(NCEngine* engine, int trackIndex, char* out, size_t outLen) {
    const auto* track = trackAt(engine, trackIndex);
    copyText(out, outLen, track != nullptr ? track->instrument.pluginName : std::string{});
}

void nc_last_plugin_message(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->lastPluginMessage : std::string{});
}

bool nc_track_add_insert(NCEngine* engine, int trackIndex, int pluginIndex) {
    auto* track = trackAt(engine, trackIndex);
    const auto* plugin = pluginAt(engine, pluginIndex);
    if (track == nullptr || plugin == nullptr) {
        return false;
    }

    // FX only: a virtual instrument belongs in the instrument slot, never an insert. Reject it
    // here so instruments and effects stay strictly separated regardless of which UI path called.
    if (plugin->category.find("Instrument") != std::string::npos) {
        return false;
    }

    // ARA plug-ins (Melodyne and friends) are not realtime effects — hosted as a plain insert they
    // run their own transport and wedge the DAW. Refuse with an explanation rather than adding a
    // slot that hangs.
    {
        const auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
            plugin->name, plugin->path, plugin->pluginClassId, plugin->pluginClassName);
        if (neuracoust::daw::requiresAraHost(descriptor)) {
            engine->lastPluginMessage = neuracoust::daw::araRequiredMessage(descriptor);
            return false;
        }
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

// --- Built-in test signal generator (a track SOURCE) --------------------------------------------
// Stored as a recognized "Signal Generator" insert so it rides the normal insert persistence / undo /
// reconcile; the renderer's synthesizeSourceGeneratorFallback voices it with the high-accuracy
// TestSignalGenerator whenever the track is otherwise silent. The six normalized params are the
// storage; these setters/getters speak in real units and use the inverse of the renderer's mappings.
namespace {
constexpr uint32_t kGenOnOff = 0, kGenWave = 1, kGenFreq = 2, kGenLevel = 3, kGenRoute = 4, kGenPhase = 5;

bool insertIsTestSignalGenerator(const neuracoust::daw::TrackInsertSlot& s) {
    std::string n = s.pluginName;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
    return n.find("signal generator") != std::string::npos || n.find("emo-generator") != std::string::npos;
}

int findTestSignalGeneratorSlot(const neuracoust::daw::TrackState& track) {
    for (size_t i = 0; i < track.inserts.size(); ++i) {
        if (insertIsTestSignalGenerator(track.inserts[i])) return static_cast<int>(i);
    }
    return -1;
}

double genParamValue(const neuracoust::daw::TrackInsertSlot& s, uint32_t id, double fallback) {
    for (const auto& p : s.parameters) {
        if (p.parameterId == id) return std::clamp(static_cast<double>(p.normalizedValue), 0.0, 1.0);
    }
    return fallback;
}

void setGenParamValue(neuracoust::daw::TrackInsertSlot& s, uint32_t id, const char* name, double norm) {
    const double v = std::clamp(norm, 0.0, 1.0);
    for (auto& p : s.parameters) {
        if (p.parameterId == id) { p.normalizedValue = static_cast<float>(v); return; }
    }
    s.parameters.push_back({id, name, static_cast<float>(v)});
}

// Real-unit ⇄ normalized, matching synthesizeSourceGeneratorFallback exactly.
double freqNormFromHz(double hz) { return std::clamp(std::log(std::clamp(hz, 20.0, 20000.0) / 20.0) / std::log(1000.0), 0.0, 1.0); }
double hzFromFreqNorm(double norm) { return std::clamp(20.0 * std::pow(1000.0, norm), 20.0, 20000.0); }
double levelNormFromDb(double db) { return std::clamp((db + 60.0) / 60.0, 0.0, 1.0); }
double dbFromLevelNorm(double norm) { return -60.0 + norm * 60.0; }

// Apply a param change on the generator insert and reconcile so the render picks it up. `record`
// makes it one undo step (discrete edits); continuous edits (freq/level sliders) pass false.
bool mutateGenerator(NCEngine* engine, int trackIndex,
                     const std::function<void(neuracoust::daw::TrackInsertSlot&)>& fn,
                     const char* step) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return false;
    const int slot = findTestSignalGeneratorSlot(*track);
    if (slot < 0) return false;
    fn(track->inserts[static_cast<size_t>(slot)]);
    engine->reconcileProject();
    if (step != nullptr) engine->recordStep(step);
    return true;
}
}  // namespace

bool nc_track_add_test_signal_generator(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return false;
    if (findTestSignalGeneratorSlot(*track) >= 0) return true;   // one per track

    const std::string trackName = track->name;
    size_t slot = track->inserts.size();
    for (size_t i = 0; i < track->inserts.size(); ++i) {
        const auto& e = track->inserts[i];
        if (!e.enabled || e.pluginName.empty() || e.pluginName == "No Insert") { slot = i; break; }
    }
    if (slot >= track->inserts.size() && !neuracoust::daw::addTrackInsertSlot(engine->project, trackName)) {
        return false;
    }
    neuracoust::daw::TrackInsertSlot gen;
    gen.pluginName = "Signal Generator";
    gen.pluginFormat = "Builtin";
    gen.enabled = true;
    gen.dspAvailable = true;
    gen.dspExecutionMode = "native";
    gen.parameters = {
        {kGenOnOff, "On Off", 1.0f},
        {kGenWave, "Waveform", 0.0f},              // Sine
        {kGenFreq, "Frequency", static_cast<float>(freqNormFromHz(1000.0))},
        {kGenLevel, "Level", static_cast<float>(levelNormFromDb(-6.0))},
        {kGenRoute, "Channel", 0.5f},              // Stereo
        {kGenPhase, "Polarity", 0.0f},
    };
    if (!neuracoust::daw::setTrackInsertSlot(engine->project, trackName, slot, gen)) return false;
    engine->reconcileProjectDeclicked();
    engine->recordStep("Add signal generator");
    return true;
}

bool nc_track_remove_test_signal_generator(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return false;
    const int slot = findTestSignalGeneratorSlot(*track);
    if (slot < 0) return false;
    if (!neuracoust::daw::removeTrackInsertSlot(engine->project, track->name, static_cast<size_t>(slot))) {
        return false;
    }
    engine->reconcileProjectDeclicked();
    engine->recordStep("Remove signal generator");
    return true;
}

// -1 = no generator on this track, else its insert slot index.
int nc_track_test_signal_generator_slot(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    return track != nullptr ? findTestSignalGeneratorSlot(*track) : -1;
}

void nc_track_test_signal_set_enabled(NCEngine* engine, int trackIndex, bool enabled) {
    mutateGenerator(engine, trackIndex,
                    [&](auto& s) { setGenParamValue(s, kGenOnOff, "On Off", enabled ? 1.0 : 0.0); },
                    enabled ? "Signal generator on" : "Signal generator off");
}
void nc_track_test_signal_set_waveform(NCEngine* engine, int trackIndex, int waveform) {
    const int w = std::max(0, std::min(6, waveform));
    mutateGenerator(engine, trackIndex,
                    [&](auto& s) { setGenParamValue(s, kGenWave, "Waveform", w / 6.0); }, "Signal waveform");
}
void nc_track_test_signal_set_frequency_hz(NCEngine* engine, int trackIndex, double hz) {
    mutateGenerator(engine, trackIndex,
                    [&](auto& s) { setGenParamValue(s, kGenFreq, "Frequency", freqNormFromHz(hz)); }, nullptr);
}
void nc_track_test_signal_set_level_db(NCEngine* engine, int trackIndex, double db) {
    mutateGenerator(engine, trackIndex,
                    [&](auto& s) { setGenParamValue(s, kGenLevel, "Level", levelNormFromDb(db)); }, nullptr);
}
void nc_track_test_signal_set_channel(NCEngine* engine, int trackIndex, int channel) {
    const double norm = channel <= 0 ? 0.0 : (channel >= 2 ? 1.0 : 0.5);   // 0=L, 1=Stereo, 2=R
    mutateGenerator(engine, trackIndex,
                    [&](auto& s) { setGenParamValue(s, kGenRoute, "Channel", norm); }, "Signal channel");
}
void nc_track_test_signal_set_polarity(NCEngine* engine, int trackIndex, bool inverted) {
    mutateGenerator(engine, trackIndex,
                    [&](auto& s) { setGenParamValue(s, kGenPhase, "Polarity", inverted ? 1.0 : 0.0); }, "Signal polarity");
}

bool nc_track_test_signal_enabled(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return false;
    const int slot = findTestSignalGeneratorSlot(*track);
    return slot >= 0 && genParamValue(track->inserts[static_cast<size_t>(slot)], kGenOnOff, 1.0) >= 0.5;
}
int nc_track_test_signal_waveform(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return 0;
    const int slot = findTestSignalGeneratorSlot(*track);
    if (slot < 0) return 0;
    return std::max(0, std::min(6, static_cast<int>(std::lround(
        genParamValue(track->inserts[static_cast<size_t>(slot)], kGenWave, 0.0) * 6.0))));
}
double nc_track_test_signal_frequency_hz(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return 1000.0;
    const int slot = findTestSignalGeneratorSlot(*track);
    return slot < 0 ? 1000.0 : hzFromFreqNorm(genParamValue(track->inserts[static_cast<size_t>(slot)], kGenFreq, 0.5));
}
double nc_track_test_signal_level_db(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return -6.0;
    const int slot = findTestSignalGeneratorSlot(*track);
    return slot < 0 ? -6.0 : dbFromLevelNorm(genParamValue(track->inserts[static_cast<size_t>(slot)], kGenLevel, 0.9));
}
int nc_track_test_signal_channel(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return 1;
    const int slot = findTestSignalGeneratorSlot(*track);
    if (slot < 0) return 1;
    const double r = genParamValue(track->inserts[static_cast<size_t>(slot)], kGenRoute, 0.5);
    return r < 0.25 ? 0 : (r > 0.75 ? 2 : 1);
}
bool nc_track_test_signal_polarity(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr) return false;
    const int slot = findTestSignalGeneratorSlot(*track);
    return slot >= 0 && genParamValue(track->inserts[static_cast<size_t>(slot)], kGenPhase, 0.0) >= 0.5;
}

// Copy a filled insert (with its parameters) to another slot on the same or a different
// track — Option-drag in the mixer. dstSlot < 0 appends at the end of the destination.
bool nc_track_copy_insert(NCEngine* engine, int srcTrackIndex, int srcSlot,
                          int dstTrackIndex, int dstSlot) {
    auto* src = trackAt(engine, srcTrackIndex);
    auto* dst = trackAt(engine, dstTrackIndex);
    if (src == nullptr || dst == nullptr || srcSlot < 0 ||
        static_cast<size_t>(srcSlot) >= src->inserts.size()) {
        return false;
    }
    neuracoust::daw::TrackInsertSlot copied = src->inserts[static_cast<size_t>(srcSlot)];  // by value: params included
    if (copied.pluginName.empty() || copied.pluginName == "No Insert") return false;
    copied.dspExecutionMode = neuracoust::daw::defaultPluginInsertDspExecutionMode(
        engine->project, engine->monitorDspEnabled, copied);
    const std::string dstName = dst->name;
    const size_t placed = dst->inserts.size();
    if (!neuracoust::daw::addTrackInsertSlot(engine->project, dstName)) return false;
    if (!neuracoust::daw::setTrackInsertSlot(engine->project, dstName, placed, copied)) return false;
    if (dstSlot >= 0 && static_cast<size_t>(dstSlot) < placed) {
        neuracoust::daw::moveTrackInsertSlotToIndex(engine->project, dstName, placed, static_cast<size_t>(dstSlot));
    }
    engine->reconcileProject();
    engine->recordStep("Copy " + copied.pluginName);
    return true;
}

// Move a filled insert to another track (plain drag across channels). Within one track use
// nc_track_move_insert_to_index. dstSlot < 0 appends.
bool nc_track_move_insert_across(NCEngine* engine, int srcTrackIndex, int srcSlot,
                                 int dstTrackIndex, int dstSlot) {
    auto* src = trackAt(engine, srcTrackIndex);
    if (src == nullptr || srcSlot < 0 || static_cast<size_t>(srcSlot) >= src->inserts.size()) {
        return false;
    }
    if (srcTrackIndex == dstTrackIndex) {
        return nc_track_move_insert_to_index(engine, srcTrackIndex, srcSlot, dstSlot) >= 0;
    }
    if (!nc_track_copy_insert(engine, srcTrackIndex, srcSlot, dstTrackIndex, dstSlot)) return false;
    // src is still valid (copy touched dst's insert vector, not project.tracks).
    neuracoust::daw::removeTrackInsertSlot(engine->project, src->name, static_cast<size_t>(srcSlot));
    engine->reconcileProject();
    engine->recordStep("Move insert");
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

namespace {
// Only `native` (in-process, on the audio thread) and `internal` (out-of-process on the isolated
// performance core, via the sandbox bridge) are user-selectable per channel. remote_internal /
// external need a matching Neuracoust module on a Remote Core and are assigned automatically, not
// from this menu. Works for both TrackInsertSlot and InsertState (both carry dspExecutionMode).
template <typename InsertT>
bool applyUserInsertDspMode(InsertT& insert, const char* mode) {
    if (mode == nullptr) {
        return false;
    }
    const std::string m = mode;
    if (m != "native" && m != "internal") {
        return false;
    }
    if (insert.dspExecutionMode == m) {
        return false;
    }
    insert.dspExecutionMode = m;
    return true;
}
} // namespace

bool nc_track_insert_set_dsp_mode(NCEngine* engine, int trackIndex, int slot, const char* mode) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) {
        return false;
    }
    if (!applyUserInsertDspMode(track->inserts[static_cast<size_t>(slot)], mode)) {
        return false;
    }
    engine->reconcileProjectDeclicked();   // moving on/off the isolated core rebuilds the chain
    engine->recordStep(std::string("Insert DSP mode: ") + mode);
    return true;
}

bool nc_master_insert_set_dsp_mode(NCEngine* engine, int slot, const char* mode) {
    auto* insert = masterInsertAt(engine, slot);
    if (insert == nullptr) {
        return false;
    }
    if (!applyUserInsertDspMode(*insert, mode)) {
        return false;
    }
    engine->reconcileProjectDeclicked();
    engine->recordStep(std::string("Master insert DSP mode: ") + mode);
    return true;
}

void nc_master_insert_dsp_machine(NCEngine* engine, int slot, char* out, size_t outLen) {
    const auto* insert = masterInsertAt(engine, slot);
    copyText(out, outLen, insert != nullptr ? insert->assignedDspServerId : std::string{});
}

void nc_master_insert_set_dsp_machine(NCEngine* engine, int slot, const char* machine) {
    auto* insert = masterInsertAt(engine, slot);
    if (insert == nullptr || machine == nullptr) return;
    const std::string next = machine;
    if (next != "" && next != "internal" && next != "nds" && next != "external") return;
    if (insert->assignedDspServerId == next) return;
    insert->assignedDspServerId = next;
    engine->reconcileProjectDeclicked();
    engine->recordStep("Master insert DSP");
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
// PDC applied to one track/bus (by strip index), in samples. Shows a reverb aux's return latency etc.
int nc_track_delay_compensation_samples(NCEngine* engine, int index) {
    auto* track = trackAt(engine, index);
    if (track == nullptr) return 0;
    return engine->engine.routeDelayCompensationSamplesFor(track->name);
}

bool nc_dsp_core_isolation(NCEngine* engine) {
    return engine != nullptr && engine->project.appleSiliconCoreIsolationEnabled;
}

void nc_dsp_set_core_isolation(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->project.appleSiliconCoreIsolationEnabled == enabled) {
        return;
    }
    engine->project.appleSiliconCoreIsolationEnabled = enabled;
    // No 4-core floor on enable — the user can reserve as few as 1 (kept 1–16 elsewhere).
    engine->recordStep(enabled ? "Enable core isolation" : "Disable core isolation");
    restartEngineForSettings(engine);
}

int nc_dsp_core_count(NCEngine* engine) {
    return engine != nullptr ? std::max(1, std::min(16, engine->project.requestedDspCoreCount)) : 0;
}

void nc_dsp_set_core_count(NCEngine* engine, int count) {
    if (engine == nullptr) return;
    // 1–16, no isolation floor — the user asked to be able to reserve fewer than 4 cores.
    const int clamped = std::max(1, std::min(16, count));
    if (clamped == engine->project.requestedDspCoreCount) {
        return;
    }
    engine->project.requestedDspCoreCount = clamped;
    engine->recordStep("Set DSP core count");
    restartEngineForSettings(engine);
}

int nc_buffer_size(NCEngine* engine) {
    return engine != nullptr ? engine->project.defaultBufferSize : 0;
}

void nc_set_buffer_size(NCEngine* engine, int frames) {
    if (engine == nullptr) return;
    // 16..2048 frames covers the useful range; the audio device clamps to its own supported
    // set at start(), and nc_engine_status().requestedBufferSize reports what was granted.
    // Only takes effect at start(), so restart the engine (a brief dropout, like a device
    // or core-count change).
    const int clamped = std::max(16, std::min(2048, frames));
    if (clamped == engine->project.defaultBufferSize) return;
    engine->project.defaultBufferSize = clamped;
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

namespace {

/// Canonicalize a device identity to its stable UID, so a legacy numeric AudioObjectID
/// (persisted before device identity became the UID) heals to the UID on the next save.
/// An empty string (system default) and an unrecognized id pass through unchanged.
std::string canonicalDeviceIdentity(const std::string& identity) {
    if (identity.empty()) {
        return identity;
    }
    for (const auto& device : neuracoust::daw::enumerateAudioDevices()) {
        if (device.id == identity || device.uid == identity) {
            return device.id;   // the UID (see enumerateAudioDevices)
        }
    }
    return identity;
}

} // namespace

void nc_set_output_device(NCEngine* engine, const char* deviceId) {
    if (engine == nullptr) return;
    const std::string next = canonicalDeviceIdentity(deviceId != nullptr ? deviceId : "");
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
    const std::string next = canonicalDeviceIdentity(deviceId != nullptr ? deviceId : "");
    if (next == engine->inputDeviceId) {
        return;
    }
    engine->inputDeviceId = next;
    // The monitor input device only feeds the input queue, so swap it live rather than
    // restarting the whole engine — the master transport keeps playing while you A/B a
    // reference source (BlackHole). A restart would stop playback.
    engine->engine.setInputDeviceLive(next);
}

void nc_monitor_path_mode(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->monitorDspPathMode : std::string{});
}

void nc_monitor_set_path_mode(NCEngine* engine, const char* mode) {
    if (engine == nullptr || mode == nullptr) {
        return;
    }
    engine->monitorDspPathMode = mode;
    // Mirror the choice into the 모니터 assignment so the role table and the DSP-source row
    // never show two different answers. "auto" is the overflow policy, not a machine.
    if (engine->monitorDspPathMode == "auto") {
        engine->project.dspAutoOverflow = true;
    } else {
        engine->project.dspRoleMonitor = monitorRoleForPathMode(engine->monitorDspPathMode);
        engine->project.dspAutoOverflow = false;
    }
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

int nc_dsp_external_enabled(NCEngine* engine) {
    return (engine != nullptr && engine->project.externalDspEnabled) ? 1 : 0;
}

void nc_dsp_set_external_enabled(NCEngine* engine, int enabled) {
    if (engine == nullptr) return;
    const bool next = enabled != 0;
    if (next == engine->project.externalDspEnabled) return;
    engine->project.externalDspEnabled = next;
    engine->recordStep(next ? "Enable external DSP node" : "Disable external DSP node");
    // settings.enabled gates makeRemoteDspCorePlan, so re-apply the monitor path to take effect live.
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
}

namespace {

// Shared by both node-info entry points: turn a reachable server report into the C struct.
int fillNodeInfo(const neuracoust::daw::RemoteDspServerSettings& settings, NCRemoteNodeInfo* out) {
    const auto info = neuracoust::daw::queryRemoteDspServerInfo(settings);
    if (!info.reachable) return 0;
    const auto setField = [](char* dst, size_t cap, const std::string& s) {
        if (cap == 0) return;
        const size_t n = std::min(cap - 1, s.size());
        std::memcpy(dst, s.data(), n);
        dst[n] = '\0';
    };
    out->reachable = 1;
    out->roundTripMs = info.roundTripMs;
    setField(out->host, sizeof(out->host), settings.host);
    setField(out->model, sizeof(out->model), info.model);
    setField(out->cpuModel, sizeof(out->cpuModel), info.cpuModel.empty() ? std::string("unknown") : info.cpuModel);
    out->cpuMhz = info.cpuMhz;
    out->memoryMb = static_cast<int>(info.memoryMb);
    out->coreCount = static_cast<int>(info.coreCount);
    // The node reports a load per core; the monitor station wants one number, and the busiest core
    // is the one that decides whether the node can keep up.
    out->cpuLoadPercent = info.cpuCoreLoads.empty()
        ? -1.0
        : *std::max_element(info.cpuCoreLoads.begin(), info.cpuCoreLoads.end());
    out->temperatureC = info.temperatureC;
    out->packetsIn = info.packetsIn;
    out->packetsOut = info.packetsOut;
    out->badPackets = info.badPackets;
    return 1;
}

} // namespace

int nc_dsp_remote_node_info(NCEngine* engine, NCRemoteNodeInfo* out) {
    if (out == nullptr) return 0;
    std::memset(out, 0, sizeof(*out));
    if (engine == nullptr) return 0;
    return fillNodeInfo(buildRemoteDspSettings(engine), out);
}

int nc_dsp_probe_node_info(const char* host, int timeoutMs, NCRemoteNodeInfo* out) {
    if (out == nullptr) return 0;
    std::memset(out, 0, sizeof(*out));
    if (host == nullptr || *host == '\0') return 0;
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    settings.nodes.clear();          // the address given is the target, not a starting point
    settings.host = host;
    neuracoust::daw::applyRemoteDspHostPort(settings);   // "ip:port" probes a scratch instance
    settings.timeoutMs = timeoutMs > 0 ? timeoutMs : 150;
    return fillNodeInfo(settings, out);
}

void nc_net_ports_for_host(const char* host, char* out, size_t outLen) {
    copyText(out, outLen, "");
    if (host == nullptr || *host == '\0') return;

    // Strip a port suffix: the probe uses the status port, not whatever the caller addressed.
    std::string base = host;
    const auto lastColon = base.rfind(':');
    uint16_t statusPort = 20001;
    if (lastColon != std::string::npos && base.find(':') == lastColon) {
        const std::string portText = base.substr(lastColon + 1);
        base = base.substr(0, lastColon);
        const int parsed = std::atoi(portText.c_str());
        if (parsed > 0 && parsed < 65535) statusPort = static_cast<uint16_t>(parsed + 1);
    }

    std::string joined;
    for (const auto& port : neuracoust::daw::enumerateHostNetworkPorts(base, statusPort)) {
        if (!joined.empty()) joined += '\n';
        char line[192];
        std::snprintf(line, sizeof(line), "%s|%s|%.2f",
                      port.name.c_str(), port.address.c_str(), port.roundTripMs);
        joined += line;
    }
    copyText(out, outLen, joined);
}

void nc_dsp_preferred_interface(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.remoteDspInterface : std::string{});
}

void nc_dsp_set_preferred_interface(NCEngine* engine, const char* name) {
    if (engine == nullptr) return;
    const std::string next = name != nullptr ? name : "";
    if (engine->project.remoteDspInterface == next) return;
    engine->project.remoteDspInterface = next;
    // The binding lives on the socket, so the stream has to be rebuilt for this to take effect.
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->engine.resetRemoteMonitorDspStream();
    engine->recordStep("DSP 랜 포트");
}

void nc_dsp_reconnect_remote(NCEngine* engine) {
    if (engine == nullptr) return;
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->engine.resetRemoteMonitorDspStream();
}

int nc_dsp_probe_node_kind(const char* host, int timeoutMs, char* canonicalOut, size_t canonicalLen) {
    if (canonicalOut != nullptr && canonicalLen > 0) copyText(canonicalOut, canonicalLen, "");
    if (host == nullptr || *host == '\0') return 0;

    // Strip any explicit port so both generations can be tried. A single colon is "ip:port"; more
    // than one means an IPv6 literal, which carries no port here and is left alone.
    std::string base = host;
    const auto lastColon = base.rfind(':');
    if (lastColon != std::string::npos && base.find(':') == lastColon) {
        base = base.substr(0, lastColon);
    }

    NCRemoteNodeInfo info{};
    const std::string appliance = base + ":20002";
    if (nc_dsp_probe_node_info(appliance.c_str(), timeoutMs, &info) != 0) {
        copyText(canonicalOut, canonicalLen, appliance);
        return 2;
    }
    if (nc_dsp_probe_node_info(base.c_str(), timeoutMs, &info) != 0) {
        copyText(canonicalOut, canonicalLen, base);
        return 1;
    }
    return 0;
}

int nc_dsp_probe_node_audio(const char* host, int timeoutMs, int attempts) {
    if (host == nullptr || *host == '\0') return 0;
    const int rounds = attempts > 0 ? attempts : 8;
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    settings.nodes.clear();
    settings.host = host;
    neuracoust::daw::applyRemoteDspHostPort(settings);
    settings.enabled = true;
    settings.channelCount = 2;
    settings.timeoutMs = timeoutMs > 0 ? timeoutMs : 60;

    // One 256-frame stereo contribution — the shape the realtime stream itself sends, so a server
    // that would refuse the real traffic refuses this too. Silence is fine: what is being asked is
    // whether a block goes out and comes back, not what it sounds like.
    neuracoust::daw::RemoteMixSession session;
    const std::vector<std::vector<float>> contributions{std::vector<float>(512, 0.0f)};
    std::vector<float> summed;
    int carried = 0;
    for (int i = 0; i < rounds; ++i) {
        const auto result = session.mix(settings, contributions, summed);
        if (result.processed && summed.size() == contributions.front().size()) ++carried;
    }
    return carried;
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
namespace {
/// The four assignable roles, mapped to their field on the project. A name outside this set is
/// ignored rather than guessed at.
std::string* dspRoleField(NCEngine* engine, const char* role) {
    if (engine == nullptr || role == nullptr) return nullptr;
    const std::string name = role;
    if (name == "monitor") return &engine->project.dspRoleMonitor;
    if (name == "channelStrip") return &engine->project.dspRoleChannelStrip;
    if (name == "master") return &engine->project.dspRoleMaster;
    if (name == "inserts") return &engine->project.dspRoleInserts;
    if (name == "playback") return &engine->project.dspRolePlayback;
    if (name == "recording") return &engine->project.dspRoleRecording;
    if (name == "mixer") return &engine->project.dspRoleMixer;
    if (name == "buses") return &engine->project.dspRoleBuses;
    return nullptr;
}
}  // namespace

void nc_dsp_role(NCEngine* engine, const char* role, char* out, size_t outLen) {
    const auto* field = dspRoleField(engine, role);
    copyText(out, outLen, field != nullptr ? *field : std::string("internal"));
}

void nc_dsp_set_role(NCEngine* engine, const char* role, const char* machine) {
    auto* field = dspRoleField(engine, role);
    if (field == nullptr || machine == nullptr) return;
    const std::string next = machine;
    if (next != "internal" && next != "nds" && next != "external") return;
    if (*field == next) return;
    *field = next;
    // 재생·녹음 and 믹서·버스 are one row each in the UI — keep the paired field in lockstep so
    // the project never records a split the table cannot express.
    if (field == &engine->project.dspRolePlayback) {
        engine->project.dspRoleRecording = next;
    } else if (field == &engine->project.dspRoleMixer) {
        engine->project.dspRoleBuses = next;
    }
    // The monitor's assignment IS the monitor path mode — assigning it here is what makes the
    // row do something rather than describe something.
    if (field == &engine->project.dspRoleMonitor && !engine->project.dspAutoOverflow) {
        engine->monitorDspPathMode = monitorPathModeForRole(next);
    }
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    // And REBUILD the offload lists: the console strips and master stages are derived at
    // insert-chain prepare time, so without this a role change sat dormant until the next
    // unrelated edit happened to reconcile — the row looked live and did nothing.
    engine->reconcileProjectDeclicked();
    engine->recordStep("DSP role");
}

// Waves-style "Server Network Buffer": frames of wire buffering per remote stream. Small =
// tighter latency, large = more resilience to LAN jitter. Applied live the way the external
// core hint is — re-push the monitor path, then rebuild the offload lists so every stream
// picks the new size up.
int nc_dsp_network_buffer_frames(NCEngine* engine) {
    return engine != nullptr ? engine->project.remoteNetworkBufferFrames : 128;
}

void nc_dsp_set_network_buffer_frames(NCEngine* engine, int frames) {
    if (engine == nullptr) return;
    const int clamped = std::max(40, std::min(1024, frames));
    if (clamped == engine->project.remoteNetworkBufferFrames) return;
    engine->project.remoteNetworkBufferFrames = clamped;
    engine->recordStep("Server network buffer");
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();
}

// The latency mode (auto | mixing | tracking) and the tracking-mode buffer. In auto the streams
// re-buffer themselves when arming changes — see the arm/input-monitor setters.
void nc_dsp_latency_mode(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.remoteLatencyMode : std::string("auto"));
}

void nc_dsp_set_latency_mode(NCEngine* engine, const char* mode) {
    if (engine == nullptr || mode == nullptr) return;
    const std::string next = mode;
    if (next != "auto" && next != "mixing" && next != "tracking") return;
    if (engine->project.remoteLatencyMode == next) return;
    engine->project.remoteLatencyMode = next;
    engine->recordStep("Remote latency mode");
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();
}

int nc_dsp_tracking_buffer_frames(NCEngine* engine) {
    return engine != nullptr ? engine->project.remoteTrackingBufferFrames : 48;
}

void nc_dsp_set_tracking_buffer_frames(NCEngine* engine, int frames) {
    if (engine == nullptr) return;
    const int clamped = std::max(40, std::min(1024, frames));
    if (clamped == engine->project.remoteTrackingBufferFrames) return;
    engine->project.remoteTrackingBufferFrames = clamped;
    engine->recordStep("Tracking network buffer");
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();
}

// The SoundGrid configuration ladder (8/16/32/64): how many mixer input channels the remote
// summing sessions accept. A bus with more contributions than this stays local, bit-identically.
int nc_dsp_mixer_channels(NCEngine* engine) {
    return engine != nullptr ? engine->project.remoteMixerChannels : 32;
}

void nc_dsp_set_mixer_channels(NCEngine* engine, int channels) {
    if (engine == nullptr) return;
    const int clamped = channels >= 64 ? 64 : channels >= 32 ? 32 : channels >= 16 ? 16 : 8;
    if (clamped == engine->project.remoteMixerChannels) return;
    engine->project.remoteMixerChannels = clamped;
    engine->recordStep("Remote mixer channels");
    // The ladder is a live cap on the remote summing sessions now, not stored intent — push the
    // new settings so a bus over the new size falls back locally (bit-identical) at once.
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();
}

// The NDS POOL: appliances beyond the primary that carry console strips together
// (round-robin). Membership changes rebuild the offload lists through the declicked reconcile.
int nc_dsp_pool_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->project.ndsPoolHosts.size()) : 0;
}
void nc_dsp_pool_host(NCEngine* engine, int index, char* out, size_t outLen) {
    const bool valid = engine != nullptr && index >= 0 &&
        static_cast<size_t>(index) < engine->project.ndsPoolHosts.size();
    copyText(out, outLen, valid ? engine->project.ndsPoolHosts[static_cast<size_t>(index)] : std::string{});
}
bool nc_dsp_pool_contains(NCEngine* engine, const char* host) {
    if (engine == nullptr || host == nullptr) return false;
    const auto& pool = engine->project.ndsPoolHosts;
    return std::find(pool.begin(), pool.end(), std::string(host)) != pool.end();
}
void nc_dsp_pool_toggle(NCEngine* engine, const char* host) {
    if (engine == nullptr || host == nullptr || *host == '\0') return;
    auto& pool = engine->project.ndsPoolHosts;
    const auto found = std::find(pool.begin(), pool.end(), std::string(host));
    if (found != pool.end()) pool.erase(found);
    else pool.push_back(host);
    engine->recordStep(found != pool.end() ? "NDS pool" : "NDS pool");
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();
}

int nc_dsp_auto_overflow(NCEngine* engine) {
    return engine != nullptr && engine->project.dspAutoOverflow ? 1 : 0;
}

void nc_dsp_set_auto_overflow(NCEngine* engine, int enabled) {
    if (engine == nullptr) return;
    const bool next = enabled != 0;
    if (engine->project.dspAutoOverflow == next) return;
    engine->project.dspAutoOverflow = next;
    // Overflow on means "use whatever has room" — the engine's auto mode. Off restores the
    // monitor to the machine the user actually assigned it to.
    engine->monitorDspPathMode = next ? std::string("auto")
                                      : monitorPathModeForRole(engine->project.dspRoleMonitor);
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();   // rebuild the offload lists — see nc_dsp_set_role
    engine->recordStep("DSP overflow");
}

void nc_dsp_nds_host(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.ndsHost : std::string{});
}

void nc_dsp_set_nds_host(NCEngine* engine, const char* host) {
    if (engine == nullptr || host == nullptr) return;
    std::string next = host;
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    next.erase(next.begin(), std::find_if(next.begin(), next.end(), notSpace));
    next.erase(std::find_if(next.rbegin(), next.rend(), notSpace).base(), next.end());
    if (engine->project.ndsHost == next) return;
    engine->project.ndsHost = next;
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();   // availability feeds the offload lists
}

int nc_dsp_nds_enabled(NCEngine* engine) {
    return engine != nullptr && engine->project.ndsEnabled ? 1 : 0;
}

void nc_dsp_set_nds_enabled(NCEngine* engine, int enabled) {
    if (engine == nullptr) return;
    const bool next = enabled != 0;
    if (engine->project.ndsEnabled == next) return;
    engine->project.ndsEnabled = next;
    engine->engine.setMonitorDspPathMode(engine->monitorDspPathMode, buildRemoteDspSettings(engine));
    engine->reconcileProjectDeclicked();   // availability feeds the offload lists
}

namespace {

// Empty means "follow the project assignment"; anything else must name a real machine, so a
// typo cannot quietly become a route.
bool isDspMachineName(const std::string& machine) {
    return machine.empty() || machine == "internal" || machine == "nds" || machine == "external";
}

} // namespace

void nc_track_console_dsp_machine(NCEngine* engine, int trackIndex, char* out, size_t outLen) {
    const auto* track = trackAt(engine, trackIndex);
    copyText(out, outLen, track != nullptr ? track->consoleDspMachine : std::string{});
}

void nc_track_set_console_dsp_machine(NCEngine* engine, int trackIndex, const char* machine) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || machine == nullptr) return;
    const std::string next = machine;
    if (!isDspMachineName(next) || track->consoleDspMachine == next) return;
    track->consoleDspMachine = next;
    // Which machine runs a strip is a graph decision, so it needs a reconcile rather than the
    // lightweight value push a knob turn uses.
    engine->reconcileProject();
    engine->recordStep("Channel strip DSP");
}

void nc_track_insert_dsp_machine(NCEngine* engine, int trackIndex, int slot, char* out, size_t outLen) {
    const auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) {
        copyText(out, outLen, std::string{});
        return;
    }
    copyText(out, outLen, track->inserts[static_cast<size_t>(slot)].assignedDspServerId);
}

void nc_track_set_insert_dsp_machine(NCEngine* engine, int trackIndex, int slot, const char* machine) {
    auto* track = trackAt(engine, trackIndex);
    if (track == nullptr || machine == nullptr) return;
    if (slot < 0 || static_cast<size_t>(slot) >= track->inserts.size()) return;
    auto& insert = track->inserts[static_cast<size_t>(slot)];
    const std::string next = machine;
    if (!isDspMachineName(next) || insert.assignedDspServerId == next) return;
    insert.assignedDspServerId = next;
    engine->reconcileProject();
    engine->recordStep("Insert DSP");
}

// 서버 지터 자가진단. 두 종류의 교환을 연달아 보내 왕복 분포를 재고, 어느 쪽이 흔들리는지로
// 원인을 사람 말로 추정한다:
//   - 상태 핑(작은 패킷, 상태 포트): 경로 자체의 최소 변동.
//   - 오디오 블록 교환(256프레임 믹스, 오디오 포트): 실제 스트림이 겪는 변동.
// 작은 패킷은 안정한데 큰 패킷만 흔들리면 링크 속도/케이블/스위치 큐잉, 둘 다 흔들리면 공유
// 트래픽·절전, 드문 스파이크는 절전/백그라운드 전송/무선 구간, 손실은 케이블/커넥터 순으로
// 본다 — 진단은 추정이며 리포트에 근거 수치를 함께 적는다. 엔진 프리·블로킹(~1 s):
// 백그라운드 스레드에서 부를 것.
bool nc_remote_jitter_probe(const char* hostPort, char* out, size_t outLen) {
    if (out == nullptr || outLen == 0) return false;
    copyText(out, outLen, "");
    if (hostPort == nullptr || *hostPort == '\0') return false;
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    settings.nodes.clear();
    settings.host = hostPort;
    neuracoust::daw::applyRemoteDspHostPort(settings);
    settings.enabled = true;

    const auto quantiles = [](std::vector<double>& ms, double& p50, double& p95, double& peak) {
        std::sort(ms.begin(), ms.end());
        p50 = ms[ms.size() / 2];
        p95 = ms[std::min(ms.size() - 1, static_cast<size_t>(ms.size() * 95 / 100))];
        peak = ms.back();
    };

    // Small-packet baseline: status pings.
    std::vector<double> statusMs;
    statusMs.reserve(60);
    auto statusSettings = settings;
    statusSettings.timeoutMs = 50;
    for (int i = 0; i < 60; ++i) {
        const auto info = neuracoust::daw::queryRemoteDspServerInfo(statusSettings);
        if (info.reachable && info.roundTripMs > 0.0) statusMs.push_back(info.roundTripMs);
    }
    if (statusMs.size() < 30) {
        copyText(out, outLen, "노드가 상태 핑에 답하지 않습니다 — 주소/전원/케이블부터 확인하세요.");
        return true;
    }

    // Audio-sized exchanges: one-contribution mixes, the realtime stream's own packet shape.
    neuracoust::daw::RemoteMixSession session;
    auto mixSettings = settings;
    mixSettings.channelCount = 2;
    mixSettings.timeoutMs = 10;
    const std::vector<std::vector<float>> contributions{std::vector<float>(512, 0.0f)};
    std::vector<float> summed;
    std::vector<double> audioMs;
    audioMs.reserve(150);
    int misses = 0;
    for (int i = 0; i < 150; ++i) {
        const auto result = session.mix(mixSettings, contributions, summed);
        if (result.processed && result.roundTripMs > 0.0) audioMs.push_back(result.roundTripMs);
        else ++misses;
    }
    if (audioMs.size() < 50) {
        copyText(out, outLen,
                 "상태 핑은 통하는데 오디오 교환이 거의 실패합니다 — 오디오 포트(방화벽)나 엔진 상태를 확인하세요.");
        return true;
    }

    double statusP50 = 0, statusP95 = 0, statusMax = 0;
    double audioP50 = 0, audioP95 = 0, audioMax = 0;
    quantiles(statusMs, statusP50, statusP95, statusMax);
    quantiles(audioMs, audioP50, audioP95, audioMax);
    const double statusJitterUs = (statusP95 - statusP50) * 1000.0;
    const double audioJitterUs = (audioP95 - audioP50) * 1000.0;
    const double spikeRatio = audioP50 > 0.0 ? audioMax / audioP50 : 1.0;
    const double missRate = 150.0 > 0 ? static_cast<double>(misses) / 150.0 : 0.0;

    std::string verdict;
    if (missRate > 0.05) {
        verdict = "패킷 손실이 보입니다 → 케이블/커넥터/포트 불량을 가장 먼저 점검하세요.";
    } else if (audioJitterUs < 150.0) {
        verdict = "정상 — 유선 기가비트 직결 수준입니다. 지금 배선을 유지하세요.";
    } else if (statusJitterUs < 150.0 && audioJitterUs >= 400.0) {
        verdict = "큰(오디오) 패킷에서만 변동 → 링크 속도(100 Mbps 협상)·케이블 등급·스위치 큐잉 의심. "
                  "기가비트 확인 및 DAW-노드 직결(또는 같은 스위치)로 좁혀 보세요.";
    } else if (spikeRatio > 8.0 && audioJitterUs < 400.0) {
        verdict = "평소엔 안정한데 간헐 스파이크 → 절전(EEE/링크 절전)·백그라운드 전송·무선 구간 의심.";
    } else {
        verdict = "작은 패킷까지 함께 변동 → 경로 공유 트래픽(허브/스위치에 물린 다른 장비)이나 "
                  "노드 쪽 스케줄링 의심. 스위치에서 다른 장비를 빼고 재측정해 보세요.";
    }

    char report[1024];   // Korean UTF-8 runs ~3 bytes/char; NC_TEXT_LEN truncated the numbers line
    std::snprintf(report, sizeof(report),
                  "%s\n오디오 왕복 p50 %.2f ms · p95 %.2f ms · 최대 %.2f ms · 지터 %.0f µs · 손실 %d/150\n"
                  "상태 핑 p50 %.2f ms · 지터 %.0f µs",
                  verdict.c_str(), audioP50, audioP95, audioMax, audioJitterUs, misses,
                  statusP50, statusJitterUs);
    copyText(out, outLen, report);
    return true;
}

// SoundGrid-style inventory scan: broadcast on both status-port generations and return EVERY
// server that answers — appliance engines (20003) normalized to their host:20002 audio address,
// legacy remote_core_servers (20001) as plain hosts. Engine-free and blocking (~1 s): call it
// from a background thread; the UI turns the result into a pick-to-connect list, which is the
// whole point — a server is something you SELECT, not an IP you transcribe.
void nc_dsp_scan_lan(char* out, size_t outLen) {
    copyText(out, outLen, "");
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    settings.nodes.clear();
    std::set<std::string> seen;
    // One machine with two NICs answers the broadcast on BOTH, and listing it twice reads as two
    // DSP servers — the inventory then invites assigning work to a second machine that does not
    // exist. The name a node reports for itself is the identity; the address is only a way to
    // reach it. 20003 is swept first, so an appliance engine outranks the legacy server on the
    // same box; between two addresses of one machine a routable one beats a self-assigned
    // 169.254, which changes on every replug.
    std::vector<std::string> order;                        // identities, in the order they answered
    std::map<std::string, std::string> chosen;             // identity -> address to show
    const auto isLinkLocal = [](const std::string& address) {
        return address.rfind("169.254.", 0) == 0;
    };
    for (const uint16_t statusPort : {uint16_t(20003), uint16_t(20001)}) {
        settings.statusPort = statusPort;
        for (const auto& found : neuracoust::daw::discoverRemoteDspServers(settings, {}, 500)) {
            if (found.node.host.empty()) continue;
            // Hand back the name the node answers to, not the number it happens to hold: over a
            // direct cable or an audio hub that number is a self-assigned 169.254 address and it
            // changes on every replug, so a saved one stops working the next time the box is
            // plugged in. preferredNodeAddress only substitutes a name that resolves back here.
            const std::string stable =
                neuracoust::daw::preferredNodeAddress(found.node.host, found.info.hostname);
            const std::string address = statusPort == 20003 ? stable + ":20002" : stable;
            if (!seen.insert(address).second) continue;
            const std::string identity =
                found.info.hostname.empty() ? address : found.info.hostname;
            const auto existing = chosen.find(identity);
            if (existing == chosen.end()) {
                order.push_back(identity);
                chosen.emplace(identity, address);
            } else if (isLinkLocal(existing->second) && !isLinkLocal(address)) {
                existing->second = address;
            }
        }
    }
    std::string joined;
    for (const auto& identity : order) {
        if (!joined.empty()) joined += '\n';
        joined += chosen[identity];
    }
    copyText(out, outLen, joined);
}

void nc_dsp_discover_remote_host(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, std::string{});
    auto settings = engine != nullptr ? buildRemoteDspSettings(engine)
                                      : neuracoust::daw::defaultRemoteDspServerSettings();
    // Two generations of status port: the appliance engine answers on 20003, the legacy
    // remote_core_server on 20001. Probe both; an appliance answer wins and carries its port.
    for (const uint16_t statusPort : {uint16_t(20003), settings.statusPort}) {
        settings.statusPort = statusPort;
        for (const auto& found : neuracoust::daw::discoverRemoteDspServers(settings, {}, 400)) {
            if (!found.node.host.empty()) {
                // Same rule as the scan: prefer the node's own name so the setting survives a replug.
                const std::string stable =
                    neuracoust::daw::preferredNodeAddress(found.node.host, found.info.hostname);
                copyText(out, outLen, statusPort == 20003 ? stable + ":20002" : stable);
                return;
            }
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
    engine->project.monitorVolumeDb = std::max(-60.0f, std::min(-12.0f, db));   // -12 dB max: speaker-sim headroom
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
// How far Dim pulls the monitor down, in dB (negative). Applied live.
float nc_monitor_dim_db(NCEngine* engine) {
    return engine != nullptr ? engine->project.monitorStationDimDb : -20.0f;
}
void nc_monitor_set_dim_db(NCEngine* engine, float db) {
    if (engine == nullptr) return;
    if (db > 0.0f) db = -db;                 // dim is an attenuation
    if (db < -80.0f) db = -80.0f;
    if (engine->project.monitorStationDimDb == db) return;
    engine->project.monitorStationDimDb = db;
    engine->pushStationControls();
}

// Master (false, default) vs the computer's input source (true) for the monitor bus.
bool nc_monitor_listen_source(NCEngine* engine) {
    return engine != nullptr && engine->monitorListenSource;
}

static void applyReferenceTapRun(NCEngine* engine);   // defined below

void nc_monitor_set_listen_source(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->monitorListenSource = on;
    engine->engine.setMonitorListenSource(on);
    // Re-run the reference-tap policy after the listen source changes. On the FIRST "다른 앱" press
    // right after launch, arming started the tap while the listen source was still off and the
    // process-tap / aggregate device isn't fully up yet — so the first press was silent until a
    // master↔다른앱 toggle re-applied this. Re-confirming the tap here makes the first press work.
    applyReferenceTapRun(engine);
}

// Reference-hold: while armed the process tap runs and the tapped apps are muted at their own
// output, so A/B-ing between the master and the reference never leaks their sound out of the
// computer. Disarming (on == false) also clears the listening state.
bool nc_monitor_reference_armed(NCEngine* engine) {
    return engine != nullptr && engine->monitorReferenceArmed;
}

// The tap runs (apps muted) if EITHER the "다른 앱" button armed it OR a tap-input track is capturing
// — so turning one off never stops the tap the other still needs.
static void applyReferenceTapRun(NCEngine* engine) {
    engine->engine.setMonitorReferenceArmed(
        engine->monitorReferenceArmed || engine->tapCaptureActive || engine->tapInputHold);
}

void nc_monitor_set_reference_armed(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->monitorReferenceArmed = on;
    if (!on) {
        engine->monitorListenSource = false;
        engine->engine.setMonitorListenSource(false);   // leave A/B listening explicitly
    }
    applyReferenceTapRun(engine);
}

// AUDIBLE tap input-monitor: heard on the master only while punched in (Record active), auto-input
// style — on plain playback you hear the recorded tape, not the live tap. Does NOT run the tap.
void nc_monitor_set_tap_input_monitor(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->engine.setTapInputMonitor(on);
}

// A tap-input track's Input-Monitor toggle is on/off: run the tap and hear it continuously (Input
// mode, as opposed to the auto-input punch above). ORs into the tap-run so the apps stay muted.
void nc_monitor_set_tap_input_hold(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    engine->tapInputHold = on;
    engine->engine.setTapInputHold(on);
    applyReferenceTapRun(engine);
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

// Talkback destination: "monitor_bus" (engineer's speakers), "listen_room" (remote
// listeners only, dry local monitor — the default), or "all" (both).
void nc_monitor_talkback_route(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.monitorStationTalkbackRoute : std::string{});
}

void nc_monitor_set_talkback_route(NCEngine* engine, const char* route) {
    if (engine == nullptr || route == nullptr) return;
    std::string value = route;
    if (value != "monitor_bus" && value != "listen_room" && value != "all") {
        value = "listen_room";
    }
    engine->project.monitorStationTalkbackRoute = value;
    engine->pushStationControls();
}

// Talkback mic input channel (1-based): which physical input the talkback mic is on. A talkback
// mic is mono; the engine captures just this channel and centers it, so a talkback mic on ch2
// (ch1 = singer) is heard clean on both speakers/listeners and does not pull the singer in.
int nc_monitor_talkback_channel(NCEngine* engine) {
    return engine != nullptr ? std::max(1, engine->project.monitorStationTalkbackChannel) : 1;
}
void nc_monitor_set_talkback_channel(NCEngine* engine, int oneBased) {
    if (engine == nullptr) return;
    engine->project.monitorStationTalkbackChannel = std::max(1, oneBased);
    engine->engine.setTalkbackInputChannel(engine->project.monitorStationTalkbackChannel);
}

// Number of physical input channels on the talkback device (native width), for the channel picker.
int nc_talkback_channel_count(NCEngine* engine) {
    return engine != nullptr ? std::max(1, engine->engine.selectedInputChannelCount()) : 1;
}
// Live decayed peak (0..1) of a physical input channel — lets the picker show which mics are live.
// Reads 0 when the input queue is idle (no monitoring/talkback/measurement running).
float nc_talkback_channel_activity(NCEngine* engine, int oneBased) {
    return engine != nullptr ? engine->engine.inputChannelActivity(std::max(1, oneBased)) : 0.0f;
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

// Swap left/right in the monitor path — for when the speakers are wired backwards. Meaningless
// (and forced off) in Mid/Side, but preserved across the normal stereo/mono listen modes.
bool nc_monitor_swap_left_right(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorStationSwapLeftRight;
}
void nc_monitor_toggle_swap_left_right(NCEngine* engine) {
    if (engine == nullptr) return;
    auto& p = engine->project;
    p.monitorStationSwapLeftRight = !p.monitorStationSwapLeftRight;
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
std::string* speakerAmpFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.powerAmpB : slot == 2 ? &m.powerAmpC : &m.powerAmpA;
}
std::string* speakerRealModelFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.realModelB : slot == 2 ? &m.realModelC : &m.realModelA;
}
std::string* speakerCableFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.speakerCableB : slot == 2 ? &m.speakerCableC : &m.speakerCableA;
}
std::string* speakerRealAmpFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.realAmpB : slot == 2 ? &m.realAmpC : &m.realAmpA;
}
std::string* speakerRealCableFieldForSlot(MonitorDspModule& m, int slot) {
    return slot == 1 ? &m.realCableB : slot == 2 ? &m.realCableC : &m.realCableA;
}

// The speaker-model catalog, ported from the old UI's speakerModelBaseCatalog(). The
// (NF/MF/LF) suffix is the field category. The name drives the monitor tone model.
const std::vector<std::string>& speakerModelCatalog() {
    static const std::vector<std::string> models = {
        "Flat",
        "Yamaha NS-10 (NF)", "Yamaha NS-10M (NF)", "Yamaha NS-10M Pro (NF)", "Yamaha NS-10M Studio (NF)", "Yamaha HS3 (NF)", "Yamaha HS4 (NF)", "Yamaha HS5 (NF)", "Yamaha HS7 (NF)", "Yamaha HS8 (NF)", "Yamaha MSP3A (NF)", "Yamaha MSP5 Studio (NF)", "Yamaha MSP7 Studio (NF)",
        "Auratone 5C Sound Cube (NF)", "Avantone Pro MixCube Active (NF)", "Avantone Pro MixCube Passive (NF)", "Avantone Pro CLA-10 Passive (NF)", "Avantone Pro CLA-10 Active (NF)", "Avantone Pro CLA-10A (NF)", "Avantone Pro CLA-10A Limited Edition (NF)", "Avantone Pro Gauss 7 (NF)",
        "Genelec 8010A (NF)", "Genelec 8020D (NF)", "Genelec 8030C (NF)", "Genelec 8040B (NF)", "Genelec 8050B (NF)", "Genelec 8320A (NF)", "Genelec 8330A (NF)", "Genelec 8331A (NF)", "Genelec 8341A (MF)", "Genelec 8351B (MF)", "Genelec 8361A (MF)", "Genelec S360A (MF)", "Genelec 1030A (NF)", "Genelec 1031A (MF)", "Genelec 1032A (MF)", "Genelec 1037C (LF)", "Genelec 1038C (LF)", "Genelec M040 (NF)",
        "Neumann KH 80 DSP (NF)", "Neumann KH 120 II (NF)", "Neumann KH 150 (NF)", "Neumann KH 310 (MF)", "Neumann KH 420 (MF)",
        "ADAM D3V (NF)", "ADAM T5V (NF)", "ADAM T7V (NF)", "ADAM T8V (NF)", "ADAM A3X (NF)", "ADAM A4V (NF)", "ADAM A44H (NF)", "ADAM A5X (NF)", "ADAM A7V (NF)", "ADAM A7X (NF)", "ADAM A77H (MF)", "ADAM A8H (MF)", "ADAM S2V (NF)", "ADAM S3V (MF)", "ADAM S3H (MF)", "ADAM S5V (LF)", "ADAM S5H (LF)", "ADAM S6X (LF)",
        "Focal Alpha 50 Evo (NF)", "Focal Alpha 65 Evo (NF)", "Focal Alpha Twin Evo (MF)", "Focal Shape 40 (NF)", "Focal Shape 50 (NF)", "Focal Shape 65 (NF)", "Focal Solo6 Be (NF)", "Focal Solo6 ST6 (NF)", "Focal Twin6 Be (MF)", "Focal Twin6 ST6 (MF)", "Focal Trio6 Be (MF)", "Focal Trio6 ST6 (MF)", "Focal Trio11 Be (MF)", "Focal SM9 (MF)", "Focal Grande Utopia EM (LF)",
        "Dynaudio BM5A (NF)", "Dynaudio BM6A (NF)", "Dynaudio BM15A (MF)", "Dynaudio LYD 5 (NF)", "Dynaudio LYD 7 (NF)", "Dynaudio LYD 8 (NF)", "Dynaudio LYD 48 (MF)", "Dynaudio Core 5 (NF)", "Dynaudio Core 7 (NF)", "Dynaudio Core 47 (MF)", "Dynaudio Core 59 (MF)", "Dynaudio M3VE (LF)",
        "KRK 9000B (NF)", "KRK Rokit 5 G4 (NF)", "KRK Rokit 7 G4 (NF)", "KRK Rokit 8 G4 (NF)", "KRK V4 (NF)", "KRK V6 (NF)", "KRK V8 (NF)", "KRK Expose E8B (MF)",
        "JBL 305P MkII (NF)", "JBL 306P MkII (NF)", "JBL 308P MkII (NF)", "JBL 705P (NF)", "JBL 708P (MF)", "JBL 4312 (MF)", "JBL 4329P (MF)", "JBL LSR6328P (MF)", "JBL M2 (LF)",
        "Mackie HR624 (NF)", "Mackie HR824 (NF)", "PreSonus Eris E5 (NF)", "PreSonus Eris E5 XT (NF)", "PreSonus Eris E8 (NF)", "Kali LP-6 (NF)", "Kali LP-6v2 (NF)", "Kali LP-8 (NF)", "Kali IN-5 (NF)", "Kali IN-8 (MF)", "Kali LP-UNF (NF)", "Kali SM-5 (NF)",
        "EVE Audio SC205 (NF)", "EVE Audio SC207 (NF)", "EVE Audio SC307 (MF)", "HEDD Type 05 MK2 (NF)", "HEDD Type 07 MK2 (NF)", "HEDD Type 20 MK2 (MF)", "HEDD Type 30 MK2 (MF)",
        "Amphion One12 (NF)", "Amphion One15 (NF)", "Amphion One18 (NF)", "Amphion One25A (MF)", "Amphion Two15 (MF)", "Amphion Two18 (MF)",
        "ATC SCM12 Pro (NF)", "ATC SCM20ASL Pro (NF)", "ATC SCM25A Pro (MF)", "ATC SCM25A (MF)", "ATC SCM45A Pro (MF)", "ATC SCM45A (MF)", "ATC SCM50ASL Pro (MF)", "ATC SCM50A (MF)", "ATC SCM100ASL Pro (LF)", "ATC SCM100A (LF)", "ATC SCM150ASL Pro (LF)",
        "PMC Result6 (NF)", "PMC twotwo.5 (NF)", "PMC twotwo.6 (NF)", "PMC twotwo.8 (MF)", "PMC 6 (NF)", "PMC 6-2 (MF)", "PMC 8-2 (MF)", "PMC IB1S-AIII (MF)", "PMC MB2S XBD (LF)", "PMC BB6 XBD (LF)",
        "Barefoot Footprint01 (MF)", "Barefoot Footprint02 (MF)", "Barefoot Footprint03 (NF)", "Barefoot MicroMain26 (MF)", "Barefoot MicroMain27 (MF)", "Barefoot MicroMain45 (MF)", "Barefoot MiniMain12 (LF)", "Barefoot MasterStack12 (LF)",
        "Quested S7R (NF)", "Quested V2108 (MF)", "Quested VH3208 (LF)", "Ocean Way HR5 (MF)", "Ocean Way HR4 (MF)", "Ocean Way HR3 (LF)", "Ocean Way HR2 (LF)", "Augspurger Duo 8 (MF)", "Augspurger Duo 12 (LF)", "Augspurger Duo 15 (LF)", "Meyer Sound HD-1 (NF)", "Meyer Sound Amie (MF)", "Meyer Sound Bluehorn (LF)",
        "Kii THREE (MF)", "Dutch & Dutch 8c (MF)", "GGNTKT M1 (MF)", "PSI Audio A17-M (NF)", "PSI Audio A21-M (MF)", "PSI Audio A25-M (MF)", "Manger P1 (MF)", "Unity Audio The Rock MkII (NF)", "Unity Audio Boulder MkIII (MF)",
        "Klein + Hummel O 300 (MF)", "Tannoy Gold 5 (NF)", "Tannoy Gold 8 (NF)", "Tannoy Reveal 502 (NF)", "Tannoy Reveal 802 (NF)", "Tannoy System 600 (NF)", "Tannoy System 800 (MF)", "Tannoy Profile 638 Black Ash Plus (MF)", "Westlake BBSM-10 (MF)", "Westlake BBSM-15 (LF)",
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
        "Sennheiser HD 600", "Sennheiser HD 650", "Sennheiser HD 800S", "Sennheiser HD 25", "Sennheiser HD 280 Pro",
        "Sennheiser HD 560S", "Sennheiser HD 660 S", "Sennheiser HD 660S2", "Sennheiser HD 620S", "Sennheiser HD 800", "Sennheiser HD 820",
        "Beyerdynamic DT 770 Pro", "Beyerdynamic DT 880 Pro", "Beyerdynamic DT 990 Pro", "Beyerdynamic DT 1990 Pro", "Beyerdynamic DT 700 Pro X", "Beyerdynamic DT 900 Pro X",
        "AKG K240 Studio", "AKG K271 MkII", "AKG K361", "AKG K371", "AKG K701", "AKG K702", "AKG K712 Pro",
        "Audio-Technica ATH-M20x", "Audio-Technica ATH-M40x", "Audio-Technica ATH-M50x", "Audio-Technica ATH-M60x", "Audio-Technica ATH-M70x", "Audio-Technica ATH-R70x",
        "Sony MDR-7506", "Sony MDR-CD900ST", "Sony MDR-MV1",
        "Focal Listen Pro", "Focal Clear", "Focal Clear Mg", "Focal Utopia",
        "Audeze LCD-2 Classic", "Audeze LCD-X", "Audeze LCD-XC", "Audeze MM-100", "Audeze MM-500",
        "HIFIMAN Sundara", "HIFIMAN Ananda", "HIFIMAN Edition XS", "HIFIMAN HE400se",
        "Shure SRH440", "Shure SRH840A", "Shure SRH1540", "Shure SRH1840",
        "Austrian Audio Hi-X60", "Dan Clark Audio E3", "Dan Clark Audio Stealth", "Fostex TH900mk2",
        "Grado SR325x", "Neumann NDH 20", "Neumann NDH 30",
        "Apple AirPods Max", "Sony WH-1000XM5",
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

namespace {
// Power amplifiers driving a passive speaker. "None" is the default (unset).
const std::vector<std::string>& powerAmpModelCatalog() {
    static const std::vector<std::string> models = [] {
        std::vector<std::string> out;
        for (const auto& spec : neuracoust::daw::powerAmpCatalogSpecs()) out.emplace_back(spec.name);
        return out;
    }();
    return models;
}
// Speaker cable between a power amp and a passive speaker.
const std::vector<std::string>& speakerCableModelCatalog() {
    static const std::vector<std::string> models = [] {
        std::vector<std::string> out;
        for (const auto& spec : neuracoust::daw::speakerCableCatalogSpecs()) out.emplace_back(spec.name);
        return out;
    }();
    return models;
}
const std::vector<std::string>& powerCableModelCatalog() {
    static const std::vector<std::string> models = {
        "None", "Standard IEC C13 18 AWG", "Standard IEC C13 14 AWG",
        "Shielded IEC C13 14 AWG", "Neutrik powerCON TRUE1 TOP lead",
        "Furutech IEC power cord", "Oyaide IEC power cord",
    };
    return models;
}
const std::vector<std::string>& connectorModelCatalog() {
    static const std::vector<std::string> models = {
        "None", "IEC 60320 C13/C14", "IEC 60320 C19/C20", "Neutrik powerCON TRUE1 TOP",
        "Neutrik speakON NL4", "5-way binding post", "1/4 inch TS speaker plug",
        "Furutech IEC connector", "Oyaide IEC connector",
    };
    return models;
}
// Measurement microphones for room measurement. Those with a per-unit or model calibration
// file support ABSOLUTE tone correction; the rest are only trustworthy for L/R matching and
// relative correction (their own coloration can't be removed).
const std::vector<std::string>& measurementMicCatalog() {
    static const std::vector<std::string> models = {
        "미선택",
        "miniDSP UMIK-1", "miniDSP UMIK-2",
        "Dayton Audio EMM-6", "Dayton Audio UMM-6",
        "Sonarworks SoundID XREF20", "iSEMcon EMX-7150", "Earthworks M23R", "Earthworks M30",
        "Behringer ECM8000 (캘 파일 있음)", "Behringer ECM8000 (캘 없음)",
        "Superlux ECM999", "Shure SM57 (측정용 아님)", "일반 측정 마이크 (캘 없음)",
    };
    return models;
}
// True if the mic carries a calibration file, so absolute tone correction is reliable.
bool measurementMicHasCalibration(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("캘 없음") != std::string::npos || lower.find("측정용 아님") != std::string::npos ||
        lower.find("일반 측정") != std::string::npos || lower.find("미선택") != std::string::npos ||
        lower.empty()) {
        return false;
    }
    static const std::vector<std::string> calibrated = {
        "umik", "umm-6", "emm-6", "xref", "emx-7150", "m23r", "m30", "earthworks", "캘 파일 있음",
    };
    for (const auto& c : calibrated) if (lower.find(c) != std::string::npos) return true;
    return false;
}

// Passive speakers need an external amp + cable; active/powered monitors have the amp built in.
bool speakerModelIsPassive(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("passive") != std::string::npos) return true;
    if (lower.find("active") != std::string::npos) return false;   // explicitly powered
    // Curated classic passives that famously run off a separate amplifier.
    static const std::vector<std::string> passive = {
        "yamaha ns-10", "auratone 5c", "jbl 4312", "profile 638", "mercury 638", "638 black ash",
        "westlake bbsm", "tannoy system",
    };
    for (const auto& p : passive) if (lower.find(p) != std::string::npos) return true;
    return false;   // default: an active studio monitor
}
} // namespace

int nc_power_amp_model_count() { return static_cast<int>(powerAmpModelCatalog().size()); }
void nc_power_amp_model_name(int index, char* out, size_t outLen) {
    const auto& c = powerAmpModelCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < c.size()) ? c[static_cast<size_t>(index)] : std::string{});
}
int nc_speaker_cable_model_count() { return static_cast<int>(speakerCableModelCatalog().size()); }
void nc_speaker_cable_model_name(int index, char* out, size_t outLen) {
    const auto& c = speakerCableModelCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < c.size()) ? c[static_cast<size_t>(index)] : std::string{});
}
int nc_power_cable_model_count() { return static_cast<int>(powerCableModelCatalog().size()); }
void nc_power_cable_model_name(int index, char* out, size_t outLen) {
    const auto& c = powerCableModelCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < c.size()) ? c[static_cast<size_t>(index)] : std::string{});
}
int nc_connector_model_count() { return static_cast<int>(connectorModelCatalog().size()); }
void nc_connector_model_name(int index, char* out, size_t outLen) {
    const auto& c = connectorModelCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < c.size()) ? c[static_cast<size_t>(index)] : std::string{});
}

int nc_measurement_mic_model_count() { return static_cast<int>(measurementMicCatalog().size()); }
void nc_measurement_mic_model_name(int index, char* out, size_t outLen) {
    const auto& c = measurementMicCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < c.size()) ? c[static_cast<size_t>(index)] : std::string{});
}
bool nc_measurement_mic_has_calibration(const char* name) {
    return name != nullptr && measurementMicHasCalibration(name);
}
void nc_measurement_mic_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.measurementMicModel : std::string{});
}
void nc_set_measurement_mic_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr) return;
    if (engine->project.measurementMicModel == model) return;
    engine->project.measurementMicModel = model;
    engine->recordStep("Set measurement mic");
}
bool nc_speaker_model_is_passive(const char* name) {
    return name != nullptr && speakerModelIsPassive(name);
}

void nc_monitor_physical_power_amp_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalPowerAmpModel : std::string{});
}
void nc_monitor_set_physical_power_amp_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr) return;
    if (engine->project.physicalPowerAmpModel == model) return;
    engine->project.physicalPowerAmpModel = model;
    engine->recordStep("Set power amp");
}
void nc_monitor_physical_speaker_cable_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalSpeakerCableModel : std::string{});
}
void nc_monitor_set_physical_speaker_cable_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr) return;
    if (engine->project.physicalSpeakerCableModel == model) return;
    engine->project.physicalSpeakerCableModel = model;
    engine->recordStep("Set speaker cable");
}
void nc_monitor_physical_power_cable_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalPowerCableModel : std::string{});
}
void nc_monitor_set_physical_power_cable_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr || engine->project.physicalPowerCableModel == model) return;
    engine->project.physicalPowerCableModel = model;
    engine->recordStep("Set power cable");
}
void nc_monitor_physical_connector_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalConnectorModel : std::string{});
}
void nc_monitor_set_physical_connector_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr || engine->project.physicalConnectorModel == model) return;
    engine->project.physicalConnectorModel = model;
    engine->recordStep("Set connector");
}

// Audio-interface D/A output-stage model — catalog + measurement status only (no audio effect yet).
int nc_audio_interface_model_count() { return static_cast<int>(neuracoust::daw::audioInterfaceModelCatalog().size()); }
void nc_audio_interface_model_name(int index, char* out, size_t outLen) {
    const auto& c = neuracoust::daw::audioInterfaceModelCatalog();
    copyText(out, outLen, (index >= 0 && static_cast<size_t>(index) < c.size()) ? c[static_cast<size_t>(index)] : std::string{});
}
bool nc_audio_interface_model_measured(const char* name) {
    return name != nullptr && neuracoust::daw::audioInterfaceModelMeasured(name);
}
void nc_monitor_physical_audio_interface_model(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalAudioInterfaceModel : std::string{});
}
void nc_monitor_set_physical_audio_interface_model(NCEngine* engine, const char* model) {
    if (engine == nullptr || model == nullptr) return;
    if (engine->project.physicalAudioInterfaceModel == model) return;
    engine->project.physicalAudioInterfaceModel = model;
    engine->pushInterfaceModeler();
    engine->recordStep("Set audio interface output-stage model");
}
void nc_monitor_physical_audio_interface_target(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.physicalAudioInterfaceTargetModel : std::string{});
}
void nc_monitor_set_physical_audio_interface_target(NCEngine* engine, const char* model) {
    if (engine == nullptr) return;
    const std::string next = model != nullptr ? model : "";
    if (engine->project.physicalAudioInterfaceTargetModel == next) return;
    engine->project.physicalAudioInterfaceTargetModel = next;
    engine->pushInterfaceModeler();
    engine->recordStep("Set audio interface simulate-as target");
}
// Optional 2단계 harmonic modeling toggle.
bool nc_monitor_interface_modeling_enabled(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorInterfaceModelingEnabled;
}
void nc_monitor_set_interface_modeling_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->project.monitorInterfaceModelingEnabled == enabled) return;
    engine->project.monitorInterfaceModelingEnabled = enabled;
    engine->pushInterfaceModeler();
    engine->recordStep(enabled ? "Enable interface harmonic modeling" : "Disable interface harmonic modeling");
}
// Whether the A->B "render as another interface" transform actually touches audio. TRUE when the
// modeling TARGET has a measured FR profile — its coloration is then applied (+) on top of the
// output-stage compensation. Summary specs / the "measured" badge never qualify (that would be
// fabricated DSP, forbidden by claude_handoff.md #2/#5); only a bundled measured curve does.
bool nc_audio_interface_transform_active(NCEngine* engine) {
    if (engine == nullptr) return false;
    // Live measurement OR baked profile on the target both make the A→B transform real.
    return !engine->project.physicalAudioInterfaceTargetModel.empty()
        && !engine->interfaceCurveFor(engine->project.physicalAudioInterfaceTargetModel).empty();
}

// --- Monitor parametric EQ (0–64 bands, added on demand; monitor path only) ---
namespace {
std::string sanitizeEqType(const char* type) {
    const std::string t = type != nullptr ? type : "";
    if (t == "low_shelf" || t == "high_shelf" || t == "high_pass" ||
        t == "low_pass" || t == "notch" || t == "peaking") {
        return t;
    }
    return "peaking";
}
} // namespace

int nc_monitor_eq_band_count(NCEngine* engine) {
    return engine != nullptr ? static_cast<int>(engine->project.monitorEqBands.size()) : 0;
}

bool nc_monitor_eq_band(NCEngine* engine, int index, bool* enabled, char* typeOut, size_t typeLen,
                        double* freq, double* gain, double* q) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.monitorEqBands.size()) {
        return false;
    }
    const auto& band = engine->project.monitorEqBands[static_cast<size_t>(index)];
    if (enabled != nullptr) *enabled = band.enabled;
    if (typeOut != nullptr) copyText(typeOut, typeLen, band.type);
    if (freq != nullptr) *freq = band.frequencyHz;
    if (gain != nullptr) *gain = band.gainDb;
    if (q != nullptr) *q = band.q;
    return true;
}

int nc_monitor_eq_add_band(NCEngine* engine, const char* type, double freq, double gain, double q) {
    if (engine == nullptr || engine->project.monitorEqBands.size() >= 64) return -1;
    neuracoust::daw::MonitorEqBandState band;
    band.enabled = true;
    band.type = sanitizeEqType(type);
    band.frequencyHz = std::max(10.0, std::min(40000.0, freq));
    band.gainDb = std::max(-30.0, std::min(30.0, gain));
    band.q = std::max(0.05, std::min(40.0, q));
    engine->project.monitorEqBands.push_back(band);
    engine->reconcileProject();
    engine->recordStep("Add EQ band");
    return static_cast<int>(engine->project.monitorEqBands.size()) - 1;
}

bool nc_monitor_eq_set_band(NCEngine* engine, int index, bool enabled, const char* type,
                            double freq, double gain, double q) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.monitorEqBands.size()) {
        return false;
    }
    auto& band = engine->project.monitorEqBands[static_cast<size_t>(index)];
    band.enabled = enabled;
    band.type = sanitizeEqType(type);
    band.frequencyHz = std::max(10.0, std::min(40000.0, freq));
    band.gainDb = std::max(-30.0, std::min(30.0, gain));
    band.q = std::max(0.05, std::min(40.0, q));
    // Lightweight: push ONLY the EQ, not a full graph reconcile — a frequency drag calls this
    // 30–60×/s and reconcileProject() per frame gapped the audio. No undo step (one gesture).
    engine->engine.updateMonitorEq(engine->project.monitorEqBands);
    return true;
}

bool nc_monitor_eq_remove_band(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0 ||
        static_cast<size_t>(index) >= engine->project.monitorEqBands.size()) {
        return false;
    }
    engine->project.monitorEqBands.erase(engine->project.monitorEqBands.begin() + index);
    engine->reconcileProject();
    engine->recordStep("Remove EQ band");
    return true;
}

void nc_monitor_eq_clear(NCEngine* engine) {
    if (engine == nullptr || engine->project.monitorEqBands.empty()) return;
    engine->project.monitorEqBands.clear();
    engine->reconcileProject();
    engine->recordStep("Clear EQ");
}

// --- Virtual monitor (④): model a target speaker on the physical monitor ---
// Fits the target's measured (flat-relative) curve to EQ bands and loads them into the monitor
// EQ, so the output takes on that speaker's tonal character. Reuses the monitor EQ, so it
// records one undo step; clearing the EQ removes it.
namespace {
std::vector<std::string>& virtualMonitorNames() {
    static std::vector<std::string> names = neuracoust::daw::speakerProfilesWithCurve();
    return names;
}
}

// --- Acoustic measurement (②b): sweep out a channel, capture the mic, deconvolve to a curve ---
namespace {
neuracoust::daw::SweepParams measurementSweepParams(double sampleRate) {
    neuracoust::daw::SweepParams p;
    p.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    p.startHz = 20.0;
    p.endHz = std::min(20000.0, p.sampleRate * 0.45);
    p.durationSeconds = 3.0;
    p.amplitude = 0.5;
    return p;
}
}

bool nc_measure_start(NCEngine* engine, int channel) {
    if (engine == nullptr) return false;
    const auto p = measurementSweepParams(engine->engine.status().sampleRate);
    auto sweep = neuracoust::daw::generateLogSweep(p);
    sweep.resize(sweep.size() + static_cast<size_t>(p.sampleRate * 0.7), 0.0f);  // room-decay tail
    engine->engine.setMeasurementChannels(-1, 0);   // room: emit via the monitor route, capture ch 1
    engine->engine.startMeasurement(channel == 1 ? 1 : 0, std::move(sweep));
    return true;
}

// Interface loopback: pin the sweep to the chosen physical DAC channel and capture the chosen ADC
// channel (both 1-based in the UI), so DigiGrid out 3 → in 3 works without moving the monitor.
bool nc_measure_interface_start(NCEngine* engine) {
    if (engine == nullptr) return false;
    auto p = measurementSweepParams(engine->engine.status().sampleRate);
    p.amplitude = std::max(0.001, std::min(0.99, engine->measureSweepAmplitude));   // auto-scaled level
    auto sweep = neuracoust::daw::generateLogSweep(p);
    sweep.resize(sweep.size() + static_cast<size_t>(p.sampleRate * 0.3), 0.0f);   // short settle tail
    engine->engine.setMeasurementChannels(std::max(1, engine->measureOutputChannel) - 1,
                                          std::max(1, engine->measureInputChannel) - 1);
    engine->engine.startMeasurement(0, std::move(sweep));   // sweep on the bus' ch 0 → redirected to measOut
    return true;
}

void nc_measure_set_sweep_amplitude(NCEngine* engine, double amplitude) {
    if (engine != nullptr) engine->measureSweepAmplitude = std::max(0.001, std::min(0.99, amplitude));
}

// Multi-level auto run (A단계): reset the accumulator, record each finished sweep at its return
// level, and read back the THD-vs-level table for the display.
void nc_measure_interface_reset_levels(NCEngine* engine) {
    if (engine != nullptr) engine->pendingLevels.clear();
}
void nc_measure_interface_record_level(NCEngine* engine, double returnDbfs) {
    if (engine == nullptr || !engine->pendingInterfaceValid) return;
    engine->pendingLevels.push_back({returnDbfs, engine->pendingInterfaceProfile});
}
int nc_measure_interface_level_count(NCEngine* engine) {
    if (engine == nullptr) return 0;
    if (!engine->pendingLevels.empty()) return static_cast<int>(engine->pendingLevels.size());
    const auto it = engine->measuredInterfaces.find(engine->project.physicalAudioInterfaceModel);
    return it != engine->measuredInterfaces.end() ? static_cast<int>(it->second.thdVsLevel.size()) : 0;
}
double nc_measure_interface_level_dbfs(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0) return 0.0;
    if (index < static_cast<int>(engine->pendingLevels.size())) return engine->pendingLevels[static_cast<size_t>(index)].dbfs;
    const auto it = engine->measuredInterfaces.find(engine->project.physicalAudioInterfaceModel);
    if (it != engine->measuredInterfaces.end() && index < static_cast<int>(it->second.thdVsLevel.size()))
        return it->second.thdVsLevel[static_cast<size_t>(index)].first;
    return 0.0;
}
double nc_measure_interface_level_thd(NCEngine* engine, int index) {
    if (engine == nullptr || index < 0) return 0.0;
    if (index < static_cast<int>(engine->pendingLevels.size())) return engine->pendingLevels[static_cast<size_t>(index)].prof.thdPercent;
    const auto it = engine->measuredInterfaces.find(engine->project.physicalAudioInterfaceModel);
    if (it != engine->measuredInterfaces.end() && index < static_cast<int>(it->second.thdVsLevel.size()))
        return it->second.thdVsLevel[static_cast<size_t>(index)].second;
    return 0.0;
}

// Live gain-setup meter: force the chosen input channel to capture and report its peak so the
// user can set loopback gain (avoid ADC clipping) before running the sweep.
void nc_measure_level_check(NCEngine* engine, bool on) {
    if (engine == nullptr) return;
    if (on) {
        engine->engine.setMeasurementChannels(std::max(1, engine->measureOutputChannel) - 1,
                                              std::max(1, engine->measureInputChannel) - 1);
    }
    engine->engine.setMeasurementLevelCheck(on);
}
float nc_measure_input_level(NCEngine* engine) {   // linear peak, 0..1
    return engine != nullptr ? engine->engine.measurementInputPeak() : 0.0f;
}

void nc_measure_set_output_channel(NCEngine* engine, int oneBased) {
    if (engine != nullptr) engine->measureOutputChannel = std::max(1, oneBased);
}
void nc_measure_set_input_channel(NCEngine* engine, int oneBased) {
    if (engine != nullptr) engine->measureInputChannel = std::max(1, oneBased);
}
int nc_measure_output_channel(NCEngine* engine) { return engine != nullptr ? engine->measureOutputChannel : 1; }
int nc_measure_input_channel(NCEngine* engine) { return engine != nullptr ? engine->measureInputChannel : 1; }
int nc_measure_output_channel_count(NCEngine* engine) {
    return engine != nullptr ? std::max(2, engine->engine.status().outputChannels) : 2;
}
int nc_measure_input_channel_count(NCEngine* engine) {
    return engine != nullptr ? std::max(1, engine->engine.selectedInputChannelCount()) : 1;
}

bool nc_measure_active(NCEngine* engine) { return engine != nullptr && engine->engine.measurementActive(); }
double nc_measure_progress(NCEngine* engine) { return engine != nullptr ? engine->engine.measurementProgress() : 0.0; }
void nc_measure_cancel(NCEngine* engine) { if (engine != nullptr) engine->engine.cancelMeasurement(); }

// Deconvolve the captured mic into the channel's in-room response curve (midband-normalized).
// Returns false if too little was captured (e.g. no mic / input monitoring was off).
bool nc_measure_finish(NCEngine* engine, int channel) {
    if (engine == nullptr) return false;
    const auto capture = engine->engine.takeMeasurementCapture();
    const auto p = measurementSweepParams(engine->engine.status().sampleRate);
    if (capture.size() < static_cast<size_t>(p.sampleRate * 0.5)) return false;
    const auto ir = neuracoust::daw::deconvolveSweep(capture, p);
    const int pts = 200;
    const auto mags = neuracoust::daw::impulseResponseMagnitudeDb(ir, p.sampleRate, pts, 20.0, 20000.0);
    neuracoust::daw::ResponseCurve curve;
    const double lo = 20.0, hi = 20000.0, ratio = std::log(hi / lo);
    for (int i = 0; i < pts; ++i) {
        curve.push_back({lo * std::exp(ratio * (pts > 1 ? static_cast<double>(i) / (pts - 1) : 0.0)),
                         static_cast<double>(mags[static_cast<size_t>(i)])});
    }
    curve = neuracoust::daw::normalizeCurveMidband(curve);
    (channel == 1 ? engine->measuredCurveR : engine->measuredCurveL) = std::move(curve);
    return true;
}

bool nc_measure_has_curve(NCEngine* engine, int channel) {
    if (engine == nullptr) return false;
    return !(channel == 1 ? engine->measuredCurveR : engine->measuredCurveL).empty();
}

void nc_measure_curve_response(NCEngine* engine, int channel, double* out, int count, double minHz, double maxHz) {
    for (int i = 0; i < count; ++i) out[i] = 0.0;
    if (engine == nullptr || out == nullptr || count <= 0) return;
    const auto& c = (channel == 1) ? engine->measuredCurveR : engine->measuredCurveL;
    if (c.empty()) return;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz), ratio = std::log(hi / lo);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::exp(ratio * (count > 1 ? static_cast<double>(i) / (count - 1) : 0.0));
        out[i] = neuracoust::daw::interpolateCurveDb(c, f);
    }
}

// --- VR / headset-worn monitor correction --------------------------------------------------
// Capture the room measurement with the headset OFF, then ON; the correction is (baseline − worn),
// added to the monitor EQ to undo how the worn headset reshapes the sound at the ears.
namespace {
neuracoust::daw::ResponseCurve currentMeasurementAvg(NCEngine* engine) {
    const auto& L = engine->measuredCurveL;
    const auto& R = engine->measuredCurveR;
    if (L.empty() && R.empty()) return {};
    if (R.empty()) return L;
    if (L.empty()) return R;
    neuracoust::daw::ResponseCurve avg;
    avg.reserve(L.size());
    for (const auto& [f, db] : L) {
        avg.push_back({f, (db + neuracoust::daw::interpolateCurveDb(R, f)) * 0.5});
    }
    return avg;
}
}  // namespace

// Snapshot the current measurement as the headset-OFF reference. Run a room measurement first.
bool nc_vr_capture_baseline(NCEngine* engine) {
    if (engine == nullptr) return false;
    auto avg = currentMeasurementAvg(engine);
    if (avg.empty()) return false;
    engine->vrHeadsetBaseline = std::move(avg);
    return true;
}

// With a baseline captured and a fresh headset-ON measurement present, build the correction.
bool nc_vr_capture_worn(NCEngine* engine) {
    if (engine == nullptr || engine->vrHeadsetBaseline.empty()) return false;
    auto worn = currentMeasurementAvg(engine);
    if (worn.empty()) return false;
    neuracoust::daw::ResponseCurve corr;
    corr.reserve(worn.size());
    for (const auto& [f, wdb] : worn) {
        const double bdb = neuracoust::daw::interpolateCurveDb(engine->vrHeadsetBaseline, f);
        corr.push_back({f, bdb - wdb});   // add this to undo the worn headset's coloring
    }
    engine->vrHeadsetCorrection = neuracoust::daw::normalizeCurveMidband(corr);
    engine->vrHeadsetCorrectionEnabled = true;
    return true;
}

bool nc_vr_correction_enabled(NCEngine* engine) { return engine != nullptr && engine->vrHeadsetCorrectionEnabled; }
bool nc_vr_correction_active(NCEngine* engine) { return engine != nullptr && !engine->vrHeadsetCorrection.empty(); }
bool nc_vr_has_baseline(NCEngine* engine) { return engine != nullptr && !engine->vrHeadsetBaseline.empty(); }
void nc_vr_set_correction_enabled(NCEngine* engine, bool on) { if (engine != nullptr) engine->vrHeadsetCorrectionEnabled = on; }
void nc_vr_clear_correction(NCEngine* engine) {
    if (engine == nullptr) return;
    engine->vrHeadsetCorrection.clear();
    engine->vrHeadsetBaseline.clear();
    engine->vrHeadsetCorrectionEnabled = false;
}
void nc_vr_correction_response(NCEngine* engine, double* out, int count, double minHz, double maxHz) {
    for (int i = 0; i < count; ++i) out[i] = 0.0;
    if (engine == nullptr || out == nullptr || count <= 0 || engine->vrHeadsetCorrection.empty()) return;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz), ratio = std::log(hi / lo);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::exp(ratio * (count > 1 ? static_cast<double>(i) / (count - 1) : 0.0));
        out[i] = neuracoust::daw::interpolateCurveDb(engine->vrHeadsetCorrection, f);
    }
}

// --- Audio-interface loopback measurement (②d) --------------------------------------------
// Patch the interface's DAC output back to its ADC input, sweep, and one ESS capture gives BOTH
// the D/A frequency response (→ FIR compensation) and the harmonic coefficients (→ waveshaper).
// Real measurement overrides the offline baked profile; persisted per-model so it lasts.
namespace {

std::filesystem::path measuredInterfaceDir() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".neuracoust" / "measured_interfaces";
}

// A model name maps to a filesystem-safe stem (keep it readable, replace path-hostile chars).
std::string interfaceFileStem(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) {
        s += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    }
    return s.empty() ? "interface" : s;
}

// Self-contained JSON bits — the file-local parse helpers in ProjectDocument.cpp are not
// visible here, and the schema we read is the flat one we write just below.
std::string ifaceJsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}
std::string ifaceJsonString(const std::string& text, const std::string& key) {
    const auto k = text.find("\"" + key + "\"");
    if (k == std::string::npos) return {};
    const auto q1 = text.find('"', text.find(':', k));
    if (q1 == std::string::npos) return {};
    const auto q2 = text.find('"', q1 + 1);
    return q2 == std::string::npos ? std::string{} : text.substr(q1 + 1, q2 - q1 - 1);
}
double ifaceJsonNumber(const std::string& text, const std::string& key, double def) {
    const auto k = text.find("\"" + key + "\"");
    if (k == std::string::npos) return def;
    const auto colon = text.find(':', k);
    if (colon == std::string::npos) return def;
    try { return std::stod(text.substr(colon + 1)); } catch (...) { return def; }
}

void saveMeasuredInterface(const std::string& name, const NCEngine::MeasuredInterfaceProfile& prof) {
    std::error_code ec;
    std::filesystem::create_directories(measuredInterfaceDir(), ec);
    std::ofstream f(measuredInterfaceDir() / (interfaceFileStem(name) + ".json"));
    if (!f) return;
    f << "{\n  \"name\": \"" << ifaceJsonEscape(name) << "\",\n";
    f << "  \"thdPercent\": " << prof.thdPercent << ",\n";
    f << "  \"harmonics\": [";
    for (size_t i = 0; i < prof.harmonics.size(); ++i) f << (i ? ", " : "") << prof.harmonics[i];
    f << "],\n  \"thdVsLevel\": [";
    for (size_t i = 0; i < prof.thdVsLevel.size(); ++i)
        f << (i ? ", " : "") << "[" << prof.thdVsLevel[i].first << ", " << prof.thdVsLevel[i].second << "]";
    f << "],\n  \"curve\": [";
    for (size_t i = 0; i < prof.curve.size(); ++i)
        f << (i ? ", " : "") << "[" << prof.curve[i].first << ", " << prof.curve[i].second << "]";
    f << "]\n}\n";
}

void loadMeasuredInterfaces(NCEngine* engine) {
    std::error_code ec;
    const auto dir = measuredInterfaceDir();
    if (!std::filesystem::exists(dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path());
        if (!f) continue;
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const std::string name = ifaceJsonString(text, "name");
        if (name.empty()) continue;
        NCEngine::MeasuredInterfaceProfile prof;
        prof.thdPercent = ifaceJsonNumber(text, "thdPercent", 0.0);
        // harmonics: the first flat array in the file
        if (const auto hpos = text.find("\"harmonics\""); hpos != std::string::npos) {
            const auto lb = text.find('[', hpos), rb = text.find(']', lb);
            if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                std::stringstream ss(text.substr(lb + 1, rb - lb - 1));
                std::string tok;
                while (std::getline(ss, tok, ',')) { try { prof.harmonics.push_back(std::stod(tok)); } catch (...) {} }
            }
        }
        // Parse an "key": [[a, b], …] array-of-pairs by matching the outer brackets.
        auto parsePairs = [&text](const std::string& key) {
            std::vector<std::pair<double, double>> out;
            const auto kpos = text.find("\"" + key + "\"");
            if (kpos == std::string::npos) return out;
            const auto outerOpen = text.find('[', kpos);
            if (outerOpen == std::string::npos) return out;
            std::size_t outerClose = std::string::npos, depth = 0;
            for (std::size_t i = outerOpen; i < text.size(); ++i) {
                if (text[i] == '[') ++depth;
                else if (text[i] == ']') { if (--depth == 0) { outerClose = i; break; } }
            }
            std::size_t inner = text.find('[', outerOpen + 1);
            while (inner != std::string::npos && inner < outerClose) {
                const auto close = text.find(']', inner);
                if (close == std::string::npos || close > outerClose) break;
                const std::string pair = text.substr(inner + 1, close - inner - 1);
                const auto comma = pair.find(',');
                if (comma != std::string::npos) {
                    try { out.push_back({std::stod(pair.substr(0, comma)), std::stod(pair.substr(comma + 1))}); }
                    catch (...) {}
                }
                inner = text.find('[', close + 1);
            }
            return out;
        };
        prof.curve = parsePairs("curve");
        prof.thdVsLevel = parsePairs("thdVsLevel");
        if (!prof.harmonics.empty() || !prof.curve.empty()) engine->measuredInterfaces[name] = std::move(prof);
    }
}

} // namespace

bool nc_measure_interface_finish(NCEngine* engine) {
    if (engine == nullptr) return false;
    const std::string name = engine->project.physicalAudioInterfaceModel;
    if (name.empty()) return false;   // nothing to attribute the measurement to
    const auto capture = engine->engine.takeMeasurementCapture();
    const auto p = measurementSweepParams(engine->engine.status().sampleRate);
    if (capture.size() < static_cast<size_t>(p.sampleRate * 0.5)) return false;

    // FR curve (midband-normalized deviation from flat), same as the room path.
    const auto ir = neuracoust::daw::deconvolveSweep(capture, p);
    const int pts = 200;
    const auto mags = neuracoust::daw::impulseResponseMagnitudeDb(ir, p.sampleRate, pts, 20.0, 20000.0);
    neuracoust::daw::ResponseCurve curve;
    const double lo = 20.0, hi = 20000.0, ratio = std::log(hi / lo);
    for (int i = 0; i < pts; ++i) {
        curve.push_back({lo * std::exp(ratio * (pts > 1 ? static_cast<double>(i) / (pts - 1) : 0.0)),
                         static_cast<double>(mags[static_cast<size_t>(i)])});
    }
    curve = neuracoust::daw::normalizeCurveMidband(curve);

    // Harmonics from the SAME capture — the Farina separation.
    const auto harm = neuracoust::daw::separateHarmonics(capture, p, 7);

    NCEngine::MeasuredInterfaceProfile prof;
    prof.curve = std::move(curve);
    if (harm.valid) { prof.harmonics = harm.coefficients; prof.thdPercent = harm.thdPercent; }

    // Hold as PENDING for the user to review (quality verdict) and confirm — do NOT save/apply yet,
    // so a clipped or too-low capture cannot silently overwrite a good profile.
    engine->pendingInterfaceProfile = std::move(prof);
    engine->pendingInterfaceName = name;
    engine->pendingInterfacePeak = engine->engine.measurementSweepPeak();
    engine->pendingInterfaceValid = true;
    return true;
}

// Review data for the pending measurement (for the quality verdict).
bool nc_measure_interface_pending(NCEngine* engine) { return engine != nullptr && engine->pendingInterfaceValid; }
double nc_measure_interface_pending_thd(NCEngine* engine) {
    return engine != nullptr && engine->pendingInterfaceValid ? engine->pendingInterfaceProfile.thdPercent : 0.0;
}
float nc_measure_interface_pending_peak(NCEngine* engine) {   // sweep peak, 0..1 (>=~0.99 = clipped)
    return engine != nullptr ? engine->pendingInterfacePeak : 0.0f;
}
void nc_measure_interface_pending_name(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->pendingInterfaceName : std::string{});
}

// User accepted the measurement: commit it (store + persist + apply). A multi-level run builds the
// profile from its accumulator — FR + harmonics from the loudest clean level (best SNR), plus the
// full THD-vs-level table; a single-shot run uses the pending profile as-is.
void nc_measure_interface_commit(NCEngine* engine) {
    if (engine == nullptr) return;
    NCEngine::MeasuredInterfaceProfile profile;
    std::string name = engine->pendingInterfaceName;
    if (!engine->pendingLevels.empty()) {
        // Representative = loudest level that did not clip (dbfs < -0.5); fallback to the loudest.
        int rep = 0;
        double bestDb = -1e9;
        for (int i = 0; i < static_cast<int>(engine->pendingLevels.size()); ++i) {
            const double d = engine->pendingLevels[static_cast<size_t>(i)].dbfs;
            if (d < -0.5 && d > bestDb) { bestDb = d; rep = i; }
        }
        profile = engine->pendingLevels[static_cast<size_t>(rep)].prof;
        profile.thdVsLevel.clear();
        for (const auto& lp : engine->pendingLevels) profile.thdVsLevel.push_back({lp.dbfs, lp.prof.thdPercent});
    } else if (engine->pendingInterfaceValid) {
        profile = engine->pendingInterfaceProfile;
    } else {
        return;
    }
    if (name.empty()) name = engine->project.physicalAudioInterfaceModel;
    if (name.empty()) return;
    engine->measuredInterfaces[name] = profile;
    saveMeasuredInterface(name, profile);
    engine->pendingInterfaceValid = false;
    engine->pendingLevels.clear();
    engine->pushInterfaceModeler();  // EQ re-sync (interface FR) is driven from Swift via nc_monitor_eq_sync
}

void nc_measure_interface_discard(NCEngine* engine) {
    if (engine != nullptr) { engine->pendingInterfaceValid = false; engine->pendingLevels.clear(); }
}

bool nc_measure_interface_has_profile(NCEngine* engine, const char* name) {
    if (engine == nullptr || name == nullptr) return false;
    const auto it = engine->measuredInterfaces.find(name);
    return it != engine->measuredInterfaces.end() && (!it->second.curve.empty() || !it->second.harmonics.empty());
}

double nc_measure_interface_thd(NCEngine* engine, const char* name) {
    if (engine == nullptr || name == nullptr) return 0.0;
    const auto it = engine->measuredInterfaces.find(name);
    return it != engine->measuredInterfaces.end() ? it->second.thdPercent : 0.0;
}

void nc_measure_interface_harmonics(NCEngine* engine, const char* name, double* out, int count) {
    for (int i = 0; i < count; ++i) out[i] = 0.0;
    if (engine == nullptr || name == nullptr || out == nullptr) return;
    const auto it = engine->measuredInterfaces.find(name);
    if (it == engine->measuredInterfaces.end()) return;
    const auto& h = it->second.harmonics;
    for (int i = 0; i < count && i < static_cast<int>(h.size()); ++i) out[i] = h[i];
}

void nc_measure_interface_curve_response(NCEngine* engine, const char* name, double* out, int count,
                                         double minHz, double maxHz) {
    for (int i = 0; i < count; ++i) out[i] = 0.0;
    if (engine == nullptr || name == nullptr || out == nullptr || count <= 0) return;
    const auto it = engine->measuredInterfaces.find(name);
    if (it == engine->measuredInterfaces.end() || it->second.curve.empty()) return;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz), ratio = std::log(hi / lo);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::exp(ratio * (count > 1 ? static_cast<double>(i) / (count - 1) : 0.0));
        out[i] = neuracoust::daw::interpolateCurveDb(it->second.curve, f);
    }
}

void nc_measure_interface_clear(NCEngine* engine, const char* name) {
    if (engine == nullptr || name == nullptr) return;
    engine->measuredInterfaces.erase(name);
    std::error_code ec;
    std::filesystem::remove(measuredInterfaceDir() / (interfaceFileStem(name) + ".json"), ec);
    engine->pushInterfaceModeler();  // EQ re-sync (interface FR) is driven from Swift via nc_monitor_eq_sync
}

int nc_virtual_monitor_count(NCEngine*) {
    return static_cast<int>(virtualMonitorNames().size());
}

void nc_virtual_monitor_name(NCEngine*, int index, char* out, size_t outLen) {
    const auto& names = virtualMonitorNames();
    if (index < 0 || static_cast<size_t>(index) >= names.size()) { copyText(out, outLen, ""); return; }
    copyText(out, outLen, names[static_cast<size_t>(index)]);
}

// Headphone models that carry a measured curve (the headphone equivalent of virtual monitors).
namespace {
std::vector<std::string>& headphoneProfileNames() {
    static std::vector<std::string> names = neuracoust::daw::headphoneProfilesWithCurve();
    return names;
}
}
int nc_headphone_profile_count(NCEngine*) {
    return static_cast<int>(headphoneProfileNames().size());
}
void nc_headphone_profile_name(NCEngine*, int index, char* out, size_t outLen) {
    const auto& names = headphoneProfileNames();
    if (index < 0 || static_cast<size_t>(index) >= names.size()) { copyText(out, outLen, ""); return; }
    copyText(out, outLen, names[static_cast<size_t>(index)]);
}
// A headphone model's measured curve sampled on a log grid (for the UI overlay). False if unmeasured.
bool nc_headphone_profile_response(NCEngine*, const char* name, double* outMagsDb, int count,
                                   double minHz, double maxHz) {
    if (outMagsDb == nullptr || count <= 0) return false;
    for (int i = 0; i < count; ++i) outMagsDb[i] = 0.0;
    if (name == nullptr) return false;
    const auto curve = neuracoust::daw::headphoneProfileCurve(name);
    if (curve.empty()) return false;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::pow(hi / lo, count > 1 ? static_cast<double>(i) / (count - 1) : 0.0);
        outMagsDb[i] = neuracoust::daw::interpolateCurveDb(curve, f);
    }
    return true;
}
// The FR curve of a speaker model — measured profile, else the spec-derived approximation — so the
// response-window overlay matches what actually voices the monitor EQ. Empty only for Flat/Off.
bool nc_speaker_profile_response(NCEngine*, const char* name, double* outMagsDb, int count,
                                 double minHz, double maxHz) {
    if (outMagsDb == nullptr || count <= 0) return false;
    for (int i = 0; i < count; ++i) outMagsDb[i] = 0.0;
    if (name == nullptr) return false;
    auto curve = neuracoust::daw::speakerProfileCurve(name);
    if (curve.empty()) curve = neuracoust::daw::speakerSpecCurve(name);
    if (curve.empty()) return false;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::pow(hi / lo, count > 1 ? static_cast<double>(i) / (count - 1) : 0.0);
        outMagsDb[i] = neuracoust::daw::interpolateCurveDb(curve, f);
    }
    return true;
}
// The D/A FR of an audio-interface model — measured profile, else spec approximation (matches the EQ).
bool nc_audio_interface_profile_response(NCEngine*, const char* name, double* outMagsDb, int count,
                                         double minHz, double maxHz) {
    if (outMagsDb == nullptr || count <= 0) return false;
    for (int i = 0; i < count; ++i) outMagsDb[i] = 0.0;
    if (name == nullptr) return false;
    auto curve = neuracoust::daw::audioInterfaceProfileCurve(name);
    if (curve.empty()) curve = neuracoust::daw::audioInterfaceSpecCurve(name);
    if (curve.empty()) return false;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::pow(hi / lo, count > 1 ? static_cast<double>(i) / (count - 1) : 0.0);
        outMagsDb[i] = neuracoust::daw::interpolateCurveDb(curve, f);
    }
    return true;
}

namespace {
// Load fitted EQ bands into the project's monitor EQ and push them live (no full reconcile,
// so it never drops the audio). Shared by virtual monitor and room correction.
void loadEqBandsIntoMonitorEq(NCEngine* engine, const std::vector<neuracoust::daw::EqBandSpec>& bands,
                              const std::string& stepName) {
    engine->project.monitorEqBands.clear();
    for (const auto& b : bands) {
        neuracoust::daw::MonitorEqBandState state;
        state.enabled = b.enabled;
        switch (b.type) {
            case neuracoust::daw::EqBandType::LowShelf: state.type = "low_shelf"; break;
            case neuracoust::daw::EqBandType::HighShelf: state.type = "high_shelf"; break;
            case neuracoust::daw::EqBandType::HighPass: state.type = "high_pass"; break;
            case neuracoust::daw::EqBandType::LowPass: state.type = "low_pass"; break;
            case neuracoust::daw::EqBandType::Notch: state.type = "notch"; break;
            default: state.type = "peaking"; break;
        }
        state.frequencyHz = b.frequencyHz;
        state.gainDb = b.gainDb;
        state.q = b.q;
        engine->project.monitorEqBands.push_back(state);
    }
    engine->engine.updateMonitorEq(engine->project.monitorEqBands);
    engine->recordStep(stepName);
}
}

bool nc_monitor_eq_apply_virtual_monitor(NCEngine* engine, const char* catalogName) {
    if (engine == nullptr || catalogName == nullptr) return false;
    const auto curve = neuracoust::daw::speakerProfileCurve(catalogName);
    if (curve.empty()) return false;
    // The dataset curve is already midband-normalized, i.e. the speaker's deviation from flat —
    // impose it directly to take on its character. 48 bands; boost limited more than cut.
    const auto bands = neuracoust::daw::fitCurveToEqBands(curve, 64, 20.0, 20000.0, 9.0, 15.0);
    loadEqBandsIntoMonitorEq(engine, bands, std::string("Virtual monitor: ") + catalogName);
    return true;
}

// Room correction (③): flatten the measured in-room response toward the Harman target.
// correction = Harman_target − measured; boost limited more than cut (can't fill a null).
bool nc_monitor_eq_apply_room_correction(NCEngine* engine, int channel) {
    if (engine == nullptr) return false;
    const auto& measured = (channel == 1) ? engine->measuredCurveR : engine->measuredCurveL;
    if (measured.empty()) return false;
    neuracoust::daw::ResponseCurve correction;
    correction.reserve(measured.size());
    for (const auto& [f, db] : measured) {
        correction.push_back({f, neuracoust::daw::harmanTargetDb(f) - db});
    }
    const auto bands = neuracoust::daw::fitCurveToEqBands(correction, 64, 20.0, 20000.0, 9.0, 12.0);
    loadEqBandsIntoMonitorEq(engine, bands, channel == 1 ? "Room correction (R)" : "Room correction (L)");
    return true;
}

// The single monitor EQ, rebuilt from the active monitoring context (the "one EQ, values swap"
// design): impose the selected speaker/headphone MODEL's measured curve, plus the room-tuning
// correction when one was measured, as one fitted 48-band set. Empty model + no room = flat.
// Records NO history — it is derived state, re-run on every context change, not a user edit.
namespace {
// Heuristic (name-based) tone for a passive speaker's power amp and cable — the same honest
// approximation the speaker sim uses, applied only until a real measurement exists. Deliberately
// small and physically motivated: modern solid-state amps and decent cables are essentially flat.
std::string lowerOf(const std::string& s) {
    std::string o; for (char c : s) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}
neuracoust::daw::ResponseCurve powerAmpToneCurve(const std::string& name) {
    return neuracoust::daw::powerAmpCatalogToneCurve(name);
}
neuracoust::daw::ResponseCurve speakerCableToneCurve(const std::string& name) {
    return neuracoust::daw::speakerCableCatalogToneCurve(name);
}
}  // namespace

// Whether the passive speaker's amp / cable heuristic actually colours the sound (non-flat).
bool nc_power_amp_tone_active(NCEngine* engine) {
    if (engine == nullptr || engine->project.physicalSpeakerModel.empty() ||
        !speakerModelIsPassive(engine->project.physicalSpeakerModel)) return false;
    return !powerAmpToneCurve(engine->project.physicalPowerAmpModel).empty();
}
bool nc_speaker_cable_tone_active(NCEngine* engine) {
    if (engine == nullptr || engine->project.physicalSpeakerModel.empty() ||
        !speakerModelIsPassive(engine->project.physicalSpeakerModel)) return false;
    return !speakerCableToneCurve(engine->project.physicalSpeakerCableModel).empty();
}

void nc_monitor_eq_sync(NCEngine* engine, const char* slotModel, const char* correctionHeadphone, bool applyRoom) {
    if (engine == nullptr) return;
    // Two terms into the one EQ:
    //  • slotModel — the active A/B/C target to SIMULATE (+): a speaker profile, or a headphone
    //    profile (referenced to the OE target when enabled). This is what you want to hear.
    //  • correctionHeadphone — the PHYSICAL headphone you are wearing, CORRECTED toward neutral (−),
    //    so it reproduces the target instead of colouring it. Empty in speaker mode.
    neuracoust::daw::ResponseCurve slot;
    bool slotIsHeadphone = false;
    if (slotModel != nullptr && slotModel[0] != '\0') {
        slot = neuracoust::daw::speakerProfileCurve(slotModel);
        if (slot.empty()) {
            slot = neuracoust::daw::headphoneProfileCurve(slotModel);
            slotIsHeadphone = !slot.empty();
            // A speaker with no measured profile falls back to its spec-derived curve, so the graph
            // and the monitor EQ voice it instead of leaving the target blank.
            if (slot.empty()) slot = neuracoust::daw::speakerSpecCurve(slotModel);
        }
    }
    if (slotIsHeadphone && engine->monitorEqHeadphoneOeTarget) {
        for (auto& [f, db] : slot) db -= neuracoust::daw::harmanHeadphoneOeTargetDb(f);
    }
    neuracoust::daw::ResponseCurve correction;
    if (correctionHeadphone != nullptr && correctionHeadphone[0] != '\0') {
        correction = neuracoust::daw::headphoneProfileCurve(correctionHeadphone);
    }
    const bool oe = engine->monitorEqHeadphoneOeTarget;
    // The physical output-stage interface's measured D/A FR is COMPENSATED (−) so monitoring is
    // flattened for that converter — the real-measurement half of the interface modeler.
    neuracoust::daw::ResponseCurve interfaceFr;
    if (!engine->project.physicalAudioInterfaceModel.empty()) {
        interfaceFr = engine->interfaceCurveFor(engine->project.physicalAudioInterfaceModel);
    }
    // A→B modeling: after flattening your own interface (−), COLOR the output with the target
    // interface's measured FR (+) so it sounds like you're monitoring through that converter.
    neuracoust::daw::ResponseCurve interfaceTargetFr;
    if (!engine->project.physicalAudioInterfaceTargetModel.empty()) {
        interfaceTargetFr = engine->interfaceCurveFor(engine->project.physicalAudioInterfaceTargetModel);
    }
    // Room correction for the single stereo EQ. Uses the L/R AVERAGE when both channels are measured
    // (Codex #4) — a single EQ can't correct the two channels independently, so applying L's curve to
    // both was wrong for an asymmetric room; the average is the least-wrong shared correction. True
    // per-channel room correction needs a per-channel EQ (structural, not built).
    neuracoust::daw::ResponseCurve room;
    if (applyRoom) {
        room = neuracoust::daw::roomCorrectionCurve(engine->measuredCurveL, engine->measuredCurveR);
    }
    // VR/headset-worn correction (added): the stored (baseline − worn) curve, undoing the acoustic
    // change the headset makes at the ears. Independent of room correction.
    neuracoust::daw::ResponseCurve vrCorrection;
    if (engine->vrHeadsetCorrectionEnabled && !engine->vrHeadsetCorrection.empty()) {
        vrCorrection = engine->vrHeadsetCorrection;
    }
    // Passive speaker → its power amp and cable colour the output too (name heuristic, honest,
    // until measured). Only when the physical speaker is passive; active monitors have no amp/cable.
    neuracoust::daw::ResponseCurve ampCurve, cableCurve;
    if (!engine->project.physicalSpeakerModel.empty() &&
        speakerModelIsPassive(engine->project.physicalSpeakerModel)) {
        ampCurve = powerAmpToneCurve(engine->project.physicalPowerAmpModel);
        cableCurve = speakerCableToneCurve(engine->project.physicalSpeakerCableModel);
    }
    // The MODELING speaker (active A/B/C slot): if the modeled speaker is passive, its own amp and
    // cable colour the simulation too — the same per-slot heuristic as the physical chain.
    neuracoust::daw::ResponseCurve slotAmpCurve, slotCableCurve;
    if (slotModel != nullptr && *slotModel != '\0' && speakerModelIsPassive(slotModel)) {
        if (MonitorDspModule* sim = engine->speakerSimulation()) {
            const int as = std::max(0, std::min(2, sim->activeTargetSlot));
            slotAmpCurve = powerAmpToneCurve(*speakerAmpFieldForSlot(*sim, as));
            slotCableCurve = speakerCableToneCurve(*speakerCableFieldForSlot(*sim, as));
        }
    }
    // The REAL speaker (active A/B/C slot): if the speaker you actually monitor on is passive, its
    // own amp + cable colour the chain you hear — SUBTRACTED, so the correction flattens them (the
    // mirror of the modeled speaker's amp/cable, which are added). Independent of the modeled ones.
    neuracoust::daw::ResponseCurve realAmpCurve, realCableCurve;
    if (MonitorDspModule* sim = engine->speakerSimulation()) {
        const int as = std::max(0, std::min(2, sim->activeTargetSlot));
        std::string realSpeaker = as == 1 ? sim->realModelB : as == 2 ? sim->realModelC : sim->realModelA;
        if (realSpeaker.empty()) realSpeaker = sim->realModel;   // old-project fallback
        if (!realSpeaker.empty() && speakerModelIsPassive(realSpeaker)) {
            realAmpCurve = powerAmpToneCurve(*speakerRealAmpFieldForSlot(*sim, as));
            realCableCurve = speakerCableToneCurve(*speakerRealCableFieldForSlot(*sim, as));
        }
    }
    // NOTE: the REAL speaker's full response is deliberately NOT subtracted here. "target − real" is
    // right for midband voicing but a full subtraction tries to EQ a sealed/ported speaker's own
    // bass rolloff back to flat — a huge (+20 dB) LF boost that clips and adds noise (the NS-10 boost
    // trap). Spec models therefore impose the TARGET absolutely, exactly like measured models; the
    // real speaker is assumed handled by room correction. Bounded target−real is future measured work.
    // Combine slot (+) + physical-headphone correction (target − worn) + room on a shared log grid.
    neuracoust::daw::ResponseCurve combined;
    if (!slot.empty() || !correction.empty() || !room.empty() || !interfaceFr.empty() ||
        !interfaceTargetFr.empty() || !ampCurve.empty() || !cableCurve.empty() ||
        !slotAmpCurve.empty() || !slotCableCurve.empty() ||
        !realAmpCurve.empty() || !realCableCurve.empty() ||
        !vrCorrection.empty()) {
        const int points = 96;
        for (int i = 0; i < points; ++i) {
            const double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / (points - 1));
            double db = 0.0;
            if (!slot.empty()) db += neuracoust::daw::interpolateCurveDb(slot, f);
            if (!correction.empty()) db += (oe ? neuracoust::daw::harmanHeadphoneOeTargetDb(f) : 0.0)
                                          - neuracoust::daw::interpolateCurveDb(correction, f);
            if (!room.empty()) db += neuracoust::daw::interpolateCurveDb(room, f);
            if (!vrCorrection.empty()) db += neuracoust::daw::interpolateCurveDb(vrCorrection, f);   // undo headset-worn coloring
            if (!interfaceFr.empty()) db -= neuracoust::daw::interpolateCurveDb(interfaceFr, f);         // flatten own D/A
            if (!interfaceTargetFr.empty()) db += neuracoust::daw::interpolateCurveDb(interfaceTargetFr, f); // colour as target
            if (!ampCurve.empty()) db += neuracoust::daw::interpolateCurveDb(ampCurve, f);              // power amp voicing
            if (!cableCurve.empty()) db += neuracoust::daw::interpolateCurveDb(cableCurve, f);          // cable HF loss
            if (!slotAmpCurve.empty()) db += neuracoust::daw::interpolateCurveDb(slotAmpCurve, f);      // modeled speaker's amp
            if (!slotCableCurve.empty()) db += neuracoust::daw::interpolateCurveDb(slotCableCurve, f);  // modeled speaker's cable
            if (!realAmpCurve.empty()) db -= neuracoust::daw::interpolateCurveDb(realAmpCurve, f);       // flatten real speaker's amp
            if (!realCableCurve.empty()) db -= neuracoust::daw::interpolateCurveDb(realCableCurve, f);   // flatten real speaker's cable
            combined.push_back({f, db});
        }
    }

    // Perceptual level match (not peak normalization). A monitor SIMULATION shapes tone; ON vs OFF
    // must sit at the same PERCEIVED level or the modelled path reads as quieter and "darker". The
    // old code shifted the whole curve down by its PEAK, so a +6 dB presence bump dropped everything
    // else by 6 dB — the exact dark/dull A-B mismatch. Instead pivot around the 300 Hz–3 kHz mean
    // (where loudness lives): the midband sits at 0 dB, presence/air bumps stay as real boosts (fair
    // A-B), and the residual boost is caught by the safety soft-clip AFTER the monitor EQ
    // (monitorSafetySoftClip in the render), NOT by pre-darkening the tone here.
    if (!combined.empty()) {
        combined = neuracoust::daw::normalizeCurveMidband(combined);
    }

    // Performing or recording? Take the zero-latency path even when linear phase is on.
    const bool monitoringLive = engine->monitorEqLowLatencyWhileMonitoring &&
        std::any_of(engine->project.tracks.begin(), engine->project.tracks.end(),
                    [](const neuracoust::daw::TrackState& track) {
                        return track.recordArmed || track.inputMonitoring;
                    });
    engine->monitorEqLowLatencyActive = monitoringLive && engine->monitorEqLinearPhase;

    if (engine->monitorEqLinearPhase && !monitoringLive) {
        // Linear-phase path: design a FIR that matches the target across the whole band (steep
        // bass rolloff + treble dips included), and clear the biquad so only one runs.
        engine->project.monitorEqBands.clear();
        engine->engine.updateMonitorEq(engine->project.monitorEqBands);
        engine->engine.updateMonitorFir(combined, engine->monitorEqFirTaps);
        return;
    }

    // Biquad path: clear any FIR and fit the target to 64 peaking bands. The fit must run at the
    // engine's ACTUAL sample rate — the biquad magnitude warps near the top octave, so a curve fit at
    // a fixed 48 kHz but rendered at 44.1/88.2/96 kHz drifts in the treble (Codex #2).
    engine->engine.updateMonitorFir({}, 0);
    engine->project.monitorEqBands.clear();
    if (!combined.empty()) {
        const double fitRate = engine->engine.status().sampleRate > 1000.0
            ? engine->engine.status().sampleRate : engine->project.sampleRate;
        const auto bands = neuracoust::daw::fitCurveToEqBands(combined, 64, 20.0, 20000.0, 9.0, 15.0, 1.0 / 6.0, fitRate);
        for (const auto& b : bands) {
            neuracoust::daw::MonitorEqBandState state;
            state.enabled = b.enabled;
            switch (b.type) {
                case neuracoust::daw::EqBandType::LowShelf: state.type = "low_shelf"; break;
                case neuracoust::daw::EqBandType::HighShelf: state.type = "high_shelf"; break;
                case neuracoust::daw::EqBandType::HighPass: state.type = "high_pass"; break;
                case neuracoust::daw::EqBandType::LowPass: state.type = "low_pass"; break;
                case neuracoust::daw::EqBandType::Notch: state.type = "notch"; break;
                default: state.type = "peaking"; break;
            }
            state.frequencyHz = b.frequencyHz;
            state.gainDb = b.gainDb;
            state.q = b.q;
            engine->project.monitorEqBands.push_back(state);
        }
    }
    engine->engine.updateMonitorEq(engine->project.monitorEqBands);
}

// Log-spaced magnitude response (dB) across [minHz, maxHz] for the UI curve. Reflects whatever
// monitor EQ is actually live — the linear-phase FIR when active, otherwise the biquad chain.
void nc_monitor_eq_response(NCEngine* engine, double* outMagsDb, int count, double minHz, double maxHz) {
    if (outMagsDb == nullptr || count <= 0) return;
    for (int i = 0; i < count; ++i) outMagsDb[i] = 0.0;
    if (engine == nullptr) return;
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    const double ratio = std::log(hi / lo);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::exp(ratio * (count > 1 ? static_cast<double>(i) / (count - 1) : 0.0));
        outMagsDb[i] = engine->engine.monitorEqMagnitudeDb(f);
    }
}

// Linear-phase (FIR) monitor EQ toggle. Re-derives the current context through the chosen path.
bool nc_monitor_eq_linear_phase(NCEngine* engine) {
    return engine != nullptr && engine->monitorEqLinearPhase;
}
void nc_monitor_eq_set_linear_phase(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->monitorEqLinearPhase == enabled) return;
    engine->monitorEqLinearPhase = enabled;
    // The caller re-runs nc_monitor_eq_sync (via reloadMonitorState) to rebuild through the new path.
}
bool nc_monitor_eq_headphone_oe_target(NCEngine* engine) {
    return engine != nullptr && engine->monitorEqHeadphoneOeTarget;
}
// Low-latency monitoring: while any track is armed or input-monitoring, drop the FIR's
// numTaps/2 delay by using the minimum-phase fit of the same curve.
bool nc_monitor_eq_low_latency_monitoring(NCEngine* engine) {
    return engine != nullptr && engine->monitorEqLowLatencyWhileMonitoring;
}
void nc_monitor_eq_set_low_latency_monitoring(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->monitorEqLowLatencyWhileMonitoring == enabled) return;
    engine->monitorEqLowLatencyWhileMonitoring = enabled;
    // The caller re-runs nc_monitor_eq_sync to rebuild through the path this now selects.
}
bool nc_monitor_eq_low_latency_active(NCEngine* engine) {
    return engine != nullptr && engine->monitorEqLowLatencyActive;
}
/// Latency the monitor EQ is adding right now, in samples. 0 on the biquad path.
int nc_monitor_eq_latency_samples(NCEngine* engine) {
    return engine != nullptr ? engine->engine.monitorFirLatencySamples() : 0;
}

void nc_monitor_eq_set_headphone_oe_target(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->monitorEqHeadphoneOeTarget == enabled) return;
    engine->monitorEqHeadphoneOeTarget = enabled;
    // The caller re-runs nc_monitor_eq_sync (via reloadMonitorState) to rebuild with/without the target.
}
// Latency (ms) the active monitor FIR adds; 0 on the biquad path.
double nc_monitor_eq_latency_ms(NCEngine* engine) {
    if (engine == nullptr) return 0.0;
    const double sr = std::max(1.0, engine->engine.status().sampleRate);
    return 1000.0 * static_cast<double>(engine->engine.monitorFirLatencySamples()) / sr;
}
// The room-tuning correction (Harman target − measured), fitted to bands and sampled on the
// same log grid as nc_monitor_eq_response — i.e. exactly what room correction WOULD impose,
// shown without touching the live EQ. All zeros until a room measurement exists (channel 0=L,
// 1=R). Returns true when a measurement was available, false when the curve is flat/unmeasured.
bool nc_monitor_room_correction_response(NCEngine* engine, int channel,
                                         double* outMagsDb, int count, double minHz, double maxHz) {
    if (outMagsDb == nullptr || count <= 0) return false;
    for (int i = 0; i < count; ++i) outMagsDb[i] = 0.0;
    if (engine == nullptr) return false;
    const auto& measured = (channel == 1) ? engine->measuredCurveR : engine->measuredCurveL;
    if (measured.empty()) return false;
    neuracoust::daw::ResponseCurve correction;
    correction.reserve(measured.size());
    for (const auto& [f, db] : measured) {
        correction.push_back({f, neuracoust::daw::harmanTargetDb(f) - db});
    }
    const auto bands = neuracoust::daw::fitCurveToEqBands(correction, 64, 20.0, 20000.0, 9.0, 12.0);
    std::vector<neuracoust::daw::EqBandSpec> specs;
    specs.reserve(bands.size());
    for (const auto& b : bands) specs.push_back({b.enabled, b.type, b.frequencyHz, b.gainDb, b.q});
    neuracoust::daw::ParametricEq eq;
    eq.configure(48000.0, specs);
    const double lo = std::max(1.0, minHz), hi = std::max(lo + 1.0, maxHz);
    const double ratio = std::log(hi / lo);
    for (int i = 0; i < count; ++i) {
        const double f = lo * std::exp(ratio * (count > 1 ? static_cast<double>(i) / (count - 1) : 0.0));
        outMagsDb[i] = eq.magnitudeDb(f);
    }
    return true;
}
bool nc_monitor_output_exclusive(NCEngine* engine) {
    return engine != nullptr && engine->project.monitorSpeakerHeadphoneExclusive;
}
void nc_monitor_set_output_exclusive(NCEngine* engine, bool exclusive) {
    if (engine == nullptr || engine->project.monitorSpeakerHeadphoneExclusive == exclusive) return;
    engine->project.monitorSpeakerHeadphoneExclusive = exclusive;
    engine->recordStep(exclusive ? "Enable speaker/headphone exclusive" : "Disable speaker/headphone exclusive");
    // Off = simultaneous: the inactive tab's pair also carries the monitor signal. pushModules
    // mirrors the flag into the speaker-sim module and hands the set to the engine live.
    engine->pushModules();
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

int nc_speaker_output_route_count(NCEngine* engine) {
    const int channels = engine != nullptr
        ? std::max(2, engine->engine.status().outputChannels)
        : 2;
    // None + Main 1-2 + one entry for each complete pair above channels 1-2.
    return 1 + channels / 2;
}

void nc_speaker_output_route(NCEngine* engine, int index, char* out, size_t outLen) {
    const int count = nc_speaker_output_route_count(engine);
    if (index < 0 || index >= count) {
        copyText(out, outLen, {});
        return;
    }
    if (index == 0) {
        copyText(out, outLen, "None");
        return;
    }
    if (index == 1) {
        copyText(out, outLen, "Main 1-2");
        return;
    }
    const int left = index * 2 - 1;
    copyText(out, outLen, "Output " + std::to_string(left) + "-" + std::to_string(left + 1));
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

// The REAL speaker the user monitors on for this slot (empty = none). Drives the correction.
void nc_monitor_set_speaker_real_model(NCEngine* engine, int slot, const char* model) {
    if (engine == nullptr || model == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    std::string* field = speakerRealModelFieldForSlot(*module, slot);
    if (*field == model) return;
    *field = model;
    engine->recordStep("Set real speaker");
    engine->pushModules();
}

void nc_monitor_speaker_real_model(NCEngine* engine, int slot, char* out, size_t outLen) {
    if (engine == nullptr || slot < 0 || slot > 2) { copyText(out, outLen, std::string{}); return; }
    MonitorDspModule* module = engine->speakerSimulation();
    copyText(out, outLen, module != nullptr ? *speakerRealModelFieldForSlot(*module, slot) : std::string{});
}

void nc_monitor_set_speaker_output(NCEngine* engine, int slot, const char* route) {
    if (engine == nullptr || route == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    const std::string value = route;
    std::string* out = speakerOutputFieldForSlot(*module, slot);
    if (*out == value) return;
    *out = value;
    // A physical output pair now keeps the slot's speaker model + room EQ, so each A/B/C
    // speaker can run its own simulator on its own output pair. For a raw reference monitor,
    // pick the "Flat" model on that slot.
    engine->recordStep("Set speaker output");
    engine->pushModules();
}

void nc_monitor_headphone_output(NCEngine* engine, char* out, size_t outLen) {
    const MonitorDspModule* module = engine != nullptr ? engine->speakerSimulation() : nullptr;
    copyText(out, outLen, module != nullptr ? module->headphoneOutput : std::string{});
}

void nc_monitor_set_headphone_output(NCEngine* engine, const char* route) {
    if (engine == nullptr || route == nullptr) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr || module->headphoneOutput == route) return;
    module->headphoneOutput = route;
    engine->recordStep("Set headphone output");
    engine->pushModules();
}

bool nc_monitor_output_to_headphone(NCEngine* engine) {
    const MonitorDspModule* module = engine != nullptr ? engine->speakerSimulation() : nullptr;
    return module != nullptr && module->monitorToHeadphone;
}

// The 스피커/헤드폰 tab, delivered to the engine so ROUTING can follow it — the tab used to
// switch only the DSP context while the audio kept leaving on the speaker slot's pair. Not an
// undo step, same as the listen buttons: it is monitor state, not an edit.
void nc_monitor_set_output_to_headphone(NCEngine* engine, bool headphone) {
    if (engine == nullptr) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr || module->monitorToHeadphone == headphone) return;
    module->monitorToHeadphone = headphone;
    engine->pushModules();
}

// Per-slot power amp / cable for a passive modeled speaker (heuristic tone; applied in eq_sync).
void nc_monitor_speaker_amp(NCEngine* engine, int slot, char* out, size_t outLen) {
    MonitorDspModule* module = engine != nullptr ? engine->speakerSimulation() : nullptr;
    copyText(out, outLen, (module != nullptr && slot >= 0 && slot <= 2) ? *speakerAmpFieldForSlot(*module, slot) : std::string{});
}
void nc_monitor_speaker_cable(NCEngine* engine, int slot, char* out, size_t outLen) {
    MonitorDspModule* module = engine != nullptr ? engine->speakerSimulation() : nullptr;
    copyText(out, outLen, (module != nullptr && slot >= 0 && slot <= 2) ? *speakerCableFieldForSlot(*module, slot) : std::string{});
}
void nc_monitor_set_speaker_amp(NCEngine* engine, int slot, const char* model) {
    if (engine == nullptr || model == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    std::string* field = speakerAmpFieldForSlot(*module, slot);
    if (*field == model) return;
    *field = model;
    engine->recordStep("Set speaker amp");
    engine->pushModules();   // Swift re-runs nc_monitor_eq_sync to fold in the amp tone
}
void nc_monitor_set_speaker_cable(NCEngine* engine, int slot, const char* model) {
    if (engine == nullptr || model == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    std::string* field = speakerCableFieldForSlot(*module, slot);
    if (*field == model) return;
    *field = model;
    engine->recordStep("Set speaker cable");
    engine->pushModules();
}

// Per-slot power amp / cable for a passive REAL speaker (the chain you actually hear on — it is
// SUBTRACTED in eq_sync, flattening the real chain, unlike the modeled speaker's amp/cable above).
void nc_monitor_speaker_real_amp(NCEngine* engine, int slot, char* out, size_t outLen) {
    MonitorDspModule* module = engine != nullptr ? engine->speakerSimulation() : nullptr;
    copyText(out, outLen, (module != nullptr && slot >= 0 && slot <= 2) ? *speakerRealAmpFieldForSlot(*module, slot) : std::string{});
}
void nc_monitor_speaker_real_cable(NCEngine* engine, int slot, char* out, size_t outLen) {
    MonitorDspModule* module = engine != nullptr ? engine->speakerSimulation() : nullptr;
    copyText(out, outLen, (module != nullptr && slot >= 0 && slot <= 2) ? *speakerRealCableFieldForSlot(*module, slot) : std::string{});
}
void nc_monitor_set_speaker_real_amp(NCEngine* engine, int slot, const char* model) {
    if (engine == nullptr || model == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    std::string* field = speakerRealAmpFieldForSlot(*module, slot);
    if (*field == model) return;
    *field = model;
    engine->recordStep("Set real speaker amp");
    engine->pushModules();
}
void nc_monitor_set_speaker_real_cable(NCEngine* engine, int slot, const char* model) {
    if (engine == nullptr || model == nullptr || slot < 0 || slot > 2) return;
    MonitorDspModule* module = engine->speakerSimulation();
    if (module == nullptr) return;
    std::string* field = speakerRealCableFieldForSlot(*module, slot);
    if (*field == model) return;
    *field = model;
    engine->recordStep("Set real speaker cable");
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

namespace {

/// Raw MIDI bytes for a recorded event, for mirroring the live stream to editor processes.
bool recordedMidiEventToRawBytes(const neuracoust::daw::RecordedMidiEvent& e, NCMidiLiveEvent& out) {
    using neuracoust::daw::RecordedMidiEventKind;
    const unsigned char channel = static_cast<unsigned char>(std::max(1, std::min(16, e.channel)) - 1);
    switch (e.kind) {
    case RecordedMidiEventKind::NoteOn:
        out = {static_cast<unsigned char>(0x90u | channel),
               static_cast<unsigned char>(std::max(0, std::min(127, e.pitch))),
               static_cast<unsigned char>(std::max(1, std::min(127, e.velocity)))};
        return true;
    case RecordedMidiEventKind::NoteOff:
        out = {static_cast<unsigned char>(0x80u | channel),
               static_cast<unsigned char>(std::max(0, std::min(127, e.pitch))), 0u};
        return true;
    case RecordedMidiEventKind::Controller:
        out = {static_cast<unsigned char>(0xB0u | channel),
               static_cast<unsigned char>(std::max(0, std::min(127, e.controller))),
               static_cast<unsigned char>(std::max(0, std::min(127, e.value)))};
        return true;
    case RecordedMidiEventKind::PitchBend: {
        const int bend = std::max(0, std::min(16383, e.value));
        out = {static_cast<unsigned char>(0xE0u | channel),
               static_cast<unsigned char>(bend & 0x7F),
               static_cast<unsigned char>((bend >> 7) & 0x7F)};
        return true;
    }
    case RecordedMidiEventKind::ProgramChange:
        out = {static_cast<unsigned char>(0xC0u | channel),
               static_cast<unsigned char>(std::max(0, std::min(127, e.program))), 0u};
        return true;
    }
    return false;
}

} // namespace

int nc_midi_pump_live_input(NCEngine* engine, NCMidiLiveEvent* outEvents, int maxEvents) {
    if (engine == nullptr || !engine->midiInputRecorder.status().recording) return 0;
    const auto pending = engine->midiInputRecorder.consumePendingEvents();
    if (pending.empty()) return 0;
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
    // Mirror the drained batch back to the caller so the UI can forward it to any open
    // instrument editor process (the editor hosts a SECOND plug-in instance whose GUI
    // keyboard/wheels otherwise never hear the keyboard).
    int mirrored = 0;
    if (outEvents != nullptr && maxEvents > 0) {
        for (const auto& event : pending) {
            if (mirrored >= maxEvents) break;
            NCMidiLiveEvent raw {};
            if (recordedMidiEventToRawBytes(event, raw)) outEvents[mirrored++] = raw;
        }
    }
    std::vector<neuracoust::daw::Vst3MidiEvent> liveEvents;
    liveEvents.reserve(pending.size());
    for (const auto& event : pending) {
        neuracoust::daw::Vst3MidiEvent v;
        if (recordedMidiEventToVst3Event(event, v)) liveEvents.push_back(v);
    }
    if (liveEvents.empty()) return mirrored;
    // Every armed / input-monitoring instrument track hears the keyboard, plus the currently
    // selected instrument track (the live-MIDI target) so playing just works after loading an
    // instrument. A track with no instrument plug-in simply renders nothing.
    for (const auto& track : engine->project.tracks) {
        if (track.trackType != "instrument") continue;
        // Only feed a track that actually carries a loaded instrument. Queuing to an empty-slot track
        // renders nothing, but it flips the engine into "has render content" for that one block and
        // back, and the engage/disengage transition ticks — a short blip on every keypress even though
        // no instrument is playing. No instrument → no queue → no blip.
        const bool hasInstrument = instrumentSlotFilled(track.instrument) ||
            std::any_of(track.instrumentSlots.begin(), track.instrumentSlots.end(), instrumentSlotFilled);
        if (!hasInstrument) continue;
        const bool isTarget = !engine->liveMidiTargetTrack.empty() && track.name == engine->liveMidiTargetTrack;
        if (!(track.recordArmed || track.inputMonitoring || isTarget)) continue;
        // While this track's instrument editor is open, the EDITOR's instance owns the
        // live path (its audio comes back over the monitor ring); queueing here too
        // would sound every note twice.
        if (!engine->instrumentEditorMonitor.trackName.empty() &&
            track.name == engine->instrumentEditorMonitor.trackName) continue;
        engine->engine.queueLiveMidiEvents(track.name, liveEvents);
    }
    return mirrored;
}

// ---------------------------------------------------------------------------
// Instrument editor reverse-audio monitor (GUI keyboard clicks → speakers)
// ---------------------------------------------------------------------------

bool nc_track_instrument_editor_opened(NCEngine* engine, int index,
                                       char* shmName, size_t shmNameLen,
                                       int* maxBlock, double* sampleRate) {
    auto* track = trackAt(engine, index);
    if (engine == nullptr || track == nullptr || track->trackType != "instrument") {
        return false;
    }
    engine->stopInstrumentEditorMonitor();

    const int block = engine->project.defaultBufferSize > 0 ? engine->project.defaultBufferSize : 256;
    const double rate = engine->project.sampleRate > 0.0 ? engine->project.sampleRate : 48000.0;
    const std::string name =
        neuracoust::daw::vst3InstrumentMonitorShmName(track->name + "\x1finstmon");
    auto reader = std::make_unique<neuracoust::daw::Vst3InstrumentMonitorReader>();
    if (!reader->create(name, block, rate)) {
        return false;
    }

    engine->engine.setEditorInstrumentMonitor(true, track->name);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto* readerPtr = reader.get();
    auto* realtimeEngine = &engine->engine;
    engine->instrumentEditorMonitor.trackName = track->name;
    engine->instrumentEditorMonitor.reader = std::move(reader);
    engine->instrumentEditorMonitor.stop = stop;
    // Drain the ring every ~2 ms (one audio-block period) into the engine's monitor
    // FIFO. The reader object outlives the thread: stopInstrumentEditorMonitor joins
    // before releasing it.
    engine->instrumentEditorMonitor.pumpThread = std::thread([readerPtr, stop, realtimeEngine]() {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        while (!stop->load(std::memory_order_relaxed)) {
            readerPtr->drain([&](const float* samples, int frames) {
                realtimeEngine->pushEditorInstrumentMonitorInterleaved(samples, frames);
            });
            usleep(2000);
        }
    });

    copyText(shmName, shmNameLen, name);
    if (maxBlock != nullptr) {
        *maxBlock = block;
    }
    if (sampleRate != nullptr) {
        *sampleRate = rate;
    }
    return true;
}

void nc_track_instrument_editor_closed(NCEngine* engine, int index) {
    if (engine == nullptr) {
        return;
    }
    // Only tear down if the closing editor is the one that owns the monitor — a
    // stale close from an earlier editor must not kill a newer one's ring.
    const auto* track = trackAt(engine, index);
    if (track == nullptr || engine->instrumentEditorMonitor.trackName == track->name) {
        engine->stopInstrumentEditorMonitor();
    }
}

bool nc_track_instrument_slot_write_state_file(NCEngine* engine, int index, int slotIndex,
                                               const char* path) {
    if (path == nullptr || *path == '\0' || slotIndex < 0) {
        return false;
    }
    const auto* slot = instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    if (slot == nullptr || slot->pluginStateBase64.empty()) {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!neuracoust::daw::decodeBase64(slot->pluginStateBase64, bytes) || bytes.empty()) {
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

bool nc_track_instrument_slot_read_state_file(NCEngine* engine, int index, int slotIndex,
                                              const char* path) {
    if (path == nullptr || *path == '\0' || slotIndex < 0) {
        return false;
    }
    auto* slot = instrumentSlotMutable(engine, index, static_cast<size_t>(slotIndex));
    if (slot == nullptr) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return false;
    }
    const std::string encoded = neuracoust::daw::encodeBase64(bytes.data(), bytes.size());
    if (encoded == slot->pluginStateBase64) {
        // The user opened the editor and changed nothing: rebuilding the instrument here
        // would cut the sound for no reason.
        return false;
    }
    slot->pluginStateBase64 = encoded;
    auto* track = trackAt(engine, index);
    if (track != nullptr && slotIndex == 0 && !track->instrumentSlots.empty()) {
        // Keep the legacy mirror in step, as the parameter setter does.
        track->instrument = track->instrumentSlots.front();
    }
    // Apply the patch to the LIVE instrument voice on the main thread — deactivate, setState,
    // reactivate the existing instance — rather than reconciling. A reconcile would make the
    // realtime audio thread destroy and re-instantiate the whole workstation instrument, which
    // stalls the render for hundreds of milliseconds (the sound drops out) and was the crash /
    // "old preset then silence" on editor close. If the voice is not prepared yet, the deferred
    // prepare applies the patch from the field we just set, so either way the patch is not lost.
    if (track != nullptr) {
        engine->engine.updateInstrumentComponentState(track->name, static_cast<size_t>(slotIndex),
                                                      encoded);
    }
    engine->recordStep("Change instrument patch");
    return true;
}

// ---------------------------------------------------------------------------
// MIDI recording — accumulate the live keyboard into a region while the transport records
// ---------------------------------------------------------------------------

bool nc_midi_record_begin(NCEngine* engine, int trackIndex, double startSeconds) {
    auto* track = trackAt(engine, trackIndex);
    if (engine == nullptr || track == nullptr) {
        return false;
    }
    engine->midiRecordTake = {};
    auto& take = engine->midiRecordTake;
    take.active = true;
    take.trackName = track->name;
    take.startSeconds = std::max(0.0, startSeconds);
    take.tempoBpm = engine->project.tempoBpm > 0.0 ? engine->project.tempoBpm : 120.0;
    take.lastPlayheadSeconds = take.startSeconds;
    // Create the region up front (model-only, no reconcile/history) so the timeline draws it
    // the instant recording starts; notes land in it live as they are played. The single
    // history step is recorded at commit, so undo returns to the pre-recording state.
    take.regionId = neuracoust::daw::addMidiRegion(engine->project, take.trackName,
                                                   take.startSeconds, 0.25, "Recording");
    if (take.regionId.empty()) {
        take.active = false;
        return false;
    }
    return true;
}

bool nc_midi_record_active(NCEngine* engine) {
    return engine != nullptr && engine->midiRecordTake.active;
}

// Feed the events drained this tick (the same raw batch nc_midi_pump_live_input returns),
// stamped at the current playhead. Note-ons are held until their matching note-off closes them.
void nc_midi_record_feed(NCEngine* engine, const NCMidiLiveEvent* events, int count,
                         double playheadSeconds) {
    if (engine == nullptr || !engine->midiRecordTake.active || events == nullptr || count <= 0) {
        return;
    }
    auto& take = engine->midiRecordTake;
    take.lastPlayheadSeconds = std::max(take.startSeconds, playheadSeconds);
    const double beatsPerSecond = take.tempoBpm / 60.0;
    const double beat = std::max(0.0, take.lastPlayheadSeconds - take.startSeconds) * beatsPerSecond;
    for (int i = 0; i < count; ++i) {
        const unsigned char statusByte = events[i].status & 0xF0u;
        const int pitch = events[i].data1 & 0x7F;
        const bool noteOn = statusByte == 0x90u && (events[i].data2 & 0x7F) > 0;
        const bool noteOff = statusByte == 0x80u || (statusByte == 0x90u && (events[i].data2 & 0x7F) == 0);
        // Continuous controllers. Only the ones the project whitelists are written into the
        // region — everything a keyboard sends is heard live either way, but recording all
        // of it buries a part under aftertouch and unused lanes. The pedal is what makes a
        // piano take sound like the performance, so CC64 is on by default.
        if (statusByte == 0xB0u) {
            const int controller = events[i].data1 & 0x7F;
            const auto& allowed = engine->project.midiRecordControllers;
            if (std::find(allowed.begin(), allowed.end(), controller) != allowed.end()) {
                neuracoust::daw::addMidiControllerEvent(engine->project, take.regionId, beat,
                                                        controller, events[i].data2 & 0x7F,
                                                        (events[i].status & 0x0Fu) + 1);
                ++take.controllerCount;
            }
            continue;
        }
        // Pitch bend is not a CC and has its own switch. 14-bit, LSB then MSB.
        if (statusByte == 0xE0u) {
            if (engine->project.midiRecordPitchBend) {
                const int value = ((events[i].data2 & 0x7F) << 7) | (events[i].data1 & 0x7F);
                neuracoust::daw::addMidiPitchBendEvent(engine->project, take.regionId, beat, value,
                                                       (events[i].status & 0x0Fu) + 1);
                ++take.controllerCount;
            }
            continue;
        }
        if (noteOn) {
            take.held[static_cast<size_t>(pitch)] = { beat, events[i].data2 & 0x7F, true };
        } else if (noteOff && take.held[static_cast<size_t>(pitch)].on) {
            auto& h = take.held[static_cast<size_t>(pitch)];
            // Add the completed note to the region immediately (model-only, no reconcile) so it
            // draws as the key is released. Playback still monitors live through the instrument;
            // the notes enter the render plan only at commit, so there is no double-trigger.
            neuracoust::daw::addMidiNote(engine->project, take.regionId, pitch,
                                         h.startBeat, std::max(0.01, beat - h.startBeat), h.velocity);
            ++take.noteCount;
            h.on = false;
        }
    }
    // Grow the region to the playhead so the clip visibly extends while recording.
    neuracoust::daw::resizeMidiRegion(engine->project, take.regionId,
                                      std::max(0.25, take.lastPlayheadSeconds - take.startSeconds));
}

// Finish the take: close any still-held notes at the last playhead, create the region and its
// notes, and return the new region id (empty if nothing was recorded).
bool nc_midi_record_commit(NCEngine* engine, char* outRegionId, size_t outLen) {
    copyText(outRegionId, outLen, "");
    if (engine == nullptr || !engine->midiRecordTake.active) {
        return false;
    }
    auto& take = engine->midiRecordTake;
    take.active = false;
    const double beatsPerSecond = take.tempoBpm / 60.0;
    const double endBeat = std::max(0.0, take.lastPlayheadSeconds - take.startSeconds) * beatsPerSecond;
    // Close any keys still held at stop, landing them in the live region like the rest.
    for (size_t pitch = 0; pitch < take.held.size(); ++pitch) {
        auto& h = take.held[pitch];
        if (h.on) {
            neuracoust::daw::addMidiNote(engine->project, take.regionId, static_cast<int>(pitch),
                                         h.startBeat, std::max(0.01, endBeat - h.startBeat), h.velocity);
            ++take.noteCount;
            h.on = false;
        }
    }
    if (take.noteCount == 0 && take.controllerCount == 0) {
        // Nothing was played: drop the placeholder region and reconcile it out of the picture.
        // A take with no notes but recorded controller moves — a pedal or mod-wheel overdub —
        // is real work and is kept.
        neuracoust::daw::deleteMidiRegion(engine->project, take.regionId);
        engine->reconcileProject();
        take.regionId.clear();
        return false;
    }
    neuracoust::daw::resizeMidiRegion(engine->project, take.regionId,
                                      std::max(0.25, take.lastPlayheadSeconds - take.startSeconds));
    // One reconcile brings the take into the render plan, one history step for the whole take.
    applyMidiEdit(engine, true, "Record MIDI");
    copyText(outRegionId, outLen, take.regionId);
    take.regionId.clear();
    return true;
}

void nc_midi_preview_note(NCEngine* engine, int trackIndex, int pitch, int velocity, bool noteOn) {
    auto* track = trackAt(engine, trackIndex);
    if (engine == nullptr || track == nullptr || track->trackType != "instrument") {
        return;
    }
    neuracoust::daw::Vst3MidiEvent event;
    event.frameOffset = 0;
    event.channel = 1;
    event.kind = neuracoust::daw::Vst3MidiEventKind::Note;
    event.pitch = std::max(0, std::min(127, pitch));
    event.noteOn = noteOn;
    event.velocity = noteOn ? std::max(1, std::min(127, velocity)) : 0;
    engine->engine.queueLiveMidiEvents(track->name, {event});
    // While this track's editor is open the EDITOR's instance owns the live path, so the
    // render instance would answer a click the user cannot hear. Mirror it there too.
    if (noteOn) {
        engine->previewHeldNotes[track->name].insert(event.pitch);
    } else {
        auto held = engine->previewHeldNotes.find(track->name);
        if (held != engine->previewHeldNotes.end()) {
            held->second.erase(event.pitch);
            if (held->second.empty()) engine->previewHeldNotes.erase(held);
        }
    }
}

void nc_midi_preview_all_notes_off(NCEngine* engine, int trackIndex) {
    auto* track = trackAt(engine, trackIndex);
    if (engine == nullptr || track == nullptr) {
        return;
    }
    const auto held = engine->previewHeldNotes.find(track->name);
    if (held == engine->previewHeldNotes.end()) {
        return;
    }
    std::vector<neuracoust::daw::Vst3MidiEvent> offs;
    offs.reserve(held->second.size());
    for (const int pitch : held->second) {
        neuracoust::daw::Vst3MidiEvent event;
        event.frameOffset = 0;
        event.channel = 1;
        event.kind = neuracoust::daw::Vst3MidiEventKind::Note;
        event.pitch = pitch;
        event.noteOn = false;
        event.velocity = 0;
        offs.push_back(event);
    }
    engine->previewHeldNotes.erase(held);
    engine->engine.queueLiveMidiEvents(track->name, offs);
}

bool nc_midi_record_controller_enabled(NCEngine* engine, int controller) {
    if (engine == nullptr || controller < 0 || controller > 127) return false;
    const auto& allowed = engine->project.midiRecordControllers;
    return std::find(allowed.begin(), allowed.end(), controller) != allowed.end();
}

void nc_midi_record_set_controller_enabled(NCEngine* engine, int controller, bool enabled) {
    if (engine == nullptr || controller < 0 || controller > 127) return;
    auto& allowed = engine->project.midiRecordControllers;
    const auto found = std::find(allowed.begin(), allowed.end(), controller);
    if (enabled && found == allowed.end()) {
        allowed.push_back(controller);
        std::sort(allowed.begin(), allowed.end());
    } else if (!enabled && found != allowed.end()) {
        allowed.erase(found);
    } else {
        return;
    }
    // A capture setting, not a graph change: nothing to reconcile, but it is part of the
    // document, so the project is dirty and it belongs in the history.
    engine->recordStep("Set recorded MIDI controllers");
}

bool nc_midi_record_pitch_bend_enabled(NCEngine* engine) {
    return engine != nullptr && engine->project.midiRecordPitchBend;
}

void nc_midi_record_set_pitch_bend_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->project.midiRecordPitchBend == enabled) return;
    engine->project.midiRecordPitchBend = enabled;
    engine->recordStep("Set recorded MIDI controllers");
}

// The selected instrument track hears the keyboard without being record-armed (Logic/Live
// style). Pass -1 to clear. Transient routing only — touches no project state.
void nc_set_live_midi_target(NCEngine* engine, int trackIndex) {
    if (engine == nullptr) return;
    const auto* track = trackAt(engine, trackIndex);
    engine->liveMidiTargetTrack = (track != nullptr && track->trackType == "instrument")
        ? track->name : std::string{};
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

// --- AI assistant (Phase 0) -------------------------------------------------
// The ported neuracoust::daw AiAssistant library owns the brains — project
// snapshot, command validation, prompting. These functions expose it to Swift:
// build a request for the local Ollama server, and apply the safe, reversible
// commands the assistant proposes (one undo step each). The HTTP call itself
// lives in Swift (URLSession), so nothing here blocks or touches the network.

namespace {

std::string aiJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// The vague ported system prompt gets an explicit, parseable command schema so
// the model's output can drive real edits. Kept to the safe, reversible subset.
std::string aiSystemPromptWithSchema() {
    return
        "You are the Neuracoust DAW assistant. The user writes in Korean or English; "
        "reply in the user's language. Respond with ONE JSON object and nothing else: "
        "{\"reply\":\"<short conversational answer>\",\"commands\":[<zero or more commands>]}. "
        "Each command is exactly one of (the type string is lowercase with underscores): "
        "{\"type\":\"set_track_gain\",\"track\":\"<name>\",\"gainDb\":<-60..12>,\"reason\":\"<why>\"} , "
        "{\"type\":\"set_track_pan\",\"track\":\"<name>\",\"pan\":<-1..1>,\"reason\":\"<why>\"} , "
        "{\"type\":\"set_track_mute\",\"track\":\"<name>\",\"enabled\":<true|false>} , "
        "{\"type\":\"set_track_solo\",\"track\":\"<name>\",\"enabled\":<true|false>} , "
        "{\"type\":\"arm_track_for_recording\",\"track\":\"<name>\",\"enabled\":<true|false>} , "
        "{\"type\":\"add_marker\",\"label\":\"<text>\",\"timeSeconds\":<number>} . "
        "Only use track names that appear in the snapshot. If no change is needed, return an "
        "empty commands array. Never claim the audio changed; the host applies each command "
        "only after the user confirms.";
}

} // namespace

void nc_ai_project_context(NCEngine* engine, char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    const auto snapshot = neuracoust::daw::makeAiProjectSnapshot(engine->project);
    copyText(out, outLen, neuracoust::daw::serializeAiProjectSnapshot(snapshot));
}

void nc_ai_build_request(NCEngine* engine, const char* model, const char* userText,
                         char* out, size_t outLen) {
    if (engine == nullptr) { copyText(out, outLen, ""); return; }
    const auto snapshot = neuracoust::daw::makeAiProjectSnapshot(engine->project);
    const std::string snapshotJson = neuracoust::daw::serializeAiProjectSnapshot(snapshot);
    const std::string modelName = (model != nullptr && *model != '\0') ? model : "qwen2.5-coder:14b";
    const std::string user = userText != nullptr ? userText : "";
    std::ostringstream body;
    body << "{\"model\":\"" << aiJsonEscape(modelName) << "\","
         << "\"stream\":false,\"format\":\"json\","
         // Cap generation and keep it near-deterministic: a weak model can otherwise
         // degenerate under the JSON grammar and run to the token ceiling.
         << "\"options\":{\"num_predict\":512,\"temperature\":0.2},"
         << "\"messages\":["
         << "{\"role\":\"system\",\"content\":\"" << aiJsonEscape(aiSystemPromptWithSchema()) << "\"},"
         << "{\"role\":\"user\",\"content\":\"Project snapshot: " << aiJsonEscape(snapshotJson)
         << "\\nUser request: " << aiJsonEscape(user) << "\"}"
         << "]}";
    copyText(out, outLen, body.str());
}

bool nc_ai_apply_command(NCEngine* engine, const char* typeStr, const char* trackName,
                         float gainDb, float pan, bool enabled, double timeSeconds,
                         const char* label, char* msg, size_t msgLen) {
    using namespace neuracoust::daw;
    if (engine == nullptr) { copyText(msg, msgLen, "no engine"); return false; }
    AiCommand cmd;
    cmd.type = aiCommandTypeFromString(typeStr != nullptr ? typeStr : "");
    cmd.targetTrackName = trackName != nullptr ? trackName : "";
    cmd.label = label != nullptr ? label : "";
    cmd.gainDb = gainDb;
    cmd.pan = pan;
    cmd.enabled = enabled;
    cmd.timeSeconds = timeSeconds;
    const auto validation = validateAiCommand(engine->project, cmd);
    if (!validation.ok) { copyText(msg, msgLen, validation.message); return false; }

    bool applied = false;
    switch (cmd.type) {
    case AiCommandType::SetTrackGain:
        applied = setTrackVolumeDb(engine->project, cmd.targetTrackName, gainDb);
        break;
    case AiCommandType::SetTrackPan:
        applied = setTrackPan(engine->project, cmd.targetTrackName, pan);
        break;
    case AiCommandType::SetTrackMute:
        applied = setTrackMuted(engine->project, cmd.targetTrackName, enabled);
        break;
    case AiCommandType::SetTrackSolo:
        applied = setTrackSolo(engine->project, cmd.targetTrackName, enabled);
        break;
    case AiCommandType::ArmTrackForRecording:
        applied = setTrackRecordArmed(engine->project, cmd.targetTrackName, enabled);
        break;
    case AiCommandType::AddMarker:
        applied = !addMarkerAt(engine->project, timeSeconds).empty();
        if (applied && !cmd.label.empty()) {
            renameNearestMarker(engine->project, timeSeconds, 0.01, cmd.label);
        }
        break;
    default:
        copyText(msg, msgLen, "unsupported command");
        return false;
    }
    if (!applied) { copyText(msg, msgLen, "apply failed"); return false; }
    engine->reconcileProject();
    engine->recordStep(std::string("AI: ") + aiCommandTypeToString(cmd.type));
    copyText(msg, msgLen, serializeAiCommandPreview(cmd));
    return true;
}

namespace {
std::vector<int> huiTrackIndices(NCEngine* engine) {
    std::vector<int> result;
    if (!engine) return result;
    for (int i = 0; i < static_cast<int>(engine->project.tracks.size()); ++i) {
        const auto& track = engine->project.tracks[static_cast<size_t>(i)];
        if (track.trackType != "master" && track.trackType != "monitor") result.push_back(i);
    }
    return result;
}
int huiMasterIndex(NCEngine* engine) {
    if (!engine) return -1;
    for (int i = 0; i < static_cast<int>(engine->project.tracks.size()); ++i)
        if (engine->project.tracks[static_cast<size_t>(i)].trackType == "master") return i;
    return -1;
}
float huiDbToNormalized(float db) {
    if (db <= -60.0f) return 0.0f;
    return std::clamp((db + 60.0f) / 72.0f, 0.0f, 1.0f);
}
int huiEventCode(neuracoust::daw::HuiActionType type) {
    using T = neuracoust::daw::HuiActionType;
    switch (type) {
    case T::Fader: return 1; case T::PanDelta: return 2; case T::Select: return 3;
    case T::Mute: return 4; case T::Solo: return 5; case T::RecordArm: return 6;
    case T::Play: return 7; case T::Stop: return 8; case T::Record: return 9;
    case T::Rewind: return 10; case T::FastForward: return 11;
    default: return 0;
    }
}
}

int nc_hui_input_count(NCEngine* engine) {
    return engine ? static_cast<int>(engine->huiMidi.inputs().size()) : 0;
}
int nc_hui_output_count(NCEngine* engine) {
    return engine ? static_cast<int>(engine->huiMidi.outputs().size()) : 0;
}
void nc_hui_input_id(NCEngine* engine, int index, char* out, size_t len) {
    const auto list = engine ? engine->huiMidi.inputs() : std::vector<neuracoust::daw::ControlSurfaceMidiEndpoint>{};
    copyText(out, len, index >= 0 && index < static_cast<int>(list.size()) ? list[static_cast<size_t>(index)].id : "");
}
void nc_hui_input_name(NCEngine* engine, int index, char* out, size_t len) {
    const auto list = engine ? engine->huiMidi.inputs() : std::vector<neuracoust::daw::ControlSurfaceMidiEndpoint>{};
    copyText(out, len, index >= 0 && index < static_cast<int>(list.size()) ? list[static_cast<size_t>(index)].name : "");
}
void nc_hui_output_id(NCEngine* engine, int index, char* out, size_t len) {
    const auto list = engine ? engine->huiMidi.outputs() : std::vector<neuracoust::daw::ControlSurfaceMidiEndpoint>{};
    copyText(out, len, index >= 0 && index < static_cast<int>(list.size()) ? list[static_cast<size_t>(index)].id : "");
}
void nc_hui_output_name(NCEngine* engine, int index, char* out, size_t len) {
    const auto list = engine ? engine->huiMidi.outputs() : std::vector<neuracoust::daw::ControlSurfaceMidiEndpoint>{};
    copyText(out, len, index >= 0 && index < static_cast<int>(list.size()) ? list[static_cast<size_t>(index)].name : "");
}
bool nc_hui_connect(NCEngine* engine, const char* inputId, const char* outputId) {
    if (!engine) return false;
    engine->huiEvents.clear(); engine->huiBankOffset = 0;
    engine->huiLastFaders.fill(-1); engine->huiLastFlags.fill(-1);
    engine->huiLastNames.fill("");
    const bool ok = engine->huiMidi.connect(inputId ? inputId : "", outputId ? outputId : "");
    if (ok) engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::keepAlive().bytes);
    return ok;
}
void nc_hui_disconnect(NCEngine* engine) { if (engine) engine->huiMidi.disconnect(); }
bool nc_hui_connected(NCEngine* engine) { return engine && engine->huiMidi.connected(); }
void nc_hui_status(NCEngine* engine, char* out, size_t len) {
    copyText(out, len, engine ? engine->huiMidi.statusMessage() : "연결 안 됨");
}
bool nc_hui_next_event(NCEngine* engine, NCHuiEvent* out) {
    if (!engine || !out || !engine->huiMidi.connected()) return false;
    const auto tracks = huiTrackIndices(engine);
    for (const auto& message : engine->huiMidi.consumeMessages()) {
        const auto action = engine->huiProtocol.decode(message);
        if (!action) continue;
        using T = neuracoust::daw::HuiActionType;
        if (action->type == T::BankLeft || action->type == T::BankRight ||
            action->type == T::ChannelLeft || action->type == T::ChannelRight) {
            if (!action->pressed) continue;
            const int step = (action->type == T::BankLeft || action->type == T::BankRight) ? 8 : 1;
            const int direction = (action->type == T::BankLeft || action->type == T::ChannelLeft) ? -1 : 1;
            engine->huiBankOffset = std::clamp(engine->huiBankOffset + direction * step,
                                               0, std::max(0, static_cast<int>(tracks.size()) - 1));
            engine->huiLastFaders.fill(-1); engine->huiLastFlags.fill(-1); engine->huiLastNames.fill("");
            continue;
        }
        NCHuiEvent event{};
        event.type = huiEventCode(action->type);
        event.value = action->value; event.pressed = action->pressed;
        if (action->channel == 8) event.trackIndex = huiMasterIndex(engine);
        else if (action->channel >= 0 && engine->huiBankOffset + action->channel < static_cast<int>(tracks.size()))
            event.trackIndex = tracks[static_cast<size_t>(engine->huiBankOffset + action->channel)];
        else event.trackIndex = -1;
        if (event.type) engine->huiEvents.push_back(event);
    }
    if (engine->huiEvents.empty()) return false;
    *out = engine->huiEvents.front(); engine->huiEvents.pop_front(); return true;
}

void nc_hui_sync(NCEngine* engine, bool transportRunning, bool recording) {
    if (!engine || !engine->huiMidi.connected()) return;
    const auto tracks = huiTrackIndices(engine);
    for (int channel = 0; channel < 8; ++channel) {
        const int pos = engine->huiBankOffset + channel;
        if (pos >= static_cast<int>(tracks.size())) continue;
        const auto& track = engine->project.tracks[static_cast<size_t>(tracks[static_cast<size_t>(pos)])];
        const float fader = huiDbToNormalized(track.volumeDb);
        if (std::abs(fader - engine->huiLastFaders[static_cast<size_t>(channel)]) > 0.0005f) {
            engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::fader(channel, fader).bytes);
            engine->huiLastFaders[static_cast<size_t>(channel)] = fader;
        }
        const int flags = (track.recordArmed ? 1 : 0) | (track.solo ? 2 : 0) | (track.muted ? 4 : 0);
        if (flags != engine->huiLastFlags[static_cast<size_t>(channel)]) {
            for (int port = 0; port < 3; ++port)
                engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::switchLed(channel, port,
                                      (flags & (1 << port)) != 0).bytes);
            engine->huiLastFlags[static_cast<size_t>(channel)] = flags;
        }
        if (track.name != engine->huiLastNames[static_cast<size_t>(channel)]) {
            engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::displayText(channel, track.name).bytes);
            engine->huiLastNames[static_cast<size_t>(channel)] = track.name;
        }
    }
    const int master = huiMasterIndex(engine);
    if (master >= 0) {
        const float fader = huiDbToNormalized(engine->project.tracks[static_cast<size_t>(master)].volumeDb);
        if (std::abs(fader - engine->huiLastFaders[8]) > 0.0005f) {
            engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::fader(8, fader).bytes);
            engine->huiLastFaders[8] = fader;
        }
    }
    if (transportRunning != engine->huiLastPlaying) {
        engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::switchLed(0x0e, 3, transportRunning).bytes);
        engine->huiLastPlaying = transportRunning;
    }
    if (recording != engine->huiLastRecording) {
        engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::switchLed(0x0e, 4, recording).bytes);
        engine->huiLastRecording = recording;
    }
    if (++engine->huiKeepAliveCounter >= 25) {
        engine->huiKeepAliveCounter = 0;
        engine->huiMidi.send(neuracoust::daw::MackieHuiProtocol::keepAlive().bytes);
    }
}
