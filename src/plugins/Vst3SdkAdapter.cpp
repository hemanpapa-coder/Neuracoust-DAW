#include "plugins/Vst3SdkAdapter.h"
#include "plugins/Vst3RealtimeBridge.h"
#include "plugins/Vst3RealtimeBridgeProtocol.h"
#include "plugins/Vst3ModuleRuntime.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <climits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#if defined(NEURACOUST_HAS_VST3_SDK)
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vsttypes.h"
#endif

namespace neuracoust::daw {

namespace {

#if defined(NEURACOUST_HAS_VST3_SDK)
class MinimalHostApplication final : public Steinberg::Vst::IHostApplication {
public:
    MinimalHostApplication() { FUNKNOWN_CTOR }
    ~MinimalHostApplication() noexcept { FUNKNOWN_DTOR }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IHostApplication)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IHostApplication::iid, Steinberg::Vst::IHostApplication)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override {
        return Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, 1);
    }

    Steinberg::uint32 PLUGIN_API release() override {
        if (Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0) {
            delete this;
            return 0;
        }
        return __funknownRefCount;
    }

    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* appName = "Neuracoust DAW";
        int index = 0;
        for (; appName[index] != '\0' && index < 127; ++index) {
            name[index] = static_cast<Steinberg::Vst::TChar>(appName[index]);
        }
        name[index] = 0;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID, void** obj) override {
        *obj = nullptr;
        return Steinberg::kNotImplemented;
    }

private:
    Steinberg::int32 __funknownRefCount = 1;
};

const char* vstAudioEffectCategory() {
#if defined(kVstAudioEffectClass)
    return kVstAudioEffectClass;
#else
    return "Audio Module Class";
#endif
}

const char* vstControllerCategory() {
#if defined(kVstComponentControllerClass)
    return kVstComponentControllerClass;
#else
    return "Component Controller Class";
#endif
}

bool isAudioComponentCategory(const char* category) {
    if (category == nullptr) {
        return false;
    }
    const std::string value(category);
    return value == vstAudioEffectCategory() ||
           value == "Audio Module Class" ||
           value == "Audio Effect Class";
}

std::string tuidToHex(const Steinberg::TUID cid) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (int index = 0; index < 16; ++index) {
        out << std::setw(2) << (static_cast<int>(cid[index]) & 0xff);
    }
    return out.str();
}

