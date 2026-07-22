#include "audio/RemoteDspServerClient.h"
#include "audio/MasterInsertProcessor.h"
#include "audio/MonitorDspProcessor.h"
#include "audio/ProjectAudioRenderer.h"
#include "audio/RemoteDspPluginCatalog.h"
#include "plugins/MonitorDspModules.h"
#include "plugins/Vst3HostFoundation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <processthreadsapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace {

constexpr uint32_t kNaRtMagic = 0x4e415254u;
constexpr uint16_t kNaRtVersion = 1u;
constexpr size_t kNaRtHeaderSize = 20u;
constexpr uint32_t kNaRtFlagParameters = 1u;
constexpr size_t kMaxPacketBytes = kNaRtHeaderSize + 4u + 64u * 8u + 64u * 1024u * sizeof(float);
constexpr uint32_t kNaRtPluginAbiVersion = 1u;
constexpr uint32_t kNaRtMaxPluginChannels = 64u;
constexpr uint32_t kNaRtMaxPluginFrames = 256u;

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
using DynamicLibraryHandle = HMODULE;

struct SocketRuntime {
    SocketRuntime() {
        WSADATA data;
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() {
        if (ready) {
            WSACleanup();
        }
    }
    bool ready = false;
};

void closeSocket(SocketHandle socketHandle) {
    closesocket(socketHandle);
}

DynamicLibraryHandle openDynamicLibrary(const std::string& path, std::string& error) {
    DynamicLibraryHandle handle = LoadLibraryA(path.c_str());
    if (handle == nullptr) {
        error = "could not load RT module DLL: " + path;
    }
    return handle;
}

void* loadDynamicSymbol(DynamicLibraryHandle handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
}

void closeDynamicLibrary(DynamicLibraryHandle handle) {
    if (handle != nullptr) {
        FreeLibrary(handle);
    }
}
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;
using DynamicLibraryHandle = void*;

struct SocketRuntime {
    bool ready = true;
};

void closeSocket(SocketHandle socketHandle) {
    close(socketHandle);
}

DynamicLibraryHandle openDynamicLibrary(const std::string& path, std::string& error) {
    DynamicLibraryHandle handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* detail = dlerror();
        error = std::string("could not load RT module: ") + path + (detail != nullptr ? " (" + std::string(detail) + ")" : "");
    }
    return handle;
}

void* loadDynamicSymbol(DynamicLibraryHandle handle, const char* name) {
    return dlsym(handle, name);
}

void closeDynamicLibrary(DynamicLibraryHandle handle) {
    if (handle != nullptr) {
        dlclose(handle);
    }
}
#endif

enum NaRtParamType {
    NA_RT_PARAM_FLOAT = 0,
    NA_RT_PARAM_BOOL = 1,
    NA_RT_PARAM_CHOICE = 2
};

struct NaRtParamInfo {
    uint32_t index;
    const char* id;
    const char* name;
    NaRtParamType type;
    float default_value;
    float min_value;
    float max_value;
    uint32_t choice_count;
};

struct NaRtPluginInfo {
    uint32_t abi_version;
    const char* id;
    const char* name;
    uint32_t state_size;
    uint32_t param_count;
    const NaRtParamInfo* params;
};

struct NaRtAudioBlock {
    float* channels[kNaRtMaxPluginChannels];
    uint32_t channel_count;
    uint32_t frame_count;
    double sample_rate;
};

struct NaRtPlugin {
    NaRtPluginInfo info;
    void (*init)(void* state, double sample_rate);
    void (*process)(void* state, NaRtAudioBlock* block);
    void (*set_param)(void* state, uint32_t index, float value);
};

using NaRtGetPluginFn = const NaRtPlugin* (*)(void);

std::atomic<bool> gStopRequested {false};

void signalStop(int) {
    gStopRequested = true;
}

void setProcessEnvironmentValue(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

uint16_t readU16(const uint8_t* data) {
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

uint32_t readU32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

std::string percentEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ' ') {
            encoded.push_back(ch == ' ' ? '+' : static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4u) & 0x0Fu]);
            encoded.push_back(kHex[ch & 0x0Fu]);
        }
    }
    return encoded;
}

void writeU16(std::vector<uint8_t>& packet, uint16_t value) {
    const uint16_t network = htons(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    packet.insert(packet.end(), bytes, bytes + sizeof(network));
}

void writeU32(std::vector<uint8_t>& packet, uint32_t value) {
    const uint32_t network = htonl(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    packet.insert(packet.end(), bytes, bytes + sizeof(network));
}

uint16_t boundPort(SocketHandle socketHandle) {
    sockaddr_in addr {};
    SocketLength length = static_cast<SocketLength>(sizeof(addr));
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

struct RemoteCoreOptions {
    std::string bindAddress = "0.0.0.0";
    uint16_t audioPort = 20000;
    uint16_t statusPort = 20001;
    std::string moduleId = "na.neuracoust.remote-core.gain";
    std::string moduleName = "Neuracoust Remote Core Gain";
    std::string modulePath;
    std::string vst3Path;
    std::string vst3Name;
    std::string vst3ClassId;
    std::string vst3ClassName;
    float gain = 1.0f;
    double sampleRate = 48000.0;
    bool once = false;
    bool selfTest = false;
};

struct RemoteCoreStats {
    std::atomic<uint64_t> packetsIn {0};
    std::atomic<uint64_t> packetsOut {0};
    std::atomic<uint64_t> badPackets {0};
};

std::string sanitizedExternalDspKey(std::string value) {
    if (value.empty()) {
        return "unknown";
    }
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '-' || c == '_' || c == '.')) {
            c = '_';
        }
    }
    return value;
}

