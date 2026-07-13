#include "project/EditOperations.h"
#include "audio/WavFile.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace neuracoust::daw {

namespace {

constexpr size_t kMaxInstrumentRackSlots = 8;

std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isSignalGeneratorInsertName(const TrackInsertSlot& insert) {
    const std::string name = lowercaseCopy(insert.pluginName);
    const std::string className = lowercaseCopy(insert.pluginClassName);
    return name.find("emo-generator") != std::string::npos ||
        name.find("signal generator") != std::string::npos ||
        className.find("emo-generator") != std::string::npos ||
        className.find("signal generator") != std::string::npos;
}

void ensureSignalGeneratorDefaults(TrackInsertSlot& insert) {
    if (!isSignalGeneratorInsertName(insert)) {
        return;
    }
    const auto onOffIt = std::find_if(insert.parameters.begin(), insert.parameters.end(), [](const Vst3ParameterValueState& parameter) {
        return parameter.parameterId == 0u;
    });
    if (onOffIt == insert.parameters.end()) {
        insert.parameters.push_back({0u, "On Off", 1.0});
    }
}

ClipState* findClip(ProjectDocument& project, const std::string& clipId) {
    auto it = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == clipId;
    });
    return it == project.clips.end() ? nullptr : &(*it);
}

const ClipState* findClip(const ProjectDocument& project, const std::string& clipId) {
    auto it = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == clipId;
    });
    return it == project.clips.end() ? nullptr : &(*it);
}

double clipEnd(const ClipState& clip) {
    return clip.startSeconds + clip.durationSeconds;
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

VideoClipState* findVideoClip(ProjectDocument& project, const std::string& clipId) {
    auto it = std::find_if(project.videoClips.begin(), project.videoClips.end(), [&](const VideoClipState& clip) {
        return clip.id == clipId;
    });
    return it == project.videoClips.end() ? nullptr : &(*it);
}

double videoClipEnd(const VideoClipState& clip) {
    return clip.startSeconds + clip.durationSeconds;
}

bool clipsCanGlue(const ClipState& left, const ClipState& right) {
    constexpr double kEpsilon = 0.0005;
    if (left.locked || right.locked ||
        left.trackName != right.trackName ||
        left.sourcePath != right.sourcePath ||
        left.sourceFileUid != right.sourceFileUid ||
        left.sourceChannels != right.sourceChannels ||
        std::abs(left.sourceSampleRate - right.sourceSampleRate) > kEpsilon ||
        left.sourceBitsPerSample != right.sourceBitsPerSample ||
        left.sourceFloatingPoint != right.sourceFloatingPoint ||
        left.sourceHasBroadcastTimeReference != right.sourceHasBroadcastTimeReference ||
        left.sourceTimeReferenceSamples != right.sourceTimeReferenceSamples ||
        left.muted != right.muted ||
        left.polarityInverted != right.polarityInverted ||
        left.colorHex != right.colorHex) {
        return false;
    }
    if (std::abs(clipEnd(left) - right.startSeconds) > kEpsilon) {
        return false;
    }
    const double expectedRightOffset = left.sourceOffsetSeconds + left.durationSeconds;
    return std::abs(expectedRightOffset - right.sourceOffsetSeconds) <= kEpsilon;
}

void clampClipFades(ClipState& clip) {
    const double maxFade = std::max(0.0, clip.durationSeconds * 0.5);
    clip.fadeInSeconds = std::isfinite(clip.fadeInSeconds)
        ? std::max(0.0, std::min(clip.fadeInSeconds, maxFade))
        : 0.0;
    clip.fadeOutSeconds = std::isfinite(clip.fadeOutSeconds)
        ? std::max(0.0, std::min(clip.fadeOutSeconds, maxFade))
        : 0.0;
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

bool clipIdExists(const ProjectDocument& project, const std::string& clipId) {
    return std::any_of(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == clipId;
    });
}

size_t removePlaylistPlacementsForClip(ProjectDocument& project, const std::string& clipId) {
    if (clipId.empty()) {
        return 0;
    }
    size_t removed = 0;
    for (auto& playlist : project.trackPlaylists) {
        const auto before = playlist.placements.size();
        playlist.placements.erase(
            std::remove_if(playlist.placements.begin(), playlist.placements.end(), [&](const PlaylistClipPlacementState& placement) {
                return placement.id == clipId || placement.legacyClipId == clipId;
            }),
            playlist.placements.end());
        removed += before - playlist.placements.size();
    }
    return removed;
}

bool syncClipGainToActivePlaylistPlacement(ProjectDocument& project, const ClipState& clip) {
    if (clip.id.empty()) {
        return false;
    }
    bool synced = false;
    for (auto& playlist : project.trackPlaylists) {
        if (!playlist.active) {
            continue;
        }
        if (!clip.trackName.empty() && !playlist.trackName.empty() && playlist.trackName != clip.trackName) {
            continue;
        }
        for (auto& placement : playlist.placements) {
            if (placement.id == clip.id || placement.legacyClipId == clip.id) {
                placement.gainDb = clip.gainDb;
                synced = true;
            }
        }
    }
    return synced;
}

std::string trim(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) {
        return !isSpace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) {
        return !isSpace(ch);
    }).base(), value.end());
    return value;
}

bool videoSourceIdExists(const ProjectDocument& project, const std::string& sourceId) {
    return std::any_of(project.videoSources.begin(), project.videoSources.end(), [&](const VideoSourceState& source) {
        return source.id == sourceId;
    });
}

bool videoClipIdExists(const ProjectDocument& project, const std::string& clipId) {
    return std::any_of(project.videoClips.begin(), project.videoClips.end(), [&](const VideoClipState& clip) {
        return clip.id == clipId;
    });
}

bool playlistIdExists(const ProjectDocument& project, const std::string& playlistId) {
    return std::any_of(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& playlist) {
        return playlist.id == playlistId;
    });
}

std::string uniqueTrackPlaylistId(const ProjectDocument& project, const std::string& trackName) {
    std::string stem = "playlist";
    for (const unsigned char ch : trackName) {
        if (std::isalnum(ch) != 0) {
            stem.push_back(static_cast<char>(std::tolower(ch)));
        } else if (stem.back() != '-') {
            stem.push_back('-');
        }
    }
    while (!stem.empty() && stem.back() == '-') {
        stem.pop_back();
    }
    if (stem.empty()) {
        stem = "playlist";
    }
    int suffix = 1;
    while (true) {
        std::string candidate = stem + "-" + std::to_string(suffix++);
        if (!playlistIdExists(project, candidate)) {
            return candidate;
        }
    }
}

TrackPlaylistState* activePlaylistForTrack(ProjectDocument& project, const std::string& trackName) {
    auto it = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](TrackPlaylistState& playlist) {
        return playlist.trackName == trackName && playlist.active;
    });
    if (it != project.trackPlaylists.end()) {
        return &(*it);
    }
    it = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](TrackPlaylistState& playlist) {
        return playlist.trackName == trackName;
    });
    return it == project.trackPlaylists.end() ? nullptr : &(*it);
}

bool markerIdExists(const ProjectDocument& project, const std::string& markerId) {
    return std::any_of(project.markers.begin(), project.markers.end(), [&](const MarkerState& marker) {
        return marker.id == markerId;
    });
}

bool chordEventIdExists(const ProjectDocument& project, const std::string& chordId) {
    return std::any_of(project.chordEvents.begin(), project.chordEvents.end(), [&](const ChordEventState& chord) {
        return chord.id == chordId;
    });
}

bool lyricEventIdExists(const ProjectDocument& project, const std::string& lyricId) {
    return std::any_of(project.lyricEvents.begin(), project.lyricEvents.end(), [&](const LyricEventState& lyric) {
        return lyric.id == lyricId;
    });
}

bool midiRegionIdExists(const ProjectDocument& project, const std::string& regionId) {
    return std::any_of(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
        return region.id == regionId;
    });
}

bool midiNoteIdExists(const MidiRegionState& region, const std::string& noteId) {
    return std::any_of(region.notes.begin(), region.notes.end(), [&](const MidiNoteState& note) {
        return note.id == noteId;
    });
}

bool midiControllerEventIdExists(const MidiRegionState& region, const std::string& eventId) {
    return std::any_of(region.controllerEvents.begin(), region.controllerEvents.end(), [&](const MidiControllerEventState& event) {
        return event.id == eventId;
    });
}

bool midiPitchBendEventIdExists(const MidiRegionState& region, const std::string& eventId) {
    return std::any_of(region.pitchBendEvents.begin(), region.pitchBendEvents.end(), [&](const MidiPitchBendEventState& event) {
        return event.id == eventId;
    });
}

bool midiProgramChangeEventIdExists(const MidiRegionState& region, const std::string& eventId) {
    return std::any_of(region.programChangeEvents.begin(), region.programChangeEvents.end(), [&](const MidiProgramChangeEventState& event) {
        return event.id == eventId;
    });
}

