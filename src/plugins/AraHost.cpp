#include "plugins/AraHost.h"

#include "core/Base64.h"

#include "audio/WavFile.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <set>
#include <vector>

#if defined(NEURACOUST_HAS_ARA_SDK) && defined(NEURACOUST_HAS_VST3_SDK)
#include "ARA_API/ARAInterface.h"
#include "ARA_API/ARAVST3.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#endif

#if defined(NEURACOUST_HAS_ARA_SDK) && defined(NEURACOUST_HAS_VST3_SDK)
// VST3 convention: DECLARE_CLASS_IID in the header only declares the iid; exactly one translation
// unit must define it. This is that unit for the ARA VST3 binding interfaces.
DEF_CLASS_IID(ARA::IMainFactory)
DEF_CLASS_IID(ARA::IPlugInEntryPoint)
DEF_CLASS_IID(ARA::IPlugInEntryPoint2)
#endif

namespace neuracoust::daw {

#if !defined(NEURACOUST_HAS_ARA_SDK) || !defined(NEURACOUST_HAS_VST3_SDK)

bool araHostingCompiledIn() { return false; }

AraFactoryInfo inspectAraFactory(const Vst3PluginDescriptor&) {
    AraFactoryInfo info;
    info.message = "이 빌드에는 ARA SDK가 없습니다.";
    return info;
}

struct AraDocumentController::Impl {};
AraDocumentController::AraDocumentController() = default;
bool AraDocumentController::bindPlugInInstance(std::string& message) {
    message = "이 빌드에는 ARA SDK가 없습니다.";
    return false;
}
bool AraDocumentController::createEditorView(void*, int&, int&, std::string& message) {
    message = "이 빌드에는 ARA SDK가 없습니다.";
    return false;
}
bool AraDocumentController::hasEditorView() const { return false; }
bool AraDocumentController::renderToWavFile(const std::string&, std::string& message) {
    message = "이 빌드에는 ARA SDK가 없습니다.";
    return false;
}
bool AraDocumentController::storeArchive(std::string&, std::string& message) {
    message = "이 빌드에는 ARA SDK가 없습니다.";
    return false;
}
bool AraDocumentController::restoreArchive(const std::string&, std::string& message) {
    message = "이 빌드에는 ARA SDK가 없습니다.";
    return false;
}
void AraDocumentController::destroyEditorView() {}
bool AraDocumentController::hasBoundInstance() const { return false; }
bool AraDocumentController::boundPlaybackRenderer() const { return false; }
bool AraDocumentController::boundEditorRenderer() const { return false; }
bool AraDocumentController::boundEditorView() const { return false; }
AraDocumentController::~AraDocumentController() = default;
bool AraDocumentController::create(const Vst3PluginDescriptor&, const std::string&, std::string& message) {
    message = "이 빌드에는 ARA SDK가 없습니다.";
    return false;
}
bool AraDocumentController::addAudioFile(const std::string&, const std::string&, const std::string&,
                                         std::string& message) {
    message = "이 빌드에는 ARA SDK가 없습니다.";
    return false;
}
bool AraDocumentController::isValid() const { return false; }
size_t AraDocumentController::audioSourceCount() const { return 0; }
void AraDocumentController::destroy() {}

#else

namespace {

/// The ARA API generation this host targets. 2.3 Final is what the vendored SDK declares; a plug-in
/// is hostable only if its supported range covers a generation we can also speak.
constexpr ARA::ARAAPIGeneration kHostApiGeneration = ARA::kARAAPIGeneration_2_3_Final;
constexpr ARA::ARAAPIGeneration kHostMinApiGeneration = ARA::kARAAPIGeneration_2_0_Final;

std::string textOrEmpty(const char* text) { return text != nullptr ? std::string(text) : std::string(); }

/// A minimal IHostApplication, enough for IComponent::initialize(). ARA plug-ins ask the host for
/// very little at this stage; anything they genuinely need arrives through the ARA interfaces.
class MinimalAraHostApplication final : public Steinberg::Vst::IHostApplication {
public:
    MinimalAraHostApplication() { FUNKNOWN_CTOR }
    ~MinimalAraHostApplication() noexcept { FUNKNOWN_DTOR }

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
        static const char16_t kName[] = u"Neuracoust DAW";
        std::memcpy(name, kName, sizeof(kName));
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID, void** obj) override {
        if (obj != nullptr) *obj = nullptr;
        return Steinberg::kNotImplemented;
    }

private:
    Steinberg::int32 __funknownRefCount = 1;
};

/// Opens the plug-in's VST3 factory. The module stays loaded for the process lifetime, so repeated
/// calls simply return the same factory.
Steinberg::IPluginFactory* openVst3Factory(const Vst3PluginDescriptor& descriptor, std::string& message) {
    message.clear();
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        message = "플러그인 바이너리를 찾을 수 없습니다.";
        return nullptr;
    }
    void* module = dlopen(descriptor.executablePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (module == nullptr) {
        const char* error = dlerror();
        message = error != nullptr ? error : "플러그인을 열 수 없습니다.";
        return nullptr;
    }
    using BundleEntryFn = bool (*)(void*);
    if (auto* bundleEntry = reinterpret_cast<BundleEntryFn>(dlsym(module, "bundleEntry"))) {
        bundleEntry(nullptr);
    }
    using GetFactoryFn = Steinberg::IPluginFactory* (*)();
    auto* symbol = reinterpret_cast<GetFactoryFn>(dlsym(module, "GetPluginFactory"));
    if (symbol == nullptr) {
        message = "VST3 팩토리 심볼이 없습니다.";
        return nullptr;
    }
    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        message = "VST3 팩토리가 null입니다.";
    }
    return factory;
}

/// ARA's assert hook. The plug-in calls this instead of aborting, so a contract violation on our
/// side surfaces as a log line naming the file and line rather than a bare crash.
void ARA_CALL araAssert(ARA::ARAAssertCategory category, const void* /*problematicArgument*/,
                        const char* diagnosis) {
    fprintf(stderr, "[ARA] assert (category %d): %s\n", static_cast<int>(category),
            diagnosis != nullptr ? diagnosis : "(no diagnosis)");
    fflush(stderr);
}

/// Factories already brought up in this process. ARA allows initializeARAWithConfiguration() only
/// once per factory, so a second document controller for the same plug-in must not repeat it.
std::set<const ARA::ARAFactory*>& initializedFactories() {
    static std::set<const ARA::ARAFactory*> factories;
    return factories;
}

