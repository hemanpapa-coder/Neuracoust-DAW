#include "audio/RealtimeAudioEngine.h"
#include "audio/MetronomeClick.h"
#include "audio/MonitorOutputRouting.h"

#include <algorithm>
#include <optional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio/AsioRuntimeAdapter.h"
#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/ProjectAudioRenderer.h"
#include "audio/RemoteDspPluginCatalog.h"
#include "audio/WavFile.h"
#include <audioclient.h>
#include <cmath>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>
#include <windows.h>
#include <avrt.h>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace neuracoust::daw {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

std::string safeMonitorListenMode(const std::string& listenMode) {
    return (listenMode == "L" || listenMode == "R" || listenMode == "M" || listenMode == "S") ? listenMode : "LR";
}

bool monitorListenModeIsMidSide(const std::string& listenMode) {
    return listenMode == "M" || listenMode == "S";
}

float dbToGain(float db) {
    if (db <= -119.5f) {
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

template <typename T>
void releaseIfSet(T*& ptr) {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    return result;
}

std::string wideToUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string hresultMessage(const char* prefix, HRESULT hr) {
    char buffer[160] = {};
    std::snprintf(buffer, sizeof(buffer), "%s (0x%08lX)", prefix, static_cast<unsigned long>(hr));
    return buffer;
}

std::string stripWasapiOutputPrefix(const std::string& id) {
    constexpr const char* prefix = "wasapi-output:";
    return id.rfind(prefix, 0) == 0 ? id.substr(std::strlen(prefix)) : id;
}

bool hasPrefix(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

AudioDriverKind outputDriverForSettings(const AudioEngineSettings& settings) {
    if (settings.outputDriver != AudioDriverKind::Unknown) {
        return settings.outputDriver;
    }
    if (hasPrefix(settings.outputDeviceId, "asio:")) {
        return AudioDriverKind::ASIO;
    }
    if (hasPrefix(settings.outputDeviceId, "wasapi-output:") || settings.outputDeviceId.empty()) {
        return AudioDriverKind::WASAPI;
    }
    return AudioDriverKind::WASAPI;
}

std::string affinityMaskHex(DWORD_PTR mask) {
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<unsigned long long>(mask);
    return stream.str();
}

DWORD_PTR parseAffinityMask(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
    return end != text.c_str() ? static_cast<DWORD_PTR>(value) : 0;
}

DWORD_PTR highestEfficiencyProcessorMask() {
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        length == 0) {
        return 0;
    }

    std::vector<unsigned char> buffer(length);
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        return 0;
    }

    BYTE bestEfficiency = 0;
    DWORD_PTR bestMask = 0;
    for (DWORD offset = 0; offset < length;) {
        auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore && entry->Processor.GroupCount > 0) {
            const auto& group = entry->Processor.GroupMask[0];
            if (group.Group == 0) {
                const BYTE efficiency = entry->Processor.EfficiencyClass;
                if (bestMask == 0 || efficiency > bestEfficiency) {
                    bestEfficiency = efficiency;
                    bestMask = group.Mask;
                } else if (efficiency == bestEfficiency) {
                    bestMask |= group.Mask;
                }
            }
        }
        offset += entry->Size;
    }
    return bestMask;
}

DWORD_PTR fallbackAudioAffinityMask() {
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || processMask == 0) {
        return 0;
    }
    if ((processMask & (processMask - 1)) == 0) {
        return processMask;
    }
    return processMask & ~static_cast<DWORD_PTR>(1);
}

std::string effectiveTrackInsertDspExecutionMode(const TrackInsertSlot& insert) {
    if (isRemoteInternalDspExecutionMode(insert.dspExecutionMode) &&
        remoteDspCapabilityForInsert(insert, false, true).moduleId.empty()) {
        return "native";
    }
    return insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode;
}

InsertState trackInsertToRenderInsert(const TrackInsertSlot& insert) {
    InsertState renderInsert;
    renderInsert.pluginName = insert.pluginName;
    renderInsert.pluginFormat = insert.pluginFormat;
    renderInsert.pluginPath = insert.pluginPath;
    renderInsert.pluginClassId = insert.pluginClassId;
    renderInsert.pluginClassName = insert.pluginClassName;
    renderInsert.bypassed = insert.bypassed;
    renderInsert.available = insert.enabled;
    renderInsert.dspExecutionMode = effectiveTrackInsertDspExecutionMode(insert);
    renderInsert.assignedDspServerId = insert.assignedDspServerId;
    renderInsert.serverModuleId = insert.serverModuleId;
    renderInsert.reportedLatencySamples = insert.reportedLatencySamples;
    renderInsert.dspAvailable = insert.dspAvailable;
    renderInsert.dspLastError = insert.dspLastError;
    renderInsert.parameters = insert.parameters;
    return renderInsert;
}

bool trackInsertShouldRunInNativeRouteGraph(const TrackInsertSlot& insert) {
    if (!insert.enabled || insert.bypassed || insert.pluginPath.empty() ||
        (isRemoteInternalDspExecutionMode(effectiveTrackInsertDspExecutionMode(insert)) && !insert.serverModuleId.empty())) {
        return false;
    }
    return isVst3MasterInsert(trackInsertToRenderInsert(insert));
}

std::optional<size_t> nativeRouteGraphInsertIndex(const TrackState& track, size_t slotIndex) {
    if (slotIndex >= track.inserts.size() ||
        !trackInsertShouldRunInNativeRouteGraph(track.inserts[slotIndex])) {
        return std::nullopt;
    }
    size_t routeIndex = 0;
    for (size_t index = 0; index < slotIndex; ++index) {
        if (trackInsertShouldRunInNativeRouteGraph(track.inserts[index])) {
            ++routeIndex;
        }
    }
    return routeIndex;
}

DWORD_PTR selectedWindowsAffinityMask(const AudioEngineSettings& settings) {
    if (settings.windowsProcessorAffinityMask != 0) {
        return static_cast<DWORD_PTR>(settings.windowsProcessorAffinityMask);
    }
    if (const char* env = std::getenv("NEURACOUST_DAW_WINDOWS_AFFINITY_MASK")) {
        if (const auto mask = parseAffinityMask(env); mask != 0) {
            return mask;
        }
    }
    if (settings.windowsProcessorAffinityMode == "p_core_preferred" ||
        settings.windowsProcessorAffinityMode == "p_core_high_priority") {
        if (const auto mask = highestEfficiencyProcessorMask(); mask != 0) {
            return mask;
        }
    }
    return fallbackAudioAffinityMask();
}

std::string deviceFriendlyName(IMMDevice* device) {
    IPropertyStore* properties = nullptr;
    if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return {};
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(properties->GetValue(kPkeyDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = wideToUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    properties->Release();
    return name;
}

bool isFloatFormat(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && format->wBitsPerSample == 32;
    }
    return false;
}

bool isPcm16Format(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && format->wBitsPerSample == 16;
    }
    return false;
}

float clampSample(float sample) {
    return std::max(-1.0f, std::min(1.0f, sample));
}

std::pair<float, float> applyMonitorStationControls(float left, float right, const AudioEngineSettings& settings) {
    if (settings.monitorStationMute) {
        return {0.0f, 0.0f};
    }
    if (settings.monitorStationListenMode == "M") {
        const float mid = (left + right) * 0.5f;
        left = mid;
        right = mid;
    } else if (settings.monitorStationListenMode == "S") {
        const float side = (left - right) * 0.5f;
        left = side;
        right = -side;
    } else if (settings.monitorStationListenMode == "L") {
        if (settings.monitorStationMono) {
            right = left;
        } else {
            right = 0.0f;
        }
    } else if (settings.monitorStationListenMode == "R") {
        if (settings.monitorStationMono) {
            left = right;
        } else {
            left = 0.0f;
        }
    } else if (settings.monitorStationMono) {
        const float mono = (left + right) * 0.5f;
        left = mono;
        right = mono;
    }
    if (settings.monitorStationSwapLeftRight) {
        std::swap(left, right);
    }
    if (settings.monitorStationInvertLeft) {
        left = -left;
    }
    if (settings.monitorStationInvertRight) {
        right = -right;
    }
    if (settings.monitorStationDim) {
        const float dimGain = dbToGain(std::max(-60.0f, std::min(0.0f, settings.monitorStationDimDb)));
        left *= dimGain;
        right *= dimGain;
    }
    const float monitorGain = dbToGain(settings.monitorVolumeDb);
    left *= monitorGain;
    right *= monitorGain;
    return {left, right};
}

} // namespace

class RealtimeAudioEngine::Impl {
public:
    ~Impl() { stop(); }

