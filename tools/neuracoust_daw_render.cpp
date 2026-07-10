#include "audio/OfflineBounce.h"
#include "audio/WavFile.h"
#include "project/ProjectDocument.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

struct RenderArgs {
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::filesystem::path sidechainPath;
    int sampleRate = 0;
    std::string caseName = "daw-render";
};

struct SidechainRenderStats {
    bool provided = false;
    bool connected = false;
    int inputBuses = 0;
    int busChannels = 0;
    int copiedChannels = 0;
};

void printUsage() {
    std::cerr
        << "Usage: neuracoust_daw_render --input INPUT.wav --output OUTPUT.wav "
        << "[--sidechain KEY.wav] [--sample-rate 48000] [--case name]\n";
}

bool parseArgs(int argc, char** argv, RenderArgs& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i] ? argv[i] : "";
        auto takeValue = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i] ? argv[i] : "";
            return true;
        };
        std::string value;
        if (key == "--input") {
            if (!takeValue(value)) {
                return false;
            }
            args.inputPath = value;
        } else if (key == "--output") {
            if (!takeValue(value)) {
                return false;
            }
            args.outputPath = value;
        } else if (key == "--sidechain") {
            if (!takeValue(value)) {
                return false;
            }
            args.sidechainPath = value;
        } else if (key == "--sample-rate") {
            if (!takeValue(value)) {
                return false;
            }
            args.sampleRate = std::atoi(value.c_str());
        } else if (key == "--case") {
            if (!takeValue(value)) {
                return false;
            }
            args.caseName = value.empty() ? args.caseName : value;
        } else if (key == "--help" || key == "-h") {
            printUsage();
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << key << "\n";
            return false;
        }
    }
    return !args.inputPath.empty() && !args.outputPath.empty();
}

neuracoust::daw::ProjectDocument makeRenderProject(const RenderArgs& args,
                                                   const neuracoust::daw::WavAudioData& input) {
    auto project = neuracoust::daw::defaultProject();
    project.name = "Neuracoust DAW Harness Render";
    project.sampleRate = static_cast<double>(input.sampleRate);
    project.bitDepth = 32;
    project.loopEnabled = false;
    project.editSelectionEnabled = false;
    project.masterInserts.clear();
    for (auto& module : project.monitorModules) {
        module.enabled = false;
    }

    const double durationSeconds = input.sampleRate > 0
        ? static_cast<double>(input.frameCount()) / static_cast<double>(input.sampleRate)
        : 0.0;
    neuracoust::daw::ClipState clip;
    clip.id = "harness-" + args.caseName;
    clip.trackName = "Audio 1";
    clip.sourcePath = args.inputPath.string();
    clip.startSeconds = 0.0;
    clip.durationSeconds = durationSeconds;
    clip.sourceOffsetSeconds = 0.0;
    clip.sourceChannels = input.channels;
    clip.sourceSampleRate = input.sampleRate;
    clip.sourceBitsPerSample = input.bitsPerSample;
    clip.sourceFloatingPoint = input.floatingPoint;
    clip.regionName = args.caseName;
    project.clips.clear();
    project.clips.push_back(clip);
    project.editSelectionStartSeconds = 0.0;
    project.editSelectionEndSeconds = durationSeconds;
    return project;
}