std::string externalDspModuleIdForDescriptor(const neuracoust::daw::Vst3PluginDescriptor& descriptor) {
    neuracoust::daw::TrackInsertSlot insert;
    insert.pluginName = descriptor.name;
    insert.pluginFormat = "VST3";
    insert.pluginPath = descriptor.bundlePath;
    insert.pluginClassId = descriptor.componentClassCid;
    insert.pluginClassName = descriptor.componentClassName;
    const auto capability = neuracoust::daw::remoteDspCapabilityForInsert(insert, false, true);
    if (!capability.moduleId.empty()) {
        return capability.moduleId;
    }
    if (!descriptor.componentClassCid.empty()) {
        return "vst3:" + sanitizedExternalDspKey(descriptor.componentClassCid);
    }
    if (!descriptor.componentClassName.empty()) {
        return "vst3-class:" + sanitizedExternalDspKey(descriptor.componentClassName);
    }
    if (!descriptor.bundlePath.empty()) {
        return "vst3-path:" + sanitizedExternalDspKey(std::filesystem::path(descriptor.bundlePath).filename().string());
    }
    return "vst3-name:" + sanitizedExternalDspKey(descriptor.name);
}

bool parseU16(const char* text, uint16_t& value) {
    char* end = nullptr;
    const auto parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > 65535u) {
        return false;
    }
    value = static_cast<uint16_t>(parsed);
    return true;
}

bool parseFloat(const char* text, float& value) {
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

void printUsage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [--bind 0.0.0.0] [--port 20000] [--status-port 20001]\n"
        << "       [--module-id id] [--module-name name] [--module-path path] [--gain value] [--once]\n"
        << "       [--vst3-path /path/plugin.vst3] [--vst3-name name] [--vst3-class-id id]\n"
        << "       " << argv0 << " --self-test\n";
}

bool parseArgs(int argc, char** argv, RemoteCoreOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto requireValue = [&](const char* name) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[++index];
        };
        if (arg == "--bind") {
            if (const char* value = requireValue("--bind")) {
                options.bindAddress = value;
            } else {
                return false;
            }
        } else if (arg == "--port") {
            const char* value = requireValue("--port");
            if (value == nullptr || !parseU16(value, options.audioPort)) {
                std::cerr << "Invalid --port value\n";
                return false;
            }
        } else if (arg == "--status-port") {
            const char* value = requireValue("--status-port");
            if (value == nullptr || !parseU16(value, options.statusPort)) {
                std::cerr << "Invalid --status-port value\n";
                return false;
            }
        } else if (arg == "--module-id") {
            if (const char* value = requireValue("--module-id")) {
                options.moduleId = value;
            } else {
                return false;
            }
        } else if (arg == "--module-name") {
            if (const char* value = requireValue("--module-name")) {
                options.moduleName = value;
            } else {
                return false;
            }
        } else if (arg == "--module-path") {
            if (const char* value = requireValue("--module-path")) {
                options.modulePath = value;
            } else {
                return false;
            }
        } else if (arg == "--vst3-path") {
            if (const char* value = requireValue("--vst3-path")) {
                options.vst3Path = value;
            } else {
                return false;
            }
        } else if (arg == "--vst3-name") {
            if (const char* value = requireValue("--vst3-name")) {
                options.vst3Name = value;
            } else {
                return false;
            }
        } else if (arg == "--vst3-class-id") {
            if (const char* value = requireValue("--vst3-class-id")) {
                options.vst3ClassId = value;
            } else {
                return false;
            }
        } else if (arg == "--vst3-class-name") {
            if (const char* value = requireValue("--vst3-class-name")) {
                options.vst3ClassName = value;
            } else {
                return false;
            }
        } else if (arg == "--gain") {
            const char* value = requireValue("--gain");
            if (value == nullptr || !parseFloat(value, options.gain)) {
                std::cerr << "Invalid --gain value\n";
                return false;
            }
        } else if (arg == "--sample-rate") {
            float value = 0.0f;
            const char* text = requireValue("--sample-rate");
            if (text == nullptr || !parseFloat(text, value) || value < 1000.0f) {
                std::cerr << "Invalid --sample-rate value\n";
                return false;
            }
            options.sampleRate = value;
        } else if (arg == "--once") {
            options.once = true;
        } else if (arg == "--self-test") {
            options.selfTest = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

SocketHandle bindUdpSocket(const std::string& address, uint16_t port, std::string& error) {
    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle == kInvalidSocket) {
        error = "could not create UDP socket";
        return kInvalidSocket;
    }

    sockaddr_in bindAddr {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &bindAddr.sin_addr) != 1) {
        closeSocket(socketHandle);
        error = "bind address must be an IPv4 address";
        return kInvalidSocket;
    }
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        closeSocket(socketHandle);
        error = "could not bind UDP socket";
        return kInvalidSocket;
    }
    return socketHandle;
}

std::string hostName() {
    std::array<char, 256> name {};
#ifdef _WIN32
    DWORD size = static_cast<DWORD>(name.size());
    if (GetComputerNameA(name.data(), &size) != 0) {
        return name.data();
    }
#else
    if (gethostname(name.data(), name.size() - 1u) == 0) {
        return name.data();
    }
#endif
    return "neuracoust-remote-core";
}

