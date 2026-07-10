// Reproduces the DAW realtime engine path for a track (channel) insert and
// reports whether the insert actually changes the rendered audio. Mirrors how
// AudioEngineSmokeTest drives NeuracoustDspEngine, but with an arbitrary plugin
// descriptor so we can test the user's exact plugin (e.g. Waves H-Reverb).

#include "audio/NeuracoustDspEngine.h"
#include "audio/WavFile.h"
#include "core/DawState.h"
#include "project/ProjectDocument.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string arg(int argc, char** argv, const std::string& key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (key == argv[i]) return argv[i + 1];
    }
    return {};
}

double renderEnergy(neuracoust::daw::NeuracoustDspEngine& engine, int blocks, int blockSize) {
    std::vector<float> out;
    double energy = 0.0;
    for (int b = 0; b < blocks; ++b) {
        engine.renderInterleavedStereo(blockSize, out);
        for (float v : out) {
            energy += std::abs(static_cast<double>(v));
        }
    }
    return energy;
}

double renderPeakDbfs(neuracoust::daw::NeuracoustDspEngine& engine, int blocks, int blockSize) {
    std::vector<float> out;
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        engine.renderInterleavedStereo(blockSize, out);
        for (float v : out) {
            peak = std::max(peak, std::abs(v));
        }
    }
    return peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -144.0;
}
} // namespace

int main(int argc, char** argv) {
    using namespace neuracoust::daw;
    const std::string path = arg(argc, argv, "--plugin");
    const std::string name = arg(argc, argv, "--name");
    const std::string classId = arg(argc, argv, "--class-id");
    const std::string className = arg(argc, argv, "--class-name");
    if (path.empty()) {
        std::cerr << "need --plugin\n";
        return 2;
    }

    std::string wavPath = arg(argc, argv, "--source-wav");
    if (wavPath.empty()) {
        wavPath = (std::filesystem::temp_directory_path() / "nc-engine-insert-probe.wav").string();
        WavAudioData src;
        src.channels = 1;
        src.sampleRate = 48000;
        src.interleavedSamples.assign(48000 * 3, 0.0f);
        double phase = 0.0;
        for (size_t i = 0; i < src.interleavedSamples.size(); ++i) {
            src.interleavedSamples[i] = static_cast<float>(std::sin(phase) * 0.2);
            phase += 2.0 * 3.14159265358979323846 * 220.0 / 48000.0;
        }
        std::string werr;
        if (!writePcm16WavFile(wavPath, src, werr)) {
            std::cerr << "could not write source wav: " << werr << "\n";
            return 3;
        }
    }

    float clipGainDb = 0.0f;
    {
        const std::string cg = arg(argc, argv, "--clip-gain");
        if (!cg.empty()) clipGainDb = std::stof(cg);
    }
    std::string dspMode = arg(argc, argv, "--dsp-mode");
    if (dspMode.empty()) dspMode = "native";
    auto buildProject = [&](bool bypass) {
        auto project = defaultProject();
        project.sampleRate = 48000.0;
        project.monitorInputTrimDb = 0.0f;
        project.monitorVolumeDb = 0.0f;
        project.clips.push_back({"probe-clip", "Audio 1", wavPath, 0.0, 2.5, 0.0, clipGainDb});
        TrackInsertSlot insert;
        insert.pluginName = name.empty() ? "Insert" : name;
        insert.pluginFormat = "VST3";
        insert.pluginPath = path;
        insert.bypassed = bypass;
        insert.enabled = true;
        insert.dspExecutionMode = dspMode;
        insert.pluginClassId = classId;
        insert.pluginClassName = className;
        project.tracks[0].inserts.push_back(insert);
        return project;
    };

    const int blockSize = 512;
    const int blocks = 200;

    AudioEngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 512;
    settings.monitorDspEnabled = false;
    settings.monitorInputTrimDb = 0.0f;
    settings.monitorVolumeDb = 0.0f;
    settings.transportRunning = true;

    // Wet render (insert active)
    NeuracoustDspEngine wetEngine;
    std::string error;
    if (!wetEngine.configure(settings, settings.bufferSize, error) ||
        !wetEngine.loadProject(buildProject(false), error)) {
        std::cerr << "wet configure/load failed: " << error << "\n";
        return 4;
    }
    const auto wetStatus = wetEngine.statusSnapshot();
    std::cout << "WET status: " << wetStatus.message << "\n";
    std::cout << "  activeRealtimeVst3TrackInsertCount=" << wetStatus.activeRealtimeVst3TrackInsertCount
              << " activeOfflineVst3TrackInsertCount=" << wetStatus.activeOfflineVst3TrackInsertCount
              << " activeRealtimeVst3MasterInsertCount=" << wetStatus.activeRealtimeVst3MasterInsertCount
              << " activeRemoteDspTrackInsertCount=" << wetStatus.activeRemoteDspTrackInsertCount << "\n";
    const double wetEnergy = renderEnergy(wetEngine, blocks, blockSize);
    const auto wetStatusAfter = wetEngine.statusSnapshot();
    std::cout << "WET status after render: " << wetStatusAfter.message << "\n";

    // Bypassed render (dry reference)
    NeuracoustDspEngine dryEngine;
    if (!dryEngine.configure(settings, settings.bufferSize, error) ||
        !dryEngine.loadProject(buildProject(true), error)) {
        std::cerr << "dry configure/load failed: " << error << "\n";
        return 5;
    }
    const double dryEnergy = renderEnergy(dryEngine, blocks, blockSize);

    // Peak of the bypassed render == the level the insert (and its editor
    // observer) sees. For a -18 dBFS source with clip gain 0 this must read -18.
    NeuracoustDspEngine peakEngine;
    if (peakEngine.configure(settings, settings.bufferSize, error) &&
        peakEngine.loadProject(buildProject(true), error)) {
        std::cout << "BYPASSED render peak = " << renderPeakDbfs(peakEngine, blocks, blockSize) << " dBFS\n";
    }

    std::cout << "wetEnergy=" << wetEnergy << " dryEnergy=" << dryEnergy << "\n";
    const double rel = dryEnergy > 0 ? std::abs(wetEnergy - dryEnergy) / dryEnergy : 0.0;
    std::cout << "relativeDifference=" << rel << "\n";
    if (wetEnergy <= 1e-3) {
        std::cout << "RESULT: WET render is SILENT (insert produced silence)\n";
        return 6;
    }
    if (rel < 0.01) {
        std::cout << "RESULT: WET ~= DRY -> insert is NOT being applied in the engine path\n";
        return 7;
    }
    std::cout << "RESULT: insert IS applied in the engine path (wet differs from dry)\n";
    return 0;
}
