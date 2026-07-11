#include "audio/NeuracoustDspEngine.h"
#include "audio/MetronomeClick.h"
#include "audio/RemoteDspPluginCatalog.h"
#include "audio/RemoteDspServerClient.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>
#include <vector>

namespace neuracoust::daw {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isSignalGeneratorInsertName(const std::string& pluginName, const std::string& pluginClassName) {
    const std::string name = lowercaseCopy(pluginName);
    const std::string className = lowercaseCopy(pluginClassName);
    return name.find("emo-generator") != std::string::npos ||
        name.find("signal generator") != std::string::npos ||
        className.find("emo-generator") != std::string::npos ||
        className.find("signal generator") != std::string::npos;
}

void ensureSignalGeneratorDefaults(InsertState& insert) {
    if (!isSignalGeneratorInsertName(insert.pluginName, insert.pluginClassName)) {
        return;
    }
    auto onOffIt = std::find_if(insert.parameters.begin(), insert.parameters.end(), [](const Vst3ParameterValueState& parameter) {
        return parameter.parameterId == 0u;
    });
    if (onOffIt == insert.parameters.end()) {
        insert.parameters.push_back({0u, "On Off", 1.0});
    } else {
        if (onOffIt->displayName.empty()) {
            onOffIt->displayName = "On Off";
        }
        onOffIt->normalizedValue = 1.0f;
    }
}

std::string effectiveTrackInsertDspExecutionMode(const TrackInsertSlot& insert) {
    if (isRemoteInternalDspExecutionMode(insert.dspExecutionMode) &&
        remoteDspCapabilityForInsert(insert, false, true).moduleId.empty()) {
        return "native";
    }
    return insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode;
}

unsigned int estimatedRemoteDspLatencySamples(const RemoteDspServerSettings& settings,
                                              double sampleRate,
                                              int maxBlockSize,
                                              double measuredRoundTripMs) {
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const uint16_t networkBufferFrames = static_cast<uint16_t>(
        std::max<uint16_t>(128u, std::min<uint16_t>(1024u, settings.networkBufferFrames)));
    const double measuredSamples = std::isfinite(measuredRoundTripMs) && measuredRoundTripMs > 0.0
        ? measuredRoundTripMs * sr / 1000.0
        : 0.0;
    const double safetyBlock = static_cast<double>(std::max(1, maxBlockSize));
    return static_cast<unsigned int>(std::ceil(std::max<double>(
        networkBufferFrames,
        static_cast<double>(networkBufferFrames) + measuredSamples + safetyBlock)));
}

float dbToGain(float db) {
    if (db <= -119.5f) {
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

std::pair<float, float> applyStereoGainPan(float left, float right, float gainDb, float pan) {
    const float clamped = std::max(-1.0f, std::min(1.0f, pan));
    const float leftTrim = clamped > 0.0f ? 1.0f - clamped : 1.0f;
    const float rightTrim = clamped < 0.0f ? 1.0f + clamped : 1.0f;
    const float gain = dbToGain(gainDb);
    return {left * gain * leftTrim, right * gain * rightTrim};
}

std::string safeMonitorListenMode(const std::string& listenMode) {
    return (listenMode == "L" || listenMode == "R" || listenMode == "M" || listenMode == "S") ? listenMode : "LR";
}

bool monitorListenModeIsMidSide(const std::string& listenMode) {
    return listenMode == "M" || listenMode == "S";
}

bool trackSupportsPhysicalInputMonitoring(const TrackState& track) {
    return track.trackType == "audio" ||
        track.trackType == "aux" ||
        track.trackType == "bus_folder";
}

bool isMainBus(const std::string& busName) {
    return busName.rfind("Main", 0) == 0;
}

bool isMasterOutputBus(const std::string& busName) {
    return busName == "Master" || isMainBus(busName);
}

const std::vector<TrackInsertSlot>* activeSpeakerOutputInserts(const std::vector<MonitorDspModule>& modules,
                                                               int* activeSlot = nullptr) {
    if (activeSlot != nullptr) {
        *activeSlot = -1;
    }
    if (modules.empty() || !modules[0].enabled) {
        return nullptr;
    }
    const auto& module = modules[0];
    const int slot = std::max(0, std::min(2, module.activeTargetSlot)) + 1;
    const std::string* route = &module.speakerOutputA;
    const std::vector<TrackInsertSlot>* inserts = &module.speakerInsertsA;
    if (slot == 2) {
        route = &module.speakerOutputB;
        inserts = &module.speakerInsertsB;
    } else if (slot == 3) {
        route = &module.speakerOutputC;
        inserts = &module.speakerInsertsC;
    }
    if (route->empty() || *route == "None") {
        return nullptr;
    }
    if (activeSlot != nullptr) {
        *activeSlot = slot;
    }
    return inserts;
}

std::vector<TrackInsertSlot>* monitorSpeakerInsertsForSlot(std::vector<MonitorDspModule>& modules,
                                                           int speakerSlot) {
    if (modules.empty()) {
        return nullptr;
    }
    auto& module = modules[0];
    if (speakerSlot == 2) {
        return &module.speakerInsertsB;
    }
    if (speakerSlot == 3) {
        return &module.speakerInsertsC;
    }
    return &module.speakerInsertsA;
}

bool parameterStatesEqual(const std::vector<Vst3ParameterValueState>& left,
                          const std::vector<Vst3ParameterValueState>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].parameterId != right[index].parameterId ||
            left[index].displayName != right[index].displayName ||
            left[index].normalizedValue != right[index].normalizedValue) {
            return false;
        }
    }
    return true;
}

bool trackInsertSlotsEqual(const std::vector<TrackInsertSlot>& left,
                           const std::vector<TrackInsertSlot>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.pluginName != b.pluginName ||
            a.pluginFormat != b.pluginFormat ||
            a.pluginPath != b.pluginPath ||
            a.bypassed != b.bypassed ||
            a.enabled != b.enabled ||
            a.dspExecutionMode != b.dspExecutionMode ||
            a.assignedDspServerId != b.assignedDspServerId ||
            a.serverModuleId != b.serverModuleId ||
            a.reportedLatencySamples != b.reportedLatencySamples ||
            a.dspAvailable != b.dspAvailable ||
            a.dspLastError != b.dspLastError ||
            a.pluginClassId != b.pluginClassId ||
            a.pluginClassName != b.pluginClassName ||
            !parameterStatesEqual(a.parameters, b.parameters)) {
            return false;
        }
    }
    return true;
}

bool monitorModulesOnlyActiveTargetSlotChanged(const std::vector<MonitorDspModule>& left,
                                               const std::vector<MonitorDspModule>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    bool slotChanged = false;
    for (size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.activeTargetSlot != b.activeTargetSlot) {
            slotChanged = true;
        }
        if (a.id != b.id ||
            a.displayName != b.displayName ||
            a.stage != b.stage ||
            a.enabled != b.enabled ||
            a.realModel != b.realModel ||
            a.targetModelA != b.targetModelA ||
            a.targetModelB != b.targetModelB ||
            a.targetModelC != b.targetModelC ||
            a.speakerOutputA != b.speakerOutputA ||
            a.speakerOutputB != b.speakerOutputB ||
            a.speakerOutputC != b.speakerOutputC ||
            a.streamingPreview != b.streamingPreview ||
            a.speakerRoomEqA != b.speakerRoomEqA ||
            a.speakerRoomEqB != b.speakerRoomEqB ||
            a.speakerRoomEqC != b.speakerRoomEqC ||
            a.speakerSimulationWeightA != b.speakerSimulationWeightA ||
            a.speakerSimulationWeightB != b.speakerSimulationWeightB ||
            a.speakerSimulationWeightC != b.speakerSimulationWeightC ||
            !trackInsertSlotsEqual(a.speakerInsertsA, b.speakerInsertsA) ||
            !trackInsertSlotsEqual(a.speakerInsertsB, b.speakerInsertsB) ||
            !trackInsertSlotsEqual(a.speakerInsertsC, b.speakerInsertsC)) {
            return false;
        }
    }
    return slotChanged;
}

float stereoPeak(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return std::min(1.0f, std::max(0.0f, peak));
}

std::pair<float, float> stereoPeaks(const std::vector<float>& samples) {
    float left = 0.0f;
    float right = 0.0f;
    for (size_t index = 0; index + 1 < samples.size(); index += 2) {
        left = std::max(left, std::abs(samples[index]));
        right = std::max(right, std::abs(samples[index + 1]));
    }
    return {std::clamp(left, 0.0f, 1.0f), std::clamp(right, 0.0f, 1.0f)};
}

int64_t wrappedPlaybackFrameForPlan(const ProjectAudioRenderPlan& plan, int64_t playbackFrame) {
    if (!plan.loopEnabled || plan.loopEndSeconds <= plan.loopStartSeconds || plan.sampleRate <= 0.0) {
        return playbackFrame;
    }
    const int64_t loopStart = std::max<int64_t>(0, static_cast<int64_t>(std::round(plan.loopStartSeconds * plan.sampleRate)));
    const int64_t loopEnd = std::max<int64_t>(0, static_cast<int64_t>(std::round(plan.loopEndSeconds * plan.sampleRate)));
    const int64_t loopLength = loopEnd - loopStart;
    if (loopLength <= 0 || playbackFrame < loopEnd) {
        return playbackFrame;
    }
    return loopStart + ((playbackFrame - loopStart) % loopLength);
}

int64_t loopEndFrameForPlan(const ProjectAudioRenderPlan& plan) {
    if (!plan.loopEnabled || plan.loopEndSeconds <= plan.loopStartSeconds || plan.sampleRate <= 0.0) {
        return 0;
    }
    return std::max<int64_t>(0, static_cast<int64_t>(std::round(plan.loopEndSeconds * plan.sampleRate)));
}

void mergeBlockMeters(ProjectAudioBlockMeters& destination, const ProjectAudioBlockMeters& source) {
    for (size_t index = 0; index < source.trackNames.size(); ++index) {
        const auto& name = source.trackNames[index];
        auto found = std::find(destination.trackNames.begin(), destination.trackNames.end(), name);
        if (found == destination.trackNames.end()) {
            destination.trackNames.push_back(name);
            destination.trackPeakLeft.push_back(index < source.trackPeakLeft.size() ? source.trackPeakLeft[index] : 0.0f);
            destination.trackPeakRight.push_back(index < source.trackPeakRight.size() ? source.trackPeakRight[index] : 0.0f);
            continue;
        }
        const size_t destinationIndex = static_cast<size_t>(std::distance(destination.trackNames.begin(), found));
        if (destinationIndex < destination.trackPeakLeft.size() && index < source.trackPeakLeft.size()) {
            destination.trackPeakLeft[destinationIndex] = std::max(destination.trackPeakLeft[destinationIndex], source.trackPeakLeft[index]);
        }
        if (destinationIndex < destination.trackPeakRight.size() && index < source.trackPeakRight.size()) {
            destination.trackPeakRight[destinationIndex] = std::max(destination.trackPeakRight[destinationIndex], source.trackPeakRight[index]);
        }
    }
    for (size_t index = 0; index < source.trackInsertMeterTrackNames.size(); ++index) {
        destination.trackInsertMeterTrackNames.push_back(source.trackInsertMeterTrackNames[index]);
        destination.trackInsertMeterSlotIndices.push_back(index < source.trackInsertMeterSlotIndices.size() ? source.trackInsertMeterSlotIndices[index] : -1);
        destination.trackInsertInputPeak.push_back(index < source.trackInsertInputPeak.size() ? source.trackInsertInputPeak[index] : 0.0f);
        destination.trackInsertOutputPeak.push_back(index < source.trackInsertOutputPeak.size() ? source.trackInsertOutputPeak[index] : 0.0f);
    }
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << ch; break;
        }
    }
    return out.str();
}

