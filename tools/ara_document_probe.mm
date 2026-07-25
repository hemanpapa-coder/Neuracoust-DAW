// ARA document-controller probe — a throwaway harness, NOT part of the DAW.
//
// Melodyne blocks when its document controller is created from a plain console process: it expects
// a running main run loop (NSApplication), the same trap TRITON's state restore hit. So this probe
// is a real Cocoa app that does its ARA work from inside the run loop, which is the only honest way
// to tell "our ARA code is wrong" apart from "the test environment was too bare".
//
// It never ships. Its whole job is to answer, for one plug-in:
//   - does createDocumentControllerWithDocument return?
//   - does the plug-in accept an audio source / modification / playback region?
//   - does teardown come back cleanly?
//
// Run:  neuracoust_ara_document_probe <name> <path.vst3>
// Exits non-zero on failure, and — importantly — exits by itself, so a hang is visible as a timeout
// rather than a window sitting there forever.

#import <Cocoa/Cocoa.h>

#include "audio/WavFile.h"
#include "plugins/AraHost.h"
#include "plugins/Vst3HostFoundation.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

/// A couple of seconds of a two-partial tone, so an ARA plug-in has something with a pitch to find.
std::string writeProbeTone() {
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
    const std::string path = "/tmp/neuracoust-ara-probe-tone.wav";
    std::string error;
    if (!neuracoust::daw::writePcm16WavFile(path, wav, error)) {
        fprintf(stderr, "probe: could not write test tone: %s\n", error.c_str());
        return {};
    }
    return path;
}

