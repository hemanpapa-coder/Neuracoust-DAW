#pragma once

#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/MonitorFirEq.h"
#include "audio/AudioInterfaceModeler.h"
#include "audio/ParametricEq.h"
#include "audio/ListenRoom.h"
#include "audio/LoudnessMeter.h"
#include "audio/ProjectAudioRenderer.h"
#include "audio/RealtimeAudioEngine.h"
#include "audio/RemoteDspAsyncStream.h"
#include "audio/RecordingTake.h"
#include "audio/WavFile.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace neuracoust::daw {

class NeuracoustDspEngine {
public:
    bool configure(const AudioEngineSettings& settings, int maxBlockSize, std::string& error);
    void resetRuntime();

    void setTestToneEnabled(bool enabled);
    void setMetronomeEnabled(bool enabled,
                             int tempoBpm,
                             const std::vector<TempoMarkerState>& tempoMap = {},
                             int timeSignatureNumerator = 4,
                             int timeSignatureDenominator = 4,
                             const std::string& grooveFeel = "straight",
                             double grooveSwingAmount = 0.0,
                             const std::vector<TimeSignatureMarkerState>& timeSignatureMap = {},
                             const std::string& metronomeSubdivision = "auto",
                             double metronomeGain = 1.0,
                             const std::string& metronomeSound = "beep",
                             bool metronomeAccentFirst = true);
    void setMetronomeAccentPattern(const std::vector<float>& pattern);
    void setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled);
    void setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer);
    void setListenRoomSettings(const ListenRoomSettings& settings);
    void setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb = -20.0f, const std::string& talkbackRoute = "listen_room");
    void setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, float volumeDb) {
        setMonitorStationControls(mono, listenMode, swapLeftRight, invertLeft, invertRight, mute, dim, false, 0.0f, volumeDb, -20.0f, "listen_room");
    }
    bool loadAudioFile(const std::string& path, std::string& error);
    bool loadProject(const ProjectDocument& project, std::string& error);
    bool updateProject(const ProjectDocument& project, std::string& error);
    // Glitch-free graph swap: call begin BEFORE a structural change (add track/bus/send, send
    // pre-post, clip-gain commit) — it fades the monitor to silence (bounded wait for the render
    // to reach it) so the reconcile's discontinuity happens at silence — then end AFTER, which
    // fades the monitor back in and masks the new graph's state reset. Plain clip edits don't use
    // these; their path is already declicked.
    void beginGraphChangeDeclick();
    void endGraphChangeDeclick();
    // Plugin delay compensation applied to a specific track/bus route, in samples (0 = none / PDC off).
    int routeDelayCompensationSamplesFor(const std::string& routeName);
    // Lightweight monitor-EQ push: reconfigures only the EQ (state preserved), NOT the whole
    // render graph — so dragging a band's frequency doesn't glitch the audio the way a full
    // reconcileProject() per drag frame does.
    void updateMonitorEq(const std::vector<MonitorEqBandState>& bands);
    // Linear-phase FIR monitor EQ: designed from a target curve at the engine's real sample rate.
    // When active it replaces the biquad EQ in the monitor path (an empty curve clears it back to
    // the biquad). numTaps sets resolution vs latency (latency = numTaps/2 samples).
    void updateMonitorFir(const ResponseCurve& curveDb, int numTaps);
    // Nonlinear interface modeling (2단계): a Chebyshev waveshaper seeded from measured H2–H7. An
    // empty/all-zero coefficient list (or mix 0) bypasses it. Applied in the monitor path after EQ.
    void updateInterfaceModeler(const std::vector<double>& harmonics, double mix);
    // Samples of latency the active monitor FIR adds (0 when on the biquad path).
    int monitorFirLatencySamples() const;
    // Magnitude (dB) of whatever monitor EQ is live — FIR if active, else the biquad — for the UI.
    double monitorEqMagnitudeDb(double frequencyHz) const;
    // Lightweight aux-send level push: patches only the live render plan's send gain, NOT the
    // whole graph — so dragging a send level during playback doesn't reconcile and drop out.
    void updateTrackSendGain(const std::string& trackName, int slot, float gainDb);
    bool updateClipGain(const std::string& clipId, float gainDb);
    bool updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds);
    // In-place clip move for a live drag: slides the clip in the render plan with no rebuild.
    bool updateClipStart(const std::string& clipId, double startSeconds);
    // In-place clip TRIM for a live drag: patches start + duration + source offset in the render plan
    // with no rebuild, so lengthening/shortening a clip while playing never stops or gaps the music.
    bool updateClipBounds(const std::string& clipId, double startSeconds, double durationSeconds,
                          double sourceOffsetSeconds);
    bool updateTrackMix(const std::string& trackName, float volumeDb, float pan);
    // Live-push a track's built-in console channel (EQ/dynamics params) into the render plan
    // without rebuilding it — so a knob drag doesn't reconcile the whole project 60×/s (which clicks).
    bool updateTrackConsoleChannel(const std::string& trackName, const ConsoleChannelState& console);
    bool updateTrackSendSlot(const std::string& trackName, size_t sendIndex, const TrackSendState& send);
    bool updateTrackInsertBypassState(const std::string& trackName, size_t insertIndex, bool bypassed);
    bool updateMasterInsertBypassState(size_t insertIndex, bool bypassed);
    bool updateTrackPlaybackState(const std::string& trackName, bool muted, bool solo);
    bool updateTrackRealtimeState(const std::string& trackName, bool recordArmed, bool inputMonitoring, bool muted, bool solo);
    bool updateMasterVst3Parameter(size_t insertIndex,
                                   uint32_t parameterId,
                                   const std::string& displayName,
                                   double normalizedValue);
    bool updateTrackVst3Parameter(const std::string& trackName,
                                  size_t insertIndex,
                                  uint32_t parameterId,
                                  const std::string& displayName,
                                  double normalizedValue);
    // Live push for an instrument-rack slot's parameter: the renderer reads instrument parameters
    // from the plan each block, so patching the plan here is heard on the next block with no reconcile
    // (no processor rebuild, no audible gap while turning an instrument knob).
    bool updateInstrumentVst3Parameter(const std::string& trackName,
                                       size_t slotIndex,
                                       uint32_t parameterId,
                                       const std::string& displayName,
                                       double normalizedValue);
    bool updateInstrumentComponentState(const std::string& trackName,
                                        size_t slotIndex,
                                        const std::string& componentStateBase64);
    bool updateMonitorSpeakerVst3Parameter(int speakerSlot,
                                           size_t insertIndex,
                                           uint32_t parameterId,
                                           const std::string& displayName,
                                           double normalizedValue);
    void queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events);
    void setTransportRunning(bool running);
    void setTransportRecordingActive(bool active);
    void rewind();
    void seek(double seconds);
    void armSeekRamp();

    void pushInputMonitorInterleaved(const float* samples, int64_t frameCount, int channels);
    /// Reference tap (other-apps monitoring) feed. Kept in its OWN FIFO (referenceBuffer_), NOT the
    /// mic's inputMonitorBuffer_, so talkback/record-arm engaging the mic never collides with — or
    /// re-primes — the reference stream. Always stereo.
    void pushReferenceInterleaved(const float* interleavedStereo, int64_t frameCount);

    // Instrument-editor monitor: audio rendered by an OPEN instrument editor's own
    // plug-in instance (GUI keyboard clicks, forwarded live MIDI), mixed into the
    // monitor path like input monitoring. `trackName` scales the mix by that
    // track's fader/pan so it sits where the render instance would.
    void setEditorInstrumentMonitor(bool active, const std::string& trackName);
    void pushEditorInstrumentMonitorInterleaved(const float* samples, int64_t frameCount);

    // Acoustic measurement (roadmap ②b): emit `signal` out one output channel (0=L,1=R) while
    // capturing the mic, so the caller can deconvolve to the system's response. The signal
    // should already include a trailing tail of silence for the room decay. Requires input
    // monitoring so the mic reaches pushInputMonitorInterleaved.
    void startMeasurement(int channel, std::vector<float> signal);
    void cancelMeasurement();
    bool measurementActive() const { return measurementActive_.load(std::memory_order_relaxed); }
    double measurementProgress() const;                // 0..1
    std::vector<float> takeMeasurementCapture();        // stops and returns the mono capture
    // Loopback-measurement channel selection: which physical output DAC channel the sweep exits
    // (-1 = follow the normal monitor route) and which input ADC channel to capture. Interface
    // measurement patches DAC N → ADC M; room measurement leaves these at the defaults.
    void setMeasurementChannels(int outputChannel, int inputChannel) {
        measurementOutputChannel_.store(outputChannel, std::memory_order_relaxed);
        measurementInputChannel_.store(std::max(0, inputChannel), std::memory_order_relaxed);
    }
    int measurementOutputChannel() const { return measurementOutputChannel_.load(std::memory_order_relaxed); }
    // Pre-measurement level check: force input capture so the chosen ADC channel meters live,
    // letting the user set loopback gain (avoid ADC clipping) before running the sweep.
    void setMeasurementLevelCheck(bool on);
    bool measurementLevelCheck() const { return measurementLevelCheck_.load(std::memory_order_relaxed); }
    float measurementInputPeak() const { return measurementInputPeak_.load(std::memory_order_relaxed); }
    // True (undecayed) max input peak captured during the last sweep — for clip detection so the
    // finish can tell the user the measurement is valid vs. clipped/too-low.
    float measurementSweepPeak() const { return measurementSweepPeak_.load(std::memory_order_relaxed); }

    // Talkback mic channel: a talkback mic is one input channel (e.g. ch2 on an interface whose
    // ch1 carries the singer). The talkback path captures that channel mono and centers it on the
    // monitor / listen-room, so the voice is not stuck on one side and does not pull in the singer.
    void setTalkbackInputChannel(int channel) { talkbackInputChannel_.store(std::max(0, channel), std::memory_order_relaxed); }
    int  talkbackInputChannel() const { return talkbackInputChannel_.load(std::memory_order_relaxed); }
    // Per-channel input activity for the talkback channel picker ("which mics are live"): a decayed
    // peak per physical input channel, metered whenever the input is flowing (input monitoring on,
    // talkback engaged, or a measurement). Idle (input queue closed) reports 0.
    int   inputChannelCount() const { return inputChannelCount_.load(std::memory_order_relaxed); }
    float inputChannelPeak(int channel) const {
        if (channel < 0 || channel >= kMaxMeteredInputChannels) return 0.0f;
        return inputChannelPeak_[static_cast<size_t>(channel)].load(std::memory_order_relaxed);
    }

    void renderInterleavedStereo(int64_t frameCount, std::vector<float>& interleavedStereo);
    AudioEngineStatus statusSnapshot() const;
    // Fills `status` reusing its vector capacity; caller MUST hold mutex_. Used by the render's
    // throttled publish into statusShadow_.
    void populateStatusLocked(AudioEngineStatus& status) const;
    std::string lastMessage() const;
    int activeVst3MasterInsertCount() const;
    int activeVst3TrackInsertCount() const;
    int activeRemoteDspTrackInsertCount() const;

    // FFT spectrum for the analyzer: kSpectrumBins log-usable magnitude bins (0..1),
    // published from the realtime metering path. Copies at most `count` bins.
    static constexpr int kSpectrumBins = 1024;
    int spectrumBinCount() const;
    void copySpectrumBins(float* out, int count) const;
    // Feeds a synthetic tone through the FFT and checks the peak lands in the right bin.
    // Returns 0 on success, non-zero on failure. Used by a ctest.
    static int runSpectrumSelfTest();

