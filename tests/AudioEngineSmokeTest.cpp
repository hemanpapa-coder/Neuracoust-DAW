#include "audio/NeuracoustDspEngine.h"
#include "audio/ListenRoom.h"
#include "audio/MetronomeClick.h"
#include "audio/MonitorOutputRouting.h"
#include "audio/RealtimeAudioEngine.h"
#include "audio/RecordingTake.h"
#include "audio/WavFile.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>

#if defined(NEURACOUST_DAW_HAS_OPUS) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

std::filesystem::path audioEngineSmokeTempRoot() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("neuracoust-daw-audio-engine-smoke-" + unique);
    std::filesystem::create_directories(root);
    return root;
}

double stereoRmsDb(const std::vector<float>& interleavedStereo) {
    if (interleavedStereo.empty()) {
        return -120.0;
    }
    double sumSquares = 0.0;
    for (const float sample : interleavedStereo) {
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const double rms = std::sqrt(sumSquares / static_cast<double>(interleavedStereo.size()));
    return rms > 0.0 ? 20.0 * std::log10(rms) : -120.0;
}

#if defined(NEURACOUST_DAW_HAS_OPUS) && !defined(_WIN32)
bool recvExact(int fd, void* out, size_t size) {
    auto* bytes = static_cast<uint8_t*>(out);
    size_t done = 0;
    while (done < size) {
        const ssize_t got = recv(fd, bytes + done, size - done, 0);
        if (got <= 0) {
            return false;
        }
        done += static_cast<size_t>(got);
    }
    return true;
}
#endif

int main() {
    const auto smokeTempRoot = audioEngineSmokeTempRoot();
    {
        auto modules = neuracoust::daw::defaultMonitorDspModules();
        modules[0].speakerOutputA = "Main 1-2";
        modules[0].speakerOutputB = "Output 3-4";
        modules[0].speakerOutputC = "None";

        modules[0].activeTargetSlot = 0;
        auto route = neuracoust::daw::resolveMonitorOutputRoute(modules, 8);
        if (!route.assigned || !route.available || route.leftChannel != 0 || route.rightChannel != 1) {
            std::cerr << "Monitor speaker A did not resolve to Main 1-2\n";
            return 43;
        }

        modules[0].activeTargetSlot = 1;
        route = neuracoust::daw::resolveMonitorOutputRoute(modules, 8);
        if (!route.assigned || !route.available || route.leftChannel != 2 || route.rightChannel != 3 ||
            neuracoust::daw::monitorOutputRequiredChannels(modules) != 4) {
            std::cerr << "Monitor speaker B did not resolve to Output 3-4\n";
            return 44;
        }
        route = neuracoust::daw::resolveMonitorOutputRoute(modules, 2);
        if (!route.assigned || route.available || route.leftChannel != 0 || route.rightChannel != 1 ||
            route.description.find("unavailable") == std::string::npos) {
            std::cerr << "Unavailable monitor speaker route did not fall back to Main 1-2\n";
            return 45;
        }

        modules[0].activeTargetSlot = 2;
        route = neuracoust::daw::resolveMonitorOutputRoute(modules, 8);
        // A "None" route is the modelled/virtual path, not a mute: it must still leave on
        // the main L/R pair so a selected speaker model is audible. (Selecting a model sets
        // the slot's route to "None" — treating that as silent black-holed the monitor.)
        if (!route.assigned || !route.available || route.leftChannel != 0 || route.rightChannel != 1 ||
            route.requestedRoute != "None" || route.description.find("Speaker C") == std::string::npos) {
            std::cerr << "Monitor speaker C modelled (None) route should still resolve to Main 1-2\n";
            return 46;
        }
    }

    {
        const auto projectRoot = smokeTempRoot / "portable-render-session";
        const auto projectPath = projectRoot / "Session.ndaw";
        const auto audioPath = projectRoot / "Audio Files" / "take.wav";
        std::filesystem::create_directories(audioPath.parent_path());

        neuracoust::daw::WavAudioData source;
        source.channels = 2;
        source.sampleRate = 100;
        source.interleavedSamples.assign(16000 * 2, 0.0f);
        for (size_t frame = 0; frame < 16000; ++frame) {
            source.interleavedSamples[frame * 2] = 0.25f;
            source.interleavedSamples[frame * 2 + 1] = -0.125f;
        }
        std::string error;
        if (!neuracoust::daw::writePcm16WavFile(audioPath.string(), source, error)) {
            std::cerr << "Could not write portable render source WAV: " << error << "\n";
            return 5201;
        }

        auto project = neuracoust::daw::defaultProject();
        project.sampleRate = 100.0;
        project.defaultBufferSize = 16;
        project.monitorInputTrimDb = 0.0f;
        project.monitorVolumeDb = 0.0f;
        project.clips.push_back({"portable-render-clip", "Audio 1", audioPath.string(), 0.0, 160.0, 0.0, 0.0f});
        const auto serialized = neuracoust::daw::serializeProjectForPath(project, projectPath);
        if (serialized.find("Audio Files/take.wav") == std::string::npos) {
            std::cerr << "Portable render fixture was not saved with a project-relative media path\n";
            return 5202;
        }

        neuracoust::daw::ProjectDocument roundTrip;
        if (!neuracoust::daw::deserializeProjectForPath(serialized, projectPath, roundTrip, error)) {
            std::cerr << "Could not deserialize portable render project: " << error << "\n";
            return 5203;
        }
        if (roundTrip.clips.empty() ||
            roundTrip.clips.front().sourcePath != audioPath.lexically_normal().generic_string()) {
            std::cerr << "Portable render media path was not resolved before playlist rebuild\n";
            return 5204;
        }

        neuracoust::daw::NeuracoustDspEngine engine;
        neuracoust::daw::AudioEngineSettings settings;
        settings.sampleRate = 100.0;
        settings.bufferSize = 16;
        settings.monitorDspEnabled = false;
        if (!engine.configure(settings, settings.bufferSize, error) ||
            !engine.loadProject(roundTrip, error)) {
            std::cerr << "Could not load portable render project into engine: " << error << "\n";
            return 5205;
        }
        std::vector<float> block;
        engine.seek(150.0);
        engine.renderInterleavedStereo(16, block);
        const auto status = engine.statusSnapshot();
        const auto blockPeakIt = std::max_element(block.begin(), block.end(), [](float a, float b) {
            return std::abs(a) < std::abs(b);
        });
        const float blockPeak = blockPeakIt == block.end() ? 0.0f : std::abs(*blockPeakIt);
        if (block.size() != 32 ||
            blockPeak < 0.05f ||
            status.outputPeakLeft < 0.05f ||
            status.trackPeakLeft.empty() ||
            status.trackPeakLeft.front() < 0.2f) {
            std::cerr << "Portable project render went silent at a later playback position\n";
            return 5206;
        }
    }

    {
        const auto loopTonePath = (smokeTempRoot / "realtime-loop-tone.wav").string();
        neuracoust::daw::WavAudioData source;
        source.channels = 1;
        source.sampleRate = 10;
        source.interleavedSamples.assign(10, 0.25f);
        std::string error;
        if (!neuracoust::daw::writePcm16WavFile(loopTonePath, source, error)) {
            std::cerr << "Could not write realtime loop tone source WAV: " << error << "\n";
            return 5210;
        }

        auto loopProject = neuracoust::daw::defaultProject();
        loopProject.sampleRate = 10.0;
        loopProject.defaultBufferSize = 8;
        loopProject.monitorInputTrimDb = 0.0f;
        loopProject.monitorVolumeDb = 0.0f;
        loopProject.loopEnabled = true;
        loopProject.loopStartSeconds = 0.2;
        loopProject.loopEndSeconds = 0.5;
        loopProject.clips.push_back({"realtime-loop-tone", "Audio 1", loopTonePath, 0.0, 1.0, 0.0, 0.0f});

        neuracoust::daw::NeuracoustDspEngine engine;
        neuracoust::daw::AudioEngineSettings settings;
        settings.sampleRate = 10.0;
        settings.bufferSize = 8;
        settings.monitorDspEnabled = false;
        if (!engine.configure(settings, settings.bufferSize, error) ||
            !engine.loadProject(loopProject, error)) {
            std::cerr << "Could not load realtime loop project into engine: " << error << "\n";
            return 5211;
        }
        engine.seek(0.4);
        std::vector<float> block;
        engine.renderInterleavedStereo(8, block);
        if (block.size() != 16) {
            std::cerr << "Realtime loop render returned the wrong block size\n";
            return 5212;
        }
        for (size_t index = 0; index < block.size(); index += 2) {
            if (std::abs(block[index]) < 0.05f || std::abs(block[index + 1]) < 0.05f) {
                std::cerr << "Realtime loop render inserted silence across the loop boundary at frame "
                          << (index / 2) << "\n";
                return 5213;
            }
        }
        const auto loopStatus = engine.statusSnapshot();
        if (loopStatus.playbackSeconds < loopProject.loopStartSeconds ||
            loopStatus.playbackSeconds >= loopProject.loopEndSeconds) {
            std::cerr << "Realtime loop playback position did not wrap into the loop range: "
                      << loopStatus.playbackSeconds << "\n";
            return 5214;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine toneEngine;
        neuracoust::daw::AudioEngineSettings toneSettings;
        toneSettings.sampleRate = 48000.0;
        toneSettings.bufferSize = 256;
        toneSettings.testToneEnabled = true;
        toneSettings.monitorDspEnabled = false;
        toneSettings.monitorStationDim = false;
        toneSettings.monitorStationMute = false;
        toneSettings.monitorInputTrimDb = 0.0f;
        toneSettings.monitorVolumeDb = 0.0f;
        std::string error;
        if (!toneEngine.configure(toneSettings, toneSettings.bufferSize, error)) {
            std::cerr << "Could not configure test tone engine: " << error << "\n";
            return 5215;
        }
        std::vector<float> toneBlock;
        toneEngine.renderInterleavedStereo(512, toneBlock);
        const auto tonePeakIt = std::max_element(toneBlock.begin(), toneBlock.end(), [](float a, float b) {
            return std::abs(a) < std::abs(b);
        });
        const float tonePeak = tonePeakIt == toneBlock.end() ? 0.0f : std::abs(*tonePeakIt);
        if (tonePeak < 0.08f) {
            std::cerr << "Test tone render was too quiet or silent: " << tonePeak << "\n";
            return 5216;
        }
        toneEngine.setTestToneEnabled(false);
        toneEngine.renderInterleavedStereo(512, toneBlock);
        const auto silentPeakIt = std::max_element(toneBlock.begin(), toneBlock.end(), [](float a, float b) {
            return std::abs(a) < std::abs(b);
        });
        const float silentPeak = silentPeakIt == toneBlock.end() ? 0.0f : std::abs(*silentPeakIt);
        if (silentPeak > 0.001f) {
            std::cerr << "Test tone did not turn off cleanly: " << silentPeak << "\n";
            return 5217;
        }
    }

    {
        auto glueProject = neuracoust::daw::defaultProject();
        glueProject.clips.clear();
        neuracoust::daw::ClipState sourceClip;
        sourceClip.id = "glue-source";
        sourceClip.trackName = "Audio 1";
        sourceClip.sourcePath = "/tmp/neuracoust-daw-glue.wav";
        sourceClip.startSeconds = 1.0;
        sourceClip.durationSeconds = 4.0;
        sourceClip.sourceOffsetSeconds = 2.0;
        sourceClip.gainDb = -1.0f;
        sourceClip.regionName = "Non destructive glue";
        sourceClip.sourceFileUid = "glue-source-uid";
        sourceClip.fadeInSeconds = 0.25;
        sourceClip.fadeOutSeconds = 0.4;
        glueProject.clips.push_back(sourceClip);

        std::string rightClipId;
        if (!neuracoust::daw::splitClip(glueProject, "glue-source", 3.0, rightClipId) ||
            glueProject.clips.size() != 2 ||
            rightClipId.empty()) {
            std::cerr << "Could not split clip for glue regression\n";
            return 38;
        }
        std::string gluedClipId;
        if (!neuracoust::daw::glueAdjacentClip(glueProject, rightClipId, gluedClipId) ||
            gluedClipId != "glue-source" ||
            glueProject.clips.size() != 1 ||
            std::abs(glueProject.clips.front().startSeconds - 1.0) > 0.0001 ||
            std::abs(glueProject.clips.front().durationSeconds - 4.0) > 0.0001 ||
            std::abs(glueProject.clips.front().sourceOffsetSeconds - 2.0) > 0.0001 ||
            std::abs(glueProject.clips.front().fadeInSeconds - 0.25) > 0.0001 ||
            std::abs(glueProject.clips.front().fadeOutSeconds - 0.4) > 0.0001) {
            std::cerr << "Glue adjacent clip did not restore the non-destructive source span\n";
            return 39;
        }

        std::string middleClipId;
        std::string tailClipId;
        if (!neuracoust::daw::splitClip(glueProject, "glue-source", 2.5, middleClipId) ||
            !neuracoust::daw::splitClip(glueProject, middleClipId, 4.0, tailClipId) ||
            glueProject.clips.size() != 3) {
            std::cerr << "Could not prepare three split clips for glue range regression\n";
            return 41;
        }
        std::vector<std::string> gluedClipIds;
        if (!neuracoust::daw::glueClipRange(glueProject, 2.5, 4.0, gluedClipIds) ||
            gluedClipIds.size() != 2 ||
            glueProject.clips.size() != 1 ||
            glueProject.clips.front().id != "glue-source" ||
            std::abs(glueProject.clips.front().startSeconds - 1.0) > 0.0001 ||
            std::abs(glueProject.clips.front().durationSeconds - 4.0) > 0.0001 ||
            std::abs(glueProject.clips.front().sourceOffsetSeconds - 2.0) > 0.0001) {
            std::cerr << "Glue edit selection did not restore the non-destructive clip span\n";
            return 42;
        }

        neuracoust::daw::ClipState mismatchA = sourceClip;
        neuracoust::daw::ClipState mismatchB = sourceClip;
        mismatchA.id = "mismatch-a";
        mismatchA.startSeconds = 0.0;
        mismatchA.durationSeconds = 1.0;
        mismatchA.sourceOffsetSeconds = 0.0;
        mismatchB.id = "mismatch-b";
        mismatchB.startSeconds = 1.0;
        mismatchB.durationSeconds = 1.0;
        mismatchB.sourceOffsetSeconds = 4.0;
        glueProject.clips = {mismatchA, mismatchB};
        if (neuracoust::daw::glueAdjacentClip(glueProject, "mismatch-a", gluedClipId) ||
            glueProject.clips.size() != 2) {
            std::cerr << "Glue incorrectly joined clips with discontinuous source offsets\n";
            return 40;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine mixEngine;
        neuracoust::daw::AudioEngineSettings mixSettings;
        mixSettings.sampleRate = 100.0;
        mixSettings.bufferSize = 16;
        mixSettings.monitorDspEnabled = false;
        mixSettings.monitorInputTrimDb = 0.0f;
        mixSettings.monitorVolumeDb = 0.0f;
        std::string error;
        if (!mixEngine.configure(mixSettings, mixSettings.bufferSize, error)) {
            std::cerr << "Could not configure mix DSP engine: " << error << "\n";
            return 15;
        }

        neuracoust::daw::WavAudioData source;
        source.channels = 1;
        source.sampleRate = 100;
        source.interleavedSamples.assign(16, 0.5f);
        const auto sourcePath = (smokeTempRoot / "neuracoust-daw-track-mix-reload.wav").string();
        if (!neuracoust::daw::writePcm16WavFile(sourcePath, source, error)) {
            std::cerr << "Could not write mix reload source WAV: " << error << "\n";
            return 16;
        }

        auto mixProject = neuracoust::daw::defaultProject();
        mixProject.monitorInputTrimDb = 0.0f;
        mixProject.monitorVolumeDb = 0.0f;
        mixProject.sampleRate = 100.0;
        mixProject.tracks[0].pan = -1.0f;
        mixProject.clips.push_back({"mix-reload-clip", "Audio 1", sourcePath, 0.0, 0.16, 0.0, 0.0f});
        if (!mixEngine.loadProject(mixProject, error)) {
            std::cerr << "Could not load mix baseline project: " << error << "\n";
            return 17;
        }
        std::vector<float> renderBlock;
        mixEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 || renderBlock[0] < 0.49f || std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Mix baseline pan did not render left-only audio\n";
            return 18;
        }
        const auto baselineMeterStatus = mixEngine.statusSnapshot();
        const auto baselineMeterIt = std::find(baselineMeterStatus.trackMeterNames.begin(),
                                               baselineMeterStatus.trackMeterNames.end(),
                                               "Audio 1");
        if (baselineMeterIt == baselineMeterStatus.trackMeterNames.end()) {
            std::cerr << "Track meter status did not expose Audio 1\n";
            return 181;
        }
        const auto baselineMeterIndex = static_cast<size_t>(std::distance(baselineMeterStatus.trackMeterNames.begin(), baselineMeterIt));
        if (baselineMeterIndex >= baselineMeterStatus.trackPeakLeft.size() ||
            baselineMeterIndex >= baselineMeterStatus.trackPeakRight.size() ||
            baselineMeterStatus.trackPeakLeft[baselineMeterIndex] < 0.49f ||
            baselineMeterStatus.trackPeakRight[baselineMeterIndex] > 0.0001f) {
            std::cerr << "Track meter status did not follow Audio 1 post-fader signal\n";
            return 182;
        }
        if (baselineMeterStatus.outputPeakLeft < 0.49f) {
            std::cerr << "Output peak did not expose the rendered master/monitor signal\n";
            return 183;
        }
        if (baselineMeterStatus.activeRealtimeVst3MasterInsertCount != 0 ||
            baselineMeterStatus.activeOfflineVst3TrackInsertCount != 0) {
            std::cerr << "Clean realtime project unexpectedly reported active VST3 inserts\n";
            return 185;
        }
        auto trackInsertStatusProject = mixProject;
        trackInsertStatusProject.tracks[0].inserts.push_back({
            "Realtime Status Track VST3",
            "VST3",
            "/Library/Audio/Plug-Ins/VST3/Realtime Status Track.vst3",
            false,
            true
        });
        if (!mixEngine.loadProject(trackInsertStatusProject, error)) {
            std::cerr << "Could not load track insert status project: " << error << "\n";
            return 186;
        }
        const auto trackInsertStatus = mixEngine.statusSnapshot();
        if (trackInsertStatus.activeRealtimeVst3MasterInsertCount != 0 ||
            trackInsertStatus.activeOfflineVst3TrackInsertCount != 1) {
            std::cerr << "Realtime DSP status did not separate master realtime and track bounce VST3 counts\n";
            return 187;
        }
        mixEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 || renderBlock[0] < 0.49f || std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Realtime DSP render should keep dry playback when a track VST3 cannot be prepared\n";
            return 189;
        }
        const auto validTrackVst3Path = std::filesystem::path("/Library/Audio/Plug-Ins/VST3/Newacoust4001E.vst3");
        if (std::filesystem::exists(validTrackVst3Path)) {
            neuracoust::daw::NeuracoustDspEngine validTrackInsertEngine;
            neuracoust::daw::AudioEngineSettings validSettings;
            validSettings.sampleRate = 48000.0;
            validSettings.bufferSize = 128;
            validSettings.monitorDspEnabled = false;
            validSettings.monitorInputTrimDb = 0.0f;
            validSettings.monitorVolumeDb = 0.0f;
            if (!validTrackInsertEngine.configure(validSettings, validSettings.bufferSize, error)) {
                std::cerr << "Could not configure valid realtime track VST3 engine: " << error << "\n";
                return 190;
            }
            neuracoust::daw::WavAudioData validSource;
            validSource.channels = 1;
            validSource.sampleRate = 48000;
            validSource.interleavedSamples.assign(8192, 0.05f);
            const auto validSourcePath = (smokeTempRoot / "neuracoust-daw-valid-track-vst3.wav").string();
            if (!neuracoust::daw::writePcm16WavFile(validSourcePath, validSource, error)) {
                std::cerr << "Could not write valid realtime track VST3 source WAV: " << error << "\n";
                return 191;
            }
            auto validTrackInsertProject = neuracoust::daw::defaultProject();
            validTrackInsertProject.sampleRate = 48000.0;
            validTrackInsertProject.monitorInputTrimDb = 0.0f;
            validTrackInsertProject.monitorVolumeDb = 0.0f;
            validTrackInsertProject.clips.push_back({"valid-track-vst3-clip", "Audio 1", validSourcePath, 0.0, 0.170, 0.0, 0.0f});
            validTrackInsertProject.tracks[0].inserts.push_back({
                "Newacoust4001E",
                "VST3",
                validTrackVst3Path.string(),
                false,
                true
            });
            if (!validTrackInsertEngine.loadProject(validTrackInsertProject, error)) {
                std::cerr << "Could not load valid realtime track VST3 project: " << error << "\n";
                return 192;
            }
            const auto validTrackInsertStatus = validTrackInsertEngine.statusSnapshot();
            if (validTrackInsertStatus.activeRealtimeVst3TrackInsertCount < 1 ||
                validTrackInsertStatus.activeOfflineVst3TrackInsertCount < 1) {
                std::cerr << "Valid track VST3 insert was not prepared for realtime playback: "
                          << validTrackInsertStatus.message << "\n";
                return 193;
            }
            validTrackInsertEngine.renderInterleavedStereo(16, renderBlock);
            if (renderBlock.size() != 32) {
                std::cerr << "Valid realtime track VST3 render returned the wrong block size\n";
                return 194;
            }
            neuracoust::daw::WavAudioData expectedTrackInsertInput;
            expectedTrackInsertInput.channels = 2;
            expectedTrackInsertInput.sampleRate = 48000;
            expectedTrackInsertInput.interleavedSamples.assign(16 * 2, 0.05f);
            auto expectedDescriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert("Newacoust4001E", validTrackVst3Path.string());
            const auto expectedTrackInsertProcess = neuracoust::daw::processStereoBufferWithVst3(expectedDescriptor,
                                                                                                 expectedTrackInsertInput,
                                                                                                 128);
            if (!expectedTrackInsertProcess.processed || expectedTrackInsertInput.interleavedSamples.size() < renderBlock.size()) {
                std::cerr << "Could not produce an offline reference for realtime track VST3 processing: "
                          << expectedTrackInsertProcess.message << "\n";
                return 195;
            }
            double realtimeTrackInsertError = 0.0;
            double realtimeTrackInsertEnergy = 0.0;
            double dryDifference = 0.0;
            for (size_t i = 0; i < renderBlock.size(); ++i) {
                realtimeTrackInsertError = std::max(realtimeTrackInsertError,
                                                    static_cast<double>(std::abs(renderBlock[i] - expectedTrackInsertInput.interleavedSamples[i])));
                realtimeTrackInsertEnergy += std::abs(renderBlock[i]);
                dryDifference += std::abs(renderBlock[i] - 0.05f);
            }
            if (realtimeTrackInsertEnergy <= 0.0 || realtimeTrackInsertError > 0.04) {
                std::cerr << "Realtime track VST3 render did not match the offline VST3 signal path; max error "
                          << realtimeTrackInsertError << "\n";
                return 196;
            }
            if (dryDifference <= 0.0001) {
                std::cerr << "Realtime track VST3 render appears to be dry passthrough instead of processed audio\n";
                return 197;
            }

            const bool scanInstalledVst3 = std::getenv("NEURACOUST_CORE_SMOKE_SCAN_INSTALLED_VST3") != nullptr;
            const auto wavesShellPath = std::filesystem::path("/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.0.vst3");
            if (scanInstalledVst3 && std::filesystem::exists(wavesShellPath)) {
                const auto wavesGeneratorDescriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
                    "EMO-Generator",
                    wavesShellPath.string(),
                    "56535453494753656d6f2d67656e6572",
                    "EMO-Generator Stereo");
                auto generatorProject = neuracoust::daw::defaultProject();
                generatorProject.sampleRate = 44100.0;
                generatorProject.defaultBufferSize = 256;
                generatorProject.monitorInputTrimDb = 0.0f;
                generatorProject.monitorVolumeDb = 0.0f;
                generatorProject.monitorModules.clear();
                generatorProject.clips.clear();
                generatorProject.tracks[0].inputMonitoring = true;
                neuracoust::daw::TrackInsertSlot generatorInsert;
                generatorInsert.pluginName = "EMO-Generator";
                generatorInsert.pluginFormat = "VST3";
                generatorInsert.pluginPath = wavesGeneratorDescriptor.bundlePath;
                generatorInsert.pluginClassId = wavesGeneratorDescriptor.componentClassCid;
                generatorInsert.pluginClassName = wavesGeneratorDescriptor.componentClassName;
                generatorInsert.dspExecutionMode = "native";
                generatorInsert.enabled = true;
                generatorInsert.bypassed = false;
                generatorInsert.parameters.push_back({0u, "On Off", 1.0});
                generatorProject.tracks[0].inserts.push_back(generatorInsert);

                neuracoust::daw::AudioEngineSettings generatorSettings;
                generatorSettings.sampleRate = 44100.0;
                generatorSettings.bufferSize = 256;
                generatorSettings.monitorDspEnabled = false;
                generatorSettings.monitorInputTrimDb = 0.0f;
                generatorSettings.monitorVolumeDb = 0.0f;
                neuracoust::daw::NeuracoustDspEngine generatorEngine;
                if (!generatorEngine.configure(generatorSettings, generatorSettings.bufferSize, error) ||
                    !generatorEngine.loadProject(generatorProject, error)) {
                    std::cerr << "Could not load source-generator insert project: " << error << "\n";
                    return 330;
                }
                std::vector<float> generatorRender;
                generatorEngine.renderInterleavedStereo(44100 / 2, generatorRender);
                const double generatorRmsDb = stereoRmsDb(generatorRender);
                if (generatorRmsDb < -80.0) {
                    std::cerr << "Waves EMO-Generator insert stayed silent on input-monitored source-less track: "
                              << generatorRmsDb << " dBFS\n";
                    return 331;
                }
                auto stoppedGeneratorProject = generatorProject;
                neuracoust::daw::AudioEngineSettings stoppedGeneratorSettings = generatorSettings;
                stoppedGeneratorSettings.transportRunning = false;
                neuracoust::daw::NeuracoustDspEngine stoppedGeneratorEngine;
                if (!stoppedGeneratorEngine.configure(stoppedGeneratorSettings, stoppedGeneratorSettings.bufferSize, error) ||
                    !stoppedGeneratorEngine.loadProject(stoppedGeneratorProject, error)) {
                    std::cerr << "Could not load stopped-transport source-generator project: " << error << "\n";
                    return 332;
                }
                std::vector<float> stoppedGeneratorRender;
                stoppedGeneratorEngine.renderInterleavedStereo(44100 / 2, stoppedGeneratorRender);
                const double stoppedGeneratorRmsDb = stereoRmsDb(stoppedGeneratorRender);
                const auto stoppedGeneratorStatus = stoppedGeneratorEngine.statusSnapshot();
                if (stoppedGeneratorRmsDb < -80.0 || stoppedGeneratorStatus.playbackSeconds > 0.001) {
                    std::cerr << "Stopped transport did not keep live source-generator DSP running independently of playhead: rms "
                              << stoppedGeneratorRmsDb << " dBFS, playhead "
                              << stoppedGeneratorStatus.playbackSeconds << "s\n";
                    return 333;
                }
                auto sourceLessGeneratorProject = generatorProject;
                sourceLessGeneratorProject.tracks[0].inputMonitoring = false;
                neuracoust::daw::NeuracoustDspEngine sourceLessGeneratorEngine;
                if (!sourceLessGeneratorEngine.configure(stoppedGeneratorSettings, stoppedGeneratorSettings.bufferSize, error) ||
                    !sourceLessGeneratorEngine.loadProject(sourceLessGeneratorProject, error)) {
                    std::cerr << "Could not load stopped source-less generator project: " << error << "\n";
                    return 334;
                }
                std::vector<float> sourceLessGeneratorRender;
                sourceLessGeneratorEngine.renderInterleavedStereo(44100 / 2, sourceLessGeneratorRender);
                const double sourceLessGeneratorRmsDb = stereoRmsDb(sourceLessGeneratorRender);
                if (sourceLessGeneratorRmsDb < -80.0) {
                    std::cerr << "Source-less stopped generator insert stayed silent without input monitoring: "
                              << sourceLessGeneratorRmsDb << " dBFS\n";
                    return 335;
                }

                auto stoppedGeneratorWithMonitorDspProject = sourceLessGeneratorProject;
                stoppedGeneratorWithMonitorDspProject.monitorModules = neuracoust::daw::defaultMonitorDspModules();
                neuracoust::daw::AudioEngineSettings stoppedGeneratorWithMonitorDspSettings = stoppedGeneratorSettings;
                stoppedGeneratorWithMonitorDspSettings.monitorDspEnabled = true;
                stoppedGeneratorWithMonitorDspSettings.monitorDspPathMode = "internal";
                stoppedGeneratorWithMonitorDspSettings.monitorModules = stoppedGeneratorWithMonitorDspProject.monitorModules;
                neuracoust::daw::NeuracoustDspEngine stoppedGeneratorWithMonitorDspEngine;
                if (!stoppedGeneratorWithMonitorDspEngine.configure(stoppedGeneratorWithMonitorDspSettings,
                                                                    stoppedGeneratorWithMonitorDspSettings.bufferSize,
                                                                    error) ||
                    !stoppedGeneratorWithMonitorDspEngine.loadProject(stoppedGeneratorWithMonitorDspProject, error)) {
                    std::cerr << "Could not load stopped source-less generator project with monitor DSP enabled: "
                              << error << "\n";
                    return 336;
                }
                std::vector<float> stoppedGeneratorWithMonitorDspRender;
                stoppedGeneratorWithMonitorDspEngine.renderInterleavedStereo(44100 / 2, stoppedGeneratorWithMonitorDspRender);
                const double stoppedGeneratorWithMonitorDspRmsDb = stereoRmsDb(stoppedGeneratorWithMonitorDspRender);
                const auto stoppedGeneratorWithMonitorDspStatus = stoppedGeneratorWithMonitorDspEngine.statusSnapshot();
                if (stoppedGeneratorWithMonitorDspRmsDb < -80.0 ||
                    stoppedGeneratorWithMonitorDspStatus.activeRealtimeVst3TrackInsertCount <= 0 ||
                    stoppedGeneratorWithMonitorDspStatus.outputPeakLeft <= 0.0f ||
                    stoppedGeneratorWithMonitorDspStatus.outputPeakRight <= 0.0f) {
                    std::cerr << "Stopped source-less generator insert stayed silent with internal monitor DSP enabled: rms "
                              << stoppedGeneratorWithMonitorDspRmsDb
                              << " dBFS, track inserts "
                              << stoppedGeneratorWithMonitorDspStatus.activeRealtimeVst3TrackInsertCount
                              << ", peaks "
                              << stoppedGeneratorWithMonitorDspStatus.outputPeakLeft
                              << "/"
                              << stoppedGeneratorWithMonitorDspStatus.outputPeakRight
                              << "\n";
                    return 337;
                }

                auto legacyGeneratorProject = stoppedGeneratorWithMonitorDspProject;
                legacyGeneratorProject.tracks[0].inserts[0].parameters.clear();
                neuracoust::daw::NeuracoustDspEngine legacyGeneratorEngine;
                if (!legacyGeneratorEngine.configure(stoppedGeneratorWithMonitorDspSettings,
                                                     stoppedGeneratorWithMonitorDspSettings.bufferSize,
                                                     error) ||
                    !legacyGeneratorEngine.loadProject(legacyGeneratorProject, error)) {
                    std::cerr << "Could not load legacy source-less generator project without stored ON parameter: "
                              << error << "\n";
                    return 338;
                }
                std::vector<float> legacyGeneratorRender;
                legacyGeneratorEngine.renderInterleavedStereo(44100 / 2, legacyGeneratorRender);
                const double legacyGeneratorRmsDb = stereoRmsDb(legacyGeneratorRender);
                const auto legacyGeneratorStatus = legacyGeneratorEngine.statusSnapshot();
                if (legacyGeneratorRmsDb < -80.0 ||
                    legacyGeneratorStatus.activeRealtimeVst3TrackInsertCount <= 0 ||
                    legacyGeneratorStatus.outputPeakLeft <= 0.0f ||
                    legacyGeneratorStatus.outputPeakRight <= 0.0f) {
                    std::cerr << "Legacy source-less generator insert without stored ON parameter stayed silent: rms "
                              << legacyGeneratorRmsDb
                              << " dBFS, track inserts "
                              << legacyGeneratorStatus.activeRealtimeVst3TrackInsertCount
                              << ", peaks "
                              << legacyGeneratorStatus.outputPeakLeft
                              << "/"
                              << legacyGeneratorStatus.outputPeakRight
                              << "\n";
                    return 339;
                }

                auto legacyGenerator48kProject = legacyGeneratorProject;
                legacyGenerator48kProject.sampleRate = 48000.0;
                legacyGenerator48kProject.defaultBufferSize = 256;
                neuracoust::daw::AudioEngineSettings legacyGenerator48kSettings = stoppedGeneratorWithMonitorDspSettings;
                legacyGenerator48kSettings.sampleRate = 48000.0;
                legacyGenerator48kSettings.bufferSize = 256;
                neuracoust::daw::NeuracoustDspEngine legacyGenerator48kEngine;
                if (!legacyGenerator48kEngine.configure(legacyGenerator48kSettings,
                                                        legacyGenerator48kSettings.bufferSize,
                                                        error) ||
                    !legacyGenerator48kEngine.loadProject(legacyGenerator48kProject, error)) {
                    std::cerr << "Could not load 48 kHz legacy source-less generator project without stored ON parameter: "
                              << error << "\n";
                    return 340;
                }
                std::vector<float> legacyGenerator48kRender;
                legacyGenerator48kEngine.renderInterleavedStereo(48000 / 2, legacyGenerator48kRender);
                const double legacyGenerator48kRmsDb = stereoRmsDb(legacyGenerator48kRender);
                const auto legacyGenerator48kStatus = legacyGenerator48kEngine.statusSnapshot();
                if (legacyGenerator48kRmsDb < -80.0 ||
                    legacyGenerator48kStatus.activeRealtimeVst3TrackInsertCount <= 0 ||
                    legacyGenerator48kStatus.outputPeakLeft <= 0.0f ||
                    legacyGenerator48kStatus.outputPeakRight <= 0.0f) {
                    std::cerr << "48 kHz legacy source-less generator insert without stored ON parameter stayed silent: rms "
                              << legacyGenerator48kRmsDb
                              << " dBFS, track inserts "
                              << legacyGenerator48kStatus.activeRealtimeVst3TrackInsertCount
                              << ", peaks "
                              << legacyGenerator48kStatus.outputPeakLeft
                              << "/"
                              << legacyGenerator48kStatus.outputPeakRight
                              << "\n";
                    return 341;
                }

                auto offStateGeneratorProject = legacyGenerator48kProject;
                offStateGeneratorProject.tracks[0].inserts[0].parameters = {{0u, "On Off", 0.0}};
                neuracoust::daw::NeuracoustDspEngine offStateGeneratorEngine;
                if (!offStateGeneratorEngine.configure(legacyGenerator48kSettings,
                                                       legacyGenerator48kSettings.bufferSize,
                                                       error) ||
                    !offStateGeneratorEngine.loadProject(offStateGeneratorProject, error)) {
                    std::cerr << "Could not load 48 kHz source-less generator project with stale OFF parameter: "
                              << error << "\n";
                    return 342;
                }
                std::vector<float> offStateGeneratorRender;
                offStateGeneratorEngine.renderInterleavedStereo(48000 / 2, offStateGeneratorRender);
                const double offStateGeneratorRmsDb = stereoRmsDb(offStateGeneratorRender);
                const auto offStateGeneratorStatus = offStateGeneratorEngine.statusSnapshot();
                if (offStateGeneratorRmsDb < -80.0 ||
                    offStateGeneratorStatus.activeRealtimeVst3TrackInsertCount <= 0 ||
                    offStateGeneratorStatus.outputPeakLeft <= 0.0f ||
                    offStateGeneratorStatus.outputPeakRight <= 0.0f) {
                    std::cerr << "48 kHz source-less generator insert with stale OFF parameter stayed silent: rms "
                              << offStateGeneratorRmsDb
                              << " dBFS, track inserts "
                              << offStateGeneratorStatus.activeRealtimeVst3TrackInsertCount
                              << ", peaks "
                              << offStateGeneratorStatus.outputPeakLeft
                              << "/"
                              << offStateGeneratorStatus.outputPeakRight
                              << "\n";
                    return 343;
                }

                auto liveInsertBaseProject = neuracoust::daw::defaultProject();
                liveInsertBaseProject.sampleRate = 48000.0;
                liveInsertBaseProject.defaultBufferSize = 256;
                liveInsertBaseProject.monitorInputTrimDb = 0.0f;
                liveInsertBaseProject.monitorVolumeDb = 0.0f;
                liveInsertBaseProject.monitorModules = neuracoust::daw::defaultMonitorDspModules();
                liveInsertBaseProject.clips.clear();
                liveInsertBaseProject.tracks[0].inputMonitoring = false;
                neuracoust::daw::AudioEngineSettings liveInsertSettings;
                liveInsertSettings.sampleRate = 48000.0;
                liveInsertSettings.bufferSize = 256;
                liveInsertSettings.transportRunning = false;
                liveInsertSettings.monitorDspEnabled = true;
                liveInsertSettings.monitorDspPathMode = "internal";
                liveInsertSettings.monitorInputTrimDb = 0.0f;
                liveInsertSettings.monitorVolumeDb = 0.0f;
                liveInsertSettings.monitorModules = liveInsertBaseProject.monitorModules;
                neuracoust::daw::NeuracoustDspEngine liveInsertEngine;
                if (!liveInsertEngine.configure(liveInsertSettings, liveInsertSettings.bufferSize, error) ||
                    !liveInsertEngine.loadProject(liveInsertBaseProject, error)) {
                    std::cerr << "Could not load live update base project for source-less generator: "
                              << error << "\n";
                    return 344;
                }
                std::vector<float> liveInsertSilentRender;
                liveInsertEngine.renderInterleavedStereo(48000 / 10, liveInsertSilentRender);
                if (stereoRmsDb(liveInsertSilentRender) > -80.0) {
                    std::cerr << "Live update base project was not silent before inserting generator: "
                              << stereoRmsDb(liveInsertSilentRender) << " dBFS\n";
                    return 345;
                }
                auto liveInsertProject = liveInsertBaseProject;
                neuracoust::daw::TrackInsertSlot liveGeneratorInsert;
                liveGeneratorInsert.pluginName = "EMO-Generator Stereo";
                liveGeneratorInsert.pluginFormat = "VST3";
                liveGeneratorInsert.pluginPath = wavesGeneratorDescriptor.bundlePath;
                liveGeneratorInsert.pluginClassId = wavesGeneratorDescriptor.componentClassCid;
                liveGeneratorInsert.pluginClassName = wavesGeneratorDescriptor.componentClassName;
                liveGeneratorInsert.dspExecutionMode = "internal";
                liveGeneratorInsert.enabled = true;
                liveGeneratorInsert.bypassed = false;
                liveGeneratorInsert.parameters.push_back({0u, "On Off", 0.0});
                liveInsertProject.tracks[0].inserts.push_back(liveGeneratorInsert);
                if (!liveInsertEngine.updateProject(liveInsertProject, error)) {
                    std::cerr << "Could not live-update source-less generator insert project: "
                              << error << "\n";
                    return 346;
                }
                std::vector<float> liveInsertGeneratorRender;
                liveInsertEngine.renderInterleavedStereo(48000 / 2, liveInsertGeneratorRender);
                liveInsertEngine.renderInterleavedStereo(48000 / 10, liveInsertGeneratorRender);
                const double liveInsertGeneratorRmsDb = stereoRmsDb(liveInsertGeneratorRender);
                const auto liveInsertGeneratorStatus = liveInsertEngine.statusSnapshot();
                bool sawLiveGeneratorMeter = false;
                for (size_t meterIndex = 0;
                     meterIndex < liveInsertGeneratorStatus.trackInsertMeterTrackNames.size();
                     ++meterIndex) {
                    if (meterIndex >= liveInsertGeneratorStatus.trackInsertMeterSlotIndices.size() ||
                        meterIndex >= liveInsertGeneratorStatus.trackInsertOutputPeak.size()) {
                        continue;
                    }
                    if (liveInsertGeneratorStatus.trackInsertMeterTrackNames[meterIndex] == "Audio 1" &&
                        liveInsertGeneratorStatus.trackInsertMeterSlotIndices[meterIndex] == 0 &&
                        liveInsertGeneratorStatus.trackInsertOutputPeak[meterIndex] > 0.0f) {
                        sawLiveGeneratorMeter = true;
                        break;
                    }
                }
                if (liveInsertGeneratorRmsDb < -80.0 ||
                    liveInsertGeneratorStatus.outputPeakLeft <= 0.0f ||
                    liveInsertGeneratorStatus.outputPeakRight <= 0.0f ||
                    !sawLiveGeneratorMeter) {
                    std::cerr << "Live update source-less internal generator stayed silent: rms "
                              << liveInsertGeneratorRmsDb
                              << " dBFS, output peaks "
                              << liveInsertGeneratorStatus.outputPeakLeft
                              << "/"
                              << liveInsertGeneratorStatus.outputPeakRight
                              << ", insert meters "
                              << liveInsertGeneratorStatus.trackInsertMeterTrackNames.size()
                              << "\n";
                    return 347;
                }

                auto liveNativeProject = liveInsertBaseProject;
                auto liveNativeInsert = liveGeneratorInsert;
                liveNativeInsert.dspExecutionMode = "native";
                liveNativeProject.tracks[0].inserts.push_back(liveNativeInsert);
                neuracoust::daw::NeuracoustDspEngine liveNativeEngine;
                if (!liveNativeEngine.configure(liveInsertSettings, liveInsertSettings.bufferSize, error) ||
                    !liveNativeEngine.loadProject(liveInsertBaseProject, error) ||
                    !liveNativeEngine.updateProject(liveNativeProject, error)) {
                    std::cerr << "Could not live-update native source-less generator insert project: "
                              << error << "\n";
                    return 348;
                }
                std::vector<float> liveNativeRender;
                liveNativeEngine.renderInterleavedStereo(48000 / 2, liveNativeRender);
                liveNativeEngine.renderInterleavedStereo(48000 / 10, liveNativeRender);
                const double liveNativeRmsDb = stereoRmsDb(liveNativeRender);
                const auto liveNativeStatus = liveNativeEngine.statusSnapshot();
                bool sawLiveNativeMeter = false;
                for (size_t meterIndex = 0; meterIndex < liveNativeStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
                    if (meterIndex < liveNativeStatus.trackInsertMeterSlotIndices.size() &&
                        meterIndex < liveNativeStatus.trackInsertOutputPeak.size() &&
                        liveNativeStatus.trackInsertMeterTrackNames[meterIndex] == "Audio 1" &&
                        liveNativeStatus.trackInsertMeterSlotIndices[meterIndex] == 0 &&
                        liveNativeStatus.trackInsertOutputPeak[meterIndex] > 0.0f) {
                        sawLiveNativeMeter = true;
                        break;
                    }
                }
                if (liveNativeRmsDb < -80.0 ||
                    liveNativeStatus.outputPeakLeft <= 0.0f ||
                    liveNativeStatus.outputPeakRight <= 0.0f ||
                    !sawLiveNativeMeter) {
                    std::cerr << "Live update source-less native generator stayed silent: rms "
                              << liveNativeRmsDb
                              << " dBFS, output peaks "
                              << liveNativeStatus.outputPeakLeft
                              << "/"
                              << liveNativeStatus.outputPeakRight
                              << ", insert meters "
                              << liveNativeStatus.trackInsertMeterTrackNames.size()
                              << "\n";
                    return 349;
                }

                auto liveStereoAliasProject = liveInsertBaseProject;
                liveStereoAliasProject.tracks[0].trackType = "stereo";
                liveStereoAliasProject.tracks[0].channelFormat = "stereo";
                liveStereoAliasProject.tracks[0].inserts.push_back(liveNativeInsert);
                neuracoust::daw::NeuracoustDspEngine liveStereoAliasEngine;
                if (!liveStereoAliasEngine.configure(liveInsertSettings, liveInsertSettings.bufferSize, error) ||
                    !liveStereoAliasEngine.loadProject(liveInsertBaseProject, error) ||
                    !liveStereoAliasEngine.updateProject(liveStereoAliasProject, error)) {
                    std::cerr << "Could not live-update stereo-alias source-less generator insert project: "
                              << error << "\n";
                    return 361;
                }
                std::vector<float> liveStereoAliasRender;
                liveStereoAliasEngine.renderInterleavedStereo(48000 / 2, liveStereoAliasRender);
                liveStereoAliasEngine.renderInterleavedStereo(48000 / 10, liveStereoAliasRender);
                const double liveStereoAliasRmsDb = stereoRmsDb(liveStereoAliasRender);
                const auto liveStereoAliasStatus = liveStereoAliasEngine.statusSnapshot();
                bool sawLiveStereoAliasMeter = false;
                for (size_t meterIndex = 0; meterIndex < liveStereoAliasStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
                    if (meterIndex < liveStereoAliasStatus.trackInsertMeterSlotIndices.size() &&
                        meterIndex < liveStereoAliasStatus.trackInsertOutputPeak.size() &&
                        liveStereoAliasStatus.trackInsertMeterTrackNames[meterIndex] == "Audio 1" &&
                        liveStereoAliasStatus.trackInsertMeterSlotIndices[meterIndex] == 0 &&
                        liveStereoAliasStatus.trackInsertOutputPeak[meterIndex] > 0.0f) {
                        sawLiveStereoAliasMeter = true;
                        break;
                    }
                }
                if (liveStereoAliasRmsDb < -80.0 ||
                    liveStereoAliasStatus.outputPeakLeft <= 0.0f ||
                    liveStereoAliasStatus.outputPeakRight <= 0.0f ||
                    !sawLiveStereoAliasMeter) {
                    std::cerr << "Stereo track-type alias source-less native generator stayed silent: rms "
                              << liveStereoAliasRmsDb
                              << " dBFS, output peaks "
                              << liveStereoAliasStatus.outputPeakLeft
                              << "/"
                              << liveStereoAliasStatus.outputPeakRight
                              << ", insert meters "
                              << liveStereoAliasStatus.trackInsertMeterTrackNames.size()
                              << "\n";
                    return 362;
                }

                auto staleRemoteProject = liveInsertBaseProject;
                auto staleRemoteInsert = liveGeneratorInsert;
                staleRemoteInsert.dspExecutionMode = "external";
                staleRemoteInsert.assignedDspServerId = "stale-remote";
                staleRemoteInsert.serverModuleId.clear();
                staleRemoteInsert.dspAvailable = false;
                staleRemoteInsert.dspLastError = "stale remote mode";
                staleRemoteProject.tracks[0].inserts.push_back(staleRemoteInsert);
                neuracoust::daw::NeuracoustDspEngine staleRemoteEngine;
                if (!staleRemoteEngine.configure(liveInsertSettings, liveInsertSettings.bufferSize, error) ||
                    !staleRemoteEngine.loadProject(liveInsertBaseProject, error) ||
                    !staleRemoteEngine.updateProject(staleRemoteProject, error)) {
                    std::cerr << "Could not live-update stale remote source-less generator insert project: "
                              << error << "\n";
                    return 350;
                }
                std::vector<float> staleRemoteRender;
                staleRemoteEngine.renderInterleavedStereo(48000 / 2, staleRemoteRender);
                staleRemoteEngine.renderInterleavedStereo(48000 / 10, staleRemoteRender);
                const double staleRemoteRmsDb = stereoRmsDb(staleRemoteRender);
                const auto staleRemoteStatus = staleRemoteEngine.statusSnapshot();
                bool sawStaleRemoteMeter = false;
                for (size_t meterIndex = 0; meterIndex < staleRemoteStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
                    if (meterIndex < staleRemoteStatus.trackInsertMeterSlotIndices.size() &&
                        meterIndex < staleRemoteStatus.trackInsertOutputPeak.size() &&
                        staleRemoteStatus.trackInsertMeterTrackNames[meterIndex] == "Audio 1" &&
                        staleRemoteStatus.trackInsertMeterSlotIndices[meterIndex] == 0 &&
                        staleRemoteStatus.trackInsertOutputPeak[meterIndex] > 0.0f) {
                        sawStaleRemoteMeter = true;
                        break;
                    }
                }
                if (staleRemoteRmsDb < -80.0 ||
                    staleRemoteStatus.outputPeakLeft <= 0.0f ||
                    staleRemoteStatus.outputPeakRight <= 0.0f ||
                    !sawStaleRemoteMeter) {
                    std::cerr << "Stale remote third-party generator did not fall back to local native processing: rms "
                              << staleRemoteRmsDb
                              << " dBFS, output peaks "
                              << staleRemoteStatus.outputPeakLeft
                              << "/"
                              << staleRemoteStatus.outputPeakRight
                              << ", insert meters "
                              << staleRemoteStatus.trackInsertMeterTrackNames.size()
                              << "\n";
                    return 351;
                }

                const auto wavesL2Descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
                    "L2",
                    wavesShellPath.string(),
                    "5653544c324d536c322073746572656f",
                    "L2 Stereo");
                const auto wavesL2Parameters = neuracoust::daw::inspectVst3ParametersWithSdk(wavesL2Descriptor, 512);
	                auto thresholdParameter = std::find_if(wavesL2Parameters.parameters.begin(), wavesL2Parameters.parameters.end(), [](const auto& parameter) {
	                    std::string name = parameter.title + " " + parameter.units;
	                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
	                        return static_cast<char>(std::tolower(ch));
	                    });
	                    return name.find("thresh") != std::string::npos;
	                });
	                auto ceilingParameter = std::find_if(wavesL2Parameters.parameters.begin(), wavesL2Parameters.parameters.end(), [](const auto& parameter) {
	                    std::string name = parameter.title + " " + parameter.units;
	                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
	                        return static_cast<char>(std::tolower(ch));
	                    });
	                    return name.find("ceiling") != std::string::npos;
	                });
	                if (thresholdParameter == wavesL2Parameters.parameters.end()) {
	                    std::cerr << "Could not locate Waves L2 threshold parameter for realtime project render test\n";
	                    return 301;
	                }
	                if (ceilingParameter == wavesL2Parameters.parameters.end()) {
	                    std::cerr << "Could not locate Waves L2 output ceiling parameter for realtime project render test\n";
	                    return 329;
	                }

                neuracoust::daw::WavAudioData l2Source;
                l2Source.channels = 2;
                l2Source.sampleRate = 44100;
                l2Source.interleavedSamples.resize(44100 * 2);
                constexpr double kPi = 3.141592653589793238462643383279502884;
                for (size_t frame = 0; frame < 44100; ++frame) {
                    const float sample = static_cast<float>(std::sin((2.0 * kPi * 1000.0 * static_cast<double>(frame)) / 44100.0) * 0.125);
                    l2Source.interleavedSamples[frame * 2] = sample;
                    l2Source.interleavedSamples[frame * 2 + 1] = sample;
                }
                const auto l2SourcePath = (smokeTempRoot / "waves-l2-realtime-project-source.wav").string();
                if (!neuracoust::daw::writePcm16WavFile(l2SourcePath, l2Source, error)) {
                    std::cerr << "Could not write Waves L2 realtime project source WAV: " << error << "\n";
                    return 302;
                }

                auto dryL2Project = neuracoust::daw::defaultProject();
                dryL2Project.sampleRate = 44100.0;
                dryL2Project.monitorInputTrimDb = 0.0f;
                dryL2Project.monitorVolumeDb = 0.0f;
                dryL2Project.monitorModules.clear();
                dryL2Project.clips.clear();
                dryL2Project.clips.push_back({"waves-l2-source", "Audio 1", l2SourcePath, 0.0, 1.0, 0.0, 0.0f});

                auto wetL2Project = dryL2Project;
                neuracoust::daw::TrackInsertSlot l2Insert;
                l2Insert.pluginName = "L2";
                l2Insert.pluginFormat = "VST3";
                l2Insert.pluginPath = wavesL2Descriptor.bundlePath;
                l2Insert.pluginClassId = wavesL2Descriptor.componentClassCid;
                l2Insert.pluginClassName = wavesL2Descriptor.componentClassName;
                l2Insert.dspExecutionMode = "native";
                l2Insert.enabled = true;
                l2Insert.bypassed = false;
                l2Insert.parameters.push_back({
                    thresholdParameter->id,
                    thresholdParameter->title.empty() ? "Thresh Slider" : thresholdParameter->title,
                    0.02
                });
                wetL2Project.tracks[0].inserts.push_back(l2Insert);

                neuracoust::daw::AudioEngineSettings l2Settings;
                l2Settings.sampleRate = 44100.0;
                l2Settings.bufferSize = 256;
                l2Settings.monitorDspEnabled = false;
                l2Settings.monitorInputTrimDb = 0.0f;
                l2Settings.monitorVolumeDb = 0.0f;

                neuracoust::daw::NeuracoustDspEngine dryL2Engine;
                if (!dryL2Engine.configure(l2Settings, l2Settings.bufferSize, error) ||
                    !dryL2Engine.loadProject(dryL2Project, error)) {
                    std::cerr << "Could not load dry Waves L2 realtime project: " << error << "\n";
                    return 303;
                }
                std::vector<float> dryL2Render;
                dryL2Engine.renderInterleavedStereo(44100, dryL2Render);

                neuracoust::daw::NeuracoustDspEngine wetL2Engine;
                if (!wetL2Engine.configure(l2Settings, l2Settings.bufferSize, error) ||
                    !wetL2Engine.loadProject(wetL2Project, error)) {
                    std::cerr << "Could not load wet Waves L2 realtime project: " << error << "\n";
                    return 304;
                }
                std::vector<float> wetL2Render;
                wetL2Engine.renderInterleavedStereo(44100, wetL2Render);
                const double dryL2RmsDb = stereoRmsDb(dryL2Render);
                const double wetL2RmsDb = stereoRmsDb(wetL2Render);
                if (wetL2RmsDb <= dryL2RmsDb + 6.0) {
                    std::cerr << "Waves L2 track insert did not raise realtime project output enough: dry "
                              << dryL2RmsDb << " dB, wet " << wetL2RmsDb << " dB\n";
                    return 305;
                }
                if (!wetL2Engine.updateTrackInsertBypassState("Audio 1", 0, true)) {
                    std::cerr << "Could not update Waves L2 track insert bypass without reloading project\n";
                    return 306;
                }
                wetL2Engine.seek(0.0);
                std::vector<float> bypassedL2Render;
                wetL2Engine.renderInterleavedStereo(44100, bypassedL2Render);
                const double bypassedL2RmsDb = stereoRmsDb(bypassedL2Render);
                if (std::abs(bypassedL2RmsDb - dryL2RmsDb) > 1.0) {
                    std::cerr << "Waves L2 direct bypass update did not return close to dry output: dry "
                              << dryL2RmsDb << " dB, bypassed " << bypassedL2RmsDb << " dB\n";
                    return 307;
                }
                if (!wetL2Engine.updateTrackInsertBypassState("Audio 1", 0, false)) {
                    std::cerr << "Could not re-enable Waves L2 track insert without reloading project\n";
                    return 308;
                }
                wetL2Engine.seek(0.0);
                std::vector<float> reenabledL2Render;
                wetL2Engine.renderInterleavedStereo(44100, reenabledL2Render);
                const double reenabledL2RmsDb = stereoRmsDb(reenabledL2Render);
	                if (reenabledL2RmsDb <= dryL2RmsDb + 6.0) {
	                    std::cerr << "Waves L2 direct re-enable update did not restore wet output: dry "
	                              << dryL2RmsDb << " dB, re-enabled " << reenabledL2RmsDb << " dB\n";
	                    return 309;
	                }
	                if (!wetL2Engine.updateTrackVst3Parameter("Audio 1",
	                                                           0,
	                                                           ceilingParameter->id,
	                                                           ceilingParameter->title.empty() ? "Ceiling Slider" : ceilingParameter->title,
	                                                           0.0)) {
	                    std::cerr << "Could not update Waves L2 output ceiling without reloading project\n";
	                    return 329;
	                }
	                wetL2Engine.seek(0.0);
	                std::vector<float> ceilingDownL2Render;
	                wetL2Engine.renderInterleavedStereo(44100, ceilingDownL2Render);
	                const double ceilingDownL2RmsDb = stereoRmsDb(ceilingDownL2Render);
	                if (ceilingDownL2RmsDb >= reenabledL2RmsDb - 12.0) {
	                    std::cerr << "Waves L2 output ceiling realtime update did not reduce output enough: wet "
	                              << reenabledL2RmsDb << " dB, ceiling down " << ceilingDownL2RmsDb << " dB\n";
	                    return 329;
	                }

	                const auto geqDescriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
	                    "GEQ Modern",
	                    wavesShellPath.string(),
	                    "56535453485153676571206d6f646572",
	                    "GEQ Modern Stereo");
	                const auto geqParameters = neuracoust::daw::inspectVst3ParametersWithSdk(geqDescriptor, 512);
	                auto geq1kParameter = std::find_if(geqParameters.parameters.begin(), geqParameters.parameters.end(), [](const auto& parameter) {
	                    std::string name = parameter.title;
	                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
	                        return static_cast<char>(std::tolower(ch));
	                    });
	                    return name.find("left 1khz") != std::string::npos;
	                });
	                if (geq1kParameter == geqParameters.parameters.end()) {
	                    std::cerr << "Could not locate Waves GEQ Modern left 1 kHz parameter for chained insert meter test\n";
	                    return 326;
	                }
	                auto chainedWavesProject = dryL2Project;
	                neuracoust::daw::TrackInsertSlot geqInsert;
	                geqInsert.pluginName = "GEQ Modern";
	                geqInsert.pluginFormat = "VST3";
	                geqInsert.pluginPath = geqDescriptor.bundlePath;
	                geqInsert.pluginClassId = geqDescriptor.componentClassCid;
	                geqInsert.pluginClassName = geqDescriptor.componentClassName;
	                geqInsert.dspExecutionMode = "native";
	                geqInsert.enabled = true;
	                geqInsert.bypassed = false;
	                geqInsert.parameters.push_back({
	                    geq1kParameter->id,
	                    geq1kParameter->title.empty() ? "Left 1Khz" : geq1kParameter->title,
	                    1.0
	                });
	                chainedWavesProject.tracks[0].inserts.push_back(geqInsert);
	                chainedWavesProject.tracks[0].inserts.push_back(l2Insert);
	                neuracoust::daw::NeuracoustDspEngine chainedWavesEngine;
	                if (!chainedWavesEngine.configure(l2Settings, l2Settings.bufferSize, error) ||
	                    !chainedWavesEngine.loadProject(chainedWavesProject, error)) {
	                    std::cerr << "Could not load chained Waves GEQ/L2 realtime project: " << error << "\n";
	                    return 327;
	                }
	                std::vector<float> chainedWavesRender;
	                chainedWavesEngine.renderInterleavedStereo(44100, chainedWavesRender);
	                const auto chainedStatus = chainedWavesEngine.statusSnapshot();
	                float geqInputPeak = 0.0f;
	                float geqOutputPeak = 0.0f;
	                float l2InputPeak = 0.0f;
	                bool geqMeterFound = false;
	                bool l2MeterFound = false;
	                for (size_t meterIndex = 0; meterIndex < chainedStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
	                    if (meterIndex >= chainedStatus.trackInsertMeterSlotIndices.size() ||
	                        chainedStatus.trackInsertMeterTrackNames[meterIndex] != "Audio 1") {
	                        continue;
	                    }
	                    const float inputPeak = meterIndex < chainedStatus.trackInsertInputPeak.size()
	                        ? chainedStatus.trackInsertInputPeak[meterIndex]
	                        : 0.0f;
	                    const float outputPeak = meterIndex < chainedStatus.trackInsertOutputPeak.size()
	                        ? chainedStatus.trackInsertOutputPeak[meterIndex]
	                        : 0.0f;
	                    if (chainedStatus.trackInsertMeterSlotIndices[meterIndex] == 0) {
	                        geqInputPeak = inputPeak;
	                        geqOutputPeak = outputPeak;
	                        geqMeterFound = true;
	                    } else if (chainedStatus.trackInsertMeterSlotIndices[meterIndex] == 1) {
	                        l2InputPeak = inputPeak;
	                        l2MeterFound = true;
	                    }
	                }
	                if (!geqMeterFound || !l2MeterFound ||
	                    geqInputPeak <= 0.01f ||
	                    geqOutputPeak <= geqInputPeak * 1.2f ||
	                    l2InputPeak <= geqInputPeak * 1.2f) {
	                    std::cerr << "Chained Waves insert meters did not follow per-slot input/process/output contract: "
	                              << "GEQ in " << geqInputPeak << ", GEQ out " << geqOutputPeak
	                              << ", L2 in " << l2InputPeak << "\n";
	                    return 328;
	                }

	                auto drySendReturnProject = dryL2Project;
                drySendReturnProject.tracks[0].outputBus.clear();
                drySendReturnProject.tracks[0].sends.push_back({"Bus 1-2", 0.0f, 0.0f, true, false, true});
                neuracoust::daw::TrackState auxReturnTrack;
                auxReturnTrack.name = "Aux 1";
                auxReturnTrack.trackType = "aux";
                auxReturnTrack.inputBus = "Bus 1-2";
                auxReturnTrack.outputBus = "Master";
                auto sendReturnMasterIt = std::find_if(drySendReturnProject.tracks.begin(),
                                                        drySendReturnProject.tracks.end(),
                                                        [](const auto& track) { return track.name == "Master"; });
                drySendReturnProject.tracks.insert(sendReturnMasterIt, auxReturnTrack);

                auto wetSendReturnProject = drySendReturnProject;
                auto auxL2Insert = l2Insert;
                auxL2Insert.parameters = {{
                    thresholdParameter->id,
                    thresholdParameter->title.empty() ? "Thresh Slider" : thresholdParameter->title,
                    0.02
                }};
                auto auxIt = std::find_if(wetSendReturnProject.tracks.begin(),
                                          wetSendReturnProject.tracks.end(),
                                          [](const auto& track) { return track.name == "Aux 1"; });
                if (auxIt == wetSendReturnProject.tracks.end()) {
                    std::cerr << "Could not create Aux return track for Waves L2 send-return test\n";
                    return 319;
                }
                auxIt->inserts.push_back(auxL2Insert);

                neuracoust::daw::NeuracoustDspEngine drySendReturnEngine;
                if (!drySendReturnEngine.configure(l2Settings, l2Settings.bufferSize, error) ||
                    !drySendReturnEngine.loadProject(drySendReturnProject, error)) {
                    std::cerr << "Could not load dry Waves L2 send-return project: " << error << "\n";
                    return 320;
                }
                std::vector<float> drySendReturnRender;
                drySendReturnEngine.renderInterleavedStereo(44100, drySendReturnRender);
                const double drySendReturnRmsDb = stereoRmsDb(drySendReturnRender);

                neuracoust::daw::NeuracoustDspEngine wetSendReturnEngine;
                if (!wetSendReturnEngine.configure(l2Settings, l2Settings.bufferSize, error) ||
                    !wetSendReturnEngine.loadProject(wetSendReturnProject, error)) {
                    std::cerr << "Could not load wet Waves L2 send-return project: " << error << "\n";
                    return 321;
                }
                std::vector<float> wetSendReturnRender;
                wetSendReturnEngine.renderInterleavedStereo(44100, wetSendReturnRender);
                const double wetSendReturnRmsDb = stereoRmsDb(wetSendReturnRender);
                if (wetSendReturnRmsDb <= drySendReturnRmsDb + 6.0) {
                    std::cerr << "Waves L2 Aux send-return insert did not raise realtime project output enough: dry "
                              << drySendReturnRmsDb << " dB, wet " << wetSendReturnRmsDb << " dB\n";
                    return 322;
                }
                const auto sendReturnStatus = wetSendReturnEngine.statusSnapshot();
                bool auxInsertMeterFound = false;
                for (size_t meterIndex = 0; meterIndex < sendReturnStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
                    if (meterIndex >= sendReturnStatus.trackInsertMeterSlotIndices.size() ||
                        sendReturnStatus.trackInsertMeterTrackNames[meterIndex] != "Aux 1" ||
                        sendReturnStatus.trackInsertMeterSlotIndices[meterIndex] != 0) {
                        continue;
                    }
                    const float inputPeak = meterIndex < sendReturnStatus.trackInsertInputPeak.size()
                        ? sendReturnStatus.trackInsertInputPeak[meterIndex]
                        : 0.0f;
                    const float outputPeak = meterIndex < sendReturnStatus.trackInsertOutputPeak.size()
                        ? sendReturnStatus.trackInsertOutputPeak[meterIndex]
                        : 0.0f;
                    auxInsertMeterFound = inputPeak > 0.01f && outputPeak > inputPeak * 1.25f;
                    break;
                }
                if (!auxInsertMeterFound) {
                    std::cerr << "Waves L2 Aux send-return insert did not publish input/output metering\n";
                    return 323;
                }
                if (!wetSendReturnEngine.updateTrackInsertBypassState("Aux 1", 0, true)) {
                    std::cerr << "Could not bypass Waves L2 Aux send-return insert without reloading project\n";
                    return 324;
                }
                wetSendReturnEngine.seek(0.0);
                std::vector<float> bypassedSendReturnRender;
                wetSendReturnEngine.renderInterleavedStereo(44100, bypassedSendReturnRender);
                const double bypassedSendReturnRmsDb = stereoRmsDb(bypassedSendReturnRender);
                if (std::abs(bypassedSendReturnRmsDb - drySendReturnRmsDb) > 1.0) {
                    std::cerr << "Waves L2 Aux send-return bypass did not return close to dry output: dry "
                              << drySendReturnRmsDb << " dB, bypassed " << bypassedSendReturnRmsDb << " dB\n";
                    return 325;
                }

                auto masterBypassedL2Project = dryL2Project;
                neuracoust::daw::InsertState masterL2Insert;
                masterL2Insert.pluginName = "L2";
                masterL2Insert.pluginAppId = "external-vst3";
                masterL2Insert.pluginFormat = "VST3/AU";
                masterL2Insert.pluginPath = wavesL2Descriptor.bundlePath;
                masterL2Insert.pluginClassId = wavesL2Descriptor.componentClassCid;
                masterL2Insert.pluginClassName = wavesL2Descriptor.componentClassName;
                masterL2Insert.bypassed = true;
                masterL2Insert.available = true;
                masterL2Insert.parameters.push_back({
                    thresholdParameter->id,
                    thresholdParameter->title.empty() ? "Thresh Slider" : thresholdParameter->title,
                    0.02
                });
                masterBypassedL2Project.masterInserts.push_back(masterL2Insert);

                neuracoust::daw::NeuracoustDspEngine masterL2Engine;
                if (!masterL2Engine.configure(l2Settings, l2Settings.bufferSize, error) ||
                    !masterL2Engine.loadProject(masterBypassedL2Project, error)) {
                    std::cerr << "Could not load bypassed Waves L2 master insert project: " << error << "\n";
                    return 310;
                }
                std::vector<float> masterBypassedRender;
                masterL2Engine.renderInterleavedStereo(44100, masterBypassedRender);
                const double masterBypassedRmsDb = stereoRmsDb(masterBypassedRender);
                if (std::abs(masterBypassedRmsDb - dryL2RmsDb) > 1.0) {
                    std::cerr << "Bypassed Waves L2 master insert was not close to dry output: dry "
                              << dryL2RmsDb << " dB, bypassed master " << masterBypassedRmsDb << " dB\n";
                    return 311;
                }
                if (!masterL2Engine.updateMasterInsertBypassState(0, false)) {
                    std::cerr << "Could not enable bypassed Waves L2 master insert without reloading project\n";
                    return 312;
                }
                masterL2Engine.seek(0.0);
                std::vector<float> masterEnabledRender;
                masterL2Engine.renderInterleavedStereo(44100, masterEnabledRender);
                const double masterEnabledRmsDb = stereoRmsDb(masterEnabledRender);
                if (masterEnabledRmsDb <= dryL2RmsDb + 6.0) {
                    std::cerr << "Waves L2 master insert realtime enable did not raise output enough: dry "
                              << dryL2RmsDb << " dB, enabled " << masterEnabledRmsDb << " dB\n";
                    return 313;
                }
                if (!masterL2Engine.updateMasterInsertBypassState(0, true)) {
                    std::cerr << "Could not bypass Waves L2 master insert without reloading project\n";
                    return 314;
                }
                masterL2Engine.seek(0.0);
                std::vector<float> masterRebypassedRender;
                masterL2Engine.renderInterleavedStereo(44100, masterRebypassedRender);
                const double masterRebypassedRmsDb = stereoRmsDb(masterRebypassedRender);
                if (std::abs(masterRebypassedRmsDb - dryL2RmsDb) > 1.0) {
                    std::cerr << "Waves L2 master insert realtime bypass did not return close to dry output: dry "
                              << dryL2RmsDb << " dB, rebypassed " << masterRebypassedRmsDb << " dB\n";
                    return 315;
                }

                auto monitorL2Project = dryL2Project;
                monitorL2Project.monitorModules = neuracoust::daw::defaultMonitorDspModules();
                auto monitorL2Insert = l2Insert;
                monitorL2Insert.pluginFormat = "VST3/AU";
                monitorL2Insert.parameters.clear();
                monitorL2Project.monitorModules[0].activeTargetSlot = 0;
                monitorL2Project.monitorModules[0].speakerOutputA = "Main 1-2";
                monitorL2Project.monitorModules[0].speakerInsertsA = {monitorL2Insert};

                neuracoust::daw::NeuracoustDspEngine monitorL2Engine;
                if (!monitorL2Engine.configure(l2Settings, l2Settings.bufferSize, error) ||
                    !monitorL2Engine.loadProject(monitorL2Project, error)) {
                    std::cerr << "Could not load Waves L2 monitor output insert project: " << error << "\n";
                    return 316;
                }
                std::vector<float> monitorL2DefaultRender;
                monitorL2Engine.renderInterleavedStereo(44100, monitorL2DefaultRender);
                const double monitorL2DefaultRmsDb = stereoRmsDb(monitorL2DefaultRender);
                if (!monitorL2Engine.updateMonitorSpeakerVst3Parameter(1,
                                                                        0,
                                                                        thresholdParameter->id,
                                                                        thresholdParameter->title.empty() ? "Thresh Slider" : thresholdParameter->title,
                                                                        0.02)) {
                    std::cerr << "Could not update Waves L2 monitor output insert parameter without reloading project\n";
                    return 317;
                }
                monitorL2Engine.seek(0.0);
                std::vector<float> monitorL2WetRender;
                monitorL2Engine.renderInterleavedStereo(44100, monitorL2WetRender);
                const double monitorL2WetRmsDb = stereoRmsDb(monitorL2WetRender);
                if (monitorL2WetRmsDb <= monitorL2DefaultRmsDb + 6.0) {
                    std::cerr << "Waves L2 monitor output insert parameter update did not raise output enough: default "
                              << monitorL2DefaultRmsDb << " dB, updated " << monitorL2WetRmsDb << " dB\n";
                    return 318;
                }

                struct WavesRealtimeCase {
                    const char* label;
                    const char* pluginName;
                    const char* classId;
                    const char* className;
                    const char* parameterNeedle;
                    double value;
                    double minimumRmsRiseDb;
                    bool requireOutputMeterRise;
                    int failureBase;
                };
                const WavesRealtimeCase wavesRealtimeCases[] = {
                    {
                        "GEQ Modern 1 kHz boost",
                        "GEQ Modern",
                        "56535453485153676571206d6f646572",
                        "GEQ Modern Stereo",
                        "Left 1Khz",
                        1.0,
                        2.0,
                        true,
                        330
                    },
                    {
                        "H-Reverb output gain",
                        "H-Reverb",
                        "56535448525353682d72657665726220",
                        "H-Reverb Stereo",
                        "Output",
                        1.0,
                        2.0,
                        false,
                        340
                    },
                    {
                        "Abbey Road RS124 output attenuator",
                        "Abbey Road RS124",
                        "56535452533153616262657920726f61",
                        "Abbey Road RS124 Stereo",
                        "Output Attenuator",
                        1.0,
                        2.0,
                        false,
                        350
                    }
                };
                auto lowerText = [](std::string text) {
                    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                    return text;
                };
                for (const auto& realtimeCase : wavesRealtimeCases) {
                    const auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
                        realtimeCase.pluginName,
                        wavesShellPath.string(),
                        realtimeCase.classId,
                        realtimeCase.className);
                    const auto parameters = neuracoust::daw::inspectVst3ParametersWithSdk(descriptor, 512);
                    const std::string parameterNeedle = lowerText(realtimeCase.parameterNeedle);
                    auto parameter = std::find_if(parameters.parameters.begin(), parameters.parameters.end(), [&](const auto& candidate) {
                        return lowerText(candidate.title).find(parameterNeedle) != std::string::npos;
                    });
                    if (parameter == parameters.parameters.end()) {
                        std::cerr << "Could not locate Waves parameter for " << realtimeCase.label
                                  << ": " << realtimeCase.parameterNeedle << "\n";
                        return realtimeCase.failureBase;
                    }

                    auto realtimeProject = dryL2Project;
                    neuracoust::daw::TrackInsertSlot insert;
                    insert.pluginName = realtimeCase.pluginName;
                    insert.pluginFormat = "VST3";
                    insert.pluginPath = descriptor.bundlePath;
                    insert.pluginClassId = descriptor.componentClassCid;
                    insert.pluginClassName = descriptor.componentClassName;
                    insert.dspExecutionMode = "native";
                    insert.enabled = true;
                    insert.bypassed = false;
                    realtimeProject.tracks[0].inserts = {insert};

                    neuracoust::daw::NeuracoustDspEngine realtimeEngine;
                    if (!realtimeEngine.configure(l2Settings, l2Settings.bufferSize, error) ||
                        !realtimeEngine.loadProject(realtimeProject, error)) {
                        std::cerr << "Could not load Waves realtime parameter project for "
                                  << realtimeCase.label << ": " << error << "\n";
                        return realtimeCase.failureBase + 1;
                    }
                    std::vector<float> defaultRender;
                    realtimeEngine.renderInterleavedStereo(44100, defaultRender);
                    const double defaultRmsDb = stereoRmsDb(defaultRender);
                    if (!realtimeEngine.updateTrackVst3Parameter("Audio 1",
                                                                  0,
                                                                  parameter->id,
                                                                  parameter->title,
                                                                  realtimeCase.value)) {
                        std::cerr << "Could not update Waves realtime parameter for "
                                  << realtimeCase.label << "\n";
                        return realtimeCase.failureBase + 2;
                    }
                    realtimeEngine.seek(0.0);
                    std::vector<float> updatedRender;
                    realtimeEngine.renderInterleavedStereo(44100, updatedRender);
                    const double updatedRmsDb = stereoRmsDb(updatedRender);
                    if (updatedRmsDb <= defaultRmsDb + realtimeCase.minimumRmsRiseDb) {
                        std::cerr << "Waves realtime parameter did not affect DAW output for "
                                  << realtimeCase.label << ": default " << defaultRmsDb
                                  << " dB, updated " << updatedRmsDb << " dB\n";
                        return realtimeCase.failureBase + 3;
                    }
                    const auto parameterStatus = realtimeEngine.statusSnapshot();
                    bool meterFound = false;
                    bool meterMoved = false;
                    for (size_t meterIndex = 0; meterIndex < parameterStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
                        if (meterIndex >= parameterStatus.trackInsertMeterSlotIndices.size() ||
                            parameterStatus.trackInsertMeterTrackNames[meterIndex] != "Audio 1" ||
                            parameterStatus.trackInsertMeterSlotIndices[meterIndex] != 0) {
                            continue;
                        }
                        const float inputPeak = meterIndex < parameterStatus.trackInsertInputPeak.size()
                            ? parameterStatus.trackInsertInputPeak[meterIndex]
                            : 0.0f;
                        const float outputPeak = meterIndex < parameterStatus.trackInsertOutputPeak.size()
                            ? parameterStatus.trackInsertOutputPeak[meterIndex]
                            : 0.0f;
                        meterFound = inputPeak > 0.01f && outputPeak > 0.01f;
                        meterMoved = outputPeak > inputPeak * 1.25f;
                        break;
                    }
                    if (!meterFound || (realtimeCase.requireOutputMeterRise && !meterMoved)) {
                        std::cerr << "Waves realtime insert meter did not follow DAW output for "
                                  << realtimeCase.label << "\n";
                        return realtimeCase.failureBase + 4;
                    }
                }
            }

            auto internalTrackInsertProject = validTrackInsertProject;
            internalTrackInsertProject.tracks[0].inserts.front().dspExecutionMode = "internal";
            internalTrackInsertProject.tracks[0].inserts.front().serverModuleId = "na.neuracoust.4001e";
            internalTrackInsertProject.tracks[0].inserts.front().dspAvailable = true;
            neuracoust::daw::NeuracoustDspEngine internalTrackInsertEngine;
            if (!internalTrackInsertEngine.configure(validSettings, validSettings.bufferSize, error) ||
                !internalTrackInsertEngine.loadProject(internalTrackInsertProject, error)) {
                std::cerr << "Could not load internal DSP track VST3 project: " << error << "\n";
                return 198;
            }
            std::vector<float> internalRenderBlock;
            internalTrackInsertEngine.renderInterleavedStereo(16, internalRenderBlock);
            const auto internalTrackInsertStatus = internalTrackInsertEngine.statusSnapshot();
            if (internalTrackInsertStatus.activeRealtimeVst3TrackInsertCount < 1 ||
                internalTrackInsertStatus.activeRemoteDspTrackInsertCount != 0) {
                std::cerr << "Internal DSP track insert was not prepared on the local realtime DSP path: "
                          << internalTrackInsertStatus.message << "\n";
                return 199;
            }
            bool internalMeterFound = false;
            for (size_t meterIndex = 0; meterIndex < internalTrackInsertStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
                if (meterIndex >= internalTrackInsertStatus.trackInsertMeterSlotIndices.size() ||
                    internalTrackInsertStatus.trackInsertMeterTrackNames[meterIndex] != "Audio 1" ||
                    internalTrackInsertStatus.trackInsertMeterSlotIndices[meterIndex] != 0) {
                    continue;
                }
                const float internalInputPeak = meterIndex < internalTrackInsertStatus.trackInsertInputPeak.size()
                    ? internalTrackInsertStatus.trackInsertInputPeak[meterIndex]
                    : 0.0f;
                const float internalOutputPeak = meterIndex < internalTrackInsertStatus.trackInsertOutputPeak.size()
                    ? internalTrackInsertStatus.trackInsertOutputPeak[meterIndex]
                    : 0.0f;
                internalMeterFound = internalInputPeak > 0.001f && internalOutputPeak > 0.001f;
                break;
            }
            if (!internalMeterFound) {
                std::cerr << "Internal DSP track insert did not publish input/output metering for the plugin editor\n";
                return 200;
            }

            auto toggledInternalTrackInsertProject = internalTrackInsertProject;
            toggledInternalTrackInsertProject.tracks[0].inserts.front().bypassed = true;
            if (!internalTrackInsertEngine.updateProject(toggledInternalTrackInsertProject, error)) {
                std::cerr << "Could not bypass internal DSP track insert during playback: " << error << "\n";
                return 2001;
            }
            internalTrackInsertEngine.renderInterleavedStereo(16, internalRenderBlock);
            if (!internalTrackInsertEngine.statusSnapshot().trackInsertMeterTrackNames.empty()) {
                std::cerr << "Internal DSP track insert metering latched a stale peak immediately after bypass\n";
                return 2002;
            }
            toggledInternalTrackInsertProject.tracks[0].inserts.front().bypassed = false;
            if (!internalTrackInsertEngine.updateProject(toggledInternalTrackInsertProject, error)) {
                std::cerr << "Could not re-enable internal DSP track insert during playback: " << error << "\n";
                return 2003;
            }
            internalTrackInsertEngine.renderInterleavedStereo(16, internalRenderBlock);
            if (!internalTrackInsertEngine.statusSnapshot().trackInsertMeterTrackNames.empty()) {
                std::cerr << "Internal DSP track insert metering latched a graph-change peak immediately after enable\n";
                return 2004;
            }
            for (int warmupBlock = 0; warmupBlock < 180; ++warmupBlock) {
                internalTrackInsertEngine.renderInterleavedStereo(16, internalRenderBlock);
            }
            bool resumedInternalMeterFound = false;
            const auto resumedInternalTrackInsertStatus = internalTrackInsertEngine.statusSnapshot();
            for (size_t meterIndex = 0; meterIndex < resumedInternalTrackInsertStatus.trackInsertMeterTrackNames.size(); ++meterIndex) {
                if (meterIndex >= resumedInternalTrackInsertStatus.trackInsertMeterSlotIndices.size() ||
                    resumedInternalTrackInsertStatus.trackInsertMeterTrackNames[meterIndex] != "Audio 1" ||
                    resumedInternalTrackInsertStatus.trackInsertMeterSlotIndices[meterIndex] != 0) {
                    continue;
                }
                const float resumedInputPeak = meterIndex < resumedInternalTrackInsertStatus.trackInsertInputPeak.size()
                    ? resumedInternalTrackInsertStatus.trackInsertInputPeak[meterIndex]
                    : 0.0f;
                const float resumedOutputPeak = meterIndex < resumedInternalTrackInsertStatus.trackInsertOutputPeak.size()
                    ? resumedInternalTrackInsertStatus.trackInsertOutputPeak[meterIndex]
                    : 0.0f;
                resumedInternalMeterFound = resumedInputPeak > 0.001f && resumedOutputPeak > 0.001f;
                break;
            }
            if (!resumedInternalMeterFound) {
                std::cerr << "Internal DSP track insert metering did not resume after graph-change stabilization\n";
                return 2005;
            }

            double internalTrackInsertError = 0.0;
            double internalDryDifference = 0.0;
            for (size_t i = 0; i < internalRenderBlock.size() && i < expectedTrackInsertInput.interleavedSamples.size(); ++i) {
                internalTrackInsertError = std::max(internalTrackInsertError,
                                                    static_cast<double>(std::abs(internalRenderBlock[i] - expectedTrackInsertInput.interleavedSamples[i])));
                internalDryDifference += std::abs(internalRenderBlock[i] - 0.05f);
            }
            if (internalRenderBlock.size() != renderBlock.size() ||
                internalTrackInsertError > 0.04 ||
                internalDryDifference <= 0.0001) {
                std::cerr << "Internal DSP track insert did not process the incoming track signal; max error "
                          << internalTrackInsertError << "\n";
                return 201;
            }

            auto busRoutedTrackInsertProject = validTrackInsertProject;
            busRoutedTrackInsertProject.tracks[0].outputBus = "Bus 1-2";
            neuracoust::daw::TrackState busTrack;
            busTrack.name = "Bus 1-2";
            busTrack.trackType = "aux";
            busTrack.inputBus = "Bus 1-2";
            busTrack.outputBus = "Master";
            busRoutedTrackInsertProject.tracks.insert(busRoutedTrackInsertProject.tracks.begin() + 1, busTrack);
            neuracoust::daw::NeuracoustDspEngine busRoutedTrackInsertEngine;
            if (!busRoutedTrackInsertEngine.configure(validSettings, validSettings.bufferSize, error) ||
                !busRoutedTrackInsertEngine.loadProject(busRoutedTrackInsertProject, error)) {
                std::cerr << "Could not load bus-routed track insert project: " << error << "\n";
                return 202;
            }
            std::vector<float> busRoutedRenderBlock;
            busRoutedTrackInsertEngine.renderInterleavedStereo(16, busRoutedRenderBlock);
            double busRoutedTrackInsertError = 0.0;
            for (size_t i = 0; i < busRoutedRenderBlock.size() && i < expectedTrackInsertInput.interleavedSamples.size(); ++i) {
                busRoutedTrackInsertError = std::max(busRoutedTrackInsertError,
                                                     static_cast<double>(std::abs(busRoutedRenderBlock[i] - expectedTrackInsertInput.interleavedSamples[i])));
            }
            if (busRoutedRenderBlock.size() != renderBlock.size() || busRoutedTrackInsertError > 0.04) {
                std::cerr << "Bus-routed track insert did not process before the internal bus; max error "
                          << busRoutedTrackInsertError << "\n";
                return 203;
            }

            auto busInsertProject = busRoutedTrackInsertProject;
            busInsertProject.tracks[0].inserts.clear();
            busInsertProject.tracks[1].inserts.push_back({
                "Newacoust4001E",
                "VST3",
                validTrackVst3Path.string(),
                false,
                true
            });
            neuracoust::daw::NeuracoustDspEngine busInsertEngine;
            if (!busInsertEngine.configure(validSettings, validSettings.bufferSize, error) ||
                !busInsertEngine.loadProject(busInsertProject, error)) {
                std::cerr << "Could not load bus insert project: " << error << "\n";
                return 204;
            }
            std::vector<float> busInsertRenderBlock;
            busInsertEngine.renderInterleavedStereo(16, busInsertRenderBlock);
            double busInsertError = 0.0;
            for (size_t i = 0; i < busInsertRenderBlock.size() && i < expectedTrackInsertInput.interleavedSamples.size(); ++i) {
                busInsertError = std::max(busInsertError,
                                          static_cast<double>(std::abs(busInsertRenderBlock[i] - expectedTrackInsertInput.interleavedSamples[i])));
            }
            if (busInsertRenderBlock.size() != renderBlock.size() || busInsertError > 0.04) {
                std::cerr << "Bus insert did not process the internal bus signal; max error "
                          << busInsertError << "\n";
                return 205;
            }

            const auto printedTakePath = (smokeTempRoot / "neuracoust-daw-record-print-vst3.wav").string();
            if (!neuracoust::daw::writePcm16WavFile(printedTakePath, validSource, error)) {
                std::cerr << "Could not write recorded print source WAV: " << error << "\n";
                return 206;
            }
            auto recordPrintProject = validTrackInsertProject;
            if (!neuracoust::daw::printRecordedTakeThroughTrackDsp(recordPrintProject,
                                                                   "Audio 1",
                                                                   printedTakePath,
                                                                   error)) {
                std::cerr << "Recorded take DSP print failed: " << error << "\n";
                return 207;
            }
            neuracoust::daw::WavAudioData printedTake;
            if (!neuracoust::daw::readPcmWavFile(printedTakePath, printedTake, error)) {
                std::cerr << "Could not read printed take WAV: " << error << "\n";
                return 208;
            }
            if (printedTake.frameCount() < 16) {
                std::cerr << "Printed take WAV is unexpectedly short\n";
                return 209;
            }
            double printDryDifference = 0.0;
            for (size_t i = 0; i < std::min<size_t>(16, printedTake.interleavedSamples.size()); ++i) {
                printDryDifference += std::abs(printedTake.interleavedSamples[i] - 0.05f);
            }
            if (printDryDifference <= 0.0001) {
                std::cerr << "Recorded take DSP print left the WAV dry\n";
                return 210;
            }
        }
        if (!mixEngine.loadProject(mixProject, error)) {
            std::cerr << "Could not restore clean mix project after insert status check: " << error << "\n";
            return 188;
        }
        mixEngine.resetRuntime();
        const auto resetMeterStatus = mixEngine.statusSnapshot();
        if (resetMeterStatus.outputPeakLeft != 0.0f ||
            resetMeterStatus.outputPeakRight != 0.0f ||
            resetMeterStatus.phaseCorrelation != 0.0f ||
            !resetMeterStatus.trackMeterNames.empty() ||
            !resetMeterStatus.trackPeakLeft.empty() ||
            !resetMeterStatus.trackPeakRight.empty()) {
            std::cerr << "DSP reset did not clear output and track meters\n";
            return 184;
        }

        if (!neuracoust::daw::setTrackVolumeDb(mixProject, "Audio 1", -6.0f) ||
            !mixEngine.loadProject(mixProject, error)) {
            std::cerr << "Could not reload mix project after volume edit: " << error << "\n";
            return 19;
        }
        mixEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock[0] < 0.24f || renderBlock[0] > 0.26f || std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Track volume reload did not affect realtime DSP render\n";
            return 20;
        }

        if (!neuracoust::daw::setTrackVolumeDb(mixProject, "Audio 1", 0.0f) ||
            !neuracoust::daw::setTrackPan(mixProject, "Audio 1", 1.0f) ||
            !mixEngine.loadProject(mixProject, error)) {
            std::cerr << "Could not reload mix project after pan edit: " << error << "\n";
            return 21;
        }
        mixEngine.renderInterleavedStereo(1, renderBlock);
        if (std::abs(renderBlock[0]) > 0.0001f || renderBlock[1] < 0.49f) {
            std::cerr << "Track pan reload did not affect realtime DSP render\n";
            return 22;
        }

        if (!mixEngine.updateTrackMix("Audio 1", -6.0f, -1.0f)) {
            std::cerr << "Track mix realtime update was rejected\n";
            return 23;
        }
        mixEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock[0] < 0.24f || renderBlock[0] > 0.26f || std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Track mix realtime update did not affect DSP render without project reload\n";
            return 24;
        }
        if (!mixEngine.updateTrackMix("Audio 1", -120.0f, 0.0f)) {
            std::cerr << "Track mix minimum gain update was rejected\n";
            return 25;
        }
        mixEngine.renderInterleavedStereo(1, renderBlock);
        if (std::abs(renderBlock[0]) > 0.0001f || std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Track minimum gain did not mute realtime DSP render\n";
            return 26;
        }

        const auto liveInstrumentPath = std::filesystem::path("/Library/Audio/Plug-Ins/VST3/FabFilter Twin 3.vst3");
        if (std::filesystem::exists(liveInstrumentPath)) {
            neuracoust::daw::NeuracoustDspEngine liveInstrumentEngine;
            neuracoust::daw::AudioEngineSettings liveInstrumentSettings;
            liveInstrumentSettings.sampleRate = 48000.0;
            liveInstrumentSettings.bufferSize = 256;
            liveInstrumentSettings.monitorDspEnabled = false;
            if (!liveInstrumentEngine.configure(liveInstrumentSettings, liveInstrumentSettings.bufferSize, error)) {
                std::cerr << "Could not configure live MIDI instrument engine: " << error << "\n";
                return 203;
            }
            auto liveInstrumentProject = neuracoust::daw::defaultProject();
            liveInstrumentProject.sampleRate = 48000.0;
            liveInstrumentProject.clips.clear();
            const auto instrumentTrackName = neuracoust::daw::addInstrumentTrack(liveInstrumentProject);
            neuracoust::daw::InstrumentSlotState instrument;
            instrument.pluginName = "FabFilter Twin 3";
            instrument.pluginFormat = "VST3";
            instrument.pluginPath = liveInstrumentPath.string();
            instrument.enabled = true;
            instrument.bypassed = false;
            instrument.midiChannel = 0;
            if (!neuracoust::daw::setTrackInstrumentSlot(liveInstrumentProject, instrumentTrackName, instrument)) {
                std::cerr << "Could not assign live MIDI instrument slot\n";
                return 204;
            }
            for (auto& track : liveInstrumentProject.tracks) {
                if (track.name == instrumentTrackName) {
                    track.recordArmed = true;
                    track.inputMonitoring = true;
                }
            }
            if (!liveInstrumentEngine.loadProject(liveInstrumentProject, error)) {
                std::cerr << "Could not load live MIDI instrument project: " << error << "\n";
                return 205;
            }
            double liveInstrumentEnergy = 0.0;
            for (int block = 0; block < 80; ++block) {
                if (block % 8 == 0) {
                    liveInstrumentEngine.queueLiveMidiEvents(instrumentTrackName, {
                        {0, 60, 110, 1, true, neuracoust::daw::Vst3MidiEventKind::Note}
                    });
                }
                liveInstrumentEngine.renderInterleavedStereo(256, renderBlock);
                for (const float sample : renderBlock) {
                    liveInstrumentEnergy += std::abs(sample);
                }
                if (liveInstrumentEnergy > 0.01) {
                    break;
                }
            }
            if (liveInstrumentEnergy <= 0.01) {
                std::cerr << "Warning: installed FabFilter Twin 3 did not produce live MIDI smoke audio; continuing because third-party default preset output is not deterministic.\n";
            }
        }

        neuracoust::daw::NeuracoustDspEngine listenEngine;
        neuracoust::daw::AudioEngineSettings listenSettings;
        listenSettings.sampleRate = 48000.0;
        listenSettings.bufferSize = 64;
        listenSettings.monitorDspEnabled = false;
        listenSettings.listenRoom.enabled = true;
        listenSettings.listenRoom.sessionName = "smoke";
        listenSettings.listenRoom.quality = "pcm_lossless";
        listenSettings.listenRoom.latencyMode = "low";
        listenSettings.listenRoom.transportMode = "direct_fallback";
        listenSettings.listenRoom.relayHost = "127.0.0.1";
        listenSettings.listenRoom.accessToken = "smoke-token";
        listenSettings.listenRoom.relayHttpPort = 8787;
        listenSettings.listenRoom.relayTcpIngestPort = 8791;
        if (!listenEngine.configure(listenSettings, listenSettings.bufferSize, error)) {
            std::cerr << "Could not configure Listen Room DSP engine: " << error << "\n";
            return 202;
        }
        auto listenProject = neuracoust::daw::defaultProject();
        listenProject.sampleRate = 48000.0;
        listenProject.listenRoomEnabled = true;
        listenProject.listenRoomSessionName = "smoke";
        listenProject.listenRoomQuality = "pcm_lossless";
        listenProject.listenRoomLatencyMode = "low";
        listenProject.listenRoomTransportMode = "direct_fallback";
        listenProject.listenRoomRelayHost = "127.0.0.1";
        listenProject.listenRoomAccessToken = "smoke-token";
        listenProject.listenRoomRelayHttpPort = 8787;
        listenProject.listenRoomRelayTcpIngestPort = 8791;
        neuracoust::daw::WavAudioData listenSource;
        listenSource.channels = 2;
        listenSource.sampleRate = 48000;
        listenSource.interleavedSamples.assign(64 * 2 * 8, 0.1f);
        const auto listenSourcePath = (smokeTempRoot / "neuracoust-daw-listen-room.wav").string();
        if (!neuracoust::daw::writePcm16WavFile(listenSourcePath, listenSource, error)) {
            std::cerr << "Could not write Listen Room smoke WAV: " << error << "\n";
            return 203;
        }
        listenProject.clips.push_back({"listen-room-clip", "Audio 1", listenSourcePath, 0.0, 0.02, 0.0, 0.0f});
        if (!listenEngine.loadProject(listenProject, error)) {
            std::cerr << "Could not load Listen Room smoke project: " << error << "\n";
            return 204;
        }
        std::vector<float> listenBlock;
        const auto listenRenderStart = std::chrono::steady_clock::now();
        for (int block = 0; block < 8; ++block) {
            listenEngine.renderInterleavedStereo(64, listenBlock);
            if (listenBlock.size() != 128) {
                std::cerr << "Listen Room render returned the wrong block size\n";
                return 205;
            }
        }
        const auto listenRenderMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - listenRenderStart).count();
        if (listenRenderMs > 2000) {
            std::cerr << "Listen Room sender blocked realtime render for " << listenRenderMs << " ms\n";
            return 206;
        }
        auto listenStatus = listenEngine.statusSnapshot().listenRoom;
        for (int attempt = 0; attempt < 20 && (!listenStatus.senderRunning || listenStatus.packetsQueued == 0); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            listenStatus = listenEngine.statusSnapshot().listenRoom;
        }
        if (!listenStatus.enabled ||
            !listenStatus.senderRunning ||
            listenStatus.sessionId == 0 ||
            listenStatus.shareUrl.find("smoke") == std::string::npos ||
            listenStatus.shareUrl.find("smoke-token") == std::string::npos ||
            listenStatus.activeCodec.find("PCM") == std::string::npos ||
            listenStatus.packetFrames != 96 ||
            listenStatus.maxQueuedBlocks != 4 ||
            listenStatus.latencyTargetMs != 120 ||
            listenStatus.packetsQueued == 0) {
            std::cerr << "Listen Room status did not expose an armed non-blocking sender\n";
            return 207;
        }
        listenSettings.listenRoom.enabled = false;
        listenEngine.setListenRoomSettings(listenSettings.listenRoom);
        const auto stoppedListenStatus = listenEngine.statusSnapshot().listenRoom;
        if (stoppedListenStatus.enabled || stoppedListenStatus.senderRunning) {
            std::cerr << "Listen Room sender did not stop when disabled\n";
            return 208;
        }

        listenSettings.listenRoom.enabled = true;
        listenSettings.listenRoom.transportMode = "native_webrtc";
        listenEngine.setListenRoomSettings(listenSettings.listenRoom);
        listenEngine.renderInterleavedStereo(64, listenBlock);
        const auto nativeListenStatus = listenEngine.statusSnapshot().listenRoom;
        if (!nativeListenStatus.enabled ||
            nativeListenStatus.transportMode != "native_webrtc" ||
            nativeListenStatus.nativeWebRtcSignalingUrl.find("/api/native-webrtc/offer") == std::string::npos ||
            nativeListenStatus.nativeWebRtcFramesQueued == 0 ||
            nativeListenStatus.activeCodec.find("Native WebRTC") == std::string::npos) {
            std::cerr << "Listen Room native WebRTC status path was not exposed\n";
            return 210;
        }
        listenSettings.listenRoom.enabled = false;
        listenEngine.setListenRoomSettings(listenSettings.listenRoom);