bool ensureFactoryInitialized(const ARA::ARAFactory* factory, std::string& message) {
    if (factory == nullptr || factory->initializeARAWithConfiguration == nullptr) {
        message = "ARA 팩토리를 초기화할 수 없습니다.";
        return false;
    }
    if (initializedFactories().count(factory) != 0) {
        return true;
    }
    // Ask for the newest generation both sides support, clamped into the plug-in's range.
    ARA::ARAAPIGeneration desired = kHostApiGeneration;
    if (desired > factory->highestSupportedApiGeneration) {
        desired = factory->highestSupportedApiGeneration;
    }
    if (desired < factory->lowestSupportedApiGeneration) {
        message = "ARA 세대가 호스트와 맞지 않습니다.";
        return false;
    }
    static ARA::ARAAssertFunction assertFunction = &araAssert;
    ARA::ARAInterfaceConfiguration config {};
    config.structSize = ARA::kARAInterfaceConfigurationMinSize;
    config.desiredApiGeneration = desired;
    config.assertFunctionAddress = &assertFunction;
    factory->initializeARAWithConfiguration(&config);
    initializedFactories().insert(factory);
    // ARA also requires exactly one uninitializeARA() before the binary unloads. The module stays
    // resident for the process lifetime (unloading a plug-in with live threads crashes), so process
    // exit is the right and only moment — and registering here guarantees it pairs with the
    // initialize we just did, once per factory.
    static bool exitHookInstalled = false;
    if (!exitHookInstalled) {
        exitHookInstalled = true;
        std::atexit([] {
            for (const ARA::ARAFactory* registered : initializedFactories()) {
                if (registered != nullptr && registered->uninitializeARA != nullptr) {
                    registered->uninitializeARA();
                }
            }
            initializedFactories().clear();
        });
    }
    return true;
}

/// Opens the plug-in and returns its ARAFactory, or null with `message` set.
///
/// The module is deliberately left loaded: unloading a plug-in that has started its own threads is
/// a known crash, and the factory's lifetime is tied to the module anyway.
const ARA::ARAFactory* acquireAraFactory(const Vst3PluginDescriptor& descriptor, std::string& message) {
    message.clear();
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        message = "플러그인 바이너리를 찾을 수 없습니다.";
        return nullptr;
    }
    void* module = dlopen(descriptor.executablePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (module == nullptr) {
        const char* error = dlerror();
        message = error != nullptr ? error : "플러그인을 열 수 없습니다.";
        return nullptr;
    }
    // A VST3 bundle's factory is only valid after bundleEntry(); Melodyne's is a thin loader that
    // does its real work there.
    using BundleEntryFn = bool (*)(void*);
    if (auto* bundleEntry = reinterpret_cast<BundleEntryFn>(dlsym(module, "bundleEntry"))) {
        bundleEntry(nullptr);
    }
    using GetFactoryFn = Steinberg::IPluginFactory* (*)();
    auto* symbol = reinterpret_cast<GetFactoryFn>(dlsym(module, "GetPluginFactory"));
    if (symbol == nullptr) {
        message = "VST3 팩토리 심볼이 없습니다.";
        return nullptr;
    }
    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        message = "VST3 팩토리가 null입니다.";
        return nullptr;
    }
    const int classCount = factory->countClasses();
    for (int index = 0; index < classCount; ++index) {
        Steinberg::PClassInfo classInfo;
        if (factory->getClassInfo(index, &classInfo) != Steinberg::kResultOk) {
            continue;
        }
        if (std::string(classInfo.category) != kARAMainFactoryClass) {
            continue;
        }
        void* object = nullptr;
        if (factory->createInstance(classInfo.cid, ARA::IMainFactory::iid, &object) != Steinberg::kResultOk ||
            object == nullptr) {
            message = "ARA 메인 팩토리를 만들 수 없습니다.";
            return nullptr;
        }
        auto* mainFactory = static_cast<ARA::IMainFactory*>(object);
        const ARA::ARAFactory* araFactory = nullptr;
        try {
            araFactory = mainFactory->getFactory();
        } catch (...) {
            araFactory = nullptr;
        }
        mainFactory->release();
        if (araFactory == nullptr) {
            message = "ARA 팩토리가 null입니다.";
        }
        return araFactory;
    }
    message = "ARA 플러그인이 아닙니다.";
    return nullptr;
}

} // namespace

bool araHostingCompiledIn() { return true; }

AraFactoryInfo inspectAraFactory(const Vst3PluginDescriptor& descriptor) {
    AraFactoryInfo info;

    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        info.message = "플러그인 바이너리를 찾을 수 없습니다.";
        return info;
    }

    // Opened here rather than through Vst3SdkAdapter's helpers, which are private to that file.
    void* module = dlopen(descriptor.executablePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (module == nullptr) {
        const char* error = dlerror();
        info.message = error != nullptr ? error : "플러그인을 열 수 없습니다.";
        return info;
    }
    // A VST3 bundle's factory is only valid after bundleEntry(); Melodyne's is a thin loader that
    // does its real work there.
    using BundleEntryFn = bool (*)(void*);
    if (auto* bundleEntry = reinterpret_cast<BundleEntryFn>(dlsym(module, "bundleEntry"))) {
        bundleEntry(nullptr);
    }
    using GetFactoryFn = Steinberg::IPluginFactory* (*)();
    auto* symbol = reinterpret_cast<GetFactoryFn>(dlsym(module, "GetPluginFactory"));
    if (symbol == nullptr) {
        dlclose(module);
        info.message = "VST3 팩토리 심볼이 없습니다.";
        return info;
    }
    Steinberg::IPluginFactory* factory = symbol();
    if (factory == nullptr) {
        dlclose(module);
        info.message = "VST3 팩토리가 null입니다.";
        return info;
    }

    // An ARA plug-in advertises a separate factory class whose category is kARAMainFactoryClass.
    // Instantiate THAT class (not the audio component) and ask it for the ARAFactory.
    const int classCount = factory->countClasses();
    for (int index = 0; index < classCount; ++index) {
        Steinberg::PClassInfo classInfo;
        if (factory->getClassInfo(index, &classInfo) != Steinberg::kResultOk) {
            continue;
        }
        if (std::string(classInfo.category) != kARAMainFactoryClass) {
            continue;
        }
        void* object = nullptr;
        if (factory->createInstance(classInfo.cid, ARA::IMainFactory::iid, &object) != Steinberg::kResultOk ||
            object == nullptr) {
            info.message = "ARA 메인 팩토리를 만들 수 없습니다.";
            break;
        }
        auto* mainFactory = static_cast<ARA::IMainFactory*>(object);
        const ARA::ARAFactory* araFactory = nullptr;
        try {
            araFactory = mainFactory->getFactory();
        } catch (...) {
            araFactory = nullptr;
        }
        if (araFactory == nullptr) {
            mainFactory->release();
            info.message = "ARA 팩토리가 null입니다.";
            break;
        }

        info.available = true;
        info.factoryId = textOrEmpty(araFactory->factoryID);
        info.plugInName = textOrEmpty(araFactory->plugInName);
        info.manufacturerName = textOrEmpty(araFactory->manufacturerName);
        info.versionString = textOrEmpty(araFactory->version);
        info.documentArchiveId = textOrEmpty(araFactory->documentArchiveID);
        info.lowestSupportedApiGeneration = static_cast<int>(araFactory->lowestSupportedApiGeneration);
        info.highestSupportedApiGeneration = static_cast<int>(araFactory->highestSupportedApiGeneration);
        // Hostable only where the plug-in's range and ours overlap. Reported rather than assumed:
        // an out-of-range plug-in must be refused, not driven through an API neither side agrees on.
        info.compatibleWithHost =
            araFactory->lowestSupportedApiGeneration <= kHostApiGeneration &&
            araFactory->highestSupportedApiGeneration >= kHostMinApiGeneration;
        info.message = info.compatibleWithHost
            ? "ARA 팩토리를 읽었습니다."
            : "ARA 세대가 호스트와 맞지 않습니다.";

        mainFactory->release();
        break;
    }

    if (!info.available && info.message.empty()) {
        info.message = "ARA 플러그인이 아닙니다.";
    }
    // Deliberately NOT dlclose()d: unloading a plug-in that has spun up its own threads is a known
    // way to crash, and this module stays useful for the hosting that follows.
    return info;
}