    bool start(const AudioEngineSettings& requestedSettings) {
        const double requestedPlaybackSeconds = std::max(0.0, status().playbackSeconds);
        stop();
        settings_ = requestedSettings;
        settings_.outputDriver = outputDriverForSettings(settings_);
        if (settings_.monitorModules.empty()) {
            settings_.monitorModules = defaultMonitorDspModules();
        }
        status_.outputDriver = settings_.outputDriver;

        if (settings_.outputDriver == AudioDriverKind::ASIO) {
            const auto asioStatus = asioAdapterStatusForDeviceId(settings_.outputDeviceId);
            status_.running = false;
            status_.sampleRate = settings_.sampleRate;
            status_.outputChannels = 0;
            status_.deviceName = settings_.outputDeviceId.empty() ? "ASIO Output" : settings_.outputDeviceId;
            status_.message = asioStatus.diagnosticSummary.empty()
                ? asioStatus.message
                : asioStatus.message + " " + asioStatus.diagnosticSummary;
            return false;
        }
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInitialized_ = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            status_.message = hresultMessage("Could not initialize COM for WASAPI.", hr);
            return false;
        }

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                              nullptr,
                              CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator_));
        if (FAILED(hr)) {
            status_.message = hresultMessage("Could not create WASAPI device enumerator.", hr);
            stop();
            return false;
        }

        const auto requestedDeviceId = stripWasapiOutputPrefix(settings_.outputDeviceId);
        if (!requestedDeviceId.empty()) {
            const auto wideId = utf8ToWide(requestedDeviceId);
            hr = enumerator_->GetDevice(wideId.c_str(), &device_);
        } else {
            hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        }
        if (FAILED(hr) || device_ == nullptr) {
            status_.message = hresultMessage("No usable WASAPI output device is available.", hr);
            stop();
            return false;
        }

        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
        if (FAILED(hr)) {
            status_.message = hresultMessage("Could not activate WASAPI audio client.", hr);
            stop();
            return false;
        }

        hr = client_->GetMixFormat(&mixFormat_);
        if (FAILED(hr) || mixFormat_ == nullptr) {
            status_.message = hresultMessage("Could not read WASAPI mix format.", hr);
            stop();
            return false;
        }

        const REFERENCE_TIME requestedBufferDuration100ns = static_cast<REFERENCE_TIME>(
            std::max(1.0, (static_cast<double>(std::max(1, settings_.bufferSize)) /
                           std::max(1.0, static_cast<double>(mixFormat_->nSamplesPerSec))) *
                              10000000.0));
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 requestedBufferDuration100ns,
                                 0,
                                 mixFormat_,
                                 nullptr);
        if (FAILED(hr)) {
            status_.message = hresultMessage("Could not initialize WASAPI shared output.", hr);
            stop();
            return false;
        }

        hr = client_->GetBufferSize(&bufferFrameCount_);
        if (FAILED(hr) || bufferFrameCount_ == 0) {
            status_.message = hresultMessage("Could not get WASAPI output buffer size.", hr);
            stop();
            return false;
        }

        hr = client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient_));
        if (FAILED(hr)) {
            status_.message = hresultMessage("Could not get WASAPI render client.", hr);
            stop();
            return false;
        }

        renderEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (renderEvent_ == nullptr || FAILED(client_->SetEventHandle(renderEvent_))) {
            status_.message = "Could not configure WASAPI event-driven render scheduling.";
            stop();
            return false;
        }

        const double sampleRate = mixFormat_->nSamplesPerSec > 0 ? static_cast<double>(mixFormat_->nSamplesPerSec) : settings_.sampleRate;
        settings_.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        monitorProcessor_.configure(settings_.sampleRate, settings_.monitorModules);
        {
            std::lock_guard<std::mutex> lock(playbackMutex_);
            std::string prepareError;
            if (!prepareRealtimeInsertChainLocked(bufferFrameCount_, prepareError)) {
                status_.running = false;
                status_.sampleRate = settings_.sampleRate;
                status_.outputChannels = 0;
                status_.deviceName = "Realtime Project";
                status_.message = prepareError;
                stop();
                return false;
            }
        }
        if (requestedPlaybackSeconds > 0.0) {
            seek(requestedPlaybackSeconds);
        }

        running_.store(true);
        resetRealtimeTelemetry();
        applyWindowsProcessorPolicy();
        renderThread_ = std::thread([this] { renderLoop(); });

        hr = client_->Start();
        if (FAILED(hr)) {
            running_.store(false);
            if (renderThread_.joinable()) {
                renderThread_.join();
            }
            status_.message = hresultMessage("Could not start WASAPI output.", hr);
            stop();
            return false;
        }

        status_.running = true;
        status_.outputDriver = AudioDriverKind::WASAPI;
        status_.sampleRate = settings_.sampleRate;
        status_.outputChannels = mixFormat_->nChannels;
        status_.deviceName = deviceFriendlyName(device_);
        if (status_.deviceName.empty()) {
            status_.deviceName = "WASAPI Output";
        }
        status_.message = "WASAPI shared output engine running.";
        if (realtimeInsertChain_.activeVst3Count() > 0) {
            status_.message = "WASAPI shared output engine running with realtime VST3 master inserts.";
        }
        if (!windowsProcessorPolicyMessage_.empty()) {
            status_.message += " " + windowsProcessorPolicyMessage_;
        }
        sampleRateForStatus_.store(std::max(1.0, settings_.sampleRate));
        listenRoomSender_.configure(settings_.sampleRate, settings_.listenRoom);
        return true;
    }

    void stop() {
        running_.store(false);
        if (renderThread_.joinable()) {
            renderThread_.join();
        }
        if (client_ != nullptr) {
            client_->Stop();
        }
        restoreWindowsProcessorPolicy();
        releaseIfSet(renderClient_);
        if (renderEvent_ != nullptr) {
            CloseHandle(renderEvent_);
            renderEvent_ = nullptr;
        }
        if (mixFormat_ != nullptr) {
            CoTaskMemFree(mixFormat_);
            mixFormat_ = nullptr;
        }
        releaseIfSet(client_);
        releaseIfSet(device_);
        releaseIfSet(enumerator_);
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        realtimeInsertChain_.reset();
        resetMeteringLocked();
        projectMeters_ = {};
        playbackFrame_ = 0;
        playbackFrameForStatus_.store(0);
        listenRoomSender_.stop();
        status_.running = false;
    }

    AudioEngineStatus status() const {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto copy = status_;
        copy.outputPeakLeft = outputPeakLeft_.load();
        copy.outputPeakRight = outputPeakRight_.load();
        copy.phaseCorrelation = phaseCorrelation_.load();
        copy.spectrumLow = spectrumLow_.load();
        copy.spectrumMid = spectrumMid_.load();
        copy.spectrumHigh = spectrumHigh_.load();
        copy.trackMeterNames = projectMeters_.trackNames;
        copy.trackPeakLeft = projectMeters_.trackPeakLeft;
        copy.trackPeakRight = projectMeters_.trackPeakRight;
        copy.playbackSeconds = static_cast<double>(playbackFrameForStatus_.load()) / std::max(1.0, sampleRateForStatus_.load());
        copy.requestedPerformanceCoreCount = std::max(1, settings_.requestedPerformanceCoreCount);
        copy.realtimeCallbackCount = realtimeCallbackCount_.load();
        copy.realtimeAverageWakeJitterUs = realtimeAverageWakeJitterUs_.load();
        copy.realtimeMaxWakeJitterUs = realtimeMaxWakeJitterUs_.load();
        copy.realtimeMaxRenderDurationUs = realtimeMaxRenderDurationUs_.load();
        copy.realtimeLateWakeCount = realtimeLateWakeCount_.load();
        copy.activeRealtimeVst3MasterInsertCount = static_cast<int>(realtimeInsertChain_.activeVst3Count());
        copy.activeRealtimeVst3TrackInsertCount = 0;
        copy.activeOfflineVst3TrackInsertCount = static_cast<int>(projectPlan_.activeTrackVst3InsertLabels.size());
        copy.listenRoom = listenRoomSender_.status();
        copy.monitorDspPathMode = settings_.monitorDspPathMode;
        copy.monitorPathDescription = resolveMonitorOutputRoute(settings_.monitorModules, std::max(2, static_cast<int>(copy.outputChannels))).description;
        return copy;
    }

    void setTestToneEnabled(bool enabled) {
        settings_.testToneEnabled = enabled;
        syncProjectMonitorDspRenderPathLocked();
    }

    void setMetronomeEnabled(bool enabled,
                             int tempoBpm,
                             const std::vector<TempoMarkerState>& tempoMap,
                             int timeSignatureNumerator,
                             int timeSignatureDenominator,
                             const std::string& grooveFeel,
                             double grooveSwingAmount,
                             const std::vector<TimeSignatureMarkerState>& timeSignatureMap,
                             const std::string& metronomeSubdivision) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
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
        settings_.tempoMap = tempoMap;
        std::sort(settings_.tempoMap.begin(), settings_.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });
        settings_.timeSignatureMap = timeSignatureMap;
        std::sort(settings_.timeSignatureMap.begin(), settings_.timeSignatureMap.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });
        syncProjectMonitorDspRenderPathLocked();
    }

    void setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        settings_.monitorModules = modules;
        settings_.monitorDspEnabled = enabled;
        monitorProcessor_.configure(settings_.sampleRate, settings_.monitorModules);
        projectRenderState_.reset();
        syncProjectMonitorDspRenderPathLocked();
        suppressRealtimeTelemetryAfterGraphChange();
        status_.monitorPathDescription = resolveMonitorOutputRoute(settings_.monitorModules, std::max(2, static_cast<int>(status_.outputChannels))).description;
    }

    void setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        settings_.monitorDspPathMode = (mode == "external" || mode == "nds" || mode == "remote_external" || mode == "auto") ? mode : "internal";
        settings_.remoteDspServer = remoteDspServer;
        syncProjectMonitorDspRenderPathLocked();
        status_.monitorDspPathMode = settings_.monitorDspPathMode;
    }
    void setListenRoomSettings(const ListenRoomSettings& settings) {
        settings_.listenRoom = normalizedListenRoomSettings(settings);
        listenRoomSender_.configure(settings_.sampleRate, settings_.listenRoom);
        status_.listenRoom = listenRoomSender_.status();
        status_.listenRoom.message = settings_.listenRoom.enabled ? "Listen Room sender armed." : "Listen Room stopped.";
    }

    void setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb = -20.0f, const std::string& talkbackRoute = "listen_room") {
        std::lock_guard<std::mutex> lock(playbackMutex_);
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
        projectPlan_.monitorInputTrimDb = settings_.monitorInputTrimDb;
        settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, volumeDb));
    }

    void setPhysicalInputAccessAllowed(bool allowed) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        settings_.physicalInputAccessAllowed = allowed;
    }

    bool loadAudioFile(const std::string& path, std::string& error) {
        WavAudioData data;
        if (!readPcmWavFile(path, data, error)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(playbackMutex_);
        const double sourceSampleRate = data.sampleRate > 0 ? static_cast<double>(data.sampleRate) : settings_.sampleRate;
        playback_ = std::move(data);
        projectPlan_ = {};
        projectRenderState_.reset();
        projectBlock_.clear();
        projectMeters_ = {};
        realtimeInsertChain_.reset();
        playbackFrame_ = 0;
        playbackFrameForStatus_.store(0);
        resetMeteringLocked();
        sampleRateForStatus_.store(std::max(1.0, sourceSampleRate));
        status_.message = "Loaded audio file.";
        return true;
    }

    bool loadProject(const ProjectDocument& project, std::string& error) {
        ProjectAudioRenderPlan plan;
        if (!makeProjectAudioRenderPlan(project, plan, error)) {
            return false;
        }
        plan.renderTrackVst3Inserts = true;
        std::lock_guard<std::mutex> lock(playbackMutex_);
        const double projectSampleRate = plan.sampleRate > 0.0 ? plan.sampleRate : settings_.sampleRate;
        plan.transportRecordingActive = settings_.transportRecordingActive;
        projectPlan_ = std::move(plan);
        projectRenderState_.reset();
        settings_.tempoMap = project.tempoMap;
        settings_.tempoBpm = std::max(1, project.tempoBpm);
        settings_.timeSignatureNumerator = std::max(1, std::min(16, project.timeSignatureNumerator));
        settings_.timeSignatureDenominator = std::max(1, std::min(16, project.timeSignatureDenominator));
        settings_.timeSignatureMap = project.timeSignatureMap;
        settings_.grooveFeel = project.grooveFeel.empty() ? "straight" : project.grooveFeel;
        settings_.grooveSwingAmount = std::max(0.0, std::min(1.0, project.grooveSwingAmount));
        settings_.monitorStationListenMode = safeMonitorListenMode(project.monitorStationListenMode);
        const bool msMode = monitorListenModeIsMidSide(settings_.monitorStationListenMode);
        settings_.monitorStationMono = msMode ? false : project.monitorStationMono;
        settings_.monitorStationSwapLeftRight = msMode ? false : project.monitorStationSwapLeftRight;
        settings_.monitorStationInvertLeft = project.monitorStationInvertLeft;
        settings_.monitorStationInvertRight = project.monitorStationInvertRight;
        settings_.monitorStationMute = project.monitorStationMute;
        settings_.monitorStationDim = project.monitorStationDim;
        settings_.monitorStationTalkback = project.monitorStationTalkback;
        settings_.monitorStationDimDb = project.monitorStationDimDb;
        settings_.monitorStationTalkbackRoute = project.monitorStationTalkbackRoute.empty() ? "listen_room" : project.monitorStationTalkbackRoute;
        settings_.monitorInputTrimDb = std::max(-12.0f, std::min(0.0f, project.monitorInputTrimDb));
        settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, project.monitorVolumeDb));
        settings_.monitorModules = project.monitorModules.empty() ? defaultMonitorDspModules() : project.monitorModules;
        projectPlan_.monitorModules = settings_.monitorModules;
        monitorProcessor_.configure(std::max(1.0, projectSampleRate), settings_.monitorModules);
        settings_.listenRoom.enabled = project.listenRoomEnabled;
        settings_.listenRoom.sessionName = project.listenRoomSessionName;
        settings_.listenRoom.source = project.listenRoomSource;
        settings_.listenRoom.quality = project.listenRoomQuality;
        settings_.listenRoom.latencyMode = project.listenRoomLatencyMode;
        settings_.listenRoom.transportMode = project.listenRoomTransportMode;
        settings_.listenRoom.relayHost = project.listenRoomRelayHost;
        settings_.listenRoom.accessToken = project.listenRoomAccessToken;
        settings_.listenRoom.relayHttpPort = project.listenRoomRelayHttpPort;
        settings_.listenRoom.relayTcpIngestPort = project.listenRoomRelayTcpIngestPort;
        settings_.listenRoom = normalizedListenRoomSettings(settings_.listenRoom);
        listenRoomSender_.configure(settings_.sampleRate, settings_.listenRoom);
        playback_ = {};
        realtimeInsertChain_.reset();
        resetTrackMetersLocked();
        playbackFrame_ = 0;
        playbackFrameForStatus_.store(0);
        resetMeteringLocked();
        sampleRateForStatus_.store(std::max(1.0, projectSampleRate));
        syncProjectMonitorDspRenderPathLocked();
        suppressRealtimeTelemetryAfterGraphChange();
        status_.message = projectPlan_.hasMissingMedia
            ? "Loaded project arrangement with missing media rendered as silence."
            : "Loaded project arrangement.";
        return true;
    }

    bool updateProject(const ProjectDocument& project, std::string& error) {
        ProjectAudioRenderPlan plan;
        if (!makeProjectAudioRenderPlan(project, plan, error)) {
            return false;
        }
        plan.renderTrackVst3Inserts = true;
        std::lock_guard<std::mutex> lock(playbackMutex_);
        const double projectSampleRate = plan.sampleRate > 0.0 ? plan.sampleRate : settings_.sampleRate;
        plan.transportRecordingActive = settings_.transportRecordingActive;
        projectPlan_ = std::move(plan);
        projectRenderState_.reset();
        settings_.tempoMap = project.tempoMap;
        settings_.tempoBpm = std::max(1, project.tempoBpm);
        settings_.timeSignatureNumerator = std::max(1, std::min(16, project.timeSignatureNumerator));
        settings_.timeSignatureDenominator = std::max(1, std::min(16, project.timeSignatureDenominator));
        settings_.timeSignatureMap = project.timeSignatureMap;
        settings_.grooveFeel = project.grooveFeel.empty() ? "straight" : project.grooveFeel;
        settings_.grooveSwingAmount = std::max(0.0, std::min(1.0, project.grooveSwingAmount));
        settings_.monitorStationListenMode = safeMonitorListenMode(project.monitorStationListenMode);
        const bool msMode = monitorListenModeIsMidSide(settings_.monitorStationListenMode);
        settings_.monitorStationMono = msMode ? false : project.monitorStationMono;
        settings_.monitorStationSwapLeftRight = msMode ? false : project.monitorStationSwapLeftRight;
        settings_.monitorStationInvertLeft = project.monitorStationInvertLeft;
        settings_.monitorStationInvertRight = project.monitorStationInvertRight;
        settings_.monitorStationMute = project.monitorStationMute;
        settings_.monitorStationDim = project.monitorStationDim;
        settings_.monitorStationTalkback = project.monitorStationTalkback;
        settings_.monitorStationDimDb = project.monitorStationDimDb;
        settings_.monitorStationTalkbackRoute = project.monitorStationTalkbackRoute.empty() ? "listen_room" : project.monitorStationTalkbackRoute;
        settings_.monitorInputTrimDb = std::max(-12.0f, std::min(0.0f, project.monitorInputTrimDb));
        settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, project.monitorVolumeDb));
        settings_.monitorModules = project.monitorModules.empty() ? defaultMonitorDspModules() : project.monitorModules;
        projectPlan_.monitorModules = settings_.monitorModules;
        monitorProcessor_.configure(std::max(1.0, projectSampleRate), settings_.monitorModules);
        settings_.listenRoom.enabled = project.listenRoomEnabled;
        settings_.listenRoom.sessionName = project.listenRoomSessionName;
        settings_.listenRoom.source = project.listenRoomSource;
        settings_.listenRoom.quality = project.listenRoomQuality;
        settings_.listenRoom.latencyMode = project.listenRoomLatencyMode;
        settings_.listenRoom.transportMode = project.listenRoomTransportMode;
        settings_.listenRoom.relayHost = project.listenRoomRelayHost;
        settings_.listenRoom.accessToken = project.listenRoomAccessToken;
        settings_.listenRoom.relayHttpPort = project.listenRoomRelayHttpPort;
        settings_.listenRoom.relayTcpIngestPort = project.listenRoomRelayTcpIngestPort;
        settings_.listenRoom = normalizedListenRoomSettings(settings_.listenRoom);
        listenRoomSender_.configure(settings_.sampleRate, settings_.listenRoom);
        playback_ = {};
        sampleRateForStatus_.store(std::max(1.0, projectSampleRate));
        syncProjectMonitorDspRenderPathLocked();
        suppressRealtimeTelemetryAfterGraphChange();
        status_.message = projectPlan_.hasMissingMedia
            ? "Updated project arrangement with missing media rendered as silence."
            : "Updated project arrangement without resetting playback.";
        error.clear();
        return true;
    }

    bool updateClipGain(const std::string& clipId, float gainDb) {
        if (clipId.empty() || !std::isfinite(gainDb)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto clipIt = std::find_if(projectPlan_.clips.begin(), projectPlan_.clips.end(), [&](const ProjectRenderClip& renderClip) {
            return renderClip.clip.id == clipId;
        });
        if (clipIt == projectPlan_.clips.end()) {
            return false;
        }
        clipIt->clip.gainDb = std::max(-60.0f, std::min(24.0f, gainDb));
        status_.message = "Updated clip gain without reloading project.";
        return true;
    }

    bool updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds) {
        if (clipId.empty() || !std::isfinite(fadeInSeconds) || !std::isfinite(fadeOutSeconds)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto clipIt = std::find_if(projectPlan_.clips.begin(), projectPlan_.clips.end(), [&](const ProjectRenderClip& renderClip) {
            return renderClip.clip.id == clipId;
        });
        if (clipIt == projectPlan_.clips.end()) {
            return false;
        }
        const double maxFade = std::max(0.0, clipIt->clip.durationSeconds * 0.5);
        clipIt->clip.fadeInSeconds = std::max(0.0, std::min(maxFade, fadeInSeconds));
        clipIt->clip.fadeOutSeconds = std::max(0.0, std::min(maxFade, fadeOutSeconds));
        status_.message = "Updated clip fades without reloading project.";
        return true;
    }

    bool updateTrackMix(const std::string& trackName, float volumeDb, float pan) {
        if (trackName.empty() || !std::isfinite(volumeDb) || !std::isfinite(pan)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
            return track.name == trackName;
        });
        if (trackIt == projectPlan_.tracks.end()) {
            return false;
        }
        trackIt->volumeDb = std::max(-120.0f, std::min(12.0f, volumeDb));
        trackIt->pan = std::max(-1.0f, std::min(1.0f, pan));
        status_.message = "Updated track mix without reloading project.";
        return true;
    }

    bool updateTrackSendSlot(const std::string& trackName, size_t sendIndex, const TrackSendState& send) {
        if (trackName.empty() || !std::isfinite(send.gainDb) || !std::isfinite(send.pan)) {
            return false;
        }
        std::unique_lock<std::mutex> lock(playbackMutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
            return track.name == trackName;
        });
        if (trackIt == projectPlan_.tracks.end() || sendIndex >= trackIt->sends.size()) {
            return false;
        }
        TrackSendState normalized = send;
        if (normalized.busName.empty()) {
            normalized.enabled = false;
        }
        normalized.gainDb = std::max(-60.0f, std::min(12.0f, normalized.gainDb));
        normalized.pan = std::max(-1.0f, std::min(1.0f, normalized.pan));
        trackIt->sends[sendIndex] = normalized;
        status_.message = "Updated track send without reloading project.";
        return true;
    }

    bool updateTrackInsertBypassState(const std::string& trackName, size_t insertIndex, bool bypassed) {
        if (trackName.empty()) {
            return false;
        }
        std::unique_lock<std::mutex> lock(playbackMutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
            return track.name == trackName;
        });
        if (trackIt == projectPlan_.tracks.end() || insertIndex >= trackIt->inserts.size()) {
            return false;
        }
        auto& insert = trackIt->inserts[insertIndex];
        if (!insert.enabled || insert.pluginPath.empty() || insert.pluginFormat == "None") {
            return false;
        }
        insert.bypassed = bypassed;
        status_.message = bypassed
            ? "Bypassed track insert without reloading project."
            : "Enabled track insert without reloading project.";
        return true;
    }

    bool updateMasterInsertBypassState(size_t insertIndex, bool bypassed) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (insertIndex >= projectPlan_.activeVst3Inserts.size()) {
            return false;
        }
        projectPlan_.activeVst3Inserts[insertIndex].bypassed = bypassed;
        if (!realtimeInsertChain_.isPrepared() ||
            realtimeInsertChain_.activeVst3Count() != projectPlan_.activeVst3Inserts.size()) {
            return false;
        }
        std::vector<bool> bypassStates;
        bypassStates.reserve(projectPlan_.activeVst3Inserts.size());
        for (const auto& insert : projectPlan_.activeVst3Inserts) {
            bypassStates.push_back(insert.bypassed || !insert.available);
        }
        realtimeInsertChain_.setBypassStates(bypassStates);
        projectRenderState_.masterInsertChain.setBypassStates(bypassStates);
        status_.message = bypassed
            ? "Bypassed master insert without reloading project."
            : "Enabled master insert without reloading project.";
        return true;
    }

    bool updateTrackPlaybackState(const std::string& trackName, bool muted, bool solo) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
            return track.name == trackName;
        });
        if (trackIt == projectPlan_.tracks.end()) {
            return false;
        }
        trackIt->muted = muted;
        trackIt->solo = solo;
        status_.message = "Updated track mute/solo without reloading project.";
        return true;
    }

    bool updateTrackRealtimeState(const std::string& trackName, bool recordArmed, bool inputMonitoring, bool muted, bool solo) {
        if (trackName.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
            return track.name == trackName;
        });
        if (trackIt == projectPlan_.tracks.end()) {
            return false;
        }
        trackIt->recordArmed = recordArmed;
        trackIt->inputMonitoring = inputMonitoring;
        trackIt->muted = muted;
        trackIt->solo = solo;
        status_.message = "Updated track live monitoring state without reloading project.";
        return true;
    }

    bool updateMasterVst3Parameter(size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (insertIndex >= projectPlan_.activeVst3Inserts.size()) {
            return false;
        }
        auto& parameters = projectPlan_.activeVst3Inserts[insertIndex].parameters;
        const float value = static_cast<float>(std::max(0.0, std::min(1.0, normalizedValue)));
        auto found = std::find_if(parameters.begin(), parameters.end(), [&](const Vst3ParameterValueState& parameter) {
            return parameter.parameterId == parameterId;
        });
        if (found == parameters.end()) {
            parameters.push_back({parameterId, displayName.empty() ? "Param " + std::to_string(parameterId) : displayName, value});
        } else {
            if (!displayName.empty()) {
                found->displayName = displayName;
            }
            found->normalizedValue = value;
        }
        realtimeInsertChain_.updateParameter(insertIndex, parameterId, displayName, normalizedValue);
        status_.message = "Updated master VST3 parameter without reloading project.";
        return true;
    }

    bool updateTrackVst3Parameter(const std::string& trackName, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
            return track.name == trackName;
        });
        if (trackIt == projectPlan_.tracks.end() || insertIndex >= trackIt->inserts.size()) {
            return false;
        }
        auto& parameters = trackIt->inserts[insertIndex].parameters;
        const float value = static_cast<float>(std::max(0.0, std::min(1.0, normalizedValue)));
        auto found = std::find_if(parameters.begin(), parameters.end(), [&](const Vst3ParameterValueState& parameter) {
            return parameter.parameterId == parameterId;
        });
        if (found == parameters.end()) {
            parameters.push_back({parameterId, displayName.empty() ? "Param " + std::to_string(parameterId) : displayName, value});
        } else {
            if (!displayName.empty()) {
                found->displayName = displayName;
            }
            found->normalizedValue = value;
        }
        bool updatedChain = false;
        if (const auto routeInsertIndex = nativeRouteGraphInsertIndex(*trackIt, insertIndex)) {
            auto routeChainIt = projectRenderState_.routeInsertChains.find(trackName);
            if (routeChainIt != projectRenderState_.routeInsertChains.end()) {
                updatedChain = routeChainIt->second.updateParameter(*routeInsertIndex,
                                                                    parameterId,
                                                                    displayName,
                                                                    normalizedValue) || updatedChain;
            }
        }
        status_.message = "Updated track VST3 parameter without reloading project.";
        return true;
    }

    bool updateMonitorSpeakerVst3Parameter(int speakerSlot, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (settings_.monitorModules.empty()) {
            return false;
        }
        const int safeSlot = std::max(1, std::min(3, speakerSlot));
        auto& module = settings_.monitorModules[0];
        std::vector<TrackInsertSlot>* inserts = &module.speakerInsertsA;
        if (safeSlot == 2) {
            inserts = &module.speakerInsertsB;
        } else if (safeSlot == 3) {
            inserts = &module.speakerInsertsC;
        }
        if (insertIndex >= inserts->size()) {
            return false;
        }
        auto& parameters = (*inserts)[insertIndex].parameters;
        const float value = static_cast<float>(std::max(0.0, std::min(1.0, normalizedValue)));
        auto found = std::find_if(parameters.begin(), parameters.end(), [&](const Vst3ParameterValueState& parameter) {
            return parameter.parameterId == parameterId;
        });
        if (found == parameters.end()) {
            parameters.push_back({parameterId, displayName.empty() ? "Param " + std::to_string(parameterId) : displayName, value});
        } else {
            if (!displayName.empty()) {
                found->displayName = displayName;
            }
            found->normalizedValue = value;
        }
        status_.message = "Stored monitor speaker VST3 parameter.";
        return true;
    }

    void queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events) {
        if (trackName.empty() || events.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto& queue = projectRenderState_.liveMidiEvents[trackName];
        queue.insert(queue.end(), events.begin(), events.end());
        constexpr size_t kMaxQueuedLiveMidiEvents = 4096;
        if (queue.size() > kMaxQueuedLiveMidiEvents) {
            queue.erase(queue.begin(), queue.end() - static_cast<std::ptrdiff_t>(kMaxQueuedLiveMidiEvents));
        }
    }

    void setTransportRecordingActive(bool active) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        settings_.transportRecordingActive = active;
        projectPlan_.transportRecordingActive = active;
        syncProjectMonitorDspRenderPathLocked();
        status_.message = active
            ? "Tape record monitor path active for record-armed tracks."
            : "Tape record monitor path returned to playback monitoring.";
    }

    void rewind() {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        playbackFrame_ = 0;
        projectRenderState_.reset();
        playbackFrameForStatus_.store(0);
    }

    void seek(double seconds) {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        const double seekSampleRate = projectPlan_.sampleRate > 0.0
            ? projectPlan_.sampleRate
            : (playback_.sampleRate > 0 ? static_cast<double>(playback_.sampleRate) : settings_.sampleRate);
        playbackFrame_ = std::max<int64_t>(0, static_cast<int64_t>(std::round(std::max(0.0, seconds) * seekSampleRate)));
        projectRenderState_.resetForSeek();
        armSeekRampLocked(seekSampleRate);
        playbackFrameForStatus_.store(playbackFrame_);
        sampleRateForStatus_.store(std::max(1.0, seekSampleRate));
    }

