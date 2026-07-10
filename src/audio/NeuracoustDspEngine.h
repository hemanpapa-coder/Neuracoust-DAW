#pragma once

#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/ListenRoom.h"
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
    std::atomic<bool> physicalInputMonitoringActiveForStatus_ {false};
    std::atomic<int> inputMonitorChannelsForStatus_ {0};
    std::atomic<float> inputPeakForStatus_ {0.0f};
    float lowBandState_ = 0.0f;
    float midBandState_ = 0.0f;
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