std::string uniqueMarkerId(const ProjectDocument& project) {
    int suffix = 1;
    while (true) {
        std::string candidate = "marker-" + std::to_string(suffix++);
        if (!markerIdExists(project, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueChordEventId(const ProjectDocument& project) {
    int suffix = 1;
    while (true) {
        std::string candidate = "chord-" + std::to_string(suffix++);
        if (!chordEventIdExists(project, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueMidiRegionId(const ProjectDocument& project) {
    int suffix = 1;
    while (true) {
        std::string candidate = "midi-region-" + std::to_string(suffix++);
        if (!midiRegionIdExists(project, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueMidiNoteId(const MidiRegionState& region) {
    int suffix = 1;
    while (true) {
        std::string candidate = "note-" + std::to_string(suffix++);
        if (!midiNoteIdExists(region, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueMidiControllerEventId(const MidiRegionState& region) {
    int suffix = 1;
    while (true) {
        std::string candidate = "cc-" + std::to_string(suffix++);
        if (!midiControllerEventIdExists(region, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueMidiPitchBendEventId(const MidiRegionState& region) {
    int suffix = 1;
    while (true) {
        std::string candidate = "bend-" + std::to_string(suffix++);
        if (!midiPitchBendEventIdExists(region, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueMidiProgramChangeEventId(const MidiRegionState& region) {
    int suffix = 1;
    while (true) {
        std::string candidate = "program-" + std::to_string(suffix++);
        if (!midiProgramChangeEventIdExists(region, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueLyricEventId(const ProjectDocument& project) {
    int suffix = 1;
    while (true) {
        std::string candidate = "lyric-" + std::to_string(suffix++);
        if (!lyricEventIdExists(project, candidate)) {
            return candidate;
        }
    }
}

std::string trimmedLyricText(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string uniqueSplitId(const ProjectDocument& project, const std::string& baseClipId) {
    std::string candidate = baseClipId + "-split";
    int suffix = 2;
    while (clipIdExists(project, candidate)) {
        candidate = baseClipId + "-split-" + std::to_string(suffix++);
    }
    return candidate;
}

std::string uniqueVideoSplitId(const ProjectDocument& project, const std::string& baseClipId) {
    std::string candidate = baseClipId + "-split";
    int suffix = 2;
    while (videoClipIdExists(project, candidate)) {
        candidate = baseClipId + "-split-" + std::to_string(suffix++);
    }
    return candidate;
}

std::string uniqueClipId(const ProjectDocument& project, const std::string& baseClipId) {
    std::string stem;
    stem.reserve(baseClipId.size());
    for (const unsigned char ch : baseClipId) {
        if (std::isalnum(ch) != 0) {
            stem.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!stem.empty() && stem.back() != '-') {
            stem.push_back('-');
        }
    }
    while (!stem.empty() && stem.back() == '-') {
        stem.pop_back();
    }
    if (stem.empty()) {
        stem = "clip";
    }
    std::string candidate = stem;
    int suffix = 2;
    while (clipIdExists(project, candidate)) {
        candidate = stem + "-" + std::to_string(suffix++);
    }
    return candidate;
}

std::string uniqueDuplicateId(const ProjectDocument& project, const std::string& baseClipId) {
    std::string candidate = baseClipId + "-copy";
    int suffix = 2;
    while (clipIdExists(project, candidate)) {
        candidate = baseClipId + "-copy-" + std::to_string(suffix++);
    }
    return candidate;
}

std::string pathStemForRegionName(const std::string& sourcePath) {
    const std::filesystem::path path(sourcePath);
    const auto stem = path.stem().string();
    if (!stem.empty()) {
        return stem;
    }
    const auto filename = path.filename().string();
    return filename.empty() ? "Audio Clip" : filename;
}

std::string sourceUidForPath(const std::string& sourcePath) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : sourcePath) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "src-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

void populateClipSourceMetadata(ClipState& clip) {
    clip.sourceChannels = 0;
    clip.sourceSampleRate = 0.0;
    clip.sourceBitsPerSample = 0;
    clip.sourceFloatingPoint = false;
    clip.sourceHasBroadcastTimeReference = false;
    clip.sourceTimeReferenceSamples = 0;
    clip.sourceTimeReferenceSeconds = 0.0;
    if (clip.sourcePath.empty()) {
        return;
    }
    WavAudioData audio;
    std::string error;
    if (!readPcmWavFile(clip.sourcePath, audio, error)) {
        return;
    }
    clip.sourceChannels = std::max(0, audio.channels);
    clip.sourceSampleRate = std::max(0, audio.sampleRate);
    clip.sourceBitsPerSample = std::max(0, audio.bitsPerSample);
    clip.sourceFloatingPoint = audio.floatingPoint;
    clip.sourceHasBroadcastTimeReference = audio.hasBroadcastTimeReference;
    clip.sourceTimeReferenceSamples = audio.broadcastTimeReferenceSamples;
    clip.sourceTimeReferenceSeconds = audio.broadcastTimeReferenceSeconds;
}

std::string uniqueImportedClipId(const ProjectDocument& project) {
    int suffix = 1;
    while (true) {
        std::string candidate = "clip-" + std::to_string(suffix++);
        if (!clipIdExists(project, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueVideoSourceId(const ProjectDocument& project) {
    int suffix = 1;
    while (true) {
        std::string candidate = "video-src-" + std::to_string(suffix++);
        if (!videoSourceIdExists(project, candidate)) {
            return candidate;
        }
    }
}

std::string uniqueVideoClipId(const ProjectDocument& project) {
    int suffix = 1;
    while (true) {
        std::string candidate = "video-clip-" + std::to_string(suffix++);
        if (!videoClipIdExists(project, candidate)) {
            return candidate;
        }
    }
}

std::string automaticClipColorForTrack(const ProjectDocument& project, const TrackState* track) {
    static const char* colors[] = {"#35BFA8", "#4B84E8", "#F0B84D", "#D86BA6", "#7CCB5E", "#A078E8", "#E26D5A", "#5BC0DE"};
    if (track == nullptr) {
        return colors[project.clips.size() % (sizeof(colors) / sizeof(colors[0]))];
    }
    size_t sameTrackClips = 0;
    for (const auto& clip : project.clips) {
        if (clip.trackName == track->name) {
            ++sameTrackClips;
        }
    }
    if (sameTrackClips == 0 && !track->colorHex.empty()) {
        return track->colorHex;
    }
    size_t trackColorIndex = 0;
    for (size_t index = 0; index < sizeof(colors) / sizeof(colors[0]); ++index) {
        if (track->colorHex == colors[index]) {
            trackColorIndex = index;
            break;
        }
    }
    return colors[(trackColorIndex + sameTrackClips) % (sizeof(colors) / sizeof(colors[0]))];
}

bool isProtectedTrackName(const std::string& trackName) {
    return trackName == "Master" || trackName == "Monitor";
}

bool isExternalPluginInsertFormat(const std::string& format) {
    return format == "VST3" || format == "VST3/AU" || format == "Audio Unit";
}

bool isFolderTrackType(const TrackState& track) {
    return track.trackType == "folder" || track.trackType == "bus_folder";
}

bool trackBelongsToFolderBlock(const TrackState& track, const std::string& folderName) {
    return track.name == folderName || (!folderName.empty() && track.folderName == folderName);
}

bool trackNameExists(const ProjectDocument& project, const std::string& trackName) {
    return std::any_of(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
}

MidiRegionState* findMidiRegion(ProjectDocument& project, const std::string& regionId) {
    auto it = std::find_if(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
        return region.id == regionId;
    });
    return it == project.midiRegions.end() ? nullptr : &(*it);
}

MidiNoteState* findMidiNote(MidiRegionState& region, const std::string& noteId) {
    auto it = std::find_if(region.notes.begin(), region.notes.end(), [&](const MidiNoteState& note) {
        return note.id == noteId;
    });
    return it == region.notes.end() ? nullptr : &(*it);
}

std::string trimTrackName(std::string name) {
    auto first = std::find_if_not(name.begin(), name.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    auto last = std::find_if_not(name.rbegin(), name.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool trackHasClips(const ProjectDocument& project, const std::string& trackName) {
    return std::any_of(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.trackName == trackName;
    });
}

bool trackHasMidiRegions(const ProjectDocument& project, const std::string& trackName) {
    return std::any_of(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
        return region.trackName == trackName;
    });
}

bool clipIsLocked(const ClipState* clip) {
    return clip != nullptr && clip->locked;
}

std::string nextAudioTrackName(const ProjectDocument& project) {
    int index = 1;
    while (true) {
        std::ostringstream name;
        name << "Audio " << index++;
        if (!trackNameExists(project, name.str())) {
            return name.str();
        }
    }
}

std::string nextTrackNameWithPrefix(const ProjectDocument& project, const std::string& prefix) {
    for (int suffix = 1; suffix < 100000; ++suffix) {
        const std::string candidate = prefix + " " + std::to_string(suffix);
        if (!trackNameExists(project, candidate)) {
            return candidate;
        }
    }
    return prefix;
}

std::string uniqueTrackCopyName(const ProjectDocument& project, const std::string& sourceTrackName) {
    const std::string base = trimTrackName(sourceTrackName).empty() ? "Track" : trimTrackName(sourceTrackName);
    std::string candidate = base + " Copy";
    int suffix = 2;
    while (trackNameExists(project, candidate) || isProtectedTrackName(candidate)) {
        candidate = base + " Copy " + std::to_string(suffix++);
    }
    return candidate;
}

double projectEndSeconds(const ProjectDocument& project) {
    double endSeconds = 0.0;
    for (const auto& clip : project.clips) {
        endSeconds = std::max(endSeconds, clipEnd(clip));
    }
    return endSeconds;
}

TrackState* findTrack(ProjectDocument& project, const std::string& trackName) {
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    return it == project.tracks.end() ? nullptr : &(*it);
}

TrackInsertSlot normalizedTrackInsert(TrackInsertSlot insert) {
    if (trimTrackName(insert.pluginName).empty() || insert.pluginName == "No Insert") {
        insert.pluginName = "No Insert";
        insert.pluginFormat = "None";
        insert.pluginPath.clear();
        insert.pluginClassId.clear();
        insert.pluginClassName.clear();
        insert.enabled = false;
        insert.bypassed = false;
        insert.dspExecutionMode = "native";
        insert.assignedDspServerId.clear();
        insert.serverModuleId.clear();
        insert.reportedLatencySamples = 0;
        insert.dspAvailable = true;
        insert.dspLastError.clear();
        return insert;
    }
    if (trimTrackName(insert.pluginFormat).empty()) {
        insert.pluginFormat = "Unknown";
    }
    ensureSignalGeneratorDefaults(insert);
    if (!(insert.dspExecutionMode == "native" ||
          insert.dspExecutionMode == "internal" ||
          insert.dspExecutionMode == "remote_internal" ||
          insert.dspExecutionMode == "external")) {
        insert.dspExecutionMode = "native";
    }
    if (!isRemoteInternalDspExecutionMode(insert.dspExecutionMode)) {
        insert.assignedDspServerId.clear();
    }
    if (insert.dspExecutionMode == "native") {
        insert.reportedLatencySamples = 0;
        insert.dspAvailable = true;
        insert.dspLastError.clear();
    }
    insert.enabled = true;
    return insert;
}

bool trackInsertHasPlugin(const TrackInsertSlot& insert) {
    return insert.enabled && !insert.pluginName.empty() && insert.pluginName != "No Insert";
}

InstrumentSlotState normalizedInstrumentSlot(InstrumentSlotState instrument) {
    if (trimTrackName(instrument.pluginName).empty() || instrument.pluginName == "No Instrument") {
        instrument.pluginName = "No Instrument";
        instrument.pluginFormat = "None";
        instrument.pluginPath.clear();
        instrument.enabled = false;
        instrument.bypassed = false;
        instrument.soloed = false;
        instrument.midiInput = "MIDI Input";
        instrument.midiChannel = 0;
        instrument.reportedLatencySamples = 0;
        instrument.parameters.clear();
        return instrument;
    }
    if (trimTrackName(instrument.pluginFormat).empty()) {
        instrument.pluginFormat = "VST3";
    }
    if (trimTrackName(instrument.midiInput).empty()) {
        instrument.midiInput = "MIDI Input";
    }
    instrument.midiChannel = std::max(0, std::min(16, instrument.midiChannel));
    instrument.enabled = true;
    return instrument;
}

bool instrumentSlotHasPlugin(const InstrumentSlotState& instrument) {
    return instrument.enabled && !instrument.pluginName.empty() && instrument.pluginName != "No Instrument";
}

void compactAndSyncInstrumentRack(TrackState& track) {
    if (track.instrumentSlots.size() > kMaxInstrumentRackSlots) {
        track.instrumentSlots.resize(kMaxInstrumentRackSlots);
    }
    for (auto& slot : track.instrumentSlots) {
        slot = normalizedInstrumentSlot(slot);
    }
    while (!track.instrumentSlots.empty() && !instrumentSlotHasPlugin(track.instrumentSlots.back())) {
        track.instrumentSlots.pop_back();
    }
    track.instrument = track.instrumentSlots.empty()
        ? normalizedInstrumentSlot(InstrumentSlotState {})
        : track.instrumentSlots.front();
    if (track.instrumentRackMode != "serial" && track.instrumentRackMode != "parallel") {
        track.instrumentRackMode = "parallel";
    }
}

TrackSendState normalizedTrackSend(TrackSendState send) {
    send.busName = trimTrackName(send.busName);
    if (send.busName.empty()) {
        send.enabled = false;
    }
    send.gainDb = std::isfinite(send.gainDb) ? std::max(-60.0f, std::min(12.0f, send.gainDb)) : 0.0f;
    send.pan = std::isfinite(send.pan) ? std::max(-1.0f, std::min(1.0f, send.pan)) : 0.0f;
    return send;
}

bool busIsUsedByAnySend(const ProjectDocument& project, const std::string& busName) {
    return std::any_of(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return std::any_of(track.sends.begin(), track.sends.end(), [&](const TrackSendState& send) {
            return send.busName == busName;
        });
    });
}

bool isTimelineTargetTrackType(const TrackState& track) {
    return track.trackType == "audio" ||
        track.trackType == "aux" ||
        track.trackType == "bus_folder" ||
        track.trackType == "midi" ||
        track.trackType == "instrument";
}

bool trackSupportsSignalControls(const TrackState& track) {
    return track.trackType != "folder";
}

bool trackSupportsAudioRoutingControls(const TrackState& track) {
    return trackSupportsSignalControls(track) && track.trackType != "vca";
}

bool trackSupportsPhysicalInputMonitoring(const TrackState& track) {
    return track.trackType == "audio" ||
        track.trackType == "aux" ||
        track.trackType == "bus_folder";
}

bool trackSupportsVcaControlAssignment(const TrackState& track) {
    return track.trackType == "audio" ||
        track.trackType == "aux" ||
        track.trackType == "bus_folder" ||
        track.trackType == "instrument";
}

bool trackWouldSendToOwnInput(const TrackState& track, const TrackSendState& send) {
    return !track.inputBus.empty() && !send.busName.empty() && track.inputBus == send.busName;
}

bool musicalGridSpec(const std::string& gridUnit, double& beatMultiplier, int& subdivisionsPerBeat) {
    if (gridUnit == "1 bar") {
        beatMultiplier = 4.0;
        subdivisionsPerBeat = 0;
        return true;
    }
    if (gridUnit == "1 beat") {
        beatMultiplier = 1.0;
        subdivisionsPerBeat = 1;
        return true;
    }
    if (gridUnit == "1/4 beat") {
        beatMultiplier = 0.25;
        subdivisionsPerBeat = 4;
        return true;
    }
    if (gridUnit == "1/8 beat") {
        beatMultiplier = 0.125;
        subdivisionsPerBeat = 8;
        return true;
    }
    if (gridUnit == "1/16 beat") {
        beatMultiplier = 0.0625;
        subdivisionsPerBeat = 16;
        return true;
    }
    return false;
}

double musicalGridStepSecondsAt(const ProjectDocument& project, double timeSeconds, double beatMultiplier) {
    const double tempo = std::max(20.0, std::min(400.0, projectTempoAtSeconds(project, timeSeconds)));
    return std::max(0.01, (60.0 / tempo) * beatMultiplier);
}

double quarterNotesPerBar(const ProjectDocument& project) {
    const double numerator = static_cast<double>(std::max(1, project.timeSignatureNumerator));
    const double denominator = static_cast<double>(std::max(1, project.timeSignatureDenominator));
    return std::max(0.25, numerator * (4.0 / denominator));
}

double musicalGridBeatMultiplierForProject(const ProjectDocument& project, const std::string& gridUnit, double beatMultiplier) {
    return gridUnit == "1 bar" ? quarterNotesPerBar(project) : beatMultiplier;
}

void removeUnusedAuxForBus(ProjectDocument& project, const std::string& busName) {
    if (busName.empty() || busIsUsedByAnySend(project, busName)) {
        return;
    }
    auto auxIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.inputBus == busName &&
            track.name.rfind("Aux ", 0) == 0 &&
            !trackHasClips(project, track.name);
    });
    if (auxIt != project.tracks.end()) {
        project.tracks.erase(auxIt);
    }
}

} // namespace

double projectTimelineQuantumSeconds(const ProjectDocument& project) {
    const double beatSeconds = project.tempoBpm > 0
        ? std::max(0.01, 60.0 / static_cast<double>(project.tempoBpm))
        : 0.5;
    if (project.editMode == "Grid") {
        if (project.gridUnit == "0.1s") {
            return 0.1;
        }
        if (project.gridUnit == "1 frame") {
            return projectFrameDurationSeconds(project);
        }
        if (project.gridUnit == "1 bar") {
            return beatSeconds * quarterNotesPerBar(project);
        }
        if (project.gridUnit == "1 beat") {
            return beatSeconds;
        }
        if (project.gridUnit == "1/4 beat") {
            return beatSeconds * 0.25;
        }
        if (project.gridUnit == "1/8 beat") {
            return beatSeconds * 0.125;
        }
        if (project.gridUnit == "1/16 beat") {
            return beatSeconds * 0.0625;
        }
        return 1.0;
    }
    return project.beatSnapEnabled ? beatSeconds : 0.1;
}

double projectVideoFrameRate(const ProjectDocument& project) {
    return std::isfinite(project.videoFrameRate)
        ? std::max(1.0, std::min(240.0, project.videoFrameRate))
        : 30.0;
}

double projectFrameDurationSeconds(const ProjectDocument& project) {
    return 1.0 / projectVideoFrameRate(project);
}

std::string projectTimecodeString(const ProjectDocument& project, double seconds) {
    const double frameRate = projectVideoFrameRate(project);
    const long long nominalFramesPerSecond = std::max<long long>(1, static_cast<long long>(std::llround(frameRate)));
    const double timelineSeconds = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    const double absoluteSeconds = std::max(0.0, project.timecodeStartSeconds + timelineSeconds);
    if (project.timecodeDropFrame && frameRateSupportsDropFrame(frameRate)) {
        const long long realFrames = static_cast<long long>(std::llround(absoluteSeconds * frameRate));
        const long long labelFrames = dropFrameLabelFrames(realFrames, nominalFramesPerSecond);
        const long long frame = labelFrames % nominalFramesPerSecond;
        const long long totalSeconds = labelFrames / nominalFramesPerSecond;
        const long long wholeSeconds = totalSeconds % 60;
        const long long minutes = (totalSeconds / 60) % 60;
        const long long hours = (totalSeconds / 3600) % 24;
        std::ostringstream out;
        out << std::setfill('0')
            << std::setw(2) << hours << ":"
            << std::setw(2) << minutes << ":"
            << std::setw(2) << wholeSeconds << ";"
            << std::setw(2) << frame;
        return out.str();
    }
    long long totalWholeSeconds = static_cast<long long>(std::floor(absoluteSeconds));
    long long frame = static_cast<long long>(std::llround((absoluteSeconds - static_cast<double>(totalWholeSeconds)) * frameRate));
    if (frame >= nominalFramesPerSecond) {
        frame = 0;
        ++totalWholeSeconds;
    }
    const long long wholeSeconds = totalWholeSeconds % 60;
    const long long minutes = (totalWholeSeconds / 60) % 60;
    const long long hours = totalWholeSeconds / 3600;

    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(2) << hours << ":"
        << std::setw(2) << minutes << ":"
        << std::setw(2) << wholeSeconds << ":"
        << std::setw(2) << frame;
    return out.str();
}

double projectTempoAtSeconds(const ProjectDocument& project, double seconds) {
    const double fallbackBpm = project.tempoBpm > 0
        ? std::max(20.0, std::min(400.0, static_cast<double>(project.tempoBpm)))
        : 120.0;
    if (!std::isfinite(seconds) || project.tempoMap.empty()) {
        return fallbackBpm;
    }

    const double safeSeconds = std::max(0.0, seconds);
    const TempoMarkerState* left = nullptr;
    const TempoMarkerState* right = nullptr;
    for (const auto& marker : project.tempoMap) {
        if (!std::isfinite(marker.timeSeconds) || !std::isfinite(marker.bpm) ||
            marker.timeSeconds < 0.0 || marker.bpm < 20.0 || marker.bpm > 400.0) {
            continue;
        }
        if (marker.timeSeconds <= safeSeconds) {
            if (left == nullptr || marker.timeSeconds >= left->timeSeconds) {
                left = &marker;
            }
        } else if (right == nullptr || marker.timeSeconds < right->timeSeconds) {
            right = &marker;
        }
    }
    if (left == nullptr && right == nullptr) {
        return fallbackBpm;
    }
    if (left == nullptr) {
        return right->bpm;
    }
    if (right == nullptr) {
        return left->bpm;
    }
    const double span = right->timeSeconds - left->timeSeconds;
    if (span <= 0.0) {
        return right->bpm;
    }
    const double t = std::max(0.0, std::min(1.0, (safeSeconds - left->timeSeconds) / span));
    return left->bpm + (right->bpm - left->bpm) * t;
}

namespace {

bool validTempoForTimeScale(double bpm) {
    return std::isfinite(bpm) && bpm >= 20.0 && bpm <= 400.0;
}

double safeClipTimeScale(double timeScale) {
    return std::max(0.05, std::min(20.0, std::isfinite(timeScale) ? timeScale : 1.0));
}

bool isPendingProjectTempoStretch(const ClipState& clip) {
    return clip.pendingTimeStretchToProject || clip.tempoSyncPolicy == "stretch-to-project";
}

bool applyClipTimeScaleToProjectTempoInPlace(ProjectDocument& project,
                                             ClipState& clip,
                                             std::string& message) {
    if (clip.locked) {
        message = "Clip is locked";
        return false;
    }
    const double sourceTempo = clip.sourceTempoBpm;
    const double projectTempo = projectTempoAtSeconds(project, clip.startSeconds);
    if (!validTempoForTimeScale(sourceTempo) || !validTempoForTimeScale(projectTempo)) {
        message = "Clip/project tempo information is not usable";
        return false;
    }

    const double oldScale = safeClipTimeScale(clip.timeScale);
    const double sourceCoverageSeconds = clip.durationSeconds / oldScale;
    const double nextScale = safeClipTimeScale(sourceTempo / projectTempo);
    clip.timeScale = nextScale;
    clip.durationSeconds = std::max(0.01, sourceCoverageSeconds * nextScale);
    clip.tempoSyncPolicy = "project-tempo";
    clip.pendingTimeStretchToProject = false;

    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << "Applied time scale " << sourceTempo << " -> " << projectTempo
        << " BPM";
    out << std::defaultfloat << " x" << nextScale;
    message = out.str();
    return true;
}

}

bool applyClipTimeScaleToProjectTempo(ProjectDocument& project,
                                      const std::string& clipId,
                                      std::string& message) {
    message.clear();
    ClipState* clip = findClip(project, clipId);
    if (clip == nullptr) {
        message = "Clip not found";
        return false;
    }
    return applyClipTimeScaleToProjectTempoInPlace(project, *clip, message);
}

bool applyPendingClipTimeScaleToProjectTempo(ProjectDocument& project,
                                             ClipTimeScaleApplyResult& result) {
    result = {};
    for (auto& clip : project.clips) {
        if (!isPendingProjectTempoStretch(clip)) {
            continue;
        }
        std::string message;
        if (applyClipTimeScaleToProjectTempoInPlace(project, clip, message)) {
            ++result.changedClips;
            result.changedClipIds.push_back(clip.id);
        } else {
            ++result.skippedClips;
        }
    }

    std::ostringstream out;
    if (result.changedClips == 0) {
        out << "No pending time-stretch clips were applied";
    } else {
        out << "Applied pending time-stretch to " << result.changedClips << " clip";
        if (result.changedClips != 1) {
            out << "s";
        }
    }
    if (result.skippedClips > 0) {
        out << " · skipped " << result.skippedClips;
    }
    result.message = out.str();
    return result.changedClips > 0;
}

double snapProjectTime(const ProjectDocument& project, double seconds) {
    if (!std::isfinite(seconds)) {
        return 0.0;
    }
    if (project.editMode == "Grid") {
        double beatMultiplier = 1.0;
        int subdivisionsPerBeat = 1;
        if (musicalGridSpec(project.gridUnit, beatMultiplier, subdivisionsPerBeat)) {
            beatMultiplier = musicalGridBeatMultiplierForProject(project, project.gridUnit, beatMultiplier);
            const double safeSeconds = std::max(0.0, seconds);
            double previous = 0.0;
            double current = 0.0;
            size_t guard = 0;
            while (current < safeSeconds && guard < 1000000) {
                previous = current;
                current += musicalGridStepSecondsAt(project, current, beatMultiplier);
                ++guard;
            }
            if (guard >= 1000000) {
                return safeSeconds;
            }
            return std::max(0.0, (safeSeconds - previous) <= (current - safeSeconds) ? previous : current);
        }
    }
    const double tempoAtTime = projectTempoAtSeconds(project, seconds);
    const double beatSeconds = tempoAtTime > 0.0 ? std::max(0.01, 60.0 / tempoAtTime) : 0.5;
    double quantum = projectTimelineQuantumSeconds(project);
    if (project.editMode == "Grid") {
        if (project.gridUnit == "1 bar") {
            quantum = beatSeconds * quarterNotesPerBar(project);
        } else if (project.gridUnit == "1 beat") {
            quantum = beatSeconds;
        } else if (project.gridUnit == "1/4 beat") {
            quantum = beatSeconds * 0.25;
        } else if (project.gridUnit == "1/8 beat") {
            quantum = beatSeconds * 0.125;
        } else if (project.gridUnit == "1/16 beat") {
            quantum = beatSeconds * 0.0625;
        }
    } else if (project.beatSnapEnabled) {
        quantum = beatSeconds;
    }
    return std::max(0.0, std::round(seconds / quantum) * quantum);
}

std::vector<TimelineGridLine> projectMusicalGridLines(const ProjectDocument& project,
                                                      const std::string& gridUnit,
                                                      double visibleStartSeconds,
                                                      double visibleEndSeconds,
                                                      size_t maxLines) {
    std::vector<TimelineGridLine> lines;
    if (!std::isfinite(visibleStartSeconds) || !std::isfinite(visibleEndSeconds) ||
        visibleEndSeconds <= visibleStartSeconds || maxLines == 0) {
        return lines;
    }

    double beatMultiplier = 1.0;
    int subdivisionsPerBeat = 1;
    if (!musicalGridSpec(gridUnit, beatMultiplier, subdivisionsPerBeat)) {
        return lines;
    }

    beatMultiplier = musicalGridBeatMultiplierForProject(project, gridUnit, beatMultiplier);
    const int quarterNotesInBar = std::max(1, static_cast<int>(std::round(quarterNotesPerBar(project))));
    const int subdivisionsPerBar = subdivisionsPerBeat > 0 ? subdivisionsPerBeat * quarterNotesInBar : 1;
    const double start = std::max(0.0, visibleStartSeconds);
    const double end = std::max(start, visibleEndSeconds);
    const bool useVisibleRangeApproximation = project.tempoMap.empty() ||
        project.tempoMap.size() > 96 ||
        (end - start) > 180.0;
    if (useVisibleRangeApproximation) {
        const double sourceTempo = project.tempoMap.empty()
            ? static_cast<double>(project.tempoBpm > 0 ? project.tempoBpm : 120)
            : projectTempoAtSeconds(project, start);
        const double tempo = std::max(20.0, std::min(400.0, sourceTempo));
        const double step = std::max(0.01, (60.0 / tempo) * beatMultiplier);
        const int firstIndex = static_cast<int>(std::max(0.0, std::floor(start / step)));
        double time = static_cast<double>(firstIndex) * step;
        int subdivisionIndex = firstIndex;
        const size_t visibleLineBudget = std::min<size_t>(maxLines, 360);
        lines.reserve(std::min<size_t>(visibleLineBudget, static_cast<size_t>(std::ceil((end - start) / step)) + 2));
        while (time <= end && lines.size() < visibleLineBudget) {
            const bool isBar = subdivisionsPerBeat == 0 || (subdivisionIndex % subdivisionsPerBar) == 0;
            const bool isBeat = subdivisionsPerBeat > 0 && (subdivisionIndex % subdivisionsPerBeat) == 0;
            TimelineGridLine line;
            line.timeSeconds = std::max(0.0, time);
            line.bar = isBar;
            line.beat = isBeat;
            const int beatsPerBar = subdivisionsPerBeat > 0
                ? std::max(1, subdivisionsPerBar / std::max(1, subdivisionsPerBeat))
                : 1;
            line.barNumber = subdivisionsPerBeat == 0 ? subdivisionIndex + 1 : (subdivisionIndex / subdivisionsPerBar) + 1;
            line.beatInBar = subdivisionsPerBeat > 0 ? ((subdivisionIndex / std::max(1, subdivisionsPerBeat)) % beatsPerBar) + 1 : 1;
            lines.push_back(line);
            time += step;
            ++subdivisionIndex;
        }
        return lines;
    }
    double time = 0.0;
    int subdivisionIndex = 0;
    size_t guard = 0;
    while (time <= end && lines.size() < maxLines && guard < 1000000) {
        if (time >= start - 0.0001) {
            const bool isBar = subdivisionsPerBeat == 0 || (subdivisionIndex % subdivisionsPerBar) == 0;
            const bool isBeat = subdivisionsPerBeat > 0 && (subdivisionIndex % subdivisionsPerBeat) == 0;
            TimelineGridLine line;
            line.timeSeconds = std::max(0.0, time);
            line.bar = isBar;
            line.beat = isBeat;
            const int beatsPerBar = subdivisionsPerBeat > 0
                ? std::max(1, subdivisionsPerBar / std::max(1, subdivisionsPerBeat))
                : 1;
            line.barNumber = subdivisionsPerBeat == 0 ? subdivisionIndex + 1 : (subdivisionIndex / subdivisionsPerBar) + 1;
            line.beatInBar = subdivisionsPerBeat > 0 ? ((subdivisionIndex / std::max(1, subdivisionsPerBeat)) % beatsPerBar) + 1 : 1;
            lines.push_back(line);
        }
        time += musicalGridStepSecondsAt(project, time, beatMultiplier);
        ++subdivisionIndex;
        ++guard;
    }
    return lines;
}

bool moveClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    clip->startSeconds = newStartSeconds;
    return true;
}

bool moveVideoClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds) {
    auto* clip = findVideoClip(project, clipId);
    if (clip == nullptr || clip->locked || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    clip->startSeconds = newStartSeconds;
    return true;
}

bool shuffleMoveClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds, const std::string& newTrackName) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    const std::string sourceTrack = clip->trackName;
    const std::string targetTrack = newTrackName.empty() ? sourceTrack : trimTrackName(newTrackName);
    if (isProtectedTrackName(targetTrack) || !trackNameExists(project, targetTrack)) {
        return false;
    }

    const double oldStart = clip->startSeconds;
    const double duration = std::max(0.0, clip->durationSeconds);
    const double oldEnd = oldStart + duration;
    const double insertStart = std::max(0.0, newStartSeconds);
    bool changed = false;

    if (targetTrack == sourceTrack) {
        if (std::abs(insertStart - oldStart) < 0.0000001) {
            return false;
        }
        if (insertStart > oldStart) {
            const double insertionPoint = std::max(oldStart, insertStart - duration);
            for (auto& other : project.clips) {
                if (other.id == clipId || other.locked || other.trackName != sourceTrack) {
                    continue;
                }
                if (other.startSeconds >= oldEnd && other.startSeconds < insertStart + 0.0000001) {
                    other.startSeconds = std::max(0.0, other.startSeconds - duration);
                    changed = true;
                }
            }
            clip = findClip(project, clipId);
            if (clip == nullptr || clipIsLocked(clip)) {
                return changed;
            }
            clip->startSeconds = insertionPoint;
            changed = true;
        } else {
            for (auto& other : project.clips) {
                if (other.id == clipId || other.locked || other.trackName != sourceTrack) {
                    continue;
                }
                if (other.startSeconds >= insertStart && other.startSeconds < oldStart) {
                    other.startSeconds += duration;
                    changed = true;
                }
            }
            clip = findClip(project, clipId);
            if (clip == nullptr || clipIsLocked(clip)) {
                return changed;
            }
            clip->startSeconds = insertStart;
            changed = true;
        }
        return changed;
    }

    for (auto& other : project.clips) {
        if (other.id == clipId || other.locked) {
            continue;
        }
        if (other.trackName == sourceTrack && other.startSeconds >= oldEnd) {
            other.startSeconds = std::max(0.0, other.startSeconds - duration);
            changed = true;
        } else if (other.trackName == targetTrack && other.startSeconds >= insertStart) {
            other.startSeconds += duration;
            changed = true;
        }
    }
    clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip)) {
        return changed;
    }
    clip->trackName = targetTrack;
    clip->startSeconds = insertStart;
    return true;
}

bool nudgeClip(ProjectDocument& project, const std::string& clipId, double deltaSeconds) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || !std::isfinite(deltaSeconds)) {
        return false;
    }
    const double nextStart = std::max(0.0, clip->startSeconds + deltaSeconds);
    if (std::abs(nextStart - clip->startSeconds) < 0.0000001) {
        return false;
    }
    clip->startSeconds = nextStart;
    return true;
}

bool nextClipBoundaryAfter(const ProjectDocument& project,
                           double seconds,
                           double& boundarySeconds,
                           const std::string& trackName) {
    if (!std::isfinite(seconds)) {
        return false;
    }
    const double epsilon = 0.000001;
    const double current = std::max(0.0, seconds);
    double best = std::numeric_limits<double>::infinity();
    for (const auto& clip : project.clips) {
        if (!trackName.empty() && clip.trackName != trackName) {
            continue;
        }
        const double start = clip.startSeconds;
        const double end = clipEnd(clip);
        if (std::isfinite(start) && start > current + epsilon) {
            best = std::min(best, start);
        }
        if (std::isfinite(end) && end > current + epsilon) {
            best = std::min(best, end);
        }
    }
    if (!std::isfinite(best)) {
        return false;
    }
    boundarySeconds = best;
    return true;
}

bool previousClipBoundaryBefore(const ProjectDocument& project,
                                double seconds,
                                double& boundarySeconds,
                                const std::string& trackName) {
    if (!std::isfinite(seconds)) {
        return false;
    }
    const double epsilon = 0.000001;
    const double current = std::max(0.0, seconds);
    double best = -1.0;
    for (const auto& clip : project.clips) {
        if (!trackName.empty() && clip.trackName != trackName) {
            continue;
        }
        const double start = clip.startSeconds;
        const double end = clipEnd(clip);
        if (std::isfinite(start) && start < current - epsilon) {
            best = std::max(best, std::max(0.0, start));
        }
        if (std::isfinite(end) && end < current - epsilon) {
            best = std::max(best, std::max(0.0, end));
        }
    }
    if (best < 0.0) {
        return false;
    }
    boundarySeconds = best;
    return true;
}

bool trimClipStart(ProjectDocument& project, const std::string& clipId, double newStartSeconds) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    const double end = clipEnd(*clip);
    if (newStartSeconds >= end) {
        return false;
    }
    const double delta = newStartSeconds - clip->startSeconds;
    const double nextSourceOffset = clip->sourceOffsetSeconds + delta;
    if (nextSourceOffset < 0.0) {
        return false;
    }
    clip->startSeconds = newStartSeconds;
    clip->sourceOffsetSeconds = nextSourceOffset;
    clip->durationSeconds = end - newStartSeconds;
    clampClipFades(*clip);
    return true;
}

bool trimClipEnd(ProjectDocument& project, const std::string& clipId, double newEndSeconds) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || !std::isfinite(newEndSeconds) || newEndSeconds <= clip->startSeconds) {
        return false;
    }
    clip->durationSeconds = newEndSeconds - clip->startSeconds;
    clampClipFades(*clip);
    return true;
}

bool trimVideoClipStart(ProjectDocument& project, const std::string& clipId, double newStartSeconds) {
    auto* clip = findVideoClip(project, clipId);
    if (clip == nullptr || clip->locked || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    const double end = videoClipEnd(*clip);
    if (newStartSeconds >= end) {
        return false;
    }
    const double delta = newStartSeconds - clip->startSeconds;
    const double nextSourceOffset = clip->sourceOffsetSeconds + delta;
    if (nextSourceOffset < 0.0) {
        return false;
    }
    clip->startSeconds = newStartSeconds;
    clip->sourceOffsetSeconds = nextSourceOffset;
    clip->durationSeconds = end - newStartSeconds;
    return true;
}

bool trimVideoClipEnd(ProjectDocument& project, const std::string& clipId, double newEndSeconds) {
    auto* clip = findVideoClip(project, clipId);
    if (clip == nullptr || clip->locked || !std::isfinite(newEndSeconds) || newEndSeconds <= clip->startSeconds) {
        return false;
    }
    clip->durationSeconds = newEndSeconds - clip->startSeconds;
    return true;
}

bool deleteVideoClip(ProjectDocument& project, const std::string& clipId) {
    auto it = std::find_if(project.videoClips.begin(), project.videoClips.end(), [&](const VideoClipState& clip) {
        return clip.id == clipId;
    });
    if (it == project.videoClips.end() || it->locked) {
        return false;
    }
    const std::string sourceId = it->sourceId;
    project.videoClips.erase(it);
    const bool sourceStillUsed = std::any_of(project.videoClips.begin(), project.videoClips.end(), [&](const VideoClipState& clip) {
        return clip.sourceId == sourceId;
    });
    if (!sourceStillUsed) {
        project.videoSources.erase(std::remove_if(project.videoSources.begin(), project.videoSources.end(), [&](const VideoSourceState& source) {
            return source.id == sourceId;
        }), project.videoSources.end());
    }
    return true;
}

bool setVideoClipMuted(ProjectDocument& project, const std::string& clipId, bool muted) {
    auto* clip = findVideoClip(project, clipId);
    if (clip == nullptr || clip->locked) {
        return false;
    }
    clip->muted = muted;
    return true;
}

bool setVideoClipLocked(ProjectDocument& project, const std::string& clipId, bool locked) {
    auto* clip = findVideoClip(project, clipId);
    if (clip == nullptr) {
        return false;
    }
    clip->locked = locked;
    return true;
}

bool setVideoClipName(ProjectDocument& project, const std::string& clipId, const std::string& name) {
    auto* clip = findVideoClip(project, clipId);
    if (clip == nullptr || clip->locked) {
        return false;
    }
    const std::string trimmed = trimTrackName(name);
    if (trimmed.empty()) {
        return false;
    }
    clip->name = trimmed;
    return true;
}

bool relinkVideoSource(ProjectDocument& project,
                       const std::string& sourceId,
                       const std::string& sourcePath,
                       const std::string& displayName) {
    auto it = std::find_if(project.videoSources.begin(), project.videoSources.end(), [&](const VideoSourceState& source) {
        return source.id == sourceId;
    });
    if (it == project.videoSources.end() || sourcePath.empty()) {
        return false;
    }
    it->path = sourcePath;
    if (!displayName.empty()) {
        it->displayName = displayName;
    }
    return true;
}

bool splitVideoClip(ProjectDocument& project, const std::string& clipId, double splitSeconds, std::string& newClipId) {
    newClipId.clear();
    auto* clip = findVideoClip(project, clipId);
    if (clip == nullptr || clip->locked || !std::isfinite(splitSeconds)) {
        return false;
    }
    const double end = videoClipEnd(*clip);
    if (splitSeconds <= clip->startSeconds + 0.0005 || splitSeconds >= end - 0.0005) {
        return false;
    }

    VideoClipState right = *clip;
    right.id = uniqueVideoSplitId(project, clip->id);
    right.startSeconds = splitSeconds;
    right.sourceOffsetSeconds += splitSeconds - clip->startSeconds;
    right.durationSeconds = end - splitSeconds;

    clip->durationSeconds = splitSeconds - clip->startSeconds;
    newClipId = right.id;
    project.videoClips.push_back(right);
    return true;
}

bool splitClip(ProjectDocument& project, const std::string& clipId, double splitSeconds, std::string& newClipId) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || !std::isfinite(splitSeconds)) {
        return false;
    }
    const double end = clipEnd(*clip);
    if (splitSeconds <= clip->startSeconds || splitSeconds >= end) {
        return false;
    }

    const double originalEnd = end;
    ClipState right = *clip;
    right.id = uniqueSplitId(project, clip->id);
    right.startSeconds = splitSeconds;
    right.sourceOffsetSeconds += splitSeconds - clip->startSeconds;
    right.durationSeconds = originalEnd - splitSeconds;

    clip->durationSeconds = splitSeconds - clip->startSeconds;
    clampClipFades(*clip);
    clampClipFades(right);
    newClipId = right.id;
    project.clips.push_back(right);
    return true;
}

bool glueAdjacentClip(ProjectDocument& project, const std::string& clipId, std::string& gluedClipId) {
    gluedClipId.clear();
    auto selectedIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == clipId;
    });
    if (selectedIt == project.clips.end() || selectedIt->locked) {
        return false;
    }

    auto bestIt = project.clips.end();
    bool selectedIsLeft = false;
    for (auto it = project.clips.begin(); it != project.clips.end(); ++it) {
        if (it == selectedIt) {
            continue;
        }
        if (clipsCanGlue(*selectedIt, *it)) {
            bestIt = it;
            selectedIsLeft = true;
            break;
        }
        if (clipsCanGlue(*it, *selectedIt)) {
            bestIt = it;
            selectedIsLeft = false;
            break;
        }
    }

    if (bestIt == project.clips.end()) {
        return false;
    }

    const ClipState left = selectedIsLeft ? *selectedIt : *bestIt;
    const ClipState right = selectedIsLeft ? *bestIt : *selectedIt;
    auto leftIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == left.id;
    });
    auto rightIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == right.id;
    });
    if (leftIt == project.clips.end() || rightIt == project.clips.end()) {
        return false;
    }

    leftIt->durationSeconds = std::max(0.0, clipEnd(right) - left.startSeconds);
    leftIt->fadeOutSeconds = right.fadeOutSeconds;
    leftIt->fadeOutCurve = right.fadeOutCurve;
    clampClipFades(*leftIt);
    gluedClipId = leftIt->id;

    project.clips.erase(rightIt);
    return true;
}

bool glueSelectedAdjacentClips(ProjectDocument& project,
                               const std::string& firstClipId,
                               const std::string& secondClipId,
                               std::string& gluedClipId) {
    gluedClipId.clear();
    if (firstClipId.empty() || secondClipId.empty() || firstClipId == secondClipId) {
        return false;
    }

    auto firstIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == firstClipId;
    });
    auto secondIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == secondClipId;
    });
    if (firstIt == project.clips.end() || secondIt == project.clips.end() ||
        firstIt->locked || secondIt->locked) {
        return false;
    }

    const ClipState first = *firstIt;
    const ClipState second = *secondIt;
    bool firstIsLeft = false;
    if (clipsCanGlue(first, second)) {
        firstIsLeft = true;
    } else if (clipsCanGlue(second, first)) {
        firstIsLeft = false;
    } else {
        return false;
    }

    const ClipState left = firstIsLeft ? first : second;
    const ClipState right = firstIsLeft ? second : first;
    auto leftIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == left.id;
    });
    auto rightIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == right.id;
    });
    if (leftIt == project.clips.end() || rightIt == project.clips.end()) {
        return false;
    }

    leftIt->durationSeconds = std::max(0.0, clipEnd(right) - left.startSeconds);
    leftIt->fadeOutSeconds = right.fadeOutSeconds;
    leftIt->fadeOutCurve = right.fadeOutCurve;
    clampClipFades(*leftIt);
    gluedClipId = leftIt->id;
    project.clips.erase(rightIt);
    return true;
}

bool glueClipRange(ProjectDocument& project,
                   double rangeStartSeconds,
                   double rangeEndSeconds,
                   std::vector<std::string>& gluedClipIds) {
    gluedClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }

    constexpr double kEpsilon = 0.0005;
    bool changed = false;
    const size_t maxPasses = std::max<size_t>(project.clips.size() * 2, 1);
    for (size_t pass = 0; pass < maxPasses; ++pass) {
        std::vector<size_t> order(project.clips.size());
        for (size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::sort(order.begin(), order.end(), [&](size_t lhsIndex, size_t rhsIndex) {
            const auto& lhs = project.clips[lhsIndex];
            const auto& rhs = project.clips[rhsIndex];
            if (lhs.trackName != rhs.trackName) {
                return lhs.trackName < rhs.trackName;
            }
            if (std::abs(lhs.startSeconds - rhs.startSeconds) > kEpsilon) {
                return lhs.startSeconds < rhs.startSeconds;
            }
            return lhs.id < rhs.id;
        });

        bool gluedOne = false;
        for (size_t position = 1; position < order.size(); ++position) {
            const auto& left = project.clips[order[position - 1]];
            const auto& right = project.clips[order[position]];
            const double boundary = clipEnd(left);
            if (boundary < start - kEpsilon || boundary > end + kEpsilon) {
                continue;
            }
            if (!clipsCanGlue(left, right)) {
                continue;
            }

            std::string gluedClipId;
            if (glueAdjacentClip(project, left.id, gluedClipId)) {
                if (!gluedClipId.empty()) {
                    gluedClipIds.push_back(gluedClipId);
                }
                changed = true;
                gluedOne = true;
                break;
            }
        }
        if (!gluedOne) {
            break;
        }
    }
    return changed;
}

bool duplicateClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds, std::string& newClipId) {
    newClipId.clear();
    const auto* clip = findClip(project, clipId);
    if (clip == nullptr || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }

    ClipState copy = *clip;
    copy.id = uniqueDuplicateId(project, clip->id);
    copy.startSeconds = newStartSeconds;
    newClipId = copy.id;
    project.clips.push_back(copy);
    return true;
}

bool duplicateClipToTrack(ProjectDocument& project,
                          const std::string& clipId,
                          double newStartSeconds,
                          const std::string& targetTrackName,
                          std::string& newClipId) {
    newClipId.clear();
    const auto* clip = findClip(project, clipId);
    if (clip == nullptr || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    const std::string targetTrack = targetTrackName.empty() ? clip->trackName : trimTrackName(targetTrackName);
    if (isProtectedTrackName(targetTrack) || !trackNameExists(project, targetTrack)) {
        return false;
    }

    ClipState copy = *clip;
    copy.id = uniqueDuplicateId(project, clip->id);
    copy.trackName = targetTrack;
    copy.startSeconds = newStartSeconds;
    clampClipFades(copy);
    newClipId = copy.id;
    project.clips.push_back(copy);
    return true;
}

bool shuffleDuplicateClip(ProjectDocument& project,
                          const std::string& clipId,
                          double newStartSeconds,
                          const std::string& newTrackName,
                          std::string& newClipId) {
    newClipId.clear();
    const auto* clip = findClip(project, clipId);
    if (clip == nullptr || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    const std::string targetTrack = newTrackName.empty() ? clip->trackName : trimTrackName(newTrackName);
    if (isProtectedTrackName(targetTrack) || !trackNameExists(project, targetTrack)) {
        return false;
    }

    const double insertStart = std::max(0.0, newStartSeconds);
    const double duration = std::max(0.0, clip->durationSeconds);
    for (auto& other : project.clips) {
        if (other.locked || other.trackName != targetTrack || other.startSeconds < insertStart) {
            continue;
        }
        other.startSeconds += duration;
    }

    ClipState copy = *clip;
    copy.id = uniqueDuplicateId(project, clip->id);
    copy.trackName = targetTrack;
    copy.startSeconds = insertStart;
    clampClipFades(copy);
    newClipId = copy.id;
    project.clips.push_back(copy);
    return true;
}

bool pasteClip(ProjectDocument& project, const ClipState& sourceClip, double newStartSeconds, std::string& newClipId) {
    if (sourceClip.id.empty() || sourceClip.sourcePath.empty() ||
        sourceClip.durationSeconds <= 0.0 || !std::isfinite(sourceClip.durationSeconds) ||
        newStartSeconds < 0.0 || !std::isfinite(newStartSeconds) ||
        isProtectedTrackName(sourceClip.trackName) || !trackNameExists(project, sourceClip.trackName)) {
        return false;
    }

    ClipState copy = sourceClip;
    copy.id = uniqueDuplicateId(project, sourceClip.id);
    copy.startSeconds = newStartSeconds;
    clampClipFades(copy);
    newClipId = copy.id;
    project.clips.push_back(copy);
    return true;
}

bool deleteClip(ProjectDocument& project, const std::string& clipId) {
    const auto* target = findClip(project, clipId);
    if (clipIsLocked(target)) {
        return false;
    }
    const auto before = project.clips.size();
    project.clips.erase(
        std::remove_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
            return clip.id == clipId;
        }),
        project.clips.end());
    const size_t removedPlaylistPlacements = removePlaylistPlacementsForClip(project, clipId);
    return project.clips.size() != before || removedPlaylistPlacements > 0;
}

bool clearClipRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds) {
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }

    std::vector<ClipState> nextClips;
    nextClips.reserve(project.clips.size());
    bool changed = false;
    for (const auto& clip : project.clips) {
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        if (clip.locked || clipEndSeconds <= start || clipStart >= end) {
            nextClips.push_back(clip);
            continue;
        }

        changed = true;
        if (clipStart < start) {
            ClipState left = clip;
            left.durationSeconds = start - clipStart;
            clampClipFades(left);
            if (left.durationSeconds > 0.0) {
                nextClips.push_back(left);
            }
        }

        if (clipEndSeconds > end) {
            ClipState right = clip;
            right.id = uniqueSplitId(project, clip.id);
            right.startSeconds = end;
            right.sourceOffsetSeconds += end - clipStart;
            right.durationSeconds = clipEndSeconds - end;
            clampClipFades(right);
            if (right.durationSeconds > 0.0) {
                nextClips.push_back(right);
            }
        }
    }

    if (changed) {
        project.clips = std::move(nextClips);
    }
    return changed;
}

bool shuffleDeleteClipRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds) {
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }

    const bool cleared = clearClipRange(project, start, end);
    const double delta = end - start;
    bool moved = false;
    for (auto& clip : project.clips) {
        if (!clip.locked && clip.startSeconds >= end) {
            clip.startSeconds = std::max(0.0, clip.startSeconds - delta);
            moved = true;
        }
    }
    return cleared || moved;
}