// Real hardware identity of this node, so the DAW's Remote Core panel can show what it is streaming
// to (CPU / clock / RAM). core_count is reported separately via std::thread::hardware_concurrency.
std::string systemCpuModel() {
#if defined(__APPLE__)
    std::array<char, 256> buf {};
    size_t size = buf.size();
    if (sysctlbyname("machdep.cpu.brand_string", buf.data(), &size, nullptr, 0) == 0 && buf[0] != '\0') {
        return buf.data();
    }
#elif defined(_WIN32)
    HKEY key {};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        char buf[256] = {};
        DWORD size = sizeof(buf);
        const LSTATUS status = RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr,
                                                reinterpret_cast<LPBYTE>(buf), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && buf[0] != '\0') {
            return buf;
        }
    }
#else
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos && line.rfind("model name", 0) == 0) {
            std::string value = line.substr(colon + 1);
            const auto start = value.find_first_not_of(" \t");
            return start == std::string::npos ? "unknown" : value.substr(start);
        }
    }
#endif
    return "unknown";
}

double systemCpuMhz() {
#if defined(__APPLE__)
    uint64_t hz = 0;
    size_t size = sizeof(hz);
    if (sysctlbyname("hw.cpufrequency", &hz, &size, nullptr, 0) == 0 && hz > 0) {
        return static_cast<double>(hz) / 1.0e6;   // Intel Macs report this; Apple Silicon returns 0.
    }
#elif defined(_WIN32)
    HKEY key {};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD mhz = 0, size = sizeof(mhz);
        const LSTATUS status = RegQueryValueExA(key, "~MHz", nullptr, nullptr,
                                                reinterpret_cast<LPBYTE>(&mhz), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && mhz > 0) {
            return static_cast<double>(mhz);
        }
    }
#else
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos && line.rfind("cpu MHz", 0) == 0) {
            return std::atof(line.substr(colon + 1).c_str());
        }
    }
#endif
    return 0.0;   // 0 = unknown (Apple Silicon has no fixed clock to report)
}

uint32_t systemMemoryMb() {
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 && bytes > 0) {
        return static_cast<uint32_t>(bytes / (1024ull * 1024ull));
    }
#elif defined(_WIN32)
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<uint32_t>(status.ullTotalPhys / (1024ull * 1024ull));
    }
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            const unsigned long long kb = std::strtoull(line.c_str() + 9, nullptr, 10);
            return static_cast<uint32_t>(kb / 1024ull);
        }
    }
#endif
    return 0;
}

struct ParsedParameter {
    uint32_t index = 0;
    float value = 0.0f;
};

