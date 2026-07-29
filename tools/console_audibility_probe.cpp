// Reproduces the live report "컴프를 걸면 UI는 도는데 소리가 안 바뀐다" against the REALTIME
// render path (renderInterleavedStereo — the exact code the CoreAudio callback drives), fully
// headless. A 440 Hz tone CLIP voices one track — a clip, not the signal generator, because the
// console strip is pre-insert and the generator is synthesized after it, so a generator can
// never prove the strip audible. The render is measured three ways:
//
//   A. console modules off
//   B. compressor leaned on hard (threshold way down, ratio high)
//   C. EQ LF shelf +15 dB on a 100 Hz tone
//
// B must come out QUIETER than A and C LOUDER than A by amounts no meter can miss. If they come
// out equal, the console stage is running for the needles but not for the sound — the bug.
//
//   neuracoust_console_audibility_probe            # local strips (내장)
//   neuracoust_console_audibility_probe <host>     # strips assigned to a node

#include "audio/NeuracoustDspEngine.h"
#include "audio/RemoteDspServerClient.h"
#include "audio/WavFile.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace neuracoust::daw;

namespace {

double stereoRmsDb(const std::vector<float>& samples) {
    if (samples.empty()) return -160.0;
    double energy = 0.0;
    for (const float sample : samples) {
        energy += static_cast<double>(sample) * sample;
    }
    const double rms = std::sqrt(energy / static_cast<double>(samples.size()));
    return rms > 1e-9 ? 20.0 * std::log10(rms) : -160.0;
}

ProjectDocument makeProject(const std::string& tonePath,
                            const std::string& ndsHost,
                            int variant) {
    ProjectDocument project = defaultProject();
    if (!ndsHost.empty()) {
        project.ndsHost = ndsHost;
        project.ndsEnabled = true;
        project.dspRoleChannelStrip = "nds";
    }
    for (auto& track : project.tracks) {
        if (track.trackType != "audio") continue;
        appendAudioClipAt(project, track.name, tonePath, 0.0, 2.0);
        if (variant == 1) {          // compressor, leaned on
            track.consoleChannel.compEnabled = true;
            track.consoleChannel.compThresholdDb = -40.0f;
            track.consoleChannel.compRatio = 10.0f;
            track.consoleChannel.compMix = 1.0f;
        } else if (variant == 2) {   // LF shelf, loud — the tone below sits at 100 Hz for it
            track.consoleChannel.eqEnabled = true;
            track.consoleChannel.eqLfGainDb = 15.0f;
        } else if (variant == 3) {   // the user's exact setup: a model picked from the plate
            track.consoleChannel.model = "SSL 4000E";   // the UI's display string, verbatim
            track.consoleChannel.compEnabled = true;
            track.consoleChannel.compThresholdDb = -40.0f;
            track.consoleChannel.compRatio = 10.0f;
            track.consoleChannel.compMix = 1.0f;
        }
        break;
    }
    return project;
}

double renderVariantDb(const std::string& tonePath, const std::string& ndsHost, int variant) {
    AudioEngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    settings.monitorDspEnabled = false;
    settings.monitorInputTrimDb = 0.0f;
    settings.monitorVolumeDb = 0.0f;
    settings.transportRunning = true;
    if (!ndsHost.empty()) {
        // The engine decides placement from settings_.remoteDspServer (the bridge builds it from
        // the project); driving the engine directly means building the same thing here.
        settings.remoteDspServer.ndsEnabled = true;
        settings.remoteDspServer.ndsHost = ndsHost;
        settings.remoteDspServer.roleChannelStrip = "nds";
        applyRemoteDspHostPort(settings.remoteDspServer);
    }
    NeuracoustDspEngine engine;
    std::string error;
    if (!engine.configure(settings, settings.bufferSize, error) ||
        !engine.loadProject(makeProject(tonePath, ndsHost, variant), error)) {
        std::cerr << "engine setup failed: " << error << '\n';
        return -1000.0;
    }
    // One second of the 2 s clip, rendered the way CoreAudio drives the engine: 256-frame
    // callbacks. One giant block would trip the remote path's frame-count ceiling and
    // silently measure the local fallback instead.
    std::vector<float> render;
    std::vector<float> block;
    for (int i = 0; i < 48000 / 256; ++i) {
        engine.renderInterleavedStereo(256, block);
        render.insert(render.end(), block.begin(), block.end());
    }
    // Measure the tail only, past attack/warm-up ramps (and any remote-stream priming).
    const std::vector<float> tail(render.begin() + render.size() / 2, render.end());
    return stereoRmsDb(tail);
}

// Loads the project with the console FLAT, renders half a second, then enables the compressor
// through the same runtime edit the mixer lamp uses (updateTrackConsoleChannel), renders on, and
// measures the tail. viaChip assigns the strip with the per-track DSP chip instead of the global
// role — the path the mixer's machine button actually sets.
double renderLiveToggleDb(const std::string& tonePath, const std::string& ndsHost, bool viaChip) {
    AudioEngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    settings.monitorDspEnabled = false;
    settings.monitorInputTrimDb = 0.0f;
    settings.monitorVolumeDb = 0.0f;
    settings.transportRunning = true;
    if (!ndsHost.empty()) {
        settings.remoteDspServer.ndsEnabled = true;
        settings.remoteDspServer.ndsHost = ndsHost;
        if (!viaChip) {
            settings.remoteDspServer.roleChannelStrip = "nds";
        }
        applyRemoteDspHostPort(settings.remoteDspServer);
    }
    ProjectDocument project = makeProject(tonePath, ndsHost, 0);   // console flat at load
    std::string trackName;
    for (auto& track : project.tracks) {
        if (track.trackType == "audio") {
            trackName = track.name;
            if (viaChip) track.consoleDspMachine = "nds";
            break;
        }
    }
    NeuracoustDspEngine engine;
    std::string error;
    if (!engine.configure(settings, settings.bufferSize, error) ||
        !engine.loadProject(project, error)) {
        std::cerr << "engine setup failed: " << error << '\n';
        return -1000.0;
    }
    std::vector<float> render;
    std::vector<float> block;
    for (int i = 0; i < 24000 / 256; ++i) {                        // 0.5 s flat
        engine.renderInterleavedStereo(256, block);
    }
    ConsoleChannelState console;                                    // the lamp goes on
    console.compEnabled = true;
    console.compThresholdDb = -40.0f;
    console.compRatio = 10.0f;
    console.compMix = 1.0f;
    engine.updateTrackConsoleChannel(trackName, console);
    for (int i = 0; i < 48000 / 256; ++i) {                        // 1 s compressed
        engine.renderInterleavedStereo(256, block);
        render.insert(render.end(), block.begin(), block.end());
    }
    const std::vector<float> tail(render.begin() + render.size() / 2, render.end());
    return stereoRmsDb(tail);
}

} // namespace