bool separateClipRange(ProjectDocument& project,
                       double rangeStartSeconds,
                       double rangeEndSeconds,
                       std::vector<std::string>& newClipIds) {
    newClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }

    std::vector<std::string> originalClipIds;
    originalClipIds.reserve(project.clips.size());
    for (const auto& clip : project.clips) {
        if (clip.locked) {
            continue;
        }
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        if ((start > clipStart && start < clipEndSeconds) ||
            (end > clipStart && end < clipEndSeconds)) {
            originalClipIds.push_back(clip.id);
        }
    }

    bool changed = false;
    for (const auto& clipId : originalClipIds) {
        auto* clip = findClip(project, clipId);
        if (clip == nullptr) {
            continue;
        }
        std::string splitId;
        const double currentEnd = clipEnd(*clip);
        if (end > clip->startSeconds && end < currentEnd && splitClip(project, clipId, end, splitId)) {
            newClipIds.push_back(splitId);
            changed = true;
        }
        clip = findClip(project, clipId);
        if (clip == nullptr) {
            continue;
        }
        if (start > clip->startSeconds && start < clipEnd(*clip) && splitClip(project, clipId, start, splitId)) {
            newClipIds.push_back(splitId);
            changed = true;
        }
    }
    return changed;
}

std::vector<ClipState> copyClipRange(const ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds) {
    std::vector<ClipState> copied;
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return copied;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return copied;
    }

    for (const auto& clip : project.clips) {
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        const double overlapStart = std::max(start, clipStart);
        const double overlapEnd = std::min(end, clipEndSeconds);
        if (overlapEnd <= overlapStart) {
            continue;
        }
        ClipState copy = clip;
        copy.startSeconds = overlapStart - start;
        copy.sourceOffsetSeconds += overlapStart - clipStart;
        copy.durationSeconds = overlapEnd - overlapStart;
        clampClipFades(copy);
        if (copy.durationSeconds > 0.0) {
            copied.push_back(copy);
        }
    }
    std::sort(copied.begin(), copied.end(), [](const ClipState& left, const ClipState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.trackName < right.trackName;
        }
        return left.startSeconds < right.startSeconds;
    });
    return copied;
}

bool cutClipRange(ProjectDocument& project,
                  double rangeStartSeconds,
                  double rangeEndSeconds,
                  std::vector<ClipState>& copiedClips) {
    copiedClips.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }

    ProjectDocument editableOnly = project;
    editableOnly.clips.erase(
        std::remove_if(editableOnly.clips.begin(), editableOnly.clips.end(), [](const ClipState& clip) {
            return clip.locked;
        }),
        editableOnly.clips.end());
    copiedClips = copyClipRange(editableOnly, start, end);
    if (copiedClips.empty()) {
        return false;
    }
    if (!clearClipRange(project, start, end)) {
        copiedClips.clear();
        return false;
    }
    return true;
}

bool pasteClipRange(ProjectDocument& project,
                    const std::vector<ClipState>& sourceClips,
                    double startSeconds,
                    std::vector<std::string>& newClipIds) {
    newClipIds.clear();
    if (sourceClips.empty() || startSeconds < 0.0 || !std::isfinite(startSeconds)) {
        return false;
    }

    std::vector<ClipState> copies;
    copies.reserve(sourceClips.size());
    for (const auto& sourceClip : sourceClips) {
        if (sourceClip.id.empty() || sourceClip.sourcePath.empty() ||
            sourceClip.durationSeconds <= 0.0 || !std::isfinite(sourceClip.durationSeconds) ||
            isProtectedTrackName(sourceClip.trackName) || !trackNameExists(project, sourceClip.trackName)) {
            newClipIds.clear();
            return false;
        }
        ClipState copy = sourceClip;
        copy.id = uniqueDuplicateId(project, sourceClip.id);
        copy.startSeconds = startSeconds + std::max(0.0, sourceClip.startSeconds);
        clampClipFades(copy);
        copies.push_back(copy);
        newClipIds.push_back(copy.id);
    }

    project.clips.insert(project.clips.end(), copies.begin(), copies.end());
    return true;
}

bool duplicateClipRange(ProjectDocument& project,
                        double rangeStartSeconds,
                        double rangeEndSeconds,
                        std::vector<std::string>& newClipIds) {
    newClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }
    const auto sourceClips = copyClipRange(project, start, end);
    if (sourceClips.empty()) {
        return false;
    }
    return pasteClipRange(project, sourceClips, end, newClipIds);
}

bool trimClipRangeToSelection(ProjectDocument& project,
                              double rangeStartSeconds,
                              double rangeEndSeconds,
                              std::vector<std::string>& keptClipIds) {
    keptClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }

    std::vector<ClipState> nextClips;
    nextClips.reserve(project.clips.size());
    bool changed = false;
    for (const auto& clip : project.clips) {
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        const double overlapStart = std::max(start, clipStart);
        const double overlapEnd = std::min(end, clipEndSeconds);

        if (clip.locked) {
            nextClips.push_back(clip);
            if (overlapEnd > overlapStart) {
                keptClipIds.push_back(clip.id);
            }
            continue;
        }

        if (overlapEnd <= overlapStart) {
            changed = true;
            continue;
        }

        ClipState kept = clip;
        kept.startSeconds = overlapStart;
        kept.sourceOffsetSeconds += overlapStart - clipStart;
        kept.durationSeconds = overlapEnd - overlapStart;
        clampClipFades(kept);
        if (kept.durationSeconds > 0.0) {
            if (std::abs(kept.startSeconds - clip.startSeconds) > 0.0000001 ||
                std::abs(kept.durationSeconds - clip.durationSeconds) > 0.0000001 ||
                std::abs(kept.sourceOffsetSeconds - clip.sourceOffsetSeconds) > 0.0000001) {
                changed = true;
            }
            keptClipIds.push_back(kept.id);
            nextClips.push_back(kept);
        } else {
            changed = true;
        }
    }

    if (changed) {
        project.clips = std::move(nextClips);
    }
    return changed && !keptClipIds.empty();
}

bool quantizeClipStartsInRange(ProjectDocument& project,
                               double rangeStartSeconds,
                               double rangeEndSeconds,
                               double quantumSeconds,
                               std::vector<std::string>& changedClipIds) {
    changedClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds) ||
        !std::isfinite(quantumSeconds) || quantumSeconds <= 0.0) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start) {
        return false;
    }

    for (auto& clip : project.clips) {
        if (clip.locked || clip.startSeconds < start || clip.startSeconds >= end) {
            continue;
        }
        const double quantized = std::max(0.0, std::round(clip.startSeconds / quantumSeconds) * quantumSeconds);
        if (std::abs(quantized - clip.startSeconds) > 0.0000001) {
            clip.startSeconds = quantized;
            changedClipIds.push_back(clip.id);
        }
    }
    return !changedClipIds.empty();
}

bool setEditSelectionRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds) {
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start + 0.000001) {
        return false;
    }
    project.editSelectionEnabled = true;
    project.editSelectionStartSeconds = start;
    project.editSelectionEndSeconds = end;
    return true;
}

bool projectClipTimeRange(const ProjectDocument& project, double& rangeStartSeconds, double& rangeEndSeconds) {
    double start = std::numeric_limits<double>::infinity();
    double end = 0.0;
    for (const auto& clip : project.clips) {
        if (!std::isfinite(clip.startSeconds) ||
            !std::isfinite(clip.durationSeconds) ||
            clip.durationSeconds <= 0.0) {
            continue;
        }
        start = std::min(start, clip.startSeconds);
        end = std::max(end, clipEnd(clip));
    }
    if (!std::isfinite(start) || end <= start + 0.01) {
        return false;
    }
    rangeStartSeconds = start;
    rangeEndSeconds = end;
    return true;
}

bool setEditSelectionToAdjacentClipBoundary(ProjectDocument& project,
                                            double seconds,
                                            bool forward,
                                            double& boundarySeconds,
                                            const std::string& trackName) {
    if (!std::isfinite(seconds)) {
        return false;
    }
    const double origin = std::max(0.0, seconds);
    const bool found = forward
        ? nextClipBoundaryAfter(project, origin, boundarySeconds, trackName)
        : previousClipBoundaryBefore(project, origin, boundarySeconds, trackName);
    if (!found) {
        return false;
    }
    return setEditSelectionRange(project, origin, boundarySeconds);
}

bool setEditSelectionToSurroundingClipBoundaries(ProjectDocument& project,
                                                 double seconds,
                                                 double& rangeStartSeconds,
                                                 double& rangeEndSeconds,
                                                 const std::string& trackName) {
    if (!std::isfinite(seconds)) {
        return false;
    }
    const double origin = std::max(0.0, seconds);
    double previousBoundary = 0.0;
    double nextBoundary = 0.0;
    if (!previousClipBoundaryBefore(project, origin, previousBoundary, trackName) ||
        !nextClipBoundaryAfter(project, origin, nextBoundary, trackName) ||
        !setEditSelectionRange(project, previousBoundary, nextBoundary)) {
        return false;
    }
    rangeStartSeconds = project.editSelectionStartSeconds;
    rangeEndSeconds = project.editSelectionEndSeconds;
    return true;
}

bool setEditSelectionToSurroundingMarkers(ProjectDocument& project,
                                          double seconds,
                                          double& rangeStartSeconds,
                                          double& rangeEndSeconds) {
    if (!std::isfinite(seconds) || project.markers.size() < 2) {
        return false;
    }
    constexpr double epsilon = 0.0005;
    const double origin = std::max(0.0, seconds);
    double previousMarker = -1.0;
    double nextMarker = std::numeric_limits<double>::infinity();
    for (const auto& marker : project.markers) {
        if (!std::isfinite(marker.timeSeconds)) {
            continue;
        }
        if (marker.timeSeconds < origin - epsilon) {
            previousMarker = std::max(previousMarker, std::max(0.0, marker.timeSeconds));
        }
        if (marker.timeSeconds > origin + epsilon) {
            nextMarker = std::min(nextMarker, marker.timeSeconds);
        }
    }
    if (previousMarker < 0.0 || !std::isfinite(nextMarker) ||
        !setEditSelectionRange(project, previousMarker, nextMarker)) {
        return false;
    }
    rangeStartSeconds = project.editSelectionStartSeconds;
    rangeEndSeconds = project.editSelectionEndSeconds;
    return true;
}

bool setEditSelectionToClip(ProjectDocument& project, const std::string& clipId) {
    const auto* clip = findClip(project, clipId);
    if (clip == nullptr || clip->durationSeconds <= 0.0) {
        return false;
    }
    return setEditSelectionRange(project, clip->startSeconds, clip->startSeconds + clip->durationSeconds);
}

bool editSelectionMatchesClip(const ProjectDocument& project, const std::string& clipId, double epsilonSeconds) {
    const auto* clip = findClip(project, clipId);
    if (clip == nullptr || clip->durationSeconds <= 0.0 || !project.editSelectionEnabled) {
        return false;
    }
    const double epsilon = std::max(0.0, epsilonSeconds);
    return std::abs(project.editSelectionStartSeconds - clip->startSeconds) <= epsilon &&
        std::abs(project.editSelectionEndSeconds - (clip->startSeconds + clip->durationSeconds)) <= epsilon;
}

bool clearEditSelection(ProjectDocument& project) {
    const bool changed = project.editSelectionEnabled ||
        project.editSelectionStartSeconds != 0.0 ||
        project.editSelectionEndSeconds != 0.0;
    project.editSelectionEnabled = false;
    project.editSelectionStartSeconds = 0.0;
    project.editSelectionEndSeconds = 0.0;
    return changed;
}

bool toggleEditSelectionToClip(ProjectDocument& project, const std::string& clipId, bool clearMatchingLoop) {
    const auto* clip = findClip(project, clipId);
    if (clip == nullptr || clip->durationSeconds <= 0.0) {
        return false;
    }
    const double clipStart = clip->startSeconds;
    const double clipEnd = clip->startSeconds + clip->durationSeconds;
    if (editSelectionMatchesClip(project, clipId)) {
        const bool changed = clearEditSelection(project);
        if (clearMatchingLoop &&
            project.loopEnabled &&
            std::abs(project.loopStartSeconds - clipStart) <= 0.001 &&
            std::abs(project.loopEndSeconds - clipEnd) <= 0.001) {
            return clearLoopRange(project) || changed;
        }
        return changed;
    }
    const bool changed = setEditSelectionRange(project, clipStart, clipEnd);
    if (project.loopEnabled) {
        project.loopStartSeconds = project.editSelectionStartSeconds;
        project.loopEndSeconds = project.editSelectionEndSeconds;
    }
    return changed;
}

bool setLoopRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds) {
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds)) {
        return false;
    }
    const double start = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double end = std::max(rangeStartSeconds, rangeEndSeconds);
    if (end <= start + 0.000001) {
        return false;
    }
    project.loopEnabled = true;
    project.loopStartSeconds = start;
    project.loopEndSeconds = end;
    return true;
}

bool setLoopToEditSelection(ProjectDocument& project) {
    if (!project.editSelectionEnabled) {
        return false;
    }
    return setLoopRange(project, project.editSelectionStartSeconds, project.editSelectionEndSeconds);
}

bool setLoopToClip(ProjectDocument& project, const std::string& clipId) {
    const auto* clip = findClip(project, clipId);
    if (clip == nullptr || clip->durationSeconds <= 0.0) {
        return false;
    }
    if (!setEditSelectionToClip(project, clipId)) {
        return false;
    }
    return setLoopRange(project, clip->startSeconds, clip->startSeconds + clip->durationSeconds);
}

bool clearLoopRange(ProjectDocument& project) {
    const bool changed = project.loopEnabled ||
        project.loopStartSeconds != 0.0 ||
        project.loopEndSeconds != 0.0;
    project.loopEnabled = false;
    project.loopStartSeconds = 0.0;
    project.loopEndSeconds = 0.0;
    return changed;
}

bool setClipGainDb(ProjectDocument& project, const std::string& clipId, float gainDb) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || !std::isfinite(gainDb)) {
        return false;
    }
    clip->gainDb = std::max(-60.0f, std::min(24.0f, gainDb));
    syncClipGainToActivePlaylistPlacement(project, *clip);
    return true;
}

bool adjustClipGainInRange(ProjectDocument& project,
                           double rangeStartSeconds,
                           double rangeEndSeconds,
                           float deltaDb,
                           std::vector<std::string>& changedClipIds) {
    changedClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds) || !std::isfinite(deltaDb) ||
        std::abs(rangeEndSeconds - rangeStartSeconds) <= 0.001) {
        return false;
    }
    const double rangeStart = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double rangeEnd = std::max(rangeStart + 0.001, std::max(rangeStartSeconds, rangeEndSeconds));
    std::vector<std::string> splitIds;
    separateClipRange(project, rangeStart, rangeEnd, splitIds);

    bool changed = false;
    for (auto& clip : project.clips) {
        if (clipIsLocked(&clip)) {
            continue;
        }
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        const bool insideRange = clipStart >= rangeStart - 0.0001 && clipEndSeconds <= rangeEnd + 0.0001;
        const bool overlapsRange = clipEndSeconds > rangeStart + 0.0001 && clipStart < rangeEnd - 0.0001;
        if (!insideRange || !overlapsRange) {
            continue;
        }
        const float oldGain = clip.gainDb;
        clip.gainDb = std::max(-60.0f, std::min(24.0f, clip.gainDb + deltaDb));
        if (clip.gainDb != oldGain) {
            syncClipGainToActivePlaylistPlacement(project, clip);
            changed = true;
            changedClipIds.push_back(clip.id);
        }
    }
    return changed;
}

bool setClipRegionName(ProjectDocument& project, const std::string& clipId, const std::string& regionName) {
    auto* clip = findClip(project, clipId);
    const std::string cleanName = trimTrackName(regionName);
    if (clip == nullptr || cleanName.empty()) {
        return false;
    }
    clip->regionName = cleanName;
    return true;
}

bool setClipFades(ProjectDocument& project, const std::string& clipId, double fadeInSeconds, double fadeOutSeconds) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || !std::isfinite(fadeInSeconds) || !std::isfinite(fadeOutSeconds)) {
        return false;
    }
    clip->fadeInSeconds = fadeInSeconds;
    clip->fadeOutSeconds = fadeOutSeconds;
    clampClipFades(*clip);
    return true;
}

bool setClipFadeCurves(ProjectDocument& project,
                       const std::string& clipId,
                       const std::string& fadeInCurve,
                       const std::string& fadeOutCurve) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip)) {
        return false;
    }
    clip->fadeInCurve = normalizedFadeCurve(fadeInCurve);
    clip->fadeOutCurve = normalizedFadeCurve(fadeOutCurve);
    return true;
}

