// Exercises project open/save and audio import through the C facade, on real
// files. No audio device is opened: this is document plumbing, not playback.

#include "bridge/NeuracoustEngineBridge.h"
#include "audio/WavFile.h"

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
