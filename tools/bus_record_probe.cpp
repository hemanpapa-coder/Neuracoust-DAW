// Pins the internal-bus routing + recording path: Audio 1 plays a tone into "내부 버스 1",
// Audio 2 takes that bus as its input, a bus record pass runs on the render clock, and the
// saved take must contain the tone — same length as the pass, at real level. Also pins that a
// bus nobody feeds records SILENCE of the right length (a take is a continuous stream), and
// that the receiving track actually hears the bus (its route carries the audio).
#include "audio/NeuracoustDspEngine.h"
#include "audio/WavFile.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace neuracoust::daw;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

double rmsDb(const std::vector<float>& samples, size_t from = 0) {
    if (samples.size() <= from) return -160.0;
    double energy = 0.0;
    for (size_t i = from; i < samples.size(); ++i) energy += static_cast<double>(samples[i]) * samples[i];
    const double rms = std::sqrt(energy / static_cast<double>(samples.size() - from));
    return rms > 1e-9 ? 20.0 * std::log10(rms) : -160.0;
}

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "nc-bus-record";
    std::filesystem::create_directories(dir);
    const std::string tonePath = (dir / "tone.wav").string();
    if (!writeTestToneWavFile(tonePath, 48000, 4.0, 220.0)) {
        std::fprintf(stderr, "could not write tone\n");
        return 3;
    }

    ProjectDocument project = defaultProject();
    std::string feeder, receiver;
    for (auto& track : project.tracks) {
        if (track.trackType != "audio") continue;
        if (feeder.empty()) {
            feeder = track.name;
            appendAudioClipAt(project, track.name, tonePath, 0.0, 4.0);
            track.outputBus = "내부 버스 1";
        } else if (receiver.empty()) {
            receiver = track.name;
            track.inputBus = "내부 버스 1";
        }
    }
    if (feeder.empty() || receiver.empty()) {
        std::fprintf(stderr, "default project lacks two audio tracks\n");
        return 3;
    }

    AudioEngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    settings.monitorDspEnabled = false;
    settings.monitorInputTrimDb = 0.0f;
    settings.monitorVolumeDb = 0.0f;
    settings.transportRunning = true;
    NeuracoustDspEngine engine;
    std::string error;
    if (!engine.configure(settings, settings.bufferSize, error) ||
        !engine.loadProject(project, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    // The mix still reaches the master through the receiver — the feeder's output left the
    // master bus, so audible output proves the bus route carried it. Warm up past the engine's
    // start ramps first, then measure a settled block.
    std::vector<float> block;
    for (int i = 0; i < 48000 / 256; ++i) engine.renderInterleavedStereo(256, block);
    // The receiver's own meter is the honest witness: the bus signal entered its route. The
    // device block only confirms it survives to the output (well above silence; the absolute
    // level rides the monitor trim/volume defaults).
    const auto status = engine.statusSnapshot();
    float receiverPeak = 0.0f;
    for (size_t i = 0; i < status.trackMeterNames.size() && i < status.trackPeakLeft.size(); ++i) {
        if (status.trackMeterNames[i] == receiver) receiverPeak = status.trackPeakLeft[i];
    }
    check(receiverPeak > 0.01f, "the receiving track meters the bus signal");
    check(status.masterBusPeakLeft > 0.01f, "and it reaches the master bus");
    check(rmsDb(block) > -90.0, "and the device output is not silence");

    // Record one second of the bus on the render clock.
    engine.beginRecording(3, 0, 2, 48000, receiver);
    for (int i = 0; i < 48000 / 256; ++i) engine.renderInterleavedStereo(256, block);
    const std::string takePath = (dir / "bus-take.wav").string();
    double durationSeconds = 0.0;
    int channels = 0;
    check(engine.endRecording(takePath, 24, error, durationSeconds, channels),
          "the bus record pass saves");
    check(std::abs(durationSeconds - 1.0) < 0.05, "the take is the pass's length");
    WavAudioData take;
    if (check(readPcmWavFile(takePath, take, error), "the take reads back"), take.frameCount() > 0) {
        check(rmsDb(take.interleavedSamples) > -40.0, "and it carries the tone, not silence");
    }

    // A bus nobody feeds records silence of the right length — never a shorter file.
    ProjectDocument unfed = defaultProject();
    std::string armed;
    for (auto& track : unfed.tracks) {
        if (track.trackType != "audio") continue;
        if (armed.empty()) {
            armed = track.name;
            track.inputBus = "내부 버스 2";
        } else {
            // Keep the render alive: an entirely empty timeline can skip rendering routes.
            appendAudioClipAt(unfed, track.name, tonePath, 0.0, 4.0);
        }
    }
    NeuracoustDspEngine unfedEngine;
    if (!unfedEngine.configure(settings, settings.bufferSize, error) ||
        !unfedEngine.loadProject(unfed, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    unfedEngine.renderInterleavedStereo(4800, block);
    unfedEngine.beginRecording(3, 0, 2, 48000, armed);
    for (int i = 0; i < 24000 / 256; ++i) unfedEngine.renderInterleavedStereo(256, block);
    const std::string silentPath = (dir / "silent-take.wav").string();
    check(unfedEngine.endRecording(silentPath, 24, error, durationSeconds, channels),
          "an unfed bus still saves a take");
    check(std::abs(durationSeconds - 0.5) < 0.05, "of the pass's length");
    WavAudioData silent;
    if (readPcmWavFile(silentPath, silent, error)) {
        check(rmsDb(silent.interleavedSamples) < -80.0, "and it is silence");
    } else {
        check(false, "the silent take reads back");
    }

    if (failures == 0) std::printf("internal bus record: all pinned\n");
    return failures == 0 ? 0 : 1;
}