std::string normalizedPluginKey(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return normalized;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool descriptorLooksLikeWaves(const Vst3PluginDescriptor& descriptor) {
    const auto identity = lowerAscii(descriptor.brand + " " +
                                    descriptor.vendor + " " +
                                    descriptor.bundlePath + " " +
                                    descriptor.executablePath + " " +
                                    descriptor.name + " " +
                                    descriptor.componentClassName);
    return identity.find("waves") != std::string::npos ||
        identity.find("waveshell") != std::string::npos;
}

std::string vstString128ToUtf8(const Steinberg::Vst::String128 text) {
    std::string out;
    for (int index = 0; index < 128 && text[index] != 0; ++index) {
        const auto codepoint = static_cast<uint32_t>(text[index]);
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    return out;
}

std::string paramValueToDisplayString(Steinberg::Vst::IEditController* controller,
                                      Steinberg::Vst::ParamID parameterId,
                                      double normalizedValue) {
    if (controller == nullptr) {
        return {};
    }
    Steinberg::Vst::String128 text {};
    if (controller->getParamStringByValue(parameterId, std::clamp(normalizedValue, 0.0, 1.0), text) != Steinberg::kResultOk) {
        return {};
    }
    return vstString128ToUtf8(text);
}

bool createAndReleaseInstance(Steinberg::IPluginFactory* factory,
                              const Steinberg::TUID cid,
                              const Steinberg::TUID iid) {
    void* object = nullptr;
    Steinberg::tresult result = Steinberg::kInternalError;
    try {
        result = factory->createInstance(cid, iid, &object);
    } catch (...) {
        return false;
    }
    if (result != Steinberg::kResultOk || object == nullptr) {
        return false;
    }
    auto* unknown = static_cast<Steinberg::FUnknown*>(object);
    unknown->release();
    return true;
}

bool classMatchesDescriptor(const Steinberg::PClassInfo& classInfo,
                            const Vst3PluginDescriptor& descriptor) {
    if (!descriptor.componentClassCid.empty()) {
        const auto expected = normalizedPluginKey(descriptor.componentClassCid);
        const auto actual = normalizedPluginKey(tuidToHex(classInfo.cid));
        if (expected == actual) {
            return true;
        }
    }
    if (!descriptor.componentClassName.empty() && descriptor.componentClassName == classInfo.name) {
        return true;
    }
    if (!descriptor.name.empty() && descriptor.name == classInfo.name) {
        return true;
    }
    return false;
}

bool isKnownUnsafeForInProcessVst3Host(const Vst3PluginDescriptor& descriptor) {
    const std::string brand = descriptor.brand;
    const std::string vendor = descriptor.vendor;
    const std::string name = descriptor.name;
    const std::string bundle = descriptor.bundlePath;
    // Waves plugins (hosted through WaveShell) install objc classes that collide
    // with the DAW's own AppKit process (e.g. "ResizeWindow"), which leaves the
    // in-process realtime insert silently dry inside the full app even though the
    // same plugin processes fine in a clean console process. Host them
    // out-of-process through the realtime bridge instead, alongside the other
    // vendors that are unsafe to load in-process.
    return brand == "iZotope" ||
           vendor == "iZotope" ||
           brand == "Waves" ||
           vendor == "Waves" ||
           (brand == "FabFilter" &&
            (name == "FabFilter Pro-C 3" ||
             name == "FabFilter Pro-DS" ||
             name == "FabFilter Pro-G" ||
             name == "FabFilter Pro-Q 4")) ||
           bundle.find("WaveShell") != std::string::npos;
}

std::string pluginResourceDirectory(const Vst3PluginDescriptor& descriptor) {
    if (descriptor.bundlePath.empty()) {
        return {};
    }
    const std::filesystem::path resources = std::filesystem::path(descriptor.bundlePath) / "Contents" / "Resources";
    std::error_code error;
    if (std::filesystem::is_directory(resources, error)) {
        return resources.string();
    }
    return {};
}

class ScopedPluginResourceDirectory final {
public:
    explicit ScopedPluginResourceDirectory(const Vst3PluginDescriptor& descriptor) {
        const std::string resources = pluginResourceDirectory(descriptor);
        if (resources.empty()) {
            return;
        }

#if defined(_WIN32)
        char previous[FILENAME_MAX] {};
        if (_getcwd(previous, sizeof(previous)) == nullptr) {
            return;
        }
        if (_chdir(resources.c_str()) == 0) {
            previousDirectory_ = previous;
            active_ = true;
        }
#else
        char previous[PATH_MAX] {};
        if (getcwd(previous, sizeof(previous)) == nullptr) {
            return;
        }
        if (chdir(resources.c_str()) == 0) {
            previousDirectory_ = previous;
            active_ = true;
        }
#endif
    }

    ~ScopedPluginResourceDirectory() {
        if (!active_) {
            return;
        }
#if defined(_WIN32)
        _chdir(previousDirectory_.c_str());
#else
        chdir(previousDirectory_.c_str());
#endif
    }

    ScopedPluginResourceDirectory(const ScopedPluginResourceDirectory&) = delete;
    ScopedPluginResourceDirectory& operator=(const ScopedPluginResourceDirectory&) = delete;

private:
    std::string previousDirectory_;
    bool active_ = false;
};

std::string knownUnsafeHostMessage(const Vst3PluginDescriptor& descriptor) {
    return "VST3 plugin is blocked from the current in-process host for safety: " +
        (descriptor.brand.empty() ? descriptor.name : descriptor.brand) +
        ". Use isolated hosting before enabling this vendor.";
}

Steinberg::Vst::IComponent* createAudioComponent(Steinberg::IPluginFactory* factory,
                                                 const Vst3PluginDescriptor& descriptor,
                                                 std::string& className) {
    const int classCount = factory->countClasses();
    auto tryCreateAt = [&](int index) -> Steinberg::Vst::IComponent* {
        Steinberg::PClassInfo classInfo;
        try {
            if (factory->getClassInfo(index, &classInfo) != Steinberg::kResultOk) {
                return nullptr;
            }
        } catch (...) {
            return nullptr;
        }
        if (!isAudioComponentCategory(classInfo.category)) {
            return nullptr;
        }
        void* object = nullptr;
        Steinberg::tresult result = Steinberg::kInternalError;
        try {
            result = factory->createInstance(classInfo.cid, Steinberg::Vst::IComponent::iid, &object);
        } catch (...) {
            return nullptr;
        }
        if (result != Steinberg::kResultOk || object == nullptr) {
            return nullptr;
        }
        className = classInfo.name;
        return static_cast<Steinberg::Vst::IComponent*>(object);
    };

    if (!descriptor.name.empty() || !descriptor.componentClassName.empty()) {
        for (int index = 0; index < classCount; ++index) {
            Steinberg::PClassInfo classInfo;
            try {
                if (factory->getClassInfo(index, &classInfo) != Steinberg::kResultOk ||
                    !isAudioComponentCategory(classInfo.category) ||
                    !classMatchesDescriptor(classInfo, descriptor)) {
                    continue;
                }
            } catch (...) {
                continue;
            }
            if (auto* component = tryCreateAt(index)) {
                return component;
            }
        }
        if (descriptorLooksLikeWaves(descriptor)) {
            return nullptr;
        }
    }

    for (int index = 0; index < classCount; ++index) {
        if (auto* component = tryCreateAt(index)) {
            return component;
        }
    }
    return nullptr;
}

Steinberg::Vst::IEditController* createFirstEditController(Steinberg::IPluginFactory* factory, std::string& className) {
    const int classCount = factory->countClasses();
    for (int index = 0; index < classCount; ++index) {
        Steinberg::PClassInfo classInfo;
        try {
            if (factory->getClassInfo(index, &classInfo) != Steinberg::kResultOk) {
                continue;
            }
        } catch (...) {
            continue;
        }
        if (std::string(classInfo.category) != vstControllerCategory()) {
            continue;
        }
        void* object = nullptr;
        Steinberg::tresult result = Steinberg::kInternalError;
        try {
            result = factory->createInstance(classInfo.cid, Steinberg::Vst::IEditController::iid, &object);
        } catch (...) {
            continue;
        }
        if (result != Steinberg::kResultOk || object == nullptr) {
            continue;
        }
        className = classInfo.name;
        return static_cast<Steinberg::Vst::IEditController*>(object);
    }
    return nullptr;
}

Steinberg::Vst::IEditController* createEditControllerForAudioComponent(Steinberg::IPluginFactory* factory,
                                                                       const Vst3PluginDescriptor& descriptor,
                                                                       std::string& className) {
    std::string componentClassName;
    Steinberg::Vst::IComponent* component = createAudioComponent(factory, descriptor, componentClassName);
    if (component == nullptr) {
        return nullptr;
    }

    Steinberg::TUID controllerCid {};
    const auto cidResult = component->getControllerClassId(controllerCid);
    component->release();
    if (cidResult != Steinberg::kResultOk) {
        return nullptr;
    }

    void* object = nullptr;
    Steinberg::tresult result = Steinberg::kInternalError;
    try {
        result = factory->createInstance(controllerCid, Steinberg::Vst::IEditController::iid, &object);
    } catch (...) {
        return nullptr;
    }
    if (result != Steinberg::kResultOk || object == nullptr) {
        return nullptr;
    }
    className = componentClassName;
    return static_cast<Steinberg::Vst::IEditController*>(object);
}

struct PreparedProcessor {
    Steinberg::Vst::IComponent* component = nullptr;
    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    Steinberg::Vst::IEditController* controller = nullptr;
    Steinberg::Vst::IConnectionPoint* componentConnection = nullptr;
    Steinberg::Vst::IConnectionPoint* controllerConnection = nullptr;
    std::string className;
    int inputBusCount = 0;
    int outputBusCount = 0;
    int inputChannelCount = 2;
    int outputChannelCount = 2;
    unsigned int latencySamples = 0;
    unsigned int tailSamples = 0;
    bool controllerInitialized = false;
    bool controllerFromComponent = false;
};

unsigned int normalizedVst3TailSamples(unsigned int tailSamples) {
    return tailSamples == std::numeric_limits<unsigned int>::max() ? 0u : tailSamples;
}

bool setPreferredBusArrangement(Steinberg::Vst::IAudioProcessor* processor,
                                int inputBusCount,
                                int outputBusCount,
                                int& inputChannelCount,
                                int& outputChannelCount) {
    if (processor == nullptr) {
        return false;
    }

    auto trySetArrangement = [&](Steinberg::Vst::SpeakerArrangement inputSpeakerArrangement,
                                 int candidateInputChannelCount,
                                 Steinberg::Vst::SpeakerArrangement outputSpeakerArrangement,
                                 int candidateOutputChannelCount) {
        std::vector<Steinberg::Vst::SpeakerArrangement> inputArrangements(static_cast<size_t>(std::max(0, inputBusCount)),
                                                                          inputSpeakerArrangement);
        std::vector<Steinberg::Vst::SpeakerArrangement> outputArrangements(static_cast<size_t>(std::max(0, outputBusCount)),
                                                                           outputSpeakerArrangement);
        if (processor->setBusArrangements(
                inputArrangements.empty() ? nullptr : inputArrangements.data(),
                static_cast<Steinberg::int32>(inputArrangements.size()),
                outputArrangements.empty() ? nullptr : outputArrangements.data(),
                static_cast<Steinberg::int32>(outputArrangements.size())) != Steinberg::kResultOk) {
            return false;
        }
        inputChannelCount = inputBusCount > 0 ? candidateInputChannelCount : 0;
        outputChannelCount = outputBusCount > 0 ? candidateOutputChannelCount : 0;
        return true;
    };

    return trySetArrangement(Steinberg::Vst::SpeakerArr::kStereo, 2, Steinberg::Vst::SpeakerArr::kStereo, 2) ||
           trySetArrangement(Steinberg::Vst::SpeakerArr::kMono, 1, Steinberg::Vst::SpeakerArr::kMono, 1) ||
           trySetArrangement(Steinberg::Vst::SpeakerArr::kMono, 1, Steinberg::Vst::SpeakerArr::kStereo, 2) ||
           trySetArrangement(Steinberg::Vst::SpeakerArr::kStereo, 2, Steinberg::Vst::SpeakerArr::kMono, 1);
}

void releasePreparedProcessor(PreparedProcessor& prepared) {
    if (prepared.processor != nullptr) {
        prepared.processor->setProcessing(false);
        prepared.processor->release();
        prepared.processor = nullptr;
    }
    if (prepared.componentConnection != nullptr && prepared.controllerConnection != nullptr) {
        prepared.componentConnection->disconnect(prepared.controllerConnection);
        prepared.controllerConnection->disconnect(prepared.componentConnection);
    }
    if (prepared.componentConnection != nullptr) {
        prepared.componentConnection->release();
        prepared.componentConnection = nullptr;
    }
    if (prepared.controllerConnection != nullptr) {
        prepared.controllerConnection->release();
        prepared.controllerConnection = nullptr;
    }
    if (prepared.controller != nullptr) {
        if (prepared.controllerInitialized) {
            prepared.controller->terminate();
            prepared.controllerInitialized = false;
        }
        prepared.controller->release();
        prepared.controller = nullptr;
        prepared.controllerFromComponent = false;
    }
    if (prepared.component != nullptr) {
        prepared.component->setActive(false);
        prepared.component->terminate();
        prepared.component->release();
        prepared.component = nullptr;
    }
}

bool prepareAudioProcessor(Steinberg::IPluginFactory* factory,
                           const Vst3PluginDescriptor& descriptor,
                           double sampleRate,
                           int maxBlockSize,
                           PreparedProcessor& prepared,
                           std::string& message,
                           bool realtime = false) {
    prepared.component = createAudioComponent(factory, descriptor, prepared.className);
    if (prepared.component == nullptr) {
        message = "No audio component class could be instantiated.";
        return false;
    }

    auto* host = new MinimalHostApplication();
    Steinberg::tresult initResult = Steinberg::kInternalError;
    try {
        initResult = prepared.component->initialize(host);
    } catch (...) {
        initResult = Steinberg::kInternalError;
    }
    host->release();
    if (initResult != Steinberg::kResultOk) {
        message = "Audio component initialization failed.";
        releasePreparedProcessor(prepared);
        return false;
    }

    Steinberg::TUID controllerCid {};
    if (prepared.component->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
        void* controllerObject = nullptr;
        try {
            if (factory->createInstance(controllerCid,
                                        Steinberg::Vst::IEditController::iid,
                                        &controllerObject) == Steinberg::kResultOk &&
                controllerObject != nullptr) {
                prepared.controller = static_cast<Steinberg::Vst::IEditController*>(controllerObject);
                prepared.controllerFromComponent = false;
            }
        } catch (...) {
            prepared.controller = nullptr;
        }
    }
    if (prepared.controller == nullptr) {
        void* controllerObject = nullptr;
        try {
            if (prepared.component->queryInterface(Steinberg::Vst::IEditController::iid, &controllerObject) == Steinberg::kResultOk &&
                controllerObject != nullptr) {
                prepared.controller = static_cast<Steinberg::Vst::IEditController*>(controllerObject);
                prepared.controllerFromComponent = true;
            }
        } catch (...) {
            prepared.controller = nullptr;
        }
    }
    if (prepared.controller != nullptr && !prepared.controllerFromComponent) {
        auto* controllerHost = new MinimalHostApplication();
        Steinberg::tresult controllerInitResult = Steinberg::kInternalError;
        try {
            controllerInitResult = prepared.controller->initialize(controllerHost);
        } catch (...) {
            controllerInitResult = Steinberg::kInternalError;
        }
        controllerHost->release();
        if (controllerInitResult != Steinberg::kResultOk) {
            prepared.controller->release();
            prepared.controller = nullptr;
            prepared.controllerFromComponent = false;
        } else {
            prepared.controllerInitialized = true;
        }
    }

    if (prepared.controller != nullptr &&
        prepared.component->queryInterface(Steinberg::Vst::IConnectionPoint::iid,
                                           reinterpret_cast<void**>(&prepared.componentConnection)) == Steinberg::kResultOk &&
        prepared.controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid,
                                            reinterpret_cast<void**>(&prepared.controllerConnection)) == Steinberg::kResultOk &&
        prepared.componentConnection != nullptr &&
        prepared.controllerConnection != nullptr) {
        prepared.componentConnection->connect(prepared.controllerConnection);
        prepared.controllerConnection->connect(prepared.componentConnection);
    }

    prepared.inputBusCount = prepared.component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    prepared.outputBusCount = prepared.component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
    if (prepared.inputBusCount > 0) {
        prepared.component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
    }
    if (prepared.outputBusCount > 0) {
        prepared.component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);
    }
    const int eventInputBusCount = prepared.component->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kInput);
    if (eventInputBusCount > 0) {
        prepared.component->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0, true);
    }

    if (prepared.component->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                                           reinterpret_cast<void**>(&prepared.processor)) != Steinberg::kResultOk ||
        prepared.processor == nullptr) {
        message = "Audio component does not expose IAudioProcessor.";
        releasePreparedProcessor(prepared);
        return false;
    }

    if (prepared.processor->canProcessSampleSize(Steinberg::Vst::kSample32) != Steinberg::kResultOk) {
        message = "Audio processor does not support 32-bit float processing.";
        releasePreparedProcessor(prepared);
        return false;
    }

    if (!setPreferredBusArrangement(prepared.processor,
                                    prepared.inputBusCount,
                                    prepared.outputBusCount,
                                    prepared.inputChannelCount,
                                    prepared.outputChannelCount)) {
        Steinberg::Vst::BusInfo inputBusInfo {};
        Steinberg::Vst::BusInfo outputBusInfo {};
        if (prepared.inputBusCount > 0 &&
            prepared.component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, inputBusInfo) == Steinberg::kResultOk) {
            prepared.inputChannelCount = std::max(1, std::min(2, static_cast<int>(inputBusInfo.channelCount)));
        }
        if (prepared.outputBusCount > 0 &&
            prepared.component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, outputBusInfo) == Steinberg::kResultOk) {
            prepared.outputChannelCount = std::max(1, std::min(2, static_cast<int>(outputBusInfo.channelCount)));
        }
    }

    Steinberg::Vst::ProcessSetup setup;
    setup.processMode = realtime ? Steinberg::Vst::kRealtime : Steinberg::Vst::kOffline;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = maxBlockSize > 0 ? maxBlockSize : 256;
    setup.sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
    if (prepared.processor->setupProcessing(setup) != Steinberg::kResultOk) {
        message = "Audio processor setupProcessing failed.";
        releasePreparedProcessor(prepared);
        return false;
    }

    if (prepared.component->setActive(true) != Steinberg::kResultOk) {
        message = "Audio component setActive(true) failed.";
        releasePreparedProcessor(prepared);
        return false;
    }
    if (prepared.processor->setProcessing(true) != Steinberg::kResultOk) {
        message = "Audio processor setProcessing(true) failed.";
        releasePreparedProcessor(prepared);
        return false;
    }

    prepared.latencySamples = prepared.processor->getLatencySamples();
    prepared.tailSamples = normalizedVst3TailSamples(prepared.processor->getTailSamples());
    return true;
}