bool applyAutomaticClipCrossfades(ProjectDocument& project, const std::string& clipId) {
    const auto* selected = findClip(project, clipId);
    if (selected == nullptr || selected->locked) {
        return false;
    }

    struct FadeEdit {
        std::string clipId;
        double fadeInSeconds;
        double fadeOutSeconds;
    };

    std::vector<FadeEdit> edits;
    const auto addFadeEdit = [&](const std::string& id, double fadeInSeconds, double fadeOutSeconds) {
        auto existing = std::find_if(edits.begin(), edits.end(), [&](const FadeEdit& edit) {
            return edit.clipId == id;
        });
        if (existing != edits.end()) {
            existing->fadeInSeconds = std::max(existing->fadeInSeconds, fadeInSeconds);
            existing->fadeOutSeconds = std::max(existing->fadeOutSeconds, fadeOutSeconds);
            return;
        }
        edits.push_back({id, fadeInSeconds, fadeOutSeconds});
    };
    const double selectedStart = selected->startSeconds;
    const double selectedEnd = clipEnd(*selected);
    const std::string selectedTrack = selected->trackName;

    for (const auto& other : project.clips) {
        if (other.id == clipId || other.trackName != selectedTrack || clipIsLocked(&other)) {
            continue;
        }
        const double overlapStart = std::max(selectedStart, other.startSeconds);
        const double overlapEnd = std::min(selectedEnd, clipEnd(other));
        const double overlapSeconds = overlapEnd - overlapStart;
        if (overlapSeconds <= 0.001 || !std::isfinite(overlapSeconds)) {
            continue;
        }

        const bool selectedIsLater = selectedStart >= other.startSeconds;
        if (selectedIsLater) {
            addFadeEdit(other.id, other.fadeInSeconds, std::max(other.fadeOutSeconds, overlapSeconds));
            addFadeEdit(clipId, std::max(selected->fadeInSeconds, overlapSeconds), selected->fadeOutSeconds);
        } else {
            addFadeEdit(clipId, selected->fadeInSeconds, std::max(selected->fadeOutSeconds, overlapSeconds));
            addFadeEdit(other.id, std::max(other.fadeInSeconds, overlapSeconds), other.fadeOutSeconds);
        }
    }

    bool changed = false;
    for (const auto& edit : edits) {
        auto* clip = findClip(project, edit.clipId);
        if (clip == nullptr || clipIsLocked(clip)) {
            continue;
        }
        const double oldFadeIn = clip->fadeInSeconds;
        const double oldFadeOut = clip->fadeOutSeconds;
        clip->fadeInSeconds = edit.fadeInSeconds;
        clip->fadeOutSeconds = edit.fadeOutSeconds;
        clampClipFades(*clip);
        changed = changed || clip->fadeInSeconds != oldFadeIn || clip->fadeOutSeconds != oldFadeOut;
    }
    return changed;
}

bool applyFadeOrCrossfadeToClipRange(ProjectDocument& project,
                                     double rangeStartSeconds,
                                     double rangeEndSeconds,
                                     std::vector<std::string>& changedClipIds) {
    changedClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds) ||
        std::abs(rangeEndSeconds - rangeStartSeconds) <= 0.001) {
        return false;
    }
    const double rangeStart = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double rangeEnd = std::max(rangeStart + 0.001, std::max(rangeStartSeconds, rangeEndSeconds));
    std::vector<std::string> candidateIds;
    for (const auto& clip : project.clips) {
        if (clipIsLocked(&clip)) {
            continue;
        }
        const double start = clip.startSeconds;
        const double end = clipEnd(clip);
        if (end > rangeStart + 0.0001 && start < rangeEnd - 0.0001) {
            candidateIds.push_back(clip.id);
        }
    }
    if (candidateIds.empty()) {
        return false;
    }

    struct FadeSnapshot {
        std::string clipId;
        double fadeInSeconds = 0.0;
        double fadeOutSeconds = 0.0;
    };
    std::vector<FadeSnapshot> beforeSnapshots;
    beforeSnapshots.reserve(candidateIds.size());
    for (const auto& clipId : candidateIds) {
        const ClipState* clip = findClip(project, clipId);
        if (clip != nullptr) {
            beforeSnapshots.push_back({clipId, clip->fadeInSeconds, clip->fadeOutSeconds});
        }
    }

    bool changed = false;
    for (const auto& clipId : candidateIds) {
        ClipState* before = findClip(project, clipId);
        if (before == nullptr) {
            continue;
        }
        applyAutomaticClipCrossfades(project, clipId);
        ClipState* after = findClip(project, clipId);
        if (after == nullptr || clipIsLocked(after)) {
            continue;
        }
        const auto snapshot = std::find_if(beforeSnapshots.begin(), beforeSnapshots.end(), [&](const FadeSnapshot& item) {
            return item.clipId == clipId;
        });
        const bool changedFromInitial = snapshot != beforeSnapshots.end() &&
            (after->fadeInSeconds != snapshot->fadeInSeconds || after->fadeOutSeconds != snapshot->fadeOutSeconds);
        if (!changedFromInitial) {
            const bool selectionTouchesStart = after->startSeconds >= rangeStart - 0.0001 && after->startSeconds < rangeEnd;
            const bool selectionTouchesEnd = clipEnd(*after) > rangeStart && clipEnd(*after) <= rangeEnd + 0.0001;
            const double fadeSeconds = std::min(0.05, std::max(0.0, after->durationSeconds * 0.5));
            const double nextFadeIn = selectionTouchesStart ? std::max(after->fadeInSeconds, fadeSeconds) : after->fadeInSeconds;
            const double nextFadeOut = selectionTouchesEnd ? std::max(after->fadeOutSeconds, fadeSeconds) : after->fadeOutSeconds;
            if (nextFadeIn != after->fadeInSeconds || nextFadeOut != after->fadeOutSeconds) {
                after->fadeInSeconds = nextFadeIn;
                after->fadeOutSeconds = nextFadeOut;
                clampClipFades(*after);
            }
        }
    }

    for (const auto& snapshot : beforeSnapshots) {
        const ClipState* clip = findClip(project, snapshot.clipId);
        if (clip == nullptr) {
            continue;
        }
        if (clip->fadeInSeconds != snapshot.fadeInSeconds || clip->fadeOutSeconds != snapshot.fadeOutSeconds) {
            changed = true;
            changedClipIds.push_back(snapshot.clipId);
        }
    }
    return changed;
}

bool setClipColor(ProjectDocument& project, const std::string& clipId, const std::string& colorHex) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || colorHex.empty()) {
        return false;
    }
    clip->colorHex = colorHex;
    return true;
}

bool setClipMuted(ProjectDocument& project, const std::string& clipId, bool muted) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip)) {
        return false;
    }
    clip->muted = muted;
    return true;
}

bool setClipMutedInRange(ProjectDocument& project,
                         double rangeStartSeconds,
                         double rangeEndSeconds,
                         bool muted,
                         std::vector<std::string>& changedClipIds) {
    changedClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds) ||
        std::abs(rangeEndSeconds - rangeStartSeconds) <= 0.001) {
        return false;
    }
    const double rangeStart = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double rangeEnd = std::max(rangeStart + 0.001, std::max(rangeStartSeconds, rangeEndSeconds));
    std::vector<std::string> splitIds;
    separateClipRange(project, rangeStart, rangeEnd, splitIds);

    bool changed = false;
    for (auto& clip : project.clips) {
        if (clipIsLocked(&clip)) {
            continue;
        }
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        const bool insideRange = clipStart >= rangeStart - 0.0001 && clipEndSeconds <= rangeEnd + 0.0001;
        const bool overlapsRange = clipEndSeconds > rangeStart + 0.0001 && clipStart < rangeEnd - 0.0001;
        if (!insideRange || !overlapsRange || clip.muted == muted) {
            continue;
        }
        clip.muted = muted;
        changed = true;
        changedClipIds.push_back(clip.id);
    }
    return changed;
}

bool setClipPolarityInverted(ProjectDocument& project, const std::string& clipId, bool inverted) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip)) {
        return false;
    }
    clip->polarityInverted = inverted;
    return true;
}

bool setClipPolarityInvertedInRange(ProjectDocument& project,
                                    double rangeStartSeconds,
                                    double rangeEndSeconds,
                                    bool inverted,
                                    std::vector<std::string>& changedClipIds) {
    changedClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds) ||
        std::abs(rangeEndSeconds - rangeStartSeconds) <= 0.001) {
        return false;
    }
    const double rangeStart = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double rangeEnd = std::max(rangeStart + 0.001, std::max(rangeStartSeconds, rangeEndSeconds));
    std::vector<std::string> splitIds;
    separateClipRange(project, rangeStart, rangeEnd, splitIds);

    bool changed = false;
    for (auto& clip : project.clips) {
        if (clipIsLocked(&clip)) {
            continue;
        }
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        const bool insideRange = clipStart >= rangeStart - 0.0001 && clipEndSeconds <= rangeEnd + 0.0001;
        const bool overlapsRange = clipEndSeconds > rangeStart + 0.0001 && clipStart < rangeEnd - 0.0001;
        if (!insideRange || !overlapsRange || clip.polarityInverted == inverted) {
            continue;
        }
        clip.polarityInverted = inverted;
        changed = true;
        changedClipIds.push_back(clip.id);
    }
    return changed;
}

bool setClipLocked(ProjectDocument& project, const std::string& clipId, bool locked) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr) {
        return false;
    }
    clip->locked = locked;
    return true;
}

bool setClipTrack(ProjectDocument& project, const std::string& clipId, const std::string& trackName) {
    auto* clip = findClip(project, clipId);
    TrackState* targetTrack = findTrack(project, trackName);
    if (clip == nullptr || clipIsLocked(clip) || isProtectedTrackName(trackName) || targetTrack == nullptr || !isTimelineTargetTrackType(*targetTrack)) {
        return false;
    }
    clip->trackName = trackName;
    return true;
}

bool slipClipSourceOffset(ProjectDocument& project,
                          const std::string& clipId,
                          double deltaSeconds,
                          std::string& message) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr) {
        message = "No clip is selected.";
        return false;
    }
    if (clipIsLocked(clip)) {
        message = "Selected clip is locked.";
        return false;
    }
    if (clip->sourcePath.empty()) {
        message = "Selected clip has no source WAV path.";
        return false;
    }
    if (!std::isfinite(deltaSeconds)) {
        message = "Invalid slip amount.";
        return false;
    }

    WavAudioData audio;
    std::string error;
    if (!readPcmWavFile(clip->sourcePath, audio, error)) {
        message = "Could not read selected clip WAV: " + error;
        return false;
    }
    const double sourceSeconds = audio.sampleRate > 0
        ? static_cast<double>(audio.frameCount()) / static_cast<double>(audio.sampleRate)
        : 0.0;
    if (sourceSeconds <= 0.0 || clip->durationSeconds <= 0.0 || clip->durationSeconds > sourceSeconds) {
        message = "Selected clip cannot be slipped within the source WAV.";
        return false;
    }

    const double maxOffset = std::max(0.0, sourceSeconds - clip->durationSeconds);
    const double nextOffset = clip->sourceOffsetSeconds + deltaSeconds;
    if (nextOffset < 0.0 || nextOffset > maxOffset) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << "Slip range is 0.00-" << maxOffset << "s for this clip.";
        message = out.str();
        return false;
    }
    clip->sourceOffsetSeconds = nextOffset;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << "Slipped clip source to " << clip->sourceOffsetSeconds << "s.";
    message = out.str();
    return true;
}

bool normalizeClipGainToPeak(ProjectDocument& project,
                             const std::string& clipId,
                             float targetPeakDb,
                             std::string& message) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr) {
        message = "No clip is selected.";
        return false;
    }
    if (clipIsLocked(clip)) {
        message = "Selected clip is locked.";
        return false;
    }
    if (clip->sourcePath.empty()) {
        message = "Selected clip has no source WAV path.";
        return false;
    }
    if (!std::isfinite(targetPeakDb)) {
        message = "Invalid target peak.";
        return false;
    }

    WavAudioData audio;
    std::string error;
    if (!readPcmWavFile(clip->sourcePath, audio, error)) {
        message = "Could not read selected clip WAV: " + error;
        return false;
    }
    if (audio.channels <= 0 || audio.sampleRate <= 0 || audio.interleavedSamples.empty()) {
        message = "Selected clip WAV has no readable audio.";
        return false;
    }

    const int64_t frameCount = audio.frameCount();
    const int64_t startFrame = std::max<int64_t>(0, static_cast<int64_t>(std::llround(clip->sourceOffsetSeconds * audio.sampleRate)));
    const int64_t durationFrames = std::max<int64_t>(1, static_cast<int64_t>(std::llround(clip->durationSeconds * audio.sampleRate)));
    const int64_t endFrame = std::min(frameCount, startFrame + durationFrames);
    if (startFrame >= endFrame) {
        message = "Selected clip range is outside the source WAV.";
        return false;
    }

    float peak = 0.0f;
    for (int64_t frame = startFrame; frame < endFrame; ++frame) {
        const size_t sampleOffset = static_cast<size_t>(frame * audio.channels);
        for (int channel = 0; channel < audio.channels; ++channel) {
            peak = std::max(peak, std::abs(audio.interleavedSamples[sampleOffset + static_cast<size_t>(channel)]));
        }
    }
    if (peak <= 0.000001f) {
        message = "Selected clip range is silent.";
        return false;
    }

    const float clampedTarget = std::max(-60.0f, std::min(0.0f, targetPeakDb));
    const float sourcePeakDb = 20.0f * std::log10(peak);
    clip->gainDb = std::max(-60.0f, std::min(24.0f, clampedTarget - sourcePeakDb));
    syncClipGainToActivePlaylistPlacement(project, *clip);
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << "Normalized clip gain to " << clip->gainDb
        << " dB for " << clampedTarget << " dBFS peak.";
    message = out.str();
    return true;
}

bool normalizeClipGainInRange(ProjectDocument& project,
                              double rangeStartSeconds,
                              double rangeEndSeconds,
                              float targetPeakDb,
                              std::vector<std::string>& changedClipIds,
                              std::string& message) {
    changedClipIds.clear();
    if (!std::isfinite(rangeStartSeconds) || !std::isfinite(rangeEndSeconds) || !std::isfinite(targetPeakDb) ||
        std::abs(rangeEndSeconds - rangeStartSeconds) <= 0.001) {
        message = "Invalid edit selection for normalize.";
        return false;
    }
    const double rangeStart = std::max(0.0, std::min(rangeStartSeconds, rangeEndSeconds));
    const double rangeEnd = std::max(rangeStart + 0.001, std::max(rangeStartSeconds, rangeEndSeconds));
    std::vector<std::string> splitIds;
    separateClipRange(project, rangeStart, rangeEnd, splitIds);

    std::vector<std::string> candidateIds;
    for (const auto& clip : project.clips) {
        if (clipIsLocked(&clip)) {
            continue;
        }
        const double clipStart = clip.startSeconds;
        const double clipEndSeconds = clipEnd(clip);
        const bool insideRange = clipStart >= rangeStart - 0.0001 && clipEndSeconds <= rangeEnd + 0.0001;
        const bool overlapsRange = clipEndSeconds > rangeStart + 0.0001 && clipStart < rangeEnd - 0.0001;
        if (insideRange && overlapsRange) {
            candidateIds.push_back(clip.id);
        }
    }
    if (candidateIds.empty()) {
        message = "No editable clips in selected range.";
        return false;
    }

    size_t skipped = 0;
    std::string lastError;
    for (const auto& clipId : candidateIds) {
        ClipState* before = findClip(project, clipId);
        if (before == nullptr) {
            ++skipped;
            continue;
        }
        const float oldGain = before->gainDb;
        std::string clipMessage;
        if (!normalizeClipGainToPeak(project, clipId, targetPeakDb, clipMessage)) {
            ++skipped;
            lastError = clipMessage;
            continue;
        }
        ClipState* after = findClip(project, clipId);
        if (after != nullptr && after->gainDb != oldGain) {
            changedClipIds.push_back(clipId);
        }
    }
    if (changedClipIds.empty()) {
        message = lastError.empty() ? "No selected clip gain changed." : lastError;
        return false;
    }
    std::ostringstream out;
    out << "Normalized " << changedClipIds.size() << " selected clip"
        << (changedClipIds.size() == 1 ? "" : "s")
        << " to " << std::fixed << std::setprecision(1)
        << std::max(-60.0f, std::min(0.0f, targetPeakDb)) << " dBFS";
    if (skipped > 0) {
        out << " (" << skipped << " skipped)";
    }
    out << ".";
    message = out.str();
    return true;
}

bool setClipSourcePath(ProjectDocument& project,
                       const std::string& clipId,
                       const std::string& sourcePath,
                       double durationSeconds) {
    auto* clip = findClip(project, clipId);
    if (clip == nullptr || clipIsLocked(clip) || sourcePath.empty() || durationSeconds <= 0.0 || !std::isfinite(durationSeconds)) {
        return false;
    }
    clip->sourcePath = sourcePath;
    if (clip->regionName.empty() || clip->regionName == clip->id) {
        clip->regionName = pathStemForRegionName(sourcePath);
    }
    clip->sourceFileUid = sourceUidForPath(sourcePath);
    populateClipSourceMetadata(*clip);
    clip->sourceOffsetSeconds = 0.0;
    clip->durationSeconds = durationSeconds;
    clampClipFades(*clip);
    return true;
}

bool setTrackRecordArmed(ProjectDocument& project, const std::string& trackName, bool armed) {
    TrackState* target = findTrack(project, trackName);
    if (target == nullptr || isProtectedTrackName(trackName) || !isTimelineTargetTrackType(*target)) {
        return false;
    }
    target->recordArmed = armed;
    return true;
}

bool setTrackInputMonitoring(ProjectDocument& project, const std::string& trackName, bool monitoring) {
    TrackState* target = findTrack(project, trackName);
    if (target == nullptr || isProtectedTrackName(trackName) || !isTimelineTargetTrackType(*target)) {
        return false;
    }
    target->inputMonitoring = monitoring;
    return true;
}

bool setTrackMuted(ProjectDocument& project, const std::string& trackName, bool muted) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr ||
        track->trackType == "monitor" ||
        track->name == "Monitor" ||
        (isProtectedTrackName(trackName) && track->name != "Master")) {
        return false;
    }
    track->muted = muted;
    return true;
}

bool setTrackSolo(ProjectDocument& project, const std::string& trackName, bool solo) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName)) {
        return false;
    }
    track->solo = solo;
    return true;
}

bool setTrackVolumeDb(ProjectDocument& project, const std::string& trackName, float volumeDb) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || !trackSupportsSignalControls(*track) || !std::isfinite(volumeDb)) {
        return false;
    }
    track->volumeDb = std::max(-120.0f, std::min(12.0f, volumeDb));
    return true;
}

bool setTrackPan(ProjectDocument& project, const std::string& trackName, float pan) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || !trackSupportsAudioRoutingControls(*track) || !std::isfinite(pan)) {
        return false;
    }
    track->pan = std::max(-1.0f, std::min(1.0f, pan));
    return true;
}

bool setTrackColor(ProjectDocument& project, const std::string& trackName, const std::string& colorHex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || colorHex.empty()) {
        return false;
    }
    track->colorHex = colorHex;
    return true;
}

bool setTrackControlMaster(ProjectDocument& project, const std::string& trackName, const std::string& controlMasterTrackName) {
    TrackState* target = findTrack(project, trackName);
    if (target == nullptr || isProtectedTrackName(trackName) || !trackSupportsVcaControlAssignment(*target)) {
        return false;
    }
    const std::string cleanControlMasterName = trimTrackName(controlMasterTrackName);
    if (cleanControlMasterName.empty()) {
        target->controlMasterTrackName.clear();
        return true;
    }
    if (cleanControlMasterName == target->name || isProtectedTrackName(cleanControlMasterName)) {
        return false;
    }
    TrackState* controlMaster = findTrack(project, cleanControlMasterName);
    if (controlMaster == nullptr || controlMaster->trackType != "vca") {
        return false;
    }
    target->controlMasterTrackName = cleanControlMasterName;
    return true;
}

bool addTrackInsertSlot(ProjectDocument& project, const std::string& trackName) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track)) {
        return false;
    }
    if (track->inserts.size() >= kMaxTrackInsertSlots) {
        return false;
    }
    TrackInsertSlot insert;
    insert.enabled = false;
    track->inserts.push_back(insert);
    return true;
}

bool setTrackInsertSlot(ProjectDocument& project,
                        const std::string& trackName,
                        size_t insertIndex,
                        const TrackInsertSlot& insert) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || insertIndex >= track->inserts.size()) {
        return false;
    }
    track->inserts[insertIndex] = normalizedTrackInsert(insert);
    return true;
}

bool toggleTrackInsertBypass(ProjectDocument& project, const std::string& trackName, size_t insertIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || insertIndex >= track->inserts.size()) {
        return false;
    }
    auto& insert = track->inserts[insertIndex];
    if (!trackInsertHasPlugin(insert)) {
        return false;
    }
    insert.bypassed = !insert.bypassed;
    return true;
}

bool removeTrackInsertSlot(ProjectDocument& project, const std::string& trackName, size_t insertIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || insertIndex >= track->inserts.size()) {
        return false;
    }
    track->inserts.erase(track->inserts.begin() + static_cast<std::ptrdiff_t>(insertIndex));
    return true;
}

bool setTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName, const InstrumentSlotState& instrument) {
    return setTrackInstrumentSlot(project, trackName, 0, instrument);
}

bool setTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName, size_t slotIndex, const InstrumentSlotState& instrument) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || track->trackType != "instrument" || isProtectedTrackName(trackName) || slotIndex >= kMaxInstrumentRackSlots) {
        return false;
    }
    // Materialize a legacy single instrument into slot 0 before writing any higher slot, or
    // adding a layer would leave slot 0 empty and compaction would drop the original
    // instrument (front() becomes the empty slot). clearTrackInstrumentSlot and
    // toggleTrackInstrumentBypass already do this; the setter must too.
    if (slotIndex > 0 && track->instrumentSlots.empty() && instrumentSlotHasPlugin(track->instrument)) {
        track->instrumentSlots.push_back(track->instrument);
    }
    if (track->instrumentSlots.size() <= slotIndex) {
        track->instrumentSlots.resize(slotIndex + 1);
    }
    track->instrumentSlots[slotIndex] = normalizedInstrumentSlot(instrument);
    compactAndSyncInstrumentRack(*track);
    track->inputBus = track->instrument.midiInput.empty() ? "MIDI Input" : track->instrument.midiInput;
    track->outputBus = track->outputBus.empty() || track->outputBus == "Instrument" ? "Master" : track->outputBus;
    return true;
}

bool toggleTrackInstrumentBypass(ProjectDocument& project, const std::string& trackName) {
    return toggleTrackInstrumentBypass(project, trackName, 0);
}

bool toggleTrackInstrumentBypass(ProjectDocument& project, const std::string& trackName, size_t slotIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || track->trackType != "instrument" || slotIndex >= kMaxInstrumentRackSlots) {
        return false;
    }
    if (track->instrumentSlots.empty() && instrumentSlotHasPlugin(track->instrument)) {
        track->instrumentSlots.push_back(track->instrument);
    }
    if (slotIndex >= track->instrumentSlots.size() || !instrumentSlotHasPlugin(track->instrumentSlots[slotIndex])) {
        return false;
    }
    track->instrumentSlots[slotIndex].bypassed = !track->instrumentSlots[slotIndex].bypassed;
    compactAndSyncInstrumentRack(*track);
    return true;
}

bool toggleTrackInstrumentSlotSolo(ProjectDocument& project, const std::string& trackName, size_t slotIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || track->trackType != "instrument" || slotIndex >= kMaxInstrumentRackSlots) {
        return false;
    }
    if (track->instrumentSlots.empty() && instrumentSlotHasPlugin(track->instrument)) {
        track->instrumentSlots.push_back(track->instrument);
    }
    if (slotIndex >= track->instrumentSlots.size() || !instrumentSlotHasPlugin(track->instrumentSlots[slotIndex])) {
        return false;
    }
    track->instrumentSlots[slotIndex].soloed = !track->instrumentSlots[slotIndex].soloed;
    compactAndSyncInstrumentRack(*track);
    return true;
}

bool setTrackInstrumentMidiChannel(ProjectDocument& project, const std::string& trackName, int midiChannel) {
    return setTrackInstrumentMidiChannel(project, trackName, 0, midiChannel);
}

bool setTrackInstrumentMidiChannel(ProjectDocument& project, const std::string& trackName, size_t slotIndex, int midiChannel) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || track->trackType != "instrument" || isProtectedTrackName(trackName) ||
        midiChannel < 0 || midiChannel > 16 || slotIndex >= kMaxInstrumentRackSlots) {
        return false;
    }
    if (track->instrumentSlots.empty() && instrumentSlotHasPlugin(track->instrument)) {
        track->instrumentSlots.push_back(track->instrument);
    }
    if (track->instrumentSlots.size() <= slotIndex) {
        track->instrumentSlots.resize(slotIndex + 1);
    }
    track->instrumentSlots[slotIndex] = normalizedInstrumentSlot(track->instrumentSlots[slotIndex]);
    track->instrumentSlots[slotIndex].midiChannel = midiChannel;
    track->instrumentSlots[slotIndex].midiInput = track->instrumentSlots[slotIndex].midiInput.empty() ? "MIDI Input" : track->instrumentSlots[slotIndex].midiInput;
    compactAndSyncInstrumentRack(*track);
    track->inputBus = track->instrument.midiInput;
    return true;
}

bool clearTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName) {
    return clearTrackInstrumentSlot(project, trackName, 0);
}

bool clearTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName, size_t slotIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || track->trackType != "instrument" || slotIndex >= kMaxInstrumentRackSlots) {
        return false;
    }
    if (track->instrumentSlots.empty() && instrumentSlotHasPlugin(track->instrument)) {
        track->instrumentSlots.push_back(track->instrument);
    }
    if (slotIndex < track->instrumentSlots.size()) {
        track->instrumentSlots[slotIndex] = normalizedInstrumentSlot(InstrumentSlotState {});
    }
    compactAndSyncInstrumentRack(*track);
    track->inputBus = "MIDI Input";
    if (track->outputBus.empty() || track->outputBus == "Instrument") {
        track->outputBus = "Master";
    }
    return true;
}

int moveTrackInsertSlot(ProjectDocument& project, const std::string& trackName, size_t insertIndex, int direction) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || insertIndex >= track->inserts.size() || direction == 0) {
        return -1;
    }
    if (direction < 0) {
        if (insertIndex == 0) {
            return -1;
        }
        std::swap(track->inserts[insertIndex - 1], track->inserts[insertIndex]);
        return static_cast<int>(insertIndex - 1);
    }
    if (insertIndex + 1 >= track->inserts.size()) {
        return -1;
    }
    std::swap(track->inserts[insertIndex], track->inserts[insertIndex + 1]);
    return static_cast<int>(insertIndex + 1);
}

