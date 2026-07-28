#include "project/ProjectDocument.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace neuracoust::daw {

namespace {

constexpr size_t kMaxInstrumentRackSlots = 8;

std::string normalizedTrackViewMode(std::string mode);
std::string normalizedTrackTimebaseMode(std::string mode);
std::string normalizedElasticAudioMode(std::string mode);

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string normalizedFadeCurve(std::string curve) {
    std::transform(curve.begin(), curve.end(), curve.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    curve.erase(std::remove_if(curve.begin(), curve.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), curve.end());
    std::replace(curve.begin(), curve.end(), '-', '_');
    if (curve == "linear" || curve == "slow" || curve == "fast" || curve == "equal_power") {
        return curve;
    }
    return "equal_power";
}

int inputChannelCountForManifestBus(const std::string& inputBusName) {
    const std::string cleanName = trim(inputBusName);
    if (cleanName.empty()) {
        return 0;
    }
    for (size_t index = 0; index < cleanName.size(); ++index) {
        if (cleanName[index] != '-') {
            continue;
        }
        size_t leftBegin = index;
        while (leftBegin > 0 && std::isdigit(static_cast<unsigned char>(cleanName[leftBegin - 1])) != 0) {
            --leftBegin;
        }
        size_t rightEnd = index + 1;
        while (rightEnd < cleanName.size() && std::isdigit(static_cast<unsigned char>(cleanName[rightEnd])) != 0) {
            ++rightEnd;
        }
        if (leftBegin == index || rightEnd == index + 1) {
            continue;
        }
        const int left = std::stoi(cleanName.substr(leftBegin, index - leftBegin));
        const int right = std::stoi(cleanName.substr(index + 1, rightEnd - index - 1));
        if (right >= left) {
            return std::max(1, std::min(2, right - left + 1));
        }
    }
    return 1;
}

std::string normalizedEditMode(std::string mode) {
    mode = trim(mode);
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (mode == "grid") {
        return "Grid";
    }
    if (mode == "shuffle") {
        return "Shuffle";
    }
    if (mode == "spot") {
        return "Spot";
    }
    return "Slip";
}

std::string normalizedGridUnit(std::string unit) {
    unit = trim(unit);
    std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (unit == "0.1s" || unit == "100ms") {
        return "0.1s";
    }
    if (unit == "1 frame" || unit == "frame") {
        return "1 frame";
    }
    if (unit == "1 bar" || unit == "bar") {
        return "1 bar";
    }
    if (unit == "1 beat" || unit == "beat") {
        return "1 beat";
    }
    if (unit == "1/4 beat" || unit == "quarter beat" || unit == "quarter") {
        return "1/4 beat";
    }
    if (unit == "1/8 beat" || unit == "eighth beat" || unit == "eighth") {
        return "1/8 beat";
    }
    if (unit == "1/16 beat" || unit == "sixteenth beat" || unit == "sixteenth") {
        return "1/16 beat";
    }
    return "1s";
}

std::string normalizedPlaybackStartMode(std::string mode) {
    mode = trim(mode);
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (mode == "selection" || mode == "edit selection") {
        return "Selection";
    }
    if (mode == "timeline start" || mode == "start" || mode == "zero") {
        return "Timeline Start";
    }
    if (mode == "return" || mode == "return to start" || mode == "return to start on stop") {
        return "Return to Start";
    }
    if (mode == "insertion" || mode == "insert" || mode == "insert position") {
        return "Insertion";
    }
    return "Return to Start";
}

bool isProtectedTrackName(const std::string& trackName) {
    return trackName == "Master" || trackName == "Monitor";
}

bool trackNameExists(const std::vector<TrackState>& tracks, const std::string& trackName) {
    return std::any_of(tracks.begin(), tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
}

bool hasTrackNamedOrTyped(const std::vector<TrackState>& tracks, const std::string& trackName, const std::string& trackType) {
    return std::any_of(tracks.begin(), tracks.end(), [&](const TrackState& track) {
        return track.name == trackName || track.trackType == trackType;
    });
}

bool isAudioTrackTypeAlias(const std::string& type) {
    return type == "audio" ||
        type == "mono" ||
        type == "stereo" ||
        type == "audio_mono" ||
        type == "audio_stereo";
}

bool isTimelineTargetTrackType(const TrackState& track) {
    return isAudioTrackTypeAlias(track.trackType) ||
        track.trackType == "aux" ||
        track.trackType == "bus_folder" ||
        track.trackType == "routing_folder" ||
        track.trackType == "midi" ||
        track.trackType == "instrument";
}

bool isPhysicalOrMonitorOutputRoute(const std::string& route) {
    const std::string clean = trim(route);
    if (clean.empty() || clean == "Master") {
        return false;
    }
    return clean == "Monitor" ||
        clean.rfind("Main", 0) == 0 ||
        clean.find(" Out ") != std::string::npos ||
        clean.find(" Output") != std::string::npos ||
        clean.find("Default Output") != std::string::npos;
}

std::string firstEditableTrackName(const std::vector<TrackState>& tracks) {
    for (const auto& track : tracks) {
        if (!track.name.empty() && !isProtectedTrackName(track.name) && isTimelineTargetTrackType(track)) {
            return track.name;
        }
    }
    return {};
}

void normalizeMasterMonitorRouting(std::vector<TrackState>& tracks) {
    for (auto& track : tracks) {
        if (track.name == "Master" || track.trackType == "master") {
            track.name = "Master";
            track.trackType = "master";
            track.inputBus.clear();
            track.outputBus = "Monitor";
            track.folderName.clear();
            track.recordArmed = false;
            track.inputMonitoring = false;
            continue;
        }
        if (track.name == "Monitor" || track.trackType == "monitor") {
            track.name = "Monitor";
            track.trackType = "monitor";
            track.inputBus = "Monitor";
            track.outputBus = "Main 1-2";
            track.folderName.clear();
            track.recordArmed = false;
            track.inputMonitoring = false;
            continue;
        }
        if (track.trackType == "basic_folder") {
            track.trackType = "folder";
        }
        if (track.trackType == "routing_folder") {
            track.trackType = "bus_folder";
        }
        if (isAudioTrackTypeAlias(track.trackType)) {
            track.trackType = "audio";
        }
        if (!(track.automationMode == "off" ||
              track.automationMode == "read" ||
              track.automationMode == "touch" ||
              track.automationMode == "latch" ||
              track.automationMode == "touch_latch" ||
              track.automationMode == "write" ||
              track.automationMode == "trim")) {
            track.automationMode = "read";
        }
        track.trackViewMode = normalizedTrackViewMode(track.trackViewMode);
        track.timebaseMode = normalizedTrackTimebaseMode(track.timebaseMode);
        track.elasticAudioMode = normalizedElasticAudioMode(track.elasticAudioMode);
        if (track.trackType == "folder") {
            track.inputBus.clear();
            track.outputBus.clear();
            track.folderName.clear();
            track.recordArmed = false;
            track.inputMonitoring = false;
            continue;
        }
        if (track.trackType == "bus_folder") {
            track.folderName.clear();
        }
        if (track.trackType == "midi") {
            track.controlMasterTrackName.clear();
            track.sends.clear();
            if (track.outputBus == "Master" || track.outputBus == "Monitor" || track.outputBus == "Main 1-2" || track.outputBus == "Instrument") {
                track.outputBus.clear();
            }
        }
        if (isTimelineTargetTrackType(track) && isPhysicalOrMonitorOutputRoute(track.outputBus)) {
            track.outputBus = "Master";
        }
    }
}

void normalizeRecordArmedTracks(std::vector<TrackState>& tracks) {
    for (auto& track : tracks) {
        if (isProtectedTrackName(track.name) || !isTimelineTargetTrackType(track)) {
            track.recordArmed = false;
            track.inputMonitoring = false;
        }
    }
}

std::string normalizedTrackViewMode(std::string mode) {
    mode = trim(mode);
    if (mode == "blocks" ||
        mode == "region-list" ||
        mode == "analysis" ||
        mode == "external" ||
        mode == "waveform" ||
        mode == "volume" ||
        mode == "volume-trim" ||
        mode == "mute" ||
        mode == "pan") {
        return mode;
    }
    return "waveform";
}

std::string normalizedTrackTimebaseMode(std::string mode) {
    mode = trim(mode);
    return mode == "ticks" ? "ticks" : "samples";
}

std::string normalizedElasticAudioMode(std::string mode) {
    mode = trim(mode);
    if (mode == "polyphonic" ||
        mode == "rhythmic" ||
        mode == "monophonic" ||
        mode == "varispeed" ||
        mode == "x-form") {
        return mode;
    }
    return "none";
}

std::string uniqueTrackNameForImport(const std::string& requestedName, std::set<std::string>& usedTrackNames) {
    const std::string cleanName = trim(requestedName);
    if (cleanName.empty()) {
        return {};
    }
    if (usedTrackNames.insert(cleanName).second) {
        return cleanName;
    }
    if (isProtectedTrackName(cleanName)) {
        return {};
    }

    for (int suffix = 2; suffix < 100000; ++suffix) {
        const std::string candidate = cleanName + " " + std::to_string(suffix);
        if (usedTrackNames.insert(candidate).second) {
            return candidate;
        }
    }
    return {};
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

std::string escapeJsonString(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

std::string parseJsonStringAt(const std::string& text, size_t firstQuote) {
    if (firstQuote == std::string::npos || firstQuote >= text.size() || text[firstQuote] != '"') {
        return {};
    }
    std::string value;
    for (size_t index = firstQuote + 1; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '"') {
            return value;
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (++index >= text.size()) {
            break;
        }
        const char escaped = text[index];
        switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                if (index + 4 >= text.size()) {
                    return value;
                }
                int codepoint = 0;
                bool valid = true;
                for (size_t offset = 1; offset <= 4; ++offset) {
                    const int digit = hexValue(text[index + offset]);
                    if (digit < 0) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 4) | digit;
                }
                if (!valid) {
                    value.push_back('?');
                    break;
                }
                if (codepoint <= 0x7f) {
                    value.push_back(static_cast<char>(codepoint));
                }
                index += 4;
                break;
            }
            default:
                value.push_back(escaped);
                break;
        }
    }
    return value;
}

std::string stringAfterKey(const std::string& text, const std::string& key, size_t start = 0) {
    const auto marker = "\"" + key + "\"";
    const auto keyPos = text.find(marker, start);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colon = text.find(':', keyPos + marker.size());
    if (colon == std::string::npos) {
        return {};
    }
    return parseJsonStringAt(text, text.find('"', colon + 1));
}

double numberAfterKey(const std::string& text, const std::string& key, double fallback, size_t start = 0) {
    const auto marker = "\"" + key + "\"";
    const auto keyPos = text.find(marker, start);
    if (keyPos == std::string::npos) {
        return fallback;
    }
    const auto colon = text.find(':', keyPos + marker.size());
    if (colon == std::string::npos) {
        return fallback;
    }
    const auto end = text.find_first_of(",}\n", colon + 1);
    const auto token = trim(text.substr(colon + 1, end == std::string::npos ? std::string::npos : end - colon - 1));
    char* parseEnd = nullptr;
    const double parsed = std::strtod(token.c_str(), &parseEnd);
    return parseEnd != token.c_str() ? parsed : fallback;
}

bool boolAfterKey(const std::string& text, const std::string& key, bool fallback, size_t start = 0) {
    const auto marker = "\"" + key + "\"";
    const auto keyPos = text.find(marker, start);
    if (keyPos == std::string::npos) {
        return fallback;
    }
    const auto colon = text.find(':', keyPos + marker.size());
    if (colon == std::string::npos) {
        return fallback;
    }
    // Skip the whitespace first, then read. Taking a fixed 5-character window from after the
    // colon and trimming it cut " false" down to "fals", so every value written as
    // `"key": false` — which is how this file writes them — matched neither literal and fell
    // back. Any flag whose fallback is true could not be saved as false.
    auto valuePos = colon + 1;
    while (valuePos < text.size() && std::isspace(static_cast<unsigned char>(text[valuePos]))) {
        ++valuePos;
    }
    if (text.compare(valuePos, 4, "true") == 0) {
        return true;
    }
    if (text.compare(valuePos, 5, "false") == 0) {
        return false;
    }
    return fallback;
}

double finiteRange(double value, double fallback, double minimum, double maximum) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::max(minimum, std::min(maximum, value));
}

float finiteRange(float value, float fallback, float minimum, float maximum) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::max(minimum, std::min(maximum, value));
}

int finiteIntRange(double value, int fallback, int minimum, int maximum) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::max(minimum, std::min(maximum, static_cast<int>(value)));
}

std::string arrayBodyAfterKey(const std::string& text, const std::string& key) {
    const auto marker = "\"" + key + "\"";
    const auto keyPos = text.find(marker);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto open = text.find('[', keyPos + marker.size());
    if (open == std::string::npos) {
        return {};
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t index = open; index < text.size(); ++index) {
        const char ch = text[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0) {
                return text.substr(open + 1, index - open - 1);
            }
        }
    }
    return {};
}

std::string objectBodyAfterKey(const std::string& text, const std::string& key) {
    const auto marker = "\"" + key + "\"";
    const auto keyPos = text.find(marker);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto open = text.find('{', keyPos + marker.size());
    if (open == std::string::npos) {
        return {};
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t index = open; index < text.size(); ++index) {
        const char ch = text[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(open + 1, index - open - 1);
            }
        }
    }
    return {};
}