void publishNdsPluginTelemetry(const std::string& moduleId,
                               const std::string& trackName,
                               int slotIndex,
                               const std::vector<float>& input,
                               const std::vector<float>& output,
                               double roundTripMs) {
    if (moduleId != "na.neuracoust.4001e") {
        return;
    }
    std::error_code ec;
    const auto directory = std::filesystem::temp_directory_path(ec) / "neuracoust-nds";
    if (ec) {
        return;
    }
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return;
    }
    const auto target = directory / "na.neuracoust.4001e.telemetry.json";
    const auto temp = directory / "na.neuracoust.4001e.telemetry.tmp";
    const auto [inputLeft, inputRight] = stereoPeaks(input);
    const auto [outputLeft, outputRight] = stereoPeaks(output);
    const auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::ofstream out(temp, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "{"
        << "\"schema\":\"com.neuracoust.nds.telemetry.v1\","
        << "\"serverModuleId\":\"" << jsonEscape(moduleId) << "\","
        << "\"target\":\"external\","
        << "\"serverName\":\"Neuracoust DSP Server\","
        << "\"trackName\":\"" << jsonEscape(trackName) << "\","
        << "\"slotIndex\":" << slotIndex << ","
        << "\"timestampMs\":" << timestampMs << ","
        << "\"roundTripMs\":" << roundTripMs << ","
        << "\"inputPeakL\":" << inputLeft << ","
        << "\"inputPeakR\":" << inputRight << ","
        << "\"outputPeakL\":" << outputLeft << ","
        << "\"outputPeakR\":" << outputRight << ","
        << "\"compReduction\":0.0,"
        << "\"gateReduction\":0.0"
        << "}\n";
    out.close();
    if (!out) {
        return;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(temp, target, ec);
    }
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
    ensureSignalGeneratorDefaults(renderInsert);
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

void appendParameterSignature(std::ostringstream& out, const Vst3ParameterValueState& parameter) {
    out << parameter.parameterId << '=' << parameter.normalizedValue << ';';
}

bool isNeuracoust4001EInsert(const TrackInsertSlot& insert) {
    return insert.serverModuleId == "na.neuracoust.4001e" ||
           insert.pluginName.find("4001") != std::string::npos ||
           insert.pluginName.find("Newacoust4001E") != std::string::npos ||
           insert.pluginPath.find("Newacoust4001E") != std::string::npos;
}

int nds4001EParameterIndexForVst3Id(uint32_t parameterId) {
    if (parameterId < 52u) {
        return static_cast<int>(parameterId);
    }
    switch (parameterId) {
        case 1706969868u: return 0;
        case 1361682400u: return 1;
        case 1266089414u: return 2;
        case 1266093258u: return 3;
        case 921887787u: return 4;
        case 720421436u: return 5;
        case 942264824u: return 6;
        case 2107524327u: return 7;
        case 758661938u: return 8;
        case 578377770u: return 9;
        case 1189775548u: return 10;
        case 1444078953u: return 11;
        case 1444065378u: return 12;
        case 2045025492u: return 13;
        case 2045011917u: return 14;
        case 850671420u: return 15;
        case 1300072920u: return 16;
        case 1300059345u: return 17;
        case 850790584u: return 18;
        case 1558595557u: return 19;
        case 1558581982u: return 20;
        case 1980608203u: return 21;
        case 1281963043u: return 22;
        case 1518646659u: return 23;
        case 97188214u: return 24;
        case 773352680u: return 25;
        case 1310540656u: return 26;
        case 1602534558u: return 27;
        case 373379829u: return 28;
        case 1559016369u: return 29;
        case 1242253010u: return 30;
        case 811126732u: return 31;
        case 66174160u: return 32;
        case 377764314u: return 33;
        case 211543279u: return 34;
        case 1563400854u: return 35;
        case 731801461u: return 36;
        case 1638639995u: return 37;
        case 1125760831u: return 38;
        case 1769326783u: return 39;
        case 900588294u: return 40;
        case 1825261951u: return 41;
        case 1486159719u: return 42;
        case 771066792u: return 43;
        case 83719505u: return 44;
        case 634628570u: return 45;
        case 882139294u: return 46;
        case 1799893405u: return 47;
        case 2039914848u: return 48;
        case 808156282u: return 49;
        case 1728609495u: return 50;
        case 1200570224u: return 51;
        default: return -1;
    }
}

std::vector<RemoteDspParameterValue> remoteParametersForInsert(const TrackInsertSlot& insert) {
    std::vector<RemoteDspParameterValue> values;
    values.reserve(std::min<size_t>(insert.parameters.size(), 64u));
    const bool map4001EParameters = isNeuracoust4001EInsert(insert);
    for (const auto& parameter : insert.parameters) {
        if (!std::isfinite(parameter.normalizedValue)) {
            continue;
        }
        const int parameterIndex = map4001EParameters
            ? nds4001EParameterIndexForVst3Id(parameter.parameterId)
            : (parameter.parameterId < 64u ? static_cast<int>(parameter.parameterId) : -1);
        if (parameterIndex < 0) {
            continue;
        }
        values.push_back({static_cast<uint32_t>(parameterIndex),
                          static_cast<float>(std::clamp(parameter.normalizedValue, 0.0, 1.0))});
    }
    return values;
}

void appendInsertSignature(std::ostringstream& out, const InsertState& insert) {
    out << insert.pluginFormat << '|'
        << insert.pluginPath << '|'
        << insert.pluginName << '|'
        << (insert.available ? '1' : '0') << '|';
    out << "dsp=" << (insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode)
        << ";server=" << insert.assignedDspServerId
        << ";module=" << insert.serverModuleId
        << ";lat=" << insert.reportedLatencySamples << '|';
    out << '\n';
}

std::string realtimeInsertGraphSignature(const ProjectAudioRenderPlan& plan,
                                         const AudioEngineSettings& settings,
                                         int maxBlockSize) {
    std::ostringstream out;
    out << "sr=" << settings.sampleRate
        << ";block=" << maxBlockSize
        << ";pdc=" << (settings.delayCompensationEnabled ? '1' : '0') << '\n';
    out << "master\n";
    for (const auto& insert : plan.activeVst3Inserts) {
        appendInsertSignature(out, insert);
    }
    out << "tracks\n";
    for (const auto& track : plan.tracks) {
        if (track.inserts.empty() ||
            track.trackType == "folder" ||
            track.trackType == "bus_folder" ||
            track.trackType == "master" ||
            track.trackType == "monitor" ||
            !isMasterOutputBus(track.outputBus)) {
            continue;
        }
        bool hasRealtimeInsert = false;
        std::ostringstream trackOut;
        for (const auto& insert : track.inserts) {
            InsertState renderInsert = trackInsertToRenderInsert(insert);
            if (!insert.enabled || !isVst3MasterInsert(renderInsert)) {
                continue;
            }
            hasRealtimeInsert = true;
            appendInsertSignature(trackOut, renderInsert);
        }
        if (hasRealtimeInsert) {
            out << track.name << '|' << track.outputBus << '\n' << trackOut.str();
        }
    }
    int monitorSlot = -1;
    if (const auto* monitorInserts = activeSpeakerOutputInserts(plan.monitorModules, &monitorSlot)) {
        out << "monitor-output|" << monitorSlot << '\n';
        for (const auto& insert : *monitorInserts) {
            InsertState renderInsert = trackInsertToRenderInsert(insert);
            if (!insert.enabled || !isVst3MasterInsert(renderInsert)) {
                continue;
            }
            appendInsertSignature(out, renderInsert);
        }
    }
    return out.str();
}

const TrackState* findTrack(const ProjectAudioRenderPlan& plan, const std::string& trackName) {
    const auto found = std::find_if(plan.tracks.begin(), plan.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    return found == plan.tracks.end() ? nullptr : &(*found);
}

bool trackExcludedFromSoloPlayback(const TrackState& track) {
    return track.trackType == "master" ||
        track.trackType == "monitor" ||
        track.trackType == "vca" ||
        track.trackType == "folder" ||
        track.name == "Master" ||
        track.name == "Monitor";
}

bool trackParticipatesInSolo(const TrackState& track) {
    return track.solo && !trackExcludedFromSoloPlayback(track);
}

bool hasSoloedPlaybackTrack(const ProjectAudioRenderPlan& plan) {
    return std::any_of(plan.tracks.begin(), plan.tracks.end(), [](const TrackState& track) {
        return trackParticipatesInSolo(track);
    });
}

bool busAlreadyMarkedForSoloPlayback(const std::vector<std::string>& buses, const std::string& busName) {
    return !busName.empty() && std::find(buses.begin(), buses.end(), busName) != buses.end();
}

bool trackReceivesSoloedPlayback(const ProjectAudioRenderPlan& plan, const TrackState& targetTrack) {
    if (targetTrack.inputBus.empty() || trackExcludedFromSoloPlayback(targetTrack)) {
        return false;
    }
    std::vector<std::string> soloBuses;
    for (const auto& track : plan.tracks) {
        if (trackParticipatesInSolo(track) && !track.muted && !track.outputBus.empty()) {
            soloBuses.push_back(track.outputBus);
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& track : plan.tracks) {
            if (track.muted ||
                trackExcludedFromSoloPlayback(track) ||
                track.inputBus.empty() ||
                !busAlreadyMarkedForSoloPlayback(soloBuses, track.inputBus)) {
                continue;
            }
            if (track.name == targetTrack.name) {
                return true;
            }
            if (!track.outputBus.empty() && !busAlreadyMarkedForSoloPlayback(soloBuses, track.outputBus)) {
                soloBuses.push_back(track.outputBus);
                changed = true;
            }
        }
    }
    return false;
}

bool trackAllowedBySoloPlayback(const ProjectAudioRenderPlan& plan, const TrackState& track) {
    return trackParticipatesInSolo(track) ||
        trackReceivesSoloedPlayback(plan, track) ||
        track.trackType == "master" ||
        track.trackType == "monitor" ||
        track.name == "Master" ||
        track.name == "Monitor";
}

bool realtimeTrackPlaybackMuted(const ProjectAudioRenderPlan& plan, const TrackState& track) {
    return track.muted || (hasSoloedPlaybackTrack(plan) && !trackAllowedBySoloPlayback(plan, track));
}

} // namespace

bool NeuracoustDspEngine::configure(const AudioEngineSettings& settings, int maxBlockSize, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    if (settings_.monitorModules.empty()) {
        settings_.monitorModules = defaultMonitorDspModules();
    }
    settings_.sampleRate = settings_.sampleRate > 0.0 ? settings_.sampleRate : 48000.0;
    settings_.playbackStabilityBufferMultiplier = std::max(1, settings_.playbackStabilityBufferMultiplier);
    maxBlockSize_ = std::max(1, maxBlockSize);
    configured_ = true;
    projectPlan_.transportRecordingActive = settings_.transportRecordingActive;
    monitorProcessor_.configure(settings_.sampleRate, settings_.monitorModules);
    previousMonitorProcessor_ = monitorProcessor_;
    monitorStationGainSmoothed_ = (settings_.monitorStationMute ? 0.0f : 1.0f) *
        (settings_.monitorStationDim ? dbToGain(settings_.monitorStationDimDb) : 1.0f) *
        dbToGain(settings_.monitorVolumeDb);
    monitorStationGainInitialized_ = true;
    monitorDspTransitionFromMode_.clear();
    monitorDspTransitionToMode_.clear();
    monitorDspTransitionSamplesRemaining_ = 0;
    monitorDspTransitionSamplesTotal_ = 0;
    monitorDspModuleTransitionSamplesRemaining_ = 0;
    monitorDspModuleTransitionSamplesTotal_ = 0;
    sampleRateForStatus_.store(std::max(1.0, settings_.sampleRate));
    talkbackCaptureActive_.store(settings_.monitorStationTalkback, std::memory_order_relaxed);
    physicalInputMonitoringActiveForStatus_.store(false, std::memory_order_relaxed);
    inputMonitorChannelsForStatus_.store(0, std::memory_order_relaxed);
    inputPeakForStatus_.store(0.0f, std::memory_order_relaxed);
    listenRoomSender_.configure(settings_.sampleRate, settings_.listenRoom);
    updateProjectMonitorPolicyLocked();
    if (!prepareRealtimeInsertChainLocked(maxBlockSize_, error)) {
        return false;
    }
    message_ = "Neuracoust DSP engine configured.";
    if (lowLatencyRecordMonitoringActive_) {
        message_ += " Low-latency input monitor path armed.";
    }
    error.clear();
    return true;
}

void NeuracoustDspEngine::resetRuntime() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    realtimeInsertChain_.reset();
    realtimeTrackInsertChains_.clear();
    realtimeInsertGraphSignature_.clear();
    projectRenderState_.reset();
    projectBlock_.clear();
    projectSegmentBlock_.clear();
    trackInsertDryBlock_.clear();
    trackInsertCompensatedInputBlock_.clear();
    trackInsertProcessedBlock_.clear();
    remoteTrackInsertProcessedBlock_.clear();
    remoteDspProcessedBlock_.clear();
    const bool preserveReloadCrossfadeBlock = configured_ && !previousOutputBlock_.empty();
    if (!preserveReloadCrossfadeBlock) {
        previousOutputBlock_.clear();
        reloadCrossfadeSamplesRemaining_ = 0;
        reloadCrossfadeSamplesTotal_ = 0;
    }
    monitorDspModuleTransitionSamplesRemaining_ = 0;
    monitorDspModuleTransitionSamplesTotal_ = 0;
    remoteMonitorDspStream_.reset();
    listenRoomSender_.stop();
    projectMeters_ = {};
    trackInsertMeterTrackNames_.clear();
    trackInsertMeterSlotIndices_.clear();
    trackInsertInputPeaks_.clear();
    trackInsertOutputPeaks_.clear();
    {
        std::lock_guard<std::mutex> inputLock(inputMonitorMutex_);
        inputMonitorBuffer_.clear();
        inputPeak_ = 0.0f;
        inputMonitorChannels_ = 0;
        physicalInputMonitoringActive_ = false;
    }
    inputMonitorCaptureActive_.store(false, std::memory_order_relaxed);
    talkbackCaptureActive_.store(false, std::memory_order_relaxed);
    physicalInputMonitoringActiveForStatus_.store(false, std::memory_order_relaxed);
    inputMonitorChannelsForStatus_.store(0, std::memory_order_relaxed);
    inputPeakForStatus_.store(0.0f, std::memory_order_relaxed);
    remoteDspMonitorActive_ = false;
    resetRemoteDspTelemetryLocked();
    playbackFrame_ = 0;
    realtimeProcessFrame_ = 0;
    playbackFrameForStatus_.store(0);
    phase_ = 0.0;
    monitorStationGainSmoothed_ = 1.0f;
    monitorStationGainInitialized_ = false;
    resetMeteringLocked();
    configured_ = false;
}

void NeuracoustDspEngine::setTestToneEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.testToneEnabled = enabled;
    syncProjectMonitorDspRenderPathLocked();
}

void NeuracoustDspEngine::setMetronomeEnabled(bool enabled,
                                             int tempoBpm,
                                             const std::vector<TempoMarkerState>& tempoMap,
                                             int timeSignatureNumerator,
                                             int timeSignatureDenominator,
                                             const std::string& grooveFeel,
                                             double grooveSwingAmount,
                                             const std::vector<TimeSignatureMarkerState>& timeSignatureMap,
                                             const std::string& metronomeSubdivision) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    if (!tempoMap.empty()) {
        settings_.tempoMap = tempoMap;
        std::sort(settings_.tempoMap.begin(), settings_.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });
    } else {
        settings_.tempoMap.clear();
    }
    settings_.timeSignatureMap = timeSignatureMap;
    std::sort(settings_.timeSignatureMap.begin(), settings_.timeSignatureMap.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    syncProjectMonitorDspRenderPathLocked();
}

void NeuracoustDspEngine::setMonitorDspModules(const std::vector<MonitorDspModule>& modules, bool enabled) {
    auto nextModules = modules.empty() ? defaultMonitorDspModules() : modules;
    MonitorDspProcessor nextMonitorProcessor;
    nextMonitorProcessor.configure(std::max(1.0, sampleRateForStatus_.load(std::memory_order_relaxed)), nextModules);

    std::lock_guard<std::mutex> lock(mutex_);
    const bool previousEnabled = settings_.monitorDspEnabled;
    const bool onlyActiveTargetSlotChanged =
        previousEnabled == enabled &&
        monitorModulesOnlyActiveTargetSlotChanged(settings_.monitorModules, nextModules);
    const bool canCrossfade =
        configured_ &&
        previousEnabled &&
        enabled &&
        !onlyActiveTargetSlotChanged &&
        !monitorDspModeRequestsRemoteLocked(settings_.monitorDspPathMode);
    if (canCrossfade) {
        previousMonitorProcessor_ = monitorProcessor_;
    }
    settings_.monitorModules = std::move(nextModules);
    projectPlan_.monitorModules = settings_.monitorModules;
    settings_.monitorDspEnabled = enabled;
    monitorProcessor_ = nextMonitorProcessor;
    if (canCrossfade) {
        monitorDspModuleTransitionSamplesTotal_ = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.045));
        monitorDspModuleTransitionSamplesRemaining_ = monitorDspModuleTransitionSamplesTotal_;
    } else {
        previousMonitorProcessor_ = monitorProcessor_;
        monitorDspModuleTransitionSamplesRemaining_ = 0;
        monitorDspModuleTransitionSamplesTotal_ = 0;
    }
    syncProjectMonitorDspRenderPathLocked();
    std::string monitorInsertError;
    if (!prepareMonitorOutputInsertChainLocked(maxBlockSize_, monitorInsertError)) {
        message_ = "Monitor output VST3 inserts stayed dry: " + monitorInsertError;
    }
}

void NeuracoustDspEngine::setMonitorDspPathMode(const std::string& mode, const RemoteDspServerSettings& remoteDspServer) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string safeMode = (mode == "external" || mode == "nds" || mode == "remote_external" || mode == "auto") ? mode : "internal";
    const std::string previousMode = settings_.monitorDspPathMode.empty() ? "internal" : settings_.monitorDspPathMode;
    settings_.remoteDspServer = remoteDspServer;
    if (safeMode == previousMode) {
        settings_.monitorDspPathMode = safeMode;
        return;
    }
    monitorDspTransitionFromMode_ = previousMode;
    monitorDspTransitionToMode_ = safeMode;
    monitorDspTransitionSamplesTotal_ = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.045));
    monitorDspTransitionSamplesRemaining_ = monitorDspTransitionSamplesTotal_;
    monitorDspModuleTransitionSamplesRemaining_ = 0;
    monitorDspModuleTransitionSamplesTotal_ = 0;
    settings_.monitorDspPathMode = safeMode;
    syncProjectMonitorDspRenderPathLocked();
    message_ = "Monitor DSP path switching from " + previousMode + " to " + safeMode + ".";
}

void NeuracoustDspEngine::setListenRoomSettings(const ListenRoomSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.listenRoom = normalizedListenRoomSettings(settings);
    listenRoomSender_.configure(settings_.sampleRate, settings_.listenRoom);
    message_ = settings_.listenRoom.enabled ? "Listen Room sender armed." : "Listen Room stopped.";
}

void NeuracoustDspEngine::setMonitorStationControls(bool mono, const std::string& listenMode, bool swapLeftRight, bool invertLeft, bool invertRight, bool mute, bool dim, bool talkback, float inputTrimDb, float volumeDb, float dimDb, const std::string& talkbackRoute) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string safeListenMode = (listenMode == "L" || listenMode == "R" || listenMode == "M" || listenMode == "S") ? listenMode : "LR";
    const bool msMode = safeListenMode == "M" || safeListenMode == "S";
    const bool gainButtonChanged = settings_.monitorStationMute != mute || settings_.monitorStationDim != dim;
    const bool explicitMonitorTargetChanged =
        settings_.monitorStationListenMode != safeListenMode ||
        std::abs(settings_.monitorInputTrimDb - inputTrimDb) > 0.001f ||
        std::abs(settings_.monitorVolumeDb - volumeDb) > 0.001f;
    const bool shouldRampGain = gainButtonChanged && !explicitMonitorTargetChanged;
    settings_.monitorStationMono = msMode ? false : mono;
    settings_.monitorStationListenMode = safeListenMode;
    settings_.monitorStationSwapLeftRight = msMode ? false : swapLeftRight;
    settings_.monitorStationInvertLeft = invertLeft;
    settings_.monitorStationInvertRight = invertRight;
    settings_.monitorStationMute = mute;
    settings_.monitorStationDim = dim;
    settings_.monitorStationTalkback = talkback;
    talkbackCaptureActive_.store(talkback, std::memory_order_relaxed);
    settings_.monitorStationDimDb = std::max(-60.0f, std::min(0.0f, dimDb));
    settings_.monitorStationTalkbackRoute = talkbackRoute.empty() ? "listen_room" : talkbackRoute;
    settings_.monitorInputTrimDb = std::max(-12.0f, std::min(0.0f, inputTrimDb));
    projectPlan_.monitorInputTrimDb = settings_.monitorInputTrimDb;
    settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, volumeDb));
    if (!talkback && !inputMonitorCaptureActive_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> inputLock(inputMonitorMutex_);
        inputMonitorBuffer_.clear();
        inputPeak_ = 0.0f;
        inputMonitorChannels_ = 0;
        physicalInputMonitoringActive_ = false;
        physicalInputMonitoringActiveForStatus_.store(false, std::memory_order_relaxed);
        inputMonitorChannelsForStatus_.store(0, std::memory_order_relaxed);
        inputPeakForStatus_.store(0.0f, std::memory_order_relaxed);
    }
    if (!shouldRampGain) {
        monitorStationGainSmoothed_ = (settings_.monitorStationMute ? 0.0f : 1.0f) *
            (settings_.monitorStationDim ? dbToGain(settings_.monitorStationDimDb) : 1.0f) *
            dbToGain(settings_.monitorVolumeDb);
        monitorStationGainInitialized_ = true;
    }
}

