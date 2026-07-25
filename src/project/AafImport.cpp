#include "project/AafImport.h"

#include "project/EditOperations.h"

#include <algorithm>
#include <cmath>

#if defined(NEURACOUST_HAS_LIBAAF)
extern "C" {
#include "aaf/libaaf.h"
}
#endif

namespace neuracoust::daw {

#if !defined(NEURACOUST_HAS_LIBAAF)

bool aafImportAvailable() { return false; }

AafImportResult importAafSession(const std::filesystem::path&, ProjectDocument&) {
    AafImportResult result;
    result.message = "이 빌드에는 AAF 읽기 라이브러리가 없습니다.";
    return result;
}

#else

namespace {

/// AAF stores positions in "edit units" whose rate the track carries as a rational. Everything in
/// our project is seconds, so every position and length crosses through here.
double editUnitsToSeconds(aafPosition_t units, const aafRational_t* editRate) {
    if (editRate == nullptr || editRate->numerator == 0) {
        return 0.0;
    }
    const double rate = static_cast<double>(editRate->numerator) /
                        static_cast<double>(editRate->denominator);
    if (!(rate > 0.0)) {
        return 0.0;
    }
    return static_cast<double>(units) / rate;
}

std::string textOrEmpty(const char* text) { return text != nullptr ? std::string(text) : std::string(); }

/// libAAF hands back a URI-ish path for external media. Take whatever it resolved to on disk first;
/// fall back to the original path so a missing file is still reported with something useful.
std::string essencePath(const aafiAudioEssenceFile* essence) {
    if (essence == nullptr) {
        return {};
    }
    if (essence->usable_file_path != nullptr && *essence->usable_file_path != '\0') {
        return essence->usable_file_path;
    }
    return textOrEmpty(essence->original_file_path);
}

} // namespace

bool aafImportAvailable() { return true; }

AafImportResult importAafSession(const std::filesystem::path& path, ProjectDocument& project) {
    AafImportResult result;

    AAF_Iface* aafi = aafi_alloc(nullptr);
    if (aafi == nullptr) {
        result.message = "AAF 리더를 초기화할 수 없습니다.";
        return result;
    }
    if (aafi_load_file(aafi, path.string().c_str()) != 0) {
        aafi_release(&aafi);
        result.message = "AAF 파일을 열 수 없습니다: " + path.filename().string();
        return result;
    }

    // Start from a clean default project so an import never half-merges into the open session.
    ProjectDocument imported = defaultProject();
    imported.name = path.stem().string();
    imported.tracks.clear();
    imported.clips.clear();
    imported.trackPlaylists.clear();
    imported.markers.clear();

    // Keep Master/Monitor: the rest of the engine assumes they exist.
    for (const auto& track : defaultProject().tracks) {
        if (track.trackType == "master" || track.trackType == "monitor") {
            imported.tracks.push_back(track);
        }
    }

    aafiAudioTrack* audioTrack = nullptr;
    int trackIndex = 0;
    AAFI_foreachAudioTrack(aafi, audioTrack) {
        ++trackIndex;
        TrackState track;
        track.name = textOrEmpty(audioTrack->name);
        if (track.name.empty()) {
            track.name = "Audio " + std::to_string(trackIndex);
        }
        track.trackType = "audio";
        track.channelFormat = audioTrack->format == 2 ? "stereo" : "mono";
        track.inputBus = "Input 1";
        track.outputBus = "Master";
        // Insert before Master/Monitor so those stay last, which normalizeProjectRouting expects.
        imported.tracks.insert(imported.tracks.end() - 2, track);
        ++result.trackCount;

        aafiTimelineItem* item = nullptr;
        AAFI_foreachTrackItem(audioTrack, item) {
            if (item->type != AAFI_AUDIO_CLIP || item->data == nullptr) {
                continue;   // transitions/crossfades are derived from clip overlap on our side
            }
            auto* audioClip = static_cast<aafiAudioClip*>(item->data);
            const aafRational_t* editRate = audioTrack->edit_rate;

            ClipState clip;
            clip.id = "aafclip-" + std::to_string(imported.clips.size() + 1);
            clip.trackName = track.name;
            clip.startSeconds = std::max(0.0, editUnitsToSeconds(audioClip->pos, editRate));
            clip.durationSeconds = std::max(0.01, editUnitsToSeconds(audioClip->len, editRate));
            clip.sourceOffsetSeconds = std::max(0.0, editUnitsToSeconds(audioClip->essence_offset, editRate));
            clip.muted = audioClip->mute != 0;

            // The clip's media. An AAF may reference several essence files for a multi-channel clip;
            // the first is what our single-source clip model can carry.
            const aafiAudioEssencePointer* pointer = audioClip->essencePointerList;
            const aafiAudioEssenceFile* essence = pointer != nullptr ? pointer->essenceFile : nullptr;
            clip.sourcePath = essencePath(essence);
            clip.regionName = essence != nullptr ? textOrEmpty(essence->name) : std::string();
            if (clip.regionName.empty()) {
                clip.regionName = "Clip " + std::to_string(imported.clips.size() + 1);
            }
            if (clip.sourcePath.empty() || !std::filesystem::exists(clip.sourcePath)) {
                ++result.missingMediaCount;
            }

            // Clip gain, when the session carried a single fixed value (a curve is automation and
            // is deliberately not guessed at).
            if (audioClip->gain != nullptr && audioClip->gain->value != nullptr &&
                audioClip->gain->pts_cnt == 1 && audioClip->gain->value[0].denominator != 0) {
                const double linear = static_cast<double>(audioClip->gain->value[0].numerator) /
                                      static_cast<double>(audioClip->gain->value[0].denominator);
                if (linear > 0.0) {
                    clip.gainDb = static_cast<float>(20.0 * std::log10(linear));
                }
            }

            imported.clips.push_back(clip);
            ++result.clipCount;
        }
    }

    aafiMarker* marker = nullptr;
    AAFI_foreachMarker(aafi, marker) {
        MarkerState imported_marker;
        imported_marker.id = "aafmarker-" + std::to_string(imported.markers.size() + 1);
        imported_marker.name = textOrEmpty(marker->name);
        if (imported_marker.name.empty()) {
            imported_marker.name = std::to_string(imported.markers.size() + 1);
        }
        imported_marker.timeSeconds = std::max(0.0, editUnitsToSeconds(marker->start, marker->edit_rate));
        imported_marker.comment = textOrEmpty(marker->comment);
        imported.markers.push_back(imported_marker);
        ++result.markerCount;
    }

    aafi_release(&aafi);

    if (result.trackCount == 0) {
        result.message = "AAF에 오디오 트랙이 없습니다.";
        return result;
    }

    // The playlist model is the render source of truth and almost nothing maintains it, so rebuild
    // it from the clips we just placed before handing the project over.
    normalizeProjectRouting(imported);
    rebuildProjectEditModelFromClips(imported);

    project = std::move(imported);
    result.ok = true;
    result.message = "AAF 가져오기: 트랙 " + std::to_string(result.trackCount) +
                     " · 클립 " + std::to_string(result.clipCount) +
                     " · 마커 " + std::to_string(result.markerCount);
    if (result.missingMediaCount > 0) {
        result.message += " · 미디어 없음 " + std::to_string(result.missingMediaCount);
    }
    return result;
}

#endif

} // namespace neuracoust::daw
