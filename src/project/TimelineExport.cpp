#include "project/TimelineExport.h"
#include "project/EditOperations.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace neuracoust::daw {

namespace {

struct EdlEvent {
    const ClipState* clip = nullptr;
    const TrackState* track = nullptr;
};

bool protectedTrackName(const std::string& trackName) {
    return trackName == "Master" || trackName == "Monitor";
}

const TrackState* findTrack(const ProjectDocument& project, const std::string& trackName) {
    const auto it = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    return it == project.tracks.end() ? nullptr : &*it;
}

const VideoSourceState* findVideoSource(const ProjectDocument& project, const std::string& sourceId) {
    const auto it = std::find_if(project.videoSources.begin(), project.videoSources.end(), [&](const VideoSourceState& source) {
        return source.id == sourceId;
    });
    return it == project.videoSources.end() ? nullptr : &*it;
}

std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (char ch : value) {
        if (ch == '\'') {
            result += "'\\''";
        } else {
            result.push_back(ch);
        }
    }
    result.push_back('\'');
    return result;
}

std::string sanitizeEdlToken(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            result.push_back(static_cast<char>(std::toupper(ch)));
        } else if (ch == '_' || ch == '-' || ch == ' ') {
            result.push_back('_');
        }
        if (result.size() == 8) {
            break;
        }
    }
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }
    return result.empty() ? "AX" : result;
}

std::string clipReelName(const ClipState& clip, const TrackState* track) {
    if (!clip.regionName.empty()) {
        return sanitizeEdlToken(clip.regionName);
    }
    const auto sourceStem = std::filesystem::path(clip.sourcePath).stem().string();
    if (!sourceStem.empty()) {
        return sanitizeEdlToken(sourceStem);
    }
    if (track != nullptr && !track->name.empty()) {
        return sanitizeEdlToken(track->name);
    }
    return sanitizeEdlToken(clip.id);
}

bool frameRateSupportsDropFrame(double frameRate) {
    return std::abs(frameRate - 29.97) < 0.02 ||
        std::abs(frameRate - 59.94) < 0.03;
}

long long dropFrameLabelFrames(long long realFrames, long long nominalFramesPerSecond) {
    const long long dropFrames = nominalFramesPerSecond == 60 ? 4 : 2;
    const long long framesPerMinute = nominalFramesPerSecond * 60 - dropFrames;
    const long long framesPer10Minutes = nominalFramesPerSecond * 60 * 10 - dropFrames * 9;
    const long long framesPer24Hours = nominalFramesPerSecond * 60 * 60 * 24;
    long long frames = ((realFrames % framesPer24Hours) + framesPer24Hours) % framesPer24Hours;
    const long long tenMinuteChunks = frames / framesPer10Minutes;
    const long long remainder = frames % framesPer10Minutes;
    if (remainder >= dropFrames) {
        frames += dropFrames * (9 * tenMinuteChunks + (remainder - dropFrames) / framesPerMinute);
    } else {
        frames += dropFrames * 9 * tenMinuteChunks;
    }
    return frames;
}

std::string timecodeFromSeconds(double seconds, double frameRate, bool dropFrame = false) {
    seconds = std::max(0.0, seconds);
    const auto framesPerSecond = std::max<long long>(1, static_cast<long long>(std::llround(frameRate)));
    auto wholeFrames = static_cast<long long>(std::llround(seconds * frameRate));
    const char frameSeparator = dropFrame && frameRateSupportsDropFrame(frameRate) ? ';' : ':';
    if (frameSeparator == ';') {
        wholeFrames = dropFrameLabelFrames(wholeFrames, framesPerSecond);
    }
    const auto frame = wholeFrames % framesPerSecond;
    const auto totalSeconds = wholeFrames / framesPerSecond;
    const auto sec = totalSeconds % 60;
    const auto totalMinutes = totalSeconds / 60;
    const auto min = totalMinutes % 60;
    const auto hour = totalMinutes / 60;

    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << hour << ":"
        << std::setw(2) << min << ":"
        << std::setw(2) << sec << frameSeparator
        << std::setw(2) << frame;
    return out.str();
}