bool NeuracoustDspEngine::loadAudioFile(const std::string& path, std::string& error) {
    WavAudioData data;
    if (!readPcmWavFile(path, data, error)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const double sourceSampleRate = data.sampleRate > 0 ? static_cast<double>(data.sampleRate) : settings_.sampleRate;
    playback_ = std::move(data);
    projectPlan_ = {};
    projectRenderState_.reset();
    projectBlock_.clear();
    projectSegmentBlock_.clear();
    trackInsertDryBlock_.clear();
    trackInsertCompensatedInputBlock_.clear();
    trackInsertProcessedBlock_.clear();
    projectMeters_ = {};
    realtimeInsertChain_.reset();
    realtimeTrackInsertChains_.clear();
    realtimeInsertGraphSignature_.clear();
    delayCompensationSamples_ = 0;
    playbackFrame_ = 0;
    realtimeProcessFrame_ = 0;
    playbackFrameForStatus_.store(0);
    resetMeteringLocked();
    sampleRateForStatus_.store(std::max(1.0, sourceSampleRate));
    updateProjectMonitorPolicyLocked();
    message_ = "Loaded audio file into Neuracoust DSP engine.";
    error.clear();
    return true;
}

bool NeuracoustDspEngine::loadProject(const ProjectDocument& project, std::string& error) {
    ProjectAudioRenderPlan plan;
    if (!makeProjectAudioRenderPlan(project, plan, error)) {
        return false;
    }
    plan.renderTrackVst3Inserts = true;
    std::lock_guard<std::mutex> lock(mutex_);
    const double projectSampleRate = plan.sampleRate > 0.0 ? plan.sampleRate : settings_.sampleRate;
    plan.transportRecordingActive = settings_.transportRecordingActive;
    projectPlan_ = std::move(plan);
    projectRenderState_.reset();
    settings_.delayCompensationEnabled = project.delayCompensationEnabled;
    settings_.lowLatencyRecordMonitoringEnabled = project.directMonitoringEnabled;
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
    talkbackCaptureActive_.store(settings_.monitorStationTalkback, std::memory_order_relaxed);
    settings_.monitorStationDimDb = project.monitorStationDimDb;
    settings_.monitorStationTalkbackRoute = project.monitorStationTalkbackRoute.empty() ? "listen_room" : project.monitorStationTalkbackRoute;
    settings_.monitorInputTrimDb = std::max(-12.0f, std::min(0.0f, project.monitorInputTrimDb));
    settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, project.monitorVolumeDb));
    settings_.monitorModules = project.monitorModules.empty() ? defaultMonitorDspModules() : project.monitorModules;
    projectPlan_.monitorModules = settings_.monitorModules;
    monitorProcessor_.configure(std::max(1.0, projectSampleRate), settings_.monitorModules);
    previousMonitorProcessor_ = monitorProcessor_;
    monitorDspModuleTransitionSamplesRemaining_ = 0;
    monitorDspModuleTransitionSamplesTotal_ = 0;
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
    // Do NOT tear down the realtime insert chains / signature here. Playback start
    // routes through loadProject, and clearing these forced a full rebuild — every
    // out-of-process plug-in worker respawned and reloaded (a Waves shell is several
    // seconds each) on the main thread, so pressing Play stalled for seconds. Leave
    // them intact: prepareRealtimeInsertChainLocked reuses still-valid workers, and
    // the signature comparison below skips the rebuild entirely when nothing changed
    // (instant start). The master chain is reset inside prepare only when it rebuilds.
    previousOutputBlock_.clear();
    reloadCrossfadeSamplesRemaining_ = 0;
    reloadCrossfadeSamplesTotal_ = 0;
    resetTrackMetersLocked();
    playbackFrame_ = 0;
    realtimeProcessFrame_ = 0;
    playbackFrameForStatus_.store(0);
    resetMeteringLocked();
    sampleRateForStatus_.store(std::max(1.0, projectSampleRate));
    updateProjectMonitorPolicyLocked();
    syncProjectMonitorDspRenderPathLocked();
    if (configured_) {
        const std::string nextInsertGraphSignature = realtimeInsertGraphSignature(projectPlan_, settings_, maxBlockSize_);
        if (nextInsertGraphSignature != realtimeInsertGraphSignature_) {
            if (!prepareRealtimeInsertChainLocked(maxBlockSize_, error)) {
                return false;
            }
            if (!previousOutputBlock_.empty()) {
                reloadCrossfadeSamplesTotal_ = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.080));
                reloadCrossfadeSamplesRemaining_ = reloadCrossfadeSamplesTotal_;
            }
	        } else if (settings_.delayCompensationEnabled) {
	            delayCompensationSamples_ = std::max(
	                projectPlan_.delayCompensationSamples,
	                realtimeInsertChain_.activeLatencySamples());
            for (const auto& chain : realtimeTrackInsertChains_) {
                delayCompensationSamples_ = std::max(delayCompensationSamples_, chain.latencySamples);
            }
        } else {
            delayCompensationSamples_ = 0;
        }
        warmRouteInsertChainsLocked();
    }
    if (message_.rfind("Some track VST3 inserts could not be prepared", 0) != 0) {
        message_ = projectPlan_.hasMissingMedia
            ? "Loaded project into Neuracoust DSP engine with missing media rendered as silence."
            : "Loaded project into Neuracoust DSP engine.";
    }
    if (lowLatencyRecordMonitoringActive_) {
        message_ += " Low-latency input monitor path armed.";
    }
    error.clear();
    return true;
}

bool NeuracoustDspEngine::updateProject(const ProjectDocument& project, std::string& error) {
    ProjectAudioRenderPlan plan;
    if (!makeProjectAudioRenderPlan(project, plan, error)) {
        return false;
    }
    plan.renderTrackVst3Inserts = true;
    std::lock_guard<std::mutex> lock(mutex_);
    const double projectSampleRate = plan.sampleRate > 0.0 ? plan.sampleRate : settings_.sampleRate;
    plan.transportRecordingActive = settings_.transportRecordingActive;
    projectPlan_ = std::move(plan);
    projectRenderState_.reset();
    settings_.delayCompensationEnabled = project.delayCompensationEnabled;
    settings_.lowLatencyRecordMonitoringEnabled = project.directMonitoringEnabled;
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
    talkbackCaptureActive_.store(settings_.monitorStationTalkback, std::memory_order_relaxed);
    settings_.monitorStationDimDb = project.monitorStationDimDb;
    settings_.monitorStationTalkbackRoute = project.monitorStationTalkbackRoute.empty() ? "listen_room" : project.monitorStationTalkbackRoute;
    settings_.monitorInputTrimDb = std::max(-12.0f, std::min(0.0f, project.monitorInputTrimDb));
    settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, project.monitorVolumeDb));
    settings_.monitorModules = project.monitorModules.empty() ? defaultMonitorDspModules() : project.monitorModules;
    projectPlan_.monitorModules = settings_.monitorModules;
    monitorProcessor_.configure(std::max(1.0, projectSampleRate), settings_.monitorModules);
    previousMonitorProcessor_ = monitorProcessor_;
    monitorDspModuleTransitionSamplesRemaining_ = 0;
    monitorDspModuleTransitionSamplesTotal_ = 0;
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
    suppressTrackInsertMetersForGraphChangeLocked();
    sampleRateForStatus_.store(std::max(1.0, projectSampleRate));
    updateProjectMonitorPolicyLocked();
    syncProjectMonitorDspRenderPathLocked();
    if (configured_) {
        const std::string nextInsertGraphSignature = realtimeInsertGraphSignature(projectPlan_, settings_, maxBlockSize_);
        if (nextInsertGraphSignature != realtimeInsertGraphSignature_) {
            if (!prepareRealtimeInsertChainLocked(maxBlockSize_, error)) {
                return false;
            }
            if (!previousOutputBlock_.empty()) {
                reloadCrossfadeSamplesTotal_ = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.080));
                reloadCrossfadeSamplesRemaining_ = reloadCrossfadeSamplesTotal_;
            }
	        } else if (settings_.delayCompensationEnabled) {
	            delayCompensationSamples_ = std::max(
	                projectPlan_.delayCompensationSamples,
	                realtimeInsertChain_.activeLatencySamples());
            for (const auto& chain : realtimeTrackInsertChains_) {
                delayCompensationSamples_ = std::max(delayCompensationSamples_, chain.latencySamples);
            }
        } else {
            delayCompensationSamples_ = 0;
        }
        warmRouteInsertChainsLocked();
    }
    if (message_.rfind("Some track VST3 inserts could not be prepared", 0) != 0) {
        message_ = projectPlan_.hasMissingMedia
            ? "Updated project in Neuracoust DSP engine with missing media rendered as silence."
            : "Updated project in Neuracoust DSP engine without resetting playback.";
    }
    if (lowLatencyRecordMonitoringActive_) {
        message_ += " Low-latency input monitor path armed.";
    }
    error.clear();
    return true;
}