// ---------------------------------------------------------------------------
// Host-side callbacks. The plug-in calls INTO these; every one of them must survive being called
// from whatever thread the plug-in chooses, so they only touch state owned by the reader they were
// handed. Audio access and archiving are mandatory; the rest we leave null (the plug-in checks).
// ---------------------------------------------------------------------------

namespace {

/// One open reader over a decoded WAV. ARA hands this back to us as an opaque host ref, so the
/// plug-in can pull any sample range at any time — the "random access" the API is named for.
struct AraAudioReader {
    const WavAudioData* audio = nullptr;
};

/// An audio source the host registered, with its decoded samples kept resident for the plug-in.
struct AraAudioSourceEntry {
    std::string persistentId;
    std::string name;
    WavAudioData audio;
    ARA::ARAAudioSourceRef sourceRef = nullptr;
    ARA::ARAAudioModificationRef modificationRef = nullptr;
    ARA::ARAPlaybackRegionRef playbackRegionRef = nullptr;
    double playbackStartSeconds = 0.0;
    double playbackDurationSeconds = 0.0;
};

ARA::ARAAudioReaderHostRef ARA_CALL araCreateAudioReaderForSource(
    ARA::ARAAudioAccessControllerHostRef /*controllerHostRef*/,
    ARA::ARAAudioSourceHostRef audioSourceHostRef, ARA::ARABool /*use64BitSamples*/) {
    auto* entry = reinterpret_cast<AraAudioSourceEntry*>(audioSourceHostRef);
    if (entry == nullptr) {
        return nullptr;
    }
    auto* reader = new AraAudioReader{&entry->audio};
    return reinterpret_cast<ARA::ARAAudioReaderHostRef>(reader);
}

/// Deinterleave the requested range into the plug-in's per-channel buffers. Out-of-range reads are
/// answered with silence rather than refused: the plug-in is allowed to ask past the end while it
/// windows, and failing the whole call would abort its analysis.
ARA::ARABool ARA_CALL araReadAudioSamples(ARA::ARAAudioAccessControllerHostRef /*controllerHostRef*/,
                                          ARA::ARAAudioReaderHostRef audioReaderHostRef,
                                          ARA::ARASamplePosition samplePosition,
                                          ARA::ARASampleCount samplesPerChannel,
                                          void* const buffers[]) {
    auto* reader = reinterpret_cast<AraAudioReader*>(audioReaderHostRef);
    if (reader == nullptr || reader->audio == nullptr || buffers == nullptr || samplesPerChannel < 0) {
        return ARA::kARAFalse;
    }
    const WavAudioData& audio = *reader->audio;
    const int channels = std::max(1, audio.channels);
    const int64_t frames = static_cast<int64_t>(audio.frameCount());
    for (int channel = 0; channel < channels; ++channel) {
        auto* out = static_cast<float*>(buffers[channel]);
        if (out == nullptr) {
            continue;
        }
        for (ARA::ARASampleCount i = 0; i < samplesPerChannel; ++i) {
            const int64_t frame = samplePosition + static_cast<int64_t>(i);
            const size_t index = static_cast<size_t>(frame) * static_cast<size_t>(channels) +
                                 static_cast<size_t>(channel);
            out[i] = (frame >= 0 && frame < frames && index < audio.interleavedSamples.size())
                ? audio.interleavedSamples[index] : 0.0f;
        }
    }
    return ARA::kARATrue;
}

void ARA_CALL araDestroyAudioReader(ARA::ARAAudioAccessControllerHostRef /*controllerHostRef*/,
                                    ARA::ARAAudioReaderHostRef audioReaderHostRef) {
    delete reinterpret_cast<AraAudioReader*>(audioReaderHostRef);
}

// Archiving — how a plug-in's edits survive a project save.
//
// The archive is opaque: the plug-in writes its own bytes through these callbacks and the host only
// stores them. So the host side is a plain growable buffer, passed to ARA as the reader/writer host
// ref. Random access is real (the plug-in seeks), hence the position argument.
struct AraArchiveBuffer {
    std::vector<ARA::ARAByte> bytes;
    /// The format tag the plug-in reported when this archive was written. Handed back on restore so
    /// the plug-in can recognise an archive from an older version of itself.
    std::string documentArchiveId;
    bool failed = false;
};

ARA::ARASize ARA_CALL araGetArchiveSize(ARA::ARAArchivingControllerHostRef,
                                        ARA::ARAArchiveReaderHostRef archiveReaderHostRef) {
    const auto* archive = reinterpret_cast<const AraArchiveBuffer*>(archiveReaderHostRef);
    return archive != nullptr ? static_cast<ARA::ARASize>(archive->bytes.size()) : 0;
}

ARA::ARABool ARA_CALL araReadBytesFromArchive(ARA::ARAArchivingControllerHostRef,
                                              ARA::ARAArchiveReaderHostRef archiveReaderHostRef,
                                              ARA::ARASize position, ARA::ARASize length,
                                              ARA::ARAByte buffer[]) {
    auto* archive = reinterpret_cast<AraArchiveBuffer*>(archiveReaderHostRef);
    if (archive == nullptr || buffer == nullptr) {
        return ARA::kARAFalse;
    }
    if (length == 0) {
        return ARA::kARATrue;
    }
    // A read past the end is a corrupt or truncated archive, not something to paper over with zeros:
    // report the failure so the plug-in can tell the user which objects it lost.
    if (position > archive->bytes.size() || length > archive->bytes.size() - position) {
        std::memset(buffer, 0, length);
        archive->failed = true;
        return ARA::kARAFalse;
    }
    std::memcpy(buffer, archive->bytes.data() + position, length);
    return ARA::kARATrue;
}

ARA::ARABool ARA_CALL araWriteBytesToArchive(ARA::ARAArchivingControllerHostRef,
                                             ARA::ARAArchiveWriterHostRef archiveWriterHostRef,
                                             ARA::ARASize position, ARA::ARASize length,
                                             const ARA::ARAByte buffer[]) {
    auto* archive = reinterpret_cast<AraArchiveBuffer*>(archiveWriterHostRef);
    if (archive == nullptr || (buffer == nullptr && length > 0)) {
        return ARA::kARAFalse;
    }
    if (length == 0) {
        return ARA::kARATrue;
    }
    if (position + length > archive->bytes.size()) {
        archive->bytes.resize(position + length, 0);
    }
    std::memcpy(archive->bytes.data() + position, buffer, length);
    return ARA::kARATrue;
}

void ARA_CALL araNotifyDocumentArchivingProgress(ARA::ARAArchivingControllerHostRef, float) {}
void ARA_CALL araNotifyDocumentUnarchivingProgress(ARA::ARAArchivingControllerHostRef, float) {}

ARA::ARAPersistentID ARA_CALL araGetDocumentArchiveID(ARA::ARAArchivingControllerHostRef,
                                                      ARA::ARAArchiveReaderHostRef archiveReaderHostRef) {
    const auto* archive = reinterpret_cast<const AraArchiveBuffer*>(archiveReaderHostRef);
    // NULL means "written by the plug-in version that is reading it" — the right answer for an
    // archive we have no recorded ID for, and the only honest one.
    if (archive == nullptr || archive->documentArchiveId.empty()) {
        return nullptr;
    }
    return archive->documentArchiveId.c_str();
}

} // namespace