class SimpleParamValueQueue final : public Steinberg::Vst::IParamValueQueue {
public:
    explicit SimpleParamValueQueue(Steinberg::Vst::ParamID parameterId) : parameterId_(parameterId) {}

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IParamValueQueue)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IParamValueQueue::iid, Steinberg::Vst::IParamValueQueue)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return parameterId_; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return static_cast<Steinberg::int32>(points_.size()); }

    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index,
                                           Steinberg::int32& sampleOffset,
                                           Steinberg::Vst::ParamValue& value) override {
        if (index < 0 || static_cast<size_t>(index) >= points_.size()) {
            return Steinberg::kInvalidArgument;
        }
        sampleOffset = points_[static_cast<size_t>(index)].first;
        value = points_[static_cast<size_t>(index)].second;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset,
                                           Steinberg::Vst::ParamValue value,
                                           Steinberg::int32& index) override {
        points_.push_back({sampleOffset, std::clamp(value, 0.0, 1.0)});
        index = static_cast<Steinberg::int32>(points_.size() - 1);
        return Steinberg::kResultOk;
    }

    bool latestValue(Steinberg::Vst::ParamValue& value) const {
        if (points_.empty()) {
            return false;
        }
        value = points_.back().second;
        return true;
    }

private:
    Steinberg::Vst::ParamID parameterId_ = 0;
    std::vector<std::pair<Steinberg::int32, Steinberg::Vst::ParamValue>> points_;
};

class SimpleParameterChanges final : public Steinberg::Vst::IParameterChanges {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IParameterChanges)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IParameterChanges::iid, Steinberg::Vst::IParameterChanges)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::int32 PLUGIN_API getParameterCount() override {
        return static_cast<Steinberg::int32>(queues_.size());
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override {
        if (index < 0 || static_cast<size_t>(index) >= queues_.size()) {
            return nullptr;
        }
        return &queues_[static_cast<size_t>(index)];
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id,
                                                                  Steinberg::int32& index) override {
        for (size_t queueIndex = 0; queueIndex < queues_.size(); ++queueIndex) {
            if (queues_[queueIndex].getParameterId() == id) {
                index = static_cast<Steinberg::int32>(queueIndex);
                return &queues_[queueIndex];
            }
        }
        queues_.emplace_back(id);
        index = static_cast<Steinberg::int32>(queues_.size() - 1);
        return &queues_.back();
    }

    void addValues(const std::vector<Vst3ParameterValueState>& parameters) {
        for (const auto& parameter : parameters) {
            Steinberg::int32 queueIndex = 0;
            auto* queue = addParameterData(parameter.parameterId, queueIndex);
            if (queue == nullptr) {
                continue;
            }
            Steinberg::int32 pointIndex = 0;
            queue->addPoint(0, std::clamp(parameter.normalizedValue, 0.0, 1.0), pointIndex);
        }
    }

    std::vector<Vst3ParameterValueState> latestValues() {
        std::vector<Vst3ParameterValueState> values;
        values.reserve(queues_.size());
        for (auto& queue : queues_) {
            Steinberg::Vst::ParamValue value = 0.0;
            if (!queue.latestValue(value)) {
                continue;
            }
            values.push_back({static_cast<uint32_t>(queue.getParameterId()),
                              "Output Param " + std::to_string(static_cast<uint32_t>(queue.getParameterId())),
                              static_cast<float>(std::clamp(value, 0.0, 1.0))});
        }
        return values;
    }

private:
    std::vector<SimpleParamValueQueue> queues_;
};

class SimpleEventList final : public Steinberg::Vst::IEventList {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IEventList)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IEventList::iid, Steinberg::Vst::IEventList)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::int32 PLUGIN_API getEventCount() override {
        return static_cast<Steinberg::int32>(events_.size());
    }

    Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& event) override {
        if (index < 0 || static_cast<size_t>(index) >= events_.size()) {
            return Steinberg::kInvalidArgument;
        }
        event = events_[static_cast<size_t>(index)];
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& event) override {
        events_.push_back(event);
        return Steinberg::kResultOk;
    }

    void clear() {
        events_.clear();
    }

    void addMidiEvents(const std::vector<Vst3MidiEvent>& midiEvents, int blockStartFrame, int blockFrameCount) {
        clear();
        const int blockEndFrame = blockStartFrame + blockFrameCount;
        for (const auto& midiEvent : midiEvents) {
            if (midiEvent.frameOffset < blockStartFrame || midiEvent.frameOffset >= blockEndFrame) {
                continue;
            }
            Steinberg::Vst::Event event {};
            event.busIndex = 0;
            event.sampleOffset = std::max(0, std::min(blockFrameCount - 1, midiEvent.frameOffset - blockStartFrame));
            event.ppqPosition = 0.0;
            event.flags = 0;
            if (midiEvent.kind == Vst3MidiEventKind::Controller) {
                if (midiEvent.controller < 0 || midiEvent.controller > 127) {
                    continue;
                }
                event.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
                event.midiCCOut.controlNumber = static_cast<Steinberg::uint8>(midiEvent.controller);
                event.midiCCOut.channel = static_cast<Steinberg::int8>(std::max(0, std::min(15, midiEvent.channel - 1)));
                event.midiCCOut.value = static_cast<Steinberg::int8>(std::max(0, std::min(127, midiEvent.value)));
                event.midiCCOut.value2 = 0;
            } else if (midiEvent.kind == Vst3MidiEventKind::PitchBend) {
                const int value = std::max(0, std::min(16383, midiEvent.value));
                event.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
                event.midiCCOut.controlNumber = static_cast<Steinberg::uint8>(Steinberg::Vst::kPitchBend);
                event.midiCCOut.channel = static_cast<Steinberg::int8>(std::max(0, std::min(15, midiEvent.channel - 1)));
                event.midiCCOut.value = static_cast<Steinberg::int8>(value & 0x7f);
                event.midiCCOut.value2 = static_cast<Steinberg::int8>((value >> 7) & 0x7f);
            } else if (midiEvent.kind == Vst3MidiEventKind::ProgramChange) {
                event.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
                event.midiCCOut.controlNumber = static_cast<Steinberg::uint8>(Steinberg::Vst::kCtrlProgramChange);
                event.midiCCOut.channel = static_cast<Steinberg::int8>(std::max(0, std::min(15, midiEvent.channel - 1)));
                event.midiCCOut.value = static_cast<Steinberg::int8>(std::max(0, std::min(127, midiEvent.program)));
                event.midiCCOut.value2 = 0;
            } else if (midiEvent.noteOn && midiEvent.velocity > 0) {
                if (midiEvent.pitch < 0 || midiEvent.pitch > 127) {
                    continue;
                }
                event.type = Steinberg::Vst::Event::kNoteOnEvent;
                event.noteOn.channel = static_cast<Steinberg::int16>(std::max(0, std::min(15, midiEvent.channel - 1)));
                event.noteOn.pitch = static_cast<Steinberg::int16>(midiEvent.pitch);
                event.noteOn.tuning = 0.0f;
                event.noteOn.velocity = static_cast<float>(std::max(1, std::min(127, midiEvent.velocity))) / 127.0f;
                event.noteOn.length = 0;
                event.noteOn.noteId = -1;
            } else {
                if (midiEvent.pitch < 0 || midiEvent.pitch > 127) {
                    continue;
                }
                event.type = Steinberg::Vst::Event::kNoteOffEvent;
                event.noteOff.channel = static_cast<Steinberg::int16>(std::max(0, std::min(15, midiEvent.channel - 1)));
                event.noteOff.pitch = static_cast<Steinberg::int16>(midiEvent.pitch);
                event.noteOff.velocity = 0.0f;
                event.noteOff.noteId = -1;
                event.noteOff.tuning = 0.0f;
            }
            events_.push_back(event);
        }
        std::sort(events_.begin(), events_.end(), [](const Steinberg::Vst::Event& left, const Steinberg::Vst::Event& right) {
            if (left.sampleOffset != right.sampleOffset) {
                return left.sampleOffset < right.sampleOffset;
            }
            return left.type < right.type;
        });
    }

private:
    std::vector<Steinberg::Vst::Event> events_;
};

struct LoadedVst3Module {
#if defined(_WIN32)
    HMODULE library = nullptr;
#elif defined(__APPLE__)
    CFBundleRef bundle = nullptr;
    void* library = nullptr;
    using BundleExitFn = bool (*)();
    BundleExitFn bundleExit = nullptr;
    bool bundleEntryCalled = false;
#else
    void* library = nullptr;
#endif
};

void closeModuleHandle(void* moduleHandle) {
    auto* module = static_cast<LoadedVst3Module*>(moduleHandle);
    if (module == nullptr) {
        return;
    }
#if defined(_WIN32)
    if (module->library != nullptr) {
        FreeLibrary(module->library);
    }
#elif defined(__APPLE__)
    if (module->bundle != nullptr) {
        if (module->bundleEntryCalled && module->bundleExit != nullptr) {
            module->bundleExit();
        }
        CFBundleUnloadExecutable(module->bundle);
        CFRelease(module->bundle);
    }
    if (module->library != nullptr) {
        dlclose(module->library);
    }
#else
    if (module->library != nullptr) {
        dlclose(module->library);
    }
#endif
    delete module;
}

