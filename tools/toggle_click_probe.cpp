// Reproduces the live report "EQ나 역할 스위치를 토글할 때마다 디지털 노이즈" headlessly, and
// measures it: a render thread streams 256-frame blocks (the CoreAudio cadence) while the main
// thread performs the exact edits the bridge setters perform, and every captured sample is
// checked for a discontinuity no clean signal could produce.
//
// A 220 Hz tone at 48 kHz slews at most 2π·220/48000 ≈ 0.029 of its amplitude per sample, and
// the declick envelopes move ~0.002/sample — so a jump above 0.12× amplitude is a CLICK, not
// programme material. Scenarios:
//
//   1. declicked no-op reconcile        (the role/NDS-switch path with nothing really changing)
//   2. declicked reconcile, role field  (the actual 믹서·버스 내장↔NDS press)
//   3. EQ enable toggle, local strip    (updateTrackConsoleChannel — reported clean live)
//   4. EQ enable toggle, remote strip   (the "NDS로 EQ on/off" report; needs a host argument)
//
//   neuracoust_toggle_click_probe [host:port]

#include "audio/NeuracoustDspEngine.h"
#include "audio/RemoteDspServerClient.h"
#include "audio/WavFile.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace neuracoust::daw;

namespace {

struct CaptureRig {
    NeuracoustDspEngine engine;
    std::thread renderThread;
    std::atomic<bool> running {false};
    std::mutex captureMutex;
    std::vector<float> captured;      // interleaved stereo, appended per block
    std::vector<size_t> markers;      // capture positions (frames) when the edit was made

    bool start(const ProjectDocument& project, const std::string& ndsHost, std::string& error,
               bool monitorDsp = false) {
        AudioEngineSettings settings;
        settings.sampleRate = 48000.0;
        settings.bufferSize = 256;
        settings.monitorDspEnabled = monitorDsp;
        if (monitorDsp) settings.monitorModules = project.monitorModules;
        settings.monitorInputTrimDb = 0.0f;
        settings.monitorVolumeDb = 0.0f;
        settings.transportRunning = true;
        if (!ndsHost.empty()) {
            settings.remoteDspServer.ndsEnabled = true;
            settings.remoteDspServer.ndsHost = ndsHost;
            settings.remoteDspServer.roleChannelStrip = "nds";
            applyRemoteDspHostPort(settings.remoteDspServer);
        }
        if (!engine.configure(settings, settings.bufferSize, error) ||
            !engine.loadProject(project, error)) {
            return false;
        }
        running.store(true);
        renderThread = std::thread([this] {
            std::vector<float> block;
            while (running.load(std::memory_order_relaxed)) {
                engine.renderInterleavedStereo(256, block);
                {
                    std::lock_guard<std::mutex> lock(captureMutex);
                    captured.insert(captured.end(), block.begin(), block.end());
                }
                // Real cadence, so the declick's bounded waits behave the way they do live.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        return true;
    }

    void mark() {
        std::lock_guard<std::mutex> lock(captureMutex);
        markers.push_back(captured.size() / 2);
    }

    void stop() {
        running.store(false);
        if (renderThread.joinable()) renderThread.join();
    }

    // Worst sample-to-sample jump in a ±window around each marker, per channel. Prints where
    // each marker's worst jump sits (ms after the edit), which is what localises the cause.
    double worstClickNearMarkers(double windowSeconds = 0.35) const {
        const size_t window = static_cast<size_t>(windowSeconds * 48000.0);
        double worst = 0.0;
        for (size_t markerIndex = 0; markerIndex < markers.size(); ++markerIndex) {
            const size_t marker = markers[markerIndex];
            const size_t first = marker > window ? marker - window : 1;
            const size_t last = std::min(captured.size() / 2, marker + window);
            double markerWorst = 0.0;
            size_t markerWorstFrame = 0;
            for (size_t frame = first; frame < last; ++frame) {
                for (int channel = 0; channel < 2; ++channel) {
                    const float now = captured[frame * 2 + channel];
                    const float before = captured[(frame - 1) * 2 + channel];
                    const double jump = std::abs(now - before);
                    if (jump > markerWorst) { markerWorst = jump; markerWorstFrame = frame; }
                }
            }
            std::printf("    marker %zu: worst %.4f at %+.1f ms\n", markerIndex, markerWorst,
                        (static_cast<double>(markerWorstFrame) - static_cast<double>(marker)) / 48.0);
            worst = std::max(worst, markerWorst);
        }
        return worst;
    }

    float toneAmplitude() const {
        float peak = 0.0f;
        const size_t begin = captured.size() / 4;   // settled region
        for (size_t i = begin; i < captured.size() / 2; ++i) peak = std::max(peak, std::abs(captured[i]));
        return peak;
    }
};

ProjectDocument toneProject(const std::string& tonePath, const std::string& ndsHost) {
    ProjectDocument project = defaultProject();
    if (!ndsHost.empty()) {
        project.ndsHost = ndsHost;
        project.ndsEnabled = true;
        project.dspRoleChannelStrip = "nds";
    }
    for (auto& track : project.tracks) {
        if (track.trackType != "audio") continue;
        appendAudioClipAt(project, track.name, tonePath, 0.0, 8.0);
        break;
    }
    return project;
}

int failures = 0;
void check(bool ok, const char* what, double clickSize, double amplitude) {
    const double relative = amplitude > 0 ? clickSize / amplitude : 0.0;
    std::printf("%s: worst jump %.4f (%.0f%% of the tone) — %s\n",
                what, clickSize, relative * 100.0, ok ? "ok" : "CLICK");
    if (!ok) ++failures;
}

} // namespace

int main(int argc, char** argv) {
    const std::string ndsHost = argc > 1 ? argv[1] : "";
    const auto dir = std::filesystem::temp_directory_path() / "nc-toggle-click";
    std::filesystem::create_directories(dir);
    const std::string tonePath = (dir / "tone220.wav").string();
    if (!writeTestToneWavFile(tonePath, 48000, 8.0, 220.0)) {
        std::fprintf(stderr, "could not write tone\n");
        return 3;
    }
    const auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(600)); };
    std::string error;