int moveTrackInsertSlotToIndex(ProjectDocument& project,
                               const std::string& trackName,
                               size_t insertIndex,
                               size_t targetIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr ||
        isProtectedTrackName(trackName) ||
        !trackSupportsAudioRoutingControls(*track) ||
        insertIndex >= track->inserts.size() ||
        targetIndex > track->inserts.size()) {
        return -1;
    }
    if (targetIndex == insertIndex || targetIndex == insertIndex + 1) {
        return static_cast<int>(insertIndex);
    }
    TrackInsertSlot moved = track->inserts[insertIndex];
    track->inserts.erase(track->inserts.begin() + static_cast<std::ptrdiff_t>(insertIndex));
    if (targetIndex > insertIndex) {
        --targetIndex;
    }
    targetIndex = std::min(targetIndex, track->inserts.size());
    track->inserts.insert(track->inserts.begin() + static_cast<std::ptrdiff_t>(targetIndex), moved);
    return static_cast<int>(targetIndex);
}

bool addTrackSendSlot(ProjectDocument& project, const std::string& trackName, const TrackSendState& send) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track)) {
        return false;
    }
    TrackSendState normalized = normalizedTrackSend(send);
    if (trackWouldSendToOwnInput(*track, normalized)) {
        normalized.enabled = false;
    }
    track->sends.push_back(normalized);
    return true;
}

bool setTrackSendSlot(ProjectDocument& project,
                      const std::string& trackName,
                      size_t sendIndex,
                      const TrackSendState& send) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || sendIndex >= track->sends.size()) {
        return false;
    }
    const std::string oldBus = track->sends[sendIndex].busName;
    TrackSendState normalized = normalizedTrackSend(send);
    if (trackWouldSendToOwnInput(*track, normalized)) {
        return false;
    }
    track->sends[sendIndex] = normalized;
    if (oldBus != track->sends[sendIndex].busName) {
        removeUnusedAuxForBus(project, oldBus);
    }
    return true;
}

bool setTrackSendEnabled(ProjectDocument& project, const std::string& trackName, size_t sendIndex, bool enabled) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || sendIndex >= track->sends.size()) {
        return false;
    }
    if (enabled && trackWouldSendToOwnInput(*track, track->sends[sendIndex])) {
        return false;
    }
    track->sends[sendIndex].enabled = enabled &&
        !trimTrackName(track->sends[sendIndex].busName).empty();
    return true;
}

bool toggleTrackSendPreFader(ProjectDocument& project, const std::string& trackName, size_t sendIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || sendIndex >= track->sends.size()) {
        return false;
    }
    track->sends[sendIndex].preFader = !track->sends[sendIndex].preFader;
    return true;
}

bool toggleTrackSendStereo(ProjectDocument& project, const std::string& trackName, size_t sendIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || sendIndex >= track->sends.size()) {
        return false;
    }
    track->sends[sendIndex].stereo = !track->sends[sendIndex].stereo;
    return true;
}

bool removeTrackSendSlot(ProjectDocument& project, const std::string& trackName, size_t sendIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || sendIndex >= track->sends.size()) {
        return false;
    }
    const std::string removedBus = track->sends[sendIndex].busName;
    track->sends.erase(track->sends.begin() + static_cast<std::ptrdiff_t>(sendIndex));
    removeUnusedAuxForBus(project, removedBus);
    return true;
}

int moveTrackSendSlot(ProjectDocument& project, const std::string& trackName, size_t sendIndex, int direction) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsAudioRoutingControls(*track) || sendIndex >= track->sends.size() || direction == 0) {
        return -1;
    }
    if (direction < 0) {
        if (sendIndex == 0) {
            return -1;
        }
        std::swap(track->sends[sendIndex - 1], track->sends[sendIndex]);
        return static_cast<int>(sendIndex - 1);
    }
    if (sendIndex + 1 >= track->sends.size()) {
        return -1;
    }
    std::swap(track->sends[sendIndex], track->sends[sendIndex + 1]);
    return static_cast<int>(sendIndex + 1);
}

int moveTrackSendSlotToIndex(ProjectDocument& project,
                             const std::string& trackName,
                             size_t sendIndex,
                             size_t targetIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr ||
        isProtectedTrackName(trackName) ||
        !trackSupportsAudioRoutingControls(*track) ||
        sendIndex >= track->sends.size() ||
        targetIndex > track->sends.size()) {
        return -1;
    }
    if (targetIndex == sendIndex || targetIndex == sendIndex + 1) {
        return static_cast<int>(sendIndex);
    }
    TrackSendState moved = track->sends[sendIndex];
    track->sends.erase(track->sends.begin() + static_cast<std::ptrdiff_t>(sendIndex));
    if (targetIndex > sendIndex) {
        --targetIndex;
    }
    targetIndex = std::min(targetIndex, track->sends.size());
    track->sends.insert(track->sends.begin() + static_cast<std::ptrdiff_t>(targetIndex), moved);
    return static_cast<int>(targetIndex);
}

bool setTrackVolumeAutomationPoint(ProjectDocument& project, const std::string& trackName, double timeSeconds, float volumeDb) {
    TrackState* track = findTrack(project, trackName);
    const bool protectedAutomationTarget = isProtectedTrackName(trackName) && trackName != "Master";
    if (track == nullptr || protectedAutomationTarget || !trackSupportsSignalControls(*track) || !std::isfinite(timeSeconds) || !std::isfinite(volumeDb)) {
        return false;
    }
    AutomationPointState point;
    point.timeSeconds = std::max(0.0, timeSeconds);
    point.value = std::max(-120.0f, std::min(12.0f, volumeDb));
    constexpr double mergeToleranceSeconds = 0.0005;
    for (auto& existing : track->volumeAutomation) {
        if (std::abs(existing.timeSeconds - point.timeSeconds) <= mergeToleranceSeconds) {
            existing.timeSeconds = point.timeSeconds;
            existing.value = point.value;
            return true;
        }
    }
    track->volumeAutomation.push_back(point);
    std::sort(track->volumeAutomation.begin(), track->volumeAutomation.end(), [](const AutomationPointState& left, const AutomationPointState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    return true;
}

bool moveTrackVolumeAutomationPoint(ProjectDocument& project, const std::string& trackName, size_t pointIndex, double timeSeconds, float volumeDb) {
    TrackState* track = findTrack(project, trackName);
    const bool protectedAutomationTarget = isProtectedTrackName(trackName) && trackName != "Master";
    if (track == nullptr || protectedAutomationTarget || !trackSupportsSignalControls(*track) ||
        pointIndex >= track->volumeAutomation.size() ||
        !std::isfinite(timeSeconds) || !std::isfinite(volumeDb)) {
        return false;
    }
    track->volumeAutomation[pointIndex].timeSeconds = std::max(0.0, timeSeconds);
    track->volumeAutomation[pointIndex].value = std::max(-120.0f, std::min(12.0f, volumeDb));
    std::sort(track->volumeAutomation.begin(), track->volumeAutomation.end(), [](const AutomationPointState& left, const AutomationPointState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    return true;
}

bool deleteTrackVolumeAutomationPoint(ProjectDocument& project, const std::string& trackName, size_t pointIndex) {
    TrackState* track = findTrack(project, trackName);
    const bool protectedAutomationTarget = isProtectedTrackName(trackName) && trackName != "Master";
    if (track == nullptr || protectedAutomationTarget || !trackSupportsSignalControls(*track) ||
        pointIndex >= track->volumeAutomation.size()) {
        return false;
    }
    track->volumeAutomation.erase(track->volumeAutomation.begin() + static_cast<std::ptrdiff_t>(pointIndex));
    return true;
}

size_t deleteTrackVolumeAutomationPointsInRange(ProjectDocument& project,
                                                const std::string& trackName,
                                                double rangeStartSeconds,
                                                double rangeEndSeconds) {
    TrackState* track = findTrack(project, trackName);
    const bool protectedAutomationTarget = isProtectedTrackName(trackName) && trackName != "Master";
    if (track == nullptr ||
        protectedAutomationTarget ||
        !trackSupportsSignalControls(*track) ||
        !std::isfinite(rangeStartSeconds) ||
        !std::isfinite(rangeEndSeconds)) {
        return 0;
    }
    if (rangeEndSeconds < rangeStartSeconds) {
        std::swap(rangeStartSeconds, rangeEndSeconds);
    }
    constexpr double epsilon = 0.0005;
    const double start = std::max(0.0, rangeStartSeconds - epsilon);
    const double end = std::max(start, rangeEndSeconds + epsilon);
    const size_t before = track->volumeAutomation.size();
    track->volumeAutomation.erase(std::remove_if(track->volumeAutomation.begin(), track->volumeAutomation.end(), [&](const AutomationPointState& point) {
        return std::isfinite(point.timeSeconds) && point.timeSeconds >= start && point.timeSeconds <= end;
    }), track->volumeAutomation.end());
    return before - track->volumeAutomation.size();
}

bool setTrackAutomationLanePoint(ProjectDocument& project,
                                 const std::string& trackName,
                                 const std::string& parameterId,
                                 const std::string& displayName,
                                 double timeSeconds,
                                 float value) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsSignalControls(*track) ||
        parameterId.empty() || !std::isfinite(timeSeconds) || !std::isfinite(value)) {
        return false;
    }

    const double clampedTime = std::max(0.0, timeSeconds);
    const float clampedValue = std::max(-1.0f, std::min(1.0f, value));
    auto laneIt = std::find_if(track->automationLanes.begin(), track->automationLanes.end(), [&](const AutomationLaneState& lane) {
        return lane.parameterId == parameterId;
    });
    if (laneIt == track->automationLanes.end()) {
        AutomationLaneState lane;
        lane.parameterId = parameterId;
        lane.displayName = displayName.empty() ? parameterId : displayName;
        track->automationLanes.push_back(lane);
        laneIt = std::prev(track->automationLanes.end());
    } else if (!displayName.empty()) {
        laneIt->displayName = displayName;
    }

    constexpr double mergeToleranceSeconds = 0.0005;
    for (auto& existing : laneIt->points) {
        if (std::abs(existing.timeSeconds - clampedTime) <= mergeToleranceSeconds) {
            existing.timeSeconds = clampedTime;
            existing.value = clampedValue;
            return true;
        }
    }

    laneIt->points.push_back({clampedTime, clampedValue});
    std::sort(laneIt->points.begin(), laneIt->points.end(), [](const AutomationPointState& left, const AutomationPointState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    return true;
}

bool moveTrackAutomationLanePoint(ProjectDocument& project,
                                  const std::string& trackName,
                                  const std::string& parameterId,
                                  size_t pointIndex,
                                  double timeSeconds,
                                  float value) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsSignalControls(*track) ||
        parameterId.empty() || !std::isfinite(timeSeconds) || !std::isfinite(value)) {
        return false;
    }
    auto laneIt = std::find_if(track->automationLanes.begin(), track->automationLanes.end(), [&](const AutomationLaneState& lane) {
        return lane.parameterId == parameterId;
    });
    if (laneIt == track->automationLanes.end() || pointIndex >= laneIt->points.size()) {
        return false;
    }
    laneIt->points[pointIndex].timeSeconds = std::max(0.0, timeSeconds);
    laneIt->points[pointIndex].value = std::max(-1.0f, std::min(1.0f, value));
    std::sort(laneIt->points.begin(), laneIt->points.end(), [](const AutomationPointState& left, const AutomationPointState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    return true;
}

bool normalizeMonitorStationProjectState(ProjectDocument& project) {
    bool changed = false;
    const bool msMode = project.monitorStationListenMode == "M" || project.monitorStationListenMode == "S";
    if (msMode) {
        if (project.monitorStationMono) {
            project.monitorStationMono = false;
            changed = true;
        }
        if (project.monitorStationSwapLeftRight) {
            project.monitorStationSwapLeftRight = false;
            changed = true;
        }
        return changed;
    }
    if (project.monitorStationListenMode != "LR" &&
        project.monitorStationListenMode != "L" &&
        project.monitorStationListenMode != "R") {
        project.monitorStationListenMode = "LR";
        changed = true;
    }
    return changed;
}

bool renameTrack(ProjectDocument& project, const std::string& oldName, const std::string& newName) {
    // A caller that passes project.tracks[i].name binds oldName to the very string
    // assigned below; every comparison after that would be against the *new* name,
    // and the clips would silently keep pointing at a track that no longer exists.
    const std::string previousName = oldName;

    TrackState* track = findTrack(project, previousName);
    const std::string cleanName = trimTrackName(newName);
    if (track == nullptr || cleanName.empty() ||
        isProtectedTrackName(previousName) || isProtectedTrackName(cleanName)) {
        return false;
    }
    if (cleanName == previousName) {
        return true;
    }
    if (trackNameExists(project, cleanName)) {
        return false;
    }

    track->name = cleanName;
    if (project.tempoMasterTrackName == previousName) {
        project.tempoMasterTrackName = cleanName;
    }
    for (auto& otherTrack : project.tracks) {
        if (otherTrack.inputBus == previousName) {
            otherTrack.inputBus = cleanName;
        }
        if (otherTrack.outputBus == previousName) {
            otherTrack.outputBus = cleanName;
        }
        for (auto& send : otherTrack.sends) {
            if (send.busName == previousName) {
                send.busName = cleanName;
            }
        }
        if (otherTrack.controlMasterTrackName == previousName) {
            otherTrack.controlMasterTrackName = cleanName;
        }
    }
    for (auto& clip : project.clips) {
        if (clip.trackName == previousName) {
            clip.trackName = cleanName;
        }
    }
    for (auto& playlist : project.trackPlaylists) {
        if (playlist.trackName == previousName) {
            playlist.trackName = cleanName;
        }
    }
    for (auto& region : project.midiRegions) {
        if (region.trackName == previousName) {
            region.trackName = cleanName;
        }
    }
    return true;
}

bool duplicateTrackWithClips(ProjectDocument& project,
                             const std::string& sourceTrackName,
                             std::string& newTrackName,
                             std::vector<std::string>& newClipIds) {
    newTrackName.clear();
    newClipIds.clear();
    if (isProtectedTrackName(sourceTrackName)) {
        return false;
    }

    auto sourceIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == sourceTrackName;
    });
    if (sourceIt == project.tracks.end()) {
        return false;
    }

    TrackState duplicatedTrack = *sourceIt;
    duplicatedTrack.name = uniqueTrackCopyName(project, sourceTrackName);
    duplicatedTrack.recordArmed = false;
    duplicatedTrack.muted = false;
    duplicatedTrack.solo = false;

    std::vector<ClipState> duplicatedClips;
    std::map<std::string, std::string> duplicatedClipIdBySourceId;
    for (const auto& clip : project.clips) {
        if (clip.trackName != sourceTrackName) {
            continue;
        }
        ClipState duplicate = clip;
        duplicate.id = uniqueDuplicateId(project, clip.id);
        while (std::any_of(duplicatedClips.begin(), duplicatedClips.end(), [&](const ClipState& existing) {
            return existing.id == duplicate.id;
        })) {
            duplicate.id += "-copy";
        }
        duplicate.trackName = duplicatedTrack.name;
        duplicatedClips.push_back(duplicate);
        duplicatedClipIdBySourceId[clip.id] = duplicate.id;
        newClipIds.push_back(duplicate.id);
    }

    std::vector<TrackPlaylistState> duplicatedPlaylists;
    for (const auto& playlist : project.trackPlaylists) {
        if (playlist.trackName != sourceTrackName) {
            continue;
        }
        TrackPlaylistState duplicatePlaylist = playlist;
        duplicatePlaylist.id = uniqueTrackPlaylistId(project, duplicatedTrack.name);
        while (std::any_of(duplicatedPlaylists.begin(), duplicatedPlaylists.end(), [&](const TrackPlaylistState& existing) {
            return existing.id == duplicatePlaylist.id;
        })) {
            duplicatePlaylist.id += "-copy";
        }
        duplicatePlaylist.trackName = duplicatedTrack.name;
        for (auto& placement : duplicatePlaylist.placements) {
            const std::string sourceClipId = placement.legacyClipId.empty() ? placement.id : placement.legacyClipId;
            auto mappedIt = duplicatedClipIdBySourceId.find(sourceClipId);
            if (mappedIt == duplicatedClipIdBySourceId.end()) {
                mappedIt = duplicatedClipIdBySourceId.find(placement.id);
            }
            if (mappedIt != duplicatedClipIdBySourceId.end()) {
                placement.id = mappedIt->second;
                placement.legacyClipId = mappedIt->second;
            } else if (!placement.id.empty()) {
                placement.id = uniqueDuplicateId(project, placement.id);
                while (std::any_of(duplicatedPlaylists.begin(), duplicatedPlaylists.end(), [&](const TrackPlaylistState& existingPlaylist) {
                    return std::any_of(existingPlaylist.placements.begin(), existingPlaylist.placements.end(), [&](const PlaylistClipPlacementState& existingPlacement) {
                        return existingPlacement.id == placement.id;
                    });
                })) {
                    placement.id += "-copy";
                }
                placement.legacyClipId = placement.id;
            }
        }
        duplicatedPlaylists.push_back(duplicatePlaylist);
    }
    if (duplicatedPlaylists.empty() && !duplicatedClips.empty()) {
        TrackPlaylistState playlist;
        playlist.id = uniqueTrackPlaylistId(project, duplicatedTrack.name);
        playlist.trackName = duplicatedTrack.name;
        playlist.name = "Playlist 1";
        playlist.active = true;
        for (const auto& clip : duplicatedClips) {
            PlaylistClipPlacementState placement;
            placement.id = clip.id;
            placement.clipDefinitionId = "clipdef-" + clip.id;
            placement.startSeconds = clip.startSeconds;
            placement.layer = 0;
            placement.gainDb = clip.gainDb;
            placement.fadeInSeconds = clip.fadeInSeconds;
            placement.fadeOutSeconds = clip.fadeOutSeconds;
            placement.fadeInCurve = normalizedFadeCurve(clip.fadeInCurve);
            placement.fadeOutCurve = normalizedFadeCurve(clip.fadeOutCurve);
            placement.muted = clip.muted;
            placement.polarityInverted = clip.polarityInverted;
            placement.locked = clip.locked;
            placement.colorHex = clip.colorHex;
            placement.timeScale = clip.timeScale;
            placement.tempoSyncPolicy = clip.tempoSyncPolicy.empty() ? "project-tempo" : clip.tempoSyncPolicy;
            placement.pendingTimeStretchToProject = clip.pendingTimeStretchToProject;
            placement.legacyClipId = clip.id;
            playlist.placements.push_back(placement);
        }
        duplicatedPlaylists.push_back(playlist);
    }

    project.tracks.insert(sourceIt + 1, duplicatedTrack);
    project.clips.insert(project.clips.end(), duplicatedClips.begin(), duplicatedClips.end());
    project.trackPlaylists.insert(project.trackPlaylists.end(), duplicatedPlaylists.begin(), duplicatedPlaylists.end());
    newTrackName = duplicatedTrack.name;
    return true;
}

std::string recordingTargetTrackName(const ProjectDocument& project) {
    for (const auto& track : project.tracks) {
        if (track.recordArmed && !isProtectedTrackName(track.name) && isTimelineTargetTrackType(track)) {
            return track.name;
        }
    }
    for (const auto& track : project.tracks) {
        if (!track.name.empty() && !isProtectedTrackName(track.name) && isTimelineTargetTrackType(track)) {
            return track.name;
        }
    }
    return {};
}

int inputChannelCountForBusName(const std::string& inputBusName) {
    const std::string cleanName = trimTrackName(inputBusName);
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

int recordingInputChannelCount(const ProjectDocument& project) {
    const std::string targetTrackName = recordingTargetTrackName(project);
    auto trackIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == targetTrackName;
    });
    if (trackIt == project.tracks.end()) {
        return 0;
    }
    if (!trackSupportsPhysicalInputMonitoring(*trackIt)) {
        return 0;
    }
    if (trackIt->channelFormat == "mono") {
        return 1;
    }
    return inputChannelCountForBusName(trackIt->inputBus);
}

std::string appendAudioClip(ProjectDocument& project,
                            const std::string& trackName,
                            const std::string& sourcePath,
                            double durationSeconds) {
    return appendAudioClipAt(project, trackName, sourcePath, projectEndSeconds(project), durationSeconds);
}

std::string appendAudioClipAt(ProjectDocument& project,
                              const std::string& trackName,
                              const std::string& sourcePath,
                              double startSeconds,
                              double durationSeconds) {
    if (sourcePath.empty() || durationSeconds <= 0.0 || !std::isfinite(durationSeconds) ||
        startSeconds < 0.0 || !std::isfinite(startSeconds) ||
        isProtectedTrackName(trackName) || !trackNameExists(project, trackName)) {
        return {};
    }
    TrackState* targetTrack = findTrack(project, trackName);
    if (targetTrack == nullptr || !isTimelineTargetTrackType(*targetTrack)) {
        return {};
    }

    ClipState clip;
    clip.id = uniqueImportedClipId(project);
    clip.trackName = trackName;
    clip.sourcePath = sourcePath;
    clip.regionName = pathStemForRegionName(sourcePath);
    clip.sourceFileUid = sourceUidForPath(sourcePath);
    populateClipSourceMetadata(clip);
    clip.colorHex = automaticClipColorForTrack(project, findTrack(project, trackName));
    clip.startSeconds = startSeconds;
    clip.durationSeconds = durationSeconds;
    clip.sourceOffsetSeconds = 0.0;
    clip.gainDb = 0.0f;
    clip.fadeInSeconds = 0.0;
    clip.fadeOutSeconds = 0.0;
    clip.muted = false;
    project.clips.push_back(clip);
    rebuildProjectEditModelFromClips(project);
    return clip.id;
}

std::string appendVideoReferenceClip(ProjectDocument& project,
                                     const std::string& sourcePath,
                                     double startSeconds,
                                     double durationSeconds,
                                     double frameRate,
                                     int width,
                                     int height,
                                     bool hasAudio) {
    if (sourcePath.empty() ||
        startSeconds < 0.0 || !std::isfinite(startSeconds) ||
        durationSeconds <= 0.0 || !std::isfinite(durationSeconds)) {
        return {};
    }
    frameRate = std::isfinite(frameRate) ? std::max(1.0, std::min(240.0, frameRate)) : 30.0;
    project.videoFrameRate = frameRate;

    VideoSourceState source;
    source.id = uniqueVideoSourceId(project);
    source.path = sourcePath;
    source.displayName = pathStemForRegionName(sourcePath);
    source.frameRate = frameRate;
    source.durationSeconds = durationSeconds;
    source.width = std::max(0, width);
    source.height = std::max(0, height);
    source.hasAudio = hasAudio;
    project.videoSources.push_back(source);

    VideoClipState clip;
    clip.id = uniqueVideoClipId(project);
    clip.sourceId = source.id;
    clip.name = source.displayName;
    clip.startSeconds = startSeconds;
    clip.durationSeconds = durationSeconds;
    clip.sourceOffsetSeconds = 0.0;
    clip.sourceTimecodeStartSeconds = project.timecodeStartSeconds;
    clip.muted = false;
    clip.locked = false;
    project.videoClips.push_back(clip);
    return clip.id;
}

bool appendRecordedTakeClip(ProjectDocument& project,
                            const std::string& trackName,
                            const std::string& sourcePath,
                            double startSeconds,
                            double durationSeconds,
                            std::string& newClipId,
                            std::string& message) {
    newClipId.clear();
    message.clear();
    if (sourcePath.empty()) {
        message = "Recorded take path is empty.";
        return false;
    }
    if (!std::isfinite(durationSeconds)) {
        message = "Recorded take reported an invalid duration.";
        return false;
    }
    if (startSeconds < 0.0 || !std::isfinite(startSeconds)) {
        message = "Recording start time is invalid.";
        return false;
    }
    TrackState* targetTrack = findTrack(project, trackName);
    if (trackName.empty() || isProtectedTrackName(trackName) || targetTrack == nullptr || !isTimelineTargetTrackType(*targetTrack)) {
        message = "No editable recording target track is available.";
        return false;
    }
    {
        std::error_code error;
        const std::filesystem::path source(sourcePath);
        if (!std::filesystem::exists(source, error) || !std::filesystem::is_regular_file(source, error)) {
            message = "Recorded take file is missing.";
            return false;
        }
    }
    double actualDurationSeconds = 0.0;
    {
        WavAudioData audio;
        std::string error;
        if (!readPcmWavFile(sourcePath, audio, error) ||
            audio.channels <= 0 || audio.sampleRate <= 0 || audio.frameCount() <= 0) {
        message = error.empty()
                ? "Recorded take file is not a readable WAV."
                : "Recorded take file is not a readable WAV: " + error;
            return false;
        }
        actualDurationSeconds = static_cast<double>(audio.frameCount()) / static_cast<double>(audio.sampleRate);
    }
    newClipId = appendAudioClipAt(project, trackName, sourcePath, startSeconds, actualDurationSeconds);
    if (newClipId.empty()) {
        message = "Could not append recorded take to the project.";
        return false;
    }
    message = "Recorded take appended.";
    return true;
}

bool appendRecordedMidiTakeRegion(ProjectDocument& project,
                                  const std::string& trackName,
                                  const std::vector<RecordedMidiEvent>& events,
                                  double startSeconds,
                                  double durationSeconds,
                                  std::string& newRegionId,
                                  std::string& message) {
    return appendRecordedMidiTakeRegion(project,
                                        trackName,
                                        events,
                                        startSeconds,
                                        durationSeconds,
                                        newRegionId,
                                        message,
                                        "new-region",
                                        -1.0,
                                        -1.0);
}

bool appendRecordedMidiTakeRegion(ProjectDocument& project,
                                  const std::string& trackName,
                                  const std::vector<RecordedMidiEvent>& events,
                                  double startSeconds,
                                  double durationSeconds,
                                  std::string& newRegionId,
                                  std::string& message,
                                  const std::string& mode,
                                  double punchStartSeconds,
                                  double punchEndSeconds) {
    newRegionId.clear();
    message.clear();
    if (trackName.empty() || isProtectedTrackName(trackName) || startSeconds < 0.0 ||
        !std::isfinite(startSeconds) || !std::isfinite(durationSeconds)) {
        message = "No editable MIDI recording target track is available.";
        return false;
    }
    TrackState* targetTrack = findTrack(project, trackName);
    if (targetTrack == nullptr || (targetTrack->trackType != "midi" && targetTrack->trackType != "instrument")) {
        message = "No editable MIDI recording target track is available.";
        return false;
    }
    if (events.empty() || durationSeconds <= 0.0) {
        message = "MIDI recording did not capture any events.";
        return false;
    }

    struct ActiveNote {
        double startSeconds = 0.0;
        int velocity = 1;
    };
    std::map<std::pair<int, int>, ActiveNote> activeNotes;
    struct CompletedNote {
        int channel = 1;
        int pitch = 60;
        int velocity = 96;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
    };
    std::vector<CompletedNote> notes;
    std::vector<RecordedMidiEvent> controllerEvents;
    std::vector<RecordedMidiEvent> pitchBendEvents;
    std::vector<RecordedMidiEvent> programChangeEvents;
    double lastEventSeconds = 0.0;

    for (const auto& event : events) {
        if (!std::isfinite(event.timeSeconds) || event.timeSeconds < 0.0) {
            continue;
        }
        const double eventSeconds = std::max(0.0, event.timeSeconds);
        lastEventSeconds = std::max(lastEventSeconds, eventSeconds);
        const int channel = std::max(1, std::min(16, event.channel));
        const int pitch = std::max(0, std::min(127, event.pitch));
        const auto noteKey = std::make_pair(channel, pitch);
        if (event.kind == RecordedMidiEventKind::NoteOn && event.velocity > 0) {
            activeNotes[noteKey] = {eventSeconds, std::max(1, std::min(127, event.velocity))};
        } else if (event.kind == RecordedMidiEventKind::NoteOff ||
                   (event.kind == RecordedMidiEventKind::NoteOn && event.velocity <= 0)) {
            const auto activeIt = activeNotes.find(noteKey);
            if (activeIt != activeNotes.end()) {
                notes.push_back({
                    channel,
                    pitch,
                    activeIt->second.velocity,
                    activeIt->second.startSeconds,
                    std::max(activeIt->second.startSeconds + 0.001, eventSeconds)
                });
                activeNotes.erase(activeIt);
            }
        } else if (event.kind == RecordedMidiEventKind::Controller) {
            controllerEvents.push_back(event);
        } else if (event.kind == RecordedMidiEventKind::PitchBend) {
            pitchBendEvents.push_back(event);
        } else if (event.kind == RecordedMidiEventKind::ProgramChange) {
            programChangeEvents.push_back(event);
        }
    }

    const double regionDurationSeconds = std::max(durationSeconds, lastEventSeconds + 0.001);
    for (const auto& active : activeNotes) {
        notes.push_back({
            active.first.first,
            active.first.second,
            active.second.velocity,
            active.second.startSeconds,
            std::max(active.second.startSeconds + 0.001, regionDurationSeconds)
        });
    }

    if (notes.empty() && controllerEvents.empty() && pitchBendEvents.empty() && programChangeEvents.empty()) {
        message = "MIDI recording did not capture any usable events.";
        return false;
    }

    const double recordEndSeconds = startSeconds + regionDurationSeconds;
    double replaceStartSeconds = startSeconds;
    double replaceEndSeconds = recordEndSeconds;
    if (mode == "punch" && std::isfinite(punchStartSeconds) && std::isfinite(punchEndSeconds) &&
        punchEndSeconds > punchStartSeconds) {
        replaceStartSeconds = std::max(startSeconds, punchStartSeconds);
        replaceEndSeconds = std::min(recordEndSeconds, punchEndSeconds);
    }

    MidiRegionState* targetRegion = nullptr;
    if (mode == "merge" || mode == "overdub" || mode == "replace" || mode == "punch") {
        for (auto& region : project.midiRegions) {
            const double regionEnd = region.startSeconds + region.durationSeconds;
            if (region.trackName == trackName && !region.locked && !region.muted &&
                region.startSeconds <= replaceEndSeconds && regionEnd >= replaceStartSeconds) {
                targetRegion = &region;
                break;
            }
        }
    }

    if (targetRegion == nullptr) {
        newRegionId = addMidiRegion(project, trackName, startSeconds, std::max(0.05, regionDurationSeconds), "MIDI Recording");
        if (newRegionId.empty()) {
            message = "Could not append MIDI recording to the project.";
            return false;
        }
        auto targetRegionIt = std::find_if(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
            return region.id == newRegionId;
        });
        targetRegion = targetRegionIt == project.midiRegions.end() ? nullptr : &(*targetRegionIt);
        if (targetRegion == nullptr) {
            message = "Could not append MIDI recording to the project.";
            return false;
        }
    } else {
        newRegionId = targetRegion->id;
        const double targetEnd = std::max(targetRegion->startSeconds + targetRegion->durationSeconds, recordEndSeconds);
        targetRegion->durationSeconds = std::max(0.05, targetEnd - targetRegion->startSeconds);
    }

    const double bpm = std::max(20.0, std::min(400.0, projectTempoAtSeconds(project, targetRegion->startSeconds)));
    const double beatsPerSecond = bpm / 60.0;
    const double secondsPerBeat = 60.0 / bpm;

    if ((mode == "replace" || mode == "punch") && replaceEndSeconds > replaceStartSeconds) {
        targetRegion->notes.erase(std::remove_if(targetRegion->notes.begin(), targetRegion->notes.end(), [&](const MidiNoteState& note) {
            const double noteStart = targetRegion->startSeconds + note.startBeats * secondsPerBeat;
            const double noteEnd = noteStart + note.durationBeats * secondsPerBeat;
            return noteStart < replaceEndSeconds && noteEnd > replaceStartSeconds;
        }), targetRegion->notes.end());
        targetRegion->controllerEvents.erase(std::remove_if(targetRegion->controllerEvents.begin(), targetRegion->controllerEvents.end(), [&](const MidiControllerEventState& event) {
            const double eventSeconds = targetRegion->startSeconds + event.beat * secondsPerBeat;
            return eventSeconds >= replaceStartSeconds && eventSeconds <= replaceEndSeconds;
        }), targetRegion->controllerEvents.end());
        targetRegion->pitchBendEvents.erase(std::remove_if(targetRegion->pitchBendEvents.begin(), targetRegion->pitchBendEvents.end(), [&](const MidiPitchBendEventState& event) {
            const double eventSeconds = targetRegion->startSeconds + event.beat * secondsPerBeat;
            return eventSeconds >= replaceStartSeconds && eventSeconds <= replaceEndSeconds;
        }), targetRegion->pitchBendEvents.end());
        targetRegion->programChangeEvents.erase(std::remove_if(targetRegion->programChangeEvents.begin(), targetRegion->programChangeEvents.end(), [&](const MidiProgramChangeEventState& event) {
            const double eventSeconds = targetRegion->startSeconds + event.beat * secondsPerBeat;
            return eventSeconds >= replaceStartSeconds && eventSeconds <= replaceEndSeconds;
        }), targetRegion->programChangeEvents.end());
    }

    const auto eventBeat = [&](double eventSeconds) {
        return std::max(0.0, (startSeconds + eventSeconds - targetRegion->startSeconds) * beatsPerSecond);
    };
    for (const auto& note : notes) {
        addMidiNote(project,
                    newRegionId,
                    note.pitch,
                    eventBeat(note.startSeconds),
                    std::max(1.0 / 960.0, (note.endSeconds - note.startSeconds) * beatsPerSecond),
                    note.velocity,
                    note.channel);
    }
    for (const auto& event : controllerEvents) {
        addMidiControllerEvent(project,
                               newRegionId,
                               eventBeat(event.timeSeconds),
                               event.controller,
                               event.value,
                               event.channel);
    }
    for (const auto& event : pitchBendEvents) {
        addMidiPitchBendEvent(project,
                              newRegionId,
                              eventBeat(event.timeSeconds),
                              event.value,
                              event.channel);
    }
    for (const auto& event : programChangeEvents) {
        addMidiProgramChangeEvent(project,
                                  newRegionId,
                                  eventBeat(event.timeSeconds),
                                  event.program,
                                  event.channel);
    }
    if (mode == "replace" || mode == "punch") {
        message = "MIDI recording replaced.";
    } else if (mode == "merge" || mode == "overdub") {
        message = "MIDI recording merged.";
    } else {
        message = "MIDI recording appended.";
    }
    return true;
}