private:
    struct RealtimeTrackInsertChain {
        std::string trackName;
        std::string signature; // identity of this local chain; lets a rebuild reuse the running worker
        RealtimeMasterInsertChain chain;
        unsigned int latencySamples = 0;
        bool remoteDsp = false;
        /// Which machine this chain streams to ("nds" | "remote_external" | "auto"), resolved
        /// from the inserts' own assignment. One chain per machine per track.
        std::string remoteMode;
        std::string remoteModuleId;
        std::vector<RemoteDspParameterValue> remoteParameters;
        std::unique_ptr<RemoteDspAsyncStream> remoteStream;
        int slotIndex = -1;
        std::vector<int> slotIndices;
        int64_t transitionSamplesRemaining = 0;
        int64_t transitionSamplesTotal = 0;
        bool protectDryWhenSilent = false;
    };

    /// A channel whose console strip runs on a remote machine. One stream per track, addressed by
    /// the same NART module the node builds from this library's ConsoleChannelProcessor — so the
    /// strip is the same code, just executed elsewhere.
    struct RemoteConsoleStrip {
        std::string mode;                       // "nds" | "remote_external" | "auto"
        std::vector<RemoteDspParameterValue> parameters;
        std::unique_ptr<RemoteDspAsyncStream> stream;
    };
    std::map<std::string, RemoteConsoleStrip> remoteConsoleStrips_;
    std::vector<float> remoteConsoleProcessedBlock_;
    int activeRemoteConsoleStripCount_ = 0;

    /// One master insert offloaded to a node: its module, its parameters, its stream. The chain
    /// is SERIAL, so the stages run in order, each through its own session on the node.
    struct RemoteMasterInsertStage {
        std::string moduleId;
        std::vector<RemoteDspParameterValue> parameters;
        std::unique_ptr<RemoteDspAsyncStream> stream;
    };
    std::vector<RemoteMasterInsertStage> remoteMasterInserts_;
    std::string remoteMasterMode_;
    std::vector<float> remoteMasterScratch_;

    void prepareRemoteMasterInsertsLocked(int maxBlockSize);
    bool processRemoteMasterInsertsLocked(std::vector<float>& interleavedStereo);

    /// Remote summing buses in REALTIME (remote-mixer M2): one session per bus, engaged when
    /// 믹서·버스 resolves to a reachable machine. Synchronous with a short timeout on purpose —
    /// the local fallback produces bit-identical samples (same buffers, same order), so a missed
    /// block costs provenance and nothing audible; a one-block async pipeline would instead
    /// time-shift send paths against direct paths and comb on transients.
    std::string remoteMixerMode_;
    std::map<std::string, std::unique_ptr<RemoteMixSession>> realtimeMixSessions_;
    // PER BUS: a shared streak let one healthy bus keep resetting it, so a failing bus never
    // reached backoff and burned its timeout every block instead.
    std::map<std::string, int> remoteMixerMissStreaks_;
    std::map<std::string, uint32_t> remoteMixerProbeCountdowns_;

    void prepareRemoteMixerLocked();
    bool processRealtimeBusSumLocked(const std::string& busName,
                                     const std::deque<std::vector<float>>& contributions,
                                     std::vector<float>& summed);

    void prepareRemoteConsoleStripsLocked(int maxBlockSize);
    /// Declare each remote strip's crossing into the mixer's delay compensation, so a channel
    /// that leaves the host does not simply arrive late against the rest of the mix.
    void realignRemoteConsoleStripsLocked(int maxBlockSize);
    bool processRemoteConsoleStripLocked(const std::string& routeName,
                                         std::vector<float>& interleavedStereo);

    bool prepareRealtimeInsertChainLocked(int maxBlockSize, std::string& error);
    void warmRouteInsertChainsLocked();
    void applyRealtimeTrackInsertChainsLocked(int64_t startFrame,
                                             int64_t frameCount,
                                             std::vector<float>& interleavedStereo);
    bool remoteMonitorDspRequestedLocked() const;
    bool monitorDspModeRequestsRemoteLocked(const std::string& mode) const;
    bool projectMonitorDspCanRenderInGraphLocked() const;
    void syncProjectMonitorDspRenderPathLocked();
    bool applyMonitorDspPathLocked(const std::string& mode, std::vector<float>& interleavedStereo);
    void applyMonitorDspTransitionLocked(std::vector<float>& interleavedStereo);
    void applyMonitorDspModuleTransitionLocked(std::vector<float>& interleavedStereo);
    bool prepareMonitorOutputInsertChainLocked(int maxBlockSize, std::string& error);
    void applyMonitorOutputInsertChainLocked(std::vector<float>& interleavedStereo);
    void applyLocalMonitorDspLocked(std::vector<float>& interleavedStereo);
    void applyLocalMonitorDspLocked(MonitorDspProcessor& processor, std::vector<float>& interleavedStereo);
    void configureMonitorEqLocked(const ProjectDocument& project, double sampleRate);
    bool applyRemoteMonitorDspLocked(std::vector<float>& interleavedStereo);
    void recordRemoteDspRoundTripLocked(double roundTripMs);
    void resetRemoteDspTelemetryLocked();
    void updateProjectMonitorPolicyLocked();
    void mixInputMonitorLocked(int64_t frameCount, std::vector<float>& interleavedStereo);
    void mixEditorInstrumentMonitorLocked(int64_t frameCount, std::vector<float>& interleavedStereo);
    void applyMonitorStationControlsLocked(std::vector<float>& interleavedStereo);
    void storeMetering(const std::vector<float>& interleavedStereo);
    void updateSpectrum(const std::vector<float>& interleavedStereo);