int main(int argc, char** argv) {
    const std::string ndsHost = argc > 1 ? argv[1] : "";
    const auto dir = std::filesystem::temp_directory_path() / "nc-console-audibility";
    std::filesystem::create_directories(dir);
    const std::string tonePath = (dir / "tone100.wav").string();
    // 100 Hz: inside the LF shelf's reach AND loud enough to lean on the compressor.
    if (!writeTestToneWavFile(tonePath, 48000, 2.0, 100.0)) {
        std::cerr << "could not write tone fixture\n";
        return 3;
    }
    const double offDb = renderVariantDb(tonePath, ndsHost, 0);
    const double compDb = renderVariantDb(tonePath, ndsHost, 1);
    const double eqDb = renderVariantDb(tonePath, ndsHost, 2);
    const double modelCompDb = renderVariantDb(tonePath, ndsHost, 3);
    // The user's actual gesture: playback already running, THEN the comp lamp goes on — the
    // runtime edit path (updateTrackConsoleChannel), not load-time state.
    const double liveToggleDb = renderLiveToggleDb(tonePath, ndsHost, /*viaChip=*/false);
    // And assignment the way the mixer UI does it: the strip's own DSP chip
    // (track.consoleDspMachine), not the global role.
    const double chipToggleDb = ndsHost.empty() ? liveToggleDb
                                                : renderLiveToggleDb(tonePath, ndsHost, true);
    std::cout << (ndsHost.empty() ? "strips LOCAL" : "strips REMOTE " + ndsHost) << '\n'
              << "  console off        : " << offDb << " dBFS\n"
              << "  comp leaned        : " << compDb << " dBFS (expect clearly quieter)\n"
              << "  EQ LF +15          : " << eqDb << " dBFS (expect clearly louder)\n"
              << "  comp toggled live  : " << liveToggleDb << " dBFS (expect ~= comp leaned)\n"
              << "  comp live via chip : " << chipToggleDb << " dBFS (expect ~= comp leaned)\n"
              << "  comp + plate model : " << modelCompDb << " dBFS (expect ~= comp leaned)\n";
    bool ok = true;
    if (!(compDb < offDb - 3.0)) {
        std::cerr << "COMPRESSOR IS INAUDIBLE on the realtime path\n";
        ok = false;
    }
    if (!(eqDb > offDb + 3.0)) {
        std::cerr << "EQ IS INAUDIBLE on the realtime path\n";
        ok = false;
    }
    if (!(liveToggleDb < offDb - 3.0)) {
        std::cerr << "COMPRESSOR TOGGLED DURING PLAYBACK IS INAUDIBLE\n";
        ok = false;
    }
    if (!(chipToggleDb < offDb - 3.0)) {
        std::cerr << "COMPRESSOR ON A CHIP-ASSIGNED (NDS) STRIP IS INAUDIBLE\n";
        ok = false;
    }
    if (!(modelCompDb < offDb - 3.0)) {
        std::cerr << "A PLATE-PICKED CONSOLE MODEL BYPASSES THE WHOLE STRIP\n";
        ok = false;
    }

    // Remote-wait accounting: with strips local, the engine must report ZERO remote wait; on a
    // node, most blocks must report real waiting time. That figure is what the dock subtracts
    // from the 내장 load meter, so a lie here mislabels the load split.
    {
        AudioEngineSettings settings;
        settings.sampleRate = 48000.0;
        settings.bufferSize = 256;
        settings.monitorDspEnabled = false;
        settings.monitorInputTrimDb = 0.0f;
        settings.monitorVolumeDb = 0.0f;
        settings.transportRunning = true;
        if (!ndsHost.empty()) {
            settings.remoteDspServer.ndsEnabled = true;
            settings.remoteDspServer.ndsHost = ndsHost;
            settings.remoteDspServer.roleChannelStrip = "nds";
            applyRemoteDspHostPort(settings.remoteDspServer);
        }
        NeuracoustDspEngine engine;
        std::string error;
        if (engine.configure(settings, settings.bufferSize, error) &&
            engine.loadProject(makeProject(tonePath, ndsHost, 1), error)) {
            std::vector<float> block;
            double waitSum = 0.0;
            int waitedBlocks = 0;
            for (int i = 0; i < 200; ++i) {
                engine.renderInterleavedStereo(256, block);
                const double us = engine.lastBlockRemoteWaitUs();
                if (us > 0.0) { waitSum += us; ++waitedBlocks; }
            }
            std::cout << "  remote wait        : " << waitedBlocks << "/200 blocks, avg "
                      << (waitedBlocks > 0 ? waitSum / waitedBlocks : 0.0) << " us\n";
            if (ndsHost.empty() && waitedBlocks != 0) {
                std::cerr << "LOCAL RENDER REPORTED REMOTE WAIT TIME\n";
                ok = false;
            }
            if (!ndsHost.empty() && waitedBlocks <= 100) {
                std::cerr << "REMOTE STRIPS REPORTED NO WAIT TIME (load split would lie)\n";
                ok = false;
            }
        }
    }
    return ok ? 0 : 2;
}
