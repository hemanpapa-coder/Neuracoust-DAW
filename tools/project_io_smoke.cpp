// Exercises project open/save and audio import through the C facade, on real
// files. No audio device is opened: this is document plumbing, not playback.

#include "bridge/NeuracoustEngineBridge.h"
#include "audio/OfflineBounce.h"
#include "audio/WavFile.h"
#include "project/ProjectDocument.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

static int failures = 0;

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/// Builds its own fixtures so the test needs nothing from the tree: a 2 s tone,
/// and an m4a made from it so the non-WAV conversion path is exercised.
static bool makeFixtures(std::string& wavPath, std::string& m4aPath) {
    const std::filesystem::path dir = "/tmp/neuracoust-io-smoke-media";
    std::error_code fsError;
    std::filesystem::remove_all(dir, fsError);
    std::filesystem::create_directories(dir, fsError);
    if (fsError) {
        return false;
    }

    wavPath = (dir / "tone.wav").string();
    m4aPath = (dir / "tone.m4a").string();

    if (!neuracoust::daw::writeTestToneWavFile(wavPath, 48000, 2.0, 440.0)) {
        return false;
    }

    const std::string command =
        "/usr/bin/afconvert '" + wavPath + "' '" + m4aPath + "' -f m4af -d aac >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

/// Seconds at which the rendered file first carries signal, or -1.
static double firstAudibleSecond(const std::string& path) {
    neuracoust::daw::WavAudioData audio;
    std::string error;
    if (!neuracoust::daw::readPcmWavFile(path, audio, error) || audio.channels <= 0) {
        return -1;
    }
    for (int64_t frame = 0; frame < audio.frameCount(); ++frame) {
        for (int channel = 0; channel < audio.channels; ++channel) {
            if (std::abs(audio.interleavedSamples[frame * audio.channels + channel]) > 0.01f) {
                return static_cast<double>(frame) / audio.sampleRate;
            }
        }
    }
    return -1;
}

/// True when nothing louder than the noise floor plays between two times.
static bool isSilentBetween(const std::string& path, double fromSeconds, double toSeconds) {
    neuracoust::daw::WavAudioData audio;
    std::string error;
    if (!neuracoust::daw::readPcmWavFile(path, audio, error) || audio.channels <= 0) {
        return false;
    }
    // Stay off the edges: a clip boundary carries a click, and a fade needs room.
    const int64_t first = static_cast<int64_t>((fromSeconds + 0.05) * audio.sampleRate);
    const int64_t last = std::min(audio.frameCount(),
                                  static_cast<int64_t>((toSeconds - 0.05) * audio.sampleRate));
    for (int64_t frame = std::max<int64_t>(0, first); frame < last; ++frame) {
        for (int channel = 0; channel < audio.channels; ++channel) {
            if (std::abs(audio.interleavedSamples[frame * audio.channels + channel]) > 0.01f) {
                return false;
            }
        }
    }
    return last > first;
}

/// Loudest sample between two times, so a gain ramp can be measured rather than seen.
static float peakBetween(const std::string& path, double fromSeconds, double toSeconds) {
    neuracoust::daw::WavAudioData audio;
    std::string error;
    if (!neuracoust::daw::readPcmWavFile(path, audio, error) || audio.channels <= 0) {
        return -1.0f;
    }
    const int64_t first = std::max<int64_t>(0, static_cast<int64_t>(fromSeconds * audio.sampleRate));
    const int64_t last = std::min(audio.frameCount(), static_cast<int64_t>(toSeconds * audio.sampleRate));
    float peak = 0.0f;
    for (int64_t frame = first; frame < last; ++frame) {
        for (int channel = 0; channel < audio.channels; ++channel) {
            peak = std::max(peak, std::abs(audio.interleavedSamples[frame * audio.channels + channel]));
        }
    }
    return peak;
}

/// Name of the step a Cmd-Z would undo, or "" when the history is empty.
static std::string topUndoStep(NCEngine* engine) {
    if (!nc_history_can_undo(engine)) {
        return {};
    }
    char name[256] = {0};
    nc_history_undo_step_name(engine, name, sizeof(name));
    return name;
}

/// Where a clip sits now, by id. Batch edits renumber the index order.
static double startOfClip(NCEngine* engine, const char* clipId) {
    for (int index = 0; index < nc_clip_count(engine); ++index) {
        char id[128] = {0};
        nc_clip_id(engine, index, id, sizeof(id));
        if (strcmp(id, clipId) == 0) {
            return nc_clip_start_seconds(engine, index);
        }
    }
    return -1.0;
}

/// Undo until the history is empty, counting; then redo back to where we were.
///
/// Destructive to a gesture in progress: undo/redo restore snapshots, so any
/// continuous edit that has not been committed with nc_history_record_gesture is
/// wiped. Only call this when nothing is mid-drag.
static int countUndoSteps(NCEngine* engine) {
    int steps = 0;
    while (nc_history_can_undo(engine) && nc_history_undo(engine)) {
        ++steps;
    }
    for (int index = 0; index < steps; ++index) {
        nc_history_redo(engine);
    }
    return steps;
}

int main() {
    std::string wavFixture;
    std::string m4aFixture;
    if (!makeFixtures(wavFixture, m4aFixture)) {
        fprintf(stderr, "SKIP: could not build audio fixtures (afconvert missing?)\n");
        return 0;
    }
    const char* wavPath = wavFixture.c_str();
    const char* m4aPath = m4aFixture.c_str();

    NCEngine* engine = nc_engine_create();
    check(engine != NULL, "engine created");
    if (engine == NULL) {
        return 1;
    }

    // --- supported formats ---------------------------------------------------
    check(nc_audio_import_supported(wavPath), "wav is supported");
    check(nc_audio_import_supported(m4aPath), "m4a is supported");
    check(!nc_audio_import_supported("/tmp/nope.txt"), "txt is not supported");

    // --- import a WAV into an unsaved project --------------------------------
    // With no project path, media lands in a temporary folder.
    char error[256] = {0};
    check(nc_clip_count(engine) == 0, "a new project has no clips");
    check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)),
          "wav imported into the unsaved project");
    if (strlen(error) > 0) {
        fprintf(stderr, "  import error: %s\n", error);
    }
    check(nc_clip_count(engine) == 1, "one clip after importing a wav");

    char clipTrack[128] = {0};
    char clipSource[512] = {0};
    nc_clip_track(engine, 0, clipTrack, sizeof(clipTrack));
    nc_clip_source_path(engine, 0, clipSource, sizeof(clipSource));
    const double duration = nc_clip_duration_seconds(engine, 0);
    printf("clip 0: track='%s' start=%.2f duration=%.3f\n  source=%s\n",
           clipTrack, nc_clip_start_seconds(engine, 0), duration, clipSource);

    check(strcmp(clipTrack, "Audio 1") == 0, "the clip landed on track 0");
    check(duration > 1.9 && duration < 2.1, "the 2 s tone reports a 2 s duration");
    check(strstr(clipSource, "Unsaved Imports") != NULL,
          "an unsaved project keeps imported media in a temporary folder");

    // Importing records an undo step.
    check(nc_history_can_undo(engine), "import recorded a step");
    char stepName[128] = {0};
    nc_history_undo_step_name(engine, stepName, sizeof(stepName));
    printf("undo step: '%s'\n", stepName);
    check(strstr(stepName, "Import") != NULL, "the step is named after the import");

    check(nc_history_undo(engine), "undo the import");
    check(nc_clip_count(engine) == 0, "undo removed the clip");
    check(nc_history_redo(engine), "redo the import");
    check(nc_clip_count(engine) == 1, "redo restored the clip");

    // --- save, then import a non-WAV so it converts into Audio Files ---------
    const char* projectPath = "/tmp/neuracoust-io-smoke/Session.ndaw";
    std::system("rm -rf /tmp/neuracoust-io-smoke && mkdir -p /tmp/neuracoust-io-smoke");

    check(!nc_project_save(engine, error, sizeof(error)), "save without a path fails");
    check(nc_project_save_as(engine, projectPath, error, sizeof(error)), "save as succeeds");
    if (strlen(error) > 0) {
        fprintf(stderr, "  save error: %s\n", error);
    }
    check(!nc_project_dirty(engine), "saving clears dirty");

    check(nc_audio_import(engine, 1, m4aPath, 1.5, error, sizeof(error)),
          "m4a imported and converted");
    if (strlen(error) > 0) {
        fprintf(stderr, "  import error: %s\n", error);
    }
    check(nc_clip_count(engine) == 2, "two clips now");

    char convertedSource[512] = {0};
    for (int index = 0; index < nc_clip_count(engine); ++index) {
        char track[128] = {0};
        nc_clip_track(engine, index, track, sizeof(track));
        if (strcmp(track, "Audio 2") == 0) {
            nc_clip_source_path(engine, index, convertedSource, sizeof(convertedSource));
            printf("converted clip on %s at %.2f s\n  source=%s\n",
                   track, nc_clip_start_seconds(engine, index), convertedSource);
        }
    }
    check(strstr(convertedSource, "/Audio Files/") != NULL,
          "a saved project keeps converted media beside itself");
    check(strstr(convertedSource, ".wav") != NULL, "the m4a became a wav");
    check(nc_project_dirty(engine), "importing after a save makes the document dirty");

    // --- reopen from disk ----------------------------------------------------
    check(nc_project_save(engine, error, sizeof(error)), "save to the known path");

    nc_project_new(engine);
    check(nc_clip_count(engine) == 0, "new project is empty");
    check(!nc_history_can_undo(engine), "new project has no history");

    check(nc_project_open(engine, projectPath, false, error, sizeof(error)), "reopen the project");
    if (strlen(error) > 0) {
        fprintf(stderr, "  open error: %s\n", error);
    }
    check(nc_clip_count(engine) == 2, "both clips came back");
    check(!nc_project_dirty(engine), "a freshly opened project is clean");
    check(!nc_history_can_undo(engine), "opening resets history");

    char reopenedPath[512] = {0};
    nc_project_path(engine, reopenedPath, sizeof(reopenedPath));
    check(strcmp(reopenedPath, projectPath) == 0, "the project remembers its path");

    // --- autosave recovery ---------------------------------------------------
    // A dirty document autosaves; that autosave is newer than the saved project.
    nc_track_set_muted(engine, 0, true);
    check(nc_project_dirty(engine), "muting made it dirty");
    check(nc_project_autosave_is_newer(projectPath), "the autosave is newer than the project");

    nc_project_new(engine);
    check(nc_project_open(engine, projectPath, true, error, sizeof(error)),
          "reopen preferring the autosave");
    check(nc_track_muted(engine, 0), "the recovered document has the unsaved mute");
    check(!nc_project_autosave_is_newer(projectPath), "the autosave was cleared after recovery");

    // --- clip editing ---------------------------------------------------------
    {
        nc_project_new(engine);
        check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "import for editing");
        check(nc_clip_count(engine) == 1, "one clip to edit");

        char clipId[128] = {0};
        nc_clip_id(engine, 0, clipId, sizeof(clipId));
        const double originalDuration = nc_clip_duration_seconds(engine, 0);

        // Move is continuous: no step until the caller records the gesture.
        nc_history_reset(engine);
        for (int frame = 0; frame < 20; ++frame) {
            nc_clip_move(engine, clipId, 0.05 * frame);
        }
        check(nc_history_undo_depth(engine) == 0, "dragging a clip records nothing by itself");
        check(nc_clip_start_seconds(engine, 0) > 0.9, "the clip actually moved");
        check(nc_history_record_gesture(engine, "Move clip"), "the gesture records one step");
        check(nc_history_undo_depth(engine) == 1, "exactly one step for the drag");

        // Trimming the start shortens the clip and leaves the end where it was.
        const double startBefore = nc_clip_start_seconds(engine, 0);
        const double endBefore = startBefore + nc_clip_duration_seconds(engine, 0);
        check(nc_clip_trim_start(engine, clipId, startBefore + 0.5), "trim start");
        const double endAfter = nc_clip_start_seconds(engine, 0) + nc_clip_duration_seconds(engine, 0);
        check(std::abs(endAfter - endBefore) < 0.01, "trimming the start keeps the end in place");
        check(nc_clip_duration_seconds(engine, 0) < originalDuration, "and shortens the clip");

        // Split is discrete: it records itself and yields two clips.
        const int depthBeforeSplit = nc_history_undo_depth(engine);
        const double splitAt = nc_clip_start_seconds(engine, 0) + 0.4;
        check(nc_clip_split(engine, clipId, splitAt), "split the clip");
        check(nc_clip_count(engine) == 2, "split produced two clips");
        check(nc_history_undo_depth(engine) == depthBeforeSplit + 1, "split recorded one step");

        check(nc_history_undo(engine), "undo the split");
        check(nc_clip_count(engine) == 1, "undo rejoined the clip");

        // Delete records itself too.
        nc_clip_id(engine, 0, clipId, sizeof(clipId));
        check(nc_clip_delete(engine, clipId), "delete the clip");
        check(nc_clip_count(engine) == 0, "the clip is gone");
        check(nc_history_undo(engine), "undo the delete");
        check(nc_clip_count(engine) == 1, "the clip came back");

        // A clip edit that only touches project.clips is invisible to the renderer:
        // it rebuilds clips from trackPlaylists. Moving a clip must move the sound,
        // not just the picture. Bounce and measure where the audio actually starts.
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "import for bounce");
            char bounceClip[128] = {0};
            nc_clip_id(engine, 0, bounceClip, sizeof(bounceClip));

            check(nc_clip_move(engine, bounceClip, 3.0), "move the clip to 3 s");
            check(std::abs(nc_clip_start_seconds(engine, 0) - 3.0) < 0.01, "the model agrees");

            // Bounce the document the bridge is holding, through the same plan the
            // realtime engine builds.
            neuracoust::daw::ProjectDocument rendered;
            std::string parseError;
            char projectFile[256] = "/tmp/neuracoust-io-smoke/Moved.ndaw";
            check(nc_project_save_as(engine, projectFile, error, sizeof(error)), "save for bounce");

            FILE* file = fopen(projectFile, "rb");
            check(file != nullptr, "read back the saved project");
            std::string text;
            if (file != nullptr) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
                fclose(file);
            }
            check(neuracoust::daw::deserializeProjectForPath(text, projectFile, rendered, parseError),
                  "parse it back");

            const std::string bouncePath = "/tmp/neuracoust-io-smoke/moved.wav";
            const auto bounce = neuracoust::daw::bounceProjectToWav(rendered, bouncePath);
            check(bounce.ok, "bounce succeeded");

            const double audibleAt = firstAudibleSecond(bouncePath);
            printf("moved clip: audio starts at %.3f s (expect 3.000)\n", audibleAt);
            check(std::abs(audibleAt - 3.0) < 0.05,
                  "moving a clip moves the sound, not just the picture");
        }

        // --- clipboard: a pasted clip must be audible, not just visible ---------
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "import for clipboard");
            char sourceClip[128] = {0};
            nc_clip_id(engine, 0, sourceClip, sizeof(sourceClip));

            check(!nc_clipboard_has_clip(engine), "the clipboard starts empty");
            check(nc_clip_copy(engine, sourceClip), "copy the clip");
            check(nc_clipboard_has_clip(engine), "the clipboard holds a clip");

            char pasted[128] = {0};
            check(nc_clip_paste(engine, 4.0, pasted, sizeof(pasted)), "paste at 4 s");
            check(nc_clip_count(engine) == 2, "two clips after pasting");
            check(strlen(pasted) > 0 && strcmp(pasted, sourceClip) != 0, "the paste has its own id");

            // Delete the original so only the pasted clip can make a sound.
            check(nc_clip_delete(engine, sourceClip), "delete the original");
            check(nc_clip_count(engine) == 1, "only the pasted clip remains");

            char clipboardProject[256] = "/tmp/neuracoust-io-smoke/Pasted.ndaw";
            check(nc_project_save_as(engine, clipboardProject, error, sizeof(error)), "save the paste");

            neuracoust::daw::ProjectDocument pastedProject;
            std::string pasteParseError;
            FILE* pasteFile = fopen(clipboardProject, "rb");
            std::string pasteText;
            if (pasteFile != nullptr) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), pasteFile)) > 0) pasteText.append(buffer, read);
                fclose(pasteFile);
            }
            check(neuracoust::daw::deserializeProjectForPath(pasteText, clipboardProject,
                                                             pastedProject, pasteParseError),
                  "parse the pasted project");

            const std::string pasteBounce = "/tmp/neuracoust-io-smoke/pasted.wav";
            const auto bounce = neuracoust::daw::bounceProjectToWav(pastedProject, pasteBounce);
            check(bounce.ok, "bounce the paste");
            const double audibleAt = firstAudibleSecond(pasteBounce);
            printf("pasted clip: audio starts at %.3f s (expect 4.000)\n", audibleAt);
            check(std::abs(audibleAt - 4.0) < 0.05, "a pasted clip is audible where it was pasted");

            // Cut removes it and fills the clipboard.
            char remaining[128] = {0};
            nc_clip_id(engine, 0, remaining, sizeof(remaining));
            check(nc_clip_cut(engine, remaining), "cut the clip");
            check(nc_clip_count(engine) == 0, "cut removed it");
            check(nc_clipboard_has_clip(engine), "cut left it on the clipboard");

            // Duplicate places a copy right after the original.
            char duplicated[128] = {0};
            check(nc_clip_paste(engine, 0.0, pasted, sizeof(pasted)), "paste it back");
            nc_clip_id(engine, 0, remaining, sizeof(remaining));
            check(nc_clip_duplicate(engine, remaining, duplicated, sizeof(duplicated)), "duplicate it");
            check(nc_clip_count(engine) == 2, "duplicate made a second clip");
        }

        // --- fades and clip gain must reach the render, not just the picture ----
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "import for fades");
            char fadeClip[128] = {0};
            nc_clip_id(engine, 0, fadeClip, sizeof(fadeClip));

            check(nc_clip_fade_in(engine, 0) == 0.0, "a fresh clip has no fade in");
            check(nc_clip_set_fades(engine, fadeClip, 1.0, 0.5), "set a 1 s fade in");
            check(std::abs(nc_clip_fade_in(engine, 0) - 1.0) < 0.001, "the fade in stuck");
            check(std::abs(nc_clip_fade_out(engine, 0) - 0.5) < 0.001, "the fade out stuck");

            check(nc_clip_set_gain_db(engine, fadeClip, -12.0f), "set clip gain");
            check(std::abs(nc_clip_gain_db(engine, 0) + 12.0f) < 0.01f, "the gain stuck");

            char fadeProject[256] = "/tmp/neuracoust-io-smoke/Faded.ndaw";
            check(nc_project_save_as(engine, fadeProject, error, sizeof(error)), "save the fade");

            neuracoust::daw::ProjectDocument fadedProject;
            std::string fadeParseError;
            FILE* fadeFile = fopen(fadeProject, "rb");
            std::string fadeText;
            if (fadeFile != nullptr) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), fadeFile)) > 0) fadeText.append(buffer, read);
                fclose(fadeFile);
            }
            check(neuracoust::daw::deserializeProjectForPath(fadeText, fadeProject,
                                                             fadedProject, fadeParseError),
                  "parse the faded project");

            const std::string fadeBounce = "/tmp/neuracoust-io-smoke/faded.wav";
            check(neuracoust::daw::bounceProjectToWav(fadedProject, fadeBounce).ok, "bounce the fade");

            // Read the rendered peak in the first 100 ms and around the middle of
            // the fade. A 1 s fade in must leave the start far quieter than 0.5 s in.
            neuracoust::daw::WavAudioData rendered;
            std::string readError;
            check(neuracoust::daw::readPcmWavFile(fadeBounce, rendered, readError), "read the bounce");
            if (rendered.channels > 0 && rendered.sampleRate > 0) {
                auto peakBetween = [&](double from, double to) {
                    float peak = 0.0f;
                    const int64_t begin = static_cast<int64_t>(from * rendered.sampleRate);
                    const int64_t end = std::min<int64_t>(rendered.frameCount(),
                                                          static_cast<int64_t>(to * rendered.sampleRate));
                    for (int64_t frame = begin; frame < end; ++frame) {
                        for (int channel = 0; channel < rendered.channels; ++channel) {
                            peak = std::max(peak, std::abs(rendered.interleavedSamples[frame * rendered.channels + channel]));
                        }
                    }
                    return peak;
                };
                const float early = peakBetween(0.0, 0.1);
                const float late = peakBetween(0.85, 0.95);
                printf("fade in: peak 0-100ms = %.4f, peak 850-950ms = %.4f\n", early, late);
                check(early < late * 0.3f, "a fade in really attenuates the start");
                check(late > 0.001f, "and the clip is audible once the fade completes");
            }
        }

        // --- tracks: add, rename, delete -----------------------------------------
        {
            nc_project_new(engine);
            const int before = nc_track_count(engine);
            const int added = nc_track_add_audio(engine);
            check(added >= 0, "added an audio track");
            check(nc_track_count(engine) == before + 1, "track count grew");

            char addedName[128] = {0};
            nc_track_name(engine, added, addedName, sizeof(addedName));
            printf("added track: index=%d name='%s'\n", added, addedName);
            check(nc_history_can_undo(engine), "adding a track records a step");

            check(nc_track_rename(engine, added, "Guitar"), "rename the track");
            char renamed[128] = {0};
            nc_track_name(engine, added, renamed, sizeof(renamed));
            check(strcmp(renamed, "Guitar") == 0, "the rename stuck");
            check(!nc_track_rename(engine, added, "Master"), "a protected name is refused");
            check(!nc_track_rename(engine, added, "Audio 1"), "a name already in use is refused");

            check(nc_track_delete(engine, added, true), "delete the track");
            check(nc_track_count(engine) == before, "track count came back down");

            check(nc_history_undo(engine), "undo the delete");
            check(nc_track_count(engine) == before + 1, "the track returned");

            // Master must refuse deletion.
            int masterIndex = -1;
            for (int index = 0; index < nc_track_count(engine); ++index) {
                char type[64] = {0};
                nc_track_type(engine, index, type, sizeof(type));
                if (strcmp(type, "master") == 0) masterIndex = index;
            }
            check(masterIndex >= 0, "found the master track");
            check(!nc_track_delete(engine, masterIndex, true), "master refuses deletion");
        }

        // --- renaming a track must drag its clips along, or the track goes silent -
        {

            // The clips carry a track *name*. A rename that misses them leaves the
            // clip attached to a track that no longer exists, so its fader, mute and
            // solo stop reaching it. The bounce below still plays either way — the
            // renderer works off the rebuilt playlist — so it is the clip's own
            // trackName that has to be checked.
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 1.0, error, sizeof(error)), "a clip on Audio 1");
            check(nc_track_rename(engine, 0, "Guitar"), "rename the track under the clip");

            char clipTrackAfter[128] = {0};
            nc_clip_track(engine, 0, clipTrackAfter, sizeof(clipTrackAfter));

            check(strcmp(clipTrackAfter, "Guitar") == 0, "the clip followed the rename");

            char renameProject[256] = "/tmp/neuracoust-io-smoke/Renamed.ndaw";
            check(nc_project_save_as(engine, renameProject, error, sizeof(error)), "save the rename");
            neuracoust::daw::ProjectDocument renamedDocument;
            std::string renameError;
            std::string renameText;
            if (FILE* file = fopen(renameProject, "rb")) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) renameText.append(buffer, read);
                fclose(file);
            }
            check(neuracoust::daw::deserializeProjectForPath(renameText, renameProject,
                                                             renamedDocument, renameError),
                  "parse it");
            const std::string renameBounce = "/tmp/neuracoust-io-smoke/renamed.wav";
            check(neuracoust::daw::bounceProjectToWav(renamedDocument, renameBounce).ok, "bounce it");
            const double renamedAudibleAt = firstAudibleSecond(renameBounce);
            printf("renamed track: audio starts at %.3f s (expect 1.000)\n", renamedAudibleAt);
            check(std::abs(renamedAudibleAt - 1.0) < 0.05, "the renamed track still plays its clip");
        }

        // --- moving a clip to another track must not disturb its neighbours -------
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "clip A on track 0");
            check(nc_audio_import(engine, 0, wavPath, 5.0, error, sizeof(error)), "clip B on track 0");
            check(nc_clip_count(engine) == 2, "two clips on Audio 1");

            char clipA[128] = {0};
            for (int index = 0; index < 2; ++index) {
                if (nc_clip_start_seconds(engine, index) < 0.5) {
                    nc_clip_id(engine, index, clipA, sizeof(clipA));
                }
            }

            char moved[128] = {0};
            check(nc_clip_move_to_track(engine, clipA, 1, 2.0, moved, sizeof(moved)),
                  "move clip A to Audio 2 at 2 s");
            check(strlen(moved) > 0 && strcmp(moved, clipA) != 0, "it got a new id");

            // Neighbour untouched, mover relocated.
            bool sawNeighbourAtFive = false;
            bool sawMoverOnAudio2 = false;
            for (int index = 0; index < nc_clip_count(engine); ++index) {
                char track[128] = {0};
                nc_clip_track(engine, index, track, sizeof(track));
                const double start = nc_clip_start_seconds(engine, index);
                if (strcmp(track, "Audio 1") == 0 && std::abs(start - 5.0) < 0.01) sawNeighbourAtFive = true;
                if (strcmp(track, "Audio 2") == 0 && std::abs(start - 2.0) < 0.01) sawMoverOnAudio2 = true;
            }
            check(sawNeighbourAtFive, "the neighbour stayed at 5 s");
            check(sawMoverOnAudio2, "the mover sits on Audio 2 at 2 s");

            // And it still makes a sound there.
            char crossProject[256] = "/tmp/neuracoust-io-smoke/Crossed.ndaw";
            check(nc_project_save_as(engine, crossProject, error, sizeof(error)), "save the cross-track move");
            neuracoust::daw::ProjectDocument crossed;
            std::string crossError;
            FILE* crossFile = fopen(crossProject, "rb");
            std::string crossText;
            if (crossFile != nullptr) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), crossFile)) > 0) crossText.append(buffer, read);
                fclose(crossFile);
            }
            check(neuracoust::daw::deserializeProjectForPath(crossText, crossProject, crossed, crossError),
                  "parse it");
            const std::string crossBounce = "/tmp/neuracoust-io-smoke/crossed.wav";
            check(neuracoust::daw::bounceProjectToWav(crossed, crossBounce).ok, "bounce it");
            const double audibleAt = firstAudibleSecond(crossBounce);
            printf("cross-track move: audio starts at %.3f s (expect 2.000)\n", audibleAt);
            check(std::abs(audibleAt - 2.0) < 0.05, "the relocated clip is audible on its new track");
        }

        // --- batch edits: one undo step, and the selection keeps its spacing -------
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 1.0, error, sizeof(error)), "clip at 1 s");
            check(nc_audio_import(engine, 1, wavPath, 3.0, error, sizeof(error)), "clip at 3 s");
            check(nc_clip_count(engine) == 2, "two clips to select");

            char idA[128] = {0};
            char idB[128] = {0};
            for (int index = 0; index < 2; ++index) {
                if (nc_clip_start_seconds(engine, index) < 2.0) nc_clip_id(engine, index, idA, sizeof(idA));
                else nc_clip_id(engine, index, idB, sizeof(idB));
            }
            const char* selection[2] = {idA, idB};

            // Moving the selection right shifts both by the same amount.
            check(nc_clip_move_many(engine, selection, 2, 1.5) == 2, "moved both clips");
            check(startOfClip(engine, idA) > 2.49 && startOfClip(engine, idA) < 2.51, "clip A at 2.5 s");
            check(startOfClip(engine, idB) > 4.49 && startOfClip(engine, idB) < 4.51, "clip B at 4.5 s");

            // Dragging past zero holds the whole selection back rather than piling
            // the clips onto each other at 0 s.
            check(nc_clip_move_many(engine, selection, 2, -99.0) == 2, "moved both clips against zero");
            check(startOfClip(engine, idA) < 0.001, "clip A stopped at 0 s");
            check(std::abs(startOfClip(engine, idB) - 2.0) < 0.01, "clip B kept its 2 s spacing");

            // move_many is continuous, like a drag: nothing is undoable until the
            // gesture ends. Without this the delete below would undo to 1 s / 3 s.
            check(nc_history_record_gesture(engine, "Move clips"), "the drag recorded one step");

            // A batch delete is a single undo step, not one per clip.
            const int stepsBefore = countUndoSteps(engine);
            check(nc_clip_delete_many(engine, selection, 2) == 2, "deleted both clips");
            check(nc_clip_count(engine) == 0, "both clips gone");
            check(countUndoSteps(engine) == stepsBefore + 1, "the batch delete recorded one step");
            check(nc_history_undo(engine), "undo the batch delete");
            check(nc_clip_count(engine) == 2, "one undo brought both clips back");

            // Copy the pair, paste it at 6 s: the earliest lands there, the other
            // keeps its offset.
            char freshA[128] = {0};
            char freshB[128] = {0};
            for (int index = 0; index < 2; ++index) {
                if (nc_clip_start_seconds(engine, index) < 1.0) nc_clip_id(engine, index, freshA, sizeof(freshA));
                else nc_clip_id(engine, index, freshB, sizeof(freshB));
            }
            const char* restored[2] = {freshA, freshB};
            check(nc_clip_copy_many(engine, restored, 2), "copied the selection");
            check(nc_clipboard_clip_count(engine) == 2, "the clipboard holds two clips");
            check(nc_clip_paste_all(engine, 6.0) == 2, "pasted two clips");
            check(nc_result_count(engine) == 2, "the paste reported two new ids");

            char pastedA[128] = {0};
            nc_result_id(engine, 0, pastedA, sizeof(pastedA));
            char pastedB[128] = {0};
            nc_result_id(engine, 1, pastedB, sizeof(pastedB));
            printf("paste selection: %.3f s and %.3f s (expect 6.000 and 8.000)\n",
                   startOfClip(engine, pastedA), startOfClip(engine, pastedB));
            check(std::abs(startOfClip(engine, pastedA) - 6.0) < 0.01, "the earliest pasted clip is at 6 s");
            check(std::abs(startOfClip(engine, pastedB) - 8.0) < 0.01, "the other kept its 2 s offset");

            // And the pasted pair is audible where it looks, not silent.
            char batchProject[256] = "/tmp/neuracoust-io-smoke/Batch.ndaw";
            check(nc_project_save_as(engine, batchProject, error, sizeof(error)), "save the batch edit");
            check(nc_clip_delete_many(engine, restored, 2) == 2, "delete the originals");
            check(nc_project_save(engine, error, sizeof(error)), "save again with only the paste");

            neuracoust::daw::ProjectDocument batched;
            std::string batchError;
            std::string batchText;
            if (FILE* file = fopen(batchProject, "rb")) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) batchText.append(buffer, read);
                fclose(file);
            }
            check(neuracoust::daw::deserializeProjectForPath(batchText, batchProject, batched, batchError),
                  "parse the batch project");
            const std::string batchBounce = "/tmp/neuracoust-io-smoke/batch.wav";
            check(neuracoust::daw::bounceProjectToWav(batched, batchBounce).ok, "bounce the batch project");
            const double batchAudibleAt = firstAudibleSecond(batchBounce);
            printf("pasted selection: audio starts at %.3f s (expect 6.000)\n", batchAudibleAt);
            check(std::abs(batchAudibleAt - 6.0) < 0.05, "the pasted selection is audible at 6 s");
        }

        // --- range editing slices clips, it does not delete them whole -------------
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "a 2 s tone at 0 s");
            check(nc_clip_count(engine) == 1, "one clip");

            check(nc_project_set_loop_range(engine, 0.5, 1.5), "set the edit range to 0.5–1.5 s");
            check(std::abs(nc_project_loop_start(engine) - 0.5) < 0.001, "range start reads back");
            check(std::abs(nc_project_loop_end(engine) - 1.5) < 0.001, "range end reads back");
            check(!nc_project_set_loop_range(engine, 2.0, 2.0), "an empty range is refused");

            // Copy takes only the slice inside the range, not the whole clip.
            check(nc_range_copy(engine, 0.5, 1.5) == 1, "the range holds one clip's slice");
            check(nc_clipboard_clip_count(engine) == 1, "the clipboard took the slice");

            // Clearing leaves the head and the tail of the clip behind.
            check(nc_range_clear(engine, 0.5, 1.5), "clear the range");
            check(nc_clip_count(engine) == 2, "the clip became a head and a tail");

            char rangeProject[256] = "/tmp/neuracoust-io-smoke/Range.ndaw";
            check(nc_project_save_as(engine, rangeProject, error, sizeof(error)), "save the cleared range");
            neuracoust::daw::ProjectDocument ranged;
            std::string rangeError;
            std::string rangeText;
            if (FILE* file = fopen(rangeProject, "rb")) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) rangeText.append(buffer, read);
                fclose(file);
            }
            check(neuracoust::daw::deserializeProjectForPath(rangeText, rangeProject, ranged, rangeError),
                  "parse it");
            const std::string rangeBounce = "/tmp/neuracoust-io-smoke/range.wav";
            check(neuracoust::daw::bounceProjectToWav(ranged, rangeBounce).ok, "bounce it");

            const double audibleAt = firstAudibleSecond(rangeBounce);
            printf("cleared range: audio starts at %.3f s, hole 0.5–1.5 s silent = %d\n",
                   audibleAt, isSilentBetween(rangeBounce, 0.5, 1.5) ? 1 : 0);
            check(audibleAt >= 0.0 && audibleAt < 0.05, "the head still plays from 0 s");
            check(isSilentBetween(rangeBounce, 0.5, 1.5), "the cleared range is silent");
            check(!isSilentBetween(rangeBounce, 1.5, 2.0), "the tail after the range still plays");

            // Separate splits at both edges without removing anything.
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "a fresh 2 s tone");
            check(nc_range_separate(engine, 0.5, 1.5) > 0, "separate the range");
            check(nc_clip_count(engine) == 3, "head, range, tail");
        }

        // --- automation has to move the sound, not just draw a line ---------------
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "a 2 s tone at 0 s");

            check(nc_automation_parameter_supported("track.volume"), "volume is automatable");
            check(nc_automation_parameter_supported("track.pan"), "pan is automatable");
            check(!nc_automation_parameter_supported("track.mute"),
                  "a parameter the renderer ignores is refused");

            // What the clip sounds like before any automation touches it.
            char plainProject[256] = "/tmp/neuracoust-io-smoke/Unautomated.ndaw";
            check(nc_project_save_as(engine, plainProject, error, sizeof(error)), "save it plain");
            const std::string plainBounce = "/tmp/neuracoust-io-smoke/unautomated.wav";
            {
                neuracoust::daw::ProjectDocument plain;
                std::string plainError;
                std::string plainText;
                if (FILE* file = fopen(plainProject, "rb")) {
                    char buffer[8192];
                    size_t read = 0;
                    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) plainText.append(buffer, read);
                    fclose(file);
                }
                check(neuracoust::daw::deserializeProjectForPath(plainText, plainProject, plain, plainError),
                      "parse the plain project");
                check(neuracoust::daw::bounceProjectToWav(plain, plainBounce).ok, "bounce it plain");
            }
            const float plainPeak = peakBetween(plainBounce, 0.0, 0.2);
            check(plainPeak > 0.001f, "the untouched tone makes a sound");

            // Full level at 0 s, silence by 2 s: a ramp the ear could hear.
            check(nc_track_automation_add(engine, 0, "track.volume", 0.0, 0.0f), "point at 0 s, 0 dB");
            check(nc_track_automation_add(engine, 0, "track.volume", 2.0, -60.0f), "point at 2 s, -60 dB");
            check(nc_track_automation_count(engine, 0, "track.volume") == 2, "two points");
            check(std::abs(nc_track_automation_time(engine, 0, "track.volume", 0)) < 0.001,
                  "the points came back sorted by time");
            check(!nc_track_automation_add(engine, 0, "track.mute", 0.0, 1.0f),
                  "an unsupported parameter stores nothing");

            char autoProject[256] = "/tmp/neuracoust-io-smoke/Automated.ndaw";
            check(nc_project_save_as(engine, autoProject, error, sizeof(error)), "save the automation");
            neuracoust::daw::ProjectDocument automated;
            std::string autoError;
            std::string autoText;
            if (FILE* file = fopen(autoProject, "rb")) {
                char buffer[8192];
                size_t read = 0;
                while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) autoText.append(buffer, read);
                fclose(file);
            }
            check(neuracoust::daw::deserializeProjectForPath(autoText, autoProject, automated, autoError),
                  "parse it");
            const std::string autoBounce = "/tmp/neuracoust-io-smoke/automated.wav";
            check(neuracoust::daw::bounceProjectToWav(automated, autoBounce).ok, "bounce it");

            const float headPeak = peakBetween(autoBounce, 0.0, 0.2);
            const float tailPeak = peakBetween(autoBounce, 1.8, 2.0);
            printf("volume automation: plain %.4f, head %.4f, tail %.4f\n",
                   plainPeak, headPeak, tailPeak);
            // A 0 dB point changes nothing; the -60 dB point at the far end must.
            check(std::abs(headPeak - plainPeak) < plainPeak * 0.05f, "0 dB leaves the head alone");
            check(tailPeak < headPeak * 0.05f, "the ramp brought the tail down");

            // Deleting the points puts the fader back in charge.
            check(nc_track_automation_delete(engine, 0, "track.volume", 1), "delete the second point");
            check(nc_track_automation_delete(engine, 0, "track.volume", 0), "delete the first point");
            check(nc_track_automation_count(engine, 0, "track.volume") == 0, "no points left");

            // Pan lives in a lane rather than its own vector; same door.
            check(nc_track_automation_add(engine, 0, "track.pan", 0.0, -1.0f), "pan hard left at 0 s");
            check(nc_track_automation_add(engine, 0, "track.pan", 2.0, 1.0f), "pan hard right at 2 s");
            check(nc_track_automation_count(engine, 0, "track.pan") == 2, "two pan points");
            check(nc_track_automation_clear_range(engine, 0, "track.pan", -0.1, 3.0) == 2,
                  "clearing the range took both");
        }

        // --- markers: navigation only, but they have to survive a save ------------
        {
            nc_project_new(engine);
            check(nc_marker_count(engine) == 0, "a new project has no markers");

            char firstId[128] = {0};
            check(nc_marker_add(engine, 4.0, firstId, sizeof(firstId)), "add a marker at 4 s");
            check(strlen(firstId) > 0, "it got an id");
            char secondId[128] = {0};
            check(nc_marker_add(engine, 1.0, secondId, sizeof(secondId)), "add a marker at 1 s");
            check(nc_marker_count(engine) == 2, "two markers");

            // The engine keeps them sorted, so index 0 is the earlier one.
            check(std::abs(nc_marker_time(engine, 0) - 1.0) < 0.001, "markers come back sorted");
            check(std::abs(nc_marker_time(engine, 1) - 4.0) < 0.001, "the later marker is second");

            check(nc_marker_rename(engine, 1.0, 0.2, "Verse"), "rename the marker at 1 s");
            char markerName[128] = {0};
            nc_marker_name(engine, 0, markerName, sizeof(markerName));
            check(strcmp(markerName, "Verse") == 0, "the rename stuck");
            check(!nc_marker_rename(engine, 9.0, 0.2, "Nope"), "no marker near 9 s to rename");

            // The range between the markers around 2 s is 1 s to 4 s.
            double rangeStart = -1.0;
            double rangeEnd = -1.0;
            check(nc_marker_surrounding_range(engine, 2.0, &rangeStart, &rangeEnd),
                  "the markers around 2 s bound a range");
            printf("marker range around 2 s: %.3f-%.3f (expect 1.000-4.000)\n", rangeStart, rangeEnd);
            check(std::abs(rangeStart - 1.0) < 0.001 && std::abs(rangeEnd - 4.0) < 0.001,
                  "it is the stretch between them");

            // Dragging is continuous: it must not pile steps into the history. The
            // step count cannot be used to check that — undo/redo would restore a
            // snapshot and throw the uncommitted drag away — so read the top step.
            check(topUndoStep(engine) == "Rename marker", "the rename is the last step");
            check(nc_marker_move(engine, 1.0, 0.2, 1.5), "drag the marker");
            check(nc_marker_move(engine, 1.5, 0.2, 2.0), "keep dragging it");
            check(std::abs(nc_marker_time(engine, 0) - 2.0) < 0.001, "it landed at 2 s");
            check(topUndoStep(engine) == "Rename marker", "the drag recorded nothing on its own");

            check(nc_history_record_gesture(engine, "Move marker"), "the gesture records one step");
            check(topUndoStep(engine) == "Move marker", "and it is the whole drag");
            check(nc_history_undo(engine), "undo the drag");
            check(std::abs(nc_marker_time(engine, 0) - 1.0) < 0.001, "one undo took it all the way back");
            check(nc_history_redo(engine), "redo it");

            char markerProject[256] = "/tmp/neuracoust-io-smoke/Markers.ndaw";
            check(nc_project_save_as(engine, markerProject, error, sizeof(error)), "save the markers");
            check(nc_project_open(engine, markerProject, false, error, sizeof(error)), "reopen it");
            check(nc_marker_count(engine) == 2, "both markers came back");
            nc_marker_name(engine, 0, markerName, sizeof(markerName));
            check(strcmp(markerName, "Verse") == 0, "with their names");
            check(std::abs(nc_marker_time(engine, 0) - 2.0) < 0.001, "and where the drag left them");

            check(nc_marker_delete(engine, 2.0, 0.2), "delete the first marker");
            check(nc_marker_count(engine) == 1, "one left");
            check(!nc_marker_delete(engine, 20.0, 0.2), "nothing to delete out at 20 s");
        }

        // --- a MIDI note must reach the instrument and make a sound ---------------
        {
            nc_project_new(engine);
            const int instrumentTrack = nc_track_add_instrument(engine);
            check(instrumentTrack >= 0, "added an instrument track");

            // FabFilter One is a synth; without an instrument a region is silent by design.
            nc_plugin_scan(engine);
            const int instruments = nc_plugin_apply_filter(engine, "", "", "Instrument", "VST3");
            if (instruments <= 0) {
                printf("(no instrument plug-ins installed — skipping the MIDI sound check)\n");
            } else {
                char instrumentName[128] = {0};
                nc_plugin_name(engine, 0, instrumentName, sizeof(instrumentName));
                check(nc_track_set_instrument(engine, instrumentTrack, 0), "load the instrument");
                char loaded[128] = {0};
                nc_track_instrument_name(engine, instrumentTrack, loaded, sizeof(loaded));
                printf("instrument: %s\n", loaded);
                check(strcmp(loaded, instrumentName) == 0, "the instrument slot holds it");

                char regionId[128] = {0};
                check(nc_midi_region_add(engine, instrumentTrack, 1.0, 2.0, regionId, sizeof(regionId)),
                      "add a 2 s region at 1 s");
                check(nc_midi_region_count(engine) == 1, "one region");
                check(std::abs(nc_midi_region_start_seconds(engine, 0) - 1.0) < 0.001, "it starts at 1 s");

                // One beat in from the region start, one beat long, at 120 bpm: 1.5-2.0 s.
                char noteId[128] = {0};
                check(nc_midi_note_add(engine, regionId, 60, 1.0, 1.0, 100, noteId, sizeof(noteId)),
                      "add a middle-C note");
                check(nc_midi_note_count(engine, regionId) == 1, "one note");
                check(nc_midi_note_pitch(engine, regionId, 0) == 60, "it is middle C");

                char midiProject[256] = "/tmp/neuracoust-io-smoke/Midi.ndaw";
                check(nc_project_save_as(engine, midiProject, error, sizeof(error)), "save the MIDI project");
                neuracoust::daw::ProjectDocument midiDocument;
                std::string midiError;
                std::string midiText;
                if (FILE* file = fopen(midiProject, "rb")) {
                    char buffer[8192];
                    size_t read = 0;
                    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) midiText.append(buffer, read);
                    fclose(file);
                }
                check(neuracoust::daw::deserializeProjectForPath(midiText, midiProject, midiDocument, midiError),
                      "parse it");
                check(midiDocument.midiRegions.size() == 1 && midiDocument.midiRegions[0].notes.size() == 1,
                      "the note survived the round trip");

                const std::string midiBounce = "/tmp/neuracoust-io-smoke/midi.wav";
                check(neuracoust::daw::bounceProjectToWav(midiDocument, midiBounce).ok, "bounce it");
                const double midiAudibleAt = firstAudibleSecond(midiBounce);
                const float beforeNote = peakBetween(midiBounce, 0.0, 1.4);
                const float duringNote = peakBetween(midiBounce, 1.5, 2.0);
                printf("midi note: first audible %.3f s (expect ~1.500), peak before %.4f during %.4f\n",
                       midiAudibleAt, beforeNote, duringNote);
                check(duringNote > 0.001f, "the instrument played the note");
                check(beforeNote < duringNote * 0.05f, "and nothing sounded before it");
                check(std::abs(midiAudibleAt - 1.5) < 0.05, "the note landed a beat into the region");

                // The editor host talks to an instrument slot the same way it talks to
                // an insert, through a different door in the bridge.
                char instrumentPath[512] = {0};
                nc_track_instrument_plugin_path(engine, instrumentTrack, instrumentPath,
                                                sizeof(instrumentPath));
                check(strlen(instrumentPath) > 0, "the instrument slot has a plug-in path");
                check(nc_track_instrument_param_count(engine, instrumentTrack) == 0,
                      "and no stored parameters yet");
                check(nc_track_set_instrument_vst3_parameter(engine, instrumentTrack, 24, "Volume", 0.9),
                      "an editor edit lands on the instrument");
                check(nc_track_instrument_param_count(engine, instrumentTrack) == 1, "one parameter");
                check(nc_track_instrument_param_id(engine, instrumentTrack, 0) == 24, "by id");
                check(nc_track_instrument_param_value(engine, instrumentTrack, 0) == 0.9, "and value");
                nc_track_set_instrument_vst3_parameter(engine, instrumentTrack, 24, "Volume", 4.0);
                check(nc_track_instrument_param_value(engine, instrumentTrack, 0) == 1.0,
                      "out-of-range values are clamped");
                check(!nc_track_set_instrument_vst3_parameter(engine, 0, 1, "None", 0.5),
                      "a track with no instrument refuses");

                check(nc_midi_note_delete(engine, regionId, noteId), "delete the note");
                check(nc_midi_note_count(engine, regionId) == 0, "no notes left");
                check(nc_midi_region_delete(engine, regionId), "delete the region");
                check(nc_midi_region_count(engine) == 0, "no regions left");
            }
        }

        // --- a master insert has to reach the mix, not just the project file ------
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "a 2 s tone");
            check(nc_master_insert_count(engine) == 0, "the master chain starts empty");

            // Micro at its own defaults is a 4166 Hz low-pass. The fixture tone is a
            // 440 Hz sine, so it should survive; a plug-in that never got its
            // parameters would flatten it.
            const int filters = nc_plugin_apply_filter(engine, "FabFilter Micro", "", "", "VST3");
            if (filters <= 0) {
                printf("(FabFilter Micro not installed — skipping the master insert check)\n");
            } else {
                check(nc_master_add_insert(engine, 0), "add it to the master chain");
                check(nc_master_insert_count(engine) == 1, "one master insert");
                char masterName[128] = {0};
                nc_master_insert_name(engine, 0, masterName, sizeof(masterName));
                check(strcmp(masterName, "FabFilter Micro") == 0, "by name");
                check(!nc_master_add_insert(engine, 0), "the same plug-in twice is refused");

                check(nc_master_set_vst3_parameter(engine, 0, 0, "Frequency", 0.7),
                      "an editor edit lands on the master insert");
                check(nc_master_insert_param_count(engine, 0) == 1, "one stored parameter");
                check(nc_master_insert_param_value(engine, 0, 0) == 0.7, "with its value");

                char masterProject[256] = "/tmp/neuracoust-io-smoke/Master.ndaw";
                check(nc_project_save_as(engine, masterProject, error, sizeof(error)), "save it");
                neuracoust::daw::ProjectDocument mastered;
                std::string masterError;
                std::string masterText;
                if (FILE* file = fopen(masterProject, "rb")) {
                    char buffer[8192];
                    size_t read = 0;
                    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) masterText.append(buffer, read);
                    fclose(file);
                }
                check(neuracoust::daw::deserializeProjectForPath(masterText, masterProject, mastered, masterError),
                      "parse it");
                check(mastered.masterInserts.size() == 1, "the insert survived the round trip");

                const std::string masterBounce = "/tmp/neuracoust-io-smoke/master.wav";
                check(neuracoust::daw::bounceProjectToWav(mastered, masterBounce).ok, "bounce it");
                const float throughInsert = peakBetween(masterBounce, 0.2, 1.8);
                printf("master insert: 440 Hz tone peaks at %.4f through FabFilter Micro\n", throughInsert);
                check(throughInsert > 0.001f, "the tone came through the master insert");

                // Bypassing it must not silence the mix either.
                check(nc_master_set_insert_bypassed(engine, 0, true), "bypass it");
                check(nc_master_insert_bypassed(engine, 0), "the bypass latched");
                check(nc_master_remove_insert(engine, 0), "remove it");
                check(nc_master_insert_count(engine) == 0, "the chain is empty again");
            }
        }

        // --- bounce through the bridge, and time it -------------------------------
        {
            nc_project_new(engine);
            check(nc_audio_import(engine, 0, wavPath, 0.0, error, sizeof(error)), "import for bounce ui");

            NCBounceResult bounced;
            const auto began = std::chrono::steady_clock::now();
            const bool ok = nc_bounce_to_wav(engine, "/tmp/neuracoust-io-smoke/export.wav", &bounced);
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - began).count();

            printf("bounce: ok=%d duration=%.2f s peak=%.4f/%.4f clipped=%d silent=%d in %.0f ms\n",
                   (int)ok, bounced.durationSeconds, bounced.peakLeft, bounced.peakRight,
                   (int)bounced.clippingDetected, (int)bounced.nearSilent, elapsedMs);

            check(ok && bounced.ok, "the bounce succeeded");
            check(bounced.durationSeconds > 1.9, "it rendered the whole 2 s clip");
            check(bounced.peakLeft > 0.001f, "it is not silent");
            check(!bounced.nearSilent, "and the engine agrees it is not silent");
            check(bounced.missingMediaClipCount == 0, "no media went missing");
            check(firstAudibleSecond("/tmp/neuracoust-io-smoke/export.wav") < 0.05,
                  "the exported audio starts where the clip does");

            // The off-thread path renders a serialized copy. It must agree exactly
            // with the engine's own bounce, or a background export would lie.
            const int needed = nc_project_serialize(engine, nullptr, 0);
            check(needed > 0, "the document serializes");
            std::string snapshot(static_cast<size_t>(needed) + 1, '\0');
            check(nc_project_serialize(engine, snapshot.data(), snapshot.size()) == needed,
                  "the buffer round-trips");

            NCBounceResult fromSnapshot;
            const auto snapBegan = std::chrono::steady_clock::now();
            check(nc_bounce_snapshot_to_wav(snapshot.c_str(),
                                            "/tmp/neuracoust-io-smoke/export-snapshot.wav",
                                            &fromSnapshot),
                  "the snapshot bounce succeeded");
            const double snapMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - snapBegan).count();
            printf("snapshot bounce: peak=%.4f in %.0f ms\n", fromSnapshot.peakLeft, snapMs);

            check(std::abs(fromSnapshot.durationSeconds - bounced.durationSeconds) < 0.001,
                  "same duration as the engine bounce");
            check(std::abs(fromSnapshot.peakLeft - bounced.peakLeft) < 0.0001f,
                  "same peak as the engine bounce");

            neuracoust::daw::WavAudioData direct;
            neuracoust::daw::WavAudioData viaSnapshot;
            std::string readError;
            const std::string directPath = "/tmp/neuracoust-io-smoke/export.wav";
            const std::string snapshotPath = "/tmp/neuracoust-io-smoke/export-snapshot.wav";
            check(neuracoust::daw::readPcmWavFile(directPath, direct, readError) &&
                  neuracoust::daw::readPcmWavFile(snapshotPath, viaSnapshot, readError),
                  "read both renders");
            check(direct.interleavedSamples.size() == viaSnapshot.interleavedSamples.size(),
                  "both renders are the same length");
            check(std::equal(direct.interleavedSamples.begin(), direct.interleavedSamples.end(),
                             viaSnapshot.interleavedSamples.begin()),
                  "both renders are sample-for-sample identical");

            // A bad snapshot must fail, not render silence.
            NCBounceResult broken;
            check(!nc_bounce_snapshot_to_wav("{ not a project }",
                                             "/tmp/neuracoust-io-smoke/broken.wav", &broken),
                  "a corrupt snapshot fails");
            check(strlen(broken.message) > 0, "and says why");
        }

        // snapProjectTime always snaps — it does not consult a "snap enabled" flag.
        // The default project's grid unit is 1 s, so 1.234 lands on 1.0. Whether to
        // snap at all is the caller's decision.
        // The default project's timeline quantum is 0.1 s, so 1.234 lands on 1.2.
        const double snapped = nc_project_snap_time(engine, 1.234);
        printf("snap(1.234) = %.3f\n", snapped);
        check(std::abs(snapped - 1.2) < 0.0001, "snapping lands on the default 0.1 s quantum");
    }

    // --- waveform peaks -------------------------------------------------------
    {
        std::vector<float> mins(NC_WAVEFORM_BUCKETS, 0.0f);
        std::vector<float> maxs(NC_WAVEFORM_BUCKETS, 0.0f);
        check(!nc_waveform_peaks(engine, "/tmp/definitely-not-audio.wav", mins.data(), maxs.data()),
              "peaks fail on a missing file");

        check(nc_waveform_peaks(engine, wavPath, mins.data(), maxs.data()), "peaks read from a wav");

        float lowest = 0.0f;
        float highest = 0.0f;
        int silentBuckets = 0;
        for (int bucket = 0; bucket < NC_WAVEFORM_BUCKETS; ++bucket) {
            lowest = std::min(lowest, mins[bucket]);
            highest = std::max(highest, maxs[bucket]);
            if (maxs[bucket] - mins[bucket] < 0.01f) {
                ++silentBuckets;
            }
        }
        printf("waveform: min=%.3f max=%.3f silent buckets=%d/%d\n",
               lowest, highest, silentBuckets, NC_WAVEFORM_BUCKETS);

        // writeTestToneWavFile emits about -21 dBFS; the point is that the envelope
        // is present, full and symmetric, not that it is loud.
        check(highest > 0.05f, "the tone reaches a real positive peak");
        check(lowest < -0.05f, "and a real negative peak");
        check(silentBuckets == 0, "no bucket of a continuous tone is silent");
        check(std::abs(highest + lowest) < 0.05f, "the envelope is symmetric");

        // The second read must come from the cache and agree exactly.
        std::vector<float> mins2(NC_WAVEFORM_BUCKETS, 0.0f);
        std::vector<float> maxs2(NC_WAVEFORM_BUCKETS, 0.0f);
        check(nc_waveform_peaks(engine, wavPath, mins2.data(), maxs2.data()), "peaks read again");
        check(std::equal(mins.begin(), mins.end(), mins2.begin()), "cached mins match");
        check(std::equal(maxs.begin(), maxs.end(), maxs2.begin()), "cached maxs match");
    }

    nc_engine_destroy(engine);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("project io + audio import OK\n");
    return 0;
}