public:
    /// Monitor the computer's input source (e.g. BlackHole) through the monitor bus,
    /// instead of only the DAW master. Enables input capture so the source is heard.
    /// Re-runs the monitor policy so inputMonitorCaptureActive_ picks up the change now,
    /// not on the next unrelated policy update.
    void setMonitorListenSource(bool active) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-prime the FIFO cushion on every A/B *into* the reference, so switching back after
        // auditioning the master starts cleanly instead of reading a stale/near-empty buffer.
        if (active) listenJustEngaged_.store(true, std::memory_order_relaxed);
        listenSourceActive_.store(active, std::memory_order_relaxed);
        updateProjectMonitorPolicyLocked();
    }
    /// Reference-hold: keep the process tap running (tapped apps muted) whether or not you are
    /// currently listening to them, so A/B-ing to the master never leaks their sound. Disarming
    /// also drops the listening state.
    void setMonitorReferenceArmed(bool armed) {
        std::lock_guard<std::mutex> lock(mutex_);
        referenceTapArmed_.store(armed, std::memory_order_relaxed);
        if (!armed) {
            listenSourceActive_.store(false, std::memory_order_relaxed);
            listenJustEngaged_.store(false, std::memory_order_relaxed);
        }
        updateProjectMonitorPolicyLocked();
    }
    /// Input-monitor the tap additively on the master during a Record punch (auto-input).
    void setTapInputMonitor(bool active) {
        tapInputMonitorActive_.store(active, std::memory_order_relaxed);
    }
    /// Input-monitor the tap because a tap-input track's Input-Monitor toggle is on (heard always).
    void setTapInputHold(bool active) {
        tapInputHoldActive_.store(active, std::memory_order_relaxed);
    }
    /// DIAG (temporary): record the reference tap's nominal rate vs the engine output rate.
    void logReferenceRates(double tapRate, double outRate);
    /// Audio recording-to-disk (V1). Captures the armed track's input from the frames already
    /// arriving on the input thread — source 1 = physical mic (channels [offset, offset+count) of
    /// pushInputMonitorInterleaved), source 2 = reference tap (stereo, pushReferenceInterleaved).
    /// endRecording saves the accumulated take to a WAV and reports its duration/channels.
    void beginRecording(int source, int channelOffset, int channels, int sampleRate);
    bool endRecording(const std::string& path, int bitDepth, std::string& error,
                      double& outDurationSeconds, int& outChannels);
    /// Stop capturing and DROP the take without saving — the background pass was never committed
    /// (the user played over a record-armed track but never pressed Record).
    void cancelRecording();
    bool recordingActive() const { return recordingActive_.load(std::memory_order_relaxed); }
    /// True only while recording a PHYSICAL input (source 1) — the mic AudioQueue must stay open
    /// to capture it. Recording the reference tap (source 2) needs no mic.
    bool recordingWantsMic() const {
        return recordingActive_.load(std::memory_order_relaxed) &&
               recordSource_.load(std::memory_order_relaxed) == 1;
    }
    /// Live take metering for the timeline's growing waveform during a record: the seconds captured
    /// so far and a coarse L/R abs-max peak per fixed sample count (one bucket = 2 floats).
    /// Peaks are read INCREMENTALLY (copyRecordLivePeaksSince) so the read lock is always tiny —
    /// a full-array copy grew with the take and starved the capture thread (dropouts/pitch wobble).
    double recordLiveSeconds() const;
    int recordLivePeakCount() const;                                        // buckets available
    int copyRecordLivePeaksSince(int fromBucket, float* outLR, int maxBuckets) const;  // fills 2*n
    int recordChannels() const;
    int recordPeakSamples() const { return kRecordPeakSamples; }
    /// Insert tail on stop: <0 always on, 0 cut, >0 ring out N seconds.
    void setInsertTailOnStopSeconds(double seconds) {
        insertTailOnStopSeconds_.store(seconds, std::memory_order_relaxed);
    }