void* openModuleHandle(const Vst3PluginDescriptor& descriptor, std::string& message) {
    auto* module = new LoadedVst3Module();
#if defined(_WIN32)
    module->library = LoadLibraryA(descriptor.executablePath.c_str());
    if (module->library == nullptr) {
        message = "Could not load VST3 module.";
        delete module;
        return nullptr;
    }
    return module;
#elif defined(__APPLE__)
    if (!descriptor.bundlePath.empty()) {
        CFStringRef bundlePath = CFStringCreateWithCString(kCFAllocatorDefault,
                                                           descriptor.bundlePath.c_str(),
                                                           kCFStringEncodingUTF8);
        if (bundlePath != nullptr) {
            CFURLRef bundleUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                                               bundlePath,
                                                               kCFURLPOSIXPathStyle,
                                                               true);
            CFRelease(bundlePath);
            if (bundleUrl != nullptr) {
                module->bundle = CFBundleCreate(kCFAllocatorDefault, bundleUrl);
                CFRelease(bundleUrl);
            }
        }
        if (module->bundle != nullptr && CFBundleLoadExecutable(module->bundle)) {
            using BundleEntryFn = bool (*)(CFBundleRef);
            auto* bundleEntry = reinterpret_cast<BundleEntryFn>(
                CFBundleGetFunctionPointerForName(module->bundle, CFSTR("bundleEntry")));
            module->bundleExit = reinterpret_cast<LoadedVst3Module::BundleExitFn>(
                CFBundleGetFunctionPointerForName(module->bundle, CFSTR("bundleExit")));
            if (bundleEntry != nullptr && !bundleEntry(module->bundle)) {
                message = "Calling VST3 bundleEntry failed.";
                CFBundleUnloadExecutable(module->bundle);
                CFRelease(module->bundle);
                module->bundle = nullptr;
                delete module;
                return nullptr;
            }
            module->bundleEntryCalled = bundleEntry != nullptr;
            return module;
        }
        if (module->bundle != nullptr) {
            CFRelease(module->bundle);
            module->bundle = nullptr;
        }
    }
    module->library = dlopen(descriptor.executablePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (module->library == nullptr) {
        const char* error = dlerror();
        message = error != nullptr ? error : "Could not load VST3 module.";
        delete module;
        return nullptr;
    }
    return module;
#else
    module->library = dlopen(descriptor.executablePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (module->library == nullptr) {
        const char* error = dlerror();
        message = error != nullptr ? error : "Could not load VST3 module.";
        delete module;
        return nullptr;
    }
    return module;
#endif
}

using GetPluginFactoryFn = Steinberg::IPluginFactory* (*)();

GetPluginFactoryFn findFactorySymbol(void* moduleHandle) {
    auto* module = static_cast<LoadedVst3Module*>(moduleHandle);
    if (module == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<GetPluginFactoryFn>(GetProcAddress(module->library, "GetPluginFactory"));
#elif defined(__APPLE__)
    if (module->bundle != nullptr) {
        return reinterpret_cast<GetPluginFactoryFn>(
            CFBundleGetFunctionPointerForName(module->bundle, CFSTR("GetPluginFactory")));
    }
    dlerror();
    return reinterpret_cast<GetPluginFactoryFn>(dlsym(module->library, "GetPluginFactory"));
#else
    dlerror();
    return reinterpret_cast<GetPluginFactoryFn>(dlsym(module->library, "GetPluginFactory"));
#endif
}
#endif

} // namespace

struct Vst3RealtimeProcessor::Impl {
    Vst3PluginDescriptor descriptor;
    Vst3ProcessorProbe preparedProbe;
    int maxBlockSize = 256;
    void* module = nullptr;
    // Out-of-process fallback for plugins that are unsafe to host in-process.
    std::unique_ptr<Vst3RealtimeBridgeClient> bridge;
    std::vector<Vst3ParameterValueState> bridgeOutputParameters;
#if defined(NEURACOUST_HAS_VST3_SDK)
    PreparedProcessor prepared;
    std::vector<float> inputLeft;
    std::vector<float> inputRight;
    std::vector<float> outputLeft;
    std::vector<float> outputRight;
    std::vector<std::vector<float>> silentInputBusLeft;
	    std::vector<std::vector<float>> silentInputBusRight;
	    std::vector<std::vector<float>> extraOutputBusLeft;
	    std::vector<std::vector<float>> extraOutputBusRight;
	    std::vector<Steinberg::Vst::AudioBusBuffers> inputBuses;
	    std::vector<Steinberg::Vst::AudioBusBuffers> outputBuses;
	    std::vector<std::array<Steinberg::Vst::Sample32*, 2>> inputChannelPointers;
	    std::vector<std::array<Steinberg::Vst::Sample32*, 2>> outputChannelPointers;
	    std::vector<Vst3ParameterValueState> outputParameterChanges;
	    Steinberg::int64 processedSamples = 0;
	    double sampleRate = 48000.0;
#endif
};

bool isVst3HostedOutOfProcess(const Vst3PluginDescriptor& descriptor) {
    return std::getenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3") == nullptr &&
           vst3RealtimeBridgeSupported() &&
           isKnownUnsafeForInProcessVst3Host(descriptor);
}

Vst3FactoryInspection inspectVst3FactoryWithSdk(const Vst3PluginDescriptor& descriptor) {
    Vst3FactoryInspection inspection;

#if !defined(NEURACOUST_HAS_VST3_SDK)
    inspection.message = "Steinberg VST3 SDK headers are not configured.";
    return inspection;
#else
    inspection.sdkAvailable = true;
    if (std::getenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3") == nullptr &&
        isKnownUnsafeForInProcessVst3Host(descriptor)) {
        inspection.message = knownUnsafeHostMessage(descriptor);
        return inspection;
    }
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        inspection.message = "VST3 executable path is missing.";
        return inspection;
    }

    ScopedPluginResourceDirectory resourceDirectory(descriptor);
    std::string openError;
    void* module = openModuleHandle(descriptor, openError);
    if (module == nullptr) {
        inspection.message = openError;
        return inspection;
    }
    inspection.opened = true;
    auto* symbol = findFactorySymbol(module);

    if (symbol == nullptr) {
        inspection.message = "VST3 factory symbol was not found.";
        closeModuleHandle(module);
        return inspection;
    }

    inspection.hasFactory = true;
    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        inspection.message = "VST3 factory returned null.";
        closeModuleHandle(module);
        return inspection;
    }

    Steinberg::PFactoryInfo factoryInfo;
    if (factory->getFactoryInfo(&factoryInfo) == Steinberg::kResultOk) {
        inspection.vendor = factoryInfo.vendor;
        inspection.url = factoryInfo.url;
    }

    inspection.classCount = factory->countClasses();
    for (int index = 0; index < inspection.classCount; ++index) {
        Steinberg::PClassInfo classInfo;
        if (factory->getClassInfo(index, &classInfo) == Steinberg::kResultOk) {
            bool created = false;
            if (std::string(classInfo.category) == vstAudioEffectCategory()) {
                created = createAndReleaseInstance(factory, classInfo.cid, Steinberg::Vst::IComponent::iid);
                if (created) {
                    ++inspection.componentInstanceCount;
                }
            } else if (std::string(classInfo.category) == vstControllerCategory()) {
                created = createAndReleaseInstance(factory, classInfo.cid, Steinberg::Vst::IEditController::iid);
                if (created) {
                    ++inspection.controllerInstanceCount;
                }
            }
            inspection.classes.push_back({
                tuidToHex(classInfo.cid),
                classInfo.category,
                classInfo.name,
                created,
                created
            });
        }
    }

    inspection.message = "VST3 SDK factory inspection completed.";
    closeModuleHandle(module);
    return inspection;
#endif
}

Vst3ProcessorProbe probeVst3ProcessorWithSdk(const Vst3PluginDescriptor& descriptor,
                                             double sampleRate,
                                             int maxBlockSize) {
    Vst3ProcessorProbe probe;

#if !defined(NEURACOUST_HAS_VST3_SDK)
    probe.message = "Steinberg VST3 SDK headers are not configured.";
    return probe;
#else
    probe.sdkAvailable = true;
    if (std::getenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3") == nullptr &&
        isKnownUnsafeForInProcessVst3Host(descriptor)) {
        probe.message = knownUnsafeHostMessage(descriptor);
        return probe;
    }
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        probe.message = "VST3 executable path is missing.";
        return probe;
    }

    ScopedPluginResourceDirectory resourceDirectory(descriptor);
    std::string openError;
    void* module = openModuleHandle(descriptor, openError);
    if (module == nullptr) {
        probe.message = openError;
        return probe;
    }
    probe.opened = true;

    auto* symbol = findFactorySymbol(module);
    if (symbol == nullptr) {
        probe.message = "VST3 factory symbol was not found.";
        closeModuleHandle(module);
        return probe;
    }
    probe.hasFactory = true;

    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        probe.message = "VST3 factory returned null.";
        closeModuleHandle(module);
        return probe;
    }

    auto* component = createAudioComponent(factory, descriptor, probe.className);
    if (component == nullptr) {
        probe.message = "No audio component class could be instantiated.";
        closeModuleHandle(module);
        return probe;
    }
    probe.componentCreated = true;

    auto* host = new MinimalHostApplication();
    Steinberg::tresult initResult = Steinberg::kInternalError;
    try {
        initResult = component->initialize(host);
    } catch (...) {
        initResult = Steinberg::kInternalError;
    }
    host->release();
    if (initResult != Steinberg::kResultOk) {
        probe.message = "Audio component initialization failed.";
        component->release();
        closeModuleHandle(module);
        return probe;
    }
    probe.initialized = true;

    probe.inputBusCount = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    probe.outputBusCount = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
    if (probe.inputBusCount > 0) {
        Steinberg::Vst::BusInfo busInfo;
        if (component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, busInfo) == Steinberg::kResultOk) {
            probe.mainInputChannels = busInfo.channelCount;
        }
        component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
    }
    if (probe.outputBusCount > 0) {
        Steinberg::Vst::BusInfo busInfo;
        if (component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, busInfo) == Steinberg::kResultOk) {
            probe.mainOutputChannels = busInfo.channelCount;
        }
        component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);
    }

    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    if (component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&processor)) == Steinberg::kResultOk && processor != nullptr) {
        probe.audioProcessorAvailable = true;
        probe.sample32Supported = processor->canProcessSampleSize(Steinberg::Vst::kSample32) == Steinberg::kResultOk;

        int negotiatedInputChannels = probe.mainInputChannels > 0 ? probe.mainInputChannels : 2;
        int negotiatedOutputChannels = probe.mainOutputChannels > 0 ? probe.mainOutputChannels : 2;
        probe.busArrangementAccepted = setPreferredBusArrangement(processor,
                                                                  probe.inputBusCount,
                                                                  probe.outputBusCount,
                                                                  negotiatedInputChannels,
                                                                  negotiatedOutputChannels);
        probe.mainInputChannels = negotiatedInputChannels;
        probe.mainOutputChannels = negotiatedOutputChannels;

        Steinberg::Vst::ProcessSetup setup;
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = maxBlockSize > 0 ? maxBlockSize : 256;
        setup.sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        probe.setupProcessingOk = processor->setupProcessing(setup) == Steinberg::kResultOk;
        probe.latencySamples = processor->getLatencySamples();
        probe.tailSamples = normalizedVst3TailSamples(processor->getTailSamples());
        processor->release();
    }

    component->terminate();
    component->release();
    closeModuleHandle(module);
    probe.message = probe.setupProcessingOk
        ? "VST3 processor readiness probe completed."
        : "VST3 processor was inspected, but setupProcessing did not complete.";
    return probe;