std::vector<std::string> objectBodies(const std::string& arrayBody) {
    std::vector<std::string> bodies;
    int depth = 0;
    size_t objectStart = std::string::npos;
    bool inString = false;
    bool escaped = false;
    for (size_t index = 0; index < arrayBody.size(); ++index) {
        const char ch = arrayBody[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            if (depth == 0) {
                objectStart = index;
            }
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                bodies.push_back(arrayBody.substr(objectStart, index - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return bodies;
}

std::vector<std::string> stringArrayAfterKey(const std::string& text, const std::string& key) {
    std::vector<std::string> values;
    const auto body = arrayBodyAfterKey(text, key);
    size_t search = 0;
    while (search < body.size()) {
        const auto quote = body.find('"', search);
        if (quote == std::string::npos) {
            break;
        }
        values.push_back(parseJsonStringAt(body, quote));
        search = quote + 1;
        bool escaped = false;
        for (; search < body.size(); ++search) {
            if (escaped) {
                escaped = false;
            } else if (body[search] == '\\') {
                escaped = true;
            } else if (body[search] == '"') {
                ++search;
                break;
            }
        }
    }
    return values;
}

void writeStringArray(std::ostream& out, const std::vector<std::string>& values) {
    out << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        out << "\"" << escapeJsonString(values[index]) << "\"";
        if (index + 1 != values.size()) {
            out << ",";
        }
    }
    out << "]";
}

std::vector<Vst3ParameterValueState> vst3ParametersAfterKey(const std::string& text, const std::string& key) {
    std::vector<Vst3ParameterValueState> parameters;
    for (const auto& object : objectBodies(arrayBodyAfterKey(text, key))) {
        Vst3ParameterValueState parameter;
        parameter.parameterId = static_cast<uint32_t>(std::max(0.0, numberAfterKey(object, "parameterId", 0.0)));
        parameter.displayName = trim(stringAfterKey(object, "displayName"));
        parameter.normalizedValue = finiteRange(numberAfterKey(object, "normalizedValue", 0.0), 0.0, 0.0, 1.0);
        parameters.push_back(parameter);
    }
    return parameters;
}

std::vector<TrackInsertSlot> trackInsertSlotsAfterKey(const std::string& text, const std::string& key) {
    std::vector<TrackInsertSlot> inserts;
    const auto body = arrayBodyAfterKey(text, key);
    const auto objects = objectBodies(body);
    if (objects.empty()) {
        for (const auto& name : stringArrayAfterKey(text, key)) {
            TrackInsertSlot insert;
            insert.pluginName = name.empty() ? "No Insert" : name;
            insert.pluginFormat = insert.pluginName == "No Insert" ? "None" : "Legacy";
            inserts.push_back(insert);
        }
        return inserts;
    }

    for (const auto& object : objects) {
        TrackInsertSlot insert;
        insert.pluginName = trim(stringAfterKey(object, "pluginName"));
        insert.pluginFormat = trim(stringAfterKey(object, "pluginFormat"));
        insert.pluginPath = stringAfterKey(object, "pluginPath");
        insert.pluginClassId = trim(stringAfterKey(object, "pluginClassId"));
        insert.pluginClassName = trim(stringAfterKey(object, "pluginClassName"));
        insert.bypassed = boolAfterKey(object, "bypassed", false);
        insert.enabled = boolAfterKey(object, "enabled", true);
        insert.dspExecutionMode = trim(stringAfterKey(object, "dspExecutionMode"));
        insert.assignedDspServerId = trim(stringAfterKey(object, "assignedDspServerId"));
        insert.serverModuleId = trim(stringAfterKey(object, "serverModuleId"));
        if (insert.serverModuleId.empty()) {
            insert.serverModuleId = trim(stringAfterKey(object, "externalPluginId"));
        }
        insert.reportedLatencySamples = static_cast<unsigned int>(std::max(0.0, numberAfterKey(object, "reportedLatencySamples", 0.0)));
        insert.dspAvailable = boolAfterKey(object, "dspAvailable", true);
        insert.dspLastError = trim(stringAfterKey(object, "dspLastError"));
        insert.parameters = vst3ParametersAfterKey(object, "parameters");
        if (insert.pluginName.empty()) {
            insert.pluginName = "No Insert";
        }
        if (insert.pluginFormat.empty()) {
            insert.pluginFormat = insert.pluginName == "No Insert" ? "None" : "Unknown";
        }
        if (!(insert.dspExecutionMode == "native" ||
              insert.dspExecutionMode == "internal" ||
              insert.dspExecutionMode == "remote_internal" ||
              insert.dspExecutionMode == "external")) {
            insert.dspExecutionMode = "native";
        }
        if (insert.pluginName == "No Insert") {
            insert.dspExecutionMode = "native";
            insert.assignedDspServerId.clear();
            insert.serverModuleId.clear();
            insert.reportedLatencySamples = 0;
            insert.dspAvailable = true;
            insert.dspLastError.clear();
        }
        inserts.push_back(insert);
    }
    return inserts;
}

InstrumentSlotState instrumentSlotAfterKey(const std::string& text, const std::string& key) {
    InstrumentSlotState slot;
    const std::string body = objectBodyAfterKey(text, key);
    if (body.empty()) {
        return slot;
    }
    slot.pluginName = trim(stringAfterKey(body, "pluginName"));
    slot.pluginFormat = trim(stringAfterKey(body, "pluginFormat"));
    slot.pluginPath = stringAfterKey(body, "pluginPath");
    slot.pluginClassId = trim(stringAfterKey(body, "pluginClassId"));
    slot.pluginClassName = trim(stringAfterKey(body, "pluginClassName"));
    slot.bypassed = boolAfterKey(body, "bypassed", false);
    slot.enabled = boolAfterKey(body, "enabled", false);
    slot.midiInput = trim(stringAfterKey(body, "midiInput"));
    slot.midiChannel = finiteIntRange(numberAfterKey(body, "midiChannel", 0.0), 0, 0, 16);
    slot.reportedLatencySamples = static_cast<unsigned int>(std::max(0.0, numberAfterKey(body, "reportedLatencySamples", 0.0)));
    slot.parameters = vst3ParametersAfterKey(body, "parameters");
    // The plug-in's own patch. Base64 carries no quotes or braces, so it is safe for
    // this parser to read as a plain string value.
    slot.pluginStateBase64 = trim(stringAfterKey(body, "pluginState"));
    if (slot.pluginName.empty()) {
        slot.pluginName = "No Instrument";
    }
    if (slot.pluginFormat.empty()) {
        slot.pluginFormat = slot.pluginName == "No Instrument" ? "None" : "Unknown";
    }
    if (slot.midiInput.empty()) {
        slot.midiInput = "MIDI Input";
    }
    if (slot.pluginName == "No Instrument") {
        slot.pluginFormat = "None";
        slot.pluginPath.clear();
        slot.pluginClassId.clear();
        slot.pluginClassName.clear();
        slot.enabled = false;
        slot.bypassed = false;
        slot.reportedLatencySamples = 0;
        slot.parameters.clear();
    }
    return slot;
}

std::vector<InstrumentSlotState> instrumentSlotsAfterKey(const std::string& text, const std::string& key) {
    std::vector<InstrumentSlotState> slots;
    for (const auto& object : objectBodies(arrayBodyAfterKey(text, key))) {
        InstrumentSlotState slot = instrumentSlotAfterKey("{\"" + key + "\":" + object + "}", key);
        slots.push_back(slot);
        if (slots.size() >= kMaxInstrumentRackSlots) {
            break;
        }
    }
    return slots;
}

bool instrumentSlotHasPluginForDocument(const InstrumentSlotState& slot) {
    return slot.enabled && !slot.pluginName.empty() && slot.pluginName != "No Instrument" && !slot.pluginPath.empty();
}

InstrumentSlotState normalizedInstrumentSlotForDocument(InstrumentSlotState slot) {
    if (trim(slot.pluginName).empty() || slot.pluginName == "No Instrument" || slot.pluginPath.empty()) {
        slot.pluginName = "No Instrument";
        slot.pluginFormat = "None";
        slot.pluginPath.clear();
        slot.pluginClassId.clear();
        slot.pluginClassName.clear();
        slot.enabled = false;
        slot.bypassed = false;
        slot.midiInput = "MIDI Input";
        slot.midiChannel = 0;
        slot.reportedLatencySamples = 0;
        slot.parameters.clear();
        return slot;
    }
    if (trim(slot.pluginFormat).empty() || slot.pluginFormat == "None") {
        slot.pluginFormat = "VST3";
    }
    if (trim(slot.midiInput).empty()) {
        slot.midiInput = "MIDI Input";
    }
    slot.midiChannel = std::max(0, std::min(16, slot.midiChannel));
    slot.enabled = true;
    return slot;
}

void normalizeInstrumentRack(TrackState& track) {
    if (track.instrumentRackMode != "serial" && track.instrumentRackMode != "parallel") {
        track.instrumentRackMode = "parallel";
    }
    if (track.instrumentSlots.empty() && instrumentSlotHasPluginForDocument(track.instrument)) {
        track.instrumentSlots.push_back(track.instrument);
    }
    if (track.instrumentSlots.size() > kMaxInstrumentRackSlots) {
        track.instrumentSlots.resize(kMaxInstrumentRackSlots);
    }
    for (auto& slot : track.instrumentSlots) {
        slot = normalizedInstrumentSlotForDocument(slot);
    }
    while (!track.instrumentSlots.empty() && !instrumentSlotHasPluginForDocument(track.instrumentSlots.back())) {
        track.instrumentSlots.pop_back();
    }
    track.instrument = track.instrumentSlots.empty()
        ? normalizedInstrumentSlotForDocument(InstrumentSlotState {})
        : track.instrumentSlots.front();
}

std::vector<TrackSendState> trackSendsAfterKey(const std::string& text, const std::string& key) {
    std::vector<TrackSendState> sends;
    const auto body = arrayBodyAfterKey(text, key);
    const auto objects = objectBodies(body);
    if (objects.empty()) {
        for (const auto& busName : stringArrayAfterKey(text, key)) {
            TrackSendState send;
            send.busName = trim(busName);
            if (send.busName.empty()) {
                send.enabled = false;
            }
            sends.push_back(send);
        }
        return sends;
    }

    for (const auto& object : objects) {
        TrackSendState send;
        send.busName = trim(stringAfterKey(object, "busName"));
        send.gainDb = finiteRange(static_cast<float>(numberAfterKey(object, "gainDb", 0.0)), 0.0f, -60.0f, 12.0f);
        send.pan = finiteRange(static_cast<float>(numberAfterKey(object, "pan", 0.0)), 0.0f, -1.0f, 1.0f);
        send.enabled = boolAfterKey(object, "enabled", true);
        send.preFader = boolAfterKey(object, "preFader", false);
        send.stereo = boolAfterKey(object, "stereo", true);
        if (send.busName.empty()) {
            send.enabled = false;
        }
        sends.push_back(send);
    }
    return sends;
}

std::vector<AutomationPointState> automationPointsAfterKey(const std::string& text, const std::string& key) {
    std::vector<AutomationPointState> points;
    for (const auto& object : objectBodies(arrayBodyAfterKey(text, key))) {
        AutomationPointState point;
        point.timeSeconds = finiteRange(numberAfterKey(object, "timeSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        point.value = finiteRange(static_cast<float>(numberAfterKey(object, "value", 0.0)), 0.0f, -120.0f, 24.0f);
        points.push_back(point);
    }
    std::sort(points.begin(), points.end(), [](const AutomationPointState& left, const AutomationPointState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    return points;
}

std::vector<AutomationLaneState> automationLanesAfterKey(const std::string& text, const std::string& key) {
    std::vector<AutomationLaneState> lanes;
    for (const auto& object : objectBodies(arrayBodyAfterKey(text, key))) {
        AutomationLaneState lane;
        lane.parameterId = trim(stringAfterKey(object, "parameterId"));
        lane.displayName = trim(stringAfterKey(object, "displayName"));
        lane.points = automationPointsAfterKey(object, "points");
        if (lane.parameterId.empty()) {
            continue;
        }
        if (lane.displayName.empty()) {
            lane.displayName = lane.parameterId;
        }
        lanes.push_back(lane);
    }
    return lanes;
}

void writeVst3Parameters(std::ostream& out, const std::vector<Vst3ParameterValueState>& parameters) {
    out << "[";
    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        out << "{\"parameterId\":" << parameter.parameterId
            << ",\"displayName\":\"" << escapeJsonString(parameter.displayName)
            << "\",\"normalizedValue\":" << finiteRange(parameter.normalizedValue, 0.0, 0.0, 1.0) << "}";
        if (index + 1 != parameters.size()) {
            out << ",";
        }
    }
    out << "]";
}

void writeTrackInsertSlots(std::ostream& out, const std::vector<TrackInsertSlot>& inserts) {
    out << "[";
    for (size_t index = 0; index < inserts.size(); ++index) {
        const auto& insert = inserts[index];
        out << "{\"pluginName\":\"" << escapeJsonString(insert.pluginName)
            << "\",\"pluginFormat\":\"" << escapeJsonString(insert.pluginFormat)
            << "\",\"pluginPath\":\"" << escapeJsonString(insert.pluginPath)
            << "\",\"pluginClassId\":\"" << escapeJsonString(insert.pluginClassId)
            << "\",\"pluginClassName\":\"" << escapeJsonString(insert.pluginClassName)
            << "\",\"bypassed\":" << (insert.bypassed ? "true" : "false")
            << ",\"enabled\":" << (insert.enabled ? "true" : "false")
            << ",\"dspExecutionMode\":\"" << escapeJsonString(insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode)
            << "\",\"assignedDspServerId\":\"" << escapeJsonString(insert.assignedDspServerId)
            << "\",\"serverModuleId\":\"" << escapeJsonString(insert.serverModuleId)
            << "\",\"externalPluginId\":\"" << escapeJsonString(insert.serverModuleId)
            << "\",\"reportedLatencySamples\":" << insert.reportedLatencySamples
            << ",\"dspAvailable\":" << (insert.dspAvailable ? "true" : "false")
            << ",\"dspLastError\":\"" << escapeJsonString(insert.dspLastError)
            << "\",\"parameters\":";
        writeVst3Parameters(out, insert.parameters);
        out << "}";
        if (index + 1 != inserts.size()) {
            out << ",";
        }
    }
    out << "]";
}

void writeInstrumentSlot(std::ostream& out, const InstrumentSlotState& slot) {
    out << "{\"pluginName\":\"" << escapeJsonString(slot.pluginName.empty() ? "No Instrument" : slot.pluginName)
        << "\",\"pluginFormat\":\"" << escapeJsonString(slot.pluginFormat.empty() ? "None" : slot.pluginFormat)
        << "\",\"pluginPath\":\"" << escapeJsonString(slot.pluginPath)
        << "\",\"pluginClassId\":\"" << escapeJsonString(slot.pluginClassId)
        << "\",\"pluginClassName\":\"" << escapeJsonString(slot.pluginClassName)
        << "\",\"bypassed\":" << (slot.bypassed ? "true" : "false")
        << ",\"enabled\":" << (slot.enabled ? "true" : "false")
        << ",\"midiInput\":\"" << escapeJsonString(slot.midiInput.empty() ? "MIDI Input" : slot.midiInput)
        << "\",\"midiChannel\":" << std::max(0, std::min(16, slot.midiChannel))
        << ",\"reportedLatencySamples\":" << slot.reportedLatencySamples
        << ",\"pluginState\":\"" << escapeJsonString(slot.pluginStateBase64)
        << "\",\"parameters\":";
    writeVst3Parameters(out, slot.parameters);
    out << "}";
}

void writeInstrumentSlots(std::ostream& out, const std::vector<InstrumentSlotState>& slots) {
    out << "[";
    size_t emitted = 0;
    for (size_t index = 0; index < slots.size() && emitted < kMaxInstrumentRackSlots; ++index) {
        if (emitted > 0) {
            out << ",";
        }
        writeInstrumentSlot(out, slots[index]);
        ++emitted;
    }
    out << "]";
}

void writeTrackSends(std::ostream& out, const std::vector<TrackSendState>& sends) {
    out << "[";
    for (size_t index = 0; index < sends.size(); ++index) {
        const auto& send = sends[index];
        out << "{\"busName\":\"" << escapeJsonString(send.busName)
            << "\",\"gainDb\":" << send.gainDb
            << ",\"pan\":" << send.pan
            << ",\"enabled\":" << (send.enabled ? "true" : "false")
            << ",\"preFader\":" << (send.preFader ? "true" : "false")
            << ",\"stereo\":" << (send.stereo ? "true" : "false") << "}";
        if (index + 1 != sends.size()) {
            out << ",";
        }
    }
    out << "]";
}

void writeAutomationPoints(std::ostream& out, const std::vector<AutomationPointState>& points) {
    out << "[";
    for (size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        out << "{\"timeSeconds\":" << point.timeSeconds << ",\"value\":" << point.value << "}";
        if (index + 1 != points.size()) {
            out << ",";
        }
    }
    out << "]";
}

void writeAutomationLanes(std::ostream& out, const std::vector<AutomationLaneState>& lanes) {
    out << "[";
    for (size_t index = 0; index < lanes.size(); ++index) {
        const auto& lane = lanes[index];
        out << "{\"parameterId\":\"" << escapeJsonString(lane.parameterId)
            << "\",\"displayName\":\"" << escapeJsonString(lane.displayName)
            << "\",\"points\":";
        writeAutomationPoints(out, lane.points);
        out << "}";
        if (index + 1 != lanes.size()) {
            out << ",";
        }
    }
    out << "]";
}

std::filesystem::path projectBaseDirectory(const std::filesystem::path& projectPath) {
    const auto parent = projectPath.parent_path();
    return parent.empty() ? std::filesystem::current_path() : parent;
}

std::filesystem::path normalizedLexicalPath(std::filesystem::path path) {
    return path.lexically_normal();
}

bool pathStartsWith(const std::filesystem::path& path, const std::filesystem::path& base) {
    auto pathIt = path.begin();
    for (auto baseIt = base.begin(); baseIt != base.end(); ++baseIt, ++pathIt) {
        if (pathIt == path.end() || *pathIt != *baseIt) {
            return false;
        }
    }
    return true;
}

std::string pathToProjectPortableString(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string makePathRelativeToProject(const std::string& value, const std::filesystem::path& projectPath) {
    if (value.empty()) {
        return value;
    }

    const std::filesystem::path rawPath(value);
    if (!rawPath.is_absolute()) {
        return pathToProjectPortableString(normalizedLexicalPath(rawPath));
    }

    const auto base = normalizedLexicalPath(std::filesystem::absolute(projectBaseDirectory(projectPath)));
    const auto fullPath = normalizedLexicalPath(rawPath);
    if (!pathStartsWith(fullPath, base)) {
        return value;
    }

    const auto relative = fullPath.lexically_relative(base);
    if (relative.empty()) {
        return value;
    }
    const auto first = relative.begin();
    if (first == relative.end() || *first == "." || *first == "..") {
        return value;
    }
    return pathToProjectPortableString(relative);
}

std::string resolvePathFromProject(const std::string& value, const std::filesystem::path& projectPath) {
    if (value.empty()) {
        return value;
    }

    const std::filesystem::path rawPath(value);
    if (rawPath.is_absolute()) {
        return value;
    }

    return pathToProjectPortableString(normalizedLexicalPath(projectBaseDirectory(projectPath) / rawPath));
}

bool pluginPathShouldResolveWithProject(const std::string& format) {
    return format == "VST3" || format == "VST3/AU";
}

void makeTrackPluginPathsRelativeToProject(TrackState& track, const std::filesystem::path& projectPath) {
    for (auto& insert : track.inserts) {
        if (pluginPathShouldResolveWithProject(insert.pluginFormat)) {
            insert.pluginPath = makePathRelativeToProject(insert.pluginPath, projectPath);
        }
    }
    if (pluginPathShouldResolveWithProject(track.instrument.pluginFormat)) {
        track.instrument.pluginPath = makePathRelativeToProject(track.instrument.pluginPath, projectPath);
    }
    for (auto& slot : track.instrumentSlots) {
        if (pluginPathShouldResolveWithProject(slot.pluginFormat)) {
            slot.pluginPath = makePathRelativeToProject(slot.pluginPath, projectPath);
        }
    }
}

void resolveTrackPluginPathsFromProject(TrackState& track, const std::filesystem::path& projectPath) {
    for (auto& insert : track.inserts) {
        if (pluginPathShouldResolveWithProject(insert.pluginFormat)) {
            insert.pluginPath = resolvePathFromProject(insert.pluginPath, projectPath);
        }
    }
    if (pluginPathShouldResolveWithProject(track.instrument.pluginFormat)) {
        track.instrument.pluginPath = resolvePathFromProject(track.instrument.pluginPath, projectPath);
    }
    for (auto& slot : track.instrumentSlots) {
        if (pluginPathShouldResolveWithProject(slot.pluginFormat)) {
            slot.pluginPath = resolvePathFromProject(slot.pluginPath, projectPath);
        }
    }
}

bool pathsReferToSameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    return std::filesystem::exists(a, error) &&
        std::filesystem::exists(b, error) &&
        std::filesystem::equivalent(a, b, error);
}

std::filesystem::path uniqueMediaPath(const std::filesystem::path& mediaDirectory,
                                      const std::filesystem::path& sourcePath) {
    std::filesystem::path candidate = mediaDirectory / sourcePath.filename();
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }

    const auto stem = sourcePath.stem().string();
    const auto extension = sourcePath.extension().string();
    for (int suffix = 2; suffix < 10000; ++suffix) {
        std::ostringstream name;
        name << stem << " " << suffix << extension;
        candidate = mediaDirectory / name.str();
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path projectMediaDirectory(const std::filesystem::path& projectPath) {
    return normalizedLexicalPath(std::filesystem::absolute(projectBaseDirectory(projectPath))) / "Audio Files";
}

std::filesystem::path projectVideoDirectory(const std::filesystem::path& projectPath) {
    return normalizedLexicalPath(std::filesystem::absolute(projectBaseDirectory(projectPath))) / "Video Files";
}

std::string pathStemString(const std::filesystem::path& path) {
    const auto stem = path.stem().string();
    return stem.empty() ? path.filename().string() : stem;
}

std::string defaultRegionNameForSource(const std::string& sourcePath) {
    const std::filesystem::path path(sourcePath);
    const auto stem = path.stem().string();
    if (!stem.empty()) {
        return stem;
    }
    const auto filename = path.filename().string();
    return filename.empty() ? "Audio Clip" : filename;
}

std::string defaultSourceFileUid(const std::string& sourcePath) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : sourcePath) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "src-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

bool isVst3Insert(const InsertState& insert) {
    return insert.pluginFormat == "VST3" || insert.pluginFormat == "VST3/AU";
}

bool isMonitorDspInsert(const InsertState& insert) {
    return insert.pluginAppId == "neuracoust-monitor-dsp";
}

bool isValidWindowsProcessorAffinityMode(const std::string& mode) {
    return mode == "p_core_preferred" ||
        mode == "p_core_high_priority" ||
        mode == "custom_mask";
}

bool isValidTrackType(const std::string& type) {
    return type == "audio" ||
        type == "aux" ||
        type == "folder" ||
        type == "basic_folder" ||
        type == "bus_folder" ||
        type == "routing_folder" ||
        type == "master" ||
        type == "monitor" ||
        type == "midi" ||
        type == "instrument" ||
        type == "vca";
}

std::string inferredTrackType(const TrackState& track) {
    if (track.name == "Master") {
        return "master";
    }
    if (track.name == "Monitor") {
        return "monitor";
    }
    if (track.name.rfind("Folder ", 0) == 0) {
        return "folder";
    }
    if (track.name.rfind("Bus Folder ", 0) == 0) {
        return "bus_folder";
    }
    if (track.name.rfind("Aux ", 0) == 0 || track.inputBus.rfind("Bus ", 0) == 0) {
        return "aux";
    }
    return "audio";
}

std::string uniqueIdForImport(const std::string& requestedId, std::set<std::string>& usedIds) {
    if (requestedId.empty()) {
        return {};
    }
    if (usedIds.insert(requestedId).second) {
        return requestedId;
    }

    for (int suffix = 2; suffix < 100000; ++suffix) {
        const std::string candidate = requestedId + "-" + std::to_string(suffix);
        if (usedIds.insert(candidate).second) {
            return candidate;
        }
    }
    return {};
}

std::string sourceIdForClip(const ClipState& clip) {
    return clip.sourceFileUid.empty() ? defaultSourceFileUid(clip.sourcePath) : clip.sourceFileUid;
}

std::string clipDefinitionIdForClip(const ClipState& clip) {
    return clip.id.empty() ? std::string{} : "clipdef-" + clip.id;
}

std::string playlistIdForTrackName(const std::string& trackName) {
    std::string id = "playlist";
    for (const unsigned char ch : trackName) {
        if (std::isalnum(ch) != 0) {
            id.push_back(static_cast<char>(std::tolower(ch)));
        } else if (id.back() != '-') {
            id.push_back('-');
        }
    }
    while (!id.empty() && id.back() == '-') {
        id.pop_back();
    }
    return id.empty() ? "playlist-main" : id + "-main";
}

const MediaSourceState* mediaSourceById(const ProjectDocument& project, const std::string& sourceId) {
    auto it = std::find_if(project.mediaSources.begin(), project.mediaSources.end(), [&](const MediaSourceState& source) {
        return source.id == sourceId;
    });
    return it == project.mediaSources.end() ? nullptr : &(*it);
}

const ClipDefinitionState* clipDefinitionById(const ProjectDocument& project, const std::string& definitionId) {
    auto it = std::find_if(project.clipDefinitions.begin(), project.clipDefinitions.end(), [&](const ClipDefinitionState& definition) {
        return definition.id == definitionId;
    });
    return it == project.clipDefinitions.end() ? nullptr : &(*it);
}

bool hasAnyPlaylistPlacements(const ProjectDocument& project) {
    return std::any_of(project.trackPlaylists.begin(), project.trackPlaylists.end(), [](const TrackPlaylistState& playlist) {
        return !playlist.placements.empty();
    });
}

std::string uniquePlaylistId(const ProjectDocument& project, const std::string& baseId) {
    std::set<std::string> usedIds;
    for (const auto& playlist : project.trackPlaylists) {
        usedIds.insert(playlist.id);
    }
    if (!baseId.empty() && usedIds.find(baseId) == usedIds.end()) {
        return baseId;
    }
    const std::string prefix = baseId.empty() ? "playlist" : baseId;
    for (int suffix = 2; suffix < 100000; ++suffix) {
        const std::string candidate = prefix + "-" + std::to_string(suffix);
        if (usedIds.find(candidate) == usedIds.end()) {
            return candidate;
        }
    }
    return {};
}

bool pathExists(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool pathExists(const std::string& path) {
    return pathExists(std::filesystem::path(path));
}

bool writeTextFileAtomically(const std::filesystem::path& targetPath,
                             const std::string& text,
                             const char* tempSuffix,
                             std::string& error) {
    error.clear();
    if (targetPath.empty()) {
        error = "Target path is empty.";
        return false;
    }

    std::error_code fsError;
    if (!targetPath.parent_path().empty()) {
        std::filesystem::create_directories(targetPath.parent_path(), fsError);
        if (fsError) {
            error = "Could not create target directory: " + fsError.message();
            return false;
        }
    }

    auto tempPath = targetPath;
    tempPath += tempSuffix;
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not create temporary file.";
            return false;
        }
        out << text;
        out.close();
        if (!out) {
            std::filesystem::remove(tempPath, fsError);
            error = "Could not finish writing temporary file.";
            return false;
        }
    }

    std::filesystem::rename(tempPath, targetPath, fsError);
    if (fsError) {
        std::filesystem::remove(targetPath, fsError);
        fsError.clear();
        std::filesystem::rename(tempPath, targetPath, fsError);
    }
    if (fsError) {
        const auto message = fsError.message();
        std::filesystem::remove(tempPath, fsError);
        error = "Could not replace target file: " + message;
        return false;
    }

    return true;
}

} // namespace

ProjectDocument defaultProject() {
    ProjectDocument project;
    TrackState audio1;
    audio1.name = "Audio 1";
    audio1.trackType = "audio";
    audio1.colorHex = "#35BFA8";
    TrackState audio2;
    audio2.name = "Audio 2";
    audio2.trackType = "audio";
    audio2.colorHex = "#4B84E8";
    TrackState master;
    master.name = "Master";
    master.trackType = "master";
    master.colorHex = "#9AA0A6";
    master.inputBus.clear();
    master.outputBus = "Monitor";
    TrackState monitor;
    monitor.name = "Monitor";
    monitor.trackType = "monitor";
    monitor.colorHex = "#6EC6FF";
    monitor.inputBus = "Monitor";
    monitor.outputBus = "Main 1-2";
    audio1.outputBus = "Master";
    audio2.outputBus = "Master";
    project.tracks = {audio1, audio2, master, monitor};
    project.tempoMasterTrackName = audio1.name;
    project.editMode = "Grid";   // snap to the musical grid by default, matching the UI
    project.audioImportTempoPolicy = "preserve-project";
    normalizeMasterMonitorRouting(project.tracks);
    project.tempoMap.push_back({0.0, static_cast<double>(project.tempoBpm)});
    project.timeSignatureMap.push_back({0.0, project.timeSignatureNumerator, project.timeSignatureDenominator});
    // A marker on the first beat, so a new project opens with tempo/meter/marker/key all
    // seeded at the start.
    MarkerState startMarker;
    startMarker.id = "marker-start";
    startMarker.name = "1";
    startMarker.timeSeconds = 0.0;
    project.markers.push_back(startMarker);
    project.masterInserts.clear();
    project.monitorModules = defaultMonitorDspModules();
    return project;
}

void normalizeProjectRouting(ProjectDocument& project) {
    const auto defaultTracks = defaultProject().tracks;
    for (const auto& defaultTrack : defaultTracks) {
        if (defaultTrack.name == "Master" && !hasTrackNamedOrTyped(project.tracks, "Master", "master")) {
            project.tracks.push_back(defaultTrack);
        }
        if (defaultTrack.name == "Monitor" && !hasTrackNamedOrTyped(project.tracks, "Monitor", "monitor")) {
            project.tracks.push_back(defaultTrack);
        }
    }
    normalizeMasterMonitorRouting(project.tracks);
    normalizeRecordArmedTracks(project.tracks);

    std::vector<TrackState> timelineTracks;
    timelineTracks.reserve(project.tracks.size());
    TrackState masterTrack;
    TrackState monitorTrack;
    bool hasMaster = false;
    bool hasMonitor = false;
    for (const auto& track : project.tracks) {
        if (track.name == "Master" || track.trackType == "master") {
            if (!hasMaster) {
                masterTrack = track;
                hasMaster = true;
            }
            continue;
        }
        if (track.name == "Monitor" || track.trackType == "monitor") {
            if (!hasMonitor) {
                monitorTrack = track;
                hasMonitor = true;
            }
            continue;
        }
        timelineTracks.push_back(track);
    }
    if (hasMaster) {
        timelineTracks.push_back(masterTrack);
    }
    if (hasMonitor) {
        timelineTracks.push_back(monitorTrack);
    }
    project.tracks = std::move(timelineTracks);
    if (project.audioImportTempoPolicy != "ask" &&
        project.audioImportTempoPolicy != "apply-to-project" &&
        project.audioImportTempoPolicy != "preserve-project" &&
        project.audioImportTempoPolicy != "stretch-to-project") {
        project.audioImportTempoPolicy = "preserve-project";
    }
    if (project.tempoMasterTrackName.empty() || !trackNameExists(project.tracks, project.tempoMasterTrackName)) {
        project.tempoMasterTrackName = firstEditableTrackName(project.tracks);
    }
}

void rebuildProjectEditModelFromClips(ProjectDocument& project) {
    project.mediaSources.clear();
    project.clipDefinitions.clear();
    project.trackPlaylists.clear();

    std::set<std::string> validTrackNames;
    for (const auto& track : project.tracks) {
        if (!track.name.empty() && !isProtectedTrackName(track.name) && isTimelineTargetTrackType(track)) {
            validTrackNames.insert(track.name);
        }
    }

    std::set<std::string> usedSourceIds;
    std::set<std::string> usedDefinitionIds;
    std::map<std::string, size_t> playlistIndexByTrack;
    for (const auto& trackName : validTrackNames) {
        TrackPlaylistState playlist;
        playlist.id = uniquePlaylistId(project, playlistIdForTrackName(trackName));
        playlist.trackName = trackName;
        playlist.name = "Playlist 1";
        playlist.active = true;
        playlistIndexByTrack[trackName] = project.trackPlaylists.size();
        project.trackPlaylists.push_back(playlist);
    }

    for (const auto& clip : project.clips) {
        if (clip.id.empty() || clip.durationSeconds <= 0.0 || validTrackNames.find(clip.trackName) == validTrackNames.end()) {
            continue;
        }

        const std::string sourceId = sourceIdForClip(clip);
        if (!clip.sourcePath.empty() && usedSourceIds.insert(sourceId).second) {
            MediaSourceState source;
            source.id = sourceId;
            source.path = clip.sourcePath;
            source.displayName = defaultRegionNameForSource(clip.sourcePath);
            source.channels = clip.sourceChannels;
            source.sampleRate = clip.sourceSampleRate;
            source.bitsPerSample = clip.sourceBitsPerSample;
            source.floatingPoint = clip.sourceFloatingPoint;
            source.hasBroadcastTimeReference = clip.sourceHasBroadcastTimeReference;
            source.timeReferenceSamples = clip.sourceTimeReferenceSamples;
            source.timeReferenceSeconds = clip.sourceTimeReferenceSeconds;
            project.mediaSources.push_back(source);
        }

        const std::string definitionId = clipDefinitionIdForClip(clip);
        if (!definitionId.empty() && usedDefinitionIds.insert(definitionId).second) {
            ClipDefinitionState definition;
            definition.id = definitionId;
            definition.sourceId = sourceId;
            definition.name = clip.regionName.empty() ? defaultRegionNameForSource(clip.sourcePath) : clip.regionName;
            definition.sourceOffsetSeconds = clip.sourceOffsetSeconds;
            definition.durationSeconds = clip.durationSeconds;
            definition.sourceTempoBpm = clip.sourceTempoBpm;
            definition.sourceTimeSignatureNumerator = clip.sourceTimeSignatureNumerator;
            definition.sourceTimeSignatureDenominator = clip.sourceTimeSignatureDenominator;
            definition.sourceGrooveFeel = clip.sourceGrooveFeel;
            definition.sourceGrooveSwingAmount = clip.sourceGrooveSwingAmount;
            project.clipDefinitions.push_back(definition);
        }

        auto playlistIt = playlistIndexByTrack.find(clip.trackName);
        if (playlistIt == playlistIndexByTrack.end()) {
            continue;
        }
        PlaylistClipPlacementState placement;
        placement.id = clip.id;
        placement.clipDefinitionId = definitionId;
        placement.startSeconds = clip.startSeconds;
        placement.originalStartSeconds = clip.originalStartSeconds;
        placement.layer = 0;
        placement.gainDb = clip.gainDb;
        placement.fadeInSeconds = clip.fadeInSeconds;
        placement.fadeOutSeconds = clip.fadeOutSeconds;
        placement.fadeInCurve = normalizedFadeCurve(clip.fadeInCurve);
        placement.fadeOutCurve = normalizedFadeCurve(clip.fadeOutCurve);
        placement.fadeInCurvature = clip.fadeInCurvature;
        placement.fadeOutCurvature = clip.fadeOutCurvature;
        placement.muted = clip.muted;
        placement.polarityInverted = clip.polarityInverted;
        placement.reversed = clip.reversed;
        placement.araPluginName = clip.araPluginName;
        placement.araPluginPath = clip.araPluginPath;
        placement.araSourcePath = clip.araSourcePath;
        placement.araArchiveBase64 = clip.araArchiveBase64;
        placement.locked = clip.locked;
        placement.colorHex = clip.colorHex;
        placement.timeScale = clip.timeScale;
        placement.tempoSyncPolicy = clip.tempoSyncPolicy.empty() ? "project-tempo" : clip.tempoSyncPolicy;
        placement.pendingTimeStretchToProject = clip.pendingTimeStretchToProject;
        placement.legacyClipId = clip.id;
        project.trackPlaylists[playlistIt->second].placements.push_back(placement);
    }
}

// Merge any flat project.clips that are not yet represented in their track's
// active playlist into that playlist (creating the backing media source and clip
// definition if missing). Copy/duplicate/paste operations append to project.clips
// only; because rendering regenerates project.clips from the active playlists, an
// un-merged copy would draw as a waveform but never be heard. This reconciliation
// makes those copied clips real placements so they play, without disturbing the
// existing playlist/comp structure.
void mergeOrphanClipsIntoActivePlaylists(ProjectDocument& project) {
    if (project.trackPlaylists.empty()) {
        return;
    }
    std::set<std::string> validTrackNames;
    for (const auto& track : project.tracks) {
        if (!track.name.empty() && !isProtectedTrackName(track.name) && isTimelineTargetTrackType(track)) {
            validTrackNames.insert(track.name);
        }
    }
    // A duplicated track (option-drag copy) is inserted with its clips but WITHOUT
    // a playlist. Since rendering only plays clips that live in a track's active
    // playlist, ensure every valid track has one active playlist before merging —
    // otherwise the copied track's clips would be skipped and stay silent.
    for (const auto& trackName : validTrackNames) {
        const bool hasActive = std::any_of(project.trackPlaylists.begin(), project.trackPlaylists.end(),
            [&](const TrackPlaylistState& playlist) {
                return playlist.trackName == trackName && playlist.active;
            });
        if (hasActive) {
            continue;
        }
        auto anyIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(),
            [&](const TrackPlaylistState& playlist) {
                return playlist.trackName == trackName;
            });
        if (anyIt != project.trackPlaylists.end()) {
            anyIt->active = true;
        } else {
            TrackPlaylistState playlist;
            playlist.id = uniquePlaylistId(project, playlistIdForTrackName(trackName));
            playlist.trackName = trackName;
            playlist.name = "Playlist 1";
            playlist.active = true;
            project.trackPlaylists.push_back(playlist);
        }
    }
    std::set<std::string> knownSourceIds;
    for (const auto& source : project.mediaSources) {
        knownSourceIds.insert(source.id);
    }
    std::set<std::string> knownDefinitionIds;
    for (const auto& definition : project.clipDefinitions) {
        knownDefinitionIds.insert(definition.id);
    }
    for (const auto& clip : project.clips) {
        if (clip.id.empty() || clip.durationSeconds <= 0.0 ||
            validTrackNames.find(clip.trackName) == validTrackNames.end()) {
            continue;
        }
        auto activeIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(),
            [&](const TrackPlaylistState& playlist) {
                return playlist.active && playlist.trackName == clip.trackName;
            });
        if (activeIt == project.trackPlaylists.end()) {
            continue;
        }
        const bool alreadyPlaced = std::any_of(activeIt->placements.begin(), activeIt->placements.end(),
            [&](const PlaylistClipPlacementState& placement) {
                return placement.id == clip.id || placement.legacyClipId == clip.id;
            });
        if (alreadyPlaced) {
            continue;
        }

        const std::string sourceId = sourceIdForClip(clip);
        if (!clip.sourcePath.empty() && knownSourceIds.insert(sourceId).second) {
            MediaSourceState source;
            source.id = sourceId;
            source.path = clip.sourcePath;
            source.displayName = defaultRegionNameForSource(clip.sourcePath);
            source.channels = clip.sourceChannels;
            source.sampleRate = clip.sourceSampleRate;
            source.bitsPerSample = clip.sourceBitsPerSample;
            source.floatingPoint = clip.sourceFloatingPoint;
            source.hasBroadcastTimeReference = clip.sourceHasBroadcastTimeReference;
            source.timeReferenceSamples = clip.sourceTimeReferenceSamples;
            source.timeReferenceSeconds = clip.sourceTimeReferenceSeconds;
            project.mediaSources.push_back(source);
        }

        const std::string definitionId = clipDefinitionIdForClip(clip);
        if (!definitionId.empty() && knownDefinitionIds.insert(definitionId).second) {
            ClipDefinitionState definition;
            definition.id = definitionId;
            definition.sourceId = sourceId;
            definition.name = clip.regionName.empty() ? defaultRegionNameForSource(clip.sourcePath) : clip.regionName;
            definition.sourceOffsetSeconds = clip.sourceOffsetSeconds;
            definition.durationSeconds = clip.durationSeconds;
            definition.sourceTempoBpm = clip.sourceTempoBpm;
            definition.sourceTimeSignatureNumerator = clip.sourceTimeSignatureNumerator;
            definition.sourceTimeSignatureDenominator = clip.sourceTimeSignatureDenominator;
            definition.sourceGrooveFeel = clip.sourceGrooveFeel;
            definition.sourceGrooveSwingAmount = clip.sourceGrooveSwingAmount;
            project.clipDefinitions.push_back(definition);
        }
        if (definitionId.empty()) {
            continue;
        }

        PlaylistClipPlacementState placement;
        placement.id = clip.id;
        placement.clipDefinitionId = definitionId;
        placement.startSeconds = clip.startSeconds;
        placement.originalStartSeconds = clip.originalStartSeconds;
        placement.layer = 0;
        placement.gainDb = clip.gainDb;
        placement.fadeInSeconds = clip.fadeInSeconds;
        placement.fadeOutSeconds = clip.fadeOutSeconds;
        placement.fadeInCurve = normalizedFadeCurve(clip.fadeInCurve);
        placement.fadeOutCurve = normalizedFadeCurve(clip.fadeOutCurve);
        placement.fadeInCurvature = clip.fadeInCurvature;
        placement.fadeOutCurvature = clip.fadeOutCurvature;
        placement.muted = clip.muted;
        placement.polarityInverted = clip.polarityInverted;
        placement.reversed = clip.reversed;
        placement.araPluginName = clip.araPluginName;
        placement.araPluginPath = clip.araPluginPath;
        placement.araSourcePath = clip.araSourcePath;
        placement.araArchiveBase64 = clip.araArchiveBase64;
        placement.locked = clip.locked;
        placement.colorHex = clip.colorHex;
        placement.timeScale = clip.timeScale;
        placement.tempoSyncPolicy = clip.tempoSyncPolicy.empty() ? "project-tempo" : clip.tempoSyncPolicy;
        placement.pendingTimeStretchToProject = clip.pendingTimeStretchToProject;
        placement.legacyClipId = clip.id;
        activeIt->placements.push_back(placement);
    }
}

bool rebuildProjectClipsFromActivePlaylists(ProjectDocument& project) {
    if (project.trackPlaylists.empty()) {
        return false;
    }
    const bool hasActivePlaylist = std::any_of(project.trackPlaylists.begin(), project.trackPlaylists.end(), [](const TrackPlaylistState& playlist) {
        return playlist.active;
    });
    if (project.clipDefinitions.empty()) {
        if (hasActivePlaylist) {
            project.clips.clear();
            return true;
        }
        return false;
    }

    std::vector<ClipState> rebuiltClips;
    std::set<std::string> usedClipIds;
    bool rebuiltAny = false;
    for (const auto& playlist : project.trackPlaylists) {
        if (!playlist.active || playlist.trackName.empty()) {
            continue;
        }
        for (const auto& placement : playlist.placements) {
            const auto* definition = clipDefinitionById(project, placement.clipDefinitionId);
            if (definition == nullptr || definition->durationSeconds <= 0.0) {
                continue;
            }
            const auto* source = mediaSourceById(project, definition->sourceId);
            ClipState clip;
            const std::string requestedId = placement.legacyClipId.empty() ? placement.id : placement.legacyClipId;
            clip.id = requestedId.empty() ? placement.id : requestedId;
            if (clip.id.empty()) {
                clip.id = "clip";
            }
            if (!usedClipIds.insert(clip.id).second) {
                const std::string baseId = clip.id;
                for (int suffix = 2; suffix < 100000; ++suffix) {
                    const std::string candidate = baseId + "-" + std::to_string(suffix);
                    if (usedClipIds.insert(candidate).second) {
                        clip.id = candidate;
                        break;
                    }
                }
            }
            clip.trackName = playlist.trackName;
            clip.sourcePath = source != nullptr ? source->path : std::string();
            clip.regionName = definition->name;
            clip.sourceFileUid = source != nullptr ? source->id : definition->sourceId;
            clip.sourceChannels = source != nullptr ? source->channels : 0;
            clip.sourceSampleRate = source != nullptr ? source->sampleRate : 0.0;
            clip.sourceBitsPerSample = source != nullptr ? source->bitsPerSample : 0;
            clip.sourceFloatingPoint = source != nullptr && source->floatingPoint;
            clip.sourceHasBroadcastTimeReference = source != nullptr && source->hasBroadcastTimeReference;
            clip.sourceTimeReferenceSamples = source != nullptr ? source->timeReferenceSamples : 0;
            clip.sourceTimeReferenceSeconds = source != nullptr ? source->timeReferenceSeconds : 0.0;
            clip.sourceTempoBpm = definition->sourceTempoBpm;
            clip.sourceTimeSignatureNumerator = definition->sourceTimeSignatureNumerator;
            clip.sourceTimeSignatureDenominator = definition->sourceTimeSignatureDenominator;
            clip.sourceGrooveFeel = definition->sourceGrooveFeel;
            clip.sourceGrooveSwingAmount = definition->sourceGrooveSwingAmount;
            clip.startSeconds = placement.startSeconds;
            clip.originalStartSeconds = placement.originalStartSeconds;
            clip.durationSeconds = definition->durationSeconds;
            clip.sourceOffsetSeconds = definition->sourceOffsetSeconds;
            clip.gainDb = placement.gainDb;
            clip.fadeInSeconds = placement.fadeInSeconds;
            clip.fadeOutSeconds = placement.fadeOutSeconds;
            clip.fadeInCurve = normalizedFadeCurve(placement.fadeInCurve);
            clip.fadeOutCurve = normalizedFadeCurve(placement.fadeOutCurve);
            clip.fadeInCurvature = placement.fadeInCurvature;
            clip.fadeOutCurvature = placement.fadeOutCurvature;
            clip.muted = placement.muted;
            clip.polarityInverted = placement.polarityInverted;
            clip.reversed = placement.reversed;
            clip.araPluginName = placement.araPluginName;
            clip.araPluginPath = placement.araPluginPath;
            clip.araSourcePath = placement.araSourcePath;
            clip.araArchiveBase64 = placement.araArchiveBase64;
            clip.locked = placement.locked;
            clip.colorHex = placement.colorHex;
            clip.timeScale = placement.timeScale;
            clip.tempoSyncPolicy = placement.tempoSyncPolicy.empty() ? "project-tempo" : placement.tempoSyncPolicy;
            clip.pendingTimeStretchToProject = placement.pendingTimeStretchToProject;
            rebuiltClips.push_back(clip);
            rebuiltAny = true;
        }
    }
    if (!rebuiltAny && hasAnyPlaylistPlacements(project)) {
        project.clips.clear();
        return true;
    }
    if (!rebuiltAny && hasActivePlaylist) {
        project.clips.clear();
        return true;
    }
    if (!rebuiltAny) {
        return false;
    }
    std::sort(rebuiltClips.begin(), rebuiltClips.end(), [](const ClipState& left, const ClipState& right) {
        if (left.trackName != right.trackName) {
            return left.trackName < right.trackName;
        }
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });
    project.clips = std::move(rebuiltClips);
    return true;
}

void normalizeProjectEditModel(ProjectDocument& project) {
    std::set<std::string> validTrackNames;
    for (const auto& track : project.tracks) {
        if (!track.name.empty() && !isProtectedTrackName(track.name) && isTimelineTargetTrackType(track)) {
            validTrackNames.insert(track.name);
        }
    }

    if (project.mediaSources.empty() || project.clipDefinitions.empty() || project.trackPlaylists.empty()) {
        rebuildProjectEditModelFromClips(project);
    } else {
        // The edit model already exists: fold in any clips that copy/paste/duplicate
        // appended to the flat list so they become real, playable placements.
        mergeOrphanClipsIntoActivePlaylists(project);
    }

    std::set<std::string> validMidiTrackNames;
    for (auto& track : project.tracks) {
        if (track.name.empty() || isProtectedTrackName(track.name) ||
            track.trackType == "folder" || track.trackType == "bus_folder" ||
            track.trackType == "master" || track.trackType == "monitor") {
            continue;
        }
        if (track.trackType == "midi" || track.trackType == "instrument") {
            validMidiTrackNames.insert(track.name);
            if (track.inputBus.empty() || track.inputBus == "Input 1") {
                track.inputBus = "MIDI Input";
            }
            if (track.trackType == "midi" && (track.outputBus.empty() || track.outputBus == "Master")) {
                track.outputBus = "Instrument";
            }
            if (track.trackType == "instrument" && (track.outputBus.empty() || track.outputBus == "Instrument")) {
                track.outputBus = "Master";
            }
            if (track.instrument.midiInput.empty()) {
                track.instrument.midiInput = "MIDI Input";
            }
            track.instrument.midiChannel = std::max(0, std::min(16, track.instrument.midiChannel));
            if (track.trackType == "instrument") {
                normalizeInstrumentRack(track);
            } else {
                track.instrument = normalizedInstrumentSlotForDocument(InstrumentSlotState {});
                track.instrumentSlots.clear();
                track.instrumentRackMode = "parallel";
            }
        }
    }
    project.midiRegions.erase(
        std::remove_if(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
            return region.id.empty() ||
                region.trackName.empty() ||
                validMidiTrackNames.find(region.trackName) == validMidiTrackNames.end() ||
                region.durationSeconds <= 0.0 ||
                !std::isfinite(region.startSeconds) ||
                !std::isfinite(region.durationSeconds);
        }),
        project.midiRegions.end());
    for (auto& region : project.midiRegions) {
        if (region.name.empty()) {
            region.name = "MIDI Region";
        }
        region.startSeconds = finiteRange(region.startSeconds, 0.0, 0.0, 24.0 * 60.0 * 60.0);
        region.durationSeconds = finiteRange(region.durationSeconds, 4.0, 0.01, 24.0 * 60.0 * 60.0);
        region.ticksPerQuarter = std::max(24, std::min(9600, region.ticksPerQuarter));
        for (auto& note : region.notes) {
            note.pitch = std::max(0, std::min(127, note.pitch));
            note.startBeats = finiteRange(note.startBeats, 0.0, 0.0, 1000000.0);
            note.durationBeats = finiteRange(note.durationBeats, 1.0, 1.0 / 960.0, 1000000.0);
            note.velocity = std::max(1, std::min(127, note.velocity));
            note.channel = std::max(1, std::min(16, note.channel));
        }
        region.notes.erase(
            std::remove_if(region.notes.begin(), region.notes.end(), [](const MidiNoteState& note) {
                return note.id.empty();
            }),
            region.notes.end());
        std::sort(region.notes.begin(), region.notes.end(), [](const MidiNoteState& left, const MidiNoteState& right) {
            if (left.startBeats == right.startBeats) {
                if (left.pitch == right.pitch) {
                    return left.id < right.id;
                }
                return left.pitch < right.pitch;
            }
            return left.startBeats < right.startBeats;
        });
        for (auto& event : region.controllerEvents) {
            event.beat = finiteRange(event.beat, 0.0, 0.0, 1000000.0);
            event.controller = std::max(0, std::min(127, event.controller));
            event.value = std::max(0, std::min(127, event.value));
            event.channel = std::max(1, std::min(16, event.channel));
        }
        region.controllerEvents.erase(
            std::remove_if(region.controllerEvents.begin(), region.controllerEvents.end(), [](const MidiControllerEventState& event) {
                return event.id.empty();
            }),
            region.controllerEvents.end());
        std::sort(region.controllerEvents.begin(), region.controllerEvents.end(), [](const MidiControllerEventState& left, const MidiControllerEventState& right) {
            if (left.beat == right.beat) {
                if (left.controller == right.controller) {
                    return left.id < right.id;
                }
                return left.controller < right.controller;
            }
            return left.beat < right.beat;
        });
        for (auto& event : region.pitchBendEvents) {
            event.beat = finiteRange(event.beat, 0.0, 0.0, 1000000.0);
            event.value = std::max(0, std::min(16383, event.value));
            event.channel = std::max(1, std::min(16, event.channel));
        }
        region.pitchBendEvents.erase(
            std::remove_if(region.pitchBendEvents.begin(), region.pitchBendEvents.end(), [](const MidiPitchBendEventState& event) {
                return event.id.empty();
            }),
            region.pitchBendEvents.end());
        std::sort(region.pitchBendEvents.begin(), region.pitchBendEvents.end(), [](const MidiPitchBendEventState& left, const MidiPitchBendEventState& right) {
            if (left.beat == right.beat) {
                return left.id < right.id;
            }
            return left.beat < right.beat;
        });
        for (auto& event : region.programChangeEvents) {
            event.beat = finiteRange(event.beat, 0.0, 0.0, 1000000.0);
            event.program = std::max(0, std::min(127, event.program));
            event.channel = std::max(1, std::min(16, event.channel));
        }
        region.programChangeEvents.erase(
            std::remove_if(region.programChangeEvents.begin(), region.programChangeEvents.end(), [](const MidiProgramChangeEventState& event) {
                return event.id.empty();
            }),
            region.programChangeEvents.end());
        std::sort(region.programChangeEvents.begin(), region.programChangeEvents.end(), [](const MidiProgramChangeEventState& left, const MidiProgramChangeEventState& right) {
            if (left.beat == right.beat) {
                return left.id < right.id;
            }
            return left.beat < right.beat;
        });
    }
    std::sort(project.midiRegions.begin(), project.midiRegions.end(), [](const MidiRegionState& left, const MidiRegionState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });
}

std::string serializeProject(const ProjectDocument& inputProject) {
    ProjectDocument serializable = inputProject;
    normalizeProjectEditModel(serializable);
    std::ostringstream out;
    out << "{\n";
    out << "  \"format\": \"neuracoust-daw-project-v1\",\n";
    const auto& project = serializable;
    out << "  \"name\": \"" << escapeJsonString(project.name) << "\",\n";
    out << "  \"sampleRate\": " << project.sampleRate << ",\n";
    out << "  \"defaultBufferSize\": " << project.defaultBufferSize << ",\n";
    out << "  \"bitDepth\": " << project.bitDepth << ",\n";
    out << "  \"tempoBpm\": " << project.tempoBpm << ",\n";
    out << "  \"timeSignatureNumerator\": " << project.timeSignatureNumerator << ",\n";
    out << "  \"timeSignatureDenominator\": " << project.timeSignatureDenominator << ",\n";
    out << "  \"grooveFeel\": \"" << escapeJsonString(project.grooveFeel.empty() ? "straight" : project.grooveFeel) << "\",\n";
    out << "  \"panLaw\": \"" << escapeJsonString(project.panLaw.empty() ? "legacy" : project.panLaw) << "\",\n";
    out << "  \"grooveSwingAmount\": " << project.grooveSwingAmount << ",\n";
    out << "  \"metronomeSubdivision\": \"" << escapeJsonString(project.metronomeSubdivision.empty() ? "auto" : project.metronomeSubdivision) << "\",\n";
    out << "  \"metronomeGain\": " << project.metronomeGain << ",\n";
    out << "  \"metronomeSound\": \"" << escapeJsonString(project.metronomeSound.empty() ? "beep" : project.metronomeSound) << "\",\n";
    out << "  \"metronomeAccentFirst\": " << (project.metronomeAccentFirst ? "true" : "false") << ",\n";
    out << "  \"metronomeGenre\": \"" << escapeJsonString(project.metronomeGenre.empty() ? "straight" : project.metronomeGenre) << "\",\n";
    out << "  \"metronomeAccentPattern\": [";
    for (size_t i = 0; i < project.metronomeAccentPattern.size(); ++i) {
        if (i != 0) { out << ","; }
        out << project.metronomeAccentPattern[i];
    }
    out << "],\n";
    out << "  \"midiRecordControllers\": [";
    for (size_t i = 0; i < project.midiRecordControllers.size(); ++i) {
        if (i != 0) { out << ","; }
        out << project.midiRecordControllers[i];
    }
    out << "],\n";
    out << "  \"midiRecordPitchBend\": " << (project.midiRecordPitchBend ? "true" : "false") << ",\n";
    out << "  \"detectedKey\": \"" << escapeJsonString(project.detectedKey.empty() ? "C" : project.detectedKey) << "\",\n";
    out << "  \"detectedKeyMode\": \"" << escapeJsonString(project.detectedKeyMode.empty() ? "major" : project.detectedKeyMode) << "\",\n";
    out << "  \"chordKeyModePreference\": \"" << escapeJsonString(project.chordKeyModePreference.empty() ? "auto" : project.chordKeyModePreference) << "\",\n";
    out << "  \"tempoMasterTrackName\": \"" << escapeJsonString(project.tempoMasterTrackName) << "\",\n";
    out << "  \"audioImportTempoPolicy\": \"" << escapeJsonString(project.audioImportTempoPolicy.empty() ? "preserve-project" : project.audioImportTempoPolicy) << "\",\n";
    out << "  \"timecodeStartSeconds\": " << project.timecodeStartSeconds << ",\n";
    out << "  \"videoFrameRate\": " << std::max(1.0, std::min(240.0, project.videoFrameRate)) << ",\n";
    out << "  \"timecodeDropFrame\": " << (project.timecodeDropFrame ? "true" : "false") << ",\n";
    out << "  \"tempoMap\": [\n";
    for (size_t i = 0; i < project.tempoMap.size(); ++i) {
        const auto& tempo = project.tempoMap[i];
        out << "    {\"timeSeconds\":" << tempo.timeSeconds << ",\"bpm\":" << tempo.bpm << "}";
        out << (i + 1 == project.tempoMap.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"timeSignatureMap\": [\n";
    for (size_t i = 0; i < project.timeSignatureMap.size(); ++i) {
        const auto& signature = project.timeSignatureMap[i];
        out << "    {\"timeSeconds\":" << signature.timeSeconds
            << ",\"numerator\":" << signature.numerator
            << ",\"denominator\":" << signature.denominator << "}";
        out << (i + 1 == project.timeSignatureMap.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"beatSnapEnabled\": " << (project.beatSnapEnabled ? "true" : "false") << ",\n";
    out << "  \"editMode\": \"" << escapeJsonString(normalizedEditMode(project.editMode)) << "\",\n";
    out << "  \"gridUnit\": \"" << escapeJsonString(normalizedGridUnit(project.gridUnit)) << "\",\n";
    out << "  \"playbackStartMode\": \"" << escapeJsonString(normalizedPlaybackStartMode(project.playbackStartMode)) << "\",\n";
    out << "  \"delayCompensationEnabled\": " << (project.delayCompensationEnabled ? "true" : "false") << ",\n";
    out << "  \"directMonitoringEnabled\": " << (project.directMonitoringEnabled ? "true" : "false") << ",\n";
    out << "  \"timelineZoomFactor\": " << project.timelineZoomFactor << ",\n";
    out << "  \"timelineFollowMode\": " << project.timelineFollowMode << ",\n";
    out << "  \"trackHeightScale\": " << project.trackHeightScale << ",\n";
    out << "  \"tempoLaneHeightScale\": " << project.tempoLaneHeightScale << ",\n";
    out << "  \"waveformGainScale\": " << project.waveformGainScale << ",\n";
    out << "  \"loopEnabled\": " << (project.loopEnabled ? "true" : "false") << ",\n";
    out << "  \"loopStartSeconds\": " << project.loopStartSeconds << ",\n";
    out << "  \"loopEndSeconds\": " << project.loopEndSeconds << ",\n";
    out << "  \"preRollSeconds\": " << project.preRollSeconds << ",\n";
    out << "  \"postRollSeconds\": " << project.postRollSeconds << ",\n";
    out << "  \"editSelectionEnabled\": " << (project.editSelectionEnabled ? "true" : "false") << ",\n";
    out << "  \"editSelectionStartSeconds\": " << project.editSelectionStartSeconds << ",\n";
    out << "  \"editSelectionEndSeconds\": " << project.editSelectionEndSeconds << ",\n";
    out << "  \"appleSiliconCoreIsolationEnabled\": " << (project.appleSiliconCoreIsolationEnabled ? "true" : "false") << ",\n";
    out << "  \"requestedDspCoreCount\": " << std::max(1, std::min(16, project.requestedDspCoreCount)) << ",\n";
    out << "  \"externalDspCoreCount\": " << std::max(1, std::min(16, project.externalDspCoreCount)) << ",\n";
    out << "  \"externalDspEnabled\": " << (project.externalDspEnabled ? "true" : "false") << ",\n";
    out << "  \"remoteDspHost\": \"" << escapeJsonString(project.remoteDspHost) << "\",\n";
    out << "  \"ndsHost\": \"" << escapeJsonString(project.ndsHost) << "\",\n";
    out << "  \"ndsEnabled\": " << (project.ndsEnabled ? "true" : "false") << ",\n";
    out << "  \"dspRoleMonitor\": \"" << escapeJsonString(project.dspRoleMonitor) << "\",\n";
    out << "  \"dspRoleChannelStrip\": \"" << escapeJsonString(project.dspRoleChannelStrip) << "\",\n";
    out << "  \"dspRoleMaster\": \"" << escapeJsonString(project.dspRoleMaster) << "\",\n";
    out << "  \"dspRoleInserts\": \"" << escapeJsonString(project.dspRoleInserts) << "\",\n";
    out << "  \"dspAutoOverflow\": " << (project.dspAutoOverflow ? "true" : "false") << ",\n";
    out << "  \"physicalSpeakerModel\": \"" << escapeJsonString(project.physicalSpeakerModel) << "\",\n";
    out << "  \"physicalHeadphoneModel\": \"" << escapeJsonString(project.physicalHeadphoneModel) << "\",\n";
    out << "  \"measurementMicModel\": \"" << escapeJsonString(project.measurementMicModel) << "\",\n";
    out << "  \"physicalPowerAmpModel\": \"" << escapeJsonString(project.physicalPowerAmpModel) << "\",\n";
    out << "  \"physicalSpeakerCableModel\": \"" << escapeJsonString(project.physicalSpeakerCableModel) << "\",\n";
    out << "  \"physicalPowerCableModel\": \"" << escapeJsonString(project.physicalPowerCableModel) << "\",\n";
    out << "  \"physicalConnectorModel\": \"" << escapeJsonString(project.physicalConnectorModel) << "\",\n";
    out << "  \"physicalAudioInterfaceModel\": \"" << escapeJsonString(project.physicalAudioInterfaceModel) << "\",\n";
    out << "  \"physicalAudioInterfaceTargetModel\": \"" << escapeJsonString(project.physicalAudioInterfaceTargetModel) << "\",\n";
    out << "  \"monitorInterfaceModelingEnabled\": " << (project.monitorInterfaceModelingEnabled ? "true" : "false") << ",\n";
    out << "  \"monitorEqBands\": [\n";
    for (size_t i = 0; i < project.monitorEqBands.size(); ++i) {
        const auto& band = project.monitorEqBands[i];
        out << "    {\"enabled\":" << (band.enabled ? "true" : "false")
            << ",\"type\":\"" << escapeJsonString(band.type) << "\""
            << ",\"frequencyHz\":" << band.frequencyHz
            << ",\"gainDb\":" << band.gainDb
            << ",\"q\":" << band.q << "}";
        out << (i + 1 == project.monitorEqBands.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"monitorSpeakerHeadphoneExclusive\": " << (project.monitorSpeakerHeadphoneExclusive ? "true" : "false") << ",\n";
    out << "  \"autoFadeOutSeconds\": " << project.autoFadeOutSeconds << ",\n";
    out << "  \"autoFadeOutCurve\": \"" << escapeJsonString(project.autoFadeOutCurve) << "\",\n";
    out << "  \"windowsProcessorAffinityEnabled\": " << (project.windowsProcessorAffinityEnabled ? "true" : "false") << ",\n";
    out << "  \"windowsProcessorAffinityMode\": \"" << escapeJsonString(project.windowsProcessorAffinityMode) << "\",\n";
    const std::string monitorListenMode = project.monitorStationListenMode.empty() ? "LR" : project.monitorStationListenMode;
    const bool monitorMsMode = monitorListenMode == "M" || monitorListenMode == "S";
    out << "  \"monitorStationMono\": " << (!monitorMsMode && project.monitorStationMono ? "true" : "false") << ",\n";
    out << "  \"monitorStationListenMode\": \"" << escapeJsonString(monitorListenMode) << "\",\n";
    out << "  \"monitorStationSwapLeftRight\": " << (!monitorMsMode && project.monitorStationSwapLeftRight ? "true" : "false") << ",\n";
    out << "  \"monitorStationInvertLeft\": " << (project.monitorStationInvertLeft ? "true" : "false") << ",\n";
    out << "  \"monitorStationInvertRight\": " << (project.monitorStationInvertRight ? "true" : "false") << ",\n";
    out << "  \"monitorStationMute\": " << (project.monitorStationMute ? "true" : "false") << ",\n";
    out << "  \"monitorStationDim\": " << (project.monitorStationDim ? "true" : "false") << ",\n";
    out << "  \"monitorStationTalkback\": " << (project.monitorStationTalkback ? "true" : "false") << ",\n";
    out << "  \"monitorStationDimDb\": " << std::max(-60.0f, std::min(0.0f, project.monitorStationDimDb)) << ",\n";
    out << "  \"monitorStationTalkbackRoute\": \"" << escapeJsonString(project.monitorStationTalkbackRoute.empty() ? "listen_room" : project.monitorStationTalkbackRoute) << "\",\n";
    out << "  \"monitorStationTalkbackChannel\": " << std::max(1, project.monitorStationTalkbackChannel) << ",\n";
    out << "  \"monitorInputTrimDb\": " << std::max(-12.0f, std::min(0.0f, project.monitorInputTrimDb)) << ",\n";
    out << "  \"monitorVolumeDb\": " << std::max(-120.0f, std::min(12.0f, project.monitorVolumeDb)) << ",\n";
    out << "  \"listenRoomEnabled\": " << (project.listenRoomEnabled ? "true" : "false") << ",\n";
    out << "  \"listenRoomSessionName\": \"" << escapeJsonString(project.listenRoomSessionName.empty() ? "mix" : project.listenRoomSessionName) << "\",\n";
    out << "  \"listenRoomSource\": \"" << escapeJsonString(project.listenRoomSource.empty() ? "monitor" : project.listenRoomSource) << "\",\n";
    out << "  \"listenRoomQuality\": \"" << escapeJsonString(project.listenRoomQuality.empty() ? "opus_high" : project.listenRoomQuality) << "\",\n";
    out << "  \"listenRoomLatencyMode\": \"" << escapeJsonString(project.listenRoomLatencyMode.empty() ? "stable" : project.listenRoomLatencyMode) << "\",\n";
    out << "  \"listenRoomTransportMode\": \"" << escapeJsonString(project.listenRoomTransportMode.empty() ? "direct_fallback" : project.listenRoomTransportMode) << "\",\n";
    out << "  \"listenRoomRelayHost\": \"" << escapeJsonString(project.listenRoomRelayHost.empty() ? "127.0.0.1" : project.listenRoomRelayHost) << "\",\n";
    out << "  \"listenRoomAccessToken\": \"" << escapeJsonString(project.listenRoomAccessToken) << "\",\n";
    out << "  \"listenRoomRelayHttpPort\": " << std::max(1, std::min(65535, project.listenRoomRelayHttpPort)) << ",\n";
    out << "  \"listenRoomRelayTcpIngestPort\": " << std::max(1, std::min(65535, project.listenRoomRelayTcpIngestPort)) << ",\n";
    out << "  \"tracks\": [\n";
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const auto& track = project.tracks[i];
        out << "    {\"name\":\"" << escapeJsonString(track.name) << "\",\"volumeDb\":" << track.volumeDb
            << ",\"trackType\":\"" << escapeJsonString(track.trackType.empty() ? inferredTrackType(track) : track.trackType) << "\""
            << ",\"colorHex\":\"" << escapeJsonString(track.colorHex) << "\""
            << ",\"displayHeightScale\":" << track.displayHeightScale
            << ",\"folderName\":\"" << escapeJsonString(track.folderName) << "\""
            << ",\"folderCollapsed\":" << (track.folderCollapsed ? "true" : "false")
            << ",\"mixerHidden\":" << (track.mixerHidden ? "true" : "false")
            << ",\"mixerOrder\":" << track.mixerOrder
            << ",\"automationMode\":\"" << escapeJsonString(track.automationMode.empty() ? "read" : track.automationMode) << "\""
            << ",\"trackViewMode\":\"" << escapeJsonString(normalizedTrackViewMode(track.trackViewMode)) << "\""
            << ",\"timebaseMode\":\"" << escapeJsonString(normalizedTrackTimebaseMode(track.timebaseMode)) << "\""
            << ",\"elasticAudioMode\":\"" << escapeJsonString(normalizedElasticAudioMode(track.elasticAudioMode)) << "\""
            << ",\"mixGroupName\":\"" << escapeJsonString(track.mixGroupName) << "\""
            << ",\"controlMasterTrackName\":\"" << escapeJsonString(track.controlMasterTrackName) << "\""
            << ",\"notes\":\"" << escapeJsonString(track.notes) << "\""
            << ",\"consoleDspMachine\":\"" << escapeJsonString(track.consoleDspMachine) << "\""
            << ",\"consoleModel\":\"" << escapeJsonString(track.consoleChannel.model) << "\""
            << ",\"consoleModuleOrder\":\"" << escapeJsonString(track.consoleChannel.moduleOrder) << "\""
            << ",\"consoleFilterEnabled\":" << (track.consoleChannel.filterEnabled ? "true" : "false")
            << ",\"consoleFilterCircuitMode\":" << (track.consoleChannel.filterCircuitMode ? "true" : "false")
            << ",\"consoleHighPassEnabled\":" << (track.consoleChannel.highPassEnabled ? "true" : "false")
            << ",\"consoleLowPassEnabled\":" << (track.consoleChannel.lowPassEnabled ? "true" : "false")
            << ",\"consoleHighPassHz\":" << track.consoleChannel.highPassHz
            << ",\"consoleLowPassHz\":" << track.consoleChannel.lowPassHz
            << ",\"consoleEqEnabled\":" << (track.consoleChannel.eqEnabled ? "true" : "false")
            << ",\"consoleEqCircuitMode\":" << (track.consoleChannel.eqCircuitMode ? "true" : "false")
            << ",\"consoleCompEnabled\":" << (track.consoleChannel.compEnabled ? "true" : "false")
            << ",\"consoleCompCircuitMode\":" << (track.consoleChannel.compCircuitMode ? "true" : "false")
            << ",\"consoleGateEnabled\":" << (track.consoleChannel.gateEnabled ? "true" : "false")
            << ",\"consoleGateCircuitMode\":" << (track.consoleChannel.gateCircuitMode ? "true" : "false")
            << ",\"consoleSaturatorEnabled\":" << (track.consoleChannel.saturatorEnabled ? "true" : "false")
            << ",\"consoleDualMono\":" << (track.consoleChannel.dualMono ? "true" : "false")
            << ",\"consoleFilterDualMono\":" << (track.consoleChannel.filterDualMono ? "true" : "false")
            << ",\"consoleEqDualMono\":" << (track.consoleChannel.eqDualMono ? "true" : "false")
            << ",\"consoleCompDualMono\":" << (track.consoleChannel.compDualMono ? "true" : "false")
            << ",\"consoleGateDualMono\":" << (track.consoleChannel.gateDualMono ? "true" : "false")
            << ",\"consoleSaturatorDualMono\":" << (track.consoleChannel.saturatorDualMono ? "true" : "false")
            << ",\"consoleSaturatorCircuitMode\":" << (track.consoleChannel.saturatorCircuitMode ? "true" : "false")
            << ",\"consoleSaturatorDriveDb\":" << track.consoleChannel.saturatorDriveDb
            << ",\"consoleSaturatorMix\":" << track.consoleChannel.saturatorMix
            << ",\"consoleExpanderMode\":" << (track.consoleChannel.expanderMode ? "true" : "false")
            << ",\"consoleCompThresholdDb\":" << track.consoleChannel.compThresholdDb
            << ",\"consoleCompRatio\":" << track.consoleChannel.compRatio
            << ",\"consoleCompAttackMs\":" << track.consoleChannel.compAttackMs
            << ",\"consoleCompReleaseMs\":" << track.consoleChannel.compReleaseMs
            << ",\"consoleCompMix\":" << track.consoleChannel.compMix
            << ",\"consoleCompFastAttack\":" << (track.consoleChannel.compFastAttack ? "true" : "false")
            << ",\"consoleCompPeakMode\":" << (track.consoleChannel.compPeakMode ? "true" : "false")
            << ",\"consoleCompType\":\"" << escapeJsonString(track.consoleChannel.compType) << "\""
            << ",\"consoleGateThresholdDb\":" << track.consoleChannel.gateThresholdDb
            << ",\"consoleGateRangeDb\":" << track.consoleChannel.gateRangeDb
            << ",\"consoleGateAttackMs\":" << track.consoleChannel.gateAttackMs
            << ",\"consoleGateHoldMs\":" << track.consoleChannel.gateHoldMs
            << ",\"consoleGateReleaseMs\":" << track.consoleChannel.gateReleaseMs
            << ",\"consoleGateFastAttack\":" << (track.consoleChannel.gateFastAttack ? "true" : "false")
            << ",\"consoleGateType\":\"" << escapeJsonString(track.consoleChannel.gateType) << "\""
            << ",\"consoleEqHfGainDb\":" << track.consoleChannel.eqHfGainDb
            << ",\"consoleEqHfHz\":" << track.consoleChannel.eqHfHz
            << ",\"consoleEqHfBell\":" << (track.consoleChannel.eqHfBell ? "true" : "false")
            << ",\"consoleEqHmfGainDb\":" << track.consoleChannel.eqHmfGainDb
            << ",\"consoleEqHmfHz\":" << track.consoleChannel.eqHmfHz
            << ",\"consoleEqHmfQ\":" << track.consoleChannel.eqHmfQ
            << ",\"consoleEqLmfGainDb\":" << track.consoleChannel.eqLmfGainDb
            << ",\"consoleEqLmfHz\":" << track.consoleChannel.eqLmfHz
            << ",\"consoleEqLmfQ\":" << track.consoleChannel.eqLmfQ
            << ",\"consoleEqLfGainDb\":" << track.consoleChannel.eqLfGainDb
            << ",\"consoleEqLfHz\":" << track.consoleChannel.eqLfHz
            << ",\"consoleEqLfBell\":" << (track.consoleChannel.eqLfBell ? "true" : "false")
            << ",\"consoleEqEPattern\":" << (track.consoleChannel.eqEMode ? "true" : "false")
            << ",\"consolePhaseInvert\":" << (track.consoleChannel.phaseInvert ? "true" : "false")
            << ",\"consolePhaseInvertL\":" << (track.consoleChannel.phaseInvertL ? "true" : "false")
            << ",\"consolePhaseInvertR\":" << (track.consoleChannel.phaseInvertR ? "true" : "false")
            << ",\"consoleChannelBiasSeed\":" << track.consoleChannel.channelBiasSeed
            << ",\"consoleChannelBiasDepth\":" << track.consoleChannel.channelBiasDepth
            << ",\"consoleEqType\":\"" << escapeJsonString(track.consoleChannel.eqType) << "\""
            << ",\"channelFormat\":\"" << escapeJsonString(track.channelFormat == "mono" ? "mono" : "stereo") << "\""
            << ",\"pan\":" << track.pan << ",\"muted\":" << (track.muted ? "true" : "false")
            << ",\"solo\":" << (track.solo ? "true" : "false")
            << ",\"recordArmed\":" << (track.recordArmed ? "true" : "false")
            << ",\"inputMonitoring\":" << (track.inputMonitoring ? "true" : "false")
            << ",\"inputBus\":\"" << escapeJsonString(track.inputBus)
            << "\",\"outputBus\":\"" << escapeJsonString(track.outputBus) << "\",\"inserts\":";
        writeTrackInsertSlots(out, track.inserts);
        out << ",\"instrument\":";
        writeInstrumentSlot(out, track.instrument);
        out << ",\"instrumentRackMode\":\"" << escapeJsonString(track.instrumentRackMode.empty() ? "parallel" : track.instrumentRackMode) << "\"";
        out << ",\"instrumentSlots\":";
        writeInstrumentSlots(out, track.instrumentSlots);
        out << ",\"sends\":";
        writeTrackSends(out, track.sends);
        out << ",\"volumeAutomation\":";
        writeAutomationPoints(out, track.volumeAutomation);
        out << ",\"automationLanes\":";
        writeAutomationLanes(out, track.automationLanes);
        out << "}";
        out << (i + 1 == project.tracks.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"clips\": [\n";
    for (size_t i = 0; i < project.clips.size(); ++i) {
        const auto& clip = project.clips[i];
        out << "    {\"id\":\"" << escapeJsonString(clip.id) << "\",\"trackName\":\"" << escapeJsonString(clip.trackName)
            << "\",\"sourcePath\":\"" << escapeJsonString(clip.sourcePath)
            << "\",\"regionName\":\"" << escapeJsonString(clip.regionName)
            << "\",\"sourceFileUid\":\"" << escapeJsonString(clip.sourceFileUid)
            << "\",\"sourceChannels\":" << clip.sourceChannels
            << ",\"sourceSampleRate\":" << clip.sourceSampleRate
            << ",\"sourceBitsPerSample\":" << clip.sourceBitsPerSample
            << ",\"sourceFloatingPoint\":" << (clip.sourceFloatingPoint ? "true" : "false")
            << ",\"sourceHasBroadcastTimeReference\":" << (clip.sourceHasBroadcastTimeReference ? "true" : "false")
            << ",\"sourceTimeReferenceSamples\":" << clip.sourceTimeReferenceSamples
            << ",\"sourceTimeReferenceSeconds\":" << clip.sourceTimeReferenceSeconds
            << ",\"sourceTempoBpm\":" << clip.sourceTempoBpm
            << ",\"sourceTimeSignatureNumerator\":" << clip.sourceTimeSignatureNumerator
            << ",\"sourceTimeSignatureDenominator\":" << clip.sourceTimeSignatureDenominator
            << ",\"sourceGrooveFeel\":\"" << escapeJsonString(clip.sourceGrooveFeel) << "\""
            << ",\"sourceGrooveSwingAmount\":" << clip.sourceGrooveSwingAmount
            << ",\"araPluginName\":\"" << escapeJsonString(clip.araPluginName) << "\""
            << ",\"araPluginPath\":\"" << escapeJsonString(clip.araPluginPath) << "\""
            << ",\"araSourcePath\":\"" << escapeJsonString(clip.araSourcePath) << "\""
            << ",\"araArchiveBase64\":\"" << escapeJsonString(clip.araArchiveBase64) << "\""
            << ",\"timeScale\":" << clip.timeScale
            << ",\"tempoSyncPolicy\":\"" << escapeJsonString(clip.tempoSyncPolicy.empty() ? "project-tempo" : clip.tempoSyncPolicy) << "\""
            << ",\"pendingTimeStretchToProject\":" << (clip.pendingTimeStretchToProject ? "true" : "false")
            << ",\"colorHex\":\"" << escapeJsonString(clip.colorHex) << "\",\"startSeconds\":" << clip.startSeconds
            << ",\"durationSeconds\":" << clip.durationSeconds << ",\"sourceOffsetSeconds\":"
            << clip.sourceOffsetSeconds << ",\"gainDb\":" << clip.gainDb
            << ",\"fadeInSeconds\":" << clip.fadeInSeconds
            << ",\"fadeOutSeconds\":" << clip.fadeOutSeconds
            << ",\"fadeInCurve\":\"" << escapeJsonString(normalizedFadeCurve(clip.fadeInCurve))
            << "\",\"fadeOutCurve\":\"" << escapeJsonString(normalizedFadeCurve(clip.fadeOutCurve)) << "\""
            << ",\"fadeInCurvature\":" << clip.fadeInCurvature
            << ",\"fadeOutCurvature\":" << clip.fadeOutCurvature
            << ",\"originalStartSeconds\":" << clip.originalStartSeconds
            << ",\"muted\":" << (clip.muted ? "true" : "false")
            << ",\"polarityInverted\":" << (clip.polarityInverted ? "true" : "false")
            << ",\"reversed\":" << (clip.reversed ? "true" : "false")
            << ",\"locked\":" << (clip.locked ? "true" : "false") << "}";
        out << (i + 1 == project.clips.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"sessionEditModelVersion\": 1,\n";
    out << "  \"mediaSources\": [\n";
    for (size_t i = 0; i < project.mediaSources.size(); ++i) {
        const auto& source = project.mediaSources[i];
        out << "    {\"id\":\"" << escapeJsonString(source.id)
            << "\",\"path\":\"" << escapeJsonString(source.path)
            << "\",\"displayName\":\"" << escapeJsonString(source.displayName)
            << "\",\"channels\":" << source.channels
            << ",\"sampleRate\":" << source.sampleRate
            << ",\"bitsPerSample\":" << source.bitsPerSample
            << ",\"floatingPoint\":" << (source.floatingPoint ? "true" : "false")
            << ",\"hasBroadcastTimeReference\":" << (source.hasBroadcastTimeReference ? "true" : "false")
            << ",\"timeReferenceSamples\":" << source.timeReferenceSamples
            << ",\"timeReferenceSeconds\":" << source.timeReferenceSeconds << "}";
        out << (i + 1 == project.mediaSources.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"clipDefinitions\": [\n";
    for (size_t i = 0; i < project.clipDefinitions.size(); ++i) {
        const auto& definition = project.clipDefinitions[i];
        out << "    {\"id\":\"" << escapeJsonString(definition.id)
            << "\",\"sourceId\":\"" << escapeJsonString(definition.sourceId)
            << "\",\"name\":\"" << escapeJsonString(definition.name)
            << "\",\"sourceOffsetSeconds\":" << definition.sourceOffsetSeconds
            << ",\"durationSeconds\":" << definition.durationSeconds
            << ",\"sourceTempoBpm\":" << definition.sourceTempoBpm
            << ",\"sourceTimeSignatureNumerator\":" << definition.sourceTimeSignatureNumerator
            << ",\"sourceTimeSignatureDenominator\":" << definition.sourceTimeSignatureDenominator
            << ",\"sourceGrooveFeel\":\"" << escapeJsonString(definition.sourceGrooveFeel)
            << "\",\"sourceGrooveSwingAmount\":" << definition.sourceGrooveSwingAmount << "}";
        out << (i + 1 == project.clipDefinitions.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"videoSources\": [\n";
    for (size_t i = 0; i < project.videoSources.size(); ++i) {
        const auto& source = project.videoSources[i];
        out << "    {\"id\":\"" << escapeJsonString(source.id)
            << "\",\"path\":\"" << escapeJsonString(source.path)
            << "\",\"displayName\":\"" << escapeJsonString(source.displayName)
            << "\",\"frameRate\":" << std::max(1.0, std::min(240.0, source.frameRate))
            << ",\"durationSeconds\":" << std::max(0.0, source.durationSeconds)
            << ",\"width\":" << std::max(0, source.width)
            << ",\"height\":" << std::max(0, source.height)
            << ",\"hasAudio\":" << (source.hasAudio ? "true" : "false") << "}";
        out << (i + 1 == project.videoSources.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"videoClips\": [\n";
    for (size_t i = 0; i < project.videoClips.size(); ++i) {
        const auto& clip = project.videoClips[i];
        out << "    {\"id\":\"" << escapeJsonString(clip.id)
            << "\",\"sourceId\":\"" << escapeJsonString(clip.sourceId)
            << "\",\"name\":\"" << escapeJsonString(clip.name)
            << "\",\"startSeconds\":" << std::max(0.0, clip.startSeconds)
            << ",\"durationSeconds\":" << std::max(0.0, clip.durationSeconds)
            << ",\"sourceOffsetSeconds\":" << std::max(0.0, clip.sourceOffsetSeconds)
            << ",\"sourceTimecodeStartSeconds\":" << std::max(0.0, clip.sourceTimecodeStartSeconds)
            << ",\"muted\":" << (clip.muted ? "true" : "false")
            << ",\"locked\":" << (clip.locked ? "true" : "false") << "}";
        out << (i + 1 == project.videoClips.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"trackPlaylists\": [\n";
    for (size_t i = 0; i < project.trackPlaylists.size(); ++i) {
        const auto& playlist = project.trackPlaylists[i];
        out << "    {\"id\":\"" << escapeJsonString(playlist.id)
            << "\",\"trackName\":\"" << escapeJsonString(playlist.trackName)
            << "\",\"name\":\"" << escapeJsonString(playlist.name.empty() ? "Playlist 1" : playlist.name)
            << "\",\"active\":" << (playlist.active ? "true" : "false")
            << ",\"placements\":[";
        for (size_t placementIndex = 0; placementIndex < playlist.placements.size(); ++placementIndex) {
            const auto& placement = playlist.placements[placementIndex];
            out << "{\"id\":\"" << escapeJsonString(placement.id)
                << "\",\"clipDefinitionId\":\"" << escapeJsonString(placement.clipDefinitionId)
                << "\",\"startSeconds\":" << placement.startSeconds
                << ",\"originalStartSeconds\":" << placement.originalStartSeconds
                << ",\"layer\":" << placement.layer
                << ",\"gainDb\":" << placement.gainDb
                << ",\"fadeInSeconds\":" << placement.fadeInSeconds
                << ",\"fadeOutSeconds\":" << placement.fadeOutSeconds
                << ",\"fadeInCurve\":\"" << escapeJsonString(normalizedFadeCurve(placement.fadeInCurve))
                << "\",\"fadeOutCurve\":\"" << escapeJsonString(normalizedFadeCurve(placement.fadeOutCurve))
                << "\",\"fadeInCurvature\":" << placement.fadeInCurvature
                << ",\"fadeOutCurvature\":" << placement.fadeOutCurvature
                << ",\"muted\":" << (placement.muted ? "true" : "false")
                << ",\"polarityInverted\":" << (placement.polarityInverted ? "true" : "false")
                << ",\"reversed\":" << (placement.reversed ? "true" : "false")
                << ",\"araPluginName\":\"" << escapeJsonString(placement.araPluginName) << "\""
                << ",\"araPluginPath\":\"" << escapeJsonString(placement.araPluginPath) << "\""
                << ",\"araSourcePath\":\"" << escapeJsonString(placement.araSourcePath) << "\""
                << ",\"araArchiveBase64\":\"" << escapeJsonString(placement.araArchiveBase64) << "\""
                << ",\"locked\":" << (placement.locked ? "true" : "false")
                << ",\"colorHex\":\"" << escapeJsonString(placement.colorHex)
                << "\",\"timeScale\":" << placement.timeScale
                << ",\"tempoSyncPolicy\":\"" << escapeJsonString(placement.tempoSyncPolicy.empty() ? "project-tempo" : placement.tempoSyncPolicy)
                << "\",\"pendingTimeStretchToProject\":" << (placement.pendingTimeStretchToProject ? "true" : "false")
                << ",\"legacyClipId\":\"" << escapeJsonString(placement.legacyClipId) << "\"}";
            out << (placementIndex + 1 == playlist.placements.size() ? "" : ",");
        }
        out << "]}";
        out << (i + 1 == project.trackPlaylists.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"markers\": [\n";
    for (size_t i = 0; i < project.markers.size(); ++i) {
        const auto& marker = project.markers[i];
        out << "    {\"id\":\"" << escapeJsonString(marker.id)
            << "\",\"name\":\"" << escapeJsonString(marker.name)
            << "\",\"timeSeconds\":" << marker.timeSeconds
            << ",\"memoryType\":\"" << escapeJsonString(marker.memoryType.empty() ? "marker" : marker.memoryType)
            << "\",\"selectionStartSeconds\":" << marker.selectionStartSeconds
            << ",\"selectionEndSeconds\":" << marker.selectionEndSeconds
            << ",\"referenceMode\":\"" << escapeJsonString(marker.referenceMode.empty() ? "absolute" : marker.referenceMode)
            << "\",\"recallZoom\":" << (marker.recallZoom ? "true" : "false")
            << ",\"recallPrePostRoll\":" << (marker.recallPrePostRoll ? "true" : "false")
            << ",\"recallTrackVisibility\":" << (marker.recallTrackVisibility ? "true" : "false")
            << ",\"recallTrackHeights\":" << (marker.recallTrackHeights ? "true" : "false")
            << ",\"recallGroups\":" << (marker.recallGroups ? "true" : "false")
            << ",\"recallWindowConfiguration\":" << (marker.recallWindowConfiguration ? "true" : "false")
            << ",\"storedTimelineZoomFactor\":" << marker.storedTimelineZoomFactor
            << ",\"storedTrackHeightScale\":" << marker.storedTrackHeightScale
            << ",\"storedPreRollSeconds\":" << marker.storedPreRollSeconds
            << ",\"storedPostRollSeconds\":" << marker.storedPostRollSeconds
            << ",\"windowConfigurationName\":\"" << escapeJsonString(marker.windowConfigurationName)
            << "\",\"comment\":\"" << escapeJsonString(marker.comment) << "\"}";
        out << (i + 1 == project.markers.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"chordEvents\": [\n";
    for (size_t i = 0; i < project.chordEvents.size(); ++i) {
        const auto& chord = project.chordEvents[i];
        out << "    {\"id\":\"" << escapeJsonString(chord.id) << "\",\"name\":\"" << escapeJsonString(chord.name)
            << "\",\"timeSeconds\":" << chord.timeSeconds << "}";
        out << (i + 1 == project.chordEvents.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"songSections\": [\n";
    for (size_t i = 0; i < project.songSections.size(); ++i) {
        const auto& section = project.songSections[i];
        out << "    {\"id\":\"" << escapeJsonString(section.id) << "\",\"name\":\"" << escapeJsonString(section.name)
            << "\",\"timeSeconds\":" << section.timeSeconds << "}";
        out << (i + 1 == project.songSections.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"lyricEvents\": [\n";
    for (size_t i = 0; i < project.lyricEvents.size(); ++i) {
        const auto& lyric = project.lyricEvents[i];
        out << "    {\"id\":\"" << escapeJsonString(lyric.id) << "\",\"text\":\"" << escapeJsonString(lyric.text)
            << "\",\"timeSeconds\":" << lyric.timeSeconds << "}";
        out << (i + 1 == project.lyricEvents.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"midiRegions\": [\n";
    for (size_t i = 0; i < project.midiRegions.size(); ++i) {
        const auto& region = project.midiRegions[i];
        out << "    {\"id\":\"" << escapeJsonString(region.id)
            << "\",\"trackName\":\"" << escapeJsonString(region.trackName)
            << "\",\"name\":\"" << escapeJsonString(region.name.empty() ? "MIDI Region" : region.name)
            << "\",\"startSeconds\":" << region.startSeconds
            << ",\"durationSeconds\":" << region.durationSeconds
            << ",\"ticksPerQuarter\":" << std::max(24, std::min(9600, region.ticksPerQuarter))
            << ",\"loopEnabled\":" << (region.loopEnabled ? "true" : "false")
            << ",\"muted\":" << (region.muted ? "true" : "false")
            << ",\"locked\":" << (region.locked ? "true" : "false")
            << ",\"colorHex\":\"" << escapeJsonString(region.colorHex)
            << "\",\"notes\":[";
        for (size_t noteIndex = 0; noteIndex < region.notes.size(); ++noteIndex) {
            const auto& note = region.notes[noteIndex];
            out << "{\"id\":\"" << escapeJsonString(note.id)
                << "\",\"pitch\":" << std::max(0, std::min(127, note.pitch))
                << ",\"startBeats\":" << note.startBeats
                << ",\"durationBeats\":" << note.durationBeats
                << ",\"velocity\":" << std::max(1, std::min(127, note.velocity))
                << ",\"channel\":" << std::max(1, std::min(16, note.channel))
                << ",\"muted\":" << (note.muted ? "true" : "false")
                << ",\"colorHex\":\"" << escapeJsonString(note.colorHex) << "\"}";
            out << (noteIndex + 1 == region.notes.size() ? "" : ",");
        }
        out << "],\"controllerEvents\":[";
        for (size_t eventIndex = 0; eventIndex < region.controllerEvents.size(); ++eventIndex) {
            const auto& event = region.controllerEvents[eventIndex];
            out << "{\"id\":\"" << escapeJsonString(event.id)
                << "\",\"beat\":" << event.beat
                << ",\"controller\":" << std::max(0, std::min(127, event.controller))
                << ",\"value\":" << std::max(0, std::min(127, event.value))
                << ",\"channel\":" << std::max(1, std::min(16, event.channel)) << "}";
            out << (eventIndex + 1 == region.controllerEvents.size() ? "" : ",");
        }
        out << "],\"pitchBendEvents\":[";
        for (size_t eventIndex = 0; eventIndex < region.pitchBendEvents.size(); ++eventIndex) {
            const auto& event = region.pitchBendEvents[eventIndex];
            out << "{\"id\":\"" << escapeJsonString(event.id)
                << "\",\"beat\":" << event.beat
                << ",\"value\":" << std::max(0, std::min(16383, event.value))
                << ",\"channel\":" << std::max(1, std::min(16, event.channel)) << "}";
            out << (eventIndex + 1 == region.pitchBendEvents.size() ? "" : ",");
        }
        out << "],\"programChangeEvents\":[";
        for (size_t eventIndex = 0; eventIndex < region.programChangeEvents.size(); ++eventIndex) {
            const auto& event = region.programChangeEvents[eventIndex];
            out << "{\"id\":\"" << escapeJsonString(event.id)
                << "\",\"beat\":" << event.beat
                << ",\"program\":" << std::max(0, std::min(127, event.program))
                << ",\"channel\":" << std::max(1, std::min(16, event.channel)) << "}";
            out << (eventIndex + 1 == region.programChangeEvents.size() ? "" : ",");
        }
        out << "]}";
        out << (i + 1 == project.midiRegions.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"masterInserts\": [\n";
    for (size_t i = 0; i < project.masterInserts.size(); ++i) {
        const auto& insert = project.masterInserts[i];
        out << "    {\"pluginName\":\"" << escapeJsonString(insert.pluginName) << "\",\"pluginAppId\":\""
            << escapeJsonString(insert.pluginAppId) << "\",\"pluginFormat\":\"" << escapeJsonString(insert.pluginFormat)
            << "\",\"pluginPath\":\"" << escapeJsonString(insert.pluginPath)
            << "\",\"pluginClassId\":\"" << escapeJsonString(insert.pluginClassId)
            << "\",\"pluginClassName\":\"" << escapeJsonString(insert.pluginClassName) << "\",\"bypassed\":"
            << (insert.bypassed ? "true" : "false") << ",\"available\":"
            << (insert.available ? "true" : "false")
            << ",\"dspExecutionMode\":\"" << escapeJsonString(insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode)
            << "\",\"assignedDspServerId\":\"" << escapeJsonString(insert.assignedDspServerId)
            << "\",\"serverModuleId\":\"" << escapeJsonString(insert.serverModuleId)
            << "\",\"externalPluginId\":\"" << escapeJsonString(insert.serverModuleId)
            << "\",\"reportedLatencySamples\":" << insert.reportedLatencySamples
            << ",\"dspAvailable\":" << (insert.dspAvailable ? "true" : "false")
            << ",\"dspLastError\":\"" << escapeJsonString(insert.dspLastError)
            << "\",\"parameters\":";
        writeVst3Parameters(out, insert.parameters);
        out << "}";
        out << (i + 1 == project.masterInserts.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"monitorModules\": [\n";
    for (size_t i = 0; i < project.monitorModules.size(); ++i) {
        const auto& module = project.monitorModules[i];
	        out << "    {\"id\":\"" << escapeJsonString(module.id) << "\",\"enabled\":" << (module.enabled ? "true" : "false")
	            << ",\"realModel\":\"" << escapeJsonString(module.realModel)
	            << "\",\"targetModelA\":\"" << escapeJsonString(module.targetModelA)
	            << "\",\"targetModelB\":\"" << escapeJsonString(module.targetModelB)
	            << "\",\"targetModelC\":\"" << escapeJsonString(module.targetModelC)
	            << "\",\"speakerOutputA\":\"" << escapeJsonString(module.speakerOutputA)
	            << "\",\"speakerOutputB\":\"" << escapeJsonString(module.speakerOutputB)
	            << "\",\"speakerOutputC\":\"" << escapeJsonString(module.speakerOutputC)
	            << "\",\"powerAmpA\":\"" << escapeJsonString(module.powerAmpA)
	            << "\",\"powerAmpB\":\"" << escapeJsonString(module.powerAmpB)
	            << "\",\"powerAmpC\":\"" << escapeJsonString(module.powerAmpC)
	            << "\",\"speakerCableA\":\"" << escapeJsonString(module.speakerCableA)
	            << "\",\"speakerCableB\":\"" << escapeJsonString(module.speakerCableB)
	            << "\",\"speakerCableC\":\"" << escapeJsonString(module.speakerCableC)
	            << "\",\"realModelA\":\"" << escapeJsonString(module.realModelA)
	            << "\",\"realModelB\":\"" << escapeJsonString(module.realModelB)
	            << "\",\"realModelC\":\"" << escapeJsonString(module.realModelC)
	            << "\",\"realAmpA\":\"" << escapeJsonString(module.realAmpA)
	            << "\",\"realAmpB\":\"" << escapeJsonString(module.realAmpB)
	            << "\",\"realAmpC\":\"" << escapeJsonString(module.realAmpC)
	            << "\",\"realCableA\":\"" << escapeJsonString(module.realCableA)
	            << "\",\"realCableB\":\"" << escapeJsonString(module.realCableB)
	            << "\",\"realCableC\":\"" << escapeJsonString(module.realCableC)
	            << "\",\"streamingPreview\":\"" << escapeJsonString(module.streamingPreview)
	            << "\",\"activeTargetSlot\":" << std::max(0, std::min(2, module.activeTargetSlot))
	            << ",\"speakerRoomEqA\":" << (module.speakerRoomEqA ? "true" : "false")
	            << ",\"speakerRoomEqB\":" << (module.speakerRoomEqB ? "true" : "false")
	            << ",\"speakerRoomEqC\":" << (module.speakerRoomEqC ? "true" : "false")
		            << ",\"speakerSimulationWeightA\":" << finiteRange(module.speakerSimulationWeightA, 0.0f, -0.5f, 1.0f)
		            << ",\"speakerSimulationWeightB\":" << finiteRange(module.speakerSimulationWeightB, 0.0f, -0.5f, 1.0f)
		            << ",\"speakerSimulationWeightC\":" << finiteRange(module.speakerSimulationWeightC, 0.0f, -0.5f, 1.0f)
	            << ",\"speakerInsertsA\":";
	        writeTrackInsertSlots(out, module.speakerInsertsA);
	        out << ",\"speakerInsertsB\":";
	        writeTrackInsertSlots(out, module.speakerInsertsB);
	        out << ",\"speakerInsertsC\":";
	        writeTrackInsertSlots(out, module.speakerInsertsC);
	        out << "}";
        out << (i + 1 == project.monitorModules.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

std::string serializeProjectForPath(const ProjectDocument& project, const std::filesystem::path& projectPath) {
    ProjectDocument portable = project;
    for (auto& clip : portable.clips) {
        clip.sourcePath = makePathRelativeToProject(clip.sourcePath, projectPath);
    }
    for (auto& source : portable.mediaSources) {
        source.path = makePathRelativeToProject(source.path, projectPath);
    }
    for (auto& source : portable.videoSources) {
        source.path = makePathRelativeToProject(source.path, projectPath);
    }
    for (auto& insert : portable.masterInserts) {
        if (pluginPathShouldResolveWithProject(insert.pluginFormat)) {
            insert.pluginPath = makePathRelativeToProject(insert.pluginPath, projectPath);
        }
    }
    for (auto& track : portable.tracks) {
        makeTrackPluginPathsRelativeToProject(track, projectPath);
    }
    return serializeProject(portable);
}

bool deserializeProject(const std::string& text, ProjectDocument& project, std::string& error) {
    if (text.find("\"format\": \"neuracoust-daw-project-v1\"") == std::string::npos &&
        text.find("\"format\":\"neuracoust-daw-project-v1\"") == std::string::npos) {
        error = "Unsupported Neuracoust DAW project format.";
        return false;
    }

    ProjectDocument parsed = defaultProject();
    parsed.name = stringAfterKey(text, "name");
    if (parsed.name.empty()) {
        parsed.name = "Untitled";
    }
    parsed.sampleRate = finiteRange(numberAfterKey(text, "sampleRate", 48000.0), 48000.0, 8000.0, 384000.0);
    parsed.defaultBufferSize = finiteIntRange(numberAfterKey(text, "defaultBufferSize", 256.0), 256, 16, 8192);
    parsed.bitDepth = finiteIntRange(numberAfterKey(text, "bitDepth", 24.0), 24, 16, 64);
    if (parsed.bitDepth != 16 && parsed.bitDepth != 24 && parsed.bitDepth != 32 && parsed.bitDepth != 64) {
        parsed.bitDepth = 24;
    }
    parsed.tempoBpm = finiteIntRange(numberAfterKey(text, "tempoBpm", 120.0), 120, 20, 400);
    parsed.timeSignatureNumerator = finiteIntRange(numberAfterKey(text, "timeSignatureNumerator", 4.0), 4, 1, 16);
    parsed.timeSignatureDenominator = finiteIntRange(numberAfterKey(text, "timeSignatureDenominator", 4.0), 4, 1, 32);
    if (parsed.timeSignatureDenominator != 2 && parsed.timeSignatureDenominator != 4 &&
        parsed.timeSignatureDenominator != 8 && parsed.timeSignatureDenominator != 16 &&
        parsed.timeSignatureDenominator != 32) {
        parsed.timeSignatureDenominator = 4;
    }
    parsed.grooveFeel = trim(stringAfterKey(text, "grooveFeel"));
    if (parsed.grooveFeel != "straight" && parsed.grooveFeel != "shuffle" && parsed.grooveFeel != "triplet") {
        parsed.grooveFeel = "straight";
    }
    parsed.panLaw = trim(stringAfterKey(text, "panLaw"));
    if (parsed.panLaw != "-3dB" && parsed.panLaw != "-4.5dB" &&
        parsed.panLaw != "-6dB" && parsed.panLaw != "legacy") {
        parsed.panLaw = "legacy";   // absent/unknown → keep old projects' linear balance
    }
    parsed.grooveSwingAmount = finiteRange(numberAfterKey(text, "grooveSwingAmount", 0.0), 0.0, 0.0, 1.0);
    parsed.metronomeSubdivision = trim(stringAfterKey(text, "metronomeSubdivision"));
    if (parsed.metronomeSubdivision != "quarter" &&
        parsed.metronomeSubdivision != "eighth" &&
        parsed.metronomeSubdivision != "sixteenth") {
        parsed.metronomeSubdivision = "auto";
    }
    parsed.metronomeGain = finiteRange(numberAfterKey(text, "metronomeGain", 1.0), 1.0, 0.0, 2.0);
    parsed.metronomeSound = trim(stringAfterKey(text, "metronomeSound"));
    if (parsed.metronomeSound != "wood" && parsed.metronomeSound != "rim" && parsed.metronomeSound != "cowbell") {
        parsed.metronomeSound = "beep";
    }
    parsed.metronomeAccentFirst = boolAfterKey(text, "metronomeAccentFirst", true);
    parsed.metronomeGenre = trim(stringAfterKey(text, "metronomeGenre"));
    if (parsed.metronomeGenre.empty()) {
        parsed.metronomeGenre = "straight";
    }
    parsed.metronomeAccentPattern.clear();
    {
        const std::string patternBody = arrayBodyAfterKey(text, "metronomeAccentPattern");
        size_t pos = 0;
        while (pos < patternBody.size()) {
            size_t comma = patternBody.find(',', pos);
            const std::string token = trim(patternBody.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
            if (!token.empty()) {
                try {
                    parsed.metronomeAccentPattern.push_back(
                        static_cast<float>(std::max(0.0, std::min(4.0, std::stod(token)))));
                } catch (const std::exception&) {
                    // skip a malformed entry
                }
            }
            if (comma == std::string::npos) { break; }
            pos = comma + 1;
        }
    }
    // Absent key = a project saved before the filter existed. Those takes were recorded
    // with no controllers at all, but the useful default for them going forward is the
    // same as a new project's, so fall back to it rather than to "record nothing".
    if (text.find("\"midiRecordControllers\"") != std::string::npos) {
        parsed.midiRecordControllers.clear();
        const std::string controllerBody = arrayBodyAfterKey(text, "midiRecordControllers");
        size_t pos = 0;
        while (pos < controllerBody.size()) {
            const size_t comma = controllerBody.find(',', pos);
            const std::string token = trim(controllerBody.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
            if (!token.empty()) {
                try {
                    const int controller = std::stoi(token);
                    if (controller >= 0 && controller <= 127 &&
                        std::find(parsed.midiRecordControllers.begin(), parsed.midiRecordControllers.end(),
                                  controller) == parsed.midiRecordControllers.end()) {
                        parsed.midiRecordControllers.push_back(controller);
                    }
                } catch (const std::exception&) {
                    // skip a malformed entry
                }
            }
            if (comma == std::string::npos) { break; }
            pos = comma + 1;
        }
    }
    parsed.midiRecordPitchBend = boolAfterKey(text, "midiRecordPitchBend", true);
    parsed.detectedKey = trim(stringAfterKey(text, "detectedKey"));
    if (parsed.detectedKey.empty()) {
        parsed.detectedKey = "C";
    }
    parsed.detectedKeyMode = trim(stringAfterKey(text, "detectedKeyMode"));
    if (parsed.detectedKeyMode != "major" &&
        parsed.detectedKeyMode != "minor" &&
        parsed.detectedKeyMode != "unknown") {
        parsed.detectedKeyMode = "major";
    }
    parsed.chordKeyModePreference = trim(stringAfterKey(text, "chordKeyModePreference"));
    if (parsed.chordKeyModePreference != "major" &&
        parsed.chordKeyModePreference != "minor" &&
        parsed.chordKeyModePreference != "auto") {
        parsed.chordKeyModePreference = "auto";
    }
    parsed.tempoMasterTrackName = trim(stringAfterKey(text, "tempoMasterTrackName"));
    parsed.audioImportTempoPolicy = trim(stringAfterKey(text, "audioImportTempoPolicy"));
    if (parsed.audioImportTempoPolicy != "ask" &&
        parsed.audioImportTempoPolicy != "apply-to-project" &&
        parsed.audioImportTempoPolicy != "preserve-project" &&
        parsed.audioImportTempoPolicy != "stretch-to-project") {
        parsed.audioImportTempoPolicy = "preserve-project";
    }
    parsed.timecodeStartSeconds = finiteRange(numberAfterKey(text, "timecodeStartSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
    parsed.videoFrameRate = finiteRange(numberAfterKey(text, "videoFrameRate", 30.0), 30.0, 1.0, 240.0);
    parsed.timecodeDropFrame = boolAfterKey(text, "timecodeDropFrame", false);
    parsed.tempoMap.clear();
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "tempoMap"))) {
        TempoMarkerState marker;
        marker.timeSeconds = finiteRange(numberAfterKey(body, "timeSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        marker.bpm = finiteRange(numberAfterKey(body, "bpm", static_cast<double>(parsed.tempoBpm)), static_cast<double>(parsed.tempoBpm), 20.0, 400.0);
        parsed.tempoMap.push_back(marker);
    }
    if (parsed.tempoMap.empty()) {
        parsed.tempoMap.push_back({0.0, static_cast<double>(parsed.tempoBpm)});
    }
    std::sort(parsed.tempoMap.begin(), parsed.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    parsed.timeSignatureMap.clear();
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "timeSignatureMap"))) {
        TimeSignatureMarkerState marker;
        marker.timeSeconds = finiteRange(numberAfterKey(body, "timeSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        marker.numerator = finiteIntRange(numberAfterKey(body, "numerator", static_cast<double>(parsed.timeSignatureNumerator)),
                                          parsed.timeSignatureNumerator,
                                          1,
                                          16);
        marker.denominator = finiteIntRange(numberAfterKey(body, "denominator", static_cast<double>(parsed.timeSignatureDenominator)),
                                            parsed.timeSignatureDenominator,
                                            1,
                                            32);
        if (marker.denominator != 2 && marker.denominator != 4 &&
            marker.denominator != 8 && marker.denominator != 16 &&
            marker.denominator != 32) {
            marker.denominator = parsed.timeSignatureDenominator;
        }
        parsed.timeSignatureMap.push_back(marker);
    }
    if (parsed.timeSignatureMap.empty()) {
        parsed.timeSignatureMap.push_back({0.0, parsed.timeSignatureNumerator, parsed.timeSignatureDenominator});
    }
    std::sort(parsed.timeSignatureMap.begin(), parsed.timeSignatureMap.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    parsed.beatSnapEnabled = boolAfterKey(text, "beatSnapEnabled", false);
    parsed.editMode = normalizedEditMode(stringAfterKey(text, "editMode"));
    parsed.gridUnit = normalizedGridUnit(stringAfterKey(text, "gridUnit"));
    parsed.playbackStartMode = normalizedPlaybackStartMode(stringAfterKey(text, "playbackStartMode"));
    parsed.delayCompensationEnabled = boolAfterKey(text, "delayCompensationEnabled", true);
    parsed.directMonitoringEnabled = boolAfterKey(text, "directMonitoringEnabled", true);
    parsed.timelineZoomFactor = finiteRange(numberAfterKey(text, "timelineZoomFactor", 1.0), 1.0, 0.02, 16.0);
    parsed.timelineFollowMode = finiteIntRange(numberAfterKey(text, "timelineFollowMode", 1.0), 1, 0, 2);
    parsed.trackHeightScale = finiteRange(numberAfterKey(text, "trackHeightScale", 1.0), 1.0, 0.125, 4.0);
    parsed.tempoLaneHeightScale = finiteRange(numberAfterKey(text, "tempoLaneHeightScale", 0.50), 0.50, 0.50, 4.0);
    parsed.waveformGainScale = finiteRange(numberAfterKey(text, "waveformGainScale", 1.0), 1.0, 0.25, 6.0);
    parsed.loopEnabled = boolAfterKey(text, "loopEnabled", false);
    parsed.loopStartSeconds = finiteRange(numberAfterKey(text, "loopStartSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
    parsed.loopEndSeconds = finiteRange(numberAfterKey(text, "loopEndSeconds", 4.0), 4.0, 0.0, 24.0 * 60.0 * 60.0);
    parsed.preRollSeconds = finiteRange(numberAfterKey(text, "preRollSeconds", 0.0), 0.0, 0.0, 3600.0);
    parsed.postRollSeconds = finiteRange(numberAfterKey(text, "postRollSeconds", 0.0), 0.0, 0.0, 3600.0);
    parsed.editSelectionEnabled = boolAfterKey(text, "editSelectionEnabled", false);
    parsed.editSelectionStartSeconds = finiteRange(numberAfterKey(text, "editSelectionStartSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
    parsed.editSelectionEndSeconds = finiteRange(numberAfterKey(text, "editSelectionEndSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
    parsed.appleSiliconCoreIsolationEnabled = boolAfterKey(text, "appleSiliconCoreIsolationEnabled", true);
    parsed.requestedDspCoreCount = finiteIntRange(numberAfterKey(text, "requestedDspCoreCount", 4.0), 4, 1, 16);
    parsed.externalDspCoreCount = finiteIntRange(numberAfterKey(text, "externalDspCoreCount", 4.0), 4, 1, 16);
    parsed.externalDspEnabled = boolAfterKey(text, "externalDspEnabled", true);
    {
        const std::string host = stringAfterKey(text, "remoteDspHost");
        parsed.remoteDspHost = host.empty() ? std::string("studio.local") : host;
    }
    {
        const std::string host = stringAfterKey(text, "ndsHost");
        parsed.ndsHost = host.empty() ? std::string("192.168.0.198") : host;
    }
    parsed.ndsEnabled = boolAfterKey(text, "ndsEnabled", false);
    // A role only accepts the three machines it can name; anything else loads as local, so a
    // project written by a newer build cannot silently route audio somewhere this one cannot reach.
    const auto role = [&](const char* key) {
        const std::string value = stringAfterKey(text, key);
        return (value == "nds" || value == "external") ? value : std::string("internal");
    };
    parsed.dspRoleMonitor = role("dspRoleMonitor");
    parsed.dspRoleChannelStrip = role("dspRoleChannelStrip");
    parsed.dspRoleMaster = role("dspRoleMaster");
    parsed.dspRoleInserts = role("dspRoleInserts");
    parsed.dspAutoOverflow = boolAfterKey(text, "dspAutoOverflow", false);
    parsed.physicalSpeakerModel = stringAfterKey(text, "physicalSpeakerModel");
    parsed.measurementMicModel = stringAfterKey(text, "measurementMicModel");
    parsed.physicalPowerAmpModel = stringAfterKey(text, "physicalPowerAmpModel");
    parsed.physicalAudioInterfaceModel = stringAfterKey(text, "physicalAudioInterfaceModel");
    parsed.physicalAudioInterfaceTargetModel = stringAfterKey(text, "physicalAudioInterfaceTargetModel");
    parsed.monitorInterfaceModelingEnabled = boolAfterKey(text, "monitorInterfaceModelingEnabled", false);
    parsed.physicalSpeakerCableModel = stringAfterKey(text, "physicalSpeakerCableModel");
    parsed.physicalPowerCableModel = stringAfterKey(text, "physicalPowerCableModel");
    parsed.physicalConnectorModel = stringAfterKey(text, "physicalConnectorModel");
    parsed.monitorEqBands.clear();
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "monitorEqBands"))) {
        if (parsed.monitorEqBands.size() >= 64) break;
        MonitorEqBandState band;
        band.enabled = boolAfterKey(body, "enabled", true);
        band.type = stringAfterKey(body, "type");
        if (band.type.empty()) band.type = "peaking";
        band.frequencyHz = finiteRange(numberAfterKey(body, "frequencyHz", 1000.0), 1000.0, 10.0, 40000.0);
        band.gainDb = finiteRange(numberAfterKey(body, "gainDb", 0.0), 0.0, -30.0, 30.0);
        band.q = finiteRange(numberAfterKey(body, "q", 1.0), 1.0, 0.05, 40.0);
        parsed.monitorEqBands.push_back(band);
    }
    parsed.physicalHeadphoneModel = stringAfterKey(text, "physicalHeadphoneModel");
    parsed.monitorSpeakerHeadphoneExclusive = boolAfterKey(text, "monitorSpeakerHeadphoneExclusive", true);
    parsed.autoFadeOutSeconds = finiteRange(numberAfterKey(text, "autoFadeOutSeconds", 0.0), 0.0, 0.0, 600.0);
    {
        const std::string curve = stringAfterKey(text, "autoFadeOutCurve");
        parsed.autoFadeOutCurve = curve.empty() ? std::string("equal_power") : curve;
    }
    parsed.windowsProcessorAffinityEnabled = boolAfterKey(text, "windowsProcessorAffinityEnabled", false);
    parsed.windowsProcessorAffinityMode = stringAfterKey(text, "windowsProcessorAffinityMode");
    if (!isValidWindowsProcessorAffinityMode(parsed.windowsProcessorAffinityMode)) {
        parsed.windowsProcessorAffinityMode = "p_core_preferred";
    }
    parsed.monitorStationMono = boolAfterKey(text, "monitorStationMono", false);
    parsed.monitorStationListenMode = stringAfterKey(text, "monitorStationListenMode");
    if (parsed.monitorStationListenMode != "L" &&
        parsed.monitorStationListenMode != "R" &&
        parsed.monitorStationListenMode != "M" &&
        parsed.monitorStationListenMode != "S") {
        parsed.monitorStationListenMode = "LR";
    }
    parsed.monitorStationSwapLeftRight = boolAfterKey(text, "monitorStationSwapLeftRight", false);
    parsed.monitorStationInvertLeft = boolAfterKey(text, "monitorStationInvertLeft", false);
    parsed.monitorStationInvertRight = boolAfterKey(text, "monitorStationInvertRight", false);
    parsed.monitorStationMute = boolAfterKey(text, "monitorStationMute", false);
    parsed.monitorStationDim = boolAfterKey(text, "monitorStationDim", false);
    parsed.monitorStationTalkback = boolAfterKey(text, "monitorStationTalkback", false);
    parsed.monitorStationDimDb = finiteRange(static_cast<float>(numberAfterKey(text, "monitorStationDimDb", -20.0)), -20.0f, -60.0f, 0.0f);
    parsed.monitorStationTalkbackRoute = trim(stringAfterKey(text, "monitorStationTalkbackRoute"));
    if (parsed.monitorStationTalkbackRoute.empty()) {
        parsed.monitorStationTalkbackRoute = "listen_room";
    }
    parsed.monitorStationTalkbackChannel = std::max(1, static_cast<int>(numberAfterKey(text, "monitorStationTalkbackChannel", 1.0)));
    parsed.monitorInputTrimDb = finiteRange(static_cast<float>(numberAfterKey(text, "monitorInputTrimDb", -9.0)), -9.0f, -12.0f, 0.0f);
    parsed.monitorVolumeDb = finiteRange(static_cast<float>(numberAfterKey(text, "monitorVolumeDb", -6.0)), -6.0f, -120.0f, 12.0f);
    parsed.listenRoomEnabled = boolAfterKey(text, "listenRoomEnabled", false);
    parsed.listenRoomSessionName = trim(stringAfterKey(text, "listenRoomSessionName"));
    if (parsed.listenRoomSessionName.empty()) {
        parsed.listenRoomSessionName = "mix";
    }
    parsed.listenRoomSource = trim(stringAfterKey(text, "listenRoomSource"));
    if (parsed.listenRoomSource != "mix" && parsed.listenRoomSource != "monitor") {
        parsed.listenRoomSource = "monitor";
    }
    parsed.listenRoomQuality = trim(stringAfterKey(text, "listenRoomQuality"));
    if (parsed.listenRoomQuality != "pcm_lossless" &&
        parsed.listenRoomQuality != "opus_balanced" &&
        parsed.listenRoomQuality != "opus_high" &&
        parsed.listenRoomQuality != "opus_max") {
        parsed.listenRoomQuality = "opus_high";
    }
    parsed.listenRoomLatencyMode = trim(stringAfterKey(text, "listenRoomLatencyMode"));
    if (parsed.listenRoomLatencyMode != "low" &&
        parsed.listenRoomLatencyMode != "stable" &&
        parsed.listenRoomLatencyMode != "video_sync") {
        parsed.listenRoomLatencyMode = "stable";
    }
    parsed.listenRoomTransportMode = trim(stringAfterKey(text, "listenRoomTransportMode"));
    if (parsed.listenRoomTransportMode != "direct" &&
        parsed.listenRoomTransportMode != "relay" &&
        parsed.listenRoomTransportMode != "native_webrtc" &&
        parsed.listenRoomTransportMode != "direct_fallback") {
        parsed.listenRoomTransportMode = "direct_fallback";
    }
    parsed.listenRoomRelayHost = trim(stringAfterKey(text, "listenRoomRelayHost"));
    if (parsed.listenRoomRelayHost.empty()) {
        parsed.listenRoomRelayHost = "127.0.0.1";
    }
    parsed.listenRoomAccessToken = trim(stringAfterKey(text, "listenRoomAccessToken"));
    for (auto& ch : parsed.listenRoomAccessToken) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_';
        if (!ok) {
            ch = '-';
        }
    }
    parsed.listenRoomRelayHttpPort = finiteIntRange(numberAfterKey(text, "listenRoomRelayHttpPort", 8787.0), 8787, 1, 65535);
    parsed.listenRoomRelayTcpIngestPort = finiteIntRange(numberAfterKey(text, "listenRoomRelayTcpIngestPort", 8791.0), 8791, 1, 65535);
    if (parsed.monitorStationListenMode == "M" || parsed.monitorStationListenMode == "S") {
        parsed.monitorStationMono = false;
        parsed.monitorStationSwapLeftRight = false;
    }
    if (parsed.loopEndSeconds <= parsed.loopStartSeconds) {
        parsed.loopEnabled = false;
        parsed.loopStartSeconds = 0.0;
        parsed.loopEndSeconds = 4.0;
    }
    if (parsed.editSelectionEndSeconds <= parsed.editSelectionStartSeconds) {
        parsed.editSelectionEnabled = false;
        parsed.editSelectionStartSeconds = 0.0;
        parsed.editSelectionEndSeconds = 0.0;
    }

    parsed.tracks.clear();
    std::set<std::string> usedTrackNames;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "tracks"))) {
        TrackState track;
        track.name = uniqueTrackNameForImport(stringAfterKey(body, "name"), usedTrackNames);
        track.trackType = trim(stringAfterKey(body, "trackType"));
        track.colorHex = trim(stringAfterKey(body, "colorHex"));
        track.displayHeightScale = finiteRange(numberAfterKey(body, "displayHeightScale", 1.0), 1.0, 0.125, 4.0);
        track.folderName = trim(stringAfterKey(body, "folderName"));
        track.folderCollapsed = boolAfterKey(body, "folderCollapsed", false);
        track.mixerHidden = boolAfterKey(body, "mixerHidden", false);
        track.mixerOrder = static_cast<int>(finiteRange(numberAfterKey(body, "mixerOrder", 0.0), 0.0, -100000.0, 100000.0));
        track.automationMode = trim(stringAfterKey(body, "automationMode"));
        track.trackViewMode = normalizedTrackViewMode(stringAfterKey(body, "trackViewMode"));
        track.timebaseMode = normalizedTrackTimebaseMode(stringAfterKey(body, "timebaseMode"));
        track.elasticAudioMode = normalizedElasticAudioMode(stringAfterKey(body, "elasticAudioMode"));
        track.mixGroupName = trim(stringAfterKey(body, "mixGroupName"));
        track.controlMasterTrackName = trim(stringAfterKey(body, "controlMasterTrackName"));
        track.notes = stringAfterKey(body, "notes");
        track.consoleDspMachine = trim(stringAfterKey(body, "consoleDspMachine"));
        if (track.consoleDspMachine != "internal" && track.consoleDspMachine != "nds" &&
            track.consoleDspMachine != "external") {
            track.consoleDspMachine.clear();   // anything unrecognised follows the project role
        }
        track.consoleChannel.model = trim(stringAfterKey(body, "consoleModel"));
        if (track.consoleChannel.model.empty() || track.consoleChannel.model == "4001e")
            track.consoleChannel.model = "4000e";
        track.consoleChannel.moduleOrder = trim(stringAfterKey(body, "consoleModuleOrder"));
        if (track.consoleChannel.moduleOrder.empty()) track.consoleChannel.moduleOrder = "filter,eq,gate,comp,saturator";
        else if (track.consoleChannel.moduleOrder == "filter,eq,comp,gate,saturator")
            track.consoleChannel.moduleOrder = "filter,eq,gate,comp,saturator";
        else if (track.consoleChannel.moduleOrder.find("saturator") == std::string::npos)
            track.consoleChannel.moduleOrder += ",saturator";
        track.consoleChannel.filterEnabled = boolAfterKey(body, "consoleFilterEnabled", false);
        track.consoleChannel.filterCircuitMode = boolAfterKey(body, "consoleFilterCircuitMode", false);
        track.consoleChannel.highPassEnabled = boolAfterKey(body, "consoleHighPassEnabled", track.consoleChannel.filterEnabled);
        track.consoleChannel.lowPassEnabled = boolAfterKey(body, "consoleLowPassEnabled", track.consoleChannel.filterEnabled);
        track.consoleChannel.highPassHz = finiteRange((float)numberAfterKey(body, "consoleHighPassHz", 20), 20.0f, 20.0f, 350.0f);
        track.consoleChannel.lowPassHz = finiteRange((float)numberAfterKey(body, "consoleLowPassHz", 12000), 12000.0f, 3000.0f, 20000.0f);
        track.consoleChannel.eqEnabled = boolAfterKey(body, "consoleEqEnabled", false);
        track.consoleChannel.eqCircuitMode = boolAfterKey(body, "consoleEqCircuitMode", false);
        track.consoleChannel.compEnabled = boolAfterKey(body, "consoleCompEnabled", false);
        track.consoleChannel.compCircuitMode = boolAfterKey(body, "consoleCompCircuitMode", false);
        track.consoleChannel.gateEnabled = boolAfterKey(body, "consoleGateEnabled", false);
        track.consoleChannel.gateCircuitMode = boolAfterKey(body, "consoleGateCircuitMode", false);
        track.consoleChannel.saturatorEnabled = boolAfterKey(body, "consoleSaturatorEnabled", false);
        track.consoleChannel.dualMono = boolAfterKey(body, "consoleDualMono", false);
        // Per-module dual/stereo — default from the legacy shared field so old projects keep their
        // comp/gate stereo-link state, then let a per-module key override if present.
        const bool legacyDual = track.consoleChannel.dualMono;
        track.consoleChannel.filterDualMono = boolAfterKey(body, "consoleFilterDualMono", legacyDual);
        track.consoleChannel.eqDualMono = boolAfterKey(body, "consoleEqDualMono", legacyDual);
        track.consoleChannel.compDualMono = boolAfterKey(body, "consoleCompDualMono", legacyDual);
        track.consoleChannel.gateDualMono = boolAfterKey(body, "consoleGateDualMono", legacyDual);
        track.consoleChannel.saturatorDualMono = boolAfterKey(body, "consoleSaturatorDualMono", legacyDual);
        track.consoleChannel.saturatorCircuitMode = boolAfterKey(body, "consoleSaturatorCircuitMode", false);
        track.consoleChannel.saturatorDriveDb = finiteRange((float)numberAfterKey(body, "consoleSaturatorDriveDb", 6), 6.0f, 0.0f, 24.0f);
        track.consoleChannel.saturatorMix = finiteRange((float)numberAfterKey(body, "consoleSaturatorMix", 1), 1.0f, 0.0f, 1.0f);
        track.consoleChannel.expanderMode = boolAfterKey(body, "consoleExpanderMode", true);
        track.consoleChannel.compThresholdDb = finiteRange((float)numberAfterKey(body, "consoleCompThresholdDb", -18), -18.0f, -40.0f, 0.0f);
        track.consoleChannel.compRatio = finiteRange((float)numberAfterKey(body, "consoleCompRatio", 4), 4.0f, 1.0f, 20.0f);
        track.consoleChannel.compAttackMs = finiteRange((float)numberAfterKey(body, "consoleCompAttackMs", 30), 30.0f, 0.1f, 100.0f);
        track.consoleChannel.compReleaseMs = finiteRange((float)numberAfterKey(body, "consoleCompReleaseMs", 360), 360.0f, 40.0f, 1500.0f);
        track.consoleChannel.compMix = finiteRange((float)numberAfterKey(body, "consoleCompMix", 1), 1.0f, 0.0f, 1.0f);
        track.consoleChannel.compFastAttack = boolAfterKey(body, "consoleCompFastAttack", false);
        track.consoleChannel.compPeakMode = boolAfterKey(body, "consoleCompPeakMode", false);
        track.consoleChannel.compType = trim(stringAfterKey(body, "consoleCompType"));
        if (track.consoleChannel.compType.empty()) track.consoleChannel.compType = "ssl";
        track.consoleChannel.gateThresholdDb = finiteRange((float)numberAfterKey(body, "consoleGateThresholdDb", -36), -36.0f, -60.0f, 0.0f);
        track.consoleChannel.gateRangeDb = finiteRange((float)numberAfterKey(body, "consoleGateRangeDb", 20), 20.0f, 0.0f, 40.0f);
        track.consoleChannel.gateAttackMs = finiteRange((float)numberAfterKey(body, "consoleGateAttackMs", 1), 1.0f, 0.05f, 20.0f);
        track.consoleChannel.gateHoldMs = finiteRange((float)numberAfterKey(body, "consoleGateHoldMs", 0), 0.0f, 0.0f, 800.0f);
        track.consoleChannel.gateReleaseMs = finiteRange((float)numberAfterKey(body, "consoleGateReleaseMs", 360), 360.0f, 40.0f, 1500.0f);
        track.consoleChannel.gateFastAttack = boolAfterKey(body, "consoleGateFastAttack", false);
        track.consoleChannel.gateType = trim(stringAfterKey(body, "consoleGateType"));
        if (track.consoleChannel.gateType.empty()) track.consoleChannel.gateType = "ssl";
        track.consoleChannel.eqHfGainDb = finiteRange((float)numberAfterKey(body, "consoleEqHfGainDb", 0), 0.0f, -18.0f, 18.0f);
        track.consoleChannel.eqHfHz = finiteRange((float)numberAfterKey(body, "consoleEqHfHz", 8000), 8000.0f, 4000.0f, 16000.0f);
        track.consoleChannel.eqHfBell = boolAfterKey(body, "consoleEqHfBell", false);
        track.consoleChannel.eqHmfGainDb = finiteRange((float)numberAfterKey(body, "consoleEqHmfGainDb", 0), 0.0f, -18.0f, 18.0f);
        track.consoleChannel.eqHmfHz = finiteRange((float)numberAfterKey(body, "consoleEqHmfHz", 3000), 3000.0f, 1200.0f, 7500.0f);
        track.consoleChannel.eqHmfQ = finiteRange((float)numberAfterKey(body, "consoleEqHmfQ", 1), 1.0f, 0.2f, 10.0f);
        track.consoleChannel.eqLmfGainDb = finiteRange((float)numberAfterKey(body, "consoleEqLmfGainDb", 0), 0.0f, -18.0f, 18.0f);
        track.consoleChannel.eqLmfHz = finiteRange((float)numberAfterKey(body, "consoleEqLmfHz", 1000), 1000.0f, 400.0f, 2500.0f);
        track.consoleChannel.eqLmfQ = finiteRange((float)numberAfterKey(body, "consoleEqLmfQ", 1), 1.0f, 0.2f, 10.0f);
        track.consoleChannel.eqLfGainDb = finiteRange((float)numberAfterKey(body, "consoleEqLfGainDb", 0), 0.0f, -18.0f, 18.0f);
        track.consoleChannel.eqLfHz = finiteRange((float)numberAfterKey(body, "consoleEqLfHz", 200), 200.0f, 90.0f, 450.0f);
        track.consoleChannel.eqLfBell = boolAfterKey(body, "consoleEqLfBell", false);
        track.consoleChannel.eqEMode = boolAfterKey(body, "consoleEqEPattern", true);
        track.consoleChannel.phaseInvert = boolAfterKey(body, "consolePhaseInvert", false);
        // Per-side polarity — default from the legacy both-channels field so old projects keep it.
        const bool legacyPhase = track.consoleChannel.phaseInvert;
        track.consoleChannel.phaseInvertL = boolAfterKey(body, "consolePhaseInvertL", legacyPhase);
        track.consoleChannel.phaseInvertR = boolAfterKey(body, "consolePhaseInvertR", legacyPhase);
        track.consoleChannel.channelBiasSeed = (int)numberAfterKey(body, "consoleChannelBiasSeed", 0);
        track.consoleChannel.channelBiasDepth = finiteRange((float)numberAfterKey(body, "consoleChannelBiasDepth", 0), 0.0f, 0.0f, 1.0f);
        track.consoleChannel.eqType = trim(stringAfterKey(body, "consoleEqType"));
        if (track.consoleChannel.eqType.empty() || track.consoleChannel.eqType == "ssl_4001e")
            track.consoleChannel.eqType = "ssl_4000e";
        track.channelFormat = trim(stringAfterKey(body, "channelFormat"));
        if (track.channelFormat != "mono" && track.channelFormat != "stereo") {
            track.channelFormat = "stereo";
        }
        track.volumeDb = finiteRange(static_cast<float>(numberAfterKey(body, "volumeDb", 0.0)), 0.0f, -120.0f, 12.0f);
        track.pan = finiteRange(static_cast<float>(numberAfterKey(body, "pan", 0.0)), 0.0f, -1.0f, 1.0f);
        track.muted = boolAfterKey(body, "muted", false);
        track.solo = boolAfterKey(body, "solo", false);
        track.recordArmed = boolAfterKey(body, "recordArmed", false);
        track.inputMonitoring = boolAfterKey(body, "inputMonitoring", false);
        track.inputBus = trim(stringAfterKey(body, "inputBus"));
        track.outputBus = trim(stringAfterKey(body, "outputBus"));
        track.inserts = trackInsertSlotsAfterKey(body, "inserts");
        track.instrument = instrumentSlotAfterKey(body, "instrument");
        track.instrumentRackMode = trim(stringAfterKey(body, "instrumentRackMode"));
        track.instrumentSlots = instrumentSlotsAfterKey(body, "instrumentSlots");
        track.sends = trackSendsAfterKey(body, "sends");
        track.volumeAutomation = automationPointsAfterKey(body, "volumeAutomation");
        track.automationLanes = automationLanesAfterKey(body, "automationLanes");
        if (!isValidTrackType(track.trackType)) {
            track.trackType = inferredTrackType(track);
        }
        if (track.trackType == "basic_folder") {
            track.trackType = "folder";
        }
        if (track.trackType == "routing_folder") {
            track.trackType = "bus_folder";
        }
        if (track.inputBus.empty() && body.find("\"inputBus\"") == std::string::npos) {
            track.inputBus = "Input 1";
        }
        if (track.outputBus.empty() && body.find("\"outputBus\"") == std::string::npos) {
            track.outputBus = (track.trackType == "monitor" || track.name == "Monitor") ? "Main 1-2" : "Master";
        }
        if ((track.trackType == "audio" || track.trackType == "aux" || track.trackType == "bus_folder") && isPhysicalOrMonitorOutputRoute(track.outputBus)) {
            track.outputBus = "Master";
        }
        if (track.trackType == "midi") {
            if (track.inputBus.empty() || track.inputBus == "Input 1") {
                track.inputBus = "MIDI Input";
            }
            if (track.outputBus.empty() || track.outputBus == "Master") {
                track.outputBus = "Instrument";
            }
            track.instrument = {};
        }
        if (track.trackType == "instrument") {
            if (track.inputBus.empty() || track.inputBus == "Input 1") {
                track.inputBus = "MIDI Input";
            }
            if (track.outputBus.empty() || track.outputBus == "Instrument") {
                track.outputBus = "Master";
            }
            if (track.instrument.midiInput.empty()) {
                track.instrument.midiInput = "MIDI Input";
            }
            track.instrument.midiChannel = std::max(0, std::min(16, track.instrument.midiChannel));
            normalizeInstrumentRack(track);
        } else {
            track.instrument = {};
            track.instrumentSlots.clear();
            track.instrumentRackMode = "parallel";
        }
        if (track.name == "Master" || track.trackType == "master") {
            track.inputBus.clear();
            track.outputBus = "Monitor";
            track.trackType = "master";
            track.folderName.clear();
        }
        if (track.name == "Monitor" || track.trackType == "monitor") {
            track.inputBus = "Monitor";
            track.outputBus = "Main 1-2";
            track.trackType = "monitor";
            track.folderName.clear();
        }
        if (track.trackType == "folder" || track.trackType == "bus_folder" || track.trackType == "vca") {
            track.folderName.clear();
            track.recordArmed = false;
            track.inputMonitoring = false;
        }
        if (track.trackType == "vca" ||
            track.trackType == "master" ||
            track.trackType == "monitor" ||
            track.name == "Master" ||
            track.name == "Monitor") {
            track.controlMasterTrackName.clear();
        }
        if (track.colorHex.empty()) {
            static const char* colors[] = {"#35BFA8", "#4B84E8", "#F0B84D", "#D86BA6", "#7CCB5E", "#A078E8", "#E26D5A", "#5BC0DE"};
            track.colorHex = colors[parsed.tracks.size() % (sizeof(colors) / sizeof(colors[0]))];
        }
        if (!track.name.empty()) {
            parsed.tracks.push_back(track);
        }
    }
    if (parsed.tracks.empty()) {
        parsed.tracks = defaultProject().tracks;
    }
    if (firstEditableTrackName(parsed.tracks).empty()) {
        parsed.tracks = defaultProject().tracks;
    }
    normalizeProjectRouting(parsed);
    std::set<std::string> validFolderNames;
    for (const auto& track : parsed.tracks) {
        if (track.trackType == "folder" || track.trackType == "bus_folder") {
            validFolderNames.insert(track.name);
        }
    }
    for (auto& track : parsed.tracks) {
        if (track.trackType == "folder" || track.trackType == "bus_folder" ||
            track.trackType == "master" || track.trackType == "monitor" ||
            track.name == "Master" || track.name == "Monitor" ||
            validFolderNames.find(track.folderName) == validFolderNames.end()) {
            track.folderName.clear();
        }
    }
    std::set<std::string> validControlMasterNames;
    for (const auto& track : parsed.tracks) {
        if (track.trackType == "vca") {
            validControlMasterNames.insert(track.name);
        }
    }
    for (auto& track : parsed.tracks) {
        if (track.controlMasterTrackName.empty()) {
            continue;
        }
        if (validControlMasterNames.find(track.controlMasterTrackName) == validControlMasterNames.end() ||
            track.controlMasterTrackName == track.name ||
            track.trackType == "vca" ||
            track.trackType == "master" ||
            track.trackType == "monitor" ||
            track.name == "Master" ||
            track.name == "Monitor") {
            track.controlMasterTrackName.clear();
        }
    }
    const auto fallbackClipTrackName = firstEditableTrackName(parsed.tracks);

    parsed.clips.clear();
    std::set<std::string> usedClipIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "clips"))) {
        ClipState clip;
        const auto requestedClipId = stringAfterKey(body, "id");
        clip.trackName = trim(stringAfterKey(body, "trackName"));
        clip.sourcePath = stringAfterKey(body, "sourcePath");
        clip.regionName = trim(stringAfterKey(body, "regionName"));
        clip.sourceFileUid = trim(stringAfterKey(body, "sourceFileUid"));
        clip.sourceChannels = finiteIntRange(numberAfterKey(body, "sourceChannels", 0.0), 0, 0, 256);
        clip.sourceSampleRate = finiteRange(numberAfterKey(body, "sourceSampleRate", 0.0), 0.0, 0.0, 384000.0);
        clip.sourceBitsPerSample = finiteIntRange(numberAfterKey(body, "sourceBitsPerSample", 0.0), 0, 0, 64);
        clip.sourceFloatingPoint = boolAfterKey(body, "sourceFloatingPoint", false);
        clip.sourceHasBroadcastTimeReference = boolAfterKey(body, "sourceHasBroadcastTimeReference", false);
        clip.sourceTimeReferenceSamples = static_cast<uint64_t>(finiteRange(numberAfterKey(body, "sourceTimeReferenceSamples", 0.0), 0.0, 0.0, 9.223372036854776e18));
        clip.sourceTimeReferenceSeconds = finiteRange(numberAfterKey(body, "sourceTimeReferenceSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        clip.sourceTempoBpm = finiteRange(numberAfterKey(body, "sourceTempoBpm", 0.0), 0.0, 0.0, 400.0);
        clip.sourceTimeSignatureNumerator = finiteIntRange(numberAfterKey(body, "sourceTimeSignatureNumerator", 0.0), 0, 0, 64);
        clip.sourceTimeSignatureDenominator = finiteIntRange(numberAfterKey(body, "sourceTimeSignatureDenominator", 0.0), 0, 0, 64);
        clip.sourceGrooveFeel = trim(stringAfterKey(body, "sourceGrooveFeel"));
        if (clip.sourceGrooveFeel != "straight" &&
            clip.sourceGrooveFeel != "shuffle" &&
            clip.sourceGrooveFeel != "triplet") {
            clip.sourceGrooveFeel.clear();
        }
        clip.sourceGrooveSwingAmount = finiteRange(numberAfterKey(body, "sourceGrooveSwingAmount", 0.0), 0.0, 0.0, 1.0);
        clip.araPluginName = trim(stringAfterKey(body, "araPluginName"));
        clip.araPluginPath = trim(stringAfterKey(body, "araPluginPath"));
        clip.araSourcePath = trim(stringAfterKey(body, "araSourcePath"));
        // Not trimmed: base64 has no leading/trailing whitespace to begin with, and the archive can
        // be tens of kilobytes — no reason to copy it twice.
        clip.araArchiveBase64 = stringAfterKey(body, "araArchiveBase64");
        clip.timeScale = finiteRange(numberAfterKey(body, "timeScale", 1.0), 1.0, 0.05, 20.0);
        clip.tempoSyncPolicy = trim(stringAfterKey(body, "tempoSyncPolicy"));
        if (clip.tempoSyncPolicy != "tempo-master" &&
            clip.tempoSyncPolicy != "project-tempo" &&
            clip.tempoSyncPolicy != "preserve-original" &&
            clip.tempoSyncPolicy != "stretch-to-project") {
            clip.tempoSyncPolicy = "project-tempo";
        }
        clip.pendingTimeStretchToProject = boolAfterKey(body, "pendingTimeStretchToProject", false);
        clip.colorHex = trim(stringAfterKey(body, "colorHex"));
        clip.startSeconds = finiteRange(numberAfterKey(body, "startSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        // -1 = original position unknown (projects saved before the field existed).
        clip.originalStartSeconds = finiteRange(numberAfterKey(body, "originalStartSeconds", -1.0), -1.0, -1.0, 24.0 * 60.0 * 60.0);
        clip.durationSeconds = finiteRange(numberAfterKey(body, "durationSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        clip.sourceOffsetSeconds = finiteRange(numberAfterKey(body, "sourceOffsetSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        clip.gainDb = finiteRange(static_cast<float>(numberAfterKey(body, "gainDb", 0.0)), 0.0f, -60.0f, 24.0f);
        const double maxFadeSeconds = std::max(0.0, clip.durationSeconds * 0.5);
        clip.fadeInSeconds = finiteRange(numberAfterKey(body, "fadeInSeconds", 0.0), 0.0, 0.0, maxFadeSeconds);
        clip.fadeOutSeconds = finiteRange(numberAfterKey(body, "fadeOutSeconds", 0.0), 0.0, 0.0, maxFadeSeconds);
        clip.fadeInCurve = normalizedFadeCurve(stringAfterKey(body, "fadeInCurve"));
        clip.fadeOutCurve = normalizedFadeCurve(stringAfterKey(body, "fadeOutCurve"));
        clip.fadeInCurvature = finiteRange(numberAfterKey(body, "fadeInCurvature", 0.0), 0.0, -1.0, 1.0);
        clip.fadeOutCurvature = finiteRange(numberAfterKey(body, "fadeOutCurvature", 0.0), 0.0, -1.0, 1.0);
        clip.muted = boolAfterKey(body, "muted", false);
        clip.polarityInverted = boolAfterKey(body, "polarityInverted", false);
        clip.reversed = boolAfterKey(body, "reversed", false);
        clip.locked = boolAfterKey(body, "locked", false);
        if (clip.trackName.empty() ||
            isProtectedTrackName(clip.trackName) ||
            !trackNameExists(parsed.tracks, clip.trackName)) {
            clip.trackName = fallbackClipTrackName;
        }
        if (!requestedClipId.empty() && clip.durationSeconds > 0.0) {
            clip.id = uniqueIdForImport(requestedClipId, usedClipIds);
        }
        if (!clip.id.empty()) {
            if (clip.regionName.empty()) {
                clip.regionName = defaultRegionNameForSource(clip.sourcePath);
            }
            if (clip.sourceFileUid.empty()) {
                clip.sourceFileUid = defaultSourceFileUid(clip.sourcePath);
            }
            if (clip.colorHex.empty()) {
                auto trackIt = std::find_if(parsed.tracks.begin(), parsed.tracks.end(), [&](const TrackState& track) {
                    return track.name == clip.trackName;
                });
                clip.colorHex = trackIt != parsed.tracks.end() ? trackIt->colorHex : "#35BFA8";
            }
            parsed.clips.push_back(clip);
        }
    }

    parsed.mediaSources.clear();
    std::set<std::string> usedMediaSourceIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "mediaSources"))) {
        MediaSourceState source;
        source.id = uniqueIdForImport(trim(stringAfterKey(body, "id")), usedMediaSourceIds);
        source.path = stringAfterKey(body, "path");
        source.displayName = trim(stringAfterKey(body, "displayName"));
        source.channels = finiteIntRange(numberAfterKey(body, "channels", 0.0), 0, 0, 256);
        source.sampleRate = finiteRange(numberAfterKey(body, "sampleRate", 0.0), 0.0, 0.0, 384000.0);
        source.bitsPerSample = finiteIntRange(numberAfterKey(body, "bitsPerSample", 0.0), 0, 0, 64);
        source.floatingPoint = boolAfterKey(body, "floatingPoint", false);
        source.hasBroadcastTimeReference = boolAfterKey(body, "hasBroadcastTimeReference", false);
        source.timeReferenceSamples = static_cast<uint64_t>(finiteRange(numberAfterKey(body, "timeReferenceSamples", 0.0), 0.0, 0.0, 9.223372036854776e18));
        source.timeReferenceSeconds = finiteRange(numberAfterKey(body, "timeReferenceSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        if (!source.id.empty()) {
            if (source.displayName.empty()) {
                source.displayName = defaultRegionNameForSource(source.path);
            }
            parsed.mediaSources.push_back(source);
        }
    }

    parsed.clipDefinitions.clear();
    std::set<std::string> usedClipDefinitionIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "clipDefinitions"))) {
        ClipDefinitionState definition;
        definition.id = uniqueIdForImport(trim(stringAfterKey(body, "id")), usedClipDefinitionIds);
        definition.sourceId = trim(stringAfterKey(body, "sourceId"));
        definition.name = trim(stringAfterKey(body, "name"));
        definition.sourceOffsetSeconds = finiteRange(numberAfterKey(body, "sourceOffsetSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        definition.durationSeconds = finiteRange(numberAfterKey(body, "durationSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        definition.sourceTempoBpm = finiteRange(numberAfterKey(body, "sourceTempoBpm", 0.0), 0.0, 0.0, 400.0);
        definition.sourceTimeSignatureNumerator = finiteIntRange(numberAfterKey(body, "sourceTimeSignatureNumerator", 0.0), 0, 0, 64);
        definition.sourceTimeSignatureDenominator = finiteIntRange(numberAfterKey(body, "sourceTimeSignatureDenominator", 0.0), 0, 0, 64);
        definition.sourceGrooveFeel = trim(stringAfterKey(body, "sourceGrooveFeel"));
        if (definition.sourceGrooveFeel != "straight" &&
            definition.sourceGrooveFeel != "shuffle" &&
            definition.sourceGrooveFeel != "triplet") {
            definition.sourceGrooveFeel.clear();
        }
        definition.sourceGrooveSwingAmount = finiteRange(numberAfterKey(body, "sourceGrooveSwingAmount", 0.0), 0.0, 0.0, 1.0);
        if (!definition.id.empty() && definition.durationSeconds > 0.0) {
            if (definition.name.empty()) {
                definition.name = definition.id;
            }
            parsed.clipDefinitions.push_back(definition);
        }
    }

    parsed.videoSources.clear();
    std::set<std::string> usedVideoSourceIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "videoSources"))) {
        VideoSourceState source;
        source.id = uniqueIdForImport(trim(stringAfterKey(body, "id")), usedVideoSourceIds);
        source.path = stringAfterKey(body, "path");
        source.displayName = trim(stringAfterKey(body, "displayName"));
        source.frameRate = finiteRange(numberAfterKey(body, "frameRate", parsed.videoFrameRate), parsed.videoFrameRate, 1.0, 240.0);
        source.durationSeconds = finiteRange(numberAfterKey(body, "durationSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        source.width = finiteIntRange(numberAfterKey(body, "width", 0.0), 0, 0, 65535);
        source.height = finiteIntRange(numberAfterKey(body, "height", 0.0), 0, 0, 65535);
        source.hasAudio = boolAfterKey(body, "hasAudio", false);
        if (!source.id.empty()) {
            if (source.displayName.empty()) {
                source.displayName = defaultRegionNameForSource(source.path);
            }
            parsed.videoSources.push_back(source);
        }
    }

    parsed.videoClips.clear();
    std::set<std::string> usedVideoClipIds;
    std::set<std::string> validVideoSourceIds;
    for (const auto& source : parsed.videoSources) {
        if (!source.id.empty()) {
            validVideoSourceIds.insert(source.id);
        }
    }
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "videoClips"))) {
        VideoClipState clip;
        clip.id = uniqueIdForImport(trim(stringAfterKey(body, "id")), usedVideoClipIds);
        clip.sourceId = trim(stringAfterKey(body, "sourceId"));
        clip.name = trim(stringAfterKey(body, "name"));
        clip.startSeconds = finiteRange(numberAfterKey(body, "startSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        clip.durationSeconds = finiteRange(numberAfterKey(body, "durationSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        clip.sourceOffsetSeconds = finiteRange(numberAfterKey(body, "sourceOffsetSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        clip.sourceTimecodeStartSeconds = finiteRange(numberAfterKey(body, "sourceTimecodeStartSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        clip.muted = boolAfterKey(body, "muted", false);
        clip.locked = boolAfterKey(body, "locked", false);
        if (!clip.id.empty() && clip.durationSeconds > 0.0 && validVideoSourceIds.find(clip.sourceId) != validVideoSourceIds.end()) {
            if (clip.name.empty()) {
                clip.name = clip.id;
            }
            parsed.videoClips.push_back(clip);
        }
    }

    parsed.trackPlaylists.clear();
    std::set<std::string> usedPlaylistIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "trackPlaylists"))) {
        TrackPlaylistState playlist;
        playlist.id = uniqueIdForImport(trim(stringAfterKey(body, "id")), usedPlaylistIds);
        playlist.trackName = trim(stringAfterKey(body, "trackName"));
        playlist.name = trim(stringAfterKey(body, "name"));
        playlist.active = boolAfterKey(body, "active", true);
        if (playlist.name.empty()) {
            playlist.name = "Playlist 1";
        }
        if (playlist.trackName.empty() || isProtectedTrackName(playlist.trackName) || !trackNameExists(parsed.tracks, playlist.trackName)) {
            continue;
        }
        std::set<std::string> usedPlacementIds;
        for (const auto& placementBody : objectBodies(arrayBodyAfterKey(body, "placements"))) {
            PlaylistClipPlacementState placement;
            placement.id = uniqueIdForImport(trim(stringAfterKey(placementBody, "id")), usedPlacementIds);
            placement.clipDefinitionId = trim(stringAfterKey(placementBody, "clipDefinitionId"));
            placement.startSeconds = finiteRange(numberAfterKey(placementBody, "startSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
            placement.originalStartSeconds = finiteRange(numberAfterKey(placementBody, "originalStartSeconds", -1.0), -1.0, -1.0, 24.0 * 60.0 * 60.0);
            placement.layer = finiteIntRange(numberAfterKey(placementBody, "layer", 0.0), 0, 0, 1024);
            placement.gainDb = finiteRange(static_cast<float>(numberAfterKey(placementBody, "gainDb", 0.0)), 0.0f, -60.0f, 24.0f);
            placement.fadeInSeconds = finiteRange(numberAfterKey(placementBody, "fadeInSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
            placement.fadeOutSeconds = finiteRange(numberAfterKey(placementBody, "fadeOutSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
            placement.fadeInCurve = normalizedFadeCurve(stringAfterKey(placementBody, "fadeInCurve"));
            placement.fadeOutCurve = normalizedFadeCurve(stringAfterKey(placementBody, "fadeOutCurve"));
            placement.fadeInCurvature = finiteRange(numberAfterKey(placementBody, "fadeInCurvature", 0.0), 0.0, -1.0, 1.0);
            placement.fadeOutCurvature = finiteRange(numberAfterKey(placementBody, "fadeOutCurvature", 0.0), 0.0, -1.0, 1.0);
            placement.muted = boolAfterKey(placementBody, "muted", false);
            placement.polarityInverted = boolAfterKey(placementBody, "polarityInverted", false);
            placement.reversed = boolAfterKey(placementBody, "reversed", false);
            placement.araPluginName = trim(stringAfterKey(placementBody, "araPluginName"));
            placement.araPluginPath = trim(stringAfterKey(placementBody, "araPluginPath"));
            placement.araSourcePath = trim(stringAfterKey(placementBody, "araSourcePath"));
            placement.araArchiveBase64 = stringAfterKey(placementBody, "araArchiveBase64");
            placement.locked = boolAfterKey(placementBody, "locked", false);
            placement.colorHex = trim(stringAfterKey(placementBody, "colorHex"));
            placement.timeScale = finiteRange(numberAfterKey(placementBody, "timeScale", 1.0), 1.0, 0.05, 20.0);
            placement.tempoSyncPolicy = trim(stringAfterKey(placementBody, "tempoSyncPolicy"));
            if (placement.tempoSyncPolicy != "tempo-master" &&
                placement.tempoSyncPolicy != "project-tempo" &&
                placement.tempoSyncPolicy != "preserve-original" &&
                placement.tempoSyncPolicy != "stretch-to-project") {
                placement.tempoSyncPolicy = "project-tempo";
            }
            placement.pendingTimeStretchToProject = boolAfterKey(placementBody, "pendingTimeStretchToProject", false);
            placement.legacyClipId = trim(stringAfterKey(placementBody, "legacyClipId"));
            if (!placement.id.empty() && !placement.clipDefinitionId.empty()) {
                playlist.placements.push_back(placement);
            }
        }
        if (!playlist.id.empty()) {
            parsed.trackPlaylists.push_back(playlist);
        }
    }
    normalizeProjectEditModel(parsed);

    parsed.markers.clear();
    std::set<std::string> usedMarkerIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "markers"))) {
        MarkerState marker;
        marker.id = uniqueIdForImport(stringAfterKey(body, "id"), usedMarkerIds);
        marker.name = stringAfterKey(body, "name");
        marker.timeSeconds = finiteRange(numberAfterKey(body, "timeSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        marker.memoryType = stringAfterKey(body, "memoryType");
        if (!(marker.memoryType == "selection" || marker.memoryType == "marker")) {
            marker.memoryType = "marker";
        }
        marker.selectionStartSeconds = finiteRange(numberAfterKey(body, "selectionStartSeconds", marker.timeSeconds), marker.timeSeconds, 0.0, 24.0 * 60.0 * 60.0);
        marker.selectionEndSeconds = finiteRange(numberAfterKey(body, "selectionEndSeconds", marker.selectionStartSeconds), marker.selectionStartSeconds, 0.0, 24.0 * 60.0 * 60.0);
        if (marker.memoryType == "selection" && marker.selectionEndSeconds <= marker.selectionStartSeconds) {
            marker.memoryType = "marker";
            marker.selectionStartSeconds = marker.timeSeconds;
            marker.selectionEndSeconds = marker.timeSeconds;
        }
        marker.referenceMode = stringAfterKey(body, "referenceMode");
        if (!(marker.referenceMode == "bars_beats" || marker.referenceMode == "timecode" || marker.referenceMode == "absolute")) {
            marker.referenceMode = "absolute";
        }
        marker.recallZoom = boolAfterKey(body, "recallZoom", false);
        marker.recallPrePostRoll = boolAfterKey(body, "recallPrePostRoll", false);
        marker.recallTrackVisibility = boolAfterKey(body, "recallTrackVisibility", false);
        marker.recallTrackHeights = boolAfterKey(body, "recallTrackHeights", false);
        marker.recallGroups = boolAfterKey(body, "recallGroups", false);
        marker.recallWindowConfiguration = boolAfterKey(body, "recallWindowConfiguration", false);
        marker.storedTimelineZoomFactor = finiteRange(numberAfterKey(body, "storedTimelineZoomFactor", 1.0), 1.0, 0.02, 16.0);
        marker.storedTrackHeightScale = finiteRange(numberAfterKey(body, "storedTrackHeightScale", 1.0), 1.0, 0.125, 4.0);
        marker.storedPreRollSeconds = finiteRange(numberAfterKey(body, "storedPreRollSeconds", 0.0), 0.0, 0.0, 3600.0);
        marker.storedPostRollSeconds = finiteRange(numberAfterKey(body, "storedPostRollSeconds", 0.0), 0.0, 0.0, 3600.0);
        marker.windowConfigurationName = stringAfterKey(body, "windowConfigurationName");
        marker.comment = stringAfterKey(body, "comment");
        if (!marker.id.empty()) {
            if (marker.name.empty()) {
                marker.name = marker.id;
            }
            parsed.markers.push_back(marker);
        }
    }
    std::sort(parsed.markers.begin(), parsed.markers.end(), [](const MarkerState& a, const MarkerState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });

    parsed.chordEvents.clear();
    std::set<std::string> usedChordEventIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "chordEvents"))) {
        ChordEventState chord;
        chord.id = uniqueIdForImport(stringAfterKey(body, "id"), usedChordEventIds);
        chord.name = stringAfterKey(body, "name");
        chord.timeSeconds = finiteRange(numberAfterKey(body, "timeSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        if (!chord.id.empty()) {
            if (chord.name.empty()) {
                chord.name = chord.id;
            }
            parsed.chordEvents.push_back(chord);
        }
    }
    std::sort(parsed.chordEvents.begin(), parsed.chordEvents.end(), [](const ChordEventState& a, const ChordEventState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });

    parsed.songSections.clear();
    std::set<std::string> usedSongSectionIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "songSections"))) {
        ChordEventState section;
        section.id = uniqueIdForImport(stringAfterKey(body, "id"), usedSongSectionIds);
        section.name = stringAfterKey(body, "name");
        section.timeSeconds = finiteRange(numberAfterKey(body, "timeSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        if (!section.id.empty()) {
            if (section.name.empty()) section.name = section.id;
            parsed.songSections.push_back(section);
        }
    }
    std::sort(parsed.songSections.begin(), parsed.songSections.end(), [](const ChordEventState& a, const ChordEventState& b) {
        if (a.timeSeconds == b.timeSeconds) return a.id < b.id;
        return a.timeSeconds < b.timeSeconds;
    });

    parsed.lyricEvents.clear();
    std::set<std::string> usedLyricEventIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "lyricEvents"))) {
        LyricEventState lyric;
        lyric.id = uniqueIdForImport(stringAfterKey(body, "id"), usedLyricEventIds);
        lyric.text = stringAfterKey(body, "text");
        lyric.timeSeconds = finiteRange(numberAfterKey(body, "timeSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        if (!lyric.id.empty()) {
            if (lyric.text.empty()) {
                lyric.text = lyric.id;
            }
            parsed.lyricEvents.push_back(lyric);
        }
    }
    std::sort(parsed.lyricEvents.begin(), parsed.lyricEvents.end(), [](const LyricEventState& a, const LyricEventState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });

    parsed.midiRegions.clear();
    std::set<std::string> usedMidiRegionIds;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "midiRegions"))) {
        MidiRegionState region;
        region.id = uniqueIdForImport(trim(stringAfterKey(body, "id")), usedMidiRegionIds);
        region.trackName = trim(stringAfterKey(body, "trackName"));
        region.name = trim(stringAfterKey(body, "name"));
        region.startSeconds = finiteRange(numberAfterKey(body, "startSeconds", 0.0), 0.0, 0.0, 24.0 * 60.0 * 60.0);
        region.durationSeconds = finiteRange(numberAfterKey(body, "durationSeconds", 4.0), 4.0, 0.01, 24.0 * 60.0 * 60.0);
        region.ticksPerQuarter = finiteIntRange(numberAfterKey(body, "ticksPerQuarter", 960.0), 960, 24, 9600);
        region.loopEnabled = boolAfterKey(body, "loopEnabled", false);
        region.muted = boolAfterKey(body, "muted", false);
        region.locked = boolAfterKey(body, "locked", false);
        region.colorHex = trim(stringAfterKey(body, "colorHex"));
        if (region.name.empty()) {
            region.name = "MIDI Region";
        }
        if (region.trackName.empty() ||
            isProtectedTrackName(region.trackName) ||
            !trackNameExists(parsed.tracks, region.trackName)) {
            continue;
        }
        auto trackIt = std::find_if(parsed.tracks.begin(), parsed.tracks.end(), [&](const TrackState& track) {
            return track.name == region.trackName;
        });
        if (trackIt == parsed.tracks.end() ||
            trackIt->trackType == "folder" ||
            trackIt->trackType == "bus_folder" ||
            trackIt->trackType == "master" ||
            trackIt->trackType == "monitor") {
            continue;
        }
        if (trackIt->trackType != "midi" && trackIt->trackType != "instrument") {
            trackIt->trackType = "midi";
            if (trackIt->inputBus.empty() || trackIt->inputBus == "Input 1") {
                trackIt->inputBus = "MIDI Input";
            }
            if (trackIt->outputBus.empty() || trackIt->outputBus == "Master") {
                trackIt->outputBus = "Instrument";
            }
        }
        if (region.colorHex.empty()) {
            region.colorHex = trackIt->colorHex.empty() ? "#4B84E8" : trackIt->colorHex;
        }
        std::set<std::string> usedMidiNoteIds;
        for (const auto& noteBody : objectBodies(arrayBodyAfterKey(body, "notes"))) {
            MidiNoteState note;
            note.id = uniqueIdForImport(trim(stringAfterKey(noteBody, "id")), usedMidiNoteIds);
            note.pitch = finiteIntRange(numberAfterKey(noteBody, "pitch", 60.0), 60, 0, 127);
            note.startBeats = finiteRange(numberAfterKey(noteBody, "startBeats", 0.0), 0.0, 0.0, 1000000.0);
            note.durationBeats = finiteRange(numberAfterKey(noteBody, "durationBeats", 1.0), 1.0, 1.0 / 960.0, 1000000.0);
            note.velocity = finiteIntRange(numberAfterKey(noteBody, "velocity", 96.0), 96, 1, 127);
            note.channel = finiteIntRange(numberAfterKey(noteBody, "channel", 1.0), 1, 1, 16);
            note.muted = boolAfterKey(noteBody, "muted", false);
            note.colorHex = trim(stringAfterKey(noteBody, "colorHex"));
            if (!note.id.empty()) {
                region.notes.push_back(note);
            }
        }
        std::sort(region.notes.begin(), region.notes.end(), [](const MidiNoteState& left, const MidiNoteState& right) {
            if (left.startBeats == right.startBeats) {
                if (left.pitch == right.pitch) {
                    return left.id < right.id;
                }
                return left.pitch < right.pitch;
            }
            return left.startBeats < right.startBeats;
        });
        std::set<std::string> usedControllerEventIds;
        for (const auto& eventBody : objectBodies(arrayBodyAfterKey(body, "controllerEvents"))) {
            MidiControllerEventState event;
            event.id = uniqueIdForImport(trim(stringAfterKey(eventBody, "id")), usedControllerEventIds);
            event.beat = finiteRange(numberAfterKey(eventBody, "beat", 0.0), 0.0, 0.0, 1000000.0);
            event.controller = finiteIntRange(numberAfterKey(eventBody, "controller", 0.0), 0, 0, 127);
            event.value = finiteIntRange(numberAfterKey(eventBody, "value", 0.0), 0, 0, 127);
            event.channel = finiteIntRange(numberAfterKey(eventBody, "channel", 1.0), 1, 1, 16);
            if (!event.id.empty()) {
                region.controllerEvents.push_back(event);
            }
        }
        std::sort(region.controllerEvents.begin(), region.controllerEvents.end(), [](const MidiControllerEventState& left, const MidiControllerEventState& right) {
            if (left.beat == right.beat) {
                if (left.controller == right.controller) {
                    return left.id < right.id;
                }
                return left.controller < right.controller;
            }
            return left.beat < right.beat;
        });
        std::set<std::string> usedPitchBendEventIds;
        for (const auto& eventBody : objectBodies(arrayBodyAfterKey(body, "pitchBendEvents"))) {
            MidiPitchBendEventState event;
            event.id = uniqueIdForImport(trim(stringAfterKey(eventBody, "id")), usedPitchBendEventIds);
            event.beat = finiteRange(numberAfterKey(eventBody, "beat", 0.0), 0.0, 0.0, 1000000.0);
            event.value = finiteIntRange(numberAfterKey(eventBody, "value", 8192.0), 8192, 0, 16383);
            event.channel = finiteIntRange(numberAfterKey(eventBody, "channel", 1.0), 1, 1, 16);
            if (!event.id.empty()) {
                region.pitchBendEvents.push_back(event);
            }
        }
        std::sort(region.pitchBendEvents.begin(), region.pitchBendEvents.end(), [](const MidiPitchBendEventState& left, const MidiPitchBendEventState& right) {
            if (left.beat == right.beat) {
                return left.id < right.id;
            }
            return left.beat < right.beat;
        });
        std::set<std::string> usedProgramChangeEventIds;
        for (const auto& eventBody : objectBodies(arrayBodyAfterKey(body, "programChangeEvents"))) {
            MidiProgramChangeEventState event;
            event.id = uniqueIdForImport(trim(stringAfterKey(eventBody, "id")), usedProgramChangeEventIds);
            event.beat = finiteRange(numberAfterKey(eventBody, "beat", 0.0), 0.0, 0.0, 1000000.0);
            event.program = finiteIntRange(numberAfterKey(eventBody, "program", 0.0), 0, 0, 127);
            event.channel = finiteIntRange(numberAfterKey(eventBody, "channel", 1.0), 1, 1, 16);
            if (!event.id.empty()) {
                region.programChangeEvents.push_back(event);
            }
        }
        std::sort(region.programChangeEvents.begin(), region.programChangeEvents.end(), [](const MidiProgramChangeEventState& left, const MidiProgramChangeEventState& right) {
            if (left.beat == right.beat) {
                return left.id < right.id;
            }
            return left.beat < right.beat;
        });
        if (!region.id.empty()) {
            parsed.midiRegions.push_back(region);
        }
    }
    std::sort(parsed.midiRegions.begin(), parsed.midiRegions.end(), [](const MidiRegionState& left, const MidiRegionState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });

    parsed.masterInserts.clear();
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "masterInserts"))) {
        InsertState insert;
        insert.pluginName = stringAfterKey(body, "pluginName");
        insert.pluginAppId = stringAfterKey(body, "pluginAppId");
        insert.pluginFormat = stringAfterKey(body, "pluginFormat");
        insert.pluginPath = stringAfterKey(body, "pluginPath");
        insert.pluginClassId = trim(stringAfterKey(body, "pluginClassId"));
        insert.pluginClassName = trim(stringAfterKey(body, "pluginClassName"));
        insert.bypassed = boolAfterKey(body, "bypassed", false);
        insert.available = boolAfterKey(body, "available", false);
        insert.dspExecutionMode = trim(stringAfterKey(body, "dspExecutionMode"));
        if (!(insert.dspExecutionMode == "native" ||
              insert.dspExecutionMode == "internal" ||
              insert.dspExecutionMode == "remote_internal" ||
              insert.dspExecutionMode == "external")) {
            insert.dspExecutionMode = "native";
        }
        insert.assignedDspServerId = trim(stringAfterKey(body, "assignedDspServerId"));
        insert.serverModuleId = trim(stringAfterKey(body, "serverModuleId"));
        if (insert.serverModuleId.empty()) {
            insert.serverModuleId = trim(stringAfterKey(body, "externalPluginId"));
        }
        insert.reportedLatencySamples = static_cast<unsigned int>(std::max(0.0, numberAfterKey(body, "reportedLatencySamples", 0.0)));
        insert.dspAvailable = boolAfterKey(body, "dspAvailable", true);
        insert.dspLastError = trim(stringAfterKey(body, "dspLastError"));
        insert.parameters = vst3ParametersAfterKey(body, "parameters");
        if (!insert.pluginName.empty()) {
            parsed.masterInserts.push_back(insert);
        }
    }
    parsed.masterInserts.erase(
        std::remove_if(parsed.masterInserts.begin(), parsed.masterInserts.end(), isMonitorDspInsert),
        parsed.masterInserts.end());

    const auto defaultModules = defaultMonitorDspModules();
    parsed.monitorModules = defaultModules;
    for (const auto& body : objectBodies(arrayBodyAfterKey(text, "monitorModules"))) {
        const auto id = stringAfterKey(body, "id");
        const auto found = std::find_if(parsed.monitorModules.begin(), parsed.monitorModules.end(), [&](const MonitorDspModule& module) {
            return module.id == id;
        });
        if (found != parsed.monitorModules.end()) {
            found->enabled = boolAfterKey(body, "enabled", found->enabled);
            const auto realModel = stringAfterKey(body, "realModel");
	            const auto targetModelA = stringAfterKey(body, "targetModelA");
	            const auto targetModelB = stringAfterKey(body, "targetModelB");
	            const auto targetModelC = stringAfterKey(body, "targetModelC");
	            const auto speakerOutputA = stringAfterKey(body, "speakerOutputA");
	            const auto speakerOutputB = stringAfterKey(body, "speakerOutputB");
	            const auto speakerOutputC = stringAfterKey(body, "speakerOutputC");
	            const auto streamingPreview = stringAfterKey(body, "streamingPreview");
            if (!realModel.empty()) {
                found->realModel = realModel;
            }
            if (!targetModelA.empty()) {
                found->targetModelA = targetModelA;
            }
            if (!targetModelB.empty()) {
                found->targetModelB = targetModelB;
            }
	            if (!targetModelC.empty()) {
	                found->targetModelC = targetModelC;
	            }
	            if (!speakerOutputA.empty()) {
	                found->speakerOutputA = speakerOutputA;
	            }
	            if (!speakerOutputB.empty()) {
	                found->speakerOutputB = speakerOutputB;
	            }
	            if (!speakerOutputC.empty()) {
	                found->speakerOutputC = speakerOutputC;
	            }
	            // Direct assign (allows clearing) — these keys are absent in older projects.
	            found->powerAmpA = stringAfterKey(body, "powerAmpA");
	            found->powerAmpB = stringAfterKey(body, "powerAmpB");
	            found->powerAmpC = stringAfterKey(body, "powerAmpC");
	            found->speakerCableA = stringAfterKey(body, "speakerCableA");
	            found->speakerCableB = stringAfterKey(body, "speakerCableB");
	            found->speakerCableC = stringAfterKey(body, "speakerCableC");
	            found->realModelA = stringAfterKey(body, "realModelA");
	            found->realModelB = stringAfterKey(body, "realModelB");
	            found->realModelC = stringAfterKey(body, "realModelC");
	            found->realAmpA = stringAfterKey(body, "realAmpA");
	            found->realAmpB = stringAfterKey(body, "realAmpB");
	            found->realAmpC = stringAfterKey(body, "realAmpC");
	            found->realCableA = stringAfterKey(body, "realCableA");
	            found->realCableB = stringAfterKey(body, "realCableB");
	            found->realCableC = stringAfterKey(body, "realCableC");
	            if (!streamingPreview.empty()) {
	                found->streamingPreview = streamingPreview;
	            }
	            found->activeTargetSlot = static_cast<int>(finiteRange(numberAfterKey(body, "activeTargetSlot", found->activeTargetSlot), 0.0, 0.0, 2.0));
	            found->speakerRoomEqA = boolAfterKey(body, "speakerRoomEqA", found->speakerRoomEqA);
	            found->speakerRoomEqB = boolAfterKey(body, "speakerRoomEqB", found->speakerRoomEqB);
	            found->speakerRoomEqC = boolAfterKey(body, "speakerRoomEqC", found->speakerRoomEqC);
	            found->speakerSimulationWeightA = static_cast<float>(finiteRange(numberAfterKey(body, "speakerSimulationWeightA", found->speakerSimulationWeightA), 0.0, -0.5, 1.0));
	            found->speakerSimulationWeightB = static_cast<float>(finiteRange(numberAfterKey(body, "speakerSimulationWeightB", found->speakerSimulationWeightB), 0.0, -0.5, 1.0));
	            found->speakerSimulationWeightC = static_cast<float>(finiteRange(numberAfterKey(body, "speakerSimulationWeightC", found->speakerSimulationWeightC), 0.0, -0.5, 1.0));
	            found->speakerInsertsA = trackInsertSlotsAfterKey(body, "speakerInsertsA");
	            found->speakerInsertsB = trackInsertSlotsAfterKey(body, "speakerInsertsB");
	            found->speakerInsertsC = trackInsertSlotsAfterKey(body, "speakerInsertsC");
        }
    }

    project = std::move(parsed);
    error.clear();
    return true;
}

bool deserializeProjectForPath(const std::string& text,
                               const std::filesystem::path& projectPath,
                               ProjectDocument& project,
                               std::string& error) {
    if (!deserializeProject(text, project, error)) {
        return false;
    }
    for (auto& clip : project.clips) {
        clip.sourcePath = resolvePathFromProject(clip.sourcePath, projectPath);
    }
    for (auto& source : project.mediaSources) {
        source.path = resolvePathFromProject(source.path, projectPath);
    }
    for (auto& source : project.videoSources) {
        source.path = resolvePathFromProject(source.path, projectPath);
    }
    for (auto& insert : project.masterInserts) {
        if (pluginPathShouldResolveWithProject(insert.pluginFormat)) {
            insert.pluginPath = resolvePathFromProject(insert.pluginPath, projectPath);
        }
    }
    for (auto& track : project.tracks) {
        resolveTrackPluginPathsFromProject(track, projectPath);
    }
    rebuildProjectClipsFromActivePlaylists(project);
    return true;
}

std::string copyAudioFileToProjectMedia(const std::filesystem::path& sourcePath,
                                        const std::filesystem::path& projectPath,
                                        std::string& error) {
    error.clear();
    if (sourcePath.empty() || projectPath.empty()) {
        error = "Source audio path or project path is empty.";
        return {};
    }

    std::error_code fsError;
    if (!std::filesystem::exists(sourcePath, fsError) || !std::filesystem::is_regular_file(sourcePath, fsError)) {
        error = "Source audio file does not exist.";
        return {};
    }

    const auto mediaDirectory = projectMediaDirectory(projectPath);
    const auto sourceAbsolute = normalizedLexicalPath(std::filesystem::absolute(sourcePath));
    if (pathStartsWith(sourceAbsolute, mediaDirectory)) {
        return pathToProjectPortableString(sourceAbsolute);
    }

    std::filesystem::create_directories(mediaDirectory, fsError);
    if (fsError) {
        error = "Could not create project Audio Files folder: " + fsError.message();
        return {};
    }

    auto targetPath = uniqueMediaPath(mediaDirectory, sourcePath);
    if (targetPath.empty()) {
        error = "Could not find an available media filename.";
        return {};
    }
    if (pathsReferToSameFile(sourceAbsolute, targetPath)) {
        return pathToProjectPortableString(targetPath);
    }

    std::filesystem::copy_file(sourceAbsolute, targetPath, std::filesystem::copy_options::none, fsError);
    if (fsError) {
        error = "Could not copy audio into project media folder: " + fsError.message();
        return {};
    }
    return pathToProjectPortableString(targetPath);
}

std::string copyVideoFileToProjectMedia(const std::filesystem::path& sourcePath,
                                        const std::filesystem::path& projectPath,
                                        std::string& error) {
    error.clear();
    if (sourcePath.empty() || projectPath.empty()) {
        error = "Source video path or project path is empty.";
        return {};
    }

    std::error_code fsError;
    if (!std::filesystem::exists(sourcePath, fsError) || !std::filesystem::is_regular_file(sourcePath, fsError)) {
        error = "Source video file does not exist.";
        return {};
    }

    const auto videoDirectory = projectVideoDirectory(projectPath);
    const auto sourceAbsolute = normalizedLexicalPath(std::filesystem::absolute(sourcePath));
    if (pathStartsWith(sourceAbsolute, videoDirectory)) {
        return pathToProjectPortableString(sourceAbsolute);
    }

    std::filesystem::create_directories(videoDirectory, fsError);
    if (fsError) {
        error = "Could not create project Video Files folder: " + fsError.message();
        return {};
    }

    auto targetPath = uniqueMediaPath(videoDirectory, sourcePath);
    if (targetPath.empty()) {
        error = "Could not find an available video filename.";
        return {};
    }
    if (pathsReferToSameFile(sourceAbsolute, targetPath)) {
        return pathToProjectPortableString(targetPath);
    }

    std::filesystem::copy_file(sourceAbsolute, targetPath, std::filesystem::copy_options::none, fsError);
    if (fsError) {
        error = "Could not copy video into project Video Files folder: " + fsError.message();
        return {};
    }
    return pathToProjectPortableString(targetPath);
}

ProjectMediaCollectReport collectProjectMedia(ProjectDocument& project,
                                              const std::filesystem::path& projectPath) {
    ProjectMediaCollectReport report;
    if (projectPath.empty()) {
        report.failedClips = project.clips.size();
        report.messages.push_back("Project must be saved before media can be collected.");
        return report;
    }

    const auto mediaDirectory = projectMediaDirectory(projectPath);
    std::map<std::string, std::string> collectedBySource;
    for (auto& clip : project.clips) {
        if (clip.sourcePath.empty()) {
            continue;
        }

        const auto sourcePath = normalizedLexicalPath(std::filesystem::path(clip.sourcePath));
        const auto sourceAbsolute = sourcePath.is_absolute()
            ? normalizedLexicalPath(sourcePath)
            : normalizedLexicalPath(std::filesystem::absolute(projectBaseDirectory(projectPath) / sourcePath));

        std::error_code fsError;
        if (!std::filesystem::exists(sourceAbsolute, fsError) || !std::filesystem::is_regular_file(sourceAbsolute, fsError)) {
            ++report.missingClips;
            report.messages.push_back("Missing media: " + (clip.id.empty() ? clip.sourcePath : clip.id));
            continue;
        }

        if (pathStartsWith(sourceAbsolute, mediaDirectory)) {
            clip.sourcePath = pathToProjectPortableString(sourceAbsolute);
            ++report.alreadyInProjectClips;
            continue;
        }

        const auto sourceKey = pathToProjectPortableString(sourceAbsolute);
        auto reused = collectedBySource.find(sourceKey);
        if (reused != collectedBySource.end()) {
            clip.sourcePath = reused->second;
            ++report.copiedClips;
            continue;
        }

        std::string error;
        const auto copiedPath = copyAudioFileToProjectMedia(sourceAbsolute, projectPath, error);
        if (copiedPath.empty()) {
            ++report.failedClips;
            report.messages.push_back("Could not collect " + (clip.id.empty() ? clip.sourcePath : clip.id) + ": " + error);
            continue;
        }

        collectedBySource[sourceKey] = copiedPath;
        clip.sourcePath = copiedPath;
        ++report.copiedClips;
    }
    return report;
}

ProjectHealthReport analyzeProjectHealth(const ProjectDocument& project) {
    return analyzeProjectHealth(project, {});
}

ProjectHealthReport analyzeProjectHealth(const ProjectDocument& project, const std::filesystem::path& projectPath) {
    ProjectHealthReport report;
    report.clips = project.clips.size();
    report.masterInserts = project.masterInserts.size();

    for (const auto& track : project.tracks) {
        if (isProtectedTrackName(track.name)) {
            continue;
        }
        report.trackInserts += track.inserts.size();
        if (track.muted) {
            ++report.mutedAudioTracks;
            report.messages.push_back("Muted audio track: " + track.name);
        }
        if (track.solo) {
            ++report.soloedAudioTracks;
            report.messages.push_back("Soloed audio track: " + track.name);
        }
        for (const auto& insert : track.inserts) {
            InsertState insertState;
            insertState.pluginName = insert.pluginName;
            insertState.pluginFormat = insert.pluginFormat;
            insertState.pluginPath = insert.pluginPath;
            insertState.bypassed = insert.bypassed;
            insertState.available = insert.enabled;
            if (!isVst3Insert(insertState)) {
                continue;
            }
            ++report.vst3TrackInserts;
            if (insert.enabled && !insert.bypassed) {
                ++report.activeVst3TrackInserts;
                report.activeVst3TrackInsertLabels.push_back(track.name + ": " +
                    (insert.pluginName.empty() ? insert.pluginPath : insert.pluginName));
                report.messages.push_back("Active track VST3 insert will render on direct Master/Main outputs: " + track.name + " / " +
                    (insert.pluginName.empty() ? insert.pluginPath : insert.pluginName));
            }
            const auto pluginPath = projectPath.empty()
                ? std::filesystem::path(insert.pluginPath)
                : std::filesystem::path(resolvePathFromProject(insert.pluginPath, projectPath));
            if (!pathExists(pluginPath)) {
                ++report.missingVst3Inserts;
                report.messages.push_back("Missing track VST3 insert: " + track.name + " / " +
                    (insert.pluginName.empty() ? insert.pluginPath : insert.pluginName));
            }
        }
    }

    for (size_t leftIndex = 0; leftIndex < project.clips.size(); ++leftIndex) {
        const auto& left = project.clips[leftIndex];
        const double leftEnd = left.startSeconds + left.durationSeconds;
        for (size_t rightIndex = leftIndex + 1; rightIndex < project.clips.size(); ++rightIndex) {
            const auto& right = project.clips[rightIndex];
            if (left.trackName != right.trackName) {
                continue;
            }
            const double rightEnd = right.startSeconds + right.durationSeconds;
            if (std::max(left.startSeconds, right.startSeconds) < std::min(leftEnd, rightEnd)) {
                ++report.overlappingClipPairs;
                report.messages.push_back("Overlapping clips on " + left.trackName + ": " +
                    (left.id.empty() ? left.sourcePath : left.id) + " / " +
                    (right.id.empty() ? right.sourcePath : right.id));
            }
        }
    }

    for (const auto& clip : project.clips) {
        if (clip.muted) {
            ++report.mutedClips;
            report.messages.push_back("Muted clip: " + (clip.id.empty() ? clip.sourcePath : clip.id));
        }
        const auto clipPath = projectPath.empty()
            ? std::filesystem::path(clip.sourcePath)
            : std::filesystem::path(resolvePathFromProject(clip.sourcePath, projectPath));
        if (!pathExists(clipPath)) {
            ++report.missingMediaClips;
            report.messages.push_back("Missing media clip: " + (clip.id.empty() ? clip.sourcePath : clip.id));
        }
    }

    for (const auto& insert : project.masterInserts) {
        if (!isVst3Insert(insert)) {
            continue;
        }
        ++report.vst3MasterInserts;
        const auto pluginPath = projectPath.empty()
            ? std::filesystem::path(insert.pluginPath)
            : std::filesystem::path(resolvePathFromProject(insert.pluginPath, projectPath));
        if (!pathExists(pluginPath)) {
            ++report.missingVst3Inserts;
            report.messages.push_back("Missing VST3 insert: " + (insert.pluginName.empty() ? insert.pluginPath : insert.pluginName));
        }
    }

    for (const auto& module : project.monitorModules) {
        if (!module.enabled) {
            ++report.disabledMonitorModules;
        }
    }

    return report;
}

std::string summarizeProjectHealth(const ProjectHealthReport& report) {
    std::ostringstream summary;
    summary << "Project health: " << report.clips << " clip(s)";
    if (report.missingMediaClips == 0 && report.missingVst3Inserts == 0 && report.overlappingClipPairs == 0) {
        summary << ", all media/plugin references found";
    } else {
        if (report.overlappingClipPairs > 0) {
            summary << ", " << report.overlappingClipPairs << " overlapping clip pair(s)";
        }
        if (report.missingMediaClips > 0) {
            summary << ", " << report.missingMediaClips << " missing media";
        }
        if (report.missingVst3Inserts > 0) {
            summary << ", " << report.missingVst3Inserts << " missing VST3";
        }
    }
    summary << ", " << report.vst3MasterInserts << " master VST3 insert(s)";
    if (report.vst3TrackInserts > 0) {
        summary << ", " << report.vst3TrackInserts << " track VST3 insert(s)";
    }
    if (report.activeVst3TrackInserts > 0) {
        summary << ", " << report.activeVst3TrackInserts << " active track VST3 direct-output render(s)";
    }
    if (report.mutedAudioTracks > 0) {
        summary << ", " << report.mutedAudioTracks << " muted track(s)";
    }
    if (report.soloedAudioTracks > 0) {
        summary << ", " << report.soloedAudioTracks << " soloed track(s)";
    }
    if (report.mutedClips > 0) {
        summary << ", " << report.mutedClips << " muted clip(s)";
    }
    if (report.disabledMonitorModules > 0) {
        summary << ", " << report.disabledMonitorModules << " monitor module(s) disabled";
    }
    return summary.str();
}

std::string summarizeProjectHealth(const ProjectDocument& project) {
    return summarizeProjectHealth(analyzeProjectHealth(project));
}

std::string summarizeProjectHealth(const ProjectDocument& project, const std::filesystem::path& projectPath) {
    return summarizeProjectHealth(analyzeProjectHealth(project, projectPath));
}

std::filesystem::path projectBackupPath(const std::filesystem::path& projectPath) {
    if (projectPath.empty()) {
        return {};
    }
    auto backup = projectPath;
    backup.replace_extension(projectPath.extension().string() + ".bak");
    return backup;
}

bool backupExistingProjectFile(const std::filesystem::path& projectPath, std::string& error) {
    error.clear();
    if (projectPath.empty()) {
        error = "Project path is empty.";
        return false;
    }

    std::error_code fsError;
    if (!std::filesystem::exists(projectPath, fsError)) {
        return true;
    }
    if (!std::filesystem::is_regular_file(projectPath, fsError)) {
        error = "Project path is not a regular file.";
        return false;
    }

    const auto backupPath = projectBackupPath(projectPath);
    if (backupPath.empty()) {
        error = "Project backup path is empty.";
        return false;
    }

    std::filesystem::copy_file(projectPath, backupPath, std::filesystem::copy_options::overwrite_existing, fsError);
    if (fsError) {
        error = "Could not create project backup: " + fsError.message();
        return false;
    }
    return true;
}

bool saveProjectFileWithBackup(const ProjectDocument& project,
                               const std::filesystem::path& projectPath,
                               std::string& error) {
    error.clear();
    if (projectPath.empty()) {
        error = "Project path is empty.";
        return false;
    }

    if (!backupExistingProjectFile(projectPath, error)) {
        return false;
    }

    std::string writeError;
    if (!writeTextFileAtomically(projectPath, serializeProjectForPath(project, projectPath), ".saving", writeError)) {
        error = "Could not save project file: " + writeError;
        return false;
    }

    return true;
}

std::filesystem::path projectAutosavePath(const std::filesystem::path& projectPath) {
    if (projectPath.empty()) {
        return {};
    }
    auto autosave = projectPath;
    autosave.replace_extension(projectPath.extension().string() + ".autosave");
    return autosave;
}

bool projectAutosaveIsNewerThanProject(const std::filesystem::path& projectPath) {
    if (projectPath.empty()) {
        return false;
    }
    const auto autosavePath = projectAutosavePath(projectPath);
    if (autosavePath.empty()) {
        return false;
    }

    std::error_code projectError;
    std::error_code autosaveError;
    if (!std::filesystem::exists(projectPath, projectError) ||
        !std::filesystem::exists(autosavePath, autosaveError) ||
        projectError || autosaveError) {
        return false;
    }

    const auto projectTime = std::filesystem::last_write_time(projectPath, projectError);
    const auto autosaveTime = std::filesystem::last_write_time(autosavePath, autosaveError);
    if (projectError || autosaveError) {
        return false;
    }
    return autosaveTime > projectTime;
}

bool writeProjectAutosaveFile(const ProjectDocument& project, const std::filesystem::path& projectPath, std::string& error) {
    error.clear();
    if (projectPath.empty()) {
        error = "Project path is empty.";
        return false;
    }

    const auto autosavePath = projectAutosavePath(projectPath);
    if (autosavePath.empty()) {
        error = "Project autosave path is empty.";
        return false;
    }

    std::string writeError;
    if (!writeTextFileAtomically(autosavePath,
                                 serializeProjectForPath(project, projectPath),
                                 ".saving",
                                 writeError)) {
        error = "Could not save project autosave file: " + writeError;
        return false;
    }
    return true;
}

bool loadProjectAutosaveFile(const std::filesystem::path& projectPath, ProjectDocument& project, std::string& error) {
    error.clear();
    const auto autosavePath = projectAutosavePath(projectPath);
    if (autosavePath.empty()) {
        error = "Project autosave path is empty.";
        return false;
    }

    std::ifstream in(autosavePath, std::ios::binary);
    if (!in) {
        error = "Could not open project autosave file.";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return deserializeProjectForPath(text, projectPath, project, error);
}

bool removeProjectAutosaveFile(const std::filesystem::path& projectPath, std::string& error) {
    error.clear();
    if (projectPath.empty()) {
        return true;
    }
    const auto autosavePath = projectAutosavePath(projectPath);
    if (autosavePath.empty()) {
        return true;
    }

    std::error_code fsError;
    if (!std::filesystem::exists(autosavePath, fsError)) {
        return true;
    }
    std::filesystem::remove(autosavePath, fsError);
    if (fsError) {
        error = "Could not remove project autosave file: " + fsError.message();
        return false;
    }
    return true;
}

std::string nextProjectRecordingPath(const std::filesystem::path& projectPath, std::string& error) {
    error.clear();
    if (projectPath.empty()) {
        error = "Project path is empty.";
        return {};
    }

    std::error_code fsError;
    const auto mediaDirectory = projectMediaDirectory(projectPath);
    std::filesystem::create_directories(mediaDirectory, fsError);
    if (fsError) {
        error = "Could not create project Audio Files folder: " + fsError.message();
        return {};
    }

    const auto targetPath = uniqueMediaPath(mediaDirectory, "Neuracoust DAW Recording.wav");
    if (targetPath.empty()) {
        error = "Could not find an available recording filename.";
        return {};
    }
    return pathToProjectPortableString(targetPath);
}

std::filesystem::path recordedTakeManifestPath(const std::filesystem::path& recordedWavPath) {
    if (recordedWavPath.empty()) {
        return {};
    }
    auto manifestPath = recordedWavPath;
    manifestPath += ".recording.json";
    return manifestPath;
}

std::filesystem::path importedMediaManifestPath(const std::filesystem::path& mediaWavPath) {
    if (mediaWavPath.empty()) {
        return {};
    }
    auto manifestPath = mediaWavPath;
    manifestPath += ".import.json";
    return manifestPath;
}

bool writeImportedMediaManifest(const ProjectDocument& project,
                                const std::filesystem::path& mediaWavPath,
                                const std::string& originalSourcePath,
                                const std::string& clipId,
                                const std::string& trackName,
                                double startSeconds,
                                double durationSeconds,
                                int sourceBitsPerSample,
                                bool sourceFloatingPoint,
                                double sourceSampleRate,
                                int sourceChannels,
                                bool convertedToProjectSampleRate,
                                bool convertedToProjectBitDepth,
                                const std::string& sampleRateImportPolicy,
                                const std::string& bitDepthImportPolicy,
                                double sourceTempoBpm,
                                const std::string& tempoSyncPolicy,
                                bool pendingTimeStretchToProject,
                                std::string& error) {
    error.clear();
    if (mediaWavPath.empty()) {
        error = "Imported media path is empty.";
        return false;
    }
    if (clipId.empty() || trackName.empty() ||
        !std::isfinite(startSeconds) || !std::isfinite(durationSeconds) || durationSeconds < 0.0) {
        error = "Imported media manifest metadata is invalid.";
        return false;
    }

    const double importBeatSeconds = project.tempoBpm > 0 ? 60.0 / static_cast<double>(project.tempoBpm) : 0.5;
    const double importBarSeconds = importBeatSeconds *
        static_cast<double>(std::max(1, project.timeSignatureNumerator)) *
        (4.0 / static_cast<double>(std::max(1, project.timeSignatureDenominator)));
    const double importBarCount = durationSeconds / std::max(0.25, importBarSeconds);
    const double importQuarterNoteBeats = durationSeconds / std::max(0.01, importBeatSeconds);
    const double importNotatedBeatCount = importQuarterNoteBeats *
        (static_cast<double>(std::max(1, project.timeSignatureDenominator)) / 4.0);

    const auto manifestPath = importedMediaManifestPath(mediaWavPath);
    std::ostringstream out;
    out << "{\n";
    out << "  \"format\": \"neuracoust-daw-imported-media-v1\",\n";
    out << "  \"projectName\": \"" << escapeJsonString(project.name) << "\",\n";
    out << "  \"file\": \"" << escapeJsonString(mediaWavPath.filename().generic_string()) << "\",\n";
    out << "  \"originalSourcePath\": \"" << escapeJsonString(originalSourcePath) << "\",\n";
    out << "  \"clipId\": \"" << escapeJsonString(clipId) << "\",\n";
    out << "  \"trackName\": \"" << escapeJsonString(trackName) << "\",\n";
    out << "  \"startSeconds\": " << startSeconds << ",\n";
    out << "  \"durationSeconds\": " << durationSeconds << ",\n";
    out << "  \"projectSampleRate\": " << project.sampleRate << ",\n";
    out << "  \"projectBitDepth\": " << project.bitDepth << ",\n";
    out << "  \"sourceBitsPerSample\": " << sourceBitsPerSample << ",\n";
    out << "  \"sourceFloatingPoint\": " << (sourceFloatingPoint ? "true" : "false") << ",\n";
    out << "  \"sourceSampleRate\": " << sourceSampleRate << ",\n";
    out << "  \"sourceChannels\": " << sourceChannels << ",\n";
    out << "  \"convertedToProjectSampleRate\": " << (convertedToProjectSampleRate ? "true" : "false") << ",\n";
    out << "  \"convertedToProjectBitDepth\": " << (convertedToProjectBitDepth ? "true" : "false") << ",\n";
    out << "  \"sampleRateImportPolicy\": \"" << escapeJsonString(sampleRateImportPolicy.empty() ? "matched" : sampleRateImportPolicy) << "\",\n";
    out << "  \"bitDepthImportPolicy\": \"" << escapeJsonString(bitDepthImportPolicy.empty() ? "matched" : bitDepthImportPolicy) << "\",\n";
    out << "  \"sourceTempoBpm\": " << (std::isfinite(sourceTempoBpm) ? sourceTempoBpm : 0.0) << ",\n";
    out << "  \"tempoSyncPolicy\": \"" << escapeJsonString(tempoSyncPolicy.empty() ? "project-tempo" : tempoSyncPolicy) << "\",\n";
    out << "  \"pendingTimeStretchToProject\": " << (pendingTimeStretchToProject ? "true" : "false") << ",\n";
    out << "  \"timecodeStartSeconds\": " << project.timecodeStartSeconds << ",\n";
    out << "  \"analysis\": {\n";
    out << "    \"tempoBpm\": " << project.tempoBpm << ",\n";
    out << "    \"timeSignature\": \"" << project.timeSignatureNumerator << "/" << project.timeSignatureDenominator << "\",\n";
    out << "    \"barCount\": " << importBarCount << ",\n";
    out << "    \"beatCount\": " << importNotatedBeatCount << ",\n";
    out << "    \"grooveFeel\": \"" << escapeJsonString(project.grooveFeel.empty() ? "straight" : project.grooveFeel) << "\",\n";
    out << "    \"panLaw\": \"" << escapeJsonString(project.panLaw.empty() ? "legacy" : project.panLaw) << "\",\n";
    out << "    \"grooveSwingAmount\": " << project.grooveSwingAmount << ",\n";
    out << "    \"detectedKey\": \"" << escapeJsonString(project.detectedKey.empty() ? "C" : project.detectedKey) << "\",\n";
    out << "    \"detectedKeyMode\": \"" << escapeJsonString(project.detectedKeyMode.empty() ? "major" : project.detectedKeyMode) << "\",\n";
    out << "    \"chordKeyModePreference\": \"" << escapeJsonString(project.chordKeyModePreference.empty() ? "auto" : project.chordKeyModePreference) << "\",\n";
    out << "    \"tempoMarkerCount\": " << project.tempoMap.size() << ",\n";
    out << "    \"chordEventCount\": " << project.chordEvents.size() << ",\n";
    out << "    \"markerCount\": " << project.markers.size() << "\n";
    out << "  }\n";
    out << "}\n";
    if (!writeTextFileAtomically(manifestPath, out.str(), ".saving", error)) {
        error = "Could not write imported media manifest: " + error;
        return false;
    }
    return true;
}

bool writeRecordedTakeManifest(const ProjectDocument& project,
                               const std::filesystem::path& recordedWavPath,
                               const std::string& clipId,
                               const std::string& trackName,
                               double startSeconds,
                               double durationSeconds,
                               const std::string& inputDeviceId,
                               std::string& error) {
    error.clear();
    if (recordedWavPath.empty()) {
        error = "Recorded take path is empty.";
        return false;
    }
    if (clipId.empty() || trackName.empty() ||
        !std::isfinite(startSeconds) || !std::isfinite(durationSeconds) || durationSeconds < 0.0) {
        error = "Recorded take manifest metadata is invalid.";
        return false;
    }
    const auto trackIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    const std::string inputBus = trackIt != project.tracks.end() ? trackIt->inputBus : "";
    const int inputChannels = inputChannelCountForManifestBus(inputBus);
    const bool lowLatencyMonitoringEligible = trackIt != project.tracks.end() &&
        (trackIt->trackType == "audio" || trackIt->trackType == "aux" || trackIt->trackType == "bus_folder") &&
        (trackIt->recordArmed || trackIt->inputMonitoring) &&
        inputChannels > 0;

    const auto manifestPath = recordedTakeManifestPath(recordedWavPath);
    std::ostringstream out;
    out << "{\n";
    out << "  \"format\": \"neuracoust-daw-recording-take-v1\",\n";
    out << "  \"projectName\": \"" << escapeJsonString(project.name) << "\",\n";
    out << "  \"file\": \"" << escapeJsonString(recordedWavPath.filename().generic_string()) << "\",\n";
    out << "  \"clipId\": \"" << escapeJsonString(clipId) << "\",\n";
    out << "  \"trackName\": \"" << escapeJsonString(trackName) << "\",\n";
    out << "  \"startSeconds\": " << startSeconds << ",\n";
    out << "  \"durationSeconds\": " << durationSeconds << ",\n";
    out << "  \"sampleRate\": " << project.sampleRate << ",\n";
    out << "  \"bitDepth\": " << project.bitDepth << ",\n";
    out << "  \"inputDeviceId\": \"" << escapeJsonString(inputDeviceId) << "\",\n";
    out << "  \"inputBus\": \"" << escapeJsonString(inputBus) << "\",\n";
    out << "  \"inputChannels\": " << inputChannels << ",\n";
    out << "  \"lowLatencyRecordMonitoringEligible\": " << (lowLatencyMonitoringEligible ? "true" : "false") << ",\n";
    out << "  \"inputMonitoring\": " << ((trackIt != project.tracks.end() && trackIt->inputMonitoring) ? "true" : "false") << ",\n";
    out << "  \"recordMonitorPath\": \"record-or-input-monitor-low-latency\",\n";
    out << "  \"recordArmedTrack\": \"" << escapeJsonString(trackName) << "\",\n";
    out << "  \"timecodeStartSeconds\": " << project.timecodeStartSeconds << "\n";
    out << "}\n";
    if (!writeTextFileAtomically(manifestPath, out.str(), ".saving", error)) {
        error = "Could not write recorded take manifest: " + error;
        return false;
    }
    return true;
}

bool applyDefaultProjectNameFromPath(ProjectDocument& project, const std::filesystem::path& projectPath) {
    if (projectPath.empty() || (!project.name.empty() && project.name != "Untitled")) {
        return false;
    }
    const auto name = pathStemString(projectPath);
    if (name.empty()) {
        return false;
    }
    project.name = name;
    return true;
}

std::filesystem::path normalizedProjectSavePath(const std::filesystem::path& requestedPath) {
    if (requestedPath.empty()) {
        return {};
    }

    std::filesystem::path path = requestedPath;
    if (path.extension().empty()) {
        path.replace_extension(".ndaw");
    }

    const auto projectStem = path.stem();
    if (projectStem.empty()) {
        return path;
    }

    const auto parent = path.parent_path();
    if (!parent.empty() && parent.filename() == projectStem) {
        return path;
    }

    const auto projectDirectory = parent.empty() ? std::filesystem::path(projectStem) : parent / projectStem;
    return projectDirectory / path.filename();
}

} // namespace neuracoust::daw
