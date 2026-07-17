#pragma once

#include "audio/AudioDeviceModel.h"
#include "audio/ListenRoom.h"
#include "audio/RemoteDspServerClient.h"
#include "plugins/MonitorDspModules.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/ProjectDocument.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct AudioEngineSettings {
    double sampleRate = 48000.0;
    int bufferSize = 256;
    AudioDriverKind outputDriver = AudioDriverKind::Unknown;
    std::string outputDeviceId;
    std::string inputDeviceId;
    bool testToneEnabled = false;
    double testToneFrequency = 220.0;
    bool metronomeEnabled = false;
    int tempoBpm = 120;
    std::vector<TempoMarkerState> tempoMap;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    std::vector<TimeSignatureMarkerState> timeSignatureMap;
    std::string grooveFeel = "straight";
    double grooveSwingAmount = 0.0;
    std::string metronomeSubdivision = "auto";
    bool monitorDspEnabled = true;
    std::string monitorDspPathMode = "internal";
    RemoteDspServerSettings remoteDspServer = defaultRemoteDspServerSettings();
    std::vector<MonitorDspModule> monitorModules;
    bool delayCompensationEnabled = true;
    bool transportRunning = true;
    bool lowLatencyRecordMonitoringEnabled = true;
    bool transportRecordingActive = false;
    bool physicalInputAccessAllowed = false;
    int inputMonitorChannelCount = 2;
    int playbackStabilityBufferMultiplier = 2;
    bool performanceCoreIsolationEnabled = false;
    int requestedPerformanceCoreCount = 4;
    bool windowsProcessorAffinityEnabled = false;
    std::string windowsProcessorAffinityMode = "p_core_preferred";
    uint64_t windowsProcessorAffinityMask = 0;
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
    ListenRoomSettings listenRoom;
};

struct AudioEngineStatus {
    bool running = false;
    bool transportRunning = false;
    AudioDriverKind outputDriver = AudioDriverKind::Unknown;
    double sampleRate = 0.0;
    int outputChannels = 0;
    float outputPeakLeft = 0.0f;
    float outputPeakRight = 0.0f;
    float phaseCorrelation = 0.0f;
    float spectrumLow = 0.0f;
    float spectrumMid = 0.0f;
    float spectrumHigh = 0.0f;
    // Full FFT magnitude bins (0..1, dB-scaled) for the spectrum analyzer.
    std::vector<float> spectrumBins;
    // Recent L/R sample pairs (interleaved) for the goniometer / vectorscope.
    std::vector<float> goniometerSamples;
    // ITU-R BS.1770 loudness metering.
    float momentaryLufs = -70.0f;
    float shortTermLufs = -70.0f;
    float integratedLufs = -70.0f;
    float loudnessRange = 0.0f;
    float truePeakDb = -120.0f;
    std::vector<std::string> trackMeterNames;
    std::vector<float> trackPeakLeft;
    std::vector<float> trackPeakRight;
    std::vector<std::string> trackInsertMeterTrackNames;
    std::vector<int> trackInsertMeterSlotIndices;
    std::vector<float> trackInsertInputPeak;
    std::vector<float> trackInsertOutputPeak;
    std::vector<std::string> trackInsertOutputParameterTrackNames;
    std::vector<int> trackInsertOutputParameterSlotIndices;
    std::vector<uint32_t> trackInsertOutputParameterIds;
    std::vector<float> trackInsertOutputParameterValues;
    double playbackSeconds = 0.0;
    bool delayCompensationEnabled = true;
    int delayCompensationSamples = 0;
    double delayCompensationMs = 0.0;
    bool directMonitoringEnabled = true;
    bool lowLatencyRecordMonitoringActive = false;
    bool physicalInputMonitoringActive = false;
    /// "Listen to source" (e.g. BlackHole) routes a physical input through the monitor
    /// bus with no record-armed track, so the input queue must open for it too.
    bool listenSourceActive = false;
    int recordArmedTrackCount = 0;
    int inputChannels = 0;
    float inputPeak = 0.0f;
    int requestedBufferSize = 0;
    int playbackStabilityBufferSize = 0;
    std::string deviceName;
    std::string dspEngineName;
    std::string monitorPathDescription;
    std::string monitorDspPathMode = "internal";
    bool remoteDspMonitorActive = false;
    double remoteDspRoundTripMs = 0.0;
    double remoteDspAverageRoundTripJitterUs = 0.0;
    double remoteDspMaxRoundTripJitterUs = 0.0;
    RemoteDspCorePlan remoteDspCorePlan;
    int requestedPerformanceCoreCount = 4;
    uint64_t realtimeCallbackCount = 0;
    double realtimeAverageWakeJitterUs = 0.0;
    double realtimeMaxWakeJitterUs = 0.0;
    double realtimeMaxRenderDurationUs = 0.0;
    int realtimeLateWakeCount = 0;
    int activeRealtimeVst3MasterInsertCount = 0;
    int activeRealtimeVst3TrackInsertCount = 0;
    int activeRemoteDspTrackInsertCount = 0;
    int activeOfflineVst3TrackInsertCount = 0;
    ListenRoomStatus listenRoom;
    std::string message;
};

class RealtimeAudioEngine {
public:
    RealtimeAudioEngine();
    ~RealtimeAudioEngine();

    RealtimeAudioEngine(const RealtimeAudioEngine&) = delete;
    RealtimeAudioEngine& operator=(const RealtimeAudioEngine&) = delete;

    bool start(const AudioEngineSettings& settings = {});
    void stop();
    AudioEngineStatus status() const;
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
    void setPhysicalInputAccessAllowed(bool allowed);
    void setMonitorListenSource(bool active);
    void setInsertTailOnStopSeconds(double seconds);
    bool loadAudioFile(const std::string& path, std::string& error);
    bool loadProject(const ProjectDocument& project, std::string& error);
    bool updateProject(const ProjectDocument& project, std::string& error);
    void updateMonitorEq(const std::vector<MonitorEqBandState>& bands);
    void updateTrackSendGain(const std::string& trackName, int slot, float gainDb);
    void startMeasurement(int channel, std::vector<float> signal);
    void cancelMeasurement();
    bool measurementActive() const;
    double measurementProgress() const;
    std::vector<float> takeMeasurementCapture();
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
    // Live push for an instrument-rack slot's parameter: the renderer reads instrument parameters
    // from the plan each block, so patching the plan here is heard on the next block with no reconcile
    // (no processor rebuild, no audible gap while turning an instrument knob).
    bool updateInstrumentVst3Parameter(const std::string& trackName,
                                       size_t slotIndex,
                                       uint32_t parameterId,
                                       const std::string& displayName,
                                       double normalizedValue);
    bool updateMonitorSpeakerVst3Parameter(int speakerSlot,
                                           size_t insertIndex,
                                           uint32_t parameterId,
                                           const std::string& displayName,
                                           double normalizedValue);
    void queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events);
    // Instrument-editor monitor: mix audio rendered by an open instrument editor's own
    // plug-in instance into the monitor path (macOS CoreAudio engine only; no-op elsewhere).
    void setEditorInstrumentMonitor(bool active, const std::string& trackName);
    void pushEditorInstrumentMonitorInterleaved(const float* samples, int64_t frameCount);
    void setTransportRunning(bool running);
    void setTransportRecordingActive(bool active);
    void rewind();
    void seek(double seconds);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuracoust::daw