#endif
}

Vst3ParameterInspection inspectVst3ParametersWithSdk(const Vst3PluginDescriptor& descriptor,
                                                     int maxParameters) {
    Vst3ParameterInspection inspection;

#if !defined(NEURACOUST_HAS_VST3_SDK)
    inspection.message = "Steinberg VST3 SDK headers are not configured.";
    return inspection;
#else
    inspection.sdkAvailable = true;
    if (std::getenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3") == nullptr &&
        isKnownUnsafeForInProcessVst3Host(descriptor)) {
        inspection.message = knownUnsafeHostMessage(descriptor);
        return inspection;
    }
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        inspection.message = "VST3 executable path is missing.";
        return inspection;
    }

    ScopedPluginResourceDirectory resourceDirectory(descriptor);
    std::string openError;
    void* module = openModuleHandle(descriptor, openError);
    if (module == nullptr) {
        inspection.message = openError;
        return inspection;
    }
    inspection.opened = true;

    auto* symbol = findFactorySymbol(module);
    if (symbol == nullptr) {
        inspection.message = "VST3 factory symbol was not found.";
        closeModuleHandle(module);
        return inspection;
    }
    inspection.hasFactory = true;

    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        inspection.message = "VST3 factory returned null.";
        closeModuleHandle(module);
        return inspection;
    }

    auto* host = new MinimalHostApplication();
    std::string className;
    Steinberg::Vst::IComponent* component = createAudioComponent(factory, descriptor, className);
    bool componentInitialized = false;
    Steinberg::Vst::IConnectionPoint* componentConnection = nullptr;
    Steinberg::Vst::IConnectionPoint* controllerConnection = nullptr;
    bool componentControllerConnected = false;
    Steinberg::Vst::IEditController* controller = nullptr;
    bool controllerFromComponent = false;
    bool controllerInitialized = false;
    if (component != nullptr) {
        Steinberg::tresult componentInitResult = Steinberg::kInternalError;
        try {
            componentInitResult = component->initialize(host);
        } catch (...) {
            componentInitResult = Steinberg::kInternalError;
        }
        componentInitialized = componentInitResult == Steinberg::kResultOk;
    }
    if (componentInitialized) {
        Steinberg::TUID controllerCid {};
        void* object = nullptr;
        if (component->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
            Steinberg::tresult controllerResult = Steinberg::kInternalError;
            try {
                controllerResult = factory->createInstance(controllerCid, Steinberg::Vst::IEditController::iid, &object);
            } catch (...) {
                controllerResult = Steinberg::kInternalError;
            }
            if (controllerResult == Steinberg::kResultOk && object != nullptr) {
                controller = static_cast<Steinberg::Vst::IEditController*>(object);
                controllerFromComponent = false;
            }
        }
    }
    if (controller == nullptr && componentInitialized && component != nullptr) {
        void* object = nullptr;
        try {
            if (component->queryInterface(Steinberg::Vst::IEditController::iid, &object) == Steinberg::kResultOk && object != nullptr) {
                controller = static_cast<Steinberg::Vst::IEditController*>(object);
                className = className.empty() ? descriptor.componentClassName : className;
                controllerFromComponent = true;
            }
        } catch (...) {
            object = nullptr;
        }
    }
    if (controller == nullptr) {
        if (component != nullptr) {
            if (componentInitialized) {
                component->terminate();
            }
            component->release();
            component = nullptr;
            componentInitialized = false;
        }
        controller = createEditControllerForAudioComponent(factory, descriptor, className);
        controllerFromComponent = false;
    }
    if (controller == nullptr) {
        controller = createFirstEditController(factory, className);
        controllerFromComponent = false;
    }
    if (controller == nullptr) {
        inspection.message = "No VST3 edit controller class could be instantiated.";
        host->release();
        closeModuleHandle(module);
        return inspection;
    }
    inspection.controllerCreated = true;
    inspection.className = className;

    Steinberg::tresult initResult = controllerFromComponent ? Steinberg::kResultOk : Steinberg::kInternalError;
    if (!controllerFromComponent) {
        try {
            initResult = controller->initialize(host);
        } catch (...) {
            initResult = Steinberg::kInternalError;
        }
    }
    host->release();
    if (initResult != Steinberg::kResultOk) {
        inspection.message = "VST3 edit controller initialization failed.";
        controller->release();
        if (component != nullptr) {
            if (componentInitialized) {
                component->terminate();
            }
            component->release();
        }
        closeModuleHandle(module);
        return inspection;
    }
    controllerInitialized = !controllerFromComponent;
    inspection.initialized = true;

    if (component != nullptr &&
        component->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&componentConnection)) == Steinberg::kResultOk &&
        controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&controllerConnection)) == Steinberg::kResultOk &&
        componentConnection != nullptr &&
        controllerConnection != nullptr) {
        const bool componentAccepted = componentConnection->connect(controllerConnection) == Steinberg::kResultOk;
        const bool controllerAccepted = controllerConnection->connect(componentConnection) == Steinberg::kResultOk;
        componentControllerConnected = componentAccepted && controllerAccepted;
    }

    const int rawParameterCount = controller->getParameterCount();
    const int limit = std::max(0, maxParameters);
    for (int index = 0; index < rawParameterCount; ++index) {
        Steinberg::Vst::ParameterInfo info;
        if (controller->getParameterInfo(index, info) != Steinberg::kResultOk) {
            continue;
        }
        if ((info.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0) {
            continue;
        }
        ++inspection.parameterCount;
        if (static_cast<int>(inspection.parameters.size()) >= limit) {
            continue;
        }
        Vst3ParameterDescriptor parameter;
        parameter.id = static_cast<uint32_t>(info.id);
        parameter.title = vstString128ToUtf8(info.title);
        parameter.units = vstString128ToUtf8(info.units);
        parameter.defaultNormalized = std::clamp(info.defaultNormalizedValue, 0.0, 1.0);
        parameter.currentNormalized = std::clamp(controller->getParamNormalized(info.id), 0.0, 1.0);
        parameter.defaultDisplay = paramValueToDisplayString(controller, info.id, parameter.defaultNormalized);
        parameter.currentDisplay = paramValueToDisplayString(controller, info.id, parameter.currentNormalized);
        parameter.stepCount = info.stepCount;
        parameter.flags = info.flags;
        if (parameter.title.empty()) {
            parameter.title = "Parameter " + std::to_string(index + 1);
        }
        inspection.parameters.push_back(parameter);
    }

    if (controllerInitialized) {
        controller->terminate();
    }
    if (componentConnection != nullptr && controllerConnection != nullptr) {
        if (componentControllerConnected) {
            componentConnection->disconnect(controllerConnection);
            controllerConnection->disconnect(componentConnection);
        }
        componentConnection->release();
        controllerConnection->release();
    } else {
        if (componentConnection != nullptr) {
            componentConnection->release();
        }
        if (controllerConnection != nullptr) {
            controllerConnection->release();
        }
    }
    controller->release();
    if (component != nullptr) {
        if (componentInitialized) {
            component->terminate();
        }
        component->release();
    }
    closeModuleHandle(module);
    inspection.message = inspection.parameters.empty()
        ? "VST3 controller has no exposed parameters."
        : "VST3 parameters inspected.";
    return inspection;
#endif
}