bool NeuracoustDspEngine::updateClipGain(const std::string& clipId, float gainDb) {
    if (clipId.empty() || !std::isfinite(gainDb)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto clipIt = std::find_if(projectPlan_.clips.begin(), projectPlan_.clips.end(), [&](const ProjectRenderClip& renderClip) {
        return renderClip.clip.id == clipId;
    });
    if (clipIt == projectPlan_.clips.end()) {
        return false;
    }
    clipIt->clip.gainDb = std::max(-60.0f, std::min(24.0f, gainDb));
    message_ = "Updated clip gain without reloading project.";
    return true;
}

bool NeuracoustDspEngine::updateClipFades(const std::string& clipId, double fadeInSeconds, double fadeOutSeconds) {
    if (clipId.empty() || !std::isfinite(fadeInSeconds) || !std::isfinite(fadeOutSeconds)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto clipIt = std::find_if(projectPlan_.clips.begin(), projectPlan_.clips.end(), [&](const ProjectRenderClip& renderClip) {
        return renderClip.clip.id == clipId;
    });
    if (clipIt == projectPlan_.clips.end()) {
        return false;
    }
    const double maxFade = std::max(0.0, clipIt->clip.durationSeconds * 0.5);
    clipIt->clip.fadeInSeconds = std::max(0.0, std::min(maxFade, fadeInSeconds));
    clipIt->clip.fadeOutSeconds = std::max(0.0, std::min(maxFade, fadeOutSeconds));
    message_ = "Updated clip fades without reloading project.";
    return true;
}

bool NeuracoustDspEngine::updateTrackMix(const std::string& trackName, float volumeDb, float pan) {
    if (trackName.empty() || !std::isfinite(volumeDb) || !std::isfinite(pan)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    if (trackIt == projectPlan_.tracks.end()) {
        return false;
    }
    trackIt->volumeDb = std::max(-120.0f, std::min(12.0f, volumeDb));
    trackIt->pan = std::max(-1.0f, std::min(1.0f, pan));
    updateProjectMonitorPolicyLocked();
    message_ = "Updated track mix without reloading project.";
    return true;
}

bool NeuracoustDspEngine::updateTrackSendSlot(const std::string& trackName, size_t sendIndex, const TrackSendState& send) {
    if (trackName.empty() || !std::isfinite(send.gainDb) || !std::isfinite(send.pan)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
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
    updateProjectMonitorPolicyLocked();
    message_ = "Updated track send without reloading project.";
    return true;
}

bool NeuracoustDspEngine::updateTrackInsertBypassState(const std::string& trackName, size_t insertIndex, bool bypassed) {
    if (trackName.empty()) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
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
    message_ = bypassed
        ? "Bypassed track insert without reloading project."
        : "Enabled track insert without reloading project.";
    return true;
}

bool NeuracoustDspEngine::updateMasterInsertBypassState(size_t insertIndex, bool bypassed) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    delayCompensationSamples_ = settings_.delayCompensationEnabled
        ? std::max(projectPlan_.delayCompensationSamples, realtimeInsertChain_.activeLatencySamples())
        : 0u;
    if (settings_.delayCompensationEnabled) {
        for (const auto& chain : realtimeTrackInsertChains_) {
            delayCompensationSamples_ = std::max(delayCompensationSamples_, chain.latencySamples);
        }
        delayCompensationSamples_ = std::max(delayCompensationSamples_, monitorOutputInsertChain_.activeLatencySamples());
    }
    message_ = bypassed
        ? "Bypassed master insert without rebuilding plugin chain."
        : "Enabled master insert without rebuilding plugin chain.";
    return true;
}

bool NeuracoustDspEngine::updateTrackPlaybackState(const std::string& trackName, bool muted, bool solo) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    if (trackIt == projectPlan_.tracks.end()) {
        return false;
    }
    trackIt->muted = muted;
    trackIt->solo = solo;
    updateProjectMonitorPolicyLocked();
    message_ = "Updated track mute/solo without reloading project.";
    return true;
}

bool NeuracoustDspEngine::updateTrackRealtimeState(const std::string& trackName, bool recordArmed, bool inputMonitoring, bool muted, bool solo) {
    if (trackName.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
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
    updateProjectMonitorPolicyLocked();
    message_ = "Updated track live monitoring state without reloading project.";
    return true;
}

namespace {

void upsertVst3ParameterState(std::vector<Vst3ParameterValueState>& parameters,
                              uint32_t parameterId,
                              const std::string& displayName,
                              double normalizedValue) {
    const float value = static_cast<float>(std::clamp(normalizedValue, 0.0, 1.0));
    auto found = std::find_if(parameters.begin(), parameters.end(), [&](const Vst3ParameterValueState& parameter) {
        return parameter.parameterId == parameterId;
    });
    if (found == parameters.end()) {
        parameters.push_back({parameterId,
                              displayName.empty() ? "Param " + std::to_string(parameterId) : displayName,
                              value});
    } else {
        if (!displayName.empty()) {
            found->displayName = displayName;
        } else if (found->displayName.empty()) {
            found->displayName = "Param " + std::to_string(parameterId);
        }
        found->normalizedValue = value;
    }
}

} // namespace

bool NeuracoustDspEngine::updateMasterVst3Parameter(size_t insertIndex,
                                                    uint32_t parameterId,
                                                    const std::string& displayName,
                                                    double normalizedValue) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (insertIndex >= projectPlan_.activeVst3Inserts.size()) {
        return false;
    }
    upsertVst3ParameterState(projectPlan_.activeVst3Inserts[insertIndex].parameters,
                             parameterId,
                             displayName,
                             normalizedValue);
    const bool updatedChain = realtimeInsertChain_.updateParameter(insertIndex,
                                                                   parameterId,
                                                                   displayName,
                                                                   normalizedValue);
    message_ = updatedChain
        ? "Updated master VST3 parameter without rebuilding plugin chain."
        : "Stored master VST3 parameter for next plugin block.";
    return true;
}

bool NeuracoustDspEngine::updateTrackVst3Parameter(const std::string& trackName,
                                                   size_t insertIndex,
                                                   uint32_t parameterId,
                                                   const std::string& displayName,
                                                   double normalizedValue) {
    if (trackName.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    if (trackIt == projectPlan_.tracks.end() || insertIndex >= trackIt->inserts.size()) {
        return false;
    }
    upsertVst3ParameterState(trackIt->inserts[insertIndex].parameters,
                             parameterId,
                             displayName,
                             normalizedValue);
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
    for (auto& chain : realtimeTrackInsertChains_) {
        if (chain.remoteDsp || chain.trackName != trackName) {
            continue;
        }
        const auto slotIt = std::find(chain.slotIndices.begin(), chain.slotIndices.end(), static_cast<int>(insertIndex));
        if (slotIt == chain.slotIndices.end()) {
            continue;
        }
        const size_t chainIndex = static_cast<size_t>(std::distance(chain.slotIndices.begin(), slotIt));
        updatedChain = chain.chain.updateParameter(chainIndex, parameterId, displayName, normalizedValue) || updatedChain;
    }
    message_ = updatedChain
        ? "Updated track VST3 parameter without rebuilding plugin chain."
        : "Stored track VST3 parameter for next plugin block.";
    return true;
}

bool NeuracoustDspEngine::updateMonitorSpeakerVst3Parameter(int speakerSlot,
                                                            size_t insertIndex,
                                                            uint32_t parameterId,
                                                            const std::string& displayName,
                                                            double normalizedValue) {
    const int safeSlot = std::max(1, std::min(3, speakerSlot));
    std::lock_guard<std::mutex> lock(mutex_);
    auto* settingsInserts = monitorSpeakerInsertsForSlot(settings_.monitorModules, safeSlot);
    auto* planInserts = monitorSpeakerInsertsForSlot(projectPlan_.monitorModules, safeSlot);
    if (settingsInserts == nullptr || insertIndex >= settingsInserts->size()) {
        return false;
    }
    upsertVst3ParameterState((*settingsInserts)[insertIndex].parameters,
                             parameterId,
                             displayName,
                             normalizedValue);
    if (planInserts != nullptr && insertIndex < planInserts->size()) {
        upsertVst3ParameterState((*planInserts)[insertIndex].parameters,
                                 parameterId,
                                 displayName,
                                 normalizedValue);
    }

    bool updatedChain = false;
    if (monitorOutputInsertActiveSlot_ == safeSlot) {
        size_t processorIndex = 0;
        for (size_t index = 0; index < settingsInserts->size(); ++index) {
            const auto& insert = (*settingsInserts)[index];
            InsertState renderInsert = trackInsertToRenderInsert(insert);
            const bool insertRuns = insert.enabled &&
                !insert.bypassed &&
                !insert.pluginPath.empty() &&
                isVst3MasterInsert(renderInsert);
            if (index == insertIndex) {
                if (insertRuns) {
                    updatedChain = monitorOutputInsertChain_.updateParameter(processorIndex,
                                                                             parameterId,
                                                                             displayName,
                                                                             normalizedValue);
                }
                break;
            }
            if (insertRuns) {
                ++processorIndex;
            }
        }
    }
    message_ = updatedChain
        ? "Updated monitor speaker VST3 parameter without rebuilding plugin chain."
        : "Stored monitor speaker VST3 parameter for next plugin block.";
    return true;
}

void NeuracoustDspEngine::queueLiveMidiEvents(const std::string& trackName, const std::vector<Vst3MidiEvent>& events) {
    if (trackName.empty() || events.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto& queue = projectRenderState_.liveMidiEvents[trackName];
    queue.insert(queue.end(), events.begin(), events.end());
    constexpr size_t kMaxQueuedLiveMidiEvents = 4096;
    if (queue.size() > kMaxQueuedLiveMidiEvents) {
        queue.erase(queue.begin(), queue.end() - static_cast<std::ptrdiff_t>(kMaxQueuedLiveMidiEvents));
    }
}

void NeuracoustDspEngine::setTransportRecordingActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.transportRecordingActive = active;
    projectPlan_.transportRecordingActive = active;
    updateProjectMonitorPolicyLocked();
    syncProjectMonitorDspRenderPathLocked();
    message_ = active
        ? "Tape record monitor path active for record-armed tracks."
        : "Tape record monitor path returned to playback monitoring.";
}

void NeuracoustDspEngine::rewind() {
    std::lock_guard<std::mutex> lock(mutex_);
    playbackFrame_ = 0;
    realtimeProcessFrame_ = 0;
    projectRenderState_.reset();
    previousOutputBlock_.clear();
    reloadCrossfadeSamplesRemaining_ = 0;
    reloadCrossfadeSamplesTotal_ = 0;
    seekRampSamplesRemaining_ = 0;
    seekRampSamplesTotal_ = 0;
    playbackFrameForStatus_.store(0);
}

void NeuracoustDspEngine::seek(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double seekSampleRate = projectPlan_.sampleRate > 0.0
        ? projectPlan_.sampleRate
        : (playback_.sampleRate > 0 ? static_cast<double>(playback_.sampleRate) : settings_.sampleRate);
    playbackFrame_ = std::max<int64_t>(0, static_cast<int64_t>(std::round(std::max(0.0, seconds) * seekSampleRate)));
    realtimeProcessFrame_ = playbackFrame_;
    projectRenderState_.resetForSeek();
    previousOutputBlock_.clear();
    reloadCrossfadeSamplesRemaining_ = 0;
    reloadCrossfadeSamplesTotal_ = 0;
    seekRampSamplesRemaining_ = 0;
    seekRampSamplesTotal_ = 0;
    playbackFrameForStatus_.store(playbackFrame_);
    sampleRateForStatus_.store(std::max(1.0, seekSampleRate));
}

void NeuracoustDspEngine::setTransportRunning(bool running) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.transportRunning = running;
    if (running) {
        realtimeProcessFrame_ = playbackFrame_;
        projectRenderState_.resetForSeek();
    }
    message_ = running
        ? "Transport running; live DSP graph is following the timeline."
        : "Transport stopped; live DSP graph remains active.";
}

void NeuracoustDspEngine::armSeekRamp() {
    std::lock_guard<std::mutex> lock(mutex_);
    const double seekSampleRate = projectPlan_.sampleRate > 0.0
        ? projectPlan_.sampleRate
        : (playback_.sampleRate > 0 ? static_cast<double>(playback_.sampleRate) : settings_.sampleRate);
    armSeekRampLocked(seekSampleRate);
}

void NeuracoustDspEngine::pushInputMonitorInterleaved(const float* samples, int64_t frameCount, int channels) {
    if (samples == nullptr || frameCount <= 0 || channels <= 0) {
        return;
    }
    const bool monitorCaptureActive = inputMonitorCaptureActive_.load(std::memory_order_relaxed);
    const bool talkbackActive = talkbackCaptureActive_.load(std::memory_order_relaxed);
    if (!monitorCaptureActive && !talkbackActive) {
        return;
    }
    std::unique_lock<std::mutex> inputLock(inputMonitorMutex_, std::try_to_lock);
    if (!inputLock.owns_lock()) {
        return;
    }
    inputMonitorChannels_ = std::min(2, channels);
    float peak = inputPeak_;
    const auto startSize = inputMonitorBuffer_.size();
    inputMonitorBuffer_.resize(startSize + static_cast<size_t>(frameCount) * 2);
    for (int64_t frame = 0; frame < frameCount; ++frame) {
        const auto source = static_cast<size_t>(frame * channels);
        const float left = samples[source];
        const float right = channels > 1 ? samples[source + 1] : left;
        const auto destination = startSize + static_cast<size_t>(frame * 2);
        inputMonitorBuffer_[destination] = left;
        inputMonitorBuffer_[destination + 1] = right;
        peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
    }
    const double maxBufferedSeconds = talkbackActive ? 0.08 : 0.5;
    const double sampleRate = std::max(1.0, sampleRateForStatus_.load(std::memory_order_relaxed));
    const size_t maxFrames = static_cast<size_t>(std::max(1.0, sampleRate * maxBufferedSeconds));
    const size_t maxSamples = maxFrames * 2;
    if (inputMonitorBuffer_.size() > maxSamples) {
        inputMonitorBuffer_.erase(inputMonitorBuffer_.begin(), inputMonitorBuffer_.end() - static_cast<std::ptrdiff_t>(maxSamples));
    }
    inputPeak_ = std::min(1.0f, peak);
    physicalInputMonitoringActive_ = true;
    physicalInputMonitoringActiveForStatus_.store(true, std::memory_order_relaxed);
    inputMonitorChannelsForStatus_.store(inputMonitorChannels_, std::memory_order_relaxed);
    inputPeakForStatus_.store(inputPeak_, std::memory_order_relaxed);
}

void NeuracoustDspEngine::renderInterleavedStereo(int64_t frameCount, std::vector<float>& interleavedStereo) {
    if (frameCount <= 0) {
        interleavedStereo.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    interleavedStereo.assign(static_cast<size_t>(frameCount) * 2, 0.0f);
    syncProjectMonitorDspRenderPathLocked();
    const ProjectAudioRenderPlan& renderPlan = projectPlan_;
    const bool transportRunning = settings_.transportRunning;
    // Live MIDI (a keyboard held while stopped) must still sound, so do not drop queued
    // live events when the transport is idle — hasProjectRenderContent below already
    // renders the instrument whenever liveMidiEvents is non-empty. The renderer consumes
    // and erases each event, so nothing accumulates.
    const bool hasLiveMidi = !projectRenderState_.liveMidiEvents.empty();
    const bool hasInstrumentTrack = (transportRunning || hasLiveMidi) &&
        std::any_of(renderPlan.tracks.begin(), renderPlan.tracks.end(), [](const TrackState& track) {
            if (track.trackType != "instrument") {
                return false;
            }
            if (std::any_of(track.instrumentSlots.begin(), track.instrumentSlots.end(), [](const InstrumentSlotState& slot) {
                    return slot.enabled && !slot.bypassed && !slot.pluginPath.empty();
                })) {
                return true;
            }
            return
                track.instrument.enabled &&
                !track.instrument.bypassed &&
                !track.instrument.pluginPath.empty();
        });
    const bool hasRealtimeInsertRenderSource = transportRunning &&
        (renderPlan.hasActiveVst3Inserts ||
         renderPlan.hasActiveTrackVst3Inserts ||
         !realtimeTrackInsertChains_.empty() ||
         realtimeInsertChain_.activeVst3Count() > 0);
    const bool hasProjectRenderContent = (transportRunning && (!renderPlan.clips.empty() ||
        !renderPlan.midiRegions.empty())) ||
        hasInstrumentTrack ||
        !projectRenderState_.liveMidiEvents.empty() ||
        hasRealtimeInsertRenderSource;
    const bool projectMonitorDspRenderedInGraph = renderPlan.renderMonitorDsp && hasProjectRenderContent;
    if (hasProjectRenderContent) {
        ProjectAudioBlockMeters meters;
        projectBlock_.assign(static_cast<size_t>(frameCount) * 2u, 0.0f);
        int64_t framesRendered = 0;
        int64_t segmentStartFrame = transportRunning
            ? wrappedPlaybackFrameForPlan(renderPlan, playbackFrame_)
            : realtimeProcessFrame_;
        const int64_t loopEndFrame = transportRunning ? loopEndFrameForPlan(renderPlan) : 0;
        while (framesRendered < frameCount) {
            int64_t segmentFrames = frameCount - framesRendered;
            if (loopEndFrame > 0 && segmentStartFrame < loopEndFrame) {
                segmentFrames = std::min(segmentFrames, loopEndFrame - segmentStartFrame);
            }
            if (segmentFrames <= 0) {
                segmentStartFrame = wrappedPlaybackFrameForPlan(projectPlan_, segmentStartFrame);
                projectRenderState_.resetForSeek();
                continue;
            }

            projectSegmentMeters_ = {};
            renderProjectAudioBlockWithStateAndMeters(renderPlan,
                                                      projectRenderState_,
                                                      segmentStartFrame,
                                                      segmentFrames,
                                                      projectSegmentBlock_,
                                                      &projectSegmentMeters_);
            applyRealtimeTrackInsertChainsLocked(segmentStartFrame, segmentFrames, projectSegmentBlock_);
            if (projectRenderState_.masterInsertProcessingFailed) {
                std::fill(projectSegmentBlock_.begin(), projectSegmentBlock_.end(), 0.0f);
                for (auto& peak : projectSegmentMeters_.trackPeakLeft) {
                    peak = 0.0f;
                }
                for (auto& peak : projectSegmentMeters_.trackPeakRight) {
                    peak = 0.0f;
                }
                message_ = "Realtime VST3 processing failed: " + projectRenderState_.masterInsertLastError;
            }
            const size_t destinationSample = static_cast<size_t>(framesRendered) * 2u;
            const size_t copySamples = std::min(projectSegmentBlock_.size(), projectBlock_.size() - destinationSample);
            std::copy_n(projectSegmentBlock_.begin(), copySamples, projectBlock_.begin() + static_cast<std::ptrdiff_t>(destinationSample));
            mergeBlockMeters(meters, projectSegmentMeters_);

            framesRendered += segmentFrames;
            segmentStartFrame += segmentFrames;
            if (loopEndFrame > 0 && segmentStartFrame >= loopEndFrame && framesRendered < frameCount) {
                segmentStartFrame = wrappedPlaybackFrameForPlan(renderPlan, segmentStartFrame);
                projectRenderState_.resetForSeek();
            }
        }
        if (trackInsertMeterSuppressSamples_ > 0) {
            meters.trackInsertMeterTrackNames.clear();
            meters.trackInsertMeterSlotIndices.clear();
            meters.trackInsertInputPeak.clear();
            meters.trackInsertOutputPeak.clear();
            clearTrackInsertMetersLocked();
            trackInsertMeterSuppressSamples_ = std::max<int64_t>(0, trackInsertMeterSuppressSamples_ - frameCount);
        }
        storeTrackMetersLocked(meters);
    } else {
        // Nothing rendered this block (transport stopped, no live source): fall the track
        // meters to silence instead of freezing them at the last playing value.
        for (auto& peak : projectMeters_.trackPeakLeft) peak = 0.0f;
        for (auto& peak : projectMeters_.trackPeakRight) peak = 0.0f;
    }

    const double increment = kTwoPi * settings_.testToneFrequency / std::max(1.0, settings_.sampleRate);
    for (int64_t frame = 0; frame < frameCount; ++frame) {
        float left = settings_.testToneEnabled ? static_cast<float>(std::sin(phase_) * 0.12) : 0.0f;
        float right = left;
        const float click = transportRunning ? renderMetronomeClickSampleAtFrame(playbackFrame_ + frame, settings_) : 0.0f;
        left += click;
        right += click;

        if (hasProjectRenderContent) {
            const auto projectIndex = static_cast<size_t>(frame * 2);
            if (projectIndex + 1 < projectBlock_.size()) {
                left += projectBlock_[projectIndex];
                right += projectBlock_[projectIndex + 1];
            }
        } else if (transportRunning && playback_.channels > 0 && playbackFrame_ + frame < playback_.frameCount()) {
            const auto sourceFrame = playbackFrame_ + frame;
            const auto sourceIndex = static_cast<size_t>(sourceFrame * playback_.channels);
            if (sourceIndex < playback_.interleavedSamples.size()) {
                left += playback_.interleavedSamples[sourceIndex];
                right += playback_.channels > 1 && sourceIndex + 1 < playback_.interleavedSamples.size()
                    ? playback_.interleavedSamples[sourceIndex + 1]
                    : playback_.interleavedSamples[sourceIndex];
            }
        }

        if (!hasProjectRenderContent) {
            if (const TrackState* master = findTrack(projectPlan_, "Master")) {
                if (master->muted) {
                    left = 0.0f;
                    right = 0.0f;
                } else {
                    std::tie(left, right) = applyStereoGainPan(left, right, master->volumeDb, master->pan);
                }
            }
        }

        const auto destination = static_cast<size_t>(frame * 2);
        interleavedStereo[destination] = left;
        interleavedStereo[destination + 1] = right;
        phase_ += increment;
        if (phase_ >= kTwoPi) {
            phase_ -= kTwoPi;
        }
    }
    mixInputMonitorLocked(frameCount, interleavedStereo);
    const float monitorInputTrimGain = projectMonitorDspRenderedInGraph
        ? 1.0f
        : dbToGain(std::max(-12.0f, std::min(0.0f, settings_.monitorInputTrimDb)));
    if (std::abs(monitorInputTrimGain - 1.0f) > 0.0001f) {
        for (float& sample : interleavedStereo) {
            sample *= monitorInputTrimGain;
        }
    }
    if (settings_.monitorDspEnabled && !projectMonitorDspRenderedInGraph) {
        if (monitorDspTransitionSamplesRemaining_ > 0 &&
            !monitorDspTransitionFromMode_.empty() &&
            !monitorDspTransitionToMode_.empty()) {
            applyMonitorDspTransitionLocked(interleavedStereo);
        } else if (monitorDspModuleTransitionSamplesRemaining_ > 0) {
            applyMonitorDspModuleTransitionLocked(interleavedStereo);
        } else {
            applyMonitorDspPathLocked(settings_.monitorDspPathMode, interleavedStereo);
        }
    }
    applyMonitorOutputInsertChainLocked(interleavedStereo);
    applyMonitorStationControlsLocked(interleavedStereo);
    applyReloadCrossfadeLocked(interleavedStereo);
    applySeekRampLocked(interleavedStereo);
    publishListenRoomLocked(interleavedStereo);
    realtimeProcessFrame_ += frameCount;
    if (transportRunning) {
        playbackFrame_ = wrappedPlaybackFrameForPlan(projectPlan_, playbackFrame_ + frameCount);
    }
    playbackFrameForStatus_.store(playbackFrame_);
    storeMetering(interleavedStereo);
}

AudioEngineStatus NeuracoustDspEngine::statusSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AudioEngineStatus status;
    status.sampleRate = settings_.sampleRate;
    status.transportRunning = settings_.transportRunning;
    status.outputPeakLeft = outputPeakLeft_.load();
    status.outputPeakRight = outputPeakRight_.load();
    status.phaseCorrelation = phaseCorrelation_.load();
    status.spectrumLow = spectrumLow_.load();
    status.spectrumMid = spectrumMid_.load();
    status.spectrumHigh = spectrumHigh_.load();
    {
        std::unique_lock<std::mutex> lock(spectrumMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            status.spectrumBins = spectrumBins_;
            status.goniometerSamples = goniometerSamples_;
        }
    }
    status.momentaryLufs = loudnessMeter_.momentaryLufs();
    status.shortTermLufs = loudnessMeter_.shortTermLufs();
    status.integratedLufs = loudnessMeter_.integratedLufs();
    status.loudnessRange = loudnessMeter_.loudnessRange();
    status.truePeakDb = loudnessMeter_.truePeakDb();
    status.trackMeterNames = projectMeters_.trackNames;
    status.trackPeakLeft = projectMeters_.trackPeakLeft;
    status.trackPeakRight = projectMeters_.trackPeakRight;
    status.trackInsertMeterTrackNames = trackInsertMeterTrackNames_;
    status.trackInsertMeterSlotIndices = trackInsertMeterSlotIndices_;
    status.trackInsertInputPeak = trackInsertInputPeaks_;
    status.trackInsertOutputPeak = trackInsertOutputPeaks_;
    status.trackInsertOutputParameterTrackNames = trackInsertOutputParameterTrackNames_;
    status.trackInsertOutputParameterSlotIndices = trackInsertOutputParameterSlotIndices_;
    status.trackInsertOutputParameterIds = trackInsertOutputParameterIds_;
    status.trackInsertOutputParameterValues = trackInsertOutputParameterValues_;
    status.playbackSeconds = static_cast<double>(playbackFrameForStatus_.load()) / std::max(1.0, sampleRateForStatus_.load());
    status.delayCompensationEnabled = settings_.delayCompensationEnabled;
    status.delayCompensationSamples = settings_.delayCompensationEnabled
        ? static_cast<int>(delayCompensationSamples_)
        : 0;
    status.delayCompensationMs = status.sampleRate > 0.0
        ? (static_cast<double>(status.delayCompensationSamples) / status.sampleRate) * 1000.0
        : 0.0;
    status.directMonitoringEnabled = settings_.lowLatencyRecordMonitoringEnabled;
    status.lowLatencyRecordMonitoringActive = lowLatencyRecordMonitoringActive_;
    status.physicalInputMonitoringActive = physicalInputMonitoringActiveForStatus_.load(std::memory_order_relaxed);
    status.recordArmedTrackCount = recordArmedTrackCount_;
    status.inputChannels = inputMonitorChannelsForStatus_.load(std::memory_order_relaxed);
    status.inputPeak = inputPeakForStatus_.load(std::memory_order_relaxed);
    status.requestedBufferSize = settings_.bufferSize;
    status.playbackStabilityBufferSize = std::max(1, settings_.bufferSize) * std::max(1, settings_.playbackStabilityBufferMultiplier);
    status.dspEngineName = "Neuracoust DSP Engine";
    status.monitorDspPathMode = settings_.monitorDspPathMode;
    status.remoteDspMonitorActive = remoteDspMonitorActive_;
    status.remoteDspRoundTripMs = remoteDspRoundTripMs_;
    status.remoteDspAverageRoundTripJitterUs = remoteDspAverageRoundTripJitterUs_;
    status.remoteDspMaxRoundTripJitterUs = remoteDspMaxRoundTripJitterUs_;
    status.requestedPerformanceCoreCount = std::max(1, settings_.requestedPerformanceCoreCount);
    status.monitorPathDescription = lowLatencyRecordMonitoringActive_
        ? "Record-armed tracks are reserved for the low-latency monitor path; playback uses the stability buffer."
        : "Playback/mix path uses the stability buffer; no record monitor path is armed.";
    status.activeRealtimeVst3MasterInsertCount = static_cast<int>(realtimeInsertChain_.activeVst3Count());
    int activeRealtimeTrackInserts = 0;
    for (const auto& trackChain : realtimeTrackInsertChains_) {
        if (!trackChain.remoteDsp) {
            activeRealtimeTrackInserts += static_cast<int>(trackChain.chain.activeVst3Count());
        }
    }
    activeRealtimeTrackInserts = std::max(
        activeRealtimeTrackInserts,
        static_cast<int>(projectPlan_.activeTrackVst3InsertLabels.size()) - activeRemoteDspTrackInsertCount_);
    status.activeRealtimeVst3TrackInsertCount = activeRealtimeTrackInserts;
    status.activeRemoteDspTrackInsertCount = activeRemoteDspTrackInsertCount_;
    status.activeOfflineVst3TrackInsertCount = static_cast<int>(projectPlan_.activeTrackVst3InsertLabels.size());
    status.listenRoom = listenRoomSender_.status();
    status.message = message_;
    return status;
}

std::string NeuracoustDspEngine::lastMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

int NeuracoustDspEngine::activeVst3MasterInsertCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(realtimeInsertChain_.activeVst3Count());
}

int NeuracoustDspEngine::activeVst3TrackInsertCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(projectPlan_.activeTrackVst3InsertLabels.size());
}