private:
    void armSeekRampLocked(double sampleRate) {
        const double safeSampleRate = std::max(1.0, sampleRate);
        seekRampSamplesTotal_ = std::max<int64_t>(1, static_cast<int64_t>(std::round(safeSampleRate * 0.006)));
        seekRampSamplesRemaining_ = seekRampSamplesTotal_;
    }

    bool projectMonitorDspCanRenderInGraphLocked() const {
        return !projectPlan_.clips.empty() &&
            settings_.monitorDspEnabled &&
            settings_.monitorDspPathMode == "internal" &&
            !settings_.testToneEnabled &&
            !settings_.metronomeEnabled &&
            playback_.channels <= 0;
    }

    void syncProjectMonitorDspRenderPathLocked() {
        const bool shouldRenderInGraph = projectMonitorDspCanRenderInGraphLocked();
        if (projectPlan_.renderMonitorDsp == shouldRenderInGraph) {
            return;
        }
        projectPlan_.renderMonitorDsp = shouldRenderInGraph;
        projectRenderState_.reset();
    }

    void applyWindowsProcessorPolicy() {
        windowsProcessorPolicyMessage_.clear();
        if (!settings_.windowsProcessorAffinityEnabled) {
            return;
        }

        if (!processPolicyCaptured_) {
            originalPriorityClass_ = GetPriorityClass(GetCurrentProcess());
            DWORD_PTR processMask = 0;
            DWORD_PTR systemMask = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
                originalProcessAffinityMask_ = processMask;
                processPolicyCaptured_ = true;
            }
        }

        const auto mask = selectedWindowsAffinityMask(settings_);
        if (mask != 0 && SetProcessAffinityMask(GetCurrentProcess(), mask)) {
            appliedProcessAffinityMask_ = mask;
            windowsProcessorPolicyMessage_ = "Windows CPU affinity " + affinityMaskHex(mask) + " requested.";
        } else {
            windowsProcessorPolicyMessage_ = "Windows CPU affinity request could not be applied.";
        }

        if (settings_.windowsProcessorAffinityMode == "p_core_high_priority") {
            if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
                windowsPriorityRaised_ = true;
                windowsProcessorPolicyMessage_ += " Process priority: high.";
            } else {
                windowsProcessorPolicyMessage_ += " Process priority request failed.";
            }
        }
    }

    void restoreWindowsProcessorPolicy() {
        if (processPolicyCaptured_ && originalProcessAffinityMask_ != 0) {
            SetProcessAffinityMask(GetCurrentProcess(), originalProcessAffinityMask_);
        }
        if (windowsPriorityRaised_ && originalPriorityClass_ != 0) {
            SetPriorityClass(GetCurrentProcess(), originalPriorityClass_);
        }
        appliedProcessAffinityMask_ = 0;
        windowsPriorityRaised_ = false;
        processPolicyCaptured_ = false;
        originalProcessAffinityMask_ = 0;
        originalPriorityClass_ = 0;
    }

    void applyWindowsRenderThreadPolicy() {
        DWORD mmcssTaskIndex = 0;
        mmcssHandle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
        if (mmcssHandle_ != nullptr) {
            AvSetMmThreadPriority(mmcssHandle_, AVRT_PRIORITY_CRITICAL);
        }
        if (!settings_.windowsProcessorAffinityEnabled) {
            return;
        }
        if (appliedProcessAffinityMask_ != 0) {
            SetThreadAffinityMask(GetCurrentThread(), appliedProcessAffinityMask_);
        }
        SetThreadPriority(GetCurrentThread(),
                          settings_.windowsProcessorAffinityMode == "p_core_high_priority"
                              ? THREAD_PRIORITY_TIME_CRITICAL
                              : THREAD_PRIORITY_HIGHEST);
    }

    bool prepareRealtimeInsertChainLocked(UINT32 maxBlockSize, std::string& error) {
        realtimeInsertChain_.reset();
        if (!projectPlan_.hasActiveVst3Inserts) {
            error.clear();
            return true;
        }
        return realtimeInsertChain_.prepare(
            projectPlan_,
            settings_.sampleRate,
            static_cast<int>(std::max<UINT32>(1, maxBlockSize)),
            error);
    }

    void resetTrackMetersLocked() {
        projectMeters_ = {};
        for (const auto& track : projectPlan_.tracks) {
            if (track.trackType == "master" || track.trackType == "monitor" || track.name == "Master" || track.name == "Monitor") {
                continue;
            }
            projectMeters_.trackNames.push_back(track.name);
            projectMeters_.trackPeakLeft.push_back(0.0f);
            projectMeters_.trackPeakRight.push_back(0.0f);
        }
    }

    void storeTrackMetersLocked(const ProjectAudioBlockMeters& meters) {
        projectMeters_ = meters;
        for (auto& peak : projectMeters_.trackPeakLeft) {
            peak = std::min(1.0f, std::max(0.0f, peak));
        }
        for (auto& peak : projectMeters_.trackPeakRight) {
            peak = std::min(1.0f, std::max(0.0f, peak));
        }
    }

    void resetMeteringLocked() {
        outputPeakLeft_.store(0.0f);
        outputPeakRight_.store(0.0f);
        phaseCorrelation_.store(0.0f);
        spectrumLow_.store(0.0f);
        spectrumMid_.store(0.0f);
        spectrumHigh_.store(0.0f);
    }

    void resetRealtimeTelemetry() {
        realtimeCallbackCount_.store(0);
        realtimeAverageWakeJitterUs_.store(0.0);
        realtimeMaxWakeJitterUs_.store(0.0);
        realtimeMaxRenderDurationUs_.store(0.0);
        realtimeLateWakeCount_.store(0);
        lastRenderWakeTime_ = {};
    }

    void recordRealtimeTelemetry(std::chrono::steady_clock::time_point wakeTime,
                                 std::chrono::steady_clock::time_point renderStart,
                                 std::chrono::steady_clock::time_point renderEnd,
                                 UINT32 frameCount) {
        const double expectedPeriodUs = (static_cast<double>(std::max<UINT32>(1, frameCount)) /
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

    void suppressRealtimeTelemetryAfterGraphChange() {
        resetRealtimeTelemetry();
        realtimeTelemetrySuppressCallbacks_.store(12, std::memory_order_relaxed);
    }

    void renderLoop() {
        applyWindowsRenderThreadPolicy();
        while (running_.load()) {
            if (renderEvent_ != nullptr) {
                const DWORD waitResult = WaitForSingleObject(renderEvent_, 2000);
                if (waitResult != WAIT_OBJECT_0) {
                    continue;
                }
            }
            UINT32 padding = 0;
            if (client_ == nullptr || renderClient_ == nullptr || FAILED(client_->GetCurrentPadding(&padding))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const UINT32 available = bufferFrameCount_ > padding ? bufferFrameCount_ - padding : 0;
            if (available == 0) {
                if (renderEvent_ == nullptr) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
                continue;
            }

            BYTE* buffer = nullptr;
            if (FAILED(renderClient_->GetBuffer(available, &buffer)) || buffer == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const auto wakeTime = std::chrono::steady_clock::now();
            const auto renderStart = wakeTime;
            renderFrames(buffer, available);
            const auto renderEnd = std::chrono::steady_clock::now();
            renderClient_->ReleaseBuffer(available, 0);
            recordRealtimeTelemetry(wakeTime, renderStart, renderEnd, available);
        }
        if (mmcssHandle_ != nullptr) {
            AvRevertMmThreadCharacteristics(mmcssHandle_);
            mmcssHandle_ = nullptr;
        }
    }

    void renderFrames(BYTE* rawBuffer, UINT32 frameCount) {
        const int channels = mixFormat_ != nullptr ? std::max<int>(1, mixFormat_->nChannels) : 2;
        const bool floatOutput = isFloatFormat(mixFormat_);
        const bool pcm16Output = isPcm16Format(mixFormat_);
        const double increment = kTwoPi * settings_.testToneFrequency / std::max(1.0, settings_.sampleRate);

        std::lock_guard<std::mutex> lock(playbackMutex_);
        syncProjectMonitorDspRenderPathLocked();
        const bool projectMonitorDspRenderedInGraph = projectPlan_.renderMonitorDsp && !projectPlan_.clips.empty();
        if (!projectPlan_.clips.empty()) {
            ProjectAudioBlockMeters meters;
            renderProjectAudioBlockWithStateAndMeters(projectPlan_, projectRenderState_, playbackFrame_, frameCount, projectBlock_, &meters);
            if (projectRenderState_.masterInsertProcessingFailed) {
                std::fill(projectBlock_.begin(), projectBlock_.end(), 0.0f);
                for (auto& peak : meters.trackPeakLeft) {
                    peak = 0.0f;
                }
                for (auto& peak : meters.trackPeakRight) {
                    peak = 0.0f;
                }
                status_.message = "Realtime VST3 processing failed: " + projectRenderState_.masterInsertLastError;
            }
            storeTrackMetersLocked(meters);
        }
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        double crossEnergy = 0.0;
        double lowEnergy = 0.0;
        double midEnergy = 0.0;
        double highEnergy = 0.0;
        listenRoomBlock_.assign(static_cast<size_t>(frameCount) * 2u, 0.0f);
        for (UINT32 frame = 0; frame < frameCount; ++frame) {
            float left = settings_.testToneEnabled ? static_cast<float>(std::sin(phase_) * 0.12) : 0.0f;
            float right = left;
            const float click = renderMetronomeClickSampleAtFrame(playbackFrame_, settings_);
            left += click;
            right += click;
            if (!projectPlan_.clips.empty()) {
                const auto projectIndex = static_cast<size_t>(frame * 2);
                if (projectIndex + 1 < projectBlock_.size()) {
                    left += projectBlock_[projectIndex];
                    right += projectBlock_[projectIndex + 1];
                }
            } else if (playback_.channels > 0 && playbackFrame_ < playback_.frameCount()) {
                const auto sourceIndex = static_cast<size_t>(playbackFrame_ * playback_.channels);
                if (sourceIndex < playback_.interleavedSamples.size()) {
                    left += playback_.interleavedSamples[sourceIndex];
                    right += playback_.channels > 1 && sourceIndex + 1 < playback_.interleavedSamples.size()
                        ? playback_.interleavedSamples[sourceIndex + 1]
                        : playback_.interleavedSamples[sourceIndex];
                }
            }

            const float monitorInputTrimGain = projectMonitorDspRenderedInGraph
                ? 1.0f
                : dbToGain(std::max(-12.0f, std::min(0.0f, settings_.monitorInputTrimDb)));
            left *= monitorInputTrimGain;
            right *= monitorInputTrimGain;
            if (settings_.monitorDspEnabled && !projectMonitorDspRenderedInGraph) {
                const auto processed = monitorProcessor_.process({left, right});
                left = processed.left;
                right = processed.right;
            }
            std::tie(left, right) = applyMonitorStationControls(left, right, settings_);
            if (seekRampSamplesRemaining_ > 0 && seekRampSamplesTotal_ > 0) {
                const int64_t elapsed = seekRampSamplesTotal_ - seekRampSamplesRemaining_;
                const float seekGain = static_cast<float>(std::max(0.0, std::min(1.0, static_cast<double>(elapsed) / static_cast<double>(seekRampSamplesTotal_))));
                left *= seekGain;
                right *= seekGain;
                --seekRampSamplesRemaining_;
            }
            peakLeft = std::max(peakLeft, std::abs(left));
            peakRight = std::max(peakRight, std::abs(right));
            leftEnergy += static_cast<double>(left) * static_cast<double>(left);
            rightEnergy += static_cast<double>(right) * static_cast<double>(right);
            crossEnergy += static_cast<double>(left) * static_cast<double>(right);
            const float mono = (left + right) * 0.5f;
            lowBandState_ += 0.04f * (mono - lowBandState_);
            midBandState_ += 0.18f * ((mono - lowBandState_) - midBandState_);
            const float low = lowBandState_;
            const float mid = midBandState_;
            const float high = mono - low - mid;
            lowEnergy += static_cast<double>(low) * static_cast<double>(low);
            midEnergy += static_cast<double>(mid) * static_cast<double>(mid);
            highEnergy += static_cast<double>(high) * static_cast<double>(high);
            const auto listenIndex = static_cast<size_t>(frame) * 2u;
            listenRoomBlock_[listenIndex] = left;
            listenRoomBlock_[listenIndex + 1u] = right;

            const auto route = resolveMonitorOutputRoute(settings_.monitorModules, channels);
            for (int channel = 0; channel < channels; ++channel) {
                float sample = 0.0f;
                if (route.assigned && route.available) {
                    if (channel == route.leftChannel) {
                        sample = left;
                    } else if (channel == route.rightChannel) {
                        sample = right;
                    }
                }
                const auto index = static_cast<size_t>(frame * channels + channel);
                if (floatOutput) {
                    reinterpret_cast<float*>(rawBuffer)[index] = clampSample(sample);
                } else if (pcm16Output) {
                    reinterpret_cast<int16_t*>(rawBuffer)[index] = static_cast<int16_t>(clampSample(sample) * 32767.0f);
                } else {
                    rawBuffer[index * (mixFormat_->wBitsPerSample / 8)] = 0;
                }
            }

            phase_ += increment;
            if (phase_ >= kTwoPi) {
                phase_ -= kTwoPi;
            }
            ++playbackFrame_;
            playbackFrameForStatus_.store(playbackFrame_);
        }
        if (settings_.listenRoom.enabled && !listenRoomBlock_.empty()) {
            listenRoomSender_.pushInterleavedStereo(listenRoomBlock_.data(), static_cast<int64_t>(frameCount));
        }
        outputPeakLeft_.store(std::min(1.0f, peakLeft));
        outputPeakRight_.store(std::min(1.0f, peakRight));
        const double correlationDenominator = std::sqrt(leftEnergy * rightEnergy);
        const float correlation = correlationDenominator > 0.000000001
            ? static_cast<float>(std::max(-1.0, std::min(1.0, crossEnergy / correlationDenominator)))
            : 0.0f;
        const double frames = std::max<double>(1.0, frameCount);
        phaseCorrelation_.store(correlation);
        spectrumLow_.store(std::min(1.0f, static_cast<float>(std::sqrt(lowEnergy / frames))));
        spectrumMid_.store(std::min(1.0f, static_cast<float>(std::sqrt(midEnergy / frames))));
        spectrumHigh_.store(std::min(1.0f, static_cast<float>(std::sqrt(highEnergy / frames))));
    }

    AudioEngineSettings settings_;
    AudioEngineStatus status_;
    std::atomic<bool> running_ {false};
    bool comInitialized_ = false;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;
    HANDLE renderEvent_ = nullptr;
    HANDLE mmcssHandle_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;
    UINT32 bufferFrameCount_ = 0;
    std::thread renderThread_;
    mutable std::mutex playbackMutex_;
    WavAudioData playback_;
    ProjectAudioRenderPlan projectPlan_;
    ProjectAudioRenderState projectRenderState_;
    std::vector<float> projectBlock_;
    std::vector<float> listenRoomBlock_;
    ProjectAudioBlockMeters projectMeters_;
    RealtimeMasterInsertChain realtimeInsertChain_;
    MonitorDspProcessor monitorProcessor_;
    ListenRoomSender listenRoomSender_;
    double phase_ = 0.0;
    int64_t playbackFrame_ = 0;
    int64_t seekRampSamplesRemaining_ = 0;
    int64_t seekRampSamplesTotal_ = 0;
    std::atomic<int64_t> playbackFrameForStatus_ {0};
    std::atomic<double> sampleRateForStatus_ {48000.0};
    std::atomic<float> outputPeakLeft_ {0.0f};
    std::atomic<float> outputPeakRight_ {0.0f};
    std::atomic<float> phaseCorrelation_ {0.0f};
    std::atomic<float> spectrumLow_ {0.0f};
    std::atomic<float> spectrumMid_ {0.0f};
    std::atomic<float> spectrumHigh_ {0.0f};
    std::atomic<uint64_t> realtimeCallbackCount_ {0};
    std::atomic<double> realtimeAverageWakeJitterUs_ {0.0};
    std::atomic<double> realtimeMaxWakeJitterUs_ {0.0};
    std::atomic<double> realtimeMaxRenderDurationUs_ {0.0};
    std::atomic<int> realtimeLateWakeCount_ {0};
    std::atomic<int> realtimeTelemetrySuppressCallbacks_ {0};
    std::chrono::steady_clock::time_point lastRenderWakeTime_ {};
    float lowBandState_ = 0.0f;
    float midBandState_ = 0.0f;
    DWORD_PTR originalProcessAffinityMask_ = 0;
    DWORD_PTR appliedProcessAffinityMask_ = 0;
    DWORD originalPriorityClass_ = 0;
    bool processPolicyCaptured_ = false;
    bool windowsPriorityRaised_ = false;
    std::string windowsProcessorPolicyMessage_;
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
                                              const std::string& metronomeSubdivision) {
    impl_->setMetronomeEnabled(enabled, tempoBpm, tempoMap, timeSignatureNumerator, timeSignatureDenominator, grooveFeel, grooveSwingAmount, timeSignatureMap, metronomeSubdivision);
}
void RealtimeAudioEngine::setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled) { impl_->setMonitorDspModules(modules, enabled); }
void RealtimeAudioEngine::setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer) { impl_->setMonitorDspPathMode(mode, remoteDspServer); }
void RealtimeAudioEngine::setListenRoomSettings(const ListenRoomSettings& settings) { impl_->setListenRoomSettings(settings); }
void RealtimeAudioEngine::setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb, const std::string& talkbackRoute) {
    impl_->setMonitorStationControls(mono, listenMode, swapLeftRight, invertLeft, invertRight, mute, dim, talkback, inputTrimDb, volumeDb, dimDb, talkbackRoute);
}
void RealtimeAudioEngine::setPhysicalInputAccessAllowed(bool allowed) { impl_->setPhysicalInputAccessAllowed(allowed); }
bool RealtimeAudioEngine::loadAudioFile(const std::string& path, std::string& error) { return impl_->loadAudioFile(path, error); }
bool RealtimeAudioEngine::loadProject(const ProjectDocument& project, std::string& error) { return impl_->loadProject(project, error); }
bool RealtimeAudioEngine::updateProject(const ProjectDocument& project, std::string& error) { return impl_->updateProject(project, error); }
bool RealtimeAudioEngine::updateClipGain(const std::string& clipId, float gainDb) { return impl_->updateClipGain(clipId, gainDb); }
bool RealtimeAudioEngine::updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds) { return impl_->updateClipFades(clipId, fadeInSeconds, fadeOutSeconds); }
bool RealtimeAudioEngine::updateTrackMix(const std::string& trackName, float volumeDb, float pan) { return impl_->updateTrackMix(trackName, volumeDb, pan); }
bool RealtimeAudioEngine::updateTrackSendSlot(const std::string& trackName, size_t sendIndex, const TrackSendState& send) { return impl_->updateTrackSendSlot(trackName, sendIndex, send); }
bool RealtimeAudioEngine::updateMasterInsertBypassState(size_t insertIndex, bool bypassed) { return impl_->updateMasterInsertBypassState(insertIndex, bypassed); }
bool RealtimeAudioEngine::updateTrackPlaybackState(const std::string& trackName, bool muted, bool solo) { return impl_->updateTrackPlaybackState(trackName, muted, solo); }
bool RealtimeAudioEngine::updateTrackRealtimeState(const std::string& trackName, bool recordArmed, bool inputMonitoring, bool muted, bool solo) { return impl_->updateTrackRealtimeState(trackName, recordArmed, inputMonitoring, muted, solo); }
bool RealtimeAudioEngine::updateMasterVst3Parameter(size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateMasterVst3Parameter(insertIndex, parameterId, displayName, normalizedValue); }
bool RealtimeAudioEngine::updateTrackVst3Parameter(const std::string& trackName, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateTrackVst3Parameter(trackName, insertIndex, parameterId, displayName, normalizedValue); }
bool RealtimeAudioEngine::updateMonitorSpeakerVst3Parameter(int speakerSlot, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateMonitorSpeakerVst3Parameter(speakerSlot, insertIndex, parameterId, displayName, normalizedValue); }
void RealtimeAudioEngine::queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events) { impl_->queueLiveMidiEvents(trackName, events); }
void RealtimeAudioEngine::setTransportRecordingActive(bool active) { impl_->setTransportRecordingActive(active); }
void RealtimeAudioEngine::rewind() { impl_->rewind(); }
void RealtimeAudioEngine::seek(double seconds) { impl_->seek(seconds); }

} // namespace neuracoust::daw
#elif !defined(__APPLE__)
namespace neuracoust::daw {

class RealtimeAudioEngine::Impl {
public:
    bool start(const AudioEngineSettings&) {
        status_.message = "Realtime audio is not implemented on this platform yet.";
        return false;
    }
    void stop() {
        status_.running = false;
        status_.outputPeakLeft = 0.0f;
        status_.outputPeakRight = 0.0f;
        status_.phaseCorrelation = 0.0f;
        status_.spectrumLow = 0.0f;
        status_.spectrumMid = 0.0f;
        status_.spectrumHigh = 0.0f;
        for (auto& peak : status_.trackPeakLeft) {
            peak = 0.0f;
        }
        for (auto& peak : status_.trackPeakRight) {
            peak = 0.0f;
        }
    }
    AudioEngineStatus status() const { return status_; }
    void setTestToneEnabled(bool enabled) { settings_.testToneEnabled = enabled; }
    void setMetronomeEnabled(bool enabled,
                             int tempoBpm,
                             const std::vector<TempoMarkerState>& tempoMap,
                             int timeSignatureNumerator,
                             int timeSignatureDenominator,
                             const std::string& grooveFeel,
                             double grooveSwingAmount,
                             const std::vector<TimeSignatureMarkerState>& timeSignatureMap,
                             const std::string& metronomeSubdivision) {
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
        settings_.tempoMap = tempoMap;
        std::sort(settings_.tempoMap.begin(), settings_.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });
        settings_.timeSignatureMap = timeSignatureMap;
        std::sort(settings_.timeSignatureMap.begin(), settings_.timeSignatureMap.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });
    }
    void setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled) {
        settings_.monitorModules = modules;
        settings_.monitorDspEnabled = enabled;
    }
    void setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer) {
        settings_.monitorDspPathMode = (mode == "external" || mode == "nds" || mode == "remote_external" || mode == "auto") ? mode : "internal";
        settings_.remoteDspServer = remoteDspServer;
        status_.monitorDspPathMode = settings_.monitorDspPathMode;
    }
    void setListenRoomSettings(const ListenRoomSettings& settings) {
        settings_.listenRoom = normalizedListenRoomSettings(settings);
        status_.listenRoom.enabled = settings_.listenRoom.enabled;
        status_.listenRoom.shareUrl = listenRoomShareUrl(settings_.listenRoom);
        status_.listenRoom.sessionId = listenRoomSessionId(settings_.listenRoom.sessionName);
        status_.listenRoom.maxQueuedBlocks = listenRoomMaxQueuedBlocks(settings_.listenRoom);
        status_.listenRoom.packetFrames = listenRoomPacketFrames(settings_.listenRoom);
        status_.listenRoom.latencyTargetMs = listenRoomLatencyTargetMs(settings_.listenRoom);
        status_.listenRoom.targetBitrateKbps = listenRoomTargetBitrateKbps(settings_.listenRoom);
        status_.listenRoom.activeCodec = listenRoomEffectiveCodec(settings_.listenRoom);
        status_.listenRoom.qualityLabel = listenRoomQualityLabel(settings_.listenRoom);
        status_.listenRoom.transportMode = settings_.listenRoom.transportMode;
        status_.listenRoom.message = "Listen Room is not implemented on this platform yet.";
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
        projectPlan_.monitorInputTrimDb = settings_.monitorInputTrimDb;
        settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, volumeDb));
    }
    void setPhysicalInputAccessAllowed(bool allowed) { settings_.physicalInputAccessAllowed = allowed; }
    bool loadAudioFile(const std::string&, std::string& error) {
        error = "Audio file playback is not implemented on this platform yet.";
        return false;
    }
    bool loadProject(const ProjectDocument&, std::string& error) {
        error = "Project playback is not implemented on this platform yet.";
        return false;
    }
    bool updateProject(const ProjectDocument&, std::string& error) {
        error = "Project playback is not implemented on this platform yet.";
        return false;
    }
    bool updateClipGain(const std::string&, float) { return false; }
    bool updateClipFades(const std::string&, double, double) { return false; }
    bool updateTrackMix(const std::string&, float, float) { return false; }
    bool updateTrackSendSlot(const std::string&, size_t, const TrackSendState&) { return false; }
    bool updateTrackInsertBypassState(const std::string&, size_t, bool) { return false; }
    bool updateMasterInsertBypassState(size_t, bool) { return false; }
    bool updateTrackPlaybackState(const std::string&, bool, bool) { return false; }
    bool updateTrackRealtimeState(const std::string&, bool, bool, bool, bool) { return false; }
    bool updateMasterVst3Parameter(size_t, uint32_t, const std::string&, double) { return false; }
    bool updateTrackVst3Parameter(const std::string&, size_t, uint32_t, const std::string&, double) { return false; }
    bool updateMonitorSpeakerVst3Parameter(int, size_t, uint32_t, const std::string&, double) { return false; }
    void queueLiveMidiEvents(const std::string&, const std::vector<Vst3MidiEvent>&) {}
    void setTransportRunning(bool running) {
        settings_.transportRunning = running;
        status_.transportRunning = running;
    }
    void setTransportRecordingActive(bool active) { settings_.transportRecordingActive = active; }
    void rewind() { status_.playbackSeconds = 0.0; }
    void seek(double seconds) { status_.playbackSeconds = std::max(0.0, seconds); }

