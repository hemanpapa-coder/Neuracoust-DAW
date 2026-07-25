#include "audio/WavFile.h"
#include "core/Base64.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    // A saved patch (raw VST3 component state, as the editor host writes it). Workstation
    // instruments keep their program here and in no parameter, so this is the only way to
    // render one on anything but its startup default.
    const std::string stateFilePath = argValue(argc, argv, "--state-file");
    // --param <id>:<normalized>, repeatable — renders through the same parameter queues the
    // project's stored instrument parameters use.
    std::vector<neuracoust::daw::Vst3ParameterValueState> parameters;
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == nullptr || std::string("--param") != argv[index]) {
            continue;
        }
        const std::string spec = argv[index + 1] != nullptr ? argv[index + 1] : "";
        const auto colon = spec.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        neuracoust::daw::Vst3ParameterValueState parameter;
        parameter.parameterId = static_cast<uint32_t>(std::strtoul(spec.substr(0, colon).c_str(), nullptr, 10));
        parameter.normalizedValue = std::strtod(spec.substr(colon + 1).c_str(), nullptr);
        parameters.push_back(parameter);
    }

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
    std::string stateBase64;
    if (!stateFilePath.empty()) {
        std::ifstream stateFile(stateFilePath, std::ios::binary);
        if (!stateFile) {
            std::cerr << "VST3i smoke failed: state file is missing at " << stateFilePath << "\n";
            return 69;
        }
        const std::vector<char> bytes((std::istreambuf_iterator<char>(stateFile)),
                                      std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            std::cerr << "VST3i smoke failed: state file is empty at " << stateFilePath << "\n";
            return 69;
        }
        stateBase64 = neuracoust::daw::encodeBase64(bytes.data(), bytes.size());
    }
    // --reprepare reproduces the editor-close path: prepare a realtime instrument, process a
    // block, then tear it down and re-prepare it again (as the render's cacheKey change does)
    // and process once more. This is the sequence that ran on the audio thread when a patch
    // was applied on editor close.
    // --apply-live reproduces the NEW editor-close path: prepare the instrument once (default
    // patch), process, then apply a new patch to the SAME live instance via applyComponentState
    // — no teardown, no re-instantiate — and process again. This must not crash and the second
    // block must still produce sound.
    if (hasArg(argc, argv, "--apply-live")) {
        neuracoust::daw::Vst3RealtimeProcessor processor;
        std::string msg;
        if (!processor.prepare(descriptor, sampleRate, maxBlock, msg)) {
            std::cerr << "VST3i smoke failed: prepare: " << msg << "\n";
            return 66;
        }
        std::vector<neuracoust::daw::Vst3MidiEvent> ev;
        ev.push_back({0, 60, 100, 1, true});
        std::string pmsg;
        processor.processMidiInstrument(output.interleavedSamples.data(), maxBlock, ev, parameters, pmsg);
        std::string applyMsg;
        const bool applied = processor.applyComponentState(stateBase64, applyMsg);
        std::cerr << "[apply-live] applyComponentState -> " << (applied ? "ok" : "no")
                  << " (" << applyMsg << ")\n";
        std::fill(output.interleavedSamples.begin(), output.interleavedSamples.end(), 0.0f);
        auto r = processor.processMidiInstrument(output.interleavedSamples.data(), maxBlock, ev, parameters, pmsg);
        const float pk = peakAbs(output.interleavedSamples);
        std::cerr << "[apply-live] post-apply block processed=" << r.processed << " peak=" << pk << "\n";
        if (!applied) { std::cerr << "VST3i smoke failed: patch not applied\n"; return 67; }
        std::cout << "VST3i smoke apply-live ok peak=" << pk << "\n";
        return 0;
    }

    if (hasArg(argc, argv, "--reprepare")) {
        neuracoust::daw::Vst3RealtimeProcessor processor;
        std::string msg;
        std::cerr << "[reprepare] first prepare\n";
        if (!processor.prepare(descriptor, sampleRate, maxBlock, msg, {}, false, stateBase64)) {
            std::cerr << "VST3i smoke failed: first prepare: " << msg << "\n";
            return 66;
        }
        std::vector<neuracoust::daw::Vst3MidiEvent> ev;
        ev.push_back({0, 60, 100, 1, true});
        std::string pmsg;
        processor.processMidiInstrument(output.interleavedSamples.data(), maxBlock, ev, parameters, pmsg);
        std::cerr << "[reprepare] reset + second prepare (this is the audio-thread teardown)\n";
        processor.reset();
        if (!processor.prepare(descriptor, sampleRate, maxBlock, msg, {}, false, stateBase64)) {
            std::cerr << "VST3i smoke failed: second prepare: " << msg << "\n";
            return 66;
        }
        processor.processMidiInstrument(output.interleavedSamples.data(), maxBlock, ev, parameters, pmsg);
        std::cerr << "[reprepare] survived\n";
        std::cout << "VST3i smoke reprepare survived\n";
        return 0;
    }

    // --realtime-processor renders through Vst3RealtimeProcessor, the class the DAW's
    // realtime mixer actually uses. It differs from the one-shot offline entry point in
    // one way that matters here: after preparing, it reads the controller's live values
    // and sends them in with the first block, so a patch restored into the controller is
    // what the processor hears.
    neuracoust::daw::Vst3ProcessResult result;
    if (hasArg(argc, argv, "--realtime-processor")) {
        neuracoust::daw::Vst3RealtimeProcessor processor;
        std::string prepareMessage;
        if (!processor.prepare(descriptor, sampleRate, maxBlock, prepareMessage, {}, false, stateBase64)) {
            std::cerr << "VST3i smoke failed: " << prepareMessage << "\n";
            return 66;
        }
        // The processor accepts no more than its prepared block, so walk the buffer the way
        // the mixer does, re-basing each event onto the block it falls in.
        const int totalFrames = static_cast<int>(output.frameCount());
        for (int start = 0; start < totalFrames; start += maxBlock) {
            const int frames = std::min(maxBlock, totalFrames - start);
            std::vector<neuracoust::daw::Vst3MidiEvent> blockEvents;
            for (const auto& event : events) {
                if (event.frameOffset >= start && event.frameOffset < start + frames) {
                    auto shifted = event;
                    shifted.frameOffset = event.frameOffset - start;
                    blockEvents.push_back(shifted);
                }
            }
            std::string processMessage;
            result = processor.processMidiInstrument(
                output.interleavedSamples.data() + static_cast<size_t>(start) * 2,
                frames, blockEvents, parameters, processMessage);
            if (!result.processed) {
                std::cerr << "VST3i smoke failed: " << processMessage << "\n";
                return 66;
            }
        }
        result.framesProcessed = totalFrames;
    } else {
        result = neuracoust::daw::processMidiInstrumentWithVst3(descriptor, events, output, maxBlock,
                                                               parameters, stateBase64);
    }
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
              << " controls=" << (includeControls ? "yes" : "no")
              << " state=" << (stateBase64.empty() ? "none" : std::to_string(stateBase64.size()) + "b64") << "\n";
    return 0;
}