struct AraDocumentController::Impl {
    Vst3PluginDescriptor descriptor;
    const ARA::ARAFactory* factory = nullptr;
    /// The VST3 audio component put into ARA mode, and what it handed back. Kept inactive here:
    /// binding must happen before setActive(), so activation belongs to whoever renders it.
    Steinberg::Vst::IComponent* boundComponent = nullptr;
    const ARA::ARAPlugInExtensionInstance* plugInExtension = nullptr;
    Steinberg::Vst::IEditController* editController = nullptr;
    bool editControllerInitialized = false;
    Steinberg::IPlugView* editorView = nullptr;
    bool editorViewAttached = false;
    bool rendering = false;
    const ARA::ARADocumentControllerInstance* instance = nullptr;
    ARA::ARADocumentControllerRef controllerRef = nullptr;
    ARA::ARAMusicalContextRef musicalContextRef = nullptr;
    ARA::ARARegionSequenceRef regionSequenceRef = nullptr;
    std::vector<std::unique_ptr<AraAudioSourceEntry>> sources;

    ARA::ARAAudioAccessControllerInterface audioAccess {};
    ARA::ARAArchivingControllerInterface archiving {};
    ARA::ARADocumentControllerHostInstance hostInstance {};

    const ARA::ARADocumentControllerInterface* iface() const {
        return instance != nullptr ? instance->documentControllerInterface : nullptr;
    }
};

AraDocumentController::AraDocumentController() : impl_(std::make_unique<Impl>()) {}
AraDocumentController::~AraDocumentController() { destroy(); }

bool AraDocumentController::isValid() const {
    return impl_ && impl_->controllerRef != nullptr && impl_->iface() != nullptr;
}

size_t AraDocumentController::audioSourceCount() const {
    return impl_ ? impl_->sources.size() : 0;
}

bool AraDocumentController::create(const Vst3PluginDescriptor& descriptor,
                                   const std::string& documentName,
                                   std::string& message) {
    message.clear();
    destroy();

    const ARA::ARAFactory* factory = acquireAraFactory(descriptor, message);
    if (factory == nullptr) {
        return false;
    }
    // ARA REQUIRES the factory to be initialised before any document controller is created, exactly
    // once per factory for the life of the process (and uninitialised exactly once at the end).
    // Skipping it is not a soft error: Melodyne segfaults inside
    // createDocumentControllerWithDocument, because its ARA subsystem was never brought up.
    if (!ensureFactoryInitialized(factory, message)) {
        return false;
    }
    impl_->factory = factory;
    impl_->descriptor = descriptor;

    // Fill in the host interfaces the plug-in will call back into. structSize is how ARA versions
    // these — it must describe what we actually implement, not the full struct.
    impl_->audioAccess.structSize = ARA::kARAAudioAccessControllerInterfaceMinSize;
    impl_->audioAccess.createAudioReaderForSource = &araCreateAudioReaderForSource;
    impl_->audioAccess.readAudioSamples = &araReadAudioSamples;
    impl_->audioAccess.destroyAudioReader = &araDestroyAudioReader;

    // structSize must describe what we ACTUALLY implement, not the 1.0 minimum: we provide
    // getDocumentArchiveID (an ARA 2.0 Final addendum), and Melodyne asserts if the declared size
    // is smaller than the fields it finds us claiming to support.
    impl_->archiving.structSize =
        ARA_IMPLEMENTED_STRUCT_SIZE(ARAArchivingControllerInterface, getDocumentArchiveID);
    impl_->archiving.getArchiveSize = &araGetArchiveSize;
    impl_->archiving.readBytesFromArchive = &araReadBytesFromArchive;
    impl_->archiving.writeBytesToArchive = &araWriteBytesToArchive;
    impl_->archiving.notifyDocumentArchivingProgress = &araNotifyDocumentArchivingProgress;
    impl_->archiving.notifyDocumentUnarchivingProgress = &araNotifyDocumentUnarchivingProgress;
    impl_->archiving.getDocumentArchiveID = &araGetDocumentArchiveID;

    impl_->hostInstance.structSize = ARA::kARADocumentControllerHostInstanceMinSize;
    impl_->hostInstance.audioAccessControllerHostRef = nullptr;
    impl_->hostInstance.audioAccessControllerInterface = &impl_->audioAccess;
    impl_->hostInstance.archivingControllerHostRef = nullptr;
    impl_->hostInstance.archivingControllerInterface = &impl_->archiving;
    // Optional interfaces left null on purpose: the plug-in is required to check before calling,
    // and a half-implemented content/playback controller would misreport the session.
    impl_->hostInstance.contentAccessControllerInterface = nullptr;
    impl_->hostInstance.modelUpdateControllerInterface = nullptr;
    impl_->hostInstance.playbackControllerInterface = nullptr;

    ARA::ARADocumentProperties documentProperties {};
    documentProperties.structSize = ARA::kARADocumentPropertiesMinSize;
    documentProperties.name = documentName.c_str();

    impl_->instance = factory->createDocumentControllerWithDocument(&impl_->hostInstance, &documentProperties);
    if (impl_->instance == nullptr || impl_->instance->documentControllerInterface == nullptr) {
        message = "플러그인이 문서 컨트롤러를 만들지 못했습니다.";
        impl_->instance = nullptr;
        return false;
    }
    impl_->controllerRef = impl_->instance->documentControllerRef;

    // A playback region needs a region sequence, which needs a musical context. Create both once.
    const auto* iface = impl_->iface();
    iface->beginEditing(impl_->controllerRef);

    ARA::ARAMusicalContextProperties musicalContextProperties {};
    musicalContextProperties.structSize = ARA::kARAMusicalContextPropertiesMinSize;
    impl_->musicalContextRef = iface->createMusicalContext(impl_->controllerRef, nullptr,
                                                           &musicalContextProperties);

    ARA::ARARegionSequenceProperties regionSequenceProperties {};
    regionSequenceProperties.structSize = ARA::kARARegionSequencePropertiesMinSize;
    regionSequenceProperties.name = documentName.c_str();
    regionSequenceProperties.orderIndex = 0;
    regionSequenceProperties.musicalContextRef = impl_->musicalContextRef;
    impl_->regionSequenceRef = iface->createRegionSequence(impl_->controllerRef, nullptr,
                                                           &regionSequenceProperties);

    iface->endEditing(impl_->controllerRef);
    message = "ARA 문서 컨트롤러를 만들었습니다.";
    return true;
}