Vst3ProcessResult processStereoBufferWithVst3(const Vst3PluginDescriptor& descriptor,
                                              WavAudioData& audio,
                                              int maxBlockSize,
                                              const std::vector<Vst3ParameterValueState>& parameters) {
    Vst3ProcessResult result;

#if !defined(NEURACOUST_HAS_VST3_SDK)
    result.message = "Steinberg VST3 SDK headers are not configured.";
    return result;
#else
    result.sdkAvailable = true;
    if (std::getenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3") == nullptr &&
        isKnownUnsafeForInProcessVst3Host(descriptor)) {
        result.message = knownUnsafeHostMessage(descriptor);
        return result;
    }
    if (audio.channels != 2 || audio.sampleRate <= 0 || audio.interleavedSamples.empty()) {
        result.message = "VST3 processing requires a non-empty stereo buffer.";
        return result;
    }
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        result.message = "VST3 executable path is missing.";
        return result;
    }

    ScopedPluginResourceDirectory resourceDirectory(descriptor);
    std::string openError;
    void* module = openModuleHandle(descriptor, openError);
    if (module == nullptr) {
        result.message = openError;
        return result;
    }
    result.opened = true;

    auto* symbol = findFactorySymbol(module);
    if (symbol == nullptr) {
        result.message = "VST3 factory symbol was not found.";
        closeModuleHandle(module);
        return result;
    }

    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        result.message = "VST3 factory returned null.";
        closeModuleHandle(module);
        return result;
    }

    PreparedProcessor prepared;
    std::string prepareMessage;
    if (!prepareAudioProcessor(factory, descriptor, audio.sampleRate, maxBlockSize, prepared, prepareMessage)) {
        result.message = prepareMessage;
        closeModuleHandle(module);
        return result;
    }

    result.className = prepared.className;
    result.latencySamples = prepared.latencySamples;
    result.tailSamples = prepared.tailSamples;

    const int blockSize = maxBlockSize > 0 ? maxBlockSize : 256;
    const auto totalFrames = static_cast<int>(audio.frameCount());
    std::vector<float> inputLeft(blockSize, 0.0f);
    std::vector<float> inputRight(blockSize, 0.0f);
    std::vector<float> outputLeft(blockSize, 0.0f);
    std::vector<float> outputRight(blockSize, 0.0f);

    Steinberg::Vst::AudioBusBuffers inputBus;
    Steinberg::Vst::AudioBusBuffers outputBus;
    inputBus.numChannels = prepared.inputChannelCount;
    outputBus.numChannels = prepared.outputChannelCount;
    inputBus.silenceFlags = 0;
    outputBus.silenceFlags = 0;
    Steinberg::Vst::Sample32* inputChannels[2] = {inputLeft.data(), inputRight.data()};
    Steinberg::Vst::Sample32* outputChannels[2] = {outputLeft.data(), outputRight.data()};
    inputBus.channelBuffers32 = inputChannels;
    outputBus.channelBuffers32 = outputChannels;

    for (int frameStart = 0; frameStart < totalFrames; frameStart += blockSize) {
        const int framesThisBlock = (std::min)(blockSize, totalFrames - frameStart);
        std::fill(inputLeft.begin(), inputLeft.end(), 0.0f);
        std::fill(inputRight.begin(), inputRight.end(), 0.0f);
        std::fill(outputLeft.begin(), outputLeft.end(), 0.0f);
        std::fill(outputRight.begin(), outputRight.end(), 0.0f);

        for (int frame = 0; frame < framesThisBlock; ++frame) {
            const auto index = static_cast<size_t>((frameStart + frame) * 2);
            if (prepared.inputChannelCount == 1) {
                inputLeft[frame] = (audio.interleavedSamples[index] + audio.interleavedSamples[index + 1]) * 0.5f;
                inputRight[frame] = 0.0f;
            } else {
                inputLeft[frame] = audio.interleavedSamples[index];
                inputRight[frame] = audio.interleavedSamples[index + 1];
            }
        }

        Steinberg::Vst::ProcessData processData {};
        Steinberg::Vst::ProcessContext processContext {};
        processContext.state =
            Steinberg::Vst::ProcessContext::kPlaying |
            Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
            Steinberg::Vst::ProcessContext::kTempoValid |
            Steinberg::Vst::ProcessContext::kTimeSigValid |
            Steinberg::Vst::ProcessContext::kContTimeValid;
        processContext.sampleRate = audio.sampleRate;
        processContext.projectTimeSamples = frameStart;
        processContext.continousTimeSamples = frameStart;
        processContext.projectTimeMusic = audio.sampleRate > 0
            ? (static_cast<double>(frameStart) / static_cast<double>(audio.sampleRate)) * (120.0 / 60.0)
            : 0.0;
        processContext.tempo = 120.0;
        processContext.timeSigNumerator = 4;
        processContext.timeSigDenominator = 4;
        processData.processMode = Steinberg::Vst::kOffline;
        processData.symbolicSampleSize = Steinberg::Vst::kSample32;
        processData.numSamples = framesThisBlock;
        processData.processContext = &processContext;
        processData.numInputs = prepared.inputBusCount > 0 ? 1 : 0;
        processData.numOutputs = prepared.outputBusCount > 0 ? 1 : 0;
        processData.inputs = prepared.inputBusCount > 0 ? &inputBus : nullptr;
        processData.outputs = prepared.outputBusCount > 0 ? &outputBus : nullptr;
        SimpleParameterChanges parameterChanges;
        if (!parameters.empty()) {
            parameterChanges.addValues(parameters);
            processData.inputParameterChanges = &parameterChanges;
        }

        if (prepared.processor->process(processData) != Steinberg::kResultOk) {
            result.message = "VST3 process() failed.";
            releasePreparedProcessor(prepared);
            closeModuleHandle(module);
            return result;
        }

        for (int frame = 0; frame < framesThisBlock; ++frame) {
            const auto index = static_cast<size_t>((frameStart + frame) * 2);
            audio.interleavedSamples[index] = std::clamp(outputLeft[frame], -1.0f, 1.0f);
            audio.interleavedSamples[index + 1] = std::clamp(prepared.outputChannelCount == 1 ? outputLeft[frame] : outputRight[frame],
                                                             -1.0f,
                                                             1.0f);
        }
        result.framesProcessed += framesThisBlock;
    }

    releasePreparedProcessor(prepared);
    closeModuleHandle(module);
    result.processed = true;
    result.message = "VST3 stereo buffer processed.";
    return result;
#endif
}

Vst3ProcessResult processMidiInstrumentWithVst3(const Vst3PluginDescriptor& descriptor,
                                                const std::vector<Vst3MidiEvent>& midiEvents,
                                                WavAudioData& outputAudio,
                                                int maxBlockSize,
                                                const std::vector<Vst3ParameterValueState>& parameters) {
    Vst3ProcessResult result;

#if !defined(NEURACOUST_HAS_VST3_SDK)
    result.message = "Steinberg VST3 SDK headers are not configured.";
    return result;
#else
    result.sdkAvailable = true;
    if (std::getenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3") == nullptr &&
        isKnownUnsafeForInProcessVst3Host(descriptor)) {
        result.message = knownUnsafeHostMessage(descriptor);
        return result;
    }
    if (outputAudio.channels != 2 || outputAudio.sampleRate <= 0 || outputAudio.interleavedSamples.empty()) {
        result.message = "VST3 instrument processing requires a prepared stereo output buffer.";
        return result;
    }
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        result.message = "VST3 executable path is missing.";
        return result;
    }

    ScopedPluginResourceDirectory resourceDirectory(descriptor);
    std::string openError;
    void* module = openModuleHandle(descriptor, openError);
    if (module == nullptr) {
        result.message = openError;
        return result;
    }
    result.opened = true;

    auto* symbol = findFactorySymbol(module);
    if (symbol == nullptr) {
        result.message = "VST3 factory symbol was not found.";
        closeModuleHandle(module);
        return result;
    }

    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        result.message = "VST3 factory returned null.";
        closeModuleHandle(module);
        return result;
    }

    PreparedProcessor prepared;
    std::string prepareMessage;
    if (!prepareAudioProcessor(factory, descriptor, outputAudio.sampleRate, maxBlockSize, prepared, prepareMessage)) {
        result.message = prepareMessage;
        closeModuleHandle(module);
        return result;
    }
    if (prepared.outputBusCount <= 0 || prepared.outputChannelCount <= 0) {
        result.message = "VST3 instrument has no active audio output bus.";
        releasePreparedProcessor(prepared);
        closeModuleHandle(module);
        return result;
    }

    result.className = prepared.className;
    result.latencySamples = prepared.latencySamples;
    result.tailSamples = prepared.tailSamples;

    const int blockSize = maxBlockSize > 0 ? maxBlockSize : 256;
    const auto totalFrames = static_cast<int>(outputAudio.frameCount());
    std::fill(outputAudio.interleavedSamples.begin(), outputAudio.interleavedSamples.end(), 0.0f);
    std::vector<float> outputLeft(blockSize, 0.0f);
    std::vector<float> outputRight(blockSize, 0.0f);

    Steinberg::Vst::AudioBusBuffers outputBus;
    outputBus.numChannels = prepared.outputChannelCount;
    outputBus.silenceFlags = 0;
    Steinberg::Vst::Sample32* outputChannels[2] = {outputLeft.data(), outputRight.data()};
    outputBus.channelBuffers32 = outputChannels;

    SimpleParameterChanges parameterChanges;
    if (!parameters.empty()) {
        parameterChanges.addValues(parameters);
    }
    SimpleEventList inputEvents;

    for (int frameStart = 0; frameStart < totalFrames; frameStart += blockSize) {
        const int framesThisBlock = (std::min)(blockSize, totalFrames - frameStart);
        std::fill(outputLeft.begin(), outputLeft.end(), 0.0f);
        std::fill(outputRight.begin(), outputRight.end(), 0.0f);
        inputEvents.addMidiEvents(midiEvents, frameStart, framesThisBlock);

        Steinberg::Vst::ProcessData processData {};
        processData.processMode = Steinberg::Vst::kOffline;
        processData.symbolicSampleSize = Steinberg::Vst::kSample32;
        processData.numSamples = framesThisBlock;
        processData.numInputs = 0;
        processData.numOutputs = 1;
        processData.inputs = nullptr;
        processData.outputs = &outputBus;
        processData.inputEvents = inputEvents.getEventCount() > 0 ? &inputEvents : nullptr;
        if (!parameters.empty()) {
            processData.inputParameterChanges = &parameterChanges;
        }

        if (prepared.processor->process(processData) != Steinberg::kResultOk) {
            result.message = "VST3 instrument process() failed.";
            releasePreparedProcessor(prepared);
            closeModuleHandle(module);
            return result;
        }

        for (int frame = 0; frame < framesThisBlock; ++frame) {
            const auto index = static_cast<size_t>((frameStart + frame) * 2);
            outputAudio.interleavedSamples[index] = std::clamp(outputLeft[frame], -1.0f, 1.0f);
            outputAudio.interleavedSamples[index + 1] = std::clamp(prepared.outputChannelCount == 1 ? outputLeft[frame] : outputRight[frame],
                                                                   -1.0f,
                                                                   1.0f);
        }
        result.framesProcessed += framesThisBlock;
    }

    releasePreparedProcessor(prepared);
    closeModuleHandle(module);
    result.processed = true;
    result.message = "VST3 instrument MIDI buffer processed.";
    return result;
#endif
}

Vst3RealtimeProcessor::Vst3RealtimeProcessor() : impl_(std::make_unique<Impl>()) {}
Vst3RealtimeProcessor::~Vst3RealtimeProcessor() { reset(); }
Vst3RealtimeProcessor::Vst3RealtimeProcessor(Vst3RealtimeProcessor&&) noexcept = default;
Vst3RealtimeProcessor& Vst3RealtimeProcessor::operator=(Vst3RealtimeProcessor&&) noexcept = default;