int NeuracoustDspEngine::activeRemoteDspTrackInsertCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeRemoteDspTrackInsertCount_;
}

void NeuracoustDspEngine::warmRouteInsertChainsLocked() {
    // Prepare (fork/load) the route-graph insert workers off the audio thread, so
    // the first playback block does not fork/load a plug-in on the realtime thread
    // (which stuttered/crackled at the start of playback). Only while the transport
    // is stopped, so a live reload never blocks running audio.
    if (settings_.transportRunning) {
        return;
    }
    if (!projectPlan_.renderTrackVst3Inserts ||
        (!projectPlan_.hasActiveTrackVst3Inserts && !projectPlan_.hasActiveVst3Inserts)) {
        return;
    }
    std::vector<float> warmBlock;
    renderProjectAudioBlockWithStateAndMeters(projectPlan_, projectRenderState_, 0,
                                              std::max(1, maxBlockSize_), warmBlock, nullptr);
    // Keep the prepared chains, but discard the warm block's transient DSP state so
    // real playback starts clean.
    projectRenderState_.resetForSeek();
}

bool NeuracoustDspEngine::prepareRealtimeInsertChainLocked(int maxBlockSize, std::string& error) {
    realtimeInsertChain_.reset();
    monitorOutputInsertChain_.reset();
    monitorOutputInsertActiveSlot_ = -1;
    // Reuse still-valid out-of-process plug-in workers instead of tearing them all
    // down and reloading every plug-in from scratch. Rebuilding the graph (adding a
    // track/insert, copying a track) otherwise re-spawns EVERY worker and reloads
    // heavy plug-ins (a Waves shell takes several seconds each) on the main thread —
    // the multi-second beachball the user sees on every edit. We key each local
    // chain by its identity signature; a chain whose signature is unchanged keeps
    // its running worker, so only genuinely new/changed inserts pay the load cost.
    std::vector<RealtimeTrackInsertChain> oldTrackChains = std::move(realtimeTrackInsertChains_);
    realtimeTrackInsertChains_.clear();
    std::map<std::string, size_t> reusableChainBySignature;
    std::vector<bool> reusableChainConsumed(oldTrackChains.size(), false);
    for (size_t i = 0; i < oldTrackChains.size(); ++i) {
        if (!oldTrackChains[i].remoteDsp && !oldTrackChains[i].signature.empty()) {
            reusableChainBySignature.emplace(oldTrackChains[i].signature, i);
        }
    }
    activeRemoteDspTrackInsertCount_ = 0;
    delayCompensationSamples_ = 0;
    if (projectPlan_.hasActiveVst3Inserts &&
        !realtimeInsertChain_.prepare(projectPlan_, settings_.sampleRate, std::max(1, maxBlockSize), error)) {
        return false;
    }
    if (settings_.delayCompensationEnabled) {
        delayCompensationSamples_ = std::max(
            projectPlan_.delayCompensationSamples,
            realtimeInsertChain_.activeLatencySamples());
    }
    if (!prepareMonitorOutputInsertChainLocked(maxBlockSize, error)) {
        message_ = "Monitor output VST3 inserts stayed dry: " + error;
        error.clear();
    } else if (settings_.delayCompensationEnabled) {
        delayCompensationSamples_ = std::max(delayCompensationSamples_, monitorOutputInsertChain_.activeLatencySamples());
    }

    std::vector<std::string> skippedTrackInserts;
    bool hasRemoteCapableTrackInsert = false;
    for (const auto& track : projectPlan_.tracks) {
        if (track.inserts.empty() ||
            track.trackType == "folder" ||
            track.trackType == "bus_folder" ||
            track.trackType == "master" ||
            track.trackType == "monitor" ||
            !isMasterOutputBus(track.outputBus)) {
            continue;
        }
        for (const auto& insert : track.inserts) {
            const std::string effectiveMode = effectiveTrackInsertDspExecutionMode(insert);
            if (!isRemoteInternalDspExecutionMode(effectiveMode)) {
                continue;
            }
            const auto remoteCapability = remoteDspCapabilityForInsert(insert, false, true);
            if (remoteCapability.mode == RemoteDspInsertMode::RemoteCapable) {
                hasRemoteCapableTrackInsert = true;
                break;
            }
        }
        if (hasRemoteCapableTrackInsert) {
            break;
        }
    }
    RemoteDspServerSettings remotePlanSettings = settings_.remoteDspServer;
    // Never resolve a hostname on the realtime render thread: getaddrinfo() for an
    // mDNS ".local" name can block for seconds if the node is absent, freezing audio
    // (the app then looks crashed). Only probe when the host is a numeric IPv4, which
    // resolves instantly; a hostname target simply skips the auto-detect until the user
    // enters an IP.
    auto hostIsNumericIpv4 = [](const std::string& host) {
        if (host.empty()) return false;
        int dots = 0;
        for (char c : host) {
            if (c == '.') { ++dots; }
            else if (c < '0' || c > '9') { return false; }
        }
        return dots == 3;
    };
    if (remotePlanSettings.enabled && hasRemoteCapableTrackInsert) {
        remotePlanSettings.pluginDspEnabled = true;
        double measuredRemoteRoundTripMs = 0.0;
        if (remotePlanSettings.loadedPluginIdHint.empty() &&
            hostIsNumericIpv4(remotePlanSettings.host)) {
            RemoteDspServerSettings infoSettings = remotePlanSettings;
            infoSettings.timeoutMs = infoSettings.timeoutMs > 0 ? std::min(infoSettings.timeoutMs, 150) : 150;
            const auto serverInfo = queryRemoteDspServerInfo(infoSettings);
            if (serverInfo.reachable) {
                remotePlanSettings.loadedPluginIdHint = serverInfo.pluginId;
                measuredRemoteRoundTripMs = serverInfo.roundTripMs;
            }
        }
        if (measuredRemoteRoundTripMs > 0.0) {
            recordRemoteDspRoundTripLocked(measuredRemoteRoundTripMs);
        }
    }
    const auto remoteCorePlan = makeRemoteDspCorePlan(remotePlanSettings,
                                                      remotePlanSettings.totalCoreHint,
                                                      remoteMonitorDspRequestedLocked());
    const bool pluginRemoteDspAvailable =
        remotePlanSettings.enabled &&
        remotePlanSettings.pluginDspEnabled &&
        remoteCorePlan.pluginCores > 0;
    std::vector<std::string> skippedRemoteInserts;
    if (hasRemoteCapableTrackInsert && !pluginRemoteDspAvailable) {
        if (!remotePlanSettings.enabled) {
            skippedRemoteInserts.push_back("remote server disabled");
        } else if (!remotePlanSettings.pluginDspEnabled) {
            skippedRemoteInserts.push_back("plugin DSP disabled");
        } else if (remoteCorePlan.pluginCores == 0) {
            skippedRemoteInserts.push_back("no plugin DSP core assigned");
        }
    }
    for (const auto& track : projectPlan_.tracks) {
        if (track.inserts.empty() ||
            track.trackType == "folder" ||
            track.trackType == "bus_folder" ||
            track.trackType == "master" ||
            track.trackType == "monitor" ||
            !isMasterOutputBus(track.outputBus)) {
            continue;
        }
        ProjectAudioRenderPlan trackInsertPlan;
        trackInsertPlan.sampleRate = projectPlan_.sampleRate;
        std::vector<std::string> remoteModuleIds;
        std::vector<RemoteDspParameterValue> remoteParameters;
        std::vector<int> remoteSlotIndices;
        std::vector<int> localSlotIndices;
        int firstActiveSlotIndex = -1;
        bool protectLocalDryWhenSilent = false;
        for (size_t insertIndex = 0; insertIndex < track.inserts.size(); ++insertIndex) {
            const auto& insert = track.inserts[insertIndex];
            InsertState renderInsert = trackInsertToRenderInsert(insert);
            const auto remoteCapability = remoteDspCapabilityForInsert(insert, pluginRemoteDspAvailable, true);
            const std::string effectiveMode = effectiveTrackInsertDspExecutionMode(insert);
            const bool serverModuleMatches = remotePlanSettings.loadedPluginIdHint.empty() ||
                remotePlanSettings.loadedPluginIdHint == remoteCapability.moduleId;
            if (isRemoteInternalDspExecutionMode(effectiveMode) &&
                pluginRemoteDspAvailable &&
                remoteCapability.mode == RemoteDspInsertMode::RemoteActive &&
                serverModuleMatches) {
                if (firstActiveSlotIndex < 0) {
                    firstActiveSlotIndex = static_cast<int>(insertIndex);
                    remoteParameters = remoteParametersForInsert(insert);
                }
                remoteModuleIds.push_back(remoteCapability.moduleId);
                remoteSlotIndices.push_back(static_cast<int>(insertIndex));
                continue;
            }
            if (isRemoteInternalDspExecutionMode(effectiveMode) &&
                !remoteCapability.moduleId.empty() &&
                pluginRemoteDspAvailable &&
                !serverModuleMatches) {
                skippedRemoteInserts.push_back(track.name + ": " + insert.pluginName +
                    " requires " + remoteCapability.moduleId +
                    ", server loaded " + remotePlanSettings.loadedPluginIdHint);
            }
            if (!insert.enabled || !isVst3MasterInsert(renderInsert)) {
                continue;
            }
            if (!isRemoteInternalDspExecutionMode(effectiveMode)) {
                continue;
            }
            if (firstActiveSlotIndex < 0) {
                firstActiveSlotIndex = static_cast<int>(insertIndex);
            }
            localSlotIndices.push_back(static_cast<int>(insertIndex));
            if (remoteCapability.moduleId.empty()) {
                protectLocalDryWhenSilent = true;
            }
            trackInsertPlan.hasActiveVst3Inserts = true;
            trackInsertPlan.activeVst3Inserts.push_back(renderInsert);
        }
        if (!remoteModuleIds.empty()) {
            RealtimeTrackInsertChain trackChain;
            trackChain.trackName = track.name;
            trackChain.remoteDsp = true;
            trackChain.remoteModuleId = remoteModuleIds.front();
            trackChain.remoteParameters = std::move(remoteParameters);
            trackChain.remoteStream = std::make_unique<RemoteDspAsyncStream>();
            trackChain.slotIndex = remoteSlotIndices.empty() ? firstActiveSlotIndex : remoteSlotIndices.front();
            trackChain.slotIndices = std::move(remoteSlotIndices);
            trackChain.latencySamples = settings_.delayCompensationEnabled
                ? estimatedRemoteDspLatencySamples(remotePlanSettings,
                                                   settings_.sampleRate,
                                                   maxBlockSize,
                                                   remoteDspRoundTripMs_)
                : 0u;
            delayCompensationSamples_ = std::max(delayCompensationSamples_, trackChain.latencySamples);
            trackChain.transitionSamplesTotal = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.08));
            trackChain.transitionSamplesRemaining = trackChain.transitionSamplesTotal;
            realtimeTrackInsertChains_.push_back(std::move(trackChain));
            activeRemoteDspTrackInsertCount_ += static_cast<int>(remoteModuleIds.size());
        }
        if (!localSlotIndices.empty()) {
            RealtimeTrackInsertChain trackChain;
            trackChain.trackName = track.name;
            trackChain.remoteDsp = false;
            trackChain.slotIndex = localSlotIndices.front();
            trackChain.slotIndices = std::move(localSlotIndices);
            trackChain.protectDryWhenSilent = protectLocalDryWhenSilent;

            std::ostringstream signatureOut;
            signatureOut << track.name << '|' << track.outputBus
                         << "|sr=" << settings_.sampleRate << ";blk=" << std::max(1, maxBlockSize) << '\n';
            for (const auto& renderInsert : trackInsertPlan.activeVst3Inserts) {
                appendInsertSignature(signatureOut, renderInsert);
            }
            trackChain.signature = signatureOut.str();

            bool reusedRunningWorker = false;
            const auto reuseIt = reusableChainBySignature.find(trackChain.signature);
            if (reuseIt != reusableChainBySignature.end() && !reusableChainConsumed[reuseIt->second]) {
                // Identical chain already running — adopt its worker instead of reloading.
                RealtimeTrackInsertChain& old = oldTrackChains[reuseIt->second];
                trackChain.chain = std::move(old.chain);
                trackChain.latencySamples = old.latencySamples;
                reusableChainConsumed[reuseIt->second] = true;
                reusedRunningWorker = true;
            }

            if (!reusedRunningWorker) {
                if (!trackChain.chain.prepare(trackInsertPlan, settings_.sampleRate, std::max(1, maxBlockSize), error)) {
                    skippedTrackInserts.push_back(track.name + ": " + error);
                    error.clear();
                    continue;
                }
                trackChain.latencySamples = settings_.delayCompensationEnabled
                    ? trackChain.chain.totalLatencySamples()
                    : 0u;
            } else if (!settings_.delayCompensationEnabled) {
                trackChain.latencySamples = 0u;
            }
            delayCompensationSamples_ = std::max(delayCompensationSamples_, trackChain.latencySamples);
            trackChain.transitionSamplesTotal = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.08));
            // A reused worker's audio is continuous, so it needs no re-entry crossfade.
            trackChain.transitionSamplesRemaining = reusedRunningWorker ? 0 : trackChain.transitionSamplesTotal;
            realtimeTrackInsertChains_.push_back(std::move(trackChain));
        }
        (void)firstActiveSlotIndex;
    }
    if (!skippedTrackInserts.empty()) {
        message_ = "Some track VST3 inserts could not be prepared for realtime playback: " + skippedTrackInserts.front();
    } else if (!skippedRemoteInserts.empty()) {
        message_ = "Some NDS track inserts stayed dry: " + skippedRemoteInserts.front();
    } else if (activeRemoteDspTrackInsertCount_ > 0) {
        message_ = "Prepared " + std::to_string(activeRemoteDspTrackInsertCount_) +
            " NDS track insert(s) for 누라쿠스트 DSP 서버.";
    }
    realtimeInsertGraphSignature_ = realtimeInsertGraphSignature(projectPlan_, settings_, maxBlockSize);
    error.clear();
    return true;
}