bool AraDocumentController::addAudioFile(const std::string& wavPath,
                                         const std::string& persistentId,
                                         const std::string& name,
                                         std::string& message) {
    message.clear();
    if (!isValid()) {
        message = "ARA 문서 컨트롤러가 없습니다.";
        return false;
    }

    auto entry = std::make_unique<AraAudioSourceEntry>();
    entry->persistentId = persistentId;
    entry->name = name;
    std::string readError;
    if (!readPcmWavFile(wavPath, entry->audio, readError)) {
        message = readError.empty() ? "오디오 파일을 읽을 수 없습니다." : readError;
        return false;
    }
    if (entry->audio.frameCount() <= 0) {
        message = "오디오가 비어 있습니다.";
        return false;
    }

    const auto* iface = impl_->iface();
    AraAudioSourceEntry* raw = entry.get();

    ARA::ARAAudioSourceProperties sourceProperties {};
    sourceProperties.structSize = ARA::kARAAudioSourcePropertiesMinSize;
    sourceProperties.name = raw->name.c_str();
    sourceProperties.persistentID = raw->persistentId.c_str();
    sourceProperties.sampleCount = static_cast<ARA::ARASampleCount>(raw->audio.frameCount());
    sourceProperties.sampleRate = static_cast<ARA::ARASampleRate>(raw->audio.sampleRate);
    sourceProperties.channelCount = static_cast<ARA::ARAChannelCount>(std::max(1, raw->audio.channels));
    sourceProperties.merits64BitSamples = ARA::kARAFalse;

    iface->beginEditing(impl_->controllerRef);
    raw->sourceRef = iface->createAudioSource(
        impl_->controllerRef, reinterpret_cast<ARA::ARAAudioSourceHostRef>(raw), &sourceProperties);
    if (raw->sourceRef == nullptr) {
        iface->endEditing(impl_->controllerRef);
        message = "오디오 소스를 만들지 못했습니다.";
        return false;
    }

    ARA::ARAAudioModificationProperties modificationProperties {};
    modificationProperties.structSize = ARA::kARAAudioModificationPropertiesMinSize;
    modificationProperties.name = raw->name.c_str();
    modificationProperties.persistentID = raw->persistentId.c_str();
    raw->modificationRef = iface->createAudioModification(impl_->controllerRef, raw->sourceRef,
                                                          nullptr, &modificationProperties);

    // One playback region covering the whole file, placed at the timeline origin. Trimming it to a
    // clip's window is step 3's job, once regions are driven from the project.
    const double durationSeconds = static_cast<double>(raw->audio.frameCount()) /
                                   std::max(1.0, static_cast<double>(raw->audio.sampleRate));
    ARA::ARAPlaybackRegionProperties regionProperties {};
    // NOT kARAPlaybackRegionPropertiesMinSize: that constant stops at musicalContextRef, so the
    // regionSequenceRef and name we fill in below would be cut off — and ARA 2 *requires*
    // regionSequenceRef, which is exactly what Melodyne asserts on. Declare through `name`.
    regionProperties.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(ARAPlaybackRegionProperties, name);
    regionProperties.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
    regionProperties.startInModificationTime = 0.0;
    regionProperties.durationInModificationTime = durationSeconds;
    regionProperties.startInPlaybackTime = 0.0;
    regionProperties.durationInPlaybackTime = durationSeconds;
    regionProperties.musicalContextRef = impl_->musicalContextRef;
    regionProperties.regionSequenceRef = impl_->regionSequenceRef;
    regionProperties.name = raw->name.c_str();
    raw->playbackStartSeconds = regionProperties.startInPlaybackTime;
    raw->playbackDurationSeconds = regionProperties.durationInPlaybackTime;
    raw->playbackRegionRef = iface->createPlaybackRegion(impl_->controllerRef, raw->modificationRef,
                                                         nullptr, &regionProperties);
    iface->endEditing(impl_->controllerRef);

    // Only now may the plug-in pull samples: enabling access before the graph exists invites a read
    // against a source the plug-in has not been told about.
    iface->enableAudioSourceSamplesAccess(impl_->controllerRef, raw->sourceRef, ARA::kARATrue);

    impl_->sources.push_back(std::move(entry));
    message = "오디오 소스를 등록했습니다.";
    return true;
}