float readFloat32(const uint8_t* data) {
    uint32_t bits = readU32(data);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct RtPluginRuntime {
    ~RtPluginRuntime() {
        unload();
    }

    bool load(const std::string& path, double sampleRate, std::string& error) {
        unload();
        if (path.empty()) {
            return true;
        }
        library = openDynamicLibrary(path, error);
        if (library == nullptr) {
            return false;
        }
        auto* symbol = loadDynamicSymbol(library, "na_rt_get_plugin");
        if (symbol == nullptr) {
            error = "RT module does not export na_rt_get_plugin: " + path;
            unload();
            return false;
        }
        auto getPlugin = reinterpret_cast<NaRtGetPluginFn>(symbol);
        plugin = getPlugin();
        if (plugin == nullptr ||
            plugin->info.abi_version != kNaRtPluginAbiVersion ||
            plugin->process == nullptr) {
            error = "RT module ABI is incompatible: " + path;
            unload();
            return false;
        }
        state.assign(std::max<uint32_t>(1u, plugin->info.state_size), 0u);
        if (plugin->init != nullptr) {
            plugin->init(state.data(), sampleRate);
        }
        return true;
    }

    void unload() {
        plugin = nullptr;
        state.clear();
        if (library != nullptr) {
            closeDynamicLibrary(library);
            library = nullptr;
        }
    }

    bool loaded() const {
        return plugin != nullptr;
    }

    std::string id() const {
        return plugin != nullptr && plugin->info.id != nullptr ? plugin->info.id : std::string();
    }

    std::string name() const {
        return plugin != nullptr && plugin->info.name != nullptr ? plugin->info.name : std::string();
    }

    bool process(std::vector<float>& samples,
                 uint16_t channels,
                 uint16_t frames,
                 double sampleRate,
                 const std::vector<ParsedParameter>& parameters,
                 std::string& error) {
        if (plugin == nullptr) {
            error = "no RT module is loaded";
            return false;
        }
        if (channels == 0 || channels > kNaRtMaxPluginChannels || frames == 0) {
            error = "audio block is outside RT module limits";
            return false;
        }
        if (plugin->set_param != nullptr) {
            for (const auto& parameter : parameters) {
                plugin->set_param(state.data(), parameter.index, parameter.value);
            }
        }
        size_t frameOffset = 0;
        std::array<float*, kNaRtMaxPluginChannels> channelPointers {};
        while (frameOffset < frames) {
            const uint32_t chunkFrames = static_cast<uint32_t>(
                std::min<size_t>(kNaRtMaxPluginFrames, static_cast<size_t>(frames) - frameOffset));
            for (uint16_t channel = 0; channel < channels; ++channel) {
                channelPointers[channel] =
                    samples.data() + static_cast<size_t>(channel) * frames + frameOffset;
            }
            NaRtAudioBlock block {};
            for (uint16_t channel = 0; channel < channels; ++channel) {
                block.channels[channel] = channelPointers[channel];
            }
            block.channel_count = channels;
            block.frame_count = chunkFrames;
            block.sample_rate = sampleRate;
            plugin->process(state.data(), &block);
            frameOffset += chunkFrames;
        }
        return true;
    }

    DynamicLibraryHandle library = nullptr;
    const NaRtPlugin* plugin = nullptr;
    std::vector<uint8_t> state;
};

struct Vst3HostedRuntime {
    bool prepare(const RemoteCoreOptions& options, std::string& error) {
        chain.reset();
        prepared = false;
        lastParameterSignature.clear();
        if (options.vst3Path.empty()) {
            return true;
        }

        pluginName = options.vst3Name.empty() ? options.moduleName : options.vst3Name;
        pluginPath = options.vst3Path;
        pluginClassId = options.vst3ClassId;
        pluginClassName = options.vst3ClassName;
        sampleRate = options.sampleRate > 1000.0 ? options.sampleRate : 48000.0;
        maxBlockSize = 256;
        return prepareChain({}, error);
    }

    bool loaded() const {
        return prepared;
    }

    bool process(std::vector<float>& samples,
                 uint16_t channels,
                 uint16_t frames,
                 const std::vector<ParsedParameter>& parameters,
                 std::string& error) {
        if (!prepared) {
            error = "no VST3 Remote Core plugin is loaded";
            return false;
        }
        if (channels == 0 || frames == 0) {
            error = "empty VST3 Remote Core audio block";
            return false;
        }
        const std::string parameterSignature = signatureForParameters(parameters);
        if (parameterSignature != lastParameterSignature) {
            if (!prepareChain(parameters, error)) {
                return false;
            }
            lastParameterSignature = parameterSignature;
        }

        std::vector<float> interleaved(static_cast<size_t>(frames) * 2u, 0.0f);
        for (uint16_t frame = 0; frame < frames; ++frame) {
            const float left = samples[frame];
            const float right = channels > 1u ? samples[static_cast<size_t>(frames) + frame] : left;
            interleaved[static_cast<size_t>(frame) * 2u] = left;
            interleaved[static_cast<size_t>(frame) * 2u + 1u] = right;
        }

        if (!chain.processInterleavedStereo(interleaved, frames, error)) {
            return false;
        }

        for (uint16_t frame = 0; frame < frames; ++frame) {
            samples[frame] = interleaved[static_cast<size_t>(frame) * 2u];
            if (channels > 1u) {
                samples[static_cast<size_t>(frames) + frame] = interleaved[static_cast<size_t>(frame) * 2u + 1u];
            }
        }
        return true;
    }

    std::string displayName() const {
        return pluginName.empty() ? std::string("Hosted VST3 Remote Core") : pluginName;
    }

private:
    static std::string signatureForParameters(const std::vector<ParsedParameter>& parameters) {
        std::ostringstream out;
        for (const auto& parameter : parameters) {
            out << parameter.index << '=' << parameter.value << ';';
        }
        return out.str();
    }

    bool prepareChain(const std::vector<ParsedParameter>& parameters, std::string& error) {
        neuracoust::daw::ProjectAudioRenderPlan plan;
        plan.sampleRate = sampleRate;
        plan.hasActiveVst3Inserts = true;
        neuracoust::daw::InsertState insert;
        insert.pluginName = pluginName;
        insert.pluginFormat = "VST3";
        insert.pluginPath = pluginPath;
        insert.pluginClassId = pluginClassId;
        insert.pluginClassName = pluginClassName;
        insert.available = true;
        insert.dspExecutionMode = "remote_internal";
        insert.serverModuleId = "hosted-vst3";
        insert.dspAvailable = true;
        for (const auto& parameter : parameters) {
            neuracoust::daw::Vst3ParameterValueState state;
            state.parameterId = parameter.index;
            state.displayName = "Remote Param " + std::to_string(parameter.index);
            state.normalizedValue = std::clamp(static_cast<double>(parameter.value), 0.0, 1.0);
            insert.parameters.push_back(state);
        }
        plan.activeVst3Inserts.push_back(insert);
        if (!chain.prepare(plan, sampleRate, maxBlockSize, error)) {
            prepared = false;
            return false;
        }
        prepared = true;
        return true;
    }

    neuracoust::daw::RealtimeMasterInsertChain chain;
    bool prepared = false;
    std::string pluginName;
    std::string pluginPath;
    std::string pluginClassId;
    std::string pluginClassName;
    std::string lastParameterSignature;
    double sampleRate = 48000.0;
    int maxBlockSize = 256;
};

std::string buildPluginCatalogPayload(const RemoteCoreOptions& options,
                                      const RtPluginRuntime* rtPlugin,
                                      const Vst3HostedRuntime* vst3Runtime) {
    std::vector<neuracoust::daw::RemoteDspPluginInfo> catalog;
    auto addPlugin = [&](neuracoust::daw::RemoteDspPluginInfo plugin) {
        if (plugin.pluginId.empty() && plugin.pluginName.empty()) {
            return;
        }
        const auto duplicate = std::find_if(catalog.begin(), catalog.end(), [&](const auto& existing) {
            return (!plugin.pluginId.empty() && existing.pluginId == plugin.pluginId) ||
                (!plugin.pluginPath.empty() && existing.pluginPath == plugin.pluginPath &&
                 existing.pluginClassId == plugin.pluginClassId);
        });
        if (duplicate == catalog.end()) {
            catalog.push_back(std::move(plugin));
        }
    };

    if (rtPlugin != nullptr && rtPlugin->loaded()) {
        addPlugin({rtPlugin->id(), rtPlugin->name(), "RT", options.modulePath, "", ""});
    }
    if (!options.moduleId.empty()) {
        addPlugin({options.moduleId, options.moduleName, "Remote Core", options.modulePath, "", ""});
    }
    if (vst3Runtime != nullptr && vst3Runtime->loaded()) {
        addPlugin({options.vst3ClassId.empty() ? ("vst3-path:" + sanitizedExternalDspKey(std::filesystem::path(options.vst3Path).filename().string()))
                                               : ("vst3:" + sanitizedExternalDspKey(options.vst3ClassId)),
                   options.vst3Name.empty() ? options.moduleName : options.vst3Name,
                   "VST3",
                   options.vst3Path,
                   options.vst3ClassId,
                   options.vst3ClassName});
    }

    auto descriptors = neuracoust::daw::scanVst3PluginBundles();
    neuracoust::daw::sortVst3PluginDescriptorsForDisplay(descriptors);
    constexpr size_t kMaxAdvertisedPlugins = 96;
    for (const auto& descriptor : descriptors) {
        if (catalog.size() >= kMaxAdvertisedPlugins) {
            break;
        }
        neuracoust::daw::RemoteDspPluginInfo plugin;
        plugin.pluginId = externalDspModuleIdForDescriptor(descriptor);
        plugin.pluginName = descriptor.name;
        plugin.pluginFormat = "VST3";
        plugin.pluginPath = descriptor.bundlePath;
        plugin.pluginClassId = descriptor.componentClassCid;
        plugin.pluginClassName = descriptor.componentClassName;
        addPlugin(std::move(plugin));
    }

    std::ostringstream out;
    for (size_t index = 0; index < catalog.size(); ++index) {
        const auto& plugin = catalog[index];
        if (index > 0) {
            out << ';';
        }
        out << percentEncode(plugin.pluginId) << '|'
            << percentEncode(plugin.pluginName) << '|'
            << percentEncode(plugin.pluginFormat) << '|'
            << percentEncode(plugin.pluginPath) << '|'
            << percentEncode(plugin.pluginClassId) << '|'
            << percentEncode(plugin.pluginClassName);
    }
    return out.str();
}

enum class BuiltInModuleKind {
    Gain,
    MonitorSpeaker,
    MonitorHeadphone,
    MonitorRoom,
    MonitorCrossfeed,
    ProductPlaceholder
};

const char* moduleKindName(BuiltInModuleKind kind) {
    switch (kind) {
        case BuiltInModuleKind::Gain:
            return "gain";
        case BuiltInModuleKind::MonitorSpeaker:
            return "monitor.speaker";
        case BuiltInModuleKind::MonitorHeadphone:
            return "monitor.headphone";
        case BuiltInModuleKind::MonitorRoom:
            return "monitor.room";
        case BuiltInModuleKind::MonitorCrossfeed:
            return "monitor.crossfeed";
        case BuiltInModuleKind::ProductPlaceholder:
            return "product.placeholder";
    }
    return "unknown";
}

BuiltInModuleKind moduleKindForId(const std::string& moduleId) {
    if (moduleId == "na.neuracoust.monitor.speaker") {
        return BuiltInModuleKind::MonitorSpeaker;
    }
    if (moduleId == "na.neuracoust.monitor.headphone") {
        return BuiltInModuleKind::MonitorHeadphone;
    }
    if (moduleId == "na.neuracoust.monitor.room") {
        return BuiltInModuleKind::MonitorRoom;
    }
    if (moduleId == "na.neuracoust.monitor.crossfeed") {
        return BuiltInModuleKind::MonitorCrossfeed;
    }
    if (moduleId.rfind("na.neuracoust.", 0) == 0 && moduleId != "na.neuracoust.remote-core.gain") {
        return BuiltInModuleKind::ProductPlaceholder;
    }
    return BuiltInModuleKind::Gain;
}

bool isProductModuleId(const std::string& moduleId) {
    return moduleKindForId(moduleId) == BuiltInModuleKind::ProductPlaceholder;
}

std::vector<neuracoust::daw::MonitorDspModule> monitorModulesForKind(BuiltInModuleKind kind) {
    auto modules = neuracoust::daw::defaultMonitorDspModules();
    for (auto& module : modules) {
        module.enabled = false;
        module.streamingPreview = "Off";
    }
    auto enable = [&](const std::string& id) -> neuracoust::daw::MonitorDspModule* {
        auto found = std::find_if(modules.begin(), modules.end(), [&](const auto& module) {
            return module.id == id;
        });
        if (found == modules.end()) {
            return nullptr;
        }
        found->enabled = true;
        return &(*found);
    };
    switch (kind) {
        case BuiltInModuleKind::MonitorSpeaker:
            if (auto* module = enable("speaker-simulation")) {
                module->realModel = "Real Speaker: Flat";
                module->targetModelA = "Speaker A: Yamaha NS-10M Studio (NF)";
            }
            break;
        case BuiltInModuleKind::MonitorHeadphone:
            if (auto* module = enable("headphone-simulation")) {
                module->realModel = "Real Headphones: Closed";
                module->targetModelA = "Headphone A: Speaker A";
            }
            break;
        case BuiltInModuleKind::MonitorRoom:
            enable("room-correction");
            break;
        case BuiltInModuleKind::MonitorCrossfeed:
            enable("crossfeed");
            break;
        case BuiltInModuleKind::Gain:
        case BuiltInModuleKind::ProductPlaceholder:
            break;
    }
    return modules;
}

bool processAudioPacket(const RemoteCoreOptions& options,
                        RtPluginRuntime* rtPlugin,
                        Vst3HostedRuntime* vst3Runtime,
                        const uint8_t* request,
                        size_t requestSize,
                        std::vector<uint8_t>& response) {
    response.clear();
    if (requestSize < kNaRtHeaderSize) {
        return false;
    }
    const uint32_t magic = readU32(request);
    const uint16_t version = readU16(request + 4);
    const uint16_t headerSize = readU16(request + 6);
    const uint32_t sequence = readU32(request + 8);
    const uint16_t channels = readU16(request + 12);
    const uint16_t frames = readU16(request + 14);
    const uint32_t flags = readU32(request + 16);
    if (magic != kNaRtMagic || version != kNaRtVersion || headerSize != kNaRtHeaderSize ||
        channels == 0 || channels > 64u || frames == 0 || frames > 1024u) {
        return false;
    }

    size_t payloadOffset = kNaRtHeaderSize;
    std::vector<ParsedParameter> parameters;
    if ((flags & kNaRtFlagParameters) != 0u) {
        if (requestSize < payloadOffset + 4u) {
            return false;
        }
        const uint16_t parameterCount = readU16(request + payloadOffset);
        payloadOffset += 4u;
        if (requestSize < payloadOffset + static_cast<size_t>(parameterCount) * 8u) {
            return false;
        }
        parameters.reserve(parameterCount);
        for (uint16_t parameterIndex = 0; parameterIndex < parameterCount; ++parameterIndex) {
            ParsedParameter parameter;
            parameter.index = readU32(request + payloadOffset);
            parameter.value = readFloat32(request + payloadOffset + 4u);
            parameters.push_back(parameter);
            payloadOffset += 8u;
        }
    }
    const size_t sampleCount = static_cast<size_t>(channels) * frames;
    const size_t payloadBytes = sampleCount * sizeof(float);
    if (requestSize != payloadOffset + payloadBytes) {
        return false;
    }

    std::vector<float> processed(sampleCount, 0.0f);
    for (size_t index = 0; index < sampleCount; ++index) {
        float sample = 0.0f;
        std::memcpy(&sample, request + payloadOffset + index * sizeof(float), sizeof(sample));
        processed[index] = std::isfinite(sample) ? sample : 0.0f;
    }
    const BuiltInModuleKind kind = moduleKindForId(options.moduleId);
    if (vst3Runtime != nullptr && vst3Runtime->loaded()) {
        std::string error;
        if (!vst3Runtime->process(processed, channels, frames, parameters, error)) {
            return false;
        }
        if (std::isfinite(options.gain) && std::abs(options.gain - 1.0f) > 0.000001f) {
            for (auto& sample : processed) {
                sample *= options.gain;
            }
        }
    } else if (rtPlugin != nullptr && rtPlugin->loaded()) {
        std::string error;
        if (!rtPlugin->process(processed, channels, frames, options.sampleRate, parameters, error)) {
            return false;
        }
        if (std::isfinite(options.gain) && std::abs(options.gain - 1.0f) > 0.000001f) {
            for (auto& sample : processed) {
                sample *= options.gain;
            }
        }
    } else if (kind == BuiltInModuleKind::MonitorSpeaker ||
        kind == BuiltInModuleKind::MonitorHeadphone ||
        kind == BuiltInModuleKind::MonitorRoom ||
        kind == BuiltInModuleKind::MonitorCrossfeed) {
        neuracoust::daw::MonitorDspProcessor processor;
        processor.configure(options.sampleRate, monitorModulesForKind(kind));
        for (uint16_t frame = 0; frame < frames; ++frame) {
            const size_t leftIndex = frame;
            const size_t rightIndex = channels > 1u ? static_cast<size_t>(frames) + frame : frame;
            float left = 0.0f;
            float right = 0.0f;
            std::memcpy(&left, request + payloadOffset + leftIndex * sizeof(float), sizeof(left));
            std::memcpy(&right, request + payloadOffset + rightIndex * sizeof(float), sizeof(right));
            const auto out = processor.process({left, right});
            processed[leftIndex] = std::isfinite(out.left) ? out.left * options.gain : 0.0f;
            if (channels > 1u) {
                processed[rightIndex] = std::isfinite(out.right) ? out.right * options.gain : 0.0f;
            }
        }
    } else {
        const float productPlaceholderTrim = isProductModuleId(options.moduleId) ? 0.98f : 1.0f;
        for (size_t index = 0; index < sampleCount; ++index) {
            processed[index] = std::isfinite(processed[index]) ? processed[index] * options.gain * productPlaceholderTrim : 0.0f;
        }
    }

    response.reserve(kNaRtHeaderSize + payloadBytes);
    writeU32(response, kNaRtMagic);
    writeU16(response, kNaRtVersion);
    writeU16(response, static_cast<uint16_t>(kNaRtHeaderSize));
    writeU32(response, sequence);
    writeU16(response, channels);
    writeU16(response, frames);
    writeU32(response, 0u);
    const size_t responsePayloadOffset = response.size();
    response.resize(responsePayloadOffset + payloadBytes, 0u);
    for (size_t index = 0; index < sampleCount; ++index) {
        std::memcpy(response.data() + responsePayloadOffset + index * sizeof(float), &processed[index], sizeof(float));
    }
    return true;
}

void runStatusServer(SocketHandle socketHandle,
                     const RemoteCoreOptions& options,
                     const RtPluginRuntime* rtPlugin,
                     const Vst3HostedRuntime* vst3Runtime,
                     RemoteCoreStats& stats) {
    std::array<char, 4096> buffer {};
    while (!gStopRequested.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socketHandle, &readSet);
        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
#ifdef _WIN32
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) {
            continue;
        }
        sockaddr_in peer {};
        SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
        const int received = recvfrom(socketHandle,
                                      buffer.data(),
                                      static_cast<int>(buffer.size() - 1u),
                                      0,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peerLength);
        if (received <= 0) {
            continue;
        }
        buffer[static_cast<size_t>(received)] = '\0';
        const std::string request(buffer.data());
        if (request.rfind("NA_STATUS", 0) != 0 && request.rfind("NA_DISCOVER", 0) != 0) {
            stats.badPackets.fetch_add(1u);
            continue;
        }
        const std::string pluginId = rtPlugin != nullptr && rtPlugin->loaded() && !rtPlugin->id().empty()
            ? rtPlugin->id()
            : options.moduleId;
        const std::string pluginName = rtPlugin != nullptr && rtPlugin->loaded() && !rtPlugin->name().empty()
            ? rtPlugin->name()
            : (vst3Runtime != nullptr && vst3Runtime->loaded() ? vst3Runtime->displayName() : options.moduleName);
        const BuiltInModuleKind kind = moduleKindForId(options.moduleId);
        const bool rtModuleLoaded = rtPlugin != nullptr && rtPlugin->loaded();
        const bool vst3Loaded = vst3Runtime != nullptr && vst3Runtime->loaded();
        const bool hostedEngineLoaded = rtModuleLoaded || vst3Loaded;
        const std::string pluginCatalog = buildPluginCatalogPayload(options, rtPlugin, vst3Runtime);
        std::ostringstream out;
        out << "vendor=Neuracoust\n"
            << "model=Remote Core DSP\n"
            << "version=0.1.0\n"
            << "hostname=" << hostName() << "\n"
            << "mac=00:00:00:00:00:00\n"
            << "cpu_model=" << systemCpuModel() << "\n"
            << "cpu_mhz=" << systemCpuMhz() << "\n"
            << "memory_mb=" << systemMemoryMb() << "\n"
            << "temperature_c=0\n"
            << "temperature_f=32\n"
            << "cpu_core_loads=0\n"
            << "nic=udp\n"
            << "audio_port=" << options.audioPort << "\n"
            << "monitor_port=" << options.statusPort << "\n"
            << "channels=2\n"
            << "core_count=" << std::max(1u, std::thread::hardware_concurrency()) << "\n"
            << "buffer_profiles=128,256,512,1024\n"
            << "performance_modes=portable,performance\n"
            << "lpfc=available\n"
            << "lpee=available\n"
            << "plugin_id=" << pluginId << "\n"
            << "plugin_name=" << pluginName << "\n"
            << "plugin_catalog=" << pluginCatalog << "\n"
            << "module_kind=" << moduleKindName(kind) << "\n"
            << "product_dsp_linked=" << (hostedEngineLoaded || !isProductModuleId(options.moduleId) ? "1" : "0") << "\n"
            << "dispatch_state=" << (vst3Loaded ? "vst3_hosted" : (rtModuleLoaded ? "rt_module" : moduleKindName(kind))) << "\n"
            << "module_path=" << options.modulePath << "\n"
            << "vst3_path=" << options.vst3Path << "\n"
            << "module_loaded=" << (hostedEngineLoaded ? "1" : "0") << "\n"
            << "engine_type=" << (vst3Loaded ? "vst3" : (rtModuleLoaded ? "rt_abi" : "builtin")) << "\n"
            << "packets_in=" << stats.packetsIn.load() << "\n"
            << "packets_out=" << stats.packetsOut.load() << "\n"
            << "bad_packets=" << stats.badPackets.load() << "\n";
        const auto text = out.str();
        sendto(socketHandle,
               text.data(),
               static_cast<int>(text.size()),
               0,
               reinterpret_cast<const sockaddr*>(&peer),
               peerLength);
    }
}

