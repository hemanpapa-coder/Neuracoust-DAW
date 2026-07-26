#include "audio/RealtimeAudioEngine.h"
#include "audio/MonitorOutputRouting.h"
#include "audio/NeuracoustDspEngine.h"

#if defined(__APPLE__)
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <pthread.h>
#include <pthread/qos.h>
#include <unistd.h>
#include <vector>

namespace neuracoust::daw {

namespace {

AudioObjectID deviceIdFromStoredIdentity(const std::string& identity);

AudioObjectID defaultOutputDevice() {
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device);
    return device;
}

int outputDeviceChannelCount(AudioObjectID device);   // defined below
std::string deviceName(AudioObjectID device);         // defined below

// A virtual loopback (BlackHole, Loopback, Soundflower, VB-Cable) is a capture bus, not a
// speaker: audio the DAW renders into it vanishes into the loop instead of reaching the ears.
// We must never AUTO-select one for output — the classic "BlackHole is the system default
// output so the DAW is silent" trap. An explicit user pick is still honoured.
bool isVirtualLoopbackName(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* marker : {"blackhole", "loopback", "soundflower", "vb-cable", "vb cable", "aggregate output"}) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return false;
}

// Any real output device, for when the chosen one turns out to be input-only. Prevents the
// output AudioUnit from being pointed at a microphone (0 output channels), which it cannot
// render — it then spins/retries, the engine status thrashes, and the whole UI storms
// (measured ~60% CPU). Seen at startup when the system-default output resolved to a mic.
// `physicalOnly` additionally skips virtual loopbacks (see isVirtualLoopbackName).
AudioObjectID firstDeviceWithOutput(bool physicalOnly = false) {
    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr || size == 0) {
        return kAudioObjectUnknown;
    }
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr) {
        return kAudioObjectUnknown;
    }
    for (auto device : devices) {
        if (outputDeviceChannelCount(device) <= 0) continue;
        if (physicalOnly && isVirtualLoopbackName(deviceName(device))) continue;
        return device;
    }
    return kAudioObjectUnknown;
}

AudioObjectID outputDeviceFromSettings(const AudioEngineSettings& settings) {
    // An explicit user choice is honoured as-is (even BlackHole, if they really mean it).
    const bool userChose = !settings.outputDeviceId.empty();
    AudioObjectID chosen = deviceIdFromStoredIdentity(settings.outputDeviceId);
    if (chosen == kAudioObjectUnknown) {
        chosen = defaultOutputDevice();
    }
    // Never open an input-only device for output — fall back to the system default, then to
    // any device that actually has output channels.
    if (chosen == kAudioObjectUnknown || outputDeviceChannelCount(chosen) == 0) {
        const AudioObjectID def = defaultOutputDevice();
        chosen = (def != kAudioObjectUnknown && outputDeviceChannelCount(def) > 0)
            ? def : firstDeviceWithOutput();
    }
    // Auto-picking a virtual loopback would send the mix into the void (BlackHole set as the
    // system default output → DAW silent). Redirect the AUTO choice to a real physical output.
    if (!userChose && chosen != kAudioObjectUnknown && isVirtualLoopbackName(deviceName(chosen))) {
        const AudioObjectID physical = firstDeviceWithOutput(/*physicalOnly=*/true);
        if (physical != kAudioObjectUnknown) {
            chosen = physical;
        }
    }
    return chosen;
}

std::string deviceName(AudioObjectID device) {
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    AudioObjectPropertyAddress address {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) != noErr || name == nullptr) {
        return "Default Output";
    }
    char buffer[512] = {};
    CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(name);
    return buffer;
}

// Resolve a stored device identity to the CURRENT AudioObjectID. The identity is a
// stable UID string; a legacy value is a numeric AudioObjectID. Returns kAudioObjectUnknown
// when neither resolves.
AudioObjectID deviceIdFromStoredIdentity(const std::string& identity) {
    if (identity.empty()) {
        return kAudioObjectUnknown;
    }
    // UID path (preferred): translate the stable UID to whatever id it holds now.
    CFStringRef cfUid = CFStringCreateWithCString(nullptr, identity.c_str(), kCFStringEncodingUTF8);
    if (cfUid != nullptr) {
        AudioObjectID device = kAudioObjectUnknown;
        AudioValueTranslation translation {};
        translation.mInputData = &cfUid;
        translation.mInputDataSize = sizeof(cfUid);
        translation.mOutputData = &device;
        translation.mOutputDataSize = sizeof(device);
        UInt32 size = sizeof(translation);
        AudioObjectPropertyAddress address {
            kAudioHardwarePropertyDeviceForUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &translation);
        CFRelease(cfUid);
        if (status == noErr && device != kAudioObjectUnknown) {
            return device;
        }
    }
    // Legacy numeric AudioObjectID (persisted before device identity became the UID).
    char* end = nullptr;
    const auto parsed = std::strtoul(identity.c_str(), &end, 10);
    if (end != identity.c_str() && parsed != 0) {
        return static_cast<AudioObjectID>(parsed);
    }
    return kAudioObjectUnknown;
}

CFStringRef deviceUidFromId(const std::string& deviceId) {
    const AudioObjectID device = deviceIdFromStoredIdentity(deviceId);
    if (device == kAudioObjectUnknown) {
        return nullptr;
    }
    CFStringRef uid = nullptr;
    UInt32 size = sizeof(uid);
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &uid) != noErr || uid == nullptr) {
        return nullptr;
    }
    return uid;
}

int actualDeviceBufferSize(AudioObjectID device, int fallback) {
    if (device == kAudioObjectUnknown) {
        return std::max(16, fallback);
    }
    UInt32 frames = 0;
    UInt32 size = sizeof(frames);
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &frames) == noErr && frames > 0) {
        return static_cast<int>(frames);
    }
    return std::max(16, fallback);
}

int clampedDeviceBufferSize(AudioObjectID device, int requestedBufferSize) {
    if (device == kAudioObjectUnknown || requestedBufferSize <= 0) {
        return std::max(16, requestedBufferSize);
    }
    AudioValueRange range {};
    UInt32 size = sizeof(range);
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &range) == noErr &&
        range.mMinimum > 0.0 &&
        range.mMaximum >= range.mMinimum) {
        const double clamped = std::max(range.mMinimum, std::min(range.mMaximum, static_cast<double>(requestedBufferSize)));
        return static_cast<int>(std::round(clamped));
    }
    return std::max(16, requestedBufferSize);
}

void requestDeviceBufferSize(AudioObjectID device, int bufferSize) {
    if (device == kAudioObjectUnknown || bufferSize <= 0) {
        return;
    }
    UInt32 frames = static_cast<UInt32>(clampedDeviceBufferSize(device, bufferSize));
    for (AudioObjectPropertyScope scope : {kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeOutput}) {
        AudioObjectPropertyAddress address {
            kAudioDevicePropertyBufferFrameSize,
            scope,
            kAudioObjectPropertyElementMain
        };
        if (!AudioObjectHasProperty(device, &address)) {
            continue;
        }
        AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(frames), &frames);
    }
}

int outputDeviceChannelCount(AudioObjectID device) {
    if (device == kAudioObjectUnknown) {
        return 0;
    }
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    std::vector<std::byte> storage(size);
    auto* bufferList = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, bufferList) != noErr) {
        return 0;
    }
    int channelCount = 0;
    for (UInt32 index = 0; index < bufferList->mNumberBuffers; ++index) {
        channelCount += static_cast<int>(bufferList->mBuffers[index].mNumberChannels);
    }
    return channelCount;
}

// The device's native INPUT channel count. Needed so a wide loopback (BlackHole 16ch) is opened
// at its true width and captured at unity on channels 1-2 — opening it as 2ch made CoreAudio
// downmix 16→2 and bury the level ~50 dB (BlackHole monitoring was barely audible).
int inputDeviceChannelCount(AudioObjectID device) {
    if (device == kAudioObjectUnknown) {
        return 0;
    }
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    std::vector<std::byte> storage(size);
    auto* bufferList = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, bufferList) != noErr) {
        return 0;
    }
    int channelCount = 0;
    for (UInt32 index = 0; index < bufferList->mNumberBuffers; ++index) {
        channelCount += static_cast<int>(bufferList->mBuffers[index].mNumberChannels);
    }
    return channelCount;
}

int audioBufferListChannelCount(const AudioBufferList* outputData) {
    if (outputData == nullptr) {
        return 0;
    }
    int channelCount = 0;
    for (UInt32 bufferIndex = 0; bufferIndex < outputData->mNumberBuffers; ++bufferIndex) {
        channelCount += static_cast<int>(std::max<UInt32>(1, outputData->mBuffers[bufferIndex].mNumberChannels));
    }
    return channelCount;
}

void clearAudioBufferList(AudioBufferList* outputData, UInt32 frameCount) {
    if (outputData == nullptr) {
        return;
    }
    for (UInt32 bufferIndex = 0; bufferIndex < outputData->mNumberBuffers; ++bufferIndex) {
        auto& buffer = outputData->mBuffers[bufferIndex];
        auto* samples = static_cast<Float32*>(buffer.mData);
        if (samples == nullptr) {
            continue;
        }
        const UInt32 channels = std::max<UInt32>(1, buffer.mNumberChannels);
        const size_t sampleCount = std::min<size_t>(
            static_cast<size_t>(frameCount) * static_cast<size_t>(channels),
            static_cast<size_t>(buffer.mDataByteSize) / sizeof(Float32));
        std::fill(samples, samples + sampleCount, 0.0f);
    }
}

