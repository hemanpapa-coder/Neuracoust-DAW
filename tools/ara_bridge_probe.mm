// End-to-end ARA probe through the ENGINE BRIDGE — a dev harness, not part of the DAW.
//
// The document probe proves our ARA host talks to the plug-in. This proves the DAW's own path works:
// import a file, open an ARA session on the clip, commit, and check the project really changed and
// survived a save/load. Everything a user would do except moving the mouse.
//
// Like the document probe it is a Cocoa app, because Melodyne needs a running main run loop.
//
// Run:  neuracoust_ara_bridge_probe <name> <path.vst3>

#import <Cocoa/Cocoa.h>

#include "audio/WavFile.h"
#include "bridge/NeuracoustEngineBridge.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    fprintf(stderr, "  %s %s\n", condition ? "ok  " : "FAIL", what);
    fflush(stderr);
    if (!condition) {
        ++failures;
    }
}

std::string writeProbeTone(const std::string& path) {
    neuracoust::daw::WavAudioData wav;
    wav.channels = 1;
    wav.sampleRate = 44100;
    wav.bitsPerSample = 16;
    const int frames = wav.sampleRate * 2;
    wav.interleavedSamples.resize(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / wav.sampleRate;
        wav.interleavedSamples[static_cast<size_t>(i)] =
            0.4f * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * t) +
                                      0.3 * std::sin(2.0 * M_PI * 440.0 * t));
    }
    std::string error;
    if (!neuracoust::daw::writePcm16WavFile(path, wav, error)) {
        fprintf(stderr, "probe: could not write test tone: %s\n", error.c_str());
        return {};
    }
    return path;
}