#if defined(NEURACOUST_DAW_HAS_OPUS) && !defined(_WIN32)
        {
            constexpr int kOpusSmokePort = 19091;
            std::atomic<int> observedVersion {0};
            std::atomic<int> observedCodec {-1};
            std::atomic<bool> serverReady {false};
            std::thread server([&] {
                const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
                if (serverFd < 0) {
                    return;
                }
                int reuse = 1;
                setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
                timeval timeout {};
                timeout.tv_sec = 8;
                setsockopt(serverFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                sockaddr_in address {};
                address.sin_family = AF_INET;
                address.sin_port = htons(kOpusSmokePort);
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
                    listen(serverFd, 8) != 0) {
                    close(serverFd);
                    return;
                }
                serverReady.store(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (std::chrono::steady_clock::now() < deadline && observedVersion.load() == 0) {
                    const int clientFd = accept(serverFd, nullptr, nullptr);
                    if (clientFd < 0) {
                        continue;
                    }
                    uint32_t frameBytes = 0;
                    if (recvExact(clientFd, &frameBytes, sizeof(frameBytes))) {
                        std::vector<uint8_t> packet(frameBytes);
                        if (frameBytes > 0 && recvExact(clientFd, packet.data(), packet.size()) && packet.size() >= 24) {
                            observedVersion.store(static_cast<int>(packet[4]));
                            observedCodec.store(static_cast<int>(packet[5]));
                        }
                    }
                    close(clientFd);
                }
                close(serverFd);
            });

            for (int wait = 0; wait < 100 && !serverReady.load(); ++wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            neuracoust::daw::ListenRoomSender opusSender;
            neuracoust::daw::ListenRoomSettings opusSettings;
            opusSettings.enabled = true;
            opusSettings.sessionName = "opus-smoke";
            opusSettings.quality = "opus_high";
            opusSettings.latencyMode = "stable";
            opusSettings.relayHost = "127.0.0.1";
            opusSettings.relayTcpIngestPort = kOpusSmokePort;
            opusSender.configure(44100.0, opusSettings);
            std::vector<float> opusBlock(64 * 2, 0.05f);
            for (int block = 0; block < 80 && observedVersion.load() == 0; ++block) {
                opusSender.pushInterleavedStereo(opusBlock.data(), 64);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            for (int wait = 0; wait < 200 && observedVersion.load() == 0; ++wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            opusSender.stop();
            if (server.joinable()) {
                server.join();
            }
            if (observedVersion.load() != 2 || observedCodec.load() != 1) {
                std::cerr << "Listen Room Opus sender did not emit an NLST v2 Opus packet\n";
                return 209;
            }
        }
#endif

        neuracoust::daw::NeuracoustDspEngine stationEngine;
        neuracoust::daw::AudioEngineSettings stationSettings;
        stationSettings.sampleRate = 100.0;
        stationSettings.bufferSize = 16;
        stationSettings.monitorDspEnabled = false;
        stationSettings.monitorInputTrimDb = 0.0f;
        stationSettings.monitorVolumeDb = 0.0f;
        stationSettings.monitorStationMono = true;
        stationSettings.monitorStationInvertLeft = true;
        stationSettings.monitorStationDim = true;
        if (!stationEngine.configure(stationSettings, stationSettings.bufferSize, error)) {
            std::cerr << "Could not configure monitor station DSP engine: " << error << "\n";
            return 27;
        }
        neuracoust::daw::WavAudioData stationSource;
        stationSource.channels = 2;
        stationSource.sampleRate = 100;
        stationSource.interleavedSamples = {0.25f, 0.75f};
        const auto stationSourcePath = (smokeTempRoot / "neuracoust-daw-monitor-station.wav").string();
        if (!neuracoust::daw::writePcm16WavFile(stationSourcePath, stationSource, error) ||
            !stationEngine.loadAudioFile(stationSourcePath, error)) {
            std::cerr << "Could not load monitor station source WAV: " << error << "\n";
            return 28;
        }
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] > -0.045f || renderBlock[0] < -0.055f ||
            renderBlock[1] < 0.045f || renderBlock[1] > 0.055f) {
            std::cerr << "Monitor station mono/invert/dim controls did not affect DSP render\n";
            return 29;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(false, "M", false, false, false, false, false, -6.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.249f || renderBlock[0] > 0.252f ||
            renderBlock[1] < 0.249f || renderBlock[1] > 0.252f) {
            std::cerr << "Monitor station mid listen mode or monitor volume did not affect DSP render\n";
            return 291;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(false, "LR", false, false, false, false, false, -120.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            std::abs(renderBlock[0]) > 0.0001f ||
            std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Monitor station minimum volume did not mute final output\n";
            return 298;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(false, "L", false, false, false, false, false, 0.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.249f || renderBlock[0] > 0.251f ||
            std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Monitor station stereo left listen mode did not isolate left input\n";
            return 294;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(false, "R", false, false, false, false, false, 0.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            std::abs(renderBlock[0]) > 0.0001f ||
            renderBlock[1] < 0.749f || renderBlock[1] > 0.751f) {
            std::cerr << "Monitor station stereo right listen mode did not isolate right input\n";
            return 295;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(true, "L", false, false, false, false, false, 0.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.249f || renderBlock[0] > 0.251f ||
            renderBlock[1] < 0.249f || renderBlock[1] > 0.251f) {
            std::cerr << "Monitor station mono left listen mode did not mirror left input\n";
            return 2971;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(true, "R", false, false, false, false, false, 0.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.749f || renderBlock[0] > 0.751f ||
            renderBlock[1] < 0.749f || renderBlock[1] > 0.751f) {
            std::cerr << "Monitor station mono right listen mode did not mirror right input\n";
            return 2972;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(false, "S", false, false, false, false, false, 0.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] > -0.249f || renderBlock[0] < -0.251f ||
            renderBlock[1] < 0.249f || renderBlock[1] > 0.251f) {
            std::cerr << "Monitor station side listen mode did not affect DSP render\n";
            return 292;
        }
        stationEngine.seek(0.0);
        stationEngine.setMonitorStationControls(true, "S", true, false, false, false, false, 0.0f);
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] > -0.249f || renderBlock[0] < -0.251f ||
            renderBlock[1] < 0.249f || renderBlock[1] > 0.251f) {
            std::cerr << "Monitor station side listen mode should ignore mono/swap state\n";
            return 293;
        }

        auto stationProject = neuracoust::daw::defaultProject();
        stationProject.sampleRate = 100.0;
        stationProject.monitorStationListenMode = "S";
        stationProject.monitorStationMono = true;
        stationProject.monitorStationSwapLeftRight = true;
        stationProject.monitorInputTrimDb = 0.0f;
        stationProject.monitorVolumeDb = 0.0f;
        stationProject.clips.clear();
        stationProject.clips.push_back({"station-project-clip", "Audio 1", stationSourcePath, 0.0, 0.01, 0.0, 0.0f});
        if (!stationEngine.loadProject(stationProject, error)) {
            std::cerr << "Could not load monitor station project: " << error << "\n";
            return 296;
        }
        stationEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] > -0.249f || renderBlock[0] < -0.251f ||
            renderBlock[1] < 0.249f || renderBlock[1] > 0.251f) {
            std::cerr << "Project load did not normalize monitor station MS mode\n";
            return 297;
        }
    }

    {
        neuracoust::daw::WavAudioData graphMonitorSource;
        graphMonitorSource.channels = 1;
        graphMonitorSource.sampleRate = 48000;
        graphMonitorSource.interleavedSamples.assign(64, 0.25f);
        std::string error;
        const auto graphMonitorSourcePath = (smokeTempRoot / "neuracoust-daw-realtime-graph-monitor.wav").string();
        if (!neuracoust::daw::writePcm16WavFile(graphMonitorSourcePath, graphMonitorSource, error)) {
            std::cerr << "Could not create realtime graph monitor source WAV: " << error << "\n";
            return 322;
        }
        auto graphMonitorProject = neuracoust::daw::defaultProject();
        graphMonitorProject.sampleRate = 48000.0;
        graphMonitorProject.monitorInputTrimDb = 0.0f;
        graphMonitorProject.monitorVolumeDb = 0.0f;
        graphMonitorProject.monitorModules = neuracoust::daw::defaultMonitorDspModules();
        graphMonitorProject.monitorModules[0].realModel = "Real Speaker: Nearfield";
        graphMonitorProject.monitorModules[0].targetModelA = "Speaker A: Laptop";
        graphMonitorProject.clips.push_back({"graph-monitor-clip", "Audio 1", graphMonitorSourcePath, 0.0, 64.0 / 48000.0, 0.0, 0.0f});
        neuracoust::daw::ProjectAudioRenderPlan graphMonitorPlan;
        if (!neuracoust::daw::makeProjectAudioRenderPlan(graphMonitorProject, graphMonitorPlan, error)) {
            std::cerr << "Could not make realtime graph monitor render plan: " << error << "\n";
            return 323;
        }
        graphMonitorPlan.renderMonitorDsp = true;
        neuracoust::daw::ProjectAudioRenderState graphMonitorState;
        std::vector<float> expectedGraphMonitorBlock;
        neuracoust::daw::renderProjectAudioBlockWithStateAndMeters(
            graphMonitorPlan,
            graphMonitorState,
            0,
            64,
            expectedGraphMonitorBlock,
            nullptr);

        neuracoust::daw::NeuracoustDspEngine graphMonitorEngine;
        neuracoust::daw::AudioEngineSettings graphMonitorSettings;
        graphMonitorSettings.sampleRate = 48000.0;
        graphMonitorSettings.bufferSize = 64;
        graphMonitorSettings.monitorDspEnabled = true;
        graphMonitorSettings.monitorDspPathMode = "internal";
        graphMonitorSettings.monitorInputTrimDb = 0.0f;
        graphMonitorSettings.monitorVolumeDb = 0.0f;
        graphMonitorSettings.monitorModules = graphMonitorProject.monitorModules;
        if (!graphMonitorEngine.configure(graphMonitorSettings, graphMonitorSettings.bufferSize, error) ||
            !graphMonitorEngine.loadProject(graphMonitorProject, error)) {
            std::cerr << "Could not configure realtime graph monitor engine: " << error << "\n";
            return 324;
        }
        std::vector<float> graphMonitorBlock;
        graphMonitorEngine.renderInterleavedStereo(64, graphMonitorBlock);
        if (graphMonitorBlock.size() != expectedGraphMonitorBlock.size()) {
            std::cerr << "Realtime graph monitor render size mismatch\n";
            return 325;
        }
        float graphMonitorDelta = 0.0f;
        for (size_t index = 0; index < graphMonitorBlock.size(); ++index) {
            graphMonitorDelta = std::max(graphMonitorDelta, std::abs(graphMonitorBlock[index] - expectedGraphMonitorBlock[index]));
        }
        if (graphMonitorDelta > 0.0001f) {
            std::cerr << "Realtime monitor DSP did not match graph monitor route render: " << graphMonitorDelta << "\n";
            return 326;
        }
        const float baselineGraphMonitorPeak = std::accumulate(
            graphMonitorBlock.begin(),
            graphMonitorBlock.end(),
            0.0f,
            [](float peak, float sample) { return std::max(peak, std::abs(sample)); });
        graphMonitorEngine.seek(0.0);
        graphMonitorEngine.setMonitorStationControls(false, "LR", false, false, false, false, false, false, -9.0f, 0.0f, -20.0f, "listen_room");
        graphMonitorEngine.renderInterleavedStereo(64, graphMonitorBlock);
        const float trimmedGraphMonitorPeak = std::accumulate(
            graphMonitorBlock.begin(),
            graphMonitorBlock.end(),
            0.0f,
            [](float peak, float sample) { return std::max(peak, std::abs(sample)); });
        if (trimmedGraphMonitorPeak >= baselineGraphMonitorPeak * 0.70f) {
            std::cerr << "Realtime monitor input trim did not reduce loaded graph render level: "
                      << baselineGraphMonitorPeak << " -> " << trimmedGraphMonitorPeak << "\n";
            return 3261;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine dspEngine;
        neuracoust::daw::AudioEngineSettings dspSettings;
        dspSettings.sampleRate = 48000.0;
        dspSettings.bufferSize = 64;
        dspSettings.playbackStabilityBufferMultiplier = 4;
        dspSettings.monitorDspEnabled = false;
        dspSettings.monitorInputTrimDb = 0.0f;
        dspSettings.monitorVolumeDb = 0.0f;
        std::string error;
        if (!dspEngine.configure(dspSettings, dspSettings.bufferSize, error)) {
            std::cerr << "Could not configure Neuracoust DSP engine: " << error << "\n";
            return 12;
        }
        auto monitorProject = neuracoust::daw::defaultProject();
        monitorProject.monitorInputTrimDb = 0.0f;
        monitorProject.monitorVolumeDb = 0.0f;
        monitorProject.tracks[0].recordArmed = true;
        monitorProject.tracks[0].volumeDb = -6.0f;
        monitorProject.tracks[0].pan = 1.0f;
        if (!dspEngine.loadProject(monitorProject, error)) {
            std::cerr << "Could not load monitor policy project: " << error << "\n";
            return 13;
        }
        std::vector<float> inputBlock(128, 0.0f);
        for (size_t index = 0; index < inputBlock.size(); index += 2) {
            inputBlock[index] = 0.25f;
            inputBlock[index + 1] = 0.25f;
        }
        dspEngine.pushInputMonitorInterleaved(inputBlock.data(), 64, 2);
        std::vector<float> renderBlock;
        dspEngine.renderInterleavedStereo(64, renderBlock);
        const auto tapePlaybackStatus = dspEngine.statusSnapshot();
        if (tapePlaybackStatus.lowLatencyRecordMonitoringActive ||
            tapePlaybackStatus.physicalInputMonitoringActive ||
            tapePlaybackStatus.recordArmedTrackCount != 1 ||
            std::any_of(renderBlock.begin(), renderBlock.end(), [](float sample) { return std::abs(sample) > 0.0001f; })) {
            std::cerr << "Record-armed tape playback mode should not open the input monitor path before recording\n";
            return 321;
        }

        dspEngine.setTransportRecordingActive(true);
        dspEngine.pushInputMonitorInterleaved(inputBlock.data(), 64, 2);
        dspEngine.renderInterleavedStereo(64, renderBlock);
        const auto dspStatus = dspEngine.statusSnapshot();
        const auto peak = *std::max_element(renderBlock.begin(), renderBlock.end());
        if (renderBlock.size() != 128 ||
            peak <= 0.0f ||
            !dspStatus.lowLatencyRecordMonitoringActive ||
            !dspStatus.physicalInputMonitoringActive ||
            dspStatus.recordArmedTrackCount != 1 ||
            dspStatus.inputChannels != 2 ||
            dspStatus.requestedBufferSize != 64 ||
            dspStatus.playbackStabilityBufferSize != 256 ||
            dspStatus.dspEngineName != "Neuracoust DSP Engine") {
            std::cerr << "Neuracoust DSP engine monitor policy status is wrong\n";
            return 14;
        }
        if (std::abs(renderBlock[0]) > 0.0001f || renderBlock[1] < 0.12f || renderBlock[1] > 0.13f) {
            std::cerr << "Record monitor did not follow armed track volume/pan policy\n";
            return 15;
        }
        dspEngine.setTransportRecordingActive(false);

        auto inputMonitorOnlyProject = neuracoust::daw::defaultProject();
        inputMonitorOnlyProject.monitorInputTrimDb = 0.0f;
        inputMonitorOnlyProject.monitorVolumeDb = 0.0f;
        inputMonitorOnlyProject.tracks[0].recordArmed = false;
        inputMonitorOnlyProject.tracks[0].inputMonitoring = true;
        inputMonitorOnlyProject.tracks[0].volumeDb = -12.0f;
        inputMonitorOnlyProject.tracks[0].pan = 0.0f;
        if (!dspEngine.loadProject(inputMonitorOnlyProject, error)) {
            std::cerr << "Could not load input-monitor-only policy project: " << error << "\n";
            return 318;
        }
        dspEngine.pushInputMonitorInterleaved(inputBlock.data(), 64, 2);
        dspEngine.renderInterleavedStereo(64, renderBlock);
        const auto inputMonitorOnlyStatus = dspEngine.statusSnapshot();
        if (!inputMonitorOnlyStatus.lowLatencyRecordMonitoringActive ||
            !inputMonitorOnlyStatus.physicalInputMonitoringActive ||
            inputMonitorOnlyStatus.recordArmedTrackCount != 0 ||
            inputMonitorOnlyStatus.inputChannels != 2) {
            std::cerr << "Input monitor button did not activate the low-latency monitor path\n";
            return 319;
        }
        if (renderBlock.size() != 128 || renderBlock[0] < 0.06f || renderBlock[0] > 0.07f ||
            renderBlock[1] < 0.06f || renderBlock[1] > 0.07f) {
            std::cerr << "Input-monitor-only path did not follow track volume/pan policy\n";
            return 320;
        }

        neuracoust::daw::NeuracoustDspEngine talkbackEngine;
        neuracoust::daw::AudioEngineSettings talkbackSettings;
        talkbackSettings.sampleRate = 48000.0;
        talkbackSettings.bufferSize = 64;
        talkbackSettings.monitorDspEnabled = false;
        talkbackSettings.monitorInputTrimDb = 0.0f;
        talkbackSettings.monitorVolumeDb = 0.0f;
        talkbackSettings.inputMonitorChannelCount = 2;
        if (!talkbackEngine.configure(talkbackSettings, talkbackSettings.bufferSize, error)) {
            std::cerr << "Could not configure talkback route DSP engine: " << error << "\n";
            return 327;
        }
        talkbackEngine.setMonitorStationControls(false, "LR", false, false, false, false, true, true, 0.0f, 0.0f, -20.0f, "listen_room");
        talkbackEngine.pushInputMonitorInterleaved(inputBlock.data(), 64, 2);
        talkbackEngine.renderInterleavedStereo(64, renderBlock);
        if (std::any_of(renderBlock.begin(), renderBlock.end(), [](float sample) { return std::abs(sample) > 0.0001f; })) {
            std::cerr << "Listen Room talkback leaked into the local monitor bus\n";
            return 328;
        }
        talkbackEngine.setMonitorStationControls(false, "LR", false, false, false, false, true, true, 0.0f, 0.0f, -20.0f, "monitor_bus");
        talkbackEngine.pushInputMonitorInterleaved(inputBlock.data(), 64, 2);
        talkbackEngine.renderInterleavedStereo(64, renderBlock);
        if (renderBlock.size() != 128 || renderBlock[0] < 0.02f || renderBlock[1] < 0.02f) {
            std::cerr << "Monitor Bus talkback did not reach the local monitor bus\n";
            return 329;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine emptyPlaybackEngine;
        neuracoust::daw::AudioEngineSettings emptyPlaybackSettings;
        emptyPlaybackSettings.sampleRate = 100.0;
        emptyPlaybackSettings.bufferSize = 16;
        emptyPlaybackSettings.monitorDspEnabled = true;
        emptyPlaybackSettings.transportRunning = true;
        emptyPlaybackSettings.monitorInputTrimDb = 0.0f;
        emptyPlaybackSettings.monitorVolumeDb = 0.0f;
        auto emptyPlaybackProject = neuracoust::daw::defaultProject();
        emptyPlaybackProject.sampleRate = 100.0;
        emptyPlaybackProject.defaultBufferSize = 16;
        emptyPlaybackProject.clips.clear();
        std::string error;
        if (!emptyPlaybackEngine.loadProject(emptyPlaybackProject, error)) {
            std::cerr << "Could not stage empty project before realtime start: " << error << "\n";
            return 330;
        }
        emptyPlaybackEngine.seek(0.0);
        emptyPlaybackEngine.resetRuntime();
        if (!emptyPlaybackEngine.configure(emptyPlaybackSettings, emptyPlaybackSettings.bufferSize, error)) {
            std::cerr << "Could not configure empty realtime playback engine: " << error << "\n";
            return 331;
        }
        std::vector<float> emptyPlaybackBlock;
        emptyPlaybackEngine.renderInterleavedStereo(16, emptyPlaybackBlock);
        emptyPlaybackEngine.renderInterleavedStereo(16, emptyPlaybackBlock);
        const auto emptyPlaybackStatus = emptyPlaybackEngine.statusSnapshot();
        if (!emptyPlaybackStatus.transportRunning ||
            emptyPlaybackStatus.playbackSeconds < 0.30 ||
            emptyPlaybackBlock.size() != 32) {
            std::cerr << "Empty realtime playback did not advance transport after start: running="
                      << emptyPlaybackStatus.transportRunning
                      << " seconds=" << emptyPlaybackStatus.playbackSeconds
                      << " block=" << emptyPlaybackBlock.size() << "\n";
            return 332;
        }
        const float emptyPlaybackPeak = std::accumulate(
            emptyPlaybackBlock.begin(),
            emptyPlaybackBlock.end(),
            0.0f,
            [](float peak, float sample) { return std::max(peak, std::abs(sample)); });
        if (emptyPlaybackPeak > 0.0001f ||
            emptyPlaybackStatus.outputPeakLeft > 0.0001f ||
            emptyPlaybackStatus.outputPeakRight > 0.0001f) {
            std::cerr << "Empty realtime playback should advance silently without creating audio: "
                      << emptyPlaybackPeak << "\n";
            return 333;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine automationEngine;
        neuracoust::daw::AudioEngineSettings automationSettings;
        automationSettings.sampleRate = 100.0;
        automationSettings.bufferSize = 16;
        automationSettings.monitorDspEnabled = false;
        automationSettings.monitorInputTrimDb = 0.0f;
        automationSettings.monitorVolumeDb = 0.0f;
        std::string error;
        if (!automationEngine.configure(automationSettings, automationSettings.bufferSize, error)) {
            std::cerr << "Could not configure automation DSP engine: " << error << "\n";
            return 26;
        }

        neuracoust::daw::WavAudioData source;
        source.channels = 2;
        source.sampleRate = 100;
        source.interleavedSamples.assign(200, 0.5f);
        const auto sourcePath = (smokeTempRoot / "neuracoust-daw-automation-render.wav").string();
        if (!neuracoust::daw::writePcm16WavFile(sourcePath, source, error)) {
            std::cerr << "Could not write automation source WAV: " << error << "\n";
            return 27;
        }

        auto automationProject = neuracoust::daw::defaultProject();
        automationProject.sampleRate = 100.0;
        automationProject.monitorInputTrimDb = 0.0f;
        automationProject.monitorVolumeDb = 0.0f;
        automationProject.tracks[0].volumeDb = 0.0f;
        automationProject.tracks[0].pan = 0.0f;
        automationProject.clips.push_back({"automation-clip", "Audio 1", sourcePath, 0.0, 1.0, 0.0, 0.0f});
        if (!neuracoust::daw::setTrackVolumeAutomationPoint(automationProject, "Audio 1", 0.0, 0.0f) ||
            !neuracoust::daw::setTrackVolumeAutomationPoint(automationProject, "Audio 1", 1.0, -6.0f) ||
            !neuracoust::daw::setTrackAutomationLanePoint(automationProject, "Audio 1", "track.pan", "Pan", 0.0, -1.0f) ||
            !neuracoust::daw::setTrackAutomationLanePoint(automationProject, "Audio 1", "track.pan", "Pan", 1.0, 1.0f) ||
            !automationEngine.loadProject(automationProject, error)) {
            std::cerr << "Could not load automation render project: " << error << "\n";
            return 28;
        }
        std::vector<float> renderBlock;
        automationEngine.renderInterleavedStereo(100, renderBlock);
        if (renderBlock.size() != 200 ||
            renderBlock[0] < 0.49f ||
            std::abs(renderBlock[1]) > 0.0001f ||
            renderBlock[198] > 0.01f ||
            renderBlock[199] < 0.24f ||
            renderBlock[199] > 0.26f) {
            std::cerr << "Track volume/pan automation did not shape the realtime DSP render\n";
            return 29;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine sendEngine;
        neuracoust::daw::AudioEngineSettings sendSettings;
        sendSettings.sampleRate = 100.0;
        sendSettings.bufferSize = 16;
        sendSettings.monitorDspEnabled = false;
        sendSettings.monitorInputTrimDb = 0.0f;
        sendSettings.monitorVolumeDb = 0.0f;
        std::string error;
        if (!sendEngine.configure(sendSettings, sendSettings.bufferSize, error)) {
            std::cerr << "Could not configure send routing DSP engine: " << error << "\n";
            return 30;
        }

        neuracoust::daw::WavAudioData source;
        source.channels = 1;
        source.sampleRate = 100;
        source.interleavedSamples.assign(100, 0.5f);
        const auto sourcePath = (smokeTempRoot / "neuracoust-daw-send-render.wav").string();
        if (!neuracoust::daw::writePcm16WavFile(sourcePath, source, error)) {
            std::cerr << "Could not write send source WAV: " << error << "\n";
            return 31;
        }

        std::vector<float> renderBlock;
        auto directMasterProject = neuracoust::daw::defaultProject();
        directMasterProject.sampleRate = 100.0;
        directMasterProject.monitorInputTrimDb = 0.0f;
        directMasterProject.monitorVolumeDb = 0.0f;
        directMasterProject.clips.push_back({"direct-master-clip", "Audio 1", sourcePath, 0.0, 1.0, 0.0, 0.0f});
        auto directMaster = std::find_if(directMasterProject.tracks.begin(), directMasterProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Master";
        });
        if (directMaster == directMasterProject.tracks.end() || directMasterProject.tracks.front().outputBus != "Master") {
            std::cerr << "Default audio tracks are not routed into the Master bus\n";
            return 311;
        }
        directMaster->volumeDb = -6.0f;
        directMaster->pan = -1.0f;
        if (!sendEngine.loadProject(directMasterProject, error)) {
            std::cerr << "Could not load direct Master routing project: " << error << "\n";
            return 312;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.24f ||
            renderBlock[0] > 0.26f ||
            std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Master fader/pan did not affect default Audio -> Master routing\n";
            return 313;
        }
        if (!sendEngine.updateTrackMix("Master", -120.0f, 0.0f)) {
            std::cerr << "Realtime Master fader update was rejected\n";
            return 314;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            std::abs(renderBlock[0]) > 0.0001f ||
            std::abs(renderBlock[1]) > 0.0001f) {
            std::cerr << "Realtime Master fader update did not mute Audio -> Master routing\n";
            return 315;
        }
        if (!sendEngine.updateTrackMix("Master", 0.0f, 1.0f)) {
            std::cerr << "Realtime Master balance update was rejected\n";
            return 316;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            std::abs(renderBlock[0]) > 0.0001f ||
            renderBlock[1] < 0.49f) {
            std::cerr << "Realtime Master balance update did not affect Audio -> Master routing\n";
            return 317;
        }

        auto soloBusFolderProject = neuracoust::daw::defaultProject();
        soloBusFolderProject.sampleRate = 100.0;
        soloBusFolderProject.monitorInputTrimDb = 0.0f;
        soloBusFolderProject.monitorVolumeDb = 0.0f;
        soloBusFolderProject.tracks.front().solo = true;
        soloBusFolderProject.tracks.front().outputBus = "Bus 3-4";
        neuracoust::daw::TrackState competingTrack;
        competingTrack.name = "Audio 2";
        competingTrack.trackType = "audio";
        competingTrack.inputBus = "Input 2";
        competingTrack.outputBus = "Master";
        neuracoust::daw::TrackState busFolderTrack;
        busFolderTrack.name = "Stem Bus";
        busFolderTrack.trackType = "bus_folder";
        busFolderTrack.inputBus = "Bus 3-4";
        busFolderTrack.outputBus = "Master";
        auto soloMasterIt = std::find_if(soloBusFolderProject.tracks.begin(), soloBusFolderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Master";
        });
        soloBusFolderProject.tracks.insert(soloMasterIt, busFolderTrack);
        soloMasterIt = std::find_if(soloBusFolderProject.tracks.begin(), soloBusFolderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Master";
        });
        soloBusFolderProject.tracks.insert(soloMasterIt, competingTrack);
        soloBusFolderProject.clips.push_back({"solo-bus-folder-clip", "Audio 1", sourcePath, 0.0, 1.0, 0.0, 0.0f});
        soloBusFolderProject.clips.push_back({"solo-muted-competitor-clip", "Audio 2", sourcePath, 0.0, 1.0, 0.0, 0.0f});
        if (!sendEngine.loadProject(soloBusFolderProject, error)) {
            std::cerr << "Could not load solo bus folder routing project: " << error << "\n";
            return 318;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.49f ||
            renderBlock[0] > 0.51f ||
            renderBlock[1] < 0.49f ||
            renderBlock[1] > 0.51f) {
            std::cerr << "Soloed track did not pass through its bus folder route, or unsoloed audio leaked: "
                      << (renderBlock.size() > 0 ? renderBlock[0] : -999.0f)
                      << ", "
                      << (renderBlock.size() > 1 ? renderBlock[1] : -999.0f)
                      << "\n";
            return 319;
        }

        auto sendProject = neuracoust::daw::defaultProject();
        sendProject.sampleRate = 100.0;
        sendProject.monitorInputTrimDb = 0.0f;
        sendProject.monitorVolumeDb = 0.0f;
        sendProject.tracks[0].outputBus.clear();
        sendProject.tracks[0].volumeDb = -12.0f;
        sendProject.tracks[0].sends.push_back({"Bus 1-2", 0.0f, 0.0f, true, false, true});
        neuracoust::daw::TrackState auxTrack;
        auxTrack.name = "Aux 1";
        auxTrack.inputBus = "Bus 1-2";
        auxTrack.outputBus = "Main 1-2";
        sendProject.tracks.push_back(auxTrack);
        sendProject.clips.push_back({"send-clip", "Audio 1", sourcePath, 0.0, 1.0, 0.0, 0.0f});
        if (!sendEngine.loadProject(sendProject, error)) {
            std::cerr << "Could not load post-fader send project: " << error << "\n";
            return 32;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.12f ||
            renderBlock[0] > 0.13f ||
            renderBlock[1] < 0.12f ||
            renderBlock[1] > 0.13f) {
            std::cerr << "Post-fader send did not follow track fader into the Aux bus: "
                      << (renderBlock.size() > 0 ? renderBlock[0] : -999.0f)
                      << ", "
                      << (renderBlock.size() > 1 ? renderBlock[1] : -999.0f)
                      << "\n";
            return 33;
        }
        const auto sendMeterStatus = sendEngine.statusSnapshot();
        const auto hasTrackMeter = [&](const std::string& name) {
            return std::find(sendMeterStatus.trackMeterNames.begin(), sendMeterStatus.trackMeterNames.end(), name) != sendMeterStatus.trackMeterNames.end();
        };
        if (!hasTrackMeter("Audio 1") ||
            !hasTrackMeter("Aux 1") ||
            hasTrackMeter("Master") ||
            hasTrackMeter("Monitor") ||
            sendMeterStatus.trackMeterNames.size() != sendMeterStatus.trackPeakLeft.size() ||
            sendMeterStatus.trackMeterNames.size() != sendMeterStatus.trackPeakRight.size()) {
            std::cerr << "Track meters did not expose only editable audio/Aux channels\n";
            return 331;
        }
        const auto auxMeterIndexFor = [](const neuracoust::daw::AudioEngineStatus& status) -> size_t {
            const auto auxIt = std::find(status.trackMeterNames.begin(), status.trackMeterNames.end(), "Aux 1");
            return auxIt == status.trackMeterNames.end()
                ? static_cast<size_t>(-1)
                : static_cast<size_t>(std::distance(status.trackMeterNames.begin(), auxIt));
        };
        auto quietSend = sendProject.tracks[0].sends[0];
        quietSend.gainDb = -60.0f;
        if (!sendEngine.updateTrackSendSlot("Audio 1", 0, quietSend)) {
            std::cerr << "Realtime send gain update was rejected\n";
            return 332;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        const auto quietSendStatus = sendEngine.statusSnapshot();
        const size_t quietAuxMeterIndex = auxMeterIndexFor(quietSendStatus);
        if (renderBlock.size() != 2 ||
            std::abs(renderBlock[0]) > 0.001f ||
            std::abs(renderBlock[1]) > 0.001f ||
            quietAuxMeterIndex == static_cast<size_t>(-1) ||
            quietAuxMeterIndex >= quietSendStatus.trackPeakLeft.size() ||
            quietAuxMeterIndex >= quietSendStatus.trackPeakRight.size() ||
            quietSendStatus.trackPeakLeft[quietAuxMeterIndex] > 0.001f ||
            quietSendStatus.trackPeakRight[quietAuxMeterIndex] > 0.001f) {
            std::cerr << "Realtime send gain update did not immediately quiet Aux output and meter\n";
            return 333;
        }
        auto loudSend = quietSend;
        loudSend.gainDb = 12.0f;
        if (!sendEngine.updateTrackSendSlot("Audio 1", 0, loudSend)) {
            std::cerr << "Realtime send boost update was rejected\n";
            return 334;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        const auto loudSendStatus = sendEngine.statusSnapshot();
        const size_t loudAuxMeterIndex = auxMeterIndexFor(loudSendStatus);
        if (renderBlock.size() != 2 ||
            renderBlock[0] < 0.49f ||
            renderBlock[1] < 0.49f ||
            loudAuxMeterIndex == static_cast<size_t>(-1) ||
            loudAuxMeterIndex >= loudSendStatus.trackPeakLeft.size() ||
            loudAuxMeterIndex >= loudSendStatus.trackPeakRight.size() ||
            loudSendStatus.trackPeakLeft[loudAuxMeterIndex] < 0.49f ||
            loudSendStatus.trackPeakRight[loudAuxMeterIndex] < 0.49f) {
            std::cerr << "Realtime send boost update did not immediately raise Aux output and meter\n";
            return 335;
        }
        auto pannedSend = loudSend;
        pannedSend.pan = 1.0f;
        if (!sendEngine.updateTrackSendSlot("Audio 1", 0, pannedSend)) {
            std::cerr << "Realtime send pan update was rejected\n";
            return 336;
        }
        sendEngine.renderInterleavedStereo(1, renderBlock);
        const auto pannedSendStatus = sendEngine.statusSnapshot();
        const size_t pannedAuxMeterIndex = auxMeterIndexFor(pannedSendStatus);
        if (renderBlock.size() != 2 ||
            std::abs(renderBlock[0]) > 0.001f ||
            renderBlock[1] < 0.49f ||
            pannedAuxMeterIndex == static_cast<size_t>(-1) ||
            pannedAuxMeterIndex >= pannedSendStatus.trackPeakLeft.size() ||
            pannedAuxMeterIndex >= pannedSendStatus.trackPeakRight.size() ||
            pannedSendStatus.trackPeakLeft[pannedAuxMeterIndex] > 0.001f ||
            pannedSendStatus.trackPeakRight[pannedAuxMeterIndex] < 0.49f) {
            std::cerr << "Realtime send pan update did not immediately update Aux output and meter\n";
            return 337;
        }
        sendProject.tracks[0].sends[0].preFader = true;
        if (!sendEngine.loadProject(sendProject, error)) {
            std::cerr << "Could not load pre-fader send project: " << error << "\n";
            return 34;
        }
	        sendEngine.renderInterleavedStereo(1, renderBlock);
	        if (renderBlock[0] < 0.49f || renderBlock[1] < 0.49f) {
	            std::cerr << "Pre-fader send incorrectly followed the track fader\n";
	            return 35;
	        }
	        sendProject.tracks[0].sends[0].stereo = false;
	        sendProject.tracks[0].sends[0].pan = -1.0f;
	        if (!sendEngine.loadProject(sendProject, error)) {
	            std::cerr << "Could not load mono pre-fader send project: " << error << "\n";
	            return 36;
	        }
	        sendEngine.renderInterleavedStereo(1, renderBlock);
	        if (renderBlock[0] < 0.24f ||
	            renderBlock[0] > 0.26f ||
	            renderBlock[1] < 0.24f ||
	            renderBlock[1] > 0.26f) {
	            std::cerr << "Mono send did not fold the panned bus feed to center\n";
	            return 37;
	        }
	        sendProject.tracks[0].sends[0].stereo = true;
	        sendProject.tracks[0].sends[0].pan = 1.0f;
	        if (!sendEngine.loadProject(sendProject, error)) {
	            std::cerr << "Could not load panned stereo send project: " << error << "\n";
	            return 38;
	        }
	        sendEngine.renderInterleavedStereo(1, renderBlock);
	        if (std::abs(renderBlock[0]) > 0.0001f || renderBlock[1] < 0.49f) {
	            std::cerr << "Stereo send pan did not route the send feed to the expected side\n";
	            return 39;
	        }
	        auto masterIt = std::find_if(sendProject.tracks.begin(), sendProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
	            return track.name == "Master";
	        });
	        if (masterIt == sendProject.tracks.end()) {
	            std::cerr << "Send routing project has no Master track\n";
	            return 40;
	        }
	        masterIt->volumeDb = -6.0f;
	        masterIt->pan = 0.0f;
	        if (!sendEngine.loadProject(sendProject, error)) {
	            std::cerr << "Could not load mastered send project: " << error << "\n";
	            return 41;
	        }
	        sendEngine.renderInterleavedStereo(1, renderBlock);
	        if (std::abs(renderBlock[0]) > 0.0001f || renderBlock[1] < 0.24f || renderBlock[1] > 0.26f) {
	            std::cerr << "Master bus gain did not trim the Aux-routed send output\n";
	            return 42;
	        }
	        masterIt->volumeDb = 0.0f;
	        sendProject.tracks[0].sends[0].pan = 0.0f;
	        sendProject.tracks[0].sends[0].enabled = false;
	        if (!sendEngine.loadProject(sendProject, error)) {
	            std::cerr << "Could not load disabled send project: " << error << "\n";
	            return 43;
	        }
	        sendEngine.renderInterleavedStereo(1, renderBlock);
	        if (std::any_of(renderBlock.begin(), renderBlock.end(), [](float sample) { return std::abs(sample) > 0.0001f; })) {
	            std::cerr << "No Output track leaked audio after disabling its only send\n";
	            return 44;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine clickEngine;
        neuracoust::daw::AudioEngineSettings clickSettings;
        clickSettings.sampleRate = 1000.0;
        clickSettings.bufferSize = 2200;
        clickSettings.monitorDspEnabled = false;
        clickSettings.monitorInputTrimDb = 0.0f;
        clickSettings.monitorVolumeDb = 0.0f;
        clickSettings.metronomeEnabled = true;
        clickSettings.tempoBpm = 120;
        clickSettings.tempoMap = {{0.0, 120.0}, {1.0, 60.0}};
        std::string error;
        if (!clickEngine.configure(clickSettings, clickSettings.bufferSize, error)) {
            std::cerr << "Could not configure tempo-map click DSP engine: " << error << "\n";
            return 50;
        }
        std::vector<float> clickBlock;
        clickEngine.renderInterleavedStereo(2200, clickBlock);
        auto peakInFrameRange = [&](int firstFrame, int lastFrame) {
            float peak = 0.0f;
            for (int frame = std::max(0, firstFrame); frame <= lastFrame && frame * 2 + 1 < static_cast<int>(clickBlock.size()); ++frame) {
                peak = std::max(peak, std::abs(clickBlock[static_cast<size_t>(frame * 2)]));
                peak = std::max(peak, std::abs(clickBlock[static_cast<size_t>(frame * 2 + 1)]));
            }
            return peak;
        };
        if (peakInFrameRange(990, 1030) > 0.01f || peakInFrameRange(1490, 1530) < 0.02f) {
            std::cerr << "Tempo-map click did not preserve interpolated beat phase across the 60 BPM segment\n";
            return 51;
        }
        clickEngine.seek(0.0);
        clickEngine.setMetronomeEnabled(true, 120, {{0.0, 120.0}});
        clickEngine.renderInterleavedStereo(2200, clickBlock);
        if (peakInFrameRange(1490, 1530) < 0.02f) {
            std::cerr << "Live metronome tempo-map update did not restore the 120 BPM click grid\n";
            return 52;
        }

        clickEngine.seek(0.0);
        clickEngine.setMetronomeEnabled(true, 120, {{0.0, 120.0}, {0.75, 60.0}});
        clickEngine.renderInterleavedStereo(2200, clickBlock);
        if (peakInFrameRange(740, 780) > 0.01f || peakInFrameRange(1610, 1660) < 0.02f) {
            std::cerr << "Tempo-map click reset at a non-beat tempo marker instead of preserving beat phase\n";
            return 53;
        }

        auto directClickPeak = [&](const neuracoust::daw::AudioEngineSettings& settings, int firstFrame, int lastFrame) {
            float peak = 0.0f;
            for (int frame = std::max(0, firstFrame); frame <= lastFrame; ++frame) {
                peak = std::max(peak, std::abs(neuracoust::daw::renderMetronomeClickSampleAtFrame(frame, settings)));
            }
            return peak;
        };
        neuracoust::daw::AudioEngineSettings directClickSettings;
        directClickSettings.sampleRate = 1000.0;
        directClickSettings.metronomeEnabled = true;
        directClickSettings.tempoBpm = 120;
        directClickSettings.tempoMap = {{0.0, 120.0}, {0.75, 60.0}};
        if (directClickPeak(directClickSettings, 740, 780) > 0.01f ||
            directClickPeak(directClickSettings, 1610, 1660) < 0.02f) {
            std::cerr << "Shared metronome click math did not preserve beat phase across a non-beat tempo marker\n";
            return 54;
        }
        directClickSettings.tempoMap = {{0.0, 120.0}};
        directClickSettings.timeSignatureNumerator = 12;
        directClickSettings.timeSignatureDenominator = 8;
        const float directWeakPeak = directClickPeak(directClickSettings, 490, 530);
        const float directFourthPulsePeak = directClickPeak(directClickSettings, 2240, 2280);
        if (directWeakPeak < 0.005f || directFourthPulsePeak < directWeakPeak * 1.35f) {
            std::cerr << "Shared metronome click math did not accent 12/8 compound pulses\n";
            return 55;
        }

        clickEngine.seek(0.0);
        clickSettings.timeSignatureNumerator = 6;
        clickSettings.timeSignatureDenominator = 8;
        clickSettings.tempoMap = {{0.0, 120.0}};
        if (!clickEngine.configure(clickSettings, clickSettings.bufferSize, error)) {
            std::cerr << "Could not configure 6/8 click DSP engine: " << error << "\n";
            return 56;
        }
        clickEngine.renderInterleavedStereo(1600, clickBlock);
        const float weakEighthPeak = peakInFrameRange(490, 530);
        const float compoundAccentPeak = peakInFrameRange(740, 780);
        if (weakEighthPeak < 0.005f || compoundAccentPeak < weakEighthPeak * 1.35f) {
            std::cerr << "6/8 metronome did not accent the second compound pulse above weak eighth notes\n";
            return 57;
        }
        for (const auto compoundNumerator : {9, 12}) {
            clickEngine.seek(0.0);
            clickSettings.timeSignatureNumerator = compoundNumerator;
            clickSettings.timeSignatureDenominator = 8;
            clickSettings.grooveFeel = "straight";
            clickSettings.grooveSwingAmount = 0.0;
            clickSettings.tempoMap = {{0.0, 120.0}};
            if (!clickEngine.configure(clickSettings, clickSettings.bufferSize, error)) {
                std::cerr << "Could not configure " << compoundNumerator << "/8 click DSP engine: " << error << "\n";
                return 58;
            }
            clickEngine.renderInterleavedStereo(3200, clickBlock);
            const float weakPeak = peakInFrameRange(490, 530);
            const float secondPulsePeak = peakInFrameRange(740, 780);
            const float thirdPulsePeak = peakInFrameRange(1490, 1530);
            if (weakPeak < 0.005f ||
                secondPulsePeak < weakPeak * 1.35f ||
                thirdPulsePeak < weakPeak * 1.35f) {
                std::cerr << compoundNumerator << "/8 metronome did not accent compound pulses above weak eighth notes\n";
                return 59;
            }
            if (compoundNumerator == 12) {
                const float fourthPulsePeak = peakInFrameRange(2240, 2280);
                if (fourthPulsePeak < weakPeak * 1.35f) {
                    std::cerr << "12/8 metronome did not accent the fourth compound pulse above weak eighth notes\n";
                    return 60;
                }
            }
        }

        clickEngine.seek(0.0);
        clickSettings.timeSignatureNumerator = 6;
        clickSettings.timeSignatureDenominator = 8;
        clickSettings.grooveFeel = "shuffle";
        clickSettings.grooveSwingAmount = 0.57;
        if (!clickEngine.configure(clickSettings, clickSettings.bufferSize, error)) {
            std::cerr << "Could not configure shuffled 6/8 click DSP engine: " << error << "\n";
            return 59;
        }
        clickEngine.renderInterleavedStereo(800, clickBlock);
        if (peakInFrameRange(244, 270) > 0.01f || peakInFrameRange(278, 310) < 0.02f) {
            std::cerr << "Shuffled 6/8 metronome did not move the offbeat eighth click to the swing position\n";
            return 60;
        }

        clickEngine.seek(0.0);
        clickEngine.setMetronomeEnabled(true, 120, {{0.0, 120.0}}, 6, 8, "straight", 0.0);
        clickEngine.renderInterleavedStereo(1600, clickBlock);
        const float liveWeakEighthPeak = peakInFrameRange(490, 530);
        const float liveCompoundAccentPeak = peakInFrameRange(740, 780);
        if (liveWeakEighthPeak < 0.005f || liveCompoundAccentPeak < liveWeakEighthPeak * 1.35f) {
            std::cerr << "Live metronome update did not apply the project 6/8 accent pattern\n";
            return 61;
        }

        directClickSettings.grooveFeel = "straight";
        directClickSettings.grooveSwingAmount = 0.0;
        directClickSettings.timeSignatureNumerator = 4;
        directClickSettings.timeSignatureDenominator = 4;
        directClickSettings.timeSignatureMap = {{0.0, 4, 4}, {1.0, 6, 8}};
        directClickSettings.tempoMap = {{0.0, 120.0}};
        const float mappedWeakEighthPeak = directClickPeak(directClickSettings, 1248, 1285);
        const float mappedCompoundAccentPeak = directClickPeak(directClickSettings, 1748, 1785);
        if (mappedWeakEighthPeak < 0.005f || mappedCompoundAccentPeak < mappedWeakEighthPeak * 1.35f) {
            std::cerr << "Shared metronome click math did not follow the time-signature map after a 6/8 marker\n";
            return 63;
        }

        clickEngine.seek(0.0);
        clickEngine.setMetronomeEnabled(true,
                                        120,
                                        {{0.0, 120.0}},
                                        4,
                                        4,
                                        "straight",
                                        0.0,
                                        {{0.0, 4, 4}, {1.0, 6, 8}});
        clickEngine.renderInterleavedStereo(2200, clickBlock);
        const float liveMappedWeakEighthPeak = peakInFrameRange(1248, 1285);
        const float liveMappedCompoundAccentPeak = peakInFrameRange(1748, 1785);
        if (liveMappedWeakEighthPeak < 0.005f || liveMappedCompoundAccentPeak < liveMappedWeakEighthPeak * 1.35f) {
            std::cerr << "Live metronome update did not follow the project time-signature map after a 6/8 marker\n";
            return 64;
        }

        clickEngine.seek(0.0);
        clickEngine.setMetronomeEnabled(true, 120, {{0.0, 120.0}}, 6, 8, "shuffle", 0.57);
        clickEngine.renderInterleavedStereo(800, clickBlock);
        if (peakInFrameRange(244, 270) > 0.01f || peakInFrameRange(278, 310) < 0.02f) {
            std::cerr << "Live metronome update did not apply the project shuffle feel\n";
            return 65;
        }

        clickEngine.seek(0.0);
        clickEngine.setMetronomeEnabled(true, 120, {{0.0, 120.0}}, 4, 4, "straight", 0.0, {}, "sixteenth");
        clickEngine.renderInterleavedStereo(700, clickBlock);
        if (peakInFrameRange(118, 132) < 0.02f ||
            peakInFrameRange(243, 257) < 0.02f ||
            peakInFrameRange(368, 382) < 0.02f) {
            std::cerr << "16th-note metronome subdivision did not add dense click pulses\n";
            return 66;
        }

        clickEngine.seek(0.0);
        clickSettings.grooveFeel = "triplet";
        clickSettings.grooveSwingAmount = 1.0 / 3.0;
        if (!clickEngine.configure(clickSettings, clickSettings.bufferSize, error)) {
            std::cerr << "Could not configure triplet 6/8 click DSP engine: " << error << "\n";
            return 67;
        }
        clickEngine.renderInterleavedStereo(900, clickBlock);
        if (peakInFrameRange(244, 270) > 0.01f || peakInFrameRange(322, 360) < 0.02f) {
            std::cerr << "Triplet 6/8 metronome did not move the offbeat eighth click to the triplet position\n";
            return 68;
        }
    }

    {
        neuracoust::daw::NeuracoustDspEngine dspEngine;
        neuracoust::daw::AudioEngineSettings dspSettings;
        dspSettings.sampleRate = 48000.0;
        dspSettings.bufferSize = 64;
        dspSettings.inputMonitorChannelCount = 0;
        dspSettings.monitorDspEnabled = false;
        std::string error;
        if (!dspEngine.configure(dspSettings, dspSettings.bufferSize, error)) {
            std::cerr << "Could not configure No Input DSP engine: " << error << "\n";
            return 23;
        }
        auto noInputProject = neuracoust::daw::defaultProject();
        noInputProject.tracks[0].recordArmed = true;
        noInputProject.tracks[0].inputBus.clear();
        if (!dspEngine.loadProject(noInputProject, error)) {
            std::cerr << "Could not load No Input monitor policy project: " << error << "\n";
            return 24;
        }
        std::vector<float> inputBlock(128, 0.5f);
        dspEngine.pushInputMonitorInterleaved(inputBlock.data(), 64, 2);
        std::vector<float> renderBlock;
        dspEngine.renderInterleavedStereo(64, renderBlock);
        const auto dspStatus = dspEngine.statusSnapshot();
        if (dspStatus.lowLatencyRecordMonitoringActive ||
            dspStatus.physicalInputMonitoringActive ||
            dspStatus.recordArmedTrackCount != 1 ||
            dspStatus.inputChannels != 0 ||
            std::any_of(renderBlock.begin(), renderBlock.end(), [](float sample) { return std::abs(sample) > 0.0001f; })) {
            std::cerr << "No Input track unexpectedly activated the physical monitor path\n";
            return 25;
        }
    }

#if defined(_WIN32)
    {
        neuracoust::daw::RealtimeAudioEngine asioEngine;
        neuracoust::daw::AudioEngineSettings asioSettings;
        asioSettings.outputDriver = neuracoust::daw::AudioDriverKind::ASIO;
        asioSettings.outputDeviceId = "asio:test-driver";
        if (asioEngine.start(asioSettings)) {
            std::cerr << "ASIO placeholder engine unexpectedly started without runtime adapter\n";
            asioEngine.stop();
            return 6;
        }
        const auto asioStatus = asioEngine.status();
        if (asioStatus.outputDriver != neuracoust::daw::AudioDriverKind::ASIO ||
            asioStatus.message.find("ASIO runtime adapter") == std::string::npos ||
            asioStatus.message.find("ASIO diagnostic") == std::string::npos) {
            std::cerr << "ASIO placeholder status did not report adapter requirement: " << asioStatus.message << "\n";
            return 7;
        }
    }
#endif

    {
        neuracoust::daw::RecordingTake take(1, 48000);
        const int16_t samples[16] = {0, 256, -256, 512, -512, 768, -768, 1024, -1024, 768, -768, 512, -512, 256, -256, 0};
        take.appendInterleavedInt16(samples, 16);
        std::string error;
        const auto sourcePath = (smokeTempRoot / "neuracoust-daw-realtime-vst3-guard.wav").string();
        if (!take.saveWav(sourcePath, error)) {
            std::cerr << "Could not create realtime VST3 guard source WAV: " << error << "\n";
            return 8;
        }
        if (std::filesystem::exists(std::filesystem::path(sourcePath + ".writing"))) {
            std::cerr << "Recording take writer left a temporary WAV file behind\n";
            return 8;
        }
        auto guardedProject = neuracoust::daw::defaultProject();
        guardedProject.clips.push_back({"guard-clip", "Audio 1", sourcePath, 0.0, 0.001, 0.0, 0.0f});
        guardedProject.masterInserts.push_back({
            "Realtime Guard VST3",
            "external-vst3",
            "VST3",
            "/definitely/not/realtime-safe.vst3",
            false,
            false
        });
        neuracoust::daw::RealtimeAudioEngine guardedEngine;
        if (!guardedEngine.loadProject(guardedProject, error)) {
            std::cerr << "Could not load realtime VST3 guard project: " << error << "\n";
            return 9;
        }
        neuracoust::daw::AudioEngineSettings guardedSettings;
        guardedSettings.sampleRate = 48000.0;
        if (guardedEngine.start(guardedSettings)) {
            std::cerr << "Realtime engine unexpectedly started with an invalid active VST3 insert\n";
            guardedEngine.stop();
            return 10;
        }
        const auto guardedStatus = guardedEngine.status();
        if (guardedStatus.message.find("VST3 realtime insert failed") == std::string::npos) {
            std::cerr << "Realtime VST3 guard reported wrong message: " << guardedStatus.message << "\n";
            return 11;
        }
    }

    neuracoust::daw::RealtimeAudioEngine engine;
    neuracoust::daw::AudioEngineSettings settings;
    settings.testToneEnabled = false;
    settings.sampleRate = 48000.0;
    settings.performanceCoreIsolationEnabled = true;
    settings.windowsProcessorAffinityEnabled = true;
    settings.windowsProcessorAffinityMode = "p_core_preferred";

    if (!engine.start(settings)) {
        const auto status = engine.status();
        std::cerr << "Audio engine did not start: " << status.message << "\n";
        return 2;
    }
    engine.seek(0.05);
    const auto sought = engine.status();
    if (sought.playbackSeconds < 0.049 || sought.playbackSeconds > 0.051) {
        std::cerr << "Audio engine seek did not update playback position: " << sought.playbackSeconds << "\n";
        engine.stop();
        return 5;
    }
    engine.setMetronomeEnabled(true, 120);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto running = engine.status();
    engine.stop();

    std::cout << "Windows Low-Latency DSP Core telemetry: callbacks="
              << running.realtimeCallbackCount
              << " avgWakeJitterUs=" << running.realtimeAverageWakeJitterUs
              << " maxWakeJitterUs=" << running.realtimeMaxWakeJitterUs
              << " maxRenderDurationUs=" << running.realtimeMaxRenderDurationUs
              << " lateWakeCount=" << running.realtimeLateWakeCount
              << " message=\"" << running.message << "\"\n";

    if (!running.running) {
        std::cerr << "Audio engine reported stopped immediately after start\n";
        return 3;
    }
    if (running.realtimeCallbackCount == 0 ||
        running.realtimeMaxRenderDurationUs <= 0.0 ||
        running.realtimeAverageWakeJitterUs < 0.0 ||
        running.realtimeMaxWakeJitterUs < 0.0) {
        std::cerr << "Low-latency DSP Core telemetry did not record render activity\n";
        return 12;
    }
    if (running.outputPeakLeft < 0.0f || running.outputPeakLeft > 1.0f ||
        running.outputPeakRight < 0.0f || running.outputPeakRight > 1.0f) {
        std::cerr << "Audio engine meter values out of range\n";
        return 4;
    }
    if (running.phaseCorrelation < -1.0f || running.phaseCorrelation > 1.0f ||
        running.spectrumLow < 0.0f || running.spectrumLow > 1.0f ||
        running.spectrumMid < 0.0f || running.spectrumMid > 1.0f ||
        running.spectrumHigh < 0.0f || running.spectrumHigh > 1.0f) {
        std::cerr << "Audio engine analysis values out of range\n";
        return 13;
    }
    if (running.playbackSeconds <= sought.playbackSeconds) {
        std::cerr << "Metronome-only transport did not advance playback position: "
                  << running.playbackSeconds << " <= " << sought.playbackSeconds << "\n";
        return 12;
    }

    std::cout << "Audio engine smoke test passed on " << running.deviceName << "\n";
    return 0;
}
