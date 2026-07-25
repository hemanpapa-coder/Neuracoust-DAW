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

/// True when the plug-in's VST3 factory advertises an "ARA Main Factory" class — the real,
/// name-independent test for ARA support, and where a host would obtain the ARAFactory once an
/// ARA SDK is available. Opens the module, so it is a scan-time call, not a per-block one.
bool vst3AdvertisesAraFactory(const Vst3PluginDescriptor& descriptor);

/// True for ARA plug-ins (Melodyne and friends). They are not realtime effects: without an ARA
/// host they run their own transport in "transfer" mode and wedge the DAW when hosted as a plain
/// insert. Until ARA2 is supported, these are kept out of the realtime chain.
bool requiresAraHost(const Vst3PluginDescriptor& descriptor);
/// The user-facing explanation for why an ARA plug-in was not inserted.
std::string araRequiredMessage(const Vst3PluginDescriptor& descriptor);
Vst3ProcessResult processMidiInstrumentWithVst3(const Vst3PluginDescriptor& descriptor,
                                                const std::vector<Vst3MidiEvent>& midiEvents,
                                                WavAudioData& outputAudio,
                                                int maxBlockSize = 256,
                                                const std::vector<Vst3ParameterValueState>& parameters = {},
                                                const std::string& componentStateBase64 = {});

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
                 bool forceOutOfProcess = false,
                 // The plug-in's own saved patch (base64 of its VST3 component state).
                 // Applied before the component is activated, so a sampler/workstation
                 // instrument comes up on the program the project stored instead of its
                 // startup default. Ignored by the out-of-process bridge path.
                 const std::string& componentStateBase64 = {});
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
    /// Loads a new patch (VST3 component state, base64) into the ALREADY-PREPARED instance —
    /// deactivate, setState, reactivate — without reloading the module or re-instantiating. This
    /// is how a workstation instrument changes program without the heavy teardown that a full
    /// re-prepare costs, and it is safe to call from the main thread (never the audio thread).
    /// A no-op returning false when not prepared, hosted out-of-process, or the blob is empty.
    bool applyComponentState(const std::string& componentStateBase64, std::string& message);

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