bool AraDocumentController::bindPlugInInstance(std::string& message) {
    message.clear();
    if (!isValid()) {
        message = "ARA 문서 컨트롤러가 없습니다.";
        return false;
    }
    if (impl_->plugInExtension != nullptr) {
        message = "이미 인스턴스가 바인딩되어 있습니다.";
        return false;
    }

    Steinberg::IPluginFactory* factory = openVst3Factory(impl_->descriptor, message);
    if (factory == nullptr) {
        return false;
    }
    // The AUDIO component (kVstAudioEffectClass) carries the ARA entry point — not the ARA main
    // factory class, which only hands out the ARAFactory.
    Steinberg::Vst::IComponent* component = nullptr;
    const int classCount = factory->countClasses();
    for (int index = 0; index < classCount && component == nullptr; ++index) {
        Steinberg::PClassInfo classInfo;
        if (factory->getClassInfo(index, &classInfo) != Steinberg::kResultOk) {
            continue;
        }
        if (std::string(classInfo.category) != std::string(kVstAudioEffectClass)) {
            continue;
        }
        void* object = nullptr;
        if (factory->createInstance(classInfo.cid, Steinberg::Vst::IComponent::iid, &object) == Steinberg::kResultOk) {
            component = static_cast<Steinberg::Vst::IComponent*>(object);
        }
    }
    if (component == nullptr) {
        message = "오디오 컴포넌트를 만들 수 없습니다.";
        return false;
    }

    // initialize() before binding, but NOT setActive(): ARA requires the bind to precede activation,
    // setState and any GUI creation, and permits it only once per instance.
    auto* host = new MinimalAraHostApplication();
    if (component->initialize(host) != Steinberg::kResultOk) {
        host->release();
        component->release();
        message = "오디오 컴포넌트를 초기화하지 못했습니다.";
        return false;
    }
    host->release();

    void* entryObject = nullptr;
    if (component->queryInterface(ARA::IPlugInEntryPoint2::iid, &entryObject) != Steinberg::kResultOk ||
        entryObject == nullptr) {
        component->terminate();
        component->release();
        message = "플러그인이 ARA 진입점(IPlugInEntryPoint2)을 제공하지 않습니다.";
        return false;
    }
    auto* entryPoint = static_cast<ARA::IPlugInEntryPoint2*>(entryObject);

    // Claim all three roles: render playback, render while editing, and show the plug-in's editor.
    // `knownRoles` states which roles this host understands at all, which is how ARA distinguishes
    // "not asked for" from "not supported".
    const ARA::ARAPlugInInstanceRoleFlags roles = static_cast<ARA::ARAPlugInInstanceRoleFlags>(
        ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole);
    const ARA::ARAPlugInExtensionInstance* extension = nullptr;
    try {
        extension = entryPoint->bindToDocumentControllerWithRoles(impl_->controllerRef, roles, roles);
    } catch (...) {
        extension = nullptr;
    }
    entryPoint->release();

    if (extension == nullptr) {
        component->terminate();
        component->release();
        message = "문서 컨트롤러에 인스턴스를 바인딩하지 못했습니다.";
        return false;
    }

    impl_->boundComponent = component;
    impl_->plugInExtension = extension;
    message = "인스턴스를 ARA 모드로 바인딩했습니다.";
    return true;
}

bool AraDocumentController::storeArchive(std::string& base64Out, std::string& message) {
    base64Out.clear();
    message.clear();
    if (!isValid()) {
        message = "ARA 문서 컨트롤러가 없습니다.";
        return false;
    }
    const auto* iface = impl_->iface();
    if (iface->storeObjectsToArchive == nullptr) {
        message = "플러그인이 ARA 2 아카이빙을 지원하지 않습니다.";
        return false;
    }

    AraArchiveBuffer archive;
    archive.documentArchiveId = impl_->factory != nullptr && impl_->factory->documentArchiveID != nullptr
                                    ? impl_->factory->documentArchiveID
                                    : std::string();
    // "Archives may only be created from documents that are not being currently edited" — so this
    // deliberately does NOT wrap the call in beginEditing/endEditing.
    const ARA::ARABool ok = iface->storeObjectsToArchive(
        impl_->controllerRef, reinterpret_cast<ARA::ARAArchiveWriterHostRef>(&archive), nullptr);
    if (ok == ARA::kARAFalse) {
        message = "플러그인이 아카이브를 쓰지 못했습니다.";
        return false;
    }

    base64Out = encodeBase64(archive.bytes.data(), archive.bytes.size());
    message = std::to_string(archive.bytes.size()) + " 바이트를 저장했습니다.";
    return true;
}

bool AraDocumentController::restoreArchive(const std::string& base64In, std::string& message) {
    message.clear();
    if (!isValid()) {
        message = "ARA 문서 컨트롤러가 없습니다.";
        return false;
    }
    const auto* iface = impl_->iface();
    if (iface->restoreObjectsFromArchive == nullptr) {
        message = "플러그인이 ARA 2 언아카이빙을 지원하지 않습니다.";
        return false;
    }
    if (base64In.empty()) {
        message = "복원할 아카이브가 없습니다.";
        return false;
    }

    AraArchiveBuffer archive;
    std::vector<uint8_t> decoded;
    if (!decodeBase64(base64In, decoded)) {
        message = "아카이브 base64 디코딩에 실패했습니다.";
        return false;
    }
    archive.bytes.assign(decoded.begin(), decoded.end());
    archive.documentArchiveId = impl_->factory != nullptr && impl_->factory->documentArchiveID != nullptr
                                    ? impl_->factory->documentArchiveID
                                    : std::string();

    // Restoring IS an editing session, and the graph must already carry objects whose persistentIDs
    // match the archive — which is why addAudioFile() takes the clip id as the persistent id.
    iface->beginEditing(impl_->controllerRef);
    const ARA::ARABool ok = iface->restoreObjectsFromArchive(
        impl_->controllerRef, reinterpret_cast<ARA::ARAArchiveReaderHostRef>(&archive), nullptr);
    iface->endEditing(impl_->controllerRef);

    if (ok == ARA::kARAFalse || archive.failed) {
        message = "아카이브를 복원하지 못했습니다 (손상되었거나 호환되지 않음).";
        return false;
    }
    message = std::to_string(archive.bytes.size()) + " 바이트를 복원했습니다.";
    return true;
}