    // 1+2: the declicked reconcile the role/NDS-switch setters run.
    {
        CaptureRig rig;
        ProjectDocument project = toneProject(tonePath, ndsHost);
        if (!rig.start(project, ndsHost, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        settle();
        rig.mark();
        rig.engine.beginGraphChangeDeclick();
        std::string updateError;
        if (!rig.engine.updateProject(project, updateError)) {
            std::fprintf(stderr, "no-op updateProject refused: %s\n", updateError.c_str());
        }
        rig.engine.endGraphChangeDeclick();
        settle();
        rig.mark();
        project.dspRoleMixer = project.dspRoleMixer == "nds" ? "internal" : "nds";
        rig.engine.beginGraphChangeDeclick();
        if (!rig.engine.updateProject(project, updateError)) {
            std::fprintf(stderr, "role updateProject refused: %s\n", updateError.c_str());
        }
        rig.engine.endGraphChangeDeclick();
        settle();
        rig.stop();
        const float amplitude = rig.toneAmplitude();
        const double click = rig.worstClickNearMarkers();
        check(click < 0.12 * amplitude, "declicked reconcile (no-op + role flip)", click, amplitude);
    }

    // 3/4: the EQ lamp — updateTrackConsoleChannel, no reconcile.
    {
        CaptureRig rig;
        ProjectDocument project = toneProject(tonePath, ndsHost);
        std::string trackName;
        for (const auto& track : project.tracks) {
            if (track.trackType == "audio") { trackName = track.name; break; }
        }
        if (!rig.start(project, ndsHost, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        settle();
        ConsoleChannelState console;
        for (int toggle = 0; toggle < 4; ++toggle) {
            console.eqEnabled = (toggle % 2) == 0;
            console.eqLfGainDb = 6.0f;
            rig.mark();
            rig.engine.updateTrackConsoleChannel(trackName, console);
            settle();
        }
        rig.stop();
        const float amplitude = rig.toneAmplitude();
        const double click = rig.worstClickNearMarkers();
        check(click < 0.12 * amplitude,
              ndsHost.empty() ? "EQ lamp toggles (local strip)" : "EQ lamp toggles (REMOTE strip)",
              click, amplitude);
    }

    // 5: the same declicked reconcile with the MONITOR DSP path on — the live sessions that
    // clicked all monitored through it, and updateProject resets its state.
    {
        CaptureRig rig;
        ProjectDocument project = toneProject(tonePath, ndsHost);
        if (!rig.start(project, ndsHost, error, /*monitorDsp=*/true)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        settle();
        std::string updateError;
        for (int i = 0; i < 3; ++i) {
            rig.mark();
            project.dspRoleMixer = project.dspRoleMixer == "nds" ? "internal" : "nds";
            rig.engine.beginGraphChangeDeclick();
            if (!rig.engine.updateProject(project, updateError)) {
                std::fprintf(stderr, "monitor-on updateProject refused: %s\n", updateError.c_str());
            }
            rig.engine.endGraphChangeDeclick();
            settle();
        }
        rig.stop();
        const float amplitude = rig.toneAmplitude();
        const double click = rig.worstClickNearMarkers();
        check(click < 0.12 * amplitude, "declicked reconcile, monitor DSP on", click, amplitude);
    }

    if (failures == 0) std::printf("toggle click probe: clean\n");
    return failures == 0 ? 0 : 2;
}