private:
    AudioEngineSettings settings_;
    AudioEngineStatus status_;
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
                                              const std::string& metronomeSubdivision) {
    impl_->setMetronomeEnabled(enabled, tempoBpm, tempoMap, timeSignatureNumerator, timeSignatureDenominator, grooveFeel, grooveSwingAmount, timeSignatureMap, metronomeSubdivision);
}
void RealtimeAudioEngine::setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled) { impl_->setMonitorDspModules(modules, enabled); }
void RealtimeAudioEngine::setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer) { impl_->setMonitorDspPathMode(mode, remoteDspServer); }
void RealtimeAudioEngine::setListenRoomSettings(const ListenRoomSettings& settings) { impl_->setListenRoomSettings(settings); }
void RealtimeAudioEngine::setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb, const std::string& talkbackRoute) {
    (void)dimDb;
    (void)talkbackRoute;
    impl_->setMonitorStationControls(mono, listenMode, swapLeftRight, invertLeft, invertRight, mute, dim, talkback, inputTrimDb, volumeDb, dimDb, talkbackRoute);
}
void RealtimeAudioEngine::setPhysicalInputAccessAllowed(bool allowed) { impl_->setPhysicalInputAccessAllowed(allowed); }
bool RealtimeAudioEngine::loadAudioFile(const std::string& path, std::string& error) { return impl_->loadAudioFile(path, error); }
bool RealtimeAudioEngine::loadProject(const ProjectDocument& project, std::string& error) { return impl_->loadProject(project, error); }
bool RealtimeAudioEngine::updateProject(const ProjectDocument& project, std::string& error) { return impl_->updateProject(project, error); }
bool RealtimeAudioEngine::updateClipGain(const std::string& clipId, float gainDb) { return impl_->updateClipGain(clipId, gainDb); }
bool RealtimeAudioEngine::updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds) { return impl_->updateClipFades(clipId, fadeInSeconds, fadeOutSeconds); }
bool RealtimeAudioEngine::updateTrackMix(const std::string& trackName, float volumeDb, float pan) { return impl_->updateTrackMix(trackName, volumeDb, pan); }
bool RealtimeAudioEngine::updateTrackSendSlot(const std::string& trackName, size_t sendIndex, const TrackSendState& send) { return impl_->updateTrackSendSlot(trackName, sendIndex, send); }
bool RealtimeAudioEngine::updateTrackInsertBypassState(const std::string& trackName, size_t insertIndex, bool bypassed) { return impl_->updateTrackInsertBypassState(trackName, insertIndex, bypassed); }
bool RealtimeAudioEngine::updateMasterInsertBypassState(size_t insertIndex, bool bypassed) { return impl_->updateMasterInsertBypassState(insertIndex, bypassed); }
bool RealtimeAudioEngine::updateTrackPlaybackState(const std::string& trackName, bool muted, bool solo) { return impl_->updateTrackPlaybackState(trackName, muted, solo); }
bool RealtimeAudioEngine::updateTrackRealtimeState(const std::string& trackName, bool recordArmed, bool inputMonitoring, bool muted, bool solo) { return impl_->updateTrackRealtimeState(trackName, recordArmed, inputMonitoring, muted, solo); }
bool RealtimeAudioEngine::updateMasterVst3Parameter(size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateMasterVst3Parameter(insertIndex, parameterId, displayName, normalizedValue); }
bool RealtimeAudioEngine::updateTrackVst3Parameter(const std::string& trackName, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateTrackVst3Parameter(trackName, insertIndex, parameterId, displayName, normalizedValue); }
bool RealtimeAudioEngine::updateMonitorSpeakerVst3Parameter(int speakerSlot, size_t insertIndex, uint32_t parameterId, const std::string& displayName, double normalizedValue) { return impl_->updateMonitorSpeakerVst3Parameter(speakerSlot, insertIndex, parameterId, displayName, normalizedValue); }
void RealtimeAudioEngine::queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events) { impl_->queueLiveMidiEvents(trackName, events); }
void RealtimeAudioEngine::setTransportRunning(bool running) { impl_->setTransportRunning(running); }
void RealtimeAudioEngine::setTransportRecordingActive(bool active) { impl_->setTransportRecordingActive(active); }
void RealtimeAudioEngine::rewind() { impl_->rewind(); }
void RealtimeAudioEngine::seek(double seconds) { impl_->seek(seconds); }

} // namespace neuracoust::daw
#endif