bool AraDocumentController::renderToWavFile(const std::string& outPath, std::string& message) {
    message.clear();
    if (!boundPlaybackRenderer() || impl_->boundComponent == nullptr) {
        message = "플레이백 렌더러 역할이 없습니다.";
        return false;
    }
    if (impl_->sources.empty()) {
        message = "렌더링할 오디오 소스가 없습니다.";
        return false;
    }
    if (impl_->rendering) {
        message = "이미 렌더링 중입니다.";
        return false;
    }

    const auto& first = *impl_->sources.front();
    const double sampleRate = std::max(8000.0, static_cast<double>(first.audio.sampleRate));
    const int channels = std::max(1, first.audio.channels);
    // The whole document, not just the first source: regions may sit anywhere on the timeline.
    double endSeconds = 0.0;
    for (const auto& source : impl_->sources) {
        endSeconds = std::max(endSeconds, source->playbackStartSeconds + source->playbackDurationSeconds);
    }
    const int64_t totalFrames = static_cast<int64_t>(std::ceil(endSeconds * sampleRate));
    if (totalFrames <= 0) {
        message = "렌더링 길이가 0입니다.";
        return false;
    }

    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    if (impl_->boundComponent->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                                              reinterpret_cast<void**>(&processor)) != Steinberg::kResultOk ||
            processor == nullptr) {
        message = "플러그인이 오디오 프로세서를 제공하지 않습니다.";
        return false;
    }

    constexpr int kBlockSize = 512;
    Steinberg::Vst::ProcessSetup setup {};
    setup.processMode = Steinberg::Vst::kOffline;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = kBlockSize;
    setup.sampleRate = sampleRate;
    if (processor->setupProcessing(setup) != Steinberg::kResultOk) {
        processor->release();
        message = "setupProcessing 실패.";
        return false;
    }

    // An ARA playback renderer produces audio out of thin air — it pulls its samples through the
    // audio-access callbacks, not from an input bus. Any input bus is therefore left deactivated;
    // feeding one would make Melodyne treat it as a live signal.
    const int inputBusCount = impl_->boundComponent->getBusCount(Steinberg::Vst::kAudio,
                                                                 Steinberg::Vst::kInput);
    for (int bus = 0; bus < inputBusCount; ++bus) {
        impl_->boundComponent->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, bus, false);
    }
    const int outputBusCount = impl_->boundComponent->getBusCount(Steinberg::Vst::kAudio,
                                                                  Steinberg::Vst::kOutput);
    if (outputBusCount <= 0) {
        processor->release();
        message = "플러그인에 출력 버스가 없습니다.";
        return false;
    }
    for (int bus = 0; bus < outputBusCount; ++bus) {
        impl_->boundComponent->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, bus, bus == 0);
    }
    Steinberg::Vst::SpeakerArrangement outputArrangement =
        channels >= 2 ? Steinberg::Vst::SpeakerArr::kStereo : Steinberg::Vst::SpeakerArr::kMono;
    processor->setBusArrangements(nullptr, 0, &outputArrangement, 1);

    // How many channels the plug-in actually settled on — asking for mono does not guarantee mono.
    int outputChannels = channels;
    Steinberg::Vst::BusInfo busInfo {};
    if (impl_->boundComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, busInfo) ==
            Steinberg::kResultOk) {
        outputChannels = std::max(1, static_cast<int>(busInfo.channelCount));
    }

    // Regions are assigned while the instance is INACTIVE. Doing it after setActive(true) still
    // rendered here, but Melodyne asserts on it — ARA treats the region set as part of the
    // configuration a plug-in reads when it activates, not something it may be handed mid-flight.
    const auto* renderer = impl_->plugInExtension->playbackRendererInterface;
    for (const auto& source : impl_->sources) {
        if (source->playbackRegionRef != nullptr) {
            renderer->addPlaybackRegion(impl_->plugInExtension->playbackRendererRef,
                                        source->playbackRegionRef);
        }
    }

    if (impl_->boundComponent->setActive(true) != Steinberg::kResultOk) {
        for (const auto& source : impl_->sources) {
            if (source->playbackRegionRef != nullptr) {
                renderer->removePlaybackRegion(impl_->plugInExtension->playbackRendererRef,
                                               source->playbackRegionRef);
            }
        }
        processor->release();
        message = "setActive 실패.";
        return false;
    }
    impl_->rendering = true;

    processor->setProcessing(true);

    std::vector<std::vector<float>> channelBuffers(static_cast<size_t>(outputChannels),
                                                   std::vector<float>(kBlockSize, 0.0f));
    std::vector<float*> channelPointers(static_cast<size_t>(outputChannels), nullptr);

    WavAudioData out;
    out.channels = outputChannels;
    out.sampleRate = static_cast<int>(std::lround(sampleRate));
    out.bitsPerSample = 16;
    out.interleavedSamples.resize(static_cast<size_t>(totalFrames) * static_cast<size_t>(outputChannels), 0.0f);

    for (int64_t position = 0; position < totalFrames; position += kBlockSize) {
        const int frames = static_cast<int>(std::min<int64_t>(kBlockSize, totalFrames - position));
        for (int channel = 0; channel < outputChannels; ++channel) {
            std::fill(channelBuffers[static_cast<size_t>(channel)].begin(),
                      channelBuffers[static_cast<size_t>(channel)].end(), 0.0f);
            channelPointers[static_cast<size_t>(channel)] = channelBuffers[static_cast<size_t>(channel)].data();
        }

        Steinberg::Vst::AudioBusBuffers outputBuffers {};
        outputBuffers.numChannels = outputChannels;
        outputBuffers.silenceFlags = 0;
        outputBuffers.channelBuffers32 = channelPointers.data();

        // ARA reads the playhead out of the process context — this is how it knows which slice of
        // the region to render. Without kPlaying and a valid projectTimeSamples it emits silence.
        Steinberg::Vst::ProcessContext context {};
        context.state = Steinberg::Vst::ProcessContext::kPlaying |
                        Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
                        Steinberg::Vst::ProcessContext::kTempoValid |
                        Steinberg::Vst::ProcessContext::kTimeSigValid;
        context.sampleRate = sampleRate;
        context.projectTimeSamples = position;
        context.continousTimeSamples = position;
        context.projectTimeMusic = (static_cast<double>(position) / sampleRate) * (120.0 / 60.0);
        context.tempo = 120.0;
        context.timeSigNumerator = 4;
        context.timeSigDenominator = 4;

        Steinberg::Vst::ProcessData data {};
        data.processMode = Steinberg::Vst::kOffline;
        data.symbolicSampleSize = Steinberg::Vst::kSample32;
        data.numSamples = frames;
        data.numInputs = 0;
        data.numOutputs = 1;
        data.inputs = nullptr;
        data.outputs = &outputBuffers;
        data.processContext = &context;

        if (processor->process(data) != Steinberg::kResultOk) {
            break;
        }
        for (int channel = 0; channel < outputChannels; ++channel) {
            const float* source = channelBuffers[static_cast<size_t>(channel)].data();
            for (int frame = 0; frame < frames; ++frame) {
                out.interleavedSamples[static_cast<size_t>((position + frame) * outputChannels + channel)] =
                    source[frame];
            }
        }
    }

    processor->setProcessing(false);
    impl_->boundComponent->setActive(false);
    // Same rule on the way out: deactivate first, then give the regions back.
    for (const auto& source : impl_->sources) {
        if (source->playbackRegionRef != nullptr) {
            renderer->removePlaybackRegion(impl_->plugInExtension->playbackRendererRef,
                                           source->playbackRegionRef);
        }
    }
    impl_->rendering = false;
    processor->release();

    std::string writeError;
    if (!writePcm16WavFile(outPath, out, writeError)) {
        message = writeError.empty() ? "WAV를 쓰지 못했습니다." : writeError;
        return false;
    }

    double peak = 0.0;
    for (float sample : out.interleavedSamples) {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    message = "렌더링 완료 (피크 " + std::to_string(peak) + ", " +
              std::to_string(totalFrames) + " 프레임, " + std::to_string(outputChannels) + "ch)";
    return true;
}

