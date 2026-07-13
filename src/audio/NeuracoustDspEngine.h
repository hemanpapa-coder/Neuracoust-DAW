#pragma once

#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/ListenRoom.h"
#include "audio/LoudnessMeter.h"
#include "audio/ProjectAudioRenderer.h"
#include "audio/RealtimeAudioEngine.h"
#include "audio/RemoteDspAsyncStream.h"
#include "audio/WavFile.h"
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
                             const std::string& metronomeSubdivision = "auto");
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
    bool updateClipGain(const std::string& clipId, float gainDb);
    bool updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds);
    bool updateTrackMix(const std::string& trackName, float volumeDb, float pan);
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
    void renderInterleavedStereo(int64_t frameCount, std::vector<float>& interleavedStereo);
    AudioEngineStatus statusSnapshot() const;
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
        std::string remoteModuleId;
        std::vector<RemoteDspParameterValue> remoteParameters;
        std::unique_ptr<RemoteDspAsyncStream> remoteStream;
        int slotIndex = -1;
        std::vector<int> slotIndices;
        int64_t transitionSamplesRemaining = 0;
        int64_t transitionSamplesTotal = 0;
        bool protectDryWhenSilent = false;
    };

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
    bool applyRemoteMonitorDspLocked(std::vector<float>& interleavedStereo);
    void recordRemoteDspRoundTripLocked(double roundTripMs);
    void resetRemoteDspTelemetryLocked();
    void updateProjectMonitorPolicyLocked();
    void mixInputMonitorLocked(int64_t frameCount, std::vector<float>& interleavedStereo);
    void applyMonitorStationControlsLocked(std::vector<float>& interleavedStereo);
    void storeMetering(const std::vector<float>& interleavedStereo);
    void updateSpectrum(const std::vector<float>& interleavedStereo);
public:
    /// Monitor the computer's input source (e.g. BlackHole) through the monitor bus,
    /// instead of only the DAW master. Enables input capture so the source is heard.
    void setMonitorListenSource(bool active) { listenSourceActive_.store(active, std::memory_order_relaxed); }
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
    std::vector<float> inputMonitorBuffer_;
    std::string message_ = "Neuracoust DSP engine ready.";
    std::atomic<int64_t> playbackFrameForStatus_ {0};
    std::atomic<double> sampleRateForStatus_ {48000.0};
    std::atomic<float> outputPeakLeft_ {0.0f};
    std::atomic<float> outputPeakRight_ {0.0f};
    std::atomic<float> phaseCorrelation_ {0.0f};
    std::atomic<float> spectrumLow_ {0.0f};
    std::atomic<float> spectrumMid_ {0.0f};
    std::atomic<float> spectrumHigh_ {0.0f};
    std::atomic<bool> inputMonitorCaptureActive_ {false};
    std::atomic<bool> talkbackCaptureActive_ {false};
    std::atomic<bool> listenSourceActive_ {false};
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
    float lowBandState_ = 0.0f;
    float midBandState_ = 0.0f;
    // FFT spectrum analyzer state. Mono output accumulates into a window; when full a
    // Hann-windowed radix-2 FFT publishes magnitudes under a try-lock, so the UI reader
    // never blocks the render thread.
    mutable std::mutex spectrumMutex_;
    std::vector<float> spectrumAccumulator_;
    std::vector<float> spectrumBins_;
    // Goniometer: the most recent L/R sample pairs (interleaved), subsampled to a fixed
    // point count so the vectorscope stays cheap to draw.
    static constexpr int kGoniometerPoints = 512;
    std::vector<float> goniometerSamples_;
    // ITU-R BS.1770 loudness (momentary/short/integrated/LRA/true-peak).
    LoudnessMeter loudnessMeter_;
    double loudnessSampleRate_ = 0.0;
    float monitorStationGainSmoothed_ = 1.0f;
    bool monitorStationGainInitialized_ = false;
    bool remoteDspMonitorActive_ = false;
    std::string monitorDspTransitionFromMode_;
    std::string monitorDspTransitionToMode_;
    int64_t monitorDspTransitionSamplesRemaining_ = 0;
    int64_t monitorDspTransitionSamplesTotal_ = 0;
    int64_t monitorDspModuleTransitionSamplesRemaining_ = 0;
    int64_t monitorDspModuleTransitionSamplesTotal_ = 0;
    double remoteDspRoundTripMs_ = 0.0;
    double remoteDspPreviousRoundTripMs_ = 0.0;
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