int runServer(const RemoteCoreOptions& options,
              std::atomic<uint16_t>* boundAudioPort = nullptr,
              std::atomic<uint16_t>* boundStatusPort = nullptr) {
    RtPluginRuntime rtPlugin;
    Vst3HostedRuntime vst3Runtime;
    if (!options.vst3Path.empty()) {
        std::string vst3Error;
        if (!vst3Runtime.prepare(options, vst3Error)) {
            std::cerr << "VST3 Remote Core load error: " << vst3Error << "\n";
            return 5;
        }
    }
    if (!options.modulePath.empty()) {
        std::string moduleError;
        if (!rtPlugin.load(options.modulePath, options.sampleRate, moduleError)) {
            std::cerr << "RT module load error: " << moduleError << "\n";
            return 4;
        }
    }

    std::string error;
    SocketHandle audioSocket = bindUdpSocket(options.bindAddress, options.audioPort, error);
    if (audioSocket == kInvalidSocket) {
        std::cerr << "Audio server error: " << error << "\n";
        return 2;
    }
    const uint16_t actualAudioPort = boundPort(audioSocket);
    if (boundAudioPort != nullptr) {
        boundAudioPort->store(actualAudioPort, std::memory_order_release);
    }

    SocketHandle statusSocket = kInvalidSocket;
    std::thread statusThread;
    RemoteCoreStats stats;
    statusSocket = bindUdpSocket(options.bindAddress, options.statusPort, error);
    if (statusSocket == kInvalidSocket) {
        closeSocket(audioSocket);
        std::cerr << "Status server error: " << error << "\n";
        return 3;
    }
    const uint16_t actualStatusPort = boundPort(statusSocket);
    RemoteCoreOptions runtimeOptions = options;
    runtimeOptions.audioPort = actualAudioPort;
    runtimeOptions.statusPort = actualStatusPort;
    if (boundStatusPort != nullptr) {
        boundStatusPort->store(actualStatusPort, std::memory_order_release);
    }
    statusThread = std::thread(runStatusServer,
                               statusSocket,
                               std::cref(runtimeOptions),
                               &rtPlugin,
                               &vst3Runtime,
                               std::ref(stats));

    if (!options.once) {
        std::cout << "Neuracoust Remote Core listening on "
                  << options.bindAddress << ":" << actualAudioPort
                  << " status=" << actualStatusPort
                  << " module=" << (rtPlugin.loaded() && !rtPlugin.id().empty() ? rtPlugin.id() : options.moduleId)
                  << (vst3Runtime.loaded() ? " vst3=loaded" : "")
                  << (rtPlugin.loaded() ? " rt-module=loaded" : "")
                  << " gain=" << options.gain << "\n";
    }

    std::vector<uint8_t> request(kMaxPacketBytes);
    std::vector<uint8_t> response;
    while (!gStopRequested.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(audioSocket, &readSet);
        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
#ifdef _WIN32
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(audioSocket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) {
            continue;
        }
        sockaddr_in peer {};
        SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
        const int received = recvfrom(audioSocket,
                                      reinterpret_cast<char*>(request.data()),
                                      static_cast<int>(request.size()),
                                      0,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peerLength);
        if (received <= 0) {
            continue;
        }
        stats.packetsIn.fetch_add(1u);
        if (!processAudioPacket(options, &rtPlugin, &vst3Runtime, request.data(), static_cast<size_t>(received), response)) {
            stats.badPackets.fetch_add(1u);
            if (options.once) {
                break;
            }
            continue;
        }
        const int sent = sendto(audioSocket,
                                reinterpret_cast<const char*>(response.data()),
                                static_cast<int>(response.size()),
                                0,
                                reinterpret_cast<const sockaddr*>(&peer),
                                peerLength);
        if (sent == static_cast<int>(response.size())) {
            stats.packetsOut.fetch_add(1u);
        }
        if (options.once) {
            break;
        }
    }

    gStopRequested = true;
    if (statusThread.joinable()) {
        statusThread.join();
    }
    if (statusSocket != kInvalidSocket) {
        closeSocket(statusSocket);
    }
    closeSocket(audioSocket);
    return 0;
}