bool Vst3RealtimeProcessor::prepare(const Vst3PluginDescriptor& descriptor,
                                    double sampleRate,
                                    int maxBlockSize,
                                    std::string& message,
                                    const std::string& bridgeShmKey,
                                    bool forceOutOfProcess) {
    reset();
    message.clear();
    impl_->descriptor = descriptor;
    impl_->maxBlockSize = maxBlockSize > 0 ? maxBlockSize : 256;

#if !defined(NEURACOUST_HAS_VST3_SDK)
    impl_->preparedProbe.message = "Steinberg VST3 SDK headers are not configured.";
    message = impl_->preparedProbe.message;
    return false;
#else
    impl_->preparedProbe.sdkAvailable = true;
    // Host out-of-process (sandbox bridge) when the plugin is either unsafe to
    // load inside the DAW process, or explicitly designated Internal DSP
    // (forceOutOfProcess) so third-party plugins run on the isolated core.
    const bool blockedInProcess = std::getenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3") == nullptr &&
        isKnownUnsafeForInProcessVst3Host(descriptor);
    if (forceOutOfProcess || blockedInProcess) {
        std::string bridgeMessage;
        if (vst3RealtimeBridgeSupported()) {
            const std::string workerPath = defaultVst3ProcessWorkerPath();
            auto bridge = std::make_unique<Vst3RealtimeBridgeClient>();
            const std::string preferredShmName = bridgeShmKey.empty()
                ? std::string{}
                : vst3BridgeObserverShmName(bridgeShmKey);
            if (!workerPath.empty() &&
                bridge->prepare(descriptor, sampleRate, impl_->maxBlockSize, workerPath, bridgeMessage, preferredShmName)) {
                impl_->bridge = std::move(bridge);
                impl_->preparedProbe.sdkAvailable = true;
                impl_->preparedProbe.opened = true;
                impl_->preparedProbe.hasFactory = true;
                impl_->preparedProbe.componentCreated = true;
                impl_->preparedProbe.initialized = true;
                impl_->preparedProbe.audioProcessorAvailable = true;
                impl_->preparedProbe.sample32Supported = true;
                impl_->preparedProbe.busArrangementAccepted = true;
                impl_->preparedProbe.setupProcessingOk = true;
                impl_->preparedProbe.latencySamples = impl_->bridge->latencySamples();
                impl_->preparedProbe.tailSamples = impl_->bridge->tailSamples();
                impl_->preparedProbe.className = impl_->bridge->className();
                impl_->preparedProbe.message = "VST3 hosted out-of-process (sandboxed): " + bridgeMessage;
                message = impl_->preparedProbe.message;
                return true;
            }
        }
        if (blockedInProcess) {
            // Unsafe in-process AND the sandbox failed → cannot host this plugin.
            impl_->preparedProbe.message = bridgeMessage.empty()
                ? knownUnsafeHostMessage(descriptor)
                : (knownUnsafeHostMessage(descriptor) + " Sandboxed host failed: " + bridgeMessage);
            message = impl_->preparedProbe.message;
            return false;
        }
        // Internal-DSP request on an in-process-safe plugin, but the sandbox could
        // not start — fall back to hosting it in-process rather than going silent.
    }
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        impl_->preparedProbe.message = "VST3 executable path is missing.";
        message = impl_->preparedProbe.message;
        return false;
    }

    ScopedPluginResourceDirectory resourceDirectory(descriptor);
    std::string openError;
    impl_->module = openModuleHandle(descriptor, openError);
    if (impl_->module == nullptr) {
        impl_->preparedProbe.message = openError;
        message = impl_->preparedProbe.message;
        return false;
    }
    impl_->preparedProbe.opened = true;

    auto* symbol = findFactorySymbol(impl_->module);
    if (symbol == nullptr) {
        impl_->preparedProbe.message = "VST3 factory symbol was not found.";
        reset();
        message = impl_->preparedProbe.message;
        return false;
    }
    impl_->preparedProbe.hasFactory = true;

    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        impl_->preparedProbe.message = "VST3 factory returned null.";
        reset();
        message = impl_->preparedProbe.message;
        return false;
    }

    std::string prepareMessage;
    if (!prepareAudioProcessor(factory, descriptor, sampleRate, impl_->maxBlockSize, impl_->prepared, prepareMessage, true)) {
        impl_->preparedProbe.message = prepareMessage;
        reset();
        message = prepareMessage;
        return false;
    }
    impl_->processedSamples = 0;
    impl_->sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;

    impl_->inputLeft.assign(static_cast<size_t>(impl_->maxBlockSize), 0.0f);
    impl_->inputRight.assign(static_cast<size_t>(impl_->maxBlockSize), 0.0f);
    impl_->outputLeft.assign(static_cast<size_t>(impl_->maxBlockSize), 0.0f);
    impl_->outputRight.assign(static_cast<size_t>(impl_->maxBlockSize), 0.0f);
    const int extraInputBusCount = std::max(0, impl_->prepared.inputBusCount - 1);
    const int extraOutputBusCount = std::max(0, impl_->prepared.outputBusCount - 1);
    impl_->silentInputBusLeft.assign(static_cast<size_t>(extraInputBusCount),
                                     std::vector<float>(static_cast<size_t>(impl_->maxBlockSize), 0.0f));
    impl_->silentInputBusRight.assign(static_cast<size_t>(extraInputBusCount),
                                      std::vector<float>(static_cast<size_t>(impl_->maxBlockSize), 0.0f));
	    impl_->extraOutputBusLeft.assign(static_cast<size_t>(extraOutputBusCount),
	                                     std::vector<float>(static_cast<size_t>(impl_->maxBlockSize), 0.0f));
	    impl_->extraOutputBusRight.assign(static_cast<size_t>(extraOutputBusCount),
	                                      std::vector<float>(static_cast<size_t>(impl_->maxBlockSize), 0.0f));
	    const int activeInputBusCount = std::max(0, impl_->prepared.inputBusCount);
	    const int activeOutputBusCount = std::max(0, impl_->prepared.outputBusCount);
	    impl_->inputBuses.assign(static_cast<size_t>(activeInputBusCount), {});
	    impl_->outputBuses.assign(static_cast<size_t>(activeOutputBusCount), {});
	    impl_->inputChannelPointers.assign(static_cast<size_t>(activeInputBusCount), {});
	    impl_->outputChannelPointers.assign(static_cast<size_t>(activeOutputBusCount), {});

	    impl_->preparedProbe.componentCreated = true;
    impl_->preparedProbe.initialized = true;
    impl_->preparedProbe.audioProcessorAvailable = true;
    impl_->preparedProbe.sample32Supported = true;
    impl_->preparedProbe.busArrangementAccepted = true;
    impl_->preparedProbe.setupProcessingOk = true;
    impl_->preparedProbe.inputBusCount = impl_->prepared.inputBusCount;
    impl_->preparedProbe.outputBusCount = impl_->prepared.outputBusCount;
    impl_->preparedProbe.latencySamples = impl_->prepared.latencySamples;
    impl_->preparedProbe.tailSamples = impl_->prepared.tailSamples;
    impl_->preparedProbe.className = impl_->prepared.className;
    impl_->preparedProbe.message = "VST3 realtime processor prepared.";
    message = impl_->preparedProbe.message;
    return true;
#endif
}

void Vst3RealtimeProcessor::reset() {
    if (!impl_) {
        return;
    }
    if (impl_->bridge) {
        impl_->bridge->reset();
        impl_->bridge.reset();
    }
    impl_->bridgeOutputParameters.clear();
#if defined(NEURACOUST_HAS_VST3_SDK)
    releasePreparedProcessor(impl_->prepared);
    impl_->inputLeft.clear();
    impl_->inputRight.clear();
	    impl_->outputLeft.clear();
	    impl_->outputRight.clear();
	    impl_->silentInputBusLeft.clear();
	    impl_->silentInputBusRight.clear();
	    impl_->extraOutputBusLeft.clear();
	    impl_->extraOutputBusRight.clear();
	    impl_->inputBuses.clear();
	    impl_->outputBuses.clear();
	    impl_->inputChannelPointers.clear();
	    impl_->outputChannelPointers.clear();
	    if (impl_->module != nullptr) {
	        if (!descriptorLooksLikeWaves(impl_->descriptor)) {
	            closeModuleHandle(impl_->module);
	        }
	        impl_->module = nullptr;
	    }
#endif
}

bool Vst3RealtimeProcessor::isPrepared() const {
    if (impl_ && impl_->bridge) {
        return impl_->bridge->isReady();
    }
#if defined(NEURACOUST_HAS_VST3_SDK)
    if (!impl_) {
        return false;
    }
    return impl_->module != nullptr && impl_->prepared.processor != nullptr;
#else
    return false;
#endif
}

Vst3ProcessorProbe Vst3RealtimeProcessor::probe() const {
    if (!impl_) {
        return {};
    }
    return impl_->preparedProbe;
}

Vst3ProcessResult Vst3RealtimeProcessor::processInterleavedStereo(float* interleavedStereo,
                                                                  int frameCount,
                                                                  std::string& message) {
    static const std::vector<Vst3ParameterValueState> emptyParameters;
    return processInterleavedStereo(interleavedStereo, frameCount, emptyParameters, message);
}