bool NeuracoustDspEngine::prepareMonitorOutputInsertChainLocked(int maxBlockSize, std::string& error) {
    monitorOutputInsertChain_.reset();
    monitorOutputInsertActiveSlot_ = -1;
    int activeSlot = -1;
    const auto* inserts = activeSpeakerOutputInserts(settings_.monitorModules, &activeSlot);
    if (inserts == nullptr || inserts->empty()) {
        error.clear();
        return true;
    }
    ProjectAudioRenderPlan monitorInsertPlan;
    monitorInsertPlan.sampleRate = settings_.sampleRate;
    for (const auto& insert : *inserts) {
        InsertState renderInsert = trackInsertToRenderInsert(insert);
        if (!insert.enabled || insert.bypassed || !isVst3MasterInsert(renderInsert) || insert.pluginPath.empty()) {
            continue;
        }
        renderInsert.available = true;
        monitorInsertPlan.hasActiveVst3Inserts = true;
        monitorInsertPlan.activeVst3Inserts.push_back(std::move(renderInsert));
    }
    if (!monitorInsertPlan.hasActiveVst3Inserts) {
        error.clear();
        return true;
    }
    if (!monitorOutputInsertChain_.prepare(monitorInsertPlan, settings_.sampleRate, std::max(1, maxBlockSize), error)) {
        monitorOutputInsertChain_.reset();
        monitorOutputInsertActiveSlot_ = -1;
        return false;
    }
    monitorOutputInsertActiveSlot_ = activeSlot;
    return true;
}

void NeuracoustDspEngine::applyMonitorOutputInsertChainLocked(std::vector<float>& interleavedStereo) {
    if (!monitorOutputInsertChain_.isPrepared() || interleavedStereo.empty()) {
        return;
    }
    const int frameCount = static_cast<int>(interleavedStereo.size() / 2u);
    if (frameCount <= 0) {
        return;
    }
    std::string error;
    monitorOutputInsertDryFallback_ = interleavedStereo;
    if (!monitorOutputInsertChain_.processInterleavedStereo(interleavedStereo, frameCount, error)) {
        interleavedStereo = monitorOutputInsertDryFallback_;
        message_ = "Monitor output VST3 insert failed: " + error;
        monitorOutputInsertChain_.reset();
        monitorOutputInsertActiveSlot_ = -1;
    }
}

void NeuracoustDspEngine::applyRealtimeTrackInsertChainsLocked(int64_t startFrame,
                                                              int64_t frameCount,
                                                              std::vector<float>& interleavedStereo) {
    if (realtimeTrackInsertChains_.empty() ||
        frameCount <= 0 ||
        interleavedStereo.size() < static_cast<size_t>(frameCount) * 2) {
        return;
    }
    trackInsertOutputParameterTrackNames_.clear();
    trackInsertOutputParameterSlotIndices_.clear();
    trackInsertOutputParameterIds_.clear();
    trackInsertOutputParameterValues_.clear();
    const TrackState* master = findTrack(projectPlan_, "Master");
    for (auto& trackChain : realtimeTrackInsertChains_) {
        const TrackState* track = findTrack(projectPlan_, trackChain.trackName);
        if (track == nullptr) {
            continue;
        }
        if (realtimeTrackPlaybackMuted(projectPlan_, *track)) {
            storeTrackInsertMeterLocked(trackChain.trackName, trackChain.slotIndex, 0.0f, 0.0f);
            continue;
        }
        if (!renderTrackPreFaderStereoBlock(projectPlan_,
                                            trackChain.trackName,
                                            startFrame,
                                            frameCount,
                                            trackInsertDryBlock_)) {
            trackInsertDryBlock_.assign(static_cast<size_t>(frameCount) * 2, 0.0f);
        }
        const int64_t compensatedStartFrame = startFrame + static_cast<int64_t>(trackChain.latencySamples);
        if (trackChain.latencySamples > 0) {
            if (!renderTrackPreFaderStereoBlock(projectPlan_,
                                                trackChain.trackName,
                                                compensatedStartFrame,
                                                frameCount,
                                                trackInsertCompensatedInputBlock_)) {
                trackInsertCompensatedInputBlock_.assign(static_cast<size_t>(frameCount) * 2, 0.0f);
            }
            trackInsertProcessedBlock_ = trackInsertCompensatedInputBlock_;
        } else {
            trackInsertProcessedBlock_ = trackInsertDryBlock_;
        }
        const float insertInputPeak = stereoPeak(trackInsertProcessedBlock_);
        if (trackChain.remoteDsp) {
            RemoteDspServerSettings remoteSettings = settings_.remoteDspServer;
            const uint16_t networkBufferFrames = static_cast<uint16_t>(std::max<uint16_t>(128u, std::min<uint16_t>(1024u, remoteSettings.networkBufferFrames)));
            remoteSettings.channelCount = 2;
            remoteSettings.frameCount = static_cast<uint16_t>(std::min<int64_t>(frameCount, networkBufferFrames));
            remoteSettings.sampleRate = settings_.sampleRate;
            const double bufferLatencyMs = settings_.sampleRate > 0.0
                ? (static_cast<double>(networkBufferFrames) * 1000.0 / settings_.sampleRate)
                : 2.7;
            remoteSettings.timeoutMs = std::max<int>(8, static_cast<int>(std::ceil(bufferLatencyMs + 5.0)));
	            if (trackChain.remoteStream == nullptr ||
	                !trackChain.remoteStream->process(remoteSettings,
	                                                  trackInsertProcessedBlock_,
	                                                  trackChain.remoteParameters,
	                                                  remoteTrackInsertProcessedBlock_) ||
	                remoteTrackInsertProcessedBlock_.size() != trackInsertProcessedBlock_.size()) {
	                message_ = "Remote DSP track insert buffering: " + trackChain.trackName;
	                storeTrackInsertMeterLocked(trackChain.trackName,
	                                            trackChain.slotIndex,
	                                            insertInputPeak,
	                                            insertInputPeak);
	                continue;
	            }
            trackInsertProcessedBlock_ = remoteTrackInsertProcessedBlock_;
            const auto streamStatus = trackChain.remoteStream->status();
            const float remoteOutputPeak = stereoPeak(trackInsertProcessedBlock_);
            if (trackChain.remoteParameters.empty() &&
                insertInputPeak > 0.0001f &&
                remoteOutputPeak > insertInputPeak * 1.5f) {
                const float safetyGain = std::clamp(insertInputPeak / remoteOutputPeak, 0.0f, 1.0f);
                for (auto& sample : trackInsertProcessedBlock_) {
                    sample *= safetyGain;
                }
            }
            recordRemoteDspRoundTripLocked(streamStatus.averageRoundTripMs);
            publishNdsPluginTelemetry(trackChain.remoteModuleId,
                                      trackChain.trackName,
                                      trackChain.slotIndex,
                                      trackInsertDryBlock_,
                                      trackInsertProcessedBlock_,
                                      streamStatus.averageRoundTripMs);
            message_ = "Track insert routed through 누라쿠스트 DSP 서버: " + trackChain.trackName + " (" + std::to_string(streamStatus.averageRoundTripMs) + " ms).";
        } else {
            std::string vst3Error;
            std::vector<bool> bypassStates;
            bypassStates.reserve(trackChain.slotIndices.size());
            for (const int slotIndex : trackChain.slotIndices) {
                const bool bypassed = slotIndex < 0 ||
                    static_cast<size_t>(slotIndex) >= track->inserts.size() ||
                    !track->inserts[static_cast<size_t>(slotIndex)].enabled ||
                    track->inserts[static_cast<size_t>(slotIndex)].bypassed;
                bypassStates.push_back(bypassed);
            }
	            trackChain.chain.setBypassStates(bypassStates);
	            if (!trackChain.chain.processInterleavedStereo(trackInsertProcessedBlock_, static_cast<int>(frameCount), vst3Error)) {
	                message_ = "Realtime track VST3 processing failed: " + trackChain.trackName + ": " + vst3Error;
	                const float dryPeak = stereoPeak(trackInsertDryBlock_);
	                if (trackChain.slotIndices.empty()) {
	                    storeTrackInsertMeterLocked(trackChain.trackName,
	                                                trackChain.slotIndex,
	                                                insertInputPeak,
	                                                dryPeak);
	                } else {
	                    for (const int slotIndex : trackChain.slotIndices) {
	                        storeTrackInsertMeterLocked(trackChain.trackName,
	                                                    slotIndex,
	                                                    insertInputPeak,
	                                                    dryPeak);
	                    }
	                }
	                continue;
	            }
            const auto outputParameterChanges = trackChain.chain.drainOutputParameterChanges();
            for (size_t chainIndex = 0; chainIndex < outputParameterChanges.size(); ++chainIndex) {
                const int slotIndex = chainIndex < trackChain.slotIndices.size()
                    ? trackChain.slotIndices[chainIndex]
                    : trackChain.slotIndex;
                storeTrackInsertOutputParametersLocked(trackChain.trackName,
                                                       slotIndex,
                                                      outputParameterChanges[chainIndex]);
            }
        }
        if (trackChain.transitionSamplesRemaining > 0 && trackChain.transitionSamplesTotal > 0) {
            const int64_t transitionFrames = std::min<int64_t>(frameCount, trackChain.transitionSamplesRemaining);
            const int64_t total = std::max<int64_t>(1, trackChain.transitionSamplesTotal);
            for (int64_t frame = 0; frame < transitionFrames; ++frame) {
                const int64_t elapsed = total - trackChain.transitionSamplesRemaining + frame;
                const float wet = static_cast<float>(std::clamp(static_cast<double>(elapsed) /
                                                                    static_cast<double>(total),
                                                                0.0,
                                                                1.0));
                const auto index = static_cast<size_t>(frame) * 2u;
                trackInsertProcessedBlock_[index] =
                    trackInsertDryBlock_[index] + (trackInsertProcessedBlock_[index] - trackInsertDryBlock_[index]) * wet;
                trackInsertProcessedBlock_[index + 1u] =
                    trackInsertDryBlock_[index + 1u] + (trackInsertProcessedBlock_[index + 1u] - trackInsertDryBlock_[index + 1u]) * wet;
            }
            trackChain.transitionSamplesRemaining -= transitionFrames;
        }
        float insertOutputPeak = stereoPeak(trackInsertProcessedBlock_);
        if (!trackChain.remoteDsp &&
            trackChain.protectDryWhenSilent &&
            insertInputPeak > 0.0001f &&
            insertOutputPeak < std::max(0.000001f, insertInputPeak * 0.001f)) {
            trackInsertProcessedBlock_ = trackInsertDryBlock_;
            insertOutputPeak = stereoPeak(trackInsertProcessedBlock_);
            message_ = "Native VST3 insert returned silence; kept dry track signal: " + trackChain.trackName;
        }
	        const auto& processMeters = trackChain.chain.lastProcessMeters();
	        if (trackChain.slotIndices.empty()) {
	            float meterInputPeak = insertInputPeak;
	            float meterOutputPeak = insertOutputPeak;
	            if (!trackChain.remoteDsp && !processMeters.empty()) {
	                meterInputPeak = processMeters.front().inputPeak;
	                meterOutputPeak = processMeters.front().outputPeak;
	            }
	            storeTrackInsertMeterLocked(trackChain.trackName,
	                                        trackChain.slotIndex,
	                                        meterInputPeak,
	                                        meterOutputPeak);
	        } else {
	            for (size_t meterIndex = 0; meterIndex < trackChain.slotIndices.size(); ++meterIndex) {
	                const int slotIndex = trackChain.slotIndices[meterIndex];
	                float meterInputPeak = insertInputPeak;
	                float meterOutputPeak = insertOutputPeak;
	                if (!trackChain.remoteDsp && meterIndex < processMeters.size()) {
	                    meterInputPeak = processMeters[meterIndex].inputPeak;
	                    meterOutputPeak = processMeters[meterIndex].outputPeak;
	                }
	                storeTrackInsertMeterLocked(trackChain.trackName,
	                                            slotIndex,
	                                            meterInputPeak,
	                                            meterOutputPeak);
	            }
	        }
        for (int64_t frame = 0; frame < frameCount; ++frame) {
            const auto index = static_cast<size_t>(frame) * 2;
            auto [dryLeft, dryRight] = applyStereoGainPan(trackInsertDryBlock_[index],
                                                          trackInsertDryBlock_[index + 1],
                                                          track->volumeDb,
                                                          track->pan);
            auto [processedLeft, processedRight] = applyStereoGainPan(trackInsertProcessedBlock_[index],
                                                                      trackInsertProcessedBlock_[index + 1],
                                                                      track->volumeDb,
                                                                      track->pan);
            if (master != nullptr) {
                if (master->muted) {
                    dryLeft = 0.0f;
                    dryRight = 0.0f;
                    processedLeft = 0.0f;
                    processedRight = 0.0f;
                } else {
                    std::tie(dryLeft, dryRight) = applyStereoGainPan(dryLeft, dryRight, master->volumeDb, master->pan);
                    std::tie(processedLeft, processedRight) = applyStereoGainPan(processedLeft, processedRight, master->volumeDb, master->pan);
                }
            }
            interleavedStereo[index] += processedLeft - dryLeft;
            interleavedStereo[index + 1] += processedRight - dryRight;
        }
    }
}

bool NeuracoustDspEngine::remoteMonitorDspRequestedLocked() const {
    return monitorDspModeRequestsRemoteLocked(settings_.monitorDspPathMode);
}

bool NeuracoustDspEngine::monitorDspModeRequestsRemoteLocked(const std::string& mode) const {
    return settings_.remoteDspServer.enabled &&
        (mode == "external" || mode == "nds" || mode == "remote_external" || mode == "auto");
}

bool NeuracoustDspEngine::projectMonitorDspCanRenderInGraphLocked() const {
    return !projectPlan_.clips.empty() &&
        settings_.monitorDspEnabled &&
        !monitorDspModeRequestsRemoteLocked(settings_.monitorDspPathMode) &&
        monitorDspTransitionSamplesRemaining_ <= 0 &&
        monitorDspModuleTransitionSamplesRemaining_ <= 0 &&
        !settings_.testToneEnabled &&
        !settings_.metronomeEnabled &&
        !lowLatencyRecordMonitoringActive_ &&
        !physicalInputMonitoringActiveForStatus_.load(std::memory_order_relaxed) &&
        !talkbackCaptureActive_.load(std::memory_order_relaxed) &&
        playback_.channels <= 0;
}

void NeuracoustDspEngine::syncProjectMonitorDspRenderPathLocked() {
    const bool shouldRenderInGraph = projectMonitorDspCanRenderInGraphLocked();
    if (projectPlan_.renderMonitorDsp == shouldRenderInGraph) {
        return;
    }
    projectPlan_.renderMonitorDsp = shouldRenderInGraph;
    projectRenderState_.reset();
}

bool NeuracoustDspEngine::applyMonitorDspPathLocked(const std::string& mode, std::vector<float>& interleavedStereo) {
    if (monitorDspModeRequestsRemoteLocked(mode)) {
        if (applyRemoteMonitorDspLocked(interleavedStereo)) {
            return true;
        }
        applyLocalMonitorDspLocked(interleavedStereo);
        return false;
    }
    applyLocalMonitorDspLocked(interleavedStereo);
    remoteDspMonitorActive_ = false;
    remoteMonitorDspStream_.reset();
    resetRemoteDspTelemetryLocked();
    return true;
}