int runSelfTest(const char* argv0) {
    (void)argv0;
    gStopRequested = false;
    RemoteCoreOptions options;
    options.bindAddress = "127.0.0.1";
    options.audioPort = 0;
    options.statusPort = 0;
    options.gain = 0.5f;
    options.once = true;

    std::atomic<uint16_t> audioPort {0};
    std::atomic<uint16_t> statusPort {0};
    std::atomic<int> serverResult {-1};
    std::thread serverThread([&] {
        serverResult = runServer(options, &audioPort, &statusPort);
    });
    for (int attempt = 0;
         attempt < 100 && audioPort.load(std::memory_order_acquire) == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const uint16_t boundPort = audioPort.load(std::memory_order_acquire);
    const uint16_t boundStatusPort = statusPort.load(std::memory_order_acquire);
    if (boundPort == 0) {
        gStopRequested = true;
        serverThread.join();
        std::cerr << "self-test server did not bind\n";
        return 20;
    }

    neuracoust::daw::RemoteDspServerSettings settings;
    settings.host = "127.0.0.1";
    settings.rtEnginePort = boundPort;
    settings.statusPort = boundStatusPort;
    settings.timeoutMs = 1000;
    std::vector<float> input {0.25f, -0.5f, 0.5f, -1.0f};
    std::vector<float> output;
    const auto result = neuracoust::daw::processRemoteDspInterleavedStereo(settings, input, output);
    gStopRequested = true;
    serverThread.join();
    if (serverResult.load() != 0) {
        std::cerr << "self-test server failed: " << serverResult.load() << "\n";
        return 21;
    }
    if (!result.processed || output.size() != input.size()) {
        std::cerr << "self-test process failed: " << result.message << "\n";
        return 22;
    }
    for (size_t index = 0; index < input.size(); ++index) {
        const float expected = input[index] * options.gain;
        if (std::abs(output[index] - expected) > 0.000001f) {
            std::cerr << "self-test gain mismatch at " << index << "\n";
            return 23;
        }
    }
    std::cout << "Neuracoust Remote Core self-test passed on UDP "
              << boundPort << " status " << boundStatusPort << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    setProcessEnvironmentValue("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3", "1");
    SocketRuntime sockets;
    if (!sockets.ready) {
        std::cerr << "Socket runtime unavailable\n";
        return 1;
    }
    RemoteCoreOptions options;
    if (!parseArgs(argc, argv, options)) {
        printUsage(argv[0]);
        return 64;
    }
    if (options.selfTest) {
        return runSelfTest(argv[0]);
    }
    std::signal(SIGINT, signalStop);
    std::signal(SIGTERM, signalStop);
    return runServer(options);
}