bool AraDocumentController::createEditorView(void* parentNSView, int& widthOut, int& heightOut,
                                             std::string& message) {
    message.clear();
    widthOut = 0;
    heightOut = 0;
    if (!hasBoundInstance() || impl_->boundComponent == nullptr) {
        message = "먼저 인스턴스를 ARA 모드로 바인딩해야 합니다.";
        return false;
    }
    if (impl_->editorView != nullptr) {
        message = "에디터 뷰가 이미 있습니다.";
        return false;
    }

    // The GUI lives on the edit CONTROLLER, which is a separate object from the audio component.
    // Prefer the component's declared controller class; fall back to a single-object plug-in.
    if (impl_->editController == nullptr) {
        Steinberg::TUID controllerCid {};
        if (impl_->boundComponent->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
            std::string factoryMessage;
            if (Steinberg::IPluginFactory* factory = openVst3Factory(impl_->descriptor, factoryMessage)) {
                void* object = nullptr;
                if (factory->createInstance(controllerCid, Steinberg::Vst::IEditController::iid, &object) ==
                        Steinberg::kResultOk && object != nullptr) {
                    impl_->editController = static_cast<Steinberg::Vst::IEditController*>(object);
                    auto* host = new MinimalAraHostApplication();
                    if (impl_->editController->initialize(host) != Steinberg::kResultOk) {
                        impl_->editController->release();
                        impl_->editController = nullptr;
                    } else {
                        impl_->editControllerInitialized = true;
                    }
                    host->release();
                }
            }
        }
        if (impl_->editController == nullptr) {
            void* object = nullptr;
            if (impl_->boundComponent->queryInterface(Steinberg::Vst::IEditController::iid, &object) ==
                    Steinberg::kResultOk && object != nullptr) {
                impl_->editController = static_cast<Steinberg::Vst::IEditController*>(object);
            }
        }
    }
    if (impl_->editController == nullptr) {
        message = "플러그인이 에디트 컨트롤러를 제공하지 않습니다.";
        return false;
    }

    Steinberg::IPlugView* view = nullptr;
    try {
        view = impl_->editController->createView(Steinberg::Vst::ViewType::kEditor);
    } catch (...) {
        view = nullptr;
    }
    if (view == nullptr) {
        message = "플러그인이 에디터 뷰를 만들지 않았습니다.";
        return false;
    }
    if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeNSView) != Steinberg::kResultTrue) {
        view->release();
        message = "플러그인 에디터가 NSView를 지원하지 않습니다.";
        return false;
    }

    Steinberg::ViewRect size {};
    if (view->getSize(&size) == Steinberg::kResultOk) {
        widthOut = static_cast<int>(size.getWidth());
        heightOut = static_cast<int>(size.getHeight());
    }

    // Attaching is optional: creating the view already proves the plug-in offers its ARA editor,
    // and a probe with no window should not be forced to invent one.
    if (parentNSView != nullptr) {
        if (view->attached(parentNSView, Steinberg::kPlatformTypeNSView) != Steinberg::kResultOk) {
            view->release();
            message = "에디터 뷰를 창에 붙이지 못했습니다.";
            return false;
        }
        impl_->editorViewAttached = true;
    }

    impl_->editorView = view;
    message = "에디터 뷰를 만들었습니다.";
    return true;
}

bool AraDocumentController::hasEditorView() const {
    return impl_ && impl_->editorView != nullptr;
}

void AraDocumentController::destroyEditorView() {
    if (!impl_ || impl_->editorView == nullptr) {
        return;
    }
    if (impl_->editorViewAttached) {
        impl_->editorView->removed();
        impl_->editorViewAttached = false;
    }
    impl_->editorView->release();
    impl_->editorView = nullptr;
}

bool AraDocumentController::hasBoundInstance() const {
    return impl_ && impl_->plugInExtension != nullptr;
}
bool AraDocumentController::boundPlaybackRenderer() const {
    return hasBoundInstance() && impl_->plugInExtension->playbackRendererInterface != nullptr;
}
bool AraDocumentController::boundEditorRenderer() const {
    return hasBoundInstance() && impl_->plugInExtension->editorRendererInterface != nullptr;
}
bool AraDocumentController::boundEditorView() const {
    return hasBoundInstance() && impl_->plugInExtension->editorViewInterface != nullptr;
}

void AraDocumentController::destroy() {
    if (!impl_ || impl_->controllerRef == nullptr) {
        return;
    }
    destroyEditorView();
    if (impl_->editController != nullptr) {
        if (impl_->editControllerInitialized) {
            impl_->editController->terminate();
        }
        impl_->editController->release();
        impl_->editController = nullptr;
        impl_->editControllerInitialized = false;
    }

    // The plug-in instance goes first. ARA explicitly allows either destruction order; releasing
    // the instance before the controller is the order that reads correctly.
    if (impl_->boundComponent != nullptr) {
        impl_->boundComponent->terminate();
        impl_->boundComponent->release();
        impl_->boundComponent = nullptr;
    }
    impl_->plugInExtension = nullptr;

    const auto* iface = impl_->iface();
    if (iface != nullptr) {
        // ARA requires teardown in reverse dependency order: regions, modifications, sources, then
        // the sequence and context, and only then the controller itself.
        for (auto& entry : impl_->sources) {
            iface->enableAudioSourceSamplesAccess(impl_->controllerRef, entry->sourceRef, ARA::kARAFalse);
        }
        iface->beginEditing(impl_->controllerRef);
        for (auto& entry : impl_->sources) {
            if (entry->playbackRegionRef != nullptr) {
                iface->destroyPlaybackRegion(impl_->controllerRef, entry->playbackRegionRef);
            }
            if (entry->modificationRef != nullptr) {
                iface->destroyAudioModification(impl_->controllerRef, entry->modificationRef);
            }
            if (entry->sourceRef != nullptr) {
                iface->destroyAudioSource(impl_->controllerRef, entry->sourceRef);
            }
        }
        if (impl_->regionSequenceRef != nullptr) {
            iface->destroyRegionSequence(impl_->controllerRef, impl_->regionSequenceRef);
        }
        if (impl_->musicalContextRef != nullptr) {
            iface->destroyMusicalContext(impl_->controllerRef, impl_->musicalContextRef);
        }
        iface->endEditing(impl_->controllerRef);
        iface->destroyDocumentController(impl_->controllerRef);
    }
    impl_->sources.clear();
    impl_->regionSequenceRef = nullptr;
    impl_->musicalContextRef = nullptr;
    impl_->controllerRef = nullptr;
    impl_->instance = nullptr;
    impl_->factory = nullptr;
}

#endif

} // namespace neuracoust::daw