bool deleteTrackAutomationLanePoint(ProjectDocument& project,
                                    const std::string& trackName,
                                    const std::string& parameterId,
                                    size_t pointIndex) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !trackSupportsSignalControls(*track) || parameterId.empty()) {
        return false;
    }
    auto laneIt = std::find_if(track->automationLanes.begin(), track->automationLanes.end(), [&](const AutomationLaneState& lane) {
        return lane.parameterId == parameterId;
    });
    if (laneIt == track->automationLanes.end() || pointIndex >= laneIt->points.size()) {
        return false;
    }
    laneIt->points.erase(laneIt->points.begin() + static_cast<std::ptrdiff_t>(pointIndex));
    if (laneIt->points.empty()) {
        track->automationLanes.erase(laneIt);
    }
    return true;
}

size_t deleteTrackAutomationLanePointsInRange(ProjectDocument& project,
                                             const std::string& trackName,
                                             const std::string& parameterId,
                                             double rangeStartSeconds,
                                             double rangeEndSeconds) {
    TrackState* track = findTrack(project, trackName);
    const bool protectedAutomationTarget = isProtectedTrackName(trackName) && trackName != "Master";
    if (track == nullptr ||
        protectedAutomationTarget ||
        !trackSupportsSignalControls(*track) ||
        parameterId.empty() ||
        !std::isfinite(rangeStartSeconds) ||
        !std::isfinite(rangeEndSeconds)) {
        return 0;
    }
    auto laneIt = std::find_if(track->automationLanes.begin(), track->automationLanes.end(), [&](const AutomationLaneState& lane) {
        return lane.parameterId == parameterId;
    });
    if (laneIt == track->automationLanes.end()) {
        return 0;
    }
    if (rangeEndSeconds < rangeStartSeconds) {
        std::swap(rangeStartSeconds, rangeEndSeconds);
    }
    constexpr double epsilon = 0.0005;
    const double start = std::max(0.0, rangeStartSeconds - epsilon);
    const double end = std::max(start, rangeEndSeconds + epsilon);
    const size_t before = laneIt->points.size();
    laneIt->points.erase(std::remove_if(laneIt->points.begin(), laneIt->points.end(), [&](const AutomationPointState& point) {
        return std::isfinite(point.timeSeconds) && point.timeSeconds >= start && point.timeSeconds <= end;
    }), laneIt->points.end());
    const size_t deleted = before - laneIt->points.size();
    if (laneIt->points.empty()) {
        track->automationLanes.erase(laneIt);
    }
    return deleted;
}

std::string addAudioTrack(ProjectDocument& project) {
    const std::string name = nextAudioTrackName(project);
    TrackState track;
    track.name = name;
    track.trackType = "audio";
    static const char* colors[] = {"#35BFA8", "#4B84E8", "#F0B84D", "#D86BA6", "#7CCB5E", "#A078E8", "#E26D5A", "#5BC0DE"};
    size_t editableCount = 0;
    for (const auto& candidate : project.tracks) {
        if (!isProtectedTrackName(candidate.name)) {
            ++editableCount;
        }
    }
    track.colorHex = colors[editableCount % (sizeof(colors) / sizeof(colors[0]))];
    auto master = std::find_if(project.tracks.begin(), project.tracks.end(), [](const TrackState& candidate) {
        return isProtectedTrackName(candidate.name);
    });
    if (master != project.tracks.end()) {
        project.tracks.insert(master, track);
    } else {
        project.tracks.push_back(track);
    }
    return name;
}

std::string addMidiTrack(ProjectDocument& project) {
    std::set<std::string> used;
    for (const auto& track : project.tracks) {
        used.insert(track.name);
    }
    std::string name = "MIDI 1";
    for (int suffix = 1; suffix < 100000; ++suffix) {
        const std::string candidate = "MIDI " + std::to_string(suffix);
        if (used.find(candidate) == used.end()) {
            name = candidate;
            break;
        }
    }
    TrackState track;
    track.name = name;
    track.trackType = "midi";
    track.inputBus = "MIDI Input";
    track.outputBus.clear();
    static const char* colors[] = {"#4B84E8", "#35BFA8", "#A078E8", "#F0B84D", "#D86BA6", "#7CCB5E", "#E26D5A", "#5BC0DE"};
    size_t midiCount = 0;
    for (const auto& candidate : project.tracks) {
        if (candidate.trackType == "midi" || candidate.trackType == "instrument") {
            ++midiCount;
        }
    }
    track.colorHex = colors[midiCount % (sizeof(colors) / sizeof(colors[0]))];
    auto master = std::find_if(project.tracks.begin(), project.tracks.end(), [](const TrackState& candidate) {
        return isProtectedTrackName(candidate.name);
    });
    if (master != project.tracks.end()) {
        project.tracks.insert(master, track);
    } else {
        project.tracks.push_back(track);
    }
    return name;
}

std::string addInstrumentTrack(ProjectDocument& project) {
    std::set<std::string> used;
    for (const auto& track : project.tracks) {
        used.insert(track.name);
    }
    std::string name = "Instrument 1";
    for (int suffix = 1; suffix < 100000; ++suffix) {
        const std::string candidate = "Instrument " + std::to_string(suffix);
        if (used.find(candidate) == used.end()) {
            name = candidate;
            break;
        }
    }
    TrackState track;
    track.name = name;
    track.trackType = "instrument";
    track.inputBus = "MIDI Input";
    track.outputBus = "Master";
    track.instrument.pluginName = "No Instrument";
    track.instrument.pluginFormat = "None";
    track.instrument.enabled = false;
    track.instrument.midiInput = "MIDI Input";
    track.instrument.midiChannel = 0;
    static const char* colors[] = {"#A078E8", "#4B84E8", "#35BFA8", "#F0B84D", "#D86BA6", "#7CCB5E", "#E26D5A", "#5BC0DE"};
    size_t instrumentCount = 0;
    for (const auto& candidate : project.tracks) {
        if (candidate.trackType == "instrument") {
            ++instrumentCount;
        }
    }
    track.colorHex = colors[instrumentCount % (sizeof(colors) / sizeof(colors[0]))];
    auto master = std::find_if(project.tracks.begin(), project.tracks.end(), [](const TrackState& candidate) {
        return isProtectedTrackName(candidate.name);
    });
    if (master != project.tracks.end()) {
        project.tracks.insert(master, track);
    } else {
        project.tracks.push_back(track);
    }
    return name;
}

std::string addFolderTrack(ProjectDocument& project) {
    std::set<std::string> used;
    for (const auto& track : project.tracks) {
        used.insert(track.name);
    }
    std::string name = "Folder 1";
    for (int suffix = 1; suffix < 100000; ++suffix) {
        const std::string candidate = "Folder " + std::to_string(suffix);
        if (used.find(candidate) == used.end()) {
            name = candidate;
            break;
        }
    }
    TrackState track;
    track.name = name;
    track.trackType = "folder";
    track.inputBus.clear();
    track.outputBus.clear();
    track.colorHex = "#7CCB5E";
    auto master = std::find_if(project.tracks.begin(), project.tracks.end(), [](const TrackState& candidate) {
        return isProtectedTrackName(candidate.name);
    });
    if (master != project.tracks.end()) {
        project.tracks.insert(master, track);
    } else {
        project.tracks.push_back(track);
    }
    return name;
}

std::string addBusFolderTrack(ProjectDocument& project, const std::string& inputBusName) {
    std::set<std::string> used;
    for (const auto& track : project.tracks) {
        used.insert(track.name);
    }
    std::string name = "Bus Folder 1";
    for (int suffix = 1; suffix < 100000; ++suffix) {
        const std::string candidate = "Bus Folder " + std::to_string(suffix);
        if (used.find(candidate) == used.end()) {
            name = candidate;
            break;
        }
    }
    TrackState track;
    track.name = name;
    track.trackType = "bus_folder";
    track.inputBus = trimTrackName(inputBusName).empty() ? "Bus 1-2" : trimTrackName(inputBusName);
    track.outputBus = "Master";
    track.colorHex = "#F0B84D";
    auto master = std::find_if(project.tracks.begin(), project.tracks.end(), [](const TrackState& candidate) {
        return isProtectedTrackName(candidate.name);
    });
    if (master != project.tracks.end()) {
        project.tracks.insert(master, track);
    } else {
        project.tracks.push_back(track);
    }
    return name;
}

std::string addVcaTrack(ProjectDocument& project) {
    const std::string name = nextTrackNameWithPrefix(project, "VCA");
    TrackState track;
    track.name = name;
    track.trackType = "vca";
    track.inputBus.clear();
    track.outputBus.clear();
    track.pan = 0.0f;
    track.colorHex = "#D86BA6";
    auto master = std::find_if(project.tracks.begin(), project.tracks.end(), [](const TrackState& candidate) {
        return isProtectedTrackName(candidate.name);
    });
    if (master != project.tracks.end()) {
        project.tracks.insert(master, track);
    } else {
        project.tracks.push_back(track);
    }
    return name;
}

bool setTrackFolderCollapsed(ProjectDocument& project, const std::string& trackName, bool collapsed) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || (track->trackType != "folder" && track->trackType != "bus_folder")) {
        return false;
    }
    track->folderCollapsed = collapsed;
    return true;
}

int moveTrack(ProjectDocument& project, const std::string& trackName, int direction) {
    if (direction == 0 || isProtectedTrackName(trackName)) {
        return -1;
    }
    const auto it = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    if (it == project.tracks.end() || isProtectedTrackName(it->name)) {
        return -1;
    }
    const std::string sourceFolderName = isFolderTrackType(*it) ? it->name : std::string {};
    const auto sourceInBlock = [&](const TrackState& track) {
        return sourceFolderName.empty()
            ? track.name == trackName
            : trackBelongsToFolderBlock(track, sourceFolderName);
    };
    const int currentIndex = static_cast<int>(std::distance(project.tracks.begin(), it));
    int targetIndex = currentIndex + (direction < 0 ? -1 : 1);
    while (targetIndex >= 0 && targetIndex < static_cast<int>(project.tracks.size()) &&
           sourceInBlock(project.tracks[static_cast<size_t>(targetIndex)])) {
        targetIndex += direction < 0 ? -1 : 1;
    }
    if (targetIndex < 0 || targetIndex >= static_cast<int>(project.tracks.size()) ||
        isProtectedTrackName(project.tracks[static_cast<size_t>(targetIndex)].name)) {
        return -1;
    }
    const std::string targetTrackName = project.tracks[static_cast<size_t>(targetIndex)].name;
    if (!moveTrackNearTrack(project, trackName, targetTrackName, direction > 0)) {
        return -1;
    }
    const auto movedIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    return movedIt == project.tracks.end() ? -1 : static_cast<int>(std::distance(project.tracks.begin(), movedIt));
}

bool moveTrackNearTrack(ProjectDocument& project,
                        const std::string& trackName,
                        const std::string& targetTrackName,
                        bool insertAfterTarget) {
    if (trackName.empty() || targetTrackName.empty() || trackName == targetTrackName ||
        isProtectedTrackName(trackName) || targetTrackName == "Monitor") {
        return false;
    }
    auto sourceIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    auto targetIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == targetTrackName;
    });
    if (sourceIt == project.tracks.end() || targetIt == project.tracks.end() ||
        isProtectedTrackName(sourceIt->name)) {
        return false;
    }
    if (isProtectedTrackName(targetIt->name) && insertAfterTarget) {
        insertAfterTarget = false;
    }

    const std::string movingFolderName = isFolderTrackType(*sourceIt) ? sourceIt->name : std::string {};
    const std::string sourceParentFolderName = movingFolderName.empty() ? sourceIt->folderName : std::string {};
    const std::string targetParentFolderName = isFolderTrackType(*targetIt) ? targetIt->name : targetIt->folderName;
    const bool targetIsSourceParentFolder = !sourceParentFolderName.empty() &&
        targetIt->name == sourceParentFolderName &&
        isFolderTrackType(*targetIt);
    const bool keepIndividualInSourceFolder = movingFolderName.empty() &&
        !sourceParentFolderName.empty() &&
        targetParentFolderName == sourceParentFolderName;
    if (targetIsSourceParentFolder) {
        insertAfterTarget = true;
    }
    const auto movingPredicate = [&](const TrackState& track) {
        return movingFolderName.empty()
            ? track.name == trackName
            : trackBelongsToFolderBlock(track, movingFolderName);
    };
    if (movingPredicate(*targetIt)) {
        return false;
    }

    std::vector<TrackState> moving;
    moving.reserve(project.tracks.size());
    for (const auto& track : project.tracks) {
        if (movingPredicate(track)) {
            TrackState copy = track;
            if (movingFolderName.empty()) {
                copy.folderName = keepIndividualInSourceFolder ? sourceParentFolderName : std::string {};
            }
            moving.push_back(copy);
        }
    }
    if (moving.empty()) {
        return false;
    }

    project.tracks.erase(
        std::remove_if(project.tracks.begin(), project.tracks.end(), movingPredicate),
        project.tracks.end());
    targetIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == targetTrackName;
    });
    if (targetIt == project.tracks.end()) {
        project.tracks.insert(project.tracks.end(), moving.begin(), moving.end());
        return false;
    }
    size_t targetIndex = static_cast<size_t>(std::distance(project.tracks.begin(), targetIt));
    size_t insertIndex = targetIndex + (insertAfterTarget ? 1 : 0);
    insertIndex = std::min(insertIndex, project.tracks.size());
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(insertIndex), moving.begin(), moving.end());
    return true;
}

bool moveTrackIntoFolder(ProjectDocument& project, const std::string& trackName, const std::string& folderName) {
    if (trackName.empty() || folderName.empty() || trackName == folderName ||
        isProtectedTrackName(trackName) || isProtectedTrackName(folderName)) {
        return false;
    }
    auto sourceIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    auto folderIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == folderName;
    });
    if (sourceIt == project.tracks.end() || folderIt == project.tracks.end()) {
        return false;
    }
    if (sourceIt->trackType == "folder" || sourceIt->trackType == "bus_folder" ||
        (folderIt->trackType != "folder" && folderIt->trackType != "bus_folder")) {
        return false;
    }

    TrackState moved = *sourceIt;
    moved.folderName = folderName;
    size_t sourceIndex = static_cast<size_t>(std::distance(project.tracks.begin(), sourceIt));
    size_t folderIndex = static_cast<size_t>(std::distance(project.tracks.begin(), folderIt));
    project.tracks.erase(project.tracks.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
    if (sourceIndex < folderIndex) {
        --folderIndex;
    }
    project.tracks[folderIndex].folderCollapsed = false;

    size_t insertIndex = folderIndex + 1;
    while (insertIndex < project.tracks.size()) {
        const auto& candidate = project.tracks[insertIndex];
        if (isProtectedTrackName(candidate.name) ||
            candidate.trackType == "folder" ||
            candidate.trackType == "bus_folder") {
            break;
        }
        if (candidate.folderName != folderName) {
            break;
        }
        ++insertIndex;
    }
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(insertIndex), moved);
    return true;
}

size_t trackTimelineItemCount(const ProjectDocument& project, const std::string& trackName) {
    size_t count = static_cast<size_t>(std::count_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.trackName == trackName;
    }));
    count += static_cast<size_t>(std::count_if(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
        return region.trackName == trackName;
    }));
    return count;
}

size_t folderChildTrackCount(const ProjectDocument& project, const std::string& folderName) {
    return static_cast<size_t>(std::count_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.folderName == folderName;
    }));
}

bool deleteTrack(ProjectDocument& project,
                 const std::string& trackName,
                 bool removeTimelineItems,
                 bool deleteFolderChildren) {
    if (trackName.empty() || isProtectedTrackName(trackName)) {
        return false;
    }
    auto trackIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    if (trackIt == project.tracks.end()) {
        return false;
    }

    const bool deletingFolder = isFolderTrackType(*trackIt);
    std::set<std::string> removedTrackNames {trackName};
    if (deletingFolder && deleteFolderChildren) {
        for (const auto& track : project.tracks) {
            if (track.folderName == trackName && !isProtectedTrackName(track.name)) {
                removedTrackNames.insert(track.name);
            }
        }
    }

    const bool hasTimelineItems = std::any_of(removedTrackNames.begin(), removedTrackNames.end(), [&](const std::string& name) {
        return trackTimelineItemCount(project, name) > 0;
    });
    if (hasTimelineItems && !removeTimelineItems) {
        return false;
    }

    if (removeTimelineItems) {
        project.clips.erase(
            std::remove_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
                return removedTrackNames.find(clip.trackName) != removedTrackNames.end();
            }),
            project.clips.end());
        project.midiRegions.erase(
            std::remove_if(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
                return removedTrackNames.find(region.trackName) != removedTrackNames.end();
            }),
            project.midiRegions.end());
        project.trackPlaylists.erase(
            std::remove_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& playlist) {
                return removedTrackNames.find(playlist.trackName) != removedTrackNames.end();
            }),
            project.trackPlaylists.end());
    }

    const auto before = project.tracks.size();
    project.tracks.erase(
        std::remove_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
            return removedTrackNames.find(track.name) != removedTrackNames.end();
        }),
        project.tracks.end());
    if (project.tracks.size() == before) {
        return false;
    }

    for (auto& track : project.tracks) {
        if (removedTrackNames.find(track.folderName) != removedTrackNames.end()) {
            track.folderName.clear();
        }
        if (removedTrackNames.find(track.controlMasterTrackName) != removedTrackNames.end()) {
            track.controlMasterTrackName.clear();
        }
        if (removedTrackNames.find(track.inputBus) != removedTrackNames.end()) {
            track.inputBus.clear();
        }
        if (removedTrackNames.find(track.outputBus) != removedTrackNames.end()) {
            track.outputBus = "Master";
        }
        for (auto& send : track.sends) {
            if (removedTrackNames.find(send.busName) != removedTrackNames.end()) {
                send.busName.clear();
                send.enabled = false;
            }
        }
    }
    if (removedTrackNames.find(project.tempoMasterTrackName) != removedTrackNames.end()) {
        project.tempoMasterTrackName.clear();
    }
    return true;
}