void NeuracoustDspEngine::applyMonitorDspTransitionLocked(std::vector<float>& interleavedStereo) {
    monitorDspTransitionFromBlock_ = interleavedStereo;
    monitorDspTransitionToBlock_ = interleavedStereo;
    const bool fromReady = applyMonitorDspPathLocked(monitorDspTransitionFromMode_, monitorDspTransitionFromBlock_);
    const bool toReady = applyMonitorDspPathLocked(monitorDspTransitionToMode_, monitorDspTransitionToBlock_);
    if (monitorDspModeRequestsRemoteLocked(monitorDspTransitionToMode_) && !toReady) {
        interleavedStereo = monitorDspTransitionFromBlock_;
        message_ = "External monitor DSP async stream warming; holding previous monitor path.";
        return;
    }
    if (monitorDspModeRequestsRemoteLocked(monitorDspTransitionFromMode_) && !fromReady) {
        interleavedStereo = monitorDspTransitionToBlock_;
        monitorDspTransitionSamplesRemaining_ = 0;
        monitorDspTransitionSamplesTotal_ = 0;
        monitorDspTransitionFromMode_.clear();
        monitorDspTransitionToMode_.clear();
        return;
    }

    const size_t frames = std::min(monitorDspTransitionFromBlock_.size(), monitorDspTransitionToBlock_.size()) / 2u;
    const int64_t total = std::max<int64_t>(1, monitorDspTransitionSamplesTotal_);
    interleavedStereo.assign(frames * 2u, 0.0f);
    for (size_t frame = 0; frame < frames; ++frame) {
        const int64_t elapsed = total - monitorDspTransitionSamplesRemaining_ + static_cast<int64_t>(frame);
        const float toMix = static_cast<float>(std::max<double>(0.0, std::min<double>(1.0, static_cast<double>(elapsed) / static_cast<double>(total))));
        const float fromMix = 1.0f - toMix;
        const size_t index = frame * 2u;
        interleavedStereo[index] = monitorDspTransitionFromBlock_[index] * fromMix + monitorDspTransitionToBlock_[index] * toMix;
        interleavedStereo[index + 1u] = monitorDspTransitionFromBlock_[index + 1u] * fromMix + monitorDspTransitionToBlock_[index + 1u] * toMix;
    }
    monitorDspTransitionSamplesRemaining_ -= static_cast<int64_t>(frames);
    if (monitorDspTransitionSamplesRemaining_ <= 0) {
        monitorDspTransitionSamplesRemaining_ = 0;
        monitorDspTransitionSamplesTotal_ = 0;
        monitorDspTransitionFromMode_.clear();
        monitorDspTransitionToMode_.clear();
    }
}

void NeuracoustDspEngine::applyLocalMonitorDspLocked(std::vector<float>& interleavedStereo) {
    applyLocalMonitorDspLocked(monitorProcessor_, interleavedStereo);
}

void NeuracoustDspEngine::applyMonitorDspModuleTransitionLocked(std::vector<float>& interleavedStereo) {
    monitorDspTransitionFromBlock_ = interleavedStereo;
    monitorDspTransitionToBlock_ = interleavedStereo;
    applyLocalMonitorDspLocked(previousMonitorProcessor_, monitorDspTransitionFromBlock_);
    applyLocalMonitorDspLocked(monitorProcessor_, monitorDspTransitionToBlock_);

    const size_t frames = std::min(monitorDspTransitionFromBlock_.size(), monitorDspTransitionToBlock_.size()) / 2u;
    const int64_t total = std::max<int64_t>(1, monitorDspModuleTransitionSamplesTotal_);
    interleavedStereo.assign(frames * 2u, 0.0f);
    for (size_t frame = 0; frame < frames; ++frame) {
        const int64_t elapsed = total - monitorDspModuleTransitionSamplesRemaining_ + static_cast<int64_t>(frame);
        const float toMix = static_cast<float>(std::clamp(static_cast<double>(elapsed) / static_cast<double>(total), 0.0, 1.0));
        const float fromMix = 1.0f - toMix;
        const size_t index = frame * 2u;
        interleavedStereo[index] = monitorDspTransitionFromBlock_[index] * fromMix + monitorDspTransitionToBlock_[index] * toMix;
        interleavedStereo[index + 1u] = monitorDspTransitionFromBlock_[index + 1u] * fromMix + monitorDspTransitionToBlock_[index + 1u] * toMix;
    }
    monitorDspModuleTransitionSamplesRemaining_ -= static_cast<int64_t>(frames);
    if (monitorDspModuleTransitionSamplesRemaining_ <= 0) {
        monitorDspModuleTransitionSamplesRemaining_ = 0;
        monitorDspModuleTransitionSamplesTotal_ = 0;
        previousMonitorProcessor_ = monitorProcessor_;
        syncProjectMonitorDspRenderPathLocked();
    }
}

void NeuracoustDspEngine::applyLocalMonitorDspLocked(MonitorDspProcessor& processor, std::vector<float>& interleavedStereo) {
    for (size_t index = 0; index + 1 < interleavedStereo.size(); index += 2) {
        const auto processed = processor.process({interleavedStereo[index], interleavedStereo[index + 1]});
        interleavedStereo[index] = processed.left;
        interleavedStereo[index + 1] = processed.right;
    }
}

bool NeuracoustDspEngine::applyRemoteMonitorDspLocked(std::vector<float>& interleavedStereo) {
    if (interleavedStereo.empty()) {
        remoteDspMonitorActive_ = false;
        remoteMonitorDspStream_.reset();
        return false;
    }
    RemoteDspServerSettings remoteSettings = settings_.remoteDspServer;
    const uint16_t networkBufferFrames = static_cast<uint16_t>(std::max<uint16_t>(128u, std::min<uint16_t>(1024u, remoteSettings.networkBufferFrames)));
    remoteSettings.channelCount = 2;
    remoteSettings.frameCount = static_cast<uint16_t>(std::min<size_t>(interleavedStereo.size() / 2u, 1024u));
    remoteSettings.sampleRate = settings_.sampleRate;
    remoteSettings.networkBufferFrames = networkBufferFrames;
    remoteSettings.timeoutMs = (settings_.monitorDspPathMode == "external" ||
                                settings_.monitorDspPathMode == "nds" ||
                                settings_.monitorDspPathMode == "remote_external") ? 12 : 8;

    if (remoteMonitorDspStream_.process(remoteSettings, interleavedStereo, remoteDspProcessedBlock_) &&
        remoteDspProcessedBlock_.size() == interleavedStereo.size()) {
        interleavedStereo = remoteDspProcessedBlock_;
        remoteDspMonitorActive_ = true;
        const auto streamStatus = remoteMonitorDspStream_.status();
        remoteDspRoundTripMs_ = streamStatus.averageRoundTripMs;
        remoteDspAverageRoundTripJitterUs_ = streamStatus.averageRoundTripJitterUs;
        remoteDspMaxRoundTripJitterUs_ = streamStatus.maxRoundTripJitterUs;
        message_ = "Monitor audio routed through 누라쿠스트 DSP 서버 async stream (" +
            std::to_string(remoteDspRoundTripMs_) + " ms, buffered " +
            std::to_string(streamStatus.queuedOutputBlocks) + " blocks).";
        return true;
    }

    const auto streamStatus = remoteMonitorDspStream_.status();
    if (streamStatus.running) {
        remoteDspRoundTripMs_ = streamStatus.averageRoundTripMs;
        remoteDspAverageRoundTripJitterUs_ = streamStatus.averageRoundTripJitterUs;
        remoteDspMaxRoundTripJitterUs_ = streamStatus.maxRoundTripJitterUs;
    }
    remoteDspMonitorActive_ = false;
    message_ = "누라쿠스트 DSP 서버 async stream warming/fallback: " + streamStatus.message;
    return false;
}

void NeuracoustDspEngine::recordRemoteDspRoundTripLocked(double roundTripMs) {
    if (!std::isfinite(roundTripMs) || roundTripMs <= 0.0) {
        return;
    }
    remoteDspRoundTripMs_ = roundTripMs;
    if (!remoteDspRoundTripInitialized_) {
        remoteDspPreviousRoundTripMs_ = roundTripMs;
        remoteDspRoundTripInitialized_ = true;
        return;
    }
    const double jitterUs = std::abs(roundTripMs - remoteDspPreviousRoundTripMs_) * 1000.0;
    remoteDspPreviousRoundTripMs_ = roundTripMs;
    remoteDspAverageRoundTripJitterUs_ = remoteDspAverageRoundTripJitterUs_ <= 0.0
        ? jitterUs
        : remoteDspAverageRoundTripJitterUs_ + ((jitterUs - remoteDspAverageRoundTripJitterUs_) * 0.12);
    remoteDspMaxRoundTripJitterUs_ = std::max(jitterUs, remoteDspMaxRoundTripJitterUs_ * 0.995);
}

void NeuracoustDspEngine::resetRemoteDspTelemetryLocked() {
    remoteDspRoundTripMs_ = 0.0;
    remoteDspPreviousRoundTripMs_ = 0.0;
    remoteDspAverageRoundTripJitterUs_ = 0.0;
    remoteDspMaxRoundTripJitterUs_ = 0.0;
    remoteDspRoundTripInitialized_ = false;
}

void NeuracoustDspEngine::updateProjectMonitorPolicyLocked() {
    recordArmedTrackCount_ = 0;
    recordMonitorVolumeDb_ = 0.0f;
    recordMonitorPan_ = 0.0f;
    recordMonitorMuted_ = false;
    const bool soloMode = std::any_of(projectPlan_.tracks.begin(), projectPlan_.tracks.end(), [](const TrackState& track) {
        return track.solo && track.name != "Master" && track.name != "Monitor";
    });
    bool capturedMonitorTrackPolicy = false;
    for (const auto& track : projectPlan_.tracks) {
        if (track.recordArmed) {
            ++recordArmedTrackCount_;
        }
        const bool monitorEligible = trackSupportsPhysicalInputMonitoring(track) &&
            (track.inputMonitoring || (settings_.transportRecordingActive && track.recordArmed));
        if (monitorEligible) {
            if (!capturedMonitorTrackPolicy) {
                recordMonitorVolumeDb_ = track.volumeDb;
                recordMonitorPan_ = track.pan;
                recordMonitorMuted_ = track.muted || (soloMode && !track.solo);
                capturedMonitorTrackPolicy = true;
            }
        }
    }
    lowLatencyRecordMonitoringActive_ = settings_.lowLatencyRecordMonitoringEnabled &&
        capturedMonitorTrackPolicy &&
        settings_.inputMonitorChannelCount > 0;
    // "Listen to source" routes the input (e.g. BlackHole) through the monitor bus even
    // with no record-armed track, so it enables input capture on its own.
    const bool listenSource = listenSourceActive_.load(std::memory_order_relaxed);
    inputMonitorCaptureActive_.store(lowLatencyRecordMonitoringActive_ || listenSource,
                                     std::memory_order_relaxed);
    if (!lowLatencyRecordMonitoringActive_ && !listenSource &&
        !talkbackCaptureActive_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> inputLock(inputMonitorMutex_);
        physicalInputMonitoringActive_ = false;
        inputMonitorBuffer_.clear();
        inputPeak_ = 0.0f;
        inputMonitorChannels_ = 0;
        physicalInputMonitoringActiveForStatus_.store(false, std::memory_order_relaxed);
        inputMonitorChannelsForStatus_.store(0, std::memory_order_relaxed);
        inputPeakForStatus_.store(0.0f, std::memory_order_relaxed);
    }
}

void NeuracoustDspEngine::applyMonitorStationControlsLocked(std::vector<float>& interleavedStereo) {
    const float targetMonitorGain =
        (settings_.monitorStationMute ? 0.0f : 1.0f) *
        (settings_.monitorStationDim ? dbToGain(settings_.monitorStationDimDb) : 1.0f) *
        dbToGain(settings_.monitorVolumeDb);
    if (!monitorStationGainInitialized_) {
        monitorStationGainSmoothed_ = targetMonitorGain;
        monitorStationGainInitialized_ = true;
    }
    const float rampSeconds = settings_.monitorStationMute ? 0.035f : 0.045f;
    const float rampStep = 1.0f / std::max(1.0f, static_cast<float>(settings_.sampleRate) * rampSeconds);
    for (size_t index = 0; index + 1 < interleavedStereo.size(); index += 2) {
        float left = interleavedStereo[index];
        float right = interleavedStereo[index + 1];
        if (settings_.monitorStationListenMode == "M") {
            const float mid = (left + right) * 0.5f;
            left = mid;
            right = mid;
        } else if (settings_.monitorStationListenMode == "S") {
            const float side = (left - right) * 0.5f;
            left = side;
            right = -side;
        } else if (settings_.monitorStationListenMode == "L") {
            if (settings_.monitorStationMono) {
                right = left;
            } else {
                right = 0.0f;
            }
        } else if (settings_.monitorStationListenMode == "R") {
            if (settings_.monitorStationMono) {
                left = right;
            } else {
                left = 0.0f;
            }
        } else if (settings_.monitorStationMono) {
            const float mono = (left + right) * 0.5f;
            left = mono;
            right = mono;
        }
        if (settings_.monitorStationSwapLeftRight) {
            std::swap(left, right);
        }
        if (settings_.monitorStationInvertLeft) {
            left = -left;
        }
        if (settings_.monitorStationInvertRight) {
            right = -right;
        }
        const float delta = targetMonitorGain - monitorStationGainSmoothed_;
        if (std::abs(delta) <= rampStep) {
            monitorStationGainSmoothed_ = targetMonitorGain;
        } else {
            monitorStationGainSmoothed_ += delta > 0.0f ? rampStep : -rampStep;
        }
        interleavedStereo[index] = left * monitorStationGainSmoothed_;
        interleavedStereo[index + 1] = right * monitorStationGainSmoothed_;
    }
}

void NeuracoustDspEngine::mixInputMonitorLocked(int64_t frameCount, std::vector<float>& interleavedStereo) {
    const bool monitorCaptureActive = inputMonitorCaptureActive_.load(std::memory_order_relaxed);
    const bool talkbackActive = talkbackCaptureActive_.load(std::memory_order_relaxed);
    const bool talkbackToMonitor =
        talkbackActive &&
        (settings_.monitorStationTalkbackRoute == "monitor_bus" ||
         settings_.monitorStationTalkbackRoute == "all");
    if ((!monitorCaptureActive && !talkbackToMonitor) || frameCount <= 0 || interleavedStereo.empty()) {
        return;
    }
    std::unique_lock<std::mutex> inputLock(inputMonitorMutex_, std::try_to_lock);
    if (!inputLock.owns_lock()) {
        return;
    }
    const size_t neededSamples = static_cast<size_t>(frameCount) * 2;
    const size_t availableSamples = std::min(neededSamples, inputMonitorBuffer_.size());
    for (size_t index = 0; index + 1 < availableSamples; index += 2) {
        const auto [monitoredLeft, monitoredRight] = talkbackToMonitor
            ? std::pair<float, float>{inputMonitorBuffer_[index], inputMonitorBuffer_[index + 1]}
            : (recordMonitorMuted_
            ? std::pair<float, float>{0.0f, 0.0f}
            : applyStereoGainPan(inputMonitorBuffer_[index],
                                 inputMonitorBuffer_[index + 1],
                                 recordMonitorVolumeDb_,
                                 recordMonitorPan_));
        StereoFrame frame {monitoredLeft, monitoredRight};
        if (settings_.monitorDspEnabled && !remoteMonitorDspRequestedLocked()) {
            frame = monitorProcessor_.process(frame);
        }
        interleavedStereo[index] += frame.left;
        interleavedStereo[index + 1] += frame.right;
    }
    if (availableSamples > 0) {
        inputMonitorBuffer_.erase(inputMonitorBuffer_.begin(), inputMonitorBuffer_.begin() + static_cast<std::ptrdiff_t>(availableSamples));
    }
    inputPeak_ *= 0.96f;
    if (inputMonitorBuffer_.empty() && inputPeak_ < 0.0001f) {
        inputPeak_ = 0.0f;
    }
    inputPeakForStatus_.store(inputPeak_, std::memory_order_relaxed);
    inputMonitorChannelsForStatus_.store(inputMonitorChannels_, std::memory_order_relaxed);
}

void NeuracoustDspEngine::resetTrackMetersLocked() {
    projectMeters_ = {};
    clearTrackInsertMetersLocked();
    for (const auto& track : projectPlan_.tracks) {
        if (track.trackType == "master" || track.trackType == "monitor" || track.name == "Master" || track.name == "Monitor") {
            continue;
        }
        projectMeters_.trackNames.push_back(track.name);
        projectMeters_.trackPeakLeft.push_back(0.0f);
        projectMeters_.trackPeakRight.push_back(0.0f);
    }
}

void NeuracoustDspEngine::clearTrackInsertMetersLocked() {
    trackInsertMeterTrackNames_.clear();
    trackInsertMeterSlotIndices_.clear();
    trackInsertInputPeaks_.clear();
    trackInsertOutputPeaks_.clear();
    trackInsertOutputParameterTrackNames_.clear();
    trackInsertOutputParameterSlotIndices_.clear();
    trackInsertOutputParameterIds_.clear();
    trackInsertOutputParameterValues_.clear();
}

void NeuracoustDspEngine::suppressTrackInsertMetersForGraphChangeLocked() {
    clearTrackInsertMetersLocked();
    const double sampleRate = std::max(1.0, sampleRateForStatus_.load(std::memory_order_relaxed));
    trackInsertMeterSuppressSamples_ = std::max<int64_t>(
        static_cast<int64_t>(std::max(1, maxBlockSize_) * 4),
        static_cast<int64_t>(std::round(sampleRate * 0.050)));
}

