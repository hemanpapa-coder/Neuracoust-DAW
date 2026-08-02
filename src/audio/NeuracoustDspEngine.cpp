#include "audio/NeuracoustDspEngine.h"
#include <cstdlib>
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
#include <thread>
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

// The crossing cost now lives in RemoteDspServerClient so the mixer's delay compensation and the
// stream that pays the cost read the same number. Two copies of it is how a channel ends up
// compensated by a different amount than it is actually delayed.
unsigned int estimatedRemoteDspLatencySamples(const RemoteDspServerSettings& settings,
                                              double /*sampleRate*/,
                                              int maxBlockSize) {
    return remoteDspCrossingLatencySamples(settings, maxBlockSize);
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

// Full equality of the monitor DSP module chain, including the active slot. Used to skip
// reconfiguring the monitor processors (which resets their filter state — an audible click)
// on an edit that did not touch the monitor at all.
bool monitorModulesEqual(const std::vector<MonitorDspModule>& left,
                         const std::vector<MonitorDspModule>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.id != b.id ||
            a.displayName != b.displayName ||
            a.stage != b.stage ||
            a.enabled != b.enabled ||
            a.activeTargetSlot != b.activeTargetSlot ||
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
    return true;
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
    const double startSeconds = std::max(0.0, plan.loopStartSeconds - plan.preRollSeconds);
    const double endSeconds = plan.loopEndSeconds + plan.postRollSeconds;
    const int64_t loopStart = std::max<int64_t>(0, static_cast<int64_t>(std::round(startSeconds * plan.sampleRate)));
    const int64_t loopEnd = std::max<int64_t>(0, static_cast<int64_t>(std::round(endSeconds * plan.sampleRate)));
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
    const double endSeconds = plan.loopEndSeconds + plan.postRollSeconds;
    return std::max<int64_t>(0, static_cast<int64_t>(std::round(endSeconds * plan.sampleRate)));
}

void mergeBlockMeters(ProjectAudioBlockMeters& destination, const ProjectAudioBlockMeters& source) {
    for (size_t index = 0; index < source.trackNames.size(); ++index) {
        const auto& name = source.trackNames[index];
        auto found = std::find(destination.trackNames.begin(), destination.trackNames.end(), name);
        if (found == destination.trackNames.end()) {
            destination.trackNames.push_back(name);
            destination.trackPeakLeft.push_back(index < source.trackPeakLeft.size() ? source.trackPeakLeft[index] : 0.0f);
            destination.trackPeakRight.push_back(index < source.trackPeakRight.size() ? source.trackPeakRight[index] : 0.0f);
            // The console-channel arrays are part of the same parallel set: the bridge takes the
            // MIN over all of them, so leaving these short reports ZERO meters for every track.
            destination.trackConsoleGainReductionDb.push_back(
                index < source.trackConsoleGainReductionDb.size() ? source.trackConsoleGainReductionDb[index] : 0.0f);
            destination.trackConsoleGateGainReductionDb.push_back(
                index < source.trackConsoleGateGainReductionDb.size() ? source.trackConsoleGateGainReductionDb[index] : 0.0f);
            continue;
        }
        const size_t destinationIndex = static_cast<size_t>(std::distance(destination.trackNames.begin(), found));
        if (destinationIndex < destination.trackPeakLeft.size() && index < source.trackPeakLeft.size()) {
            destination.trackPeakLeft[destinationIndex] = std::max(destination.trackPeakLeft[destinationIndex], source.trackPeakLeft[index]);
        }
        if (destinationIndex < destination.trackPeakRight.size() && index < source.trackPeakRight.size()) {
            destination.trackPeakRight[destinationIndex] = std::max(destination.trackPeakRight[destinationIndex], source.trackPeakRight[index]);
        }
        // Gain reduction has to merge too. Only the peaks did, so when a block rendered in more
        // than one segment the GR kept whatever the FIRST segment saw — reading ~0 while the
        // compressor was audibly working.
        if (destinationIndex < destination.trackConsoleGainReductionDb.size() &&
            index < source.trackConsoleGainReductionDb.size()) {
            destination.trackConsoleGainReductionDb[destinationIndex] =
                std::max(destination.trackConsoleGainReductionDb[destinationIndex],
                         source.trackConsoleGainReductionDb[index]);
        }
        if (destinationIndex < destination.trackConsoleGateGainReductionDb.size() &&
            index < source.trackConsoleGateGainReductionDb.size()) {
            destination.trackConsoleGateGainReductionDb[destinationIndex] =
                std::max(destination.trackConsoleGateGainReductionDb[destinationIndex],
                         source.trackConsoleGateGainReductionDb[index]);
        }
    }
    destination.masterPeakLeft = std::max(destination.masterPeakLeft, source.masterPeakLeft);
    destination.masterPeakRight = std::max(destination.masterPeakRight, source.masterPeakRight);
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
    // The DSP assignment is a prepare-time input like everything else here: the offload lists
    // (remote console strips, remote master stages, per-machine insert chains) are derived when
    // the chains are prepared. Left out of the signature, a role or machine change matched the
    // old signature, the prepare was skipped as a no-op, and the new assignment sat dormant
    // until some unrelated edit happened to rebuild — the row looked live and did nothing.
    const auto& remote = settings.remoteDspServer;
    out << "remote=" << (remote.enabled ? '1' : '0') << remote.host
        << ";nds=" << (remote.ndsEnabled ? '1' : '0') << remote.ndsHost
        << ";roles=" << remote.roleMonitor << ',' << remote.roleChannelStrip << ','
        << remote.roleMaster << ',' << remote.roleInserts << ',' << remote.roleMixer
        << ";overflow=" << (remote.autoOverflow ? '1' : '0') << '\n';
    for (const auto& track : plan.tracks) {
        const auto& c = track.consoleChannel;
        const bool anyModuleOn = c.filterEnabled || c.eqEnabled || c.compEnabled ||
                                 c.gateEnabled || c.saturatorEnabled;
        if (anyModuleOn || !track.consoleDspMachine.empty()) {
            out << "strip:" << track.name << '=' << (anyModuleOn ? '1' : '0')
                << ',' << track.consoleDspMachine << '\n';
        }
    }
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

// Public face of the anonymous-namespace mapping above, so the offline bounce can pack the same
// parameters the realtime engine does. One mapping — the 4001E id table lives here only.
std::vector<RemoteDspParameterValue> remoteInsertParameterValues(const TrackInsertSlot& insert) {
    return remoteParametersForInsert(insert);
}

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
        talkbackMonoBuffer_.clear();
        inputPeak_ = 0.0f;
        inputPeakLeft_ = 0.0f;
        inputPeakRight_ = 0.0f;
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

void NeuracoustDspEngine::setMeasurementLevelCheck(bool on) {
    if (on == measurementLevelCheck_.load(std::memory_order_relaxed)) return;
    if (on) {
        // Force input capture so the ADC channel meters; remember the prior state to restore.
        measurementPrevInputMonitor_ = inputMonitorCaptureActive_.load(std::memory_order_relaxed);
        inputMonitorCaptureActive_.store(true, std::memory_order_relaxed);
        measurementLevelCheck_.store(true, std::memory_order_relaxed);
    } else {
        measurementLevelCheck_.store(false, std::memory_order_relaxed);
        measurementInputPeak_.store(0.0f, std::memory_order_relaxed);
        // Only release capture if a measurement sweep is not itself relying on it.
        if (!measurementActive_.load(std::memory_order_relaxed)) {
            inputMonitorCaptureActive_.store(measurementPrevInputMonitor_, std::memory_order_relaxed);
        }
    }
}

void NeuracoustDspEngine::startMeasurement(int channel, std::vector<float> signal) {
    {
        std::lock_guard<std::mutex> lock(measurementMutex_);
        measurementCapture_.clear();
        measurementCapture_.reserve(signal.size() + static_cast<size_t>(settings_.sampleRate));
    }
    measurementChannel_ = (channel == 1) ? 1 : 0;
    measurementSignal_ = std::move(signal);
    measurementEmitPos_ = 0;
    measurementSweepPeak_.store(0.0f, std::memory_order_relaxed);   // fresh clip check for this sweep
    measurementTotalFrames_.store(static_cast<int64_t>(measurementSignal_.size()), std::memory_order_relaxed);
    measurementProgressFrames_.store(0, std::memory_order_relaxed);
    // The mic only reaches pushInputMonitorInterleaved while input capture is on; remember the
    // TRUE prior state (before ANY forcer) so restore never leaves it stuck on. Only the first of
    // {measurement, level-check} to force it saves the snapshot — otherwise measuring while level
    // check is on would snapshot the forced "true" and leave the mic monitoring afterwards.
    if (!measurementLevelCheck_.load(std::memory_order_relaxed)) {
        measurementPrevInputMonitor_ = inputMonitorCaptureActive_.load(std::memory_order_relaxed);
    }
    inputMonitorCaptureActive_.store(true, std::memory_order_relaxed);
    measurementActive_.store(true, std::memory_order_relaxed);
}

void NeuracoustDspEngine::cancelMeasurement() {
    measurementActive_.store(false, std::memory_order_relaxed);
    if (!measurementLevelCheck_.load(std::memory_order_relaxed)) {
        inputMonitorCaptureActive_.store(measurementPrevInputMonitor_, std::memory_order_relaxed);
    }
}

double NeuracoustDspEngine::measurementProgress() const {
    const int64_t total = measurementTotalFrames_.load(std::memory_order_relaxed);
    if (total <= 0) return 0.0;
    return std::min(1.0, static_cast<double>(measurementProgressFrames_.load(std::memory_order_relaxed)) /
                             static_cast<double>(total));
}

std::vector<float> NeuracoustDspEngine::takeMeasurementCapture() {
    measurementActive_.store(false, std::memory_order_relaxed);
    inputMonitorCaptureActive_.store(measurementPrevInputMonitor_, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(measurementMutex_);
    return std::move(measurementCapture_);
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
                                             const std::string& metronomeSubdivision,
                                             double metronomeGain,
                                             const std::string& metronomeSound,
                                             bool metronomeAccentFirst) {
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
    settings_.metronomeGain = std::max(0.0, std::min(2.0, metronomeGain));
    settings_.metronomeSound =
        (metronomeSound == "wood" || metronomeSound == "rim" || metronomeSound == "cowbell")
            ? metronomeSound
            : "beep";
    settings_.metronomeAccentFirst = metronomeAccentFirst;
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

void NeuracoustDspEngine::setMetronomeAccentPattern(const std::vector<float>& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.metronomeAccentPattern = pattern;
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
        talkbackMonoBuffer_.clear();
        inputPeak_ = 0.0f;
        inputPeakLeft_ = 0.0f;
        inputPeakRight_ = 0.0f;
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
    settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, project.monitorVolumeDb));  // +12 dB monitor ceiling, matching setMonitorVolumeDb
    settings_.monitorModules = project.monitorModules.empty() ? defaultMonitorDspModules() : project.monitorModules;
    projectPlan_.monitorModules = settings_.monitorModules;
    monitorProcessor_.configure(std::max(1.0, projectSampleRate), settings_.monitorModules);
    previousMonitorProcessor_ = monitorProcessor_;
    monitorDspModuleTransitionSamplesRemaining_ = 0;
    monitorDspModuleTransitionSamplesTotal_ = 0;
    configureMonitorEqLocked(project, projectSampleRate);
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
    // Render-state refresh moved below (conditional resetForSeek before the configured_
    // block): a plain edit preserves the keyed plug-in processors and the monitor DSP
    // filter state instead of tearing everything down — that teardown clicked the monitor
    // and gapped plug-ins on every clip move/trim/split/fade mid-playback.
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
    settings_.monitorVolumeDb = std::max(-120.0f, std::min(12.0f, project.monitorVolumeDb));  // +12 dB monitor ceiling, matching setMonitorVolumeDb
    const auto nextMonitorModules = project.monitorModules.empty() ? defaultMonitorDspModules() : project.monitorModules;
    // A clip edit never touches the monitor chain (that path is setMonitorDspModules, which
    // crossfades). Reconfiguring the monitor processor resets its filter state — a click on
    // every edit — so only do it when the modules actually changed.
    const bool monitorModulesChanged = !monitorModulesEqual(settings_.monitorModules, nextMonitorModules);
    settings_.monitorModules = nextMonitorModules;
    projectPlan_.monitorModules = settings_.monitorModules;
    if (monitorModulesChanged) {
        monitorProcessor_.configure(std::max(1.0, projectSampleRate), settings_.monitorModules);
        previousMonitorProcessor_ = monitorProcessor_;
        monitorDspModuleTransitionSamplesRemaining_ = 0;
        monitorDspModuleTransitionSamplesTotal_ = 0;
    }
    configureMonitorEqLocked(project, projectSampleRate);
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
    // Preserve the keyed plug-in processors and the monitor / master-insert DSP across an edit.
    // resetForEdit (NOT resetForSeek) keeps the continuous-playback buffers — route delay lines and
    // generator phases — so editing a clip mid-playback (move/trim/split/delete, and edits while
    // recording) no longer clicks or gaps the sound. Only a real seek flushes those. The flag-gated
    // master-insert chain and monitor DSP still rebuild only when their content actually changed.
    const std::string nextInsertGraphSignature = configured_
        ? realtimeInsertGraphSignature(projectPlan_, settings_, maxBlockSize_)
        : std::string();
    const bool insertGraphChanged = configured_ && nextInsertGraphSignature != realtimeInsertGraphSignature_;
    projectRenderState_.resetForEdit();
    if (insertGraphChanged) {
        projectRenderState_.masterInsertChainPrepared = false;
        projectRenderState_.masterInsertChain.reset();
    }
    if (monitorModulesChanged) {
        projectRenderState_.monitorDspConfigured = false;
    }
    if (configured_) {
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

bool NeuracoustDspEngine::updateClipStart(const std::string& clipId, double startSeconds) {
    if (clipId.empty() || !std::isfinite(startSeconds)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto clipIt = std::find_if(projectPlan_.clips.begin(), projectPlan_.clips.end(), [&](const ProjectRenderClip& renderClip) {
        return renderClip.clip.id == clipId;
    });
    if (clipIt == projectPlan_.clips.end()) {
        return false;
    }
    // The render maps the playhead to the source via clip.startSeconds directly, so patching it in
    // place slides the clip during a drag with NO plan rebuild — the rebuild (per drag frame) is what
    // stopped/gapped the music while moving a clip.
    clipIt->clip.startSeconds = std::max(0.0, startSeconds);
    message_ = "Updated clip start without reloading project.";
    return true;
}

bool NeuracoustDspEngine::updateClipBounds(const std::string& clipId, double startSeconds,
                                           double durationSeconds, double sourceOffsetSeconds) {
    if (clipId.empty() || !std::isfinite(startSeconds) || !std::isfinite(durationSeconds) ||
        !std::isfinite(sourceOffsetSeconds)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto clipIt = std::find_if(projectPlan_.clips.begin(), projectPlan_.clips.end(), [&](const ProjectRenderClip& renderClip) {
        return renderClip.clip.id == clipId;
    });
    if (clipIt == projectPlan_.clips.end()) {
        return false;
    }
    // Trim maps to the source through start + sourceOffset together (a start trim slides both so the
    // retained audio keeps playing at the same timeline position); duration bounds the read. Patching
    // all three in place makes the render honour the new clip length immediately, with no plan rebuild.
    clipIt->clip.startSeconds = std::max(0.0, startSeconds);
    clipIt->clip.durationSeconds = std::max(0.0, durationSeconds);
    clipIt->clip.sourceOffsetSeconds = std::max(0.0, sourceOffsetSeconds);
    message_ = "Updated clip bounds without reloading project.";
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

bool NeuracoustDspEngine::updateTrackConsoleChannel(const std::string& trackName,
                                                    const ConsoleChannelState& console) {
    if (trackName.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(),
                                [&](const TrackState& track) { return track.name == trackName; });
    if (trackIt == projectPlan_.tracks.end()) return false;
    // The ConsoleChannelProcessor (kept in the render state, keyed by route) ramps its coefficients
    // toward these params per sample, so pushing them here is click-free.
    trackIt->consoleChannel = console;
    // Strip membership is the ASSIGNMENT (machine/role), never the module lamps — an idle
    // assigned strip stays streaming as a passthrough, so a lamp toggle is only a parameter
    // refresh and the node's own engage crossfade handles the rest. Rebuilding membership here
    // (the old behaviour) made every first-lamp toggle a local↔remote handoff, which clicked.
    if (auto strip = remoteConsoleStrips_.find(trackName); strip != remoteConsoleStrips_.end()) {
        strip->second.parameters.clear();
        for (const auto& parameter : consoleChannelParameterValues(console)) {
            strip->second.parameters.push_back({static_cast<uint32_t>(parameter.index), parameter.normalized});
        }
    }
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
        if (routeChainIt != projectRenderState_.routeInsertChains.end() && routeChainIt->second) {
            updatedChain = routeChainIt->second->updateParameter(*routeInsertIndex,
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

bool NeuracoustDspEngine::updateInstrumentVst3Parameter(const std::string& trackName,
                                                        size_t slotIndex,
                                                        uint32_t parameterId,
                                                        const std::string& displayName,
                                                        double normalizedValue) {
    if (trackName.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(),
                                [&](const TrackState& track) { return track.name == trackName; });
    if (trackIt == projectPlan_.tracks.end()) {
        return false;
    }
    // The renderer builds its per-block instrument list from THIS plan track each block — a copy of
    // track.instrumentSlots, or track.instrument when the rack is empty — and hands instrument.parameters
    // straight to processMidiInstrument. Patching them here is heard on the next block with no reconcile.
    // Keep the legacy `instrument` mirror in step with slot 0, exactly as the render fallback expects.
    bool updated = false;
    if (slotIndex < trackIt->instrumentSlots.size()) {
        upsertVst3ParameterState(trackIt->instrumentSlots[slotIndex].parameters,
                                 parameterId, displayName, normalizedValue);
        updated = true;
    }
    if (slotIndex == 0) {
        upsertVst3ParameterState(trackIt->instrument.parameters, parameterId, displayName, normalizedValue);
        updated = true;
    }
    if (!updated) {
        return false;
    }
    message_ = "Updated instrument VST3 parameter without reconciling.";
    return true;
}

bool NeuracoustDspEngine::updateInstrumentComponentState(const std::string& trackName,
                                                         size_t slotIndex,
                                                         const std::string& componentStateBase64) {
    if (trackName.empty() || componentStateBase64.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Patch the render PLAN first. The renderer reads instrument.pluginStateBase64 from the plan
    // when it prepares an instrument, and while an editor is open the render instance for that track
    // is dormant/torn down — so on editor close it is (re)prepared fresh, and it must read the NEW
    // patch, not the one the plan was loaded with. Updating only the live processor missed this:
    // the fresh prepare rebuilt the old preset, which is the "reverts on close" glitch returning.
    auto trackIt = std::find_if(projectPlan_.tracks.begin(), projectPlan_.tracks.end(),
                                [&](const TrackState& track) { return track.name == trackName; });
    if (trackIt != projectPlan_.tracks.end()) {
        if (slotIndex < trackIt->instrumentSlots.size()) {
            trackIt->instrumentSlots[slotIndex].pluginStateBase64 = componentStateBase64;
        }
        if (slotIndex == 0) {
            trackIt->instrument.pluginStateBase64 = componentStateBase64;   // legacy mirror
        }
    }
    // Then, if the instrument is already prepared and playing, apply the patch to the live instance
    // so there is no gap — deactivate/setState/reactivate, no re-instantiate. If it is not prepared,
    // the plan patch above means the next prepare comes up on the new patch anyway.
    const std::string processorKey = trackName + "#I" + std::to_string(slotIndex + 1);
    auto processor = projectRenderState_.instrumentProcessors.find(processorKey);
    if (processor == projectRenderState_.instrumentProcessors.end()) {
        message_ = "Instrument patch stored in the plan; applies on next render.";
        return true;   // not a failure: the plan carries the patch to the next prepare
    }
    std::string applyMessage;
    const bool applied = processor->second.applyComponentState(componentStateBase64, applyMessage);
    message_ = applied ? "Applied instrument patch to the live voice." : applyMessage;
    return applied;
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
    // Track how many keys are currently down so the render can keep the instrument alive
    // while notes sustain but let it fall silent when the keyboard is idle.
    int held = liveNotesHeld_.load(std::memory_order_relaxed);
    for (const auto& event : events) {
        if (event.kind != Vst3MidiEventKind::Note) {
            continue;
        }
        if (event.noteOn && event.velocity > 0) {
            ++held;
        } else if (held > 0) {
            --held;
        }
    }
    liveNotesHeld_.store(std::max(0, held), std::memory_order_relaxed);
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
    // Recording (physical mic source): extract the armed track's channel pair and append to the take.
    if (recordingActive_.load(std::memory_order_acquire) && recordSource_.load(std::memory_order_relaxed) == 1) {
        std::lock_guard<std::mutex> rlock(recordMutex_);
        if (recordTake_) {
            const int off = recordChannelOffset_;
            const int rc = recordChannels_;
            static thread_local std::vector<float> recTmp;
            recTmp.resize(static_cast<size_t>(frameCount) * static_cast<size_t>(rc));
            for (int64_t f = 0; f < frameCount; ++f) {
                for (int c = 0; c < rc; ++c) {
                    const int srcCh = off + c;
                    recTmp[static_cast<size_t>(f) * rc + c] =
                        (srcCh < channels) ? samples[static_cast<size_t>(f * channels + srcCh)] : 0.0f;
                }
            }
            recordTake_->appendInterleavedFloat(recTmp.data(), static_cast<int>(frameCount));
            accumulateRecordPeaksLocked(recTmp.data(), frameCount, rc);
        }
    }
    // Acoustic measurement: capture the chosen input channel while a sweep is playing. For an
    // interface loopback this is the ADC channel the DAC is patched back into (default 0 = ch 1).
    if (measurementActive_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> mlock(measurementMutex_, std::try_to_lock);
        if (mlock.owns_lock()) {
            const int ic = std::max(0, std::min(channels - 1, measurementInputChannel_.load(std::memory_order_relaxed)));
            for (int64_t frame = 0; frame < frameCount; ++frame) {
                measurementCapture_.push_back(samples[static_cast<size_t>(frame * channels + ic)]);
            }
        }
    }
    // Live peak of the chosen ADC channel for the gain-setup meter (also during the sweep).
    if (measurementLevelCheck_.load(std::memory_order_relaxed) || measurementActive_.load(std::memory_order_relaxed)) {
        const int ic = std::max(0, std::min(channels - 1, measurementInputChannel_.load(std::memory_order_relaxed)));
        float pk = 0.0f;
        for (int64_t frame = 0; frame < frameCount; ++frame) {
            pk = std::max(pk, std::abs(samples[static_cast<size_t>(frame * channels + ic)]));
        }
        const float prev = measurementInputPeak_.load(std::memory_order_relaxed);
        measurementInputPeak_.store(std::max(pk, prev * 0.85f), std::memory_order_relaxed);   // brief peak-hold
        if (measurementActive_.load(std::memory_order_relaxed)) {   // undecayed max, for the clip check
            measurementSweepPeak_.store(std::max(pk, measurementSweepPeak_.load(std::memory_order_relaxed)),
                                        std::memory_order_relaxed);
        }
    }

    // Per-channel input activity for the talkback channel picker ("which mics are live"). Cheap
    // decayed peak across up to 32 channels, metered whenever input flows (regardless of routing).
    {
        const int nch = std::min(channels, kMaxMeteredInputChannels);
        inputChannelCount_.store(nch, std::memory_order_relaxed);
        for (int c = 0; c < nch; ++c) {
            float pk = 0.0f;
            for (int64_t frame = 0; frame < frameCount; ++frame) {
                pk = std::max(pk, std::abs(samples[static_cast<size_t>(frame * channels + c)]));
            }
            const float prev = inputChannelPeak_[static_cast<size_t>(c)].load(std::memory_order_relaxed);
            inputChannelPeak_[static_cast<size_t>(c)].store(std::max(pk, prev * 0.90f), std::memory_order_relaxed);
        }
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
    float peakL = inputPeakLeft_, peakR = inputPeakRight_;
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
        peakL = std::max(peakL, std::abs(left));
        peakR = std::max(peakR, std::abs(right));
    }
    // Record-arm / talkback monitoring wants a shallow FIFO (low latency: your voice must
    // reach the speaker fast). Listening to a source (BlackHole reference music) is the
    // opposite — latency is irrelevant, but the input and output run on independent clocks
    // and the driver delivers in bursts, so a shallow cap dropped and starved constantly
    // (severe stutter). Give the reference feed a deep cushion (~170 ms) to ride over both.
    // This FIFO is now the MIC path only (record-arm / talkback); the reference tap has its own
    // deep FIFO in pushReferenceInterleaved. A shallow cushion keeps the mic low-latency.
    const int64_t blockFrames = std::max<int64_t>(16, maxBlockSize_);
    const int64_t capFrames = blockFrames * (talkbackActive ? 2 : 4);
    const size_t maxSamples = static_cast<size_t>(capFrames) * 2;
    if (inputMonitorBuffer_.size() > maxSamples) {
        inputMonitorBuffer_.erase(inputMonitorBuffer_.begin(), inputMonitorBuffer_.end() - static_cast<std::ptrdiff_t>(maxSamples));
    }
    // Talkback mic: one input channel, captured mono to shadow the stereo FIFO frame-for-frame
    // while talkback is engaged. Centered on the monitor / listen-room in mixInputMonitorLocked so
    // a talkback mic on ch2 is heard on both speakers, not stuck on one side, and does not pull in
    // whatever the record-monitor channels (ch1/ch2) carry.
    if (talkbackActive) {
        const int tch = std::max(0, std::min(channels - 1, talkbackInputChannel_.load(std::memory_order_relaxed)));
        const auto tStart = talkbackMonoBuffer_.size();
        talkbackMonoBuffer_.resize(tStart + static_cast<size_t>(frameCount));
        for (int64_t frame = 0; frame < frameCount; ++frame) {
            talkbackMonoBuffer_[tStart + static_cast<size_t>(frame)] =
                samples[static_cast<size_t>(frame * channels + tch)];
        }
        if (talkbackMonoBuffer_.size() > static_cast<size_t>(capFrames)) {
            talkbackMonoBuffer_.erase(talkbackMonoBuffer_.begin(),
                                      talkbackMonoBuffer_.end() - static_cast<std::ptrdiff_t>(capFrames));
        }
    } else if (!talkbackMonoBuffer_.empty()) {
        talkbackMonoBuffer_.clear();
    }
    inputPeak_ = std::min(1.0f, peak);
    inputPeakLeft_ = std::min(1.0f, peakL);
    inputPeakRight_ = std::min(1.0f, peakR);
    inputPeakLeftForStatus_.store(inputPeakLeft_, std::memory_order_relaxed);
    inputPeakRightForStatus_.store(inputPeakRight_, std::memory_order_relaxed);
    physicalInputMonitoringActive_ = true;
    physicalInputMonitoringActiveForStatus_.store(true, std::memory_order_relaxed);
    inputMonitorChannelsForStatus_.store(inputMonitorChannels_, std::memory_order_relaxed);
    inputPeakForStatus_.store(inputPeak_, std::memory_order_relaxed);
}

void NeuracoustDspEngine::pushReferenceInterleaved(const float* interleavedStereo, int64_t frameCount) {
    if (interleavedStereo == nullptr || frameCount <= 0) return;
    // Feed the reference FIFO FIRST, in its own short scope. The render thread try_locks this mutex
    // and skips the block if it can't get it, so we hold it only for the fast copy — never across
    // the recording append below. (This is the monitored audio; it must not glitch.)
    float peak = 0.0f;
    float peakL = inputPeakLeft_, peakR = inputPeakRight_;
    {
        std::lock_guard<std::mutex> lock(inputMonitorMutex_);
        const size_t start = referenceBuffer_.size();
        referenceBuffer_.resize(start + static_cast<size_t>(frameCount) * 2u);
        for (int64_t f = 0; f < frameCount; ++f) {
            const float l = interleavedStereo[f * 2];
            const float r = interleavedStereo[f * 2 + 1];
            referenceBuffer_[start + static_cast<size_t>(f) * 2u]      = l;
            referenceBuffer_[start + static_cast<size_t>(f) * 2u + 1u] = r;
            peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
            peakL = std::max(peakL, std::abs(l));
            peakR = std::max(peakR, std::abs(r));
        }
        // Deep cushion (~170 ms+): the tap and the output run on independent clocks and the tap
        // delivers in bursts, so a shallow cap would starve. The resampler parks the depth near
        // maxBlockSize_*40; keep the cap well above that.
        const int64_t blockFrames = std::max<int64_t>(16, maxBlockSize_);
        const size_t maxSamples = static_cast<size_t>(blockFrames * 96) * 2u;
        if (referenceBuffer_.size() > maxSamples) {
            // The FIFO overflowed: the tap is delivering faster than the render consumes and the
            // oldest audio is discarded. Audible as a crackle, and just as invisible to the render
            // jitter meter as an underrun.
            referenceOverrunDrops_.fetch_add(1, std::memory_order_relaxed);
            referenceBuffer_.erase(referenceBuffer_.begin(),
                                   referenceBuffer_.end() - static_cast<std::ptrdiff_t>(maxSamples));
        }
    }
    inputPeak_ = std::min(1.0f, peak);
    inputPeakLeft_ = std::min(1.0f, peakL);
    inputPeakRight_ = std::min(1.0f, peakR);
    inputPeakLeftForStatus_.store(inputPeakLeft_, std::memory_order_relaxed);
    inputPeakRightForStatus_.store(inputPeakRight_, std::memory_order_relaxed);
    inputMonitorChannels_ = 2;
    physicalInputMonitoringActive_ = true;
    physicalInputMonitoringActiveForStatus_.store(true, std::memory_order_relaxed);
    inputMonitorChannelsForStatus_.store(2, std::memory_order_relaxed);
    inputPeakForStatus_.store(inputPeak_, std::memory_order_relaxed);

    // Recording (reference-tap source) LAST: the tap is already stereo. recordMutex_ is a separate
    // lock from the FIFO's, so a slow disk-take append here can never starve the monitored feed.
    if (recordingActive_.load(std::memory_order_acquire) && recordSource_.load(std::memory_order_relaxed) == 2) {
        std::lock_guard<std::mutex> rlock(recordMutex_);
        if (recordTake_) {
            recordTake_->appendInterleavedFloat(interleavedStereo, static_cast<int>(frameCount));
            accumulateRecordPeaksLocked(interleavedStereo, frameCount, 2);
        }
    }
}

void NeuracoustDspEngine::setEditorInstrumentMonitor(bool active, const std::string& trackName) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        editorMonitorTrackName_ = active ? trackName : std::string{};
    }
    editorMonitorActive_.store(active, std::memory_order_relaxed);
    if (!active) {
        std::lock_guard<std::mutex> lock(editorMonitorMutex_);
        editorMonitorBuffer_.clear();
    }
}

void NeuracoustDspEngine::pushEditorInstrumentMonitorInterleaved(const float* samples, int64_t frameCount) {
    if (samples == nullptr || frameCount <= 0 || !editorMonitorActive_.load(std::memory_order_relaxed)) {
        return;
    }
    // Pusher is a UTILITY thread, so it may block briefly; the render side only
    // try_locks, so the audio thread never waits on this mutex.
    std::lock_guard<std::mutex> lock(editorMonitorMutex_);
    editorMonitorBuffer_.insert(editorMonitorBuffer_.end(), samples,
                                samples + static_cast<size_t>(frameCount) * 2u);
    // The host now keeps one low-latency block ahead in steady state and may publish
    // a short catch-up burst after a heavy editor redraw. Keep the full ring capacity
    // so that recovery audio is not discarded, but do not manufacture a deep FIFO here.
    // Sixteen blocks only bounds a rare accumulated burst; normal latency is one block.
    const int64_t blockFrames = std::max<int64_t>(16, maxBlockSize_);
    const size_t maxSamples = static_cast<size_t>(blockFrames * 16) * 2u;
    if (editorMonitorBuffer_.size() > maxSamples) {
        editorMonitorBuffer_.erase(editorMonitorBuffer_.begin(),
                                   editorMonitorBuffer_.end() - static_cast<std::ptrdiff_t>(maxSamples));
    }
}

void NeuracoustDspEngine::renderInterleavedStereo(int64_t frameCount, std::vector<float>& interleavedStereo) {
    if (frameCount <= 0) {
        interleavedStereo.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // Remote-wait accounting: every remote exchange below adds its wall time to the
    // accumulator; publish whatever this block collected on EVERY exit path so the load
    // meter's split can never go stale.
    remoteWaitUsBlockAccum_ = 0.0;
    struct PublishRemoteWait {
        NeuracoustDspEngine* engine;
        ~PublishRemoteWait() {
            engine->remoteWaitUsLastBlock_.store(engine->remoteWaitUsBlockAccum_,
                                                 std::memory_order_relaxed);
        }
    } publishRemoteWait {this};
    interleavedStereo.assign(static_cast<size_t>(frameCount) * 2, 0.0f);
    // Internal-bus recording (source 3): the armed track's received bus block is appended to the
    // take HERE, on the render clock — sample-locked to the timeline, unlike the input-thread
    // sources. Re-assigned per block (captures only `this`, so no allocation) so it can never
    // outlive a recording.
    if (recordingActive_.load(std::memory_order_acquire) &&
        recordSource_.load(std::memory_order_relaxed) == 3) {
        projectRenderState_.captureBusInput =
            [this](const std::string& routeName, const float* bus, int64_t frames) {
                if (frames <= 0 || routeName != recordBusRouteName_) return;
                std::lock_guard<std::mutex> recordLock(recordMutex_);
                if (!recordTake_) return;
                const float* samples = bus;
                if (samples == nullptr) {
                    recordSilentScratch_.assign(static_cast<size_t>(frames) * 2u, 0.0f);
                    samples = recordSilentScratch_.data();
                }
                recordTake_->appendInterleavedFloat(samples, static_cast<int>(frames));
                accumulateRecordPeaksLocked(samples, frames, 2);
            };
    } else {
        projectRenderState_.captureBusInput = nullptr;
    }
    syncProjectMonitorDspRenderPathLocked();
    const ProjectAudioRenderPlan& renderPlan = projectPlan_;
    const bool transportRunning = settings_.transportRunning;
    // Live MIDI (a keyboard held while stopped) must still sound, so do not drop queued
    // live events when the transport is idle — hasProjectRenderContent below already
    // renders the instrument whenever liveMidiEvents is non-empty. The renderer consumes
    // and erases each event, so nothing accumulates.
    const bool hasLiveMidi = !projectRenderState_.liveMidiEvents.empty();
    // Keep a live-monitored instrument rendering while a key is held (so it sustains between
    // the ~30 Hz pumps instead of gating) and for a short release tail after the last key
    // lifts — but no longer. Rendering it forever while armed made an idle instrument sound
    // non-stop with the transport stopped ("무조건 재생"). While notes are down the tail is
    // held full; once the keyboard is idle it counts down to silence.
    if (liveNotesHeld_.load(std::memory_order_relaxed) > 0) {
        liveMonitorTailSamplesRemaining_ = static_cast<int64_t>(std::max(1.0, settings_.sampleRate) * 4.0);
    } else if (liveMonitorTailSamplesRemaining_ > 0) {
        liveMonitorTailSamplesRemaining_ = std::max<int64_t>(0, liveMonitorTailSamplesRemaining_ - frameCount);
    }
    const bool liveMonitorActive = liveMonitorTailSamplesRemaining_ > 0;
    // An instrument voice — a held note, its release tail — lives inside the plug-in, so the
    // engine has to keep calling process on it every block while that voice is alive.
    const bool hasInstrumentTrack =
        std::any_of(renderPlan.tracks.begin(), renderPlan.tracks.end(), [&](const TrackState& track) {
            if (track.trackType != "instrument") {
                return false;
            }
            const bool hasPlayableSlot =
                std::any_of(track.instrumentSlots.begin(), track.instrumentSlots.end(), [](const InstrumentSlotState& slot) {
                    return slot.enabled && !slot.bypassed && !slot.pluginPath.empty();
                }) ||
                (track.instrument.enabled && !track.instrument.bypassed && !track.instrument.pluginPath.empty());
            if (!hasPlayableSlot) {
                return false;
            }
            // Render while a live voice is actually sounding (a key is held, or its release
            // tail) — regardless of whether the track is armed. The selected instrument track
            // (the live-MIDI target) is monitored on selection, not by arming, so gating this
            // on recordArmed/inputMonitoring dropped its sustain and release: only the ~30 Hz
            // note-on blocks rendered, so held notes came out choppy ("소리가 잘 안나요"). The
            // tail is bounded (full while keys are down, then counts down to silence), so an
            // idle instrument still falls silent — no "무조건 재생".
            return transportRunning || hasLiveMidi || liveMonitorActive;
        });
    const bool hasActiveInserts =
        renderPlan.hasActiveVst3Inserts ||
        renderPlan.hasActiveTrackVst3Inserts ||
        !realtimeTrackInsertChains_.empty() ||
        realtimeInsertChain_.activeVst3Count() > 0;
    // Insert tail behaviour on stop:
    //   < 0  → always on: the insert chains keep processing forever (Pro Tools HD),
    //          so reverb/delay never cut and live monitoring through inserts stays up.
    //   == 0 → cut immediately.
    //   > 0  → ring out for that many seconds.
    const double tailSec = insertTailOnStopSeconds_.load(std::memory_order_relaxed);
    const bool insertAlwaysOn = tailSec < 0.0;
    if (wasTransportRunning_ && !transportRunning && hasActiveInserts && tailSec > 0.0) {
        insertTailSamplesRemaining_ = static_cast<int64_t>(tailSec * std::max(1.0, settings_.sampleRate));
    }
    wasTransportRunning_ = transportRunning;
    if (transportRunning) {
        insertTailSamplesRemaining_ = 0;
    }
    const bool insertTailActive = !transportRunning &&
        (insertAlwaysOn || insertTailSamplesRemaining_ > 0) && hasActiveInserts;
    const bool hasRealtimeInsertRenderSource = (transportRunning || insertTailActive) && hasActiveInserts;
    if (insertTailActive && !insertAlwaysOn) {
        insertTailSamplesRemaining_ = std::max<int64_t>(0, insertTailSamplesRemaining_ - frameCount);
    }
    const bool hasProjectRenderContent = (transportRunning && (!renderPlan.clips.empty() ||
        !renderPlan.midiRegions.empty() ||
        // A signal generator is a track SOURCE with no clip, no MIDI and no VST3 — none of the
        // other terms see it, so on an empty timeline the render never ran and a fresh generator
        // played silence while the transport rolled.
        renderPlan.hasActiveSourceGenerator)) ||
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
                                                      &projectSegmentMeters_,
                                                      /*offline=*/false,
                                                      /*transportRunning=*/transportRunning);
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
        projectMeters_.masterPeakLeft = 0.0f;
        projectMeters_.masterPeakRight = 0.0f;
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
    // Tap the master output for the spectrum analyzer HERE — the summed, master-fadered mix, before
    // input monitoring, the monitor DSP simulation, and the monitor station volume colour or scale it.
    // The analyzer must show the printed mix; turning the monitor knob must not move the spectrum.
    spectrumSourceBlock_.assign(interleavedStereo.begin(), interleavedStereo.end());
    mixInputMonitorLocked(frameCount, interleavedStereo);
    // When monitoring a reference source (BlackHole), the master is silenced and replaced by
    // the source, so the analyzer and goniometer should show the SOURCE you are hearing, not
    // the muted master — retap after the input mix (still before the monitor-station volume,
    // so the monitor knob does not move it).
    if (listenSourceActive_.load(std::memory_order_relaxed)) {
        spectrumSourceBlock_.assign(interleavedStereo.begin(), interleavedStereo.end());
    }
    // Audio from an open instrument editor's own instance (GUI clicks, live MIDI while the
    // editor owns the live path). Mixed here — before the monitor DSP path — so it is
    // coloured exactly like the rest of the monitored mix.
    mixEditorInstrumentMonitorLocked(frameCount, interleavedStereo);
    const float monitorInputTrimGain = projectMonitorDspRenderedInGraph
        ? 1.0f
        : dbToGain(std::max(-12.0f, std::min(0.0f, settings_.monitorInputTrimDb)));
    if (std::abs(monitorInputTrimGain - 1.0f) > 0.0001f) {
        for (float& sample : interleavedStereo) {
            sample *= monitorInputTrimGain;
        }
    }
    // The monitor DSP simulation (speaker model, room EQ, monitor inserts) runs on every block
    // even when the mix is silent, which is why an idle project still shows a few % DSP load.
    // Once the output has been pin-drop silent long enough for the filters to have decayed, skip
    // it — silent in stays silent out, so this can't click, and a stopped project costs ~0 DSP.
    // A live transition/crossfade or any signal resumes full processing immediately.
    bool blockSilent = true;
    for (const float s : interleavedStereo) {
        if (std::abs(s) > 1.0e-6f) { blockSilent = false; break; }
    }
    monitorDspSilentSamples_ = blockSilent ? (monitorDspSilentSamples_ + frameCount) : 0;
    const bool monitorTransitionActive =
        monitorDspTransitionSamplesRemaining_ > 0 || monitorDspModuleTransitionSamplesRemaining_ > 0;
    const bool skipMonitorDsp = blockSilent && !monitorTransitionActive &&
        monitorDspSilentSamples_ > static_cast<int64_t>(std::max(1.0, settings_.sampleRate) * 0.25);
    if (settings_.monitorDspEnabled && !projectMonitorDspRenderedInGraph && !skipMonitorDsp) {
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
    if (!skipMonitorDsp) {
        applyMonitorOutputInsertChainLocked(interleavedStereo);
        // The user's monitor parametric EQ (room correction / tone) sits at the end of the
        // monitor chain — it colours what you hear, never the printed mix. Skipped on silence
        // with everything else.
        if (monitorFir_.active()) {
            monitorFir_.processInterleavedStereo(interleavedStereo.data(), static_cast<int>(frameCount));
        } else if (monitorEq_.active()) {
            monitorEq_.processInterleavedStereo(interleavedStereo.data(), static_cast<int>(frameCount));
        }
        // Nonlinear interface character (2단계 waveshaper) sits at the output stage, after the tone EQ.
        if (interfaceModeler_.active()) {
            interfaceModeler_.processInterleavedStereo(interleavedStereo.data(), static_cast<int>(frameCount));
        }
        // Safety soft-clip AFTER the whole monitor colouring chain: the monitor EQ is now level-matched
        // on the midband (not peak-normalized), so a presence/air boost is a real boost and a hot mix
        // can momentarily exceed full scale. Catch only that overshoot softly here — this is what lets
        // the tone stay un-darkened without the sim clipping into gritty broadband distortion.
        for (float& sample : interleavedStereo) {
            sample = monitorSafetySoftClip(sample);
        }
    }
    // Tap the meter here: everything that SHAPES the monitor signal has run (listen mode, monitor
    // DSP, speaker sim), and the monitor level has not. Solo and mono/stereo therefore move this
    // meter; the monitor volume knob does not.
    {
        float prePeakLeft = 0.0f, prePeakRight = 0.0f;
        for (size_t i = 0; i + 1 < interleavedStereo.size(); i += 2) {
            prePeakLeft = std::max(prePeakLeft, std::abs(interleavedStereo[i]));
            prePeakRight = std::max(prePeakRight, std::abs(interleavedStereo[i + 1]));
        }
        monitorPrePeakLeft_.store(std::min(1.0f, prePeakLeft));
        monitorPrePeakRight_.store(std::min(1.0f, prePeakRight));
    }
    applyMonitorStationControlsLocked(interleavedStereo);
    // An async insert chain engaged/disengaged this block (dry↔wet). Re-arm the 80 ms output
    // crossfade NOW so it masks the real swap with the previous (dry) waveform — the edit-time
    // arming already decayed while the chain prepared off-thread. Robust: runs after PDC and the
    // monitor path, and fades a real waveform, unlike the per-route 15 ms held-sample fade.
    if (projectRenderState_.routeInsertEngagementChangedThisBlock && !previousOutputBlock_.empty()) {
        reloadCrossfadeSamplesTotal_ = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.080));
        reloadCrossfadeSamplesRemaining_ = reloadCrossfadeSamplesTotal_;
    }
    projectRenderState_.routeInsertEngagementChangedThisBlock = false;
    applyReloadCrossfadeLocked(interleavedStereo);
    applySeekRampLocked(interleavedStereo);
    publishListenRoomLocked(interleavedStereo);
    realtimeProcessFrame_ += frameCount;
    if (transportRunning) {
        playbackFrame_ = wrappedPlaybackFrameForPlan(projectPlan_, playbackFrame_ + frameCount);
    }
    // Acoustic measurement: overwrite the output with the sweep on the chosen channel so it
    // reaches the speaker (the mic is captured in pushInputMonitorInterleaved). Still passes
    // through the output-safety guard downstream, so the loud sweep can't damage a speaker.
    if (measurementActive_.load(std::memory_order_relaxed)) {
        const int64_t total = static_cast<int64_t>(measurementSignal_.size());
        for (int64_t f = 0; f < frameCount; ++f) {
            const float s = (measurementEmitPos_ < total) ? measurementSignal_[static_cast<size_t>(measurementEmitPos_)] : 0.0f;
            const auto idx = static_cast<size_t>(f) * 2u;
            if (idx + 1 < interleavedStereo.size()) {
                interleavedStereo[idx] = (measurementChannel_ == 0) ? s : 0.0f;
                interleavedStereo[idx + 1] = (measurementChannel_ == 1) ? s : 0.0f;
            }
            ++measurementEmitPos_;
        }
        measurementProgressFrames_.store(measurementEmitPos_, std::memory_order_relaxed);
        if (measurementEmitPos_ >= total) {
            measurementActive_.store(false, std::memory_order_relaxed);
            // Leave capture forced if level-check still owns it; else restore the true prior state.
            if (!measurementLevelCheck_.load(std::memory_order_relaxed)) {
                inputMonitorCaptureActive_.store(measurementPrevInputMonitor_, std::memory_order_relaxed);
            }
        }
    } else if (measurementLevelCheck_.load(std::memory_order_relaxed)) {
        // Gain-setup reference tone: a 1 kHz sine at the SWEEP's level and path (overwrites the
        // output here, after the monitor gain, exactly like the sweep) so the input meter predicts
        // the sweep's return level — set gain to 적정 and the sweep will not clip.
        const double inc = 2.0 * M_PI * 1000.0 / std::max(1.0, settings_.sampleRate);
        for (int64_t f = 0; f < frameCount; ++f) {
            const float s = 0.5f * static_cast<float>(std::sin(measurementTonePhase_));
            measurementTonePhase_ += inc;
            if (measurementTonePhase_ >= 2.0 * M_PI) measurementTonePhase_ -= 2.0 * M_PI;
            const auto idx = static_cast<size_t>(f) * 2u;
            if (idx + 1 < interleavedStereo.size()) {
                interleavedStereo[idx] = (measurementChannel_ == 0) ? s : 0.0f;
                interleavedStereo[idx + 1] = (measurementChannel_ == 1) ? s : 0.0f;
            }
        }
    }

    playbackFrameForStatus_.store(playbackFrame_);
    storeMetering(interleavedStereo);
    // Publish the status snapshot for the UI poll WHILE we already hold mutex_. try_lock only, so the
    // render NEVER blocks on the reader; it reuses statusShadow_'s capacity so it doesn't allocate in
    // steady state. Published EVERY block: an earlier 3-block throttle left the shadow stale for the
    // first two blocks after a seek/start, so a poll right after seeking read zeros and the meters —
    // and any single-block probe — saw silence (audio_engine_smoke's portable-render check).
    ++statusPublishCounter_;
    if (statusShadowMutex_.try_lock()) {
        populateStatusLocked(statusShadow_);
        statusShadowMutex_.unlock();
    }
}

void NeuracoustDspEngine::populateStatusLocked(AudioEngineStatus& status) const {
    // Caller holds mutex_. Assign into `status` (the render's shadow), reusing its vector capacity
    // so this does not allocate in steady state.
    status.sampleRate = settings_.sampleRate;
    status.transportRunning = settings_.transportRunning;
    status.monitorPrePeakLeft = monitorPrePeakLeft_.load();
    status.monitorPrePeakRight = monitorPrePeakRight_.load();
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
    status.masterBusPeakLeft = projectMeters_.masterPeakLeft;
    status.masterBusPeakRight = projectMeters_.masterPeakRight;
    status.trackPeakLeft = projectMeters_.trackPeakLeft;
    status.trackPeakRight = projectMeters_.trackPeakRight;
    status.trackConsoleGainReductionDb = projectMeters_.trackConsoleGainReductionDb;
    status.trackConsoleGateGainReductionDb = projectMeters_.trackConsoleGateGainReductionDb;
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
    status.listenSourceActive = listenSourceActive_.load(std::memory_order_relaxed);
    status.referenceTapArmed = referenceTapArmed_.load(std::memory_order_relaxed);
    status.physicalInputMonitoringActive = physicalInputMonitoringActiveForStatus_.load(std::memory_order_relaxed);
    status.recordArmedTrackCount = recordArmedTrackCount_;
    status.inputChannels = inputMonitorChannelsForStatus_.load(std::memory_order_relaxed);
    status.inputPeak = inputPeakForStatus_.load(std::memory_order_relaxed);
    status.inputPeakLeft = inputPeakLeftForStatus_.load(std::memory_order_relaxed);
    status.inputPeakRight = inputPeakRightForStatus_.load(std::memory_order_relaxed);
    status.requestedBufferSize = settings_.bufferSize;
    status.playbackStabilityBufferSize = std::max(1, settings_.bufferSize) * std::max(1, settings_.playbackStabilityBufferMultiplier);
    status.dspEngineName = "Neuracoust DSP Engine";
    status.monitorDspPathMode = settings_.monitorDspPathMode;
    // Remote telemetry ages out. These numbers are only true while blocks are actually crossing:
    // once the stream stops being called at all — transport stopped, no monitor signal, the node
    // unplugged — the last values would sit on the dock forever, and a link that no longer exists
    // would go on reporting a round trip and a "connected" lamp.
    const int64_t lastExchangeUs = remoteDspLastExchangeSteadyUs_.load(std::memory_order_relaxed);
    const int64_t nowSteadyUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool remoteTelemetryFresh = lastExchangeUs > 0 && (nowSteadyUs - lastExchangeUs) < 1500000;
    status.remoteDspMonitorActive = remoteDspMonitorActive_ && remoteTelemetryFresh;
    status.remoteDspRoundTripMs = remoteTelemetryFresh ? remoteDspRoundTripMs_ : 0.0;
    status.remoteMixBusCount = remoteMixerMode_.empty()
        ? 0u : static_cast<uint32_t>(realtimeMixSessions_.size());
    status.remoteMixSums = remoteMixSums_;
    status.remoteMixMisses = remoteMixMisses_;
    status.referenceUnderrunBlocks = referenceUnderrunBlocks_.load(std::memory_order_relaxed);
    status.referenceOverrunDrops = referenceOverrunDrops_.load(std::memory_order_relaxed);
    status.remoteDspAverageRoundTripJitterUs = remoteTelemetryFresh ? remoteDspAverageRoundTripJitterUs_ : 0.0;
    status.remoteDspMaxRoundTripJitterUs = remoteTelemetryFresh ? remoteDspMaxRoundTripJitterUs_ : 0.0;
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
}

// Status read that never STALLS the render: try_lock the render mutex so the ~30 Hz UI poll can
// never hold the render lock while descheduled under background CPU load (the dropout this guards).
// When the lock is free — the common case, including whenever the transport is stopped or between
// blocks — populate a LIVE snapshot so status reflects edits (loadProject/updateProject, insert
// add/remove, seek) immediately, even with no render running to publish the shadow. Only when the
// render currently holds the lock do we fall back to the shadow it publishes every block (fresh to
// within one block). This keeps both invariants: no render stall, and no stale status after an edit.
AudioEngineStatus NeuracoustDspEngine::statusSnapshot() const {
    if (mutex_.try_lock()) {
        AudioEngineStatus live;
        populateStatusLocked(live);
        {
            std::lock_guard<std::mutex> shadowLock(statusShadowMutex_);
            statusShadow_ = live;
        }
        mutex_.unlock();
        return live;
    }
    std::lock_guard<std::mutex> lock(statusShadowMutex_);
    return statusShadow_;
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
                                              std::max(1, maxBlockSize_), warmBlock, nullptr,
                                              /*offline=*/false, /*transportRunning=*/false);
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

    // One plan per machine, built only for machines an insert actually asks for. Each machine
    // has its own address, its own core report and its own loaded module, so a single shared
    // plan could only ever be right for one of them. An insert names its machine in
    // assignedDspServerId; empty follows the project's 인서트 배정.
    struct RemoteInsertPlan {
        RemoteDspServerSettings settings;
        bool available = false;
        std::string unavailableReason;
        /// Every module the node hosts (empty on an old single-module engine, whose one module
        /// is loadedPluginIdHint). Membership here is what lets DIFFERENT inserts offload to
        /// DIFFERENT modules of one node in the same session.
        std::vector<std::string> hostedModules;
    };
    std::map<std::string, RemoteInsertPlan> remoteInsertPlans;
    auto planForMode = [&](const std::string& mode) -> const RemoteInsertPlan& {
        auto existing = remoteInsertPlans.find(mode);
        if (existing != remoteInsertPlans.end()) {
            return existing->second;
        }
        RemoteInsertPlan plan;
        plan.settings = remoteDspSettingsForMode(settings_.remoteDspServer, mode);
        if (!remoteDspModeAvailable(settings_.remoteDspServer, mode)) {
            plan.settings.enabled = false;
            plan.unavailableReason = mode == "internal"
                ? std::string("내장 DSP에 배정됨")
                : ("배정된 " + mode + " 노드가 꺼져 있거나 주소가 없음");
            return remoteInsertPlans.emplace(mode, std::move(plan)).first->second;
        }
        plan.settings.pluginDspEnabled = true;
        double measuredRemoteRoundTripMs = 0.0;
        if (plan.settings.loadedPluginIdHint.empty() && hostIsNumericIpv4(plan.settings.host)) {
            RemoteDspServerSettings infoSettings = plan.settings;
            infoSettings.timeoutMs = infoSettings.timeoutMs > 0 ? std::min(infoSettings.timeoutMs, 150) : 150;
            const auto serverInfo = queryRemoteDspServerInfo(infoSettings);
            if (serverInfo.reachable) {
                plan.settings.loadedPluginIdHint = serverInfo.pluginId;
                plan.hostedModules = serverInfo.pluginIds;
                measuredRemoteRoundTripMs = serverInfo.roundTripMs;
            }
        }
        if (measuredRemoteRoundTripMs > 0.0) {
            recordRemoteDspRoundTripLocked(measuredRemoteRoundTripMs);
        }
        const auto corePlan = makeRemoteDspCorePlan(plan.settings,
                                                    plan.settings.totalCoreHint,
                                                    remoteMonitorDspRequestedLocked());
        plan.available = plan.settings.enabled && corePlan.pluginCores > 0;
        if (!plan.available) {
            plan.unavailableReason = "배정된 " + mode + " 노드에 플러그인 코어가 없음";
        }
        return remoteInsertPlans.emplace(mode, std::move(plan)).first->second;
    };
    // The machine an insert resolves to, with auto-overflow folding every answer into "auto".
    auto modeForInsert = [&](const TrackInsertSlot& insert) {
        return remoteDspModeForRole(
            settings_.remoteDspServer,
            effectiveDspMachine(insert.assignedDspServerId, settings_.remoteDspServer.roleInserts));
    };
    std::vector<std::string> skippedRemoteInserts;
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
        // Remote inserts collected per machine: two plug-ins on one track can be assigned to
        // different nodes, and each node needs its own stream. Chains are additive deltas onto
        // the mix, so several on one track coexist the same way remote and local already do.
        struct RemoteGroup {
            std::vector<std::string> moduleIds;
            std::vector<int> slotIndices;
            std::vector<RemoteDspParameterValue> parameters;
        };
        std::map<std::string, RemoteGroup> remoteGroups;
        std::vector<int> localSlotIndices;
        int firstActiveSlotIndex = -1;
        bool protectLocalDryWhenSilent = false;
        for (size_t insertIndex = 0; insertIndex < track.inserts.size(); ++insertIndex) {
            const auto& insert = track.inserts[insertIndex];
            InsertState renderInsert = trackInsertToRenderInsert(insert);
            const std::string insertMode = modeForInsert(insert);
            const auto& plan = planForMode(insertMode);
            const auto remoteCapability = remoteDspCapabilityForInsert(insert, plan.available, true);
            const std::string effectiveMode = effectiveTrackInsertDspExecutionMode(insert);
            const bool serverModuleMatches = !plan.hostedModules.empty()
                ? std::find(plan.hostedModules.begin(), plan.hostedModules.end(),
                            remoteCapability.moduleId) != plan.hostedModules.end()
                : (plan.settings.loadedPluginIdHint.empty() ||
                   plan.settings.loadedPluginIdHint == remoteCapability.moduleId);
            if (isRemoteInternalDspExecutionMode(effectiveMode) &&
                plan.available &&
                remoteCapability.mode == RemoteDspInsertMode::RemoteActive &&
                serverModuleMatches) {
                auto& group = remoteGroups[insertMode];
                if (group.parameters.empty()) {
                    group.parameters = remoteParametersForInsert(insert);
                }
                if (firstActiveSlotIndex < 0) {
                    firstActiveSlotIndex = static_cast<int>(insertIndex);
                }
                group.moduleIds.push_back(remoteCapability.moduleId);
                group.slotIndices.push_back(static_cast<int>(insertIndex));
                continue;
            }
            if (isRemoteInternalDspExecutionMode(effectiveMode) &&
                !remoteCapability.moduleId.empty() &&
                !plan.available &&
                !plan.unavailableReason.empty()) {
                skippedRemoteInserts.push_back(track.name + ": " + insert.pluginName +
                    " — " + plan.unavailableReason);
            }
            if (isRemoteInternalDspExecutionMode(effectiveMode) &&
                !remoteCapability.moduleId.empty() &&
                plan.available &&
                !serverModuleMatches) {
                skippedRemoteInserts.push_back(track.name + ": " + insert.pluginName +
                    " requires " + remoteCapability.moduleId +
                    ", server loaded " + plan.settings.loadedPluginIdHint);
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
        for (auto& [groupMode, group] : remoteGroups) {
            if (group.moduleIds.empty()) continue;
            const auto& plan = planForMode(groupMode);
            RealtimeTrackInsertChain trackChain;
            trackChain.trackName = track.name;
            trackChain.remoteDsp = true;
            trackChain.remoteMode = groupMode;
            trackChain.remoteModuleId = group.moduleIds.front();
            trackChain.remoteParameters = std::move(group.parameters);
            trackChain.remoteStream = std::make_unique<RemoteDspAsyncStream>();
            trackChain.slotIndex = group.slotIndices.empty() ? firstActiveSlotIndex : group.slotIndices.front();
            trackChain.slotIndices = std::move(group.slotIndices);
            trackChain.latencySamples = settings_.delayCompensationEnabled
                ? estimatedRemoteDspLatencySamples(plan.settings, settings_.sampleRate, maxBlockSize)
                : 0u;
            delayCompensationSamples_ = std::max(delayCompensationSamples_, trackChain.latencySamples);
            trackChain.transitionSamplesTotal = std::max<int64_t>(1, static_cast<int64_t>(settings_.sampleRate * 0.08));
            trackChain.transitionSamplesRemaining = trackChain.transitionSamplesTotal;
            activeRemoteDspTrackInsertCount_ += static_cast<int>(trackChain.slotIndices.size());
            realtimeTrackInsertChains_.push_back(std::move(trackChain));
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
    // Last, so the realignment inside it sees every latency this pass established rather than
    // being wiped by the delayCompensationSamples_ reset that starts the function.
    prepareRemoteConsoleStripsLocked(maxBlockSize);
    prepareRemoteMasterInsertsLocked(maxBlockSize);
    prepareRemoteMixerLocked();
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
            // The machine this chain was built for, so the stream keeps talking to the address
            // its inserts named rather than to whatever the project-wide assignment says now.
            RemoteDspServerSettings remoteSettings =
                remoteDspSettingsForMode(settings_.remoteDspServer, trackChain.remoteMode);
            // And the MODULE it expects there: a multi-module engine routes on this, and any
            // engine that does not host it refuses the stream instead of processing the audio
            // through whatever it happens to have loaded.
            remoteSettings.loadedPluginIdHint = trackChain.remoteModuleId;
            const uint16_t networkBufferFrames = static_cast<uint16_t>(std::max<uint16_t>(40u, std::min<uint16_t>(1024u, remoteSettings.networkBufferFrames)));
            remoteSettings.channelCount = 2;
            remoteSettings.frameCount = static_cast<uint16_t>(std::min<int64_t>(frameCount, networkBufferFrames));
            remoteSettings.sampleRate = settings_.sampleRate;
            const double bufferLatencyMs = settings_.sampleRate > 0.0
                ? (static_cast<double>(networkBufferFrames) * 1000.0 / settings_.sampleRate)
                : 2.7;
            remoteSettings.timeoutMs = std::max<int>(8, static_cast<int>(std::ceil(bufferLatencyMs + 5.0)));
	            const auto exchangeStart = std::chrono::steady_clock::now();
	            const bool exchanged =
	                trackChain.remoteStream != nullptr &&
	                trackChain.remoteStream->process(remoteSettings,
	                                                 trackInsertProcessedBlock_,
	                                                 trackChain.remoteParameters,
	                                                 remoteTrackInsertProcessedBlock_) &&
	                remoteTrackInsertProcessedBlock_.size() == trackInsertProcessedBlock_.size();
	            remoteWaitUsBlockAccum_ += std::chrono::duration<double, std::micro>(
	                std::chrono::steady_clock::now() - exchangeStart).count();
	            if (!exchanged) {
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

// The realtime summing buses. Engaged at prepare time (same place the strips and master stages
// derive), resolved per block on the render thread with a hard 2 ms timeout per bus. Eight
// consecutive misses back the wire off to one probe every 256 blocks, so a dead node costs the
// render two milliseconds once, not per block.
void NeuracoustDspEngine::prepareRemoteMixerLocked() {
    remoteMixerMode_.clear();
    projectRenderState_.remoteBusSum = nullptr;
    const auto& remote = settings_.remoteDspServer;
    if (remote.roleMixer != "nds" && remote.roleMixer != "external") {
        realtimeMixSessions_.clear();
        return;
    }
    const std::string mode = remoteDspModeForRole(remote, remote.roleMixer);
    if (!remoteDspModeAvailable(remote, mode)) {
        realtimeMixSessions_.clear();
        return;
    }
    remoteMixerMode_ = mode;
    remoteMixerMissStreaks_.clear();
    remoteMixerProbeCountdowns_.clear();
    projectRenderState_.remoteBusSum = [this](const std::string& busName,
                                              const std::deque<std::vector<float>>& contributions,
                                              std::vector<float>& summed) {
        return processRealtimeBusSumLocked(busName, contributions, summed);
    };
    message_ = "믹서 합산을 " + remoteMixerMode_ + " 노드로 배정했습니다.";
}

// Render thread, mutex_ held by the caller (renderInterleavedStereo).
bool NeuracoustDspEngine::processRealtimeBusSumLocked(const std::string& busName,
                                                      const std::deque<std::vector<float>>& contributions,
                                                      std::vector<float>& summed) {
    // The configured channel ladder (8/16/32/64) is a real cap: a bus with more contributions
    // than the configuration stays local, bit-identically — the same rule as the node's own
    // 64-channel ceiling, just at the user's chosen size.
    const size_t mixerChannelCap =
        std::min<size_t>(64u, std::max<uint16_t>(uint16_t{8}, settings_.remoteDspServer.mixerChannels));
    if (remoteMixerMode_.empty() || contributions.empty() || contributions.size() > mixerChannelCap) {
        if (getenv("NC_DIAG_REMOTE") != nullptr && !remoteMixerMode_.empty()) {
            static int logged = 0;
            if (logged++ < 8) {
                fprintf(stderr, "[nc-mix] %s skipped: %zu contributions\n",
                        busName.c_str(), contributions.size());
            }
        }
        return false;
    }
    int& missStreak = remoteMixerMissStreaks_[busName];
    if (missStreak >= 8) {
        uint32_t& countdown = remoteMixerProbeCountdowns_[busName];
        if (countdown > 0u) {
            --countdown;
            return false;
        }
        countdown = 256u;   // one probe per ~1.4 s at 256/48k
    }
    auto& session = realtimeMixSessions_[busName];
    if (session == nullptr) {
        session = std::make_unique<RemoteMixSession>();
    }
    auto settings = remoteDspSettingsForMode(settings_.remoteDspServer, remoteMixerMode_);
    settings.channelCount = 2;
    settings.timeoutMs = 2;   // the local fallback is bit-identical; never bleed the block budget
    const auto mixExchangeStart = std::chrono::steady_clock::now();
    const auto result = session->mix(settings, contributions, summed);
    remoteWaitUsBlockAccum_ += std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - mixExchangeStart).count();
    if (!result.processed) {
        ++missStreak;
        ++remoteMixMisses_;
        if (getenv("NC_DIAG_REMOTE") != nullptr) {
            static int logged = 0;
            if (logged++ < 24) {
                fprintf(stderr, "[nc-mix] %s miss (%zu contribs, streak %d): %s\n",
                        busName.c_str(), contributions.size(), missStreak, result.message.c_str());
            }
        }
        return false;
    }
    missStreak = 0;
    ++remoteMixSums_;
    recordRemoteDspRoundTripLocked(result.roundTripMs);
    return true;
}

// Which master inserts leave the host. All-or-nothing on purpose: the master chain is SERIAL,
// and splitting it (slot 1 local, slot 2 remote, slot 3 local) would reorder the processing —
// so either every active slot resolves to a Neuracoust module on one reachable machine, or the
// whole chain stays local and the message says why.
void NeuracoustDspEngine::prepareRemoteMasterInsertsLocked(int maxBlockSize) {
    remoteMasterInserts_.clear();
    remoteMasterMode_.clear();
    projectRenderState_.remoteMasterInserts = nullptr;

    // Active slots only — a bypassed slot is not part of the sound.
    std::vector<const InsertState*> active;
    for (const auto& insert : projectPlan_.activeVst3Inserts) {
        if (!insert.bypassed && insert.available) {
            active.push_back(&insert);
        }
    }
    if (active.empty()) {
        return;
    }

    std::string mode;
    std::vector<RemoteMasterInsertStage> stages;
    for (const auto* insert : active) {
        const std::string machine =
            effectiveDspMachine(insert->assignedDspServerId, settings_.remoteDspServer.roleMaster);
        const std::string insertMode = remoteDspModeForRole(settings_.remoteDspServer, machine);
        if (!remoteDspModeAvailable(settings_.remoteDspServer, insertMode)) {
            return;   // this slot stays local, so the whole serial chain does
        }
        const auto capability = remoteDspCapabilityForMasterInsert(*insert, true, true);
        if (capability.moduleId.empty()) {
            return;   // a third-party plug-in cannot leave the host, and the chain is serial
        }
        if (mode.empty()) {
            mode = insertMode;
        } else if (mode != insertMode) {
            return;   // two machines inside one serial chain is two crossings; keep it local
        }
        RemoteMasterInsertStage stage;
        stage.moduleId = capability.moduleId;
        TrackInsertSlot slot;
        slot.parameters = insert->parameters;
        slot.pluginName = insert->pluginName;
        slot.pluginClassName = insert->pluginClassName;
        stage.parameters = remoteParametersForInsert(slot);
        stage.stream = std::make_unique<RemoteDspAsyncStream>();
        stages.push_back(std::move(stage));
    }

    remoteMasterMode_ = mode;
    remoteMasterInserts_ = std::move(stages);
    // The chain crosses the network once per stage; the whole mix is equally delayed, so this
    // only feeds the PDC total, not any relative alignment.
    if (settings_.delayCompensationEnabled) {
        delayCompensationSamples_ += static_cast<unsigned int>(remoteMasterInserts_.size()) *
            remoteDspCrossingLatencySamples(settings_.remoteDspServer, maxBlockSize);
    }
    projectRenderState_.remoteMasterInserts = [this](std::vector<float>& block) {
        return processRemoteMasterInsertsLocked(block);
    };
    message_ = "마스터 인서트 " + std::to_string(remoteMasterInserts_.size()) +
               "개를 " + remoteMasterMode_ + " 노드로 배정했습니다.";
}

// Called from the renderer on the render thread, with mutex_ held by the caller.
bool NeuracoustDspEngine::processRemoteMasterInsertsLocked(std::vector<float>& interleavedStereo) {
    if (remoteMasterInserts_.empty() || interleavedStereo.empty()) {
        return false;
    }
    auto settings = remoteDspSettingsForMode(settings_.remoteDspServer, remoteMasterMode_);
    settings.channelCount = 2;
    settings.frameCount = static_cast<uint16_t>(std::min<size_t>(interleavedStereo.size() / 2u, 1024u));
    settings.sampleRate = settings_.sampleRate;
    const uint16_t networkBufferFrames =
        std::max<uint16_t>(40u, std::min<uint16_t>(1024u, settings.networkBufferFrames));
    settings.networkBufferFrames = networkBufferFrames;
    const double bufferLatencyMs = settings_.sampleRate > 0.0
        ? (static_cast<double>(networkBufferFrames) * 1000.0 / settings_.sampleRate)
        : 2.7;
    settings.timeoutMs = std::max<int>(8, static_cast<int>(std::ceil(bufferLatencyMs + 5.0)));

    // Serial, in slot order — the same order the local chain would run. Any stage that misses
    // returns false and the WHOLE block runs locally instead: half-remote processing would apply
    // some of the chain twice.
    for (auto& stage : remoteMasterInserts_) {
        settings.loadedPluginIdHint = stage.moduleId;
        const auto exchangeStart = std::chrono::steady_clock::now();
        const bool exchanged =
            stage.stream != nullptr &&
            stage.stream->process(settings, interleavedStereo, stage.parameters, remoteMasterScratch_) &&
            remoteMasterScratch_.size() == interleavedStereo.size();
        remoteWaitUsBlockAccum_ += std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - exchangeStart).count();
        if (!exchanged) {
            return false;
        }
        interleavedStereo = remoteMasterScratch_;
        recordRemoteDspRoundTripLocked(stage.stream->status().averageRoundTripMs);
    }
    return true;
}

// Work out which channels run their console strip elsewhere, and hand the renderer a detour for
// them. Keyed by track name, which is also the route name the renderer asks about.
void NeuracoustDspEngine::prepareRemoteConsoleStripsLocked(int maxBlockSize) {
    std::map<std::string, RemoteConsoleStrip> next;
    activeRemoteConsoleStripCount_ = 0;
    for (const auto& track : projectPlan_.tracks) {
        // The master bus is its own job: a session commonly keeps the channels local and sends
        // only the master to the appliance, or the reverse.
        const bool isMaster = track.trackType == "master" || track.name == "Master";
        const std::string& projectRole = isMaster ? settings_.remoteDspServer.roleMaster
                                                  : settings_.remoteDspServer.roleChannelStrip;
        const std::string mode = remoteDspModeForRole(
            settings_.remoteDspServer,
            effectiveDspMachine(track.consoleDspMachine, projectRole));
        if (!remoteDspModeAvailable(settings_.remoteDspServer, mode)) {
            continue;
        }
        // Membership is the ASSIGNMENT, not the module lamps. An idle assigned strip streams a
        // passthrough — that wire cost buys clickless lamp toggles: skipping idle strips made
        // every first-module toggle a local↔remote handoff (plus a delay-compensation step),
        // which was the digital click on NDS strips. Assignment changes go through the declicked
        // reconcile, so entering/leaving the map lands in silence.
        const auto& c = track.consoleChannel;
        RemoteConsoleStrip strip;
        strip.mode = mode;
        // Pool distribution: NDS strips round-robin across [primary] + ndsPoolHosts, so any
        // number of appliances carry the strips as ONE machine. "" = the mode's default host.
        if (mode == "nds" && !settings_.remoteDspServer.ndsPoolHosts.empty()) {
            const auto& pool = settings_.remoteDspServer.ndsPoolHosts;
            const size_t slot = activeRemoteConsoleStripCount_ % (pool.size() + 1u);
            strip.hostOverride = slot == 0u ? std::string{} : pool[slot - 1u];
        }
        for (const auto& parameter : consoleChannelParameterValues(c)) {
            strip.parameters.push_back({static_cast<uint32_t>(parameter.index), parameter.normalized});
        }
        // Reuse the running stream for a channel that is still on the same machine — rebuilding it
        // would re-handshake and drop a block for an unrelated edit elsewhere in the project.
        // "Same machine" includes the pool assignment, or a membership change would keep a
        // stream pointed at the wrong box.
        auto existing = remoteConsoleStrips_.find(track.name);
        strip.stream = (existing != remoteConsoleStrips_.end() && existing->second.mode == mode &&
                        existing->second.hostOverride == strip.hostOverride)
                           ? std::move(existing->second.stream)
                           : std::make_unique<RemoteDspAsyncStream>();
        if (strip.stream == nullptr) {
            strip.stream = std::make_unique<RemoteDspAsyncStream>();
        }
        next.emplace(track.name, std::move(strip));
        ++activeRemoteConsoleStripCount_;
    }
    remoteConsoleStrips_ = std::move(next);
    remoteStripGr_.clear();
    projectRenderState_.remoteConsoleGr = remoteConsoleStrips_.empty()
        ? std::function<bool(const std::string&, float&, float&)>()
        : [this](const std::string& route, float& compGr, float& gateGr) {
              const auto found = remoteStripGr_.find(route);
              if (found == remoteStripGr_.end()) {
                  return false;
              }
              compGr = found->second.first;
              gateGr = found->second.second;
              return true;
          };
    if (getenv("NC_DIAG_REMOTE") != nullptr) {
        fprintf(stderr, "[nc-remote] strips prepared: %zu (nds=%d host=%s role=%s)\n",
                remoteConsoleStrips_.size(), settings_.remoteDspServer.ndsEnabled ? 1 : 0,
                settings_.remoteDspServer.ndsHost.c_str(),
                settings_.remoteDspServer.roleChannelStrip.c_str());
    }
    realignRemoteConsoleStripsLocked(maxBlockSize);
    if (remoteConsoleStrips_.empty()) {
        projectRenderState_.remoteConsoleStrip = nullptr;
        return;
    }
    // Installed once, not per block: the hook is called from the render thread, and rebuilding a
    // std::function there would allocate.
    projectRenderState_.remoteConsoleStrip =
        [this](const std::string& routeName, std::vector<float>& block) {
            return processRemoteConsoleStripLocked(routeName, block);
        };
}

// A channel whose strip runs on a node comes back a crossing later than one processed here. If
// nothing says so, that channel simply sits behind the rest of the mix — the timing smears and
// anything sharing a source with it phases.
//
// The renderer already has the fix, the same one every DAW uses when a linear-phase EQ makes one
// track late: each route declares its latency, the longest path wins, and every shorter route is
// delayed to match. Track inserts were declared there; the console strips were not. This adds the
// crossing to each remote strip's route and re-derives the compensation for all of them.
//
// Done here rather than in buildMixerGraph because the graph is built from the project alone and
// the crossing cost comes from the network settings — and because the offline bounce, which shares
// the graph and never runs a remote strip, must not be handed a compensation for a trip it does
// not take.
void NeuracoustDspEngine::realignRemoteConsoleStripsLocked(int maxBlockSize) {
    if (!settings_.delayCompensationEnabled) {
        return;
    }
    const unsigned int crossing =
        remoteConsoleStrips_.empty()
            ? 0u
            : remoteDspCrossingLatencySamples(settings_.remoteDspServer, maxBlockSize);

    // Each route's total latency: what the graph already knew, plus a crossing if this channel's
    // strip is leaving the host.
    unsigned int longestPath = 0;
    std::map<std::string, unsigned int> pathLatency;
    for (const auto& route : projectPlan_.mixerGraph.routes) {
        if (!route.audioCarrying) continue;
        const unsigned int total =
            route.pathLatencySamples +
            (remoteConsoleStrips_.count(route.name) != 0 ? crossing : 0u);
        pathLatency[route.name] = total;
        longestPath = std::max(longestPath, total);
    }
    for (const auto& [routeName, total] : pathLatency) {
        if (total >= longestPath) {
            projectPlan_.routeDelayCompensationSamples.erase(routeName);
        } else {
            projectPlan_.routeDelayCompensationSamples[routeName] = longestPath - total;
        }
    }
    projectPlan_.delayCompensationSamples =
        std::max(projectPlan_.delayCompensationSamples, longestPath);
    delayCompensationSamples_ = std::max(delayCompensationSamples_, longestPath);
}

// Called from the renderer, on the render thread, with mutex_ already held by the caller.
bool NeuracoustDspEngine::processRemoteConsoleStripLocked(const std::string& routeName,
                                                          std::vector<float>& interleavedStereo) {
    auto found = remoteConsoleStrips_.find(routeName);
    if (found == remoteConsoleStrips_.end() || found->second.stream == nullptr ||
        interleavedStereo.empty()) {
        return false;
    }
    auto settings = remoteDspSettingsForMode(settings_.remoteDspServer, found->second.mode);
    if (!found->second.hostOverride.empty()) {
        // Pool member: this strip streams to ITS appliance, not the mode's default.
        settings.host = found->second.hostOverride;
        applyRemoteDspHostPort(settings);
    }
    settings.channelCount = 2;
    settings.frameCount = static_cast<uint16_t>(std::min<size_t>(interleavedStereo.size() / 2u, 1024u));
    settings.sampleRate = settings_.sampleRate;
    settings.loadedPluginIdHint = "na.neuracoust.console.channel";
    const uint16_t networkBufferFrames =
        std::max<uint16_t>(40u, std::min<uint16_t>(1024u, settings.networkBufferFrames));
    settings.networkBufferFrames = networkBufferFrames;
    const double bufferLatencyMs = settings_.sampleRate > 0.0
        ? (static_cast<double>(networkBufferFrames) * 1000.0 / settings_.sampleRate)
        : 2.7;
    settings.timeoutMs = std::max<int>(8, static_cast<int>(std::ceil(bufferLatencyMs + 5.0)));

    const auto exchangeStart = std::chrono::steady_clock::now();
    const bool exchanged = found->second.stream->process(settings,
                                                         interleavedStereo,
                                                         found->second.parameters,
                                                         remoteConsoleProcessedBlock_) &&
                           remoteConsoleProcessedBlock_.size() == interleavedStereo.size();
    remoteWaitUsBlockAccum_ += std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - exchangeStart).count();
    if (!exchanged) {
        // Warming up or the node missed a block. Returning false runs the LOCAL strip for this
        // block instead of dropping it — the same processor, so the channel keeps its sound
        // rather than going momentarily dry.
        if (getenv("NC_DIAG_REMOTE") != nullptr) {
            static int logged = 0;
            if (logged < 20 && (logged++ % 4) == 0) {
                fprintf(stderr, "[nc-remote] strip %s fallback: %s\n", routeName.c_str(),
                        found->second.stream->status().message.c_str());
            }
        }
        return false;
    }
    {
        // GR telemetry rode the reply (ABI-2 console module): keep the latest per strip so the
        // renderer can feed the needles the local processor no longer runs to feed.
        const auto streamStatus = found->second.stream->status();
        if (streamStatus.meterCount >= 2u) {
            remoteStripGr_[routeName] = {streamStatus.meters[0], streamStatus.meters[1]};
        }
    }
    interleavedStereo = remoteConsoleProcessedBlock_;
    recordRemoteDspRoundTripLocked(found->second.stream->status().averageRoundTripMs);
    return true;
}

bool NeuracoustDspEngine::monitorDspModeRequestsRemoteLocked(const std::string& mode) const {
    // Each remote machine has its own switch, so "nds" asks whether the appliance is on and
    // "external" whether the borrowed computer is. Gating both on one flag meant turning the
    // external node off also silently disabled NDS.
    return remoteDspModeAvailable(settings_.remoteDspServer, mode);
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

void NeuracoustDspEngine::updateTrackSendGain(const std::string& trackName, int slot, float gainDb) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& track : projectPlan_.tracks) {
        if (track.name != trackName) continue;
        if (slot >= 0 && static_cast<size_t>(slot) < track.sends.size()) {
            track.sends[static_cast<size_t>(slot)].gainDb = gainDb;
        }
        return;
    }
}

void NeuracoustDspEngine::updateMonitorEq(const std::vector<MonitorEqBandState>& bands) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectDocument shim;
    shim.monitorEqBands = bands;
    configureMonitorEqLocked(shim, sampleRateForStatus_.load(std::memory_order_relaxed));
}

void NeuracoustDspEngine::updateMonitorFir(const ResponseCurve& curveDb, int numTaps) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (curveDb.empty() || numTaps < 4) {
        monitorFir_.clear();
        return;
    }
    monitorFir_.designFromCurve(sampleRateForStatus_.load(std::memory_order_relaxed), curveDb, numTaps);
}

void NeuracoustDspEngine::updateInterfaceModeler(const std::vector<double>& harmonics, double mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    interfaceModeler_.configure(harmonics, mix);
}

int NeuracoustDspEngine::monitorFirLatencySamples() const {
    return monitorFir_.latencySamples();
}

double NeuracoustDspEngine::monitorEqMagnitudeDb(double frequencyHz) const {
    if (monitorFir_.active()) {
        return monitorFir_.magnitudeDb(frequencyHz, sampleRateForStatus_.load(std::memory_order_relaxed));
    }
    return monitorEq_.magnitudeDb(frequencyHz);
}

void NeuracoustDspEngine::configureMonitorEqLocked(const ProjectDocument& project, double sampleRate) {
    auto typeFromString = [](const std::string& s) {
        if (s == "low_shelf") return EqBandType::LowShelf;
        if (s == "high_shelf") return EqBandType::HighShelf;
        if (s == "high_pass") return EqBandType::HighPass;
        if (s == "low_pass") return EqBandType::LowPass;
        if (s == "notch") return EqBandType::Notch;
        return EqBandType::Peaking;
    };
    std::vector<EqBandSpec> specs;
    specs.reserve(project.monitorEqBands.size());
    for (const auto& band : project.monitorEqBands) {
        specs.push_back({band.enabled, typeFromString(band.type), band.frequencyHz, band.gainDb, band.q});
    }
    monitorEq_.configure(std::max(1.0, sampleRate), specs);
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
    // Resolve WHICH machine first: the appliance and the borrowed computer have different
    // addresses, and streaming the monitor to the wrong one is silence, not a fallback.
    RemoteDspServerSettings remoteSettings =
        remoteDspSettingsForMode(settings_.remoteDspServer, settings_.monitorDspPathMode);
    const uint16_t networkBufferFrames = static_cast<uint16_t>(std::max<uint16_t>(40u, std::min<uint16_t>(1024u, remoteSettings.networkBufferFrames)));
    remoteSettings.channelCount = 2;
    remoteSettings.frameCount = static_cast<uint16_t>(std::min<size_t>(interleavedStereo.size() / 2u, 1024u));
    remoteSettings.sampleRate = settings_.sampleRate;
    remoteSettings.networkBufferFrames = networkBufferFrames;
    remoteSettings.timeoutMs = (settings_.monitorDspPathMode == "external" ||
                                settings_.monitorDspPathMode == "nds" ||
                                settings_.monitorDspPathMode == "remote_external") ? 12 : 8;

    const auto exchangeStart = std::chrono::steady_clock::now();
    const bool exchanged =
        remoteMonitorDspStream_.process(remoteSettings, interleavedStereo, remoteDspProcessedBlock_) &&
        remoteDspProcessedBlock_.size() == interleavedStereo.size();
    remoteWaitUsBlockAccum_ += std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - exchangeStart).count();
    if (exchanged) {
        interleavedStereo = remoteDspProcessedBlock_;
        remoteDspMonitorActive_ = true;
        remoteDspConsecutiveMissBlocks_ = 0;
        remoteDspLastExchangeSteadyUs_.store(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
        const auto streamStatus = remoteMonitorDspStream_.status();
        remoteDspRoundTripMs_ = streamStatus.averageRoundTripMs;
        remoteDspAverageRoundTripJitterUs_ = streamStatus.averageRoundTripJitterUs;
        remoteDspMaxRoundTripJitterUs_ = streamStatus.maxRoundTripJitterUs;
        message_ = "Monitor audio routed through 누라쿠스트 DSP 서버 async stream (" +
            std::to_string(remoteDspRoundTripMs_) + " ms, buffered " +
            std::to_string(streamStatus.queuedOutputBlocks) + " blocks).";
        return true;
    }

    // A failed exchange must not keep re-publishing the last good numbers. Doing that is how the
    // dock ends up showing a round trip and a jitter figure for a link that is not carrying any
    // audio at all — a reading of the past, presented as the present. A single miss is normal
    // while the stream warms, so the numbers only go blank once the misses are sustained
    // (32 blocks, under 0.2 s at 256/48k).
    const auto streamStatus = remoteMonitorDspStream_.status();
    if (++remoteDspConsecutiveMissBlocks_ >= 32) {
        resetRemoteDspTelemetryLocked();
    } else if (streamStatus.running) {
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
    remoteDspConsecutiveMissBlocks_ = 0;
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
    // Capture-active gates the monitor MIX. It must key on the *listening* state, NOT the armed
    // state: while armed but auditioning the master (holds the apps muted) the tap keeps running,
    // but its captured audio must NOT be mixed into the monitor — otherwise the record-monitor mix
    // path leaks the tapped apps on top of the master. The tap muting the apps at their own output
    // is what silences them; the monitor simply plays the master alone.
    inputMonitorCaptureActive_.store(lowLatencyRecordMonitoringActive_ || listenSource,
                                     std::memory_order_relaxed);
    if (!lowLatencyRecordMonitoringActive_ && !listenSource &&
        !talkbackCaptureActive_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> inputLock(inputMonitorMutex_);
        physicalInputMonitoringActive_ = false;
        inputMonitorBuffer_.clear();
        talkbackMonoBuffer_.clear();
        listenSourcePrerolling_ = true;   // next engagement pre-rolls a fresh cushion
        listenReadPosFrames_ = 0.0;
        listenResampleRatio_ = 1.0;
        listenSmoothedDepth_ = 0.0;
        inputPeak_ = 0.0f;
        inputPeakLeft_ = 0.0f;
        inputPeakRight_ = 0.0f;
        inputMonitorChannels_ = 0;
        physicalInputMonitoringActiveForStatus_.store(false, std::memory_order_relaxed);
        inputMonitorChannelsForStatus_.store(0, std::memory_order_relaxed);
        inputPeakForStatus_.store(0.0f, std::memory_order_relaxed);
    }
}

void NeuracoustDspEngine::beginGraphChangeDeclick() {
    // Ask the render to fade the monitor to silence, then wait (bounded) for it to get there so the
    // structural swap lands in silence. Does NOT hold the mutex — the render must be free to run
    // and advance the envelope. If no render is running (engine stopped), the wait just times out
    // and we proceed: no audio means no click to hide.
    graphChangeDeclick_.store(1, std::memory_order_relaxed);   // FadingOut
    for (int i = 0; i < 25; ++i) {                             // up to ~50 ms
        if (graphChangeDeclick_.load(std::memory_order_relaxed) == 2) break;   // Silent
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int NeuracoustDspEngine::routeDelayCompensationSamplesFor(const std::string& routeName) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!settings_.delayCompensationEnabled) return 0;
    const auto it = projectPlan_.routeDelayCompensationSamples.find(routeName);
    return it == projectPlan_.routeDelayCompensationSamples.end() ? 0 : static_cast<int>(it->second);
}

void NeuracoustDspEngine::endGraphChangeDeclick() {
    // Hold silence for ~40 ms AFTER the swap before fading in: CoreAudio is double-buffered, so the
    // new graph's first (discontinuous) output block plays ~a buffer later — this keeps that under
    // silence instead of revealing it mid fade-in.
    graphChangeDeclickHold_ = static_cast<int>(std::max(1.0, settings_.sampleRate) * 0.040);
    graphChangeDeclick_.store(3, std::memory_order_release);   // PostSwapHold → render counts down → FadingIn
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
    // Graph-change declick envelope: fade out fast (~4 ms) to reach silence for the swap, fade back
    // in a touch slower (~12 ms) after. A separate factor multiplied on top of the normal gain.
    const float declickOutStep = 1.0f / std::max(1.0f, static_cast<float>(settings_.sampleRate) * 0.004f);
    const float declickInStep = 1.0f / std::max(1.0f, static_cast<float>(settings_.sampleRate) * 0.012f);
    for (size_t index = 0; index + 1 < interleavedStereo.size(); index += 2) {
        // Advance the declick envelope per sample (state re-read each sample so transitions apply now).
        const int declickState = graphChangeDeclick_.load(std::memory_order_acquire);
        if (declickState == 1) {                 // FadingOut
            graphChangeDeclickGain_ -= declickOutStep;
            if (graphChangeDeclickGain_ <= 0.0f) {
                graphChangeDeclickGain_ = 0.0f;
                graphChangeDeclick_.store(2, std::memory_order_relaxed);   // Silent → swap may proceed
            }
        } else if (declickState == 2) {          // Silent (holding until endGraphChangeDeclick)
            graphChangeDeclickGain_ = 0.0f;
        } else if (declickState == 3) {          // PostSwapHold: stay silent while the new graph settles
            graphChangeDeclickGain_ = 0.0f;
            if (--graphChangeDeclickHold_ <= 0) {
                graphChangeDeclick_.store(4, std::memory_order_relaxed);
            }
        } else if (declickState == 4) {          // FadingIn
            graphChangeDeclickGain_ += declickInStep;
            if (graphChangeDeclickGain_ >= 1.0f) {
                graphChangeDeclickGain_ = 1.0f;
                graphChangeDeclick_.store(0, std::memory_order_relaxed);   // done
            }
        } else {
            graphChangeDeclickGain_ = 1.0f;
        }
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
        interleavedStereo[index] = left * monitorStationGainSmoothed_ * graphChangeDeclickGain_;
        interleavedStereo[index + 1] = right * monitorStationGainSmoothed_ * graphChangeDeclickGain_;
    }
}

// DIAG (temporary): reference-tap FIFO health to a file readable from an `open`-launched bundle
// (stderr is not captured when LaunchServices starts the app). Opened once, truncating.
namespace { FILE* refDiagLog() { static FILE* f = fopen("/tmp/dw_ref_diag.log", "w"); return f; } }

void NeuracoustDspEngine::beginRecording(int source, int channelOffset, int channels, int sampleRate,
                                         const std::string& busRouteName) {
    // The bus name is read on the render thread under mutex_, so write it under mutex_ FIRST —
    // the same mutex_ -> recordMutex_ order the render's capture hook takes.
    {
        std::lock_guard<std::mutex> graphLock(mutex_);
        recordBusRouteName_ = source == 3 ? busRouteName : std::string{};
    }
    std::lock_guard<std::mutex> lock(recordMutex_);
    recordChannelOffset_ = std::max(0, channelOffset);
    recordChannels_ = std::max(1, std::min(2, channels));
    const int sr = sampleRate > 0 ? sampleRate : 48000;
    recordTake_ = std::make_unique<RecordingTake>(recordChannels_, sr);
    recordLivePeaks_.clear();
    recordLivePeaks_.reserve(static_cast<size_t>(sr / kRecordPeakSamples) * 300u * 2u);   // ~5 min, L/R
    recordPeakAccumL_ = 0.0f;
    recordPeakAccumR_ = 0.0f;
    recordPeakFill_ = 0;
    recordSource_.store(source, std::memory_order_relaxed);
    recordingActive_.store(true, std::memory_order_release);
}

// One coarse L/R abs-max peak per kRecordPeakSamples input samples. Caller holds recordMutex_.
void NeuracoustDspEngine::accumulateRecordPeaksLocked(const float* interleaved, int64_t frames, int channels) {
    for (int64_t f = 0; f < frames; ++f) {
        const float l = std::fabs(interleaved[static_cast<size_t>(f) * channels]);
        const float r = channels >= 2 ? std::fabs(interleaved[static_cast<size_t>(f) * channels + 1]) : l;
        recordPeakAccumL_ = std::max(recordPeakAccumL_, l);
        recordPeakAccumR_ = std::max(recordPeakAccumR_, r);
        if (++recordPeakFill_ >= kRecordPeakSamples) {
            recordLivePeaks_.push_back(recordPeakAccumL_);
            recordLivePeaks_.push_back(recordPeakAccumR_);
            recordPeakAccumL_ = 0.0f;
            recordPeakAccumR_ = 0.0f;
            recordPeakFill_ = 0;
        }
    }
}

double NeuracoustDspEngine::recordLiveSeconds() const {
    std::lock_guard<std::mutex> lock(recordMutex_);
    return recordTake_ ? recordTake_->durationSeconds() : 0.0;
}

int NeuracoustDspEngine::recordLivePeakCount() const {
    std::lock_guard<std::mutex> lock(recordMutex_);
    return static_cast<int>(recordLivePeaks_.size() / 2);
}

int NeuracoustDspEngine::recordChannels() const {
    std::lock_guard<std::mutex> lock(recordMutex_);
    return recordChannels_;
}

// Copy only buckets [fromBucket, end) so the read lock is O(new peaks) — never the whole take.
int NeuracoustDspEngine::copyRecordLivePeaksSince(int fromBucket, float* outLR, int maxBuckets) const {
    if (outLR == nullptr || maxBuckets <= 0 || fromBucket < 0) return 0;
    std::lock_guard<std::mutex> lock(recordMutex_);
    const int total = static_cast<int>(recordLivePeaks_.size() / 2);
    if (fromBucket >= total) return 0;
    const int n = std::min(maxBuckets, total - fromBucket);
    std::copy(recordLivePeaks_.begin() + static_cast<std::ptrdiff_t>(fromBucket) * 2,
              recordLivePeaks_.begin() + static_cast<std::ptrdiff_t>(fromBucket + n) * 2,
              outLR);
    return n;
}

bool NeuracoustDspEngine::endRecording(const std::string& path, int bitDepth, std::string& error,
                                       double& outDurationSeconds, int& outChannels) {
    std::unique_ptr<RecordingTake> take;
    {
        std::lock_guard<std::mutex> lock(recordMutex_);
        recordingActive_.store(false, std::memory_order_release);
        recordSource_.store(0, std::memory_order_relaxed);
        take = std::move(recordTake_);
    }
    if (!take || take->frameCount() == 0) {
        error = "녹음된 오디오가 없습니다.";
        return false;
    }
    outDurationSeconds = take->durationSeconds();
    outChannels = take->channels();
    return take->saveWav(path, bitDepth, error);
}

void NeuracoustDspEngine::cancelRecording() {
    std::lock_guard<std::mutex> lock(recordMutex_);
    recordingActive_.store(false, std::memory_order_release);
    recordSource_.store(0, std::memory_order_relaxed);
    recordTake_.reset();
    recordLivePeaks_.clear();
    recordPeakAccumL_ = 0.0f;
    recordPeakAccumR_ = 0.0f;
    recordPeakFill_ = 0;
}

void NeuracoustDspEngine::logReferenceRates(double tapRate, double outRate) {
    if (FILE* df = refDiagLog()) {
        fprintf(df, "tap-aggregate rate=%.0f  engine output rate=%.0f\n", tapRate, outRate);
        fflush(df);
    }
}

void NeuracoustDspEngine::mixInputMonitorLocked(int64_t frameCount, std::vector<float>& interleavedStereo) {
    const bool monitorCaptureActive = inputMonitorCaptureActive_.load(std::memory_order_relaxed);
    const bool talkbackActive = talkbackCaptureActive_.load(std::memory_order_relaxed);
    const bool talkbackToMonitor =
        talkbackActive &&
        (settings_.monitorStationTalkbackRoute == "monitor_bus" ||
         settings_.monitorStationTalkbackRoute == "all");
    const bool talkbackToListenRoom =
        talkbackActive &&
        (settings_.monitorStationTalkbackRoute == "listen_room" ||
         settings_.monitorStationTalkbackRoute == "all");
    // Talkback to the remote listeners rides its own path (publishListenRoomLocked adds it
    // after the monitor mix), so the stash is rebuilt every block and left empty otherwise.
    // "all" already reaches listeners through the forwarded monitor mix, so it is NOT stashed
    // (avoids double-counting) — only a listen_room-exclusive route needs the separate inject.
    const bool stashTalkbackForListenRoom = talkbackToListenRoom && !talkbackToMonitor;
    talkbackListenRoomBlock_.clear();
    const bool listenSource = listenSourceActive_.load(std::memory_order_relaxed);
    // Record-monitor the tap: when recording a "다른 앱" (tap) input track while the monitor is on
    // the MASTER, mix the tapped source into the monitor ON TOP of the master so the engineer hears
    // what they are capturing (the tap mutes the apps' own output, so this is the only way to hear
    // it). Unlike A/B listening this does NOT silence the master.
    // Auto-input style: the tapped source is heard ONLY while actively punched in (tapInputMonitorActive_
    // set by the Record punch), not merely because a tap track is armed or capturing in the background —
    // on plain playback you hear the recorded tape, not the live input.
    const bool recTapMonitor = !listenSource &&
        (tapInputMonitorActive_.load(std::memory_order_relaxed) ||
         tapInputHoldActive_.load(std::memory_order_relaxed));
    const bool feedReference = listenSource || recTapMonitor;
    if ((!monitorCaptureActive && !talkbackToMonitor && !stashTalkbackForListenRoom && !recTapMonitor) ||
        frameCount <= 0 || interleavedStereo.empty()) {
        return;
    }
    // Source monitoring is EXCLUSIVE: while a source is A/B-selected you hear only it, never the
    // DAW master. Silence the master here — before the buffer lock and before the resample
    // pre-roll — so switching to the source never lets the master leak through during the
    // ~1 s the input device takes to open and prime (you get brief silence, then the source).
    // The master transport keeps running underneath, so switching back resumes it in place.
    // Record-monitoring (recTapMonitor) deliberately keeps the master and adds the tap on top.
    if (listenSource) {
        std::fill(interleavedStereo.begin(), interleavedStereo.end(), 0.0f);
    }
    std::unique_lock<std::mutex> inputLock(inputMonitorMutex_, std::try_to_lock);
    if (!inputLock.owns_lock()) {
        return;
    }
    // --- Reference feed (BlackHole): varispeed-resample to the output clock -----------
    // The input runs on its own clock, so a 1:1 copy drifts and stutters. Instead read the
    // FIFO with a fractional position advanced by listenResampleRatio_, and nudge that ratio
    // to hold the FIFO near a target depth. The ratio self-tunes to the true input/output
    // rate — correcting both drift and a 44.1↔48 mismatch — with no pre-roll or underrun.
    if (!feedReference) {
        referenceFeedActive_ = false;
    }
    if (feedReference) {
        // Re-prime a fresh cushion on the feeding-start edge (A/B into reference, or record-monitor
        // begins) or on an explicit A/B engage — drop stale audio so it never plays a blip.
        if (!referenceFeedActive_ || listenJustEngaged_.exchange(false, std::memory_order_relaxed)) {
            listenSourcePrerolling_ = true;
            listenReadPosFrames_ = 0.0;
            referenceBuffer_.clear();
        }
        referenceFeedActive_ = true;
        const int64_t availFrames = static_cast<int64_t>(referenceBuffer_.size() / 2);
        // Deep cushion (~213 ms): reference monitoring tolerates latency, and a deep FIFO lets
        // the ratio control be slow (stable pitch) yet never underrun on the driver's bursty,
        // core-isolation-starved delivery. Cap (pushInputMonitorInterleaved) is deeper still.
        const double targetFrames = static_cast<double>(std::max<int64_t>(16, maxBlockSize_)) * 40.0;
        // Prime the cushion once, then never stop reading.
        if (listenSourcePrerolling_) {
            if (availFrames < static_cast<int64_t>(targetFrames)) {
                inputPeak_ *= 0.96f;
                inputPeakForStatus_.store(inputPeak_, std::memory_order_relaxed);
                return;
            }
            listenSourcePrerolling_ = false;
            listenReadPosFrames_ = 0.0;
            listenResampleRatio_ = 1.0;
            listenSmoothedDepth_ = static_cast<double>(availFrames);
        }
        // (master already silenced above; the resampler now adds the source on top of silence)
        const double depthFrames = static_cast<double>(availFrames) - listenReadPosFrames_;
        // PROPORTIONAL control (not integral). The FIFO depth is already the integral of the
        // rate mismatch, so an integral controller on top made a double integrator — a pure
        // oscillator that drove the depth empty↔full and swung the ratio ±10% (severe pitch
        // wobble + dropouts). A proportional law is unconditionally stable: the ratio settles
        // at the true input/output rate with the depth parked a little off target, and holds
        // there. Smooth the depth first so burst jitter does not reach the ratio (pitch waver).
        listenSmoothedDepth_ += 0.02 * (depthFrames - listenSmoothedDepth_);
        const double err = (listenSmoothedDepth_ - targetFrames) / targetFrames;
        const double targetRatio = std::clamp(1.0 + 0.02 * err, 0.94, 1.06);
        // Slew-limit the ratio so it can never jump: a restart (e.g. toggling core isolation)
        // re-primes the FIFO, and the depth transient would otherwise swing the ratio audibly
        // for an instant. Capping the change to ~1 cent/s makes every pitch move a slow,
        // inaudible glide; steady-state tracking of the tiny clock offset is unaffected.
        const double maxStep = 5.0e-6;
        listenResampleRatio_ += std::clamp(targetRatio - listenResampleRatio_, -maxStep, maxStep);
        // DIAG: throttled steady-state report — if the ratio parks at a clamp rail (0.94/1.06) the
        // tap/output nominal rates differ by more than the resampler can track → periodic underruns.
        static int64_t s_diagCounter = 0;
        static int64_t s_underruns = 0;
        bool s_dryThisBlock = false;
        for (int64_t f = 0; f < frameCount; ++f) {
            const int64_t i0 = static_cast<int64_t>(listenReadPosFrames_);
            const int64_t i1 = i0 + 1;
            if (i1 >= availFrames) {
                s_dryThisBlock = true;
                break;   // genuinely dry this block; ratio control will refill the cushion
            }
            const float frac = static_cast<float>(listenReadPosFrames_ - static_cast<double>(i0));
            const float l = referenceBuffer_[static_cast<size_t>(i0) * 2u] * (1.0f - frac) +
                            referenceBuffer_[static_cast<size_t>(i1) * 2u] * frac;
            const float r = referenceBuffer_[static_cast<size_t>(i0) * 2u + 1u] * (1.0f - frac) +
                            referenceBuffer_[static_cast<size_t>(i1) * 2u + 1u] * frac;
            // Add raw — the monitor DSP path colours the summed block downstream, so running
            // the speaker sim per input sample here was redundant (double processing) and the
            // biggest per-block cost. The reference feed gets the same monitor colour as the mix.
            interleavedStereo[static_cast<size_t>(f) * 2u] += l;
            interleavedStereo[static_cast<size_t>(f) * 2u + 1u] += r;
            listenReadPosFrames_ += listenResampleRatio_;
        }
        if (s_dryThisBlock) {
            ++s_underruns;
            // Publish it too: this is the tap dropout the render wake-jitter meter can never see,
            // because the render thread woke on time — it was the CAPTURE side that ran dry.
            referenceUnderrunBlocks_.fetch_add(1, std::memory_order_relaxed);
        }
        if (++s_diagCounter % 200 == 0) {
            if (FILE* df = refDiagLog()) {
                fprintf(df, "ref-fifo: ratio=%.5f target=%.5f depth=%.0f/%.0f avail=%lld under=%lld blk=%lld\n",
                        listenResampleRatio_, targetRatio, listenSmoothedDepth_, targetFrames,
                        (long long)availFrames, (long long)s_underruns, (long long)maxBlockSize_);
                fflush(df);
            }
        }
        const int64_t consumedFrames = static_cast<int64_t>(listenReadPosFrames_);
        if (consumedFrames > 0) {
            const size_t consumedSamples = std::min(referenceBuffer_.size(),
                                                    static_cast<size_t>(consumedFrames) * 2u);
            referenceBuffer_.erase(referenceBuffer_.begin(),
                                   referenceBuffer_.begin() + static_cast<std::ptrdiff_t>(consumedSamples));
            listenReadPosFrames_ -= static_cast<double>(consumedFrames);
        }
        inputPeak_ *= 0.96f;
        inputPeakLeft_ *= 0.96f;
        inputPeakRight_ *= 0.96f;
        if (referenceBuffer_.empty() && inputPeak_ < 0.0001f) {
            inputPeak_ = 0.0f;
        inputPeakLeft_ = 0.0f;
        inputPeakRight_ = 0.0f;
            inputPeakLeft_ = 0.0f;
            inputPeakRight_ = 0.0f;
        }
        inputPeakForStatus_.store(inputPeak_, std::memory_order_relaxed);
        inputPeakLeftForStatus_.store(inputPeakLeft_, std::memory_order_relaxed);
        inputPeakRightForStatus_.store(inputPeakRight_, std::memory_order_relaxed);
        inputMonitorChannelsForStatus_.store(inputMonitorChannels_, std::memory_order_relaxed);
        return;
    }
    // --- Talkback / record-arm monitoring: low-latency 1:1 (unchanged) ----------------
    const bool unityPassthrough = talkbackToMonitor;
    // Who hears the mic on the local monitor bus. A listen_room-exclusive talkback route
    // (stashTalkbackForListenRoom) reaches only the remote listeners, never the monitor —
    // so the engineer does not hear their own talkback on the speakers (no feedback).
    const bool mixToMonitor = talkbackToMonitor || monitorCaptureActive;
    const size_t neededSamples = static_cast<size_t>(frameCount) * 2;
    const size_t availableSamples = std::min(neededSamples, inputMonitorBuffer_.size());
    if (stashTalkbackForListenRoom) {
        talkbackListenRoomBlock_.assign(neededSamples, 0.0f);   // full block, zero-padded
    }
    for (size_t index = 0; index + 1 < availableSamples; index += 2) {
        // Talkback is a mono mic on one chosen input channel, centered. Record-arm monitoring keeps
        // the true stereo input (the armed track's source, ch1/ch2).
        const size_t frameIdx = index / 2;
        const float talkMono = frameIdx < talkbackMonoBuffer_.size() ? talkbackMonoBuffer_[frameIdx] : 0.0f;
        if (mixToMonitor) {
            const auto [monitoredLeft, monitoredRight] = unityPassthrough
                ? std::pair<float, float>{talkMono, talkMono}
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
        if (stashTalkbackForListenRoom) {
            // Remote listeners get the raw mic at unity, unaffected by the engineer's
            // monitor gain/pan/DSP — a clean talkback voice, mono-centered, over the programme.
            talkbackListenRoomBlock_[index] = talkMono;
            talkbackListenRoomBlock_[index + 1] = talkMono;
        }
    }
    if (availableSamples > 0) {
        inputMonitorBuffer_.erase(inputMonitorBuffer_.begin(), inputMonitorBuffer_.begin() + static_cast<std::ptrdiff_t>(availableSamples));
        const size_t framesConsumed = std::min(availableSamples / 2, talkbackMonoBuffer_.size());
        if (framesConsumed > 0) {
            talkbackMonoBuffer_.erase(talkbackMonoBuffer_.begin(),
                                      talkbackMonoBuffer_.begin() + static_cast<std::ptrdiff_t>(framesConsumed));
        }
    }
    inputPeak_ *= 0.96f;
    if (inputMonitorBuffer_.empty() && inputPeak_ < 0.0001f) {
        inputPeak_ = 0.0f;
        inputPeakLeft_ = 0.0f;
        inputPeakRight_ = 0.0f;
    }
    inputPeakForStatus_.store(inputPeak_, std::memory_order_relaxed);
    inputMonitorChannelsForStatus_.store(inputMonitorChannels_, std::memory_order_relaxed);
}

void NeuracoustDspEngine::mixEditorInstrumentMonitorLocked(int64_t frameCount,
                                                           std::vector<float>& interleavedStereo) {
    if (!editorMonitorActive_.load(std::memory_order_relaxed) || frameCount <= 0 ||
        interleavedStereo.empty()) {
        return;
    }
    std::unique_lock<std::mutex> monitorLock(editorMonitorMutex_, std::try_to_lock);
    if (!monitorLock.owns_lock()) {
        return;
    }
    if (editorMonitorBuffer_.empty()) {
        return;
    }
    // Sit where the render instance would: the track's fader/pan/mute (and solo
    // elsewhere silencing it), then the master fader — the editor instance renders
    // raw plug-in output and knows nothing of the mixer.
    float gainDb = 0.0f;
    float pan = 0.0f;
    bool silenced = false;
    if (const TrackState* track = findTrack(projectPlan_, editorMonitorTrackName_)) {
        gainDb = track->volumeDb;
        pan = track->pan;
        if (track->muted) {
            silenced = true;
        }
        if (!track->solo) {
            for (const auto& other : projectPlan_.tracks) {
                if (other.solo && other.trackType != "master" && other.trackType != "monitor") {
                    silenced = true;
                    break;
                }
            }
        }
    }
    float masterGainDb = 0.0f;
    float masterPan = 0.0f;
    if (const TrackState* master = findTrack(projectPlan_, "Master")) {
        masterGainDb = master->volumeDb;
        masterPan = master->pan;
        if (master->muted) {
            silenced = true;
        }
    }
    const size_t neededSamples = static_cast<size_t>(frameCount) * 2u;
    const size_t availableSamples = std::min(neededSamples, editorMonitorBuffer_.size());
    if (!silenced) {
        for (size_t index = 0; index + 1 < availableSamples; index += 2) {
            auto [left, right] = applyStereoGainPan(editorMonitorBuffer_[index],
                                                    editorMonitorBuffer_[index + 1],
                                                    gainDb, pan);
            std::tie(left, right) = applyStereoGainPan(left, right, masterGainDb, masterPan);
            interleavedStereo[index] += left;
            interleavedStereo[index + 1] += right;
        }
    }
    if (availableSamples > 0) {
        editorMonitorBuffer_.erase(editorMonitorBuffer_.begin(),
                                   editorMonitorBuffer_.begin() + static_cast<std::ptrdiff_t>(availableSamples));
    }
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
        // Kept the same length as the peaks — the bridge's meter count is the MIN across all of
        // these arrays, so a short console array hides every track's meter.
        projectMeters_.trackConsoleGainReductionDb.push_back(0.0f);
        projectMeters_.trackConsoleGateGainReductionDb.push_back(0.0f);
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
    // Standard correlation-meter ballistics: the raw per-block value (~5 ms) jitters, so
    // integrate it with a ~100 ms time constant — stable to read, still quick to react.
    phaseCorrelationBallistics_ += 0.05f * (correlation - phaseCorrelationBallistics_);
    phaseCorrelation_.store(phaseCorrelationBallistics_);
    spectrumLow_.store(std::min(1.0f, static_cast<float>(std::sqrt(lowEnergy / frames))));
    spectrumMid_.store(std::min(1.0f, static_cast<float>(std::sqrt(midEnergy / frames))));
    spectrumHigh_.store(std::min(1.0f, static_cast<float>(std::sqrt(highEnergy / frames))));

    // Spectrum analyzer taps the master output (pre-monitor), captured above; everything else in this
    // function still reflects the signal passed in.
    updateSpectrum(spectrumSourceBlock_.empty() ? interleavedStereo : spectrumSourceBlock_);

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
    // Talkback routed exclusively to the listen room is summed on top of the programme here,
    // so remote listeners hear the engineer over the music while the local monitor stays dry.
    // The stash (filled in mixInputMonitorLocked) is empty when talkback is off or routed to
    // the monitor bus; "all" already carries the mic in the forwarded monitor mix.
    if (!talkbackListenRoomBlock_.empty()) {
        listenRoomMixBlock_.assign(interleavedStereo.begin(), interleavedStereo.end());
        const size_t n = std::min(listenRoomMixBlock_.size(), talkbackListenRoomBlock_.size());
        for (size_t i = 0; i < n; ++i) {
            listenRoomMixBlock_[i] += talkbackListenRoomBlock_[i];
        }
        listenRoomSender_.pushInterleavedStereo(listenRoomMixBlock_.data(),
                                                static_cast<int64_t>(listenRoomMixBlock_.size() / 2u));
        return;
    }
    listenRoomSender_.pushInterleavedStereo(interleavedStereo.data(), static_cast<int64_t>(interleavedStereo.size() / 2u));
}

void NeuracoustDspEngine::resetMeteringLocked() {
    monitorPrePeakLeft_.store(0.0f);
    monitorPrePeakRight_.store(0.0f);
    outputPeakLeft_.store(0.0f);
    outputPeakRight_.store(0.0f);
    phaseCorrelation_.store(0.0f);
    phaseCorrelationBallistics_ = 0.0f;
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
