#pragma once

#include "audio/AudioDeviceModel.h"
#include "audio/ListenRoom.h"
#include "audio/MonitorCorrection.h"   // ResponseCurve
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
    double metronomeGain = 1.0;            // linear level over the built-in click (0..2)
    std::string metronomeSound = "beep";   // beep / wood / rim / cowbell
    bool metronomeAccentFirst = true;      // accent the bar's downbeat (off = every beat equal)
    // Genre accent pattern: per-step gains (0..1) over one bar at the click subdivision. Empty =
    // the default bar/beat/sub-beat hierarchy. A 0 gain is a rest (no click on that step).
    std::vector<float> metronomeAccentPattern;
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
    float monitorVolumeDb = -12.0f;
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
    /// The mix leaving the master fader, BEFORE the monitor path (monitor EQ, speaker sim,
    /// monitor volume/dim/mute). outputPeak* is the device output and therefore follows the
    /// monitor knob; the mixer's Master meter must not, so it reads these instead.
    float masterBusPeakLeft = 0.0f;
    float masterBusPeakRight = 0.0f;
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
    std::vector<float> trackConsoleGainReductionDb;
    std::vector<float> trackConsoleGateGainReductionDb;
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
    /// "Listen to source" routes the reference tap through the monitor bus in place of the
    /// master. It is the A/B *listening* state — true only while you are actually hearing the
    /// other apps (not while auditioning the master with the tap still armed).
    bool listenSourceActive = false;
    /// Reference-hold: the process tap is running and the tapped apps are muted at their own
    /// output, independent of whether you are currently listening to them or to the master.
    /// Drives the tap lifecycle so switching to the master never lets the apps leak out.
    bool referenceTapArmed = false;
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
    /// Reference-tap ("다른 앱") FIFO faults since the engine started. The wake-jitter figures above
    /// describe the OUTPUT render thread only; these describe the tap CAPTURE side, which is where a
    /// crackle comes from when the render is timing-clean.
    unsigned long long referenceUnderrunBlocks = 0;
    unsigned long long referenceOverrunDrops = 0;
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
                             const std::string& metronomeSubdivision = "auto",
                             double metronomeGain = 1.0,
                             const std::string& metronomeSound = "beep",
                             bool metronomeAccentFirst = true);
    void setMetronomeAccentPattern(const std::vector<float>& pattern);
    void setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled);
    void setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer);
    void setListenRoomSettings(const ListenRoomSettings& settings);
    void setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb = -20.0f, const std::string& talkbackRoute = "listen_room");
    void setPhysicalInputAccessAllowed(bool allowed);
    void setMonitorListenSource(bool active);
    /// Arm/disarm reference-hold: run the process tap and mute the tapped apps' own output while
    /// armed, so A/B-ing back to the master never leaks their sound out of the computer. Disarming
    /// also clears the listening state and unmutes the apps.
    void setMonitorReferenceArmed(bool armed);
    /// Input-monitor the tap on top of the master — punch (setTapInputMonitor) or the track's
    /// Input-Monitor toggle (setTapInputHold).
    void setTapInputMonitor(bool active);
    void setTapInputHold(bool active);
    /// Audio recording-to-disk (V1). source 1 = physical mic (device channels [offset, offset+count)),
    /// source 2 = reference tap (stereo). endInputRecording saves the take to `path` and reports it.
    void beginInputRecording(int source, int channelOffset, int channels);
    bool endInputRecording(const std::string& path, int bitDepth, std::string& error,
                           double& durationSeconds, int& channels);
    void cancelInputRecording();
    bool inputRecordingActive() const;
    /// Live take metering for the timeline's growing waveform during a record.
    double recordingLiveSeconds() const;
    int recordingLivePeakCount() const;
    int recordingLivePeaksSince(int fromBucket, float* outLR, int maxBuckets) const;
    int recordingChannels() const;
    int recordingPeakSamples() const;
    /// Change the monitor input device live (reopens only the input queue; the output engine
    /// and its transport keep running). macOS CoreAudio engine only; no-op elsewhere.
    void setInputDeviceLive(const std::string& deviceId);
    void setInsertTailOnStopSeconds(double seconds);
    bool loadAudioFile(const std::string& path, std::string& error);
    bool loadProject(const ProjectDocument& project, std::string& error);
    bool updateProject(const ProjectDocument& project, std::string& error);
    // Wrap a structural change (add track/bus/send, send pre-post, clip-gain commit) with these to
    // fade the monitor to silence for the swap and back in after — no click. See NeuracoustDspEngine.
    void beginGraphChangeDeclick();
    void endGraphChangeDeclick();
    // PDC applied to a track/bus route (by name), in samples. 0 when off or the route has no latency.
    int routeDelayCompensationSamplesFor(const std::string& routeName);
    void updateMonitorEq(const std::vector<MonitorEqBandState>& bands);
    // Nonlinear interface modeling (Chebyshev waveshaper from measured H2–H7). Empty/zero → bypass.
    void updateInterfaceModeler(const std::vector<double>& harmonics, double mix);
    void updateMonitorFir(const ResponseCurve& curveDb, int numTaps);
    int monitorFirLatencySamples() const;
    double monitorEqMagnitudeDb(double frequencyHz) const;
    void updateTrackSendGain(const std::string& trackName, int slot, float gainDb);
    void startMeasurement(int channel, std::vector<float> signal);
    void setMeasurementChannels(int outputChannel, int inputChannel);   // loopback DAC/ADC channel
    int selectedInputChannelCount() const;   // native input width of the selected device (uncapped)
    void setMeasurementLevelCheck(bool on);  // live-meter the loopback input for gain setup
    float measurementInputPeak() const;      // peak of the chosen ADC channel, 0..1
    float measurementSweepPeak() const;      // undecayed max during the last sweep (clip check)
    void  setTalkbackInputChannel(int oneBased);   // which input channel the talkback mic is on
    int   inputChannelActivityCount() const;       // physical input channels currently metered
    float inputChannelActivity(int oneBased) const;// decayed peak of that input channel, 0..1
    void cancelMeasurement();
    bool measurementActive() const;
    double measurementProgress() const;
    std::vector<float> takeMeasurementCapture();
    bool updateClipGain(const std::string& clipId, float gainDb);
    bool updateClipStart(const std::string& clipId, double startSeconds);
    bool updateClipBounds(const std::string& clipId, double startSeconds, double durationSeconds,
                          double sourceOffsetSeconds);
    bool updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds);
    bool updateTrackMix(const std::string& trackName, float volumeDb, float pan);
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

    /// Loads a new patch (VST3 component state) into a track's live instrument voice by
    /// deactivating, setState, reactivating the existing instance — no reconcile, no module
    /// reload, no audio-thread re-instantiate. Main-thread only. False when the instrument is not
    /// prepared yet (the patch then applies on first render from the project field).
    bool updateInstrumentComponentState(const std::string& trackName,
                                        size_t slotIndex,
                                        const std::string& componentStateBase64);
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