void writeAudioBufferListChannel(AudioBufferList* outputData,
                                 int channelIndex,
                                 UInt32 frame,
                                 Float32 value) {
    if (outputData == nullptr || channelIndex < 0) {
        return;
    }
    int remainingChannel = channelIndex;
    for (UInt32 bufferIndex = 0; bufferIndex < outputData->mNumberBuffers; ++bufferIndex) {
        auto& buffer = outputData->mBuffers[bufferIndex];
        const UInt32 channels = std::max<UInt32>(1, buffer.mNumberChannels);
        if (remainingChannel >= static_cast<int>(channels)) {
            remainingChannel -= static_cast<int>(channels);
            continue;
        }
        auto* samples = static_cast<Float32*>(buffer.mData);
        if (samples == nullptr) {
            return;
        }
        const size_t sampleIndex = static_cast<size_t>(frame) * static_cast<size_t>(channels) +
            static_cast<size_t>(remainingChannel);
        if (sampleIndex < static_cast<size_t>(buffer.mDataByteSize) / sizeof(Float32)) {
            samples[sampleIndex] = value;
        }
        return;
    }
}

} // namespace

class RealtimeAudioEngine::Impl {
public:
    ~Impl() { stop(); }

    /// Creates, initialises and starts the output unit on `device`, setting `device_` and
    /// `outputChannelCount_` to match. Retries the start a few times: right after another instance
    /// quits (or a device change), the driver can hold the device for a beat and the first start
    /// returns an error. Tears the unit back down and returns false on failure, with a message set.
    bool buildOutputUnitOnDevice(AudioObjectID device) {
        device_ = device;
        const int deviceOutputChannels = outputDeviceChannelCount(device_);
        outputChannelCount_ = std::max(2, deviceOutputChannels > 0 ? deviceOutputChannels
                                          : monitorOutputRequiredChannels(settings_.monitorModules));
        outputChannelCount_ = std::min(outputChannelCount_, 64);

        AudioStreamBasicDescription format {};
        format.mSampleRate = settings_.sampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        format.mBytesPerPacket = sizeof(Float32);
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = sizeof(Float32);
        format.mChannelsPerFrame = static_cast<UInt32>(outputChannelCount_);
        format.mBitsPerChannel = 32;

        AudioComponentDescription desc {};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (component == nullptr || AudioComponentInstanceNew(component, &unit_) != noErr) {
            status_.message = "Could not create Core Audio output unit.";
            unit_ = nullptr;
            return false;
        }

        AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device_, sizeof(device_));
        AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
        UInt32 maxFramesPerSlice = static_cast<UInt32>(std::max(16, settings_.bufferSize));
        AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFramesPerSlice, sizeof(maxFramesPerSlice));

        AURenderCallbackStruct callback {};
        callback.inputProc = &Impl::renderCallback;
        callback.inputProcRefCon = this;
        AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));

        if (AudioUnitInitialize(unit_) != noErr) {
            status_.message = "Could not initialize Core Audio output unit.";
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            return false;
        }

        OSStatus startStatus = noErr;
        for (int attempt = 0; attempt < 6; ++attempt) {
            startStatus = AudioOutputUnitStart(unit_);
            if (startStatus == noErr) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        status_.message = "Could not start Core Audio output.";
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
        return false;
    }

    bool start(const AudioEngineSettings& requestedSettings) {
        const double requestedPlaybackSeconds = std::max(0.0, dspEngine_.statusSnapshot().playbackSeconds);
        stop();
        settings_ = requestedSettings;
        audioThreadPolicyApplied_.store(false);
        resetRealtimeTelemetry();
        settings_.outputDriver = AudioDriverKind::CoreAudio;
        if (settings_.monitorModules.empty()) {
            settings_.monitorModules = defaultMonitorDspModules();
        }
        device_ = outputDeviceFromSettings(settings_);
        if (device_ == kAudioObjectUnknown) {
            status_.message = "No Core Audio output device is available.";
            return false;
        }
        requestDeviceBufferSize(device_, settings_.bufferSize);
        settings_.bufferSize = actualDeviceBufferSize(device_, settings_.bufferSize);
        std::string prepareError;
        if (!dspEngine_.configure(settings_, settings_.bufferSize, prepareError)) {
            status_.running = false;
            status_.outputDriver = AudioDriverKind::CoreAudio;
            status_.sampleRate = settings_.sampleRate;
            status_.outputChannels = 0;
            status_.deviceName = "Neuracoust DSP Engine";
            status_.message = prepareError;
            return false;
        }
        if (requestedPlaybackSeconds > 0.0) {
            dspEngine_.seek(requestedPlaybackSeconds);
        }

        // Build and start the output unit on the chosen device. If that device will not start —
        // it is momentarily held by a just-quit instance, or was disconnected — fall back so the
        // app is never left with a dead engine. Crucially the fallback must NOT be a virtual
        // loopback: with BlackHole set as the system default output, falling back to the default
        // silently swallowed the whole mix (the "sound just stops" trap) while the UI still named
        // the requested interface. Prefer the system default only if it is a REAL output, else the
        // first physical output device.
        if (!buildOutputUnitOnDevice(device_)) {
            AudioObjectID fallback = defaultOutputDevice();
            if (fallback == kAudioObjectUnknown || fallback == device_ ||
                outputDeviceChannelCount(fallback) == 0 ||
                isVirtualLoopbackName(deviceName(fallback))) {
                fallback = firstDeviceWithOutput(/*physicalOnly=*/true);
            }
            if (fallback == kAudioObjectUnknown || fallback == device_ || !buildOutputUnitOnDevice(fallback)) {
                return false;   // buildOutputUnitOnDevice left status_.message set and unit torn down
            }
            device_ = fallback;   // reflect the device that actually opened, so the UI never lies
        }

        const bool inputMonitorStarted = startInputMonitorIfNeeded();
        status_.running = true;
        status_.transportRunning = settings_.transportRunning;
        status_.outputDriver = AudioDriverKind::CoreAudio;
        status_.sampleRate = settings_.sampleRate;
        status_.outputChannels = outputChannelCount_;
        status_.deviceName = deviceName(device_);
        fprintf(stderr, "[NCAudio] output opened on '%s' (%d ch) @ %.0f Hz\n",
                status_.deviceName.c_str(), outputChannelCount_, settings_.sampleRate);
        status_.dspEngineName = "Neuracoust DSP Engine";
        status_.requestedBufferSize = settings_.bufferSize;
        status_.monitorPathDescription = resolveMonitorOutputRoute(settings_.monitorModules, outputChannelCount_).description;
        status_.playbackStabilityBufferSize = settings_.bufferSize * std::max(1, settings_.playbackStabilityBufferMultiplier);
        status_.message = "Core Audio I/O connected to Neuracoust DSP engine.";
        if (inputMonitorStarted) {
            status_.message += " Physical input monitor callback active.";
        }
        if (dspEngine_.activeVst3MasterInsertCount() > 0) {
            status_.message += " Realtime VST3 master inserts enabled.";
        }
        if (settings_.performanceCoreIsolationEnabled) {
            status_.message += " Core isolation QoS hint enabled for " + std::to_string(std::max(1, settings_.requestedPerformanceCoreCount)) + " DSP core(s).";
        }
        return true;
    }

    void stop() {
        stopInputMonitor();
        stopProcessTap();
        if (unit_ != nullptr) {
            // Let the render callback ramp the output to silence (~12 ms) before we cut the DAC
            // feed, so stopping/quitting mid-playback fades instead of clicking. The unit is still
            // running here, so a couple of render blocks fire during the short sleep and fade.
            fadingOut_.store(true, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(18));
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            fadingOut_.store(false, std::memory_order_relaxed);
            shutdownGain_ = 1.0f;
        }
        dspEngine_.resetRuntime();
        audioThreadPolicyApplied_.store(false);
        status_.running = false;
    }

    AudioEngineStatus status() {
        primeTelemetryIfCoreAudioHasNotRenderedYet();
        auto copy = status_;
        const auto dspStatus = dspEngine_.statusSnapshot();
        copy.transportRunning = dspStatus.transportRunning;
        copy.outputPeakLeft = dspStatus.outputPeakLeft;
        copy.outputPeakRight = dspStatus.outputPeakRight;
        copy.phaseCorrelation = dspStatus.phaseCorrelation;
        copy.spectrumLow = dspStatus.spectrumLow;
        copy.spectrumMid = dspStatus.spectrumMid;
        copy.spectrumHigh = dspStatus.spectrumHigh;
        // Analyzer data — these were being dropped, so the spectrum/goniometer/LUFS UI
        // never received anything.
        copy.spectrumBins = dspStatus.spectrumBins;
        copy.goniometerSamples = dspStatus.goniometerSamples;
        copy.momentaryLufs = dspStatus.momentaryLufs;
        copy.shortTermLufs = dspStatus.shortTermLufs;
        copy.integratedLufs = dspStatus.integratedLufs;
        copy.loudnessRange = dspStatus.loudnessRange;
        copy.truePeakDb = dspStatus.truePeakDb;
        copy.trackMeterNames = dspStatus.trackMeterNames;
        copy.trackPeakLeft = dspStatus.trackPeakLeft;
        copy.trackPeakRight = dspStatus.trackPeakRight;
        copy.trackConsoleGainReductionDb = dspStatus.trackConsoleGainReductionDb;
        copy.trackConsoleGateGainReductionDb = dspStatus.trackConsoleGateGainReductionDb;
        copy.trackInsertMeterTrackNames = dspStatus.trackInsertMeterTrackNames;
        copy.trackInsertMeterSlotIndices = dspStatus.trackInsertMeterSlotIndices;
        copy.trackInsertInputPeak = dspStatus.trackInsertInputPeak;
        copy.trackInsertOutputPeak = dspStatus.trackInsertOutputPeak;
        copy.trackInsertOutputParameterTrackNames = dspStatus.trackInsertOutputParameterTrackNames;
        copy.trackInsertOutputParameterSlotIndices = dspStatus.trackInsertOutputParameterSlotIndices;
        copy.trackInsertOutputParameterIds = dspStatus.trackInsertOutputParameterIds;
        copy.trackInsertOutputParameterValues = dspStatus.trackInsertOutputParameterValues;
        copy.playbackSeconds = dspStatus.playbackSeconds;
        copy.delayCompensationEnabled = dspStatus.delayCompensationEnabled;
        copy.delayCompensationSamples = dspStatus.delayCompensationSamples;
        copy.delayCompensationMs = dspStatus.delayCompensationMs;
        copy.directMonitoringEnabled = dspStatus.directMonitoringEnabled;
        copy.lowLatencyRecordMonitoringActive = dspStatus.lowLatencyRecordMonitoringActive;
        copy.physicalInputMonitoringActive = dspStatus.physicalInputMonitoringActive;
        copy.recordArmedTrackCount = dspStatus.recordArmedTrackCount;
        copy.inputChannels = dspStatus.inputChannels;
        copy.inputPeak = dspStatus.inputPeak;
        copy.requestedBufferSize = dspStatus.requestedBufferSize;
        copy.playbackStabilityBufferSize = dspStatus.playbackStabilityBufferSize;
        copy.dspEngineName = dspStatus.dspEngineName;
        copy.remoteDspMonitorActive = dspStatus.remoteDspMonitorActive;
        copy.remoteDspRoundTripMs = dspStatus.remoteDspRoundTripMs;
        copy.remoteDspAverageRoundTripJitterUs = dspStatus.remoteDspAverageRoundTripJitterUs;
        copy.remoteDspMaxRoundTripJitterUs = dspStatus.remoteDspMaxRoundTripJitterUs;
        RemoteDspServerSettings planSettings = settings_.remoteDspServer;
        planSettings.pluginDspEnabled = planSettings.pluginDspEnabled || dspStatus.activeRemoteDspTrackInsertCount > 0;
        const bool remoteMonitorRequested = settings_.remoteDspServer.enabled &&
            (settings_.monitorDspPathMode == "external" ||
             settings_.monitorDspPathMode == "nds" ||
             settings_.monitorDspPathMode == "remote_external" ||
             settings_.monitorDspPathMode == "auto");
        copy.remoteDspCorePlan = makeRemoteDspCorePlan(planSettings,
                                                       copy.remoteDspCorePlan.totalCores,
                                                       remoteMonitorRequested);
        copy.requestedPerformanceCoreCount = dspStatus.requestedPerformanceCoreCount;
        const auto physicalRoute = resolveMonitorOutputRoute(settings_.monitorModules, std::max(2, outputChannelCount_));
        if (!physicalRoute.description.empty() && !dspStatus.monitorPathDescription.empty()) {
            copy.monitorPathDescription = physicalRoute.description + " · " + dspStatus.monitorPathDescription;
        } else if (!physicalRoute.description.empty()) {
            copy.monitorPathDescription = physicalRoute.description;
        } else {
            copy.monitorPathDescription = dspStatus.monitorPathDescription;
        }
        copy.activeRealtimeVst3MasterInsertCount = dspStatus.activeRealtimeVst3MasterInsertCount;
        copy.activeRealtimeVst3TrackInsertCount = dspStatus.activeRealtimeVst3TrackInsertCount;
        copy.activeRemoteDspTrackInsertCount = dspStatus.activeRemoteDspTrackInsertCount;
        copy.activeOfflineVst3TrackInsertCount = dspStatus.activeOfflineVst3TrackInsertCount;
        copy.listenRoom = dspStatus.listenRoom;
        copy.realtimeCallbackCount = realtimeCallbackCount_.load();
        copy.realtimeAverageWakeJitterUs = realtimeAverageWakeJitterUs_.load();
        copy.realtimeMaxWakeJitterUs = realtimeMaxWakeJitterUs_.load();
        copy.realtimeMaxRenderDurationUs = realtimeMaxRenderDurationUs_.load();
        copy.realtimeLateWakeCount = realtimeLateWakeCount_.load();
        return copy;
    }

    void setTestToneEnabled(bool enabled) {
        settings_.testToneEnabled = enabled;
        dspEngine_.setTestToneEnabled(enabled);
    }

    void setMetronomeEnabled(bool enabled,
                             int tempoBpm,
                             const std::vector<TempoMarkerState>& tempoMap,
                             int timeSignatureNumerator,
                             int timeSignatureDenominator,
                             const std::string& grooveFeel,
                             double grooveSwingAmount,
                             const std::vector<TimeSignatureMarkerState>& timeSignatureMap,
                             const std::string& metronomeSubdivision,
                             double metronomeGain,
                             const std::string& metronomeSound,
                             bool metronomeAccentFirst) {
        settings_.metronomeEnabled = enabled;
        settings_.tempoBpm = std::max(1, tempoBpm);
        settings_.timeSignatureNumerator = std::max(1, std::min(16, timeSignatureNumerator));
        settings_.timeSignatureDenominator = std::max(1, std::min(32, timeSignatureDenominator));
        settings_.grooveFeel = (grooveFeel == "shuffle" || grooveFeel == "triplet") ? grooveFeel : "straight";
        settings_.grooveSwingAmount = std::max(0.0, std::min(1.0, grooveSwingAmount));
        settings_.metronomeSubdivision =
            (metronomeSubdivision == "quarter" || metronomeSubdivision == "eighth" || metronomeSubdivision == "sixteenth")
                ? metronomeSubdivision
                : "auto";
        settings_.metronomeGain = std::max(0.0, std::min(2.0, metronomeGain));
        settings_.metronomeSound = metronomeSound;
        settings_.metronomeAccentFirst = metronomeAccentFirst;
        settings_.tempoMap = tempoMap;
        std::sort(settings_.tempoMap.begin(), settings_.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });
        settings_.timeSignatureMap = timeSignatureMap;
        std::sort(settings_.timeSignatureMap.begin(), settings_.timeSignatureMap.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });
        dspEngine_.setMetronomeEnabled(enabled,
                                       tempoBpm,
                                       settings_.tempoMap,
                                       settings_.timeSignatureNumerator,
                                       settings_.timeSignatureDenominator,
                                       settings_.grooveFeel,
                                       settings_.grooveSwingAmount,
                                       settings_.timeSignatureMap,
                                       settings_.metronomeSubdivision,
                                       settings_.metronomeGain,
                                       settings_.metronomeSound,
                                       settings_.metronomeAccentFirst);
    }

    void setMetronomeAccentPattern(const std::vector<float>& pattern) {
        settings_.metronomeAccentPattern = pattern;
        dspEngine_.setMetronomeAccentPattern(pattern);
    }

    void setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled) {
        suppressRealtimeTelemetryAfterGraphChange();
        settings_.monitorModules = modules;
        settings_.monitorDspEnabled = enabled;
        status_.monitorPathDescription = resolveMonitorOutputRoute(settings_.monitorModules, std::max(2, outputChannelCount_)).description;
        dspEngine_.setMonitorDspModules(settings_.monitorModules, enabled);
        suppressRealtimeTelemetryAfterGraphChange();
    }

    void setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer) {
        settings_.monitorDspPathMode = (mode == "external" || mode == "nds" || mode == "remote_external" || mode == "auto") ? mode : "internal";
        settings_.remoteDspServer = remoteDspServer;
        status_.monitorDspPathMode = settings_.monitorDspPathMode;
        const bool remoteMonitorRequested = settings_.remoteDspServer.enabled &&
            (settings_.monitorDspPathMode == "external" ||
             settings_.monitorDspPathMode == "nds" ||
             settings_.monitorDspPathMode == "remote_external" ||
             settings_.monitorDspPathMode == "auto");
        status_.remoteDspCorePlan = makeRemoteDspCorePlan(settings_.remoteDspServer,
                                                          0,
                                                          remoteMonitorRequested);
        dspEngine_.setMonitorDspPathMode(settings_.monitorDspPathMode, settings_.remoteDspServer);
    }

    void setListenRoomSettings(const ListenRoomSettings& settings) {
        settings_.listenRoom = normalizedListenRoomSettings(settings);
        dspEngine_.setListenRoomSettings(settings_.listenRoom);
        status_.listenRoom = dspEngine_.statusSnapshot().listenRoom;
    }

    void setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb = -20.0f, const std::string& talkbackRoute = "listen_room") {
        const std::string safeListenMode = (listenMode == "L" || listenMode == "R" || listenMode == "M" || listenMode == "S") ? listenMode : "LR";
        const bool msMode = safeListenMode == "M" || safeListenMode == "S";
        settings_.monitorStationMono = msMode ? false : mono;
        settings_.monitorStationListenMode = safeListenMode;
        settings_.monitorStationSwapLeftRight = msMode ? false : swapLeftRight;
        settings_.monitorStationInvertLeft = invertLeft;
        settings_.monitorStationInvertRight = invertRight;
        settings_.monitorStationMute = mute;
        settings_.monitorStationDim = dim;
        settings_.monitorStationTalkback = talkback;
        settings_.monitorStationDimDb = std::max(-60.0f, std::min(0.0f, dimDb));
        settings_.monitorStationTalkbackRoute = talkbackRoute.empty() ? "listen_room" : talkbackRoute;
        settings_.monitorInputTrimDb = std::max(-12.0f, std::min(0.0f, inputTrimDb));
        settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, volumeDb));
        dspEngine_.setMonitorStationControls(settings_.monitorStationMono,
                                             settings_.monitorStationListenMode,
                                             settings_.monitorStationSwapLeftRight,
                                             invertLeft,
                                             invertRight,
                                             mute,
                                             dim,
                                             talkback,
                                             settings_.monitorInputTrimDb,
                                             settings_.monitorVolumeDb,
                                             settings_.monitorStationDimDb,
                                             settings_.monitorStationTalkbackRoute);
        status_.message = talkback
            ? "Talkback armed for " + settings_.monitorStationTalkbackRoute + "."
            : "Talkback released.";
        refreshInputMonitorForCurrentProject();
    }

    void setMonitorListenSource(bool active) {
        dspEngine_.setMonitorListenSource(active);
        // Start/stop the process tap now, so switching to the reference starts capturing
        // immediately instead of waiting for a record-arm or talkback.
        refreshInputMonitorForCurrentProject();
    }
    void setMonitorReferenceArmed(bool armed) {
        dspEngine_.setMonitorReferenceArmed(armed);
        // The tap lifecycle keys off referenceTapArmed, so (dis)arming must re-run the policy
        // to start/stop it — and, on disarm, unmute the tapped apps by tearing the tap down.
        refreshInputMonitorForCurrentProject();
    }
    void setTapInputMonitor(bool active) { dspEngine_.setTapInputMonitor(active); }
    void setTapInputHold(bool active) { dspEngine_.setTapInputHold(active); }
    void beginInputRecording(int source, int channelOffset, int channels) {
        dspEngine_.beginRecording(source, channelOffset, channels, static_cast<int>(settings_.sampleRate));
        refreshInputMonitorForCurrentProject();   // open the mic AudioQueue if recording a physical source
    }
    bool endInputRecording(const std::string& path, int bitDepth, std::string& error,
                           double& durationSeconds, int& channels) {
        const bool ok = dspEngine_.endRecording(path, bitDepth, error, durationSeconds, channels);
        refreshInputMonitorForCurrentProject();   // release the mic if nothing else needs it
        return ok;
    }
    void cancelInputRecording() {
        dspEngine_.cancelRecording();
        refreshInputMonitorForCurrentProject();   // release the mic if nothing else needs it
    }
    bool inputRecordingActive() const { return dspEngine_.recordingActive(); }
    double recordingLiveSeconds() const { return dspEngine_.recordLiveSeconds(); }
    int recordingLivePeakCount() const { return dspEngine_.recordLivePeakCount(); }
    int recordingLivePeaksSince(int fromBucket, float* outLR, int maxBuckets) const { return dspEngine_.copyRecordLivePeaksSince(fromBucket, outLR, maxBuckets); }
    int recordingChannels() const { return dspEngine_.recordChannels(); }
    int recordingPeakSamples() const { return dspEngine_.recordPeakSamples(); }
    // Change the monitor INPUT device without restarting the output engine — only the input
    // queue reopens, so the master transport keeps playing uninterrupted while you A/B a
    // reference source. A full restart (as for the output device) would stop the transport.
    void setInputDeviceLive(const std::string& deviceId) {
        settings_.inputDeviceId = deviceId;
        stopInputMonitor();                       // close on the old device
        refreshInputMonitorForCurrentProject();   // reopen on the new device if a feature needs it
    }
    void setInsertTailOnStopSeconds(double seconds) { dspEngine_.setInsertTailOnStopSeconds(seconds); }

    void setPhysicalInputAccessAllowed(bool allowed) {
        settings_.physicalInputAccessAllowed = allowed;
        refreshInputMonitorForCurrentProject();
        if (!allowed) {
            status_.message = "Physical input access is disabled for playback.";
        } else if (inputQueue_ != nullptr) {
            status_.message = "Physical input monitor callback active.";
        } else {
            status_.message = "Physical input access enabled; waiting for record, input monitor, or talkback.";
        }
    }

    void setTransportRunning(bool running) {
        settings_.transportRunning = running;
        dspEngine_.setTransportRunning(running);
        status_.transportRunning = running;
        status_.message = running
            ? "Transport running; realtime DSP graph follows the timeline."
            : "Transport stopped; realtime DSP graph remains active.";
    }

    bool loadAudioFile(const std::string& path, std::string& error) {
        const bool loaded = dspEngine_.loadAudioFile(path, error);
        if (loaded) {
            stopInputMonitor();
            status_.message = dspEngine_.lastMessage();
        }
        return loaded;
    }

    bool loadProject(const ProjectDocument& project, std::string& error) {
        const bool loaded = dspEngine_.loadProject(project, error);
        if (loaded) {
            suppressRealtimeTelemetryAfterGraphChange();
            refreshInputMonitorForCurrentProject();
            status_.message = dspEngine_.lastMessage();
            if (inputQueue_ != nullptr) {
                status_.message += " Physical input monitor callback active.";
            }
        }
        return loaded;
    }

    bool updateProject(const ProjectDocument& project, std::string& error) {
        const bool updated = dspEngine_.updateProject(project, error);
        if (updated) {
            suppressRealtimeTelemetryAfterGraphChange();
            refreshInputMonitorForCurrentProject();
            status_.message = dspEngine_.lastMessage();
            if (inputQueue_ != nullptr) {
                status_.message += " Physical input monitor callback active.";
            }
        }
        return updated;
    }

    void beginGraphChangeDeclick() { dspEngine_.beginGraphChangeDeclick(); }
    void endGraphChangeDeclick() { dspEngine_.endGraphChangeDeclick(); }
    int routeDelayCompensationSamplesFor(const std::string& routeName) { return dspEngine_.routeDelayCompensationSamplesFor(routeName); }

    void updateMonitorEq(const std::vector<neuracoust::daw::MonitorEqBandState>& bands) {
        dspEngine_.updateMonitorEq(bands);
    }
    void updateInterfaceModeler(const std::vector<double>& harmonics, double mix) {
        dspEngine_.updateInterfaceModeler(harmonics, mix);
    }

    void updateMonitorFir(const neuracoust::daw::ResponseCurve& curveDb, int numTaps) {
        dspEngine_.updateMonitorFir(curveDb, numTaps);
    }

    int monitorFirLatencySamples() const { return dspEngine_.monitorFirLatencySamples(); }
    double monitorEqMagnitudeDb(double frequencyHz) const { return dspEngine_.monitorEqMagnitudeDb(frequencyHz); }

    void updateTrackSendGain(const std::string& trackName, int slot, float gainDb) {
        dspEngine_.updateTrackSendGain(trackName, slot, gainDb);
    }

    void startMeasurement(int channel, std::vector<float> signal) {
        dspEngine_.startMeasurement(channel, std::move(signal));
        refreshInputMonitorForCurrentProject();   // ensure the input queue is open to capture
    }
    void setMeasurementChannels(int outputChannel, int inputChannel) { dspEngine_.setMeasurementChannels(outputChannel, inputChannel); }
    // The selected input device's NATIVE channel count, queried without opening the stream — the
    // status' inputChannels is capped at 2 for the monitor mix, so the measurement UI uses this.
    int selectedInputChannelCount() const {
        const int n = inputDeviceChannelCount(deviceIdFromStoredIdentity(settings_.inputDeviceId));
        return n > 0 ? n : 2;
    }
    void setMeasurementLevelCheck(bool on) {
        dspEngine_.setMeasurementLevelCheck(on);
        refreshInputMonitorForCurrentProject();   // open the input queue to meter (or close on stop)
    }
    float measurementInputPeak() const { return dspEngine_.measurementInputPeak(); }
    float measurementSweepPeak() const { return dspEngine_.measurementSweepPeak(); }
    void setTalkbackInputChannel(int oneBased) { dspEngine_.setTalkbackInputChannel(std::max(0, oneBased - 1)); }
    int inputChannelActivityCount() const { return dspEngine_.inputChannelCount(); }
    float inputChannelActivity(int oneBased) const { return dspEngine_.inputChannelPeak(std::max(0, oneBased - 1)); }
    void cancelMeasurement() { dspEngine_.cancelMeasurement(); }
    bool measurementActive() const { return dspEngine_.measurementActive(); }
    double measurementProgress() const { return dspEngine_.measurementProgress(); }
    std::vector<float> takeMeasurementCapture() { return dspEngine_.takeMeasurementCapture(); }

    bool updateClipGain(const std::string& clipId, float gainDb) {
        const bool updated = dspEngine_.updateClipGain(clipId, gainDb);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }
    bool updateClipStart(const std::string& clipId, double startSeconds) {
        return dspEngine_.updateClipStart(clipId, startSeconds);
    }
    bool updateClipBounds(const std::string& clipId, double startSeconds, double durationSeconds,
                          double sourceOffsetSeconds) {
        return dspEngine_.updateClipBounds(clipId, startSeconds, durationSeconds, sourceOffsetSeconds);
    }

    bool updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds) {
        const bool updated = dspEngine_.updateClipFades(clipId, fadeInSeconds, fadeOutSeconds);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateTrackMix(const std::string& trackName, float volumeDb, float pan) {
        const bool updated = dspEngine_.updateTrackMix(trackName, volumeDb, pan);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateTrackConsoleChannel(const std::string& trackName, const ConsoleChannelState& console) {
        return dspEngine_.updateTrackConsoleChannel(trackName, console);
    }

    bool updateTrackSendSlot(const std::string& trackName, size_t sendIndex, const TrackSendState& send) {
        const bool updated = dspEngine_.updateTrackSendSlot(trackName, sendIndex, send);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateTrackInsertBypassState(const std::string& trackName, size_t insertIndex, bool bypassed) {
        const bool updated = dspEngine_.updateTrackInsertBypassState(trackName, insertIndex, bypassed);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateMasterInsertBypassState(size_t insertIndex, bool bypassed) {
        const bool updated = dspEngine_.updateMasterInsertBypassState(insertIndex, bypassed);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateTrackPlaybackState(const std::string& trackName, bool muted, bool solo) {
        const bool updated = dspEngine_.updateTrackPlaybackState(trackName, muted, solo);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateTrackRealtimeState(const std::string& trackName, bool recordArmed, bool inputMonitoring, bool muted, bool solo) {
        const bool updated = dspEngine_.updateTrackRealtimeState(trackName, recordArmed, inputMonitoring, muted, solo);
        if (updated) {
            refreshInputMonitorForCurrentProject();
            status_.message = dspEngine_.lastMessage();
            if (inputQueue_ != nullptr) {
                status_.message += " Physical input monitor callback active.";
            }
        }
        return updated;
    }

    bool updateMasterVst3Parameter(size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) {
        const bool updated = dspEngine_.updateMasterVst3Parameter(insertIndex, parameterId, displayName, normalizedValue);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateTrackVst3Parameter(const std::string& trackName, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) {
        const bool updated = dspEngine_.updateTrackVst3Parameter(trackName, insertIndex, parameterId, displayName, normalizedValue);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateInstrumentVst3Parameter(const std::string& trackName, size_t slotIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) {
        const bool updated = dspEngine_.updateInstrumentVst3Parameter(trackName, slotIndex, parameterId, displayName, normalizedValue);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    bool updateInstrumentComponentState(const std::string& trackName, size_t slotIndex, const std::string& componentStateBase64) {
        const bool updated = dspEngine_.updateInstrumentComponentState(trackName, slotIndex, componentStateBase64);
        status_.message = dspEngine_.lastMessage();
        return updated;
    }

    bool updateMonitorSpeakerVst3Parameter(int speakerSlot, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) {
        const bool updated = dspEngine_.updateMonitorSpeakerVst3Parameter(speakerSlot, insertIndex, parameterId, displayName, normalizedValue);
        if (updated) {
            status_.message = dspEngine_.lastMessage();
        }
        return updated;
    }

    void queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events) {
        dspEngine_.queueLiveMidiEvents(trackName, events);
    }

    void setEditorInstrumentMonitor(bool active, const std::string& trackName) {
        dspEngine_.setEditorInstrumentMonitor(active, trackName);
    }

    void pushEditorInstrumentMonitorInterleaved(const float* samples, int64_t frameCount) {
        dspEngine_.pushEditorInstrumentMonitorInterleaved(samples, frameCount);
    }

    void setTransportRecordingActive(bool active) {
        settings_.transportRecordingActive = active;
        dspEngine_.setTransportRecordingActive(active);
        refreshInputMonitorForCurrentProject();
        status_.message = dspEngine_.lastMessage();
        if (inputQueue_ != nullptr) {
            status_.message += " Physical input monitor callback active.";
        }
    }

    void rewind() {
        dspEngine_.rewind();
    }

    void seek(double seconds) {
        dspEngine_.seek(seconds);
        dspEngine_.armSeekRamp();
    }

private:
    bool startInputMonitorIfNeeded(bool prewarm = false) {
        if (inputQueue_ != nullptr) {
            return true;
        }
        if (!settings_.physicalInputAccessAllowed) {
            return false;
        }
        const auto dspStatus = dspEngine_.statusSnapshot();
        if (!prewarm && !dspStatus.lowLatencyRecordMonitoringActive &&
            !settings_.monitorStationTalkback &&
            !dspEngine_.measurementActive() && !dspEngine_.measurementLevelCheck()) {
            return false;   // reference monitoring uses the process tap, not this physical-input queue
        }

        inputFormat_ = {};
        inputFormat_.mSampleRate = settings_.sampleRate;
        inputFormat_.mFormatID = kAudioFormatLinearPCM;
        inputFormat_.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        // Open at the device's NATIVE input width. A many-channel loopback (BlackHole 16ch) opened
        // as 2ch made CoreAudio downmix 16→2, burying the level ~50 dB while the system audio only
        // occupies channels 1-2. At native width the queue delivers all channels and
        // pushInputMonitorInterleaved takes channels 1-2 at unity. Falls back to 2 when unknown.
        const AudioObjectID inputDevice = deviceIdFromStoredIdentity(settings_.inputDeviceId);
        const int nativeInputChannels = inputDeviceChannelCount(inputDevice);
        const int inputChannels = nativeInputChannels > 0
            ? std::min(64, nativeInputChannels)
            : std::max(1, std::min(2, settings_.inputMonitorChannelCount));
        inputFormat_.mBytesPerPacket = static_cast<UInt32>(inputChannels * sizeof(Float32));
        inputFormat_.mFramesPerPacket = 1;
        inputFormat_.mBytesPerFrame = static_cast<UInt32>(inputChannels * sizeof(Float32));
        inputFormat_.mChannelsPerFrame = static_cast<UInt32>(inputChannels);
        inputFormat_.mBitsPerChannel = 32;

        const OSStatus queueStatus = AudioQueueNewInput(&inputFormat_, &Impl::inputCallback, this, nullptr, nullptr, 0, &inputQueue_);
        if (queueStatus != noErr || inputQueue_ == nullptr) {
            status_.message += " Physical input monitor unavailable: Core Audio input queue could not be created.";
            return false;
        }
        inputThreadPolicyApplied_.store(false);   // fresh queue → fresh callback thread to pin
        CFStringRef inputUid = deviceUidFromId(settings_.inputDeviceId);
        if (inputUid != nullptr) {
            AudioQueueSetProperty(inputQueue_, kAudioQueueProperty_CurrentDevice, &inputUid, sizeof(inputUid));
            CFRelease(inputUid);
        }

        const UInt32 framesPerBuffer = static_cast<UInt32>(std::max(16, settings_.bufferSize));
        const UInt32 bufferBytes = framesPerBuffer * inputFormat_.mBytesPerFrame;
        for (auto& buffer : inputBuffers_) {
            if (AudioQueueAllocateBuffer(inputQueue_, bufferBytes, &buffer) != noErr || buffer == nullptr) {
                status_.message += " Physical input monitor unavailable: input buffer allocation failed.";
                stopInputMonitor();
                return false;
            }
            AudioQueueEnqueueBuffer(inputQueue_, buffer, 0, nullptr);
        }

        if (AudioQueueStart(inputQueue_, nullptr) != noErr) {
            status_.message += " Physical input monitor unavailable: microphone permission or input device unavailable.";
            stopInputMonitor();
            return false;
        }
        fprintf(stderr, "[NCAudio] input opened on '%s' (%u ch) @ %.0f Hz\n",
                settings_.inputDeviceId.empty() ? "(default)" : settings_.inputDeviceId.c_str(),
                inputFormat_.mChannelsPerFrame, inputFormat_.mSampleRate);
        return true;
    }

    void refreshInputMonitorForCurrentProject() {
        if (!status_.running && unit_ == nullptr) {
            return;
        }
        const auto dspStatus = dspEngine_.statusSnapshot();
        // Physical mic (AudioQueue): record-arm monitoring, talkback, measurement/level-check.
        const bool wantMic = settings_.physicalInputAccessAllowed &&
            (dspStatus.lowLatencyRecordMonitoringActive ||
             settings_.monitorStationTalkback ||
             dspEngine_.measurementActive() || dspEngine_.measurementLevelCheck() ||
             dspEngine_.recordingWantsMic());   // keep the mic open while recording a physical input
        // Reference monitoring of other apps comes from the process tap. Keyed on the *armed*
        // state, not the listening state, so the tap keeps running (and the apps stay muted)
        // while you A/B back to the master — no sound leaks out of the computer.
        const bool wantTap = dspStatus.referenceTapArmed;
        if (wantMic) startInputMonitorIfNeeded(); else stopInputMonitor();
        if (wantTap) startProcessTap(); else stopProcessTap();
    }

    void stopInputMonitor() {
        if (inputQueue_ != nullptr) {
            AudioQueueStop(inputQueue_, true);
            AudioQueueDispose(inputQueue_, true);
            inputQueue_ = nullptr;
        }
        for (auto& buffer : inputBuffers_) {
            buffer = nullptr;
        }
    }

    // Our own audio process object, so the tap can exclude us (else our monitor output loops back).
    AudioObjectID ownProcessObject() {
        pid_t pid = getpid();
        AudioObjectID obj = kAudioObjectUnknown;
        AudioObjectPropertyAddress addr {
            kAudioHardwarePropertyTranslatePIDToProcessObject,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        UInt32 size = sizeof(obj);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(pid), &pid, &size, &obj);
        return obj;
    }

    // Reference monitoring via a Core Audio process tap (macOS 14.4+): tap every other app's output
    // (unmuted, so they keep playing to their own device), read it through a private aggregate + an
    // IOProc, and push it into the monitor path. Replaces the BlackHole loopback entirely.
    bool startProcessTap() {
        if (tapAggregate_ != kAudioObjectUnknown) return true;   // already running

        CATapDescription* desc = nil;
        const AudioObjectID me = ownProcessObject();
        NSArray<NSNumber*>* exclude = (me != kAudioObjectUnknown) ? @[@(me)] : @[];
        desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:exclude];
        desc.privateTap = YES;
        // Mute the tapped apps' OWN output while we monitor them, so they are heard only through the
        // DAW monitor (with its EQ/level) and not doubled by their own direct output. Restored the
        // instant the tap stops.
        desc.muteBehavior = CATapMutedWhenTapped;
        desc.name = @"Neuracoust Reference Tap";

        if (AudioHardwareCreateProcessTap(desc, &tapObject_) != noErr || tapObject_ == kAudioObjectUnknown) {
            fprintf(stderr, "[NCAudio] process tap create failed\n");
            tapObject_ = kAudioObjectUnknown;
            return false;
        }
        NSString* tapUID = desc.UUID.UUIDString;

        // A PRIVATE aggregate is required (a public one comes up with sample-rate 0 and won't start).
        NSString* aggUID = [NSString stringWithFormat:@"nc-reference-tap-%@", tapUID];
        NSDictionary* aggDict = @{
            @(kAudioAggregateDeviceNameKey):       @"Neuracoust Reference Tap",
            @(kAudioAggregateDeviceUIDKey):        aggUID,
            @(kAudioAggregateDeviceIsPrivateKey):  @1,
            @(kAudioAggregateDeviceTapAutoStartKey): @1,
            @(kAudioAggregateDeviceTapListKey):    @[ @{ @(kAudioSubTapUIDKey): tapUID } ],
        };
        if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggDict, &tapAggregate_) != noErr ||
            tapAggregate_ == kAudioObjectUnknown) {
            fprintf(stderr, "[NCAudio] process-tap aggregate create failed\n");
            stopProcessTap();
            return false;
        }

        // IOProc MUST run on a dispatch queue — with a nil queue the callback never fires.
        if (tapQueue_ == nullptr) {
            tapQueue_ = dispatch_queue_create("com.neuracoust.reference-tap", DISPATCH_QUEUE_SERIAL);
        }
        OSStatus st = AudioDeviceCreateIOProcIDWithBlock(&tapIOProc_, tapAggregate_, tapQueue_,
            ^(const AudioTimeStamp*, const AudioBufferList* inInput, const AudioTimeStamp*,
              AudioBufferList*, const AudioTimeStamp*) {
                this->handleTapInput(inInput);
            });
        if (st != noErr || tapIOProc_ == nullptr) {
            fprintf(stderr, "[NCAudio] process-tap IOProc create failed (%d)\n", (int)st);
            stopProcessTap();
            return false;
        }
        if (AudioDeviceStart(tapAggregate_, tapIOProc_) != noErr) {
            fprintf(stderr, "[NCAudio] process-tap start failed\n");
            stopProcessTap();
            return false;
        }
        // DIAG: a tap-vs-output nominal-rate mismatch beyond the varispeed clamp (±6%) would cause
        // periodic reference-FIFO underruns (the intermittent dropouts). Log both rates to confirm.
        {
            Float64 tapRate = 0; UInt32 sz = sizeof(tapRate);
            AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
            AudioObjectGetPropertyData(tapAggregate_, &ra, 0, nullptr, &sz, &tapRate);
            dspEngine_.logReferenceRates(tapRate, (double)settings_.sampleRate);
        }
        return true;
    }

    void stopProcessTap() {
        if (tapAggregate_ != kAudioObjectUnknown && tapIOProc_ != nullptr) {
            AudioDeviceStop(tapAggregate_, tapIOProc_);
            AudioDeviceDestroyIOProcID(tapAggregate_, tapIOProc_);
        }
        tapIOProc_ = nullptr;
        if (tapAggregate_ != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(tapAggregate_);
            tapAggregate_ = kAudioObjectUnknown;
        }
        if (tapObject_ != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(tapObject_);
            tapObject_ = kAudioObjectUnknown;
        }
    }

    // Tap IOProc body (dispatch-queue thread): interleave the tapped audio to stereo and push it into
    // the same monitor buffer the mic path uses. The DSP engine treats it as the listen-source feed.
    void handleTapInput(const AudioBufferList* input) {
        if (input == nullptr || input->mNumberBuffers == 0) return;
        const AudioBuffer& b0 = input->mBuffers[0];
        const int chan0 = std::max<int>(1, static_cast<int>(b0.mNumberChannels));
        const int frames = static_cast<int>(b0.mDataByteSize / (sizeof(float) * chan0));
        if (frames <= 0) return;
        tapInterleaved_.assign(static_cast<size_t>(frames) * 2, 0.0f);
        if (input->mNumberBuffers >= 2) {                 // non-interleaved: buffer 0 = L, buffer 1 = R
            const float* L = static_cast<const float*>(input->mBuffers[0].mData);
            const float* R = static_cast<const float*>(input->mBuffers[1].mData);
            for (int i = 0; i < frames; ++i) {
                tapInterleaved_[i * 2]     = L ? L[i] : 0.0f;
                tapInterleaved_[i * 2 + 1] = R ? R[i] : (L ? L[i] : 0.0f);
            }
        } else {                                           // interleaved single buffer
            const float* s = static_cast<const float*>(b0.mData);
            for (int i = 0; i < frames; ++i) {
                tapInterleaved_[i * 2]     = s[i * chan0];
                tapInterleaved_[i * 2 + 1] = chan0 >= 2 ? s[i * chan0 + 1] : s[i * chan0];
            }
        }
        dspEngine_.pushReferenceInterleaved(tapInterleaved_.data(), frames);
    }

    static void inputCallback(void* userData,
                              AudioQueueRef queue,
                              AudioQueueBufferRef buffer,
                              const AudioTimeStamp* startTime,
                              UInt32 packetCount,
                              const AudioStreamPacketDescription* packetDescriptions) {
        (void)startTime;
        (void)packetDescriptions;
        auto* self = static_cast<Impl*>(userData);
        self->handleInput(queue, buffer, packetCount);
    }

    void handleInput(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 packetCount) {
        if (buffer == nullptr || inputFormat_.mBytesPerFrame == 0) {
            return;
        }
        const int frames = packetCount > 0
            ? static_cast<int>(packetCount)
            : static_cast<int>(buffer->mAudioDataByteSize / inputFormat_.mBytesPerFrame);
        applyInputThreadPolicyIfNeeded(static_cast<UInt32>(std::max(16, settings_.bufferSize)));
        if (frames > 0) {
            dspEngine_.pushInputMonitorInterleaved(static_cast<const Float32*>(buffer->mAudioData), frames, static_cast<int>(inputFormat_.mChannelsPerFrame));
        }
        if (inputQueue_ != nullptr) {
            AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        }
    }

    static OSStatus renderCallback(void* refCon,
                                   AudioUnitRenderActionFlags* flags,
                                   const AudioTimeStamp* timestamp,
                                   UInt32 busNumber,
                                   UInt32 frameCount,
                                   AudioBufferList* outputData) {
        (void)flags;
        (void)timestamp;
        (void)busNumber;
        auto* self = static_cast<Impl*>(refCon);
        self->render(outputData, frameCount);
        return noErr;
    }

    void render(AudioBufferList* outputData, UInt32 frameCount) {
        const auto wakeTime = std::chrono::steady_clock::now();
        applyAudioThreadPolicyIfNeeded(frameCount);
        if (outputData == nullptr || outputData->mNumberBuffers == 0) {
            return;
        }
        clearAudioBufferList(outputData, frameCount);
        const auto renderStart = std::chrono::steady_clock::now();
        dspEngine_.renderInterleavedStereo(frameCount, renderBlock_);
        applyOutputSafety(renderBlock_, static_cast<size_t>(frameCount) * 2u);
        // Shutdown fade: while stop() is winding down, ramp the block to silence so cutting the
        // DAC feed mid-waveform never clicks (heard on quit-during-playback and device changes).
        if (fadingOut_.load(std::memory_order_relaxed)) {
            const float step = 1.0f / std::max(1.0f, static_cast<float>(settings_.sampleRate) * 0.012f);
            float g = shutdownGain_;
            for (UInt32 frame = 0; frame < frameCount; ++frame) {
                g = std::max(0.0f, g - step);
                const size_t idx = static_cast<size_t>(frame) * 2u;
                if (idx + 1 < renderBlock_.size()) { renderBlock_[idx] *= g; renderBlock_[idx + 1] *= g; }
            }
            shutdownGain_ = g;
        }
        const int availableChannels = std::max(2, audioBufferListChannelCount(outputData));
        const auto route = resolveMonitorOutputRoute(settings_.monitorModules, availableChannels);
        // Interface loopback measurement can pin the sweep to a specific physical output channel
        // (e.g. DigiGrid out 3) instead of the monitor route, so it lands on the loopback cable.
        // The sweep sits on the stereo bus' left channel (measurement emit uses channel 0).
        const int measOut = ((dspEngine_.measurementActive() || dspEngine_.measurementLevelCheck()) &&
                             dspEngine_.measurementOutputChannel() >= 0)
                                ? dspEngine_.measurementOutputChannel() : -1;
        if (measOut >= 0 && measOut < availableChannels) {
            for (UInt32 frame = 0; frame < frameCount; ++frame) {
                const auto source = static_cast<size_t>(frame * 2);
                const float sweep = source < renderBlock_.size() ? renderBlock_[source] : 0.0f;
                writeAudioBufferListChannel(outputData, measOut, frame, sweep);
            }
        } else if (route.assigned && route.available) {
            for (UInt32 frame = 0; frame < frameCount; ++frame) {
                const auto source = static_cast<size_t>(frame * 2);
                const float left = source + 1 < renderBlock_.size() ? renderBlock_[source] : 0.0f;
                const float right = source + 1 < renderBlock_.size() ? renderBlock_[source + 1] : left;
                writeAudioBufferListChannel(outputData, route.leftChannel, frame, left);
                writeAudioBufferListChannel(outputData, route.rightChannel, frame, right);
            }
        }
        const auto renderEnd = std::chrono::steady_clock::now();
        recordRealtimeTelemetry(wakeTime, renderStart, renderEnd, frameCount);
    }

    // Speaker/hearing protection, applied to the interleaved output just before it reaches the
    // device. Runs on the audio thread — branch-light, allocation-free.
    //  1. NaN/Inf → 0 (a single non-finite sample is a full-scale click; the classic
    //     speaker-killer). Any in a block trips a short fault mute so the surrounding
    //     discontinuity is faded, not blasted.
    //  2. Hard brickwall clamp to the ceiling — a runaway gain / feedback can never exceed it.
    //  3. A fault mute that dips to silence and ramps back over ~40 ms, so a burst of garbage
    //     is smothered instead of hitting the speakers full tilt (the "never output malicious
    //     noise" behaviour, in software).
    void applyOutputSafety(std::vector<float>& block, size_t sampleCount) {
        if (sampleCount > block.size()) sampleCount = block.size();
        const float ceiling = safetyCeiling_;
        bool faulted = false;
        uint64_t clamped = 0;
        for (size_t i = 0; i < sampleCount; ++i) {
            float x = block[i];
            if (!std::isfinite(x)) { x = 0.0f; faulted = true; }
            if (x > ceiling) { x = ceiling; ++clamped; }
            else if (x < -ceiling) { x = -ceiling; ++clamped; }
            block[i] = x;
        }
        if (faulted) {
            // ~40 ms of mute + ramp-back after a non-finite fault.
            safetyRecoverySamples_ = static_cast<int>(std::max(1.0, settings_.sampleRate) * 0.04) * 2;
            safetyFaultBlocks_.fetch_add(1, std::memory_order_relaxed);
        }
        if (clamped > 0) safetyClampedSamples_.fetch_add(clamped, std::memory_order_relaxed);

        // Apply the fault-mute envelope per stereo frame (both samples share one gain step).
        if (safetyRecoverySamples_ <= 0 && safetyGain_ >= 0.999f) {
            safetyGain_ = 1.0f;
            return;   // fast path: nothing to ride
        }
        const double sr = std::max(1.0, settings_.sampleRate);
        const float rampPerFrame = static_cast<float>(1.0 / (sr * 0.02));   // ~20 ms fade-in
        for (size_t i = 0; i + 1 < sampleCount; i += 2) {
            float target = 1.0f;
            if (safetyRecoverySamples_ > 0) { target = 0.0f; safetyRecoverySamples_ -= 2; }
            if (safetyGain_ < target) { safetyGain_ = std::min(target, safetyGain_ + rampPerFrame); }
            else if (safetyGain_ > target) { safetyGain_ = target; }   // mute is instant, recovery ramps
            block[i] *= safetyGain_;
            block[i + 1] *= safetyGain_;
        }
    }

    void primeTelemetryIfCoreAudioHasNotRenderedYet() {
        if (!status_.running ||
            realtimeCallbackCount_.load() != 0 ||
            !settings_.metronomeEnabled) {
            return;
        }
        const auto wakeTime = std::chrono::steady_clock::now();
        const auto renderStart = std::chrono::steady_clock::now();
        dspEngine_.renderInterleavedStereo(static_cast<UInt32>(std::max(16, settings_.bufferSize)), renderProbeBlock_);
        const auto renderEnd = std::chrono::steady_clock::now();
        recordRealtimeTelemetry(wakeTime, renderStart, renderEnd, static_cast<UInt32>(std::max(16, settings_.bufferSize)));
    }

    void recordRealtimeTelemetry(std::chrono::steady_clock::time_point wakeTime,
                                 std::chrono::steady_clock::time_point renderStart,
                                 std::chrono::steady_clock::time_point renderEnd,
                                 UInt32 frameCount) {
        const double expectedPeriodUs = (static_cast<double>(std::max<UInt32>(1, frameCount)) /
                                         std::max(1.0, settings_.sampleRate)) *
                                        1000000.0;
        int suppressCallbacks = realtimeTelemetrySuppressCallbacks_.load(std::memory_order_relaxed);
        while (suppressCallbacks > 0) {
            if (realtimeTelemetrySuppressCallbacks_.compare_exchange_weak(
                    suppressCallbacks,
                    suppressCallbacks - 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                lastRenderWakeTime_ = wakeTime;
                return;
            }
        }
        double jitterUs = 0.0;
        if (lastRenderWakeTime_.time_since_epoch().count() != 0) {
            const double actualPeriodUs = std::chrono::duration<double, std::micro>(wakeTime - lastRenderWakeTime_).count();
            jitterUs = std::abs(actualPeriodUs - expectedPeriodUs);
        }
        lastRenderWakeTime_ = wakeTime;

        const double renderDurationUs = std::chrono::duration<double, std::micro>(renderEnd - renderStart).count();
        realtimeCallbackCount_.fetch_add(1);
        const double previousAverage = realtimeAverageWakeJitterUs_.load();
        realtimeAverageWakeJitterUs_.store(previousAverage <= 0.0 ? jitterUs : previousAverage + ((jitterUs - previousAverage) * 0.08));
        const double previousWakePeak = realtimeMaxWakeJitterUs_.load();
        realtimeMaxWakeJitterUs_.store(std::max(jitterUs, previousWakePeak * 0.985));
        const double previousRenderPeak = realtimeMaxRenderDurationUs_.load();
        realtimeMaxRenderDurationUs_.store(std::max(renderDurationUs, previousRenderPeak * 0.985));
        if (jitterUs > expectedPeriodUs * 0.5) {
            realtimeLateWakeCount_.fetch_add(1);
        }
    }

    void resetRealtimeTelemetry() {
        realtimeCallbackCount_.store(0);
        realtimeAverageWakeJitterUs_.store(0.0);
        realtimeMaxWakeJitterUs_.store(0.0);
        realtimeMaxRenderDurationUs_.store(0.0);
        realtimeLateWakeCount_.store(0);
        lastRenderWakeTime_ = {};
    }

    void suppressRealtimeTelemetryAfterGraphChange() {
        resetRealtimeTelemetry();
        realtimeTelemetrySuppressCallbacks_.store(12, std::memory_order_relaxed);
    }

    // Hard real-time (time-constraint) scheduling for the calling thread. Without it the
    // thread runs at ordinary priority and any busy out-of-process plug-in worker/observer
    // (or the plug-in editor GUI) can keep the OS from waking it on time — the callback then
    // arrives a full buffer period late. Time-constraint guarantees the thread is scheduled
    // ahead of every QoS class, so its wake timing stays locked to the buffer period.
    void applyTimeConstraintToCurrentThread(UInt32 frameCount) {
        const double periodUs = (static_cast<double>(std::max<UInt32>(1, frameCount)) /
                                 std::max(1.0, settings_.sampleRate)) * 1000000.0;
        mach_timebase_info_data_t timebase;
        if (mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.numer != 0) {
            const auto usToAbs = [&](double us) -> uint32_t {
                const double ticks = (us * 1000.0) * static_cast<double>(timebase.denom) /
                                     static_cast<double>(timebase.numer);
                return static_cast<uint32_t>(std::max(1.0, ticks));
            };
            thread_time_constraint_policy_data_t policy;
            policy.period = usToAbs(periodUs);
            policy.computation = usToAbs(periodUs * 0.5);
            policy.constraint = usToAbs(periodUs * 0.85);
            policy.preemptible = 1;
            thread_policy_set(pthread_mach_thread_np(pthread_self()),
                              THREAD_TIME_CONSTRAINT_POLICY,
                              reinterpret_cast<thread_policy_t>(&policy),
                              THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        }
    }

    void applyAudioThreadPolicyIfNeeded(UInt32 frameCount) {
        bool expected = false;
        if (!audioThreadPolicyApplied_.compare_exchange_strong(expected, true)) {
            return;
        }
        applyTimeConstraintToCurrentThread(frameCount);
    }

    // The input AudioQueue runs its callback on a CoreAudio-managed thread that, unlike the
    // render thread, gets no real-time policy by default — a scheduling asymmetry that lets
    // the capture thread wake late and forces the monitor FIFO to buffer more. Pin it to the
    // same time-constraint policy on its first callback so input can track at low latency.
    void applyInputThreadPolicyIfNeeded(UInt32 frameCount) {
        bool expected = false;
        if (!inputThreadPolicyApplied_.compare_exchange_strong(expected, true)) {
            return;
        }
        applyTimeConstraintToCurrentThread(frameCount);
    }

    AudioEngineSettings settings_;
    AudioEngineStatus status_;
    AudioUnit unit_ = nullptr;
    AudioObjectID device_ = kAudioObjectUnknown;
    AudioQueueRef inputQueue_ = nullptr;
    AudioQueueBufferRef inputBuffers_[3] {};
    AudioStreamBasicDescription inputFormat_ {};
    // Reference monitoring ("listen to other apps") via a Core Audio process tap — replaces the old
    // BlackHole loopback. Captures every other process's output (excluding ourselves, so our own
    // monitor does not feed back) and pushes it down the same monitor path the mic used.
    AudioObjectID tapObject_ = kAudioObjectUnknown;
    AudioObjectID tapAggregate_ = kAudioObjectUnknown;
    AudioDeviceIOProcID tapIOProc_ = nullptr;
    dispatch_queue_t tapQueue_ = nullptr;
    std::vector<float> tapInterleaved_;
    int outputChannelCount_ = 2;
    NeuracoustDspEngine dspEngine_;
    std::vector<float> renderBlock_;
    std::vector<float> renderProbeBlock_;
    // Output safety guard: the last line of defence for the speakers. Flushes NaN/Inf to
    // silence and hard-clamps to the ceiling, then rides a fault mute so a burst of garbage
    // (a crashed plug-in, a runaway feedback) is faded out instead of blasted at the DAC.
    float safetyCeiling_ = 1.0f;                 // linear, 1.0 = 0 dBFS
    float safetyGain_ = 1.0f;                    // current fault-mute gain, ramps 0→1
    int safetyRecoverySamples_ = 0;              // samples of mute left after a fault
    std::atomic<uint64_t> safetyFaultBlocks_ {0};
    std::atomic<uint64_t> safetyClampedSamples_ {0};
    std::atomic<bool> audioThreadPolicyApplied_ {false};
    std::atomic<bool> fadingOut_ {false};        // ramp the output to silence before stop()
    float shutdownGain_ = 1.0f;                   // current fade level (audio thread only)
    std::atomic<bool> inputThreadPolicyApplied_ {false};
    std::atomic<uint64_t> realtimeCallbackCount_ {0};
    std::atomic<double> realtimeAverageWakeJitterUs_ {0.0};
    std::atomic<double> realtimeMaxWakeJitterUs_ {0.0};
    std::atomic<double> realtimeMaxRenderDurationUs_ {0.0};
    std::atomic<int> realtimeLateWakeCount_ {0};
    std::atomic<int> realtimeTelemetrySuppressCallbacks_ {0};
    std::chrono::steady_clock::time_point lastRenderWakeTime_ {};
};

RealtimeAudioEngine::RealtimeAudioEngine() : impl_(std::make_unique<Impl>()) {}
RealtimeAudioEngine::~RealtimeAudioEngine() = default;
bool RealtimeAudioEngine::start(const AudioEngineSettings& settings) { return impl_->start(settings); }
void RealtimeAudioEngine::stop() { impl_->stop(); }
AudioEngineStatus RealtimeAudioEngine::status() const { return impl_->status(); }
void RealtimeAudioEngine::setTestToneEnabled(bool enabled) { impl_->setTestToneEnabled(enabled); }
void RealtimeAudioEngine::setMetronomeEnabled(bool enabled,
                                              int tempoBpm,
                                              const std::vector<TempoMarkerState>& tempoMap,
                                              int timeSignatureNumerator,
                                              int timeSignatureDenominator,
                                              const std::string& grooveFeel,
                                              double grooveSwingAmount,
                                              const std::vector<TimeSignatureMarkerState>& timeSignatureMap,
                                              const std::string& metronomeSubdivision,
                                              double metronomeGain,
                                              const std::string& metronomeSound,
                                              bool metronomeAccentFirst) {
    impl_->setMetronomeEnabled(enabled, tempoBpm, tempoMap, timeSignatureNumerator, timeSignatureDenominator, grooveFeel, grooveSwingAmount, timeSignatureMap, metronomeSubdivision, metronomeGain, metronomeSound, metronomeAccentFirst);
}
void RealtimeAudioEngine::setMetronomeAccentPattern(const std::vector<float>& pattern) { impl_->setMetronomeAccentPattern(pattern); }
void RealtimeAudioEngine::setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled) { impl_->setMonitorDspModules(modules, enabled); }
void RealtimeAudioEngine::setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer) { impl_->setMonitorDspPathMode(mode, remoteDspServer); }
void RealtimeAudioEngine::setListenRoomSettings(const ListenRoomSettings& settings) { impl_->setListenRoomSettings(settings); }
void RealtimeAudioEngine::setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb, const std::string& talkbackRoute) {
    impl_->setMonitorStationControls(mono, listenMode, swapLeftRight, invertLeft, invertRight, mute, dim, talkback, inputTrimDb, volumeDb, dimDb, talkbackRoute);
}
void RealtimeAudioEngine::setPhysicalInputAccessAllowed(bool allowed) { impl_->setPhysicalInputAccessAllowed(allowed); }
void RealtimeAudioEngine::setMonitorListenSource(bool active) { impl_->setMonitorListenSource(active); }
void RealtimeAudioEngine::setMonitorReferenceArmed(bool armed) { impl_->setMonitorReferenceArmed(armed); }
void RealtimeAudioEngine::setTapInputMonitor(bool active) { impl_->setTapInputMonitor(active); }
void RealtimeAudioEngine::setTapInputHold(bool active) { impl_->setTapInputHold(active); }
void RealtimeAudioEngine::beginInputRecording(int source, int channelOffset, int channels) { impl_->beginInputRecording(source, channelOffset, channels); }
bool RealtimeAudioEngine::endInputRecording(const std::string& path, int bitDepth, std::string& error, double& durationSeconds, int& channels) { return impl_->endInputRecording(path, bitDepth, error, durationSeconds, channels); }
void RealtimeAudioEngine::cancelInputRecording() { impl_->cancelInputRecording(); }
bool RealtimeAudioEngine::inputRecordingActive() const { return impl_->inputRecordingActive(); }
double RealtimeAudioEngine::recordingLiveSeconds() const { return impl_->recordingLiveSeconds(); }
int RealtimeAudioEngine::recordingLivePeakCount() const { return impl_->recordingLivePeakCount(); }
int RealtimeAudioEngine::recordingLivePeaksSince(int fromBucket, float* outLR, int maxBuckets) const { return impl_->recordingLivePeaksSince(fromBucket, outLR, maxBuckets); }
int RealtimeAudioEngine::recordingChannels() const { return impl_->recordingChannels(); }
int RealtimeAudioEngine::recordingPeakSamples() const { return impl_->recordingPeakSamples(); }
void RealtimeAudioEngine::setInputDeviceLive(const std::string& deviceId) { impl_->setInputDeviceLive(deviceId); }
void RealtimeAudioEngine::setInsertTailOnStopSeconds(double seconds) { impl_->setInsertTailOnStopSeconds(seconds); }
bool RealtimeAudioEngine::loadAudioFile(const std::string& path, std::string& error) { return impl_->loadAudioFile(path, error); }
bool RealtimeAudioEngine::loadProject(const ProjectDocument& project, std::string& error) { return impl_->loadProject(project, error); }
bool RealtimeAudioEngine::updateProject(const ProjectDocument& project, std::string& error) { return impl_->updateProject(project, error); }
void RealtimeAudioEngine::updateMonitorEq(const std::vector<MonitorEqBandState>& bands) { impl_->updateMonitorEq(bands); }
void RealtimeAudioEngine::updateInterfaceModeler(const std::vector<double>& harmonics, double mix) { impl_->updateInterfaceModeler(harmonics, mix); }
void RealtimeAudioEngine::updateMonitorFir(const ResponseCurve& curveDb, int numTaps) { impl_->updateMonitorFir(curveDb, numTaps); }
void RealtimeAudioEngine::beginGraphChangeDeclick() { impl_->beginGraphChangeDeclick(); }
void RealtimeAudioEngine::endGraphChangeDeclick() { impl_->endGraphChangeDeclick(); }
int RealtimeAudioEngine::routeDelayCompensationSamplesFor(const std::string& routeName) { return impl_->routeDelayCompensationSamplesFor(routeName); }
int RealtimeAudioEngine::monitorFirLatencySamples() const { return impl_->monitorFirLatencySamples(); }
double RealtimeAudioEngine::monitorEqMagnitudeDb(double frequencyHz) const { return impl_->monitorEqMagnitudeDb(frequencyHz); }
void RealtimeAudioEngine::updateTrackSendGain(const std::string& trackName, int slot, float gainDb) { impl_->updateTrackSendGain(trackName, slot, gainDb); }
void RealtimeAudioEngine::startMeasurement(int channel, std::vector<float> signal) { impl_->startMeasurement(channel, std::move(signal)); }
void RealtimeAudioEngine::setMeasurementChannels(int outputChannel, int inputChannel) { impl_->setMeasurementChannels(outputChannel, inputChannel); }
int RealtimeAudioEngine::selectedInputChannelCount() const { return impl_->selectedInputChannelCount(); }
void RealtimeAudioEngine::setMeasurementLevelCheck(bool on) { impl_->setMeasurementLevelCheck(on); }
float RealtimeAudioEngine::measurementInputPeak() const { return impl_->measurementInputPeak(); }
float RealtimeAudioEngine::measurementSweepPeak() const { return impl_->measurementSweepPeak(); }
void RealtimeAudioEngine::setTalkbackInputChannel(int oneBased) { impl_->setTalkbackInputChannel(oneBased); }
int RealtimeAudioEngine::inputChannelActivityCount() const { return impl_->inputChannelActivityCount(); }
float RealtimeAudioEngine::inputChannelActivity(int oneBased) const { return impl_->inputChannelActivity(oneBased); }
void RealtimeAudioEngine::cancelMeasurement() { impl_->cancelMeasurement(); }
bool RealtimeAudioEngine::measurementActive() const { return impl_->measurementActive(); }
double RealtimeAudioEngine::measurementProgress() const { return impl_->measurementProgress(); }
std::vector<float> RealtimeAudioEngine::takeMeasurementCapture() { return impl_->takeMeasurementCapture(); }
bool RealtimeAudioEngine::updateClipGain(const std::string& clipId, float gainDb) { return impl_->updateClipGain(clipId, gainDb); }
bool RealtimeAudioEngine::updateClipStart(const std::string& clipId, double startSeconds) { return impl_->updateClipStart(clipId, startSeconds); }
bool RealtimeAudioEngine::updateClipBounds(const std::string& clipId, double startSeconds, double durationSeconds, double sourceOffsetSeconds) { return impl_->updateClipBounds(clipId, startSeconds, durationSeconds, sourceOffsetSeconds); }
bool RealtimeAudioEngine::updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds) { return impl_->updateClipFades(clipId, fadeInSeconds, fadeOutSeconds); }
bool RealtimeAudioEngine::updateTrackMix(const std::string& trackName, float volumeDb, float pan) { return impl_->updateTrackMix(trackName, volumeDb, pan); }
bool RealtimeAudioEngine::updateTrackConsoleChannel(const std::string& trackName, const ConsoleChannelState& console) { return impl_->updateTrackConsoleChannel(trackName, console); }
bool RealtimeAudioEngine::updateTrackSendSlot(const std::string& trackName, size_t sendIndex, const TrackSendState& send) { return impl_->updateTrackSendSlot(trackName, sendIndex, send); }
bool RealtimeAudioEngine::updateTrackInsertBypassState(const std::string& trackName, size_t insertIndex, bool bypassed) { return impl_->updateTrackInsertBypassState(trackName, insertIndex, bypassed); }
bool RealtimeAudioEngine::updateMasterInsertBypassState(size_t insertIndex, bool bypassed) { return impl_->updateMasterInsertBypassState(insertIndex, bypassed); }
bool RealtimeAudioEngine::updateTrackPlaybackState(const std::string& trackName, bool muted, bool solo) { return impl_->updateTrackPlaybackState(trackName, muted, solo); }
bool RealtimeAudioEngine::updateTrackRealtimeState(const std::string& trackName, bool recordArmed, bool inputMonitoring, bool muted, bool solo) { return impl_->updateTrackRealtimeState(trackName, recordArmed, inputMonitoring, muted, solo); }
bool RealtimeAudioEngine::updateMasterVst3Parameter(size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateMasterVst3Parameter(insertIndex, parameterId, displayName, normalizedValue); }
bool RealtimeAudioEngine::updateTrackVst3Parameter(const std::string& trackName, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateTrackVst3Parameter(trackName, insertIndex, parameterId, displayName, normalizedValue); }
bool RealtimeAudioEngine::updateInstrumentVst3Parameter(const std::string& trackName, size_t slotIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateInstrumentVst3Parameter(trackName, slotIndex, parameterId, displayName, normalizedValue); }
bool RealtimeAudioEngine::updateInstrumentComponentState(const std::string& trackName, size_t slotIndex, const std::string& componentStateBase64) { return impl_->updateInstrumentComponentState(trackName, slotIndex, componentStateBase64); }
bool RealtimeAudioEngine::updateMonitorSpeakerVst3Parameter(int speakerSlot, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateMonitorSpeakerVst3Parameter(speakerSlot, insertIndex, parameterId, displayName, normalizedValue); }
void RealtimeAudioEngine::queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events) { impl_->queueLiveMidiEvents(trackName, events); }
void RealtimeAudioEngine::setEditorInstrumentMonitor(bool active, const std::string& trackName) { impl_->setEditorInstrumentMonitor(active, trackName); }
void RealtimeAudioEngine::pushEditorInstrumentMonitorInterleaved(const float* samples, int64_t frameCount) { impl_->pushEditorInstrumentMonitorInterleaved(samples, frameCount); }
void RealtimeAudioEngine::setTransportRunning(bool running) { impl_->setTransportRunning(running); }
void RealtimeAudioEngine::setTransportRecordingActive(bool active) { impl_->setTransportRecordingActive(active); }
void RealtimeAudioEngine::rewind() { impl_->rewind(); }
void RealtimeAudioEngine::seek(double seconds) { impl_->seek(seconds); }

} // namespace neuracoust::daw
#endif
