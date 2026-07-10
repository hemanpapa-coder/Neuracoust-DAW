#include "audio/WavFile.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string argValue(int argc, char** argv, const std::string& key) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] != nullptr && key == argv[index]) {
            return argv[index + 1] != nullptr ? argv[index + 1] : "";
        }
    }
    return {};
}

bool hasArg(int argc, char** argv, const std::string& key) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && key == argv[index]) {
            return true;
        }
    }
    return false;
}

int intArg(int argc, char** argv, const std::string& key, int fallback) {
    const std::string text = argValue(argc, argv, key);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(std::max<long>(1, std::min<long>(value, 192000)));
}

double doubleArg(int argc, char** argv, const std::string& key, double fallback) {
    const std::string text = argValue(argc, argv, key);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        return fallback;
    }
    return value;
}

float peakAbs(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

double rms(const std::vector<float>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    double sumSquares = 0.0;
    for (const float sample : samples) {
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sumSquares / static_cast<double>(samples.size()));
}

std::vector<neuracoust::daw::Vst3MidiEvent> makeProbeChord(int sampleRate, double seconds, bool includeControls) {
    const int noteOn = static_cast<int>(0.10 * sampleRate);
    const int noteOff = static_cast<int>(std::max(0.20, seconds * 0.70) * sampleRate);
    std::vector<neuracoust::daw::Vst3MidiEvent> events;
    if (includeControls) {
        neuracoust::daw::Vst3MidiEvent program;
        program.frameOffset = static_cast<int>(0.02 * sampleRate);
        program.channel = 1;
        program.kind = neuracoust::daw::Vst3MidiEventKind::ProgramChange;
        program.program = 0;
        events.push_back(program);

        neuracoust::daw::Vst3MidiEvent modWheel;
        modWheel.frameOffset = static_cast<int>(0.04 * sampleRate);
        modWheel.channel = 1;
        modWheel.kind = neuracoust::daw::Vst3MidiEventKind::Controller;
        modWheel.controller = 1;
        modWheel.value = 96;
        events.push_back(modWheel);

        neuracoust::daw::Vst3MidiEvent sustain;
        sustain.frameOffset = static_cast<int>(0.05 * sampleRate);
        sustain.channel = 1;
        sustain.kind = neuracoust::daw::Vst3MidiEventKind::Controller;
        sustain.controller = 64;
        sustain.value = 127;
        events.push_back(sustain);

        neuracoust::daw::Vst3MidiEvent bend;
        bend.frameOffset = static_cast<int>(std::max(0.12, seconds * 0.35) * sampleRate);
        bend.channel = 1;
        bend.kind = neuracoust::daw::Vst3MidiEventKind::PitchBend;
        bend.value = 12288;
        events.push_back(bend);

        neuracoust::daw::Vst3MidiEvent bendCenter = bend;
        bendCenter.frameOffset = static_cast<int>(std::max(0.16, seconds * 0.52) * sampleRate);
        bendCenter.value = 8192;
        events.push_back(bendCenter);

        neuracoust::daw::Vst3MidiEvent sustainOff = sustain;
        sustainOff.frameOffset = static_cast<int>(std::max(0.18, seconds * 0.78) * sampleRate);
        sustainOff.value = 0;
        events.push_back(sustainOff);
    }
    for (const int pitch : {60, 64, 67}) {
        events.push_back({noteOn, pitch, 108, 1, true});
        events.push_back({noteOff, pitch, 0, 1, false});
    }
    return events;
}

void setProcessEnvironmentValue(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main(int argc, char** argv) {
    const std::string pluginPath = argValue(argc, argv, "--plugin");
    const std::string pluginName = argValue(argc, argv, "--name");
    const std::string outputPath = argValue(argc, argv, "--output");
    const int sampleRate = intArg(argc, argv, "--sample-rate", 48000);
    const int maxBlock = intArg(argc, argv, "--max-block", 256);
    const double seconds = std::max(0.25, std::min(30.0, doubleArg(argc, argv, "--seconds", 2.0)));
    const bool allowUnsafe = hasArg(argc, argv, "--allow-unsafe-inprocess");
    const bool includeControls = hasArg(argc, argv, "--include-controls");

    if (pluginPath.empty() || pluginName.empty()) {
        std::cerr << "Usage: " << (argc > 0 ? argv[0] : "neuracoust_vst3_midi_instrument_smoke")
                  << " --plugin /path/instrument.vst3 --name \"Instrument\" [--output out.wav]"
                  << " [--sample-rate 48000] [--seconds 2.0] [--max-block 256] [--allow-unsafe-inprocess]\n";
        return 64;
    }
    if (!std::filesystem::exists(pluginPath)) {
        std::cerr << "VST3i smoke failed: plugin is missing at " << pluginPath << "\n";
        return 65;
    }
    if (allowUnsafe) {
        setProcessEnvironmentValue("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3", "1");
    }

    neuracoust::daw::WavAudioData output;
    output.channels = 2;
    output.sampleRate = sampleRate;
    output.floatingPoint = true;
    output.bitsPerSample = 32;
    output.interleavedSamples.resize(static_cast<size_t>(seconds * sampleRate) * 2, 0.0f);

    const auto descriptor = neuracoust::daw::resolveVst3PluginDescriptorForInsert(pluginName, pluginPath);
    const auto events = makeProbeChord(sampleRate, seconds, includeControls);
    const auto result = neuracoust::daw::processMidiInstrumentWithVst3(descriptor, events, output, maxBlock);
    if (!result.processed) {
        std::cerr << "VST3i smoke failed: " << result.message << "\n";
        return 66;
    }

    const float peak = peakAbs(output.interleavedSamples);
    const double levelRms = rms(output.interleavedSamples);
    if (!std::isfinite(peak) || !std::isfinite(levelRms) || peak <= 0.00001f || levelRms <= 0.000001) {
        std::cerr << "VST3i smoke failed: processed but output is silent. peak=" << peak
                  << " rms=" << levelRms << "\n";
        return 67;
    }

    if (!outputPath.empty()) {
        std::string error;
        if (!neuracoust::daw::writeFloat32WavFile(outputPath, output, error)) {
            std::cerr << "VST3i smoke failed: could not write output WAV: " << error << "\n";
            return 68;
        }
    }

    std::cout << "VST3i smoke processed " << result.framesProcessed
              << " frames peak=" << peak
              << " rms=" << levelRms
              << " latency=" << result.latencySamples
              << " tail=" << result.tailSamples
              << " class=" << result.className
              << " controls=" << (includeControls ? "yes" : "no") << "\n";
    return 0;
}