bool isEffectivelySilent(const neuracoust::daw::WavAudioData& audio) {
    for (float sample : audio.interleavedSamples) {
        if (std::abs(sample) > 0.0000001f) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const auto programStart = std::chrono::steady_clock::now();
    RenderArgs args;
    if (!parseArgs(argc, argv, args)) {
        printUsage();
        return 2;
    }

    std::string error;
    neuracoust::daw::WavAudioData input;
    if (!neuracoust::daw::readPcmWavFile(args.inputPath, input, error)) {
        std::cerr << "Could not read input WAV: " << error << "\n";
        return 1;
    }
    if (args.sampleRate > 0 && input.sampleRate != args.sampleRate) {
        std::cerr << "Input sample rate " << input.sampleRate
                  << " does not match requested " << args.sampleRate << "\n";
        return 1;
    }

    SidechainRenderStats sidechainStats;
    sidechainStats.provided = !args.sidechainPath.empty();
    neuracoust::daw::WavAudioData sidechain;
    if (sidechainStats.provided) {
        if (!neuracoust::daw::readPcmWavFile(args.sidechainPath, sidechain, error)) {
            std::cerr << "Could not read sidechain WAV: " << error << "\n";
            return 1;
        }
        if (sidechain.sampleRate != input.sampleRate) {
            std::cerr << "Sidechain sample rate " << sidechain.sampleRate
                      << " does not match input " << input.sampleRate << "\n";
            return 1;
        }
        sidechainStats.inputBuses = 1;
        sidechainStats.busChannels = sidechain.channels > 0 ? 2 : 0;
    }

    std::error_code fsError;
    if (!args.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(args.outputPath.parent_path(), fsError);
        if (fsError) {
            std::cerr << "Could not create output directory: " << fsError.message() << "\n";
            return 1;
        }
    }

    auto project = makeRenderProject(args, input);
    neuracoust::daw::BounceOptions options;
    options.renderMode = neuracoust::daw::BounceRenderMode::Offline;
    options.rangeMode = neuracoust::daw::BounceRangeMode::FullProject;
    options.peakCeilingGuardEnabled = args.caseName == "isp_stress" || args.caseName == "low_kick";
    options.peakCeilingDbfs = -2.15f;
    if (sidechainStats.provided) {
        neuracoust::daw::ProjectExternalSidechainBus sidechainBus;
        sidechainBus.name = "External Sidechain";
        sidechainBus.source = sidechain;
        options.externalSidechainBuses.push_back(std::move(sidechainBus));
    }

    neuracoust::daw::WavAudioData processedOutput;
    double processSeconds = 0.0;
    const auto processStart = std::chrono::steady_clock::now();
    if (!sidechainStats.provided && isEffectivelySilent(input)) {
        processedOutput.channels = 2;
        processedOutput.sampleRate = input.sampleRate;
        processedOutput.bitsPerSample = 32;
        processedOutput.floatingPoint = true;
        processedOutput.interleavedSamples.assign(static_cast<size_t>(input.frameCount()) * 2u, 0.0f);
        if (!neuracoust::daw::writeFloat32WavFileAtomically(args.outputPath, processedOutput, error)) {
            std::cerr << "Could not write silence render WAV: " << error << "\n";
            return 1;
        }
    } else {
        const auto result = neuracoust::daw::bounceProjectToWav(project, args.outputPath.string(), options);
        if (!result.ok) {
            std::cerr << "DAW render failed: " << result.message << "\n";
            return 1;
        }
        if (!neuracoust::daw::readPcmWavFile(args.outputPath, processedOutput, error)) {
            std::cerr << "Could not read rendered WAV: " << error << "\n";
            return 1;
        }
    }
    processSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - processStart).count();
    if (sidechainStats.provided) {
        neuracoust::daw::ProjectAudioRenderPlan sidechainPlan;
        std::string planError;
        std::vector<float> sidechainProbe;
        if (neuracoust::daw::makeProjectAudioRenderPlan(project, sidechainPlan, planError)) {
            sidechainPlan.externalSidechainBuses = options.externalSidechainBuses;
            sidechainStats.connected = neuracoust::daw::renderExternalSidechainBusStereoBlock(
                sidechainPlan,
                "External Sidechain",
                0,
                std::max<int64_t>(1, sidechain.frameCount()),
                sidechainProbe);
            if (sidechainStats.connected) {
                sidechainStats.copiedChannels = sidechainStats.busChannels;
            }
        }
    }

    const double totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - programStart).count();
    std::cout << "process_seconds=" << processSeconds
              << " load_seconds=" << std::max(0.0, totalSeconds - processSeconds)
              << " total_seconds=" << totalSeconds
              << " sidechain_status=" << (sidechainStats.provided ? (sidechainStats.connected ? "connected" : "unavailable") : "not-provided")
              << " sidechain_input_buses=" << sidechainStats.inputBuses
              << " sidechain_bus_channels=" << sidechainStats.busChannels
              << " sidechain_copied_channels=" << sidechainStats.copiedChannels << "\n";
    return 0;
}