int runProbe(const std::string& name, const std::string& path) {
    const auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(name, path);

    fprintf(stderr, "[1] factory…\n");
    fflush(stderr);
    const auto info = neuracoust::daw::inspectAraFactory(descriptor);
    if (!info.available) {
        fprintf(stderr, "    NOT ARA: %s\n", info.message.c_str());
        return 2;
    }
    fprintf(stderr, "    %s %s (%s) · ARA %d–%d · compatible=%d\n",
            info.plugInName.c_str(), info.versionString.c_str(), info.manufacturerName.c_str(),
            info.lowestSupportedApiGeneration, info.highestSupportedApiGeneration,
            static_cast<int>(info.compatibleWithHost));
    fflush(stderr);

    const std::string tone = writeProbeTone();
    if (tone.empty()) {
        return 3;
    }

    neuracoust::daw::AraDocumentController controller;
    std::string message;

    fprintf(stderr, "[2] createDocumentControllerWithDocument…\n");
    fflush(stderr);
    if (!controller.create(descriptor, "Neuracoust ARA Probe", message)) {
        fprintf(stderr, "    FAILED: %s\n", message.c_str());
        return 4;
    }
    fprintf(stderr, "    ok: %s\n", message.c_str());
    fflush(stderr);

    fprintf(stderr, "[3] audio source + modification + playback region…\n");
    fflush(stderr);
    if (!controller.addAudioFile(tone, "probe-clip-1", "Probe Tone", message)) {
        fprintf(stderr, "    FAILED: %s\n", message.c_str());
        return 5;
    }
    fprintf(stderr, "    ok: %s (sources=%zu)\n", message.c_str(), controller.audioSourceCount());
    fflush(stderr);

    fprintf(stderr, "[4] bindToDocumentControllerWithRoles…\n");
    fflush(stderr);
    if (!controller.bindPlugInInstance(message)) {
        fprintf(stderr, "    FAILED: %s\n", message.c_str());
        return 6;
    }
    fprintf(stderr, "    ok: %s\n", message.c_str());
    fprintf(stderr, "    roles granted — playbackRenderer=%d editorRenderer=%d editorView=%d\n",
            static_cast<int>(controller.boundPlaybackRenderer()),
            static_cast<int>(controller.boundEditorRenderer()),
            static_cast<int>(controller.boundEditorView()));
    fflush(stderr);

    fprintf(stderr, "[5] editor view (IPlugView)…\n");
    fflush(stderr);
    // A real window, because attaching is the part that can fail — creating a view proves little.
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 900, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [window setTitle:@"Neuracoust ARA Probe"];
    int viewWidth = 0;
    int viewHeight = 0;
    if (!controller.createEditorView((__bridge void*)window.contentView, viewWidth, viewHeight, message)) {
        fprintf(stderr, "    FAILED: %s\n", message.c_str());
        return 7;
    }
    fprintf(stderr, "    ok: %s (%d x %d)\n", message.c_str(), viewWidth, viewHeight);
    fflush(stderr);
    // Show it briefly so the plug-in's GUI actually gets a draw pass — a view that only exists on
    // paper is not proof. Then take it down the way a host closing the window would.
    [window makeKeyAndOrderFront:nil];
    for (int i = 0; i < 30; ++i) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    controller.destroyEditorView();
    [window orderOut:nil];
    fprintf(stderr, "    view destroyed\n");
    fflush(stderr);

    fprintf(stderr, "[6] playback-renderer render…\n");
    fflush(stderr);
    const std::string renderPath = "/tmp/neuracoust-ara-probe-render.wav";
    if (!controller.renderToWavFile(renderPath, message)) {
        fprintf(stderr, "    FAILED: %s\n", message.c_str());
        return 8;
    }
    fprintf(stderr, "    %s\n", message.c_str());
    // A file that exists proves nothing — measure it. With no edits applied the renderer should
    // hand back essentially the source tone, so compare energy against the input.
    neuracoust::daw::WavAudioData rendered;
    std::string readError;
    if (!neuracoust::daw::readPcmWavFile(renderPath, rendered, readError)) {
        fprintf(stderr, "    FAILED to read back: %s\n", readError.c_str());
        return 9;
    }
    double sum = 0.0;
    double peak = 0.0;
    for (float sample : rendered.interleavedSamples) {
        sum += static_cast<double>(sample) * sample;
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    const double rms = rendered.interleavedSamples.empty()
                           ? 0.0
                           : std::sqrt(sum / static_cast<double>(rendered.interleavedSamples.size()));
    fprintf(stderr, "    rendered %zu frames · rms=%.5f peak=%.5f\n",
            rendered.frameCount(), rms, peak);
    fflush(stderr);
    if (rms < 0.005) {
        fprintf(stderr, "    FAILED: the renderer produced (near) silence.\n");
        return 10;
    }

    fprintf(stderr, "[7] archive round-trip…\n");
    fflush(stderr);
    std::string archive;
    if (!controller.storeArchive(archive, message)) {
        fprintf(stderr, "    FAILED: %s\n", message.c_str());
        return 11;
    }
    fprintf(stderr, "    stored: %s (base64 %zu chars)\n", message.c_str(), archive.size());
    fflush(stderr);
    if (archive.empty()) {
        fprintf(stderr, "    FAILED: the plug-in wrote nothing.\n");
        return 12;
    }

    // The real test is a fresh document: tear this one down entirely, rebuild the graph with the
    // same persistent IDs, and push the archive back in. Anything less would not prove persistence.
    controller.destroy();
    neuracoust::daw::AraDocumentController restored;
    if (!restored.create(descriptor, "Neuracoust ARA Probe", message)) {
        fprintf(stderr, "    FAILED to recreate: %s\n", message.c_str());
        return 13;
    }
    if (!restored.addAudioFile(tone, "probe-clip-1", "Probe Tone", message)) {
        fprintf(stderr, "    FAILED to re-add source: %s\n", message.c_str());
        return 14;
    }
    if (!restored.restoreArchive(archive, message)) {
        fprintf(stderr, "    FAILED: %s\n", message.c_str());
        return 15;
    }
    fprintf(stderr, "    restored: %s\n", message.c_str());
    fflush(stderr);
    if (!restored.bindPlugInInstance(message)) {
        fprintf(stderr, "    FAILED to rebind: %s\n", message.c_str());
        return 16;
    }
    const std::string secondPath = "/tmp/neuracoust-ara-probe-render-2.wav";
    if (!restored.renderToWavFile(secondPath, message)) {
        fprintf(stderr, "    FAILED to re-render: %s\n", message.c_str());
        return 17;
    }
    neuracoust::daw::WavAudioData second;
    if (!neuracoust::daw::readPcmWavFile(secondPath, second, readError)) {
        fprintf(stderr, "    FAILED to read re-render: %s\n", readError.c_str());
        return 18;
    }
    // Same document state in, same audio out. A drift here means the archive did not carry
    // everything the plug-in needed.
    double worst = 0.0;
    const size_t compared = std::min(rendered.interleavedSamples.size(), second.interleavedSamples.size());
    for (size_t i = 0; i < compared; ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(rendered.interleavedSamples[i] -
                                                             second.interleavedSamples[i])));
    }
    fprintf(stderr, "    re-render matches to %.6f over %zu samples\n", worst, compared);
    fflush(stderr);
    if (compared == 0 || worst > 0.001) {
        fprintf(stderr, "    FAILED: restored document renders differently.\n");
        return 19;
    }
    restored.destroy();

    fprintf(stderr, "[8] teardown…\n");
    fflush(stderr);
    controller.destroy();
    fprintf(stderr, "    ok\n");
    fprintf(stderr, "PROBE PASSED\n");
    fflush(stderr);
    return 0;
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
        // A real NSApplication, because that is the whole point of this harness.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        __block int status = 70;
        // Run the ARA work from inside the run loop, once it is actually turning.
        dispatch_async(dispatch_get_main_queue(), ^{
            status = runProbe(name, path);
            [NSApp terminate:nil];
        });
        // Hard stop: a plug-in that blocks forever must fail the probe, not hang the session.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(60 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            fprintf(stderr, "PROBE TIMED OUT (60 s) — the plug-in never returned.\n");
            fflush(stderr);
            _Exit(75);
        });
        [NSApp run];
        return status;
    }
}