double secondsFromTimecode(const std::string& value, double frameRate, double fallback = 0.0) {
    if (!std::isfinite(frameRate) || frameRate < 1.0) {
        return fallback;
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    int frame = 0;
    char a = 0;
    char b = 0;
    char c = 0;
    std::istringstream in(value);
    if (!(in >> hour >> a >> minute >> b >> second >> c >> frame) || a != ':' || b != ':' || (c != ':' && c != ';')) {
        return fallback;
    }
    if (hour < 0 || minute < 0 || minute >= 60 || second < 0 || second >= 60 || frame < 0) {
        return fallback;
    }
    return static_cast<double>(hour * 3600 + minute * 60 + second) + static_cast<double>(frame) / frameRate;
}

std::string commentValue(std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value.empty() ? "-" : value;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string escapeXmlString(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '&': out << "&amp;"; break;
            case '<': out << "&lt;"; break;
            case '>': out << "&gt;"; break;
            case '"': out << "&quot;"; break;
            case '\'': out << "&apos;"; break;
            default:
                if (ch >= 0x20 || ch == '\t' || ch == '\n' || ch == '\r') {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

std::string secondsForFcpxml(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    const auto millis = static_cast<long long>(std::llround(seconds * 1000.0));
    return std::to_string(millis) + "/1000s";
}

std::string fixedSeconds(double seconds) {
    if (!std::isfinite(seconds)) {
        seconds = 0.0;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << seconds;
    return out.str();
}

std::string fixedNumber(double value, int precision = 3) {
    if (!std::isfinite(value)) {
        value = 0.0;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendU24(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendAscii(std::vector<uint8_t>& out, const char* text) {
    while (*text != '\0') {
        out.push_back(static_cast<uint8_t>(*text++));
    }
}

void appendVarLen(std::vector<uint8_t>& out, uint32_t value) {
    uint8_t buffer[5] {};
    int index = 0;
    buffer[index++] = static_cast<uint8_t>(value & 0x7f);
    while ((value >>= 7) != 0 && index < 5) {
        buffer[index++] = static_cast<uint8_t>((value & 0x7f) | 0x80);
    }
    while (index-- > 0) {
        out.push_back(buffer[index]);
    }
}

std::string midiTextSafe(std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

void appendMetaTextAtTick(std::vector<uint8_t>& track,
                          uint32_t& previousTick,
                          uint32_t tick,
                          uint8_t metaType,
                          const std::string& text) {
    appendVarLen(track, tick >= previousTick ? tick - previousTick : 0);
    previousTick = tick;
    track.push_back(0xff);
    track.push_back(metaType);
    const auto safe = midiTextSafe(text);
    appendVarLen(track, static_cast<uint32_t>(safe.size()));
    track.insert(track.end(), safe.begin(), safe.end());
}

void appendTempoAtTick(std::vector<uint8_t>& track, uint32_t& previousTick, uint32_t tick, double bpm) {
    appendVarLen(track, tick >= previousTick ? tick - previousTick : 0);
    previousTick = tick;
    const auto safeBpm = std::max(20.0, std::min(400.0, std::isfinite(bpm) ? bpm : 120.0));
    const auto microsecondsPerQuarter = static_cast<uint32_t>(std::llround(60000000.0 / safeBpm));
    track.push_back(0xff);
    track.push_back(0x51);
    track.push_back(0x03);
    appendU24(track, microsecondsPerQuarter);
}

uint8_t midiTimeSignatureDenominatorPower(int denominator) {
    const int safeDenominator = std::max(1, std::min(64, denominator));
    uint8_t power = 0;
    int value = 1;
    while (value < safeDenominator && value < 64) {
        value <<= 1;
        ++power;
    }
    return value == safeDenominator ? power : 2;
}

void appendTimeSignatureAtTick(std::vector<uint8_t>& track,
                               uint32_t& previousTick,
                               uint32_t tick,
                               int numerator,
                               int denominator) {
    appendVarLen(track, tick >= previousTick ? tick - previousTick : 0);
    previousTick = tick;
    const int safeNumerator = std::max(1, std::min(127, numerator));
    const int safeDenominator = std::max(1, std::min(64, denominator));
    const bool compoundEighth = safeDenominator == 8 && safeNumerator >= 6 && safeNumerator % 3 == 0;
    track.push_back(0xff);
    track.push_back(0x58);
    track.push_back(0x04);
    track.push_back(static_cast<uint8_t>(safeNumerator));
    track.push_back(midiTimeSignatureDenominatorPower(safeDenominator));
    track.push_back(static_cast<uint8_t>(compoundEighth ? 36 : 24));
    track.push_back(0x08);
}

void appendMidiChannelEventAtTick(std::vector<uint8_t>& track,
                                  uint32_t& previousTick,
                                  uint32_t tick,
                                  uint8_t status,
                                  uint8_t data1,
                                  uint8_t data2) {
    appendVarLen(track, tick >= previousTick ? tick - previousTick : 0);
    previousTick = tick;
    track.push_back(status);
    track.push_back(data1);
    track.push_back(data2);
}

void appendMidiChannelEvent1AtTick(std::vector<uint8_t>& track,
                                   uint32_t& previousTick,
                                   uint32_t tick,
                                   uint8_t status,
                                   uint8_t data1) {
    appendVarLen(track, tick >= previousTick ? tick - previousTick : 0);
    previousTick = tick;
    track.push_back(status);
    track.push_back(data1);
}

void appendEndOfTrack(std::vector<uint8_t>& track) {
    appendVarLen(track, 0);
    track.push_back(0xff);
    track.push_back(0x2f);
    track.push_back(0x00);
}

void appendMidiTrack(std::vector<uint8_t>& file, const std::vector<uint8_t>& track) {
    appendAscii(file, "MTrk");
    appendU32(file, static_cast<uint32_t>(track.size()));
    file.insert(file.end(), track.begin(), track.end());
}

std::vector<TempoMarkerState> sortedValidTempoMap(const ProjectDocument& project) {
    std::vector<TempoMarkerState> tempos;
    for (const auto& marker : project.tempoMap) {
        if (std::isfinite(marker.timeSeconds) && marker.timeSeconds >= 0.0 &&
            std::isfinite(marker.bpm) && marker.bpm >= 20.0 && marker.bpm <= 400.0) {
            tempos.push_back(marker);
        }
    }
    if (tempos.empty()) {
        tempos.push_back({0.0, static_cast<double>(std::max(20, std::min(400, project.tempoBpm)))});
    }
    std::sort(tempos.begin(), tempos.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    if (tempos.front().timeSeconds > 0.0) {
        tempos.insert(tempos.begin(), {0.0, tempos.front().bpm});
    }
    for (size_t index = 1; index < tempos.size();) {
        if (std::abs(tempos[index].timeSeconds - tempos[index - 1].timeSeconds) < 0.000001) {
            tempos[index - 1].bpm = tempos[index].bpm;
            tempos.erase(tempos.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
    return tempos;
}

std::vector<TimeSignatureMarkerState> sortedValidTimeSignatureMap(const ProjectDocument& project) {
    std::vector<TimeSignatureMarkerState> signatures;
    for (const auto& marker : project.timeSignatureMap) {
        if (!std::isfinite(marker.timeSeconds) || marker.timeSeconds < 0.0) {
            continue;
        }
        const int numerator = std::max(1, std::min(16, marker.numerator));
        int denominator = std::max(1, std::min(32, marker.denominator));
        if (denominator != 2 && denominator != 4 && denominator != 8 && denominator != 16 && denominator != 32) {
            denominator = std::max(1, std::min(32, project.timeSignatureDenominator));
        }
        signatures.push_back({marker.timeSeconds, numerator, denominator});
    }
    if (signatures.empty()) {
        signatures.push_back({0.0, project.timeSignatureNumerator, project.timeSignatureDenominator});
    }
    std::sort(signatures.begin(), signatures.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    if (signatures.front().timeSeconds > 0.0) {
        signatures.insert(signatures.begin(), {0.0, project.timeSignatureNumerator, project.timeSignatureDenominator});
    }
    for (size_t index = 1; index < signatures.size();) {
        if (std::abs(signatures[index].timeSeconds - signatures[index - 1].timeSeconds) < 0.000001) {
            signatures[index - 1].numerator = signatures[index].numerator;
            signatures[index - 1].denominator = signatures[index].denominator;
            signatures.erase(signatures.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
    return signatures;
}

uint32_t midiTickForSeconds(const std::vector<TempoMarkerState>& tempos, double seconds, int ticksPerQuarter) {
    const double safeSeconds = std::max(0.0, std::isfinite(seconds) ? seconds : 0.0);
    double ticks = 0.0;
    for (size_t index = 0; index < tempos.size(); ++index) {
        const double segmentStart = tempos[index].timeSeconds;
        const double segmentEnd = index + 1 < tempos.size() ? tempos[index + 1].timeSeconds : safeSeconds;
        if (safeSeconds <= segmentStart) {
            break;
        }
        const double usedEnd = std::min(safeSeconds, segmentEnd);
        if (usedEnd > segmentStart) {
            ticks += (usedEnd - segmentStart) * (std::max(20.0, tempos[index].bpm) / 60.0) * ticksPerQuarter;
        }
        if (safeSeconds <= segmentEnd) {
            break;
        }
    }
    return static_cast<uint32_t>(std::max<long long>(0, std::llround(ticks)));
}

double midiSecondsForTick(const std::vector<TempoMarkerState>& tempos, uint32_t tick, int ticksPerQuarter) {
    if (ticksPerQuarter <= 0) {
        return 0.0;
    }
    const double targetTick = static_cast<double>(tick);
    double elapsedTicks = 0.0;
    double elapsedSeconds = 0.0;
    for (size_t index = 0; index < tempos.size(); ++index) {
        const double bpm = std::max(20.0, std::min(400.0, tempos[index].bpm));
        const double segmentStart = tempos[index].timeSeconds;
        const double segmentEnd = index + 1 < tempos.size() ? tempos[index + 1].timeSeconds : std::numeric_limits<double>::infinity();
        const double segmentSeconds = std::max(0.0, segmentEnd - segmentStart);
        const double segmentTicks = std::isfinite(segmentSeconds) ? segmentSeconds * bpm / 60.0 * ticksPerQuarter : std::numeric_limits<double>::infinity();
        if (targetTick <= elapsedTicks + segmentTicks) {
            return elapsedSeconds + ((targetTick - elapsedTicks) / ticksPerQuarter) * (60.0 / bpm);
        }
        elapsedTicks += segmentTicks;
        elapsedSeconds += segmentSeconds;
    }
    const double bpm = tempos.empty() ? 120.0 : std::max(20.0, std::min(400.0, tempos.back().bpm));
    return elapsedSeconds + ((targetTick - elapsedTicks) / ticksPerQuarter) * (60.0 / bpm);
}

double tempoAtSecondsFromMarkers(const std::vector<TempoMarkerState>& markers,
                                 double seconds,
                                 double fallbackBpm) {
    if (markers.empty()) {
        return fallbackBpm;
    }
    const double safeSeconds = std::max(0.0, std::isfinite(seconds) ? seconds : 0.0);
    const TempoMarkerState* current = nullptr;
    for (const auto& marker : markers) {
        if (marker.timeSeconds <= safeSeconds + 0.0000001) {
            current = &marker;
        } else {
            break;
        }
    }
    return current != nullptr ? current->bpm : markers.front().bpm;
}

double secondsForBeatOffsetFromTempoMap(const std::vector<TempoMarkerState>& markers,
                                        double startSeconds,
                                        double beatOffset,
                                        double fallbackBpm) {
    if (!std::isfinite(startSeconds) || !std::isfinite(beatOffset) || beatOffset <= 0.0) {
        return std::max(0.0, startSeconds);
    }
    double time = std::max(0.0, startSeconds);
    double remainingBeats = beatOffset;
    size_t guard = 0;
    while (remainingBeats > 0.0000001 && guard++ < 10000) {
        const double currentBpm = std::max(20.0, std::min(400.0, tempoAtSecondsFromMarkers(markers, time, fallbackBpm)));
        const TempoMarkerState* right = nullptr;
        for (const auto& marker : markers) {
            if (marker.timeSeconds > time + 0.0000001) {
                right = &marker;
                break;
            }
        }
        const double segmentEnd = right != nullptr ? right->timeSeconds : std::numeric_limits<double>::infinity();
        const double segmentDuration = std::isfinite(segmentEnd) ? std::max(0.0, segmentEnd - time) : std::numeric_limits<double>::infinity();
        const double segmentBeats = std::isfinite(segmentDuration)
            ? std::max(0.0, segmentDuration * currentBpm / 60.0)
            : std::numeric_limits<double>::infinity();
        if (remainingBeats <= segmentBeats + 0.0000001 || !std::isfinite(segmentBeats)) {
            return time + remainingBeats * 60.0 / currentBpm;
        }
        if (!std::isfinite(segmentDuration) || segmentDuration <= 0.0) {
            return time + remainingBeats * 60.0 / currentBpm;
        }
        remainingBeats -= segmentBeats;
        time = segmentEnd;
    }
    return time;
}

bool readU16At(const std::vector<uint8_t>& data, size_t offset, uint16_t& value) {
    if (offset + 2 > data.size()) {
        return false;
    }
    value = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    return true;
}

bool readU32At(const std::vector<uint8_t>& data, size_t offset, uint32_t& value) {
    if (offset + 4 > data.size()) {
        return false;
    }
    value = (static_cast<uint32_t>(data[offset]) << 24) |
        (static_cast<uint32_t>(data[offset + 1]) << 16) |
        (static_cast<uint32_t>(data[offset + 2]) << 8) |
        static_cast<uint32_t>(data[offset + 3]);
    return true;
}

bool readVarLenAt(const std::vector<uint8_t>& data, size_t& offset, size_t end, uint32_t& value) {
    value = 0;
    for (int count = 0; count < 4; ++count) {
        if (offset >= end) {
            return false;
        }
        const uint8_t byte = data[offset++];
        value = (value << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<EdlEvent> timelineEventsForExport(const ProjectDocument& project) {
    std::vector<EdlEvent> events;
    for (const auto& clip : project.clips) {
        if (clip.muted || clip.durationSeconds <= 0.0 || protectedTrackName(clip.trackName)) {
            continue;
        }
        const auto* track = findTrack(project, clip.trackName);
        if (track != nullptr && (track->muted || protectedTrackName(track->name))) {
            continue;
        }
        events.push_back({&clip, track});
    }
    std::sort(events.begin(), events.end(), [](const EdlEvent& left, const EdlEvent& right) {
        if (left.clip->startSeconds != right.clip->startSeconds) {
            return left.clip->startSeconds < right.clip->startSeconds;
        }
        if (left.clip->trackName != right.clip->trackName) {
            return left.clip->trackName < right.clip->trackName;
        }
        return left.clip->id < right.clip->id;
    });
    return events;
}

InterchangeReferenceExportResult exportProjectToReferenceText(const ProjectDocument& project,
                                                              const std::string& formatLabel,
                                                              const std::string& headerToken) {
    InterchangeReferenceExportResult result;
    const auto events = timelineEventsForExport(project);
    const auto projectName = project.name.empty() ? std::string("Untitled") : project.name;

    std::set<std::string> exportedTrackNames;
    for (const auto& event : events) {
        exportedTrackNames.insert(event.track != nullptr ? event.track->name : event.clip->trackName);
    }

    std::ostringstream out;
    out << headerToken << " 1\n";
    out << "NOTE This is a Neuracoust DAW " << formatLabel
        << " reference map for session interchange, not a native binary " << formatLabel << " container.\n";
    out << "PROJECT name=\"" << commentValue(projectName) << "\" sampleRate="
        << fixedNumber(project.sampleRate, 0) << " bitDepth=" << project.bitDepth
        << " timecodeStartSeconds=" << fixedSeconds(project.timecodeStartSeconds)
        << " videoFrameRate=" << fixedNumber(project.videoFrameRate, 3)
        << " timecodeDropFrame=" << (project.timecodeDropFrame ? 1 : 0) << "\n";
    out << "SETTINGS editMode=\"" << commentValue(project.editMode)
        << "\" gridUnit=\"" << commentValue(project.gridUnit)
        << "\" snapQuantumSeconds=" << fixedSeconds(projectTimelineQuantumSeconds(project))
        << " tempoBpm=" << fixedNumber(project.tempoBpm, 3)
        << " timeSignature=\"" << project.timeSignatureNumerator << "/" << project.timeSignatureDenominator << "\""
        << " grooveFeel=\"" << commentValue(project.grooveFeel) << "\""
        << " grooveSwingAmount=" << fixedNumber(project.grooveSwingAmount, 3)
        << " detectedKey=\"" << commentValue(project.detectedKey) << "\""
        << " detectedKeyMode=\"" << commentValue(project.detectedKeyMode) << "\"\n";
    out << "TEMPO_MAP count=" << project.tempoMap.size() << "\n";
    for (size_t index = 0; index < project.tempoMap.size(); ++index) {
        const auto& point = project.tempoMap[index];
        out << "TEMPO index=" << index
            << " timeSeconds=" << fixedSeconds(point.timeSeconds)
            << " bpm=" << fixedNumber(point.bpm, 3) << "\n";
    }
    out << "TIME_SIGNATURE_MAP count=" << project.timeSignatureMap.size() << "\n";
    for (size_t index = 0; index < project.timeSignatureMap.size(); ++index) {
        const auto& signature = project.timeSignatureMap[index];
        out << "TIME_SIGNATURE index=" << index
            << " timeSeconds=" << fixedSeconds(signature.timeSeconds)
            << " signature=\"" << signature.numerator << "/" << signature.denominator << "\"\n";
    }

    out << "CHORD_SECTIONS count=" << project.chordEvents.size() << "\n";
    for (size_t index = 0; index < project.chordEvents.size(); ++index) {
        const auto& chord = project.chordEvents[index];
        out << "CHORD_SECTION index=" << index
            << " id=\"" << commentValue(chord.id) << "\""
            << " name=\"" << commentValue(chord.name) << "\""
            << " timeSeconds=" << fixedSeconds(chord.timeSeconds) << "\n";
    }

    out << "LYRICS count=" << project.lyricEvents.size() << "\n";
    for (size_t index = 0; index < project.lyricEvents.size(); ++index) {
        const auto& lyric = project.lyricEvents[index];
        out << "LYRIC index=" << index
            << " id=\"" << commentValue(lyric.id) << "\""
            << " text=\"" << commentValue(lyric.text) << "\""
            << " timeSeconds=" << fixedSeconds(lyric.timeSeconds) << "\n";
    }

    out << "VIDEO_SOURCES count=" << project.videoSources.size() << "\n";
    for (size_t index = 0; index < project.videoSources.size(); ++index) {
        const auto& source = project.videoSources[index];
        out << "VIDEO_SOURCE index=" << index
            << " id=\"" << commentValue(source.id) << "\""
            << " path=\"" << commentValue(source.path) << "\""
            << " name=\"" << commentValue(source.displayName.empty() ? source.id : source.displayName) << "\""
            << " frameRate=" << fixedNumber(source.frameRate, 3)
            << " durationSeconds=" << fixedSeconds(source.durationSeconds)
            << " width=" << source.width
            << " height=" << source.height
            << " hasAudio=" << (source.hasAudio ? 1 : 0) << "\n";
    }
    out << "VIDEO_CLIPS count=" << project.videoClips.size() << "\n";
    for (size_t index = 0; index < project.videoClips.size(); ++index) {
        const auto& clip = project.videoClips[index];
        out << "VIDEO_CLIP index=" << index
            << " id=\"" << commentValue(clip.id) << "\""
            << " sourceId=\"" << commentValue(clip.sourceId) << "\""
            << " name=\"" << commentValue(clip.name.empty() ? clip.id : clip.name) << "\""
            << " startSeconds=" << fixedSeconds(clip.startSeconds)
            << " durationSeconds=" << fixedSeconds(clip.durationSeconds)
            << " sourceOffsetSeconds=" << fixedSeconds(clip.sourceOffsetSeconds)
            << " sourceTimecodeStartSeconds=" << fixedSeconds(clip.sourceTimecodeStartSeconds)
            << " muted=" << (clip.muted ? 1 : 0)
            << " locked=" << (clip.locked ? 1 : 0) << "\n";
    }

    out << "TRACKS count=" << exportedTrackNames.size() << "\n";
    for (const auto& trackName : exportedTrackNames) {
        const auto* track = findTrack(project, trackName);
        out << "TRACK name=\"" << commentValue(trackName) << "\" type=\""
            << commentValue(track != nullptr ? track->trackType : std::string("audio")) << "\"";
        if (track != nullptr) {
            out << " input=\"" << commentValue(track->inputBus) << "\" output=\""
                << commentValue(track->outputBus) << "\" volumeDb=" << fixedNumber(track->volumeDb, 3)
                << " pan=" << fixedNumber(track->pan, 3)
                << " muted=" << (track->muted ? 1 : 0)
                << " solo=" << (track->solo ? 1 : 0)
                << " recordArmed=" << (track->recordArmed ? 1 : 0);
        }
        out << "\n";
    }

    out << "CLIPS count=" << events.size() << "\n";
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& clip = *events[index].clip;
        const auto* track = events[index].track;
        const auto trackName = track != nullptr ? track->name : clip.trackName;
        const auto clipName = clip.regionName.empty() ? clip.id : clip.regionName;
        out << "CLIP index=" << index
            << " id=\"" << commentValue(clip.id) << "\""
            << " name=\"" << commentValue(clipName) << "\""
            << " track=\"" << commentValue(trackName) << "\""
            << " source=\"" << commentValue(clip.sourcePath) << "\""
            << " sourceUid=\"" << commentValue(clip.sourceFileUid) << "\""
            << " startSeconds=" << fixedSeconds(clip.startSeconds)
            << " endSeconds=" << fixedSeconds(clip.startSeconds + clip.durationSeconds)
            << " durationSeconds=" << fixedSeconds(clip.durationSeconds)
            << " sourceOffsetSeconds=" << fixedSeconds(clip.sourceOffsetSeconds)
            << " sourceHasBroadcastTimeReference=" << (clip.sourceHasBroadcastTimeReference ? 1 : 0)
            << " sourceTimeReferenceSamples=" << clip.sourceTimeReferenceSamples
            << " sourceTimeReferenceSeconds=" << fixedSeconds(clip.sourceTimeReferenceSeconds)
            << " sourceTempoBpm=" << fixedNumber(clip.sourceTempoBpm, 3)
            << " sourceTimeSignature=\"" << clip.sourceTimeSignatureNumerator << "/" << clip.sourceTimeSignatureDenominator << "\""
            << " sourceGrooveFeel=\"" << commentValue(clip.sourceGrooveFeel) << "\""
            << " sourceGrooveSwingAmount=" << fixedNumber(clip.sourceGrooveSwingAmount, 3)
            << " gainDb=" << fixedNumber(clip.gainDb, 3)
            << " fadeInSeconds=" << fixedSeconds(clip.fadeInSeconds)
            << " fadeInCurve=\"" << commentValue(clip.fadeInCurve) << "\""
            << " fadeOutSeconds=" << fixedSeconds(clip.fadeOutSeconds)
            << " fadeOutCurve=\"" << commentValue(clip.fadeOutCurve) << "\""
            << " muted=" << (clip.muted ? 1 : 0)
            << " polarityInverted=" << (clip.polarityInverted ? 1 : 0)
            << " color=\"" << commentValue(clip.colorHex) << "\"\n";
    }

    result.ok = true;
    result.text = out.str();
    result.clipCount = events.size();
    result.trackCount = exportedTrackNames.size();
    result.chordEventCount = project.chordEvents.size();
    result.lyricEventCount = project.lyricEvents.size();
    result.message = formatLabel + " reference export complete with " + std::to_string(result.clipCount) +
        " clip(s), " + std::to_string(result.chordEventCount) + " chord/section event(s), and " +
        std::to_string(result.lyricEventCount) + " lyric event(s).";
    return result;
}

std::string trimReferenceValue(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::map<std::string, std::string> parseReferenceFields(const std::string& line) {
    std::map<std::string, std::string> fields;
    size_t pos = line.find(' ');
    if (pos == std::string::npos) {
        return fields;
    }
    ++pos;
    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        const size_t keyStart = pos;
        while (pos < line.size() && line[pos] != '=' && !std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (pos >= line.size() || line[pos] != '=') {
            break;
        }
        const std::string key = line.substr(keyStart, pos - keyStart);
        ++pos;
        std::string value;
        if (pos < line.size() && line[pos] == '"') {
            ++pos;
            while (pos < line.size() && line[pos] != '"') {
                value.push_back(line[pos++]);
            }
            if (pos < line.size() && line[pos] == '"') {
                ++pos;
            }
        } else {
            const size_t valueStart = pos;
            while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
                ++pos;
            }
            value = line.substr(valueStart, pos - valueStart);
        }
        fields[key] = value == "-" ? std::string{} : trimReferenceValue(value);
    }
    return fields;
}

double referenceDouble(const std::map<std::string, std::string>& fields, const std::string& key, double fallback = 0.0) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) {
        return fallback;
    }
    try {
        return std::stod(it->second);
    } catch (const std::exception&) {
        return fallback;
    }
}

int referenceInt(const std::map<std::string, std::string>& fields, const std::string& key, int fallback = 0) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) {
        return fallback;
    }
    try {
        return std::stoi(it->second);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string referenceString(const std::map<std::string, std::string>& fields,
                            const std::string& key,
                            const std::string& fallback = {}) {
    const auto it = fields.find(key);
    return it == fields.end() ? fallback : it->second;
}

std::string xmlUnescapeString(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t pos = 0; pos < value.size();) {
        if (value[pos] == '&') {
            if (value.compare(pos, 5, "&amp;") == 0) {
                out.push_back('&');
                pos += 5;
                continue;
            }
            if (value.compare(pos, 4, "&lt;") == 0) {
                out.push_back('<');
                pos += 4;
                continue;
            }
            if (value.compare(pos, 4, "&gt;") == 0) {
                out.push_back('>');
                pos += 4;
                continue;
            }
            if (value.compare(pos, 6, "&quot;") == 0) {
                out.push_back('"');
                pos += 6;
                continue;
            }
            if (value.compare(pos, 6, "&apos;") == 0) {
                out.push_back('\'');
                pos += 6;
                continue;
            }
        }
        out.push_back(value[pos++]);
    }
    return out;
}

std::string xmlAttributeValue(const std::string& line, const std::string& attribute) {
    const std::string token = attribute + "=\"";
    const size_t begin = line.find(token);
    if (begin == std::string::npos) {
        return {};
    }
    const size_t valueBegin = begin + token.size();
    const size_t end = line.find('"', valueBegin);
    if (end == std::string::npos) {
        return {};
    }
    return xmlUnescapeString(line.substr(valueBegin, end - valueBegin));
}

double parseFcpxmlSeconds(const std::string& value, double fallback = 0.0) {
    if (value.empty()) {
        return fallback;
    }
    std::string text = value;
    if (!text.empty() && text.back() == 's') {
        text.pop_back();
    }
    const size_t slash = text.find('/');
    try {
        if (slash != std::string::npos) {
            const double numerator = std::stod(text.substr(0, slash));
            const double denominator = std::stod(text.substr(slash + 1));
            return denominator > 0.0 ? numerator / denominator : fallback;
        }
        return std::stod(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::map<std::string, std::string> parseSemicolonFields(const std::string& text) {
    std::map<std::string, std::string> fields;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t next = text.find(';', pos);
        const std::string part = trimReferenceValue(text.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        const size_t equals = part.find('=');
        if (equals != std::string::npos) {
            const auto key = trimReferenceValue(part.substr(0, equals));
            const auto value = trimReferenceValue(part.substr(equals + 1));
            if (!key.empty()) {
                fields[key] = value;
            }
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return fields;
}

std::string noteTextFromLine(const std::string& line) {
    const size_t begin = line.find("<note>");
    const size_t end = line.find("</note>");
    if (begin == std::string::npos || end == std::string::npos || end < begin + 6) {
        return {};
    }
    return xmlUnescapeString(line.substr(begin + 6, end - (begin + 6)));
}

double semicolonDouble(const std::map<std::string, std::string>& fields, const std::string& key, double fallback = 0.0) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) {
        return fallback;
    }
    try {
        return std::stod(it->second);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string semicolonString(const std::map<std::string, std::string>& fields,
                            const std::string& key,
                            const std::string& fallback = {}) {
    const auto it = fields.find(key);
    return it == fields.end() ? fallback : it->second;
}

bool supportedGrooveFeel(const std::string& value) {
    return value.empty() || value == "straight" || value == "shuffle" || value == "triplet";
}

void applySourceTimeSignature(ClipState& clip, const std::string& value) {
    const auto slash = value.find('/');
    if (slash == std::string::npos) {
        return;
    }
    try {
        const auto numerator = std::stoi(value.substr(0, slash));
        const auto denominator = std::stoi(value.substr(slash + 1));
        clip.sourceTimeSignatureNumerator = std::max(0, std::min(64, numerator));
        clip.sourceTimeSignatureDenominator = std::max(0, std::min(64, denominator));
    } catch (const std::exception&) {
    }
}

void applySourceMusicFields(ClipState& clip, const std::map<std::string, std::string>& fields) {
    clip.sourceTempoBpm = semicolonDouble(fields, "tempo", clip.sourceTempoBpm);
    const auto sourceTimeSignature = semicolonString(fields, "timeSignature");
    if (!sourceTimeSignature.empty()) {
        applySourceTimeSignature(clip, sourceTimeSignature);
    }
    auto grooveFeel = semicolonString(fields, "grooveFeel", clip.sourceGrooveFeel);
    if (grooveFeel == "-") {
        grooveFeel.clear();
    }
    clip.sourceGrooveFeel = supportedGrooveFeel(grooveFeel) ? grooveFeel : std::string{};
    clip.sourceGrooveSwingAmount = semicolonDouble(fields, "grooveSwing", clip.sourceGrooveSwingAmount);
    clip.sourceGrooveSwingAmount = semicolonDouble(fields, "sourceGrooveSwingAmount", clip.sourceGrooveSwingAmount);
}

void sortTimelineMetadata(ProjectDocument& project) {
    std::sort(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    std::sort(project.timeSignatureMap.begin(), project.timeSignatureMap.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    std::sort(project.markers.begin(), project.markers.end(), [](const MarkerState& left, const MarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    std::sort(project.chordEvents.begin(), project.chordEvents.end(), [](const ChordEventState& left, const ChordEventState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    std::sort(project.lyricEvents.begin(), project.lyricEvents.end(), [](const LyricEventState& left, const LyricEventState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
}

InterchangeReferenceImportResult importProjectFromFcpxmlSubsetText(const std::string& text) {
    InterchangeReferenceImportResult result;
    if (text.find("<fcpxml") == std::string::npos) {
        result.message = "FCPXML import failed: unsupported document.";
        return result;
    }

    ProjectDocument project = defaultProject();
    project.tracks.clear();
    project.clips.clear();
    project.tempoMap.clear();
    project.timeSignatureMap.clear();
    project.markers.clear();
    project.chordEvents.clear();
    project.lyricEvents.clear();

    struct Asset {
        std::string sourcePath;
        double durationSeconds = 0.0;
        double sampleRate = 0.0;
        int channels = 0;
    };
    std::map<std::string, Asset> assetsById;
    std::set<std::string> importedTrackNames;
    ClipState pendingClip;
    bool hasPendingClip = false;

    auto ensureTrack = [&](const std::string& name, const std::string& type) {
        if (name.empty() || importedTrackNames.find(name) != importedTrackNames.end()) {
            return;
        }
        TrackState track;
        track.name = name;
        track.trackType = type.empty() ? "audio" : type;
        project.tracks.push_back(track);
        importedTrackNames.insert(name);
    };

    auto finishPendingClip = [&]() {
        if (!hasPendingClip) {
            return;
        }
        if (pendingClip.id.empty()) {
            pendingClip.id = "fcpxml-clip-" + std::to_string(project.clips.size() + 1);
        }
        if (pendingClip.regionName.empty()) {
            pendingClip.regionName = pendingClip.id;
        }
        if (pendingClip.trackName.empty()) {
            pendingClip.trackName = "Audio 1";
        }
        if (pendingClip.sourceFileUid.empty() && !pendingClip.sourcePath.empty()) {
            pendingClip.sourceFileUid = "fcpxml-src-" + std::to_string(project.clips.size() + 1);
        }
        ensureTrack(pendingClip.trackName, "audio");
        if (pendingClip.durationSeconds > 0.0 && !pendingClip.sourcePath.empty()) {
            project.clips.push_back(pendingClip);
        }
        pendingClip = {};
        hasPendingClip = false;
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("<project ") != std::string::npos) {
            const auto name = xmlAttributeValue(line, "name");
            if (!name.empty()) {
                project.name = name;
            }
        } else if (line.find("<sequence ") != std::string::npos) {
            project.timecodeStartSeconds = parseFcpxmlSeconds(xmlAttributeValue(line, "tcStart"), project.timecodeStartSeconds);
            const auto tcFormat = xmlAttributeValue(line, "tcFormat");
            if (tcFormat == "DF") {
                project.timecodeDropFrame = true;
            } else if (tcFormat == "NDF") {
                project.timecodeDropFrame = false;
            }
            const auto rate = xmlAttributeValue(line, "audioRate");
            if (!rate.empty()) {
                try {
                    project.sampleRate = std::stod(rate);
                } catch (const std::exception&) {
                }
            }
        } else if (line.find("<asset ") != std::string::npos) {
            Asset asset;
            const auto id = xmlAttributeValue(line, "id");
            asset.sourcePath = xmlAttributeValue(line, "src");
            if (asset.sourcePath.rfind("file://", 0) == 0) {
                asset.sourcePath.erase(0, 7);
            }
            asset.durationSeconds = parseFcpxmlSeconds(xmlAttributeValue(line, "duration"), 0.0);
            asset.sampleRate = semicolonDouble({{"rate", xmlAttributeValue(line, "audioRate")}}, "rate", project.sampleRate);
            try {
                asset.channels = std::stoi(xmlAttributeValue(line, "audioChannels"));
            } catch (const std::exception&) {
                asset.channels = 0;
            }
            if (!id.empty()) {
                assetsById[id] = asset;
            }
        } else if (line.find("<asset-clip ") != std::string::npos) {
            finishPendingClip();
            const auto ref = xmlAttributeValue(line, "ref");
            const auto assetIt = assetsById.find(ref);
            pendingClip = {};
            pendingClip.regionName = xmlAttributeValue(line, "name");
            pendingClip.sourcePath = assetIt != assetsById.end() ? assetIt->second.sourcePath : std::string{};
            pendingClip.sourceSampleRate = assetIt != assetsById.end() ? assetIt->second.sampleRate : project.sampleRate;
            pendingClip.sourceChannels = assetIt != assetsById.end() ? assetIt->second.channels : 0;
            pendingClip.startSeconds = parseFcpxmlSeconds(xmlAttributeValue(line, "offset"), 0.0);
            pendingClip.sourceOffsetSeconds = parseFcpxmlSeconds(xmlAttributeValue(line, "start"), 0.0);
            pendingClip.durationSeconds = parseFcpxmlSeconds(xmlAttributeValue(line, "duration"), 0.0);
            pendingClip.fadeInCurve = "linear";
            pendingClip.fadeOutCurve = "linear";
            hasPendingClip = true;
        } else if (hasPendingClip && line.find("<note>") != std::string::npos) {
            const auto fields = parseSemicolonFields(noteTextFromLine(line));
            pendingClip.trackName = semicolonString(fields, "track", pendingClip.trackName);
            pendingClip.id = semicolonString(fields, "clipId", pendingClip.id);
            pendingClip.sourceFileUid = semicolonString(fields, "sourceUid", pendingClip.sourceFileUid);
            pendingClip.sourceTimeReferenceSamples = static_cast<uint64_t>(std::max(0.0, semicolonDouble(fields, "bwfTimeRefSamples", 0.0)));
            pendingClip.sourceTimeReferenceSeconds = semicolonDouble(fields, "bwfTimeRefSeconds", 0.0);
            pendingClip.sourceHasBroadcastTimeReference = pendingClip.sourceTimeReferenceSamples > 0 || pendingClip.sourceTimeReferenceSeconds > 0.0;
            applySourceMusicFields(pendingClip, {
                {"tempo", semicolonString(fields, "sourceTempoBpm")},
                {"timeSignature", semicolonString(fields, "sourceTimeSignature")},
                {"grooveFeel", semicolonString(fields, "sourceGrooveFeel")},
                {"sourceGrooveSwingAmount", semicolonString(fields, "sourceGrooveSwingAmount")}
            });
            pendingClip.gainDb = static_cast<float>(semicolonDouble(fields, "gainDb", pendingClip.gainDb));
            pendingClip.colorHex = semicolonString(fields, "color", pendingClip.colorHex);
            const auto fadeIn = semicolonString(fields, "fadeIn");
            if (!fadeIn.empty()) {
                std::istringstream fade(fadeIn);
                fade >> pendingClip.fadeInSeconds >> pendingClip.fadeInCurve;
            }
            const auto fadeOut = semicolonString(fields, "fadeOut");
            if (!fadeOut.empty()) {
                std::istringstream fade(fadeOut);
                fade >> pendingClip.fadeOutSeconds >> pendingClip.fadeOutCurve;
            }
            ensureTrack(pendingClip.trackName, semicolonString(fields, "trackType", "audio"));
        } else if (hasPendingClip && line.find("</asset-clip>") != std::string::npos) {
            finishPendingClip();
        } else if (line.find("<md ") != std::string::npos) {
            const auto key = xmlAttributeValue(line, "key");
            const auto value = xmlAttributeValue(line, "value");
            if (key == "com.neuracoust.daw.sampleRate") {
                project.sampleRate = semicolonDouble({{"value", value}}, "value", project.sampleRate);
            } else if (key == "com.neuracoust.daw.bitDepth") {
                try {
                    project.bitDepth = std::stoi(value);
                } catch (const std::exception&) {
                }
            } else if (key == "com.neuracoust.daw.editMode") {
                project.editMode = value.empty() ? project.editMode : value;
            } else if (key == "com.neuracoust.daw.gridUnit") {
                project.gridUnit = value.empty() ? project.gridUnit : value;
            } else if (key == "com.neuracoust.daw.tempoBpm") {
                try {
                    project.tempoBpm = std::stoi(value);
                } catch (const std::exception&) {
                }
            } else if (key == "com.neuracoust.daw.timeSignature") {
                const auto slash = value.find('/');
                if (slash != std::string::npos) {
                    try {
                        project.timeSignatureNumerator = std::max(1, std::min(16, std::stoi(value.substr(0, slash))));
                        project.timeSignatureDenominator = std::max(1, std::min(32, std::stoi(value.substr(slash + 1))));
                    } catch (const std::exception&) {
                    }
                }
            } else if (key == "com.neuracoust.daw.grooveFeel") {
                project.grooveFeel = value.empty() ? project.grooveFeel : value;
            } else if (key == "com.neuracoust.daw.grooveSwingAmount") {
                project.grooveSwingAmount = semicolonDouble({{"value", value}}, "value", project.grooveSwingAmount);
            } else if (key == "com.neuracoust.daw.detectedKey") {
                project.detectedKey = value.empty() ? project.detectedKey : value;
            } else if (key == "com.neuracoust.daw.detectedKeyMode") {
                project.detectedKeyMode = value.empty() ? project.detectedKeyMode : value;
            } else if (key == "com.neuracoust.daw.videoFrameRate") {
                project.videoFrameRate = semicolonDouble({{"value", value}}, "value", project.videoFrameRate);
            } else if (key == "com.neuracoust.daw.timecodeDropFrame") {
                project.timecodeDropFrame = value == "1" || value == "true" || value == "YES";
            } else if (key.rfind("com.neuracoust.daw.videoSource.", 0) == 0) {
                const auto fields = parseSemicolonFields(value);
                VideoSourceState source;
                source.id = semicolonString(fields, "id");
                source.path = semicolonString(fields, "path");
                source.displayName = semicolonString(fields, "name", source.id);
                source.frameRate = semicolonDouble(fields, "frameRate", project.videoFrameRate);
                source.durationSeconds = semicolonDouble(fields, "duration", 0.0);
                const auto sizeValue = semicolonString(fields, "size");
                const auto xPos = sizeValue.find('x');
                if (xPos != std::string::npos) {
                    try {
                        source.width = std::max(0, std::stoi(sizeValue.substr(0, xPos)));
                        source.height = std::max(0, std::stoi(sizeValue.substr(xPos + 1)));
                    } catch (const std::exception&) {
                    }
                }
                source.hasAudio = semicolonString(fields, "hasAudio") == "1";
                if (!source.id.empty()) {
                    project.videoSources.push_back(source);
                }
            } else if (key.rfind("com.neuracoust.daw.videoClip.", 0) == 0) {
                const auto fields = parseSemicolonFields(value);
                VideoClipState clip;
                clip.id = semicolonString(fields, "id");
                clip.sourceId = semicolonString(fields, "sourceId");
                clip.name = semicolonString(fields, "name", clip.id);
                clip.startSeconds = semicolonDouble(fields, "start", 0.0);
                clip.durationSeconds = semicolonDouble(fields, "duration", 0.0);
                clip.sourceOffsetSeconds = semicolonDouble(fields, "sourceOffset", 0.0);
                clip.sourceTimecodeStartSeconds = semicolonDouble(fields, "sourceTimecodeStart", project.timecodeStartSeconds);
                clip.muted = semicolonString(fields, "muted") == "1";
                clip.locked = semicolonString(fields, "locked") == "1";
                const bool sourceExists = std::any_of(project.videoSources.begin(), project.videoSources.end(), [&](const VideoSourceState& source) {
                    return source.id == clip.sourceId;
                });
                if (!clip.id.empty() && sourceExists && clip.durationSeconds > 0.0) {
                    project.videoClips.push_back(clip);
                }
            } else if (key.rfind("com.neuracoust.daw.tempo.", 0) == 0) {
                const auto fields = parseSemicolonFields(value);
                TempoMarkerState tempo;
                tempo.timeSeconds = semicolonDouble(fields, "time", 0.0);
                tempo.bpm = semicolonDouble(fields, "bpm", project.tempoBpm);
                if (std::isfinite(tempo.timeSeconds) && std::isfinite(tempo.bpm) && tempo.timeSeconds >= 0.0 && tempo.bpm > 0.0) {
                    project.tempoMap.push_back(tempo);
                }
            } else if (key.rfind("com.neuracoust.daw.timeSignatureMarker.", 0) == 0) {
                const auto fields = parseSemicolonFields(value);
                TimeSignatureMarkerState signature;
                signature.timeSeconds = semicolonDouble(fields, "time", 0.0);
                const auto signatureValue = semicolonString(fields, "signature");
                const auto slash = signatureValue.find('/');
                if (slash != std::string::npos) {
                    try {
                        signature.numerator = std::max(1, std::min(16, std::stoi(signatureValue.substr(0, slash))));
                        signature.denominator = std::max(1, std::min(32, std::stoi(signatureValue.substr(slash + 1))));
                    } catch (const std::exception&) {
                    }
                }
                if (std::isfinite(signature.timeSeconds) && signature.timeSeconds >= 0.0) {
                    project.timeSignatureMap.push_back(signature);
                }
            } else if (key.rfind("com.neuracoust.daw.marker.", 0) == 0) {
                const auto fields = parseSemicolonFields(value);
                MarkerState marker;
                marker.id = semicolonString(fields, "id", "marker-" + std::to_string(project.markers.size() + 1));
                marker.name = semicolonString(fields, "name", marker.id);
                marker.timeSeconds = semicolonDouble(fields, "time", 0.0);
                if (std::isfinite(marker.timeSeconds) && marker.timeSeconds >= 0.0) {
                    project.markers.push_back(marker);
                }
            } else if (key.rfind("com.neuracoust.daw.chordSection.", 0) == 0) {
                const auto fields = parseSemicolonFields(value);
                ChordEventState chord;
                chord.id = semicolonString(fields, "id", "chord-" + std::to_string(project.chordEvents.size() + 1));
                chord.name = semicolonString(fields, "name", chord.id);
                chord.timeSeconds = semicolonDouble(fields, "time", 0.0);
                if (std::isfinite(chord.timeSeconds) && chord.timeSeconds >= 0.0) {
                    project.chordEvents.push_back(chord);
                }
            } else if (key.rfind("com.neuracoust.daw.lyric.", 0) == 0) {
                const auto fields = parseSemicolonFields(value);
                LyricEventState lyric;
                lyric.id = semicolonString(fields, "id", "lyric-" + std::to_string(project.lyricEvents.size() + 1));
                lyric.text = semicolonString(fields, "text", lyric.id);
                lyric.timeSeconds = semicolonDouble(fields, "time", 0.0);
                if (std::isfinite(lyric.timeSeconds) && lyric.timeSeconds >= 0.0) {
                    project.lyricEvents.push_back(lyric);
                }
            }
        }
    }
    finishPendingClip();

    if (project.tempoMap.empty()) {
        project.tempoMap.push_back({0.0, static_cast<double>(std::max(20, std::min(400, project.tempoBpm)))});
    }
    if (project.tracks.empty()) {
        project.tracks = defaultProject().tracks;
    }
    if (project.timeSignatureMap.empty()) {
        project.timeSignatureMap.push_back({0.0, project.timeSignatureNumerator, project.timeSignatureDenominator});
    }
    normalizeProjectRouting(project);
    sortTimelineMetadata(project);

    result.ok = !project.clips.empty() || !project.markers.empty() || !project.chordEvents.empty() || !project.lyricEvents.empty();
    result.project = std::move(project);
    result.clipCount = result.project.clips.size();
    result.trackCount = result.project.tracks.size();
    result.chordEventCount = result.project.chordEvents.size();
    result.lyricEventCount = result.project.lyricEvents.size();
    result.message = result.ok
        ? "FCPXML import complete with " + std::to_string(result.clipCount) +
            " clip(s), " + std::to_string(result.chordEventCount) + " chord/section event(s), and " +
            std::to_string(result.lyricEventCount) + " lyric event(s)."
        : "FCPXML import failed: no supported clips or timeline metadata found.";
    return result;
}

InterchangeReferenceImportResult importProjectFromCmx3600EdlSubsetText(const std::string& text, double frameRate) {
    InterchangeReferenceImportResult result;
    if (!std::isfinite(frameRate) || frameRate < 1.0 || frameRate > 240.0) {
        result.message = "EDL import failed: invalid frame rate.";
        return result;
    }
    if (text.find("TITLE:") == std::string::npos || text.find("FCM:") == std::string::npos) {
        result.message = "EDL import failed: unsupported document.";
        return result;
    }

    ProjectDocument project = defaultProject();
    project.tracks.clear();
    project.clips.clear();
    project.tempoMap.clear();
    project.timeSignatureMap.clear();
    project.markers.clear();
    project.chordEvents.clear();

    std::set<std::string> importedTrackNames;
    ClipState pendingClip;
    bool hasPendingClip = false;
    double pendingRecordInSeconds = 0.0;
    bool explicitTimecodeStart = false;
    std::vector<double> recordInSeconds;

    auto ensureTrack = [&](const std::string& name, const std::string& type) {
        if (name.empty() || importedTrackNames.find(name) != importedTrackNames.end()) {
            return;
        }
        TrackState track;
        track.name = name;
        track.trackType = type.empty() ? "audio" : type;
        project.tracks.push_back(track);
        importedTrackNames.insert(name);
    };

    auto finishPendingClip = [&]() {
        if (!hasPendingClip) {
            return;
        }
        if (pendingClip.id.empty()) {
            pendingClip.id = "edl-clip-" + std::to_string(project.clips.size() + 1);
        }
        if (pendingClip.regionName.empty()) {
            pendingClip.regionName = pendingClip.id;
        }
        if (pendingClip.trackName.empty()) {
            pendingClip.trackName = "Audio 1";
        }
        if (pendingClip.sourceFileUid.empty() && !pendingClip.sourcePath.empty()) {
            pendingClip.sourceFileUid = "edl-src-" + std::to_string(project.clips.size() + 1);
        }
        ensureTrack(pendingClip.trackName, "audio");
        if (pendingClip.durationSeconds > 0.0) {
            if (explicitTimecodeStart) {
                pendingClip.startSeconds = std::max(0.0, pendingRecordInSeconds - project.timecodeStartSeconds);
            }
            project.clips.push_back(pendingClip);
        }
        pendingClip = {};
        hasPendingClip = false;
        pendingRecordInSeconds = 0.0;
    };

    auto commentAfter = [](const std::string& line, const std::string& prefix) -> std::string {
        return trimReferenceValue(line.substr(prefix.size()));
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trimReferenceValue(line);
        if (line.empty()) {
            continue;
        }
        if (line.rfind("TITLE:", 0) == 0) {
            project.name = commentAfter(line, "TITLE:");
        } else if (line.rfind("FCM:", 0) == 0) {
            const auto value = lowerCopy(commentAfter(line, "FCM:"));
            project.timecodeDropFrame = value.find("drop") != std::string::npos && value.find("non") == std::string::npos;
        } else if (line.rfind("* SAMPLE_RATE:", 0) == 0) {
            project.sampleRate = referenceDouble({{"value", commentAfter(line, "* SAMPLE_RATE:")}}, "value", project.sampleRate);
        } else if (line.rfind("* BIT_DEPTH:", 0) == 0) {
            project.bitDepth = referenceInt({{"value", commentAfter(line, "* BIT_DEPTH:")}}, "value", project.bitDepth);
        } else if (line.rfind("* TIMECODE_START_SECONDS:", 0) == 0) {
            project.timecodeStartSeconds = referenceDouble({{"value", commentAfter(line, "* TIMECODE_START_SECONDS:")}}, "value", project.timecodeStartSeconds);
            explicitTimecodeStart = true;
        } else if (line.rfind("* EDIT_MODE:", 0) == 0) {
            project.editMode = commentAfter(line, "* EDIT_MODE:");
        } else if (line.rfind("* GRID_UNIT:", 0) == 0) {
            project.gridUnit = commentAfter(line, "* GRID_UNIT:");
        } else if (line.rfind("* TIME_SIGNATURE:", 0) == 0) {
            const auto value = commentAfter(line, "* TIME_SIGNATURE:");
            const auto slash = value.find('/');
            if (slash != std::string::npos) {
                try {
                    project.timeSignatureNumerator = std::max(1, std::min(16, std::stoi(value.substr(0, slash))));
                    project.timeSignatureDenominator = std::max(1, std::min(32, std::stoi(value.substr(slash + 1))));
                } catch (const std::exception&) {
                }
            }
        } else if (line.rfind("* GROOVE:", 0) == 0) {
            const auto fields = parseSemicolonFields(commentAfter(line, "* GROOVE:"));
            project.grooveFeel = semicolonString(fields, "feel", project.grooveFeel);
            project.grooveSwingAmount = semicolonDouble(fields, "swing", project.grooveSwingAmount);
        } else if (line.rfind("* KEY:", 0) == 0) {
            const auto fields = parseSemicolonFields(commentAfter(line, "* KEY:"));
            project.detectedKey = semicolonString(fields, "root", project.detectedKey);
            project.detectedKeyMode = semicolonString(fields, "mode", project.detectedKeyMode);
        } else if (line.rfind("* TEMPO:", 0) == 0) {
            const auto value = commentAfter(line, "* TEMPO:");
            std::istringstream tempoIn(value);
            std::string timeToken;
            double bpm = 0.0;
            tempoIn >> timeToken >> bpm;
            if (!timeToken.empty() && timeToken.back() == 's') {
                timeToken.pop_back();
            }
            TempoMarkerState tempo;
            tempo.timeSeconds = referenceDouble({{"time", timeToken}}, "time", 0.0);
            tempo.bpm = bpm > 0.0 ? bpm : static_cast<double>(project.tempoBpm);
            if (std::isfinite(tempo.timeSeconds) && std::isfinite(tempo.bpm) && tempo.timeSeconds >= 0.0 && tempo.bpm > 0.0) {
                project.tempoMap.push_back(tempo);
            }
        } else if (line.rfind("* TIME_SIGNATURE_MARKER:", 0) == 0) {
            const auto value = commentAfter(line, "* TIME_SIGNATURE_MARKER:");
            std::istringstream signatureIn(value);
            std::string timeToken;
            std::string signatureToken;
            signatureIn >> timeToken >> signatureToken;
            if (!timeToken.empty() && timeToken.back() == 's') {
                timeToken.pop_back();
            }
            TimeSignatureMarkerState signature;
            signature.timeSeconds = referenceDouble({{"time", timeToken}}, "time", 0.0);
            const auto slash = signatureToken.find('/');
            if (slash != std::string::npos) {
                try {
                    signature.numerator = std::max(1, std::min(16, std::stoi(signatureToken.substr(0, slash))));
                    signature.denominator = std::max(1, std::min(32, std::stoi(signatureToken.substr(slash + 1))));
                } catch (const std::exception&) {
                }
            }
            if (std::isfinite(signature.timeSeconds) && signature.timeSeconds >= 0.0) {
                project.timeSignatureMap.push_back(signature);
            }
        } else if (line.rfind("* MARKER:", 0) == 0) {
            const auto value = commentAfter(line, "* MARKER:");
            const size_t sPos = value.find('s');
            MarkerState marker;
            marker.id = "edl-marker-" + std::to_string(project.markers.size() + 1);
            if (sPos != std::string::npos) {
                marker.timeSeconds = referenceDouble({{"time", value.substr(0, sPos)}}, "time", 0.0);
                marker.name = trimReferenceValue(value.substr(sPos + 1));
            }
            if (marker.name.empty()) {
                marker.name = marker.id;
            }
            if (std::isfinite(marker.timeSeconds) && marker.timeSeconds >= 0.0) {
                project.markers.push_back(marker);
            }
        } else if (line.rfind("* CHORD_SECTION:", 0) == 0) {
            const auto value = commentAfter(line, "* CHORD_SECTION:");
            const size_t sPos = value.find('s');
            ChordEventState chord;
            chord.id = "edl-chord-" + std::to_string(project.chordEvents.size() + 1);
            if (sPos != std::string::npos) {
                chord.timeSeconds = referenceDouble({{"time", value.substr(0, sPos)}}, "time", 0.0);
                chord.name = trimReferenceValue(value.substr(sPos + 1));
            }
            if (chord.name.empty()) {
                chord.name = chord.id;
            }
            if (std::isfinite(chord.timeSeconds) && chord.timeSeconds >= 0.0) {
                project.chordEvents.push_back(chord);
            }
        } else if (line.rfind("* LYRIC:", 0) == 0) {
            const auto value = commentAfter(line, "* LYRIC:");
            const size_t sPos = value.find('s');
            LyricEventState lyric;
            lyric.id = "edl-lyric-" + std::to_string(project.lyricEvents.size() + 1);
            if (sPos != std::string::npos) {
                lyric.timeSeconds = referenceDouble({{"time", value.substr(0, sPos)}}, "time", 0.0);
                lyric.text = trimReferenceValue(value.substr(sPos + 1));
            }
            if (lyric.text.empty()) {
                lyric.text = lyric.id;
            }
            if (std::isfinite(lyric.timeSeconds) && lyric.timeSeconds >= 0.0) {
                project.lyricEvents.push_back(lyric);
            }
        } else if (line.rfind("* VIDEO_SOURCE:", 0) == 0) {
            const auto fields = parseSemicolonFields(commentAfter(line, "* VIDEO_SOURCE:"));
            VideoSourceState source;
            source.id = semicolonString(fields, "id");
            source.path = semicolonString(fields, "path");
            source.displayName = semicolonString(fields, "name", source.id);
            source.frameRate = semicolonDouble(fields, "frameRate", project.videoFrameRate);
            source.durationSeconds = semicolonDouble(fields, "duration", 0.0);
            const auto sizeValue = semicolonString(fields, "size");
            const auto xPos = sizeValue.find('x');
            if (xPos != std::string::npos) {
                try {
                    source.width = std::max(0, std::stoi(sizeValue.substr(0, xPos)));
                    source.height = std::max(0, std::stoi(sizeValue.substr(xPos + 1)));
                } catch (const std::exception&) {
                }
            }
            source.hasAudio = semicolonString(fields, "hasAudio") == "1";
            if (!source.id.empty()) {
                project.videoSources.push_back(source);
            }
        } else if (line.rfind("* VIDEO_CLIP:", 0) == 0) {
            const auto fields = parseSemicolonFields(commentAfter(line, "* VIDEO_CLIP:"));
            VideoClipState clip;
            clip.id = semicolonString(fields, "id");
            clip.sourceId = semicolonString(fields, "sourceId");
            clip.name = semicolonString(fields, "name", clip.id);
            clip.startSeconds = semicolonDouble(fields, "start", 0.0);
            clip.durationSeconds = semicolonDouble(fields, "duration", 0.0);
            clip.sourceOffsetSeconds = semicolonDouble(fields, "sourceOffset", 0.0);
            clip.sourceTimecodeStartSeconds = semicolonDouble(fields, "sourceTimecodeStart", project.timecodeStartSeconds);
            clip.muted = semicolonString(fields, "muted") == "1";
            clip.locked = semicolonString(fields, "locked") == "1";
            const bool sourceExists = std::any_of(project.videoSources.begin(), project.videoSources.end(), [&](const VideoSourceState& source) {
                return source.id == clip.sourceId;
            });
            if (!clip.id.empty() && sourceExists && clip.durationSeconds > 0.0) {
                project.videoClips.push_back(clip);
            }
        } else if (line.rfind("* FROM CLIP NAME:", 0) == 0 && hasPendingClip) {
            pendingClip.regionName = commentAfter(line, "* FROM CLIP NAME:");
        } else if (line.rfind("* SOURCE FILE:", 0) == 0 && hasPendingClip) {
            pendingClip.sourcePath = commentAfter(line, "* SOURCE FILE:");
        } else if (line.rfind("* SOURCE UID:", 0) == 0 && hasPendingClip) {
            pendingClip.sourceFileUid = commentAfter(line, "* SOURCE UID:");
        } else if (line.rfind("* BWF_TIME_REFERENCE:", 0) == 0 && hasPendingClip) {
            const auto value = commentAfter(line, "* BWF_TIME_REFERENCE:");
            std::istringstream bwfIn(value);
            std::string samplesToken;
            std::string samplesLabel;
            std::string secondsToken;
            bwfIn >> samplesToken >> samplesLabel >> secondsToken;
            pendingClip.sourceTimeReferenceSamples = static_cast<uint64_t>(
                std::max(0.0, referenceDouble({{"value", samplesToken}}, "value", 0.0)));
            if (!secondsToken.empty() && secondsToken.back() == 's') {
                secondsToken.pop_back();
            }
            pendingClip.sourceTimeReferenceSeconds = referenceDouble({{"value", secondsToken}}, "value", 0.0);
            pendingClip.sourceHasBroadcastTimeReference =
                pendingClip.sourceTimeReferenceSamples > 0 || pendingClip.sourceTimeReferenceSeconds > 0.0;
        } else if (line.rfind("* SOURCE_MUSIC:", 0) == 0 && hasPendingClip) {
            applySourceMusicFields(pendingClip, parseSemicolonFields(commentAfter(line, "* SOURCE_MUSIC:")));
        } else if (line.rfind("* TRACK:", 0) == 0 && hasPendingClip) {
            const auto value = commentAfter(line, "* TRACK:");
            const size_t typeBegin = value.rfind('(');
            const size_t typeEnd = value.rfind(')');
            if (typeBegin != std::string::npos && typeEnd != std::string::npos && typeEnd > typeBegin) {
                pendingClip.trackName = trimReferenceValue(value.substr(0, typeBegin));
                ensureTrack(pendingClip.trackName, trimReferenceValue(value.substr(typeBegin + 1, typeEnd - typeBegin - 1)));
            } else {
                pendingClip.trackName = value;
                ensureTrack(pendingClip.trackName, "audio");
            }
        } else if (line.rfind("* GAIN_DB:", 0) == 0 && hasPendingClip) {
            pendingClip.gainDb = static_cast<float>(referenceDouble({{"value", commentAfter(line, "* GAIN_DB:")}}, "value", pendingClip.gainDb));
        } else if (line.rfind("* FADES:", 0) == 0 && hasPendingClip) {
            const auto value = commentAfter(line, "* FADES:");
            std::istringstream fades(value);
            std::string inLabel;
            std::string outSlash;
            std::string outLabel;
            std::string fadeInSeconds;
            std::string fadeOutSeconds;
            fades >> inLabel >> fadeInSeconds >> pendingClip.fadeInCurve >> outSlash >> outLabel >> fadeOutSeconds >> pendingClip.fadeOutCurve;
            if (!fadeInSeconds.empty() && fadeInSeconds.back() == 's') {
                fadeInSeconds.pop_back();
            }
            if (!fadeOutSeconds.empty() && fadeOutSeconds.back() == 's') {
                fadeOutSeconds.pop_back();
            }
            pendingClip.fadeInSeconds = referenceDouble({{"value", fadeInSeconds}}, "value", pendingClip.fadeInSeconds);
            pendingClip.fadeOutSeconds = referenceDouble({{"value", fadeOutSeconds}}, "value", pendingClip.fadeOutSeconds);
        } else if (std::isdigit(static_cast<unsigned char>(line.front()))) {
            finishPendingClip();
            std::istringstream eventIn(line);
            std::string eventNumber;
            std::string reel;
            std::string channel;
            std::string transition;
            std::string sourceIn;
            std::string sourceOut;
            std::string recordIn;
            std::string recordOut;
            eventIn >> eventNumber >> reel >> channel >> transition >> sourceIn >> sourceOut >> recordIn >> recordOut;
            if (!eventNumber.empty() && !sourceIn.empty() && !sourceOut.empty() && !recordIn.empty() && !recordOut.empty()) {
                const double sourceInSeconds = secondsFromTimecode(sourceIn, frameRate, 0.0);
                const double sourceOutSeconds = secondsFromTimecode(sourceOut, frameRate, sourceInSeconds);
                const double recordInValue = secondsFromTimecode(recordIn, frameRate, 0.0);
                const double recordOutValue = secondsFromTimecode(recordOut, frameRate, recordInValue);
                pendingClip = {};
                pendingClip.id = "edl-clip-" + eventNumber;
                pendingClip.regionName = reel;
                pendingClip.trackName = "Audio 1";
                pendingClip.sourceOffsetSeconds = sourceInSeconds;
                pendingClip.durationSeconds = std::max(0.0, std::min(sourceOutSeconds - sourceInSeconds, recordOutValue - recordInValue));
                pendingClip.startSeconds = std::max(0.0, explicitTimecodeStart ? recordInValue - project.timecodeStartSeconds : recordInValue);
                pendingClip.fadeInCurve = "linear";
                pendingClip.fadeOutCurve = "linear";
                pendingRecordInSeconds = recordInValue;
                recordInSeconds.push_back(recordInValue);
                hasPendingClip = true;
            }
        }
    }
    finishPendingClip();

    if (!explicitTimecodeStart && !recordInSeconds.empty()) {
        const double inferredStart = *std::min_element(recordInSeconds.begin(), recordInSeconds.end());
        project.timecodeStartSeconds = inferredStart >= 3600.0 ? std::floor(inferredStart / 3600.0) * 3600.0 : 0.0;
        for (auto& clip : project.clips) {
            clip.startSeconds = std::max(0.0, clip.startSeconds - project.timecodeStartSeconds);
        }
    }
    if (project.tempoMap.empty()) {
        project.tempoMap.push_back({0.0, static_cast<double>(std::max(20, std::min(400, project.tempoBpm)))});
    } else {
        project.tempoBpm = static_cast<int>(std::round(project.tempoMap.front().bpm));
    }
    if (project.tracks.empty()) {
        project.tracks = defaultProject().tracks;
    }
    if (project.timeSignatureMap.empty()) {
        project.timeSignatureMap.push_back({0.0, project.timeSignatureNumerator, project.timeSignatureDenominator});
    }
    normalizeProjectRouting(project);
    sortTimelineMetadata(project);

    result.ok = !project.clips.empty() || !project.markers.empty() || !project.chordEvents.empty() || !project.lyricEvents.empty();
    result.project = std::move(project);
    result.clipCount = result.project.clips.size();
    result.trackCount = result.project.tracks.size();
    result.chordEventCount = result.project.chordEvents.size();
    result.lyricEventCount = result.project.lyricEvents.size();
    result.message = result.ok
        ? "EDL import complete with " + std::to_string(result.clipCount) +
            " clip(s), " + std::to_string(result.chordEventCount) + " chord/section event(s), and " +
            std::to_string(result.lyricEventCount) + " lyric event(s)."
        : "EDL import failed: no supported events found.";
    return result;
}

InterchangeReferenceImportResult importProjectFromReferenceText(const std::string& text,
                                                                const std::string& headerToken,
                                                                const std::string& formatLabel) {
    InterchangeReferenceImportResult result;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind(headerToken + " ", 0) != 0) {
        result.message = formatLabel + " reference import failed: unsupported header.";
        return result;
    }

    ProjectDocument project = defaultProject();
    project.tracks.clear();
    project.clips.clear();
    project.tempoMap.clear();
    project.timeSignatureMap.clear();
    project.videoSources.clear();
    project.videoClips.clear();
    project.chordEvents.clear();
    project.lyricEvents.clear();

    std::set<std::string> importedTrackNames;
    while (std::getline(in, line)) {
        if (line.rfind("PROJECT ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            project.name = referenceString(fields, "name", project.name);
            project.sampleRate = referenceDouble(fields, "sampleRate", project.sampleRate);
            project.bitDepth = referenceInt(fields, "bitDepth", project.bitDepth);
            project.timecodeStartSeconds = referenceDouble(fields, "timecodeStartSeconds", project.timecodeStartSeconds);
            project.videoFrameRate = referenceDouble(fields, "videoFrameRate", project.videoFrameRate);
            project.timecodeDropFrame = referenceInt(fields, "timecodeDropFrame", project.timecodeDropFrame ? 1 : 0) != 0;
        } else if (line.rfind("SETTINGS ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            project.editMode = referenceString(fields, "editMode", project.editMode);
            project.gridUnit = referenceString(fields, "gridUnit", project.gridUnit);
            project.tempoBpm = referenceInt(fields, "tempoBpm", project.tempoBpm);
            const std::string timeSignature = referenceString(fields, "timeSignature");
            const auto slash = timeSignature.find('/');
            if (slash != std::string::npos) {
                try {
                    project.timeSignatureNumerator = std::max(1, std::min(16, std::stoi(timeSignature.substr(0, slash))));
                    project.timeSignatureDenominator = std::max(1, std::min(32, std::stoi(timeSignature.substr(slash + 1))));
                } catch (const std::exception&) {
                }
            }
            project.grooveFeel = referenceString(fields, "grooveFeel", project.grooveFeel);
            project.grooveSwingAmount = referenceDouble(fields, "grooveSwingAmount", project.grooveSwingAmount);
            project.detectedKey = referenceString(fields, "detectedKey", project.detectedKey);
            project.detectedKeyMode = referenceString(fields, "detectedKeyMode", project.detectedKeyMode);
        } else if (line.rfind("TEMPO ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            TempoMarkerState marker;
            marker.timeSeconds = referenceDouble(fields, "timeSeconds", 0.0);
            marker.bpm = referenceDouble(fields, "bpm", 120.0);
            if (std::isfinite(marker.timeSeconds) && std::isfinite(marker.bpm) && marker.timeSeconds >= 0.0 && marker.bpm > 0.0) {
                project.tempoMap.push_back(marker);
            }
        } else if (line.rfind("TIME_SIGNATURE ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            TimeSignatureMarkerState signature;
            signature.timeSeconds = referenceDouble(fields, "timeSeconds", 0.0);
            const auto value = referenceString(fields, "signature");
            const auto slash = value.find('/');
            if (slash != std::string::npos) {
                try {
                    signature.numerator = std::max(1, std::min(16, std::stoi(value.substr(0, slash))));
                    signature.denominator = std::max(1, std::min(32, std::stoi(value.substr(slash + 1))));
                } catch (const std::exception&) {
                }
            }
            if (std::isfinite(signature.timeSeconds) && signature.timeSeconds >= 0.0) {
                project.timeSignatureMap.push_back(signature);
            }
        } else if (line.rfind("CHORD_SECTION ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            ChordEventState chord;
            chord.id = referenceString(fields, "id");
            chord.name = referenceString(fields, "name");
            chord.timeSeconds = referenceDouble(fields, "timeSeconds", 0.0);
            if (chord.id.empty()) {
                chord.id = "chord-" + std::to_string(project.chordEvents.size() + 1);
            }
            if (chord.name.empty()) {
                chord.name = chord.id;
            }
            if (std::isfinite(chord.timeSeconds) && chord.timeSeconds >= 0.0) {
                project.chordEvents.push_back(chord);
            }
        } else if (line.rfind("LYRIC ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            LyricEventState lyric;
            lyric.id = referenceString(fields, "id");
            lyric.text = referenceString(fields, "text");
            lyric.timeSeconds = referenceDouble(fields, "timeSeconds", 0.0);
            if (lyric.id.empty()) {
                lyric.id = "lyric-" + std::to_string(project.lyricEvents.size() + 1);
            }
            if (lyric.text.empty()) {
                lyric.text = lyric.id;
            }
            if (std::isfinite(lyric.timeSeconds) && lyric.timeSeconds >= 0.0) {
                project.lyricEvents.push_back(lyric);
            }
        } else if (line.rfind("VIDEO_SOURCE ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            VideoSourceState source;
            source.id = referenceString(fields, "id");
            source.path = referenceString(fields, "path");
            source.displayName = referenceString(fields, "name", source.id);
            source.frameRate = referenceDouble(fields, "frameRate", project.videoFrameRate);
            source.durationSeconds = referenceDouble(fields, "durationSeconds", 0.0);
            source.width = referenceInt(fields, "width", 0);
            source.height = referenceInt(fields, "height", 0);
            source.hasAudio = referenceInt(fields, "hasAudio", 0) != 0;
            if (!source.id.empty()) {
                project.videoSources.push_back(source);
            }
        } else if (line.rfind("VIDEO_CLIP ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            VideoClipState clip;
            clip.id = referenceString(fields, "id");
            clip.sourceId = referenceString(fields, "sourceId");
            clip.name = referenceString(fields, "name", clip.id);
            clip.startSeconds = referenceDouble(fields, "startSeconds", 0.0);
            clip.durationSeconds = referenceDouble(fields, "durationSeconds", 0.0);
            clip.sourceOffsetSeconds = referenceDouble(fields, "sourceOffsetSeconds", 0.0);
            clip.sourceTimecodeStartSeconds = referenceDouble(fields, "sourceTimecodeStartSeconds", project.timecodeStartSeconds);
            clip.muted = referenceInt(fields, "muted", 0) != 0;
            clip.locked = referenceInt(fields, "locked", 0) != 0;
            const bool sourceExists = std::any_of(project.videoSources.begin(), project.videoSources.end(), [&](const VideoSourceState& source) {
                return source.id == clip.sourceId;
            });
            if (!clip.id.empty() && sourceExists && clip.durationSeconds > 0.0) {
                project.videoClips.push_back(clip);
            }
        } else if (line.rfind("TRACK ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            TrackState track;
            track.name = referenceString(fields, "name");
            if (track.name.empty()) {
                continue;
            }
            track.trackType = referenceString(fields, "type", "audio");
            track.inputBus = referenceString(fields, "input", track.inputBus);
            track.outputBus = referenceString(fields, "output", track.outputBus);
            track.volumeDb = static_cast<float>(referenceDouble(fields, "volumeDb", track.volumeDb));
            track.pan = static_cast<float>(referenceDouble(fields, "pan", track.pan));
            track.muted = referenceInt(fields, "muted", 0) != 0;
            track.solo = referenceInt(fields, "solo", 0) != 0;
            track.recordArmed = referenceInt(fields, "recordArmed", 0) != 0;
            project.tracks.push_back(track);
            importedTrackNames.insert(track.name);
        } else if (line.rfind("CLIP ", 0) == 0) {
            const auto fields = parseReferenceFields(line);
            ClipState clip;
            clip.id = referenceString(fields, "id");
            clip.regionName = referenceString(fields, "name");
            clip.trackName = referenceString(fields, "track");
            clip.sourcePath = referenceString(fields, "source");
            clip.sourceFileUid = referenceString(fields, "sourceUid");
            clip.startSeconds = referenceDouble(fields, "startSeconds", 0.0);
            clip.durationSeconds = referenceDouble(fields, "durationSeconds", 0.0);
            clip.sourceOffsetSeconds = referenceDouble(fields, "sourceOffsetSeconds", 0.0);
            clip.sourceHasBroadcastTimeReference = referenceInt(fields, "sourceHasBroadcastTimeReference", 0) != 0;
            clip.sourceTimeReferenceSamples = static_cast<uint64_t>(std::max(0.0, referenceDouble(fields, "sourceTimeReferenceSamples", 0.0)));
            clip.sourceTimeReferenceSeconds = referenceDouble(fields, "sourceTimeReferenceSeconds", 0.0);
            clip.sourceTempoBpm = referenceDouble(fields, "sourceTempoBpm", 0.0);
            applySourceTimeSignature(clip, referenceString(fields, "sourceTimeSignature"));
            clip.sourceGrooveFeel = referenceString(fields, "sourceGrooveFeel");
            if (!supportedGrooveFeel(clip.sourceGrooveFeel)) {
                clip.sourceGrooveFeel.clear();
            }
            clip.sourceGrooveSwingAmount = referenceDouble(fields, "sourceGrooveSwingAmount", 0.0);
            clip.gainDb = static_cast<float>(referenceDouble(fields, "gainDb", 0.0));
            clip.fadeInSeconds = referenceDouble(fields, "fadeInSeconds", 0.0);
            clip.fadeInCurve = referenceString(fields, "fadeInCurve", "linear");
            clip.fadeOutSeconds = referenceDouble(fields, "fadeOutSeconds", 0.0);
            clip.fadeOutCurve = referenceString(fields, "fadeOutCurve", "linear");
            clip.muted = referenceInt(fields, "muted", 0) != 0;
            clip.polarityInverted = referenceInt(fields, "polarityInverted", 0) != 0;
            clip.colorHex = referenceString(fields, "color");
            if (clip.id.empty() || clip.trackName.empty() || clip.durationSeconds <= 0.0 ||
                importedTrackNames.find(clip.trackName) == importedTrackNames.end()) {
                continue;
            }
            project.clips.push_back(clip);
        }
    }

    if (project.tracks.empty()) {
        project.tracks = defaultProject().tracks;
    }
    if (project.timeSignatureMap.empty()) {
        project.timeSignatureMap.push_back({0.0, project.timeSignatureNumerator, project.timeSignatureDenominator});
    }
    normalizeProjectRouting(project);

    result.ok = true;
    result.project = std::move(project);
    result.clipCount = result.project.clips.size();
    result.trackCount = result.project.tracks.size();
    result.chordEventCount = result.project.chordEvents.size();
    result.lyricEventCount = result.project.lyricEvents.size();
    result.message = formatLabel + " reference import complete with " + std::to_string(result.clipCount) +
        " clip(s), " + std::to_string(result.chordEventCount) + " chord/section event(s), and " +
        std::to_string(result.lyricEventCount) + " lyric event(s).";
    return result;
}

} // namespace

const char* timelineInterchangeProfileName(TimelineInterchangeProfile profile) {
    switch (profile) {
        case TimelineInterchangeProfile::DaVinciResolve:
            return "DaVinci Resolve";
        case TimelineInterchangeProfile::FinalCutPro:
        default:
            return "Final Cut Pro";
    }
}

EdlExportResult exportProjectToCmx3600Edl(const ProjectDocument& project,
                                         double frameRate,
                                         TimelineInterchangeProfile profile) {
    EdlExportResult result;
    if (!std::isfinite(frameRate) || frameRate < 1.0 || frameRate > 240.0) {
        result.message = "EDL export failed: invalid frame rate.";
        return result;
    }

    std::vector<EdlEvent> events;
    for (const auto& clip : project.clips) {
        if (clip.muted || clip.durationSeconds <= 0.0 || protectedTrackName(clip.trackName)) {
            continue;
        }
        const auto* track = findTrack(project, clip.trackName);
        if (track != nullptr && (track->muted || protectedTrackName(track->name))) {
            continue;
        }
        events.push_back({&clip, track});
    }

    std::sort(events.begin(), events.end(), [](const EdlEvent& left, const EdlEvent& right) {
        if (left.clip->startSeconds != right.clip->startSeconds) {
            return left.clip->startSeconds < right.clip->startSeconds;
        }
        if (left.clip->trackName != right.clip->trackName) {
            return left.clip->trackName < right.clip->trackName;
        }
        return left.clip->id < right.clip->id;
    });

    std::ostringstream out;
    out << "TITLE: " << commentValue(project.name.empty() ? "Untitled" : project.name) << "\n";
    const bool dropFrame = project.timecodeDropFrame && frameRateSupportsDropFrame(frameRate);
    out << "FCM: " << (dropFrame ? "DROP FRAME" : "NON-DROP FRAME") << "\n";
    out << "* NEURACOUST_DAW_EDL_VERSION: 1\n";
    out << "* INTERCHANGE_PROFILE: " << timelineInterchangeProfileName(profile) << "\n";
    out << "* SAMPLE_RATE: " << std::fixed << std::setprecision(0) << project.sampleRate << "\n";
    out << "* BIT_DEPTH: " << project.bitDepth << "\n";
    out << "* TIMECODE_START_SECONDS: " << fixedSeconds(project.timecodeStartSeconds) << "\n";
    out << "* EDIT_MODE: " << commentValue(project.editMode) << "\n";
    out << "* GRID_UNIT: " << commentValue(project.gridUnit) << "\n";
    out << "* TIME_SIGNATURE: " << project.timeSignatureNumerator << "/" << project.timeSignatureDenominator << "\n";
    out << "* GROOVE: feel=" << commentValue(project.grooveFeel)
        << "; swing=" << fixedNumber(project.grooveSwingAmount, 3) << "\n";
    out << "* KEY: root=" << commentValue(project.detectedKey)
        << "; mode=" << commentValue(project.detectedKeyMode) << "\n";
    out << "* SNAP_QUANTUM_SECONDS: " << std::setprecision(6) << projectTimelineQuantumSeconds(project) << "\n\n";
    for (const auto& tempo : project.tempoMap) {
        if (std::isfinite(tempo.timeSeconds) && std::isfinite(tempo.bpm)) {
            out << "* TEMPO: " << fixedSeconds(tempo.timeSeconds) << "s " << std::fixed << std::setprecision(3) << tempo.bpm << " BPM\n";
        }
    }
    for (const auto& signature : project.timeSignatureMap) {
        if (std::isfinite(signature.timeSeconds)) {
            out << "* TIME_SIGNATURE_MARKER: " << fixedSeconds(signature.timeSeconds) << "s "
                << signature.numerator << "/" << signature.denominator << "\n";
        }
    }
    for (const auto& marker : project.markers) {
        if (std::isfinite(marker.timeSeconds)) {
            out << "* MARKER: " << fixedSeconds(marker.timeSeconds) << "s "
                << commentValue(marker.name.empty() ? marker.id : marker.name) << "\n";
        }
    }
    for (const auto& chord : project.chordEvents) {
        if (std::isfinite(chord.timeSeconds)) {
            out << "* CHORD_SECTION: " << fixedSeconds(chord.timeSeconds) << "s "
                << commentValue(chord.name.empty() ? chord.id : chord.name) << "\n";
        }
    }
    for (const auto& lyric : project.lyricEvents) {
        if (std::isfinite(lyric.timeSeconds)) {
            out << "* LYRIC: " << fixedSeconds(lyric.timeSeconds) << "s "
                << commentValue(lyric.text.empty() ? lyric.id : lyric.text) << "\n";
        }
    }
    for (const auto& source : project.videoSources) {
        out << "* VIDEO_SOURCE: id=" << commentValue(source.id)
            << "; path=" << commentValue(source.path)
            << "; name=" << commentValue(source.displayName.empty() ? source.id : source.displayName)
            << "; frameRate=" << fixedNumber(source.frameRate, 3)
            << "; duration=" << fixedSeconds(source.durationSeconds)
            << "; size=" << source.width << "x" << source.height
            << "; hasAudio=" << (source.hasAudio ? "1" : "0") << "\n";
    }
    for (const auto& clip : project.videoClips) {
        out << "* VIDEO_CLIP: id=" << commentValue(clip.id)
            << "; sourceId=" << commentValue(clip.sourceId)
            << "; name=" << commentValue(clip.name.empty() ? clip.id : clip.name)
            << "; start=" << fixedSeconds(clip.startSeconds)
            << "; duration=" << fixedSeconds(clip.durationSeconds)
            << "; sourceOffset=" << fixedSeconds(clip.sourceOffsetSeconds)
            << "; sourceTimecodeStart=" << fixedSeconds(clip.sourceTimecodeStartSeconds)
            << "; muted=" << (clip.muted ? "1" : "0")
            << "; locked=" << (clip.locked ? "1" : "0") << "\n";
    }
    if (!project.tempoMap.empty() || !project.markers.empty() || !project.chordEvents.empty() ||
        !project.lyricEvents.empty() || !project.videoSources.empty() || !project.videoClips.empty()) {
        out << "\n";
    }

    size_t eventNumber = 1;
    for (const auto& event : events) {
        const auto& clip = *event.clip;
        const auto sourceIn = std::max(0.0, clip.sourceOffsetSeconds);
        const auto sourceOut = sourceIn + clip.durationSeconds;
        const auto recordIn = project.timecodeStartSeconds + std::max(0.0, clip.startSeconds);
        const auto recordOut = recordIn + clip.durationSeconds;
        const auto trackName = event.track != nullptr ? event.track->name : clip.trackName;
        const auto trackType = event.track != nullptr ? event.track->trackType : std::string("audio");
        const auto clipName = clip.regionName.empty() ? clip.id : clip.regionName;

        out << std::setw(3) << std::setfill('0') << eventNumber << std::setfill(' ') << "  "
            << std::left << std::setw(8) << clipReelName(clip, event.track) << std::right
            << " A     C        "
            << timecodeFromSeconds(sourceIn, frameRate, dropFrame) << " "
            << timecodeFromSeconds(sourceOut, frameRate, dropFrame) << " "
            << timecodeFromSeconds(recordIn, frameRate, dropFrame) << " "
            << timecodeFromSeconds(recordOut, frameRate, dropFrame) << "\n";
        out << "* FROM CLIP NAME: " << commentValue(clipName) << "\n";
        out << "* SOURCE FILE: " << commentValue(clip.sourcePath) << "\n";
        if (!clip.sourceFileUid.empty()) {
            out << "* SOURCE UID: " << commentValue(clip.sourceFileUid) << "\n";
        }
        if (clip.sourceHasBroadcastTimeReference) {
            out << "* BWF_TIME_REFERENCE: " << clip.sourceTimeReferenceSamples
                << " samples " << fixedSeconds(clip.sourceTimeReferenceSeconds) << "s\n";
        }
        if (clip.sourceTempoBpm > 0.0 || clip.sourceTimeSignatureNumerator > 0 || !clip.sourceGrooveFeel.empty()) {
            out << "* SOURCE_MUSIC: tempo=" << fixedNumber(clip.sourceTempoBpm, 3)
                << "; timeSignature=" << clip.sourceTimeSignatureNumerator << "/" << clip.sourceTimeSignatureDenominator
                << "; grooveFeel=" << commentValue(clip.sourceGrooveFeel)
                << "; grooveSwing=" << fixedNumber(clip.sourceGrooveSwingAmount, 3) << "\n";
        }
        out << "* TRACK: " << commentValue(trackName) << " (" << commentValue(trackType) << ")\n";
        out << "* GAIN_DB: " << std::fixed << std::setprecision(2) << clip.gainDb << "\n";
        out << "* FADES: IN " << std::setprecision(3) << clip.fadeInSeconds << "s " << commentValue(clip.fadeInCurve)
            << " / OUT " << clip.fadeOutSeconds << "s " << commentValue(clip.fadeOutCurve) << "\n\n";
        ++eventNumber;
    }

    result.ok = true;
    result.text = out.str();
    result.eventCount = events.size();
    result.tempoEventCount = static_cast<size_t>(std::count_if(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& tempo) {
        return std::isfinite(tempo.timeSeconds) && std::isfinite(tempo.bpm);
    }));
    result.markerCount = static_cast<size_t>(std::count_if(project.markers.begin(), project.markers.end(), [](const MarkerState& marker) {
        return std::isfinite(marker.timeSeconds);
    }));
    result.chordEventCount = static_cast<size_t>(std::count_if(project.chordEvents.begin(), project.chordEvents.end(), [](const ChordEventState& chord) {
        return std::isfinite(chord.timeSeconds);
    }));
    result.lyricEventCount = static_cast<size_t>(std::count_if(project.lyricEvents.begin(), project.lyricEvents.end(), [](const LyricEventState& lyric) {
        return std::isfinite(lyric.timeSeconds);
    }));
    result.message = std::string(timelineInterchangeProfileName(profile)) + " EDL export complete with " + std::to_string(result.eventCount) +
        " event(s), " + std::to_string(result.markerCount) +
        " marker(s), " + std::to_string(result.chordEventCount) +
        " chord/section event(s), and " + std::to_string(result.lyricEventCount) + " lyric event(s).";
    return result;
}

FcpxmlExportResult exportProjectToFcpxml(const ProjectDocument& project, TimelineInterchangeProfile profile) {
    FcpxmlExportResult result;
    std::vector<EdlEvent> events;
    std::map<std::string, std::string> assetIdsBySourcePath;
    std::map<std::string, double> assetDurationsBySourcePath;
    std::set<std::string> assetSourcePaths;
    double timelineDurationSeconds = 1.0;

    for (const auto& clip : project.clips) {
        if (clip.muted || clip.durationSeconds <= 0.0 || protectedTrackName(clip.trackName)) {
            continue;
        }
        const auto* track = findTrack(project, clip.trackName);
        if (track != nullptr && (track->muted || protectedTrackName(track->name))) {
            continue;
        }
        events.push_back({&clip, track});
        timelineDurationSeconds = std::max(timelineDurationSeconds, clip.startSeconds + clip.durationSeconds);
        if (!clip.sourcePath.empty()) {
            assetSourcePaths.insert(clip.sourcePath);
            assetDurationsBySourcePath[clip.sourcePath] = std::max(assetDurationsBySourcePath[clip.sourcePath],
                                                                    clip.sourceOffsetSeconds + clip.durationSeconds);
        }
    }

    std::sort(events.begin(), events.end(), [](const EdlEvent& left, const EdlEvent& right) {
        if (left.clip->startSeconds != right.clip->startSeconds) {
            return left.clip->startSeconds < right.clip->startSeconds;
        }
        if (left.clip->trackName != right.clip->trackName) {
            return left.clip->trackName < right.clip->trackName;
        }
        return left.clip->id < right.clip->id;
    });

    size_t assetIndex = 1;
    for (const auto& sourcePath : assetSourcePaths) {
        assetIdsBySourcePath[sourcePath] = "r" + std::to_string(assetIndex++);
    }

    const auto projectName = project.name.empty() ? std::string("Untitled") : project.name;
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<!DOCTYPE fcpxml>\n";
    out << "<fcpxml version=\"1.10\">\n";
    out << "  <resources>\n";
    out << "    <format id=\"fmt1\" name=\"Neuracoust DAW "
        << (profile == TimelineInterchangeProfile::DaVinciResolve ? "Resolve Timeline" : "Audio Timeline")
        << "\" frameDuration=\"1/30s\" />\n";
    for (const auto& [sourcePath, assetId] : assetIdsBySourcePath) {
        const auto assetName = std::filesystem::path(sourcePath).filename().string();
        out << "    <asset id=\"" << escapeXmlString(assetId)
            << "\" name=\"" << escapeXmlString(assetName.empty() ? sourcePath : assetName)
            << "\" src=\"file://" << escapeXmlString(sourcePath)
            << "\" start=\"0s\" duration=\"" << secondsForFcpxml(assetDurationsBySourcePath[sourcePath])
            << "\" hasAudio=\"1\" audioSources=\"1\" audioChannels=\"2\" audioRate=\""
            << std::fixed << std::setprecision(0) << project.sampleRate << "\" />\n";
    }
    out << "  </resources>\n";
    out << "  <library>\n";
    out << "    <event name=\"" << escapeXmlString(projectName) << "\">\n";
    out << "      <project name=\"" << escapeXmlString(projectName) << "\">\n";
    out << "        <sequence duration=\"" << secondsForFcpxml(timelineDurationSeconds)
        << "\" format=\"fmt1\" tcStart=\"" << secondsForFcpxml(project.timecodeStartSeconds)
        << "\" tcFormat=\"" << (project.timecodeDropFrame && frameRateSupportsDropFrame(project.videoFrameRate) ? "DF" : "NDF")
        << "\" audioRate=\"" << std::fixed << std::setprecision(0) << project.sampleRate << "\">\n";
    out << "          <spine>\n";

    for (const auto& event : events) {
        const auto& clip = *event.clip;
        const auto assetIt = assetIdsBySourcePath.find(clip.sourcePath);
        if (assetIt == assetIdsBySourcePath.end()) {
            continue;
        }
        const auto trackName = event.track != nullptr ? event.track->name : clip.trackName;
        const auto trackType = event.track != nullptr ? event.track->trackType : std::string("audio");
        const auto clipName = clip.regionName.empty() ? clip.id : clip.regionName;
        out << "            <asset-clip name=\"" << escapeXmlString(clipName)
            << "\" ref=\"" << escapeXmlString(assetIt->second)
            << "\" offset=\"" << secondsForFcpxml(clip.startSeconds)
            << "\" start=\"" << secondsForFcpxml(clip.sourceOffsetSeconds)
            << "\" duration=\"" << secondsForFcpxml(clip.durationSeconds)
            << "\" audioRole=\"dialogue\">\n";
        out << "              <note>track=" << escapeXmlString(trackName)
            << "; trackType=" << escapeXmlString(trackType)
            << "; clipId=" << escapeXmlString(clip.id)
            << "; sourceUid=" << escapeXmlString(clip.sourceFileUid)
            << "; bwfTimeRefSamples=" << clip.sourceTimeReferenceSamples
            << "; bwfTimeRefSeconds=" << fixedSeconds(clip.sourceTimeReferenceSeconds)
            << "; sourceTempoBpm=" << fixedNumber(clip.sourceTempoBpm, 3)
            << "; sourceTimeSignature=" << clip.sourceTimeSignatureNumerator << "/" << clip.sourceTimeSignatureDenominator
            << "; sourceGrooveFeel=" << escapeXmlString(clip.sourceGrooveFeel)
            << "; sourceGrooveSwingAmount=" << fixedNumber(clip.sourceGrooveSwingAmount, 3)
            << "; gainDb=" << std::fixed << std::setprecision(2) << clip.gainDb
            << "; fadeIn=" << std::setprecision(3) << clip.fadeInSeconds << " " << escapeXmlString(clip.fadeInCurve)
            << "; fadeOut=" << clip.fadeOutSeconds << " " << escapeXmlString(clip.fadeOutCurve)
            << "; color=" << escapeXmlString(clip.colorHex) << "</note>\n";
        out << "            </asset-clip>\n";
    }

    out << "          </spine>\n";
    out << "          <metadata>\n";
    out << "            <md key=\"com.neuracoust.daw.interchangeProfile\" value=\""
        << timelineInterchangeProfileName(profile) << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.sampleRate\" value=\"" << std::fixed << std::setprecision(0) << project.sampleRate << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.bitDepth\" value=\"" << project.bitDepth << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.editMode\" value=\"" << escapeXmlString(project.editMode) << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.gridUnit\" value=\"" << escapeXmlString(project.gridUnit) << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.tempoBpm\" value=\"" << project.tempoBpm << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.timeSignature\" value=\"" << project.timeSignatureNumerator << "/" << project.timeSignatureDenominator << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.grooveFeel\" value=\"" << escapeXmlString(project.grooveFeel) << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.grooveSwingAmount\" value=\"" << std::fixed << std::setprecision(3) << project.grooveSwingAmount << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.detectedKey\" value=\"" << escapeXmlString(project.detectedKey) << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.detectedKeyMode\" value=\"" << escapeXmlString(project.detectedKeyMode) << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.videoFrameRate\" value=\"" << std::fixed << std::setprecision(3) << project.videoFrameRate << "\" />\n";
    out << "            <md key=\"com.neuracoust.daw.timecodeDropFrame\" value=\"" << (project.timecodeDropFrame ? "1" : "0") << "\" />\n";
    for (size_t index = 0; index < project.videoSources.size(); ++index) {
        const auto& source = project.videoSources[index];
        out << "            <md key=\"com.neuracoust.daw.videoSource." << index
            << "\" value=\"id=" << escapeXmlString(source.id)
            << "; path=" << escapeXmlString(source.path)
            << "; name=" << escapeXmlString(source.displayName.empty() ? source.id : source.displayName)
            << "; frameRate=" << std::fixed << std::setprecision(3) << source.frameRate
            << "; duration=" << fixedSeconds(source.durationSeconds)
            << "; size=" << source.width << "x" << source.height
            << "; hasAudio=" << (source.hasAudio ? "1" : "0") << "\" />\n";
    }
    for (size_t index = 0; index < project.videoClips.size(); ++index) {
        const auto& clip = project.videoClips[index];
        out << "            <md key=\"com.neuracoust.daw.videoClip." << index
            << "\" value=\"id=" << escapeXmlString(clip.id)
            << "; sourceId=" << escapeXmlString(clip.sourceId)
            << "; name=" << escapeXmlString(clip.name.empty() ? clip.id : clip.name)
            << "; start=" << fixedSeconds(clip.startSeconds)
            << "; duration=" << fixedSeconds(clip.durationSeconds)
            << "; sourceOffset=" << fixedSeconds(clip.sourceOffsetSeconds)
            << "; sourceTimecodeStart=" << fixedSeconds(clip.sourceTimecodeStartSeconds)
            << "; muted=" << (clip.muted ? "1" : "0")
            << "; locked=" << (clip.locked ? "1" : "0") << "\" />\n";
    }
    for (size_t index = 0; index < project.tempoMap.size(); ++index) {
        const auto& tempo = project.tempoMap[index];
        if (std::isfinite(tempo.timeSeconds) && std::isfinite(tempo.bpm)) {
            out << "            <md key=\"com.neuracoust.daw.tempo." << index
                << "\" value=\"time=" << fixedSeconds(tempo.timeSeconds)
                << "; bpm=" << std::fixed << std::setprecision(3) << tempo.bpm << "\" />\n";
        }
    }
    for (size_t index = 0; index < project.timeSignatureMap.size(); ++index) {
        const auto& signature = project.timeSignatureMap[index];
        if (std::isfinite(signature.timeSeconds)) {
            out << "            <md key=\"com.neuracoust.daw.timeSignatureMarker." << index
                << "\" value=\"time=" << fixedSeconds(signature.timeSeconds)
                << "; signature=" << signature.numerator << "/" << signature.denominator << "\" />\n";
        }
    }
    for (size_t index = 0; index < project.markers.size(); ++index) {
        const auto& marker = project.markers[index];
        if (std::isfinite(marker.timeSeconds)) {
            out << "            <md key=\"com.neuracoust.daw.marker." << index
                << "\" value=\"id=" << escapeXmlString(marker.id)
                << "; name=" << escapeXmlString(marker.name.empty() ? marker.id : marker.name)
                << "; time=" << fixedSeconds(marker.timeSeconds) << "\" />\n";
        }
    }
    for (size_t index = 0; index < project.chordEvents.size(); ++index) {
        const auto& chord = project.chordEvents[index];
        if (std::isfinite(chord.timeSeconds)) {
            out << "            <md key=\"com.neuracoust.daw.chordSection." << index
                << "\" value=\"id=" << escapeXmlString(chord.id)
                << "; name=" << escapeXmlString(chord.name.empty() ? chord.id : chord.name)
                << "; time=" << fixedSeconds(chord.timeSeconds) << "\" />\n";
        }
    }
    for (size_t index = 0; index < project.lyricEvents.size(); ++index) {
        const auto& lyric = project.lyricEvents[index];
        if (std::isfinite(lyric.timeSeconds)) {
            out << "            <md key=\"com.neuracoust.daw.lyric." << index
                << "\" value=\"id=" << escapeXmlString(lyric.id)
                << "; text=" << escapeXmlString(lyric.text.empty() ? lyric.id : lyric.text)
                << "; time=" << fixedSeconds(lyric.timeSeconds) << "\" />\n";
        }
    }
    out << "          </metadata>\n";
    out << "        </sequence>\n";
    out << "      </project>\n";
    out << "    </event>\n";
    out << "  </library>\n";
    out << "</fcpxml>\n";

    result.ok = true;
    result.text = out.str();
    result.clipCount = events.size();
    result.assetCount = assetIdsBySourcePath.size();
    result.tempoEventCount = static_cast<size_t>(std::count_if(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& tempo) {
        return std::isfinite(tempo.timeSeconds) && std::isfinite(tempo.bpm);
    }));
    result.markerCount = static_cast<size_t>(std::count_if(project.markers.begin(), project.markers.end(), [](const MarkerState& marker) {
        return std::isfinite(marker.timeSeconds);
    }));
    result.chordEventCount = static_cast<size_t>(std::count_if(project.chordEvents.begin(), project.chordEvents.end(), [](const ChordEventState& chord) {
        return std::isfinite(chord.timeSeconds);
    }));
    result.lyricEventCount = static_cast<size_t>(std::count_if(project.lyricEvents.begin(), project.lyricEvents.end(), [](const LyricEventState& lyric) {
        return std::isfinite(lyric.timeSeconds);
    }));
    result.message = std::string(timelineInterchangeProfileName(profile)) + " FCPXML export complete with " + std::to_string(result.clipCount) +
        " clip(s), " + std::to_string(result.markerCount) +
        " marker(s), " + std::to_string(result.chordEventCount) +
        " chord/section event(s), and " + std::to_string(result.lyricEventCount) + " lyric event(s).";
    return result;
}

VideoDeliveryPlan makeVideoDeliveryPlan(const ProjectDocument& project,
                                        VideoDeliveryPreset preset,
                                        const std::filesystem::path& outputPath,
                                        const std::filesystem::path& mixedAudioPath) {
    VideoDeliveryPlan result;
    result.container = "mp4";
    result.videoCodec = "h264";
    result.audioCodec = "aac";
    result.frameRate = std::isfinite(project.videoFrameRate) && project.videoFrameRate > 1.0
        ? project.videoFrameRate
        : 30.0;

    switch (preset) {
        case VideoDeliveryPreset::YouTube4k:
            result.presetId = "youtube-4k";
            result.presetName = "YouTube 4K";
            result.width = 3840;
            result.height = 2160;
            result.videoBitrateKbps = 45000;
            result.audioBitrateKbps = 320;
            break;
        case VideoDeliveryPreset::SharePreview720p:
            result.presetId = "share-preview-720p";
            result.presetName = "Share Preview 720p";
            result.width = 1280;
            result.height = 720;
            result.videoBitrateKbps = 4500;
            result.audioBitrateKbps = 160;
            break;
        case VideoDeliveryPreset::YouTube1080p:
        default:
            result.presetId = "youtube-1080p";
            result.presetName = "YouTube 1080p";
            result.width = 1920;
            result.height = 1080;
            result.videoBitrateKbps = 12000;
            result.audioBitrateKbps = 320;
            break;
    }

    for (const auto& clip : project.videoClips) {
        if (clip.muted || clip.durationSeconds <= 0.0) {
            continue;
        }
        const auto* source = findVideoSource(project, clip.sourceId);
        if (source == nullptr || source->path.empty()) {
            continue;
        }
        VideoDeliveryClipPlan clipPlan;
        clipPlan.clipId = clip.id;
        clipPlan.sourcePath = source->path;
        clipPlan.timelineStartSeconds = std::max(0.0, clip.startSeconds);
        clipPlan.durationSeconds = clip.durationSeconds;
        clipPlan.sourceOffsetSeconds = std::max(0.0, clip.sourceOffsetSeconds);
        result.durationSeconds = std::max(result.durationSeconds, clipPlan.timelineStartSeconds + clipPlan.durationSeconds);
        result.clips.push_back(std::move(clipPlan));
    }

    std::sort(result.clips.begin(), result.clips.end(), [](const VideoDeliveryClipPlan& left, const VideoDeliveryClipPlan& right) {
        if (left.timelineStartSeconds != right.timelineStartSeconds) {
            return left.timelineStartSeconds < right.timelineStartSeconds;
        }
        return left.clipId < right.clipId;
    });

    if (result.clips.empty()) {
        result.message = result.presetName + " delivery plan failed: no active video clips.";
        return result;
    }

    const auto& firstClip = result.clips.front();
    std::ostringstream command;
    command << "ffmpeg -y -ss " << fixedSeconds(firstClip.sourceOffsetSeconds)
            << " -t " << fixedSeconds(firstClip.durationSeconds)
            << " -i " << shellQuote(firstClip.sourcePath);
    if (!mixedAudioPath.empty()) {
        command << " -i " << shellQuote(mixedAudioPath.string())
                << " -map 0:v:0 -map 1:a:0";
    }
    command << " -c:v libx264 -preset slow -b:v " << result.videoBitrateKbps << "k"
            << " -maxrate " << result.videoBitrateKbps << "k -bufsize " << result.videoBitrateKbps * 2 << "k"
            << " -vf scale=" << result.width << ":" << result.height << ":force_original_aspect_ratio=decrease,"
            << "pad=" << result.width << ":" << result.height << ":(ow-iw)/2:(oh-ih)/2"
            << " -pix_fmt yuv420p -r " << fixedNumber(result.frameRate, 3)
            << " -c:a aac -b:a " << result.audioBitrateKbps << "k -movflags +faststart -shortest "
            << shellQuote(outputPath.string());
    result.ffmpegCommand = command.str();

    result.ok = true;
    result.message = result.presetName + " delivery plan ready with " +
        std::to_string(result.clips.size()) + " video clip(s).";
    if (result.clips.size() > 1) {
        result.message += " Multi-clip render needs the upcoming concat/mux runner; this command previews the first active clip.";
    }
    return result;
}

InterchangeReferenceExportResult exportProjectToAafReference(const ProjectDocument& project) {
    return exportProjectToReferenceText(project, "AAF", "NEURACOUST_DAW_AAF_REFERENCE_EXPORT");
}

InterchangeReferenceExportResult exportProjectToOmfReference(const ProjectDocument& project) {
    return exportProjectToReferenceText(project, "OMF", "NEURACOUST_DAW_OMF_REFERENCE_EXPORT");
}

InterchangeReferenceImportResult importProjectFromCmx3600EdlText(const std::string& text, double frameRate) {
    return importProjectFromCmx3600EdlSubsetText(text, frameRate);
}

InterchangeReferenceImportResult importProjectFromFcpxmlText(const std::string& text) {
    return importProjectFromFcpxmlSubsetText(text);
}

InterchangeReferenceImportResult importProjectFromAafReferenceText(const std::string& text) {
    return importProjectFromReferenceText(text, "NEURACOUST_DAW_AAF_REFERENCE_EXPORT", "AAF");
}

InterchangeReferenceImportResult importProjectFromOmfReferenceText(const std::string& text) {
    return importProjectFromReferenceText(text, "NEURACOUST_DAW_OMF_REFERENCE_EXPORT", "OMF");
}

MidiExportResult exportProjectTempoMapToMidi(const ProjectDocument& project, int ticksPerQuarter) {
    MidiExportResult result;
    if (ticksPerQuarter < 24 || ticksPerQuarter > 32767) {
        result.message = "MIDI export failed: invalid ticks-per-quarter value.";
        return result;
    }

    const auto tempos = sortedValidTempoMap(project);
    const auto signatures = sortedValidTimeSignatureMap(project);
    const auto projectName = project.name.empty() ? std::string("Untitled") : project.name;

    std::vector<uint8_t> conductor;
    uint32_t conductorTick = 0;
    appendMetaTextAtTick(conductor, conductorTick, 0, 0x03, projectName);
    for (const auto& signature : signatures) {
        appendTimeSignatureAtTick(conductor,
                                  conductorTick,
                                  midiTickForSeconds(tempos, signature.timeSeconds, ticksPerQuarter),
                                  signature.numerator,
                                  signature.denominator);
    }
    appendMetaTextAtTick(conductor, conductorTick, 0, 0x01,
                         "Neuracoust DAW MIDI tempo/marker export; audio clips are exported as region cue text.");
    for (const auto& tempo : tempos) {
        const uint32_t tick = midiTickForSeconds(tempos, tempo.timeSeconds, ticksPerQuarter);
        appendTempoAtTick(conductor, conductorTick, tick, tempo.bpm);
        ++result.tempoEventCount;
    }
    for (const auto& marker : project.markers) {
        if (!std::isfinite(marker.timeSeconds) || marker.timeSeconds < 0.0) {
            continue;
        }
        const uint32_t tick = midiTickForSeconds(tempos, marker.timeSeconds, ticksPerQuarter);
        const std::string name = marker.name.empty() ? marker.id : marker.name;
        appendMetaTextAtTick(conductor, conductorTick, tick, 0x06, name.empty() ? "Marker" : name);
        ++result.markerCount;
    }
    for (const auto& chord : project.chordEvents) {
        if (!std::isfinite(chord.timeSeconds) || chord.timeSeconds < 0.0) {
            continue;
        }
        const uint32_t tick = midiTickForSeconds(tempos, chord.timeSeconds, ticksPerQuarter);
        const std::string name = chord.name.empty() ? chord.id : chord.name;
        appendMetaTextAtTick(conductor, conductorTick, tick, 0x01,
                             "CHORD_SECTION id=" + commentValue(chord.id) +
                                 " name=" + commentValue(name.empty() ? std::string("Chord / Section") : name) +
                                 " time=" + fixedSeconds(chord.timeSeconds));
        ++result.chordEventCount;
    }
    for (const auto& lyric : project.lyricEvents) {
        if (!std::isfinite(lyric.timeSeconds) || lyric.timeSeconds < 0.0) {
            continue;
        }
        const uint32_t tick = midiTickForSeconds(tempos, lyric.timeSeconds, ticksPerQuarter);
        const std::string text = lyric.text.empty() ? lyric.id : lyric.text;
        appendMetaTextAtTick(conductor, conductorTick, tick, 0x01,
                             "LYRIC id=" + commentValue(lyric.id) +
                                 " text=" + commentValue(text.empty() ? std::string("Lyric") : text) +
                                 " time=" + fixedSeconds(lyric.timeSeconds));
        ++result.lyricEventCount;
    }
    appendEndOfTrack(conductor);

    std::vector<EdlEvent> events = timelineEventsForExport(project);
    std::vector<std::pair<uint32_t, std::string>> cueEvents;
    cueEvents.reserve(events.size());
    for (const auto& event : events) {
        const auto& clip = *event.clip;
        const auto* track = event.track;
        const std::string trackName = track != nullptr ? track->name : clip.trackName;
        const std::string clipName = clip.regionName.empty() ? clip.id : clip.regionName;
        std::ostringstream text;
        text << "REGION"
             << " clipId=" << commentValue(clip.id)
             << " name=" << commentValue(clipName)
             << " track=" << commentValue(trackName)
             << " sourceUid=" << commentValue(clip.sourceFileUid)
             << " source=" << commentValue(clip.sourcePath)
             << " start=" << fixedSeconds(clip.startSeconds)
             << " duration=" << fixedSeconds(clip.durationSeconds)
             << " sourceOffset=" << fixedSeconds(clip.sourceOffsetSeconds)
             << " sourceTempoBpm=" << fixedNumber(clip.sourceTempoBpm, 3)
             << " sourceTimeSignature=" << commentValue(std::to_string(clip.sourceTimeSignatureNumerator) + "/" + std::to_string(clip.sourceTimeSignatureDenominator))
             << " sourceGrooveFeel=" << commentValue(clip.sourceGrooveFeel)
             << " sourceGrooveSwingAmount=" << fixedNumber(clip.sourceGrooveSwingAmount, 3)
             << " gainDb=" << fixedNumber(clip.gainDb, 3);
        cueEvents.push_back({midiTickForSeconds(tempos, clip.startSeconds, ticksPerQuarter), text.str()});
    }
    std::sort(cueEvents.begin(), cueEvents.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second < right.second;
    });

    std::vector<uint8_t> regions;
    uint32_t regionsTick = 0;
    appendMetaTextAtTick(regions, regionsTick, 0, 0x03, "Neuracoust Audio Regions");
    for (const auto& cue : cueEvents) {
        appendMetaTextAtTick(regions, regionsTick, cue.first, 0x01, cue.second);
        ++result.regionCueCount;
    }
    appendEndOfTrack(regions);

    struct MidiTrackEvent {
        uint32_t tick = 0;
        uint8_t status = 0;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
        int dataByteCount = 2;
        bool noteOn = false;
        std::string noteId;
    };
    std::map<std::string, std::vector<MidiTrackEvent>> midiEventsByTrack;
    for (const auto& region : project.midiRegions) {
        if (region.muted || region.durationSeconds <= 0.0 || protectedTrackName(region.trackName)) {
            continue;
        }
        const auto* track = findTrack(project, region.trackName);
        if (track != nullptr && (track->muted || protectedTrackName(track->name))) {
            continue;
        }
        const double fallbackBpm = std::max(20.0, std::min(400.0, projectTempoAtSeconds(project, region.startSeconds)));
        const double regionEndSeconds = region.startSeconds + region.durationSeconds;
        auto& trackEvents = midiEventsByTrack[region.trackName.empty() ? std::string("MIDI") : region.trackName];
        const auto tickForRegionBeat = [&](double beat) {
            const double eventSeconds = secondsForBeatOffsetFromTempoMap(tempos, region.startSeconds, beat, fallbackBpm);
            return midiTickForSeconds(tempos, eventSeconds, ticksPerQuarter);
        };
        for (const auto& note : region.notes) {
            if (note.muted || note.pitch < 0 || note.pitch > 127 || note.durationBeats <= 0.0) {
                continue;
            }
            const double noteStartSeconds = secondsForBeatOffsetFromTempoMap(tempos, region.startSeconds, std::max(0.0, note.startBeats), fallbackBpm);
            if (noteStartSeconds >= regionEndSeconds) {
                continue;
            }
            const double noteEndSeconds = std::min(regionEndSeconds, secondsForBeatOffsetFromTempoMap(tempos, region.startSeconds, note.startBeats + note.durationBeats, fallbackBpm));
            if (noteEndSeconds <= noteStartSeconds) {
                continue;
            }
            const uint8_t channel = static_cast<uint8_t>(std::max(0, std::min(15, note.channel - 1)));
            const uint8_t pitch = static_cast<uint8_t>(std::max(0, std::min(127, note.pitch)));
            const uint8_t velocity = static_cast<uint8_t>(std::max(1, std::min(127, note.velocity)));
            trackEvents.push_back({midiTickForSeconds(tempos, noteStartSeconds, ticksPerQuarter),
                                   static_cast<uint8_t>(0x90 | channel),
                                   pitch,
                                   velocity,
                                   2,
                                   true,
                                   note.id});
            trackEvents.push_back({midiTickForSeconds(tempos, noteEndSeconds, ticksPerQuarter),
                                   static_cast<uint8_t>(0x80 | channel),
                                   pitch,
                                   0,
                                   2,
                                   false,
                                   note.id});
            ++result.midiNoteCount;
        }
        for (const auto& event : region.controllerEvents) {
            if (!std::isfinite(event.beat) || event.beat < 0.0) {
                continue;
            }
            const double eventSeconds = secondsForBeatOffsetFromTempoMap(tempos, region.startSeconds, event.beat, fallbackBpm);
            if (eventSeconds >= regionEndSeconds) {
                continue;
            }
            const uint8_t channel = static_cast<uint8_t>(std::max(0, std::min(15, event.channel - 1)));
            trackEvents.push_back({tickForRegionBeat(event.beat),
                                   static_cast<uint8_t>(0xb0 | channel),
                                   static_cast<uint8_t>(std::max(0, std::min(127, event.controller))),
                                   static_cast<uint8_t>(std::max(0, std::min(127, event.value))),
                                   2,
                                   false,
                                   event.id});
            ++result.midiControllerEventCount;
        }
        for (const auto& event : region.pitchBendEvents) {
            if (!std::isfinite(event.beat) || event.beat < 0.0) {
                continue;
            }
            const double eventSeconds = secondsForBeatOffsetFromTempoMap(tempos, region.startSeconds, event.beat, fallbackBpm);
            if (eventSeconds >= regionEndSeconds) {
                continue;
            }
            const int value = std::max(0, std::min(16383, event.value));
            const uint8_t channel = static_cast<uint8_t>(std::max(0, std::min(15, event.channel - 1)));
            trackEvents.push_back({tickForRegionBeat(event.beat),
                                   static_cast<uint8_t>(0xe0 | channel),
                                   static_cast<uint8_t>(value & 0x7f),
                                   static_cast<uint8_t>((value >> 7) & 0x7f),
                                   2,
                                   false,
                                   event.id});
            ++result.midiPitchBendEventCount;
        }
        for (const auto& event : region.programChangeEvents) {
            if (!std::isfinite(event.beat) || event.beat < 0.0) {
                continue;
            }
            const double eventSeconds = secondsForBeatOffsetFromTempoMap(tempos, region.startSeconds, event.beat, fallbackBpm);
            if (eventSeconds >= regionEndSeconds) {
                continue;
            }
            const uint8_t channel = static_cast<uint8_t>(std::max(0, std::min(15, event.channel - 1)));
            trackEvents.push_back({tickForRegionBeat(event.beat),
                                   static_cast<uint8_t>(0xc0 | channel),
                                   static_cast<uint8_t>(std::max(0, std::min(127, event.program))),
                                   0,
                                   1,
                                   false,
                                   event.id});
            ++result.midiProgramChangeEventCount;
        }
    }

    std::vector<std::vector<uint8_t>> midiTracks;
    for (auto& entry : midiEventsByTrack) {
        auto& eventsForTrack = entry.second;
        if (eventsForTrack.empty()) {
            continue;
        }
        std::sort(eventsForTrack.begin(), eventsForTrack.end(), [](const MidiTrackEvent& left, const MidiTrackEvent& right) {
            if (left.tick != right.tick) {
                return left.tick < right.tick;
            }
            if (left.noteOn != right.noteOn) {
                return !left.noteOn && right.noteOn;
            }
            if (left.status != right.status) {
                return left.status < right.status;
            }
            if (left.data1 != right.data1) {
                return left.data1 < right.data1;
            }
            return left.noteId < right.noteId;
        });
        std::vector<uint8_t> midiTrack;
        uint32_t midiTrackTick = 0;
        appendMetaTextAtTick(midiTrack, midiTrackTick, 0, 0x03, entry.first);
        for (const auto& event : eventsForTrack) {
            if (event.dataByteCount == 1) {
                appendMidiChannelEvent1AtTick(midiTrack, midiTrackTick, event.tick, event.status, event.data1);
            } else {
                appendMidiChannelEventAtTick(midiTrack, midiTrackTick, event.tick, event.status, event.data1, event.data2);
            }
        }
        appendEndOfTrack(midiTrack);
        midiTracks.push_back(std::move(midiTrack));
        ++result.midiTrackCount;
    }

    std::vector<uint8_t> file;
    appendAscii(file, "MThd");
    appendU32(file, 6);
    appendU16(file, 1);
    appendU16(file, static_cast<uint16_t>(2 + midiTracks.size()));
    appendU16(file, static_cast<uint16_t>(ticksPerQuarter));
    appendMidiTrack(file, conductor);
    appendMidiTrack(file, regions);
    for (const auto& midiTrack : midiTracks) {
        appendMidiTrack(file, midiTrack);
    }

    result.ok = true;
    result.data = std::move(file);
    result.message = "MIDI export complete with " + std::to_string(result.tempoEventCount) +
        " tempo event(s), " + std::to_string(result.markerCount) +
        " marker(s), " + std::to_string(result.chordEventCount) +
        " chord/section event(s), " + std::to_string(result.lyricEventCount) +
        " lyric event(s), " + std::to_string(result.regionCueCount) +
        " region cue(s), " + std::to_string(result.midiNoteCount) +
        " MIDI note(s), " + std::to_string(result.midiControllerEventCount) +
        " controller event(s), " + std::to_string(result.midiPitchBendEventCount) +
        " pitch bend event(s), and " + std::to_string(result.midiProgramChangeEventCount) +
        " program change event(s).";
    return result;
}

MidiExportResult writeProjectMidiFile(const ProjectDocument& project,
                                      const std::filesystem::path& outputPath,
                                      int ticksPerQuarter) {
    auto result = exportProjectTempoMapToMidi(project, ticksPerQuarter);
    if (!result.ok) {
        return result;
    }
    if (outputPath.empty()) {
        result.ok = false;
        result.message = "MIDI export failed: output path is empty.";
        return result;
    }

    std::error_code fsError;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(outputPath.parent_path(), fsError);
        if (fsError) {
            result.ok = false;
            result.message = "MIDI export failed: could not create output directory: " + fsError.message();
            return result;
        }
    }

    auto tempPath = outputPath;
    tempPath += ".writing";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.ok = false;
            result.message = "MIDI export failed: could not create temporary file.";
            return result;
        }
        out.write(reinterpret_cast<const char*>(result.data.data()), static_cast<std::streamsize>(result.data.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tempPath, fsError);
            result.ok = false;
            result.message = "MIDI export failed: could not finish writing temporary file.";
            return result;
        }
    }

    std::filesystem::rename(tempPath, outputPath, fsError);
    if (fsError) {
        std::filesystem::remove(outputPath, fsError);
        fsError.clear();
        std::filesystem::rename(tempPath, outputPath, fsError);
    }
    if (fsError) {
        const auto message = fsError.message();
        std::filesystem::remove(tempPath, fsError);
        result.ok = false;
        result.message = "MIDI export failed: could not replace output file: " + message;
        return result;
    }

    result.outputPath = outputPath.string();
    result.message = "MIDI export complete: " + outputPath.filename().string();
    return result;
}

MidiImportResult importProjectFromMidiData(const std::vector<uint8_t>& data) {
    MidiImportResult result;
    result.project = defaultProject();
    if (data.size() < 14 || std::string(reinterpret_cast<const char*>(data.data()), 4) != "MThd") {
        result.message = "MIDI import failed: missing MThd header.";
        return result;
    }
    uint32_t headerLength = 0;
    uint16_t format = 0;
    uint16_t trackCount = 0;
    uint16_t division = 0;
    if (!readU32At(data, 4, headerLength) || headerLength < 6 ||
        !readU16At(data, 8, format) || !readU16At(data, 10, trackCount) || !readU16At(data, 12, division)) {
        result.message = "MIDI import failed: malformed header.";
        return result;
    }
    if ((division & 0x8000) != 0 || division == 0) {
        result.message = "MIDI import failed: SMPTE time division is not supported yet.";
        return result;
    }
    result.ticksPerQuarter = division;

    struct ImportedNote {
        int sourceTrackIndex = 0;
        std::string trackName;
        int pitch = 60;
        int velocity = 96;
        int channel = 1;
        uint32_t startTick = 0;
        uint32_t endTick = 0;
    };
    struct ImportedControllerEvent {
        int sourceTrackIndex = 0;
        std::string trackName;
        int controller = 0;
        int value = 0;
        int channel = 1;
        uint32_t tick = 0;
    };
    struct ImportedPitchBendEvent {
        int sourceTrackIndex = 0;
        std::string trackName;
        int value = 8192;
        int channel = 1;
        uint32_t tick = 0;
    };
    struct ImportedProgramChangeEvent {
        int sourceTrackIndex = 0;
        std::string trackName;
        int program = 0;
        int channel = 1;
        uint32_t tick = 0;
    };
    struct ImportedTrackData {
        std::string trackName;
        std::vector<ImportedNote> notes;
        std::vector<ImportedControllerEvent> controllerEvents;
        std::vector<ImportedPitchBendEvent> pitchBendEvents;
        std::vector<ImportedProgramChangeEvent> programChangeEvents;
    };
    std::vector<ImportedNote> importedNotes;
    std::vector<ImportedControllerEvent> importedControllerEvents;
    std::vector<ImportedPitchBendEvent> importedPitchBendEvents;
    std::vector<ImportedProgramChangeEvent> importedProgramChangeEvents;
    std::vector<TempoMarkerState> tempos {{0.0, 120.0}};
    std::vector<TimeSignatureMarkerState> signatures {{0.0, 4, 4}};

    size_t offset = 8 + headerLength;
    for (uint16_t trackIndex = 0; trackIndex < trackCount && offset + 8 <= data.size(); ++trackIndex) {
        if (std::string(reinterpret_cast<const char*>(&data[offset]), 4) != "MTrk") {
            result.message = "MIDI import failed: missing MTrk chunk.";
            return result;
        }
        uint32_t length = 0;
        if (!readU32At(data, offset + 4, length) || offset + 8 + length > data.size()) {
            result.message = "MIDI import failed: malformed track length.";
            return result;
        }
        size_t cursor = offset + 8;
        const size_t end = cursor + length;
        offset = end;
        uint32_t tick = 0;
        uint8_t runningStatus = 0;
        std::string trackName = "MIDI " + std::to_string(trackIndex + 1);
        std::map<std::tuple<int, int>, std::vector<std::pair<uint32_t, int>>> activeNotes;

        while (cursor < end) {
            uint32_t delta = 0;
            if (!readVarLenAt(data, cursor, end, delta)) {
                break;
            }
            tick += delta;
            if (cursor >= end) {
                break;
            }
            uint8_t status = data[cursor++];
            if (status < 0x80) {
                if (runningStatus == 0) {
                    break;
                }
                --cursor;
                status = runningStatus;
            } else if (status < 0xf0) {
                runningStatus = status;
            }
            if (status == 0xff) {
                if (cursor >= end) {
                    break;
                }
                const uint8_t metaType = data[cursor++];
                uint32_t metaLength = 0;
                if (!readVarLenAt(data, cursor, end, metaLength) || cursor + metaLength > end) {
                    break;
                }
                if ((metaType == 0x03 || metaType == 0x04) && metaLength > 0) {
                    trackName.assign(reinterpret_cast<const char*>(&data[cursor]), reinterpret_cast<const char*>(&data[cursor + metaLength]));
                    if (trackName.empty()) {
                        trackName = "MIDI " + std::to_string(trackIndex + 1);
                    }
                } else if (metaType == 0x51 && metaLength == 3) {
                    const uint32_t mpq = (static_cast<uint32_t>(data[cursor]) << 16) |
                        (static_cast<uint32_t>(data[cursor + 1]) << 8) |
                        static_cast<uint32_t>(data[cursor + 2]);
                    if (mpq > 0) {
                        tempos.push_back({midiSecondsForTick(tempos, tick, division), 60000000.0 / static_cast<double>(mpq)});
                    }
                } else if (metaType == 0x58 && metaLength >= 2) {
                    const int numerator = std::max(1, std::min(127, static_cast<int>(data[cursor])));
                    const int denominator = 1 << std::max(0, std::min(6, static_cast<int>(data[cursor + 1])));
                    signatures.push_back({midiSecondsForTick(tempos, tick, division), numerator, denominator});
                } else if (metaType == 0x2f) {
                    cursor += metaLength;
                    break;
                }
                cursor += metaLength;
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                uint32_t sysexLength = 0;
                if (!readVarLenAt(data, cursor, end, sysexLength) || cursor + sysexLength > end) {
                    break;
                }
                cursor += sysexLength;
                continue;
            }
            const uint8_t kind = status & 0xf0;
            const int channel = (status & 0x0f) + 1;
            const int dataBytes = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
            if (cursor + static_cast<size_t>(dataBytes) > end) {
                break;
            }
            const int data1 = data[cursor++];
            const int data2 = dataBytes == 2 ? data[cursor++] : 0;
            if (kind == 0x90 && data2 > 0) {
                activeNotes[{channel, data1}].push_back({tick, data2});
            } else if (kind == 0x80 || (kind == 0x90 && data2 == 0)) {
                auto& stack = activeNotes[{channel, data1}];
                if (!stack.empty() && tick > stack.back().first) {
                    const auto started = stack.back();
                    stack.pop_back();
                    importedNotes.push_back({static_cast<int>(trackIndex), trackName, data1, started.second, channel, started.first, tick});
                }
            } else if (kind == 0xb0) {
                importedControllerEvents.push_back({static_cast<int>(trackIndex), trackName, data1, data2, channel, tick});
            } else if (kind == 0xe0) {
                const int value = std::max(0, std::min(16383, data1 | (data2 << 7)));
                importedPitchBendEvents.push_back({static_cast<int>(trackIndex), trackName, value, channel, tick});
            } else if (kind == 0xc0) {
                importedProgramChangeEvents.push_back({static_cast<int>(trackIndex), trackName, data1, channel, tick});
            }
        }
    }

    std::sort(tempos.begin(), tempos.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    std::sort(signatures.begin(), signatures.end(), [](const TimeSignatureMarkerState& left, const TimeSignatureMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    result.project.tempoMap = tempos;
    result.project.timeSignatureMap = signatures;
    if (!tempos.empty()) {
        result.project.tempoBpm = static_cast<int>(std::round(tempos.front().bpm));
    }
    if (!signatures.empty()) {
        result.project.timeSignatureNumerator = signatures.front().numerator;
        result.project.timeSignatureDenominator = signatures.front().denominator;
    }
    result.project.tracks.clear();
    std::map<int, ImportedTrackData> byTrack;
    for (const auto& note : importedNotes) {
        auto& trackData = byTrack[note.sourceTrackIndex];
        trackData.trackName = note.trackName;
        trackData.notes.push_back(note);
    }
    for (const auto& event : importedControllerEvents) {
        auto& trackData = byTrack[event.sourceTrackIndex];
        if (trackData.trackName.empty()) {
            trackData.trackName = event.trackName;
        }
        trackData.controllerEvents.push_back(event);
    }
    for (const auto& event : importedPitchBendEvents) {
        auto& trackData = byTrack[event.sourceTrackIndex];
        if (trackData.trackName.empty()) {
            trackData.trackName = event.trackName;
        }
        trackData.pitchBendEvents.push_back(event);
    }
    for (const auto& event : importedProgramChangeEvents) {
        auto& trackData = byTrack[event.sourceTrackIndex];
        if (trackData.trackName.empty()) {
            trackData.trackName = event.trackName;
        }
        trackData.programChangeEvents.push_back(event);
    }
    std::set<std::string> usedTrackNames;
    auto uniqueImportedTrackName = [&](const std::string& wanted, int fallbackIndex) {
        std::string base = wanted.empty() || protectedTrackName(wanted) ? "MIDI " + std::to_string(fallbackIndex) : wanted;
        std::string candidate = base;
        int suffix = 2;
        while (usedTrackNames.find(candidate) != usedTrackNames.end() || protectedTrackName(candidate)) {
            candidate = base + " " + std::to_string(suffix++);
        }
        usedTrackNames.insert(candidate);
        return candidate;
    };
    int trackSuffix = 1;
    for (auto& entry : byTrack) {
        auto& trackData = entry.second;
        if (trackData.notes.empty() &&
            trackData.controllerEvents.empty() &&
            trackData.pitchBendEvents.empty() &&
            trackData.programChangeEvents.empty()) {
            continue;
        }
        auto& notes = trackData.notes;
        std::sort(notes.begin(), notes.end(), [](const ImportedNote& left, const ImportedNote& right) {
            return left.startTick < right.startTick;
        });
        std::sort(trackData.controllerEvents.begin(), trackData.controllerEvents.end(), [](const ImportedControllerEvent& left, const ImportedControllerEvent& right) {
            return left.tick < right.tick;
        });
        std::sort(trackData.pitchBendEvents.begin(), trackData.pitchBendEvents.end(), [](const ImportedPitchBendEvent& left, const ImportedPitchBendEvent& right) {
            return left.tick < right.tick;
        });
        std::sort(trackData.programChangeEvents.begin(), trackData.programChangeEvents.end(), [](const ImportedProgramChangeEvent& left, const ImportedProgramChangeEvent& right) {
            return left.tick < right.tick;
        });
        TrackState track;
        track.name = uniqueImportedTrackName(trackData.trackName, trackSuffix);
        track.trackType = "midi";
        track.inputBus = "MIDI Input";
        track.outputBus = "Instrument";
        track.colorHex = "#4B84E8";
        result.project.tracks.push_back(track);

        uint32_t firstTick = std::numeric_limits<uint32_t>::max();
        uint32_t lastTick = 0;
        for (const auto& note : notes) {
            firstTick = std::min(firstTick, note.startTick);
            lastTick = std::max(lastTick, note.endTick);
        }
        for (const auto& event : trackData.controllerEvents) {
            firstTick = std::min(firstTick, event.tick);
            lastTick = std::max(lastTick, event.tick);
        }
        for (const auto& event : trackData.pitchBendEvents) {
            firstTick = std::min(firstTick, event.tick);
            lastTick = std::max(lastTick, event.tick);
        }
        for (const auto& event : trackData.programChangeEvents) {
            firstTick = std::min(firstTick, event.tick);
            lastTick = std::max(lastTick, event.tick);
        }
        if (firstTick == std::numeric_limits<uint32_t>::max()) {
            firstTick = 0;
        }
        const double startSeconds = midiSecondsForTick(tempos, firstTick, division);
        const double endSeconds = midiSecondsForTick(tempos, lastTick, division);
        const double regionBpm = std::max(20.0, std::min(400.0, projectTempoAtSeconds(result.project, startSeconds)));
        const double secondsPerBeat = 60.0 / regionBpm;
        MidiRegionState region;
        region.id = "midi-region-" + std::to_string(result.project.midiRegions.size() + 1);
        region.trackName = track.name;
        region.name = track.name;
        region.startSeconds = startSeconds;
        region.durationSeconds = std::max(0.05, endSeconds - startSeconds);
        region.ticksPerQuarter = division;
        region.colorHex = track.colorHex;
        int noteSuffix = 1;
        for (const auto& imported : notes) {
            MidiNoteState note;
            note.id = "note-" + std::to_string(noteSuffix++);
            note.pitch = std::max(0, std::min(127, imported.pitch));
            note.velocity = std::max(1, std::min(127, imported.velocity));
            note.channel = std::max(1, std::min(16, imported.channel));
            const double noteStartSeconds = midiSecondsForTick(tempos, imported.startTick, division);
            const double noteEndSeconds = midiSecondsForTick(tempos, imported.endTick, division);
            note.startBeats = std::max(0.0, (noteStartSeconds - startSeconds) / secondsPerBeat);
            note.durationBeats = std::max(1.0 / 960.0, (noteEndSeconds - noteStartSeconds) / secondsPerBeat);
            region.notes.push_back(note);
            ++result.noteCount;
        }
        int controllerSuffix = 1;
        for (const auto& imported : trackData.controllerEvents) {
            MidiControllerEventState event;
            event.id = "cc-" + std::to_string(controllerSuffix++);
            event.controller = std::max(0, std::min(127, imported.controller));
            event.value = std::max(0, std::min(127, imported.value));
            event.channel = std::max(1, std::min(16, imported.channel));
            const double eventSeconds = midiSecondsForTick(tempos, imported.tick, division);
            event.beat = std::max(0.0, (eventSeconds - startSeconds) / secondsPerBeat);
            region.controllerEvents.push_back(event);
            ++result.controllerEventCount;
        }
        int bendSuffix = 1;
        for (const auto& imported : trackData.pitchBendEvents) {
            MidiPitchBendEventState event;
            event.id = "bend-" + std::to_string(bendSuffix++);
            event.value = std::max(0, std::min(16383, imported.value));
            event.channel = std::max(1, std::min(16, imported.channel));
            const double eventSeconds = midiSecondsForTick(tempos, imported.tick, division);
            event.beat = std::max(0.0, (eventSeconds - startSeconds) / secondsPerBeat);
            region.pitchBendEvents.push_back(event);
            ++result.pitchBendEventCount;
        }
        int programSuffix = 1;
        for (const auto& imported : trackData.programChangeEvents) {
            MidiProgramChangeEventState event;
            event.id = "program-" + std::to_string(programSuffix++);
            event.program = std::max(0, std::min(127, imported.program));
            event.channel = std::max(1, std::min(16, imported.channel));
            const double eventSeconds = midiSecondsForTick(tempos, imported.tick, division);
            event.beat = std::max(0.0, (eventSeconds - startSeconds) / secondsPerBeat);
            region.programChangeEvents.push_back(event);
            ++result.programChangeEventCount;
        }
        result.project.midiRegions.push_back(region);
        ++result.trackCount;
        ++result.regionCount;
        ++trackSuffix;
    }
    TrackState master;
    master.name = "Master";
    master.trackType = "master";
    master.inputBus.clear();
    master.outputBus = "Monitor";
    TrackState monitor;
    monitor.name = "Monitor";
    monitor.trackType = "monitor";
    monitor.inputBus = "Monitor";
    monitor.outputBus = "Main 1-2";
    result.project.tracks.push_back(master);
    result.project.tracks.push_back(monitor);

    result.ok = result.noteCount > 0;
    result.message = result.ok
        ? "MIDI import complete with " + std::to_string(result.noteCount) + " note(s)."
        : "MIDI import found no note events.";
    return result;
}

MidiImportResult readProjectMidiFile(const std::filesystem::path& inputPath) {
    MidiImportResult result;
    if (inputPath.empty()) {
        result.message = "MIDI import failed: input path is empty.";
        return result;
    }
    std::ifstream in(inputPath, std::ios::binary);
    if (!in) {
        result.message = "MIDI import failed: could not open input file.";
        return result;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    result = importProjectFromMidiData(data);
    return result;
}

} // namespace neuracoust::daw
