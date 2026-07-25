#include "core/Base64.h"
#include "core/DawState.h"
#include "audio/AsioRuntimeAdapter.h"
#include "audio/MasterInsertProcessor.h"
#include "audio/MixerGraph.h"
#include "audio/MixerProcessorChain.h"
#include "audio/MixMath.h"
#include "audio/OfflineBounce.h"
#include "audio/ProjectAudioRenderer.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/RecordingTake.h"
#include "audio/WavFile.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3ModuleRuntime.h"
#include "plugins/PluginScanner.h"
#include "plugins/Vst3SdkAdapter.h"
#include "project/AafImport.h"
#include "project/AudioImportAnalysis.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"
#include "project/ProjectMediaPool.h"
#include "project/StemMagicBridge.h"
#include "project/TimelineExport.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

void setEnvironmentValue(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

neuracoust::daw::TrackInsertSlot makeTrackInsert(const std::string& name,
                                                 const std::string& format,
                                                 const std::string& path,
                                                 bool bypassed = false,
                                                 bool enabled = true) {
    neuracoust::daw::TrackInsertSlot insert;
    insert.pluginName = name;
    insert.pluginFormat = format;
    insert.pluginPath = path;
    insert.bypassed = bypassed;
    insert.enabled = enabled;
    return insert;
}

neuracoust::daw::InsertState makeMasterInsert(const std::string& name,
                                              const std::string& appId,
                                              const std::string& format,
                                              const std::string& path,
                                              bool bypassed = false,
                                              bool available = true) {
    neuracoust::daw::InsertState insert;
    insert.pluginName = name;
    insert.pluginAppId = appId;
    insert.pluginFormat = format;
    insert.pluginPath = path;
    insert.bypassed = bypassed;
    insert.available = available;
    return insert;
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

neuracoust::daw::WavAudioData makeAnalyzableImportWav() {
    neuracoust::daw::WavAudioData wav;
    wav.channels = 1;
    wav.sampleRate = 8000;
    wav.bitsPerSample = 32;
    wav.floatingPoint = true;
    wav.embeddedTempoBpm = 120.0;
    const double seconds = 8.0;
    const int frames = static_cast<int>(seconds * wav.sampleRate);
    wav.interleavedSamples.assign(static_cast<size_t>(frames), 0.0f);
    const std::array<double, 3> chordFrequencies {261.6256, 329.6276, 391.9954};
    for (int frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / wav.sampleRate;
        double sample = 0.0;
        for (double frequency : chordFrequencies) {
            sample += std::sin(2.0 * kPi * frequency * t) * 0.08;
        }
        wav.interleavedSamples[static_cast<size_t>(frame)] = static_cast<float>(sample);
    }
    const double beatSeconds = 0.5;
    for (int beat = 0; beat < 16; ++beat) {
        const int startFrame = static_cast<int>(std::round(static_cast<double>(beat) * beatSeconds * wav.sampleRate));
        const float accent = beat % 4 == 0 ? 0.95f : 0.45f;
        for (int offset = 0; offset < 64 && startFrame + offset < frames; ++offset) {
            wav.interleavedSamples[static_cast<size_t>(startFrame + offset)] += accent * static_cast<float>(1.0 - static_cast<double>(offset) / 64.0);
        }
    }
    return wav;
}

/// A four-chord progression in one key, for testing key detection with something that actually has
/// a key. A single sustained triad (what makeAnalyzableImportWav gives) cannot distinguish a major
/// key from its relative minor — a cadence can.
neuracoust::daw::WavAudioData makeKeyProgressionWav(const std::array<std::array<int, 3>, 4>& chords) {
    neuracoust::daw::WavAudioData wav;
    wav.channels = 1;
    wav.sampleRate = 8000;
    wav.bitsPerSample = 32;
    wav.floatingPoint = true;
    const double chordSeconds = 2.0;
    const int framesPerChord = static_cast<int>(chordSeconds * wav.sampleRate);
    wav.interleavedSamples.assign(static_cast<size_t>(framesPerChord) * chords.size(), 0.0f);
    for (size_t chordIndex = 0; chordIndex < chords.size(); ++chordIndex) {
        for (int frame = 0; frame < framesPerChord; ++frame) {
            const double t = static_cast<double>(frame) / wav.sampleRate;
            double sample = 0.0;
            for (const int midi : chords[chordIndex]) {
                const double frequency = 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
                sample += std::sin(2.0 * kPi * frequency * t) * 0.12;
            }
            const size_t index = chordIndex * static_cast<size_t>(framesPerChord) + static_cast<size_t>(frame);
            wav.interleavedSamples[index] = static_cast<float>(sample);
        }
    }
    return wav;
}

neuracoust::daw::WavAudioData makeCompoundEighthImportWav(int numerator, double seconds) {
    neuracoust::daw::WavAudioData wav;
    wav.channels = 1;
    wav.sampleRate = 8000;
    wav.bitsPerSample = 32;
    wav.floatingPoint = true;
    wav.embeddedTempoBpm = 120.0;
    const int frames = static_cast<int>(seconds * wav.sampleRate);
    wav.interleavedSamples.assign(static_cast<size_t>(frames), 0.0f);
    const double eighthSeconds = 0.25;
    const int eighthCount = static_cast<int>(seconds / eighthSeconds);
    for (int eighth = 0; eighth < eighthCount; ++eighth) {
        const int startFrame = static_cast<int>(std::round(static_cast<double>(eighth) * eighthSeconds * wav.sampleRate));
        const int position = eighth % std::max(6, numerator);
        const bool pulse = position % 3 == 0;
        const float secondaryPulseAccent = numerator > 6 ? 0.55f : 0.76f;
        const float accent = position == 0 ? 1.0f : (pulse ? secondaryPulseAccent : 0.20f);
        for (int offset = 0; offset < 72 && startFrame + offset < frames; ++offset) {
            const float envelope = static_cast<float>(1.0 - static_cast<double>(offset) / 72.0);
            wav.interleavedSamples[static_cast<size_t>(startFrame + offset)] += accent * envelope;
        }
    }
    return wav;
}

neuracoust::daw::WavAudioData makeCompoundSixEightImportWav() {
    return makeCompoundEighthImportWav(6, 12.0);
}

std::string jsonStringFragment(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += ch; break;
        }
    }
    return result;
}

std::filesystem::path testTempRoot() {
    static const auto root = [] {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = std::filesystem::temp_directory_path() / ("neuracoust-daw-core-smoke-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
        return path;
    }();
    return root;
}

void writeLe16(std::ostream& out, uint16_t value) {
    const uint8_t bytes[2] {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff)
    };
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeLe32(std::ostream& out, uint32_t value) {
    const uint8_t bytes[4] {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>((value >> 16) & 0xff),
        static_cast<uint8_t>((value >> 24) & 0xff)
    };
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writePcm32TestWav(const std::filesystem::path& path) {
    const int channels = 1;
    const int sampleRate = 48000;
    const int bits = 32;
    const int32_t samples[3] {0, 1073741824, -1073741824};
    const uint32_t dataBytes = sizeof(samples);
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    writeLe32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe32(out, 16);
    writeLe16(out, 1);
    writeLe16(out, channels);
    writeLe32(out, sampleRate);
    writeLe32(out, sampleRate * channels * (bits / 8));
    writeLe16(out, channels * (bits / 8));
    writeLe16(out, bits);
    out.write("data", 4);
    writeLe32(out, dataBytes);
    for (const auto sample : samples) {
        writeLe32(out, static_cast<uint32_t>(sample));
    }
}

void writeExtensibleFloat32TestWav(const std::filesystem::path& path) {
    const int channels = 2;
    const int sampleRate = 48000;
    const int bits = 32;
    const float samples[4] {0.25f, -0.25f, 0.5f, -0.5f};
    const uint32_t dataBytes = sizeof(samples);
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    writeLe32(out, 60 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe32(out, 40);
    writeLe16(out, 0xfffe);
    writeLe16(out, channels);
    writeLe32(out, sampleRate);
    writeLe32(out, sampleRate * channels * (bits / 8));
    writeLe16(out, channels * (bits / 8));
    writeLe16(out, bits);
    writeLe16(out, 22);
    writeLe16(out, bits);
    writeLe32(out, 3);
    writeLe32(out, 3);
    writeLe16(out, 0);
    writeLe16(out, 0x0010);
    const uint8_t guidTail[8] {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    out.write(reinterpret_cast<const char*>(guidTail), sizeof(guidTail));
    out.write("data", 4);
    writeLe32(out, dataBytes);
    for (const auto sample : samples) {
        uint32_t raw = 0;
        std::memcpy(&raw, &sample, sizeof(raw));
        writeLe32(out, raw);
    }
}

void writeBwfTimeReferenceTestWav(const std::filesystem::path& path, uint64_t timeReferenceSamples) {
    const int channels = 1;
    const int sampleRate = 48000;
    const int bits = 16;
    const int16_t samples[2] {1200, -1200};
    const uint32_t dataBytes = sizeof(samples);
    constexpr uint32_t bextBytes = 602;
    const uint32_t riffSize = 4 + (8 + 16) + (8 + bextBytes) + (8 + dataBytes);
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    writeLe32(out, riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe32(out, 16);
    writeLe16(out, 1);
    writeLe16(out, channels);
    writeLe32(out, sampleRate);
    writeLe32(out, sampleRate * channels * (bits / 8));
    writeLe16(out, channels * (bits / 8));
    writeLe16(out, bits);
    out.write("bext", 4);
    writeLe32(out, bextBytes);
    std::vector<uint8_t> bext(bextBytes, 0);
    const uint32_t low = static_cast<uint32_t>(timeReferenceSamples & 0xffffffffull);
    const uint32_t high = static_cast<uint32_t>((timeReferenceSamples >> 32) & 0xffffffffull);
    bext[338] = static_cast<uint8_t>(low & 0xff);
    bext[339] = static_cast<uint8_t>((low >> 8) & 0xff);
    bext[340] = static_cast<uint8_t>((low >> 16) & 0xff);
    bext[341] = static_cast<uint8_t>((low >> 24) & 0xff);
    bext[342] = static_cast<uint8_t>(high & 0xff);
    bext[343] = static_cast<uint8_t>((high >> 8) & 0xff);
    bext[344] = static_cast<uint8_t>((high >> 16) & 0xff);
    bext[345] = static_cast<uint8_t>((high >> 24) & 0xff);
    out.write(reinterpret_cast<const char*>(bext.data()), bext.size());
    out.write("data", 4);
    writeLe32(out, dataBytes);
    for (const auto sample : samples) {
        writeLe16(out, static_cast<uint16_t>(sample));
    }
}

int readWavBitsPerSample(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return 0;
    }
    char header[12] {};
    in.read(header, sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::string(header, 4) != "RIFF" ||
        std::string(header + 8, 4) != "WAVE") {
        return 0;
    }
    while (in) {
        char chunkId[4] {};
        in.read(chunkId, 4);
        if (in.gcount() != 4) {
            return 0;
        }
        uint8_t sizeBytes[4] {};
        in.read(reinterpret_cast<char*>(sizeBytes), 4);
        if (in.gcount() != 4) {
            return 0;
        }
        const uint32_t chunkSize = uint32_t(sizeBytes[0]) |
            (uint32_t(sizeBytes[1]) << 8) |
            (uint32_t(sizeBytes[2]) << 16) |
            (uint32_t(sizeBytes[3]) << 24);
        if (std::string(chunkId, 4) == "fmt ") {
            std::vector<uint8_t> fmt(chunkSize);
            in.read(reinterpret_cast<char*>(fmt.data()), chunkSize);
            if (fmt.size() < 16) {
                return 0;
            }
            return int(uint16_t(fmt[14]) | (uint16_t(fmt[15]) << 8));
        }
        in.seekg(chunkSize + (chunkSize & 1u), std::ios::cur);
    }
    return 0;
}

int readWavFormatTag(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return 0;
    }
    char header[12] {};
    in.read(header, sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::string(header, 4) != "RIFF" ||
        std::string(header + 8, 4) != "WAVE") {
        return 0;
    }
    while (in) {
        char chunkId[4] {};
        in.read(chunkId, 4);
        if (in.gcount() != 4) {
            return 0;
        }
        uint8_t sizeBytes[4] {};
        in.read(reinterpret_cast<char*>(sizeBytes), 4);
        if (in.gcount() != 4) {
            return 0;
        }
        const uint32_t chunkSize = uint32_t(sizeBytes[0]) |
            (uint32_t(sizeBytes[1]) << 8) |
            (uint32_t(sizeBytes[2]) << 16) |
            (uint32_t(sizeBytes[3]) << 24);
        if (std::string(chunkId, 4) == "fmt ") {
            std::vector<uint8_t> fmt(chunkSize);
            in.read(reinterpret_cast<char*>(fmt.data()), chunkSize);
            if (fmt.size() < 2) {
                return 0;
            }
            return int(uint16_t(fmt[0]) | (uint16_t(fmt[1]) << 8));
        }
        in.seekg(chunkSize + (chunkSize & 1u), std::ios::cur);
    }
    return 0;
}

} // namespace

int main() {
    {
        neuracoust::daw::MixerRouteProcessorInput processorInput;
        processorInput.input = {0.5f, 0.5f};
        processorInput.routeKind = neuracoust::daw::MixerRouteKind::Audio;
        processorInput.gainDb = -6.0f;
        processorInput.pan = -1.0f;
        processorInput.sends.push_back({"Bus 1-2", 0.0f, 1.0f, true, true, true});
        processorInput.sends.push_back({"Cue Mono", 0.0f, -1.0f, true, true, false});
        const auto processed = neuracoust::daw::processMixerRouteProcessors(processorInput);
        assert(processed.postFader.left > 0.24f && processed.postFader.left < 0.26f);
        assert(std::abs(processed.postFader.right) < 0.0001f);
        assert(processed.sendTaps.size() == 2);
        assert(processed.sendTaps[0].busName == "Bus 1-2");
        assert(std::abs(processed.sendTaps[0].frame.left) < 0.0001f);
        assert(processed.sendTaps[0].frame.right > 0.49f && processed.sendTaps[0].frame.right < 0.51f);
        assert(processed.sendTaps[1].busName == "Cue Mono");
        assert(processed.sendTaps[1].frame.left > 0.24f && processed.sendTaps[1].frame.left < 0.26f);
        assert(processed.sendTaps[1].frame.right > 0.24f && processed.sendTaps[1].frame.right < 0.26f);

        neuracoust::daw::MixerRouteProcessorInput unitySendInput;
        unitySendInput.input = {0.25f, 0.5f};
        unitySendInput.routeKind = neuracoust::daw::MixerRouteKind::Audio;
        unitySendInput.gainDb = 0.0f;
        unitySendInput.pan = 0.0f;
        unitySendInput.sends.push_back({"Unity Bus", 0.0f, 0.0f, true, false, true});
        const auto unitySendProcessed = neuracoust::daw::processMixerRouteProcessors(unitySendInput);
        assert(unitySendProcessed.sendTaps.size() == 1);
        assert(std::abs(unitySendProcessed.sendTaps[0].frame.left - unitySendProcessed.postFader.left) < 0.0001f);
        assert(std::abs(unitySendProcessed.sendTaps[0].frame.right - unitySendProcessed.postFader.right) < 0.0001f);
    }

    {
        auto mixerProject = neuracoust::daw::defaultProject();
        mixerProject.tracks[0].sends.push_back({"Bus 1-2", -6.0f, 0.25f, true, false, true});
        neuracoust::daw::TrackState aux;
        aux.name = "Aux 1";
        aux.trackType = "aux";
        aux.inputBus = "Bus 1-2";
        aux.outputBus = "Master";
        mixerProject.tracks.insert(mixerProject.tracks.end() - 2, aux);
        neuracoust::daw::TrackState routingFolder;
        routingFolder.name = "Drum Folder";
        routingFolder.trackType = "bus_folder";
        routingFolder.inputBus = "Drum Bus";
        routingFolder.outputBus = "Master";
        mixerProject.tracks.insert(mixerProject.tracks.end() - 2, routingFolder);
        neuracoust::daw::TrackState vca;
        vca.name = "VCA 1";
        vca.trackType = "vca";
        mixerProject.tracks.insert(mixerProject.tracks.end() - 2, vca);
        neuracoust::daw::TrackState instrument;
        instrument.name = "Instrument 1";
        instrument.trackType = "instrument";
        instrument.inputBus = "MIDI Input";
        instrument.outputBus = "Master";
        instrument.instrument.pluginName = "Test Synth";
        instrument.instrument.pluginFormat = "VST3";
        instrument.instrument.pluginPath = "/tmp/TestSynth.vst3";
        instrument.instrument.enabled = true;
        mixerProject.tracks.insert(mixerProject.tracks.end() - 2, instrument);
        mixerProject.tracks[0].controlMasterTrackName = "VCA 1";

        const auto graph = neuracoust::daw::buildMixerGraph(mixerProject);
        auto routeKind = [&](const std::string& routeName) {
            const auto it = std::find_if(graph.routes.begin(), graph.routes.end(), [&](const neuracoust::daw::MixerRouteNode& route) {
                return route.name == routeName;
            });
            assert(it != graph.routes.end());
            return it->kind;
        };
        assert(routeKind("Audio 1") == neuracoust::daw::MixerRouteKind::Audio);
        assert(routeKind("Aux 1") == neuracoust::daw::MixerRouteKind::Aux);
        assert(routeKind("Drum Folder") == neuracoust::daw::MixerRouteKind::RoutingFolder);
        assert(routeKind("VCA 1") == neuracoust::daw::MixerRouteKind::Vca);
        assert(routeKind("Instrument 1") == neuracoust::daw::MixerRouteKind::Instrument);
        assert(routeKind("Master") == neuracoust::daw::MixerRouteKind::Master);
        assert(routeKind("Monitor") == neuracoust::daw::MixerRouteKind::Monitor);
        const auto instrumentRoute = std::find_if(graph.routes.begin(), graph.routes.end(), [](const neuracoust::daw::MixerRouteNode& route) {
            return route.name == "Instrument 1";
        });
        assert(instrumentRoute != graph.routes.end());
        assert(instrumentRoute->audioCarrying);
        assert(!instrumentRoute->controlOnly);
        assert(instrumentRoute->processors.size() >= 2);
        assert(instrumentRoute->processors[0].kind == neuracoust::daw::MixerProcessorKind::MidiSource);
        assert(instrumentRoute->processors[1].kind == neuracoust::daw::MixerProcessorKind::Instrument);
        assert(instrumentRoute->processors[1].label == "Test Synth");

        const auto sendEdge = std::find_if(graph.edges.begin(), graph.edges.end(), [](const neuracoust::daw::MixerGraphEdge& edge) {
            return edge.sourceRoute == "Audio 1" && edge.destinationRoute == "Aux 1" && edge.send && edge.busName == "Bus 1-2";
        });
        assert(sendEdge != graph.edges.end());
        assert(std::abs(sendEdge->gainDb - -6.0f) < 0.0001f);
        const auto masterEdge = std::find_if(graph.edges.begin(), graph.edges.end(), [](const neuracoust::daw::MixerGraphEdge& edge) {
            return edge.sourceRoute == "Master" && edge.destinationRoute == "Monitor" && !edge.send;
        });
        assert(masterEdge != graph.edges.end());
        const auto monitorHardwareEdge = std::find_if(graph.edges.begin(), graph.edges.end(), [](const neuracoust::daw::MixerGraphEdge& edge) {
            return edge.sourceRoute == "Monitor" && edge.destinationRoute == "Main 1-2" && edge.physicalOutput;
        });
        assert(monitorHardwareEdge != graph.edges.end());
        const auto vcaEdge = std::find_if(graph.controlEdges.begin(), graph.controlEdges.end(), [](const neuracoust::daw::MixerControlEdge& edge) {
            return edge.controlRoute == "VCA 1" && edge.targetRoute == "Audio 1";
        });
        assert(vcaEdge != graph.controlEdges.end());
        assert(std::find(graph.renderOrder.begin(), graph.renderOrder.end(), "Audio 1") <
            std::find(graph.renderOrder.begin(), graph.renderOrder.end(), "Master"));
        assert(std::find(graph.renderOrder.begin(), graph.renderOrder.end(), "Master") <
            std::find(graph.renderOrder.begin(), graph.renderOrder.end(), "Monitor"));

        const auto mixerJson = neuracoust::daw::serializeProject(mixerProject);
        neuracoust::daw::ProjectDocument mixerRoundTrip;
        std::string mixerParseError;
        assert(neuracoust::daw::deserializeProject(mixerJson, mixerRoundTrip, mixerParseError));
        const auto vcaRoundTrip = std::find_if(mixerRoundTrip.tracks.begin(), mixerRoundTrip.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "VCA 1";
        });
        assert(vcaRoundTrip != mixerRoundTrip.tracks.end());
        assert(vcaRoundTrip->trackType == "vca");
        const auto instrumentRoundTrip = std::find_if(mixerRoundTrip.tracks.begin(), mixerRoundTrip.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Instrument 1";
        });
        assert(instrumentRoundTrip != mixerRoundTrip.tracks.end());
        assert(instrumentRoundTrip->trackType == "instrument");
        assert(instrumentRoundTrip->instrument.pluginName == "Test Synth");
        assert(instrumentRoundTrip->instrument.pluginFormat == "VST3");
        assert(instrumentRoundTrip->instrument.enabled);
        const auto vcaControlledRoundTrip = std::find_if(mixerRoundTrip.tracks.begin(), mixerRoundTrip.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        assert(vcaControlledRoundTrip != mixerRoundTrip.tracks.end());
        assert(vcaControlledRoundTrip->controlMasterTrackName == "VCA 1");

        auto latencyProject = neuracoust::daw::defaultProject();
        latencyProject.tracks[0].inserts.push_back(makeTrackInsert("Track Latency", "VST3", "/tmp/track-latency.vst3"));
        latencyProject.tracks[0].inserts.back().reportedLatencySamples = 128;
        latencyProject.tracks[0].sends.push_back({"Bus 1-2", 0.0f, 0.0f, true, false, true});
        neuracoust::daw::TrackState latencyAux;
        latencyAux.name = "Latency Aux";
        latencyAux.trackType = "aux";
        latencyAux.inputBus = "Bus 1-2";
        latencyAux.outputBus = "Master";
        latencyAux.inserts.push_back(makeTrackInsert("Aux Latency", "VST3", "/tmp/aux-latency.vst3"));
        latencyAux.inserts.back().reportedLatencySamples = 64;
        latencyProject.tracks.insert(latencyProject.tracks.end() - 2, latencyAux);
        const auto latencyGraph = neuracoust::daw::buildMixerGraph(latencyProject);
        const auto latencyAuxRoute = std::find_if(latencyGraph.routes.begin(), latencyGraph.routes.end(), [](const neuracoust::daw::MixerRouteNode& route) {
            return route.name == "Latency Aux";
        });
        assert(latencyAuxRoute != latencyGraph.routes.end());
        assert(latencyAuxRoute->selfLatencySamples == 64);
        assert(latencyAuxRoute->pathLatencySamples == 192);
        assert(latencyGraph.maxPathLatencySamples == 192);
        latencyProject.masterInserts.push_back(makeMasterInsert("Master Latency", "external-vst3", "VST3", ""));
        latencyProject.masterInserts.back().reportedLatencySamples = 32;
        neuracoust::daw::ProjectAudioRenderPlan latencyPlan;
        assert(neuracoust::daw::makeProjectAudioRenderPlan(latencyProject, latencyPlan, mixerParseError));
        assert(latencyPlan.delayCompensationSamples == 224);
        assert(latencyPlan.routeDelayCompensationSamples["Audio 1"] == 64);
        assert(latencyPlan.routeDelayCompensationSamples["Master"] == 0);
        auto monoRecordProject = neuracoust::daw::defaultProject();
        monoRecordProject.tracks[0].recordArmed = true;
        monoRecordProject.tracks[0].inputBus = "Input 1-2";
        monoRecordProject.tracks[0].channelFormat = "mono";
        assert(neuracoust::daw::recordingInputChannelCount(monoRecordProject) == 1);
        monoRecordProject.tracks[0].channelFormat = "stereo";
        assert(neuracoust::daw::recordingInputChannelCount(monoRecordProject) == 2);
        monoRecordProject.tracks[0].trackType = "instrument";
        assert(neuracoust::daw::recordingInputChannelCount(monoRecordProject) == 0);
    }

    auto identity = neuracoust::daw::currentAppIdentity();
    assert(identity.appId == "neuracoust-daw");
    assert(identity.productName == "Neuracoust DAW");
    auto state = neuracoust::daw::makeInitialDawState();
    assert(state.identity.appId == identity.appId);
    assert(state.tracks.size() >= 3);
    assert(state.masterInserts.empty());

    auto modules = neuracoust::daw::defaultMonitorDspModules();
    assert(modules.size() >= 3);
    assert(modules[0].id == "speaker-simulation");
    assert(modules[1].id == "headphone-simulation");
    assert(modules[2].id == "graphic-eq");
    for (auto& module : modules) {
        module.enabled = module.id == "spectrum-meter" || module.id == "phase-meter" || module.id == "oscilloscope";
    }
    neuracoust::daw::MonitorDspProcessor analyzerOnlyMonitorDsp;
    analyzerOnlyMonitorDsp.configure(48000.0, modules);
    assert(analyzerOnlyMonitorDsp.reportedLatencySamples() == 0);
    const auto analyzerOnlyFrame = analyzerOnlyMonitorDsp.process({1.25f, -1.15f});
    assert(analyzerOnlyFrame.left == 1.25f);
    assert(analyzerOnlyFrame.right == -1.15f);
    auto flatSpeakerModules = neuracoust::daw::defaultMonitorDspModules();
    for (auto& module : flatSpeakerModules) {
        module.enabled = module.id == "speaker-simulation";
    }
    auto modeledSpeakerModules = flatSpeakerModules;
    modeledSpeakerModules.front().realModel = "Real Speaker: Nearfield";
    modeledSpeakerModules.front().targetModelA = "Speaker A: Laptop";
    modeledSpeakerModules.front().targetModelB = "Speaker B: NS-10 style";
    modeledSpeakerModules.front().targetModelC = "Speaker C: Auratone style";
    modeledSpeakerModules.front().activeTargetSlot = 0;
    neuracoust::daw::MonitorDspProcessor flatSpeakerDsp;
    neuracoust::daw::MonitorDspProcessor modeledSpeakerDsp;
    flatSpeakerDsp.configure(48000.0, flatSpeakerModules);
    modeledSpeakerDsp.configure(48000.0, modeledSpeakerModules);
    const auto flatSpeakerFrame = flatSpeakerDsp.process({0.31f, -0.18f});
    const auto modeledSpeakerFrame = modeledSpeakerDsp.process({0.31f, -0.18f});
    const float modeledSpeakerDelta = std::abs(flatSpeakerFrame.left - modeledSpeakerFrame.left) +
        std::abs(flatSpeakerFrame.right - modeledSpeakerFrame.right);
    assert(modeledSpeakerDelta > 0.015f);
    auto speakerSlotBModules = modeledSpeakerModules;
    speakerSlotBModules.front().activeTargetSlot = 1;
    neuracoust::daw::MonitorDspProcessor speakerSlotBDsp;
    speakerSlotBDsp.configure(48000.0, speakerSlotBModules);
    const auto speakerSlotBFrame = speakerSlotBDsp.process({0.31f, -0.18f});
    assert(std::abs(modeledSpeakerFrame.left - speakerSlotBFrame.left) > 0.00001f ||
           std::abs(modeledSpeakerFrame.right - speakerSlotBFrame.right) > 0.00001f);
    auto identitySpeakerModules = flatSpeakerModules;
    identitySpeakerModules.front().realModel = "Real Speaker: Avantone CLA-10A (NF)";
    identitySpeakerModules.front().targetModelA = "Speaker A: CLA10A Active";
    identitySpeakerModules.front().activeTargetSlot = 0;
    neuracoust::daw::MonitorDspProcessor identitySpeakerDsp;
    identitySpeakerDsp.configure(48000.0, identitySpeakerModules);
    const auto identitySpeakerFrame = identitySpeakerDsp.process({0.31f, -0.18f});
    assert(std::abs(identitySpeakerFrame.left - 0.31f) < 0.00001f);
    assert(std::abs(identitySpeakerFrame.right + 0.18f) < 0.00001f);
    auto streamingPreviewModules = neuracoust::daw::defaultMonitorDspModules();
    for (auto& module : streamingPreviewModules) {
        module.enabled = false;
        module.streamingPreview.clear();
    }
    streamingPreviewModules.front().enabled = true;
    streamingPreviewModules.front().streamingPreview = "YouTube";
    neuracoust::daw::MonitorDspProcessor youtubePreviewDsp;
    youtubePreviewDsp.configure(48000.0, streamingPreviewModules);
    const auto youtubePreviewFrame = youtubePreviewDsp.process({0.72f, -0.18f});
    assert(std::abs(youtubePreviewFrame.left - 0.72f) > 0.0001f ||
           std::abs(youtubePreviewFrame.right + 0.18f) > 0.0001f);
    streamingPreviewModules.front().streamingPreview = "Tidal";
    neuracoust::daw::MonitorDspProcessor tidalPreviewDsp;
    tidalPreviewDsp.configure(48000.0, streamingPreviewModules);
    const auto tidalPreviewFrame = tidalPreviewDsp.process({0.72f, -0.18f});
    const float streamingPreviewDelta = std::abs(youtubePreviewFrame.left - tidalPreviewFrame.left) +
        std::abs(youtubePreviewFrame.right - tidalPreviewFrame.right);
    assert(streamingPreviewDelta > 0.001f);
    streamingPreviewModules.front().enabled = false;
    streamingPreviewModules.front().streamingPreview = "YouTube";
    streamingPreviewModules[1].enabled = true;
    streamingPreviewModules[1].streamingPreview = "Spotify";
    neuracoust::daw::MonitorDspProcessor headphonePreviewDsp;
    headphonePreviewDsp.configure(48000.0, streamingPreviewModules);
    const auto headphonePreviewFrame = headphonePreviewDsp.process({0.72f, -0.18f});
    assert(std::abs(headphonePreviewFrame.left - 0.72f) > 0.0001f ||
           std::abs(headphonePreviewFrame.right + 0.18f) > 0.0001f);
    auto guardedMonitorModules = neuracoust::daw::defaultMonitorDspModules();
    for (auto& module : guardedMonitorModules) {
        module.enabled = module.id == "speaker-simulation" ||
            module.id == "headphone-simulation" ||
            module.id == "graphic-eq" ||
            module.id == "room-correction" ||
            module.id == "crossfeed";
    }
    guardedMonitorModules.front().realModel = "Real Speaker: Nearfield";
    guardedMonitorModules.front().targetModelA = "Speaker A: Laptop";
    neuracoust::daw::MonitorDspProcessor guardedMonitorDsp;
    guardedMonitorDsp.configure(48000.0, guardedMonitorModules);
    const auto guardedBadFrame = guardedMonitorDsp.process({
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()
    });
    assert(std::isfinite(guardedBadFrame.left));
    assert(std::isfinite(guardedBadFrame.right));
    const auto guardedRecoveryFrame = guardedMonitorDsp.process({0.18f, -0.12f});
    assert(std::isfinite(guardedRecoveryFrame.left));
    assert(std::isfinite(guardedRecoveryFrame.right));
    assert(std::abs(guardedRecoveryFrame.left) <= 1.0f);
    assert(std::abs(guardedRecoveryFrame.right) <= 1.0f);

    const auto driverSupport = neuracoust::daw::supportedAudioDrivers();
    assert(driverSupport.size() == 3);
    assert(driverSupport[0].driver == neuracoust::daw::AudioDriverKind::CoreAudio);
    assert(driverSupport[1].driver == neuracoust::daw::AudioDriverKind::WASAPI);
    assert(driverSupport[2].driver == neuracoust::daw::AudioDriverKind::ASIO);
    assert(driverSupport[2].requiresExternalSdk);
    const auto devices = neuracoust::daw::enumerateAudioDevices();
    for (const auto& device : devices) {
        assert(!device.id.empty());
        assert(!device.name.empty());
        assert(device.driver != neuracoust::daw::AudioDriverKind::Unknown);
        assert(device.defaultSampleRate > 0.0);
        if (device.driver == neuracoust::daw::AudioDriverKind::ASIO) {
            const auto deviceAsioStatus = neuracoust::daw::asioAdapterStatusForDeviceId(device.id);
            assert(!device.diagnosticNote.empty());
            assert(device.diagnosticNote.find("ASIO diagnostic") != std::string::npos);
            if (!deviceAsioStatus.runtimeAdapterLinked) {
                assert(!device.available);
            }
        }
    }
    const auto asioRegistrations = neuracoust::daw::enumerateAsioDriverRegistrations();
    const auto asioStatus = neuracoust::daw::asioAdapterStatusForDeviceId(asioRegistrations.empty() ? "asio:test-driver" : asioRegistrations.front().deviceId);
#if defined(_WIN32)
    assert(asioStatus.supportedPlatform);
#else
    assert(!asioStatus.supportedPlatform);
#endif
    assert(!asioStatus.runtimeAdapterLinked);
    assert(!asioStatus.message.empty());
    assert(!asioStatus.diagnosticSummary.empty());
    const auto fakeAsioComServer = testTempRoot() / "neuracoust daw fake asio server.dll";
    const auto missingFakeAsioComServer = testTempRoot() / "neuracoust daw missing asio server.dll";
    std::filesystem::remove(missingFakeAsioComServer);
    {
        std::ofstream server(fakeAsioComServer, std::ios::binary);
        server << "not a real ASIO DLL";
    }
    assert(neuracoust::daw::normalizeAsioComServerPathForDiagnostics("  \"" + fakeAsioComServer.string() + "\" --ignored  ") == fakeAsioComServer.string());
    assert(neuracoust::daw::normalizeAsioComServerPathForDiagnostics(fakeAsioComServer.string() + " /Automation") == fakeAsioComServer.string());
    assert(neuracoust::daw::asioComServerPathExistsForDiagnostics(fakeAsioComServer.string()));
    assert(!neuracoust::daw::asioComServerPathExistsForDiagnostics(missingFakeAsioComServer.string()));
#if defined(_WIN32)
    assert(asioStatus.registeredDriverCount == asioRegistrations.size());
    const auto asioDeviceCount = std::count_if(devices.begin(), devices.end(), [](const neuracoust::daw::AudioDeviceInfo& device) {
        return device.driver == neuracoust::daw::AudioDriverKind::ASIO;
    });
    assert(static_cast<size_t>(asioDeviceCount) == asioRegistrations.size());
#endif

    auto project = neuracoust::daw::defaultProject();
    assert(project.tempoLaneHeightScale == 0.50);
    project.sampleRate = 96000.0;
    project.defaultBufferSize = 128;
    project.bitDepth = 16;
    project.timeSignatureNumerator = 6;
    project.timeSignatureDenominator = 8;
    project.timeSignatureMap = {{0.0, 6, 8}, {32.0, 4, 4}};
    project.grooveFeel = "shuffle";
    project.grooveSwingAmount = 0.57;
    project.metronomeSubdivision = "sixteenth";
    project.detectedKey = "Eb";
    project.detectedKeyMode = "minor";
    project.chordKeyModePreference = "minor";
    project.tempoMasterTrackName = "Audio 1";
    project.audioImportTempoPolicy = "stretch-to-project";
    project.beatSnapEnabled = true;
    project.editMode = "Grid";
    project.gridUnit = "1 beat";
    project.playbackStartMode = "Selection";
    project.timelineZoomFactor = 4.0;
    project.timelineFollowMode = 2;
    project.trackHeightScale = 1.75;
    project.tempoLaneHeightScale = 2.0;
    project.tracks[0].displayHeightScale = 1.5;
    project.waveformGainScale = 2.5;
    project.monitorModules[0].realModel = "Real Speaker: Avantone CLA-10A (NF)";
    project.monitorModules[0].targetModelA = "Speaker A: Flat";
    project.monitorModules[0].targetModelB = "Speaker B: Avantone CLA-10A (NF)";
    project.monitorModules[0].targetModelC = "Speaker C: Auratone style";
    project.monitorModules[0].speakerOutputA = "Main 1-2";
    project.monitorModules[0].speakerOutputB = "Output 3-4";
    project.monitorModules[0].speakerOutputC = "Output 5-6";
    neuracoust::daw::TrackInsertSlot speakerInsertA;
    speakerInsertA.pluginName = "GEQ A";
    speakerInsertA.pluginFormat = "VST3";
    speakerInsertA.pluginPath = "/tmp/geq-a.vst3";
    speakerInsertA.enabled = true;
    neuracoust::daw::TrackInsertSlot speakerInsertB;
    speakerInsertB.pluginName = "GEQ B";
    speakerInsertB.pluginFormat = "VST3";
    speakerInsertB.pluginPath = "/tmp/geq-b.vst3";
    speakerInsertB.enabled = true;
    neuracoust::daw::TrackInsertSlot speakerInsertC;
    speakerInsertC.pluginName = "GEQ C";
    speakerInsertC.pluginFormat = "VST3";
    speakerInsertC.pluginPath = "/tmp/geq-c.vst3";
    speakerInsertC.enabled = true;
    project.monitorModules[0].speakerInsertsA = {speakerInsertA};
    project.monitorModules[0].speakerInsertsB = {speakerInsertB};
    project.monitorModules[0].speakerInsertsC = {speakerInsertC};
    project.monitorModules[0].streamingPreview = "YouTube";
    project.monitorModules[0].activeTargetSlot = 1;
    project.monitorModules[1].realModel = "Real Headphones: Open";
    project.monitorModules[1].targetModelA = "Headphone A: Speaker A";
    project.monitorModules[1].targetModelB = "Headphone B: Speaker B";
    project.monitorModules[1].targetModelC = "Headphone C: Speaker C";
    project.monitorModules[1].activeTargetSlot = 2;
    project.timecodeStartSeconds = 3600.5;
    project.timecodeDropFrame = true;
    project.loopEnabled = true;
    project.loopStartSeconds = 1.0;
    project.loopEndSeconds = 3.5;
    project.editSelectionEnabled = true;
    project.editSelectionStartSeconds = 0.25;
    project.editSelectionEndSeconds = 2.75;
    project.appleSiliconCoreIsolationEnabled = true;
    project.windowsProcessorAffinityEnabled = true;
    project.windowsProcessorAffinityMode = "p_core_high_priority";
    project.monitorStationMono = true;
    project.monitorStationListenMode = "S";
    project.monitorStationSwapLeftRight = true;
    project.monitorStationInvertLeft = true;
    project.monitorStationDim = true;
    project.monitorVolumeDb = -6.0f;
    project.listenRoomEnabled = true;
    project.listenRoomSessionName = "client-a";
    project.listenRoomSource = "monitor";
    project.listenRoomQuality = "pcm_lossless";
    project.listenRoomLatencyMode = "video_sync";
    project.listenRoomTransportMode = "native_webrtc";
    project.listenRoomRelayHost = "127.0.0.1";
    project.listenRoomAccessToken = "token-123";
    project.listenRoomRelayHttpPort = 8787;
    project.listenRoomRelayTcpIngestPort = 8791;
    project.videoFrameRate = 23.976;
    project.videoSources.push_back({"vid-main", "/tmp/picture-lock.mov", "Picture Lock", 23.976, 92.5, 1920, 1080, true});
    project.videoClips.push_back({"vclip-main", "vid-main", "Scene 01", 0.5, 60.0, 10.0, 3600.0, false, true});
    // --- ARA (Melodyne) clip state survives a document round trip ------------------------------
    //
    // This is not a formality. The clip array is NOT what a reload reads: the playlist placement is,
    // and rebuildProjectEditModelFromClips regenerates the clips from it. ARA state written only on
    // the ClipState therefore came back empty — the archive was silently lost on every save, which
    // is exactly what this asserts against.
    {
        neuracoust::daw::ProjectDocument araProject = neuracoust::daw::defaultProject();
        neuracoust::daw::ClipState araClip;
        araClip.id = "clip-ara-1";
        araClip.trackName = araProject.tracks.front().name;
        araClip.sourcePath = "/tmp/neuracoust-ara-test.wav";
        araClip.startSeconds = 1.0;
        araClip.durationSeconds = 2.0;
        araClip.araPluginName = "Melodyne";
        araClip.araPluginPath = "/Library/Audio/Plug-Ins/VST3/Melodyne.vst3";
        araClip.araSourcePath = "/tmp/neuracoust-ara-test_arasrc_clip-ara-1.wav";
        araClip.araArchiveBase64 = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=";
        araProject.clips.push_back(araClip);
        neuracoust::daw::rebuildProjectEditModelFromClips(araProject);

        const auto araJson = neuracoust::daw::serializeProject(araProject);
        neuracoust::daw::ProjectDocument araRoundTrip;
        std::string araParseError;
        assert(neuracoust::daw::deserializeProject(araJson, araRoundTrip, araParseError));
        assert(araRoundTrip.clips.size() == 1);
        const auto& reloaded = araRoundTrip.clips.front();
        assert(reloaded.araPluginName == "Melodyne");
        assert(reloaded.araSourcePath == araClip.araSourcePath);
        assert(reloaded.araArchiveBase64 == araClip.araArchiveBase64);

        // And through the path a real reload takes: the playlist is the render source of truth, so
        // the clips are regenerated from it. State carried only on the ClipState dies here.
        neuracoust::daw::ProjectDocument fromPlaylist = araRoundTrip;
        assert(neuracoust::daw::rebuildProjectClipsFromActivePlaylists(fromPlaylist));
        assert(fromPlaylist.clips.size() == 1);
        const auto& rebuilt = fromPlaylist.clips.front();
        assert(rebuilt.araPluginName == "Melodyne");
        assert(rebuilt.araSourcePath == araClip.araSourcePath);
        assert(rebuilt.araArchiveBase64 == araClip.araArchiveBase64);
        // And a clip that never met an ARA plug-in stays empty rather than inheriting a neighbour's.
        assert(neuracoust::daw::defaultProject().clips.empty() ||
               neuracoust::daw::defaultProject().clips.front().araArchiveBase64.empty());
    }

    auto json = neuracoust::daw::serializeProject(project);
    assert(json.find("neuracoust-daw-project-v1") != std::string::npos);
    assert(json.find("\"sampleRate\": 96000") != std::string::npos);
    assert(json.find("\"defaultBufferSize\": 128") != std::string::npos);
    assert(json.find("\"bitDepth\": 16") != std::string::npos);
    assert(json.find("\"timeSignatureNumerator\": 6") != std::string::npos);
    assert(json.find("\"timeSignatureDenominator\": 8") != std::string::npos);
    assert(json.find("\"timeSignatureMap\"") != std::string::npos);
    assert(json.find("\"numerator\":6") != std::string::npos);
    assert(json.find("\"denominator\":8") != std::string::npos);
    assert(json.find("\"grooveFeel\": \"shuffle\"") != std::string::npos);
    assert(json.find("\"metronomeSubdivision\": \"sixteenth\"") != std::string::npos);
    assert(json.find("\"detectedKey\": \"Eb\"") != std::string::npos);
    assert(json.find("\"detectedKeyMode\": \"minor\"") != std::string::npos);
    assert(json.find("\"tempoMasterTrackName\": \"Audio 1\"") != std::string::npos);
    assert(json.find("\"audioImportTempoPolicy\": \"stretch-to-project\"") != std::string::npos);
    assert(json.find("\"timecodeStartSeconds\": 3600.5") != std::string::npos);
    assert(json.find("\"videoFrameRate\": 23.976") != std::string::npos);
    assert(json.find("\"timecodeDropFrame\": true") != std::string::npos);
    assert(json.find("\"videoSources\"") != std::string::npos);
    assert(json.find("\"path\":\"/tmp/picture-lock.mov\"") != std::string::npos);
    assert(json.find("\"videoClips\"") != std::string::npos);
    assert(json.find("\"sourceTimecodeStartSeconds\":3600") != std::string::npos);
    assert(json.find("\"beatSnapEnabled\": true") != std::string::npos);
    assert(json.find("\"editMode\": \"Grid\"") != std::string::npos);
    assert(json.find("\"gridUnit\": \"1 beat\"") != std::string::npos);
    assert(json.find("\"playbackStartMode\": \"Selection\"") != std::string::npos);
    assert(json.find("\"timelineZoomFactor\": 4") != std::string::npos);
    assert(json.find("\"timelineFollowMode\": 2") != std::string::npos);
    assert(json.find("\"trackHeightScale\": 1.75") != std::string::npos);
    assert(json.find("\"tempoLaneHeightScale\": 2") != std::string::npos);
    assert(json.find("\"displayHeightScale\":1.5") != std::string::npos);
    assert(json.find("\"waveformGainScale\": 2.5") != std::string::npos);
    assert(json.find("\"loopEnabled\": true") != std::string::npos);
    assert(json.find("\"loopStartSeconds\": 1") != std::string::npos);
    assert(json.find("\"loopEndSeconds\": 3.5") != std::string::npos);
    assert(json.find("\"editSelectionEnabled\": true") != std::string::npos);
    assert(json.find("\"editSelectionStartSeconds\": 0.25") != std::string::npos);
    assert(json.find("\"editSelectionEndSeconds\": 2.75") != std::string::npos);
    assert(json.find("\"appleSiliconCoreIsolationEnabled\": true") != std::string::npos);
    assert(json.find("\"windowsProcessorAffinityEnabled\": true") != std::string::npos);
    assert(json.find("\"windowsProcessorAffinityMode\": \"p_core_high_priority\"") != std::string::npos);
    assert(json.find("\"monitorStationMono\": false") != std::string::npos);
    assert(json.find("\"monitorStationListenMode\": \"S\"") != std::string::npos);
    assert(json.find("\"monitorStationSwapLeftRight\": false") != std::string::npos);
    assert(json.find("\"monitorStationInvertLeft\": true") != std::string::npos);
    assert(json.find("\"monitorStationDim\": true") != std::string::npos);
    assert(json.find("\"monitorVolumeDb\": -6") != std::string::npos);
    assert(json.find("\"listenRoomEnabled\": true") != std::string::npos);
    assert(json.find("\"listenRoomSessionName\": \"client-a\"") != std::string::npos);
    assert(json.find("\"listenRoomQuality\": \"pcm_lossless\"") != std::string::npos);
    assert(json.find("\"listenRoomLatencyMode\": \"video_sync\"") != std::string::npos);
    assert(json.find("\"listenRoomTransportMode\": \"native_webrtc\"") != std::string::npos);
    assert(json.find("\"listenRoomAccessToken\": \"token-123\"") != std::string::npos);
    assert(json.find("\"listenRoomRelayTcpIngestPort\": 8791") != std::string::npos);
    assert(json.find("\"trackType\":\"audio\"") != std::string::npos);
    assert(json.find("\"trackType\":\"master\"") != std::string::npos);
    assert(json.find("speaker-simulation") != std::string::npos);
    assert(json.find("\"realModel\":\"Real Speaker: Avantone CLA-10A (NF)\"") != std::string::npos);
    assert(json.find("\"targetModelB\":\"Speaker B: Avantone CLA-10A (NF)\"") != std::string::npos);
    assert(json.find("\"speakerOutputB\":\"Output 3-4\"") != std::string::npos);
    assert(json.find("\"targetModelC\":\"Headphone C: Speaker C\"") != std::string::npos);
    assert(json.find("\"activeTargetSlot\":1") != std::string::npos);
    assert(json.find("\"activeTargetSlot\":2") != std::string::npos);
    assert(json.find("masterInserts") != std::string::npos);
    assert(json.find("Neuracoust Monitor DSP") == std::string::npos);
    assert(json.find("\"pluginFormat\":\"Internal\"") == std::string::npos);
    project.masterInserts.push_back(makeMasterInsert(
        "Example VST3",
        "external-vst3",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Example.vst3",
        true));
    project.masterInserts.back().parameters.push_back({12345, "Cutoff", 0.625});
    auto vst3Json = neuracoust::daw::serializeProject(project);
    assert(vst3Json.find("\"parameters\"") != std::string::npos);
    assert(vst3Json.find("\"parameterId\":12345") != std::string::npos);
    assert(vst3Json.find("\"pluginFormat\":\"VST3\"") != std::string::npos);
    assert(vst3Json.find("\"bypassed\":true") != std::string::npos);
    assert(vst3Json.find("Example.vst3") != std::string::npos);
    neuracoust::daw::ProjectDocument roundTripProject;
    std::string projectParseError;
    assert(neuracoust::daw::deserializeProject(vst3Json, roundTripProject, projectParseError));
    assert(roundTripProject.sampleRate == 96000.0);
    assert(roundTripProject.defaultBufferSize == 128);
    assert(roundTripProject.bitDepth == 16);
    assert(roundTripProject.timeSignatureNumerator == 6);
    assert(roundTripProject.timeSignatureDenominator == 8);
    assert(roundTripProject.timeSignatureMap.size() == 2);
    assert(roundTripProject.timeSignatureMap.front().numerator == 6);
    assert(roundTripProject.timeSignatureMap.front().denominator == 8);
    assert(roundTripProject.timeSignatureMap.back().numerator == 4);
    assert(roundTripProject.timeSignatureMap.back().denominator == 4);
    assert(roundTripProject.grooveFeel == "shuffle");
    assert(std::abs(roundTripProject.grooveSwingAmount - 0.57) < 0.001);
    assert(roundTripProject.metronomeSubdivision == "sixteenth");
    assert(roundTripProject.detectedKey == "Eb");
    assert(roundTripProject.detectedKeyMode == "minor");
    assert(roundTripProject.chordKeyModePreference == "minor");
    assert(roundTripProject.tempoMasterTrackName == "Audio 1");
    assert(roundTripProject.audioImportTempoPolicy == "stretch-to-project");
    assert(roundTripProject.timecodeStartSeconds == 3600.5);
    assert(std::abs(roundTripProject.videoFrameRate - 23.976) < 0.001);
    assert(roundTripProject.timecodeDropFrame);
    assert(roundTripProject.videoSources.size() == 1);
    assert(roundTripProject.videoSources.front().id == "vid-main");
    assert(roundTripProject.videoSources.front().path == "/tmp/picture-lock.mov");
    assert(roundTripProject.videoSources.front().width == 1920);
    assert(roundTripProject.videoSources.front().height == 1080);
    assert(roundTripProject.videoSources.front().hasAudio);
    assert(roundTripProject.videoClips.size() == 1);
    assert(roundTripProject.videoClips.front().id == "vclip-main");
    assert(roundTripProject.videoClips.front().sourceId == "vid-main");
    assert(roundTripProject.videoClips.front().locked);
    assert(!roundTripProject.monitorStationMono);
    assert(roundTripProject.monitorStationListenMode == "S");
    assert(!roundTripProject.monitorStationSwapLeftRight);
    assert(roundTripProject.monitorStationInvertLeft);
    assert(roundTripProject.monitorStationDim);
    assert(!roundTripProject.monitorStationTalkback);
    assert(std::abs(roundTripProject.monitorVolumeDb + 6.0f) < 0.001f);
    assert(roundTripProject.listenRoomEnabled);
    assert(roundTripProject.listenRoomSessionName == "client-a");
    assert(roundTripProject.listenRoomSource == "monitor");
    assert(roundTripProject.listenRoomQuality == "pcm_lossless");
    assert(roundTripProject.listenRoomLatencyMode == "video_sync");
    assert(roundTripProject.listenRoomTransportMode == "native_webrtc");
    assert(roundTripProject.listenRoomRelayHost == "127.0.0.1");
    assert(roundTripProject.listenRoomAccessToken == "token-123");
    assert(roundTripProject.listenRoomRelayHttpPort == 8787);
    assert(roundTripProject.listenRoomRelayTcpIngestPort == 8791);
    {
        auto editModelProject = neuracoust::daw::defaultProject();
        neuracoust::daw::ClipState clip;
        clip.id = "clip-pt-style";
        clip.trackName = "Audio 1";
        clip.sourcePath = "/tmp/session/take.wav";
        clip.regionName = "Lead Take";
        clip.sourceFileUid = "src-lead-take";
        clip.sourceChannels = 2;
        clip.sourceSampleRate = 48000.0;
        clip.sourceBitsPerSample = 24;
        clip.startSeconds = 4.0;
        clip.durationSeconds = 2.5;
        clip.sourceOffsetSeconds = 1.25;
        clip.gainDb = -3.0f;
        clip.fadeInSeconds = 0.05;
        clip.fadeOutSeconds = 0.10;
        clip.colorHex = "#35BFA8";
        editModelProject.clips.push_back(clip);
        const auto editModelJson = neuracoust::daw::serializeProject(editModelProject);
        assert(editModelJson.find("\"sessionEditModelVersion\": 1") != std::string::npos);
        assert(editModelJson.find("\"mediaSources\"") != std::string::npos);
        assert(editModelJson.find("\"clipDefinitions\"") != std::string::npos);
        assert(editModelJson.find("\"trackPlaylists\"") != std::string::npos);
        assert(editModelJson.find("\"sourceId\":\"src-lead-take\"") != std::string::npos);
        assert(editModelJson.find("\"clipDefinitionId\":\"clipdef-clip-pt-style\"") != std::string::npos);
        assert(editModelJson.find("\"legacyClipId\":\"clip-pt-style\"") != std::string::npos);

        neuracoust::daw::ProjectDocument editModelRoundTrip;
        assert(neuracoust::daw::deserializeProject(editModelJson, editModelRoundTrip, projectParseError));
        assert(editModelRoundTrip.mediaSources.size() == 1);
        assert(editModelRoundTrip.clipDefinitions.size() == 1);
        assert(!editModelRoundTrip.trackPlaylists.empty());
        const auto playlistIt = std::find_if(editModelRoundTrip.trackPlaylists.begin(), editModelRoundTrip.trackPlaylists.end(), [](const neuracoust::daw::TrackPlaylistState& playlist) {
            return playlist.trackName == "Audio 1" && playlist.active;
        });
        assert(playlistIt != editModelRoundTrip.trackPlaylists.end());
        assert(playlistIt->placements.size() == 1);
        assert(playlistIt->placements.front().clipDefinitionId == "clipdef-clip-pt-style");
        assert(playlistIt->placements.front().legacyClipId == "clip-pt-style");
        assert(std::abs(playlistIt->placements.front().startSeconds - 4.0) < 0.001);
        const auto duplicatePlaylistId = neuracoust::daw::duplicateActiveTrackPlaylist(editModelRoundTrip, "Audio 1", "Comp A");
        assert(!duplicatePlaylistId.empty());
        auto duplicatePlaylist = std::find_if(editModelRoundTrip.trackPlaylists.begin(), editModelRoundTrip.trackPlaylists.end(), [&](const neuracoust::daw::TrackPlaylistState& playlist) {
            return playlist.id == duplicatePlaylistId;
        });
        assert(duplicatePlaylist != editModelRoundTrip.trackPlaylists.end());
        assert(!duplicatePlaylist->active);
        assert(duplicatePlaylist->placements.size() == 1);
        duplicatePlaylist->placements.front().startSeconds = 8.0;
        assert(neuracoust::daw::renameTrackPlaylist(editModelRoundTrip, duplicatePlaylistId, "Lead Comp"));
        duplicatePlaylist = std::find_if(editModelRoundTrip.trackPlaylists.begin(), editModelRoundTrip.trackPlaylists.end(), [&](const neuracoust::daw::TrackPlaylistState& playlist) {
            return playlist.id == duplicatePlaylistId;
        });
        assert(duplicatePlaylist != editModelRoundTrip.trackPlaylists.end());
        assert(duplicatePlaylist->name == "Lead Comp");
        assert(neuracoust::daw::activateTrackPlaylist(editModelRoundTrip, duplicatePlaylistId));
        assert(editModelRoundTrip.clips.size() == 1);
        assert(std::abs(editModelRoundTrip.clips.front().startSeconds - 8.0) < 0.001);
        assert(editModelRoundTrip.clips.front().regionName == "Lead Take");
        const auto alternatePlaylistId = neuracoust::daw::duplicateActiveTrackPlaylist(editModelRoundTrip, "Audio 1", "Alt Take");
        assert(!alternatePlaylistId.empty());
        auto alternatePlaylist = std::find_if(editModelRoundTrip.trackPlaylists.begin(), editModelRoundTrip.trackPlaylists.end(), [&](const neuracoust::daw::TrackPlaylistState& playlist) {
            return playlist.id == alternatePlaylistId;
        });
        assert(alternatePlaylist != editModelRoundTrip.trackPlaylists.end());
        assert(alternatePlaylist->placements.size() == 1);
        alternatePlaylist->placements.front().startSeconds = 10.0;
        const auto promotedPlacementId = alternatePlaylist->placements.front().id;
        assert(neuracoust::daw::copyPlaylistPlacementToActivePlaylist(editModelRoundTrip, alternatePlaylistId, promotedPlacementId));
        const auto activeCompPlaylist = std::find_if(editModelRoundTrip.trackPlaylists.begin(), editModelRoundTrip.trackPlaylists.end(), [](const neuracoust::daw::TrackPlaylistState& playlist) {
            return playlist.trackName == "Audio 1" && playlist.active;
        });
        assert(activeCompPlaylist != editModelRoundTrip.trackPlaylists.end());
        assert(activeCompPlaylist->placements.size() == 2);
        assert(neuracoust::daw::deleteTrackPlaylist(editModelRoundTrip, alternatePlaylistId));
        assert(std::none_of(editModelRoundTrip.trackPlaylists.begin(), editModelRoundTrip.trackPlaylists.end(), [&](const neuracoust::daw::TrackPlaylistState& playlist) {
            return playlist.id == alternatePlaylistId;
        }));
        neuracoust::daw::ProjectAudioRenderPlan playlistRenderPlan;
        std::string playlistRenderError;
        assert(neuracoust::daw::makeProjectAudioRenderPlan(editModelRoundTrip, playlistRenderPlan, playlistRenderError));
        assert(playlistRenderPlan.clips.size() == 2);
        assert(std::abs(playlistRenderPlan.clips.front().clip.startSeconds - 8.0) < 0.001);

        auto playlistDeleteProject = neuracoust::daw::defaultProject();
        neuracoust::daw::ClipState playlistDeleteClip;
        playlistDeleteClip.id = "delete-playlist-clip";
        playlistDeleteClip.trackName = "Audio 1";
        playlistDeleteClip.sourcePath = "/tmp/session/delete-me.wav";
        playlistDeleteClip.regionName = "Delete Me";
        playlistDeleteClip.sourceFileUid = "src-delete-me";
        playlistDeleteClip.sourceChannels = 2;
        playlistDeleteClip.sourceSampleRate = 48000.0;
        playlistDeleteClip.sourceBitsPerSample = 24;
        playlistDeleteClip.startSeconds = 1.0;
        playlistDeleteClip.durationSeconds = 1.5;
        playlistDeleteProject.clips.push_back(playlistDeleteClip);
        const auto playlistDeleteJson = neuracoust::daw::serializeProject(playlistDeleteProject);
        assert(neuracoust::daw::deserializeProject(playlistDeleteJson, playlistDeleteProject, projectParseError));
        assert(neuracoust::daw::deleteClip(playlistDeleteProject, "delete-playlist-clip"));
        assert(playlistDeleteProject.clips.empty());
        assert(std::none_of(playlistDeleteProject.trackPlaylists.begin(), playlistDeleteProject.trackPlaylists.end(), [](const neuracoust::daw::TrackPlaylistState& playlist) {
            return std::any_of(playlist.placements.begin(), playlist.placements.end(), [](const neuracoust::daw::PlaylistClipPlacementState& placement) {
                return placement.id == "delete-playlist-clip" || placement.legacyClipId == "delete-playlist-clip";
            });
        }));
        neuracoust::daw::ProjectAudioRenderPlan playlistDeleteRenderPlan;
        std::string playlistDeleteRenderError;
        assert(neuracoust::daw::makeProjectAudioRenderPlan(playlistDeleteProject, playlistDeleteRenderPlan, playlistDeleteRenderError));
        assert(playlistDeleteRenderPlan.clips.empty());

        auto mediaPoolProject = editModelRoundTrip;
        neuracoust::daw::MediaSourceState unusedSource;
        unusedSource.id = "unused-source";
        unusedSource.path = "/tmp/neuracoust-daw-unused-source.wav";
        unusedSource.displayName = "Unused Source";
        mediaPoolProject.mediaSources.push_back(unusedSource);
        auto mediaPoolSummary = neuracoust::daw::buildProjectMediaPoolSummary(mediaPoolProject);
        assert(mediaPoolSummary.sources.size() == 2);
        assert(mediaPoolSummary.regions.size() == 1);
        assert(!mediaPoolSummary.uses.empty());
        assert(std::any_of(mediaPoolSummary.uses.begin(), mediaPoolSummary.uses.end(), [](const neuracoust::daw::MediaPoolUseSummary& use) {
            return use.sourceId == "src-lead-take";
        }));
        assert(mediaPoolSummary.missingSources >= 1);
        assert(mediaPoolSummary.unusedSources == 1);
        assert(std::any_of(mediaPoolSummary.sources.begin(), mediaPoolSummary.sources.end(), [](const neuracoust::daw::MediaPoolSourceSummary& source) {
            return source.sourceId == "unused-source" && source.unused;
        }));
        assert(neuracoust::daw::relinkMediaSource(mediaPoolProject, "src-lead-take", "/tmp/session/relinked.wav") == 1);
        mediaPoolSummary = neuracoust::daw::buildProjectMediaPoolSummary(mediaPoolProject);
        assert(std::any_of(mediaPoolSummary.sources.begin(), mediaPoolSummary.sources.end(), [](const neuracoust::daw::MediaPoolSourceSummary& source) {
            return source.sourceId == "src-lead-take" && source.path == "/tmp/session/relinked.wav";
        }));
        assert(neuracoust::daw::deleteUnusedMediaSources(mediaPoolProject) == 1);
        assert(neuracoust::daw::removeMediaSourceFromProject(mediaPoolProject, "src-lead-take") >= 1);
        assert(mediaPoolProject.clips.empty());
    }
    assert(roundTripProject.tracks[0].trackType == "audio");
    assert(roundTripProject.tracks[0].outputBus == "Master");
    assert(roundTripProject.tracks[1].outputBus == "Master");
    assert(std::any_of(roundTripProject.tracks.begin(), roundTripProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Master" && track.trackType == "master" && track.outputBus == "Monitor";
    }));
    assert(std::any_of(roundTripProject.tracks.begin(), roundTripProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Monitor" && track.trackType == "monitor" && track.inputBus == "Monitor" && track.outputBus == "Main 1-2";
    }));
    const std::string legacyPhysicalOutputProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "tracks": [
    {"name":"Audio 1","trackType":"audio","inputBus":"Input 1","outputBus":"Main 1-2"},
    {"name":"Audio 2","trackType":"audio","inputBus":"Input 2","outputBus":"Waves SoundGrid Out 1-2"},
    {"name":"Aux 1","trackType":"aux","inputBus":"Bus 1-2","outputBus":"Monitor"},
    {"name":"Legacy Master","trackType":"master","outputBus":"Main 1-2"}
  ],
  "clips": []
})json";
    neuracoust::daw::ProjectDocument legacyPhysicalOutputProject;
    assert(neuracoust::daw::deserializeProject(legacyPhysicalOutputProjectJson, legacyPhysicalOutputProject, projectParseError));
    const auto legacyMasterCount = std::count_if(legacyPhysicalOutputProject.tracks.begin(), legacyPhysicalOutputProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Master" && track.trackType == "master" && track.outputBus == "Monitor";
    });
    const auto legacyMonitorCount = std::count_if(legacyPhysicalOutputProject.tracks.begin(), legacyPhysicalOutputProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Monitor" && track.trackType == "monitor" && track.inputBus == "Monitor" && track.outputBus == "Main 1-2";
    });
    assert(legacyMasterCount == 1);
    assert(legacyMonitorCount == 1);
    assert(legacyPhysicalOutputProject.tracks[0].outputBus == "Master");
    assert(legacyPhysicalOutputProject.tracks[1].outputBus == "Master");
    assert(legacyPhysicalOutputProject.tracks[2].outputBus == "Master");
    assert(legacyPhysicalOutputProject.tracks[legacyPhysicalOutputProject.tracks.size() - 2].name == "Master");
    assert(legacyPhysicalOutputProject.tracks.back().name == "Monitor");
    {
        auto folderProject = neuracoust::daw::defaultProject();
        const auto folderName = neuracoust::daw::addFolderTrack(folderProject);
        assert(!folderName.empty());
        assert(neuracoust::daw::moveTrackIntoFolder(folderProject, "Audio 1", folderName));
        const auto audio1InFolder = std::find_if(folderProject.tracks.begin(), folderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        assert(audio1InFolder != folderProject.tracks.end());
        assert(audio1InFolder->folderName == folderName);
        assert(!neuracoust::daw::moveTrackIntoFolder(folderProject, "Master", folderName));
        assert(!neuracoust::daw::moveTrackIntoFolder(folderProject, "Monitor", folderName));
        auto masterAfterFolderAttempt = std::find_if(folderProject.tracks.begin(), folderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Master";
        });
        auto monitorAfterFolderAttempt = std::find_if(folderProject.tracks.begin(), folderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Monitor";
        });
        assert(masterAfterFolderAttempt != folderProject.tracks.end() && masterAfterFolderAttempt->folderName.empty());
        assert(monitorAfterFolderAttempt != folderProject.tracks.end() && monitorAfterFolderAttempt->folderName.empty());
        assert(neuracoust::daw::moveTrackNearTrack(folderProject, "Audio 1", "Audio 2", true));
        const auto audio1MovedOut = std::find_if(folderProject.tracks.begin(), folderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        assert(audio1MovedOut != folderProject.tracks.end());
        assert(audio1MovedOut->folderName.empty());

        auto folderDeleteProject = neuracoust::daw::defaultProject();
        const auto deleteFolderName = neuracoust::daw::addFolderTrack(folderDeleteProject);
        assert(neuracoust::daw::moveTrackIntoFolder(folderDeleteProject, "Audio 1", deleteFolderName));
        assert(neuracoust::daw::folderChildTrackCount(folderDeleteProject, deleteFolderName) == 1);
        assert(!neuracoust::daw::deleteTrackIfEmpty(folderDeleteProject, deleteFolderName));
        assert(neuracoust::daw::deleteTrack(folderDeleteProject, deleteFolderName, false, false));
        auto unwrappedAudio = std::find_if(folderDeleteProject.tracks.begin(), folderDeleteProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        assert(unwrappedAudio != folderDeleteProject.tracks.end());
        assert(unwrappedAudio->folderName.empty());
        assert(std::none_of(folderDeleteProject.tracks.begin(), folderDeleteProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == deleteFolderName;
        }));

        auto folderDeleteChildrenProject = neuracoust::daw::defaultProject();
        const auto deleteChildrenFolderName = neuracoust::daw::addFolderTrack(folderDeleteChildrenProject);
        assert(neuracoust::daw::moveTrackIntoFolder(folderDeleteChildrenProject, "Audio 1", deleteChildrenFolderName));
        folderDeleteChildrenProject.clips.push_back({"folder-child-clip", "Audio 1", "/tmp/folder-child.wav", 0.0, 1.0, 0.0, 0.0f});
        assert(neuracoust::daw::trackTimelineItemCount(folderDeleteChildrenProject, "Audio 1") == 1);
        assert(!neuracoust::daw::deleteTrack(folderDeleteChildrenProject, deleteChildrenFolderName, false, true));
        assert(neuracoust::daw::deleteTrack(folderDeleteChildrenProject, deleteChildrenFolderName, true, true));
        assert(std::none_of(folderDeleteChildrenProject.tracks.begin(), folderDeleteChildrenProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        }));
        assert(folderDeleteChildrenProject.clips.empty());
    }
    auto scrambledRoutingProject = neuracoust::daw::defaultProject();
    neuracoust::daw::TrackState auxBeforeNormalize;
    auxBeforeNormalize.name = "Aux 1";
    auxBeforeNormalize.trackType = "aux";
    auxBeforeNormalize.inputBus = "Bus 1-2";
    auxBeforeNormalize.outputBus = "Main 1-2";
    scrambledRoutingProject.tracks.insert(scrambledRoutingProject.tracks.end(), auxBeforeNormalize);
    std::rotate(scrambledRoutingProject.tracks.begin(),
                scrambledRoutingProject.tracks.begin() + 2,
                scrambledRoutingProject.tracks.end());
    neuracoust::daw::normalizeProjectRouting(scrambledRoutingProject);
    assert(scrambledRoutingProject.tracks[scrambledRoutingProject.tracks.size() - 2].name == "Master");
    assert(scrambledRoutingProject.tracks.back().name == "Monitor");
    auto scrambledAux = std::find_if(scrambledRoutingProject.tracks.begin(), scrambledRoutingProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Aux 1";
    });
    assert(scrambledAux != scrambledRoutingProject.tracks.end());
    assert(scrambledAux->outputBus == "Master");
    auto analyzedImportProject = neuracoust::daw::defaultProject();
    const auto analysisWav = makeAnalyzableImportWav();
    const auto analysisSummary = neuracoust::daw::analyzeImportedAudioIntoProject(analyzedImportProject,
                                                                                  analysisWav,
                                                                                  0.0,
                                                                                  static_cast<double>(analysisWav.frameCount()) / analysisWav.sampleRate,
                                                                                  true);
    assert(analyzedImportProject.tempoBpm == 120);
    assert(analyzedImportProject.timeSignatureNumerator >= 3);
    assert(analyzedImportProject.timeSignatureDenominator == 4 || analyzedImportProject.timeSignatureDenominator == 8);
    assert(analyzedImportProject.grooveFeel == "straight" ||
           analyzedImportProject.grooveFeel == "shuffle" ||
           analyzedImportProject.grooveFeel == "triplet");
    assert(!analyzedImportProject.detectedKey.empty());
    assert(analyzedImportProject.detectedKeyMode == "major" || analyzedImportProject.detectedKeyMode == "minor");

    // --- key detection actually identifies the key -------------------------------
    //
    // A I–IV–V–I in D major and a i–iv–V–i in A minor. The V chord's leading tone (C# / G#) is what
    // separates a key from its relative, which a single sustained triad cannot test at all.
    {
        auto keyProject = neuracoust::daw::defaultProject();
        // D F# A / G B D / A C# E / D F# A
        const std::array<std::array<int, 3>, 4> dMajor {{{62, 66, 69}, {67, 71, 74}, {69, 73, 76}, {62, 66, 69}}};
        const auto dMajorWav = makeKeyProgressionWav(dMajor);
        neuracoust::daw::analyzeImportedAudioIntoProject(
            keyProject, dMajorWav, 0.0,
            static_cast<double>(dMajorWav.frameCount()) / dMajorWav.sampleRate, true);
        assert(keyProject.detectedKey == "D");
        assert(keyProject.detectedKeyMode == "major");

        auto minorProject = neuracoust::daw::defaultProject();
        // A C E / D F A / E G# B / A C E
        const std::array<std::array<int, 3>, 4> aMinor {{{69, 72, 76}, {62, 65, 69}, {64, 68, 71}, {69, 72, 76}}};
        const auto aMinorWav = makeKeyProgressionWav(aMinor);
        neuracoust::daw::analyzeImportedAudioIntoProject(
            minorProject, aMinorWav, 0.0,
            static_cast<double>(aMinorWav.frameCount()) / aMinorWav.sampleRate, true);
        assert(minorProject.detectedKey == "A");
        assert(minorProject.detectedKeyMode == "minor");
    }
    assert(analyzedImportProject.tempoMap.size() >= 3);
    assert(!analyzedImportProject.chordEvents.empty());
    assert(analyzedImportProject.chordEvents.size() == 1);
    assert(analyzedImportProject.chordEvents.front().name.find("C") != std::string::npos);
    assert(!analyzedImportProject.markers.empty());
    assert(analysisSummary.find("120 BPM") != std::string::npos);
    assert(analysisSummary.find("bar(s)") != std::string::npos);
    assert(analysisSummary.find("beat(s)") != std::string::npos);
    assert(analysisSummary.find("chord key mode") != std::string::npos);
    assert(analysisSummary.find("chord change") != std::string::npos);
    auto sixEightProject = neuracoust::daw::defaultProject();
    sixEightProject.editMode = "Grid";
    sixEightProject.gridUnit = "1 bar";
    const auto sixEightWav = makeCompoundSixEightImportWav();
    const auto sixEightSummary = neuracoust::daw::analyzeImportedAudioIntoProject(sixEightProject,
                                                                                  sixEightWav,
                                                                                  0.0,
                                                                                  static_cast<double>(sixEightWav.frameCount()) / sixEightWav.sampleRate,
                                                                                  true);
    assert(sixEightProject.tempoBpm == 120);
    assert(sixEightProject.timeSignatureNumerator == 6);
    assert(sixEightProject.timeSignatureDenominator == 8);
    assert(sixEightSummary.find("6/8") != std::string::npos);
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(sixEightProject) - 1.5) < 0.0001);
    assert(std::abs(neuracoust::daw::snapProjectTime(sixEightProject, 1.40) - 1.5) < 0.0001);
    const auto sixEightBars = neuracoust::daw::projectMusicalGridLines(sixEightProject, "1 bar", 0.0, 4.0);
    assert(sixEightBars.size() >= 3);
    assert(std::abs(sixEightBars[0].timeSeconds - 0.0) < 0.0001);
    assert(std::abs(sixEightBars[1].timeSeconds - 1.5) < 0.0001);
    assert(std::abs(sixEightBars[2].timeSeconds - 3.0) < 0.0001);
    const auto sixEightBeats = neuracoust::daw::projectMusicalGridLines(sixEightProject, "1 beat", 0.0, 3.1);
    assert(sixEightBeats.size() >= 7);
    assert(sixEightBeats[0].barNumber == 1 && sixEightBeats[0].beatInBar == 1);
    assert(sixEightBeats[1].barNumber == 1 && sixEightBeats[1].beatInBar == 2);
    assert(sixEightBeats[2].barNumber == 1 && sixEightBeats[2].beatInBar == 3);
    assert(sixEightBeats[3].barNumber == 2 && sixEightBeats[3].beatInBar == 1);
    assert(sixEightBeats[4].barNumber == 2 && sixEightBeats[4].beatInBar == 2);
    assert(sixEightBeats[5].barNumber == 2 && sixEightBeats[5].beatInBar == 3);
    auto sixEightClipMetadataProject = neuracoust::daw::defaultProject();
    sixEightClipMetadataProject.clips.push_back({"six-eight-analysis-clip", "Audio 1", "/tmp/six-eight.wav", 0.0, 6.0, 0.0, 0.0f});
    std::string sixEightReanalysisError;
    const auto sixEightClipSummary = neuracoust::daw::reanalyzeClipMusicalMetadata(sixEightClipMetadataProject,
                                                                                  "six-eight-analysis-clip",
                                                                                  sixEightWav,
                                                                                  sixEightReanalysisError);
    assert(sixEightReanalysisError.empty());
    assert(sixEightClipSummary.find("6/8") != std::string::npos);
    assert(sixEightClipMetadataProject.clips.front().sourceTimeSignatureNumerator == 6);
    assert(sixEightClipMetadataProject.clips.front().sourceTimeSignatureDenominator == 8);
    assert(sixEightClipMetadataProject.clips.front().sourceGrooveFeel == "straight" ||
           sixEightClipMetadataProject.clips.front().sourceGrooveFeel == "shuffle" ||
           sixEightClipMetadataProject.clips.front().sourceGrooveFeel == "triplet");
    auto dottedPulseSixEightProject = neuracoust::daw::defaultProject();
    auto dottedPulseSixEightWav = makeCompoundSixEightImportWav();
    dottedPulseSixEightWav.embeddedTempoBpm = 80.0;
    const auto dottedPulseSummary = neuracoust::daw::analyzeImportedAudioIntoProject(dottedPulseSixEightProject,
                                                                                    dottedPulseSixEightWav,
                                                                                    0.0,
                                                                                    static_cast<double>(dottedPulseSixEightWav.frameCount()) / dottedPulseSixEightWav.sampleRate,
                                                                                    true);
    assert(dottedPulseSixEightProject.tempoBpm == 120);
    assert(dottedPulseSixEightProject.timeSignatureNumerator == 6);
    assert(dottedPulseSixEightProject.timeSignatureDenominator == 8);
    assert(dottedPulseSummary.find("dotted-pulse") != std::string::npos);
    for (const auto compoundNumerator : {9, 12}) {
        auto compoundProject = neuracoust::daw::defaultProject();
        compoundProject.editMode = "Grid";
        compoundProject.gridUnit = "1 bar";
        const auto compoundWav = makeCompoundEighthImportWav(compoundNumerator, compoundNumerator == 9 ? 18.0 : 24.0);
        const auto compoundSummary = neuracoust::daw::analyzeImportedAudioIntoProject(compoundProject,
                                                                                      compoundWav,
                                                                                      0.0,
                                                                                      static_cast<double>(compoundWav.frameCount()) / compoundWav.sampleRate,
                                                                                      true);
        assert(compoundProject.tempoBpm == 120);
        assert(compoundProject.timeSignatureNumerator == compoundNumerator);
        assert(compoundProject.timeSignatureDenominator == 8);
        assert(compoundSummary.find(std::to_string(compoundNumerator) + "/8") != std::string::npos);
        const double expectedBarSeconds = static_cast<double>(compoundNumerator) * 0.25;
        assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(compoundProject) - expectedBarSeconds) < 0.0001);
        const auto compoundBars = neuracoust::daw::projectMusicalGridLines(compoundProject, "1 bar", 0.0, expectedBarSeconds * 2.5);
        assert(compoundBars.size() >= 3);
        assert(std::abs(compoundBars[1].timeSeconds - expectedBarSeconds) < 0.0001);
    }
    auto reanalysisProject = neuracoust::daw::defaultProject();
    reanalysisProject.chordKeyModePreference = "major";
    reanalysisProject.clips.push_back({"analysis-clip-1", "Audio 1", "/tmp/analysis.wav", 10.0, 4.0, 2.0, 0.0f});
    reanalysisProject.tempoMap = {{0.0, 90.0}, {10.5, 91.0}};
    reanalysisProject.chordEvents.push_back({"old-chord", "Wrong", 10.5});
    reanalysisProject.markers.push_back({"user-marker", "Custom Cue", 10.5});
    reanalysisProject.markers.push_back({"auto-marker", "Verse", 10.6});
    std::string reanalysisError;
    const auto reanalysisSummary = neuracoust::daw::reanalyzeClipMusicalMetadata(reanalysisProject,
                                                                                 "analysis-clip-1",
                                                                                 analysisWav,
                                                                                 reanalysisError);
    assert(reanalysisError.empty());
    assert(reanalysisSummary.find("120 BPM") != std::string::npos);
    assert(reanalysisSummary.find("chord key mode major") != std::string::npos);
    assert(reanalysisProject.tempoBpm == 120);
    assert(reanalysisProject.tempoMap.front().timeSeconds == 0.0);
    assert(std::any_of(reanalysisProject.tempoMap.begin(), reanalysisProject.tempoMap.end(), [](const neuracoust::daw::TempoMarkerState& tempo) {
        return std::abs(tempo.timeSeconds - 10.0) < 0.05 && std::abs(tempo.bpm - 120.0) < 0.001;
    }));
    assert(std::none_of(reanalysisProject.chordEvents.begin(), reanalysisProject.chordEvents.end(), [](const neuracoust::daw::ChordEventState& chord) {
        return chord.id == "old-chord";
    }));
    assert(std::any_of(reanalysisProject.markers.begin(), reanalysisProject.markers.end(), [](const neuracoust::daw::MarkerState& marker) {
        return marker.id == "user-marker" && marker.name == "Custom Cue";
    }));
    assert(std::none_of(reanalysisProject.markers.begin(), reanalysisProject.markers.end(), [](const neuracoust::daw::MarkerState& marker) {
        return marker.id == "auto-marker";
    }));
    auto projectWideReanalysisProject = neuracoust::daw::defaultProject();
    projectWideReanalysisProject.chordKeyModePreference = "minor";
    projectWideReanalysisProject.clips.push_back({"analysis-clip-a", "Audio 1", "/tmp/shared-analysis.wav", 10.0, 4.0, 0.0, 0.0f});
    projectWideReanalysisProject.clips.push_back({"analysis-clip-b", "Audio 2", "/tmp/shared-analysis.wav", 20.0, 4.0, 2.0, 0.0f});
    projectWideReanalysisProject.clips.push_back({"analysis-clip-missing", "Audio 2", "", 30.0, 4.0, 0.0, 0.0f});
    projectWideReanalysisProject.chordEvents.push_back({"old-project-chord", "Wrong", 20.5});
    projectWideReanalysisProject.markers.push_back({"project-user-marker", "Custom Cue", 20.5});
    projectWideReanalysisProject.markers.push_back({"project-auto-marker", "Chorus", 20.6});
    int projectWideLoads = 0;
    const auto projectWideReport = neuracoust::daw::reanalyzeProjectMusicalMetadata(
        projectWideReanalysisProject,
        [&](const std::string& path, neuracoust::daw::WavAudioData& data, std::string& error) {
            ++projectWideLoads;
            if (path != "/tmp/shared-analysis.wav") {
                error = "unexpected path";
                return false;
            }
            data = analysisWav;
            return true;
        });
    assert(projectWideReport.analyzedClips == 2);
    assert(projectWideReport.skippedClips == 1);
    assert(projectWideReport.reusedSourceFiles == 1);
    assert(projectWideLoads == 1);
    assert(projectWideReport.summary.find("reanalyzed 2 clip") != std::string::npos);
    assert(projectWideReanalysisProject.tempoBpm == 120);
    assert(projectWideReanalysisProject.timeSignatureNumerator >= 3);
    assert(projectWideReanalysisProject.grooveFeel == "straight" ||
           projectWideReanalysisProject.grooveFeel == "shuffle" ||
           projectWideReanalysisProject.grooveFeel == "triplet");
    assert(std::any_of(projectWideReanalysisProject.tempoMap.begin(), projectWideReanalysisProject.tempoMap.end(), [](const neuracoust::daw::TempoMarkerState& tempo) {
        return std::abs(tempo.timeSeconds - 20.0) < 0.05 && std::abs(tempo.bpm - 120.0) < 0.001;
    }));
    assert(std::none_of(projectWideReanalysisProject.chordEvents.begin(), projectWideReanalysisProject.chordEvents.end(), [](const neuracoust::daw::ChordEventState& chord) {
        return chord.id == "old-project-chord";
    }));
    assert(std::any_of(projectWideReanalysisProject.markers.begin(), projectWideReanalysisProject.markers.end(), [](const neuracoust::daw::MarkerState& marker) {
        return marker.id == "project-user-marker" && marker.name == "Custom Cue";
    }));
    assert(std::none_of(projectWideReanalysisProject.markers.begin(), projectWideReanalysisProject.markers.end(), [](const neuracoust::daw::MarkerState& marker) {
        return marker.id == "project-auto-marker";
    }));
    auto rhythmOnlyImportProject = neuracoust::daw::defaultProject();
    auto rhythmOnlyWav = analysisWav;
    std::fill(rhythmOnlyWav.interleavedSamples.begin(), rhythmOnlyWav.interleavedSamples.end(), 0.0f);
    for (int beat = 0; beat < 16; ++beat) {
        const int startFrame = static_cast<int>(std::round(static_cast<double>(beat) * 0.5 * rhythmOnlyWav.sampleRate));
        const float accent = beat % 4 == 0 ? 0.95f : 0.45f;
        for (int offset = 0; offset < 64 && startFrame + offset < rhythmOnlyWav.frameCount(); ++offset) {
            rhythmOnlyWav.interleavedSamples[static_cast<size_t>(startFrame + offset)] = accent * static_cast<float>(1.0 - static_cast<double>(offset) / 64.0);
        }
    }
    neuracoust::daw::analyzeImportedAudioIntoProject(rhythmOnlyImportProject,
                                                     rhythmOnlyWav,
                                                     0.0,
                                                     static_cast<double>(rhythmOnlyWav.frameCount()) / rhythmOnlyWav.sampleRate,
                                                     true);
    assert(rhythmOnlyImportProject.chordEvents.empty());
    assert(rhythmOnlyImportProject.detectedKey == "Unknown");
    assert(rhythmOnlyImportProject.detectedKeyMode == "unknown");
    {
        std::string unknownKeyError;
        neuracoust::daw::ProjectDocument unknownKeyRoundTrip;
        assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(rhythmOnlyImportProject), unknownKeyRoundTrip, unknownKeyError));
        assert(unknownKeyRoundTrip.detectedKey == "Unknown");
        assert(unknownKeyRoundTrip.detectedKeyMode == "unknown");
        assert(unknownKeyRoundTrip.chordEvents.empty());
    }
    const auto analysisManifestMedia = testTempRoot() / "neuracoust-daw-analysis-import.wav";
    std::string analysisManifestError;
    assert(neuracoust::daw::writeImportedMediaManifest(analyzedImportProject,
                                                       analysisManifestMedia,
                                                       "/tmp/source-analysis.wav",
                                                       "analysis-clip",
                                                       "Audio 1",
                                                       0.0,
                                                       8.0,
                                                       analysisWav.bitsPerSample,
                                                       analysisWav.floatingPoint,
                                                       analysisWav.sampleRate,
                                                       analysisWav.channels,
                                                       false,
                                                       false,
                                                       "matched",
                                                       "matched",
                                                       120.0,
                                                       "tempo-master",
                                                       false,
                                                       analysisManifestError));
    {
        std::ifstream analysisManifest(neuracoust::daw::importedMediaManifestPath(analysisManifestMedia), std::ios::binary);
        std::string analysisManifestText((std::istreambuf_iterator<char>(analysisManifest)), std::istreambuf_iterator<char>());
        assert(analysisManifestText.find("\"analysis\"") != std::string::npos);
        assert(analysisManifestText.find("\"tempoBpm\": 120") != std::string::npos);
        assert(analysisManifestText.find("\"sourceTempoBpm\": 120") != std::string::npos);
        assert(analysisManifestText.find("\"tempoSyncPolicy\": \"tempo-master\"") != std::string::npos);
        assert(analysisManifestText.find("\"timeSignature\": \"") != std::string::npos);
        assert(analysisManifestText.find("\"grooveFeel\"") != std::string::npos);
        assert(analysisManifestText.find("\"grooveSwingAmount\"") != std::string::npos);
        assert(analysisManifestText.find("\"barCount\"") != std::string::npos);
        assert(analysisManifestText.find("\"beatCount\"") != std::string::npos);
        assert(analysisManifestText.find("\"detectedKey\"") != std::string::npos);
        assert(analysisManifestText.find("\"detectedKeyMode\"") != std::string::npos);
        assert(analysisManifestText.find("\"chordKeyModePreference\"") != std::string::npos);
        assert(analysisManifestText.find("\"chordEventCount\": ") != std::string::npos);
        assert(analysisManifestText.find("\"markerCount\": ") != std::string::npos);
    }
    project.bitDepth = 24;
    assert(neuracoust::daw::addTempoMarkerAt(roundTripProject, 4.0, 132.5));
    assert(neuracoust::daw::addTempoMarkerAt(roundTripProject, 2.0, 98.0));
    assert(roundTripProject.tempoMap.size() == 3);
    assert(roundTripProject.tempoMap[1].timeSeconds == 2.0);
    assert(roundTripProject.tempoMap[1].bpm == 98.0);
    assert(neuracoust::daw::addTempoMarkerAt(roundTripProject, 2.002, 101.0));
    assert(roundTripProject.tempoMap.size() == 3);
    assert(std::abs(roundTripProject.tempoMap[1].bpm - 101.0) < 0.0001);
    assert(!neuracoust::daw::addTempoMarkerAt(roundTripProject, 5.0, 401.0));
    assert(neuracoust::daw::setNearestTempoMarkerBpm(roundTripProject, 2.03, 0.1, 111.0));
    assert(std::abs(roundTripProject.tempoMap[1].bpm - 111.0) < 0.0001);
    assert(neuracoust::daw::moveNearestTempoMarker(roundTripProject, 2.0, 0.2, 2.5, 118.0));
    assert(std::abs(roundTripProject.tempoMap[1].timeSeconds - 2.5) < 0.0001);
    assert(std::abs(roundTripProject.tempoMap[1].bpm - 118.0) < 0.0001);
    assert(!neuracoust::daw::setNearestTempoMarkerBpm(roundTripProject, 20.0, 0.1, 120.0));
    assert(!neuracoust::daw::setNearestTempoMarkerBpm(roundTripProject, 2.0, 0.1, 401.0));
    assert(neuracoust::daw::deleteNearestTempoMarker(roundTripProject, 2.0, 0.5));
    assert(roundTripProject.tempoMap.size() == 2);
    assert(!neuracoust::daw::deleteNearestTempoMarker(roundTripProject, 20.0, 0.1));
    assert(roundTripProject.beatSnapEnabled);
    assert(roundTripProject.editMode == "Grid");
    assert(roundTripProject.gridUnit == "1 beat");
    assert(roundTripProject.playbackStartMode == "Selection");
    assert(roundTripProject.timelineZoomFactor == 4.0);
    assert(roundTripProject.timelineFollowMode == 2);
    assert(roundTripProject.trackHeightScale == 1.75);
    assert(roundTripProject.tempoLaneHeightScale == 2.0);
    assert(roundTripProject.tracks[0].displayHeightScale == 1.5);
    assert(roundTripProject.waveformGainScale == 2.5);
    assert(roundTripProject.monitorModules[0].realModel == "Real Speaker: Avantone CLA-10A (NF)");
    assert(roundTripProject.monitorModules[0].targetModelB == "Speaker B: Avantone CLA-10A (NF)");
    assert(roundTripProject.monitorModules[0].speakerOutputA == "Main 1-2");
    assert(roundTripProject.monitorModules[0].speakerOutputB == "Output 3-4");
    assert(roundTripProject.monitorModules[0].speakerOutputC == "Output 5-6");
    assert(roundTripProject.monitorModules[0].speakerInsertsA.size() == 1);
    assert(roundTripProject.monitorModules[0].speakerInsertsB.size() == 1);
    assert(roundTripProject.monitorModules[0].speakerInsertsC.size() == 1);
    assert(roundTripProject.monitorModules[0].speakerInsertsA[0].pluginName == "GEQ A");
    assert(roundTripProject.monitorModules[0].speakerInsertsB[0].pluginName == "GEQ B");
    assert(roundTripProject.monitorModules[0].speakerInsertsC[0].pluginName == "GEQ C");
    assert(roundTripProject.monitorModules[0].streamingPreview == "YouTube");
    assert(roundTripProject.monitorModules[0].activeTargetSlot == 1);
    assert(roundTripProject.monitorModules[1].realModel == "Real Headphones: Open");
    assert(roundTripProject.monitorModules[1].targetModelC == "Headphone C: Speaker C");
    assert(roundTripProject.monitorModules[1].activeTargetSlot == 2);
    auto variableTempoProject = neuracoust::daw::defaultProject();
    variableTempoProject.editMode = "Grid";
    variableTempoProject.gridUnit = "1 beat";
    variableTempoProject.tempoBpm = 120;
    variableTempoProject.tempoMap = {{0.0, 120.0}, {10.0, 60.0}};
    assert(std::abs(neuracoust::daw::projectTempoAtSeconds(variableTempoProject, 0.0) - 120.0) < 0.0001);
    assert(std::abs(neuracoust::daw::projectTempoAtSeconds(variableTempoProject, 5.0) - 90.0) < 0.0001);
    assert(std::abs(neuracoust::daw::projectTempoAtSeconds(variableTempoProject, 15.0) - 60.0) < 0.0001);
    const double variableSnap = neuracoust::daw::snapProjectTime(variableTempoProject, 5.1);
    const auto snapReferenceLines = neuracoust::daw::projectMusicalGridLines(variableTempoProject, "1 beat", 0.0, 6.0);
    assert(!snapReferenceLines.empty());
    const auto closestSnapLine = std::min_element(snapReferenceLines.begin(), snapReferenceLines.end(), [](const neuracoust::daw::TimelineGridLine& left, const neuracoust::daw::TimelineGridLine& right) {
        return std::abs(left.timeSeconds - 5.1) < std::abs(right.timeSeconds - 5.1);
    });
    assert(closestSnapLine != snapReferenceLines.end());
    assert(std::abs(variableSnap - closestSnapLine->timeSeconds) < 0.0001);
    const auto variableGridLines = neuracoust::daw::projectMusicalGridLines(variableTempoProject, "1 beat", 4.8, 7.4);
    assert(variableGridLines.size() >= 3);
    assert(variableGridLines.front().timeSeconds >= 4.8 - 0.0001);
    const double earlyVariableGridGap = variableGridLines[1].timeSeconds - variableGridLines[0].timeSeconds;
    const double laterVariableGridGap = variableGridLines[2].timeSeconds - variableGridLines[1].timeSeconds;
    assert(laterVariableGridGap > earlyVariableGridGap);
    const auto barGridLines = neuracoust::daw::projectMusicalGridLines(variableTempoProject, "1 bar", 0.0, 9.0);
    assert(!barGridLines.empty());
    assert(std::all_of(barGridLines.begin(), barGridLines.end(), [](const neuracoust::daw::TimelineGridLine& line) {
        return line.bar;
    }));
    assert(roundTripProject.loopEnabled);
    assert(roundTripProject.loopStartSeconds == 1.0);
    assert(roundTripProject.loopEndSeconds == 3.5);
    assert(roundTripProject.editSelectionEnabled);
    assert(roundTripProject.editSelectionStartSeconds == 0.25);
    assert(roundTripProject.editSelectionEndSeconds == 2.75);
    assert(roundTripProject.appleSiliconCoreIsolationEnabled);
    assert(roundTripProject.windowsProcessorAffinityEnabled);
    assert(roundTripProject.windowsProcessorAffinityMode == "p_core_high_priority");
    assert(roundTripProject.masterInserts.size() == project.masterInserts.size());
    assert(roundTripProject.masterInserts.back().pluginFormat == "VST3");
    assert(roundTripProject.masterInserts.back().bypassed);
    assert(roundTripProject.masterInserts.back().pluginPath.find("Example.vst3") != std::string::npos);
    assert(roundTripProject.masterInserts.back().parameters.size() == 1);
    assert(roundTripProject.masterInserts.back().parameters.front().parameterId == 12345);
    assert(roundTripProject.masterInserts.back().parameters.front().displayName == "Cutoff");
    assert(std::abs(roundTripProject.masterInserts.back().parameters.front().normalizedValue - 0.625) < 0.0001);
    assert(roundTripProject.monitorModules[0].id == "speaker-simulation");
    assert(neuracoust::daw::activeVst3MasterInsertCount(roundTripProject) == 0);
    assert(!neuracoust::daw::hasActiveVst3MasterInserts(roundTripProject));
    const auto roundTripHealth = neuracoust::daw::analyzeProjectHealth(roundTripProject);
    assert(roundTripHealth.clips == 0);
    assert(roundTripHealth.vst3MasterInserts == 1);
    assert(roundTripHealth.missingVst3Inserts == 1);
    assert(neuracoust::daw::summarizeProjectHealth(roundTripHealth).find("missing VST3") != std::string::npos);
    auto clipMetadataProject = neuracoust::daw::defaultProject();
    const auto metadataSourcePath = testTempRoot() / "Session Take 01.wav";
    neuracoust::daw::WavAudioData metadataSource;
    metadataSource.channels = 2;
    metadataSource.sampleRate = 48000;
    metadataSource.interleavedSamples.assign(96, 0.125f);
    std::string metadataWriteError;
    assert(neuracoust::daw::writePcm24WavFile(metadataSourcePath, metadataSource, metadataWriteError));
    const auto metadataClipId = neuracoust::daw::appendAudioClipAt(clipMetadataProject, "Audio 1", metadataSourcePath.string(), 0.0, 0.001);
    assert(metadataClipId == "clip-1");
    assert(clipMetadataProject.clips.back().regionName == "Session Take 01");
    assert(clipMetadataProject.clips.back().sourceFileUid.rfind("src-", 0) == 0);
    assert(clipMetadataProject.clips.back().sourceChannels == 2);
    assert(clipMetadataProject.clips.back().sourceSampleRate == 48000.0);
    assert(clipMetadataProject.clips.back().sourceBitsPerSample == 24);
    assert(!clipMetadataProject.clips.back().sourceFloatingPoint);
    assert(!clipMetadataProject.clips.back().sourceHasBroadcastTimeReference);
    const auto originalSourceUid = clipMetadataProject.clips.back().sourceFileUid;
    assert(neuracoust::daw::setClipRegionName(clipMetadataProject, metadataClipId, "  Verse Guitar  "));
    assert(clipMetadataProject.clips.back().regionName == "Verse Guitar");
    assert(!neuracoust::daw::setClipRegionName(clipMetadataProject, metadataClipId, "   "));
    const auto metadataReplacementPath = testTempRoot() / "Replacement.wav";
    neuracoust::daw::WavAudioData metadataReplacement;
    metadataReplacement.channels = 1;
    metadataReplacement.sampleRate = 96000;
    metadataReplacement.interleavedSamples.assign(96, 0.0625f);
    assert(neuracoust::daw::writeFloat32WavFile(metadataReplacementPath, metadataReplacement, metadataWriteError));
    assert(neuracoust::daw::setClipSourcePath(clipMetadataProject, metadataClipId, metadataReplacementPath.string(), 0.001));
    assert(clipMetadataProject.clips.back().regionName == "Verse Guitar");
    assert(clipMetadataProject.clips.back().sourceFileUid != originalSourceUid);
    assert(clipMetadataProject.clips.back().sourceChannels == 1);
    assert(clipMetadataProject.clips.back().sourceSampleRate == 96000.0);
    assert(clipMetadataProject.clips.back().sourceBitsPerSample == 32);
    assert(clipMetadataProject.clips.back().sourceFloatingPoint);
    clipMetadataProject.clips.back().sourceTempoBpm = 123.5;
    clipMetadataProject.clips.back().sourceTimeSignatureNumerator = 6;
    clipMetadataProject.clips.back().sourceTimeSignatureDenominator = 8;
    clipMetadataProject.clips.back().sourceGrooveFeel = "shuffle";
    clipMetadataProject.clips.back().sourceGrooveSwingAmount = 0.57;
    clipMetadataProject.clips.back().timeScale = 0.875;
    clipMetadataProject.clips.back().tempoSyncPolicy = "stretch-to-project";
    clipMetadataProject.clips.back().pendingTimeStretchToProject = true;
    const auto clipMetadataJson = neuracoust::daw::serializeProject(clipMetadataProject);
    assert(clipMetadataJson.find("\"regionName\":\"Verse Guitar\"") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceFileUid\":\"src-") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceChannels\":1") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceSampleRate\":96000") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceBitsPerSample\":32") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceFloatingPoint\":true") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceTempoBpm\":123.5") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceTimeSignatureNumerator\":6") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceTimeSignatureDenominator\":8") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceGrooveFeel\":\"shuffle\"") != std::string::npos);
    assert(clipMetadataJson.find("\"sourceGrooveSwingAmount\":0.57") != std::string::npos);
    assert(clipMetadataJson.find("\"timeScale\":0.875") != std::string::npos);
    assert(clipMetadataJson.find("\"tempoSyncPolicy\":\"stretch-to-project\"") != std::string::npos);
    assert(clipMetadataJson.find("\"pendingTimeStretchToProject\":true") != std::string::npos);
    neuracoust::daw::ProjectDocument clipMetadataRoundTrip;
    assert(neuracoust::daw::deserializeProject(clipMetadataJson, clipMetadataRoundTrip, projectParseError));
    assert(clipMetadataRoundTrip.clips.back().regionName == "Verse Guitar");
    assert(clipMetadataRoundTrip.clips.back().sourceFileUid == clipMetadataProject.clips.back().sourceFileUid);
    assert(clipMetadataRoundTrip.clips.back().sourceChannels == 1);
    assert(clipMetadataRoundTrip.clips.back().sourceSampleRate == 96000.0);
    assert(clipMetadataRoundTrip.clips.back().sourceBitsPerSample == 32);
    assert(clipMetadataRoundTrip.clips.back().sourceFloatingPoint);
    assert(std::abs(clipMetadataRoundTrip.clips.back().sourceTempoBpm - 123.5) < 0.001);
    assert(clipMetadataRoundTrip.clips.back().sourceTimeSignatureNumerator == 6);
    assert(clipMetadataRoundTrip.clips.back().sourceTimeSignatureDenominator == 8);
    assert(clipMetadataRoundTrip.clips.back().sourceGrooveFeel == "shuffle");
    assert(std::abs(clipMetadataRoundTrip.clips.back().sourceGrooveSwingAmount - 0.57) < 0.001);
    assert(std::abs(clipMetadataRoundTrip.clips.back().timeScale - 0.875) < 0.001);
    assert(clipMetadataRoundTrip.clips.back().tempoSyncPolicy == "stretch-to-project");
    assert(clipMetadataRoundTrip.clips.back().pendingTimeStretchToProject);

    auto pendingStretchProject = neuracoust::daw::defaultProject();
    pendingStretchProject.tempoBpm = 120;
    pendingStretchProject.tempoMap.clear();
    pendingStretchProject.tempoMap.push_back({0.0, 120.0});
    pendingStretchProject.clips.clear();
    const auto pendingStretchId = neuracoust::daw::appendAudioClipAt(pendingStretchProject,
                                                                    "Audio 1",
                                                                    metadataReplacementPath.string(),
                                                                    0.0,
                                                                    4.0);
    assert(!pendingStretchId.empty());
    pendingStretchProject.clips.back().sourceTempoBpm = 60.0;
    pendingStretchProject.clips.back().tempoSyncPolicy = "stretch-to-project";
    pendingStretchProject.clips.back().pendingTimeStretchToProject = true;
    const auto skippedStretchId = neuracoust::daw::appendAudioClipAt(pendingStretchProject,
                                                                    "Audio 1",
                                                                    metadataReplacementPath.string(),
                                                                    5.0,
                                                                    4.0);
    assert(!skippedStretchId.empty());
    pendingStretchProject.clips.back().sourceTempoBpm = 0.0;
    pendingStretchProject.clips.back().tempoSyncPolicy = "stretch-to-project";
    pendingStretchProject.clips.back().pendingTimeStretchToProject = true;
    neuracoust::daw::ClipTimeScaleApplyResult stretchResult;
    assert(neuracoust::daw::applyPendingClipTimeScaleToProjectTempo(pendingStretchProject, stretchResult));
    assert(stretchResult.changedClips == 1);
    assert(stretchResult.skippedClips == 1);
    assert(stretchResult.changedClipIds.size() == 1);
    assert(stretchResult.changedClipIds.front() == pendingStretchId);
    assert(std::abs(pendingStretchProject.clips.front().timeScale - 0.5) < 0.0001);
    assert(std::abs(pendingStretchProject.clips.front().durationSeconds - 2.0) < 0.0001);
    assert(pendingStretchProject.clips.front().tempoSyncPolicy == "project-tempo");
    assert(!pendingStretchProject.clips.front().pendingTimeStretchToProject);
    assert(pendingStretchProject.clips.back().tempoSyncPolicy == "stretch-to-project");
    assert(pendingStretchProject.clips.back().pendingTimeStretchToProject);
    std::string stretchMessage;
    assert(!neuracoust::daw::applyClipTimeScaleToProjectTempo(pendingStretchProject, skippedStretchId, stretchMessage));
    assert(!stretchMessage.empty());

    const auto bwfPath = testTempRoot() / "BWF Timestamp.wav";
    writeBwfTimeReferenceTestWav(bwfPath, 96000);
    neuracoust::daw::WavAudioData bwfAudio;
    assert(neuracoust::daw::readPcmWavFile(bwfPath, bwfAudio, metadataWriteError));
    assert(bwfAudio.hasBroadcastTimeReference);
    assert(bwfAudio.broadcastTimeReferenceSamples == 96000);
    assert(std::abs(bwfAudio.broadcastTimeReferenceSeconds - 2.0) < 0.0001);
    auto bwfProject = neuracoust::daw::defaultProject();
    const auto bwfClipId = neuracoust::daw::appendAudioClipAt(bwfProject, "Audio 1", bwfPath.string(), 0.0, 0.001);
    assert(!bwfClipId.empty());
    assert(bwfProject.clips.back().sourceHasBroadcastTimeReference);
    assert(bwfProject.clips.back().sourceTimeReferenceSamples == 96000);
    assert(std::abs(bwfProject.clips.back().sourceTimeReferenceSeconds - 2.0) < 0.0001);
    const auto bwfJson = neuracoust::daw::serializeProject(bwfProject);
    assert(bwfJson.find("\"sourceHasBroadcastTimeReference\":true") != std::string::npos);
    assert(bwfJson.find("\"sourceTimeReferenceSamples\":96000") != std::string::npos);
    neuracoust::daw::ProjectDocument bwfRoundTrip;
    assert(neuracoust::daw::deserializeProject(bwfJson, bwfRoundTrip, projectParseError));
    assert(bwfRoundTrip.clips.back().sourceHasBroadcastTimeReference);
    assert(bwfRoundTrip.clips.back().sourceTimeReferenceSamples == 96000);
    assert(std::abs(bwfRoundTrip.clips.back().sourceTimeReferenceSeconds - 2.0) < 0.0001);

    auto edlProject = neuracoust::daw::defaultProject();
    edlProject.name = "EDL Test";
    edlProject.sampleRate = 48000.0;
    edlProject.bitDepth = 64;
    edlProject.timecodeStartSeconds = 3600.0;
    edlProject.editMode = "Grid";
    edlProject.gridUnit = "1 beat";
    edlProject.timeSignatureNumerator = 3;
    edlProject.timeSignatureDenominator = 4;
    edlProject.timeSignatureMap = {{0.0, 3, 4}, {16.0, 6, 8}};
    edlProject.grooveFeel = "triplet";
    edlProject.grooveSwingAmount = 0.333;
    edlProject.detectedKey = "G";
    edlProject.detectedKeyMode = "major";
    edlProject.tempoBpm = 120;
    edlProject.tempoMap.clear();
    edlProject.tempoMap.push_back({0.0, 120.0});
    edlProject.tempoMap.push_back({12.0, 126.5});
    edlProject.markers.clear();   // defaultProject() seeds a start marker; this fixture controls its own
    edlProject.markers.push_back({"midi-marker-a", "Verse 1", 1.25});
    edlProject.chordEvents.push_back({"chord-export-a", "Verse / Cmaj7", 1.5});
    edlProject.lyricEvents.push_back({"lyric-export-a", "Hello Neuracoust", 1.75});
    edlProject.videoFrameRate = 29.97;
    edlProject.videoSources.push_back({"edl-video", "/tmp/Picture Lock.mov", "Picture Lock", 29.97, 90.0, 1920, 1080, true});
    edlProject.videoClips.push_back({"edl-video-clip", "edl-video", "Picture Lock", 0.0, 90.0, 0.0, 3600.0, false, false});
    const auto edlClipId = neuracoust::daw::appendAudioClipAt(edlProject, "Audio 1", "/tmp/Session Take 01.wav", 2.0, 1.0);
    assert(neuracoust::daw::setClipRegionName(edlProject, edlClipId, "Verse Guitar"));
    assert(neuracoust::daw::setClipGainDb(edlProject, edlClipId, -3.0f));
    assert(neuracoust::daw::setClipFades(edlProject, edlClipId, 0.1, 0.2));
    assert(neuracoust::daw::setClipFadeCurves(edlProject, edlClipId, "linear", "fast"));
    auto edlClipIt = std::find_if(edlProject.clips.begin(), edlProject.clips.end(), [&](const auto& clip) {
        return clip.id == edlClipId;
    });
    assert(edlClipIt != edlProject.clips.end());
    edlClipIt->sourceHasBroadcastTimeReference = true;
    edlClipIt->sourceTimeReferenceSamples = 96000;
    edlClipIt->sourceTimeReferenceSeconds = 2.0;
    edlClipIt->sourceTempoBpm = 117.6;
    edlClipIt->sourceTimeSignatureNumerator = 6;
    edlClipIt->sourceTimeSignatureDenominator = 8;
    edlClipIt->sourceGrooveFeel = "shuffle";
    edlClipIt->sourceGrooveSwingAmount = 0.58;
    const auto mutedEdlClipId = neuracoust::daw::appendAudioClipAt(edlProject, "Audio 1", "/tmp/Muted.wav", 4.0, 1.0);
    assert(neuracoust::daw::setClipMuted(edlProject, mutedEdlClipId, true));
    edlProject.clips.push_back({"master-edl", "Master", "/tmp/Master.wav", 5.0, 1.0, 0.0, 0.0f});
    const auto edlResult = neuracoust::daw::exportProjectToCmx3600Edl(edlProject, 30.0);
    assert(edlResult.ok);
    assert(edlResult.eventCount == 1);
    assert(edlResult.tempoEventCount == 2);
    assert(edlResult.markerCount == 1);
    assert(edlResult.chordEventCount == 1);
    assert(edlResult.lyricEventCount == 1);
    assert(edlResult.text.find("TITLE: EDL Test") != std::string::npos);
    assert(edlResult.text.find("FCM: NON-DROP FRAME") != std::string::npos);
    assert(edlResult.text.find("* BIT_DEPTH: 64") != std::string::npos);
    assert(edlResult.text.find("* INTERCHANGE_PROFILE: Final Cut Pro") != std::string::npos);
    assert(edlResult.text.find("* TIMECODE_START_SECONDS: 3600.000000") != std::string::npos);
    assert(edlResult.text.find("* EDIT_MODE: Grid") != std::string::npos);
    assert(edlResult.text.find("* GRID_UNIT: 1 beat") != std::string::npos);
    assert(edlResult.text.find("* TIME_SIGNATURE: 3/4") != std::string::npos);
    assert(edlResult.text.find("* TIME_SIGNATURE_MARKER: 16.000000s 6/8") != std::string::npos);
    assert(edlResult.text.find("* GROOVE: feel=triplet; swing=0.333") != std::string::npos);
    assert(edlResult.text.find("* KEY: root=G; mode=major") != std::string::npos);
    assert(edlResult.text.find("* TEMPO: 12.000000s 126.500 BPM") != std::string::npos);
    assert(edlResult.text.find("* MARKER: 1.250000s Verse 1") != std::string::npos);
    assert(edlResult.text.find("* CHORD_SECTION: 1.500000s Verse / Cmaj7") != std::string::npos);
    assert(edlResult.text.find("* LYRIC: 1.750000s Hello Neuracoust") != std::string::npos);
    assert(edlResult.text.find("* VIDEO_SOURCE: id=edl-video") != std::string::npos);
    assert(edlResult.text.find("* VIDEO_CLIP: id=edl-video-clip") != std::string::npos);
    assert(edlResult.text.find("001  VERSE_GU A     C        00:00:00:00 00:00:01:00 01:00:02:00 01:00:03:00") != std::string::npos);
    assert(edlResult.text.find("* FROM CLIP NAME: Verse Guitar") != std::string::npos);
    assert(edlResult.text.find("* SOURCE UID: src-") != std::string::npos);
    assert(edlResult.text.find("* BWF_TIME_REFERENCE: 96000 samples 2.000000s") != std::string::npos);
    assert(edlResult.text.find("* SOURCE_MUSIC: tempo=117.600; timeSignature=6/8; grooveFeel=shuffle; grooveSwing=0.580") != std::string::npos);
    assert(edlResult.text.find("* TRACK: Audio 1 (audio)") != std::string::npos);
    assert(edlResult.text.find("* FADES: IN 0.100s linear / OUT 0.200s fast") != std::string::npos);
    assert(edlResult.text.find("Muted") == std::string::npos);
    assert(edlResult.text.find("master-edl") == std::string::npos);
    edlProject.videoFrameRate = 29.97;
    edlProject.timecodeDropFrame = true;
    const auto dropFrameEdlResult = neuracoust::daw::exportProjectToCmx3600Edl(edlProject, 29.97);
    assert(dropFrameEdlResult.ok);
    assert(dropFrameEdlResult.text.find("FCM: DROP FRAME") != std::string::npos);
    assert(dropFrameEdlResult.text.find(";") != std::string::npos);
    const auto emptyEdlResult = neuracoust::daw::exportProjectToCmx3600Edl(neuracoust::daw::defaultProject(), 30.0);
    assert(emptyEdlResult.ok);
    assert(emptyEdlResult.eventCount == 0);
    const auto invalidEdlResult = neuracoust::daw::exportProjectToCmx3600Edl(edlProject, 0.0);
    assert(!invalidEdlResult.ok);
    const auto resolveEdlResult = neuracoust::daw::exportProjectToCmx3600Edl(edlProject, 29.97, neuracoust::daw::TimelineInterchangeProfile::DaVinciResolve);
    assert(resolveEdlResult.ok);
    assert(resolveEdlResult.text.find("* INTERCHANGE_PROFILE: DaVinci Resolve") != std::string::npos);
    assert(resolveEdlResult.message.find("DaVinci Resolve") != std::string::npos);
    const auto edlImport = neuracoust::daw::importProjectFromCmx3600EdlText(edlResult.text, 30.0);
    assert(edlImport.ok);
    assert(edlImport.clipCount == 1);
    assert(edlImport.trackCount == 3);
    assert(edlImport.project.tracks.front().outputBus == "Master");
    assert(edlImport.project.tracks[edlImport.project.tracks.size() - 2].name == "Master");
    assert(edlImport.project.tracks[edlImport.project.tracks.size() - 2].outputBus == "Monitor");
    assert(edlImport.project.tracks.back().name == "Monitor");
    assert(edlImport.project.tracks.back().outputBus == "Main 1-2");
    assert(edlImport.chordEventCount == 1);
    assert(edlImport.lyricEventCount == 1);
    assert(edlImport.project.name == "EDL Test");
    assert(std::abs(edlImport.project.sampleRate - 48000.0) < 0.001);
    assert(edlImport.project.bitDepth == 64);
    assert(edlImport.project.videoSources.size() == 1);
    assert(edlImport.project.videoClips.size() == 1);
    assert(std::abs(edlImport.project.timecodeStartSeconds - 3600.0) < 0.0001);
    assert(edlImport.project.editMode == "Grid");
    assert(edlImport.project.gridUnit == "1 beat");
    assert(edlImport.project.timeSignatureMap.size() == 2);
    assert(edlImport.project.timeSignatureMap.back().numerator == 6);
    assert(edlImport.project.timeSignatureMap.back().denominator == 8);
    assert(edlImport.project.tempoMap.size() == 2);
    assert(std::abs(edlImport.project.tempoMap.back().bpm - 126.5) < 0.001);
    assert(edlImport.project.markers.size() == 1);
    assert(edlImport.project.markers.front().name == "Verse 1");
    assert(edlImport.project.chordEvents.size() == 1);
    assert(edlImport.project.chordEvents.front().name == "Verse / Cmaj7");
    assert(edlImport.project.lyricEvents.size() == 1);
    assert(edlImport.project.lyricEvents.front().text == "Hello Neuracoust");
    assert(edlImport.project.tracks.front().name == "Audio 1");
    assert(edlImport.project.tracks.front().trackType == "audio");
    assert(edlImport.project.tracks[edlImport.project.tracks.size() - 2].trackType == "master");
    assert(edlImport.project.tracks.back().trackType == "monitor");
    assert(edlImport.project.clips.front().regionName == "Verse Guitar");
    assert(edlImport.project.clips.front().trackName == "Audio 1");
    assert(edlImport.project.clips.front().sourcePath == "/tmp/Session Take 01.wav");
    assert(edlImport.project.clips.front().sourceFileUid.find("src-") == 0);
    assert(edlImport.project.clips.front().sourceHasBroadcastTimeReference);
    assert(edlImport.project.clips.front().sourceTimeReferenceSamples == 96000);
    assert(std::abs(edlImport.project.clips.front().sourceTimeReferenceSeconds - 2.0) < 0.0001);
    assert(std::abs(edlImport.project.clips.front().sourceTempoBpm - 117.6) < 0.001);
    assert(edlImport.project.clips.front().sourceTimeSignatureNumerator == 6);
    assert(edlImport.project.clips.front().sourceTimeSignatureDenominator == 8);
    assert(edlImport.project.clips.front().sourceGrooveFeel == "shuffle");
    assert(std::abs(edlImport.project.clips.front().sourceGrooveSwingAmount - 0.58) < 0.001);
    assert(std::abs(edlImport.project.clips.front().sourceOffsetSeconds - 0.0) < 0.0001);
    assert(std::abs(edlImport.project.clips.front().startSeconds - 2.0) < 0.0001);
    assert(std::abs(edlImport.project.clips.front().durationSeconds - 1.0) < 0.0001);
    assert(std::abs(edlImport.project.clips.front().gainDb + 3.0f) < 0.001f);
    assert(std::abs(edlImport.project.clips.front().fadeInSeconds - 0.1) < 0.0001);
    assert(edlImport.project.clips.front().fadeOutCurve == "fast");
    assert(!neuracoust::daw::importProjectFromCmx3600EdlText("bad\n", 30.0).ok);
    assert(!neuracoust::daw::importProjectFromCmx3600EdlText(edlResult.text, 0.0).ok);
    const auto fcpxmlResult = neuracoust::daw::exportProjectToFcpxml(edlProject);
    assert(fcpxmlResult.ok);
    assert(fcpxmlResult.text.find("tcFormat=\"DF\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.timecodeDropFrame") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.interchangeProfile\" value=\"Final Cut Pro\"") != std::string::npos);
    assert(fcpxmlResult.clipCount == 1);
    assert(fcpxmlResult.assetCount == 1);
    assert(fcpxmlResult.tempoEventCount == 2);
    assert(fcpxmlResult.markerCount == 1);
    assert(fcpxmlResult.chordEventCount == 1);
    assert(fcpxmlResult.lyricEventCount == 1);
    assert(fcpxmlResult.text.find("<fcpxml version=\"1.10\">") != std::string::npos);
    assert(fcpxmlResult.text.find("<format id=\"fmt1\"") != std::string::npos);
    assert(fcpxmlResult.text.find("<asset id=\"r1\" name=\"Session Take 01.wav\"") != std::string::npos);
    assert(fcpxmlResult.text.find("duration=\"3000/1000s\" format=\"fmt1\" tcStart=\"3600000/1000s\"") != std::string::npos);
    assert(fcpxmlResult.text.find("<asset-clip name=\"Verse Guitar\" ref=\"r1\" offset=\"2000/1000s\" start=\"0/1000s\" duration=\"1000/1000s\"") != std::string::npos);
    assert(fcpxmlResult.text.find("sourceUid=src-") != std::string::npos);
    assert(fcpxmlResult.text.find("bwfTimeRefSamples=96000") != std::string::npos);
    assert(fcpxmlResult.text.find("sourceTempoBpm=117.600") != std::string::npos);
    assert(fcpxmlResult.text.find("sourceTimeSignature=6/8") != std::string::npos);
    assert(fcpxmlResult.text.find("sourceGrooveFeel=shuffle") != std::string::npos);
    assert(fcpxmlResult.text.find("sourceGrooveSwingAmount=0.580") != std::string::npos);
    assert(fcpxmlResult.text.find("fadeIn=0.100 linear; fadeOut=0.200 fast") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.bitDepth\" value=\"64\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.timeSignature\" value=\"3/4\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.timeSignatureMarker.1\" value=\"time=16.000000; signature=6/8\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.grooveFeel\" value=\"triplet\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.grooveSwingAmount\" value=\"0.333\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.detectedKey\" value=\"G\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.detectedKeyMode\" value=\"major\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.tempo.1\" value=\"time=12.000000; bpm=126.500\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.marker.0\" value=\"id=midi-marker-a; name=Verse 1; time=1.250000\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.chordSection.0\" value=\"id=chord-export-a; name=Verse / Cmaj7; time=1.500000\"") != std::string::npos);
    assert(fcpxmlResult.text.find("com.neuracoust.daw.lyric.0\" value=\"id=lyric-export-a; text=Hello Neuracoust; time=1.750000\"") != std::string::npos);
    assert(fcpxmlResult.text.find("Muted") == std::string::npos);
    assert(fcpxmlResult.text.find("master-edl") == std::string::npos);
    const auto resolveFcpxmlResult = neuracoust::daw::exportProjectToFcpxml(edlProject, neuracoust::daw::TimelineInterchangeProfile::DaVinciResolve);
    assert(resolveFcpxmlResult.ok);
    assert(resolveFcpxmlResult.text.find("Neuracoust DAW Resolve Timeline") != std::string::npos);
    assert(resolveFcpxmlResult.text.find("com.neuracoust.daw.interchangeProfile\" value=\"DaVinci Resolve\"") != std::string::npos);
    assert(resolveFcpxmlResult.message.find("DaVinci Resolve") != std::string::npos);
    const auto youtubePlan = neuracoust::daw::makeVideoDeliveryPlan(edlProject,
                                                                    neuracoust::daw::VideoDeliveryPreset::YouTube1080p,
                                                                    "/tmp/EDL Test YouTube.mp4",
                                                                    "/tmp/EDL Test Mix.wav");
    assert(youtubePlan.ok);
    assert(youtubePlan.presetId == "youtube-1080p");
    assert(youtubePlan.width == 1920);
    assert(youtubePlan.height == 1080);
    assert(youtubePlan.audioBitrateKbps == 320);
    assert(youtubePlan.clips.size() == 1);
    assert(youtubePlan.ffmpegCommand.find("-movflags +faststart") != std::string::npos);
    assert(youtubePlan.ffmpegCommand.find("EDL Test Mix.wav") != std::string::npos);
    const auto sharePlan = neuracoust::daw::makeVideoDeliveryPlan(edlProject,
                                                                  neuracoust::daw::VideoDeliveryPreset::SharePreview720p,
                                                                  "/tmp/EDL Test Share.mp4");
    assert(sharePlan.ok);
    assert(sharePlan.presetId == "share-preview-720p");
    assert(sharePlan.videoBitrateKbps < youtubePlan.videoBitrateKbps);
    const auto fcpxmlImport = neuracoust::daw::importProjectFromFcpxmlText(fcpxmlResult.text);
    assert(fcpxmlImport.ok);
    assert(fcpxmlImport.clipCount == 1);
    assert(fcpxmlImport.trackCount == 3);
    assert(fcpxmlImport.chordEventCount == 1);
    assert(fcpxmlImport.lyricEventCount == 1);
    assert(fcpxmlImport.project.name == "EDL Test");
    assert(std::abs(fcpxmlImport.project.sampleRate - 48000.0) < 0.001);
    assert(fcpxmlImport.project.bitDepth == 64);
    assert(fcpxmlImport.project.timecodeDropFrame);
    assert(fcpxmlImport.project.videoSources.size() == 1);
    assert(fcpxmlImport.project.videoClips.size() == 1);
    assert(fcpxmlImport.project.editMode == "Grid");
    assert(fcpxmlImport.project.gridUnit == "1 beat");
    assert(fcpxmlImport.project.timeSignatureNumerator == 3);
    assert(fcpxmlImport.project.timeSignatureDenominator == 4);
    assert(fcpxmlImport.project.timeSignatureMap.size() == 2);
    assert(fcpxmlImport.project.timeSignatureMap.back().numerator == 6);
    assert(fcpxmlImport.project.timeSignatureMap.back().denominator == 8);
    assert(fcpxmlImport.project.grooveFeel == "triplet");
    assert(std::abs(fcpxmlImport.project.grooveSwingAmount - 0.333) < 0.001);
    assert(fcpxmlImport.project.detectedKey == "G");
    assert(fcpxmlImport.project.detectedKeyMode == "major");
    assert(fcpxmlImport.project.tempoMap.size() == 2);
    assert(std::abs(fcpxmlImport.project.tempoMap.back().bpm - 126.5) < 0.001);
    assert(fcpxmlImport.project.markers.size() == 1);
    assert(fcpxmlImport.project.markers.front().name == "Verse 1");
    assert(fcpxmlImport.project.chordEvents.size() == 1);
    assert(fcpxmlImport.project.chordEvents.front().name == "Verse / Cmaj7");
    assert(fcpxmlImport.project.lyricEvents.size() == 1);
    assert(fcpxmlImport.project.lyricEvents.front().text == "Hello Neuracoust");
    assert(fcpxmlImport.project.tracks.front().name == "Audio 1");
    assert(fcpxmlImport.project.tracks.front().trackType == "audio");
    assert(fcpxmlImport.project.tracks.front().outputBus == "Master");
    assert(fcpxmlImport.project.tracks[fcpxmlImport.project.tracks.size() - 2].name == "Master");
    assert(fcpxmlImport.project.tracks[fcpxmlImport.project.tracks.size() - 2].outputBus == "Monitor");
    assert(fcpxmlImport.project.tracks.back().name == "Monitor");
    assert(fcpxmlImport.project.tracks.back().outputBus == "Main 1-2");
    assert(fcpxmlImport.project.tracks.back().trackType == "monitor");
    assert(fcpxmlImport.project.clips.front().id.find("clip-") == 0);
    assert(fcpxmlImport.project.clips.front().regionName == "Verse Guitar");
    assert(fcpxmlImport.project.clips.front().trackName == "Audio 1");
    assert(fcpxmlImport.project.clips.front().sourcePath == "/tmp/Session Take 01.wav");
    assert(fcpxmlImport.project.clips.front().sourceFileUid.find("src-") == 0);
    assert(fcpxmlImport.project.clips.front().sourceHasBroadcastTimeReference);
    assert(fcpxmlImport.project.clips.front().sourceTimeReferenceSamples == 96000);
    assert(std::abs(fcpxmlImport.project.clips.front().sourceTempoBpm - 117.6) < 0.001);
    assert(fcpxmlImport.project.clips.front().sourceTimeSignatureNumerator == 6);
    assert(fcpxmlImport.project.clips.front().sourceTimeSignatureDenominator == 8);
    assert(fcpxmlImport.project.clips.front().sourceGrooveFeel == "shuffle");
    assert(std::abs(fcpxmlImport.project.clips.front().sourceGrooveSwingAmount - 0.58) < 0.001);
    assert(std::abs(fcpxmlImport.project.clips.front().startSeconds - 2.0) < 0.0001);
    assert(std::abs(fcpxmlImport.project.clips.front().durationSeconds - 1.0) < 0.0001);
    assert(std::abs(fcpxmlImport.project.clips.front().gainDb + 3.0f) < 0.001f);
    assert(std::abs(fcpxmlImport.project.clips.front().fadeInSeconds - 0.1) < 0.0001);
    assert(fcpxmlImport.project.clips.front().fadeOutCurve == "fast");
    assert(!neuracoust::daw::importProjectFromFcpxmlText("bad\n").ok);
    const auto midiExport = neuracoust::daw::exportProjectTempoMapToMidi(edlProject, 480);
    assert(midiExport.ok);
    assert(midiExport.tempoEventCount >= 2);
    assert(midiExport.markerCount == 1);
    assert(midiExport.chordEventCount == 1);
    assert(midiExport.lyricEventCount == 1);
    assert(midiExport.regionCueCount == 1);
    assert(midiExport.data.size() > 64);
    const std::string midiBytes(reinterpret_cast<const char*>(midiExport.data.data()), midiExport.data.size());
    assert(midiBytes.find("MThd") == 0);
    assert(midiExport.data[8] == 0x00 && midiExport.data[9] == 0x01);
    assert(midiExport.data[10] == 0x00 && midiExport.data[11] == 0x02);
    assert(midiExport.data[12] == 0x01 && midiExport.data[13] == 0xe0);
    assert(midiBytes.find("MTrk") != std::string::npos);
    assert(midiBytes.find("Verse 1") != std::string::npos);
    assert(midiBytes.find("CHORD_SECTION id=chord-export-a") != std::string::npos);
    assert(midiBytes.find("Verse / Cmaj7") != std::string::npos);
    assert(midiBytes.find("LYRIC id=lyric-export-a") != std::string::npos);
    assert(midiBytes.find("Hello Neuracoust") != std::string::npos);
    assert(midiBytes.find("REGION clipId=clip-") != std::string::npos);
    assert(midiBytes.find("sourceTempoBpm=117.600") != std::string::npos);
    assert(midiBytes.find("sourceTimeSignature=6/8") != std::string::npos);
    assert(midiBytes.find("sourceGrooveFeel=shuffle") != std::string::npos);
    const std::vector<uint8_t> tempoMetaPrefix {0xff, 0x51, 0x03};
    assert(std::search(midiExport.data.begin(), midiExport.data.end(), tempoMetaPrefix.begin(), tempoMetaPrefix.end()) != midiExport.data.end());
    const std::vector<uint8_t> threeFourSignatureMeta {0xff, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08};
    assert(std::search(midiExport.data.begin(), midiExport.data.end(), threeFourSignatureMeta.begin(), threeFourSignatureMeta.end()) != midiExport.data.end());
    const std::vector<uint8_t> edlSixEightSignatureMeta {0xff, 0x58, 0x04, 0x06, 0x03, 0x24, 0x08};
    assert(std::search(midiExport.data.begin(), midiExport.data.end(), edlSixEightSignatureMeta.begin(), edlSixEightSignatureMeta.end()) != midiExport.data.end());
    const auto sixEightMidiExport = neuracoust::daw::exportProjectTempoMapToMidi(sixEightProject, 480);
    assert(sixEightMidiExport.ok);
    const std::vector<uint8_t> sixEightSignatureMeta {0xff, 0x58, 0x04, 0x06, 0x03, 0x24, 0x08};
    assert(std::search(sixEightMidiExport.data.begin(), sixEightMidiExport.data.end(), sixEightSignatureMeta.begin(), sixEightSignatureMeta.end()) != sixEightMidiExport.data.end());
    auto midiNoteExportProject = edlProject;
    const auto exportMidiTrack = neuracoust::daw::addMidiTrack(midiNoteExportProject);
    const auto exportMidiRegion = neuracoust::daw::addMidiRegion(midiNoteExportProject, exportMidiTrack, 1.0, 2.0, "Export Keys");
    assert(!exportMidiRegion.empty());
    assert(neuracoust::daw::addMidiNote(midiNoteExportProject, exportMidiRegion, 60, 0.0, 0.5, 100, 1) == "note-1");
    assert(neuracoust::daw::addMidiNote(midiNoteExportProject, exportMidiRegion, 64, 0.5, 0.5, 90, 2) == "note-2");
    assert(neuracoust::daw::addMidiControllerEvent(midiNoteExportProject, exportMidiRegion, 0.25, 1, 80, 1) == "cc-1");
    assert(neuracoust::daw::addMidiPitchBendEvent(midiNoteExportProject, exportMidiRegion, 0.75, 12288, 2) == "bend-1");
    assert(neuracoust::daw::addMidiProgramChangeEvent(midiNoteExportProject, exportMidiRegion, 0.0, 11, 1) == "program-1");
    const auto midiNoteExport = neuracoust::daw::exportProjectTempoMapToMidi(midiNoteExportProject, 480);
    assert(midiNoteExport.ok);
    assert(midiNoteExport.midiTrackCount == 1);
    assert(midiNoteExport.midiNoteCount == 2);
    assert(midiNoteExport.midiControllerEventCount == 1);
    assert(midiNoteExport.midiPitchBendEventCount == 1);
    assert(midiNoteExport.midiProgramChangeEventCount == 1);
    assert(midiNoteExport.data[10] == 0x00 && midiNoteExport.data[11] == 0x03);
    assert(std::find(midiNoteExport.data.begin(), midiNoteExport.data.end(), static_cast<uint8_t>(0x90)) != midiNoteExport.data.end());
    assert(std::find(midiNoteExport.data.begin(), midiNoteExport.data.end(), static_cast<uint8_t>(0x81)) != midiNoteExport.data.end());
    assert(std::find(midiNoteExport.data.begin(), midiNoteExport.data.end(), static_cast<uint8_t>(0xb0)) != midiNoteExport.data.end());
    assert(std::find(midiNoteExport.data.begin(), midiNoteExport.data.end(), static_cast<uint8_t>(0xe1)) != midiNoteExport.data.end());
    assert(std::find(midiNoteExport.data.begin(), midiNoteExport.data.end(), static_cast<uint8_t>(0xc0)) != midiNoteExport.data.end());
    const auto midiImport = neuracoust::daw::importProjectFromMidiData(midiNoteExport.data);
    assert(midiImport.ok);
    assert(midiImport.trackCount == 1);
    assert(midiImport.regionCount == 1);
    assert(midiImport.noteCount == 2);
    assert(midiImport.controllerEventCount == 1);
    assert(midiImport.pitchBendEventCount == 1);
    assert(midiImport.programChangeEventCount == 1);
    assert(midiImport.project.midiRegions.size() == 1);
    assert(midiImport.project.midiRegions.front().notes.size() == 2);
    assert(midiImport.project.midiRegions.front().notes[0].pitch == 60);
    assert(midiImport.project.midiRegions.front().notes[1].pitch == 64);
    assert(midiImport.project.midiRegions.front().notes[1].channel == 2);
    assert(midiImport.project.midiRegions.front().controllerEvents.size() == 1);
    assert(midiImport.project.midiRegions.front().controllerEvents.front().controller == 1);
    assert(midiImport.project.midiRegions.front().controllerEvents.front().value == 80);
    assert(midiImport.project.midiRegions.front().pitchBendEvents.size() == 1);
    assert(midiImport.project.midiRegions.front().pitchBendEvents.front().value == 12288);
    assert(midiImport.project.midiRegions.front().pitchBendEvents.front().channel == 2);
    assert(midiImport.project.midiRegions.front().programChangeEvents.size() == 1);
    assert(midiImport.project.midiRegions.front().programChangeEvents.front().program == 11);
    std::vector<uint8_t> duplicateNamedMidi {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x02, 0x01, 0xe0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x16,
        0x00, 0xff, 0x03, 0x05, 'L', 'a', 'y', 'e', 'r',
        0x00, 0x90, 0x3c, 0x60, 0x83, 0x60, 0x80, 0x3c, 0x00, 0x00, 0xff, 0x2f, 0x00,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x16,
        0x00, 0xff, 0x03, 0x05, 'L', 'a', 'y', 'e', 'r',
        0x00, 0x91, 0x40, 0x5a, 0x83, 0x60, 0x81, 0x40, 0x00, 0x00, 0xff, 0x2f, 0x00
    };
    const auto duplicateNamedMidiImport = neuracoust::daw::importProjectFromMidiData(duplicateNamedMidi);
    assert(duplicateNamedMidiImport.ok);
    assert(duplicateNamedMidiImport.trackCount == 2);
    assert(duplicateNamedMidiImport.regionCount == 2);
    assert(duplicateNamedMidiImport.noteCount == 2);
    assert(duplicateNamedMidiImport.project.tracks[0].name == "Layer");
    assert(duplicateNamedMidiImport.project.tracks[1].name == "Layer 2");
    assert(duplicateNamedMidiImport.project.midiRegions[0].notes.front().pitch == 60);
    assert(duplicateNamedMidiImport.project.midiRegions[1].notes.front().pitch == 64);
    const auto midiPath = testTempRoot() / "interchange" / "EDL Test.mid";
    const auto writtenMidi = neuracoust::daw::writeProjectMidiFile(edlProject, midiPath, 960);
    assert(writtenMidi.ok);
    assert(writtenMidi.outputPath == midiPath.string());
    assert(std::filesystem::exists(midiPath));
    assert(!std::filesystem::exists(std::filesystem::path(midiPath.string() + ".writing")));
    {
        std::ifstream midiIn(midiPath, std::ios::binary);
        std::string writtenMidiBytes((std::istreambuf_iterator<char>(midiIn)), std::istreambuf_iterator<char>());
        assert(writtenMidiBytes.find("MThd") == 0);
        assert(static_cast<unsigned char>(writtenMidiBytes[12]) == 0x03);
        assert(static_cast<unsigned char>(writtenMidiBytes[13]) == 0xc0);
        assert(writtenMidiBytes.find("REGION clipId=clip-") != std::string::npos);
        assert(writtenMidiBytes.find("CHORD_SECTION id=chord-export-a") != std::string::npos);
        assert(writtenMidiBytes.find("LYRIC id=lyric-export-a") != std::string::npos);
    }
    auto aafReference = neuracoust::daw::exportProjectToAafReference(edlProject);
    assert(aafReference.ok);
    assert(aafReference.clipCount == 1);
    assert(aafReference.trackCount == 1);
    assert(aafReference.chordEventCount == 1);
    assert(aafReference.lyricEventCount == 1);
    assert(aafReference.text.find("NEURACOUST_DAW_AAF_REFERENCE_EXPORT 1") != std::string::npos);
    assert(aafReference.text.find("not a native binary AAF container") != std::string::npos);
    assert(aafReference.text.find("PROJECT name=\"EDL Test\" sampleRate=48000 bitDepth=64 timecodeStartSeconds=3600.000000") != std::string::npos);
    assert(aafReference.text.find("VIDEO_SOURCE index=0 id=\"edl-video\"") != std::string::npos);
    assert(aafReference.text.find("VIDEO_CLIP index=0 id=\"edl-video-clip\"") != std::string::npos);
    assert(aafReference.text.find("timeSignature=\"3/4\" grooveFeel=\"triplet\" grooveSwingAmount=0.333 detectedKey=\"G\" detectedKeyMode=\"major\"") != std::string::npos);
    assert(aafReference.text.find("TIME_SIGNATURE index=1 timeSeconds=16.000000 signature=\"6/8\"") != std::string::npos);
    assert(aafReference.text.find("timeSeconds=12.000000 bpm=126.500") != std::string::npos);
    assert(aafReference.text.find("CHORD_SECTION index=0 id=\"chord-export-a\" name=\"Verse / Cmaj7\" timeSeconds=1.500000") != std::string::npos);
    assert(aafReference.text.find("LYRIC index=0 id=\"lyric-export-a\" text=\"Hello Neuracoust\" timeSeconds=1.750000") != std::string::npos);
    assert(aafReference.text.find("TRACK name=\"Audio 1\" type=\"audio\" input=\"Input 1\" output=\"Master\"") != std::string::npos);
    assert(aafReference.text.find("CLIP index=0 id=\"clip-") != std::string::npos);
    assert(aafReference.text.find("name=\"Verse Guitar\" track=\"Audio 1\" source=\"/tmp/Session Take 01.wav\"") != std::string::npos);
    assert(aafReference.text.find("startSeconds=2.000000 endSeconds=3.000000 durationSeconds=1.000000") != std::string::npos);
    assert(aafReference.text.find("sourceTimeReferenceSamples=96000") != std::string::npos);
    assert(aafReference.text.find("sourceTempoBpm=117.600 sourceTimeSignature=\"6/8\" sourceGrooveFeel=\"shuffle\" sourceGrooveSwingAmount=0.580") != std::string::npos);
    assert(aafReference.text.find("gainDb=-3.000 fadeInSeconds=0.100000 fadeInCurve=\"linear\" fadeOutSeconds=0.200000 fadeOutCurve=\"fast\"") != std::string::npos);
    assert(aafReference.text.find("Muted") == std::string::npos);
    const auto aafImport = neuracoust::daw::importProjectFromAafReferenceText(aafReference.text);
    assert(aafImport.ok);
    assert(aafImport.clipCount == 1);
    assert(aafImport.chordEventCount == 1);
    assert(aafImport.lyricEventCount == 1);
    assert(aafImport.project.name == "EDL Test");
    assert(aafImport.project.videoSources.size() == 1);
    assert(aafImport.project.videoClips.size() == 1);
    assert(aafImport.project.bitDepth == 64);
    assert(aafImport.project.timeSignatureNumerator == 3);
    assert(aafImport.project.timeSignatureDenominator == 4);
    assert(aafImport.project.timeSignatureMap.size() == 2);
    assert(aafImport.project.timeSignatureMap.back().numerator == 6);
    assert(aafImport.project.timeSignatureMap.back().denominator == 8);
    assert(aafImport.project.grooveFeel == "triplet");
    assert(std::abs(aafImport.project.grooveSwingAmount - 0.333) < 0.001);
    assert(aafImport.project.detectedKey == "G");
    assert(aafImport.project.detectedKeyMode == "major");
    assert(aafImport.project.tracks.size() == 3);
    assert(aafImport.project.tracks.front().name == "Audio 1");
    assert(aafImport.project.tracks.front().outputBus == "Master");
    assert(aafImport.project.tracks[aafImport.project.tracks.size() - 2].trackType == "master");
    assert(aafImport.project.tracks[aafImport.project.tracks.size() - 2].outputBus == "Monitor");
    assert(aafImport.project.tracks.back().trackType == "monitor");
    assert(aafImport.project.tracks.back().inputBus == "Monitor");
    assert(aafImport.project.tracks.back().outputBus == "Main 1-2");
    assert(aafImport.project.clips.front().regionName == "Verse Guitar");
    assert(aafImport.project.clips.front().sourcePath == "/tmp/Session Take 01.wav");
    assert(aafImport.project.clips.front().sourceFileUid.find("src-") == 0);
    assert(aafImport.project.clips.front().sourceHasBroadcastTimeReference);
    assert(aafImport.project.clips.front().sourceTimeReferenceSamples == 96000);
    assert(std::abs(aafImport.project.clips.front().sourceTempoBpm - 117.6) < 0.001);
    assert(aafImport.project.clips.front().sourceTimeSignatureNumerator == 6);
    assert(aafImport.project.clips.front().sourceTimeSignatureDenominator == 8);
    assert(aafImport.project.clips.front().sourceGrooveFeel == "shuffle");
    assert(std::abs(aafImport.project.clips.front().sourceGrooveSwingAmount - 0.58) < 0.001);
    assert(std::abs(aafImport.project.clips.front().startSeconds - 2.0) < 0.0001);
    assert(std::abs(aafImport.project.clips.front().fadeOutSeconds - 0.2) < 0.0001);
    assert(aafImport.project.chordEvents.front().id == "chord-export-a");
    assert(aafImport.project.chordEvents.front().name == "Verse / Cmaj7");
    assert(aafImport.project.lyricEvents.front().id == "lyric-export-a");
    assert(aafImport.project.lyricEvents.front().text == "Hello Neuracoust");
    assert(!neuracoust::daw::importProjectFromAafReferenceText("bad\n").ok);
    auto omfReference = neuracoust::daw::exportProjectToOmfReference(edlProject);
    assert(omfReference.ok);
    assert(omfReference.clipCount == 1);
    assert(omfReference.trackCount == 1);
    assert(omfReference.chordEventCount == 1);
    assert(omfReference.lyricEventCount == 1);
    assert(omfReference.text.find("NEURACOUST_DAW_OMF_REFERENCE_EXPORT 1") != std::string::npos);
    assert(omfReference.text.find("not a native binary OMF container") != std::string::npos);
    assert(omfReference.text.find("sourceUid=\"src-") != std::string::npos);
    const auto omfImport = neuracoust::daw::importProjectFromOmfReferenceText(omfReference.text);
    assert(omfImport.ok);
    assert(omfImport.chordEventCount == 1);
    assert(omfImport.lyricEventCount == 1);
    assert(omfImport.clipCount == 1);
    assert(omfImport.project.clips.front().trackName == "Audio 1");
    assert(omfImport.project.tracks.size() == 3);
    assert(omfImport.project.tracks.front().outputBus == "Master");
    assert(omfImport.project.tracks[omfImport.project.tracks.size() - 2].outputBus == "Monitor");
    assert(omfImport.project.tracks.back().trackType == "monitor");
    assert(omfImport.project.clips.front().sourceHasBroadcastTimeReference);
    assert(omfImport.project.clips.front().sourceTimeReferenceSamples == 96000);
    assert(omfImport.project.timeSignatureMap.size() == 2);
    assert(omfImport.project.timeSignatureMap.back().numerator == 6);
    assert(std::abs(omfImport.project.clips.front().sourceTempoBpm - 117.6) < 0.001);
    assert(omfImport.project.clips.front().sourceTimeSignatureNumerator == 6);
    assert(omfImport.project.clips.front().sourceTimeSignatureDenominator == 8);
    assert(omfImport.project.clips.front().sourceGrooveFeel == "shuffle");
    auto escapedFcpxmlProject = neuracoust::daw::defaultProject();
    escapedFcpxmlProject.name = "A&B <Mix>";
    const auto escapedFcpxmlClip = neuracoust::daw::appendAudioClipAt(escapedFcpxmlProject,
                                                                      "Audio 1",
                                                                      "/tmp/A&B <Take>.wav",
                                                                      0.0,
                                                                      1.0);
    assert(neuracoust::daw::setClipRegionName(escapedFcpxmlProject, escapedFcpxmlClip, "Gtr \"L\" & R"));
    const auto escapedFcpxmlResult = neuracoust::daw::exportProjectToFcpxml(escapedFcpxmlProject);
    assert(escapedFcpxmlResult.ok);
    assert(escapedFcpxmlResult.text.find("A&amp;B &lt;Mix&gt;") != std::string::npos);
    assert(escapedFcpxmlResult.text.find("name=\"Gtr &quot;L&quot; &amp; R\"") != std::string::npos);
    assert(escapedFcpxmlResult.text.find("A&amp;B &lt;Take&gt;.wav") != std::string::npos);

    auto lockedClipProject = neuracoust::daw::defaultProject();
    const auto lockedClipId = neuracoust::daw::appendAudioClipAt(lockedClipProject, "Audio 1", "/tmp/locked.wav", 0.5, 2.0);
    assert(neuracoust::daw::setClipLocked(lockedClipProject, lockedClipId, true));
    assert(lockedClipProject.clips.back().locked);
    assert(!neuracoust::daw::moveClip(lockedClipProject, lockedClipId, 1.0));
    assert(!neuracoust::daw::shuffleMoveClip(lockedClipProject, lockedClipId, 1.0));
    assert(!neuracoust::daw::nudgeClip(lockedClipProject, lockedClipId, 0.1));
    assert(!neuracoust::daw::trimClipStart(lockedClipProject, lockedClipId, 0.75));
    assert(!neuracoust::daw::trimClipEnd(lockedClipProject, lockedClipId, 1.25));
    std::string lockedSplitId;
    assert(!neuracoust::daw::splitClip(lockedClipProject, lockedClipId, 1.0, lockedSplitId));
    assert(!neuracoust::daw::setClipGainDb(lockedClipProject, lockedClipId, 3.0f));
    assert(!neuracoust::daw::setClipFades(lockedClipProject, lockedClipId, 0.1, 0.1));
    assert(!neuracoust::daw::setClipMuted(lockedClipProject, lockedClipId, true));
    assert(!neuracoust::daw::setClipPolarityInverted(lockedClipProject, lockedClipId, true));
    assert(!neuracoust::daw::setClipTrack(lockedClipProject, lockedClipId, "Audio 2"));
    assert(!neuracoust::daw::deleteClip(lockedClipProject, lockedClipId));
    assert(!neuracoust::daw::clearClipRange(lockedClipProject, 0.0, 4.0));
    std::vector<std::string> lockedChangedIds;
    assert(!neuracoust::daw::quantizeClipStartsInRange(lockedClipProject, 0.0, 4.0, 1.0, lockedChangedIds));
    assert(lockedClipProject.clips.size() == 1);
    assert(lockedClipProject.clips.front().startSeconds == 0.5);
    assert(lockedClipProject.clips.front().durationSeconds == 2.0);
    assert(neuracoust::daw::setClipRegionName(lockedClipProject, lockedClipId, "Locked Region"));
    assert(neuracoust::daw::setClipColor(lockedClipProject, lockedClipId, "#F0B84D"));

    auto shuffleMoveProject = neuracoust::daw::defaultProject();
    const auto shuffleA = neuracoust::daw::appendAudioClipAt(shuffleMoveProject, "Audio 1", "/tmp/shuffle-a.wav", 0.0, 1.0);
    const auto shuffleB = neuracoust::daw::appendAudioClipAt(shuffleMoveProject, "Audio 1", "/tmp/shuffle-b.wav", 1.0, 1.0);
    const auto shuffleC = neuracoust::daw::appendAudioClipAt(shuffleMoveProject, "Audio 1", "/tmp/shuffle-c.wav", 2.0, 1.0);
    assert(neuracoust::daw::shuffleMoveClip(shuffleMoveProject, shuffleA, 3.0));
    auto findShuffleClip = [&](const std::string& id) -> const neuracoust::daw::ClipState& {
        auto it = std::find_if(shuffleMoveProject.clips.begin(), shuffleMoveProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
            return clip.id == id;
        });
        assert(it != shuffleMoveProject.clips.end());
        return *it;
    };
    assert(findShuffleClip(shuffleB).startSeconds == 0.0);
    assert(findShuffleClip(shuffleC).startSeconds == 1.0);
    assert(findShuffleClip(shuffleA).startSeconds == 2.0);
    assert(neuracoust::daw::shuffleMoveClip(shuffleMoveProject, shuffleA, 0.0));
    assert(findShuffleClip(shuffleA).startSeconds == 0.0);
    assert(findShuffleClip(shuffleB).startSeconds == 1.0);
    assert(findShuffleClip(shuffleC).startSeconds == 2.0);
    std::string shuffleCopyId;
    assert(neuracoust::daw::shuffleDuplicateClip(shuffleMoveProject, shuffleA, 1.0, "Audio 1", shuffleCopyId));
    assert(!shuffleCopyId.empty());
    assert(findShuffleClip(shuffleA).startSeconds == 0.0);
    assert(findShuffleClip(shuffleCopyId).startSeconds == 1.0);
    assert(findShuffleClip(shuffleB).startSeconds == 2.0);
    assert(findShuffleClip(shuffleC).startSeconds == 3.0);
    const auto lockedJson = neuracoust::daw::serializeProject(lockedClipProject);
    assert(lockedJson.find("\"locked\":true") != std::string::npos);
    neuracoust::daw::ProjectDocument lockedRoundTrip;
    assert(neuracoust::daw::deserializeProject(lockedJson, lockedRoundTrip, projectParseError));
    assert(lockedRoundTrip.clips.front().locked);
    assert(neuracoust::daw::setClipLocked(lockedRoundTrip, lockedClipId, false));
    assert(neuracoust::daw::moveClip(lockedRoundTrip, lockedClipId, 1.0));
    assert(neuracoust::daw::deleteClip(lockedRoundTrip, lockedClipId));
    const std::string repairedMasterInsertJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Missing Monitor DSP",
  "tracks": [
    {"name":"Audio 1","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":false},
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":false}
  ],
  "clips": [],
  "markers": [],
  "masterInserts": [
    {"pluginName":"External Only","pluginAppId":"external-vst3","pluginFormat":"VST3","pluginPath":"/tmp/External.vst3","bypassed":false,"available":false},
    {"pluginName":"Broken Monitor","pluginAppId":"neuracoust-monitor-dsp","pluginFormat":"VST3","pluginPath":"/tmp/wrong.vst3","bypassed":true,"available":false},
    {"pluginName":"Duplicate Monitor","pluginAppId":"neuracoust-monitor-dsp","pluginFormat":"Internal","pluginPath":"","bypassed":false,"available":true}
  ],
  "monitorModules": []
})json";
    neuracoust::daw::ProjectDocument repairedMasterInsertProject;
    assert(neuracoust::daw::deserializeProject(repairedMasterInsertJson, repairedMasterInsertProject, projectParseError));
    assert(repairedMasterInsertProject.masterInserts.size() == 1);
    assert(repairedMasterInsertProject.masterInserts.front().pluginName == "External Only");
    auto missingMonitorProject = neuracoust::daw::defaultProject();
    missingMonitorProject.masterInserts = {
        makeMasterInsert("External Only", "external-vst3", "VST3", "/tmp/External.vst3", false, false)
    };
    missingMonitorProject.masterInserts.front().dspExecutionMode = "external";
    missingMonitorProject.masterInserts.front().assignedDspServerId = "Neuracoust DSP Server";
    missingMonitorProject.masterInserts.front().serverModuleId = "na.neuracoust.external-only";
    missingMonitorProject.masterInserts.front().reportedLatencySamples = 512;
    missingMonitorProject.masterInserts.front().dspAvailable = false;
    missingMonitorProject.masterInserts.front().dspLastError = "external master insert waiting for server";
    const auto missingMonitorJson = neuracoust::daw::serializeProject(missingMonitorProject);
    neuracoust::daw::ProjectDocument missingMonitorRoundTrip;
    assert(neuracoust::daw::deserializeProject(missingMonitorJson, missingMonitorRoundTrip, projectParseError));
    assert(missingMonitorRoundTrip.masterInserts.size() == 1);
    assert(missingMonitorRoundTrip.masterInserts.front().pluginName == "External Only");
    assert(missingMonitorRoundTrip.masterInserts.front().dspExecutionMode == "external");
    assert(missingMonitorRoundTrip.masterInserts.front().assignedDspServerId == "Neuracoust DSP Server");
    assert(missingMonitorRoundTrip.masterInserts.front().serverModuleId == "na.neuracoust.external-only");
    assert(missingMonitorRoundTrip.masterInserts.front().reportedLatencySamples == 512);
    assert(!missingMonitorRoundTrip.masterInserts.front().dspAvailable);
    assert(missingMonitorRoundTrip.masterInserts.front().dspLastError == "external master insert waiting for server");

    auto escapedProject = neuracoust::daw::defaultProject();
    escapedProject.name = "Mix \"A\" \\ Draft";
    escapedProject.tracks[0].name = "Audio \"Lead\"";
    escapedProject.clips.push_back({
        "clip\"1",
        "Audio \"Lead\"",
        "/tmp/Neuracoust \"Clip\" \\ One.wav",
        0.0,
        1.0,
        0.0,
        0.0f
    });
    escapedProject.markers.push_back({"marker\\1", "Verse\nOne", 0.5});
    escapedProject.masterInserts.push_back(makeMasterInsert(
        "Quoted \"VST3\"",
        "external-vst3",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Quoted \\ Path.vst3"));
    const auto escapedJson = neuracoust::daw::serializeProject(escapedProject);
    assert(escapedJson.find("Mix \\\"A\\\" \\\\ Draft") != std::string::npos);
    assert(escapedJson.find("Verse\\nOne") != std::string::npos);
    neuracoust::daw::ProjectDocument escapedRoundTrip;
    assert(neuracoust::daw::deserializeProject(escapedJson, escapedRoundTrip, projectParseError));
    assert(escapedRoundTrip.name == escapedProject.name);
    assert(escapedRoundTrip.tracks[0].name == escapedProject.tracks[0].name);
    assert(escapedRoundTrip.clips.back().id == escapedProject.clips.back().id);
    assert(escapedRoundTrip.clips.back().sourcePath == escapedProject.clips.back().sourcePath);
    assert(escapedRoundTrip.markers.back().name == escapedProject.markers.back().name);
    assert(escapedRoundTrip.masterInserts.back().pluginName == escapedProject.masterInserts.back().pluginName);
    assert(escapedRoundTrip.masterInserts.back().pluginPath == escapedProject.masterInserts.back().pluginPath);

    auto portableProject = neuracoust::daw::defaultProject();
    const auto portableRoot = testTempRoot() / "neuracoust-daw-portable-project";
    const auto portableProjectPath = portableRoot / "Session.ndaw";
    const auto portableAudioPath = portableRoot / "Audio Files" / "take.wav";
    const auto portableVideoPath = portableRoot / "Video Files" / "picture-lock.mov";
    const auto portableVst3Path = portableRoot / "Plug-Ins" / "Local.vst3";
    const auto externalAudioPath = testTempRoot() / "neuracoust-daw-external.wav";
    portableProject.clips.push_back({"portable-audio", "Audio 1", portableAudioPath.string(), 0.0, 1.0, 0.0, 0.0f});
    portableProject.clips.push_back({"external-audio", "Audio 1", externalAudioPath.string(), 1.0, 1.0, 0.0, 0.0f});
    portableProject.videoSources.push_back({"portable-video", portableVideoPath.string(), "Picture Lock", 24.0, 12.0, 1280, 720, false});
    portableProject.videoClips.push_back({"portable-video-clip", "portable-video", "Picture Lock", 0.0, 12.0, 0.0, 3600.0, false, false});
    portableProject.masterInserts.push_back(makeMasterInsert(
        "Portable VST3",
        "external-vst3",
        "VST3",
        portableVst3Path.string()));
    const auto portableJson = neuracoust::daw::serializeProjectForPath(portableProject, portableProjectPath);
    assert(portableJson.find("Audio Files/take.wav") != std::string::npos);
    assert(portableJson.find("Video Files/picture-lock.mov") != std::string::npos);
    assert(portableJson.find("Plug-Ins/Local.vst3") != std::string::npos);
    assert(portableJson.find("neuracoust-daw-external.wav") != std::string::npos);
    neuracoust::daw::ProjectDocument portableRoundTrip;
    assert(neuracoust::daw::deserializeProjectForPath(portableJson, portableProjectPath, portableRoundTrip, projectParseError));
    assert(portableRoundTrip.clips[0].sourcePath == portableAudioPath.lexically_normal().generic_string());
    assert(portableRoundTrip.clips[1].sourcePath == externalAudioPath.string());
    assert(!portableRoundTrip.mediaSources.empty());
    assert(portableRoundTrip.mediaSources.front().path == portableAudioPath.lexically_normal().generic_string());
    assert(portableRoundTrip.videoSources.front().path == portableVideoPath.lexically_normal().generic_string());
    assert(portableRoundTrip.masterInserts.back().pluginPath == portableVst3Path.lexically_normal().generic_string());

    const auto collectRoot = testTempRoot() / "neuracoust-daw-media-collect";
    std::filesystem::remove_all(collectRoot);
    std::filesystem::create_directories(collectRoot / "External");
    const auto collectProjectPath = collectRoot / "Session.ndaw";
    const auto collectSourcePath = collectRoot / "External" / "take.wav";
    {
        std::ofstream audio(collectSourcePath, std::ios::binary);
        audio << "fake-wav";
    }
    std::string mediaCopyError;
    const auto collectedPath = neuracoust::daw::copyAudioFileToProjectMedia(collectSourcePath, collectProjectPath, mediaCopyError);
    assert(mediaCopyError.empty());
    assert(!collectedPath.empty());
    assert(std::filesystem::exists(std::filesystem::path(collectedPath)));
    assert(std::filesystem::path(collectedPath).filename() == "take.wav");
    const auto collectedDuplicatePath = neuracoust::daw::copyAudioFileToProjectMedia(collectSourcePath, collectProjectPath, mediaCopyError);
    assert(mediaCopyError.empty());
    assert(std::filesystem::path(collectedDuplicatePath).filename() == "take 2.wav");
    const auto collectVideoPath = collectRoot / "External" / "picture-lock.mov";
    {
        std::ofstream video(collectVideoPath, std::ios::binary);
        video << "fake-mov";
    }
    const auto collectedVideoPath = neuracoust::daw::copyVideoFileToProjectMedia(collectVideoPath, collectProjectPath, mediaCopyError);
    assert(mediaCopyError.empty());
    assert(std::filesystem::path(collectedVideoPath).parent_path().filename() == "Video Files");
    auto videoImportProject = neuracoust::daw::defaultProject();
    const auto videoClipId = neuracoust::daw::appendVideoReferenceClip(videoImportProject, collectedVideoPath, 1.0, 12.0, 23.976, 1920, 1080, true);
    assert(videoClipId == "video-clip-1");
    assert(videoImportProject.videoFrameRate > 23.97 && videoImportProject.videoFrameRate < 23.98);
    assert(videoImportProject.videoSources.size() == 1);
    assert(videoImportProject.videoClips.size() == 1);
    assert(videoImportProject.videoSources.front().path == collectedVideoPath);
    assert(videoImportProject.videoClips.front().sourceId == videoImportProject.videoSources.front().id);
    assert(neuracoust::daw::moveVideoClip(videoImportProject, videoClipId, 2.0));
    assert(videoImportProject.videoClips.front().startSeconds == 2.0);
    assert(neuracoust::daw::trimVideoClipStart(videoImportProject, videoClipId, 3.0));
    assert(videoImportProject.videoClips.front().sourceOffsetSeconds == 1.0);
    assert(videoImportProject.videoClips.front().durationSeconds == 11.0);
    assert(neuracoust::daw::trimVideoClipEnd(videoImportProject, videoClipId, 8.0));
    assert(videoImportProject.videoClips.front().durationSeconds == 5.0);
    std::string splitVideoClipId;
    assert(neuracoust::daw::splitVideoClip(videoImportProject, videoClipId, 5.0, splitVideoClipId));
    assert(!splitVideoClipId.empty());
    assert(videoImportProject.videoClips.size() == 2);
    const auto splitVideoRight = std::find_if(videoImportProject.videoClips.begin(), videoImportProject.videoClips.end(), [&](const neuracoust::daw::VideoClipState& clip) {
        return clip.id == splitVideoClipId;
    });
    assert(splitVideoRight != videoImportProject.videoClips.end());
    assert(std::abs(videoImportProject.videoClips.front().durationSeconds - 2.0) < 0.0001);
    assert(std::abs(splitVideoRight->startSeconds - 5.0) < 0.0001);
    assert(std::abs(splitVideoRight->sourceOffsetSeconds - 3.0) < 0.0001);
    assert(std::abs(splitVideoRight->durationSeconds - 3.0) < 0.0001);
    assert(splitVideoRight->sourceId == videoImportProject.videoClips.front().sourceId);
    const auto videoClipBeforeInvalidTrim = videoImportProject.videoClips.front();
    assert(!neuracoust::daw::trimVideoClipStart(videoImportProject, videoClipId, 1.0));
    assert(videoImportProject.videoClips.front().startSeconds == videoClipBeforeInvalidTrim.startSeconds);
    assert(videoImportProject.videoClips.front().sourceOffsetSeconds == videoClipBeforeInvalidTrim.sourceOffsetSeconds);
    videoImportProject.videoClips.front().locked = true;
    assert(!neuracoust::daw::moveVideoClip(videoImportProject, videoClipId, 4.0));
    assert(!neuracoust::daw::trimVideoClipEnd(videoImportProject, videoClipId, 9.0));
    assert(neuracoust::daw::setVideoClipLocked(videoImportProject, videoClipId, false));
    assert(neuracoust::daw::setVideoClipMuted(videoImportProject, videoClipId, true));
    assert(videoImportProject.videoClips.front().muted);
    assert(neuracoust::daw::setVideoClipName(videoImportProject, videoClipId, "Picture Edit"));
    assert(videoImportProject.videoClips.front().name == "Picture Edit");
    assert(neuracoust::daw::relinkVideoSource(videoImportProject,
                                             videoImportProject.videoSources.front().id,
                                             (collectRoot / "External" / "picture-v2.mov").string(),
                                             "Picture V2"));
    assert(videoImportProject.videoSources.front().displayName == "Picture V2");
    auto videoDeleteProject = videoImportProject;
    videoDeleteProject.videoClips.erase(std::remove_if(videoDeleteProject.videoClips.begin(),
                                                       videoDeleteProject.videoClips.end(),
                                                       [&](const neuracoust::daw::VideoClipState& clip) {
                                                           return clip.id != videoClipId;
                                                       }),
                                        videoDeleteProject.videoClips.end());
    assert(neuracoust::daw::deleteVideoClip(videoDeleteProject, videoClipId));
    assert(videoDeleteProject.videoClips.empty());
    assert(videoDeleteProject.videoSources.empty());
    neuracoust::daw::ProjectDocument videoImportRoundTrip;
    assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(videoImportProject), videoImportRoundTrip, projectParseError));
    assert(videoImportRoundTrip.videoSources.size() == 1);
    assert(videoImportRoundTrip.videoClips.size() == 2);
    assert(std::any_of(videoImportRoundTrip.videoClips.begin(), videoImportRoundTrip.videoClips.end(), [&](const neuracoust::daw::VideoClipState& clip) {
        return clip.id == splitVideoClipId &&
            std::abs(clip.startSeconds - 5.0) < 0.0001 &&
            std::abs(clip.sourceOffsetSeconds - 3.0) < 0.0001;
    }));
    auto collectedProject = neuracoust::daw::defaultProject();
    collectedProject.clips.push_back({"collected-audio", "Audio 1", collectedPath, 0.0, 1.0, 0.0, 0.0f});
    collectedProject.clips.push_back({"external-audio", "Audio 1", collectSourcePath.string(), 1.0, 1.0, 0.0, 0.0f});
    collectedProject.clips.push_back({"external-audio-copy", "Audio 1", collectSourcePath.string(), 2.0, 1.0, 0.0, 0.0f});
    collectedProject.clips.push_back({"missing-audio", "Audio 1", (collectRoot / "External" / "missing.wav").string(), 3.0, 1.0, 0.0, 0.0f});
    const auto collectReport = neuracoust::daw::collectProjectMedia(collectedProject, collectProjectPath);
    assert(collectReport.copiedClips == 2);
    assert(collectReport.alreadyInProjectClips == 1);
    assert(collectReport.missingClips == 1);
    assert(collectReport.failedClips == 0);
    assert(collectedProject.clips[1].sourcePath == collectedProject.clips[2].sourcePath);
    assert(std::filesystem::exists(std::filesystem::path(collectedProject.clips[1].sourcePath)));
    const auto collectedHealth = neuracoust::daw::analyzeProjectHealth(collectedProject);
    assert(collectedHealth.clips == 4);
    assert(collectedHealth.missingMediaClips == 1);
    assert(collectedHealth.missingVst3Inserts == 0);
    assert(!collectedHealth.messages.empty());
    assert(neuracoust::daw::summarizeProjectHealth(collectedHealth).find("missing media") != std::string::npos);
    auto overlapHealthProject = neuracoust::daw::defaultProject();
    overlapHealthProject.clips.push_back({"overlap-a", "Audio 1", collectSourcePath.string(), 0.0, 1.0, 0.0, 0.0f});
    overlapHealthProject.clips.push_back({"overlap-b", "Audio 1", collectSourcePath.string(), 1.0, 0.5, 0.0, 0.0f});
    overlapHealthProject.clips.push_back({"overlap-c", "Audio 1", collectSourcePath.string(), 1.25, 0.5, 0.0, 0.0f});
    overlapHealthProject.clips.push_back({"overlap-other-track", "Audio 2", collectSourcePath.string(), 1.25, 0.5, 0.0, 0.0f});
    const auto overlapHealth = neuracoust::daw::analyzeProjectHealth(overlapHealthProject);
    assert(overlapHealth.overlappingClipPairs == 1);
    assert(neuracoust::daw::summarizeProjectHealth(overlapHealth).find("overlapping clip") != std::string::npos);
    auto muteSoloHealthProject = neuracoust::daw::defaultProject();
    muteSoloHealthProject.tracks[0].muted = true;
    muteSoloHealthProject.tracks[1].solo = true;
    muteSoloHealthProject.clips.push_back({"muted-health-clip", "Audio 1", collectSourcePath.string(), 0.0, 1.0, 0.0, 0.0f});
    muteSoloHealthProject.clips.front().muted = true;
    const auto muteSoloHealth = neuracoust::daw::analyzeProjectHealth(muteSoloHealthProject);
    assert(muteSoloHealth.mutedAudioTracks == 1);
    assert(muteSoloHealth.soloedAudioTracks == 1);
    assert(muteSoloHealth.mutedClips == 1);
    assert(neuracoust::daw::summarizeProjectHealth(muteSoloHealth).find("muted track") != std::string::npos);
    assert(neuracoust::daw::summarizeProjectHealth(muteSoloHealth).find("soloed track") != std::string::npos);
    assert(neuracoust::daw::summarizeProjectHealth(muteSoloHealth).find("muted clip") != std::string::npos);
    const auto existingTrackVst3Path = collectRoot / "Example Track Insert.vst3";
    std::filesystem::create_directories(existingTrackVst3Path);
    auto trackInsertHealthProject = neuracoust::daw::defaultProject();
    trackInsertHealthProject.tracks[0].inserts.push_back(makeTrackInsert("Track EQ", "VST3", existingTrackVst3Path.string()));
    trackInsertHealthProject.tracks[0].inserts.push_back(makeTrackInsert("Track Comp", "VST3", (collectRoot / "Missing Track Comp.vst3").string()));
    trackInsertHealthProject.tracks[0].inserts.push_back(makeTrackInsert("Bypassed Track", "VST3", (collectRoot / "Missing Bypassed Track.vst3").string(), true));
    const auto trackInsertHealth = neuracoust::daw::analyzeProjectHealth(trackInsertHealthProject);
    assert(trackInsertHealth.trackInserts == 3);
    assert(trackInsertHealth.vst3TrackInserts == 3);
    assert(trackInsertHealth.activeVst3TrackInserts == 2);
    assert(trackInsertHealth.activeVst3TrackInsertLabels.size() == 2);
    assert(trackInsertHealth.activeVst3TrackInsertLabels[0] == "Audio 1: Track EQ");
    assert(trackInsertHealth.activeVst3TrackInsertLabels[1] == "Audio 1: Track Comp");
    assert(trackInsertHealth.missingVst3Inserts == 2);
    assert(neuracoust::daw::activeVst3TrackInsertCount(trackInsertHealthProject) == 2);
    assert(neuracoust::daw::hasActiveVst3TrackInserts(trackInsertHealthProject));
    trackInsertHealthProject.tracks[0].inserts[0].bypassed = true;
    trackInsertHealthProject.tracks[0].inserts[1].bypassed = true;
    assert(neuracoust::daw::activeVst3TrackInsertCount(trackInsertHealthProject) == 0);
    assert(!neuracoust::daw::hasActiveVst3TrackInserts(trackInsertHealthProject));
    assert(neuracoust::daw::summarizeProjectHealth(trackInsertHealth).find("track VST3") != std::string::npos);
    assert(neuracoust::daw::summarizeProjectHealth(trackInsertHealth).find("direct-output render") != std::string::npos);
    const auto collectedJson = neuracoust::daw::serializeProjectForPath(collectedProject, collectProjectPath);
    assert(collectedJson.find("Audio Files/take.wav") != std::string::npos);
    neuracoust::daw::ProjectDocument portableHealthProject;
    assert(neuracoust::daw::deserializeProject(collectedJson, portableHealthProject, projectParseError));
    const auto portablePathHealth = neuracoust::daw::analyzeProjectHealth(portableHealthProject, collectProjectPath);
    assert(portablePathHealth.clips == 4);
    assert(portablePathHealth.missingMediaClips == 1);
    assert(neuracoust::daw::summarizeProjectHealth(portableHealthProject, collectProjectPath).find("missing media") != std::string::npos);
    const auto collectedRecordingPath = neuracoust::daw::nextProjectRecordingPath(collectProjectPath, mediaCopyError);
    assert(mediaCopyError.empty());
    assert(std::filesystem::path(collectedRecordingPath).filename() == "Neuracoust DAW Recording.wav");
    assert(neuracoust::daw::recordedTakeManifestPath(collectedRecordingPath).filename() == "Neuracoust DAW Recording.wav.recording.json");
    const auto backupPath = neuracoust::daw::projectBackupPath(collectProjectPath);
    assert(backupPath.filename() == "Session.ndaw.bak");
    const auto normalizedFlatSavePath = neuracoust::daw::normalizedProjectSavePath(testTempRoot() / "NeuracoustDAWCoreSmoke" / "Test Session.ndaw");
    assert(normalizedFlatSavePath.filename() == "Test Session.ndaw");
    assert(normalizedFlatSavePath.parent_path().filename() == "Test Session");
    const auto normalizedNestedSavePath = neuracoust::daw::normalizedProjectSavePath(normalizedFlatSavePath);
    assert(normalizedNestedSavePath == normalizedFlatSavePath);
    const auto normalizedNoExtensionPath = neuracoust::daw::normalizedProjectSavePath(testTempRoot() / "NeuracoustDAWCoreSmoke" / "No Extension");
    assert(normalizedNoExtensionPath.filename() == "No Extension.ndaw");
    assert(normalizedNoExtensionPath.parent_path().filename() == "No Extension");
    std::string backupError;
    assert(neuracoust::daw::backupExistingProjectFile(collectProjectPath, backupError));
    assert(backupError.empty());
    assert(!std::filesystem::exists(backupPath));
    {
        std::ofstream existingProject(collectProjectPath, std::ios::binary);
        existingProject << "previous project";
    }
    assert(neuracoust::daw::backupExistingProjectFile(collectProjectPath, backupError));
    assert(backupError.empty());
    assert(std::filesystem::exists(backupPath));
    {
        std::ifstream backupIn(backupPath, std::ios::binary);
        std::string backupText((std::istreambuf_iterator<char>(backupIn)), std::istreambuf_iterator<char>());
        assert(backupText == "previous project");
    }
    std::filesystem::remove(collectProjectPath);
    std::filesystem::remove(backupPath);
    bool projectSaved = neuracoust::daw::saveProjectFileWithBackup(collectedProject, collectProjectPath, backupError);
    assert(projectSaved);
    assert(backupError.empty());
    assert(std::filesystem::exists(collectProjectPath));
    assert(!std::filesystem::exists(std::filesystem::path(collectProjectPath.string() + ".saving")));
    assert(!std::filesystem::exists(backupPath));
    {
        std::ifstream projectIn(collectProjectPath, std::ios::binary);
        std::string projectText((std::istreambuf_iterator<char>(projectIn)), std::istreambuf_iterator<char>());
        assert(projectText.find("neuracoust-daw-project-v1") != std::string::npos);
        assert(projectText.find("Audio Files/take.wav") != std::string::npos);
    }
    {
        std::ofstream existingProject(collectProjectPath, std::ios::binary);
        existingProject << "previous project";
    }
    projectSaved = neuracoust::daw::saveProjectFileWithBackup(collectedProject, collectProjectPath, backupError);
    assert(projectSaved);
    assert(backupError.empty());
    assert(std::filesystem::exists(backupPath));
    {
        std::ifstream backupIn(backupPath, std::ios::binary);
        std::string backupText((std::istreambuf_iterator<char>(backupIn)), std::istreambuf_iterator<char>());
        assert(backupText == "previous project");
    }
    {
        std::ifstream projectIn(collectProjectPath, std::ios::binary);
        std::string projectText((std::istreambuf_iterator<char>(projectIn)), std::istreambuf_iterator<char>());
        assert(projectText.find("neuracoust-daw-project-v1") != std::string::npos);
        assert(projectText.find("previous project") == std::string::npos);
    }
    const auto autosavePath = neuracoust::daw::projectAutosavePath(collectProjectPath);
    assert(autosavePath.filename() == "Session.ndaw.autosave");
    const bool autosaveWritten = neuracoust::daw::writeProjectAutosaveFile(collectedProject, collectProjectPath, backupError);
    assert(autosaveWritten);
    assert(backupError.empty());
    assert(std::filesystem::exists(autosavePath));
    assert(!std::filesystem::exists(std::filesystem::path(autosavePath.string() + ".saving")));
    if (!autosaveWritten || !std::filesystem::exists(autosavePath) || !std::filesystem::exists(collectProjectPath)) {
        std::cerr << "Could not prepare autosave freshness smoke fixture\n";
        return 1;
    }
    const auto savedProjectTime = std::filesystem::last_write_time(collectProjectPath);
    std::filesystem::last_write_time(autosavePath, savedProjectTime + std::chrono::seconds(5));
    assert(neuracoust::daw::projectAutosaveIsNewerThanProject(collectProjectPath));
    std::filesystem::last_write_time(autosavePath, savedProjectTime - std::chrono::seconds(5));
    assert(!neuracoust::daw::projectAutosaveIsNewerThanProject(collectProjectPath));
    std::filesystem::last_write_time(autosavePath, savedProjectTime + std::chrono::seconds(5));
    {
        std::ifstream autosaveIn(autosavePath, std::ios::binary);
        std::string autosaveText((std::istreambuf_iterator<char>(autosaveIn)), std::istreambuf_iterator<char>());
        assert(autosaveText.find("neuracoust-daw-project-v1") != std::string::npos);
        assert(autosaveText.find("Audio Files/take.wav") != std::string::npos);
    }
    neuracoust::daw::ProjectDocument autosaveRoundTrip;
    assert(neuracoust::daw::loadProjectAutosaveFile(collectProjectPath, autosaveRoundTrip, backupError));
    assert(backupError.empty());
    assert(autosaveRoundTrip.clips.size() == collectedProject.clips.size());
    assert(autosaveRoundTrip.clips[1].sourcePath.find("Audio Files") != std::string::npos);
    assert(neuracoust::daw::removeProjectAutosaveFile(collectProjectPath, backupError));
    assert(backupError.empty());
    assert(!std::filesystem::exists(autosavePath));
    assert(neuracoust::daw::applyDefaultProjectNameFromPath(collectedProject, collectProjectPath));
    assert(collectedProject.name == "Session");
    collectedProject.name = "Custom Mix Name";
    assert(!neuracoust::daw::applyDefaultProjectNameFromPath(collectedProject, collectProjectPath));
    assert(collectedProject.name == "Custom Mix Name");
    {
        std::ofstream recorded(collectedRecordingPath, std::ios::binary);
        recorded << "fake-recording";
    }
    const auto nextRecordingPath = neuracoust::daw::nextProjectRecordingPath(collectProjectPath, mediaCopyError);
    assert(mediaCopyError.empty());
    assert(std::filesystem::path(nextRecordingPath).filename() == "Neuracoust DAW Recording 2.wav");
    std::filesystem::remove_all(collectRoot);

    assert(neuracoust::daw::addMasterVst3Insert(project, makeMasterInsert(
        "Second VST3",
        "external-vst3",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Second.vst3")));
    assert(!neuracoust::daw::addMasterVst3Insert(project, makeMasterInsert(
        "Second VST3 Duplicate",
        "external-vst3",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Second.vst3")));
    assert(neuracoust::daw::moveMasterInsert(project, 1, -1) == 0);
    assert(project.masterInserts[0].pluginName == "Second VST3");
    assert(project.masterInserts[1].pluginName == "Example VST3");
    assert(neuracoust::daw::toggleMasterVst3InsertBypass(project, 0));
    assert(project.masterInserts[0].bypassed);
    assert(neuracoust::daw::toggleMasterVst3InsertBypass(project, 0));
    assert(!project.masterInserts[0].bypassed);
    assert(neuracoust::daw::moveMasterInsert(project, 0, -1) == -1);
    assert(neuracoust::daw::moveMasterInsert(project, 0, 1) == 1);
    assert(project.masterInserts[0].pluginName == "Example VST3");
    assert(project.masterInserts[1].pluginName == "Second VST3");
    project.masterInserts.insert(project.masterInserts.begin(), makeMasterInsert(
        "Neuracoust Monitor DSP",
        "neuracoust-monitor-dsp",
        "Internal",
        ""));
    assert(!neuracoust::daw::toggleMasterVst3InsertBypass(project, 0));
    assert(!neuracoust::daw::removeMasterVst3Insert(project, 0));
    assert(neuracoust::daw::removeMasterVst3Insert(project, 2));
    assert(project.masterInserts.size() == 2);
    assert(neuracoust::daw::clearMasterVst3Inserts(project) == 1);
    assert(project.masterInserts.size() == 1);
    assert(project.masterInserts.front().pluginAppId == "neuracoust-monitor-dsp");
    project.masterInserts.clear();
    const std::string appleAuComponentPath = "/Library/Audio/Plug-Ins/Components/AppleAU.component";
    if (std::filesystem::exists(appleAuComponentPath)) {
        auto matrixReverbInsert = makeMasterInsert(
            "Apple AU Matrix Reverb",
            "external-audio-unit",
            "Audio Unit",
            appleAuComponentPath);
        matrixReverbInsert.pluginClassName = "aumx";
        assert(neuracoust::daw::addMasterVst3Insert(project, matrixReverbInsert));
        auto duplicateMatrixReverbInsert = makeMasterInsert(
            "Apple AU Matrix Reverb",
            "external-audio-unit",
            "Audio Unit",
            appleAuComponentPath);
        duplicateMatrixReverbInsert.pluginClassName = "aumx";
        assert(!neuracoust::daw::addMasterVst3Insert(project, duplicateMatrixReverbInsert));
        auto graphicEqInsert = makeMasterInsert(
            "Apple AU Graphic EQ",
            "external-audio-unit",
            "Audio Unit",
            appleAuComponentPath);
        graphicEqInsert.pluginClassName = "augr";
        assert(neuracoust::daw::addMasterVst3Insert(project, graphicEqInsert));
        assert(project.masterInserts.size() == 2);
        assert(neuracoust::daw::toggleMasterVst3InsertBypass(project, 1));
        assert(project.masterInserts[1].bypassed);
        assert(neuracoust::daw::clearMasterVst3Inserts(project) == 2);
        assert(project.masterInserts.empty());
    }

    auto trackEditProject = neuracoust::daw::defaultProject();
    const auto addedTrack = neuracoust::daw::addAudioTrack(trackEditProject);
    assert(addedTrack == "Audio 3");
    assert(trackEditProject.tracks.size() == 5);
    assert(trackEditProject.tracks[2].name == "Audio 3");
    assert(trackEditProject.tracks[3].name == "Master");
    assert(trackEditProject.tracks.back().name == "Monitor");
    assert(neuracoust::daw::moveTrack(trackEditProject, "Audio 3", -1) == 1);
    assert(trackEditProject.tracks[1].name == "Audio 3");
    assert(trackEditProject.tracks[2].name == "Audio 2");
    assert(neuracoust::daw::moveTrack(trackEditProject, "Audio 3", 1) == 2);
    assert(trackEditProject.tracks[2].name == "Audio 3");
    assert(neuracoust::daw::moveTrack(trackEditProject, "Audio 3", 1) == -1);
    assert(neuracoust::daw::moveTrackNearTrack(trackEditProject, "Audio 3", "Audio 1", false));
    assert(trackEditProject.tracks[0].name == "Audio 3");
    assert(trackEditProject.tracks[1].name == "Audio 1");
    assert(neuracoust::daw::moveTrackNearTrack(trackEditProject, "Audio 3", "Audio 2", true));
    assert(trackEditProject.tracks[1].name == "Audio 2");
    assert(trackEditProject.tracks[2].name == "Audio 3");
    assert(!neuracoust::daw::moveTrackNearTrack(trackEditProject, "Master", "Audio 1", false));
    assert(!neuracoust::daw::moveTrackNearTrack(trackEditProject, "Audio 3", "Monitor", true));
    assert(neuracoust::daw::moveTrack(trackEditProject, "Master", -1) == -1);
    assert(neuracoust::daw::deleteTrackIfEmpty(trackEditProject, "Audio 3"));
    assert(trackEditProject.tracks.size() == 4);
    const auto protectedTrack = neuracoust::daw::addAudioTrack(trackEditProject);
    trackEditProject.clips.push_back({"track-lock", protectedTrack, "/tmp/no-audio.wav", 0.0, 1.0, 0.0, 0.0f});
    assert(!neuracoust::daw::deleteTrackIfEmpty(trackEditProject, protectedTrack));
    assert(neuracoust::daw::setClipTrack(trackEditProject, "track-lock", "Audio 1"));
    assert(trackEditProject.clips.back().trackName == "Audio 1");
    assert(!neuracoust::daw::setClipTrack(trackEditProject, "track-lock", "Master"));
    assert(!neuracoust::daw::setClipTrack(trackEditProject, "track-lock", "No Such Track"));
    assert(!neuracoust::daw::deleteTrackIfEmpty(trackEditProject, "Master"));
    assert(neuracoust::daw::setTrackRecordArmed(trackEditProject, "Audio 1", true));
    assert(trackEditProject.tracks[0].recordArmed);
    assert(!trackEditProject.tracks[1].recordArmed);
    assert(neuracoust::daw::setTrackRecordArmed(trackEditProject, "Audio 2", true));
    assert(trackEditProject.tracks[0].recordArmed);
    assert(trackEditProject.tracks[1].recordArmed);
    assert(neuracoust::daw::setTrackRecordArmed(trackEditProject, "Audio 2", false));
    assert(trackEditProject.tracks[0].recordArmed);
    assert(!trackEditProject.tracks[1].recordArmed);
    assert(!neuracoust::daw::setTrackRecordArmed(trackEditProject, "Master", true));
    assert(neuracoust::daw::setTrackInputMonitoring(trackEditProject, "Audio 1", true));
    assert(trackEditProject.tracks[0].inputMonitoring);
    assert(neuracoust::daw::setTrackInputMonitoring(trackEditProject, "Audio 1", false));
    assert(!trackEditProject.tracks[0].inputMonitoring);
    assert(!neuracoust::daw::setTrackInputMonitoring(trackEditProject, "Master", true));
    assert(neuracoust::daw::recordingTargetTrackName(trackEditProject) == "Audio 1");
    assert(neuracoust::daw::setTrackMuted(trackEditProject, "Audio 1", true));
    assert(trackEditProject.tracks[0].muted);
    assert(neuracoust::daw::setTrackSolo(trackEditProject, "Audio 1", true));
    assert(trackEditProject.tracks[0].solo);
    assert(neuracoust::daw::setTrackVolumeDb(trackEditProject, "Audio 1", 24.0f));
    assert(trackEditProject.tracks[0].volumeDb == 12.0f);
    assert(neuracoust::daw::setTrackVolumeDb(trackEditProject, "Audio 1", -240.0f));
    assert(trackEditProject.tracks[0].volumeDb == -120.0f);
    assert(neuracoust::daw::setTrackVolumeDb(trackEditProject, "Audio 1", 0.0f));
    assert(neuracoust::daw::dbToLinearGain(-120.0f) == 0.0f);
    assert(std::abs(neuracoust::daw::dbToLinearGain(-6.0f) - 0.501187f) < 0.0001f);
    assert(neuracoust::daw::dawFaderPositionForDb(-120.0f) == 0.0);
    assert(std::abs(neuracoust::daw::dawFaderPositionForDb(0.0f) - 0.82) < 0.000001);
    assert(std::abs(neuracoust::daw::dbForDawFaderPosition(0.82) - 0.0f) < 0.000001f);
    assert(neuracoust::daw::dawFaderPositionForDb(-10.0f) < neuracoust::daw::dawFaderPositionForDb(-5.0f));
    assert(neuracoust::daw::dawFaderPositionForDb(-5.0f) < neuracoust::daw::dawFaderPositionForDb(0.0f));
    for (float db : {-60.0f, -30.0f, -20.0f, -10.0f, -5.0f, 0.0f, 6.0f, 12.0f}) {
        const double position = neuracoust::daw::dawFaderPositionForDb(db);
        assert(std::abs(neuracoust::daw::dbForDawFaderPosition(position) - db) < 0.0001f);
    }
    assert(neuracoust::daw::setTrackVolumeAutomationPoint(trackEditProject, "Audio 1", 0.25, -12.0f));
    assert(neuracoust::daw::setTrackVolumeAutomationPoint(trackEditProject, "Audio 1", 0.75, -6.0f));
    assert(trackEditProject.tracks[0].volumeAutomation.size() == 2);
    assert(neuracoust::daw::setTrackVolumeAutomationPoint(trackEditProject, "Master", 1.0, 0.0f));
    assert(neuracoust::daw::setTrackVolumeAutomationPoint(trackEditProject, "Master", 4.0, -120.0f));
    auto masterAutomationTrack = std::find_if(trackEditProject.tracks.begin(), trackEditProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Master";
    });
    assert(masterAutomationTrack != trackEditProject.tracks.end());
    assert(masterAutomationTrack->volumeAutomation.size() == 2);
    assert(!neuracoust::daw::setTrackVolumeAutomationPoint(trackEditProject, "Monitor", 1.0, -12.0f));
    assert(neuracoust::daw::moveTrackVolumeAutomationPoint(trackEditProject, "Audio 1", 1, 0.5, -3.0f));
    assert(trackEditProject.tracks[0].volumeAutomation[1].timeSeconds == 0.5);
    assert(trackEditProject.tracks[0].volumeAutomation[1].value == -3.0f);
    assert(!neuracoust::daw::moveTrackVolumeAutomationPoint(trackEditProject, "Audio 1", 8, 0.5, -3.0f));
    assert(neuracoust::daw::setTrackPan(trackEditProject, "Audio 1", -2.0f));
    assert(trackEditProject.tracks[0].pan == -1.0f);
    assert(neuracoust::daw::setTrackAutomationLanePoint(trackEditProject, "Audio 1", "track.pan", "Pan", 0.25, -0.5f));
    assert(neuracoust::daw::setTrackAutomationLanePoint(trackEditProject, "Audio 1", "track.pan", "Pan", 0.75, 0.25f));
    assert(trackEditProject.tracks[0].automationLanes.size() == 1);
    assert(trackEditProject.tracks[0].automationLanes[0].parameterId == "track.pan");
    assert(trackEditProject.tracks[0].automationLanes[0].displayName == "Pan");
    assert(trackEditProject.tracks[0].automationLanes[0].points.size() == 2);
    assert(neuracoust::daw::setTrackAutomationLanePoint(trackEditProject, "Audio 1", "track.pan", "Pan", 0.7502, 0.5f));
    assert(trackEditProject.tracks[0].automationLanes[0].points.size() == 2);
    assert(std::abs(trackEditProject.tracks[0].automationLanes[0].points[1].value - 0.5f) < 0.0001f);
    assert(neuracoust::daw::moveTrackAutomationLanePoint(trackEditProject, "Audio 1", "track.pan", 1, 1.25, -0.75f));
    assert(trackEditProject.tracks[0].automationLanes[0].points[1].timeSeconds == 1.25);
    assert(std::abs(trackEditProject.tracks[0].automationLanes[0].points[1].value + 0.75f) < 0.0001f);
    trackEditProject.monitorStationListenMode = "M";
    trackEditProject.monitorStationMono = true;
    trackEditProject.monitorStationSwapLeftRight = true;
    assert(neuracoust::daw::normalizeMonitorStationProjectState(trackEditProject));
    assert(trackEditProject.monitorStationListenMode == "M");
    assert(!trackEditProject.monitorStationMono);
    assert(!trackEditProject.monitorStationSwapLeftRight);
    assert(!neuracoust::daw::normalizeMonitorStationProjectState(trackEditProject));
    trackEditProject.monitorStationListenMode = "S";
    trackEditProject.monitorStationMono = true;
    trackEditProject.monitorStationSwapLeftRight = true;
    assert(neuracoust::daw::normalizeMonitorStationProjectState(trackEditProject));
    assert(trackEditProject.monitorStationListenMode == "S");
    assert(!trackEditProject.monitorStationMono);
    assert(!trackEditProject.monitorStationSwapLeftRight);
    trackEditProject.monitorStationListenMode = "L";
    trackEditProject.monitorStationMono = false;
    trackEditProject.monitorStationSwapLeftRight = true;
    assert(!neuracoust::daw::normalizeMonitorStationProjectState(trackEditProject));
    assert(trackEditProject.monitorStationListenMode == "L");
    assert(!trackEditProject.monitorStationMono);
    assert(trackEditProject.monitorStationSwapLeftRight);
    trackEditProject.monitorStationListenMode = "R";
    trackEditProject.monitorStationMono = true;
    assert(!neuracoust::daw::normalizeMonitorStationProjectState(trackEditProject));
    assert(trackEditProject.monitorStationListenMode == "R");
    assert(trackEditProject.monitorStationMono);
    trackEditProject.monitorStationListenMode = "LR";
    trackEditProject.monitorStationMono = false;
    assert(!neuracoust::daw::normalizeMonitorStationProjectState(trackEditProject));
    assert(trackEditProject.monitorStationListenMode == "LR");
    trackEditProject.monitorStationListenMode = "Bad";
    trackEditProject.monitorStationMono = true;
    assert(neuracoust::daw::normalizeMonitorStationProjectState(trackEditProject));
    assert(trackEditProject.monitorStationListenMode == "LR");
    assert(trackEditProject.monitorStationMono);
    assert(!neuracoust::daw::moveTrackAutomationLanePoint(trackEditProject, "Audio 1", "track.pan", 9, 1.25, 0.0f));
    assert(!neuracoust::daw::setTrackAutomationLanePoint(trackEditProject, "Master", "track.pan", "Pan", 0.25, 0.0f));
    assert(neuracoust::daw::setTrackInputMonitoring(trackEditProject, "Audio 1", true));
    neuracoust::daw::ProjectDocument inputMonitorRoundTrip;
    assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(trackEditProject), inputMonitorRoundTrip, projectParseError));
    assert(!inputMonitorRoundTrip.tracks.empty());
    assert(inputMonitorRoundTrip.tracks[0].inputMonitoring);
    assert(neuracoust::daw::addTrackInsertSlot(trackEditProject, "Audio 1"));
    assert(trackEditProject.tracks[0].inserts.size() == 1);
    assert(trackEditProject.tracks[0].inserts[0].pluginName == "No Insert");
    assert(!trackEditProject.tracks[0].inserts[0].enabled);
    assert(!neuracoust::daw::toggleTrackInsertBypass(trackEditProject, "Audio 1", 0));
    auto eqInsert = makeTrackInsert("Track EQ", "VST3", "/Library/Audio/Plug-Ins/VST3/Track EQ.vst3");
    eqInsert.parameters.push_back({77, "Gain", 0.25});
    assert(neuracoust::daw::setTrackInsertSlot(trackEditProject, "Audio 1", 0, eqInsert));
    neuracoust::daw::ProjectDocument insertParameterRoundTrip;
    assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(trackEditProject), insertParameterRoundTrip, projectParseError));
    assert(!insertParameterRoundTrip.tracks.front().inserts.front().parameters.empty());
    assert(insertParameterRoundTrip.tracks.front().inserts.front().parameters.front().parameterId == 77);
    trackEditProject.tracks[0].inserts[0].dspExecutionMode = "external";
    trackEditProject.tracks[0].inserts[0].assignedDspServerId = "Neuracoust DSP Server";
    trackEditProject.tracks[0].inserts[0].serverModuleId = "na.neuracoust.4001e";
    trackEditProject.tracks[0].inserts[0].reportedLatencySamples = 384;
    trackEditProject.tracks[0].inserts[0].dspAvailable = false;
    trackEditProject.tracks[0].inserts[0].dspLastError = "waiting for matching server module";
    neuracoust::daw::ProjectDocument insertDspRoundTrip;
    assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(trackEditProject), insertDspRoundTrip, projectParseError));
    const auto& restoredDspInsert = insertDspRoundTrip.tracks.front().inserts.front();
    assert(restoredDspInsert.dspExecutionMode == "external");
    assert(restoredDspInsert.assignedDspServerId == "Neuracoust DSP Server");
    assert(restoredDspInsert.serverModuleId == "na.neuracoust.4001e");
    assert(restoredDspInsert.reportedLatencySamples == 384);
    assert(!restoredDspInsert.dspAvailable);
    assert(restoredDspInsert.dspLastError == "waiting for matching server module");
    trackEditProject.tracks[0].inserts[0].dspExecutionMode = "remote_internal";
    trackEditProject.tracks[0].inserts[0].assignedDspServerId = "Mac mini Remote Core";
    trackEditProject.tracks[0].inserts[0].reportedLatencySamples = 512;
    trackEditProject.tracks[0].inserts[0].dspAvailable = true;
    trackEditProject.tracks[0].inserts[0].dspLastError.clear();
    neuracoust::daw::ProjectDocument remoteInternalDspRoundTrip;
    assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(trackEditProject), remoteInternalDspRoundTrip, projectParseError));
    const auto& restoredRemoteInternalDspInsert = remoteInternalDspRoundTrip.tracks.front().inserts.front();
    assert(restoredRemoteInternalDspInsert.dspExecutionMode == "remote_internal");
    assert(restoredRemoteInternalDspInsert.assignedDspServerId == "Mac mini Remote Core");
    assert(restoredRemoteInternalDspInsert.serverModuleId == "na.neuracoust.4001e");
    assert(restoredRemoteInternalDspInsert.reportedLatencySamples == 512);
    assert(restoredRemoteInternalDspInsert.dspAvailable);
    assert(restoredRemoteInternalDspInsert.dspLastError.empty());
    assert(trackEditProject.tracks[0].inserts[0].enabled);
    assert(neuracoust::daw::toggleTrackInsertBypass(trackEditProject, "Audio 1", 0));
    assert(trackEditProject.tracks[0].inserts[0].bypassed);
    assert(neuracoust::daw::addTrackInsertSlot(trackEditProject, "Audio 1"));
    assert(neuracoust::daw::setTrackInsertSlot(trackEditProject, "Audio 1", 1, {"Track Comp", "VST3", "/Library/Audio/Plug-Ins/VST3/Track Comp.vst3", false, true}));
    assert(neuracoust::daw::moveTrackInsertSlot(trackEditProject, "Audio 1", 1, -1) == 0);
    assert(trackEditProject.tracks[0].inserts[0].pluginName == "Track Comp");
    assert(trackEditProject.tracks[0].inserts[1].pluginName == "Track EQ");
    assert(neuracoust::daw::removeTrackInsertSlot(trackEditProject, "Audio 1", 1));
    assert(trackEditProject.tracks[0].inserts.size() == 1);
    assert(!neuracoust::daw::addTrackInsertSlot(trackEditProject, "Master"));
    assert(!neuracoust::daw::setTrackInsertSlot(trackEditProject, "Audio 1", 99, {"Bad", "VST3", "/bad.vst3", false, true}));
    auto insertLimitProject = neuracoust::daw::defaultProject();
    for (size_t insertIndex = 0; insertIndex < neuracoust::daw::kMaxTrackInsertSlots; ++insertIndex) {
        assert(neuracoust::daw::addTrackInsertSlot(insertLimitProject, "Audio 1"));
    }
    assert(insertLimitProject.tracks[0].inserts.size() == neuracoust::daw::kMaxTrackInsertSlots);
    assert(!neuracoust::daw::addTrackInsertSlot(insertLimitProject, "Audio 1"));
    assert(insertLimitProject.tracks[0].inserts.size() == neuracoust::daw::kMaxTrackInsertSlots);
    neuracoust::daw::TrackSendState editSend;
    assert(editSend.busName.empty());
    editSend.busName = "  Bus 3-4  ";
    editSend.gainDb = 99.0f;
    editSend.pan = -2.0f;
    editSend.stereo = true;
    assert(neuracoust::daw::addTrackSendSlot(trackEditProject, "Audio 1", editSend));
    assert(trackEditProject.tracks[0].sends.size() == 1);
    assert(trackEditProject.tracks[0].sends[0].busName == "Bus 3-4");
    assert(trackEditProject.tracks[0].sends[0].gainDb == 12.0f);
    assert(trackEditProject.tracks[0].sends[0].pan == -1.0f);
    assert(neuracoust::daw::setTrackSendEnabled(trackEditProject, "Audio 1", 0, false));
    assert(!trackEditProject.tracks[0].sends[0].enabled);
    assert(neuracoust::daw::toggleTrackSendPreFader(trackEditProject, "Audio 1", 0));
    assert(trackEditProject.tracks[0].sends[0].preFader);
    assert(neuracoust::daw::toggleTrackSendStereo(trackEditProject, "Audio 1", 0));
    assert(!trackEditProject.tracks[0].sends[0].stereo);
    neuracoust::daw::TrackSendState secondSend;
    secondSend.busName = "Bus 9-10";
    secondSend.gainDb = -9.0f;
    secondSend.pan = 0.25f;
    secondSend.stereo = true;
    assert(neuracoust::daw::addTrackSendSlot(trackEditProject, "Audio 1", secondSend));
    assert(neuracoust::daw::moveTrackSendSlot(trackEditProject, "Audio 1", 1, -1) == 0);
    assert(trackEditProject.tracks[0].sends[0].busName == "Bus 9-10");
    assert(trackEditProject.tracks[0].sends[1].busName == "Bus 3-4");
    assert(neuracoust::daw::moveTrackSendSlot(trackEditProject, "Audio 1", 0, 1) == 1);
    assert(trackEditProject.tracks[0].sends[0].busName == "Bus 3-4");
    assert(trackEditProject.tracks[0].sends[1].busName == "Bus 9-10");
    assert(neuracoust::daw::removeTrackSendSlot(trackEditProject, "Audio 1", 1));
    neuracoust::daw::TrackState unusedAux;
    unusedAux.name = "Aux 1";
    unusedAux.trackType = "aux";
    unusedAux.inputBus = "Bus 3-4";
    unusedAux.outputBus = "Master";
    trackEditProject.tracks.insert(trackEditProject.tracks.end() - 1, unusedAux);
    neuracoust::daw::TrackSendState movedSend = trackEditProject.tracks[0].sends[0];
    movedSend.busName = "Bus 5-6";
    movedSend.gainDb = -18.0f;
    movedSend.pan = 0.5f;
    assert(neuracoust::daw::setTrackSendSlot(trackEditProject, "Audio 1", 0, movedSend));
    assert(trackEditProject.tracks[0].sends[0].busName == "Bus 5-6");
    assert(std::none_of(trackEditProject.tracks.begin(), trackEditProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Aux 1" && track.inputBus == "Bus 3-4";
    }));
    neuracoust::daw::TrackState removableAux;
    removableAux.name = "Aux 2";
    removableAux.trackType = "aux";
    removableAux.inputBus = "Bus 5-6";
    removableAux.outputBus = "Master";
    trackEditProject.tracks.insert(trackEditProject.tracks.end() - 1, removableAux);
    assert(neuracoust::daw::removeTrackSendSlot(trackEditProject, "Audio 1", 0));
    assert(trackEditProject.tracks[0].sends.empty());
    assert(std::none_of(trackEditProject.tracks.begin(), trackEditProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Aux 2" && track.inputBus == "Bus 5-6";
    }));
    neuracoust::daw::TrackSendState fallbackSend;
    fallbackSend.gainDb = -6.0f;
    assert(neuracoust::daw::addTrackSendSlot(trackEditProject, "Audio 1", fallbackSend));
    assert(trackEditProject.tracks[0].sends.back().busName.empty());
    assert(!trackEditProject.tracks[0].sends.back().enabled);
    assert(neuracoust::daw::setTrackSendEnabled(trackEditProject, "Audio 1", trackEditProject.tracks[0].sends.size() - 1, true));
    assert(!trackEditProject.tracks[0].sends.back().enabled);
    assert(neuracoust::daw::removeTrackSendSlot(trackEditProject, "Audio 1", trackEditProject.tracks[0].sends.size() - 1));
    {
        auto noSendProject = neuracoust::daw::defaultProject();
        noSendProject.tracks[0].sends.push_back({"", -12.0f, 0.0f, true, false, true});
        std::string noSendError;
        neuracoust::daw::ProjectDocument noSendRoundTrip;
        assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(noSendProject), noSendRoundTrip, noSendError));
        assert(noSendRoundTrip.tracks[0].sends.size() == 1);
        assert(noSendRoundTrip.tracks[0].sends[0].busName.empty());
        assert(!noSendRoundTrip.tracks[0].sends[0].enabled);
    }
    assert(!neuracoust::daw::addTrackSendSlot(trackEditProject, "Master", editSend));
    assert(!neuracoust::daw::setTrackSendSlot(trackEditProject, "Audio 1", 99, editSend));
    {
        auto mixerProject = neuracoust::daw::defaultProject();
        assert(neuracoust::daw::addTrackInsertSlot(mixerProject, "Audio 1"));
        assert(neuracoust::daw::addTrackInsertSlot(mixerProject, "Audio 1"));
        assert(neuracoust::daw::setTrackInsertSlot(mixerProject, "Audio 1", 0, makeTrackInsert("Mixer EQ", "VST3", "/Library/Audio/Plug-Ins/VST3/Mixer EQ.vst3")));
        assert(neuracoust::daw::setTrackInsertSlot(mixerProject, "Audio 1", 1, makeTrackInsert("Mixer Comp", "VST3", "/Library/Audio/Plug-Ins/VST3/Mixer Comp.vst3", true)));
        neuracoust::daw::TrackSendState sendA;
        sendA.busName = "Bus 1-2";
        sendA.stereo = true;
        sendA.preFader = true;
        sendA.gainDb = -3.0f;
        assert(neuracoust::daw::addTrackSendSlot(mixerProject, "Audio 1", sendA));
        neuracoust::daw::TrackSendState sendB;
        sendB.busName = "Bus 3";
        sendB.stereo = false;
        sendB.gainDb = -9.0f;
        assert(neuracoust::daw::addTrackSendSlot(mixerProject, "Audio 1", sendB));
        assert(neuracoust::daw::setTrackSendEnabled(mixerProject, "Audio 1", 1, true));
        neuracoust::daw::TrackSendState sendC;
        sendC.busName = "Bus 7-8";
        sendC.stereo = true;
        sendC.gainDb = -15.0f;
        assert(neuracoust::daw::addTrackSendSlot(mixerProject, "Audio 1", sendC));
        assert(neuracoust::daw::moveTrackSendSlot(mixerProject, "Audio 1", 2, -1) == 1);
        assert(neuracoust::daw::moveTrackSendSlotToIndex(mixerProject, "Audio 1", 1, 3) == 2);
        assert(neuracoust::daw::moveTrackSendSlotToIndex(mixerProject, "Audio 1", 2, 1) == 1);
        const auto vcaName = neuracoust::daw::addVcaTrack(mixerProject);
        assert(neuracoust::daw::setTrackControlMaster(mixerProject, "Audio 1", vcaName));
        mixerProject.tracks[0].inputBus = "Input 1";
        mixerProject.tracks[0].outputBus = "Master";
        mixerProject.tracks[0].automationMode = "touch_latch";
        mixerProject.tracks[0].trackViewMode = "volume";
        mixerProject.tracks[0].timebaseMode = "ticks";
        mixerProject.tracks[0].elasticAudioMode = "polyphonic";
        mixerProject.tracks[0].mixGroupName = "A";
        mixerProject.tracks[0].mixerOrder = 2;
        mixerProject.tracks[1].mixerHidden = true;
        mixerProject.tracks[1].automationMode = "latch";
        mixerProject.tracks[1].trackViewMode = "not-a-mode";
        mixerProject.tracks[1].timebaseMode = "bars";
        mixerProject.tracks[1].elasticAudioMode = "spectral";
        mixerProject.tracks[1].mixGroupName = "A";
        mixerProject.tracks[1].mixerOrder = 1;
        std::string mixerError;
        neuracoust::daw::ProjectDocument mixerRoundTrip;
        assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(mixerProject), mixerRoundTrip, mixerError));
        assert(mixerRoundTrip.tracks[0].inserts.size() == 2);
        assert(mixerRoundTrip.tracks[0].inserts[0].pluginName == "Mixer EQ");
        assert(mixerRoundTrip.tracks[0].inserts[1].bypassed);
        assert(mixerRoundTrip.tracks[0].sends.size() == 3);
        assert(mixerRoundTrip.tracks[0].sends[0].busName == "Bus 1-2");
        assert(mixerRoundTrip.tracks[0].sends[0].preFader);
        assert(mixerRoundTrip.tracks[0].sends[1].busName == "Bus 7-8");
        assert(mixerRoundTrip.tracks[0].sends[1].gainDb == -15.0f);
        assert(mixerRoundTrip.tracks[0].sends[2].busName == "Bus 3");
        assert(mixerRoundTrip.tracks[0].sends[2].enabled);
        assert(mixerRoundTrip.tracks[0].controlMasterTrackName == vcaName);
        assert(mixerRoundTrip.tracks[0].inputBus == "Input 1");
        assert(mixerRoundTrip.tracks[0].outputBus == "Master");
        assert(mixerRoundTrip.tracks[0].automationMode == "touch_latch");
        assert(mixerRoundTrip.tracks[0].trackViewMode == "volume");
        assert(mixerRoundTrip.tracks[0].timebaseMode == "ticks");
        assert(mixerRoundTrip.tracks[0].elasticAudioMode == "polyphonic");
        assert(mixerRoundTrip.tracks[0].mixGroupName == "A");
        assert(mixerRoundTrip.tracks[0].mixerOrder == 2);
        assert(mixerRoundTrip.tracks[1].mixerHidden);
        assert(mixerRoundTrip.tracks[1].automationMode == "latch");
        assert(mixerRoundTrip.tracks[1].trackViewMode == "waveform");
        assert(mixerRoundTrip.tracks[1].timebaseMode == "samples");
        assert(mixerRoundTrip.tracks[1].elasticAudioMode == "none");
        assert(mixerRoundTrip.tracks[1].mixGroupName == "A");
        assert(mixerRoundTrip.tracks[1].mixerOrder == 1);
    }
    {
        auto largeMixerProject = neuracoust::daw::defaultProject();
        for (int index = 3; index <= 128; ++index) {
            neuracoust::daw::TrackState track;
            track.name = "Audio " + std::to_string(index);
            track.trackType = "audio";
            track.inputBus = "Input 1";
            track.outputBus = "Master";
            track.mixerOrder = index;
            track.automationMode = (index % 5 == 0) ? "write" : "read";
            if (index % 7 == 0) {
                track.mixGroupName = "B";
            }
            if (index % 11 == 0) {
                track.mixerHidden = true;
            }
            for (int sendIndex = 0; sendIndex < 3; ++sendIndex) {
                neuracoust::daw::TrackSendState send;
                send.busName = sendIndex == 0 ? "Bus 1-2" : (sendIndex == 1 ? "Bus 3-4" : "Bus 5");
                send.stereo = send.busName.find('-') != std::string::npos;
                send.enabled = true;
                send.gainDb = -6.0f - static_cast<float>(sendIndex);
                track.sends.push_back(send);
            }
            largeMixerProject.tracks.insert(largeMixerProject.tracks.end() - 2, track);
        }
        std::string largeMixerError;
        neuracoust::daw::ProjectDocument largeMixerRoundTrip;
        assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(largeMixerProject), largeMixerRoundTrip, largeMixerError));
        assert(largeMixerRoundTrip.tracks.size() >= 128);
        const auto largeMixerGraph = neuracoust::daw::buildMixerGraph(largeMixerRoundTrip);
        assert(largeMixerGraph.routes.size() >= 128);
        auto audio55 = std::find_if(largeMixerRoundTrip.tracks.begin(), largeMixerRoundTrip.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 55";
        });
        assert(audio55 != largeMixerRoundTrip.tracks.end());
        assert(audio55->automationMode == "write");
        assert(audio55->mixerHidden);
        auto audio49 = std::find_if(largeMixerRoundTrip.tracks.begin(), largeMixerRoundTrip.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 49";
        });
        assert(audio49 != largeMixerRoundTrip.tracks.end());
        assert(audio49->mixGroupName == "B");
        assert(audio49->sends.size() == 3);
        auto audio126 = std::find_if(largeMixerRoundTrip.tracks.begin(), largeMixerRoundTrip.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 126";
        });
        assert(audio126 != largeMixerRoundTrip.tracks.end());
        assert(audio126->mixGroupName == "B");
        assert(audio126->sends.size() == 3);
    }
    {
        auto vcaEditProject = neuracoust::daw::defaultProject();
        const auto vcaName = neuracoust::daw::addVcaTrack(vcaEditProject);
        assert(vcaName == "VCA 1");
        auto vcaIt = std::find_if(vcaEditProject.tracks.begin(), vcaEditProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == vcaName;
        });
        assert(vcaIt != vcaEditProject.tracks.end());
        assert(vcaIt->trackType == "vca");
        assert(vcaIt->inputBus.empty());
        assert(vcaIt->outputBus.empty());
        assert(neuracoust::daw::setTrackControlMaster(vcaEditProject, "Audio 1", vcaName));
        assert(vcaEditProject.tracks[0].controlMasterTrackName == vcaName);
        assert(!neuracoust::daw::setTrackControlMaster(vcaEditProject, "Audio 1", "Master"));
        assert(!neuracoust::daw::setTrackControlMaster(vcaEditProject, vcaName, vcaName));
        assert(neuracoust::daw::setTrackVolumeDb(vcaEditProject, vcaName, -6.0f));
        assert(!neuracoust::daw::setTrackPan(vcaEditProject, vcaName, 0.5f));
        assert(!neuracoust::daw::addTrackInsertSlot(vcaEditProject, vcaName));
        assert(!neuracoust::daw::addTrackSendSlot(vcaEditProject, vcaName, editSend));
        assert(neuracoust::daw::renameTrack(vcaEditProject, vcaName, "Drums VCA"));
        assert(vcaEditProject.tracks[0].controlMasterTrackName == "Drums VCA");
        assert(neuracoust::daw::setTrackControlMaster(vcaEditProject, "Audio 1", ""));
        assert(vcaEditProject.tracks[0].controlMasterTrackName.empty());
        assert(neuracoust::daw::setTrackControlMaster(vcaEditProject, "Audio 1", "Drums VCA"));
        assert(neuracoust::daw::deleteTrackIfEmpty(vcaEditProject, "Drums VCA"));
        assert(vcaEditProject.tracks[0].controlMasterTrackName.empty());
    }
    {
        auto folderProject = neuracoust::daw::defaultProject();
        const auto folderName = neuracoust::daw::addFolderTrack(folderProject);
        const auto busFolderName = neuracoust::daw::addBusFolderTrack(folderProject, "Bus 31-32");
        assert(neuracoust::daw::moveTrack(folderProject, folderName, -1) == 1);
        assert(neuracoust::daw::moveTrack(folderProject, busFolderName, -1) == 2);
        auto folderIt = std::find_if(folderProject.tracks.begin(), folderProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == folderName;
        });
        auto busFolderIt = std::find_if(folderProject.tracks.begin(), folderProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == busFolderName;
        });
        assert(folderIt != folderProject.tracks.end());
        assert(busFolderIt != folderProject.tracks.end());
        assert(folderIt->trackType == "folder");
        assert(busFolderIt->trackType == "bus_folder");
        assert(busFolderIt->inputBus == "Bus 31-32");
        assert(neuracoust::daw::appendAudioClipAt(folderProject, folderName, "/tmp/folder.wav", 0.0, 1.0).empty());
        assert(!neuracoust::daw::appendAudioClipAt(folderProject, busFolderName, "/tmp/bus-folder.wav", 0.0, 1.0).empty());
        assert(!neuracoust::daw::setTrackVolumeDb(folderProject, folderName, -6.0f));
        assert(!neuracoust::daw::setTrackPan(folderProject, folderName, 0.5f));
        assert(!neuracoust::daw::addTrackInsertSlot(folderProject, folderName));
        assert(!neuracoust::daw::addTrackSendSlot(folderProject, folderName, neuracoust::daw::TrackSendState{}));
        assert(!neuracoust::daw::setTrackVolumeAutomationPoint(folderProject, folderName, 0.0, -3.0f));
        assert(neuracoust::daw::setTrackVolumeDb(folderProject, busFolderName, -6.0f));
        assert(neuracoust::daw::addTrackInsertSlot(folderProject, busFolderName));
        assert(neuracoust::daw::addTrackSendSlot(folderProject, busFolderName, neuracoust::daw::TrackSendState{}));
        neuracoust::daw::TrackSendState busFolderSelfSend;
        busFolderSelfSend.busName = "Bus 31-32";
        busFolderSelfSend.enabled = true;
        assert(neuracoust::daw::addTrackSendSlot(folderProject, busFolderName, busFolderSelfSend));
        assert(folderProject.tracks[static_cast<size_t>(std::distance(folderProject.tracks.begin(), busFolderIt))].sends.back().busName == "Bus 31-32");
        assert(!folderProject.tracks[static_cast<size_t>(std::distance(folderProject.tracks.begin(), busFolderIt))].sends.back().enabled);
        assert(!neuracoust::daw::setTrackSendEnabled(folderProject, busFolderName, folderProject.tracks[static_cast<size_t>(std::distance(folderProject.tracks.begin(), busFolderIt))].sends.size() - 1, true));
        assert(!folderProject.tracks[static_cast<size_t>(std::distance(folderProject.tracks.begin(), busFolderIt))].sends.back().enabled);
        assert(!neuracoust::daw::setTrackSendSlot(folderProject, busFolderName, 0, busFolderSelfSend));
        assert(neuracoust::daw::setTrackFolderCollapsed(folderProject, folderName, true));
        std::string folderError;
        neuracoust::daw::ProjectDocument folderRoundTrip;
        assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(folderProject), folderRoundTrip, folderError));
        auto folderRoundTripIt = std::find_if(folderRoundTrip.tracks.begin(), folderRoundTrip.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == folderName;
        });
        assert(folderRoundTripIt != folderRoundTrip.tracks.end());
        assert(folderRoundTripIt->trackType == "folder");
        assert(folderRoundTripIt->folderCollapsed);

        auto folderMoveProject = neuracoust::daw::defaultProject();
        const auto dragFolderName = neuracoust::daw::addFolderTrack(folderMoveProject);
        const auto childName = neuracoust::daw::addAudioTrack(folderMoveProject);
        assert(neuracoust::daw::setTrackFolderCollapsed(folderMoveProject, dragFolderName, true));
        assert(neuracoust::daw::moveTrackIntoFolder(folderMoveProject, "Audio 1", dragFolderName));
        auto dragFolderIt = std::find_if(folderMoveProject.tracks.begin(), folderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == dragFolderName;
        });
        assert(dragFolderIt != folderMoveProject.tracks.end());
        assert(!dragFolderIt->folderCollapsed);
        const auto dragFolderIndex = static_cast<size_t>(std::distance(folderMoveProject.tracks.begin(), dragFolderIt));
        assert(dragFolderIndex + 1 < folderMoveProject.tracks.size());
        assert(folderMoveProject.tracks[dragFolderIndex + 1].name == "Audio 1");
        assert(folderMoveProject.tracks[dragFolderIndex + 1].folderName == dragFolderName);
        const auto explicitNonChildIt = std::find_if(folderMoveProject.tracks.begin(), folderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == childName;
        });
        assert(explicitNonChildIt != folderMoveProject.tracks.end());
        assert(explicitNonChildIt->folderName.empty());
        assert(folderMoveProject.tracks[folderMoveProject.tracks.size() - 2].name == "Master");
        assert(folderMoveProject.tracks[folderMoveProject.tracks.size() - 2].folderName.empty());
        assert(folderMoveProject.tracks[folderMoveProject.tracks.size() - 1].name == "Monitor");
        assert(folderMoveProject.tracks[folderMoveProject.tracks.size() - 1].folderName.empty());
        assert(!neuracoust::daw::moveTrackIntoFolder(folderMoveProject, dragFolderName, dragFolderName));
        assert(!neuracoust::daw::moveTrackIntoFolder(folderMoveProject, "Master", dragFolderName));
        std::string folderMoveError;
        neuracoust::daw::ProjectDocument folderMoveRoundTrip;
        assert(neuracoust::daw::deserializeProject(neuracoust::daw::serializeProject(folderMoveProject), folderMoveRoundTrip, folderMoveError));
        auto childRoundTripIt = std::find_if(folderMoveRoundTrip.tracks.begin(), folderMoveRoundTrip.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        assert(childRoundTripIt != folderMoveRoundTrip.tracks.end());
        assert(childRoundTripIt->folderName == dragFolderName);
        assert(neuracoust::daw::moveTrackNearTrack(folderMoveRoundTrip, "Audio 1", childName, true));
        childRoundTripIt = std::find_if(folderMoveRoundTrip.tracks.begin(), folderMoveRoundTrip.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        assert(childRoundTripIt != folderMoveRoundTrip.tracks.end());
        assert(childRoundTripIt->folderName.empty());

        auto sameFolderMoveProject = neuracoust::daw::defaultProject();
        const auto sameFolderName = neuracoust::daw::addFolderTrack(sameFolderMoveProject);
        const auto sameFolderAudio2 = neuracoust::daw::addAudioTrack(sameFolderMoveProject);
        assert(neuracoust::daw::moveTrackIntoFolder(sameFolderMoveProject, "Audio 1", sameFolderName));
        assert(neuracoust::daw::moveTrackIntoFolder(sameFolderMoveProject, sameFolderAudio2, sameFolderName));
        auto sameFolderAudio1It = std::find_if(sameFolderMoveProject.tracks.begin(), sameFolderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        auto sameFolderAudio2It = std::find_if(sameFolderMoveProject.tracks.begin(), sameFolderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == sameFolderAudio2;
        });
        assert(sameFolderAudio1It != sameFolderMoveProject.tracks.end());
        assert(sameFolderAudio2It != sameFolderMoveProject.tracks.end());
        assert(sameFolderAudio1It->folderName == sameFolderName);
        assert(sameFolderAudio2It->folderName == sameFolderName);
        assert(neuracoust::daw::moveTrackNearTrack(sameFolderMoveProject, "Audio 1", sameFolderAudio2, true));
        sameFolderAudio1It = std::find_if(sameFolderMoveProject.tracks.begin(), sameFolderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        sameFolderAudio2It = std::find_if(sameFolderMoveProject.tracks.begin(), sameFolderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == sameFolderAudio2;
        });
        assert(sameFolderAudio1It != sameFolderMoveProject.tracks.end());
        assert(sameFolderAudio2It != sameFolderMoveProject.tracks.end());
        assert(sameFolderAudio1It->folderName == sameFolderName);
        assert(sameFolderAudio2It->folderName == sameFolderName);
        assert(static_cast<size_t>(std::distance(sameFolderMoveProject.tracks.begin(), sameFolderAudio2It)) <
               static_cast<size_t>(std::distance(sameFolderMoveProject.tracks.begin(), sameFolderAudio1It)));
        assert(neuracoust::daw::moveTrackNearTrack(sameFolderMoveProject, sameFolderAudio2, "Audio 1", false));
        sameFolderAudio1It = std::find_if(sameFolderMoveProject.tracks.begin(), sameFolderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == "Audio 1";
        });
        sameFolderAudio2It = std::find_if(sameFolderMoveProject.tracks.begin(), sameFolderMoveProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == sameFolderAudio2;
        });
        assert(sameFolderAudio1It != sameFolderMoveProject.tracks.end());
        assert(sameFolderAudio2It != sameFolderMoveProject.tracks.end());
        assert(sameFolderAudio1It->folderName == sameFolderName);
        assert(sameFolderAudio2It->folderName == sameFolderName);
        assert(static_cast<size_t>(std::distance(sameFolderMoveProject.tracks.begin(), sameFolderAudio2It)) <
               static_cast<size_t>(std::distance(sameFolderMoveProject.tracks.begin(), sameFolderAudio1It)));

        auto folderBlockProject = neuracoust::daw::defaultProject();
        const auto folderBlockName = neuracoust::daw::addFolderTrack(folderBlockProject);
        const auto outsideTrackName = neuracoust::daw::addAudioTrack(folderBlockProject);
        assert(neuracoust::daw::moveTrackIntoFolder(folderBlockProject, "Audio 1", folderBlockName));
        assert(neuracoust::daw::moveTrackNearTrack(folderBlockProject, folderBlockName, "Audio 2", true));
        auto folderBlockIt = std::find_if(folderBlockProject.tracks.begin(), folderBlockProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == folderBlockName;
        });
        assert(folderBlockIt != folderBlockProject.tracks.end());
        const auto movedFolderIndex = static_cast<size_t>(std::distance(folderBlockProject.tracks.begin(), folderBlockIt));
        assert(movedFolderIndex + 1 < folderBlockProject.tracks.size());
        assert(folderBlockProject.tracks[movedFolderIndex + 1].name == "Audio 1");
        assert(folderBlockProject.tracks[movedFolderIndex + 1].folderName == folderBlockName);
        auto outsideTrackIt = std::find_if(folderBlockProject.tracks.begin(), folderBlockProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
            return track.name == outsideTrackName;
        });
        auto masterBlockIt = std::find_if(folderBlockProject.tracks.begin(), folderBlockProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
            return track.name == "Master";
        });
        assert(outsideTrackIt != folderBlockProject.tracks.end());
        assert(outsideTrackIt->folderName.empty());
        assert(masterBlockIt != folderBlockProject.tracks.end());
        assert(masterBlockIt->folderName.empty());
        assert(!neuracoust::daw::moveTrackNearTrack(folderBlockProject, folderBlockName, "Audio 1", true));
    }
    const auto appendedClip = neuracoust::daw::appendAudioClip(trackEditProject, "Audio 1", "/tmp/clip.wav", 0.5);
    assert(appendedClip == "clip-1");
    assert(trackEditProject.clips.back().startSeconds == 1.0);
    const auto appendedClipColor = trackEditProject.clips.back().colorHex;
    assert(!appendedClipColor.empty());
    const auto secondAppendedClip = neuracoust::daw::appendAudioClip(trackEditProject, "Audio 1", "/tmp/clip-2.wav", 0.25);
    assert(secondAppendedClip == "clip-2");
    assert(trackEditProject.clips.back().startSeconds == 1.5);
    assert(!trackEditProject.clips.back().colorHex.empty());
    assert(trackEditProject.clips.back().colorHex != appendedClipColor);
    trackEditProject.tracks[0].inserts.push_back(makeTrackInsert("Example Insert", "VST3", "/Library/Audio/Plug-Ins/VST3/Example.vst3"));
    trackEditProject.tracks[0].sends.push_back({"Bus 1-2", -9.0f, 0.25f, true, true, true});
    trackEditProject.tracks[0].recordArmed = true;
    trackEditProject.tracks[0].muted = true;
    trackEditProject.tracks[0].solo = true;
    std::string copiedTrackName;
    std::vector<std::string> copiedTrackClipIds;
    assert(neuracoust::daw::duplicateTrackWithClips(trackEditProject, "Audio 1", copiedTrackName, copiedTrackClipIds));
    assert(copiedTrackName == "Audio 1 Copy");
    assert(copiedTrackClipIds.size() == 3);
    assert(trackEditProject.tracks[1].name == copiedTrackName);
    assert(trackEditProject.tracks[1].trackType == "audio");
    assert(!trackEditProject.tracks[1].recordArmed);
    assert(!trackEditProject.tracks[1].muted);
    assert(!trackEditProject.tracks[1].solo);
    assert(trackEditProject.tracks[1].volumeDb == trackEditProject.tracks[0].volumeDb);
    assert(trackEditProject.tracks[1].pan == trackEditProject.tracks[0].pan);
    assert(trackEditProject.tracks[1].inserts.size() == 2);
    assert(trackEditProject.tracks[1].inserts[0].pluginName == "Track Comp");
    assert(trackEditProject.tracks[1].inserts[1].pluginName == "Example Insert");
    assert(trackEditProject.tracks[1].sends.size() == 1);
    assert(std::any_of(trackEditProject.clips.begin(), trackEditProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == copiedTrackClipIds.front() && clip.trackName == copiedTrackName && clip.sourcePath == "/tmp/no-audio.wav";
    }));
    auto copiedPlaylistIt = std::find_if(trackEditProject.trackPlaylists.begin(), trackEditProject.trackPlaylists.end(), [&](const neuracoust::daw::TrackPlaylistState& playlist) {
        return playlist.trackName == copiedTrackName && playlist.active;
    });
    assert(copiedPlaylistIt != trackEditProject.trackPlaylists.end());
    assert(copiedPlaylistIt->placements.size() == copiedTrackClipIds.size());
    assert(std::all_of(copiedPlaylistIt->placements.begin(), copiedPlaylistIt->placements.end(), [&](const neuracoust::daw::PlaylistClipPlacementState& placement) {
        return std::find(copiedTrackClipIds.begin(), copiedTrackClipIds.end(), placement.legacyClipId) != copiedTrackClipIds.end() ||
            std::find(copiedTrackClipIds.begin(), copiedTrackClipIds.end(), placement.id) != copiedTrackClipIds.end();
    }));
    assert(neuracoust::daw::rebuildProjectClipsFromActivePlaylists(trackEditProject));
    assert(std::all_of(copiedTrackClipIds.begin(), copiedTrackClipIds.end(), [&](const std::string& clipId) {
        return std::any_of(trackEditProject.clips.begin(), trackEditProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
            return clip.id == clipId && clip.trackName == copiedTrackName;
        });
    }));
    assert(!neuracoust::daw::duplicateTrackWithClips(trackEditProject, "Master", copiedTrackName, copiedTrackClipIds));
    trackEditProject.tempoMasterTrackName = "Audio 1";
    trackEditProject.tracks[1].inputBus = "Audio 1";
    trackEditProject.tracks[1].outputBus = "Audio 1";
    trackEditProject.tracks[1].sends.push_back({"Audio 1", -9.0f, 0.0f, true, false, true});
    assert(neuracoust::daw::renameTrack(trackEditProject, "Audio 1", "  Lead Vocal  "));
    assert(trackEditProject.tracks[0].name == "Lead Vocal");
    assert(trackEditProject.tempoMasterTrackName == "Lead Vocal");
    assert(trackEditProject.tracks[1].inputBus == "Lead Vocal");
    assert(trackEditProject.tracks[1].outputBus == "Lead Vocal");
    assert(trackEditProject.tracks[1].sends.back().busName == "Lead Vocal");
    assert(std::none_of(trackEditProject.clips.begin(), trackEditProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
        return clip.trackName == "Audio 1";
    }));
    assert(neuracoust::daw::recordingTargetTrackName(trackEditProject) == "Lead Vocal");
    assert(neuracoust::daw::inputChannelCountForBusName("Input 1") == 1);
    assert(neuracoust::daw::inputChannelCountForBusName("Input 1-2") == 2);
    assert(neuracoust::daw::inputChannelCountForBusName("Waves SoundGrid In 7-8") == 2);
    assert(neuracoust::daw::inputChannelCountForBusName("") == 0);
    trackEditProject.tracks[0].inputBus = "Input 1-2";
    assert(neuracoust::daw::recordingInputChannelCount(trackEditProject) == 2);
    trackEditProject.tracks[0].inputBus = "Input 1";
    assert(neuracoust::daw::recordingInputChannelCount(trackEditProject) == 1);
    trackEditProject.tracks[0].trackType = "instrument";
    assert(neuracoust::daw::recordingInputChannelCount(trackEditProject) == 0);
    trackEditProject.tracks[0].trackType = "audio";
    trackEditProject.tracks[0].inputBus.clear();
    assert(neuracoust::daw::recordingInputChannelCount(trackEditProject) == 0);
    trackEditProject.tracks[0].inputBus = "Input 1";
    assert(neuracoust::daw::setTrackRecordArmed(trackEditProject, "Lead Vocal", false));
    assert(neuracoust::daw::setTrackRecordArmed(trackEditProject, "Audio 2", true));
    assert(neuracoust::daw::recordingTargetTrackName(trackEditProject) == "Audio 2");
    for (auto& track : trackEditProject.tracks) {
        if (track.name == "Audio 2") {
            track.inputBus = "Bus 3-4";
        }
    }
    assert(neuracoust::daw::recordingInputChannelCount(trackEditProject) == 2);
    assert(neuracoust::daw::setTrackRecordArmed(trackEditProject, "Audio 2", false));
    assert(neuracoust::daw::recordingTargetTrackName(trackEditProject) == "Lead Vocal");
    auto protectedOnlyRecordingProject = neuracoust::daw::defaultProject();
    protectedOnlyRecordingProject.tracks = {{"Master", 0.0f, 0.0f, false, false, true}, {"Monitor", 0.0f, 0.0f, false, false, true}};
    assert(neuracoust::daw::recordingTargetTrackName(protectedOnlyRecordingProject).empty());
    assert(!neuracoust::daw::renameTrack(trackEditProject, "Lead Vocal", "Audio 2"));
    assert(!neuracoust::daw::renameTrack(trackEditProject, "Lead Vocal", "Master"));
    assert(!neuracoust::daw::renameTrack(trackEditProject, "Lead Vocal", "   "));
    assert(!neuracoust::daw::renameTrack(trackEditProject, "Master", "Mix Bus"));
    const auto renamedJson = neuracoust::daw::serializeProject(trackEditProject);
    assert(renamedJson.find("\"name\":\"Lead Vocal\"") != std::string::npos);
    assert(renamedJson.find("\"trackName\":\"Lead Vocal\"") != std::string::npos);
    assert(renamedJson.find("\"trackName\":\"Audio 1\"") == std::string::npos);
    neuracoust::daw::ProjectDocument renamedRoundTrip;
    assert(neuracoust::daw::deserializeProject(renamedJson, renamedRoundTrip, projectParseError));
    assert(renamedRoundTrip.tracks[0].name == "Lead Vocal");
    assert(renamedRoundTrip.tracks[0].trackType == "audio");
    assert(std::any_of(renamedRoundTrip.clips.begin(), renamedRoundTrip.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == secondAppendedClip && clip.trackName == "Lead Vocal";
    }));
    assert(renamedRoundTrip.tracks[0].volumeAutomation.size() == 2);
    assert(renamedRoundTrip.tracks[0].automationLanes.size() == 1);
    assert(renamedRoundTrip.tracks[0].automationLanes[0].points.size() == 2);
    assert(std::abs(renamedRoundTrip.tracks[0].automationLanes[0].points[1].value + 0.75f) < 0.0001f);
    assert(neuracoust::daw::appendAudioClipAt(trackEditProject, "Audio 1", "/tmp/old-name.wav", 0.25, 0.75).empty());
    const auto placedClip = neuracoust::daw::appendAudioClipAt(trackEditProject, "Lead Vocal", "/tmp/recorded.wav", 0.25, 0.75);
    assert(placedClip == "clip-3");
    assert(trackEditProject.clips.back().startSeconds == 0.25);
    assert(trackEditProject.clips.back().durationSeconds == 0.75);
    std::string recordedClipId;
    std::string recordedClipMessage;
    const auto recordedTakePath = testTempRoot() / "neuracoust-daw-recorded-append-take.wav";
    constexpr double recordedTakeDuration = 8.0 / 48000.0;
    {
        neuracoust::daw::RecordingTake appendTake(1, 48000);
        const int16_t appendTakeSamples[8] = {0, 1024, -1024, 512, -512, 256, -256, 0};
        appendTake.appendInterleavedInt16(appendTakeSamples, 8);
        std::string appendTakeError;
        assert(appendTake.saveWav(recordedTakePath.string(), appendTakeError));
    }
    assert(neuracoust::daw::appendRecordedTakeClip(trackEditProject, "Lead Vocal", recordedTakePath.string(), 1.0, 2.0, recordedClipId, recordedClipMessage));
    assert(recordedClipId == "clip-4");
    assert(recordedClipMessage == "Recorded take appended.");
    assert(trackEditProject.clips.back().sourcePath == recordedTakePath.string());
    assert(trackEditProject.clips.back().startSeconds == 1.0);
    assert(std::abs(trackEditProject.clips.back().durationSeconds - recordedTakeDuration) < 0.000001);
    std::string recordedManifestError;
    assert(neuracoust::daw::writeRecordedTakeManifest(trackEditProject,
                                                      recordedTakePath,
                                                      recordedClipId,
                                                      "Lead Vocal",
                                                      1.0,
                                                      recordedTakeDuration,
                                                      "core-audio-input-1",
                                                      recordedManifestError));
    assert(recordedManifestError.empty());
    {
        std::ifstream recordedManifest(neuracoust::daw::recordedTakeManifestPath(recordedTakePath), std::ios::binary);
        std::string recordedManifestText((std::istreambuf_iterator<char>(recordedManifest)), std::istreambuf_iterator<char>());
        assert(recordedManifestText.find("neuracoust-daw-recording-take-v1") != std::string::npos);
        assert(recordedManifestText.find("\"clipId\": \"clip-4\"") != std::string::npos);
        assert(recordedManifestText.find("\"trackName\": \"Lead Vocal\"") != std::string::npos);
        assert(recordedManifestText.find("\"inputDeviceId\": \"core-audio-input-1\"") != std::string::npos);
        assert(recordedManifestText.find("\"inputBus\": \"Input 1\"") != std::string::npos);
        assert(recordedManifestText.find("\"inputChannels\": 1") != std::string::npos);
        assert(recordedManifestText.find("\"lowLatencyRecordMonitoringEligible\": true") != std::string::npos);
        assert(recordedManifestText.find("\"inputMonitoring\": true") != std::string::npos);
        assert(recordedManifestText.find("\"recordMonitorPath\": \"record-or-input-monitor-low-latency\"") != std::string::npos);
        assert(recordedManifestText.find("\"sampleRate\": 48000") != std::string::npos);
    }
    for (auto& track : trackEditProject.tracks) {
        if (track.name == "Lead Vocal") {
            track.recordArmed = true;
            track.inputBus.clear();
        }
    }
    const auto noInputTakePath = testTempRoot() / "neuracoust-daw-recorded-no-input-take.wav";
    assert(neuracoust::daw::writeRecordedTakeManifest(trackEditProject,
                                                     noInputTakePath,
                                                     "clip-no-input",
                                                     "Lead Vocal",
                                                     2.0,
                                                     recordedTakeDuration,
                                                     "core-audio-input-1",
                                                     recordedManifestError));
    {
        std::ifstream noInputManifest(neuracoust::daw::recordedTakeManifestPath(noInputTakePath), std::ios::binary);
        std::string noInputManifestText((std::istreambuf_iterator<char>(noInputManifest)), std::istreambuf_iterator<char>());
        assert(noInputManifestText.find("\"inputBus\": \"\"") != std::string::npos);
        assert(noInputManifestText.find("\"inputChannels\": 0") != std::string::npos);
        assert(noInputManifestText.find("\"lowLatencyRecordMonitoringEligible\": false") != std::string::npos);
    }
    for (auto& track : trackEditProject.tracks) {
        if (track.name == "Lead Vocal") {
            track.inputBus = "Input 1";
        }
    }
    std::string importedManifestError;
    assert(neuracoust::daw::writeImportedMediaManifest(trackEditProject,
                                                       recordedTakePath,
                                                       "/Users/example/Desktop/source-16bit.wav",
                                                       recordedClipId,
                                                       "Lead Vocal",
                                                       1.0,
                                                       recordedTakeDuration,
                                                       16,
                                                       false,
                                                       48000.0,
                                                       1,
                                                       true,
                                                       true,
                                                       "converted-copy",
                                                       "converted-copy",
                                                       98.0,
                                                       "stretch-to-project",
                                                       true,
                                                       importedManifestError));
    assert(importedManifestError.empty());
    assert(neuracoust::daw::importedMediaManifestPath(recordedTakePath).filename() == "neuracoust-daw-recorded-append-take.wav.import.json");
    {
        std::ifstream importedManifest(neuracoust::daw::importedMediaManifestPath(recordedTakePath), std::ios::binary);
        std::string importedManifestText((std::istreambuf_iterator<char>(importedManifest)), std::istreambuf_iterator<char>());
        assert(importedManifestText.find("neuracoust-daw-imported-media-v1") != std::string::npos);
        assert(importedManifestText.find("\"clipId\": \"clip-4\"") != std::string::npos);
        assert(importedManifestText.find("\"sourceBitsPerSample\": 16") != std::string::npos);
        assert(importedManifestText.find("\"convertedToProjectSampleRate\": true") != std::string::npos);
        assert(importedManifestText.find("\"convertedToProjectBitDepth\": true") != std::string::npos);
        assert(importedManifestText.find("\"sampleRateImportPolicy\": \"converted-copy\"") != std::string::npos);
        assert(importedManifestText.find("\"bitDepthImportPolicy\": \"converted-copy\"") != std::string::npos);
        assert(importedManifestText.find("\"sourceTempoBpm\": 98") != std::string::npos);
        assert(importedManifestText.find("\"tempoSyncPolicy\": \"stretch-to-project\"") != std::string::npos);
        assert(importedManifestText.find("\"pendingTimeStretchToProject\": true") != std::string::npos);
        assert(importedManifestText.find("\"projectBitDepth\": 24") != std::string::npos);
    }
    assert(!neuracoust::daw::writeImportedMediaManifest(trackEditProject,
                                                        {},
                                                        "/tmp/source.wav",
                                                        recordedClipId,
                                                        "Lead Vocal",
                                                        1.0,
                                                        recordedTakeDuration,
                                                        16,
                                                        false,
                                                        48000.0,
                                                        1,
                                                        false,
                                                        false,
                                                        "matched",
                                                        "matched",
                                                        0.0,
                                                        "project-tempo",
                                                        false,
                                                        importedManifestError));
    assert(importedManifestError.find("path is empty") != std::string::npos);
    assert(!neuracoust::daw::writeRecordedTakeManifest(trackEditProject,
                                                       {},
                                                       recordedClipId,
                                                       "Lead Vocal",
                                                       1.0,
                                                       recordedTakeDuration,
                                                       {},
                                                       recordedManifestError));
    assert(recordedManifestError.find("path is empty") != std::string::npos);
    assert(!neuracoust::daw::appendRecordedTakeClip(trackEditProject, "Lead Vocal", "/tmp/neuracoust-daw-missing-take.wav", 1.0, 2.0, recordedClipId, recordedClipMessage));
    assert(recordedClipMessage.find("file is missing") != std::string::npos);
    const auto invalidRecordedTakePath = testTempRoot() / "neuracoust-daw-recorded-invalid-take.wav";
    {
        std::ofstream invalidTake(invalidRecordedTakePath, std::ios::binary);
        invalidTake << "not a wav";
    }
    assert(!neuracoust::daw::appendRecordedTakeClip(trackEditProject, "Lead Vocal", invalidRecordedTakePath.string(), 1.0, 2.0, recordedClipId, recordedClipMessage));
    assert(recordedClipMessage.find("readable WAV") != std::string::npos);
    assert(!neuracoust::daw::appendRecordedTakeClip(trackEditProject, "Master", "/tmp/protected.wav", 1.0, 2.0, recordedClipId, recordedClipMessage));
    assert(recordedClipMessage.find("target track") != std::string::npos);
    assert(neuracoust::daw::appendRecordedTakeClip(trackEditProject, "Lead Vocal", recordedTakePath.string(), 1.25, 0.0, recordedClipId, recordedClipMessage));
    assert(recordedClipId == "clip-5");
    assert(std::abs(trackEditProject.clips.back().durationSeconds - recordedTakeDuration) < 0.000001);
    assert(!neuracoust::daw::appendRecordedTakeClip(trackEditProject, "Lead Vocal", recordedTakePath.string(), 1.0, std::numeric_limits<double>::quiet_NaN(), recordedClipId, recordedClipMessage));
    assert(recordedClipMessage.find("invalid duration") != std::string::npos);
    assert(!neuracoust::daw::appendAudioClipAt(trackEditProject, "Lead Vocal", "/tmp/bad.wav", -0.1, 0.25).size());
    assert(!neuracoust::daw::appendAudioClip(trackEditProject, "Master", "/tmp/master.wav", 0.25).size());
    trackEditProject.markers.clear();   // defaultProject() seeds a start marker; count from zero here
    const auto markerB = neuracoust::daw::addMarkerAt(trackEditProject, 1.2);
    const auto markerA = neuracoust::daw::addMarkerAt(trackEditProject, 0.4);
    assert(markerB == "marker-1");
    assert(markerA == "marker-2");
    assert(trackEditProject.markers.size() == 2);
    assert(trackEditProject.markers[0].timeSeconds == 0.4);
    assert(trackEditProject.markers[1].timeSeconds == 1.2);
    assert(neuracoust::daw::renameNearestMarker(trackEditProject, 0.41, 0.1, "Verse"));
    assert(trackEditProject.markers[0].name == "Verse");
    assert(neuracoust::daw::moveNearestMarker(trackEditProject, 1.2, 0.1, 0.2));
    assert(trackEditProject.markers[0].id == markerB);
    assert(trackEditProject.markers[0].timeSeconds == 0.2);
    assert(!neuracoust::daw::renameNearestMarker(trackEditProject, 9.0, 0.1, "Missing"));
    assert(!neuracoust::daw::moveNearestMarker(trackEditProject, 9.0, 0.1, 1.0));
    auto markerJson = neuracoust::daw::serializeProject(trackEditProject);
    assert(markerJson.find("\"markers\"") != std::string::npos);
    assert(markerJson.find("\"name\":\"Verse\"") != std::string::npos);
    neuracoust::daw::ProjectDocument markerRoundTrip;
    assert(neuracoust::daw::deserializeProject(markerJson, markerRoundTrip, projectParseError));
    assert(markerRoundTrip.markers.size() == 2);
    assert(markerRoundTrip.markers[0].id == markerB);
    assert(markerRoundTrip.markers[1].id == markerA);
    assert(neuracoust::daw::deleteNearestMarker(markerRoundTrip, 0.45, 0.1));
    assert(markerRoundTrip.markers.size() == 1);
    assert(markerRoundTrip.markers.front().id == markerB);
    assert(!neuracoust::daw::deleteNearestMarker(markerRoundTrip, 0.45, 0.01));
    const auto chordB = neuracoust::daw::addChordEventAt(trackEditProject, 1.5, "Chorus");
    const auto chordA = neuracoust::daw::addChordEventAt(trackEditProject, 0.75, "Cmaj7");
    assert(chordB == "chord-1");
    assert(chordA == "chord-2");
    assert(trackEditProject.chordEvents.size() == 2);
    assert(trackEditProject.chordEvents[0].timeSeconds == 0.75);
    assert(trackEditProject.chordEvents[1].timeSeconds == 1.5);
    assert(neuracoust::daw::renameNearestChordEvent(trackEditProject, 0.78, 0.1, "Verse / Cmaj7"));
    assert(trackEditProject.chordEvents[0].name == "Verse / Cmaj7");
    assert(neuracoust::daw::moveNearestChordEvent(trackEditProject, 1.5, 0.1, 0.25));
    assert(trackEditProject.chordEvents[0].id == chordB);
    assert(trackEditProject.chordEvents[0].timeSeconds == 0.25);
    assert(!neuracoust::daw::renameNearestChordEvent(trackEditProject, 9.0, 0.1, "Missing"));
    assert(!neuracoust::daw::moveNearestChordEvent(trackEditProject, 9.0, 0.1, 1.0));
    auto chordJson = neuracoust::daw::serializeProject(trackEditProject);
    assert(chordJson.find("\"chordEvents\"") != std::string::npos);
    assert(chordJson.find("\"name\":\"Verse / Cmaj7\"") != std::string::npos);
    neuracoust::daw::ProjectDocument chordRoundTrip;
    assert(neuracoust::daw::deserializeProject(chordJson, chordRoundTrip, projectParseError));
    assert(chordRoundTrip.chordEvents.size() == 2);
    assert(chordRoundTrip.chordEvents[0].id == chordB);
    assert(chordRoundTrip.chordEvents[1].id == chordA);
    assert(neuracoust::daw::deleteNearestChordEvent(chordRoundTrip, 0.75, 0.1));
    assert(chordRoundTrip.chordEvents.size() == 1);
    assert(chordRoundTrip.chordEvents.front().id == chordB);
    assert(!neuracoust::daw::deleteNearestChordEvent(chordRoundTrip, 0.75, 0.01));
    const auto lyricB = neuracoust::daw::addLyricEventAt(trackEditProject, 2.0, "Second line");
    const auto lyricA = neuracoust::daw::addLyricEventAt(trackEditProject, 1.0, "First line");
    assert(lyricB == "lyric-1");
    assert(lyricA == "lyric-2");
    assert(trackEditProject.lyricEvents.size() == 2);
    assert(trackEditProject.lyricEvents[0].text == "First line");
    assert(neuracoust::daw::renameNearestLyricEvent(trackEditProject, 1.0, 0.1, "Opening lyric"));
    assert(neuracoust::daw::moveNearestLyricEvent(trackEditProject, 2.0, 0.1, 0.5));
    auto lyricJson = neuracoust::daw::serializeProject(trackEditProject);
    assert(lyricJson.find("\"lyricEvents\"") != std::string::npos);
    assert(lyricJson.find("\"text\":\"Opening lyric\"") != std::string::npos);
    neuracoust::daw::ProjectDocument lyricRoundTrip;
    assert(neuracoust::daw::deserializeProject(lyricJson, lyricRoundTrip, projectParseError));
    assert(lyricRoundTrip.lyricEvents.size() == 2);
    assert(lyricRoundTrip.lyricEvents[0].id == lyricB);
    assert(neuracoust::daw::deleteNearestLyricEvent(lyricRoundTrip, 1.0, 0.1));
    assert(lyricRoundTrip.lyricEvents.size() == 1);
    std::vector<neuracoust::daw::LyricTranscriptionSegment> lyricSegments {
        {0.25, 1.0, "  Hello vocal  "},
        {1.50, 0.8, "안녕 노래"},
        {2.75, 0.5, ""}
    };
    const auto lyricApply = neuracoust::daw::applyLyricTranscription(lyricRoundTrip, lyricSegments, 10.0);
    assert(lyricApply.ok);
    assert(lyricApply.addedEvents == 2);
    assert(lyricApply.startSeconds == 10.25);
    assert(lyricRoundTrip.lyricEvents.size() == 3);
    assert(lyricRoundTrip.lyricEvents[1].text == "Hello vocal");
    assert(lyricRoundTrip.lyricEvents[1].timeSeconds == 10.25);
    assert(lyricRoundTrip.lyricEvents[2].text == "안녕 노래");
    std::vector<neuracoust::daw::LyricTranscriptionSegment> replacementLyricSegments {
        {0.30, 0.4, "Replaced line"}
    };
    const auto replacementLyricApply = neuracoust::daw::applyLyricTranscription(lyricRoundTrip,
                                                                                replacementLyricSegments,
                                                                                10.0);
    assert(replacementLyricApply.ok);
    assert(replacementLyricApply.addedEvents == 1);
    assert(replacementLyricApply.removedEvents == 1);
    assert(std::any_of(lyricRoundTrip.lyricEvents.begin(), lyricRoundTrip.lyricEvents.end(), [](const neuracoust::daw::LyricEventState& lyric) {
        return lyric.text == "Replaced line" && lyric.timeSeconds == 10.30;
    }));
    const auto midiTrackName = neuracoust::daw::addMidiTrack(trackEditProject);
    assert(midiTrackName == "MIDI 1");
    const auto instrumentTrackName = neuracoust::daw::addInstrumentTrack(trackEditProject);
    assert(instrumentTrackName == "Instrument 1");
    auto instrumentTrackIt = std::find_if(trackEditProject.tracks.begin(), trackEditProject.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
        return track.name == instrumentTrackName;
    });
    assert(instrumentTrackIt != trackEditProject.tracks.end());
    assert(instrumentTrackIt->trackType == "instrument");
    assert(instrumentTrackIt->inputBus == "MIDI Input");
    assert(instrumentTrackIt->outputBus == "Master");
    neuracoust::daw::InstrumentSlotState synthSlot;
    synthSlot.pluginName = "Round Trip Synth";
    synthSlot.pluginFormat = "VST3";
    synthSlot.pluginPath = "/tmp/RoundTripSynth.vst3";
    synthSlot.enabled = true;
    synthSlot.midiChannel = 3;
    assert(neuracoust::daw::setTrackInstrumentSlot(trackEditProject, instrumentTrackName, synthSlot));
    assert(instrumentTrackIt->instrument.pluginName == "Round Trip Synth");
    assert(instrumentTrackIt->instrument.enabled);
    assert(instrumentTrackIt->instrumentSlots.size() == 1);
    assert(instrumentTrackIt->instrumentSlots[0].pluginName == "Round Trip Synth");
    neuracoust::daw::InstrumentSlotState layerSlot = synthSlot;
    layerSlot.pluginName = "Layer Synth";
    layerSlot.pluginPath = "/tmp/LayerSynth.vst3";
    layerSlot.midiChannel = 4;
    assert(neuracoust::daw::setTrackInstrumentSlot(trackEditProject, instrumentTrackName, 1, layerSlot));
    assert(instrumentTrackIt->instrumentSlots.size() == 2);
    assert(instrumentTrackIt->instrumentSlots[1].pluginName == "Layer Synth");
    assert(instrumentTrackIt->instrument.pluginName == "Round Trip Synth");
    assert(neuracoust::daw::setTrackInstrumentMidiChannel(trackEditProject, instrumentTrackName, 1, 12));
    assert(instrumentTrackIt->instrumentSlots[1].midiChannel == 12);
    assert(neuracoust::daw::toggleTrackInstrumentBypass(trackEditProject, instrumentTrackName, 1));
    assert(instrumentTrackIt->instrumentSlots[1].bypassed);
    assert(neuracoust::daw::toggleTrackInstrumentBypass(trackEditProject, instrumentTrackName, 1));
    assert(!instrumentTrackIt->instrumentSlots[1].bypassed);
    assert(neuracoust::daw::toggleTrackInstrumentBypass(trackEditProject, instrumentTrackName));
    assert(instrumentTrackIt->instrument.bypassed);
    assert(neuracoust::daw::toggleTrackInstrumentBypass(trackEditProject, instrumentTrackName));
    assert(!instrumentTrackIt->instrument.bypassed);
    assert(neuracoust::daw::clearTrackInstrumentSlot(trackEditProject, instrumentTrackName));
    assert(instrumentTrackIt->instrument.pluginName == "No Instrument");
    assert(!instrumentTrackIt->instrument.enabled);
    assert(neuracoust::daw::setTrackInstrumentSlot(trackEditProject, instrumentTrackName, synthSlot));
    assert(neuracoust::daw::setTrackInstrumentMidiChannel(trackEditProject, instrumentTrackName, 10));
    assert(instrumentTrackIt->instrument.midiChannel == 10);
    assert(!neuracoust::daw::setTrackInstrumentMidiChannel(trackEditProject, instrumentTrackName, 17));
    assert(!neuracoust::daw::setTrackInstrumentMidiChannel(trackEditProject, midiTrackName, 1));
    assert(!neuracoust::daw::setTrackInstrumentSlot(trackEditProject, midiTrackName, synthSlot));
    const auto instrumentRegionId = neuracoust::daw::addMidiRegion(trackEditProject, instrumentTrackName, 0.25, 2.0, "Lead");
    assert(instrumentRegionId == "midi-region-1");
    assert(neuracoust::daw::addMidiNote(trackEditProject, instrumentRegionId, 72, 0.0, 1.0, 100).size());
    const auto midiRegionId = neuracoust::daw::addMidiRegion(trackEditProject, midiTrackName, 0.5, 4.0, "Verse Keys");
    assert(midiRegionId == "midi-region-2");
    assert(trackEditProject.midiRegions.size() == 2);
    auto instrumentRegionIt = std::find_if(trackEditProject.midiRegions.begin(), trackEditProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == instrumentRegionId;
    });
    auto midiRegionIt = std::find_if(trackEditProject.midiRegions.begin(), trackEditProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == midiRegionId;
    });
    assert(instrumentRegionIt != trackEditProject.midiRegions.end());
    assert(midiRegionIt != trackEditProject.midiRegions.end());
    assert(instrumentRegionIt->trackName == instrumentTrackName);
    assert(midiRegionIt->trackName == midiTrackName);
    const auto midiNoteA = neuracoust::daw::addMidiNote(trackEditProject, midiRegionId, 60, 0.03, 0.5, 90);
    const auto midiNoteB = neuracoust::daw::addMidiNote(trackEditProject, midiRegionId, 64, 0.52, 0.5, 100, 2);
    const auto midiNoteC = neuracoust::daw::addMidiNote(trackEditProject, midiRegionId, 67, 1.0, 0.25, 80);
    const auto instrumentProgramId = neuracoust::daw::addMidiProgramChangeEvent(trackEditProject, instrumentRegionId, 0.125, 7, 1);
    const auto instrumentSustainId = neuracoust::daw::addMidiSustainEvent(trackEditProject, instrumentRegionId, 0.25, true, 1);
    const auto instrumentBendId = neuracoust::daw::addMidiPitchBendEvent(trackEditProject, instrumentRegionId, 0.75, 12288, 1);
    assert(midiNoteA == "note-1");
    assert(midiNoteB == "note-2");
    assert(midiNoteC == "note-3");
    assert(instrumentProgramId == "program-1");
    assert(instrumentSustainId == "cc-1");
    assert(instrumentBendId == "bend-1");
    assert(neuracoust::daw::moveMidiControllerEvent(trackEditProject, instrumentRegionId, instrumentSustainId, 0.375, 64));
    assert(neuracoust::daw::moveMidiPitchBendEvent(trackEditProject, instrumentRegionId, instrumentBendId, 0.875, 4096));
    assert(neuracoust::daw::moveMidiProgramChangeEvent(trackEditProject, instrumentRegionId, instrumentProgramId, 0.25, 9));
    const auto deletedControllerId = neuracoust::daw::addMidiControllerEvent(trackEditProject, instrumentRegionId, 0.5, 1, 96, 1);
    const auto deletedBendId = neuracoust::daw::addMidiPitchBendEvent(trackEditProject, instrumentRegionId, 0.625, 10000, 1);
    const auto deletedProgramId = neuracoust::daw::addMidiProgramChangeEvent(trackEditProject, instrumentRegionId, 0.75, 12, 1);
    assert(!deletedControllerId.empty());
    assert(!deletedBendId.empty());
    assert(!deletedProgramId.empty());
    assert(neuracoust::daw::moveMidiControllerEvent(trackEditProject, instrumentRegionId, deletedControllerId, 0.55, 32));
    assert(neuracoust::daw::moveMidiPitchBendEvent(trackEditProject, instrumentRegionId, deletedBendId, 0.675, 8192));
    assert(neuracoust::daw::moveMidiProgramChangeEvent(trackEditProject, instrumentRegionId, deletedProgramId, 0.8, 13));
    assert(neuracoust::daw::deleteMidiControllerEvent(trackEditProject, instrumentRegionId, deletedControllerId));
    assert(neuracoust::daw::deleteMidiPitchBendEvent(trackEditProject, instrumentRegionId, deletedBendId));
    assert(neuracoust::daw::deleteMidiProgramChangeEvent(trackEditProject, instrumentRegionId, deletedProgramId));
    midiRegionIt = std::find_if(trackEditProject.midiRegions.begin(), trackEditProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == midiRegionId;
    });
    assert(midiRegionIt != trackEditProject.midiRegions.end());
    assert(midiRegionIt->notes.size() == 3);
    assert(neuracoust::daw::moveMidiRegion(trackEditProject, midiRegionId, midiTrackName, 1.25));
    assert(neuracoust::daw::resizeMidiRegion(trackEditProject, midiRegionId, 3.5));
    midiRegionIt = std::find_if(trackEditProject.midiRegions.begin(), trackEditProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == midiRegionId;
    });
    assert(midiRegionIt != trackEditProject.midiRegions.end());
    assert(std::abs(midiRegionIt->startSeconds - 1.25) < 0.000001);
    assert(std::abs(midiRegionIt->durationSeconds - 3.5) < 0.000001);
    assert(neuracoust::daw::setMidiRegionName(trackEditProject, midiRegionId, "  Verse Keys A  "));
    assert(neuracoust::daw::setMidiRegionColor(trackEditProject, midiRegionId, "#35BFA8"));
    assert(neuracoust::daw::setMidiRegionMuted(trackEditProject, midiRegionId, true));
    assert(neuracoust::daw::setMidiRegionMuted(trackEditProject, midiRegionId, false));
    std::string duplicatedMidiRegionId;
    assert(neuracoust::daw::duplicateMidiRegion(trackEditProject, midiRegionId, 4.75, duplicatedMidiRegionId));
    assert(duplicatedMidiRegionId == "midi-region-3");
    auto duplicatedMidiRegionIt = std::find_if(trackEditProject.midiRegions.begin(), trackEditProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == duplicatedMidiRegionId;
    });
    assert(duplicatedMidiRegionIt != trackEditProject.midiRegions.end());
    assert(duplicatedMidiRegionIt->name == "Verse Keys A Copy");
    assert(duplicatedMidiRegionIt->notes.size() == 3);
    assert(neuracoust::daw::deleteMidiRegion(trackEditProject, duplicatedMidiRegionId));
    assert(neuracoust::daw::moveMidiNote(trackEditProject, midiRegionId, midiNoteB, 65, 1.03));
    assert(neuracoust::daw::resizeMidiNote(trackEditProject, midiRegionId, midiNoteB, 0.75));
    assert(neuracoust::daw::setMidiNoteVelocity(trackEditProject, midiRegionId, midiNoteB, 140));
    std::vector<std::string> changedMidiNoteIds;
    assert(neuracoust::daw::moveMidiNotes(trackEditProject, midiRegionId, {midiNoteB, midiNoteC}, 2, 0.25, changedMidiNoteIds));
    assert(changedMidiNoteIds.size() == 2);
    assert(neuracoust::daw::resizeMidiNotes(trackEditProject, midiRegionId, {midiNoteB, midiNoteC}, 0.25, changedMidiNoteIds));
    assert(changedMidiNoteIds.size() == 2);
    assert(neuracoust::daw::adjustMidiNoteVelocities(trackEditProject, midiRegionId, {midiNoteB, midiNoteC}, -10, changedMidiNoteIds));
    assert(changedMidiNoteIds.size() == 2);
    assert(neuracoust::daw::setMidiNoteMuted(trackEditProject, midiRegionId, midiNoteA, true));
    std::vector<std::string> quantizedMidiNoteIds;
    assert(neuracoust::daw::quantizeMidiRegion(trackEditProject, midiRegionId, 0.25, quantizedMidiNoteIds));
    assert(quantizedMidiNoteIds.size() == 2);
    auto midiJson = neuracoust::daw::serializeProject(trackEditProject);
    assert(midiJson.find("\"midiRegions\"") != std::string::npos);
    assert(midiJson.find("\"name\":\"Verse Keys A\"") != std::string::npos);
    assert(midiJson.find("\"velocity\":117") != std::string::npos);
    assert(midiJson.find("\"controllerEvents\"") != std::string::npos);
    assert(midiJson.find("\"pitchBendEvents\"") != std::string::npos);
    assert(midiJson.find("\"programChangeEvents\"") != std::string::npos);
    neuracoust::daw::ProjectDocument midiRoundTrip;
    assert(neuracoust::daw::deserializeProject(midiJson, midiRoundTrip, projectParseError));
    assert(midiRoundTrip.midiRegions.size() == 2);
    const auto roundTripInstrumentTrack = std::find_if(midiRoundTrip.tracks.begin(), midiRoundTrip.tracks.end(), [&](const neuracoust::daw::TrackState& track) {
        return track.name == instrumentTrackName;
    });
    assert(roundTripInstrumentTrack != midiRoundTrip.tracks.end());
    assert(roundTripInstrumentTrack->instrument.pluginName == "Round Trip Synth");
    assert(roundTripInstrumentTrack->instrument.pluginFormat == "VST3");
    assert(roundTripInstrumentTrack->instrument.enabled);
    assert(roundTripInstrumentTrack->instrument.midiChannel == 10);
    assert(roundTripInstrumentTrack->instrumentSlots.size() == 2);
    assert(roundTripInstrumentTrack->instrumentSlots[0].pluginName == "Round Trip Synth");
    assert(roundTripInstrumentTrack->instrumentSlots[1].pluginName == "Layer Synth");
    assert(roundTripInstrumentTrack->instrumentSlots[1].midiChannel == 12);
    midiRoundTrip.sampleRate = 1000.0;
    midiRoundTrip.tempoBpm = 120;
    neuracoust::daw::ProjectAudioRenderPlan midiPlan;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(midiRoundTrip, midiPlan, projectParseError));
    assert(midiPlan.midiRegions.size() == 2);
    const auto instrumentEvents = neuracoust::daw::collectMidiEventsForRenderBlock(midiPlan, instrumentTrackName, 0, 1500);
    assert(instrumentEvents.size() == 5);
    assert(instrumentEvents[0].noteOn);
    assert(instrumentEvents[0].frameOffset == 250);
    assert(instrumentEvents[0].pitch == 72);
    assert(instrumentEvents[0].velocity == 100);
    assert(instrumentEvents[1].kind == neuracoust::daw::ProjectRenderMidiEventKind::ProgramChange);
    assert(instrumentEvents[1].frameOffset == 375);
    assert(instrumentEvents[1].program == 9);
    assert(instrumentEvents[2].kind == neuracoust::daw::ProjectRenderMidiEventKind::Controller);
    assert(instrumentEvents[2].frameOffset == 438);
    assert(instrumentEvents[2].controller == 64);
    assert(instrumentEvents[2].value == 64);
    assert(instrumentEvents[3].kind == neuracoust::daw::ProjectRenderMidiEventKind::PitchBend);
    assert(instrumentEvents[3].frameOffset == 688);
    assert(instrumentEvents[3].value == 4096);
    assert(!instrumentEvents[4].noteOn);
    assert(instrumentEvents[4].frameOffset == 750);
    auto variableTempoMidiProject = neuracoust::daw::defaultProject();
    variableTempoMidiProject.sampleRate = 1000.0;
    variableTempoMidiProject.tempoBpm = 120;
    variableTempoMidiProject.tempoMap = {{0.0, 120.0}, {1.0, 60.0}};
    const auto variableTempoInstrument = neuracoust::daw::addInstrumentTrack(variableTempoMidiProject);
    assert(!variableTempoInstrument.empty());
    const auto variableTempoRegion = neuracoust::daw::addMidiRegion(variableTempoMidiProject, variableTempoInstrument, 0.0, 4.0, "Variable Tempo Lead");
    assert(!variableTempoRegion.empty());
    assert(neuracoust::daw::addMidiControllerEvent(variableTempoMidiProject, variableTempoRegion, 2.0, 1, 96).size());
    assert(neuracoust::daw::addMidiNote(variableTempoMidiProject, variableTempoRegion, 60, 3.0, 1.0, 100).size());
    neuracoust::daw::ProjectAudioRenderPlan variableTempoMidiPlan;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(variableTempoMidiProject, variableTempoMidiPlan, projectParseError));
    const auto variableTempoMidiEvents = neuracoust::daw::collectMidiEventsForRenderBlock(variableTempoMidiPlan, variableTempoInstrument, 0, 4000);
    assert(variableTempoMidiEvents.size() == 3);
    assert(variableTempoMidiEvents[0].kind == neuracoust::daw::ProjectRenderMidiEventKind::Controller);
    assert(variableTempoMidiEvents[0].frameOffset == 1500);
    assert(variableTempoMidiEvents[1].noteOn);
    assert(variableTempoMidiEvents[1].frameOffset == 2500);
    assert(!variableTempoMidiEvents[2].noteOn);
    assert(variableTempoMidiEvents[2].frameOffset == 3500);
    auto midiTransformProject = neuracoust::daw::defaultProject();
    const auto transformTrack = neuracoust::daw::addMidiTrack(midiTransformProject);
    const auto transformRegion = neuracoust::daw::addMidiRegion(midiTransformProject, transformTrack, 0.0, 2.0, "Transform");
    const auto transformNoteA = neuracoust::daw::addMidiNote(midiTransformProject, transformRegion, 60, 0.5, 0.5, 90);
    const auto transformNoteB = neuracoust::daw::addMidiNote(midiTransformProject, transformRegion, 64, 1.0, 0.5, 100);
    assert(!transformNoteA.empty());
    assert(!transformNoteB.empty());
    std::vector<std::string> transformedNoteIds;
    assert(neuracoust::daw::setMidiRegionLoopEnabled(midiTransformProject, transformRegion, true));
    assert(neuracoust::daw::transposeMidiRegion(midiTransformProject, transformRegion, 12, transformedNoteIds));
    assert(transformedNoteIds.size() == 2);
    assert(neuracoust::daw::humanizeMidiRegion(midiTransformProject, transformRegion, 0.02, 6, 42, transformedNoteIds));
    assert(transformedNoteIds.size() == 2);
    const auto transformRegionIt = std::find_if(midiTransformProject.midiRegions.begin(), midiTransformProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == transformRegion;
    });
    assert(transformRegionIt != midiTransformProject.midiRegions.end());
    assert(transformRegionIt->loopEnabled);
    assert(transformRegionIt->notes.size() == 2);
    assert(transformRegionIt->notes[0].pitch >= 72 && transformRegionIt->notes[1].pitch >= 76);
    assert(transformRegionIt->notes[0].startBeats >= 0.0);
    assert(transformRegionIt->notes[0].velocity >= 84 && transformRegionIt->notes[0].velocity <= 96);
    auto midiRecordProject = neuracoust::daw::defaultProject();
    const auto midiRecordTrack = neuracoust::daw::addInstrumentTrack(midiRecordProject);
    std::vector<neuracoust::daw::RecordedMidiEvent> recordedMidiEvents;
    recordedMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::ProgramChange, 0.0, 1, 60, 0, 0, 0, 4});
    recordedMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::Controller, 0.05, 1, 60, 0, 1, 96, 0});
    recordedMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::NoteOn, 0.10, 1, 60, 100, 0, 0, 0});
    recordedMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::PitchBend, 0.20, 1, 60, 0, 0, 12288, 0});
    recordedMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::NoteOff, 0.60, 1, 60, 0, 0, 0, 0});
    std::string recordedMidiRegionId;
    std::string recordedMidiMessage;
    assert(neuracoust::daw::appendRecordedMidiTakeRegion(midiRecordProject,
                                                         midiRecordTrack,
                                                         recordedMidiEvents,
                                                         1.0,
                                                         0.75,
                                                         recordedMidiRegionId,
                                                         recordedMidiMessage));
    assert(recordedMidiMessage == "MIDI recording appended.");
    const auto recordedMidiRegionIt = std::find_if(midiRecordProject.midiRegions.begin(), midiRecordProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == recordedMidiRegionId;
    });
    assert(recordedMidiRegionIt != midiRecordProject.midiRegions.end());
    assert(recordedMidiRegionIt->trackName == midiRecordTrack);
    assert(recordedMidiRegionIt->notes.size() == 1);
    assert(recordedMidiRegionIt->notes.front().pitch == 60);
    assert(std::abs(recordedMidiRegionIt->notes.front().startBeats - 0.2) < 0.000001);
    assert(std::abs(recordedMidiRegionIt->notes.front().durationBeats - 1.0) < 0.000001);
    assert(recordedMidiRegionIt->controllerEvents.size() == 1);
    assert(recordedMidiRegionIt->controllerEvents.front().controller == 1);
    assert(recordedMidiRegionIt->controllerEvents.front().value == 96);
    assert(recordedMidiRegionIt->pitchBendEvents.size() == 1);
    assert(recordedMidiRegionIt->pitchBendEvents.front().value == 12288);
    assert(recordedMidiRegionIt->programChangeEvents.size() == 1);
    assert(recordedMidiRegionIt->programChangeEvents.front().program == 4);
    std::vector<neuracoust::daw::RecordedMidiEvent> overdubMidiEvents;
    overdubMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::NoteOn, 0.0, 1, 64, 90, 0, 0, 0});
    overdubMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::NoteOff, 0.25, 1, 64, 0, 0, 0, 0});
    std::string overdubRegionId;
    std::string overdubMessage;
    assert(neuracoust::daw::appendRecordedMidiTakeRegion(midiRecordProject,
                                                         midiRecordTrack,
                                                         overdubMidiEvents,
                                                         1.5,
                                                         0.3,
                                                         overdubRegionId,
                                                         overdubMessage,
                                                         "merge"));
    assert(overdubRegionId == recordedMidiRegionId);
    assert(overdubMessage == "MIDI recording merged.");
    auto overdubRegionIt = std::find_if(midiRecordProject.midiRegions.begin(), midiRecordProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == recordedMidiRegionId;
    });
    assert(overdubRegionIt != midiRecordProject.midiRegions.end());
    assert(overdubRegionIt->notes.size() == 2);
    std::vector<neuracoust::daw::RecordedMidiEvent> replaceMidiEvents;
    replaceMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::NoteOn, 0.0, 1, 67, 88, 0, 0, 0});
    replaceMidiEvents.push_back({neuracoust::daw::RecordedMidiEventKind::NoteOff, 0.20, 1, 67, 0, 0, 0, 0});
    std::string replaceRegionId;
    std::string replaceMessage;
    assert(neuracoust::daw::appendRecordedMidiTakeRegion(midiRecordProject,
                                                         midiRecordTrack,
                                                         replaceMidiEvents,
                                                         1.0,
                                                         0.25,
                                                         replaceRegionId,
                                                         replaceMessage,
                                                         "replace"));
    assert(replaceRegionId == recordedMidiRegionId);
    assert(replaceMessage == "MIDI recording replaced.");
    auto replaceRegionIt = std::find_if(midiRecordProject.midiRegions.begin(), midiRecordProject.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == recordedMidiRegionId;
    });
    assert(replaceRegionIt != midiRecordProject.midiRegions.end());
    assert(std::any_of(replaceRegionIt->notes.begin(), replaceRegionIt->notes.end(), [](const neuracoust::daw::MidiNoteState& note) {
        return note.pitch == 67;
    }));
    neuracoust::daw::WavAudioData missingInstrumentOutput;
    missingInstrumentOutput.channels = 2;
    missingInstrumentOutput.sampleRate = 48000;
    missingInstrumentOutput.interleavedSamples.assign(256 * 2, 0.0f);
    const auto missingInstrumentDescriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert("Missing Synth", "/tmp/DefinitelyMissingSynth.vst3");
    const auto missingInstrumentProcess = neuracoust::daw::processMidiInstrumentWithVst3(
        missingInstrumentDescriptor,
        {neuracoust::daw::Vst3MidiEvent{0, 60, 100, 1, true},
         neuracoust::daw::Vst3MidiEvent{120, 60, 0, 1, false}},
        missingInstrumentOutput,
        128);
    assert(!missingInstrumentProcess.processed);
    assert(!missingInstrumentProcess.message.empty());
    std::vector<float> missingInstrumentRender;
    neuracoust::daw::renderProjectAudioBlock(midiPlan, 0, 256, missingInstrumentRender);
    assert(missingInstrumentRender.size() == 512);
    const auto midiRegionRoundTrip = std::find_if(midiRoundTrip.midiRegions.begin(), midiRoundTrip.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == midiRegionId;
    });
    assert(midiRegionRoundTrip != midiRoundTrip.midiRegions.end());
    assert(midiRegionRoundTrip->name == "Verse Keys A");
    assert(midiRegionRoundTrip->colorHex == "#35BFA8");
    assert(std::abs(midiRegionRoundTrip->startSeconds - 1.25) < 0.000001);
    assert(std::abs(midiRegionRoundTrip->durationSeconds - 3.5) < 0.000001);
    assert(midiRegionRoundTrip->notes.size() == 3);
    assert(midiRegionRoundTrip->notes[0].id == midiNoteA);
    assert(midiRegionRoundTrip->notes[0].muted);
    assert(std::abs(midiRegionRoundTrip->notes[0].startBeats - 0.0) < 0.000001);
    assert(midiRegionRoundTrip->notes[1].id == midiNoteB);
    assert(midiRegionRoundTrip->notes[1].pitch == 67);
    assert(midiRegionRoundTrip->notes[1].velocity == 117);
    assert(midiRegionRoundTrip->notes[1].channel == 2);
    assert(std::abs(midiRegionRoundTrip->notes[1].durationBeats - 1.0) < 0.000001);
    assert(std::abs(midiRegionRoundTrip->notes[1].startBeats - 1.25) < 0.000001);
    assert(midiRegionRoundTrip->notes[2].id == midiNoteC);
    assert(midiRegionRoundTrip->notes[2].pitch == 69);
    assert(midiRegionRoundTrip->notes[2].velocity == 70);
    assert(std::abs(midiRegionRoundTrip->notes[2].durationBeats - 0.5) < 0.000001);
    assert(std::abs(midiRegionRoundTrip->notes[2].startBeats - 1.25) < 0.000001);
    auto instrumentRegionRoundTrip = std::find_if(midiRoundTrip.midiRegions.begin(), midiRoundTrip.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == instrumentRegionId;
    });
    assert(instrumentRegionRoundTrip != midiRoundTrip.midiRegions.end());
    assert(instrumentRegionRoundTrip->controllerEvents.size() == 1);
    assert(instrumentRegionRoundTrip->controllerEvents[0].id == instrumentSustainId);
    assert(instrumentRegionRoundTrip->controllerEvents[0].controller == 64);
    assert(instrumentRegionRoundTrip->controllerEvents[0].value == 64);
    assert(std::abs(instrumentRegionRoundTrip->controllerEvents[0].beat - 0.375) < 0.000001);
    assert(instrumentRegionRoundTrip->pitchBendEvents.size() == 1);
    assert(instrumentRegionRoundTrip->pitchBendEvents[0].id == instrumentBendId);
    assert(instrumentRegionRoundTrip->pitchBendEvents[0].value == 4096);
    assert(std::abs(instrumentRegionRoundTrip->pitchBendEvents[0].beat - 0.875) < 0.000001);
    assert(instrumentRegionRoundTrip->programChangeEvents.size() == 1);
    assert(instrumentRegionRoundTrip->programChangeEvents[0].id == instrumentProgramId);
    assert(instrumentRegionRoundTrip->programChangeEvents[0].program == 9);
    assert(std::abs(instrumentRegionRoundTrip->programChangeEvents[0].beat - 0.25) < 0.000001);
    assert(neuracoust::daw::deleteMidiControllerEvent(midiRoundTrip, instrumentRegionId, instrumentSustainId));
    assert(neuracoust::daw::deleteMidiPitchBendEvent(midiRoundTrip, instrumentRegionId, instrumentBendId));
    assert(neuracoust::daw::deleteMidiProgramChangeEvent(midiRoundTrip, instrumentRegionId, instrumentProgramId));
    auto instrumentRegionAfterEventDelete = std::find_if(midiRoundTrip.midiRegions.begin(), midiRoundTrip.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == instrumentRegionId;
    });
    assert(instrumentRegionAfterEventDelete != midiRoundTrip.midiRegions.end());
    assert(instrumentRegionAfterEventDelete->controllerEvents.empty());
    assert(instrumentRegionAfterEventDelete->pitchBendEvents.empty());
    assert(instrumentRegionAfterEventDelete->programChangeEvents.empty());
    assert(!neuracoust::daw::addMidiRegion(midiRoundTrip, "Master", 0.0, 1.0, "Bad").size());
    assert(!neuracoust::daw::addMidiNote(midiRoundTrip, midiRegionId, 128, 0.0, 1.0, 90).size());
    assert(!neuracoust::daw::resizeMidiNote(midiRoundTrip, midiRegionId, midiNoteA, 0.0));
    std::string splitMidiRegionId;
    assert(neuracoust::daw::splitMidiRegion(midiRoundTrip, midiRegionId, 2.25, splitMidiRegionId));
    assert(splitMidiRegionId == "midi-region-3");
    auto leftSplitRegion = std::find_if(midiRoundTrip.midiRegions.begin(), midiRoundTrip.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == midiRegionId;
    });
    auto rightSplitRegion = std::find_if(midiRoundTrip.midiRegions.begin(), midiRoundTrip.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == splitMidiRegionId;
    });
    assert(leftSplitRegion != midiRoundTrip.midiRegions.end());
    assert(rightSplitRegion != midiRoundTrip.midiRegions.end());
    assert(std::abs(leftSplitRegion->durationSeconds - 1.0) < 0.000001);
    assert(std::abs(rightSplitRegion->startSeconds - 2.25) < 0.000001);
    assert(std::abs(rightSplitRegion->durationSeconds - 2.5) < 0.000001);
    assert(rightSplitRegion->name == "Verse Keys A Split");
    assert(neuracoust::daw::setMidiRegionLocked(midiRoundTrip, splitMidiRegionId, true));
    assert(!neuracoust::daw::moveMidiRegion(midiRoundTrip, splitMidiRegionId, midiTrackName, 3.0));
    assert(neuracoust::daw::setMidiRegionLocked(midiRoundTrip, splitMidiRegionId, false));
    assert(neuracoust::daw::deleteMidiRegion(midiRoundTrip, splitMidiRegionId));
    std::vector<std::string> deletedMidiNoteIds;
    assert(neuracoust::daw::deleteMidiNotes(midiRoundTrip, midiRegionId, {midiNoteA, midiNoteC}, deletedMidiNoteIds));
    assert(deletedMidiNoteIds.size() == 2);
    auto midiRegionAfterDelete = std::find_if(midiRoundTrip.midiRegions.begin(), midiRoundTrip.midiRegions.end(), [&](const neuracoust::daw::MidiRegionState& region) {
        return region.id == midiRegionId;
    });
    assert(midiRegionAfterDelete != midiRoundTrip.midiRegions.end());
    assert(midiRegionAfterDelete->notes.size() == 1);
    assert(neuracoust::daw::deleteMidiRegion(midiRoundTrip, midiRegionId));
    assert(midiRoundTrip.midiRegions.size() == 1);
    assert(midiRoundTrip.midiRegions.front().id == instrumentRegionId);
    const std::string duplicateMarkerIdProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Duplicate Markers",
  "tracks": [
    {"name":"Audio 1","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":false}
  ],
  "clips": [],
  "markers": [
    {"id":"mk","name":"Intro","timeSeconds":2},
    {"id":"mk","name":"","timeSeconds":1},
    {"id":"mk-2","name":"Verse","timeSeconds":3}
  ],
  "masterInserts": [],
  "monitorModules": []
})json";
    neuracoust::daw::ProjectDocument duplicateMarkerIdProject;
    assert(neuracoust::daw::deserializeProject(duplicateMarkerIdProjectJson, duplicateMarkerIdProject, projectParseError));
    assert(duplicateMarkerIdProject.markers.size() == 3);
    assert(duplicateMarkerIdProject.markers[0].id == "mk-2");
    assert(duplicateMarkerIdProject.markers[0].name == "mk-2");
    assert(duplicateMarkerIdProject.markers[0].timeSeconds == 1.0);
    assert(duplicateMarkerIdProject.markers[1].id == "mk");
    assert(duplicateMarkerIdProject.markers[2].id == "mk-2-2");
    const std::string repairedProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Needs Repair",
  "sampleRate": 48000,
  "defaultBufferSize": 256,
  "tempoBpm": 120,
  "beatSnapEnabled": false,
  "loopEnabled": false,
  "loopStartSeconds": 0,
  "loopEndSeconds": 4,
  "tracks": [
    {"name":"Audio 1","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Audio 2","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true}
  ],
  "clips": [
    {"id":"missing-track","trackName":"Deleted Track","sourcePath":"/tmp/missing.wav","startSeconds":0,"durationSeconds":1},
    {"id":"protected-track","trackName":"Master","sourcePath":"/tmp/master.wav","startSeconds":1,"durationSeconds":1}
  ],
  "markers": [],
  "masterInserts": [],
  "monitorModules": []
})json";
    neuracoust::daw::ProjectDocument repairedProject;
    assert(neuracoust::daw::deserializeProject(repairedProjectJson, repairedProject, projectParseError));
    assert(repairedProject.tracks[0].recordArmed);
    assert(repairedProject.tracks[1].recordArmed);
    assert(!repairedProject.tracks[2].recordArmed);
    assert(repairedProject.clips.size() == 2);
    assert(repairedProject.clips[0].trackName == "Audio 1");
    assert(repairedProject.clips[1].trackName == "Audio 1");
    const std::string duplicateTrackNameProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Duplicate Tracks",
  "tracks": [
    {"name":"  Audio 1  ","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Audio 1","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true}
  ],
  "clips": [
    {"id":"trimmed-track","trackName":"  Audio 1  ","sourcePath":"/tmp/a.wav","startSeconds":0,"durationSeconds":1},
    {"id":"suffix-track","trackName":"Audio 1 2","sourcePath":"/tmp/b.wav","startSeconds":1,"durationSeconds":1}
  ],
  "markers": [],
  "masterInserts": [],
  "monitorModules": []
})json";
    neuracoust::daw::ProjectDocument duplicateTrackNameProject;
    assert(neuracoust::daw::deserializeProject(duplicateTrackNameProjectJson, duplicateTrackNameProject, projectParseError));
    assert(duplicateTrackNameProject.tracks.size() == 4);
    assert(duplicateTrackNameProject.tracks[0].name == "Audio 1");
    assert(duplicateTrackNameProject.tracks[1].name == "Audio 1 2");
    assert(duplicateTrackNameProject.tracks[2].name == "Master");
    assert(duplicateTrackNameProject.tracks[3].name == "Monitor");
    assert(duplicateTrackNameProject.tracks[0].recordArmed);
    assert(duplicateTrackNameProject.tracks[1].recordArmed);
    assert(!duplicateTrackNameProject.tracks[2].recordArmed);
    assert(!duplicateTrackNameProject.tracks[3].recordArmed);
    assert(duplicateTrackNameProject.clips[0].trackName == "Audio 1");
    assert(duplicateTrackNameProject.clips[1].trackName == "Audio 1 2");
    const std::string duplicateClipIdProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Duplicate Clips",
  "tracks": [
    {"name":"Audio 1","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":false}
  ],
  "clips": [
    {"id":"dup","trackName":"Audio 1","sourcePath":"/tmp/a.wav","startSeconds":0,"durationSeconds":1},
    {"id":"dup","trackName":"Audio 1","sourcePath":"/tmp/ignored.wav","startSeconds":1,"durationSeconds":-1},
    {"id":"dup","trackName":"Audio 1","sourcePath":"/tmp/b.wav","startSeconds":2,"durationSeconds":1},
    {"id":"dup-2","trackName":"Audio 1","sourcePath":"/tmp/c.wav","startSeconds":3,"durationSeconds":1}
  ],
  "markers": [],
  "masterInserts": [],
  "monitorModules": []
})json";
    neuracoust::daw::ProjectDocument duplicateClipIdProject;
    assert(neuracoust::daw::deserializeProject(duplicateClipIdProjectJson, duplicateClipIdProject, projectParseError));
    assert(duplicateClipIdProject.clips.size() == 3);
    assert(duplicateClipIdProject.clips[0].id == "dup");
    assert(duplicateClipIdProject.clips[1].id == "dup-2");
    assert(duplicateClipIdProject.clips[2].id == "dup-2-2");
    const std::string protectedOnlyProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Protected Only",
  "tracks": [
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":false,"recordArmed":true}
  ],
  "clips": [
    {"id":"clip-1","trackName":"Master","sourcePath":"/tmp/master.wav","startSeconds":0,"durationSeconds":1}
  ],
  "masterInserts": [],
  "monitorModules": []
})json";
    neuracoust::daw::ProjectDocument protectedOnlyProject;
    assert(neuracoust::daw::deserializeProject(protectedOnlyProjectJson, protectedOnlyProject, projectParseError));
    assert(protectedOnlyProject.tracks.front().name == "Audio 1");
    assert(!protectedOnlyProject.clips.empty());
    assert(protectedOnlyProject.clips.front().trackName == "Audio 1");
    const std::string sanitizedProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Sanitize Me",
  "sampleRate": nan,
  "defaultBufferSize": -32,
  "bitDepth": 20,
  "tempoBpm": 9999,
  "timelineZoomFactor": 99,
  "timelineFollowMode": 99,
  "trackHeightScale": 99,
  "waveformGainScale": -4,
  "loopEnabled": true,
  "loopStartSeconds": inf,
  "loopEndSeconds": -2,
  "windowsProcessorAffinityEnabled": true,
  "windowsProcessorAffinityMode": "realtime_everything",
  "tracks": [
    {"name":"Audio 1","volumeDb":999,"pan":-9,"displayHeightScale":99,"muted":false,"solo":false,"recordArmed":true},
    {"name":"Master","volumeDb":0,"pan":0,"muted":false,"solo":true,"recordArmed":true}
  ],
  "clips": [
    {"id":"bad-duration","trackName":"Audio 1","sourcePath":"/tmp/bad.wav","startSeconds":0,"durationSeconds":-1,"sourceOffsetSeconds":0},
    {"id":"fixed-clip","trackName":"Audio 1","sourcePath":"/tmp/fixed.wav","startSeconds":-5,"durationSeconds":2,"sourceOffsetSeconds":-3,"gainDb":99,"fadeInSeconds":9,"fadeOutSeconds":9}
  ],
  "markers": [
    {"id":"late-marker","name":"Late","timeSeconds":999999999},
    {"id":"bad-marker","name":"Bad","timeSeconds":nan}
  ],
  "masterInserts": [],
  "monitorModules": []
})json";
    neuracoust::daw::ProjectDocument sanitizedProject;
    assert(neuracoust::daw::deserializeProject(sanitizedProjectJson, sanitizedProject, projectParseError));
    assert(sanitizedProject.sampleRate == 48000.0);
    assert(sanitizedProject.defaultBufferSize == 16);
    assert(sanitizedProject.bitDepth == 24);
    assert(sanitizedProject.tempoBpm == 400);
    assert(sanitizedProject.timelineZoomFactor == 16.0);
    assert(sanitizedProject.timelineFollowMode == 2);
    assert(sanitizedProject.trackHeightScale == 4.0);
    assert(sanitizedProject.waveformGainScale == 0.25);
    assert(!sanitizedProject.loopEnabled);
    assert(sanitizedProject.windowsProcessorAffinityEnabled);
    assert(sanitizedProject.windowsProcessorAffinityMode == "p_core_preferred");
    assert(sanitizedProject.tracks[0].volumeDb == 12.0f);
    assert(sanitizedProject.tracks[0].pan == -1.0f);
    assert(sanitizedProject.tracks[0].displayHeightScale == 4.0);
    assert(!sanitizedProject.tracks[1].recordArmed);
    assert(sanitizedProject.clips.size() == 1);
    assert(sanitizedProject.clips.front().id == "fixed-clip");
    assert(sanitizedProject.clips.front().startSeconds == 0.0);
    assert(sanitizedProject.clips.front().sourceOffsetSeconds == 0.0);
    assert(sanitizedProject.clips.front().gainDb == 24.0f);

    const std::string lowTrackVolumeProjectJson = R"json({
  "format": "neuracoust-daw-project-v1",
  "name": "Low Volume Clamp",
  "version": "260701.0000",
  "sampleRate": 48000,
  "tracks": [
    {"name":"Audio 1","volumeDb":-999,"pan":0,"muted":false,"solo":false,"recordArmed":false},
    {"name":"Master","volumeDb":-999,"pan":0,"muted":false,"solo":false,"recordArmed":false}
  ],
  "clips": []
})json";
    neuracoust::daw::ProjectDocument lowTrackVolumeProject;
    assert(neuracoust::daw::deserializeProject(lowTrackVolumeProjectJson, lowTrackVolumeProject, projectParseError));
    assert(lowTrackVolumeProject.tracks[0].volumeDb == -120.0f);
    assert(lowTrackVolumeProject.tracks[1].volumeDb == -120.0f);
    assert(sanitizedProject.clips.front().fadeInSeconds == 1.0);
    assert(sanitizedProject.clips.front().fadeOutSeconds == 1.0);
    assert(sanitizedProject.markers.size() == 2);
    assert(sanitizedProject.markers.front().id == "bad-marker");
    assert(sanitizedProject.markers.front().timeSeconds == 0.0);
    assert(sanitizedProject.markers.back().timeSeconds == 24.0 * 60.0 * 60.0);
    auto snapProject = neuracoust::daw::defaultProject();
    snapProject.editMode = "Slip";   // defaultProject() ships Grid; exercise the plain 0.1 s quantum first
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - 0.1) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.24) - 1.2) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.25) - 1.3) < 0.000001);
    assert(neuracoust::daw::snapProjectTime(snapProject, -1.0) == 0.0);
    assert(neuracoust::daw::snapProjectTime(snapProject, std::numeric_limits<double>::quiet_NaN()) == 0.0);
    snapProject.beatSnapEnabled = true;
    snapProject.tempoBpm = 120;
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - 0.5) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.24) - 1.0) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.25) - 1.5) < 0.000001);
    snapProject.beatSnapEnabled = false;
    snapProject.editMode = "Grid";
    snapProject.gridUnit = "1 beat";
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - 0.5) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.24) - 1.0) < 0.000001);
    snapProject.gridUnit = "1 bar";
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - 2.0) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 3.1) - 4.0) < 0.000001);
    snapProject.gridUnit = "1/8 beat";
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - 0.0625) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.155) - 1.125) < 0.000001);
    snapProject.gridUnit = "1/16 beat";
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - 0.03125) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.155) - 1.15625) < 0.000001);
    snapProject.gridUnit = "1 frame";
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - (1.0 / 30.0)) < 0.000001);
    snapProject.videoFrameRate = 23.976;
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - (1.0 / 23.976)) < 0.000001);
    assert(std::abs(neuracoust::daw::snapProjectTime(snapProject, 1.0) - (std::round(1.0 * 23.976) / 23.976)) < 0.000001);
    snapProject.videoFrameRate = 24.0;
    snapProject.timecodeStartSeconds = 3600.0;
    assert(neuracoust::daw::projectTimecodeString(snapProject, 0.0) == "01:00:00:00");
    assert(neuracoust::daw::projectTimecodeString(snapProject, 1.5) == "01:00:01:12");
    snapProject.videoFrameRate = 29.97;
    snapProject.timecodeStartSeconds = 0.0;
    snapProject.timecodeDropFrame = true;
    assert(neuracoust::daw::projectTimecodeString(snapProject, 0.0) == "00:00:00;00");
    assert(neuracoust::daw::projectTimecodeString(snapProject, 60.06).find(';') != std::string::npos);
    snapProject.editMode = "Slip";
    assert(std::abs(neuracoust::daw::projectTimelineQuantumSeconds(snapProject) - 0.1) < 0.000001);
    double fullClipRangeStart = 0.0;
    double fullClipRangeEnd = 0.0;
    assert(!neuracoust::daw::projectClipTimeRange(snapProject, fullClipRangeStart, fullClipRangeEnd));
    assert(neuracoust::daw::setEditSelectionRange(snapProject, 2.0, 1.0));
    assert(snapProject.editSelectionEnabled);
    assert(snapProject.editSelectionStartSeconds == 1.0);
    assert(snapProject.editSelectionEndSeconds == 2.0);
    assert(!neuracoust::daw::setEditSelectionRange(snapProject, 1.0, 1.0));
    const auto selectionClipId = neuracoust::daw::appendAudioClipAt(snapProject, "Audio 1", "/tmp/selection.wav", 3.0, 1.5);
    const auto earlierSelectionClipId = neuracoust::daw::appendAudioClipAt(snapProject, "Audio 1", "/tmp/selection-earlier.wav", 1.25, 0.5);
    assert(!earlierSelectionClipId.empty());
    snapProject.clips.push_back({"ignored-zero-duration", "Audio 1", "/tmp/zero.wav", 0.0, 0.0});
    snapProject.clips.push_back({"ignored-invalid-duration", "Audio 1", "/tmp/invalid.wav", 20.0, -1.0});
    assert(neuracoust::daw::projectClipTimeRange(snapProject, fullClipRangeStart, fullClipRangeEnd));
    assert(std::abs(fullClipRangeStart - 1.25) < 0.000001);
    assert(std::abs(fullClipRangeEnd - 4.5) < 0.000001);
    assert(neuracoust::daw::setEditSelectionToClip(snapProject, selectionClipId));
    assert(snapProject.editSelectionStartSeconds == 3.0);
    assert(snapProject.editSelectionEndSeconds == 4.5);
    assert(neuracoust::daw::editSelectionMatchesClip(snapProject, selectionClipId));
    assert(neuracoust::daw::toggleEditSelectionToClip(snapProject, selectionClipId));
    assert(!snapProject.editSelectionEnabled);
    assert(neuracoust::daw::toggleEditSelectionToClip(snapProject, selectionClipId));
    assert(snapProject.editSelectionEnabled);
    assert(snapProject.editSelectionStartSeconds == 3.0);
    assert(snapProject.editSelectionEndSeconds == 4.5);
    assert(neuracoust::daw::setLoopToEditSelection(snapProject));
    assert(snapProject.loopEnabled);
    assert(snapProject.loopStartSeconds == 3.0);
    assert(snapProject.loopEndSeconds == 4.5);
    assert(neuracoust::daw::toggleEditSelectionToClip(snapProject, selectionClipId));
    assert(!snapProject.editSelectionEnabled);
    assert(!snapProject.loopEnabled);
    assert(neuracoust::daw::toggleEditSelectionToClip(snapProject, selectionClipId));
    assert(neuracoust::daw::setLoopRange(snapProject, 8.0, 9.0));
    assert(neuracoust::daw::toggleEditSelectionToClip(snapProject, selectionClipId));
    assert(!snapProject.editSelectionEnabled);
    assert(snapProject.loopEnabled);
    assert(snapProject.loopStartSeconds == 8.0);
    assert(snapProject.loopEndSeconds == 9.0);
    assert(neuracoust::daw::toggleEditSelectionToClip(snapProject, selectionClipId));
    assert(neuracoust::daw::clearLoopRange(snapProject));
    assert(!snapProject.loopEnabled);
    assert(neuracoust::daw::setLoopRange(snapProject, 5.0, 4.0));
    assert(snapProject.loopStartSeconds == 4.0);
    assert(snapProject.loopEndSeconds == 5.0);
    assert(neuracoust::daw::setLoopToClip(snapProject, selectionClipId));
    assert(snapProject.editSelectionStartSeconds == 3.0);
    assert(snapProject.loopStartSeconds == 3.0);
    assert(snapProject.loopEndSeconds == 4.5);
    assert(neuracoust::daw::clearEditSelection(snapProject));
    assert(!snapProject.editSelectionEnabled);

    const auto vst3Capabilities = neuracoust::daw::vst3HostCapabilities();
    assert(vst3Capabilities.bundleDiscovery);
    assert(vst3Capabilities.descriptorValidation);
    assert(vst3Capabilities.sdkRuntimeLoading);
    assert(vst3Capabilities.sdkProcessorProbe);
    assert(vst3Capabilities.sdkAudioProcessing);
    const auto vst3CachePath = testTempRoot() / "NeuracoustDAWCoreSmoke" / "vst3_inventory_v9.tsv";
    std::filesystem::remove_all(vst3CachePath.parent_path());
    std::filesystem::create_directories(vst3CachePath.parent_path());
    const auto legacyVst3CacheV1 = vst3CachePath.parent_path() / "vst3_inventory_v1.tsv";
    const auto legacyVst3CacheV2 = vst3CachePath.parent_path() / "vst3_inventory_v2.tsv";
    const auto legacyVst3CacheV7 = vst3CachePath.parent_path() / "vst3_inventory_v7.tsv";
    const auto legacyVst3CacheV8 = vst3CachePath.parent_path() / "vst3_inventory_v8.tsv";
    {
        std::ofstream legacy(legacyVst3CacheV1);
        legacy << "stale cache";
    }
    {
        std::ofstream legacy(legacyVst3CacheV2);
        legacy << "stale cache";
    }
    {
        std::ofstream legacy(legacyVst3CacheV7);
        legacy << "stale cache";
    }
    {
        std::ofstream legacy(legacyVst3CacheV8);
        legacy << "stale cache";
    }
    setEnvironmentValue("NEURACOUST_DAW_VST3_CACHE_PATH", vst3CachePath.string());
    const auto customVst3Root = vst3CachePath.parent_path() / "CustomVST3";
    const auto customVst3 = customVst3Root / "Neuracoust Custom Scan.vst3";
    std::filesystem::create_directories(customVst3 / "Contents" / "MacOS");
    std::filesystem::create_directories(customVst3 / "Contents" / "x86_64-win");
    {
        std::ofstream plist(customVst3 / "Contents" / "Info.plist");
        plist << "<plist><dict>"
              << "<key>CFBundleName</key><string>Neuracoust Custom Scan</string>"
              << "<key>CFBundleExecutable</key><string>CustomScan</string>"
              << "<key>VSTManufacturer</key><string>Neuracoust</string>"
              << "</dict></plist>";
    }
    {
        std::ofstream executable(customVst3 / "Contents" / "MacOS" / "CustomScan");
        executable << "not a real module";
    }
    {
        std::ofstream executable(customVst3 / "Contents" / "x86_64-win" / "CustomScan.vst3");
        executable << "not a real module";
    }
#if defined(_WIN32)
    const std::string customVst3PathList = customVst3Root.string() + ";" + customVst3Root.string();
#else
    const std::string customVst3PathList = customVst3Root.string() + ":" + customVst3Root.string();
#endif
    setEnvironmentValue("NEURACOUST_DAW_VST3_PATHS", customVst3PathList);
    neuracoust::daw::clearVst3PluginScanCache();
    const auto vst3Plugins = neuracoust::daw::scanVst3PluginBundles(neuracoust::daw::Vst3ScanMode::Refresh);
    assert(std::filesystem::exists(vst3CachePath));
    assert(!std::filesystem::exists(legacyVst3CacheV1));
    assert(!std::filesystem::exists(legacyVst3CacheV2));
    assert(!std::filesystem::exists(legacyVst3CacheV7));
    assert(!std::filesystem::exists(legacyVst3CacheV8));
    {
        std::ifstream cache(vst3CachePath, std::ios::binary);
        std::string header;
        std::getline(cache, header);
        assert(header == "Neuracoust DAW VST3 Inventory v12");
    }
    const auto customVst3Matches = std::count_if(vst3Plugins.begin(), vst3Plugins.end(), [&](const neuracoust::daw::Vst3PluginDescriptor& plugin) {
        return plugin.bundlePath == customVst3.string();
    });
    assert(customVst3Matches == 1);
    neuracoust::daw::clearVst3PluginScanCache();
    const auto cachedVst3Plugins = neuracoust::daw::scanVst3PluginBundles();
    assert(cachedVst3Plugins.size() == vst3Plugins.size());
    std::vector<neuracoust::daw::Vst3PluginDescriptor> unsortedVst3Plugins = {
        {"Zebra Room", "u-he", "Reverb", "/Library/Audio/Plug-Ins/VST3/Zebra Room.vst3", "", "", "u-he", true, 1, 1},
        {"Pro-Q 4", "FabFilter", "EQ / Filter", "/Library/Audio/Plug-Ins/VST3/Pro-Q 4.vst3", "", "", "FabFilter", true, 1, 1},
        {"4001E", "Neuracoust", "Channel Strip", "/Library/Audio/Plug-Ins/VST3/4001E.vst3", "", "", "Neuracoust", true, 1, 1}
    };
    neuracoust::daw::sortVst3PluginDescriptorsForDisplay(unsortedVst3Plugins);
    assert(unsortedVst3Plugins[0].brand == "FabFilter");
    assert(unsortedVst3Plugins[1].brand == "Neuracoust");
    assert(unsortedVst3Plugins[2].brand == "u-he");
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(unsortedVst3Plugins[0], "fabfilter pro q"));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(unsortedVst3Plugins[0], "eq-filter"));
    const neuracoust::daw::Vst3PluginDescriptor proL2Descriptor {
        "FabFilter Pro-L 2",
        "FabFilter",
        "Dynamics",
        "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-L 2.vst3",
        "",
        "",
        "FabFilter",
        true,
        1,
        1
    };
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(proL2Descriptor, "L2"));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(proL2Descriptor, "fab l2"));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(proL2Descriptor, "prol2"));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(proL2Descriptor, "ff 리미터"));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(proL2Descriptor, "페브필터 comp"));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesFilter(unsortedVst3Plugins[1], "neuracoust channel"));
    assert(!neuracoust::daw::vst3PluginDescriptorMatchesFilter(unsortedVst3Plugins[1], "fabfilter"));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesCriteria(unsortedVst3Plugins[0], {
        "pro q",
        "FabFilter",
        "EQ / Filter",
        true
    }));
    assert(!neuracoust::daw::vst3PluginDescriptorMatchesCriteria(unsortedVst3Plugins[0], {
        "pro q",
        "Neuracoust",
        "EQ / Filter",
        true
    }));
    neuracoust::daw::Vst3PluginDescriptor unavailableVst3 = unsortedVst3Plugins[0];
    unavailableVst3.loadableBundle = false;
    assert(!neuracoust::daw::vst3PluginDescriptorMatchesCriteria(unavailableVst3, {
        "pro q",
        "FabFilter",
        "EQ / Filter",
        true
    }));
    assert(neuracoust::daw::vst3PluginDescriptorMatchesCriteria(unavailableVst3, {
        "pro q",
        "FabFilter",
        "EQ / Filter",
        false
    }));
    const std::vector<neuracoust::daw::PluginCandidate> pluginCandidates = {
        {"Core Smoke VST3", "/Library/Audio/Plug-Ins/VST3/Core Smoke.vst3", "VST3", "System", "Neuracoust", "Utility", true, "Core Smoke VST3", false},
        {"Monitor Check AU", "/Users/test/Library/Audio/Plug-Ins/Components/Monitor.component", "Audio Unit", "User", "Apple", "EQ / Filter", true, {}, false}
    };
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[0], "smoke"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[0], "system"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "audio unit"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "audio-unit apple"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "plug ins components"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "components"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "apple"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "filter"));
    assert(!neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "apple mirage"));
    assert(!neuracoust::daw::pluginCandidateMatchesFilter(pluginCandidates[1], "mirage"));
    const neuracoust::daw::PluginCandidate waveShellCandidate {
        "H-Reverb",
        "/Library/Audio/Plug-Ins/Components/WaveShell1.component",
        "Audio Unit",
        "System",
        "Waves",
        "Reverb",
        true,
        "H-Reverb Stereo",
        true
    };
    assert(waveShellCandidate.requiresHostRenderer);
    assert(neuracoust::daw::pluginCandidateMatchesFilter(waveShellCandidate, "waves h reverb stereo"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(waveShellCandidate, "waveshell audio unit"));
    const neuracoust::daw::PluginCandidate fabFilterProL2Candidate {
        "FabFilter Pro-L 2",
        "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-L 2.vst3",
        "VST3",
        "System",
        "FabFilter",
        "Dynamics",
        true,
        "FabFilter Pro-L 2",
        false,
        {},
        "FabFilter Pro-L 2"
    };
    assert(neuracoust::daw::pluginCandidateMatchesFilter(fabFilterProL2Candidate, "L2"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(fabFilterProL2Candidate, "fab l2"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(fabFilterProL2Candidate, "prol2"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(fabFilterProL2Candidate, "ff 리미터"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(fabFilterProL2Candidate, "fabfilter l2"));
    const neuracoust::daw::PluginCandidate mirageCandidate {
        "Neuracoust Mirage 8",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Mirage 8.vst3",
        "VST3",
        "System",
        "Neuracoust",
        "Reverb",
        true,
        "Neuracoust Mirage 8",
        false,
        {},
        "Neuracoust Mirage 8"
    };
    assert(neuracoust::daw::pluginCandidateMatchesFilter(mirageCandidate, "리버브"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(mirageCandidate, "누라쿠스트 공간"));
    const auto describedWavesWetter = neuracoust::daw::describeInstalledPluginCandidate(
        "/Applications/Waves/Plug-Ins V16/OneKnob Wetter.bundle",
        "Audio Unit");
    assert(describedWavesWetter.brand == "Waves");
    assert(describedWavesWetter.category == "Reverb");
    assert(neuracoust::daw::pluginCandidateMatchesFilter(describedWavesWetter, "웨이브스 리버브"));
    const auto describedWavesGtr = neuracoust::daw::describeInstalledPluginCandidate(
        "/Applications/Waves/Plug-Ins V16/GTR Stomp 4.bundle",
        "Audio Unit");
    assert(describedWavesGtr.brand == "Waves");
    assert(describedWavesGtr.category == "Guitar / Amp");
    assert(neuracoust::daw::pluginCandidateMatchesFilter(describedWavesGtr, "guitar pedal"));
    const auto describedWavesKeyDetector = neuracoust::daw::describeInstalledPluginCandidate(
        "/Applications/Waves/Plug-Ins V16/Key Detector.bundle",
        "Audio Unit");
    assert(describedWavesKeyDetector.brand == "Waves");
    assert(describedWavesKeyDetector.category == "Analyzer");
    assert(neuracoust::daw::pluginCandidateMatchesFilter(describedWavesKeyDetector, "meter analysis"));
    const auto filteredPluginCandidates = neuracoust::daw::filterPluginCandidates(pluginCandidates, "user");
    assert(filteredPluginCandidates.size() == 1);
    assert(filteredPluginCandidates.front().name == "Monitor Check AU");
    const auto pluginFilterOptions = neuracoust::daw::pluginCandidateFilterOptions({
        pluginCandidates[0],
        pluginCandidates[1],
        {"Duplicate Apple AU", "/Users/test/Library/Audio/Plug-Ins/Components/Duplicate.component", "Audio Unit", "User", "Apple", "EQ / Filter", true, {}, false},
        {"Missing Metadata", "/tmp/Missing.vst3", "", "", "", "", false, {}, false}
    });
    assert((pluginFilterOptions.brands == std::vector<std::string>{"Apple", "Neuracoust"}));
    assert((pluginFilterOptions.categories == std::vector<std::string>{"EQ / Filter", "Utility"}));
    assert((pluginFilterOptions.formats == std::vector<std::string>{"Audio Unit", "VST3"}));
    assert((pluginFilterOptions.scopes == std::vector<std::string>{"System", "User"}));
    const auto auFilterResults = neuracoust::daw::filterPluginCandidates(pluginCandidates, {
        "monitor",
        "Apple",
        "EQ / Filter",
        "Audio Unit",
        "User",
        true
    });
    assert(auFilterResults.size() == 1);
    assert(auFilterResults.front().name == "Monitor Check AU");
    assert(neuracoust::daw::filterPluginCandidates(pluginCandidates, {
        "monitor",
        "Apple",
        "EQ / Filter",
        "VST3",
        "User",
        true
    }).empty());
    const std::vector<neuracoust::daw::PluginCandidate> missingCandidateInventory = {
        pluginCandidates[0],
        {"Missing Plugin", "/tmp/Missing.vst3", "VST3", "System", "Neuracoust", "Utility", false, "Missing Plugin", false}
    };
    assert(neuracoust::daw::filterPluginCandidates(missingCandidateInventory, {
        "missing",
        "Neuracoust",
        "Utility",
        "VST3",
        "System",
        true
    }).empty());
    const auto includeMissingResults = neuracoust::daw::filterPluginCandidates(missingCandidateInventory, {
        "missing",
        "Neuracoust",
        "Utility",
        "VST3",
        "System",
        false
    });
    assert(includeMissingResults.size() == 1);
    assert(includeMissingResults.front().name == "Missing Plugin");
    std::vector<neuracoust::daw::PluginCandidate> unsortedPluginCandidates = {
        {"RoomVerb", "/Library/Audio/Plug-Ins/VST3/RoomVerb.vst3", "VST3", "System", "Valhalla DSP", "Reverb", true, "RoomVerb", false},
        {"Pro-Q 4", "/Library/Audio/Plug-Ins/Components/Pro-Q 4.component", "Audio Unit", "System", "FabFilter", "EQ / Filter", true, {}, false},
        {"4001E", "/Library/Audio/Plug-Ins/VST3/4001E.vst3", "VST3", "System", "Neuracoust", "Channel Strip", true, "4001E", false}
    };
    neuracoust::daw::sortPluginCandidatesForDisplay(unsortedPluginCandidates);
    assert(unsortedPluginCandidates[0].brand == "FabFilter");
    assert(unsortedPluginCandidates[1].brand == "Neuracoust");
    assert(unsortedPluginCandidates[2].brand == "Valhalla DSP");
    const auto tempComponent = testTempRoot() / "NeuracoustDAWCoreSmoke.component";
    std::filesystem::remove_all(tempComponent);
    std::filesystem::create_directories(tempComponent / "Contents");
    {
        std::ofstream plist(tempComponent / "Contents" / "Info.plist");
        plist << "<plist><dict>"
              << "<key>CFBundleIdentifier</key><string>com.fabfilter.Pro-Q-4</string>"
              << "<key>CFBundleDisplayName</key><string>Pro-Q 4</string>"
              << "<key>CFBundleExecutable</key><string>FabFilter Pro-Q 4</string>"
              << "</dict></plist>";
    }
    const auto describedCandidate = neuracoust::daw::describeInstalledPluginCandidate(tempComponent.string(), "Audio Unit");
    assert(describedCandidate.name == "Pro-Q 4");
    assert(describedCandidate.brand == "FabFilter");
    assert(describedCandidate.category == "EQ / Filter");
    assert(describedCandidate.exists);
    assert(describedCandidate.pluginName.empty());
    assert(!describedCandidate.requiresHostRenderer);
    assert(neuracoust::daw::pluginCandidateMatchesFilter(describedCandidate, "fabfilter"));
    assert(neuracoust::daw::pluginCandidateMatchesFilter(describedCandidate, "audio unit"));
    std::filesystem::remove_all(tempComponent);
    const auto tempVst3 = testTempRoot() / "NeuracoustDAWCoreSmoke.vst3";
    std::filesystem::remove_all(tempVst3);
#if defined(_WIN32)
    std::filesystem::create_directories(tempVst3 / "Contents" / "x86_64-win");
#else
    std::filesystem::create_directories(tempVst3 / "Contents" / "MacOS");
#endif
    std::filesystem::create_directories(tempVst3 / "Contents" / "Resources");
    {
        std::ofstream plist(tempVst3 / "Contents" / "Info.plist");
        plist << "<plist><dict>"
              << "<key>CFBundleName</key><string>Core Smoke VST3</string>"
              << "<key>CFBundleExecutable</key><string>CoreSmokePlugin</string>"
              << "<key>VSTManufacturer</key><string>Neuracoust</string>"
              << "</dict></plist>";
    }
    {
#if defined(_WIN32)
        std::ofstream executable(tempVst3 / "Contents" / "x86_64-win" / "CoreSmokePlugin.vst3");
#else
        std::ofstream executable(tempVst3 / "Contents" / "MacOS" / "CoreSmokePlugin");
#endif
        executable << "not a real module";
    }
    const auto describedVst3 = neuracoust::daw::describeVst3PluginBundle(tempVst3.string());
    assert(describedVst3.name == "Core Smoke VST3");
    assert(describedVst3.vendor == "Neuracoust");
    assert(describedVst3.brand == "Neuracoust");
    assert(describedVst3.category == "Utility");
    assert(describedVst3.bundlePath == tempVst3.string());
    assert(describedVst3.loadableBundle);
    assert(describedVst3.executablePath.find("CoreSmokePlugin") != std::string::npos);
    const auto resolvedDirectVst3 = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
        "Direct Loaded VST3",
        tempVst3.string());
    assert(resolvedDirectVst3.name == "Core Smoke VST3");
    assert(resolvedDirectVst3.bundlePath == tempVst3.string());
    assert(resolvedDirectVst3.executablePath == describedVst3.executablePath);
    assert(resolvedDirectVst3.loadableBundle);
    const auto resolvedExecutableVst3 = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
        "Executable VST3",
        describedVst3.executablePath);
    assert(resolvedExecutableVst3.name == "Executable VST3");
    assert(resolvedExecutableVst3.executablePath == describedVst3.executablePath);
    assert(resolvedExecutableVst3.loadableBundle);
    std::filesystem::remove_all(tempVst3);
    for (const auto& plugin : vst3Plugins) {
        assert(!plugin.name.empty());
        assert(!plugin.brand.empty());
        assert(!plugin.category.empty());
        assert(!plugin.bundlePath.empty());
        assert(plugin.classCount >= 0);
        assert(plugin.audioClassCount >= 0);
    }
    auto withModuleInfo = std::find_if(vst3Plugins.begin(), vst3Plugins.end(), [](const neuracoust::daw::Vst3PluginDescriptor& plugin) {
        return !plugin.moduleInfoPath.empty() && plugin.classCount > 0;
    });
    if (withModuleInfo != vst3Plugins.end()) {
        const auto classes = neuracoust::daw::readVst3ModuleClasses(*withModuleInfo);
        assert(!classes.empty());
        assert(!classes.front().cid.empty());
    }
    if (!vst3Plugins.empty()) {
        neuracoust::daw::Vst3PluginDescriptor invalid = vst3Plugins.front();
        invalid.executablePath = "/definitely/not/a/vst3/module";
        const auto invalidProbe = neuracoust::daw::probeVst3ModuleFactory(invalid);
        assert(!invalidProbe.opened);
        assert(!invalidProbe.hasFactory);
        const auto invalidSdkInspection = neuracoust::daw::inspectVst3FactoryWithSdk(invalid);
#if defined(NEURACOUST_HAS_VST3_SDK)
        assert(invalidSdkInspection.sdkAvailable);
#else
        assert(!invalidSdkInspection.sdkAvailable);
#endif
        assert(!invalidSdkInspection.hasFactory);
        assert(invalidSdkInspection.componentInstanceCount == 0);
        assert(invalidSdkInspection.controllerInstanceCount == 0);
        const auto invalidProcessorProbe = neuracoust::daw::probeVst3ProcessorWithSdk(invalid);
#if defined(NEURACOUST_HAS_VST3_SDK)
        assert(invalidProcessorProbe.sdkAvailable);
#else
        assert(!invalidProcessorProbe.sdkAvailable);
#endif
        assert(!invalidProcessorProbe.opened);
        assert(!invalidProcessorProbe.setupProcessingOk);
        neuracoust::daw::WavAudioData invalidVstAudio;
        invalidVstAudio.channels = 2;
        invalidVstAudio.sampleRate = 48000;
        invalidVstAudio.interleavedSamples.assign(128 * 2, 0.125f);
        const auto invalidProcess = neuracoust::daw::processStereoBufferWithVst3(invalid, invalidVstAudio);
#if defined(NEURACOUST_HAS_VST3_SDK)
        assert(invalidProcess.sdkAvailable);
#else
        assert(!invalidProcess.sdkAvailable);
#endif
        assert(!invalidProcess.processed);
        assert(invalidProcess.framesProcessed == 0);
        neuracoust::daw::Vst3RealtimeProcessor realtimeProcessor;
        std::string realtimePrepareMessage;
        assert(!realtimeProcessor.prepare(invalid, 48000.0, 128, realtimePrepareMessage));
        assert(!realtimeProcessor.isPrepared());
#if defined(NEURACOUST_HAS_VST3_SDK)
        assert(realtimeProcessor.probe().sdkAvailable);
#else
        assert(!realtimeProcessor.probe().sdkAvailable);
#endif
        float realtimeBlock[8] = {0.0f};
        std::string realtimeProcessMessage;
        const auto realtimeProcess = realtimeProcessor.processInterleavedStereo(realtimeBlock, 4, realtimeProcessMessage);
        assert(!realtimeProcess.processed);
        assert(realtimeProcess.framesProcessed == 0);
        realtimeProcessor.reset();
        assert(!realtimeProcessor.isPrepared());
    }

    const auto wavPathFs = testTempRoot() / "neuracoust-daw-smoke.wav";
    const auto wavPath = wavPathFs.string();
    assert(neuracoust::daw::writeTestToneWavFile(wavPathFs, 48000, 0.05, 440.0));
    neuracoust::daw::WavAudioData wav;
    std::string error;
    assert(neuracoust::daw::readPcmWavFile(wavPathFs, wav, error));
    assert(wav.channels == 2);
    assert(wav.sampleRate == 48000);
    assert(wav.frameCount() > 0);
    const auto pcm32Path = testTempRoot() / "neuracoust-daw-pcm32.wav";
    writePcm32TestWav(pcm32Path);
    neuracoust::daw::WavAudioData pcm32;
    assert(neuracoust::daw::readPcmWavFile(pcm32Path, pcm32, error));
    assert(pcm32.channels == 1);
    assert(pcm32.sampleRate == 48000);
    assert(pcm32.frameCount() == 3);
    assert(std::abs(pcm32.interleavedSamples[0]) < 0.000001f);
    assert(std::abs(pcm32.interleavedSamples[1] - 0.5f) < 0.000001f);
    assert(std::abs(pcm32.interleavedSamples[2] + 0.5f) < 0.000001f);
    const auto floatExtensiblePath = testTempRoot() / "neuracoust-daw-float-extensible.wav";
    writeExtensibleFloat32TestWav(floatExtensiblePath);
    neuracoust::daw::WavAudioData floatExtensible;
    assert(neuracoust::daw::readPcmWavFile(floatExtensiblePath, floatExtensible, error));
    assert(floatExtensible.channels == 2);
    assert(floatExtensible.sampleRate == 48000);
    assert(floatExtensible.frameCount() == 2);
    assert(std::abs(floatExtensible.interleavedSamples[0] - 0.25f) < 0.000001f);
    assert(std::abs(floatExtensible.interleavedSamples[1] + 0.25f) < 0.000001f);
    assert(std::abs(floatExtensible.interleavedSamples[2] - 0.5f) < 0.000001f);
    assert(std::abs(floatExtensible.interleavedSamples[3] + 0.5f) < 0.000001f);
    auto dspWav = wav;
    const float firstDry = dspWav.interleavedSamples.size() > 100 ? dspWav.interleavedSamples[100] : 0.0f;
    neuracoust::daw::applyMonitorDspToInterleavedStereo(dspWav, neuracoust::daw::defaultMonitorDspModules());
    const float firstWet = dspWav.interleavedSamples.size() > 100 ? dspWav.interleavedSamples[100] : 0.0f;
    assert(firstDry != firstWet);
    assert(*std::max_element(dspWav.interleavedSamples.begin(), dspWav.interleavedSamples.end()) <= 1.0f);
    assert(*std::min_element(dspWav.interleavedSamples.begin(), dspWav.interleavedSamples.end()) >= -1.0f);
    auto isAudioMonitorModule = [](const std::string& id) {
        return id == "speaker-simulation" ||
               id == "headphone-simulation" ||
               id == "graphic-eq" ||
               id == "room-correction" ||
               id == "crossfeed";
    };
    for (const auto& monitorModule : neuracoust::daw::defaultMonitorDspModules()) {
        if (!isAudioMonitorModule(monitorModule.id)) {
            continue;
        }
        auto soloModules = neuracoust::daw::defaultMonitorDspModules();
        for (auto& module : soloModules) {
            module.enabled = module.id == monitorModule.id;
            if (module.id == "speaker-simulation") {
                module.realModel = "Real Speaker: Avantone CLA-10A (NF)";
                module.targetModelA = "Speaker A: Focal Solo6 (NF)";
            }
        }
        auto soloDspWav = wav;
        neuracoust::daw::applyMonitorDspToInterleavedStereo(soloDspWav, soloModules);
        assert(soloDspWav.interleavedSamples.size() == wav.interleavedSamples.size());
        assert(soloDspWav.interleavedSamples != wav.interleavedSamples);
        assert(*std::max_element(soloDspWav.interleavedSamples.begin(), soloDspWav.interleavedSamples.end()) <= 1.0f);
        assert(*std::min_element(soloDspWav.interleavedSamples.begin(), soloDspWav.interleavedSamples.end()) >= -1.0f);
    }

    const auto renderPath = (testTempRoot() / "neuracoust-daw-render-source.wav").string();
    neuracoust::daw::WavAudioData renderSource;
    renderSource.channels = 1;
    renderSource.sampleRate = 100;
    renderSource.interleavedSamples.assign(20, 0.5f);
    assert(neuracoust::daw::writePcm16WavFile(renderPath, renderSource, error));
    auto renderProject = neuracoust::daw::defaultProject();
    renderProject.sampleRate = 100.0;
    renderProject.tracks[0].pan = -1.0f;
    renderProject.clips.push_back({"render-clip", "Audio 1", renderPath, 0.05, 0.10, 0.0, 0.0f, 0.05, 0.02});
    neuracoust::daw::ProjectAudioRenderPlan renderPlan;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    std::vector<float> renderBlock;
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 20, renderBlock);
    assert(renderBlock.size() == 40);
    assert(renderBlock[8] == 0.0f);
    assert(renderBlock[10] == 0.0f);
    assert(renderBlock[20] > 0.45f);
    assert(renderBlock[14] > renderBlock[10]);
    assert(renderBlock[14] < renderBlock[20]);
    assert(renderBlock[11] == 0.0f);
    const float equalPowerFadeSample = renderBlock[14];
    assert(neuracoust::daw::setClipFadeCurves(renderProject, "render-clip", "linear", "linear"));
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 20, renderBlock);
    const float linearFadeSample = renderBlock[14];
    assert(linearFadeSample > 0.19f && linearFadeSample < 0.21f);
    assert(equalPowerFadeSample > linearFadeSample);
    renderProject.clips.front().gainDb = -6.0f;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 10, 1, renderBlock);
    assert(renderBlock[0] > 0.24f && renderBlock[0] < 0.26f);
    assert(renderBlock[1] == 0.0f);
    renderProject.clips.front().gainDb = 0.0f;
    renderProject.tracks[0].volumeDb = -6.0f;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 10, 1, renderBlock);
    assert(renderBlock[0] > 0.24f && renderBlock[0] < 0.26f);
    assert(renderBlock[1] == 0.0f);
    auto masterMuteProject = renderProject;
    assert(neuracoust::daw::setTrackMuted(masterMuteProject, "Master", true));
    assert(!neuracoust::daw::setTrackMuted(masterMuteProject, "Monitor", true));
    assert(neuracoust::daw::makeProjectAudioRenderPlan(masterMuteProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 10, 1, renderBlock);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    assert(std::abs(renderBlock[1]) < 0.0001f);

    auto vcaRenderProject = neuracoust::daw::defaultProject();
    vcaRenderProject.sampleRate = 100.0;
    vcaRenderProject.clips.push_back({"vca-render-clip", "Audio 1", renderPath, 0.0, 0.20, 0.0, 0.0f});
    neuracoust::daw::TrackState renderVca;
    renderVca.name = "VCA 1";
    renderVca.trackType = "vca";
    renderVca.volumeDb = -6.0f;
    vcaRenderProject.tracks.insert(vcaRenderProject.tracks.end() - 2, renderVca);
    vcaRenderProject.tracks[0].controlMasterTrackName = "VCA 1";
    assert(neuracoust::daw::makeProjectAudioRenderPlan(vcaRenderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(renderBlock[0] > 0.24f && renderBlock[0] < 0.26f);
    assert(renderBlock[1] > 0.24f && renderBlock[1] < 0.26f);
    auto renderVcaTrack = std::find_if(vcaRenderProject.tracks.begin(), vcaRenderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "VCA 1";
    });
    assert(renderVcaTrack != vcaRenderProject.tracks.end());
    renderVcaTrack->muted = true;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(vcaRenderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    assert(std::abs(renderBlock[1]) < 0.0001f);

    auto monitorRouteProject = renderProject;
    monitorRouteProject.monitorInputTrimDb = 0.0f;
    monitorRouteProject.monitorVolumeDb = 0.0f;
    monitorRouteProject.monitorModules = neuracoust::daw::defaultMonitorDspModules();
    monitorRouteProject.monitorModules[0].realModel = "Real Speaker: Nearfield";
    monitorRouteProject.monitorModules[0].targetModelA = "Speaker A: Laptop";
    assert(neuracoust::daw::makeProjectAudioRenderPlan(monitorRouteProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 20, renderBlock);
    neuracoust::daw::WavAudioData monitorRouteExpected;
    monitorRouteExpected.channels = 2;
    monitorRouteExpected.sampleRate = static_cast<int>(std::round(renderPlan.sampleRate));
    monitorRouteExpected.interleavedSamples = renderBlock;
    neuracoust::daw::applyMonitorDspToInterleavedStereo(monitorRouteExpected, monitorRouteProject.monitorModules);
    renderPlan.renderMonitorDsp = true;
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 20, renderBlock);
    assert(renderBlock.size() == monitorRouteExpected.interleavedSamples.size());
    float monitorRouteDelta = 0.0f;
    for (size_t sampleIndex = 0; sampleIndex < renderBlock.size(); ++sampleIndex) {
        monitorRouteDelta = std::max(
            monitorRouteDelta,
            std::abs(renderBlock[sampleIndex] - monitorRouteExpected.interleavedSamples[sampleIndex]));
    }
    assert(monitorRouteDelta < 0.0001f);
    assert(monitorRouteDelta >= 0.0f);

    const auto latencyRenderPath = (testTempRoot() / "neuracoust-daw-latency-render-source.wav").string();
    neuracoust::daw::WavAudioData latencyRenderSource;
    latencyRenderSource.channels = 1;
    latencyRenderSource.sampleRate = 100;
    latencyRenderSource.interleavedSamples.assign({0.5f, 0.0f, 0.0f, 0.0f});
    assert(neuracoust::daw::writePcm16WavFile(latencyRenderPath, latencyRenderSource, error));
    auto latencyRenderProject = neuracoust::daw::defaultProject();
    latencyRenderProject.sampleRate = 100.0;
    latencyRenderProject.clips.push_back({"latency-fast", "Audio 1", latencyRenderPath, 0.0, 0.04, 0.0, 0.0f});
    latencyRenderProject.clips.push_back({"latency-slow", "Audio 2", latencyRenderPath, 0.02, 0.04, 0.0, 0.0f});
    latencyRenderProject.tracks[1].inserts.push_back(makeTrackInsert("Latency Marker", "LatencyOnly", ""));
    latencyRenderProject.tracks[1].inserts.back().reportedLatencySamples = 2;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(latencyRenderProject, renderPlan, error));
    assert(renderPlan.routeDelayCompensationSamples["Audio 1"] == 2);
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 4, renderBlock);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    assert(std::abs(renderBlock[1]) < 0.0001f);
    assert(renderBlock[4] > 0.99f && renderBlock[4] < 1.01f);
    assert(renderBlock[5] > 0.99f && renderBlock[5] < 1.01f);
    std::vector<float> statefulLatencyBlock;
    std::vector<float> statefulLatencyJoined;
    neuracoust::daw::ProjectAudioRenderState latencyRenderState;
    for (int64_t frame = 0; frame < 4; ++frame) {
        neuracoust::daw::renderProjectAudioBlockWithStateAndMeters(
            renderPlan,
            latencyRenderState,
            frame,
            1,
            statefulLatencyBlock,
            nullptr);
        statefulLatencyJoined.insert(statefulLatencyJoined.end(), statefulLatencyBlock.begin(), statefulLatencyBlock.end());
    }
    assert(statefulLatencyJoined.size() == renderBlock.size());
    for (size_t sampleIndex = 0; sampleIndex < renderBlock.size(); ++sampleIndex) {
        assert(std::abs(statefulLatencyJoined[sampleIndex] - renderBlock[sampleIndex]) < 0.0001f);
    }
    latencyRenderState.reset();
    neuracoust::daw::renderProjectAudioBlockWithStateAndMeters(
        renderPlan,
        latencyRenderState,
        0,
        1,
        statefulLatencyBlock,
        nullptr);
    assert(statefulLatencyBlock.size() == 2);
    assert(std::abs(statefulLatencyBlock[0]) < 0.0001f);
    assert(std::abs(statefulLatencyBlock[1]) < 0.0001f);
    latencyRenderProject.delayCompensationEnabled = false;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(latencyRenderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 4, renderBlock);
    assert(renderBlock[0] > 0.49f && renderBlock[0] < 0.51f);
    assert(renderBlock[4] > 0.49f && renderBlock[4] < 0.51f);

    renderProject.tracks[0].volumeDb = 0.0f;
    renderProject.tracks[0].pan = 0.0f;
    auto masterTrackForRender = std::find_if(renderProject.tracks.begin(), renderProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Master";
    });
    assert(masterTrackForRender != renderProject.tracks.end());
    masterTrackForRender->volumeDb = -6.0f;
    masterTrackForRender->pan = 0.0f;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 10, 1, renderBlock);
    assert(renderBlock[0] > 0.24f && renderBlock[0] < 0.26f);
    assert(renderBlock[1] > 0.24f && renderBlock[1] < 0.26f);
    masterTrackForRender->volumeDb = -120.0f;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 10, 1, renderBlock);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    assert(std::abs(renderBlock[1]) < 0.0001f);
    masterTrackForRender->volumeDb = 0.0f;
    masterTrackForRender->pan = 1.0f;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 10, 1, renderBlock);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    assert(renderBlock[1] > 0.49f && renderBlock[1] < 0.51f);
    renderProject.tracks[0].pan = -1.0f;
    masterTrackForRender->pan = 0.0f;
    const auto stereoRenderPath = (testTempRoot() / "neuracoust-daw-stereo-render-source.wav").string();
    neuracoust::daw::WavAudioData stereoRenderSource;
    stereoRenderSource.channels = 2;
    stereoRenderSource.sampleRate = 100;
    stereoRenderSource.interleavedSamples.assign({0.5f, 0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f, 0.25f});
    assert(neuracoust::daw::writePcm16WavFile(stereoRenderPath, stereoRenderSource, error));
    auto stereoRenderProject = neuracoust::daw::defaultProject();
    stereoRenderProject.sampleRate = 100.0;
    stereoRenderProject.clips.push_back({"stereo-render-clip", "Audio 1", stereoRenderPath, 0.0, 0.04, 0.0, 0.0f});
    assert(neuracoust::daw::setTrackVolumeDb(stereoRenderProject, "Audio 1", -6.0f));
    assert(neuracoust::daw::setTrackPan(stereoRenderProject, "Audio 1", 1.0f));
    assert(neuracoust::daw::makeProjectAudioRenderPlan(stereoRenderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    assert(renderBlock[1] > 0.12f && renderBlock[1] < 0.13f);
    assert(neuracoust::daw::setTrackPan(stereoRenderProject, "Audio 1", -1.0f));
    assert(neuracoust::daw::makeProjectAudioRenderPlan(stereoRenderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(renderBlock[0] > 0.24f && renderBlock[0] < 0.26f);
    assert(std::abs(renderBlock[1]) < 0.0001f);
    auto monoRenderProject = neuracoust::daw::defaultProject();
    monoRenderProject.sampleRate = 100.0;
    monoRenderProject.tracks[0].channelFormat = "mono";
    monoRenderProject.clips.push_back({"mono-render-clip", "Audio 1", stereoRenderPath, 0.0, 0.04, 0.0, 0.0f});
    assert(neuracoust::daw::makeProjectAudioRenderPlan(monoRenderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(std::abs(renderBlock[0] - renderBlock[1]) < 0.0001f);
    // Mono track, centre pan under the default constant-power (-4.5 dB) law: 0.5946x, not
    // the old linear 1.0x. (The legacy law keeps the old value; see setTrackPan tests.)
    assert(renderBlock[0] > 0.22f && renderBlock[0] < 0.23f);
    std::vector<float> monoPreFaderBlock;
    assert(neuracoust::daw::renderTrackPreFaderStereoBlock(renderPlan, "Audio 1", 0, 1, monoPreFaderBlock));
    assert(monoPreFaderBlock.size() == 2);
    assert(std::abs(monoPreFaderBlock[0] - monoPreFaderBlock[1]) < 0.0001f);
    assert(monoPreFaderBlock[0] > 0.37f && monoPreFaderBlock[0] < 0.38f);
    auto automationProject = neuracoust::daw::defaultProject();
    automationProject.sampleRate = 100.0;
    automationProject.tracks[0].pan = -1.0f;
    automationProject.tracks[0].volumeAutomation.push_back({0.0, 0.0f});
    automationProject.tracks[0].volumeAutomation.push_back({0.10, -12.0f});
    automationProject.clips.push_back({"automation-clip", "Audio 1", renderPath, 0.0, 0.20, 0.0, 0.0f});
    assert(neuracoust::daw::makeProjectAudioRenderPlan(automationProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 12, renderBlock);
    assert(renderBlock[0] > 0.49f);
    assert(renderBlock[20] > 0.12f && renderBlock[20] < 0.14f);
    auto panAutomationProject = neuracoust::daw::defaultProject();
    panAutomationProject.sampleRate = 100.0;
    panAutomationProject.tracks[0].automationLanes.push_back({"track.pan", "Pan", {{0.0, -1.0f}, {0.10, 1.0f}}});
    panAutomationProject.clips.push_back({"pan-automation-clip", "Audio 1", renderPath, 0.0, 0.20, 0.0, 0.0f});
    assert(neuracoust::daw::makeProjectAudioRenderPlan(panAutomationProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 12, renderBlock);
    assert(renderBlock[0] > 0.49f);
    assert(renderBlock[1] == 0.0f);
    assert(std::abs(renderBlock[20]) < 0.0001f);
    assert(renderBlock[21] > 0.49f);
    auto sendProject = neuracoust::daw::defaultProject();
    sendProject.sampleRate = 100.0;
    sendProject.tracks[0].pan = 0.0f;
    sendProject.clips.push_back({"send-source", "Audio 1", renderPath, 0.0, 0.10, 0.0, 0.0f});
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    const float drySendBaseline = renderBlock[0];
    neuracoust::daw::TrackSendState send;
    send.busName = "Bus 1-2";
    send.gainDb = 0.0f;
    send.preFader = true;
    send.stereo = true;
    sendProject.tracks[0].sends.push_back(send);
    neuracoust::daw::TrackState aux;
    aux.name = "Aux 1";
    aux.inputBus = "Bus 1-2";
    aux.outputBus = "Master";
    sendProject.tracks.insert(sendProject.tracks.end() - 1, aux);
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(std::abs(renderBlock[0] - (drySendBaseline * 2.0f)) < 0.0001f);
    assert(std::abs(renderBlock[1] - (drySendBaseline * 2.0f)) < 0.0001f);
    auto sendMeterPeakForTrack = [](const neuracoust::daw::ProjectAudioBlockMeters& meters, const std::string& trackName) {
        for (size_t index = 0; index < meters.trackNames.size(); ++index) {
            if (meters.trackNames[index] == trackName) {
                const float left = index < meters.trackPeakLeft.size() ? meters.trackPeakLeft[index] : 0.0f;
                const float right = index < meters.trackPeakRight.size() ? meters.trackPeakRight[index] : 0.0f;
                return std::max(left, right);
            }
        }
        return 0.0f;
    };
    neuracoust::daw::ProjectAudioBlockMeters sendMeters;
    neuracoust::daw::renderProjectAudioBlockWithMeters(renderPlan, 0, 1, renderBlock, &sendMeters);
    const float fullSendAuxMeter = sendMeterPeakForTrack(sendMeters, "Aux 1");
    assert(fullSendAuxMeter > 0.49f && fullSendAuxMeter < 0.51f);
    sendProject.tracks[0].sends[0].gainDb = -12.0f;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlockWithMeters(renderPlan, 0, 1, renderBlock, &sendMeters);
    const float reducedSendAuxMeter = sendMeterPeakForTrack(sendMeters, "Aux 1");
    assert(reducedSendAuxMeter > 0.12f && reducedSendAuxMeter < 0.13f);
    assert(reducedSendAuxMeter < fullSendAuxMeter * 0.30f);
    sendProject.tracks[0].sends[0].enabled = false;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlockWithMeters(renderPlan, 0, 1, renderBlock, &sendMeters);
    assert(sendMeterPeakForTrack(sendMeters, "Aux 1") < 0.0001f);
    sendProject.tracks[0].sends[0].enabled = true;
    sendProject.tracks[0].sends[0].gainDb = 0.0f;
    sendProject.tracks[0].volumeDb = -6.0f;
    sendProject.tracks[0].sends[0].preFader = true;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    const float preFaderSendLevel = renderBlock[0];
    assert(preFaderSendLevel > 0.74f && preFaderSendLevel < 0.76f);
    sendProject.tracks[0].sends[0].preFader = false;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    const float postFaderSendLevel = renderBlock[0];
    assert(postFaderSendLevel > 0.49f && postFaderSendLevel < 0.51f);
    assert(preFaderSendLevel > postFaderSendLevel);
    sendProject.tracks[0].volumeDb = 0.0f;
    sendProject.tracks[0].pan = -1.0f;
    sendProject.tracks[0].sends[0].preFader = false;
    sendProject.tracks[0].sends[0].stereo = false;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(renderBlock[0] > 0.74f && renderBlock[0] < 0.76f);
    assert(renderBlock[1] > 0.24f && renderBlock[1] < 0.26f);
    sendProject.tracks[0].pan = 0.0f;
    sendProject.tracks[0].sends[0].stereo = true;
    sendProject.tracks[0].sends[0].enabled = false;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(std::abs(renderBlock[0] - drySendBaseline) < 0.0001f);
    sendProject.tracks[0].sends[0].enabled = true;
    sendProject.tracks[0].outputBus.clear();
    sendProject.tracks[1].outputBus = "Master";
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(renderBlock[0] > 0.49f && renderBlock[0] < 0.51f);
    assert(renderBlock[1] > 0.49f && renderBlock[1] < 0.51f);
    sendProject.tracks[0].sends[0].enabled = false;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(sendProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 1, renderBlock);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    assert(std::abs(renderBlock[1]) < 0.0001f);
    sendProject.tracks[0].outputBus = "Master";
    auto sendEditProject = neuracoust::daw::defaultProject();
    neuracoust::daw::TrackSendState monoSend;
    monoSend.busName = "Bus 1";
    monoSend.stereo = false;
    monoSend.enabled = true;
    monoSend.preFader = false;
    monoSend.gainDb = -18.0f;
    assert(neuracoust::daw::addTrackSendSlot(sendEditProject, "Audio 1", monoSend));
    assert(sendEditProject.tracks[0].sends.size() == 1);
    assert(sendEditProject.tracks[0].sends[0].busName == "Bus 1");
    assert(!sendEditProject.tracks[0].sends[0].stereo);
    assert(neuracoust::daw::toggleTrackSendStereo(sendEditProject, "Audio 1", 0));
    assert(sendEditProject.tracks[0].sends[0].stereo);
    assert(neuracoust::daw::toggleTrackSendPreFader(sendEditProject, "Audio 1", 0));
    assert(sendEditProject.tracks[0].sends[0].preFader);
    assert(neuracoust::daw::setTrackSendEnabled(sendEditProject, "Audio 1", 0, false));
    assert(!sendEditProject.tracks[0].sends[0].enabled);
    neuracoust::daw::TrackState sendAux;
    sendAux.name = "Aux 1";
    sendAux.trackType = "aux";
    sendAux.inputBus = "Bus 1";
    sendAux.outputBus = "Master";
    sendEditProject.tracks.insert(sendEditProject.tracks.end() - 1, sendAux);
    assert(neuracoust::daw::removeTrackSendSlot(sendEditProject, "Audio 1", 0));
    assert(sendEditProject.tracks[0].sends.empty());
    assert(std::none_of(sendEditProject.tracks.begin(), sendEditProject.tracks.end(), [](const neuracoust::daw::TrackState& track) {
        return track.name == "Aux 1" && track.inputBus == "Bus 1";
    }));
    const auto resamplePath = (testTempRoot() / "neuracoust-daw-render-resample-source.wav").string();
    neuracoust::daw::WavAudioData resampleSource;
    resampleSource.channels = 1;
    resampleSource.sampleRate = 10;
    resampleSource.bitsPerSample = 16;
    resampleSource.interleavedSamples = {0.0f, 1.0f};
    const auto convertedResampleSource = neuracoust::daw::resampleAudioDataLinear(resampleSource, 20);
    assert(convertedResampleSource.sampleRate == 20);
    assert(convertedResampleSource.channels == 1);
    assert(convertedResampleSource.frameCount() == 4);
    assert(std::abs(convertedResampleSource.interleavedSamples[0]) < 0.0001f);
    assert(convertedResampleSource.interleavedSamples[1] > 0.45f && convertedResampleSource.interleavedSamples[1] < 0.55f);
    assert(convertedResampleSource.interleavedSamples[2] > 0.99f);
    assert(convertedResampleSource.interleavedSamples[3] > 0.99f);
    assert(neuracoust::daw::writePcm16WavFile(resamplePath, resampleSource, error));
    auto resampleProject = neuracoust::daw::defaultProject();
    resampleProject.sampleRate = 20.0;
    resampleProject.tracks[0].pan = -1.0f;
    resampleProject.clips.push_back({"resample-clip", "Audio 1", resamplePath, 0.0, 0.20, 0.0, 0.0f});
    assert(neuracoust::daw::makeProjectAudioRenderPlan(resampleProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 4, renderBlock);
    assert(renderBlock.size() == 8);
    assert(std::abs(renderBlock[0]) < 0.0001f);
    // Windowed-sinc SRC: interpolating this impulse-like source at the half-sample point
    // gives its bandlimited value ~sinc(0.5)=0.637, not the old linear 0.5.
    assert(renderBlock[2] > 0.60f && renderBlock[2] < 0.67f);
    assert(renderBlock[4] > 0.99f);
    assert(renderBlock[1] == 0.0f);
    assert(renderBlock[3] == 0.0f);
    assert(renderBlock[5] == 0.0f);
    const auto stretchPath = (testTempRoot() / "neuracoust-daw-render-timescale-source.wav").string();
    neuracoust::daw::WavAudioData stretchSource;
    stretchSource.channels = 1;
    stretchSource.sampleRate = 4;
    stretchSource.bitsPerSample = 16;
    stretchSource.interleavedSamples = {0.0f, 0.25f, 0.5f, 1.0f};
    assert(neuracoust::daw::writePcm16WavFile(stretchPath, stretchSource, error));
    auto stretchProject = neuracoust::daw::defaultProject();
    stretchProject.sampleRate = 4.0;
    stretchProject.tracks[0].pan = -1.0f;
    neuracoust::daw::ClipState stretchClip;
    stretchClip.id = "timescale-clip";
    stretchClip.trackName = "Audio 1";
    stretchClip.sourcePath = stretchPath;
    stretchClip.startSeconds = 0.0;
    stretchClip.durationSeconds = 0.5;
    stretchClip.timeScale = 0.5;
    stretchProject.clips.push_back(stretchClip);
    assert(neuracoust::daw::makeProjectAudioRenderPlan(stretchProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 2, renderBlock);
    assert(renderBlock.size() == 4);
    // 2x time compression downsamples, so the windowed-sinc SRC lowpasses (anti-aliases):
    // the first sample carries a small anti-alias skirt instead of exact 0, and the mid
    // sample reads its bandlimited value rather than the old linear 0.5.
    assert(std::abs(renderBlock[0]) < 0.05f);
    assert(renderBlock[2] > 0.60f && renderBlock[2] < 0.70f);
    assert(renderBlock[1] == 0.0f);
    assert(renderBlock[3] == 0.0f);
    renderProject.tracks[0].muted = true;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 20, renderBlock);
    assert(std::all_of(renderBlock.begin(), renderBlock.end(), [](float sample) { return sample == 0.0f; }));
    renderProject.tracks[0].muted = false;
    renderProject.clips.push_back({"missing-media", "Audio 1", "/definitely/not/a/source.wav", 0.20, 0.10, 0.0, 0.0f});
    assert(neuracoust::daw::makeProjectAudioRenderPlan(renderProject, renderPlan, error));
    assert(error.empty());
    assert(renderPlan.hasMissingMedia);
    assert(renderPlan.missingMediaClipIds.size() == 1);
    assert(renderPlan.missingMediaClipIds.front() == "missing-media");
    assert(renderPlan.clips.size() == 2);
    assert(renderPlan.clips[1].missingSource);
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 20, 10, renderBlock);
    assert(renderBlock.size() == 20);
    assert(std::all_of(renderBlock.begin(), renderBlock.end(), [](float sample) { return sample == 0.0f; }));
    const auto missingMediaBouncePath = (testTempRoot() / "neuracoust-daw-missing-media-bounce.wav").string();
    const auto missingMediaBounce = neuracoust::daw::bounceProjectToWav(renderProject, missingMediaBouncePath);
    assert(missingMediaBounce.ok);
    assert(missingMediaBounce.message.find("missing media") != std::string::npos);
    assert(missingMediaBounce.missingMediaClipIds.size() == 1);
    assert(missingMediaBounce.missingMediaClipIds.front() == "missing-media");
    assert(!missingMediaBounce.manifestPath.empty());
    assert(missingMediaBounce.levelStats.peakLeft > 0.0f);
    assert(missingMediaBounce.levelStats.rmsLeft > 0.0f);
    {
        std::ifstream bounceManifest(missingMediaBounce.manifestPath, std::ios::binary);
        std::string bounceManifestText((std::istreambuf_iterator<char>(bounceManifest)), std::istreambuf_iterator<char>());
        assert(bounceManifestText.find("neuracoust-daw-bounce-v1") != std::string::npos);
        assert(bounceManifestText.find("\"levelStats\"") != std::string::npos);
        assert(bounceManifestText.find("\"peakLeftDb\"") != std::string::npos);
        assert(bounceManifestText.find("\"clippingDetected\"") != std::string::npos);
        assert(bounceManifestText.find("\"missingMediaRenderedAsSilence\": true") != std::string::npos);
        assert(bounceManifestText.find("\"missing-media\"") != std::string::npos);
    }
    neuracoust::daw::WavAudioData missingMediaBounced;
    assert(neuracoust::daw::readPcmWavFile(missingMediaBouncePath, missingMediaBounced, error));
    assert(missingMediaBounced.frameCount() >= 30);

    const auto loopSourcePath = (testTempRoot() / "neuracoust-daw-loop-source.wav").string();
    neuracoust::daw::WavAudioData loopSource;
    loopSource.channels = 1;
    loopSource.sampleRate = 10;
    for (int frame = 0; frame < 10; ++frame) {
        loopSource.interleavedSamples.push_back(static_cast<float>(frame + 1) * 0.1f);
    }
    assert(neuracoust::daw::writePcm16WavFile(loopSourcePath, loopSource, error));
    auto loopProject = neuracoust::daw::defaultProject();
    loopProject.sampleRate = 10.0;
    loopProject.tracks[0].pan = -1.0f;
    loopProject.loopEnabled = true;
    loopProject.loopStartSeconds = 0.2;
    loopProject.loopEndSeconds = 0.5;
    loopProject.clips.push_back({"loop-clip", "Audio 1", loopSourcePath, 0.0, 1.0, 0.0, 0.0f});
    assert(neuracoust::daw::makeProjectAudioRenderPlan(loopProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 8, renderBlock);
    assert(renderBlock.size() == 16);
    assert(renderBlock[0] > 0.09f && renderBlock[0] < 0.11f);
    assert(renderBlock[2] > 0.19f && renderBlock[2] < 0.21f);
    assert(renderBlock[4] > 0.29f && renderBlock[4] < 0.31f);
    assert(renderBlock[6] > 0.39f && renderBlock[6] < 0.41f);
    assert(renderBlock[8] > 0.49f && renderBlock[8] < 0.51f);
    assert(renderBlock[10] > 0.29f && renderBlock[10] < 0.31f);
    assert(renderBlock[12] > 0.39f && renderBlock[12] < 0.41f);
    assert(renderBlock[14] > 0.49f && renderBlock[14] < 0.51f);

    // Pre/post-roll extends the repeated audition window on both sides of the marked loop.
    loopProject.preRollSeconds = 0.1;
    loopProject.postRollSeconds = 0.2;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(loopProject, renderPlan, error));
    assert(renderPlan.preRollSeconds == 0.1);
    assert(renderPlan.postRollSeconds == 0.2);
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 9, renderBlock);
    assert(renderBlock.size() == 18);
    assert(renderBlock[12] > 0.69f && renderBlock[12] < 0.71f);
    assert(renderBlock[14] > 0.19f && renderBlock[14] < 0.21f);
    assert(renderBlock[16] > 0.29f && renderBlock[16] < 0.31f);

    auto slipProject = neuracoust::daw::defaultProject();
    slipProject.sampleRate = 10.0;
    slipProject.tracks[0].pan = -1.0f;
    slipProject.clips.push_back({"slip-clip", "Audio 1", loopSourcePath, 0.0, 0.3, 0.0, 0.0f});
    std::string slipMessage;
    assert(neuracoust::daw::slipClipSourceOffset(slipProject, "slip-clip", 0.2, slipMessage));
    assert(slipProject.clips.front().sourceOffsetSeconds == 0.2);
    assert(slipMessage.find("Slipped") != std::string::npos);
    assert(!neuracoust::daw::slipClipSourceOffset(slipProject, "slip-clip", -0.3, slipMessage));
    assert(slipProject.clips.front().sourceOffsetSeconds == 0.2);
    assert(neuracoust::daw::makeProjectAudioRenderPlan(slipProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 3, renderBlock);
    assert(renderBlock.size() == 6);
    assert(renderBlock[0] > 0.29f && renderBlock[0] < 0.31f);
    assert(renderBlock[2] > 0.39f && renderBlock[2] < 0.41f);
    assert(neuracoust::daw::setClipPolarityInverted(slipProject, "slip-clip", true));
    assert(slipProject.clips.front().polarityInverted);
    assert(neuracoust::daw::makeProjectAudioRenderPlan(slipProject, renderPlan, error));
    neuracoust::daw::renderProjectAudioBlock(renderPlan, 0, 3, renderBlock);
    assert(renderBlock.size() == 6);
    assert(renderBlock[0] < -0.29f && renderBlock[0] > -0.31f);
    assert(renderBlock[2] < -0.39f && renderBlock[2] > -0.41f);

    project.tracks[0].volumeDb = -3.0f;
    project.tracks[0].pan = -0.25f;
    project.clips.push_back({"clip-1", "Audio 1", wavPath, 0.0, 0.05, 0.0, -6.0f});
    assert(neuracoust::daw::moveClip(project, "clip-1", 0.01));
    assert(neuracoust::daw::setClipFades(project, "clip-1", 0.02, 0.02));
    const auto clipBeforeInvalidTrim = project.clips[0];
    assert(!neuracoust::daw::trimClipStart(project, "clip-1", 0.0));
    assert(project.clips[0].startSeconds == clipBeforeInvalidTrim.startSeconds);
    assert(project.clips[0].durationSeconds == clipBeforeInvalidTrim.durationSeconds);
    assert(project.clips[0].sourceOffsetSeconds == clipBeforeInvalidTrim.sourceOffsetSeconds);
    assert(neuracoust::daw::trimClipStart(project, "clip-1", 0.02));
    assert(project.clips[0].sourceOffsetSeconds > 0.0);
    assert(neuracoust::daw::trimClipEnd(project, "clip-1", 0.045));
    assert(project.clips[0].durationSeconds < 0.03);
    assert(project.clips[0].fadeInSeconds <= project.clips[0].durationSeconds * 0.5);
    assert(project.clips[0].fadeOutSeconds <= project.clips[0].durationSeconds * 0.5);
    assert(neuracoust::daw::setClipGainDb(project, "clip-1", -2.5f));
    assert(project.clips[0].gainDb == -2.5f);
    assert(neuracoust::daw::setClipGainDb(project, "clip-1", 99.0f));
    assert(project.clips[0].gainDb == 24.0f);
    assert(neuracoust::daw::setClipGainDb(project, "clip-1", -99.0f));
    assert(project.clips[0].gainDb == -60.0f);
    assert(!neuracoust::daw::setClipGainDb(project, "clip-1", std::numeric_limits<float>::quiet_NaN()));
    assert(project.clips[0].gainDb == -60.0f);

    auto playlistGainProject = neuracoust::daw::defaultProject();
    playlistGainProject.clips.push_back({"playlist-gain-clip", "Audio 1", wavPath, 0.0, 0.05, 0.0, 0.0f});
    neuracoust::daw::normalizeProjectEditModel(playlistGainProject);
    assert(neuracoust::daw::setClipGainDb(playlistGainProject, "playlist-gain-clip", -8.4f));
    assert(std::abs(playlistGainProject.clips.front().gainDb + 8.4f) < 0.001f);
    assert(neuracoust::daw::rebuildProjectClipsFromActivePlaylists(playlistGainProject));
    assert(std::abs(playlistGainProject.clips.front().gainDb + 8.4f) < 0.001f);

    const auto normalizeSourcePath = (testTempRoot() / "neuracoust-daw-normalize-source.wav").string();
    neuracoust::daw::WavAudioData normalizeSource;
    normalizeSource.channels = 1;
    normalizeSource.sampleRate = 10;
    normalizeSource.interleavedSamples = {0.0f, 0.25f, -0.125f, 0.10f};
    assert(neuracoust::daw::writePcm16WavFile(normalizeSourcePath, normalizeSource, error));
    auto normalizeProject = neuracoust::daw::defaultProject();
    normalizeProject.clips.push_back({"normalize-clip", "Audio 1", normalizeSourcePath, 0.0, 0.4, 0.0, 0.0f});
    std::string normalizeMessage;
    assert(neuracoust::daw::normalizeClipGainToPeak(normalizeProject, "normalize-clip", -6.0f, normalizeMessage));
    assert(std::abs(normalizeProject.clips.front().gainDb - 6.0f) < 0.2f);
    assert(normalizeMessage.find("Normalized clip gain") != std::string::npos);
    auto normalizeRangeProject = neuracoust::daw::defaultProject();
    normalizeRangeProject.clips.push_back({"normalize-range-wide", "Audio 1", normalizeSourcePath, 0.0, 0.4, 0.0, 0.0f});
    normalizeRangeProject.clips.push_back({"normalize-range-locked", "Audio 2", normalizeSourcePath, 0.1, 0.2, 0.0, 0.0f});
    normalizeRangeProject.clips.back().locked = true;
    std::vector<std::string> normalizeRangeIds;
    assert(neuracoust::daw::normalizeClipGainInRange(normalizeRangeProject, 0.1, 0.3, -6.0f, normalizeRangeIds, normalizeMessage));
    assert(std::find(normalizeRangeIds.begin(), normalizeRangeIds.end(), "normalize-range-wide-split-2") != normalizeRangeIds.end());
    assert(std::find(normalizeRangeIds.begin(), normalizeRangeIds.end(), "normalize-range-locked") == normalizeRangeIds.end());
    const auto normalizedRangeClip = std::find_if(normalizeRangeProject.clips.begin(), normalizeRangeProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
        return clip.id == "normalize-range-wide-split-2";
    });
    assert(normalizedRangeClip != normalizeRangeProject.clips.end());
    assert(std::abs(normalizedRangeClip->startSeconds - 0.1) < 0.0001);
    assert(std::abs(normalizedRangeClip->durationSeconds - 0.2) < 0.0001);
    assert(std::abs(normalizedRangeClip->gainDb - 6.0f) < 0.2f);
    assert(normalizeRangeProject.clips.front().gainDb == 0.0f);
    assert(normalizeRangeProject.clips[1].gainDb == 0.0f);
    assert(!neuracoust::daw::normalizeClipGainInRange(normalizeRangeProject, 2.0, 3.0, -6.0f, normalizeRangeIds, normalizeMessage));
    assert(normalizeRangeIds.empty());
    normalizeProject.clips.front().sourceOffsetSeconds = 99.0;
    assert(!neuracoust::daw::normalizeClipGainToPeak(normalizeProject, "normalize-clip", -6.0f, normalizeMessage));
    assert(normalizeMessage.find("outside") != std::string::npos);
    assert(neuracoust::daw::setClipFades(project, "clip-1", 0.01, 0.01));
    assert(project.clips[0].fadeInSeconds == 0.01);
    assert(project.clips[0].fadeOutSeconds == 0.01);
    assert(neuracoust::daw::setClipFadeCurves(project, "clip-1", "linear", "FAST"));
    assert(project.clips[0].fadeInCurve == "linear");
    assert(project.clips[0].fadeOutCurve == "fast");
    assert(neuracoust::daw::setClipFadeCurves(project, "clip-1", "bad curve", "slow"));
    assert(project.clips[0].fadeInCurve == "equal_power");
    assert(project.clips[0].fadeOutCurve == "slow");
    assert(neuracoust::daw::setClipMuted(project, "clip-1", true));
    assert(project.clips[0].muted);
    assert(neuracoust::daw::setClipPolarityInverted(project, "clip-1", true));
    assert(project.clips[0].polarityInverted);
    assert(neuracoust::daw::setClipSourcePath(project, "clip-1", "/tmp/relinked.wav", 0.04));
    assert(project.clips[0].sourcePath == "/tmp/relinked.wav");
    assert(project.clips[0].durationSeconds == 0.04);
    assert(project.clips[0].sourceOffsetSeconds == 0.0);
    assert(!neuracoust::daw::setClipSourcePath(project, "clip-1", "", 0.04));
    assert(!neuracoust::daw::setClipSourcePath(project, "clip-1", "/tmp/bad.wav", 0.0));
    project.clips[0].sourcePath = wavPath;
    auto fadeJson = neuracoust::daw::serializeProject(project);
    assert(fadeJson.find("\"muted\":true") != std::string::npos);
    assert(fadeJson.find("\"polarityInverted\":true") != std::string::npos);
    neuracoust::daw::ProjectDocument fadeRoundTrip;
    assert(neuracoust::daw::deserializeProject(fadeJson, fadeRoundTrip, projectParseError));
    assert(fadeRoundTrip.clips[0].fadeInSeconds == 0.01);
    assert(fadeRoundTrip.clips[0].fadeOutSeconds == 0.01);
    assert(fadeRoundTrip.clips[0].fadeInCurve == "equal_power");
    assert(fadeRoundTrip.clips[0].fadeOutCurve == "slow");
    assert(fadeRoundTrip.clips[0].muted);
    assert(fadeRoundTrip.clips[0].polarityInverted);
    std::vector<float> mutedBlock;
    neuracoust::daw::ProjectAudioRenderPlan mutedPlan;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(project, mutedPlan, error));
    neuracoust::daw::renderProjectAudioBlock(mutedPlan, 0, 32, mutedBlock);
    assert(std::all_of(mutedBlock.begin(), mutedBlock.end(), [](float sample) {
        return std::abs(sample) < 0.0001f;
    }));
    assert(neuracoust::daw::setClipMuted(project, "clip-1", false));
    assert(!project.clips[0].muted);
    assert(neuracoust::daw::setClipPolarityInverted(project, "clip-1", false));
    assert(!project.clips[0].polarityInverted);
    assert(neuracoust::daw::setClipFades(project, "clip-1", 0.02, 0.02));
    std::string splitId;
    assert(neuracoust::daw::splitClip(project, "clip-1", 0.04, splitId));
    assert(!splitId.empty());
    assert(project.clips.size() == 2);
    assert(project.clips[0].fadeInSeconds <= project.clips[0].durationSeconds * 0.5);
    assert(project.clips[0].fadeOutSeconds <= project.clips[0].durationSeconds * 0.5);
    assert(project.clips[1].fadeInSeconds <= project.clips[1].durationSeconds * 0.5);
    assert(project.clips[1].fadeOutSeconds <= project.clips[1].durationSeconds * 0.5);
    {
        auto playheadSplitProject = neuracoust::daw::defaultProject();
        playheadSplitProject.clips.push_back({"playhead-cut-a", "Audio 1", wavPath, 2.0, 6.0, 1.25, 0.0f});
        playheadSplitProject.clips.front().fadeInSeconds = 0.5;
        playheadSplitProject.clips.front().fadeOutSeconds = 0.5;
        std::string playheadRightId;
        assert(neuracoust::daw::splitClip(playheadSplitProject, "playhead-cut-a", 4.75, playheadRightId));
        assert(playheadRightId == "playhead-cut-a-split");
        assert(playheadSplitProject.clips.size() == 2);
        const auto leftPlayheadClip = std::find_if(playheadSplitProject.clips.begin(), playheadSplitProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
            return clip.id == "playhead-cut-a";
        });
        const auto rightPlayheadClip = std::find_if(playheadSplitProject.clips.begin(), playheadSplitProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
            return clip.id == playheadRightId;
        });
        assert(leftPlayheadClip != playheadSplitProject.clips.end());
        assert(rightPlayheadClip != playheadSplitProject.clips.end());
        assert(std::abs(leftPlayheadClip->startSeconds - 2.0) < 0.0001);
        assert(std::abs(leftPlayheadClip->durationSeconds - 2.75) < 0.0001);
        assert(std::abs(leftPlayheadClip->sourceOffsetSeconds - 1.25) < 0.0001);
        assert(std::abs(rightPlayheadClip->startSeconds - 4.75) < 0.0001);
        assert(std::abs(rightPlayheadClip->durationSeconds - 3.25) < 0.0001);
        assert(std::abs(rightPlayheadClip->sourceOffsetSeconds - 4.0) < 0.0001);
        const auto clipCountBeforeRejectedSplit = playheadSplitProject.clips.size();
        std::string rejectedSplitId;
        assert(!neuracoust::daw::splitClip(playheadSplitProject, "playhead-cut-a", 7.0, rejectedSplitId));
        assert(rejectedSplitId.empty());
        assert(playheadSplitProject.clips.size() == clipCountBeforeRejectedSplit);
    }
    {
        auto splitGlueProject = neuracoust::daw::defaultProject();
        splitGlueProject.clips.push_back({"split-glue-a", "Audio 1", wavPath, 0.0, 1.0, 0.2, 0.0f});
        splitGlueProject.clips.front().sourceFileUid = "src-split-glue-001";
        splitGlueProject.clips.front().regionName = "Lead Vocal";
        splitGlueProject.clips.front().colorHex = "#35BFA8";
        splitGlueProject.clips.front().sourceChannels = 2;
        splitGlueProject.clips.front().sourceSampleRate = 48000.0;
        splitGlueProject.clips.front().sourceBitsPerSample = 24;
        splitGlueProject.clips.front().sourceFloatingPoint = false;
        splitGlueProject.clips.front().sourceHasBroadcastTimeReference = true;
        splitGlueProject.clips.front().sourceTimeReferenceSamples = 48000;
        splitGlueProject.clips.front().fadeInSeconds = 0.02;
        splitGlueProject.clips.front().fadeInCurve = "linear";
        std::string splitGlueRightId;
        assert(neuracoust::daw::splitClip(splitGlueProject, "split-glue-a", 0.5, splitGlueRightId));
        auto splitGlueRight = std::find_if(splitGlueProject.clips.begin(), splitGlueProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
            return clip.id == splitGlueRightId;
        });
        assert(splitGlueRight != splitGlueProject.clips.end());
        splitGlueRight->fadeOutSeconds = 0.02;
        splitGlueRight->fadeOutCurve = "slow";
        std::string gluedClipId;
        assert(neuracoust::daw::glueAdjacentClip(splitGlueProject, "split-glue-a", gluedClipId));
        assert(gluedClipId == "split-glue-a");
        assert(splitGlueProject.clips.size() == 1);
        const auto& gluedClip = splitGlueProject.clips.front();
        assert(gluedClip.sourcePath == wavPath);
        assert(gluedClip.sourceFileUid == "src-split-glue-001");
        assert(gluedClip.regionName == "Lead Vocal");
        assert(gluedClip.colorHex == "#35BFA8");
        assert(gluedClip.sourceChannels == 2);
        assert(std::abs(gluedClip.sourceSampleRate - 48000.0) < 0.0001);
        assert(gluedClip.sourceBitsPerSample == 24);
        assert(!gluedClip.sourceFloatingPoint);
        assert(gluedClip.sourceHasBroadcastTimeReference);
        assert(gluedClip.sourceTimeReferenceSamples == 48000);
        assert(std::abs(gluedClip.startSeconds - 0.0) < 0.0001);
        assert(std::abs(gluedClip.sourceOffsetSeconds - 0.2) < 0.0001);
        assert(std::abs(gluedClip.durationSeconds - 1.0) < 0.0001);
        assert(gluedClip.fadeInCurve == "linear");
        assert(gluedClip.fadeOutCurve == "slow");

        auto mismatchedGlueProject = neuracoust::daw::defaultProject();
        mismatchedGlueProject.clips.push_back({"glue-meta-a", "Audio 1", wavPath, 0.0, 0.5, 0.0, 0.0f});
        mismatchedGlueProject.clips.push_back({"glue-meta-b", "Audio 1", wavPath, 0.5, 0.5, 0.5, 0.0f});
        mismatchedGlueProject.clips[0].sourceFileUid = "src-glue-meta";
        mismatchedGlueProject.clips[1].sourceFileUid = "src-glue-meta";
        mismatchedGlueProject.clips[0].sourceChannels = 2;
        mismatchedGlueProject.clips[1].sourceChannels = 1;
        assert(!neuracoust::daw::glueAdjacentClip(mismatchedGlueProject, "glue-meta-a", gluedClipId));
    }
    std::string secondSplitId;
    assert(neuracoust::daw::splitClip(project, "clip-1", 0.03, secondSplitId));
    assert(secondSplitId == "clip-1-split-2");
    assert(project.clips.size() == 3);
    std::string duplicateId;
    assert(neuracoust::daw::duplicateClip(project, "clip-1-split", 0.08, duplicateId));
    assert(duplicateId == "clip-1-split-copy");
    assert(project.clips.back().id == duplicateId);
    assert(project.clips.back().startSeconds == 0.08);
    project.clips.back().sourceFileUid = "src-shared-nondestructive";
    std::string duplicateToTrackId;
    assert(neuracoust::daw::duplicateClipToTrack(project, duplicateId, 0.22, "Audio 2", duplicateToTrackId));
    assert(duplicateToTrackId == duplicateId + "-copy");
    assert(project.clips.back().id == duplicateToTrackId);
    assert(project.clips.back().trackName == "Audio 2");
    assert(project.clips.back().startSeconds == 0.22);
    assert(project.clips.back().sourcePath == project.clips[3].sourcePath);
    assert(project.clips.back().sourceFileUid == "src-shared-nondestructive");
    const auto copiedTrackDuplicateId = duplicateToTrackId;
    const auto duplicateToTrackCount = project.clips.size();
    assert(!neuracoust::daw::duplicateClipToTrack(project, duplicateId, 0.24, "Master", duplicateToTrackId));
    assert(project.clips.size() == duplicateToTrackCount);
    assert(!neuracoust::daw::duplicateClipToTrack(project, duplicateId, 0.24, "No Such Track", duplicateToTrackId));
    assert(project.clips.size() == duplicateToTrackCount);
    assert(neuracoust::daw::deleteClip(project, copiedTrackDuplicateId));
    assert(neuracoust::daw::nudgeClip(project, duplicateId, 0.10));
    assert(std::abs(project.clips.back().startSeconds - 0.18) < 0.0001);
    assert(neuracoust::daw::nudgeClip(project, duplicateId, -0.50));
    assert(project.clips.back().startSeconds == 0.0);
    assert(!neuracoust::daw::nudgeClip(project, duplicateId, -0.50));
    auto boundaryProject = neuracoust::daw::defaultProject();
    boundaryProject.clips.push_back({"boundary-a", "Audio 1", wavPath, 1.0, 0.5, 0.0, 0.0f});
    boundaryProject.clips.push_back({"boundary-b", "Audio 1", wavPath, 2.0, 0.75, 0.0, 0.0f});
    boundaryProject.clips.push_back({"boundary-c", "Audio 2", wavPath, 1.25, 0.25, 0.0, 0.0f});
    double boundarySeconds = 0.0;
    assert(neuracoust::daw::nextClipBoundaryAfter(boundaryProject, 0.0, boundarySeconds));
    assert(std::abs(boundarySeconds - 1.0) < 0.0001);
    assert(neuracoust::daw::nextClipBoundaryAfter(boundaryProject, 1.0, boundarySeconds));
    assert(std::abs(boundarySeconds - 1.25) < 0.0001);
    assert(neuracoust::daw::nextClipBoundaryAfter(boundaryProject, 1.0, boundarySeconds, "Audio 1"));
    assert(std::abs(boundarySeconds - 1.5) < 0.0001);
    assert(neuracoust::daw::previousClipBoundaryBefore(boundaryProject, 2.10, boundarySeconds, "Audio 1"));
    assert(std::abs(boundarySeconds - 2.0) < 0.0001);
    assert(!neuracoust::daw::previousClipBoundaryBefore(boundaryProject, 1.0, boundarySeconds, "Audio 1"));
    assert(!neuracoust::daw::nextClipBoundaryAfter(boundaryProject, 3.0, boundarySeconds));
    assert(neuracoust::daw::setEditSelectionToAdjacentClipBoundary(boundaryProject, 1.1, true, boundarySeconds, "Audio 1"));
    assert(std::abs(boundarySeconds - 1.5) < 0.0001);
    assert(boundaryProject.editSelectionEnabled);
    assert(std::abs(boundaryProject.editSelectionStartSeconds - 1.1) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionEndSeconds - 1.5) < 0.0001);
    assert(neuracoust::daw::setEditSelectionToAdjacentClipBoundary(boundaryProject, 1.9, false, boundarySeconds, "Audio 1"));
    assert(std::abs(boundarySeconds - 1.5) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionStartSeconds - 1.5) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionEndSeconds - 1.9) < 0.0001);
    assert(!neuracoust::daw::setEditSelectionToAdjacentClipBoundary(boundaryProject, 3.0, true, boundarySeconds, "Audio 1"));
    assert(std::abs(boundaryProject.editSelectionStartSeconds - 1.5) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionEndSeconds - 1.9) < 0.0001);
    double rangeStartSeconds = 0.0;
    double rangeEndSeconds = 0.0;
    assert(neuracoust::daw::setEditSelectionToSurroundingClipBoundaries(boundaryProject, 1.75, rangeStartSeconds, rangeEndSeconds, "Audio 1"));
    assert(std::abs(rangeStartSeconds - 1.5) < 0.0001);
    assert(std::abs(rangeEndSeconds - 2.0) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionStartSeconds - 1.5) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionEndSeconds - 2.0) < 0.0001);
    assert(!neuracoust::daw::setEditSelectionToSurroundingClipBoundaries(boundaryProject, 0.5, rangeStartSeconds, rangeEndSeconds, "Audio 1"));
    boundaryProject.markers.clear();   // drop the seeded start marker so 0.1 s has no surrounding pair
    boundaryProject.markers.push_back({"mk-a", "Verse", 0.5});
    boundaryProject.markers.push_back({"mk-b", "Chorus", 2.25});
    boundaryProject.markers.push_back({"mk-c", "Bridge", 4.0});
    assert(neuracoust::daw::setEditSelectionToSurroundingMarkers(boundaryProject, 3.0, rangeStartSeconds, rangeEndSeconds));
    assert(std::abs(rangeStartSeconds - 2.25) < 0.0001);
    assert(std::abs(rangeEndSeconds - 4.0) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionStartSeconds - 2.25) < 0.0001);
    assert(std::abs(boundaryProject.editSelectionEndSeconds - 4.0) < 0.0001);
    assert(!neuracoust::daw::setEditSelectionToSurroundingMarkers(boundaryProject, 0.1, rangeStartSeconds, rangeEndSeconds));
    assert(project.clips.size() == 4);
    const auto clipboardClip = project.clips.back();
    std::string pastedId;
    assert(neuracoust::daw::pasteClip(project, clipboardClip, 0.12, pastedId));
    assert(pastedId == duplicateId + "-copy");
    assert(project.clips.back().id == pastedId);
    assert(project.clips.back().startSeconds == 0.12);
    assert(project.clips.back().sourcePath == clipboardClip.sourcePath);
    assert(project.clips.back().trackName == clipboardClip.trackName);
    assert(project.clips.size() == 5);
    auto crossfadeProject = neuracoust::daw::defaultProject();
    crossfadeProject.clips.push_back({"xfade-left", "Audio 1", wavPath, 0.0, 1.0, 0.0, 0.0f});
    crossfadeProject.clips.push_back({"xfade-right", "Audio 1", wavPath, 0.75, 1.0, 0.0, 0.0f});
    crossfadeProject.clips.push_back({"xfade-other-track", "Audio 2", wavPath, 0.8, 1.0, 0.0, 0.0f});
    assert(neuracoust::daw::applyAutomaticClipCrossfades(crossfadeProject, "xfade-right"));
    assert(std::abs(crossfadeProject.clips[0].fadeOutSeconds - 0.25) < 0.0001);
    assert(std::abs(crossfadeProject.clips[1].fadeInSeconds - 0.25) < 0.0001);
    assert(crossfadeProject.clips[2].fadeInSeconds == 0.0);
    assert(crossfadeProject.clips[2].fadeOutSeconds == 0.0);
    auto multiCrossfadeProject = neuracoust::daw::defaultProject();
    multiCrossfadeProject.clips.push_back({"xfade-prev-small", "Audio 1", wavPath, 0.40, 0.50, 0.0, 0.0f});
    multiCrossfadeProject.clips.push_back({"xfade-prev-large", "Audio 1", wavPath, 0.25, 0.85, 0.0, 0.0f});
    multiCrossfadeProject.clips.push_back({"xfade-center", "Audio 1", wavPath, 0.80, 1.00, 0.0, 0.0f});
    multiCrossfadeProject.clips.push_back({"xfade-next", "Audio 1", wavPath, 1.60, 0.60, 0.0, 0.0f});
    multiCrossfadeProject.clips.push_back({"xfade-locked", "Audio 1", wavPath, 1.70, 0.40, 0.0, 0.0f});
    multiCrossfadeProject.clips.back().locked = true;
    assert(neuracoust::daw::applyAutomaticClipCrossfades(multiCrossfadeProject, "xfade-center"));
    assert(std::abs(multiCrossfadeProject.clips[0].fadeOutSeconds - 0.10) < 0.0001);
    assert(std::abs(multiCrossfadeProject.clips[1].fadeOutSeconds - 0.30) < 0.0001);
    assert(std::abs(multiCrossfadeProject.clips[2].fadeInSeconds - 0.30) < 0.0001);
    assert(std::abs(multiCrossfadeProject.clips[2].fadeOutSeconds - 0.20) < 0.0001);
    assert(std::abs(multiCrossfadeProject.clips[3].fadeInSeconds - 0.20) < 0.0001);
    assert(multiCrossfadeProject.clips[4].fadeInSeconds == 0.0);
    assert(multiCrossfadeProject.clips[4].fadeOutSeconds == 0.0);
    auto rangeFadeProject = neuracoust::daw::defaultProject();
    rangeFadeProject.clips.push_back({"range-fade-left", "Audio 1", wavPath, 0.00, 1.00, 0.0, 0.0f});
    rangeFadeProject.clips.push_back({"range-fade-right", "Audio 1", wavPath, 0.75, 1.00, 0.0, 0.0f});
    rangeFadeProject.clips.push_back({"range-fade-single", "Audio 2", wavPath, 0.20, 0.40, 0.0, 0.0f});
    rangeFadeProject.clips.push_back({"range-fade-locked", "Audio 2", wavPath, 0.50, 0.40, 0.0, 0.0f});
    rangeFadeProject.clips.back().locked = true;
    std::vector<std::string> rangeFadeIds;
    assert(neuracoust::daw::applyFadeOrCrossfadeToClipRange(rangeFadeProject, 0.0, 1.2, rangeFadeIds));
    assert(std::find(rangeFadeIds.begin(), rangeFadeIds.end(), "range-fade-left") != rangeFadeIds.end());
    assert(std::find(rangeFadeIds.begin(), rangeFadeIds.end(), "range-fade-right") != rangeFadeIds.end());
    assert(std::find(rangeFadeIds.begin(), rangeFadeIds.end(), "range-fade-single") != rangeFadeIds.end());
    assert(std::find(rangeFadeIds.begin(), rangeFadeIds.end(), "range-fade-locked") == rangeFadeIds.end());
    assert(std::abs(rangeFadeProject.clips[0].fadeOutSeconds - 0.25) < 0.0001);
    assert(std::abs(rangeFadeProject.clips[1].fadeInSeconds - 0.25) < 0.0001);
    assert(std::abs(rangeFadeProject.clips[2].fadeInSeconds - 0.05) < 0.0001);
    assert(std::abs(rangeFadeProject.clips[2].fadeOutSeconds - 0.05) < 0.0001);
    assert(rangeFadeProject.clips[3].fadeInSeconds == 0.0);
    assert(rangeFadeProject.clips[3].fadeOutSeconds == 0.0);
    assert(!neuracoust::daw::applyFadeOrCrossfadeToClipRange(rangeFadeProject, 4.0, 5.0, rangeFadeIds));
    assert(rangeFadeIds.empty());
    auto noOverlapCrossfadeProject = neuracoust::daw::defaultProject();
    noOverlapCrossfadeProject.clips.push_back({"single-fade", "Audio 1", wavPath, 0.0, 1.0, 0.0, 0.0f});
    assert(!neuracoust::daw::applyAutomaticClipCrossfades(noOverlapCrossfadeProject, "single-fade"));
    assert(noOverlapCrossfadeProject.clips.front().fadeInSeconds == 0.0);
    assert(noOverlapCrossfadeProject.clips.front().fadeOutSeconds == 0.0);
    auto rangeClearProject = neuracoust::daw::defaultProject();
    rangeClearProject.clips.push_back({"range-wide", "Audio 1", wavPath, 0.0, 1.0, 0.0, 0.0f});
    rangeClearProject.clips.push_back({"range-inside", "Audio 1", wavPath, 0.30, 0.10, 0.0, 0.0f});
    rangeClearProject.clips.push_back({"range-after", "Audio 2", wavPath, 1.20, 0.20, 0.0, 0.0f});
    assert(neuracoust::daw::clearClipRange(rangeClearProject, 0.25, 0.75));
    assert(rangeClearProject.clips.size() == 3);
    assert(rangeClearProject.clips[0].id == "range-wide");
    assert(rangeClearProject.clips[0].startSeconds == 0.0);
    assert(std::abs(rangeClearProject.clips[0].durationSeconds - 0.25) < 0.0001);
    assert(rangeClearProject.clips[1].id == "range-wide-split");
    assert(std::abs(rangeClearProject.clips[1].startSeconds - 0.75) < 0.0001);
    assert(std::abs(rangeClearProject.clips[1].sourceOffsetSeconds - 0.75) < 0.0001);
    assert(rangeClearProject.clips[2].id == "range-after");
    assert(!neuracoust::daw::clearClipRange(rangeClearProject, 2.0, 3.0));
    auto separateRangeProject = neuracoust::daw::defaultProject();
    separateRangeProject.clips.push_back({"separate-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    separateRangeProject.clips.push_back({"separate-inside", "Audio 2", wavPath, 0.35, 0.10, 0.0, 0.0f});
    std::vector<std::string> separatedClipIds;
    assert(neuracoust::daw::separateClipRange(separateRangeProject, 0.25, 0.75, separatedClipIds));
    assert(separatedClipIds.size() == 2);
    assert(separateRangeProject.clips.size() == 4);
    assert(separateRangeProject.clips[0].id == "separate-wide");
    assert(std::abs(separateRangeProject.clips[0].durationSeconds - 0.25) < 0.0001);
    assert(separateRangeProject.clips[2].id == "separate-wide-split");
    assert(std::abs(separateRangeProject.clips[2].startSeconds - 0.75) < 0.0001);
    assert(separateRangeProject.clips[3].id == "separate-wide-split-2");
    assert(std::abs(separateRangeProject.clips[3].startSeconds - 0.25) < 0.0001);
    assert(std::abs(separateRangeProject.clips[3].durationSeconds - 0.50) < 0.0001);
    assert(std::abs(separateRangeProject.clips[3].sourceOffsetSeconds - 0.35) < 0.0001);
    assert(!neuracoust::daw::separateClipRange(separateRangeProject, 1.50, 2.00, separatedClipIds));
    auto rangeGainProject = neuracoust::daw::defaultProject();
    rangeGainProject.clips.push_back({"range-gain-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    rangeGainProject.clips.push_back({"range-gain-inside", "Audio 2", wavPath, 0.35, 0.10, 0.0, -2.0f});
    rangeGainProject.clips.push_back({"range-gain-locked", "Audio 1", wavPath, 0.30, 0.10, 0.0, 0.0f});
    rangeGainProject.clips.back().locked = true;
    std::vector<std::string> rangeGainIds;
    assert(neuracoust::daw::adjustClipGainInRange(rangeGainProject, 0.25, 0.75, 3.0f, rangeGainIds));
    assert(std::find(rangeGainIds.begin(), rangeGainIds.end(), "range-gain-wide-split-2") != rangeGainIds.end());
    assert(std::find(rangeGainIds.begin(), rangeGainIds.end(), "range-gain-inside") != rangeGainIds.end());
    assert(std::find(rangeGainIds.begin(), rangeGainIds.end(), "range-gain-locked") == rangeGainIds.end());
    assert(rangeGainProject.clips.size() == 5);
    assert(rangeGainProject.clips[0].id == "range-gain-wide");
    assert(rangeGainProject.clips[0].gainDb == 0.0f);
    assert(rangeGainProject.clips[2].id == "range-gain-locked");
    assert(rangeGainProject.clips[2].gainDb == 0.0f);
    const auto rangeGainMiddle = std::find_if(rangeGainProject.clips.begin(), rangeGainProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
        return clip.id == "range-gain-wide-split-2";
    });
    assert(rangeGainMiddle != rangeGainProject.clips.end());
    assert(std::abs(rangeGainMiddle->startSeconds - 0.25) < 0.0001);
    assert(std::abs(rangeGainMiddle->durationSeconds - 0.50) < 0.0001);
    assert(rangeGainMiddle->gainDb == 3.0f);
    const auto rangeGainInside = std::find_if(rangeGainProject.clips.begin(), rangeGainProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
        return clip.id == "range-gain-inside";
    });
    assert(rangeGainInside != rangeGainProject.clips.end());
    assert(rangeGainInside->gainDb == 1.0f);
    assert(!neuracoust::daw::adjustClipGainInRange(rangeGainProject, 4.0, 5.0, 1.0f, rangeGainIds));
    assert(rangeGainIds.empty());
    auto rangeMuteProject = neuracoust::daw::defaultProject();
    rangeMuteProject.clips.push_back({"range-mute-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    rangeMuteProject.clips.push_back({"range-mute-inside", "Audio 2", wavPath, 0.35, 0.10, 0.0, 0.0f});
    rangeMuteProject.clips.push_back({"range-mute-locked", "Audio 1", wavPath, 0.30, 0.10, 0.0, 0.0f});
    rangeMuteProject.clips.back().locked = true;
    std::vector<std::string> rangeMuteIds;
    assert(neuracoust::daw::setClipMutedInRange(rangeMuteProject, 0.25, 0.75, true, rangeMuteIds));
    assert(std::find(rangeMuteIds.begin(), rangeMuteIds.end(), "range-mute-wide-split-2") != rangeMuteIds.end());
    assert(std::find(rangeMuteIds.begin(), rangeMuteIds.end(), "range-mute-inside") != rangeMuteIds.end());
    assert(std::find(rangeMuteIds.begin(), rangeMuteIds.end(), "range-mute-locked") == rangeMuteIds.end());
    const auto rangeMuteMiddle = std::find_if(rangeMuteProject.clips.begin(), rangeMuteProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
        return clip.id == "range-mute-wide-split-2";
    });
    assert(rangeMuteMiddle != rangeMuteProject.clips.end());
    assert(rangeMuteMiddle->muted);
    assert(!rangeMuteProject.clips.front().muted);
    assert(!rangeMuteProject.clips[2].muted);
    assert(neuracoust::daw::setClipMutedInRange(rangeMuteProject, 0.25, 0.75, false, rangeMuteIds));
    assert(!rangeMuteMiddle->muted);
    assert(!neuracoust::daw::setClipMutedInRange(rangeMuteProject, 4.0, 5.0, true, rangeMuteIds));
    assert(rangeMuteIds.empty());
    auto rangePolarityProject = neuracoust::daw::defaultProject();
    rangePolarityProject.clips.push_back({"range-pol-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    rangePolarityProject.clips.push_back({"range-pol-inside", "Audio 2", wavPath, 0.35, 0.10, 0.0, 0.0f});
    rangePolarityProject.clips.push_back({"range-pol-locked", "Audio 1", wavPath, 0.30, 0.10, 0.0, 0.0f});
    rangePolarityProject.clips.back().locked = true;
    std::vector<std::string> rangePolarityIds;
    assert(neuracoust::daw::setClipPolarityInvertedInRange(rangePolarityProject, 0.25, 0.75, true, rangePolarityIds));
    assert(std::find(rangePolarityIds.begin(), rangePolarityIds.end(), "range-pol-wide-split-2") != rangePolarityIds.end());
    assert(std::find(rangePolarityIds.begin(), rangePolarityIds.end(), "range-pol-inside") != rangePolarityIds.end());
    assert(std::find(rangePolarityIds.begin(), rangePolarityIds.end(), "range-pol-locked") == rangePolarityIds.end());
    const auto rangePolarityMiddle = std::find_if(rangePolarityProject.clips.begin(), rangePolarityProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
        return clip.id == "range-pol-wide-split-2";
    });
    assert(rangePolarityMiddle != rangePolarityProject.clips.end());
    assert(rangePolarityMiddle->polarityInverted);
    assert(!rangePolarityProject.clips.front().polarityInverted);
    assert(!rangePolarityProject.clips[2].polarityInverted);
    assert(neuracoust::daw::setClipPolarityInvertedInRange(rangePolarityProject, 0.25, 0.75, false, rangePolarityIds));
    assert(!rangePolarityMiddle->polarityInverted);
    assert(!neuracoust::daw::setClipPolarityInvertedInRange(rangePolarityProject, 4.0, 5.0, true, rangePolarityIds));
    assert(rangePolarityIds.empty());
    auto shuffleDeleteProject = neuracoust::daw::defaultProject();
    shuffleDeleteProject.clips.push_back({"shuffle-wide", "Audio 1", wavPath, 0.0, 1.0, 0.0, 0.0f});
    shuffleDeleteProject.clips.push_back({"shuffle-after", "Audio 2", wavPath, 1.20, 0.20, 0.0, 0.0f});
    assert(neuracoust::daw::shuffleDeleteClipRange(shuffleDeleteProject, 0.25, 0.75));
    assert(shuffleDeleteProject.clips.size() == 3);
    assert(shuffleDeleteProject.clips[0].id == "shuffle-wide");
    assert(std::abs(shuffleDeleteProject.clips[0].durationSeconds - 0.25) < 0.0001);
    assert(shuffleDeleteProject.clips[1].id == "shuffle-wide-split");
    assert(std::abs(shuffleDeleteProject.clips[1].startSeconds - 0.25) < 0.0001);
    assert(std::abs(shuffleDeleteProject.clips[1].sourceOffsetSeconds - 0.75) < 0.0001);
    assert(shuffleDeleteProject.clips[2].id == "shuffle-after");
    assert(std::abs(shuffleDeleteProject.clips[2].startSeconds - 0.70) < 0.0001);
    auto rangeCopyProject = neuracoust::daw::defaultProject();
    rangeCopyProject.clips.push_back({"range-copy-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    rangeCopyProject.clips.push_back({"range-copy-late", "Audio 2", wavPath, 0.45, 0.20, 0.0, 0.0f});
    const auto rangeCopies = neuracoust::daw::copyClipRange(rangeCopyProject, 0.25, 0.60);
    assert(rangeCopies.size() == 2);
    assert(rangeCopies[0].id == "range-copy-wide");
    assert(std::abs(rangeCopies[0].startSeconds - 0.0) < 0.0001);
    assert(std::abs(rangeCopies[0].durationSeconds - 0.35) < 0.0001);
    assert(std::abs(rangeCopies[0].sourceOffsetSeconds - 0.35) < 0.0001);
    assert(rangeCopies[1].id == "range-copy-late");
    assert(std::abs(rangeCopies[1].startSeconds - 0.20) < 0.0001);
    assert(std::abs(rangeCopies[1].durationSeconds - 0.15) < 0.0001);
    std::vector<std::string> pastedRangeIds;
    assert(neuracoust::daw::pasteClipRange(rangeCopyProject, rangeCopies, 2.0, pastedRangeIds));
    assert(pastedRangeIds.size() == 2);
    assert(rangeCopyProject.clips.size() == 4);
    assert(std::abs(rangeCopyProject.clips[2].startSeconds - 2.0) < 0.0001);
    assert(std::abs(rangeCopyProject.clips[3].startSeconds - 2.20) < 0.0001);
    assert(!neuracoust::daw::pasteClipRange(rangeCopyProject, {}, 2.0, pastedRangeIds));
    auto cutRangeProject = neuracoust::daw::defaultProject();
    cutRangeProject.clips.push_back({"cut-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    cutRangeProject.clips.push_back({"cut-locked", "Audio 2", wavPath, 0.30, 0.20, 0.0, 0.0f});
    cutRangeProject.clips.back().locked = true;
    std::vector<neuracoust::daw::ClipState> cutCopies;
    assert(neuracoust::daw::cutClipRange(cutRangeProject, 0.25, 0.60, cutCopies));
    assert(cutCopies.size() == 1);
    assert(cutCopies.front().id == "cut-wide");
    assert(std::abs(cutCopies.front().startSeconds - 0.0) < 0.0001);
    assert(std::abs(cutCopies.front().durationSeconds - 0.35) < 0.0001);
    assert(std::abs(cutCopies.front().sourceOffsetSeconds - 0.35) < 0.0001);
    assert(cutRangeProject.clips.size() == 3);
    assert(cutRangeProject.clips[1].id == "cut-wide-split");
    assert(cutRangeProject.clips[2].id == "cut-locked");
    assert(!neuracoust::daw::cutClipRange(cutRangeProject, 3.0, 4.0, cutCopies));
    assert(cutCopies.empty());
    auto duplicateRangeProject = neuracoust::daw::defaultProject();
    duplicateRangeProject.clips.push_back({"duplicate-range-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    duplicateRangeProject.clips.push_back({"duplicate-range-late", "Audio 2", wavPath, 0.45, 0.20, 0.0, 0.0f});
    std::vector<std::string> duplicatedRangeIds;
    assert(neuracoust::daw::duplicateClipRange(duplicateRangeProject, 0.25, 0.60, duplicatedRangeIds));
    assert(duplicatedRangeIds.size() == 2);
    assert(duplicateRangeProject.clips.size() == 4);
    assert(std::abs(duplicateRangeProject.clips[2].startSeconds - 0.60) < 0.0001);
    assert(std::abs(duplicateRangeProject.clips[3].startSeconds - 0.80) < 0.0001);
    assert(!neuracoust::daw::duplicateClipRange(duplicateRangeProject, 5.0, 6.0, duplicatedRangeIds));
    auto trimRangeProject = neuracoust::daw::defaultProject();
    trimRangeProject.clips.push_back({"trim-wide", "Audio 1", wavPath, 0.0, 1.0, 0.10, 0.0f});
    trimRangeProject.clips.push_back({"trim-inside", "Audio 2", wavPath, 0.35, 0.10, 0.0, 0.0f});
    trimRangeProject.clips.push_back({"trim-outside", "Audio 1", wavPath, 1.20, 0.20, 0.0, 0.0f});
    std::vector<std::string> trimKeptIds;
    assert(neuracoust::daw::trimClipRangeToSelection(trimRangeProject, 0.25, 0.75, trimKeptIds));
    assert(trimKeptIds.size() == 2);
    assert(trimRangeProject.clips.size() == 2);
    assert(trimRangeProject.clips[0].id == "trim-wide");
    assert(std::abs(trimRangeProject.clips[0].startSeconds - 0.25) < 0.0001);
    assert(std::abs(trimRangeProject.clips[0].durationSeconds - 0.50) < 0.0001);
    assert(std::abs(trimRangeProject.clips[0].sourceOffsetSeconds - 0.35) < 0.0001);
    assert(trimRangeProject.clips[1].id == "trim-inside");
    assert(std::abs(trimRangeProject.clips[1].startSeconds - 0.35) < 0.0001);
    assert(std::abs(trimRangeProject.clips[1].durationSeconds - 0.10) < 0.0001);
    assert(!neuracoust::daw::trimClipRangeToSelection(trimRangeProject, 2.0, 3.0, trimKeptIds));
    auto quantizeRangeProject = neuracoust::daw::defaultProject();
    quantizeRangeProject.clips.push_back({"quantize-a", "Audio 1", wavPath, 0.26, 0.25, 0.0, 0.0f});
    quantizeRangeProject.clips.push_back({"quantize-b", "Audio 2", wavPath, 0.74, 0.20, 0.0, 0.0f});
    quantizeRangeProject.clips.push_back({"quantize-outside", "Audio 1", wavPath, 1.22, 0.20, 0.0, 0.0f});
    std::vector<std::string> quantizedClipIds;
    assert(neuracoust::daw::quantizeClipStartsInRange(quantizeRangeProject, 0.0, 1.0, 0.25, quantizedClipIds));
    assert(quantizedClipIds.size() == 2);
    assert(std::abs(quantizeRangeProject.clips[0].startSeconds - 0.25) < 0.0001);
    assert(std::abs(quantizeRangeProject.clips[1].startSeconds - 0.75) < 0.0001);
    assert(std::abs(quantizeRangeProject.clips[2].startSeconds - 1.22) < 0.0001);
    assert(!neuracoust::daw::quantizeClipStartsInRange(quantizeRangeProject, 0.0, 1.0, 0.25, quantizedClipIds));
    auto invalidClipboard = clipboardClip;
    invalidClipboard.trackName = "Master";
    assert(!neuracoust::daw::pasteClip(project, invalidClipboard, 0.2, pastedId));
    assert(project.clips.size() == 5);
    assert(neuracoust::daw::deleteClip(project, pastedId));
    assert(project.clips.size() == 4);
    assert(neuracoust::daw::deleteClip(project, duplicateId));
    assert(project.clips.size() == 3);
    assert(!neuracoust::daw::deleteClip(project, duplicateId));
    project.tracks[0].colorHex = "#35BFA8";
    project.tracks[0].inputBus = "Input 1";
    project.tracks[0].outputBus = "Master";
    project.clips[0].gainDb = -6.0f;
    project.tracks[0].sends.push_back({"Bus 1-2", -9.0f, 0.25f, true, true, true});
    project.clips[0].regionName = "Bounce Region";
    project.clips[0].sourceFileUid = "src-bounce-001";
    project.clips[0].colorHex = "#F0B84D";
    project.clips[0].sourceChannels = 1;
    project.clips[0].sourceSampleRate = 100.0;
    project.clips[0].sourceBitsPerSample = 16;
    project.clips[0].sourceFloatingPoint = false;
    project.tempoBpm = 90;
    project.beatSnapEnabled = true;
    project.editMode = "Grid";
    project.gridUnit = "1 beat";
    project.loopEnabled = true;
    project.loopStartSeconds = 0.25;
    project.loopEndSeconds = 0.75;
    project.markers.push_back({"bounce-marker", "Bounce Marker", 0.33});
    project.chordEvents.push_back({"bounce-chord", "Chorus / Fm9", 0.5});
    project.masterInserts.push_back(makeMasterInsert(
        "Bypassed Audit VST3",
        "external-vst3",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Bypassed Audit.vst3",
        true,
        false));
    project.masterInserts.back().dspExecutionMode = "external";
    project.masterInserts.back().assignedDspServerId = "Neuracoust Rack A";
    project.masterInserts.back().serverModuleId = "na.neuracoust.audit-bypass";
    project.masterInserts.back().reportedLatencySamples = 256;
    project.masterInserts.back().dspAvailable = false;
    project.masterInserts.back().dspLastError = "External DSP bypassed but status preserved";
    const auto bounceTrackVst3Path = testTempRoot() / "neuracoust-daw-track-audit.vst3";
    std::filesystem::create_directories(bounceTrackVst3Path);
    project.tracks[0].inserts.push_back(makeTrackInsert("Track Audit VST3", "VST3", bounceTrackVst3Path.string()));
    const auto bouncePath = (testTempRoot() / "neuracoust-daw-bounce.wav").string();
    const auto bounce = neuracoust::daw::bounceProjectToWav(project, bouncePath);
    assert(bounce.ok);
    assert(bounce.message.find("saved but not rendered") == std::string::npos);
    assert(!bounce.manifestPath.empty());
    assert(bounce.levelStats.peakLeft > 0.0f || bounce.levelStats.peakRight > 0.0f);
    assert(!bounce.levelStats.nearSilent);
    assert(!std::filesystem::exists(std::filesystem::path(bouncePath + ".writing")));
    assert(!std::filesystem::exists(std::filesystem::path(bounce.manifestPath + ".writing")));
    assert(readWavBitsPerSample(bouncePath) == 24);
    neuracoust::daw::WavAudioData bounced;
    assert(neuracoust::daw::readPcmWavFile(bouncePath, bounced, error));
    assert(bounced.channels == 2);
    assert(bounced.frameCount() > 0);
    {
        std::ifstream bounceManifest(bounce.manifestPath, std::ios::binary);
        std::string bounceManifestText((std::istreambuf_iterator<char>(bounceManifest)), std::istreambuf_iterator<char>());
        assert(bounceManifestText.find("neuracoust-daw-bounce-v1") != std::string::npos);
        assert(bounceManifestText.find("\"renderType\": \"stereo-master-bounce\"") != std::string::npos);
        assert(bounceManifestText.find("\"bounceMode\": \"offline\"") != std::string::npos);
        assert(bounceManifestText.find("\"rangeMode\": \"full-project\"") != std::string::npos);
        assert(bounceManifestText.find("\"renderStartSeconds\": 0") != std::string::npos);
        assert(bounceManifestText.find("\"renderEndSeconds\":") != std::string::npos);
        assert(bounceManifestText.find("\"renderDurationSeconds\":") != std::string::npos);
        assert(bounceManifestText.find("\"renderElapsedSeconds\":") != std::string::npos);
        assert(bounceManifestText.find("\"realtimeRequested\": false") != std::string::npos);
        assert(bounceManifestText.find("\"realtimePacingApplied\": false") != std::string::npos);
        assert(bounceManifestText.find("\"realtimePacingTargetSeconds\": 0") != std::string::npos);
        assert(bounceManifestText.find("\"bitDepth\": 24") != std::string::npos);
        assert(bounceManifestText.find("\"externalSidechainBusCount\": 0") != std::string::npos);
        assert(bounceManifestText.find("\"externalSidechainDuckingEnabled\": true") != std::string::npos);
        assert(bounceManifestText.find("\"peakCeilingGuardEnabled\": false") != std::string::npos);
        assert(bounceManifestText.find("\"peakCeilingDbfs\": -1") != std::string::npos);
        assert(bounceManifestText.find("\"includesMonitorDsp\": true") != std::string::npos);
        assert(bounceManifestText.find("\"monitorDspRenderPath\": \"mixer-graph-monitor-route\"") != std::string::npos);
        assert(bounceManifestText.find("\"activeVst3TrackInserts\": 1") != std::string::npos);
        assert(bounceManifestText.find("\"trackVst3AudioRendered\": true") != std::string::npos);
        assert(bounceManifestText.find("\"trackVst3RenderStatus\": \"direct-master-output-rendered\"") != std::string::npos);
        assert(bounceManifestText.find("Audio 1: Track Audit VST3") != std::string::npos);
        assert(bounceManifestText.find("\"activeVst3TrackInsertLabels\": [\"Audio 1: Track Audit VST3\"]") != std::string::npos);
        assert(bounceManifestText.find("\"monitorDspModules\"") != std::string::npos);
        assert(bounceManifestText.find("\"activeTargetSlot\":1") != std::string::npos);
        assert(bounceManifestText.find("\"realModel\":\"Real Speaker: Avantone CLA-10A (NF)\"") != std::string::npos);
        assert(bounceManifestText.find("\"targetModelB\":\"Speaker B: Avantone CLA-10A (NF)\"") != std::string::npos);
        assert(bounceManifestText.find("\"speakerOutputA\":\"Main 1-2\"") != std::string::npos);
        assert(bounceManifestText.find("\"speakerOutputB\":\"Output 3-4\"") != std::string::npos);
        assert(bounceManifestText.find("\"speakerOutputC\":\"Output 5-6\"") != std::string::npos);
        assert(bounceManifestText.find("\"streamingPreview\":\"YouTube\"") != std::string::npos);
        assert(bounceManifestText.find("\"masterInsertChain\"") != std::string::npos);
        assert(bounceManifestText.find("\"pluginName\":\"Neuracoust Monitor DSP\"") == std::string::npos);
        assert(bounceManifestText.find("\"pluginName\":\"Bypassed Audit VST3\"") != std::string::npos);
        assert(bounceManifestText.find("\"bypassed\":true") != std::string::npos);
        assert(bounceManifestText.find("\"activeInRender\":false") != std::string::npos);
        assert(bounceManifestText.find("\"dspExecutionMode\":\"external\"") != std::string::npos);
        assert(bounceManifestText.find("\"assignedDspServerId\":\"Neuracoust Rack A\"") != std::string::npos);
        assert(bounceManifestText.find("\"serverModuleId\":\"na.neuracoust.audit-bypass\"") != std::string::npos);
        assert(bounceManifestText.find("\"reportedLatencySamples\":256") != std::string::npos);
        assert(bounceManifestText.find("\"dspAvailable\":false") != std::string::npos);
        assert(bounceManifestText.find("\"dspLastError\":\"External DSP bypassed but status preserved\"") != std::string::npos);
        assert(bounceManifestText.find("\"projectHealth\"") != std::string::npos);
        assert(bounceManifestText.find("\"summary\": \"Project health:") != std::string::npos);
        assert(bounceManifestText.find("\"missingVst3Inserts\": 1") != std::string::npos);
        assert(bounceManifestText.find("\"trackInserts\": 1") != std::string::npos);
        assert(bounceManifestText.find("\"vst3TrackInserts\": 1") != std::string::npos);
        assert(bounceManifestText.find("\"activeVst3TrackInserts\": 1") != std::string::npos);
        assert(bounceManifestText.find("\"mutedAudioTracks\":") != std::string::npos);
        assert(bounceManifestText.find("\"soloedAudioTracks\":") != std::string::npos);
        assert(bounceManifestText.find("\"mutedClips\":") != std::string::npos);
        assert(bounceManifestText.find("\"timelineContext\"") != std::string::npos);
        assert(bounceManifestText.find("\"tempoBpm\": 90") != std::string::npos);
        assert(bounceManifestText.find("\"beatSnapEnabled\": true") != std::string::npos);
        assert(bounceManifestText.find("\"editMode\": \"Grid\"") != std::string::npos);
        assert(bounceManifestText.find("\"gridUnit\": \"1 beat\"") != std::string::npos);
        assert(bounceManifestText.find("\"snapQuantumSeconds\":") != std::string::npos);
        assert(bounceManifestText.find("\"projectLoopEnabled\": true") != std::string::npos);
        assert(bounceManifestText.find("\"renderedLoop\": false") != std::string::npos);
        assert(bounceManifestText.find("\"name\":\"Bounce Marker\"") != std::string::npos);
        assert(bounceManifestText.find("\"chordSections\"") != std::string::npos);
        assert(bounceManifestText.find("\"name\":\"Chorus / Fm9\"") != std::string::npos);
        assert(bounceManifestText.find("\"trackMixSnapshot\"") != std::string::npos);
        assert(bounceManifestText.find("\"name\":\"Audio 1\"") != std::string::npos);
        assert(bounceManifestText.find("\"trackType\":\"audio\"") != std::string::npos);
        assert(bounceManifestText.find("\"colorHex\":\"#35BFA8\"") != std::string::npos);
        assert(bounceManifestText.find("\"inputBus\":\"Input 1\"") != std::string::npos);
        assert(bounceManifestText.find("\"outputBus\":\"Master\"") != std::string::npos);
        assert(bounceManifestText.find("\"sends\":[") != std::string::npos);
        assert(bounceManifestText.find("\"busName\":\"Bus 1-2\"") != std::string::npos);
        assert(bounceManifestText.find("\"preFader\":true") != std::string::npos);
        assert(bounceManifestText.find("\"exportable\":true") != std::string::npos);
        assert(bounceManifestText.find("\"clipCount\":3") != std::string::npos);
        assert(bounceManifestText.find("\"clips\":[") != std::string::npos);
        assert(bounceManifestText.find("\"id\":\"clip-1\"") != std::string::npos);
        assert(bounceManifestText.find("\"trackName\":\"Audio 1\"") != std::string::npos);
        assert(bounceManifestText.find("\"regionName\":\"Bounce Region\"") != std::string::npos);
        assert(bounceManifestText.find("\"sourceFileUid\":\"src-bounce-001\"") != std::string::npos);
        assert(bounceManifestText.find("\"sourceChannels\":1") != std::string::npos);
        assert(bounceManifestText.find("\"sourceSampleRate\":100") != std::string::npos);
        assert(bounceManifestText.find("\"sourceBitsPerSample\":16") != std::string::npos);
        assert(bounceManifestText.find("\"sourceFloatingPoint\":false") != std::string::npos);
        assert(bounceManifestText.find("\"colorHex\":\"#F0B84D\"") != std::string::npos);
        assert(bounceManifestText.find("\"sourcePath\":\"" + jsonStringFragment(wavPath) + "\"") != std::string::npos);
        assert(bounceManifestText.find("\"startSeconds\":0") != std::string::npos);
        assert(bounceManifestText.find("\"durationSeconds\":") != std::string::npos);
        assert(bounceManifestText.find("\"sourceOffsetSeconds\":0") != std::string::npos);
        assert(bounceManifestText.find("\"fadeInSeconds\":") != std::string::npos);
        assert(bounceManifestText.find("\"fadeOutSeconds\":") != std::string::npos);
        assert(bounceManifestText.find("\"fadeInCurve\":") != std::string::npos);
        assert(bounceManifestText.find("\"fadeOutCurve\":") != std::string::npos);
        assert(bounceManifestText.find("\"muted\":") != std::string::npos);
        assert(bounceManifestText.find("\"polarityInverted\":") != std::string::npos);
        assert(bounceManifestText.find("\"missingSource\":false") != std::string::npos);
        assert(bounceManifestText.find("\"recordArmed\":") != std::string::npos);
        assert(bounceManifestText.find("\"inserts\":[") != std::string::npos);
        assert(bounceManifestText.find("\"activeTrackVst3PendingRenderHost\":true") != std::string::npos);
        assert(bounceManifestText.find("\"rmsRightDb\"") != std::string::npos);
        assert(bounceManifestText.find("\"nearSilent\": false") != std::string::npos);
    }
    auto sidechainBounceProject = neuracoust::daw::defaultProject();
    sidechainBounceProject.sampleRate = 100.0;
    sidechainBounceProject.bitDepth = 32;
    sidechainBounceProject.monitorModules.clear();
    sidechainBounceProject.clips.clear();
    sidechainBounceProject.clips.push_back({"clip-sidechain-main", "Audio 1", wavPath, 0.0, 0.05, 0.0, 0.0f});
    neuracoust::daw::WavAudioData sidechainKey;
    sidechainKey.channels = 2;
    sidechainKey.sampleRate = 100;
    sidechainKey.bitsPerSample = 32;
    sidechainKey.floatingPoint = true;
    sidechainKey.interleavedSamples.assign(10u, 0.9f);
    neuracoust::daw::BounceOptions sidechainBounceOptions;
    sidechainBounceOptions.externalSidechainBuses.push_back({"External Sidechain", sidechainKey, 0.0, 0.0});
    auto sidechainDryOptions = sidechainBounceOptions;
    sidechainDryOptions.externalSidechainDuckingEnabled = false;
    const auto sidechainDryBounce = neuracoust::daw::bounceProjectToWav(
        sidechainBounceProject,
        (testTempRoot() / "neuracoust-daw-sidechain-dry.wav").string(),
        sidechainDryOptions);
    const auto sidechainDuckedBounce = neuracoust::daw::bounceProjectToWav(
        sidechainBounceProject,
        (testTempRoot() / "neuracoust-daw-sidechain-ducked.wav").string(),
        sidechainBounceOptions);
    assert(sidechainDryBounce.ok);
    assert(sidechainDuckedBounce.ok);
    assert(sidechainDuckedBounce.levelStats.rmsLeft < sidechainDryBounce.levelStats.rmsLeft * 0.75f);
    {
        std::ifstream sidechainManifest(sidechainDuckedBounce.manifestPath, std::ios::binary);
        std::string sidechainManifestText((std::istreambuf_iterator<char>(sidechainManifest)), std::istreambuf_iterator<char>());
        assert(sidechainManifestText.find("\"externalSidechainBusCount\": 1") != std::string::npos);
        assert(sidechainManifestText.find("\"externalSidechainDuckingEnabled\": true") != std::string::npos);
    }
    auto peakGuardProject = sidechainBounceProject;
    peakGuardProject.clips.front().gainDb = 48.0f;
    neuracoust::daw::BounceOptions peakGuardOptions;
    peakGuardOptions.peakCeilingGuardEnabled = true;
    peakGuardOptions.peakCeilingDbfs = -6.0f;
    const auto peakGuardBounce = neuracoust::daw::bounceProjectToWav(
        peakGuardProject,
        (testTempRoot() / "neuracoust-daw-peak-guard.wav").string(),
        peakGuardOptions);
    assert(peakGuardBounce.ok);
    assert(std::max(peakGuardBounce.levelStats.peakLeft, peakGuardBounce.levelStats.peakRight) <= 0.502f);
    {
        std::ifstream peakGuardManifest(peakGuardBounce.manifestPath, std::ios::binary);
        std::string peakGuardManifestText((std::istreambuf_iterator<char>(peakGuardManifest)), std::istreambuf_iterator<char>());
        assert(peakGuardManifestText.find("\"peakCeilingGuardEnabled\": true") != std::string::npos);
        assert(peakGuardManifestText.find("\"peakCeilingDbfs\": -6") != std::string::npos);
    }
    auto selectionBounceProject = project;
    selectionBounceProject.editSelectionEnabled = true;
    selectionBounceProject.editSelectionStartSeconds = 0.01;
    selectionBounceProject.editSelectionEndSeconds = 0.03;
    neuracoust::daw::BounceOptions selectionBounceOptions;
    selectionBounceOptions.rangeMode = neuracoust::daw::BounceRangeMode::EditSelection;
    const auto selectionBouncePath = (testTempRoot() / "neuracoust-daw-selection-bounce.wav").string();
    const auto selectionBounce = neuracoust::daw::bounceProjectToWav(selectionBounceProject, selectionBouncePath, selectionBounceOptions);
    assert(selectionBounce.ok);
    assert(selectionBounce.durationSeconds > 0.019 && selectionBounce.durationSeconds < 0.021);
    neuracoust::daw::WavAudioData selectionBounced;
    assert(neuracoust::daw::readPcmWavFile(selectionBouncePath, selectionBounced, error));
    assert(selectionBounced.channels == 2);
    const auto expectedSelectionFrames = static_cast<size_t>(std::round(selectionBounced.sampleRate * 0.02));
    assert(selectionBounced.frameCount() == expectedSelectionFrames);
    {
        std::ifstream selectionManifest(selectionBounce.manifestPath, std::ios::binary);
        std::string selectionManifestText((std::istreambuf_iterator<char>(selectionManifest)), std::istreambuf_iterator<char>());
        assert(selectionManifestText.find("\"rangeMode\": \"edit-selection\"") != std::string::npos);
        assert(selectionManifestText.find("\"renderStartSeconds\": 0.01") != std::string::npos);
        assert(selectionManifestText.find("\"renderEndSeconds\": 0.03") != std::string::npos);
        assert(selectionManifestText.find("\"renderDurationSeconds\": 0.02") != std::string::npos);
    }
    auto invalidSelectionBounceProject = project;
    invalidSelectionBounceProject.editSelectionEnabled = false;
    invalidSelectionBounceProject.editSelectionStartSeconds = 0.0;
    invalidSelectionBounceProject.editSelectionEndSeconds = 0.0;
    neuracoust::daw::BounceOptions invalidSelectionBounceOptions;
    invalidSelectionBounceOptions.rangeMode = neuracoust::daw::BounceRangeMode::EditSelection;
    const auto invalidSelectionBounce = neuracoust::daw::bounceProjectToWav(
        invalidSelectionBounceProject,
        (testTempRoot() / "neuracoust-daw-invalid-selection-bounce.wav").string(),
        invalidSelectionBounceOptions);
    assert(!invalidSelectionBounce.ok);
    assert(invalidSelectionBounce.message.find("No valid edit selection") != std::string::npos);
    neuracoust::daw::BounceOptions realtimeBounceOptions;
    realtimeBounceOptions.renderMode = neuracoust::daw::BounceRenderMode::Realtime;
    const auto realtimeBouncePath = (testTempRoot() / "neuracoust-daw-bounce-realtime.wav").string();
    const auto realtimeBounce = neuracoust::daw::bounceProjectToWav(project, realtimeBouncePath, realtimeBounceOptions);
    assert(realtimeBounce.ok);
    assert(realtimeBounce.message.find("Realtime bounce complete") != std::string::npos);
    {
        std::ifstream realtimeManifest(realtimeBounce.manifestPath, std::ios::binary);
        std::string realtimeManifestText((std::istreambuf_iterator<char>(realtimeManifest)), std::istreambuf_iterator<char>());
        assert(realtimeManifestText.find("\"bounceMode\": \"realtime\"") != std::string::npos);
        assert(realtimeManifestText.find("\"realtimeRequested\": true") != std::string::npos);
        assert(realtimeManifestText.find("\"realtimePacingApplied\": true") != std::string::npos);
        assert(realtimeManifestText.find("\"realtimePacingTargetSeconds\":") != std::string::npos);
        assert(realtimeManifestText.find("\"renderElapsedSeconds\":") != std::string::npos);
        assert(realtimeManifestText.find("\"externalHardwareTimingReserved\": true") != std::string::npos);
    }
    auto float32BounceProject = project;
    float32BounceProject.bitDepth = 32;
    const auto float32BouncePath = (testTempRoot() / "neuracoust-daw-bounce-f32.wav").string();
    const auto float32Bounce = neuracoust::daw::bounceProjectToWav(float32BounceProject, float32BouncePath);
    assert(float32Bounce.ok);
    assert(readWavFormatTag(float32BouncePath) == 3);
    assert(readWavBitsPerSample(float32BouncePath) == 32);
    neuracoust::daw::WavAudioData float32Bounced;
    assert(neuracoust::daw::readPcmWavFile(float32BouncePath, float32Bounced, error));
    assert(float32Bounced.floatingPoint);
    assert(float32Bounced.bitsPerSample == 32);
    {
        std::ifstream float32Manifest(float32Bounce.manifestPath, std::ios::binary);
        std::string float32ManifestText((std::istreambuf_iterator<char>(float32Manifest)), std::istreambuf_iterator<char>());
        assert(float32ManifestText.find("\"bitDepth\": 32") != std::string::npos);
    }
    auto float64BounceProject = project;
    float64BounceProject.bitDepth = 64;
    neuracoust::daw::BounceOptions float64BounceOptions;
    float64BounceOptions.ditherEnabled = true;
    float64BounceOptions.sourceBitDepth = 64;
    const auto float64BouncePath = (testTempRoot() / "neuracoust-daw-bounce-f64.wav").string();
    const auto float64Bounce = neuracoust::daw::bounceProjectToWav(float64BounceProject, float64BouncePath, float64BounceOptions);
    assert(float64Bounce.ok);
    assert(readWavFormatTag(float64BouncePath) == 3);
    assert(readWavBitsPerSample(float64BouncePath) == 64);
    neuracoust::daw::WavAudioData float64Bounced;
    assert(neuracoust::daw::readPcmWavFile(float64BouncePath, float64Bounced, error));
    assert(float64Bounced.floatingPoint);
    assert(float64Bounced.bitsPerSample == 64);
    {
        std::ifstream float64Manifest(float64Bounce.manifestPath, std::ios::binary);
        std::string float64ManifestText((std::istreambuf_iterator<char>(float64Manifest)), std::istreambuf_iterator<char>());
        assert(float64ManifestText.find("\"bitDepth\": 64") != std::string::npos);
        assert(float64ManifestText.find("\"ditherEnabled\": false") != std::string::npos);
        assert(float64ManifestText.find("\"ditherReason\": \"not-needed-for-floating-or-high-resolution-export\"") != std::string::npos);
    }
    auto ditherDownProject = project;
    ditherDownProject.bitDepth = 16;
    neuracoust::daw::BounceOptions ditherDownOptions;
    ditherDownOptions.ditherEnabled = true;
    ditherDownOptions.sourceBitDepth = 64;
    const auto ditherDownPath = (testTempRoot() / "neuracoust-daw-bounce-dither-16.wav").string();
    const auto ditherDownBounce = neuracoust::daw::bounceProjectToWav(ditherDownProject, ditherDownPath, ditherDownOptions);
    assert(ditherDownBounce.ok);
    assert(readWavBitsPerSample(ditherDownPath) == 16);
    {
        std::ifstream ditherManifest(ditherDownBounce.manifestPath, std::ios::binary);
        std::string ditherManifestText((std::istreambuf_iterator<char>(ditherManifest)), std::istreambuf_iterator<char>());
        assert(ditherManifestText.find("\"sourceBitDepth\": 64") != std::string::npos);
        assert(ditherManifestText.find("\"ditherRequested\": true") != std::string::npos);
        assert(ditherManifestText.find("\"ditherEnabled\": true") != std::string::npos);
        assert(ditherManifestText.find("\"ditherAlgorithm\": \"Neuracoust Resolution Guard shaped TPDF v1\"") != std::string::npos);
        assert(ditherManifestText.find("\"ditherReason\": \"high-resolution-to-fixed-pcm\"") != std::string::npos);
    }
    neuracoust::daw::BounceOptions ditherOffOptions;
    ditherOffOptions.ditherEnabled = false;
    ditherOffOptions.sourceBitDepth = 64;
    const auto ditherOffPath = (testTempRoot() / "neuracoust-daw-bounce-dither-off.wav").string();
    const auto ditherOffBounce = neuracoust::daw::bounceProjectToWav(ditherDownProject, ditherOffPath, ditherOffOptions);
    assert(ditherOffBounce.ok);
    {
        std::ifstream ditherOffManifest(ditherOffBounce.manifestPath, std::ios::binary);
        std::string ditherOffManifestText((std::istreambuf_iterator<char>(ditherOffManifest)), std::istreambuf_iterator<char>());
        assert(ditherOffManifestText.find("\"ditherRequested\": false") != std::string::npos);
        assert(ditherOffManifestText.find("\"ditherEnabled\": false") != std::string::npos);
        assert(ditherOffManifestText.find("\"ditherReason\": \"disabled-by-user\"") != std::string::npos);
    }
    auto silentProject = neuracoust::daw::defaultProject();
    silentProject.sampleRate = 100.0;
    silentProject.monitorModules.clear();
    const auto silentBouncePath = (testTempRoot() / "neuracoust-daw-silent-bounce.wav").string();
    const auto silentBounce = neuracoust::daw::bounceProjectToWav(silentProject, silentBouncePath);
    assert(silentBounce.ok);
    assert(silentBounce.levelStats.nearSilent);
    assert(silentBounce.message.find("near-silent") != std::string::npos);
    {
        std::ifstream silentManifest(silentBounce.manifestPath, std::ios::binary);
        std::string silentManifestText((std::istreambuf_iterator<char>(silentManifest)), std::istreambuf_iterator<char>());
        assert(silentManifestText.find("\"nearSilent\": true") != std::string::npos);
    }
    auto clippingProject = neuracoust::daw::defaultProject();
    clippingProject.sampleRate = 100.0;
    clippingProject.monitorModules.clear();
    clippingProject.clips.push_back({"clip-hot", "Audio 1", wavPath, 0.0, 0.05, 0.0, 48.0f});
    const auto clippingBouncePath = (testTempRoot() / "neuracoust-daw-clipping-bounce.wav").string();
    const auto clippingBounce = neuracoust::daw::bounceProjectToWav(clippingProject, clippingBouncePath);
    assert(clippingBounce.ok);
    assert(clippingBounce.levelStats.clippingDetected);
    assert(clippingBounce.levelStats.clippedSampleCount > 0);
    assert(!clippingBounce.levelStats.nearSilent);
    assert(clippingBounce.message.find("clipping detected") != std::string::npos);
    {
        std::ifstream clippingManifest(clippingBounce.manifestPath, std::ios::binary);
        std::string clippingManifestText((std::istreambuf_iterator<char>(clippingManifest)), std::istreambuf_iterator<char>());
        assert(clippingManifestText.find("\"clippingDetected\": true") != std::string::npos);
    }
    auto healthWarningBounceProject = neuracoust::daw::defaultProject();
    healthWarningBounceProject.sampleRate = 100.0;
    healthWarningBounceProject.monitorModules.clear();
    healthWarningBounceProject.tracks[0].solo = true;
    healthWarningBounceProject.tracks[1].muted = true;
    healthWarningBounceProject.clips.push_back({"health-warning-a", "Audio 1", wavPath, 0.0, 0.05, 0.0, 0.0f});
    healthWarningBounceProject.clips.push_back({"health-warning-b", "Audio 2", wavPath, 0.0, 0.05, 0.0, 0.0f});
    healthWarningBounceProject.clips.back().muted = true;
    const auto healthWarningBouncePath = (testTempRoot() / "neuracoust-daw-health-warning-bounce.wav").string();
    const auto healthWarningBounce = neuracoust::daw::bounceProjectToWav(healthWarningBounceProject, healthWarningBouncePath);
    assert(healthWarningBounce.ok);
    assert(healthWarningBounce.message.find("muted track") != std::string::npos);
    assert(healthWarningBounce.message.find("soloed track") != std::string::npos);
    assert(healthWarningBounce.message.find("muted clip") != std::string::npos);

    const auto stemRoot = testTempRoot() / "neuracoust-daw-stem-export";
    std::filesystem::remove_all(stemRoot);
    neuracoust::daw::WavAudioData stemSourceA;
    stemSourceA.channels = 1;
    stemSourceA.sampleRate = 100;
    stemSourceA.interleavedSamples.assign(10, 0.25f);
    neuracoust::daw::WavAudioData stemSourceB;
    stemSourceB.channels = 1;
    stemSourceB.sampleRate = 100;
    stemSourceB.interleavedSamples.assign(10, -0.5f);
    const auto stemSourceAPath = stemRoot / "source-a.wav";
    const auto stemSourceBPath = stemRoot / "source-b.wav";
    std::filesystem::create_directories(stemRoot);
    assert(neuracoust::daw::writePcm16WavFile(stemSourceAPath, stemSourceA, error));
    assert(neuracoust::daw::writePcm16WavFile(stemSourceBPath, stemSourceB, error));
    auto stemProject = neuracoust::daw::defaultProject();
    stemProject.sampleRate = 100.0;
    stemProject.tempoBpm = 135;
    stemProject.beatSnapEnabled = true;
    stemProject.editMode = "Grid";
    stemProject.gridUnit = "1/4 beat";
    stemProject.loopEnabled = true;
    stemProject.loopStartSeconds = 0.01;
    stemProject.loopEndSeconds = 0.15;
    stemProject.markers.push_back({"stem-marker", "Stem Marker", 0.05});
    assert(neuracoust::daw::addAudioTrack(stemProject) == "Audio 3");
    stemProject.tracks[0].name = "리드 보컬:Main";
    stemProject.tracks[1].name = "Drums/Room?";
    stemProject.tracks[2].name = "Drums:Room?";
    stemProject.tracks[0].volumeDb = -1.0f;
    stemProject.tracks[0].pan = -1.0f;
    stemProject.tracks[1].pan = 1.0f;
    stemProject.tracks[2].pan = 0.0f;
    stemProject.tracks[0].colorHex = "#4B84E8";
    stemProject.tracks[0].inputBus = "Input 7";
    stemProject.tracks[0].outputBus = "Master";
    stemProject.clips.push_back({"stem-a", "리드 보컬:Main", stemSourceAPath.string(), 0.0, 0.10, 0.0, 0.0f});
    stemProject.clips.push_back({"stem-b", "Drums/Room?", stemSourceBPath.string(), 0.05, 0.10, 0.0, 0.0f});
    stemProject.clips.push_back({"stem-c", "Drums:Room?", stemSourceAPath.string(), 0.02, 0.05, 0.0, 0.0f});
    stemProject.clips[0].regionName = "Stem Vocal Region";
    stemProject.clips[0].sourceFileUid = "src-stem-vocal-001";
    stemProject.clips[0].colorHex = "#D86BA6";
    stemProject.clips[0].sourceChannels = 1;
    stemProject.clips[0].sourceSampleRate = 100.0;
    stemProject.clips[0].sourceBitsPerSample = 16;
    stemProject.clips[0].sourceFloatingPoint = false;
    const auto stemExport = neuracoust::daw::exportProjectTrackStems(stemProject, stemRoot / "Stems");
    assert(stemExport.ok);
    assert(stemExport.exportedStems == 3);
    assert(stemExport.outputPaths.size() == 3);
    assert(stemExport.stemLevelStats.size() == 3);
    assert(stemExport.missingMediaClipIds.empty());
    assert(stemExport.message == "Stem export complete.");
    assert(!std::filesystem::exists(std::filesystem::path(stemExport.manifestPath + ".writing")));
    for (const auto& path : stemExport.outputPaths) {
        assert(!std::filesystem::exists(std::filesystem::path(path + ".writing")));
    }
    neuracoust::daw::WavAudioData stemA;
    neuracoust::daw::WavAudioData stemB;
    neuracoust::daw::WavAudioData stemC;
    assert(neuracoust::daw::readPcmWavFile(stemRoot / "Stems" / "리드 보컬_Main.wav", stemA, error));
    assert(neuracoust::daw::readPcmWavFile(stemRoot / "Stems" / "Drums_Room.wav", stemB, error));
    assert(neuracoust::daw::readPcmWavFile(stemRoot / "Stems" / "Drums_Room 2.wav", stemC, error));
    assert(readWavBitsPerSample(stemRoot / "Stems" / "리드 보컬_Main.wav") == 24);
    assert(readWavBitsPerSample(stemRoot / "Stems" / "Drums_Room.wav") == 24);
    assert(readWavBitsPerSample(stemRoot / "Stems" / "Drums_Room 2.wav") == 24);
    assert(!stemExport.manifestPath.empty());
    {
        std::ifstream stemManifest(stemExport.manifestPath, std::ios::binary);
        std::string stemManifestText((std::istreambuf_iterator<char>(stemManifest)), std::istreambuf_iterator<char>());
        assert(stemManifestText.find("neuracoust-daw-stem-export-v1") != std::string::npos);
        assert(stemManifestText.find("\"renderType\": \"pre-master-track-stems\"") != std::string::npos);
        assert(stemManifestText.find("\"bitDepth\": 24") != std::string::npos);
        assert(stemManifestText.find("\"excludesMonitorDsp\": true") != std::string::npos);
        assert(stemManifestText.find("\"excludedMonitorDspModules\"") != std::string::npos);
        assert(stemManifestText.find("\"excludedMasterInsertChain\"") != std::string::npos);
        assert(stemManifestText.find("\"projectHealth\"") != std::string::npos);
        assert(stemManifestText.find("\"clips\": 3") != std::string::npos);
        assert(stemManifestText.find("\"overlappingClipPairs\": 0") != std::string::npos);
        assert(stemManifestText.find("\"mutedAudioTracks\":") != std::string::npos);
        assert(stemManifestText.find("\"soloedAudioTracks\":") != std::string::npos);
        assert(stemManifestText.find("\"mutedClips\":") != std::string::npos);
        assert(stemManifestText.find("\"timelineContext\"") != std::string::npos);
        assert(stemManifestText.find("\"tempoBpm\": 135") != std::string::npos);
        assert(stemManifestText.find("\"beatSnapEnabled\": true") != std::string::npos);
        assert(stemManifestText.find("\"editMode\": \"Grid\"") != std::string::npos);
        assert(stemManifestText.find("\"gridUnit\": \"1/4 beat\"") != std::string::npos);
        assert(stemManifestText.find("\"snapQuantumSeconds\":") != std::string::npos);
        assert(stemManifestText.find("\"projectLoopEnabled\": true") != std::string::npos);
        assert(stemManifestText.find("\"renderedLoop\": false") != std::string::npos);
        assert(stemManifestText.find("\"name\":\"Stem Marker\"") != std::string::npos);
        assert(stemManifestText.find("\"missingMediaRenderedAsSilence\": false") != std::string::npos);
        assert(stemManifestText.find("\"missingMediaClipIds\": []") != std::string::npos);
        assert(stemManifestText.find("\"trackName\":\"리드 보컬:Main\"") != std::string::npos);
        assert(stemManifestText.find("\"file\":\"리드 보컬_Main.wav\"") != std::string::npos);
        assert(stemManifestText.find("\"clipCount\":1") != std::string::npos);
        assert(stemManifestText.find("\"trackState\"") != std::string::npos);
        assert(stemManifestText.find("\"trackType\":\"audio\"") != std::string::npos);
        assert(stemManifestText.find("\"colorHex\":\"#4B84E8\"") != std::string::npos);
        assert(stemManifestText.find("\"inputBus\":\"Input 7\"") != std::string::npos);
        assert(stemManifestText.find("\"outputBus\":\"Master\"") != std::string::npos);
        assert(stemManifestText.find("\"clips\":[") != std::string::npos);
        assert(stemManifestText.find("\"id\":\"stem-a\"") != std::string::npos);
        assert(stemManifestText.find("\"regionName\":\"Stem Vocal Region\"") != std::string::npos);
        assert(stemManifestText.find("\"sourceFileUid\":\"src-stem-vocal-001\"") != std::string::npos);
        assert(stemManifestText.find("\"sourceChannels\":1") != std::string::npos);
        assert(stemManifestText.find("\"sourceSampleRate\":100") != std::string::npos);
        assert(stemManifestText.find("\"sourceBitsPerSample\":16") != std::string::npos);
        assert(stemManifestText.find("\"sourceFloatingPoint\":false") != std::string::npos);
        assert(stemManifestText.find("\"colorHex\":\"#D86BA6\"") != std::string::npos);
        assert(stemManifestText.find("\"sourcePath\":\"" + jsonStringFragment(stemSourceAPath.string()) + "\"") != std::string::npos);
        assert(stemManifestText.find("\"startSeconds\":0") != std::string::npos);
        assert(stemManifestText.find("\"durationSeconds\":0.1") != std::string::npos);
        assert(stemManifestText.find("\"sourceOffsetSeconds\":0") != std::string::npos);
        assert(stemManifestText.find("\"fadeInSeconds\":0") != std::string::npos);
        assert(stemManifestText.find("\"fadeOutSeconds\":0") != std::string::npos);
        assert(stemManifestText.find("\"polarityInverted\":false") != std::string::npos);
        assert(stemManifestText.find("\"missingSource\":false") != std::string::npos);
        assert(stemManifestText.find("\"volumeDb\":-1") != std::string::npos);
        assert(stemManifestText.find("\"pan\":-1") != std::string::npos);
        assert(stemManifestText.find("\"recordArmed\":false") != std::string::npos);
        assert(stemManifestText.find("\"levelStats\"") != std::string::npos);
        assert(stemManifestText.find("\"peakRightDb\"") != std::string::npos);
        assert(stemManifestText.find("\"clippedSampleCount\"") != std::string::npos);
        assert(stemManifestText.find("\"nearSilent\":false") != std::string::npos);
    }
    assert(stemA.frameCount() == 15);
    assert(stemB.frameCount() == 15);
    assert(stemC.frameCount() == 15);
    assert(stemA.interleavedSamples[0] > 0.20f);
    assert(std::abs(stemA.interleavedSamples[1]) < 0.0001f);
    assert(std::abs(stemB.interleavedSamples[0]) < 0.0001f);
    assert(stemB.interleavedSamples[11] < -0.40f);
    assert(stemC.interleavedSamples[4] > 0.10f);
    auto stemHealthWarningProject = stemProject;
    stemHealthWarningProject.tracks[1].solo = true;
    stemHealthWarningProject.clips.front().muted = true;
    const auto stemHealthWarningExport = neuracoust::daw::exportProjectTrackStems(stemHealthWarningProject, stemRoot / "Warning Stems");
    assert(stemHealthWarningExport.ok);
    assert(stemHealthWarningExport.message.find("soloed track") != std::string::npos);
    assert(stemHealthWarningExport.message.find("muted clip") != std::string::npos);
    auto stemMissingMediaProject = stemProject;
    stemMissingMediaProject.clips.push_back({"missing-stem-media", "리드 보컬:Main", (stemRoot / "missing-source.wav").string(), 0.01, 0.04, 0.0, 0.0f});
    const auto stemMissingMediaExport = neuracoust::daw::exportProjectTrackStems(stemMissingMediaProject, stemRoot / "Missing Media Stems");
    assert(stemMissingMediaExport.ok);
    assert(stemMissingMediaExport.missingMediaClipIds.size() == 1);
    assert(stemMissingMediaExport.missingMediaClipIds.front() == "missing-stem-media");
    assert(stemMissingMediaExport.message.find("missing media") != std::string::npos);
    {
        std::ifstream missingStemManifest(stemMissingMediaExport.manifestPath, std::ios::binary);
        std::string missingStemManifestText((std::istreambuf_iterator<char>(missingStemManifest)), std::istreambuf_iterator<char>());
        assert(missingStemManifestText.find("\"missingMediaRenderedAsSilence\": true") != std::string::npos);
        assert(missingStemManifestText.find("\"missing-stem-media\"") != std::string::npos);
        assert(missingStemManifestText.find("\"missingSource\":true") != std::string::npos);
    }
    const auto stemMagicJob = neuracoust::daw::writeStemMagicJobManifest(stemProject.clips.front(),
                                                                        stemRoot / "Stem Magic Test.ndaw",
                                                                        neuracoust::daw::StemMagicJobMode::FourStem);
    assert(stemMagicJob.ok);
    assert(std::filesystem::exists(stemMagicJob.manifestPath));
    assert(std::filesystem::exists(stemMagicJob.progressPath));
    assert(stemMagicJob.outputDirectory.filename().string().find("stem-a-four-stem") != std::string::npos);
    {
        const auto staleDrumsPath = stemMagicJob.outputDirectory / "source-a_Drums.wav";
        const auto staleBassPath = stemMagicJob.outputDirectory / "source-a_Bass.wav";
        assert(neuracoust::daw::writePcm16WavFile(staleDrumsPath, stemSourceA, error));
        assert(neuracoust::daw::writePcm16WavFile(staleBassPath, stemSourceB, error));
        const auto refreshedStemMagicJob = neuracoust::daw::writeStemMagicJobManifest(stemProject.clips.front(),
                                                                                     stemRoot / "Stem Magic Test.ndaw",
                                                                                     neuracoust::daw::StemMagicJobMode::FourStem);
        assert(refreshedStemMagicJob.ok);
        assert(!std::filesystem::exists(staleDrumsPath));
        assert(!std::filesystem::exists(staleBassPath));
    }
    {
        std::ifstream stemMagicManifest(stemMagicJob.manifestPath, std::ios::binary);
        std::string stemMagicText((std::istreambuf_iterator<char>(stemMagicManifest)), std::istreambuf_iterator<char>());
        assert(stemMagicText.find("com.neuracoust.daw.stem-magic-job.v1") != std::string::npos);
        assert(stemMagicText.find("\"status\": \"queued-for-internal-render\"") != std::string::npos);
        assert(stemMagicText.find("\"engineMode\": \"internal-background\"") != std::string::npos);
        assert(stemMagicText.find("\"engine\": \"Neuracoust Stem Magic Metal\"") != std::string::npos);
        assert(stemMagicText.find("\"mode\": \"four-stem\"") != std::string::npos);
        assert(stemMagicText.find("\"clipId\": \"stem-a\"") != std::string::npos);
        assert(stemMagicText.find("\"trackName\": \"리드 보컬:Main\"") != std::string::npos);
        assert(stemMagicText.find("\"sourcePath\": \"" + jsonStringFragment(stemMagicJob.resolvedSourcePath.string()) + "\"") != std::string::npos);
        assert(stemMagicText.find("\"expectedStems\": [\"Drums\", \"Bass\", \"Other\", \"Vocals\"]") != std::string::npos);
        assert(stemMagicText.find("\"outputDirectory\":") != std::string::npos);
        assert(stemMagicText.find("\"progressPath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimeAppPath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimePythonPath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimePythonExtraPath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimeScriptPath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimeBatchBridgePath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimeMpsServerPath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimeDrumSplitServerPath\":") != std::string::npos);
        assert(stemMagicText.find("\"runtimeModelPath\":") != std::string::npos);
        assert(stemMagicText.find("\"supportsBatchFourStem\":") != std::string::npos);
        assert(stemMagicText.find("\"supportsMpsServer\":") != std::string::npos);
        assert(stemMagicText.find("\"supportsDrumSplitServer\":") != std::string::npos);
        assert(stemMagicText.find("\"batchUsesMetalAcceleration\":") != std::string::npos);
    }
    {
        std::ifstream stemMagicProgress(stemMagicJob.progressPath, std::ios::binary);
        std::string stemMagicProgressText((std::istreambuf_iterator<char>(stemMagicProgress)), std::istreambuf_iterator<char>());
        assert(stemMagicProgressText.find("com.neuracoust.daw.stem-magic-progress.v1") != std::string::npos);
        assert(stemMagicProgressText.find("\"status\": \"queued\"") != std::string::npos);
        assert(stemMagicProgressText.find("\"progress\": 0.0") != std::string::npos);
    }
    const auto drumStemNames = neuracoust::daw::stemMagicExpectedStemNames(neuracoust::daw::StemMagicJobMode::DrumSplit);
    assert(drumStemNames.size() == 4);
    assert(drumStemNames.front() == "Kick");
    assert(drumStemNames.back() == "Toms");
    const auto stemRuntime = neuracoust::daw::resolveStemMagicRuntimeResources(stemMagicJob.runtimeAppPath);
    if (!stemMagicJob.runtimeAppPath.empty()) {
        assert(stemRuntime.ok);
        assert(stemRuntime.supportsBatchFourStem);
        assert(std::filesystem::exists(stemRuntime.pythonPath));
            assert(std::filesystem::exists(stemRuntime.scriptPath));
            if (!stemRuntime.batchBridgePath.empty()) {
                assert(std::filesystem::exists(stemRuntime.batchBridgePath));
            }
            assert(std::filesystem::exists(stemRuntime.modelPath));
            if (stemRuntime.batchUsesMetalAcceleration) {
                assert(stemRuntime.supportsMpsServer);
                assert(std::filesystem::exists(stemRuntime.mpsServerPath));
            }
        if (!stemRuntime.pythonExtraPath.empty()) {
            if (stemRuntime.supportsCoreMlAne) {
                assert(std::filesystem::exists(stemRuntime.pythonExtraPath / "coremltools"));
            }
            if (stemRuntime.supportsMpsServer || stemRuntime.supportsDrumSplitServer || stemRuntime.batchUsesMetalAcceleration) {
                assert(std::filesystem::exists(stemRuntime.pythonExtraPath / "torchgen"));
            }
        }
    }
    auto emptyStemApplyProject = stemProject;
    const auto emptyStemApply = neuracoust::daw::applyStemMagicRenderedStems(emptyStemApplyProject,
                                                                            stemMagicJob,
                                                                            stemProject.clips.front(),
                                                                            neuracoust::daw::StemMagicJobMode::FourStem);
    assert(!emptyStemApply.ok);
	    assert(emptyStemApply.createdTracks == 0);
	    assert(emptyStemApplyProject.tracks.size() == stemProject.tracks.size());
	    assert(emptyStemApply.missingStemPaths.size() == 4);
	    {
	        const std::vector<std::filesystem::path> corruptStemPaths {
	            stemMagicJob.outputDirectory / "source-a_Drums.wav",
	            stemMagicJob.outputDirectory / "source-a_Bass.wav",
	            stemMagicJob.outputDirectory / "source-a_Other.wav",
	            stemMagicJob.outputDirectory / "source-a_Vocals.wav"
	        };
	        for (const auto& corruptStemPath : corruptStemPaths) {
	            std::ofstream corruptStem(corruptStemPath, std::ios::binary | std::ios::trunc);
	            corruptStem << "not a rendered wav stem";
	        }
	        auto corruptStemApplyProject = stemProject;
	        const auto corruptStemApply = neuracoust::daw::applyStemMagicRenderedStems(corruptStemApplyProject,
	                                                                                  stemMagicJob,
	                                                                                  stemProject.clips.front(),
	                                                                                  neuracoust::daw::StemMagicJobMode::FourStem);
	        assert(!corruptStemApply.ok);
	        assert(corruptStemApply.createdTracks == 0);
	        assert(corruptStemApply.missingStemPaths.size() == 4);
	        assert(corruptStemApplyProject.tracks.size() == stemProject.tracks.size());
	        for (const auto& corruptStemPath : corruptStemPaths) {
	            std::filesystem::remove(corruptStemPath);
	        }
	    }

	    const auto renderedDrumsPath = stemMagicJob.outputDirectory / "source-a_Drums.wav";
    const auto renderedBassPath = stemMagicJob.outputDirectory / "source-a_Bass.wav";
    const auto renderedOtherPath = stemMagicJob.outputDirectory / "source-a_Other.wav";
    const auto renderedVocalsPath = stemMagicJob.outputDirectory / "source-a_Vocals.wav";
    assert(neuracoust::daw::writePcm16WavFile(renderedDrumsPath, stemSourceA, error));
    assert(neuracoust::daw::writePcm16WavFile(renderedBassPath, stemSourceB, error));
    auto partialStemApplyProject = stemProject;
    partialStemApplyProject.clips.front().sourceOffsetSeconds = 0.025;
    partialStemApplyProject.clips.front().timeScale = 1.25;
    partialStemApplyProject.clips.front().tempoSyncPolicy = "stretch-to-project";
    partialStemApplyProject.clips.front().pendingTimeStretchToProject = true;
    const auto beforePartialStemTrackCount = partialStemApplyProject.tracks.size();
    const auto beforePartialStemClipCount = partialStemApplyProject.clips.size();
    const auto partialStemApply = neuracoust::daw::applyStemMagicRenderedStems(partialStemApplyProject,
                                                                              stemMagicJob,
                                                                              partialStemApplyProject.clips.front(),
                                                                              neuracoust::daw::StemMagicJobMode::FourStem);
    assert(!partialStemApply.ok);
    assert(partialStemApply.createdTracks == 0);
    assert(partialStemApply.missingStemPaths.size() == 2);
    assert(partialStemApplyProject.tracks.size() == beforePartialStemTrackCount);
    assert(partialStemApplyProject.clips.size() == beforePartialStemClipCount);
    assert(partialStemApply.message.find("not complete") != std::string::npos);
    assert(neuracoust::daw::writePcm16WavFile(renderedOtherPath, stemSourceA, error));
    assert(neuracoust::daw::writePcm16WavFile(renderedVocalsPath, stemSourceB, error));
    const auto completeStemApply = neuracoust::daw::applyStemMagicRenderedStems(partialStemApplyProject,
                                                                               stemMagicJob,
                                                                               partialStemApplyProject.clips.front(),
                                                                               neuracoust::daw::StemMagicJobMode::FourStem);
    assert(completeStemApply.ok);
    assert(completeStemApply.createdTracks == 4);
    assert(completeStemApply.missingStemPaths.empty());
    assert(partialStemApplyProject.tracks.size() == beforePartialStemTrackCount + 4);
    assert(partialStemApplyProject.clips.size() == beforePartialStemClipCount + 4);
    auto renderedDrumsTrack = std::find_if(partialStemApplyProject.tracks.begin(), partialStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Drums";
    });
    auto renderedBassTrack = std::find_if(partialStemApplyProject.tracks.begin(), partialStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Bass";
    });
    auto renderedOtherTrack = std::find_if(partialStemApplyProject.tracks.begin(), partialStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Other";
    });
    auto renderedVocalsTrack = std::find_if(partialStemApplyProject.tracks.begin(), partialStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Vocals";
    });
    assert(renderedDrumsTrack != partialStemApplyProject.tracks.end());
    assert(renderedBassTrack != partialStemApplyProject.tracks.end());
    assert(renderedOtherTrack != partialStemApplyProject.tracks.end());
    assert(renderedVocalsTrack != partialStemApplyProject.tracks.end());
    assert(renderedDrumsTrack->inputBus == "None");
    assert(renderedDrumsTrack->outputBus == "Master");
    auto renderedDrumsClip = std::find_if(partialStemApplyProject.clips.begin(), partialStemApplyProject.clips.end(), [&](const auto& clip) {
        return clip.trackName == "Stem Magic Drums" && clip.sourcePath == renderedDrumsPath.string();
    });
    assert(renderedDrumsClip != partialStemApplyProject.clips.end());
    assert(renderedDrumsClip->regionName == "Drums");
    assert(renderedDrumsClip->startSeconds == stemProject.clips.front().startSeconds);
    assert(renderedDrumsClip->durationSeconds == stemProject.clips.front().durationSeconds);
    assert(std::abs(renderedDrumsClip->sourceOffsetSeconds - 0.025) < 0.0001);
    assert(std::abs(renderedDrumsClip->timeScale - 1.25) < 0.0001);
    assert(renderedDrumsClip->tempoSyncPolicy == "stretch-to-project");
    assert(renderedDrumsClip->pendingTimeStretchToProject);

    auto folderStemApplyProject = stemProject;
    const auto beforeFolderStemTrackCount = folderStemApplyProject.tracks.size();
    const auto beforeFolderStemClipCount = folderStemApplyProject.clips.size();
    const auto folderStemApply = neuracoust::daw::applyStemMagicRenderedStems(folderStemApplyProject,
                                                                             stemMagicJob,
                                                                             folderStemApplyProject.clips.front(),
                                                                             neuracoust::daw::StemMagicJobMode::FourStem,
                                                                             neuracoust::daw::StemMagicTrackLayout::FolderWithOriginal);
    assert(folderStemApply.ok);
    assert(folderStemApply.createdTracks == 4);
    assert(folderStemApplyProject.tracks.size() == beforeFolderStemTrackCount + 5);
    assert(folderStemApplyProject.clips.size() == beforeFolderStemClipCount + 4);
    auto folderStemFolder = std::find_if(folderStemApplyProject.tracks.begin(), folderStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Stem Vocal Region" && track.trackType == "folder";
    });
    assert(folderStemFolder != folderStemApplyProject.tracks.end());
    auto folderStemSource = std::find_if(folderStemApplyProject.tracks.begin(), folderStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "리드 보컬:Main";
    });
    auto folderStemDrums = std::find_if(folderStemApplyProject.tracks.begin(), folderStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Drums";
    });
    assert(folderStemSource != folderStemApplyProject.tracks.end());
    assert(folderStemDrums != folderStemApplyProject.tracks.end());
    assert(folderStemSource->folderName == "Stem Magic Stem Vocal Region");
    assert(folderStemDrums->folderName == "Stem Magic Stem Vocal Region");
    assert(folderStemDrums->outputBus == "Master");

    auto busStemApplyProject = stemProject;
    const auto beforeBusStemTrackCount = busStemApplyProject.tracks.size();
    const auto beforeBusStemClipCount = busStemApplyProject.clips.size();
    const auto busStemApply = neuracoust::daw::applyStemMagicRenderedStems(busStemApplyProject,
                                                                          stemMagicJob,
                                                                          busStemApplyProject.clips.front(),
                                                                          neuracoust::daw::StemMagicJobMode::FourStem,
                                                                          neuracoust::daw::StemMagicTrackLayout::BusFolder);
    assert(busStemApply.ok);
    assert(busStemApply.createdTracks == 4);
    assert(busStemApplyProject.tracks.size() == beforeBusStemTrackCount + 5);
    assert(busStemApplyProject.clips.size() == beforeBusStemClipCount + 4);
    auto busStemFolder = std::find_if(busStemApplyProject.tracks.begin(), busStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Stem Vocal Region Bus" && track.trackType == "bus_folder";
    });
    assert(busStemFolder != busStemApplyProject.tracks.end());
    assert(busStemFolder->inputBus == "Bus 1-2");
    assert(busStemFolder->outputBus == "Master");
    auto busStemSource = std::find_if(busStemApplyProject.tracks.begin(), busStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "리드 보컬:Main";
    });
    auto busStemDrums = std::find_if(busStemApplyProject.tracks.begin(), busStemApplyProject.tracks.end(), [](const auto& track) {
        return track.name == "Stem Magic Drums";
    });
    assert(busStemSource != busStemApplyProject.tracks.end());
    assert(busStemDrums != busStemApplyProject.tracks.end());
    assert(busStemSource->folderName == "Stem Magic Stem Vocal Region Bus");
    assert(busStemDrums->folderName == "Stem Magic Stem Vocal Region Bus");
    assert(busStemSource->outputBus == "Bus 1-2");
    assert(busStemDrums->outputBus == "Bus 1-2");

    neuracoust::daw::ClipState relativeStemClip = stemProject.clips.front();
    relativeStemClip.id = "relative-stem";
    relativeStemClip.sourcePath = "Audio Files/relative-source.wav";
    const auto relativeStemMagicJob = neuracoust::daw::writeStemMagicJobManifest(relativeStemClip,
                                                                                stemRoot / "Relative Stem.ndaw",
                                                                                neuracoust::daw::StemMagicJobMode::DrumSplit);
    assert(relativeStemMagicJob.ok);
    assert(relativeStemMagicJob.resolvedSourcePath.is_absolute());
    assert(relativeStemMagicJob.resolvedSourcePath.lexically_normal().generic_string().find("Audio Files/relative-source.wav") != std::string::npos);
    {
        std::ifstream relativeStemManifest(relativeStemMagicJob.manifestPath, std::ios::binary);
        std::string relativeStemText((std::istreambuf_iterator<char>(relativeStemManifest)), std::istreambuf_iterator<char>());
        assert(relativeStemText.find("\"sourcePath\": \"" + jsonStringFragment(relativeStemMagicJob.resolvedSourcePath.string()) + "\"") != std::string::npos);
        assert(relativeStemText.find("\"originalSourcePath\": \"Audio Files/relative-source.wav\"") != std::string::npos);
        assert(relativeStemText.find("\"mode\": \"drum-split\"") != std::string::npos);
    }
    std::filesystem::remove_all(stemRoot);

    const auto silentStemRoot = testTempRoot() / "neuracoust-daw-silent-stem-export";
    std::filesystem::remove_all(silentStemRoot);
    std::filesystem::create_directories(silentStemRoot);
    neuracoust::daw::WavAudioData silentStemSource;
    silentStemSource.channels = 1;
    silentStemSource.sampleRate = 100;
    silentStemSource.interleavedSamples.assign(10, 0.0f);
    const auto silentStemSourcePath = silentStemRoot / "silent.wav";
    assert(neuracoust::daw::writePcm16WavFile(silentStemSourcePath, silentStemSource, error));
    auto silentStemProject = neuracoust::daw::defaultProject();
    silentStemProject.sampleRate = 100.0;
    silentStemProject.monitorModules.clear();
    silentStemProject.clips.push_back({"silent-stem", "Audio 1", silentStemSourcePath.string(), 0.0, 0.10, 0.0, 0.0f});
    const auto silentStemExport = neuracoust::daw::exportProjectTrackStems(silentStemProject, silentStemRoot / "Stems");
    assert(silentStemExport.ok);
    assert(silentStemExport.exportedStems == 1);
    assert(silentStemExport.stemLevelStats.front().nearSilent);
    assert(silentStemExport.message.find("near-silent stem") != std::string::npos);
    std::filesystem::remove_all(silentStemRoot);

    neuracoust::daw::WavAudioData insertTestMix;
    insertTestMix.channels = 2;
    insertTestMix.sampleRate = 48000;
    insertTestMix.interleavedSamples = {0.25f, -0.25f, 0.1f, -0.1f};
    std::string insertError;
    assert(neuracoust::daw::applyProjectMasterInsertsToStereoMix(project, insertTestMix, insertError));
    assert(insertError.empty());
    assert(insertTestMix.channels == 2);
    assert(insertTestMix.frameCount() == 2);
    auto invalidInsertProject = project;
    invalidInsertProject.masterInserts.push_back(makeMasterInsert(
        "Missing VST3",
        "external-vst3",
        "VST3",
        "/definitely/not/a/plugin.vst3",
        false,
        false));
    neuracoust::daw::ProjectAudioRenderPlan invalidInsertPlan;
    assert(neuracoust::daw::makeProjectAudioRenderPlan(invalidInsertProject, invalidInsertPlan, error));
    assert(neuracoust::daw::activeVst3MasterInsertCount(invalidInsertProject) == 1);
    assert(neuracoust::daw::hasActiveVst3MasterInserts(invalidInsertProject));
    assert(invalidInsertPlan.hasActiveVst3Inserts);
    assert(invalidInsertPlan.activeVst3Inserts.size() == 1);
    assert(invalidInsertPlan.activeVst3Inserts.front().pluginName == "Missing VST3");
    invalidInsertProject.tracks[0].inserts.push_back(makeTrackInsert("Track Missing VST3", "VST3", "/definitely/not/a/track-plugin.vst3"));
    assert(neuracoust::daw::makeProjectAudioRenderPlan(invalidInsertProject, invalidInsertPlan, error));
    assert(invalidInsertPlan.hasActiveTrackVst3Inserts);
    assert(invalidInsertPlan.activeTrackVst3InsertLabels.size() == 2);
    assert(std::any_of(invalidInsertPlan.activeTrackVst3InsertLabels.begin(), invalidInsertPlan.activeTrackVst3InsertLabels.end(), [](const std::string& label) {
        return label.find("Audio 1: Track Missing VST3") != std::string::npos;
    }));
    const auto invalidInsertBounce = neuracoust::daw::bounceProjectToWav(
        invalidInsertProject,
        (testTempRoot() / "neuracoust-daw-invalid-vst3-bounce.wav").string());
    assert(!invalidInsertBounce.ok);
    assert(invalidInsertBounce.message.find("VST3 insert failed") != std::string::npos);

    auto wavesProject = neuracoust::daw::defaultProject();
    auto wavesInsert = makeTrackInsert(
        "Immersive Wrapper Stereo",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.4.vst3");
    wavesInsert.pluginClassId = "56535432303049696d6d657273697665";
    wavesInsert.pluginClassName = "Immersive Wrapper Stereo";
    wavesInsert.dspExecutionMode = "native";
    wavesInsert.dspAvailable = true;
    wavesProject.tracks.front().inserts.push_back(wavesInsert);
    const auto wavesProjectPath = testTempRoot() / "waves-class-restore.ndaw";
    assert(neuracoust::daw::saveProjectFileWithBackup(wavesProject, wavesProjectPath, error));
    std::ifstream wavesProjectIn(wavesProjectPath, std::ios::binary);
    const std::string wavesText((std::istreambuf_iterator<char>(wavesProjectIn)), std::istreambuf_iterator<char>());
    assert(wavesText.find("\"pluginName\":\"Immersive Wrapper Stereo\"") != std::string::npos);
    assert(wavesText.find("\"pluginClassId\":\"56535432303049696d6d657273697665\"") != std::string::npos);
    assert(wavesText.find("\"pluginClassName\":\"Immersive Wrapper Stereo\"") != std::string::npos);
    neuracoust::daw::ProjectDocument restoredWavesProject;
    assert(neuracoust::daw::deserializeProjectForPath(wavesText, wavesProjectPath, restoredWavesProject, error));
    assert(!restoredWavesProject.tracks.empty());
    assert(restoredWavesProject.tracks.front().inserts.size() == 1);
    const auto& restoredWavesInsert = restoredWavesProject.tracks.front().inserts.front();
    assert(restoredWavesInsert.pluginName == "Immersive Wrapper Stereo");
    assert(restoredWavesInsert.pluginFormat == "VST3");
    assert(restoredWavesInsert.pluginPath.find("WaveShell1-VST3") != std::string::npos);
    assert(restoredWavesInsert.pluginClassId == "56535432303049696d6d657273697665");
    assert(restoredWavesInsert.pluginClassName == "Immersive Wrapper Stereo");

	    const char* scanInstalledVst3Env = std::getenv("NEURACOUST_CORE_SMOKE_SCAN_INSTALLED_VST3");
	    const bool scanInstalledVst3 = scanInstalledVst3Env != nullptr && std::string(scanInstalledVst3Env) == "1";
    const bool wavesShellInstalled = scanInstalledVst3 &&
        std::filesystem::exists("/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.0.vst3");
    if (wavesShellInstalled) {
        neuracoust::daw::clearVst3PluginScanCache();
        const auto scannedPlugins = neuracoust::daw::scanKnownPluginLocations();
        const auto wavesCandidate = std::find_if(scannedPlugins.begin(), scannedPlugins.end(), [](const auto& plugin) {
            return plugin.format == "VST3" &&
                plugin.brand == "Waves" &&
                plugin.name == "API-2500" &&
                plugin.path.find("WaveShell1-VST3") != std::string::npos &&
                !plugin.pluginClassId.empty() &&
                plugin.pluginClassName == "API-2500 Stereo";
        });
        assert(wavesCandidate != scannedPlugins.end());
        const auto genericWaveShell = std::find_if(scannedPlugins.begin(), scannedPlugins.end(), [](const auto& plugin) {
            const std::string name = plugin.name + " " + plugin.pluginName + " " + plugin.pluginClassName;
            return plugin.format == "VST3" &&
                plugin.path.find("WaveShell1-VST3") != std::string::npos &&
                name.find("WaveShell") != std::string::npos;
        });
        assert(genericWaveShell == scannedPlugins.end());
        const auto internalWavesWrapper = std::find_if(scannedPlugins.begin(), scannedPlugins.end(), [](const auto& plugin) {
            return plugin.format == "VST3" &&
                plugin.path.find("WaveShell1-VST3") != std::string::npos &&
                plugin.name.find("Immersive Wrapper") != std::string::npos;
        });
        assert(internalWavesWrapper == scannedPlugins.end());
        const auto resolvedWaves = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
            "API-2500",
            "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.0.vst3",
            "565354415043536170692d3235303020",
            "API-2500 Stereo");
        assert(resolvedWaves.name == "API-2500");
        assert(resolvedWaves.componentClassCid == "565354415043536170692d3235303020");
        assert(resolvedWaves.componentClassName == "API-2500 Stereo");
        const auto wavesL2Candidate = std::find_if(scannedPlugins.begin(), scannedPlugins.end(), [](const auto& plugin) {
            return plugin.format == "VST3" &&
                plugin.brand == "Waves" &&
                plugin.name.find("L2") != std::string::npos &&
                plugin.path.find("WaveShell1-VST3") != std::string::npos &&
                plugin.pluginClassId == "5653544c324d536c322073746572656f" &&
                plugin.pluginClassName == "L2 Stereo";
        });
        assert(wavesL2Candidate != scannedPlugins.end());
        const auto resolvedWavesL2 = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
            "L2 Ultramaximizer Stereo",
            "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.0.vst3");
        assert(resolvedWavesL2.name == "L2 Ultramaximizer Stereo");
        assert(resolvedWavesL2.componentClassCid == "5653544c324d536c322073746572656f");
        assert(resolvedWavesL2.componentClassName == "L2 Stereo");
        const auto resolvedWavesL2FromStaleClass = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
            "L2",
            "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.0.vst3",
            "5653545354454d616262657920726f61",
            "Abbey Road Chambers Mono");
        assert(resolvedWavesL2FromStaleClass.name == "L2");
        assert(resolvedWavesL2FromStaleClass.componentClassCid == "5653544c324d536c322073746572656f");
        assert(resolvedWavesL2FromStaleClass.componentClassName == "L2 Stereo");
        const auto migratedWaves = neuracoust::daw::resolveVst3PluginDescriptorForInsert(
            "API-2500",
            "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.4.vst3",
            "56535432303049696d6d657273697665",
            "Immersive Wrapper Stereo");
        assert(migratedWaves.name == "API-2500");
        assert(migratedWaves.bundlePath.find("WaveShell1-VST3 16.0.vst3") != std::string::npos);
        assert(migratedWaves.componentClassCid == "565354415043536170692d3235303020");
        assert(migratedWaves.componentClassName == "API-2500 Stereo");

        const auto l2Parameters = neuracoust::daw::inspectVst3ParametersWithSdk(resolvedWavesL2, 512);
        auto thresholdParameter = std::find_if(l2Parameters.parameters.begin(), l2Parameters.parameters.end(), [](const auto& parameter) {
            std::string name = parameter.title + " " + parameter.units;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return name.find("thresh") != std::string::npos;
        });
        assert(thresholdParameter != l2Parameters.parameters.end());

        neuracoust::daw::ProjectAudioRenderPlan wavesL2RealtimePlan;
        wavesL2RealtimePlan.sampleRate = 44100.0;
        wavesL2RealtimePlan.hasActiveVst3Inserts = true;
        neuracoust::daw::InsertState wavesL2RealtimeInsert;
        wavesL2RealtimeInsert.pluginName = "L2";
        wavesL2RealtimeInsert.pluginFormat = "VST3";
        wavesL2RealtimeInsert.pluginPath = resolvedWavesL2.bundlePath;
        wavesL2RealtimeInsert.pluginClassId = resolvedWavesL2.componentClassCid;
        wavesL2RealtimeInsert.pluginClassName = resolvedWavesL2.componentClassName;
        wavesL2RealtimeInsert.available = true;
        wavesL2RealtimeInsert.parameters.push_back({
            thresholdParameter->id,
            thresholdParameter->title.empty() ? "Thresh Slider" : thresholdParameter->title,
            0.02
        });
        wavesL2RealtimePlan.activeVst3Inserts.push_back(wavesL2RealtimeInsert);

        neuracoust::daw::RealtimeMasterInsertChain wavesL2RealtimeChain;
        std::string wavesL2RealtimeError;
        assert(wavesL2RealtimeChain.prepare(wavesL2RealtimePlan, 44100.0, 256, wavesL2RealtimeError));
        std::vector<float> wavesL2RealtimeBlock(44100 * 2);
        for (size_t frame = 0; frame < 44100; ++frame) {
            const float sample = static_cast<float>(std::sin((2.0 * kPi * 1000.0 * static_cast<double>(frame)) / 44100.0) * 0.125);
            wavesL2RealtimeBlock[frame * 2] = sample;
            wavesL2RealtimeBlock[frame * 2 + 1] = sample;
        }
        const double wavesL2InputRmsDb = stereoRmsDb(wavesL2RealtimeBlock);
        size_t processedFrames = 0;
        while (processedFrames < 44100) {
            const int frames = static_cast<int>(std::min<size_t>(256, 44100 - processedFrames));
            std::vector<float> block(wavesL2RealtimeBlock.begin() + static_cast<std::ptrdiff_t>(processedFrames * 2),
                                     wavesL2RealtimeBlock.begin() + static_cast<std::ptrdiff_t>((processedFrames + frames) * 2));
            assert(wavesL2RealtimeChain.processInterleavedStereo(block, frames, wavesL2RealtimeError));
            std::copy(block.begin(), block.end(), wavesL2RealtimeBlock.begin() + static_cast<std::ptrdiff_t>(processedFrames * 2));
            processedFrames += static_cast<size_t>(frames);
        }
        const double wavesL2OutputRmsDb = stereoRmsDb(wavesL2RealtimeBlock);
        assert(wavesL2OutputRmsDb > wavesL2InputRmsDb + 6.0);
    }

    neuracoust::daw::RecordingTake take(1, 48000);
    const int16_t recordedSamples[8] = {0, 512, -512, 1024, -1024, 2048, -2048, 0};
    take.appendInterleavedInt16(recordedSamples, 8);
    assert(take.frameCount() == 8);
    const auto recordingPath = (testTempRoot() / "neuracoust-daw-recording-take.wav").string();
    assert(take.saveWav(recordingPath, error));
    assert(!std::filesystem::exists(std::filesystem::path(recordingPath + ".writing")));
    neuracoust::daw::WavAudioData recording;
    assert(neuracoust::daw::readPcmWavFile(recordingPath, recording, error));
    assert(recording.channels == 1);
    assert(recording.frameCount() == 8);
    assert(recording.bitsPerSample == 16);
    assert(!recording.floatingPoint);
    const auto recordingPath32 = (testTempRoot() / "neuracoust-daw-recording-take-32f.wav").string();
    assert(take.saveWav(recordingPath32, 32, error));
    neuracoust::daw::WavAudioData recording32;
    assert(neuracoust::daw::readPcmWavFile(recordingPath32, recording32, error));
    assert(recording32.channels == 1);
    assert(recording32.frameCount() == 8);
    assert(recording32.bitsPerSample == 32);
    assert(recording32.floatingPoint);
    const auto recordingPath64 = (testTempRoot() / "neuracoust-daw-recording-take-64f.wav").string();
    assert(take.saveWav(recordingPath64, 64, error));
    neuracoust::daw::WavAudioData recording64;
    assert(neuracoust::daw::readPcmWavFile(recordingPath64, recording64, error));
    assert(recording64.channels == 1);
    assert(recording64.frameCount() == 8);
    assert(recording64.bitsPerSample == 64);
    assert(recording64.floatingPoint);

    {
        // Instrument rack layering: adding a layer must not drop the first instrument.
        // Regression for the indexed setTrackInstrumentSlot missing the legacy-materialize
        // guard — a layer at slot 1 while the instrument lived only in track.instrument
        // used to compact to an empty front slot and lose the original.
        using namespace neuracoust::daw;
        ProjectDocument project;
        const std::string trackName = addInstrumentTrack(project);
        assert(!trackName.empty());

        auto makeInstrument = [](const std::string& name) {
            InstrumentSlotState slot;
            slot.pluginName = name;
            slot.pluginPath = "/tmp/" + name + ".vst3";
            slot.pluginFormat = "VST3";
            slot.enabled = true;
            slot.bypassed = false;
            return slot;
        };

        auto trackByName = [&project](const std::string& name) -> const TrackState* {
            for (const auto& track : project.tracks) {
                if (track.name == name) return &track;
            }
            return nullptr;
        };

        // First instrument via the legacy single-slot path (mirrors nc_track_set_instrument).
        assert(setTrackInstrumentSlot(project, trackName, makeInstrument("Alpha")));
        const TrackState* t = trackByName(trackName);
        assert(t != nullptr);
        assert(t->instrument.pluginName == "Alpha");

        // Layer a second instrument at slot 1 — both must survive and stay ordered.
        assert(setTrackInstrumentSlot(project, trackName, 1, makeInstrument("Beta")));
        t = trackByName(trackName);
        assert(t->instrumentSlots.size() == 2);
        assert(t->instrumentSlots[0].pluginName == "Alpha");
        assert(t->instrumentSlots[1].pluginName == "Beta");
        assert(t->instrument.pluginName == "Alpha");   // front mirrors slot 0, not the empty gap

        // Removing the layer leaves the primary intact.
        assert(clearTrackInstrumentSlot(project, trackName, 1));
        t = trackByName(trackName);
        assert(t->instrument.pluginName == "Alpha");
    }

    // --- a plug-in's own patch survives base64 and the document ------------------
    //
    // A workstation instrument keeps its selected program in its VST3 component state,
    // not in its parameters (KORG TRITON publishes 2,573 parameters and none of them
    // selects the program). If the blob does not round-trip byte-for-byte, the project
    // reopens on whatever the plug-in calls its startup default.
    {
        using namespace neuracoust::daw;

        // Every byte value, at every length remainder — base64 pads in three shapes and
        // a state blob is arbitrary binary, not text.
        for (size_t length = 0; length <= 259; ++length) {
            std::vector<uint8_t> original(length);
            for (size_t index = 0; index < length; ++index) {
                original[index] = static_cast<uint8_t>((index * 37 + length) & 0xFF);
            }
            const std::string encoded = encodeBase64(original);
            std::vector<uint8_t> decoded;
            assert(decodeBase64(encoded, decoded));
            assert(decoded == original);
        }
        // Garbage must fail loudly rather than hand a plug-in a truncated state.
        std::vector<uint8_t> rejected;
        assert(!decodeBase64("not valid base64!", rejected));
        assert(rejected.empty());
        assert(decodeBase64("QQ", rejected) && rejected.size() == 1);   // unpadded is legal

        ProjectDocument project = defaultProject();
        const std::string trackName = addInstrumentTrack(project);
        assert(!trackName.empty());

        InstrumentSlotState instrument;
        instrument.pluginName = "TRITON";
        instrument.pluginFormat = "VST3";
        instrument.pluginPath = "/Library/Audio/Plug-Ins/VST3/TRITON.vst3";
        instrument.enabled = true;
        // Binary with the bytes a naive text path would eat: NUL, quote, backslash, DEL.
        const std::vector<uint8_t> patch {0x00, 0x22, 0x5C, 0x7F, 0xFF, 0x0A, 0x0D, 0x01};
        instrument.pluginStateBase64 = encodeBase64(patch);
        assert(setTrackInstrumentSlot(project, trackName, instrument));

        std::string error;
        ProjectDocument reloaded;
        assert(deserializeProject(serializeProject(project), reloaded, error));

        const TrackState* reloadedTrack = nullptr;
        for (const auto& track : reloaded.tracks) {
            if (track.name == trackName) reloadedTrack = &track;
        }
        assert(reloadedTrack != nullptr);
        assert(!reloadedTrack->instrumentSlots.empty());
        std::vector<uint8_t> reloadedPatch;
        assert(decodeBase64(reloadedTrack->instrumentSlots[0].pluginStateBase64, reloadedPatch));
        assert(reloadedPatch == patch);
        // The legacy single-slot mirror carries it too, since the renderer falls back to it.
        assert(reloadedTrack->instrument.pluginStateBase64 ==
               reloadedTrack->instrumentSlots[0].pluginStateBase64);
    }

    // --- recorded-controller whitelist ------------------------------------------
    //
    // A take used to capture notes and nothing else, so the sustain pedal — the thing that
    // makes a piano performance sound like one — was thrown away.
    {
        using namespace neuracoust::daw;
        ProjectDocument project = defaultProject();
        // Sustain and modulation on by default, and pitch bend with them.
        assert(std::find(project.midiRecordControllers.begin(), project.midiRecordControllers.end(), 64)
               != project.midiRecordControllers.end());
        assert(std::find(project.midiRecordControllers.begin(), project.midiRecordControllers.end(), 1)
               != project.midiRecordControllers.end());
        assert(project.midiRecordPitchBend);

        project.midiRecordControllers = {11, 64};
        project.midiRecordPitchBend = false;
        std::string error;
        ProjectDocument reloaded;
        assert(deserializeProject(serializeProject(project), reloaded, error));
        assert(reloaded.midiRecordControllers == std::vector<int>({11, 64}));
        assert(!reloaded.midiRecordPitchBend);

        // An empty whitelist is a real choice ("record no controllers") and must survive as
        // one, rather than being read back as "the key was missing, use the defaults".
        project.midiRecordControllers.clear();
        ProjectDocument none;
        assert(deserializeProject(serializeProject(project), none, error));
        assert(none.midiRecordControllers.empty());
    }

    // --- AAF import: available, and fails safely on things that are not AAF ------
    //
    // No AAF file to import here, so this pins the parts that can be checked without one: that the
    // reader is compiled in, and that garbage input is refused rather than crashing or half-loading.
    {
        using namespace neuracoust::daw;
        assert(aafImportAvailable());

        ProjectDocument target = defaultProject();
        const auto before = target.tracks.size();

        auto missing = importAafSession("/definitely/not/here.aaf", target);
        assert(!missing.ok);
        assert(!missing.message.empty());
        assert(target.tracks.size() == before);   // a failed import must not touch the document

        // A real file that is not an AAF.
        const auto notAaf = std::filesystem::temp_directory_path() / "neuracoust-not-an.aaf";
        {
            std::ofstream out(notAaf, std::ios::binary | std::ios::trunc);
            out << "this is plainly not a structured storage container";
        }
        auto garbage = importAafSession(notAaf, target);
        assert(!garbage.ok);
        assert(target.tracks.size() == before);
        std::error_code ec;
        std::filesystem::remove(notAaf, ec);
    }

    std::cout << "Core smoke test passed\n";
    return 0;
}
