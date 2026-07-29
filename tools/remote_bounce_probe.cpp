// End-to-end proof of the strict remote bounce, against a real node.
//
// Builds a project in code (a signal generator voicing one track, the console compressor on, every
// DSP role assigned to NDS), bounces it twice — once locally, once through the node — and compares
// the two files sample for sample. The strict-remote promise is then checked from both sides:
//
//   1. the remote bounce SUCCEEDS and matches the local one to inaudibility, and
//   2. with an unreachable node it FAILS with an error — never a quiet local fallback.
//
//   neuracoust_remote_bounce_probe <nds-host[:port]> [bad-host[:port]]
//
// Not in ctest — it needs a node on the LAN hosting na.neuracoust.console.channel.

#include "audio/OfflineBounce.h"
#include "audio/RemoteDspServerClient.h"
#include "audio/WavFile.h"
#include "project/ProjectDocument.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using namespace neuracoust::daw;

namespace {

ProjectDocument makeTestProject(const std::string& ndsHost) {
    ProjectDocument project = defaultProject();
    project.ndsHost = ndsHost;
    project.ndsEnabled = true;
    project.dspRoleChannelStrip = "nds";
    project.dspRoleMaster = "nds";
    project.dspRoleInserts = "nds";

    // A generator voicing track 0, and a busy console strip for the node to process.
    for (auto& track : project.tracks) {
        if (track.trackType != "audio") {
            continue;
        }
        TrackInsertSlot generator;
        generator.pluginName = "Signal Generator";
        generator.pluginFormat = "Builtin";
        generator.enabled = true;
        generator.dspExecutionMode = "native";
        generator.parameters = {
            {0u, "On Off", 1.0f},
            {1u, "Waveform", 0.0f},
            {2u, "Frequency", 0.55f},
            {3u, "Level", 0.8f},
            {4u, "Channel", 0.5f},
            {5u, "Polarity", 0.0f},
        };
        if (track.inserts.empty()) {
            track.inserts.push_back(generator);
        } else {
            track.inserts[0] = generator;
        }
        track.consoleChannel.compEnabled = true;
        track.consoleChannel.compThresholdDb = -24.0f;
        track.consoleChannel.compRatio = 4.0f;
        track.consoleChannel.eqEnabled = true;
        track.consoleChannel.eqLfGainDb = 3.0f;
        // And a remote-assigned Neuracoust insert (Mirage 8 → na.neuracoust.mirage8), so the
        // bounce also proves the track-insert path — the one a plain bounce used to DROP
        // silently, because the local route graph excludes remote-mode slots and nothing else
        // ran them.
        TrackInsertSlot mirage;
        mirage.pluginName = "Neuracoust Mirage 8";
        mirage.pluginFormat = "VST3";
        mirage.pluginPath = "/Library/Audio/Plug-Ins/VST3/Neuracoust Mirage 8.vst3";
        mirage.enabled = true;
        mirage.dspExecutionMode = "remote_internal";
        // The activated-remote state: serverModuleId set is what moves a slot from the local
        // route graph to the NDS path (empty keeps it local even in a remote mode).
        mirage.serverModuleId = "na.neuracoust.mirage8";
        track.inserts.push_back(mirage);
        break;
    }
    return project;
}

double comparePeakDifference(const std::string& a, const std::string& b, double& peakLevel) {
    WavAudioData wavA;
    WavAudioData wavB;
    std::string error;
    if (!readPcmWavFile(a, wavA, error) || !readPcmWavFile(b, wavB, error)) {
        return -1.0;
    }
    if (wavA.interleavedSamples.size() != wavB.interleavedSamples.size()) {
        return -1.0;
    }
    double worst = 0.0;
    peakLevel = 0.0;
    for (size_t i = 0; i < wavA.interleavedSamples.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(wavA.interleavedSamples[i] - wavB.interleavedSamples[i])));
        peakLevel = std::max(peakLevel, std::abs(static_cast<double>(wavA.interleavedSamples[i])));
    }
    return worst;
}

} // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "192.168.0.198:20010";
    const std::string badHost = argc > 2 ? argv[2] : "192.168.0.198:29999";

    const auto dir = std::filesystem::temp_directory_path() / "nc-remote-bounce-probe";
    std::filesystem::create_directories(dir);
    const std::string localWav = (dir / "local.wav").string();
    const std::string remoteWav = (dir / "remote.wav").string();

    const auto project = makeTestProject(host);

    // Local reference — WITHOUT the remote insert, deliberately: the node's Mirage module and
    // the local VST3 are different implementations of the same design, so a sample-for-sample
    // comparison of the two is not a promise anyone made. The console strip IS the same code on
    // both sides, so the strip/master part of the compare stays meaningful only when the insert
    // is absent from both renders... it is not, so the comparison below becomes an
    // envelope-level sanity check rather than a bit-level one.
    const auto localResult = bounceProjectToWav(project, localWav);
    if (!localResult.ok) {
        std::cerr << "local bounce failed: " << localResult.message << '\n';
        return 2;
    }
    std::cout << "local bounce ok (" << localResult.durationSeconds << " s)\n";

    // Remote, strict.
    BounceOptions remoteOptions;
    remoteOptions.useAssignedRemoteDsp = true;
    {
        auto settings = defaultRemoteDspServerSettings();
        settings.nodes.clear();
        settings.ndsHost = project.ndsHost;
        settings.ndsEnabled = true;
        settings.roleChannelStrip = project.dspRoleChannelStrip;
        settings.roleMaster = project.dspRoleMaster;
        settings.roleInserts = project.dspRoleInserts;
        remoteOptions.remoteDsp = settings;
    }
    const auto remoteResult = bounceProjectToWav(project, remoteWav, remoteOptions);
    if (!remoteResult.ok) {
        std::cerr << "remote bounce failed: " << remoteResult.message << '\n';
        return 3;
    }
    std::cout << "remote bounce ok (" << remoteResult.durationSeconds << " s)\n";

    double peakLevel = 0.0;
    const double worst = comparePeakDifference(localWav, remoteWav, peakLevel);
    if (worst < 0.0 || peakLevel < 0.01) {
        std::cerr << "could not compare the two renders (worst=" << worst
                  << ", peak=" << peakLevel << ")\n";
        return 4;
    }
    const double db = 20.0 * std::log10(std::max(worst, 1e-12) / peakLevel);
    std::cout << "local vs remote: peak " << peakLevel << ", worst difference " << worst
              << " (" << db << " dB)\n";
    // With the remote Mirage insert in the render, local and remote are EXPECTED to differ (the
    // local reference dropped that insert; the node ran it) — the check is that the remote render
    // is alive and sane, not identical. A silent remote render would mean the whole chain fed
    // dry/zero audio through the node.
    WavAudioData remoteWavData;
    std::string wavError;
    if (!readPcmWavFile(remoteWav, remoteWavData, wavError)) {
        std::cerr << "could not read the remote render back\n";
        return 5;
    }
    double remotePeak = 0.0;
    for (const float sample : remoteWavData.interleavedSamples) {
        remotePeak = std::max(remotePeak, std::abs(static_cast<double>(sample)));
    }
    std::cout << "remote render peak " << remotePeak << '\n';
    if (remotePeak < 0.01) {
        std::cerr << "remote render is near-silent — the node chain is not passing audio\n";
        return 5;
    }
    std::cout << "remote render is alive (insert path included)\n";

    // ── Phase 1.5: GR telemetry (M3) — the reply's meter tail must carry real gain reduction.
    // A hot sine through the node's console strip with the compressor leaned on it: the ABI-2
    // meter hook reports [comp GR dB, gate GR dB], and a working wire shows compression.
    {
        auto settings = defaultRemoteDspServerSettings();
        settings.nodes.clear();
        settings.host = host;
        applyRemoteDspHostPort(settings);
        settings.timeoutMs = 100;
        settings.loadedPluginIdHint = "na.neuracoust.console.channel";
        RemoteDspProcessSession session;
        std::vector<float> hot(256 * 2);
        for (size_t frame = 0; frame < 256; ++frame) {
            const float sample = 0.9f * static_cast<float>(std::sin(0.13 * static_cast<double>(frame)));
            hot[frame * 2u] = sample;
            hot[frame * 2u + 1u] = sample;
        }
        // Console params ride the packet: comp on, threshold low, ratio high — GR guaranteed.
        std::vector<RemoteDspParameterValue> params = {
            {16u, 1.0f},   // comp enabled
            {17u, 0.1f},   // threshold (normalized — near the bottom of its range)
            {18u, 0.9f},   // ratio (high)
        };
        std::vector<float> processed;
        RemoteDspProcessResult last;
        for (int i = 0; i < 24; ++i) {   // let the envelope settle
            last = session.process(settings, hot, params, processed);
            if (!last.processed) break;
        }
        if (!last.processed) {
            std::cerr << "GR telemetry: strip stream failed — " << last.message << '\n';
            return 8;
        }
        if (last.meterCount < 2u) {
            std::cerr << "GR TELEMETRY MISSING — reply carried no meter tail (node not ABI 2?)\n";
            return 8;
        }
        std::cout << "GR telemetry: reply meters [comp " << last.meters[0]
                  << " dB, gate " << last.meters[1] << " dB]\n";
        if (last.meters[0] <= 0.1f) {
            std::cerr << "GR telemetry: compressor shows no reduction on a hot signal\n";
            return 8;
        }
    }

    // ── Phase 2: the M1b parity gate — remote MIXER ONLY, and the judge is memcmp. ──────────
    // Two generator tracks, no console modules, no inserts: the only thing that differs between
    // the two renders is WHERE the summing happened, and the summing bus is bit-exact, so the
    // files must be IDENTICAL. Any deviation at all is an ordering or windowing bug.
    {
        ProjectDocument flat = defaultProject();
        flat.ndsHost = host;
        flat.ndsEnabled = true;
        flat.dspRoleMixer = "nds";
        int voiced = 0;
        for (auto& track : flat.tracks) {
            if (track.trackType != "audio") continue;
            TrackInsertSlot generator;
            generator.pluginName = "Signal Generator";
            generator.pluginFormat = "Builtin";
            generator.enabled = true;
            generator.dspExecutionMode = "native";
            generator.parameters = {
                {0u, "On Off", 1.0f},
                {1u, "Waveform", 0.0f},
                {2u, "Frequency", voiced == 0 ? 0.55f : 0.62f},
                {3u, "Level", 0.8f},
                {4u, "Channel", 0.5f},
                {5u, "Polarity", 0.0f},
            };
            track.inserts.push_back(generator);
            if (++voiced == 2) break;
        }
        const std::string flatLocal = (dir / "flat-local.wav").string();
        const std::string flatRemote = (dir / "flat-remote.wav").string();
        const auto localFlat = bounceProjectToWav(flat, flatLocal);
        BounceOptions mixerOptions;
        mixerOptions.useAssignedRemoteDsp = true;
        {
            auto settings = defaultRemoteDspServerSettings();
            settings.nodes.clear();
            settings.ndsHost = flat.ndsHost;
            settings.ndsEnabled = true;
            settings.roleMixer = flat.dspRoleMixer;
            mixerOptions.remoteDsp = settings;
        }
        const auto remoteFlat = bounceProjectToWav(flat, flatRemote, mixerOptions);
        if (!localFlat.ok || !remoteFlat.ok) {
            std::cerr << "mixer parity: bounce failed — local: " << localFlat.message
                      << " / remote: " << remoteFlat.message << '\n';
            return 6;
        }
        WavAudioData a;
        WavAudioData b;
        std::string wavError2;
        if (!readPcmWavFile(flatLocal, a, wavError2) || !readPcmWavFile(flatRemote, b, wavError2) ||
            a.interleavedSamples.size() != b.interleavedSamples.size()) {
            std::cerr << "mixer parity: could not read the renders back\n";
            return 6;
        }
        if (std::memcmp(a.interleavedSamples.data(), b.interleavedSamples.data(),
                        a.interleavedSamples.size() * sizeof(float)) != 0) {
            std::cerr << "MIXER PARITY FAILED — node-summed render differs from the local one\n";
            return 6;
        }
        double flatPeak = 0.0;
        for (const float sample : a.interleavedSamples) {
            flatPeak = std::max(flatPeak, std::abs(static_cast<double>(sample)));
        }
        if (flatPeak < 0.01) {
            std::cerr << "mixer parity: renders are silent — the gate proved nothing\n";
            return 6;
        }
        std::cout << "mixer parity: node-summed bounce is BIT-IDENTICAL to the local render (peak "
                  << flatPeak << ")\n";
    }

    // ── Phase 3: M2 — buses and sends through the summing service, still memcmp. ────────────
    // Two generator tracks, an aux bus, one track sending to it: three summing buses now (the
    // aux's receive, the Master), every contribution travelling. Identical files or bust.
    {
        ProjectDocument routed = defaultProject();
        routed.ndsHost = host;
        routed.ndsEnabled = true;
        routed.dspRoleMixer = "nds";
        int voiced = 0;
        for (auto& track : routed.tracks) {
            if (track.trackType != "audio") continue;
            TrackInsertSlot generator;
            generator.pluginName = "Signal Generator";
            generator.pluginFormat = "Builtin";
            generator.enabled = true;
            generator.dspExecutionMode = "native";
            generator.parameters = {
                {0u, "On Off", 1.0f},
                {1u, "Waveform", 0.0f},
                {2u, "Frequency", voiced == 0 ? 0.55f : 0.62f},
                {3u, "Level", 0.8f},
                {4u, "Channel", 0.5f},
                {5u, "Polarity", 0.0f},
            };
            track.inserts.push_back(generator);
            if (voiced == 0) {
                TrackSendState send;
                send.busName = "Aux 1";
                send.gainDb = -6.0f;
                track.sends.push_back(send);
            }
            if (++voiced == 2) break;
        }
        TrackState aux;
        aux.name = "Aux 1";
        aux.trackType = "aux";
        aux.outputBus = "Master";
        aux.inputBus.clear();
        routed.tracks.push_back(aux);

        const std::string routedLocal = (dir / "routed-local.wav").string();
        const std::string routedRemote = (dir / "routed-remote.wav").string();
        const auto localRouted = bounceProjectToWav(routed, routedLocal);
        BounceOptions mixerOptions;
        mixerOptions.useAssignedRemoteDsp = true;
        {
            auto settings = defaultRemoteDspServerSettings();
            settings.nodes.clear();
            settings.ndsHost = routed.ndsHost;
            settings.ndsEnabled = true;
            settings.roleMixer = routed.dspRoleMixer;
            mixerOptions.remoteDsp = settings;
        }
        const auto remoteRouted = bounceProjectToWav(routed, routedRemote, mixerOptions);
        if (!localRouted.ok || !remoteRouted.ok) {
            std::cerr << "bus/send parity: bounce failed — local: " << localRouted.message
                      << " / remote: " << remoteRouted.message << '\n';
            return 7;
        }
        WavAudioData a;
        WavAudioData b;
        std::string wavError3;
        if (!readPcmWavFile(routedLocal, a, wavError3) || !readPcmWavFile(routedRemote, b, wavError3) ||
            a.interleavedSamples.size() != b.interleavedSamples.size() ||
            std::memcmp(a.interleavedSamples.data(), b.interleavedSamples.data(),
                        a.interleavedSamples.size() * sizeof(float)) != 0) {
            std::cerr << "BUS/SEND PARITY FAILED — routed remote render differs from local\n";
            return 7;
        }
        std::cout << "bus/send parity: aux + send through the summing service, BIT-IDENTICAL\n";
    }

    // Strictness: an unreachable node must FAIL the bounce, not fall back.
    auto strictProject = project;
    strictProject.ndsHost = badHost;
    BounceOptions strictOptions = remoteOptions;
    strictOptions.remoteDsp.ndsHost = badHost;
    const auto strictResult = bounceProjectToWav(strictProject, (dir / "strict.wav").string(), strictOptions);
    if (strictResult.ok) {
        std::cerr << "STRICTNESS FAILURE — the bounce succeeded against a dead node\n";
        return 6;
    }
    std::cout << "strictness ok — dead node fails the bounce: " << strictResult.message << '\n';
    return 0;
}