Vst3ProcessResult Vst3RealtimeProcessor::processInterleavedStereo(float* interleavedStereo,
                                                                  int frameCount,
                                                                  const std::vector<Vst3ParameterValueState>& parameters,
                                                                  std::string& message) {
    Vst3ProcessResult result;
    message.clear();
    if (impl_ && impl_->bridge) {
        result.sdkAvailable = true;
        std::vector<Vst3ParameterValueState> outputParameters;
        if (impl_->bridge->process(interleavedStereo, frameCount, parameters, outputParameters, message)) {
            if (!outputParameters.empty()) {
                impl_->bridgeOutputParameters.insert(impl_->bridgeOutputParameters.end(),
                                                     outputParameters.begin(),
                                                     outputParameters.end());
            }
            result.opened = true;
            result.processed = true;
            result.framesProcessed = frameCount;
            result.latencySamples = impl_->bridge->latencySamples();
            result.tailSamples = impl_->bridge->tailSamples();
            result.className = impl_->bridge->className();
            result.message = "VST3 realtime block processed out-of-process.";
        } else {
            result.message = message.empty() ? "Out-of-process VST3 block was not processed." : message;
        }
        message = result.message;
        return result;
    }
#if !defined(NEURACOUST_HAS_VST3_SDK)
    result.message = "Steinberg VST3 SDK headers are not configured.";
    message = result.message;
    return result;
#else
    result.sdkAvailable = true;
    if (!isPrepared()) {
        result.message = "VST3 realtime processor is not prepared.";
        message = result.message;
        return result;
    }
    if (interleavedStereo == nullptr || frameCount <= 0 || frameCount > impl_->maxBlockSize) {
        result.message = "VST3 realtime process block is invalid.";
        message = result.message;
        return result;
    }

    std::fill(impl_->inputLeft.begin(), impl_->inputLeft.end(), 0.0f);
    std::fill(impl_->inputRight.begin(), impl_->inputRight.end(), 0.0f);
    std::fill(impl_->outputLeft.begin(), impl_->outputLeft.end(), 0.0f);
    std::fill(impl_->outputRight.begin(), impl_->outputRight.end(), 0.0f);
    for (auto& bus : impl_->silentInputBusLeft) {
        std::fill(bus.begin(), bus.end(), 0.0f);
    }
    for (auto& bus : impl_->silentInputBusRight) {
        std::fill(bus.begin(), bus.end(), 0.0f);
    }
    for (auto& bus : impl_->extraOutputBusLeft) {
        std::fill(bus.begin(), bus.end(), 0.0f);
    }
    for (auto& bus : impl_->extraOutputBusRight) {
        std::fill(bus.begin(), bus.end(), 0.0f);
    }
    for (int frame = 0; frame < frameCount; ++frame) {
        const auto index = static_cast<size_t>(frame * 2);
        if (impl_->prepared.inputChannelCount == 1) {
            impl_->inputLeft[static_cast<size_t>(frame)] = (interleavedStereo[index] + interleavedStereo[index + 1]) * 0.5f;
            impl_->inputRight[static_cast<size_t>(frame)] = 0.0f;
        } else {
            impl_->inputLeft[static_cast<size_t>(frame)] = interleavedStereo[index];
            impl_->inputRight[static_cast<size_t>(frame)] = interleavedStereo[index + 1];
        }
    }

	    for (size_t busIndex = 0; busIndex < impl_->inputBuses.size(); ++busIndex) {
	        const int channelCount = busIndex == 0 ? impl_->prepared.inputChannelCount : 2;
	        impl_->inputBuses[busIndex].numChannels = channelCount;
	        impl_->inputBuses[busIndex].silenceFlags = busIndex == 0 ? 0 : ((1 << std::min(channelCount, 2)) - 1);
	        if (busIndex == 0) {
	            impl_->inputChannelPointers[busIndex] = {impl_->inputLeft.data(), impl_->inputRight.data()};
	        } else {
	            const size_t auxIndex = busIndex - 1u;
	            impl_->inputChannelPointers[busIndex] = {
	                auxIndex < impl_->silentInputBusLeft.size() ? impl_->silentInputBusLeft[auxIndex].data() : impl_->inputLeft.data(),
	                auxIndex < impl_->silentInputBusRight.size() ? impl_->silentInputBusRight[auxIndex].data() : impl_->inputRight.data()
	            };
	        }
	        impl_->inputBuses[busIndex].channelBuffers32 = impl_->inputChannelPointers[busIndex].data();
	    }
	    for (size_t busIndex = 0; busIndex < impl_->outputBuses.size(); ++busIndex) {
	        const int channelCount = busIndex == 0 ? impl_->prepared.outputChannelCount : 2;
	        impl_->outputBuses[busIndex].numChannels = channelCount;
	        impl_->outputBuses[busIndex].silenceFlags = 0;
	        if (busIndex == 0) {
	            impl_->outputChannelPointers[busIndex] = {impl_->outputLeft.data(), impl_->outputRight.data()};
	        } else {
	            const size_t auxIndex = busIndex - 1u;
	            impl_->outputChannelPointers[busIndex] = {
	                auxIndex < impl_->extraOutputBusLeft.size() ? impl_->extraOutputBusLeft[auxIndex].data() : impl_->outputLeft.data(),
	                auxIndex < impl_->extraOutputBusRight.size() ? impl_->extraOutputBusRight[auxIndex].data() : impl_->outputRight.data()
	            };
	        }
	        impl_->outputBuses[busIndex].channelBuffers32 = impl_->outputChannelPointers[busIndex].data();
	    }

    Steinberg::Vst::ProcessData processData {};
    processData.processMode = Steinberg::Vst::kRealtime;
    processData.symbolicSampleSize = Steinberg::Vst::kSample32;
    processData.numSamples = frameCount;
	    processData.numInputs = static_cast<Steinberg::int32>(impl_->inputBuses.size());
	    processData.numOutputs = static_cast<Steinberg::int32>(impl_->outputBuses.size());
	    processData.inputs = impl_->inputBuses.empty() ? nullptr : impl_->inputBuses.data();
	    processData.outputs = impl_->outputBuses.empty() ? nullptr : impl_->outputBuses.data();
    SimpleParameterChanges parameterChanges;
    if (!parameters.empty()) {
        parameterChanges.addValues(parameters);
        processData.inputParameterChanges = &parameterChanges;
    }
    SimpleParameterChanges outputParameterChanges;
    processData.outputParameterChanges = &outputParameterChanges;

    if (impl_->prepared.processor->process(processData) != Steinberg::kResultOk) {
        result.message = "VST3 realtime process() failed.";
        message = result.message;
        return result;
    }
    auto outputValues = outputParameterChanges.latestValues();
    if (!outputValues.empty()) {
        impl_->outputParameterChanges.insert(impl_->outputParameterChanges.end(),
                                             outputValues.begin(),
                                             outputValues.end());
    }

	    for (int frame = 0; frame < frameCount; ++frame) {
	        const auto index = static_cast<size_t>(frame * 2);
	        if (impl_->prepared.outputBusCount <= 0 || impl_->prepared.outputChannelCount <= 0) {
	            interleavedStereo[index] = impl_->inputLeft[static_cast<size_t>(frame)];
	            interleavedStereo[index + 1] = impl_->prepared.inputChannelCount == 1
	                ? impl_->inputLeft[static_cast<size_t>(frame)]
	                : impl_->inputRight[static_cast<size_t>(frame)];
	        } else {
	            interleavedStereo[index] = impl_->outputLeft[static_cast<size_t>(frame)];
	            interleavedStereo[index + 1] = impl_->prepared.outputChannelCount == 1
	                ? impl_->outputLeft[static_cast<size_t>(frame)]
	                : impl_->outputRight[static_cast<size_t>(frame)];
	        }
	    }

    result.opened = true;
    result.processed = true;
    result.framesProcessed = frameCount;
    result.latencySamples = impl_->prepared.latencySamples;
    result.tailSamples = impl_->prepared.tailSamples;
    result.className = impl_->prepared.className;
    result.message = "VST3 realtime block processed.";
    message = result.message;
    impl_->processedSamples += frameCount;
    return result;
#endif
}

std::vector<Vst3ParameterValueState> Vst3RealtimeProcessor::drainOutputParameterChanges() {
    if (impl_ && impl_->bridge) {
        std::vector<Vst3ParameterValueState> changes;
        changes.swap(impl_->bridgeOutputParameters);
        return changes;
    }
#if !defined(NEURACOUST_HAS_VST3_SDK)
    return {};
#else
    std::vector<Vst3ParameterValueState> changes;
    changes.swap(impl_->outputParameterChanges);
    return changes;
#endif
}

Vst3ProcessResult Vst3RealtimeProcessor::processMidiInstrument(float* interleavedStereo,
                                                               int frameCount,
                                                               const std::vector<Vst3MidiEvent>& midiEvents,
                                                               const std::vector<Vst3ParameterValueState>& parameters,
                                                               std::string& message) {
    Vst3ProcessResult result;
    message.clear();
    if (impl_ && impl_->bridge) {
        // Instrument (MIDI) hosting is not yet routed through the sandbox bridge.
        result.sdkAvailable = true;
        result.message = "Out-of-process VST3 instrument hosting is not supported.";
        message = result.message;
        return result;
    }
#if !defined(NEURACOUST_HAS_VST3_SDK)
    result.message = "Steinberg VST3 SDK headers are not configured.";
    message = result.message;
    return result;
#else
    result.sdkAvailable = true;
    if (!isPrepared()) {
        result.message = "VST3 realtime instrument processor is not prepared.";
        message = result.message;
        return result;
    }
    if (interleavedStereo == nullptr || frameCount <= 0 || frameCount > impl_->maxBlockSize) {
        result.message = "VST3 realtime instrument block is invalid.";
        message = result.message;
        return result;
    }
    if (impl_->prepared.outputBusCount <= 0 || impl_->prepared.outputChannelCount <= 0) {
        result.message = "VST3 realtime instrument has no active audio output bus.";
        message = result.message;
        return result;
    }

    std::fill(impl_->outputLeft.begin(), impl_->outputLeft.end(), 0.0f);
    std::fill(impl_->outputRight.begin(), impl_->outputRight.end(), 0.0f);

    Steinberg::Vst::AudioBusBuffers outputBus;
    outputBus.numChannels = impl_->prepared.outputChannelCount;
    outputBus.silenceFlags = 0;
    Steinberg::Vst::Sample32* outputChannels[2] = {impl_->outputLeft.data(), impl_->outputRight.data()};
    outputBus.channelBuffers32 = outputChannels;

    SimpleEventList inputEvents;
    inputEvents.addMidiEvents(midiEvents, 0, frameCount);

    Steinberg::Vst::ProcessData processData {};
    Steinberg::Vst::ProcessContext processContext {};
    processContext.state =
        Steinberg::Vst::ProcessContext::kPlaying |
        Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
        Steinberg::Vst::ProcessContext::kTempoValid |
        Steinberg::Vst::ProcessContext::kTimeSigValid |
        Steinberg::Vst::ProcessContext::kContTimeValid;
    processContext.sampleRate = impl_->sampleRate;
    processContext.projectTimeSamples = impl_->processedSamples;
    processContext.continousTimeSamples = impl_->processedSamples;
    processContext.projectTimeMusic = impl_->sampleRate > 0.0
        ? (static_cast<double>(impl_->processedSamples) / impl_->sampleRate) * (120.0 / 60.0)
        : 0.0;
    processContext.tempo = 120.0;
    processContext.timeSigNumerator = 4;
    processContext.timeSigDenominator = 4;
    processData.processMode = Steinberg::Vst::kRealtime;
    processData.symbolicSampleSize = Steinberg::Vst::kSample32;
    processData.numSamples = frameCount;
    processData.processContext = &processContext;
    processData.numInputs = 0;
    processData.numOutputs = 1;
    processData.inputs = nullptr;
    processData.outputs = &outputBus;
    processData.inputEvents = inputEvents.getEventCount() > 0 ? &inputEvents : nullptr;
    SimpleParameterChanges parameterChanges;
    if (!parameters.empty()) {
        parameterChanges.addValues(parameters);
        processData.inputParameterChanges = &parameterChanges;
    }

    if (impl_->prepared.processor->process(processData) != Steinberg::kResultOk) {
        result.message = "VST3 realtime instrument process() failed.";
        message = result.message;
        return result;
    }

    for (int frame = 0; frame < frameCount; ++frame) {
        const auto index = static_cast<size_t>(frame * 2);
        interleavedStereo[index] = impl_->outputLeft[static_cast<size_t>(frame)];
        interleavedStereo[index + 1] = impl_->prepared.outputChannelCount == 1
            ? impl_->outputLeft[static_cast<size_t>(frame)]
            : impl_->outputRight[static_cast<size_t>(frame)];
    }

    result.opened = true;
    result.processed = true;
    result.framesProcessed = frameCount;
    result.latencySamples = impl_->prepared.latencySamples;
    result.tailSamples = impl_->prepared.tailSamples;
    result.className = impl_->prepared.className;
    result.message = "VST3 realtime instrument block processed.";
    message = result.message;
    return result;
#endif
}

} // namespace neuracoust::daw