private:
    void publishListenRoomLocked(const std::vector<float>& interleavedStereo);
    void storeTrackInsertMeterLocked(const std::string& trackName,
                                     int slotIndex,
                                     float inputPeak,
                                     float outputPeak);
    void storeTrackInsertOutputParametersLocked(const std::string& trackName,
                                                int slotIndex,
                                                const std::vector<Vst3ParameterValueState>& parameters);
    void applyReloadCrossfadeLocked(std::vector<float>& interleavedStereo);
    void resetMeteringLocked();
    void resetTrackMetersLocked();
    void clearTrackInsertMetersLocked();
    void suppressTrackInsertMetersForGraphChangeLocked();
    void storeTrackMetersLocked(const ProjectAudioBlockMeters& meters);
    void armSeekRampLocked(double sampleRate);
    void applySeekRampLocked(std::vector<float>& interleavedStereo);

    mutable std::mutex mutex_;
    mutable std::mutex inputMonitorMutex_;
    // The render thread publishes a full status snapshot here (throttled, try_lock only) so the
    // ~30 Hz UI poll can read it WITHOUT ever taking mutex_ — see statusSnapshot(). This removes the
    // priority inversion (UI thread descheduled under load while holding mutex_ mid-snapshot stalled
    // the realtime render → dropouts).
    mutable std::mutex statusShadowMutex_;
    mutable AudioEngineStatus statusShadow_;
    int statusPublishCounter_ = 0;
    AudioEngineSettings settings_;
    WavAudioData playback_;
    ProjectAudioRenderPlan projectPlan_;
    ProjectAudioRenderState projectRenderState_;
    std::vector<float> projectBlock_;
    std::vector<float> projectSegmentBlock_;
    ProjectAudioBlockMeters projectMeters_;
    ProjectAudioBlockMeters projectSegmentMeters_;
    RealtimeMasterInsertChain realtimeInsertChain_;
    RealtimeMasterInsertChain monitorOutputInsertChain_;
    std::vector<RealtimeTrackInsertChain> realtimeTrackInsertChains_;
    int monitorOutputInsertActiveSlot_ = -1;
    std::string realtimeInsertGraphSignature_;
    std::vector<float> trackInsertDryBlock_;
    std::vector<float> trackInsertCompensatedInputBlock_;
    std::vector<float> trackInsertProcessedBlock_;
    std::vector<float> remoteTrackInsertProcessedBlock_;
    std::vector<float> remoteDspProcessedBlock_;
    std::vector<float> monitorOutputInsertDryFallback_;
    std::vector<float> previousOutputBlock_;
    int64_t seekRampSamplesRemaining_ = 0;
    int64_t seekRampSamplesTotal_ = 0;
    int64_t reloadCrossfadeSamplesRemaining_ = 0;
    int64_t reloadCrossfadeSamplesTotal_ = 0;
    RemoteDspAsyncStream remoteMonitorDspStream_;
    ListenRoomSender listenRoomSender_;
    std::vector<float> monitorDspTransitionFromBlock_;
    std::vector<float> monitorDspTransitionToBlock_;
    MonitorDspProcessor monitorProcessor_;
    MonitorDspProcessor previousMonitorProcessor_;
    // User-built monitor parametric EQ (0–64 bands), applied in the monitor path only.
    ParametricEq monitorEq_;
    // Optional linear-phase FIR monitor EQ; when active it supersedes monitorEq_ in the monitor
    // path (exact full-band magnitude matching at the cost of numTaps/2 samples of latency).
    MonitorFirEq monitorFir_;
    AudioInterfaceModeler interfaceModeler_;
    double phase_ = 0.0;
    int64_t playbackFrame_ = 0;
    int64_t realtimeProcessFrame_ = 0;
    int maxBlockSize_ = 256;
    bool configured_ = false;
    bool lowLatencyRecordMonitoringActive_ = false;
    unsigned int delayCompensationSamples_ = 0;
    bool physicalInputMonitoringActive_ = false;
    int recordArmedTrackCount_ = 0;
    float recordMonitorVolumeDb_ = 0.0f;
    float recordMonitorPan_ = 0.0f;
    bool recordMonitorMuted_ = false;
    int inputMonitorChannels_ = 0;
    float inputPeak_ = 0.0f;
    /// Per-side input peaks. The input is a stereo pair, so the meter shows it as one — the single
    /// combined figure could not tell a dead right channel from a quiet take.
    float inputPeakLeft_ = 0.0f;
    float inputPeakRight_ = 0.0f;
    std::vector<float> inputMonitorBuffer_;
    // Reference tap FIFO — separate from the mic buffer so talkback toggling never disturbs it.
    std::vector<float> referenceBuffer_;
    // True while the monitor is feeding the reference (either A/B listening, or record-monitoring a
    // tap-input track on top of the master). Used to re-prime the cushion on the feeding-start edge.
    bool referenceFeedActive_ = false;
    // Audio recording-to-disk: the take accumulates on the input thread; saved on stop.
    std::atomic<bool> recordingActive_ {false};
    std::atomic<int> recordSource_ {0};   // 0 none, 1 mic, 2 reference tap
    int recordChannelOffset_ = 0;         // device channel offset for a physical-mic record
    int recordChannels_ = 2;
    std::unique_ptr<RecordingTake> recordTake_;
    mutable std::mutex recordMutex_;      // guards recordTake_ + live peaks create/append/finish/read
    // Live take peaks for the timeline: coarse L/R abs-max per kRecordPeakSamples samples,
    // stored interleaved (2 floats per bucket) so a stereo take draws two envelopes live.
    static constexpr int kRecordPeakSamples = 512;
    std::vector<float> recordLivePeaks_;   // {L,R} per bucket
    float recordPeakAccumL_ = 0.0f;
    float recordPeakAccumR_ = 0.0f;
    int recordPeakFill_ = 0;
    void accumulateRecordPeaksLocked(const float* interleaved, int64_t frames, int channels);
    // Talkback mic = one input channel, captured mono and shadowing inputMonitorBuffer_ frame-for-
    // frame while talkback is engaged (see pushInputMonitorInterleaved / mixInputMonitorLocked).
    static constexpr int kMaxMeteredInputChannels = 32;
    std::atomic<int> talkbackInputChannel_ {0};
    std::vector<float> talkbackMonoBuffer_;
    std::atomic<int> inputChannelCount_ {0};
    std::array<std::atomic<float>, kMaxMeteredInputChannels> inputChannelPeak_ {};
    // Talkback mic captured for the listen room when its route is listen_room-exclusive.
    // Rebuilt each render block in mixInputMonitorLocked, summed in publishListenRoomLocked.
    std::vector<float> talkbackListenRoomBlock_;
    std::vector<float> listenRoomMixBlock_;
    // Reference monitoring (listen source, e.g. BlackHole) runs on a clock independent of
    // the output, so it is varispeed-resampled to the output clock: the read advances by a
    // ratio that is nudged to hold the FIFO near a target depth. This locks input to output
    // with no drift, underrun, or drop — a fixed FIFO alone can only stutter.
    bool listenSourcePrerolling_ = true;
    double listenReadPosFrames_ = 0.0;   // fractional read position into the FIFO (frames)
    double listenResampleRatio_ = 1.0;   // input frames consumed per output frame
    double listenSmoothedDepth_ = 0.0;   // heavily low-passed FIFO depth (drives the ratio)
    // Instrument-editor monitor FIFO (see pushEditorInstrumentMonitorInterleaved).
    mutable std::mutex editorMonitorMutex_;
    std::vector<float> editorMonitorBuffer_;
    std::string editorMonitorTrackName_;
    std::atomic<bool> editorMonitorActive_ {false};
    std::string message_ = "Neuracoust DSP engine ready.";
    std::atomic<int64_t> playbackFrameForStatus_ {0};
    std::atomic<double> sampleRateForStatus_ {48000.0};
    // The monitor bus as the listener hears it in SHAPE — after listen mode (mono / M-S / phase),
    // the monitor DSP and the speaker sim — but before the monitor level (volume, dim, mute). The
    // transport meter reads this, so turning the monitor knob moves the sound, not the meter.
    std::atomic<float> monitorPrePeakLeft_ {0.0f};
    std::atomic<float> monitorPrePeakRight_ {0.0f};
    std::atomic<float> outputPeakLeft_ {0.0f};
    std::atomic<float> outputPeakRight_ {0.0f};
    std::atomic<float> phaseCorrelation_ {0.0f};
    float phaseCorrelationBallistics_ = 0.0f;   // render-thread meter smoothing (~100 ms)
    std::atomic<float> spectrumLow_ {0.0f};
    std::atomic<float> spectrumMid_ {0.0f};
    std::atomic<float> spectrumHigh_ {0.0f};
    std::atomic<bool> inputMonitorCaptureActive_ {false};

    // Acoustic measurement session. sweep/channel are set before active_ turns true and only
    // read on the audio thread thereafter; the capture buffer is mutex-guarded.
    std::atomic<bool> measurementActive_ {false};
    int measurementChannel_ = 0;
    std::atomic<int> measurementOutputChannel_ {-1};   // physical DAC channel (-1 = monitor route)
    std::atomic<int> measurementInputChannel_ {0};     // ADC channel to capture
    std::atomic<bool> measurementLevelCheck_ {false};  // metering the input for gain setup
    std::atomic<float> measurementInputPeak_ {0.0f};   // peak of the chosen ADC channel, 0..1
    std::atomic<float> measurementSweepPeak_ {0.0f};   // undecayed max during the sweep (clip check)
    double measurementTonePhase_ = 0.0;                // 1 kHz reference tone for level-check
    std::vector<float> measurementSignal_;
    int64_t measurementEmitPos_ = 0;
    bool measurementPrevInputMonitor_ = false;
    mutable std::mutex measurementMutex_;
    std::vector<float> measurementCapture_;
    std::atomic<int64_t> measurementProgressFrames_ {0};
    std::atomic<int64_t> measurementTotalFrames_ {0};
    std::atomic<bool> talkbackCaptureActive_ {false};
    std::atomic<bool> listenSourceActive_ {false};
    // Reference-hold: tap running + apps muted, independent of the A/B listening state above.
    std::atomic<bool> referenceTapArmed_ {false};
    // Input-monitor the tap on top of the master. Two independent sources make it audible:
    //  - tapInputMonitorActive_: an active Record PUNCH (auto-input — only while punched in).
    //  - tapInputHoldActive_: a tap-input track's Input-Monitor toggle (heard whenever it is on).
    std::atomic<bool> tapInputMonitorActive_ {false};
    std::atomic<bool> tapInputHoldActive_ {false};
    // Set on each A/B into the reference; the mix path consumes it to re-prime the FIFO cushion.
    std::atomic<bool> listenJustEngaged_ {false};
    // Insert tail on stop: <0 always on (default), 0 cut immediately, >0 ring out N sec.
    std::atomic<double> insertTailOnStopSeconds_ {-1.0};
    int64_t insertTailSamplesRemaining_ = 0;
    bool wasTransportRunning_ = false;
    // Live-monitor gating for instruments. A held key must keep the instrument rendering so
    // it sustains and its release tail rings; but an armed instrument with no key down must
    // go silent, or it "무조건 재생" — sounds forever with the transport stopped. Count the
    // notes the pump has queued; keep the monitor alive while any is held and for a short
    // release tail after the last one lifts, then stop rendering it.
    std::atomic<int> liveNotesHeld_ {0};
    int64_t liveMonitorTailSamplesRemaining_ = 0;
    std::atomic<bool> physicalInputMonitoringActiveForStatus_ {false};
    std::atomic<int> inputMonitorChannelsForStatus_ {0};
    std::atomic<float> inputPeakForStatus_ {0.0f};
    std::atomic<float> inputPeakLeftForStatus_ {0.0f};
    std::atomic<float> inputPeakRightForStatus_ {0.0f};
    float lowBandState_ = 0.0f;
    float midBandState_ = 0.0f;
    // FFT spectrum analyzer state. Mono output accumulates into a window; when full a
    // Hann-windowed radix-2 FFT publishes magnitudes under a try-lock, so the UI reader
    // never blocks the render thread.
    mutable std::mutex spectrumMutex_;
    std::vector<float> spectrumAccumulator_;
    std::vector<float> spectrumBins_;
    // Snapshot of the master output taken BEFORE the monitor path (input monitoring, monitor DSP,
    // monitor station volume) so the spectrum analyzer reflects the printed mix, not the level the
    // monitor knob happens to be at. Captured each render block right after the mix is summed.
    std::vector<float> spectrumSourceBlock_;
    // Goniometer: the most recent L/R sample pairs (interleaved), subsampled to a fixed
    // point count so the vectorscope stays cheap to draw.
    static constexpr int kGoniometerPoints = 512;
    std::vector<float> goniometerSamples_;
    // ITU-R BS.1770 loudness (momentary/short/integrated/LRA/true-peak).
    LoudnessMeter loudnessMeter_;
    double loudnessSampleRate_ = 0.0;
    float monitorStationGainSmoothed_ = 1.0f;
    bool monitorStationGainInitialized_ = false;
    // Graph-change declick state machine: 0=inactive, 1=fading out, 2=silent (safe to swap),
    // 3=post-swap silent hold (lets the new graph's transient pass under CoreAudio's buffer latency),
    // 4=fading back in.
    std::atomic<int> graphChangeDeclick_{0};
    float graphChangeDeclickGain_ = 1.0f;   // render-thread envelope, 0..1, multiplied into the monitor
    int graphChangeDeclickHold_ = 0;        // samples of post-swap silence still to hold
    bool remoteDspMonitorActive_ = false;
    std::string monitorDspTransitionFromMode_;
    std::string monitorDspTransitionToMode_;
    int64_t monitorDspTransitionSamplesRemaining_ = 0;
    int64_t monitorDspTransitionSamplesTotal_ = 0;
    int64_t monitorDspModuleTransitionSamplesRemaining_ = 0;
    int64_t monitorDspModuleTransitionSamplesTotal_ = 0;
    // Consecutive pin-drop-silent output samples, so the always-on monitor DSP simulation can
    // be skipped once the mix is silent and its filters have decayed — no DSP burned at idle.
    int64_t monitorDspSilentSamples_ = 0;
    double remoteDspRoundTripMs_ = 0.0;
    double remoteDspPreviousRoundTripMs_ = 0.0;
    // Reference-tap (다른 앱) FIFO health. The render wake-jitter meter only watches the OUTPUT
    // thread, so a starved or overflowing tap capture is invisible there even though it crackles.
    // Counted here and published so the meter has no blind spot.
    std::atomic<uint64_t> referenceUnderrunBlocks_ {0};
    std::atomic<uint64_t> referenceOverrunDrops_ {0};
    double remoteDspAverageRoundTripJitterUs_ = 0.0;
    double remoteDspMaxRoundTripJitterUs_ = 0.0;
    bool remoteDspRoundTripInitialized_ = false;
    int activeRemoteDspTrackInsertCount_ = 0;
    std::vector<std::string> trackInsertMeterTrackNames_;
    std::vector<int> trackInsertMeterSlotIndices_;
    std::vector<float> trackInsertInputPeaks_;
    std::vector<float> trackInsertOutputPeaks_;
    std::vector<std::string> trackInsertOutputParameterTrackNames_;
    std::vector<int> trackInsertOutputParameterSlotIndices_;
    std::vector<uint32_t> trackInsertOutputParameterIds_;
    std::vector<float> trackInsertOutputParameterValues_;
    int64_t trackInsertMeterSuppressSamples_ = 0;
};

} // namespace neuracoust::daw