int runProbe(const std::string& pluginName, const std::string& pluginPath) {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "neuracoust-ara-bridge";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::string tone = writeProbeTone((root / "tone.wav").string());
    if (tone.empty()) {
        return 3;
    }

    NCEngine* engine = nc_engine_create();
    if (engine == nullptr) {
        fprintf(stderr, "probe: engine could not be created\n");
        return 4;
    }

    fprintf(stderr, "[1] project + import\n");
    char error[512] = {0};
    const std::string projectPath = (root / "AraProbe" / "AraProbe.ndaw").string();
    std::filesystem::create_directories((root / "AraProbe"), ec);
    check(nc_project_save_as(engine, projectPath.c_str(), error, sizeof(error)), "project saved");
    check(nc_audio_import(engine, 0, tone.c_str(), 0.0, error, sizeof(error)), "tone imported");
    check(nc_clip_count(engine) == 1, "one clip");
    if (nc_clip_count(engine) != 1) {
        nc_engine_destroy(engine);
        return 5;
    }
    char clipId[128] = {0};
    nc_clip_id(engine, 0, clipId, sizeof(clipId));
    char sourceBefore[1024] = {0};
    nc_clip_source_path(engine, 0, sourceBefore, sizeof(sourceBefore));
    const double durationBefore = nc_clip_duration_seconds(engine, 0);
    check(!nc_clip_has_ara_edits(engine, clipId), "a fresh clip has no ARA edits");

    fprintf(stderr, "[2] ARA plug-in list\n");
    // The app scans at startup; a bare engine has an empty catalog, so do it here too.
    const int scanned = nc_plugin_scan(engine);
    fprintf(stderr, "    scanned %d plug-in(s)\n", scanned);
    const int araCount = nc_ara_plugin_count(engine);
    fprintf(stderr, "    %d ARA plug-in(s) installed\n", araCount);
    check(araCount > 0, "at least one ARA plug-in is listed");

    fprintf(stderr, "[3] open session\n");
    check(nc_ara_open(engine, clipId, pluginName.c_str(), pluginPath.c_str(), error, sizeof(error)),
          "ARA session opened on the clip");
    if (!nc_ara_is_open(engine)) {
        fprintf(stderr, "    open error: %s\n", error);
        nc_engine_destroy(engine);
        return 6;
    }
    char openClip[128] = {0};
    nc_ara_open_clip_id(engine, openClip, sizeof(openClip));
    check(std::string(openClip) == clipId, "the session reports the right clip");
    // A second session must be refused rather than quietly opening a rival document.
    check(!nc_ara_open(engine, clipId, pluginName.c_str(), pluginPath.c_str(), error, sizeof(error)),
          "a second session is refused");

    fprintf(stderr, "[4] editor window (exactly what AraEditor.swift does)\n");
    // Same shape as the app: window first, host view already in the hierarchy, THEN attach.
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 900, 644)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 900, 644)];
    NSView* host = [[NSView alloc] initWithFrame:NSMakeRect(0, 44, 900, 600)];
    [content addSubview:host];
    [window setContentView:content];

    int viewWidth = 0;
    int viewHeight = 0;
    const bool attached = nc_ara_attach_editor(engine, (__bridge void*)host, &viewWidth, &viewHeight,
                                               error, sizeof(error));
    check(attached, "the plug-in editor attached to a live window view");
    if (!attached) {
        fprintf(stderr, "    attach error: %s\n", error);
    }
    check(viewWidth > 0 && viewHeight > 0, "the editor reported a size");
    fprintf(stderr, "    editor %d x %d\n", viewWidth, viewHeight);
    [window makeKeyAndOrderFront:nil];
    for (int i = 0; i < 40; ++i) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    [window orderOut:nil];

    fprintf(stderr, "[5] commit\n");
    check(nc_ara_commit(engine, error, sizeof(error)), "edits committed");
    if (strlen(error) > 0) {
        fprintf(stderr, "    commit error: %s\n", error);
    }
    nc_ara_close(engine);
    check(!nc_ara_is_open(engine), "session closed");
    check(nc_clip_has_ara_edits(engine, clipId), "the clip now carries ARA edits");

    char sourceAfter[1024] = {0};
    nc_clip_source_path(engine, 0, sourceAfter, sizeof(sourceAfter));
    check(std::string(sourceAfter) != sourceBefore, "the clip points at the printed audio");
    check(std::filesystem::exists(sourceAfter), "the printed file exists");
    // A print that changed the clip's length would move everything after it on the timeline.
    check(std::fabs(nc_clip_duration_seconds(engine, 0) - durationBefore) < 0.001,
          "the clip's duration is unchanged");
    // The print must actually contain the tone, not silence.
    neuracoust::daw::WavAudioData printed;
    std::string readError;
    double rms = 0.0;
    if (neuracoust::daw::readPcmWavFile(std::string(sourceAfter), printed, readError)) {
        double sum = 0.0;
        for (float sample : printed.interleavedSamples) {
            sum += static_cast<double>(sample) * sample;
        }
        rms = printed.interleavedSamples.empty()
                  ? 0.0
                  : std::sqrt(sum / static_cast<double>(printed.interleavedSamples.size()));
    }
    fprintf(stderr, "    printed rms=%.5f\n", rms);
    check(rms > 0.005, "the printed audio is not silence");

    fprintf(stderr, "[6] save / load\n");
    check(nc_project_save(engine, error, sizeof(error)), "project saved with the edits");
    nc_engine_destroy(engine);

    NCEngine* reopened = nc_engine_create();
    check(reopened != nullptr, "second engine created");
    if (reopened == nullptr) {
        return 7;
    }
    check(nc_project_open(reopened, projectPath.c_str(), false, error, sizeof(error)), "project reopened");
    check(nc_clip_count(reopened) == 1, "the clip came back");
    char reopenedClipId[128] = {0};
    nc_clip_id(reopened, 0, reopenedClipId, sizeof(reopenedClipId));
    // This is the whole point of archiving: the edits must still be there after a round trip.
    check(nc_clip_has_ara_edits(reopened, reopenedClipId), "the ARA archive survived save/load");

    fprintf(stderr, "[7] re-open the session against the reloaded project\n");
    check(nc_ara_open(reopened, reopenedClipId, pluginName.c_str(), pluginPath.c_str(),
                      error, sizeof(error)),
          "the stored archive restores into a new session");
    if (strlen(error) > 0) {
        fprintf(stderr, "    reopen error: %s\n", error);
    }
    nc_ara_close(reopened);

    fprintf(stderr, "[8] clear the edits\n");
    check(nc_clip_clear_ara_edits(reopened, reopenedClipId, error, sizeof(error)),
          "edits cleared");
    check(!nc_clip_has_ara_edits(reopened, reopenedClipId), "the clip is back to plain audio");
    nc_engine_destroy(reopened);

    fprintf(stderr, failures == 0 ? "BRIDGE PROBE PASSED\n" : "BRIDGE PROBE FAILED (%d)\n", failures);
    fflush(stderr);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, const char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <plugin name> <path.vst3>\n", argv[0]);
        return 64;
    }
    const std::string name = argv[1];
    const std::string path = argv[2];

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        __block int status = 70;
        dispatch_async(dispatch_get_main_queue(), ^{
            status = runProbe(name, path);
            [NSApp terminate:nil];
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(120 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            fprintf(stderr, "BRIDGE PROBE TIMED OUT (120 s)\n");
            fflush(stderr);
            _Exit(75);
        });
        [NSApp run];
        return status;
    }
}