bool deleteTrackIfEmpty(ProjectDocument& project, const std::string& trackName) {
    if (isProtectedTrackName(trackName) ||
        trackHasClips(project, trackName) ||
        trackHasMidiRegions(project, trackName) ||
        folderChildTrackCount(project, trackName) > 0) {
        return false;
    }
    return deleteTrack(project, trackName, false, false);
}

std::string createTrackPlaylist(ProjectDocument& project, const std::string& trackName, const std::string& playlistName) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !isTimelineTargetTrackType(*track)) {
        return {};
    }
    normalizeProjectEditModel(project);
    TrackPlaylistState playlist;
    playlist.id = uniqueTrackPlaylistId(project, trackName);
    playlist.trackName = trackName;
    const size_t existingForTrack = static_cast<size_t>(std::count_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& existing) {
        return existing.trackName == trackName;
    }));
    playlist.name = playlistName.empty() ? "Playlist " + std::to_string(existingForTrack + 1) : playlistName;
    playlist.active = false;
    project.trackPlaylists.push_back(playlist);
    return project.trackPlaylists.back().id;
}

std::string duplicateActiveTrackPlaylist(ProjectDocument& project, const std::string& trackName, const std::string& playlistName) {
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || isProtectedTrackName(trackName) || !isTimelineTargetTrackType(*track)) {
        return {};
    }
    normalizeProjectEditModel(project);
    TrackPlaylistState* active = activePlaylistForTrack(project, trackName);
    if (active == nullptr) {
        return createTrackPlaylist(project, trackName, playlistName);
    }
    TrackPlaylistState duplicate = *active;
    duplicate.id = uniqueTrackPlaylistId(project, trackName);
    const size_t existingForTrack = static_cast<size_t>(std::count_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& existing) {
        return existing.trackName == trackName;
    }));
    duplicate.name = playlistName.empty() ? "Playlist " + std::to_string(existingForTrack + 1) : playlistName;
    duplicate.active = false;
    project.trackPlaylists.push_back(duplicate);
    return project.trackPlaylists.back().id;
}

bool activateTrackPlaylist(ProjectDocument& project, const std::string& playlistId) {
    normalizeProjectEditModel(project);
    auto targetIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& playlist) {
        return playlist.id == playlistId;
    });
    if (targetIt == project.trackPlaylists.end()) {
        return false;
    }
    const std::string trackName = targetIt->trackName;
    for (auto& playlist : project.trackPlaylists) {
        if (playlist.trackName == trackName) {
            playlist.active = playlist.id == playlistId;
        }
    }
    rebuildProjectClipsFromActivePlaylists(project);
    return true;
}

bool renameTrackPlaylist(ProjectDocument& project, const std::string& playlistId, const std::string& playlistName) {
    normalizeProjectEditModel(project);
    std::string cleanName = playlistName;
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    cleanName.erase(cleanName.begin(), std::find_if(cleanName.begin(), cleanName.end(), [&](unsigned char ch) {
        return !isSpace(ch);
    }));
    cleanName.erase(std::find_if(cleanName.rbegin(), cleanName.rend(), [&](unsigned char ch) {
        return !isSpace(ch);
    }).base(), cleanName.end());
    if (playlistId.empty() || cleanName.empty()) {
        return false;
    }
    auto targetIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](TrackPlaylistState& playlist) {
        return playlist.id == playlistId;
    });
    if (targetIt == project.trackPlaylists.end()) {
        return false;
    }
    cleanName = cleanName.substr(0, 64);
    targetIt->name = cleanName;
    return true;
}

bool deleteTrackPlaylist(ProjectDocument& project, const std::string& playlistId) {
    normalizeProjectEditModel(project);
    auto targetIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& playlist) {
        return playlist.id == playlistId;
    });
    if (targetIt == project.trackPlaylists.end()) {
        return false;
    }
    const std::string trackName = targetIt->trackName;
    const size_t playlistCountForTrack = static_cast<size_t>(std::count_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& playlist) {
        return playlist.trackName == trackName;
    }));
    if (playlistCountForTrack <= 1) {
        return false;
    }
    const bool deletingActive = targetIt->active;
    project.trackPlaylists.erase(targetIt);
    if (deletingActive) {
        auto nextIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](TrackPlaylistState& playlist) {
            return playlist.trackName == trackName;
        });
        if (nextIt != project.trackPlaylists.end()) {
            nextIt->active = true;
        }
    }
    rebuildProjectClipsFromActivePlaylists(project);
    return true;
}

bool copyPlaylistPlacementToActivePlaylist(ProjectDocument& project, const std::string& playlistId, const std::string& placementId) {
    normalizeProjectEditModel(project);
    if (playlistId.empty() || placementId.empty()) {
        return false;
    }
    auto sourcePlaylistIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](const TrackPlaylistState& playlist) {
        return playlist.id == playlistId;
    });
    if (sourcePlaylistIt == project.trackPlaylists.end()) {
        return false;
    }
    const auto placementIt = std::find_if(sourcePlaylistIt->placements.begin(), sourcePlaylistIt->placements.end(), [&](const PlaylistClipPlacementState& placement) {
        return placement.id == placementId || placement.legacyClipId == placementId;
    });
    if (placementIt == sourcePlaylistIt->placements.end()) {
        return false;
    }
    auto activePlaylistIt = std::find_if(project.trackPlaylists.begin(), project.trackPlaylists.end(), [&](TrackPlaylistState& playlist) {
        return playlist.trackName == sourcePlaylistIt->trackName && playlist.active;
    });
    if (activePlaylistIt == project.trackPlaylists.end()) {
        return false;
    }
    auto promoted = *placementIt;
    const bool needsNewPlacementId = promoted.id.empty() ||
        std::any_of(activePlaylistIt->placements.begin(), activePlaylistIt->placements.end(), [&](const PlaylistClipPlacementState& placement) {
            return placement.id == promoted.id;
        });
    if (needsNewPlacementId) {
        const std::string stem = promoted.legacyClipId.empty() ? "playlist-clip-comp" : promoted.legacyClipId + "-comp";
        std::string candidate = stem;
        int suffix = 2;
        while (std::any_of(activePlaylistIt->placements.begin(), activePlaylistIt->placements.end(), [&](const PlaylistClipPlacementState& placement) {
            return placement.id == candidate;
        })) {
            candidate = stem + "-" + std::to_string(suffix++);
        }
        promoted.id = candidate;
    }
    if (promoted.legacyClipId.empty() || needsNewPlacementId) {
        promoted.legacyClipId = promoted.id;
    }
    activePlaylistIt->placements.push_back(promoted);
    rebuildProjectClipsFromActivePlaylists(project);
    return true;
}

std::string addMarkerAt(ProjectDocument& project, double timeSeconds) {
    if (timeSeconds < 0.0 || !std::isfinite(timeSeconds)) {
        return {};
    }
    MarkerState marker;
    marker.id = uniqueMarkerId(project);
    marker.name = "Marker " + std::to_string(project.markers.size() + 1);
    marker.timeSeconds = timeSeconds;
    project.markers.push_back(marker);
    std::sort(project.markers.begin(), project.markers.end(), [](const MarkerState& a, const MarkerState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });
    return marker.id;
}

bool deleteNearestMarker(ProjectDocument& project, double timeSeconds, double toleranceSeconds) {
    if (project.markers.empty() || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.markers.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.markers.begin(); it != project.markers.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.markers.end()) {
        return false;
    }
    project.markers.erase(nearest);
    return true;
}

bool renameNearestMarker(ProjectDocument& project, double timeSeconds, double toleranceSeconds, const std::string& name) {
    if (project.markers.empty() || name.empty() || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.markers.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.markers.begin(); it != project.markers.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.markers.end()) {
        return false;
    }
    nearest->name = name;
    return true;
}

bool moveNearestMarker(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds) {
    if (project.markers.empty() || originalTimeSeconds < 0.0 || newTimeSeconds < 0.0 ||
        !std::isfinite(originalTimeSeconds) || !std::isfinite(newTimeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.markers.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.markers.begin(); it != project.markers.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - originalTimeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.markers.end()) {
        return false;
    }
    nearest->timeSeconds = newTimeSeconds;
    std::sort(project.markers.begin(), project.markers.end(), [](const MarkerState& a, const MarkerState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });
    return true;
}

std::string addChordEventAt(ProjectDocument& project, double timeSeconds, const std::string& name) {
    if (timeSeconds < 0.0 || !std::isfinite(timeSeconds) || name.empty()) {
        return {};
    }
    ChordEventState chord;
    chord.id = uniqueChordEventId(project);
    chord.name = name;
    chord.timeSeconds = timeSeconds;
    project.chordEvents.push_back(chord);
    std::sort(project.chordEvents.begin(), project.chordEvents.end(), [](const ChordEventState& a, const ChordEventState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });
    return chord.id;
}

bool renameNearestChordEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds, const std::string& name) {
    if (project.chordEvents.empty() || name.empty() || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.chordEvents.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.chordEvents.begin(); it != project.chordEvents.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.chordEvents.end()) {
        return false;
    }
    nearest->name = name;
    return true;
}

bool moveNearestChordEvent(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds) {
    if (project.chordEvents.empty() || originalTimeSeconds < 0.0 || newTimeSeconds < 0.0 ||
        !std::isfinite(originalTimeSeconds) || !std::isfinite(newTimeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.chordEvents.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.chordEvents.begin(); it != project.chordEvents.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - originalTimeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.chordEvents.end()) {
        return false;
    }
    nearest->timeSeconds = newTimeSeconds;
    std::sort(project.chordEvents.begin(), project.chordEvents.end(), [](const ChordEventState& a, const ChordEventState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });
    return true;
}

bool deleteNearestChordEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds) {
    if (project.chordEvents.empty() || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.chordEvents.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.chordEvents.begin(); it != project.chordEvents.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.chordEvents.end()) {
        return false;
    }
    project.chordEvents.erase(nearest);
    return true;
}

std::string addLyricEventAt(ProjectDocument& project, double timeSeconds, const std::string& text) {
    if (timeSeconds < 0.0 || !std::isfinite(timeSeconds) || text.empty()) {
        return {};
    }
    LyricEventState lyric;
    lyric.id = uniqueLyricEventId(project);
    lyric.text = text;
    lyric.timeSeconds = timeSeconds;
    project.lyricEvents.push_back(lyric);
    std::sort(project.lyricEvents.begin(), project.lyricEvents.end(), [](const LyricEventState& a, const LyricEventState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });
    return lyric.id;
}

LyricTranscriptionApplyResult applyLyricTranscription(ProjectDocument& project,
                                                      const std::vector<LyricTranscriptionSegment>& segments,
                                                      double projectTimeOffsetSeconds,
                                                      bool replaceOverlappingEvents) {
    LyricTranscriptionApplyResult result;
    if (!std::isfinite(projectTimeOffsetSeconds)) {
        result.message = "Invalid lyric transcription offset.";
        return result;
    }

    std::vector<LyricEventState> events;
    events.reserve(segments.size());
    double rangeStart = std::numeric_limits<double>::infinity();
    double rangeEnd = 0.0;
    for (const auto& segment : segments) {
        const std::string text = trimmedLyricText(segment.text);
        const double timeSeconds = segment.startSeconds + projectTimeOffsetSeconds;
        if (text.empty() || !std::isfinite(timeSeconds) || timeSeconds < 0.0) {
            continue;
        }
        LyricEventState lyric;
        lyric.text = text;
        lyric.timeSeconds = timeSeconds;
        events.push_back(lyric);
        rangeStart = std::min(rangeStart, timeSeconds);
        const double duration = std::isfinite(segment.durationSeconds) && segment.durationSeconds > 0.0
            ? segment.durationSeconds
            : 0.1;
        rangeEnd = std::max(rangeEnd, timeSeconds + duration);
    }

    if (events.empty()) {
        result.message = "No usable lyric transcription segments.";
        return result;
    }

    result.startSeconds = rangeStart;
    result.endSeconds = rangeEnd;
    if (replaceOverlappingEvents) {
        const double tolerance = 0.05;
        const auto beforeCount = project.lyricEvents.size();
        project.lyricEvents.erase(std::remove_if(project.lyricEvents.begin(), project.lyricEvents.end(), [&](const LyricEventState& lyric) {
            return std::isfinite(lyric.timeSeconds) &&
                   lyric.timeSeconds >= rangeStart - tolerance &&
                   lyric.timeSeconds <= rangeEnd + tolerance;
        }), project.lyricEvents.end());
        result.removedEvents = beforeCount - project.lyricEvents.size();
    }

    for (auto& event : events) {
        event.id = uniqueLyricEventId(project);
        project.lyricEvents.push_back(event);
        ++result.addedEvents;
    }
    std::sort(project.lyricEvents.begin(), project.lyricEvents.end(), [](const LyricEventState& a, const LyricEventState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });
    result.ok = result.addedEvents > 0;
    result.message = "Lyric transcription applied.";
    return result;
}

bool renameNearestLyricEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds, const std::string& text) {
    if (project.lyricEvents.empty() || text.empty() || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.lyricEvents.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.lyricEvents.begin(); it != project.lyricEvents.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.lyricEvents.end()) {
        return false;
    }
    nearest->text = text;
    return true;
}

bool moveNearestLyricEvent(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds) {
    if (project.lyricEvents.empty() || originalTimeSeconds < 0.0 || newTimeSeconds < 0.0 ||
        !std::isfinite(originalTimeSeconds) || !std::isfinite(newTimeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.lyricEvents.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.lyricEvents.begin(); it != project.lyricEvents.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - originalTimeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.lyricEvents.end()) {
        return false;
    }
    nearest->timeSeconds = newTimeSeconds;
    std::sort(project.lyricEvents.begin(), project.lyricEvents.end(), [](const LyricEventState& a, const LyricEventState& b) {
        if (a.timeSeconds == b.timeSeconds) {
            return a.id < b.id;
        }
        return a.timeSeconds < b.timeSeconds;
    });
    return true;
}

bool deleteNearestLyricEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds) {
    if (project.lyricEvents.empty() || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.lyricEvents.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.lyricEvents.begin(); it != project.lyricEvents.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.lyricEvents.end()) {
        return false;
    }
    project.lyricEvents.erase(nearest);
    return true;
}

std::string addMidiRegion(ProjectDocument& project,
                          const std::string& trackName,
                          double startSeconds,
                          double durationSeconds,
                          const std::string& name) {
    if (trackName.empty() || isProtectedTrackName(trackName) ||
        startSeconds < 0.0 || durationSeconds <= 0.0 ||
        !std::isfinite(startSeconds) || !std::isfinite(durationSeconds)) {
        return {};
    }
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || track->trackType == "folder" || track->trackType == "bus_folder" ||
        track->trackType == "master" || track->trackType == "monitor") {
        return {};
    }
    if (track->trackType != "midi" && track->trackType != "instrument") {
        track->trackType = "midi";
        if (track->inputBus.empty() || track->inputBus == "Input 1") {
            track->inputBus = "MIDI Input";
        }
        if (track->outputBus.empty() || track->outputBus == "Master") {
            track->outputBus = "Instrument";
        }
    }
    MidiRegionState region;
    region.id = uniqueMidiRegionId(project);
    region.trackName = trackName;
    region.name = name.empty() ? "MIDI Region" : name;
    region.startSeconds = startSeconds;
    region.durationSeconds = std::max(0.05, durationSeconds);
    region.ticksPerQuarter = 960;
    region.colorHex = track->colorHex.empty() ? "#4B84E8" : track->colorHex;
    project.midiRegions.push_back(region);
    std::sort(project.midiRegions.begin(), project.midiRegions.end(), [](const MidiRegionState& left, const MidiRegionState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });
    return region.id;
}

bool moveMidiRegion(ProjectDocument& project,
                    const std::string& regionId,
                    const std::string& trackName,
                    double startSeconds) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || trackName.empty() ||
        startSeconds < 0.0 || !std::isfinite(startSeconds)) {
        return false;
    }
    TrackState* track = findTrack(project, trackName);
    if (track == nullptr || track->trackType == "folder" || track->trackType == "bus_folder" ||
        track->trackType == "master" || track->trackType == "monitor" ||
        track->name == "Master" || track->name == "Monitor") {
        return false;
    }
    if (track->trackType != "midi" && track->trackType != "instrument") {
        track->trackType = "midi";
        if (track->inputBus.empty() || track->inputBus == "Input 1") {
            track->inputBus = "MIDI Input";
        }
        if (track->outputBus.empty() || track->outputBus == "Master") {
            track->outputBus = "Instrument";
        }
    }
    region->trackName = trackName;
    region->startSeconds = startSeconds;
    std::sort(project.midiRegions.begin(), project.midiRegions.end(), [](const MidiRegionState& left, const MidiRegionState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });
    return true;
}

bool resizeMidiRegion(ProjectDocument& project,
                      const std::string& regionId,
                      double durationSeconds) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || durationSeconds <= 0.0 ||
        !std::isfinite(durationSeconds)) {
        return false;
    }
    region->durationSeconds = std::max(0.05, durationSeconds);
    return true;
}

bool trimMidiRegionStart(ProjectDocument& project,
                         const std::string& regionId,
                         double newStartSeconds) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    const double oldStartSeconds = region->startSeconds;
    const double oldEndSeconds = region->startSeconds + region->durationSeconds;
    const double clampedStartSeconds = std::max(0.0, std::min(newStartSeconds, oldEndSeconds - 0.05));
    const double bpm = std::max(20.0, std::min(400.0, projectTempoAtSeconds(project, oldStartSeconds)));
    const double deltaBeats = (clampedStartSeconds - oldStartSeconds) * bpm / 60.0;
    region->startSeconds = clampedStartSeconds;
    region->durationSeconds = std::max(0.05, oldEndSeconds - clampedStartSeconds);
    for (auto& note : region->notes) {
        note.startBeats -= deltaBeats;
        if (note.startBeats < 0.0) {
            note.durationBeats = std::max(0.0, note.durationBeats + note.startBeats);
            note.startBeats = 0.0;
        }
    }
    region->notes.erase(std::remove_if(region->notes.begin(), region->notes.end(), [](const MidiNoteState& note) {
        return note.durationBeats <= 1.0 / 960.0;
    }), region->notes.end());
    auto shiftEventBeat = [&](double& beat) {
        beat -= deltaBeats;
    };
    for (auto& event : region->controllerEvents) {
        shiftEventBeat(event.beat);
    }
    for (auto& event : region->pitchBendEvents) {
        shiftEventBeat(event.beat);
    }
    for (auto& event : region->programChangeEvents) {
        shiftEventBeat(event.beat);
    }
    region->controllerEvents.erase(std::remove_if(region->controllerEvents.begin(), region->controllerEvents.end(), [](const MidiControllerEventState& event) {
        return event.beat < 0.0;
    }), region->controllerEvents.end());
    region->pitchBendEvents.erase(std::remove_if(region->pitchBendEvents.begin(), region->pitchBendEvents.end(), [](const MidiPitchBendEventState& event) {
        return event.beat < 0.0;
    }), region->pitchBendEvents.end());
    region->programChangeEvents.erase(std::remove_if(region->programChangeEvents.begin(), region->programChangeEvents.end(), [](const MidiProgramChangeEventState& event) {
        return event.beat < 0.0;
    }), region->programChangeEvents.end());
    std::sort(project.midiRegions.begin(), project.midiRegions.end(), [](const MidiRegionState& left, const MidiRegionState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });
    return true;
}

bool splitMidiRegion(ProjectDocument& project,
                     const std::string& regionId,
                     double splitSeconds,
                     std::string& newRegionId) {
    newRegionId.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || !std::isfinite(splitSeconds)) {
        return false;
    }
    const double regionEndSeconds = region->startSeconds + region->durationSeconds;
    if (splitSeconds <= region->startSeconds + 0.000001 || splitSeconds >= regionEndSeconds - 0.000001) {
        return false;
    }
    const double leftDurationSeconds = splitSeconds - region->startSeconds;
    const double rightDurationSeconds = regionEndSeconds - splitSeconds;
    const double bpm = std::max(20.0, std::min(400.0, projectTempoAtSeconds(project, region->startSeconds)));
    const double secondsPerBeat = 60.0 / bpm;
    const double splitBeat = leftDurationSeconds / secondsPerBeat;

    MidiRegionState right = *region;
    right.id = uniqueMidiRegionId(project);
    right.startSeconds = splitSeconds;
    right.durationSeconds = std::max(0.05, rightDurationSeconds);
    if (!right.name.empty()) {
        right.name += " Split";
    }
    right.notes.clear();

    std::vector<MidiNoteState> leftNotes;
    for (const auto& note : region->notes) {
        const double noteStart = std::max(0.0, note.startBeats);
        const double noteEnd = noteStart + std::max(1.0 / 960.0, note.durationBeats);
        if (noteStart < splitBeat) {
            MidiNoteState leftNote = note;
            leftNote.startBeats = noteStart;
            leftNote.durationBeats = std::max(1.0 / 960.0, std::min(noteEnd, splitBeat) - noteStart);
            leftNotes.push_back(leftNote);
        }
        if (noteEnd > splitBeat) {
            MidiNoteState rightNote = note;
            rightNote.startBeats = std::max(0.0, noteStart - splitBeat);
            rightNote.durationBeats = std::max(1.0 / 960.0, noteEnd - std::max(noteStart, splitBeat));
            right.notes.push_back(rightNote);
        }
    }
    region->durationSeconds = std::max(0.05, leftDurationSeconds);
    region->notes = std::move(leftNotes);
    project.midiRegions.push_back(right);
    std::sort(project.midiRegions.begin(), project.midiRegions.end(), [](const MidiRegionState& left, const MidiRegionState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });
    newRegionId = right.id;
    return true;
}

bool duplicateMidiRegion(ProjectDocument& project,
                         const std::string& regionId,
                         double newStartSeconds,
                         std::string& newRegionId) {
    newRegionId.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || newStartSeconds < 0.0 || !std::isfinite(newStartSeconds)) {
        return false;
    }
    MidiRegionState copy = *region;
    copy.id = uniqueMidiRegionId(project);
    copy.startSeconds = newStartSeconds;
    if (!copy.name.empty()) {
        copy.name += " Copy";
    }
    project.midiRegions.push_back(copy);
    std::sort(project.midiRegions.begin(), project.midiRegions.end(), [](const MidiRegionState& left, const MidiRegionState& right) {
        if (left.startSeconds == right.startSeconds) {
            return left.id < right.id;
        }
        return left.startSeconds < right.startSeconds;
    });
    newRegionId = copy.id;
    return true;
}

bool setMidiRegionName(ProjectDocument& project,
                       const std::string& regionId,
                       const std::string& name) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    const std::string cleanName = trimTrackName(name);
    if (region == nullptr || cleanName.empty()) {
        return false;
    }
    region->name = cleanName;
    return true;
}

bool setMidiRegionColor(ProjectDocument& project,
                        const std::string& regionId,
                        const std::string& colorHex) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || colorHex.empty()) {
        return false;
    }
    region->colorHex = colorHex;
    return true;
}

bool setMidiRegionMuted(ProjectDocument& project,
                        const std::string& regionId,
                        bool muted) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked) {
        return false;
    }
    region->muted = muted;
    return true;
}

bool setMidiRegionLocked(ProjectDocument& project,
                         const std::string& regionId,
                         bool locked) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr) {
        return false;
    }
    region->locked = locked;
    return true;
}

bool setMidiRegionLoopEnabled(ProjectDocument& project,
                              const std::string& regionId,
                              bool loopEnabled) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked) {
        return false;
    }
    region->loopEnabled = loopEnabled;
    return true;
}

bool transposeMidiRegion(ProjectDocument& project,
                         const std::string& regionId,
                         int semitones,
                         std::vector<std::string>& changedNoteIds) {
    changedNoteIds.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || semitones == 0) {
        return false;
    }
    for (auto& note : region->notes) {
        if (note.muted) {
            continue;
        }
        const int nextPitch = std::max(0, std::min(127, note.pitch + semitones));
        if (nextPitch == note.pitch) {
            continue;
        }
        note.pitch = nextPitch;
        changedNoteIds.push_back(note.id);
    }
    return !changedNoteIds.empty();
}