void NeuracoustDspEngine::storeTrackInsertMeterLocked(const std::string& trackName,
                                                     int slotIndex,
                                                     float inputPeak,
                                                     float outputPeak) {
    if (trackName.empty() || slotIndex < 0) {
        return;
    }
    inputPeak = std::min(1.0f, std::max(0.0f, inputPeak));
    outputPeak = std::min(1.0f, std::max(0.0f, outputPeak));
    for (size_t index = 0; index < trackInsertMeterTrackNames_.size(); ++index) {
        if (trackInsertMeterTrackNames_[index] == trackName &&
            index < trackInsertMeterSlotIndices_.size() &&
            trackInsertMeterSlotIndices_[index] == slotIndex) {
            const float inputHold = index < trackInsertInputPeaks_.size() ? trackInsertInputPeaks_[index] * 0.82f : 0.0f;
            const float outputHold = index < trackInsertOutputPeaks_.size() ? trackInsertOutputPeaks_[index] * 0.82f : 0.0f;
            if (index < trackInsertInputPeaks_.size()) {
                trackInsertInputPeaks_[index] = std::max(inputPeak, inputHold);
            }
            if (index < trackInsertOutputPeaks_.size()) {
                trackInsertOutputPeaks_[index] = std::max(outputPeak, outputHold);
            }
            return;
        }
    }
    trackInsertMeterTrackNames_.push_back(trackName);
    trackInsertMeterSlotIndices_.push_back(slotIndex);
    trackInsertInputPeaks_.push_back(inputPeak);
    trackInsertOutputPeaks_.push_back(outputPeak);
}

void NeuracoustDspEngine::storeTrackInsertOutputParametersLocked(const std::string& trackName,
                                                                 int slotIndex,
                                                                 const std::vector<Vst3ParameterValueState>& parameters) {
    if (trackName.empty() || slotIndex < 0 || parameters.empty()) {
        return;
    }
    constexpr size_t kMaxOutputParametersPerBlock = 128;
    size_t stored = 0;
    for (const auto& parameter : parameters) {
        if (stored >= kMaxOutputParametersPerBlock) {
            break;
        }
        if (!std::isfinite(parameter.normalizedValue)) {
            continue;
        }
        trackInsertOutputParameterTrackNames_.push_back(trackName);
        trackInsertOutputParameterSlotIndices_.push_back(slotIndex);
        trackInsertOutputParameterIds_.push_back(parameter.parameterId);
        trackInsertOutputParameterValues_.push_back(static_cast<float>(std::clamp(parameter.normalizedValue, 0.0, 1.0)));
        ++stored;
    }
}

void NeuracoustDspEngine::applyReloadCrossfadeLocked(std::vector<float>& interleavedStereo) {
    if (interleavedStereo.empty()) {
        previousOutputBlock_.clear();
        reloadCrossfadeSamplesRemaining_ = 0;
        return;
    }
    const int64_t frameCount = static_cast<int64_t>(interleavedStereo.size() / 2);
    if (reloadCrossfadeSamplesRemaining_ > 0 &&
        reloadCrossfadeSamplesTotal_ > 0 &&
        previousOutputBlock_.size() == interleavedStereo.size()) {
        for (int64_t frame = 0; frame < frameCount; ++frame) {
            const double progress = 1.0 - (static_cast<double>(reloadCrossfadeSamplesRemaining_) /
                                          static_cast<double>(reloadCrossfadeSamplesTotal_));
            const float wet = static_cast<float>(std::max(0.0, std::min(1.0, progress)));
            const float dry = 1.0f - wet;
            const auto index = static_cast<size_t>(frame) * 2;
            interleavedStereo[index] = previousOutputBlock_[index] * dry + interleavedStereo[index] * wet;
            interleavedStereo[index + 1] = previousOutputBlock_[index + 1] * dry + interleavedStereo[index + 1] * wet;
            if (reloadCrossfadeSamplesRemaining_ > 0) {
                --reloadCrossfadeSamplesRemaining_;
            }
        }
    } else if (reloadCrossfadeSamplesRemaining_ > 0) {
        reloadCrossfadeSamplesRemaining_ = 0;
    }
    previousOutputBlock_ = interleavedStereo;
}

void NeuracoustDspEngine::armSeekRampLocked(double sampleRate) {
    const double safeSampleRate = std::max(1.0, sampleRate);
    seekRampSamplesTotal_ = std::max<int64_t>(1, static_cast<int64_t>(std::round(safeSampleRate * 0.006)));
    seekRampSamplesRemaining_ = seekRampSamplesTotal_;
}

void NeuracoustDspEngine::applySeekRampLocked(std::vector<float>& interleavedStereo) {
    if (seekRampSamplesRemaining_ <= 0 || seekRampSamplesTotal_ <= 0 || interleavedStereo.empty()) {
        return;
    }
    const int64_t frameCount = static_cast<int64_t>(interleavedStereo.size() / 2);
    for (int64_t frame = 0; frame < frameCount && seekRampSamplesRemaining_ > 0; ++frame) {
        const int64_t elapsed = seekRampSamplesTotal_ - seekRampSamplesRemaining_;
        const float gain = static_cast<float>(std::max(0.0, std::min(1.0, static_cast<double>(elapsed) / static_cast<double>(seekRampSamplesTotal_))));
        const auto index = static_cast<size_t>(frame) * 2u;
        interleavedStereo[index] *= gain;
        interleavedStereo[index + 1u] *= gain;
        --seekRampSamplesRemaining_;
    }
}

void NeuracoustDspEngine::storeTrackMetersLocked(const ProjectAudioBlockMeters& meters) {
    projectMeters_ = meters;
    for (auto& peak : projectMeters_.trackPeakLeft) {
        peak = std::min(1.0f, std::max(0.0f, peak));
    }
    for (auto& peak : projectMeters_.trackPeakRight) {
        peak = std::min(1.0f, std::max(0.0f, peak));
    }
    for (size_t index = 0; index < meters.trackInsertMeterTrackNames.size(); ++index) {
        if (index >= meters.trackInsertMeterSlotIndices.size()) {
            continue;
        }
        const float inputPeak = index < meters.trackInsertInputPeak.size()
            ? meters.trackInsertInputPeak[index]
            : 0.0f;
        const float outputPeak = index < meters.trackInsertOutputPeak.size()
            ? meters.trackInsertOutputPeak[index]
            : 0.0f;
        storeTrackInsertMeterLocked(meters.trackInsertMeterTrackNames[index],
                                   meters.trackInsertMeterSlotIndices[index],
                                   inputPeak,
                                   outputPeak);
    }
}

namespace {

// FFT window: kSpectrumFftSize points, kSpectrumFftSize/2 usable magnitude bins.
constexpr int kSpectrumFftSize = NeuracoustDspEngine::kSpectrumBins * 2;

// In-place iterative radix-2 Cooley-Tukey FFT. Portable (no Accelerate) so the
// metering path stays cross-platform. `re`/`im` are length n (a power of two).
void radix2Fft(std::vector<float>& re, std::vector<float>& im) {
    const size_t n = re.size();
    if (n < 2) return;
    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const float wReStep = static_cast<float>(std::cos(ang));
        const float wImStep = static_cast<float>(std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            float wRe = 1.0f;
            float wIm = 0.0f;
            for (size_t k = 0; k < len / 2; ++k) {
                const float uRe = re[i + k];
                const float uIm = im[i + k];
                const float vRe = re[i + k + len / 2] * wRe - im[i + k + len / 2] * wIm;
                const float vIm = re[i + k + len / 2] * wIm + im[i + k + len / 2] * wRe;
                re[i + k] = uRe + vRe;
                im[i + k] = uIm + vIm;
                re[i + k + len / 2] = uRe - vRe;
                im[i + k + len / 2] = uIm - vIm;
                const float nextWRe = wRe * wReStep - wIm * wImStep;
                wIm = wRe * wImStep + wIm * wReStep;
                wRe = nextWRe;
            }
        }
    }
}

} // namespace

int NeuracoustDspEngine::spectrumBinCount() const {
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    return static_cast<int>(spectrumBins_.size());
}

void NeuracoustDspEngine::copySpectrumBins(float* out, int count) const {
    if (out == nullptr || count <= 0) return;
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    const int available = std::min(count, static_cast<int>(spectrumBins_.size()));
    for (int i = 0; i < available; ++i) out[i] = spectrumBins_[static_cast<size_t>(i)];
    for (int i = available; i < count; ++i) out[i] = 0.0f;
}

// Accumulates mono output into a window; when full, runs a Hann-windowed FFT and
// publishes normalized magnitude bins. Called from storeMetering on the render thread.
void NeuracoustDspEngine::updateSpectrum(const std::vector<float>& interleavedStereo) {
    spectrumAccumulator_.reserve(static_cast<size_t>(kSpectrumFftSize));
    for (size_t index = 0; index + 1 < interleavedStereo.size(); index += 2) {
        spectrumAccumulator_.push_back((interleavedStereo[index] + interleavedStereo[index + 1]) * 0.5f);
    }
    // A huge block could hold several windows; a tiny one fills over many calls. Bound
    // the backlog so a pathological block can't make this loop forever.
    if (spectrumAccumulator_.size() > static_cast<size_t>(kSpectrumFftSize) * 4) {
        spectrumAccumulator_.erase(spectrumAccumulator_.begin(),
                                   spectrumAccumulator_.end() - static_cast<std::ptrdiff_t>(kSpectrumFftSize));
    }
    if (static_cast<int>(spectrumAccumulator_.size()) < kSpectrumFftSize) {
        return;
    }

    std::vector<float> re(static_cast<size_t>(kSpectrumFftSize));
    std::vector<float> im(static_cast<size_t>(kSpectrumFftSize), 0.0f);
    for (int i = 0; i < kSpectrumFftSize; ++i) {
        // Hann window.
        const float w = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (kSpectrumFftSize - 1)));
        re[static_cast<size_t>(i)] = spectrumAccumulator_[static_cast<size_t>(i)] * w;
    }
    // Keep the most recent samples for the next window (50% overlap keeps it lively).
    spectrumAccumulator_.erase(spectrumAccumulator_.begin(),
                               spectrumAccumulator_.begin() + kSpectrumFftSize / 2);

    radix2Fft(re, im);

    const int bins = kSpectrumFftSize / 2;
    // 2/N normalization, then map to 0..1 on a dB scale (-90..0 dB) the UI can draw.
    std::unique_lock<std::mutex> lock(spectrumMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    spectrumBins_.assign(static_cast<size_t>(bins), 0.0f);
    for (int i = 0; i < bins; ++i) {
        const float mag = std::sqrt(re[static_cast<size_t>(i)] * re[static_cast<size_t>(i)] +
                                    im[static_cast<size_t>(i)] * im[static_cast<size_t>(i)]) * (2.0f / kSpectrumFftSize);
        const float db = 20.0f * std::log10(std::max(1e-6f, mag));
        spectrumBins_[static_cast<size_t>(i)] = std::min(1.0f, std::max(0.0f, (db + 90.0f) / 90.0f));
    }
}

int NeuracoustDspEngine::runSpectrumSelfTest() {
    // 1 kHz tone at 48 kHz: expected FFT bin = f * N / SR = 1000 * 2048 / 48000 ≈ 43.
    const double sampleRate = 48000.0;
    const double toneHz = 1000.0;
    const int expectedBin = static_cast<int>(std::lround(toneHz * kSpectrumFftSize / sampleRate));

    NeuracoustDspEngine dsp;
    std::vector<float> block(static_cast<size_t>(kSpectrumFftSize) * 2);  // interleaved stereo
    // Feed several windows so the overlap settles.
    for (int pass = 0; pass < 6; ++pass) {
        for (int frame = 0; frame < kSpectrumFftSize; ++frame) {
            const double t = static_cast<double>(pass * kSpectrumFftSize + frame) / sampleRate;
            const float s = static_cast<float>(0.5 * std::sin(2.0 * M_PI * toneHz * t));
            block[static_cast<size_t>(frame) * 2] = s;
            block[static_cast<size_t>(frame) * 2 + 1] = s;
        }
        dsp.updateSpectrum(block);
    }

    std::vector<float> bins(static_cast<size_t>(NeuracoustDspEngine::kSpectrumBins), 0.0f);
    dsp.copySpectrumBins(bins.data(), static_cast<int>(bins.size()));

    int peakBin = 0;
    float peak = 0.0f;
    for (int i = 1; i < static_cast<int>(bins.size()); ++i) {
        if (bins[static_cast<size_t>(i)] > peak) { peak = bins[static_cast<size_t>(i)]; peakBin = i; }
    }
    if (peak <= 0.0f) {
        std::cerr << "SPECTRUM_SELF_TEST failed: no energy in any bin\n";
        return 51;
    }
    if (std::abs(peakBin - expectedBin) > 2) {
        std::cerr << "SPECTRUM_SELF_TEST failed: peak at bin " << peakBin
                  << ", expected near " << expectedBin << "\n";
        return 52;
    }
    std::cout << "SPECTRUM_SELF_TEST ok (peak bin " << peakBin << ")\n";
    return 0;
}

void NeuracoustDspEngine::storeMetering(const std::vector<float>& interleavedStereo) {
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    double crossEnergy = 0.0;
    double lowEnergy = 0.0;
    double midEnergy = 0.0;
    double highEnergy = 0.0;

    for (size_t index = 0; index + 1 < interleavedStereo.size(); index += 2) {
        const float left = interleavedStereo[index];
        const float right = interleavedStereo[index + 1];
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
    }

    const double frames = std::max<double>(1.0, static_cast<double>(interleavedStereo.size() / 2));
    const double correlationDenominator = std::sqrt(leftEnergy * rightEnergy);
    const float correlation = correlationDenominator > 0.000000001
        ? static_cast<float>(std::max(-1.0, std::min(1.0, crossEnergy / correlationDenominator)))
        : 0.0f;
    outputPeakLeft_.store(std::min(1.0f, peakLeft));
    outputPeakRight_.store(std::min(1.0f, peakRight));
    phaseCorrelation_.store(correlation);
    spectrumLow_.store(std::min(1.0f, static_cast<float>(std::sqrt(lowEnergy / frames))));
    spectrumMid_.store(std::min(1.0f, static_cast<float>(std::sqrt(midEnergy / frames))));
    spectrumHigh_.store(std::min(1.0f, static_cast<float>(std::sqrt(highEnergy / frames))));

    updateSpectrum(interleavedStereo);

    // Loudness (BS.1770). Re-prepare the filter when the sample rate changes.
    const double sr = sampleRateForStatus_.load(std::memory_order_relaxed);
    if (sr > 0.0 && sr != loudnessSampleRate_) {
        loudnessMeter_.prepare(sr);
        loudnessSampleRate_ = sr;
    }
    if (loudnessSampleRate_ > 0.0 && !interleavedStereo.empty()) {
        loudnessMeter_.process(interleavedStereo.data(),
                               static_cast<int64_t>(interleavedStereo.size() / 2), 2);
    }

    // Goniometer: subsample the block's L/R pairs to a fixed point count.
    const int64_t pairCount = static_cast<int64_t>(interleavedStereo.size() / 2);
    if (pairCount > 0) {
        std::lock_guard<std::mutex> lock(spectrumMutex_);
        goniometerSamples_.assign(static_cast<size_t>(kGoniometerPoints) * 2, 0.0f);
        for (int i = 0; i < kGoniometerPoints; ++i) {
            const int64_t frame = pairCount == 1 ? 0
                : (static_cast<int64_t>(i) * (pairCount - 1)) / (kGoniometerPoints - 1);
            goniometerSamples_[static_cast<size_t>(i) * 2] = interleavedStereo[static_cast<size_t>(frame) * 2];
            goniometerSamples_[static_cast<size_t>(i) * 2 + 1] = interleavedStereo[static_cast<size_t>(frame) * 2 + 1];
        }
    }
}

void NeuracoustDspEngine::publishListenRoomLocked(const std::vector<float>& interleavedStereo) {
    if (!settings_.listenRoom.enabled || interleavedStereo.empty()) {
        return;
    }
    listenRoomSender_.pushInterleavedStereo(interleavedStereo.data(), static_cast<int64_t>(interleavedStereo.size() / 2u));
}

void NeuracoustDspEngine::resetMeteringLocked() {
    outputPeakLeft_.store(0.0f);
    outputPeakRight_.store(0.0f);
    phaseCorrelation_.store(0.0f);
    spectrumLow_.store(0.0f);
    spectrumMid_.store(0.0f);
    spectrumHigh_.store(0.0f);
    lowBandState_ = 0.0f;
    midBandState_ = 0.0f;
    {
        std::lock_guard<std::mutex> lock(spectrumMutex_);
        spectrumAccumulator_.clear();
        std::fill(spectrumBins_.begin(), spectrumBins_.end(), 0.0f);
        std::fill(goniometerSamples_.begin(), goniometerSamples_.end(), 0.0f);
    }
    loudnessMeter_.reset();
}

} // namespace neuracoust::daw
