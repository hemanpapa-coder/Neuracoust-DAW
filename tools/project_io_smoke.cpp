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