bool humanizeMidiRegion(ProjectDocument& project,
                        const std::string& regionId,
                        double maxTimingBeats,
                        int maxVelocityDelta,
                        unsigned int seed,
                        std::vector<std::string>& changedNoteIds) {
    changedNoteIds.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || !std::isfinite(maxTimingBeats) ||
        (maxTimingBeats <= 0.0 && maxVelocityDelta <= 0)) {
        return false;
    }
    const double timingRange = std::max(0.0, maxTimingBeats);
    const int velocityRange = std::max(0, maxVelocityDelta);
    uint32_t state = seed == 0 ? 0x9e3779b9u : seed;
    auto nextUnit = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state & 0x00ffffffu) / static_cast<double>(0x00ffffffu);
    };
    for (auto& note : region->notes) {
        if (note.muted) {
            continue;
        }
        const double oldStart = note.startBeats;
        const int oldVelocity = note.velocity;
        if (timingRange > 0.0) {
            const double timingOffset = (nextUnit() * 2.0 - 1.0) * timingRange;
            note.startBeats = std::max(0.0, note.startBeats + timingOffset);
        }
        if (velocityRange > 0) {
            const int velocityOffset = static_cast<int>(std::round((nextUnit() * 2.0 - 1.0) * velocityRange));
            note.velocity = std::max(1, std::min(127, note.velocity + velocityOffset));
        }
        if (std::abs(note.startBeats - oldStart) > 0.0000001 || note.velocity != oldVelocity) {
            changedNoteIds.push_back(note.id);
        }
    }
    std::sort(region->notes.begin(), region->notes.end(), [](const MidiNoteState& left, const MidiNoteState& right) {
        if (left.startBeats == right.startBeats) {
            return left.id < right.id;
        }
        return left.startBeats < right.startBeats;
    });
    return !changedNoteIds.empty();
}

bool deleteMidiRegion(ProjectDocument& project, const std::string& regionId) {
    if (regionId.empty()) {
        return false;
    }
    const auto oldSize = project.midiRegions.size();
    project.midiRegions.erase(
        std::remove_if(project.midiRegions.begin(), project.midiRegions.end(), [&](const MidiRegionState& region) {
            return region.id == regionId && !region.locked;
        }),
        project.midiRegions.end());
    return project.midiRegions.size() != oldSize;
}

std::string addMidiNote(ProjectDocument& project,
                        const std::string& regionId,
                        int pitch,
                        double startBeats,
                        double durationBeats,
                        int velocity,
                        int channel) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || pitch < 0 || pitch > 127 ||
        startBeats < 0.0 || durationBeats <= 0.0 ||
        !std::isfinite(startBeats) || !std::isfinite(durationBeats)) {
        return {};
    }
    MidiNoteState note;
    note.id = uniqueMidiNoteId(*region);
    note.pitch = std::max(0, std::min(127, pitch));
    note.startBeats = startBeats;
    note.durationBeats = std::max(1.0 / 960.0, durationBeats);
    note.velocity = std::max(1, std::min(127, velocity));
    note.channel = std::max(1, std::min(16, channel));
    region->notes.push_back(note);
    std::sort(region->notes.begin(), region->notes.end(), [](const MidiNoteState& left, const MidiNoteState& right) {
        if (left.startBeats == right.startBeats) {
            if (left.pitch == right.pitch) {
                return left.id < right.id;
            }
            return left.pitch < right.pitch;
        }
        return left.startBeats < right.startBeats;
    });
    return note.id;
}

std::string addMidiControllerEvent(ProjectDocument& project,
                                   const std::string& regionId,
                                   double beat,
                                   int controller,
                                   int value,
                                   int channel) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || !std::isfinite(beat) || beat < 0.0 || controller < 0 || controller > 127) {
        return {};
    }
    MidiControllerEventState event;
    event.id = uniqueMidiControllerEventId(*region);
    event.beat = beat;
    event.controller = std::max(0, std::min(127, controller));
    event.value = std::max(0, std::min(127, value));
    event.channel = std::max(1, std::min(16, channel));
    region->controllerEvents.push_back(event);
    std::sort(region->controllerEvents.begin(), region->controllerEvents.end(), [](const MidiControllerEventState& left, const MidiControllerEventState& right) {
        if (left.beat == right.beat) {
            if (left.controller == right.controller) {
                return left.id < right.id;
            }
            return left.controller < right.controller;
        }
        return left.beat < right.beat;
    });
    return event.id;
}

std::string addMidiSustainEvent(ProjectDocument& project,
                                const std::string& regionId,
                                double beat,
                                bool down,
                                int channel) {
    return addMidiControllerEvent(project, regionId, beat, 64, down ? 127 : 0, channel);
}

std::string addMidiPitchBendEvent(ProjectDocument& project,
                                  const std::string& regionId,
                                  double beat,
                                  int value,
                                  int channel) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || !std::isfinite(beat) || beat < 0.0) {
        return {};
    }
    MidiPitchBendEventState event;
    event.id = uniqueMidiPitchBendEventId(*region);
    event.beat = beat;
    event.value = std::max(0, std::min(16383, value));
    event.channel = std::max(1, std::min(16, channel));
    region->pitchBendEvents.push_back(event);
    std::sort(region->pitchBendEvents.begin(), region->pitchBendEvents.end(), [](const MidiPitchBendEventState& left, const MidiPitchBendEventState& right) {
        if (left.beat == right.beat) {
            return left.id < right.id;
        }
        return left.beat < right.beat;
    });
    return event.id;
}

std::string addMidiProgramChangeEvent(ProjectDocument& project,
                                      const std::string& regionId,
                                      double beat,
                                      int program,
                                      int channel) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || !std::isfinite(beat) || beat < 0.0) {
        return {};
    }
    MidiProgramChangeEventState event;
    event.id = uniqueMidiProgramChangeEventId(*region);
    event.beat = beat;
    event.program = std::max(0, std::min(127, program));
    event.channel = std::max(1, std::min(16, channel));
    region->programChangeEvents.push_back(event);
    std::sort(region->programChangeEvents.begin(), region->programChangeEvents.end(), [](const MidiProgramChangeEventState& left, const MidiProgramChangeEventState& right) {
        if (left.beat == right.beat) {
            return left.id < right.id;
        }
        return left.beat < right.beat;
    });
    return event.id;
}

bool deleteMidiControllerEvent(ProjectDocument& project,
                               const std::string& regionId,
                               const std::string& eventId) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || eventId.empty()) {
        return false;
    }
    const auto oldSize = region->controllerEvents.size();
    region->controllerEvents.erase(
        std::remove_if(region->controllerEvents.begin(), region->controllerEvents.end(), [&](const MidiControllerEventState& event) {
            return event.id == eventId;
        }),
        region->controllerEvents.end());
    return region->controllerEvents.size() != oldSize;
}

bool deleteMidiPitchBendEvent(ProjectDocument& project,
                              const std::string& regionId,
                              const std::string& eventId) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || eventId.empty()) {
        return false;
    }
    const auto oldSize = region->pitchBendEvents.size();
    region->pitchBendEvents.erase(
        std::remove_if(region->pitchBendEvents.begin(), region->pitchBendEvents.end(), [&](const MidiPitchBendEventState& event) {
            return event.id == eventId;
        }),
        region->pitchBendEvents.end());
    return region->pitchBendEvents.size() != oldSize;
}

bool deleteMidiProgramChangeEvent(ProjectDocument& project,
                                  const std::string& regionId,
                                  const std::string& eventId) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || eventId.empty()) {
        return false;
    }
    const auto oldSize = region->programChangeEvents.size();
    region->programChangeEvents.erase(
        std::remove_if(region->programChangeEvents.begin(), region->programChangeEvents.end(), [&](const MidiProgramChangeEventState& event) {
            return event.id == eventId;
        }),
        region->programChangeEvents.end());
    return region->programChangeEvents.size() != oldSize;
}

bool moveMidiControllerEvent(ProjectDocument& project,
                             const std::string& regionId,
                             const std::string& eventId,
                             double beat,
                             int value) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || eventId.empty() || !std::isfinite(beat) || beat < 0.0) {
        return false;
    }
    auto it = std::find_if(region->controllerEvents.begin(), region->controllerEvents.end(), [&](const MidiControllerEventState& event) {
        return event.id == eventId;
    });
    if (it == region->controllerEvents.end()) {
        return false;
    }
    it->beat = beat;
    it->value = std::max(0, std::min(127, value));
    std::sort(region->controllerEvents.begin(), region->controllerEvents.end(), [](const MidiControllerEventState& left, const MidiControllerEventState& right) {
        if (left.beat == right.beat) {
            if (left.controller == right.controller) {
                return left.id < right.id;
            }
            return left.controller < right.controller;
        }
        return left.beat < right.beat;
    });
    return true;
}

bool moveMidiPitchBendEvent(ProjectDocument& project,
                            const std::string& regionId,
                            const std::string& eventId,
                            double beat,
                            int value) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || eventId.empty() || !std::isfinite(beat) || beat < 0.0) {
        return false;
    }
    auto it = std::find_if(region->pitchBendEvents.begin(), region->pitchBendEvents.end(), [&](const MidiPitchBendEventState& event) {
        return event.id == eventId;
    });
    if (it == region->pitchBendEvents.end()) {
        return false;
    }
    it->beat = beat;
    it->value = std::max(0, std::min(16383, value));
    std::sort(region->pitchBendEvents.begin(), region->pitchBendEvents.end(), [](const MidiPitchBendEventState& left, const MidiPitchBendEventState& right) {
        if (left.beat == right.beat) {
            return left.id < right.id;
        }
        return left.beat < right.beat;
    });
    return true;
}

bool moveMidiProgramChangeEvent(ProjectDocument& project,
                                const std::string& regionId,
                                const std::string& eventId,
                                double beat,
                                int program) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || eventId.empty() || !std::isfinite(beat) || beat < 0.0) {
        return false;
    }
    auto it = std::find_if(region->programChangeEvents.begin(), region->programChangeEvents.end(), [&](const MidiProgramChangeEventState& event) {
        return event.id == eventId;
    });
    if (it == region->programChangeEvents.end()) {
        return false;
    }
    it->beat = beat;
    it->program = std::max(0, std::min(127, program));
    std::sort(region->programChangeEvents.begin(), region->programChangeEvents.end(), [](const MidiProgramChangeEventState& left, const MidiProgramChangeEventState& right) {
        if (left.beat == right.beat) {
            return left.id < right.id;
        }
        return left.beat < right.beat;
    });
    return true;
}

bool moveMidiNote(ProjectDocument& project,
                  const std::string& regionId,
                  const std::string& noteId,
                  int pitch,
                  double startBeats) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteId.empty() || pitch < 0 || pitch > 127 ||
        startBeats < 0.0 || !std::isfinite(startBeats)) {
        return false;
    }
    MidiNoteState* note = findMidiNote(*region, noteId);
    if (note == nullptr) {
        return false;
    }
    note->pitch = std::max(0, std::min(127, pitch));
    note->startBeats = startBeats;
    std::sort(region->notes.begin(), region->notes.end(), [](const MidiNoteState& left, const MidiNoteState& right) {
        if (left.startBeats == right.startBeats) {
            if (left.pitch == right.pitch) {
                return left.id < right.id;
            }
            return left.pitch < right.pitch;
        }
        return left.startBeats < right.startBeats;
    });
    return true;
}

bool moveMidiNotes(ProjectDocument& project,
                   const std::string& regionId,
                   const std::vector<std::string>& noteIds,
                   int pitchDelta,
                   double startBeatDelta,
                   std::vector<std::string>& changedNoteIds) {
    changedNoteIds.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteIds.empty() || !std::isfinite(startBeatDelta)) {
        return false;
    }
    std::set<std::string> wanted;
    for (const auto& id : noteIds) {
        if (!id.empty()) {
            wanted.insert(id);
        }
    }
    if (wanted.empty()) {
        return false;
    }
    for (auto& note : region->notes) {
        if (wanted.find(note.id) == wanted.end()) {
            continue;
        }
        const int nextPitch = std::max(0, std::min(127, note.pitch + pitchDelta));
        const double nextStart = std::max(0.0, note.startBeats + startBeatDelta);
        if (nextPitch == note.pitch && std::abs(nextStart - note.startBeats) < 0.000001) {
            continue;
        }
        note.pitch = nextPitch;
        note.startBeats = nextStart;
        changedNoteIds.push_back(note.id);
    }
    if (changedNoteIds.empty()) {
        return false;
    }
    std::sort(region->notes.begin(), region->notes.end(), [](const MidiNoteState& left, const MidiNoteState& right) {
        if (left.startBeats == right.startBeats) {
            if (left.pitch == right.pitch) {
                return left.id < right.id;
            }
            return left.pitch < right.pitch;
        }
        return left.startBeats < right.startBeats;
    });
    return true;
}

bool resizeMidiNote(ProjectDocument& project,
                    const std::string& regionId,
                    const std::string& noteId,
                    double durationBeats) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteId.empty() ||
        durationBeats <= 0.0 || !std::isfinite(durationBeats)) {
        return false;
    }
    MidiNoteState* note = findMidiNote(*region, noteId);
    if (note == nullptr) {
        return false;
    }
    note->durationBeats = std::max(1.0 / 960.0, durationBeats);
    return true;
}

bool resizeMidiNotes(ProjectDocument& project,
                     const std::string& regionId,
                     const std::vector<std::string>& noteIds,
                     double durationBeatDelta,
                     std::vector<std::string>& changedNoteIds) {
    changedNoteIds.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteIds.empty() || !std::isfinite(durationBeatDelta)) {
        return false;
    }
    std::set<std::string> wanted;
    for (const auto& id : noteIds) {
        if (!id.empty()) {
            wanted.insert(id);
        }
    }
    if (wanted.empty()) {
        return false;
    }
    for (auto& note : region->notes) {
        if (wanted.find(note.id) == wanted.end()) {
            continue;
        }
        const double nextDuration = std::max(1.0 / 960.0, note.durationBeats + durationBeatDelta);
        if (std::abs(nextDuration - note.durationBeats) < 0.000001) {
            continue;
        }
        note.durationBeats = nextDuration;
        changedNoteIds.push_back(note.id);
    }
    return !changedNoteIds.empty();
}

bool setMidiNoteVelocity(ProjectDocument& project,
                         const std::string& regionId,
                         const std::string& noteId,
                         int velocity) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteId.empty()) {
        return false;
    }
    MidiNoteState* note = findMidiNote(*region, noteId);
    if (note == nullptr) {
        return false;
    }
    note->velocity = std::max(1, std::min(127, velocity));
    return true;
}

bool adjustMidiNoteVelocities(ProjectDocument& project,
                              const std::string& regionId,
                              const std::vector<std::string>& noteIds,
                              int velocityDelta,
                              std::vector<std::string>& changedNoteIds) {
    changedNoteIds.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteIds.empty()) {
        return false;
    }
    std::set<std::string> wanted;
    for (const auto& id : noteIds) {
        if (!id.empty()) {
            wanted.insert(id);
        }
    }
    if (wanted.empty()) {
        return false;
    }
    for (auto& note : region->notes) {
        if (wanted.find(note.id) == wanted.end()) {
            continue;
        }
        const int nextVelocity = std::max(1, std::min(127, note.velocity + velocityDelta));
        if (nextVelocity == note.velocity) {
            continue;
        }
        note.velocity = nextVelocity;
        changedNoteIds.push_back(note.id);
    }
    return !changedNoteIds.empty();
}

bool setMidiNoteMuted(ProjectDocument& project,
                      const std::string& regionId,
                      const std::string& noteId,
                      bool muted) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteId.empty()) {
        return false;
    }
    MidiNoteState* note = findMidiNote(*region, noteId);
    if (note == nullptr) {
        return false;
    }
    note->muted = muted;
    return true;
}

bool deleteMidiNote(ProjectDocument& project,
                    const std::string& regionId,
                    const std::string& noteId) {
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteId.empty()) {
        return false;
    }
    const auto oldSize = region->notes.size();
    region->notes.erase(
        std::remove_if(region->notes.begin(), region->notes.end(), [&](const MidiNoteState& note) {
            return note.id == noteId;
        }),
        region->notes.end());
    return region->notes.size() != oldSize;
}

bool deleteMidiNotes(ProjectDocument& project,
                     const std::string& regionId,
                     const std::vector<std::string>& noteIds,
                     std::vector<std::string>& deletedNoteIds) {
    deletedNoteIds.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || noteIds.empty()) {
        return false;
    }
    std::set<std::string> wanted;
    for (const auto& id : noteIds) {
        if (!id.empty()) {
            wanted.insert(id);
        }
    }
    if (wanted.empty()) {
        return false;
    }
    region->notes.erase(
        std::remove_if(region->notes.begin(), region->notes.end(), [&](const MidiNoteState& note) {
            if (wanted.find(note.id) == wanted.end()) {
                return false;
            }
            deletedNoteIds.push_back(note.id);
            return true;
        }),
        region->notes.end());
    return !deletedNoteIds.empty();
}

bool quantizeMidiRegion(ProjectDocument& project,
                        const std::string& regionId,
                        double beatQuantum,
                        std::vector<std::string>& changedNoteIds) {
    changedNoteIds.clear();
    MidiRegionState* region = findMidiRegion(project, regionId);
    if (region == nullptr || region->locked || beatQuantum <= 0.0 || !std::isfinite(beatQuantum)) {
        return false;
    }
    for (auto& note : region->notes) {
        const double snapped = std::max(0.0, std::round(note.startBeats / beatQuantum) * beatQuantum);
        if (std::abs(snapped - note.startBeats) > 0.000001) {
            note.startBeats = snapped;
            changedNoteIds.push_back(note.id);
        }
    }
    if (!changedNoteIds.empty()) {
        std::sort(region->notes.begin(), region->notes.end(), [](const MidiNoteState& left, const MidiNoteState& right) {
            if (left.startBeats == right.startBeats) {
                if (left.pitch == right.pitch) {
                    return left.id < right.id;
                }
                return left.pitch < right.pitch;
            }
            return left.startBeats < right.startBeats;
        });
    }
    return !changedNoteIds.empty();
}

bool addTempoMarkerAt(ProjectDocument& project, double timeSeconds, double bpm) {
    if (timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        bpm < 20.0 || bpm > 400.0 || !std::isfinite(bpm)) {
        return false;
    }
    constexpr double mergeToleranceSeconds = 0.005;
    for (auto& marker : project.tempoMap) {
        if (std::abs(marker.timeSeconds - timeSeconds) <= mergeToleranceSeconds) {
            marker.timeSeconds = timeSeconds;
            marker.bpm = bpm;
            std::sort(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
                return left.timeSeconds < right.timeSeconds;
            });
            project.tempoBpm = static_cast<int>(std::round(project.tempoMap.front().bpm));
            return true;
        }
    }
    project.tempoMap.push_back({timeSeconds, bpm});
    std::sort(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    project.tempoBpm = static_cast<int>(std::round(project.tempoMap.front().bpm));
    return true;
}

bool setNearestTempoMarkerBpm(ProjectDocument& project, double timeSeconds, double toleranceSeconds, double bpm) {
    if (project.tempoMap.empty() || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds) ||
        bpm < 20.0 || bpm > 400.0 || !std::isfinite(bpm)) {
        return false;
    }
    auto nearest = project.tempoMap.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.tempoMap.begin(); it != project.tempoMap.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.tempoMap.end()) {
        return false;
    }
    nearest->bpm = bpm;
    std::sort(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    project.tempoBpm = static_cast<int>(std::round(project.tempoMap.front().bpm));
    return true;
}

bool moveNearestTempoMarker(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds, double bpm) {
    if (project.tempoMap.empty() || originalTimeSeconds < 0.0 || newTimeSeconds < 0.0 ||
        !std::isfinite(originalTimeSeconds) || !std::isfinite(newTimeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds) ||
        bpm < 20.0 || bpm > 400.0 || !std::isfinite(bpm)) {
        return false;
    }
    auto nearest = project.tempoMap.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.tempoMap.begin(); it != project.tempoMap.end(); ++it) {
        const double distance = std::abs(it->timeSeconds - originalTimeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.tempoMap.end()) {
        return false;
    }
    nearest->timeSeconds = newTimeSeconds;
    nearest->bpm = bpm;
    std::sort(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    project.tempoBpm = static_cast<int>(std::round(project.tempoMap.front().bpm));
    return true;
}

bool deleteNearestTempoMarker(ProjectDocument& project, double timeSeconds, double toleranceSeconds) {
    if (project.tempoMap.size() <= 1 || timeSeconds < 0.0 || !std::isfinite(timeSeconds) ||
        toleranceSeconds < 0.0 || !std::isfinite(toleranceSeconds)) {
        return false;
    }
    auto nearest = project.tempoMap.end();
    double nearestDistance = toleranceSeconds;
    for (auto it = project.tempoMap.begin(); it != project.tempoMap.end(); ++it) {
        if (it->timeSeconds <= 1e-6) {
            continue;   // the initial tempo anchor at t=0 defines the base and is required
        }
        const double distance = std::abs(it->timeSeconds - timeSeconds);
        if (distance <= nearestDistance) {
            nearest = it;
            nearestDistance = distance;
        }
    }
    if (nearest == project.tempoMap.end()) {
        return false;
    }
    project.tempoMap.erase(nearest);
    std::sort(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    project.tempoBpm = static_cast<int>(std::round(project.tempoMap.front().bpm));
    return true;
}

bool addMasterVst3Insert(ProjectDocument& project, const InsertState& insert) {
    if (!isExternalPluginInsertFormat(insert.pluginFormat) || trimTrackName(insert.pluginPath).empty()) {
        return false;
    }
    const auto duplicate = std::find_if(project.masterInserts.begin(), project.masterInserts.end(), [&](const InsertState& existing) {
        if (!isExternalPluginInsertFormat(existing.pluginFormat) || trimTrackName(existing.pluginPath).empty()) {
            return false;
        }
        if (existing.pluginFormat != insert.pluginFormat || existing.pluginPath != insert.pluginPath) {
            return false;
        }
        if (insert.pluginFormat == "Audio Unit") {
            const std::string insertClass = !insert.pluginClassId.empty() ? insert.pluginClassId : insert.pluginClassName;
            const std::string existingClass = !existing.pluginClassId.empty() ? existing.pluginClassId : existing.pluginClassName;
            if (!insertClass.empty() || !existingClass.empty()) {
                return insertClass == existingClass;
            }
        }
        if (!insert.pluginClassId.empty() && !existing.pluginClassId.empty()) {
            return existing.pluginClassId == insert.pluginClassId;
        }
        if (!insert.pluginClassName.empty() && !existing.pluginClassName.empty()) {
            return existing.pluginClassName == insert.pluginClassName;
        }
        if (!insert.pluginClassId.empty() || !existing.pluginClassId.empty() ||
            !insert.pluginClassName.empty() || !existing.pluginClassName.empty()) {
            return false;
        }
        return true;
    });
    if (duplicate != project.masterInserts.end()) {
        return false;
    }
    InsertState normalized = insert;
    if (trimTrackName(normalized.pluginName).empty()) {
        normalized.pluginName = normalized.pluginPath;
    }
    if (trimTrackName(normalized.pluginAppId).empty()) {
        normalized.pluginAppId = normalized.pluginFormat == "Audio Unit" ? "external-audio-unit" : "external-vst3";
    }
    for (auto& existing : project.masterInserts) {
        if (!isExternalPluginInsertFormat(existing.pluginFormat) || trimTrackName(existing.pluginPath).empty()) {
            existing = normalized;
            return true;
        }
    }
    if (project.masterInserts.size() >= kMaxTrackInsertSlots) {
        return false;
    }
    project.masterInserts.push_back(normalized);
    return true;
}

bool addMasterInsertSlot(ProjectDocument& project) {
    if (project.masterInserts.size() >= kMaxTrackInsertSlots) {
        return false;
    }
    InsertState insert;
    insert.pluginName = "No Insert";
    insert.pluginFormat = "None";
    insert.pluginAppId = "empty-master-insert";
    insert.bypassed = false;
    insert.available = false;
    project.masterInserts.push_back(insert);
    return true;
}

bool isEmptyMasterInsertSlot(const InsertState& insert) {
    return trimTrackName(insert.pluginPath).empty() &&
        insert.pluginAppId == "empty-master-insert" &&
        insert.pluginFormat == "None" &&
        (insert.pluginName.empty() || insert.pluginName == "No Insert");
}

bool toggleMasterVst3InsertBypass(ProjectDocument& project, size_t insertIndex) {
    if (insertIndex >= project.masterInserts.size() || !isExternalPluginInsertFormat(project.masterInserts[insertIndex].pluginFormat)) {
        return false;
    }
    project.masterInserts[insertIndex].bypassed = !project.masterInserts[insertIndex].bypassed;
    return true;
}

bool removeMasterVst3Insert(ProjectDocument& project, size_t insertIndex) {
    if (insertIndex >= project.masterInserts.size()) {
        return false;
    }
    const auto& insert = project.masterInserts[insertIndex];
    const bool emptySlot = isEmptyMasterInsertSlot(insert);
    if (!emptySlot && !isExternalPluginInsertFormat(insert.pluginFormat)) {
        return false;
    }
    project.masterInserts.erase(project.masterInserts.begin() + static_cast<std::ptrdiff_t>(insertIndex));
    return true;
}

size_t clearMasterVst3Inserts(ProjectDocument& project) {
    const auto originalCount = project.masterInserts.size();
    project.masterInserts.erase(
        std::remove_if(project.masterInserts.begin(), project.masterInserts.end(), [](const InsertState& insert) {
            return isExternalPluginInsertFormat(insert.pluginFormat);
        }),
        project.masterInserts.end());
    return originalCount - project.masterInserts.size();
}

int moveMasterInsert(ProjectDocument& project, size_t insertIndex, int direction) {
    if (insertIndex >= project.masterInserts.size() || direction == 0 ||
        !isExternalPluginInsertFormat(project.masterInserts[insertIndex].pluginFormat)) {
        return -1;
    }

    if (direction < 0) {
        for (size_t index = insertIndex; index > 0; --index) {
            const size_t candidate = index - 1;
            if (isExternalPluginInsertFormat(project.masterInserts[candidate].pluginFormat)) {
                std::swap(project.masterInserts[insertIndex], project.masterInserts[candidate]);
                return static_cast<int>(candidate);
            }
        }
        return -1;
    }

    for (size_t candidate = insertIndex + 1; candidate < project.masterInserts.size(); ++candidate) {
        if (isExternalPluginInsertFormat(project.masterInserts[candidate].pluginFormat)) {
            std::swap(project.masterInserts[insertIndex], project.masterInserts[candidate]);
            return static_cast<int>(candidate);
        }
    }
    return -1;
}

} // namespace neuracoust::daw
