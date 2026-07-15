#pragma once

#include "core/DawState.h"
#include "plugins/Vst3HostFoundation.h"
#include "audio/WavFile.h"
#include <memory>
#include <string>
#include <vector>

namespace neuracoust::daw {

enum class Vst3MidiEventKind {
    Note,
    Controller,
    PitchBend,
    ProgramChange
};

struct Vst3FactoryClassInfo {
    std::string cidHex;
    std::string category;
    std::string name;
    bool instanceCreated = false;
    bool instanceReleased = false;
};

struct Vst3FactoryInspection {
    bool sdkAvailable = false;
    bool opened = false;
    bool hasFactory = false;
    int classCount = 0;
    int componentInstanceCount = 0;
    int controllerInstanceCount = 0;
    std::string vendor;
    std::string url;
    std::string message;
    std::vector<Vst3FactoryClassInfo> classes;
};

struct Vst3ProcessorProbe {
    bool sdkAvailable = false;
    bool opened = false;
    bool hasFactory = false;
    bool componentCreated = false;
    bool initialized = false;
    bool audioProcessorAvailable = false;
    bool sample32Supported = false;
    bool busArrangementAccepted = false;
    bool setupProcessingOk = false;
    int inputBusCount = 0;
    int outputBusCount = 0;
    int mainInputChannels = 0;
    int mainOutputChannels = 0;
    unsigned int latencySamples = 0;
    unsigned int tailSamples = 0;
    std::string className;
    std::string message;
};

struct Vst3ProcessResult {
    bool sdkAvailable = false;
    bool opened = false;
    bool processed = false;
    int framesProcessed = 0;
    unsigned int latencySamples = 0;
    unsigned int tailSamples = 0;
    std::string className;
    std::string message;
};

struct Vst3MidiEvent {
    int frameOffset = 0;
    int pitch = 60;
    int velocity = 0;
    int channel = 1;
    bool noteOn = false;
    Vst3MidiEventKind kind = Vst3MidiEventKind::Note;
    int controller = 0;
    int value = 0;
    int program = 0;
};

struct Vst3ParameterDescriptor {
    uint32_t id = 0;
    std::string title;
    std::string units;
    double defaultNormalized = 0.0;
    double currentNormalized = 0.0;
    std::string defaultDisplay;
    std::string currentDisplay;
    int32_t stepCount = 0;
    int32_t flags = 0;
};

struct Vst3ParameterInspection {
    bool sdkAvailable = false;
    bool opened = false;
    bool hasFactory = false;
    bool controllerCreated = false;
    bool initialized = false;
    int parameterCount = 0;
    std::string className;
    std::string message;
    std::vector<Vst3ParameterDescriptor> parameters;
};

Vst3FactoryInspection inspectVst3FactoryWithSdk(const Vst3PluginDescriptor& descriptor);
Vst3ProcessorProbe probeVst3ProcessorWithSdk(const Vst3PluginDescriptor& descriptor,
                                             double sampleRate = 48000.0,
                                             int maxBlockSize = 256);
Vst3ParameterInspection inspectVst3ParametersWithSdk(const Vst3PluginDescriptor& descriptor,
                                                     int maxParameters = 16);
Vst3ProcessResult processStereoBufferWithVst3(const Vst3PluginDescriptor& descriptor,
                                              WavAudioData& audio,
                                              int maxBlockSize = 256,
                                              const std::vector<Vst3ParameterValueState>& parameters = {});

// True when this plugin is hosted out-of-process (sandbox bridge) for realtime
// inserts, so its editor should observe the bridge to drive its own meters.
bool isVst3HostedOutOfProcess(const Vst3PluginDescriptor& descriptor);
Vst3ProcessResult processMidiInstrumentWithVst3(const Vst3PluginDescriptor& descriptor,
                                                const std::vector<Vst3MidiEvent>& midiEvents,
                                                WavAudioData& outputAudio,
                                                int maxBlockSize = 256,
                                                const std::vector<Vst3ParameterValueState>& parameters = {});

class Vst3RealtimeProcessor {
public:
    Vst3RealtimeProcessor();
    ~Vst3RealtimeProcessor();
    Vst3RealtimeProcessor(const Vst3RealtimeProcessor&) = delete;
    Vst3RealtimeProcessor& operator=(const Vst3RealtimeProcessor&) = delete;
    Vst3RealtimeProcessor(Vst3RealtimeProcessor&&) noexcept;
    Vst3RealtimeProcessor& operator=(Vst3RealtimeProcessor&&) noexcept;

    bool prepare(const Vst3PluginDescriptor& descriptor,
                 double sampleRate,
                 int maxBlockSize,
                 std::string& message,
                 const std::string& bridgeShmKey = {},
                 bool forceOutOfProcess = false);
    void reset();
    bool isPrepared() const;
    // True when the last process produced this insert's real (wet) output. In-process hosting is
    // always wet; an out-of-process insert is dry while its worker warms up — this reports that edge
    // so the render can mask the dry→wet swap when the worker finally engages.
    bool producedWetLastBlock() const;
    Vst3ProcessorProbe probe() const;
    Vst3ProcessResult processInterleavedStereo(float* interleavedStereo,
                                               int frameCount,
                                               std::string& message);
    Vst3ProcessResult processInterleavedStereo(float* interleavedStereo,
                                               int frameCount,
                                               const std::vector<Vst3ParameterValueState>& parameters,
                                               std::string& message);
    std::vector<Vst3ParameterValueState> drainOutputParameterChanges();
    Vst3ProcessResult processMidiInstrument(float* interleavedStereo,
                                            int frameCount,
                                            const std::vector<Vst3MidiEvent>& midiEvents,
                                            const std::vector<Vst3ParameterValueState>& parameters,
                                            std::string& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuracoust::daw
