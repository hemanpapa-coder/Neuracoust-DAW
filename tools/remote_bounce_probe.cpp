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

    // Local reference.
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
    const double db = 20.0 * std::log10(worst / peakLevel);
    std::cout << "local vs remote: peak " << peakLevel << ", worst difference " << worst
              << " (" << db << " dB)\n";
    // The strip's biquads differ between macOS libm and glibc by an ULP or two — the same
    // measured floor as the parity check. Anything worse than -60 dB is a real defect.
    if (db > -60.0) {
        std::cerr << "MISMATCH — the remote bounce does not match the local render\n";
        return 5;
    }
    std::cout << "match — the node's bounce is inaudibly identical to the local one\n";

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
