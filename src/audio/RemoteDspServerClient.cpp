#include "audio/RemoteDspServerClient.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neuracoust::daw {

namespace {

constexpr uint32_t kNaRtMagic = 0x4e415254u;
constexpr uint16_t kNaRtVersion = 1u;
constexpr size_t kNaRtHeaderSize = 20u;
constexpr uint32_t kNaRtFlagParameters = 1u;
constexpr uint16_t kMaxProbeChannels = 64u;
constexpr uint16_t kMaxProbeFrames = 1024u;
constexpr size_t kMaxRemoteParameterCount = 64u;

std::string percentDecode(std::string value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[index + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[index + 2]))) {
            const char hex[3] = {value[index + 1], value[index + 2], '\0'};
            decoded.push_back(static_cast<char>(std::strtoul(hex, nullptr, 16)));
            index += 2;
        } else if (value[index] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(value[index]);
        }
    }
    return decoded;
}

std::vector<RemoteDspPluginInfo> parseRemotePluginCatalog(const std::string& encoded) {
    std::vector<RemoteDspPluginInfo> catalog;
    size_t entryStart = 0;
    while (entryStart < encoded.size()) {
        const size_t entryEnd = encoded.find(';', entryStart);
        const std::string entry = encoded.substr(entryStart, entryEnd == std::string::npos ? std::string::npos : entryEnd - entryStart);
        if (!entry.empty()) {
            std::array<std::string, 6> fields {};
            size_t fieldStart = 0;
            for (size_t fieldIndex = 0; fieldIndex < fields.size(); ++fieldIndex) {
                const size_t fieldEnd = entry.find('|', fieldStart);
                fields[fieldIndex] = percentDecode(entry.substr(fieldStart, fieldEnd == std::string::npos ? std::string::npos : fieldEnd - fieldStart));
                if (fieldEnd == std::string::npos) {
                    break;
                }
                fieldStart = fieldEnd + 1u;
            }
            RemoteDspPluginInfo plugin;
            plugin.pluginId = fields[0];
            plugin.pluginName = fields[1];
            plugin.pluginFormat = fields[2];
            plugin.pluginPath = fields[3];
            plugin.pluginClassId = fields[4];
            plugin.pluginClassName = fields[5];
            if (!plugin.pluginId.empty() || !plugin.pluginName.empty()) {
                catalog.push_back(std::move(plugin));
            }
        }
        if (entryEnd == std::string::npos) {
            break;
        }
        entryStart = entryEnd + 1u;
    }
    return catalog;
}

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

struct WinsockSession {
    WinsockSession() {
        WSADATA data;
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession() {
        if (ready) {
            WSACleanup();
        }
    }
    bool ready = false;
};

void closeSocket(SocketHandle socketHandle) {
    closesocket(socketHandle);
}
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;

void closeSocket(SocketHandle socketHandle) {
    close(socketHandle);
}
#endif

bool resolveIpv4Endpoint(const std::string& host, uint16_t port, sockaddr_in& target, std::string& message) {
    target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &target.sin_addr) == 1) {
        return true;
    }

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr) {
        message = "could not resolve remote DSP host: " + host;
        return false;
    }
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(resolved->ai_addr);
    target = *ipv4;
    target.sin_port = htons(port);
    freeaddrinfo(resolved);
    return true;
}

void appendU16(std::vector<uint8_t>& packet, uint16_t value) {
    const uint16_t network = htons(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    packet.insert(packet.end(), bytes, bytes + sizeof(network));
}

void appendU32(std::vector<uint8_t>& packet, uint32_t value) {
    const uint32_t network = htonl(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    packet.insert(packet.end(), bytes, bytes + sizeof(network));
}

void appendFloat32(std::vector<uint8_t>& packet, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(packet, bits);
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

std::vector<uint8_t> makeProbePacket(uint32_t sequence, uint16_t channelCount, uint16_t frameCount) {
    auto samples = makeRemoteDspProbeBlock(channelCount, frameCount);
    std::vector<uint8_t> packet;
    packet.reserve(kNaRtHeaderSize + samples.size() * sizeof(float));
    appendU32(packet, kNaRtMagic);
    appendU16(packet, kNaRtVersion);
    appendU16(packet, static_cast<uint16_t>(kNaRtHeaderSize));
    appendU32(packet, sequence);
    appendU16(packet, channelCount);
    appendU16(packet, frameCount);
    appendU32(packet, 0u);
    const auto* sampleBytes = reinterpret_cast<const uint8_t*>(samples.data());
    packet.insert(packet.end(), sampleBytes, sampleBytes + samples.size() * sizeof(float));
    return packet;
}

std::vector<uint8_t> makeStereoProcessPacket(uint32_t sequence,
                                             uint16_t frameCount,
                                             const std::vector<float>& interleavedStereo,
                                             const std::vector<RemoteDspParameterValue>& parameters = {}) {
    std::vector<uint8_t> packet;
    constexpr uint16_t channelCount = 2u;
    const size_t parameterCount = std::min(parameters.size(), kMaxRemoteParameterCount);
    const size_t parameterBytes = parameterCount > 0 ? 4u + parameterCount * 8u : 0u;
    packet.reserve(kNaRtHeaderSize + parameterBytes + static_cast<size_t>(channelCount) * frameCount * sizeof(float));
    appendU32(packet, kNaRtMagic);
    appendU16(packet, kNaRtVersion);
    appendU16(packet, static_cast<uint16_t>(kNaRtHeaderSize));
    appendU32(packet, sequence);
    appendU16(packet, channelCount);
    appendU16(packet, frameCount);
    appendU32(packet, parameterCount > 0 ? kNaRtFlagParameters : 0u);
    if (parameterCount > 0) {
        appendU16(packet, static_cast<uint16_t>(parameterCount));
        appendU16(packet, 0u);
        for (size_t index = 0; index < parameterCount; ++index) {
            appendU32(packet, parameters[index].index);
            appendFloat32(packet, std::clamp(parameters[index].normalizedValue, 0.0f, 1.0f));
        }
    }
    const size_t payloadStart = packet.size();
    packet.resize(payloadStart + static_cast<size_t>(channelCount) * frameCount * sizeof(float), 0u);
    auto* payload = reinterpret_cast<float*>(packet.data() + payloadStart);
    for (uint16_t frame = 0; frame < frameCount; ++frame) {
        const size_t source = static_cast<size_t>(frame) * 2u;
        payload[frame] = source < interleavedStereo.size() ? interleavedStereo[source] : 0.0f;
        payload[static_cast<size_t>(frameCount) + frame] =
            source + 1u < interleavedStereo.size() ? interleavedStereo[source + 1u] : payload[frame];
    }
    return packet;
}

RemoteDspServerProbeResult parseProbeResponse(const uint8_t* data,
                                              size_t size,
                                              uint32_t expectedSequence,
                                              uint16_t expectedChannels,
                                              uint16_t expectedFrames) {
    RemoteDspServerProbeResult result;
    if (size < kNaRtHeaderSize) {
        result.message = "response is shorter than the RT header";
        return result;
    }

    const uint32_t magic = readU32(data);
    const uint16_t version = readU16(data + 4);
    const uint16_t headerSize = readU16(data + 6);
    const uint32_t sequence = readU32(data + 8);
    const uint16_t channels = readU16(data + 12);
    const uint16_t frames = readU16(data + 14);
    const size_t payloadBytes = static_cast<size_t>(channels) * frames * sizeof(float);

    result.sequence = sequence;
    result.channelCount = channels;
    result.frameCount = frames;
    result.protocolMatched = magic == kNaRtMagic &&
        version == kNaRtVersion &&
        headerSize == kNaRtHeaderSize &&
        sequence == expectedSequence &&
        channels == expectedChannels &&
        frames == expectedFrames &&
        size == kNaRtHeaderSize + payloadBytes;
    if (!result.protocolMatched) {
        result.message = "response did not match Neuracoust RT UDP protocol";
        return result;
    }

    const auto* payload = reinterpret_cast<const float*>(data + kNaRtHeaderSize);
    const size_t sampleCount = payloadBytes / sizeof(float);
    float peak = 0.0f;
    for (size_t index = 0; index < sampleCount; ++index) {
        if (std::isfinite(payload[index])) {
            peak = std::max(peak, std::abs(payload[index]));
        }
    }
    result.reachable = true;
    result.returnedPeak = peak;
    result.message = "RT DSP server responded";
    return result;
}

} // namespace

RemoteDspServerSettings defaultRemoteDspServerSettings() {
    RemoteDspServerSettings settings;
    settings.nodes = defaultRemoteDspServerNodes();
    return settings;
}

bool remoteDspModeAvailable(const RemoteDspServerSettings& settings, const std::string& mode) {
    if (mode == "nds") {
        return settings.ndsEnabled && !settings.ndsHost.empty();
    }
    if (mode == "external" || mode == "remote_external") {
        return settings.enabled && !settings.host.empty();
    }
    if (mode == "auto") {
        return (settings.ndsEnabled && !settings.ndsHost.empty()) ||
               (settings.enabled && !settings.host.empty());
    }
    return false;
}

std::string remoteDspModeForRole(const RemoteDspServerSettings& settings, const std::string& role) {
    if (settings.autoOverflow) {
        return "auto";
    }
    if (role == "nds" || role == "external" || role == "remote_external") {
        return role;
    }
    return "internal";
}

RemoteDspServerSettings remoteDspSettingsForMode(const RemoteDspServerSettings& settings,
                                                 const std::string& mode) {
    RemoteDspServerSettings resolved = settings;
    const bool ndsUsable = settings.ndsEnabled && !settings.ndsHost.empty();
    const bool externalUsable = settings.enabled && !settings.host.empty();
    // "auto" prefers the appliance: it is the machine whose timing we control.
    const bool useNds = (mode == "nds") || (mode == "auto" && ndsUsable);
    if (useNds) {
        resolved.host = settings.ndsHost;
        resolved.enabled = ndsUsable;
    } else {
        resolved.enabled = externalUsable;
    }
    // The node list would otherwise steer the client back to whatever it lists; the chosen
    // machine is the target, full stop.
    resolved.nodes.clear();
    return resolved;
}

std::vector<RemoteDspServerNode> defaultRemoteDspServerNodes() {
    RemoteDspServerNode node;
    node.id = "neuracoust-dsp-server";
    node.displayName = "누라쿠스트 DSP 서버";
    node.host = "studio.local";
    node.rtEnginePort = 20000;
    node.dedicatedLink = true;
    node.reportedCoreCount = 4;
    node.dawCoreReserve = 1;
    node.monitorCoreReserve = 1;
    node.pluginCoreReserve = 2;
    return {node};
}

std::vector<RemoteDspServerNode> effectiveRemoteDspServerNodes(const RemoteDspServerSettings& settings) {
    if (!settings.nodes.empty()) {
        return settings.nodes;
    }
    if (!settings.host.empty()) {
        RemoteDspServerNode node;
        node.id = "legacy";
        node.displayName = "누라쿠스트 DSP 서버";
        node.host = settings.host;
        node.rtEnginePort = settings.rtEnginePort;
        node.reportedCoreCount = settings.totalCoreHint;
        node.dawCoreReserve = settings.dawCoreReserve;
        node.monitorCoreReserve = settings.monitorCoreReserve;
        node.pluginCoreReserve = settings.pluginCoreReserve;
        return {node};
    }
    return defaultRemoteDspServerNodes();
}

RemoteDspServerSettings settingsForRemoteDspServerNode(const RemoteDspServerSettings& settings,
                                                       const RemoteDspServerNode& node) {
    RemoteDspServerSettings nodeSettings = settings;
    nodeSettings.host = node.host;
    nodeSettings.rtEnginePort = node.rtEnginePort;
    nodeSettings.statusPort = static_cast<uint16_t>(node.rtEnginePort + 1u);
    nodeSettings.totalCoreHint = node.reportedCoreCount > 0 ? node.reportedCoreCount : nodeSettings.totalCoreHint;
    nodeSettings.dawCoreReserve = node.dawCoreReserve;
    nodeSettings.monitorCoreReserve = node.monitorCoreReserve;
    nodeSettings.pluginCoreReserve = node.pluginCoreReserve;
    return nodeSettings;
}

RemoteDspCorePlan makeRemoteDspCorePlan(const RemoteDspServerSettings& settings,
                                        uint32_t reportedCoreCount,
                                        bool monitorExternal) {
    RemoteDspCorePlan plan;
    const uint32_t hintedCoreCount = reportedCoreCount > 0 ? reportedCoreCount : settings.totalCoreHint;
    plan.totalCores = static_cast<uint16_t>(std::max<uint32_t>(1u, std::min<uint32_t>(16u, hintedCoreCount)));
    plan.monitorExternal = settings.enabled && monitorExternal;
    plan.dawExternal = settings.enabled && settings.dawDspEnabled;
    plan.pluginExternal = settings.enabled && settings.pluginDspEnabled;
    plan.pluginAutoAssigned = settings.autoAssignPluginCores;

    uint16_t remaining = plan.totalCores;
    plan.monitorCores = 0;
    plan.dawCores = 0;
    plan.pluginCores = 0;

    if (plan.monitorExternal && settings.monitorCoreReserve > 0) {
        plan.monitorCores = std::min<uint16_t>(remaining, std::max<uint16_t>(1, settings.monitorCoreReserve));
        remaining = static_cast<uint16_t>(remaining - plan.monitorCores);
    }
    if (plan.dawExternal && settings.dawCoreReserve > 0 && remaining > 0) {
        plan.dawCores = std::min<uint16_t>(remaining, std::max<uint16_t>(1, settings.dawCoreReserve));
        remaining = static_cast<uint16_t>(remaining - plan.dawCores);
    }
    if (plan.pluginExternal && remaining > 0) {
        const uint16_t requestedPluginCores = settings.autoAssignPluginCores
            ? remaining
            : std::max<uint16_t>(1, settings.pluginCoreReserve);
        plan.pluginCores = std::min<uint16_t>(remaining, requestedPluginCores);
        remaining = static_cast<uint16_t>(remaining - plan.pluginCores);
    }
    plan.idleCores = remaining;
    const uint16_t requestedMinimum =
        static_cast<uint16_t>((monitorExternal ? std::max<uint16_t>(1, settings.monitorCoreReserve) : 0) +
                              (plan.dawExternal ? std::max<uint16_t>(1, settings.dawCoreReserve) : 0) +
                              (plan.pluginExternal ? std::max<uint16_t>(1, settings.pluginCoreReserve) : 0));
    plan.saturated = requestedMinimum > plan.totalCores || (plan.pluginExternal && plan.pluginCores == 0);
    plan.summary = "MON " + std::to_string(plan.monitorCores) +
        " / DAW " + std::to_string(plan.dawCores) +
        " / PLG " + std::to_string(plan.pluginCores);
    if (plan.saturated) {
        plan.summary += " (limited)";
    }
    return plan;
}

std::vector<float> makeRemoteDspProbeBlock(uint16_t channelCount, uint16_t frameCount) {
    const size_t sampleCount = static_cast<size_t>(channelCount) * frameCount;
    std::vector<float> samples(sampleCount, 0.0f);
    if (channelCount == 0 || frameCount == 0) {
        return samples;
    }

    for (uint16_t channel = 0; channel < channelCount; ++channel) {
        for (uint16_t frame = 0; frame < frameCount; ++frame) {
            const float phase = static_cast<float>(frame % 32u) / 32.0f;
            samples[static_cast<size_t>(channel) * frameCount + frame] =
                (channel % 2u == 0u ? 0.125f : -0.125f) * phase;
        }
    }
    return samples;
}

RemoteDspServerProbeResult probeRemoteDspServer(const RemoteDspServerSettings& settings) {
    RemoteDspServerProbeResult result;
    if (settings.host.empty()) {
        result.message = "remote DSP host is empty";
        return result;
    }
    if (settings.channelCount == 0 || settings.channelCount > kMaxProbeChannels ||
        settings.frameCount == 0 || settings.frameCount > kMaxProbeFrames) {
        result.message = "remote DSP probe block size is out of range";
        return result;
    }

#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.ready) {
        result.message = "could not initialize Winsock";
        return result;
    }
#endif

    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle == kInvalidSocket) {
        result.message = "could not create UDP socket";
        return result;
    }

    sockaddr_in target {};
    std::string resolveError;
    if (!resolveIpv4Endpoint(settings.host, settings.rtEnginePort, target, resolveError)) {
        closeSocket(socketHandle);
        result.message = resolveError;
        return result;
    }

    constexpr uint32_t sequence = 1u;
    const auto packet = makeProbePacket(sequence, settings.channelCount, settings.frameCount);
    const auto started = std::chrono::steady_clock::now();
    const int sent = sendto(socketHandle,
                            reinterpret_cast<const char*>(packet.data()),
                            static_cast<int>(packet.size()),
                            0,
                            reinterpret_cast<const sockaddr*>(&target),
                            static_cast<SocketLength>(sizeof(target)));
    if (sent != static_cast<int>(packet.size())) {
        closeSocket(socketHandle);
        result.message = "could not send RT DSP probe packet";
        return result;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);
    timeval timeout {};
    timeout.tv_sec = settings.timeoutMs / 1000;
    timeout.tv_usec = (settings.timeoutMs % 1000) * 1000;
#ifdef _WIN32
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const int ready = select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    if (ready <= 0) {
        closeSocket(socketHandle);
        result.message = "RT DSP server did not respond before timeout";
        return result;
    }

    std::array<uint8_t, kNaRtHeaderSize + kMaxProbeChannels * kMaxProbeFrames * sizeof(float)> response {};
    sockaddr_in peer {};
    SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
    const int received = recvfrom(socketHandle,
                                  reinterpret_cast<char*>(response.data()),
                                  static_cast<int>(response.size()),
                                  0,
                                  reinterpret_cast<sockaddr*>(&peer),
                                  &peerLength);
    const auto finished = std::chrono::steady_clock::now();
    closeSocket(socketHandle);

    if (received <= 0) {
        result.message = "could not receive RT DSP probe response";
        return result;
    }

    result = parseProbeResponse(response.data(),
                                static_cast<size_t>(received),
                                sequence,
                                settings.channelCount,
                                settings.frameCount);
    result.roundTripMs = std::chrono::duration<double, std::milli>(finished - started).count();
    return result;
}

RemoteDspServerProbeResult probeRemoteDspServerNode(const RemoteDspServerSettings& settings,
                                                    const RemoteDspServerNode& node) {
    if (!node.enabled) {
        RemoteDspServerProbeResult result;
        result.message = "remote DSP node is disabled";
        return result;
    }
    return probeRemoteDspServer(settingsForRemoteDspServerNode(settings, node));
}

RemoteDspProcessResult processRemoteDspInterleavedStereo(const RemoteDspServerSettings& settings,
                                                         const std::vector<float>& interleavedStereo,
                                                         std::vector<float>& processedInterleavedStereo) {
    static const std::vector<RemoteDspParameterValue> emptyParameters;
    return processRemoteDspInterleavedStereo(settings,
                                            interleavedStereo,
                                            emptyParameters,
                                            processedInterleavedStereo);
}

RemoteDspProcessResult processRemoteDspInterleavedStereo(const RemoteDspServerSettings& settings,
                                                         const std::vector<float>& interleavedStereo,
                                                         const std::vector<RemoteDspParameterValue>& parameters,
                                                         std::vector<float>& processedInterleavedStereo) {
    RemoteDspProcessResult result;
    processedInterleavedStereo.clear();
    if (settings.host.empty()) {
        result.message = "remote DSP host is empty";
        return result;
    }
    if (interleavedStereo.empty() || interleavedStereo.size() % 2u != 0u) {
        result.message = "remote DSP process block must be interleaved stereo";
        return result;
    }
    const auto frameCount = static_cast<uint16_t>(interleavedStereo.size() / 2u);
    if (frameCount == 0 || frameCount > kMaxProbeFrames) {
        result.message = "remote DSP process frame count is out of range";
        return result;
    }

#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.ready) {
        result.message = "could not initialize Winsock";
        return result;
    }
#endif

    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle == kInvalidSocket) {
        result.message = "could not create UDP socket";
        return result;
    }

    sockaddr_in target {};
    std::string resolveError;
    if (!resolveIpv4Endpoint(settings.host, settings.rtEnginePort, target, resolveError)) {
        closeSocket(socketHandle);
        result.message = resolveError;
        return result;
    }

    static std::atomic<uint32_t> sequenceCounter {1000u};
    const uint32_t sequence = sequenceCounter.fetch_add(1u, std::memory_order_relaxed);
    const auto packet = makeStereoProcessPacket(sequence, frameCount, interleavedStereo, parameters);
    const auto started = std::chrono::steady_clock::now();
    const int sent = sendto(socketHandle,
                            reinterpret_cast<const char*>(packet.data()),
                            static_cast<int>(packet.size()),
                            0,
                            reinterpret_cast<const sockaddr*>(&target),
                            static_cast<SocketLength>(sizeof(target)));
    if (sent != static_cast<int>(packet.size())) {
        closeSocket(socketHandle);
        result.message = "could not send RT DSP audio packet";
        return result;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);
    const int timeoutMs = settings.timeoutMs > 0 ? settings.timeoutMs : 4;
    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const int ready = select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    if (ready <= 0) {
        closeSocket(socketHandle);
        result.message = "RT DSP server did not return audio before timeout";
        return result;
    }

    std::array<uint8_t, kNaRtHeaderSize + kMaxProbeChannels * kMaxProbeFrames * sizeof(float)> response {};
    sockaddr_in peer {};
    SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
    const int received = recvfrom(socketHandle,
                                  reinterpret_cast<char*>(response.data()),
                                  static_cast<int>(response.size()),
                                  0,
                                  reinterpret_cast<sockaddr*>(&peer),
                                  &peerLength);
    const auto finished = std::chrono::steady_clock::now();
    closeSocket(socketHandle);

    if (received <= 0) {
        result.message = "could not receive RT DSP audio response";
        return result;
    }

    const uint8_t* data = response.data();
    if (static_cast<size_t>(received) < kNaRtHeaderSize) {
        result.message = "response is shorter than the RT header";
        return result;
    }
    const uint32_t magic = readU32(data);
    const uint16_t version = readU16(data + 4);
    const uint16_t headerSize = readU16(data + 6);
    const uint32_t returnedSequence = readU32(data + 8);
    const uint16_t channels = readU16(data + 12);
    const uint16_t frames = readU16(data + 14);
    const size_t payloadBytes = static_cast<size_t>(channels) * frames * sizeof(float);

    result.reachable = true;
    result.sequence = returnedSequence;
    result.channelCount = channels;
    result.frameCount = frames;
    result.roundTripMs = std::chrono::duration<double, std::milli>(finished - started).count();
    result.protocolMatched = magic == kNaRtMagic &&
        version == kNaRtVersion &&
        headerSize == kNaRtHeaderSize &&
        returnedSequence == sequence &&
        channels == 2u &&
        frames == frameCount &&
        static_cast<size_t>(received) == kNaRtHeaderSize + payloadBytes;
    if (!result.protocolMatched) {
        result.message = "response did not match Neuracoust RT UDP protocol";
        return result;
    }

    const auto* payload = reinterpret_cast<const float*>(data + kNaRtHeaderSize);
    processedInterleavedStereo.assign(static_cast<size_t>(frames) * 2u, 0.0f);
    float peak = 0.0f;
    for (uint16_t frame = 0; frame < frames; ++frame) {
        const float left = payload[frame];
        const float right = payload[static_cast<size_t>(frames) + frame];
        processedInterleavedStereo[static_cast<size_t>(frame) * 2u] = left;
        processedInterleavedStereo[static_cast<size_t>(frame) * 2u + 1u] = right;
        if (std::isfinite(left)) {
            peak = std::max(peak, std::abs(left));
        }
        if (std::isfinite(right)) {
            peak = std::max(peak, std::abs(right));
        }
    }
    result.returnedPeak = peak;
    result.processed = true;
    result.message = "RT DSP server processed audio";
    return result;
}

struct RemoteDspProcessSession::Impl {
#ifdef _WIN32
    WinsockSession winsock;
#endif
    SocketHandle socketHandle = kInvalidSocket;
    sockaddr_in target {};
    std::string key;
    std::atomic<uint32_t> sequenceCounter {200000u};

    ~Impl() {
        reset();
    }

    void reset() {
        if (socketHandle != kInvalidSocket) {
            closeSocket(socketHandle);
            socketHandle = kInvalidSocket;
        }
        key.clear();
        target = {};
    }

    static std::string makeKey(const RemoteDspServerSettings& settings) {
        return settings.host + ":" + std::to_string(settings.rtEnginePort);
    }

    bool ensureConfigured(const RemoteDspServerSettings& settings, std::string& message) {
        if (settings.host.empty()) {
            message = "remote DSP host is empty";
            return false;
        }
#ifdef _WIN32
        if (!winsock.ready) {
            message = "could not initialize Winsock";
            return false;
        }
#endif
        const std::string nextKey = makeKey(settings);
        if (socketHandle != kInvalidSocket && key == nextKey) {
            return true;
        }
        reset();
        socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
        if (socketHandle == kInvalidSocket) {
            message = "could not create persistent UDP socket";
            return false;
        }
        std::string resolveError;
        if (!resolveIpv4Endpoint(settings.host, settings.rtEnginePort, target, resolveError)) {
            reset();
            message = resolveError;
            return false;
        }
        if (connect(socketHandle,
                    reinterpret_cast<const sockaddr*>(&target),
                    static_cast<SocketLength>(sizeof(target))) != 0) {
            reset();
            message = "could not connect persistent UDP socket";
            return false;
        }
        key = nextKey;
        return true;
    }
};

RemoteDspProcessSession::RemoteDspProcessSession()
    : impl_(std::make_unique<Impl>()) {
}

RemoteDspProcessSession::~RemoteDspProcessSession() = default;

void RemoteDspProcessSession::reset() {
    impl_->reset();
}

RemoteDspProcessResult RemoteDspProcessSession::process(const RemoteDspServerSettings& settings,
                                                        const std::vector<float>& interleavedStereo,
                                                        std::vector<float>& processedInterleavedStereo) {
    static const std::vector<RemoteDspParameterValue> emptyParameters;
    return process(settings, interleavedStereo, emptyParameters, processedInterleavedStereo);
}

RemoteDspProcessResult RemoteDspProcessSession::process(const RemoteDspServerSettings& settings,
                                                        const std::vector<float>& interleavedStereo,
                                                        const std::vector<RemoteDspParameterValue>& parameters,
                                                        std::vector<float>& processedInterleavedStereo) {
    RemoteDspProcessResult result;
    processedInterleavedStereo.clear();
    if (interleavedStereo.empty() || interleavedStereo.size() % 2u != 0u) {
        result.message = "remote DSP process block must be interleaved stereo";
        return result;
    }
    const auto frameCount = static_cast<uint16_t>(interleavedStereo.size() / 2u);
    if (frameCount == 0 || frameCount > kMaxProbeFrames) {
        result.message = "remote DSP process frame count is out of range";
        return result;
    }
    if (!impl_->ensureConfigured(settings, result.message)) {
        return result;
    }

    const uint32_t sequence = impl_->sequenceCounter.fetch_add(1u, std::memory_order_relaxed);
    const auto packet = makeStereoProcessPacket(sequence, frameCount, interleavedStereo, parameters);
    const auto started = std::chrono::steady_clock::now();
    const int sent = send(impl_->socketHandle,
                          reinterpret_cast<const char*>(packet.data()),
                          static_cast<int>(packet.size()),
                          0);
    if (sent != static_cast<int>(packet.size())) {
        result.message = "could not send RT DSP audio packet on persistent socket";
        impl_->reset();
        return result;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(impl_->socketHandle, &readSet);
    const int timeoutMs = settings.timeoutMs > 0 ? settings.timeoutMs : 4;
    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const int ready = select(impl_->socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    if (ready <= 0) {
        result.message = "RT DSP server did not return audio before timeout";
        impl_->reset();
        return result;
    }

    std::array<uint8_t, kNaRtHeaderSize + kMaxProbeChannels * kMaxProbeFrames * sizeof(float)> response {};
    const int received = recv(impl_->socketHandle,
                              reinterpret_cast<char*>(response.data()),
                              static_cast<int>(response.size()),
                              0);
    const auto finished = std::chrono::steady_clock::now();
    if (received <= 0) {
        result.message = "could not receive RT DSP audio response";
        return result;
    }

    const uint8_t* data = response.data();
    if (static_cast<size_t>(received) < kNaRtHeaderSize) {
        result.message = "response is shorter than the RT header";
        return result;
    }
    const uint32_t magic = readU32(data);
    const uint16_t version = readU16(data + 4);
    const uint16_t headerSize = readU16(data + 6);
    const uint32_t returnedSequence = readU32(data + 8);
    const uint16_t channels = readU16(data + 12);
    const uint16_t frames = readU16(data + 14);
    const size_t payloadBytes = static_cast<size_t>(channels) * frames * sizeof(float);

    result.reachable = true;
    result.sequence = returnedSequence;
    result.channelCount = channels;
    result.frameCount = frames;
    result.roundTripMs = std::chrono::duration<double, std::milli>(finished - started).count();
    result.protocolMatched = magic == kNaRtMagic &&
        version == kNaRtVersion &&
        headerSize == kNaRtHeaderSize &&
        returnedSequence == sequence &&
        channels == 2u &&
        frames == frameCount &&
        static_cast<size_t>(received) == kNaRtHeaderSize + payloadBytes;
    if (!result.protocolMatched) {
        result.message = "response did not match Neuracoust RT UDP protocol";
        impl_->reset();
        return result;
    }

    const auto* payload = reinterpret_cast<const float*>(data + kNaRtHeaderSize);
    processedInterleavedStereo.assign(static_cast<size_t>(frames) * 2u, 0.0f);
    float peak = 0.0f;
    for (uint16_t frame = 0; frame < frames; ++frame) {
        const float left = payload[frame];
        const float right = payload[static_cast<size_t>(frames) + frame];
        processedInterleavedStereo[static_cast<size_t>(frame) * 2u] = left;
        processedInterleavedStereo[static_cast<size_t>(frame) * 2u + 1u] = right;
        if (std::isfinite(left)) {
            peak = std::max(peak, std::abs(left));
        }
        if (std::isfinite(right)) {
            peak = std::max(peak, std::abs(right));
        }
    }
    result.returnedPeak = peak;
    result.processed = true;
    result.message = "RT DSP server processed audio on persistent socket";
    return result;
}

RemoteDspServerInfo queryRemoteDspServerInfo(const RemoteDspServerSettings& settings) {
    RemoteDspServerInfo info;
    if (settings.host.empty()) {
        info.message = "remote DSP host is empty";
        return info;
    }

#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.ready) {
        info.message = "could not initialize Winsock";
        return info;
    }
#endif

    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle == kInvalidSocket) {
        info.message = "could not create UDP socket";
        return info;
    }

    sockaddr_in target {};
    std::string resolveError;
    if (!resolveIpv4Endpoint(settings.host, settings.statusPort, target, resolveError)) {
        closeSocket(socketHandle);
        info.message = resolveError;
        return info;
    }

    const char request[] = "NA_STATUS\n";
    const auto started = std::chrono::steady_clock::now();
    const int sent = sendto(socketHandle,
                            request,
                            static_cast<int>(sizeof(request) - 1u),
                            0,
                            reinterpret_cast<const sockaddr*>(&target),
                            static_cast<SocketLength>(sizeof(target)));
    if (sent <= 0) {
        closeSocket(socketHandle);
        info.message = "could not send RT DSP status request";
        return info;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);
    const int timeoutMs = settings.timeoutMs > 0 ? std::min(settings.timeoutMs, 1000) : 250;
    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const int ready = select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    if (ready <= 0) {
        closeSocket(socketHandle);
        info.message = "RT DSP status server did not respond before timeout";
        return info;
    }

    std::array<char, 65507> response {};
    sockaddr_in peer {};
    SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
    const int received = recvfrom(socketHandle,
                                  response.data(),
                                  static_cast<int>(response.size() - 1u),
                                  0,
                                  reinterpret_cast<sockaddr*>(&peer),
                                  &peerLength);
    const auto finished = std::chrono::steady_clock::now();
    closeSocket(socketHandle);
    if (received <= 0) {
        info.message = "could not receive RT DSP status response";
        return info;
    }
    response[static_cast<size_t>(received)] = '\0';
    std::istringstream lines(response.data());
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(lines, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        values[line.substr(0, equals)] = line.substr(equals + 1u);
    }
    auto text = [&](const char* key) -> std::string {
        const auto found = values.find(key);
        return found == values.end() ? std::string() : found->second;
    };
    auto number = [&](const char* key) -> double {
        const auto value = text(key);
        return value.empty() ? 0.0 : std::strtod(value.c_str(), nullptr);
    };
    auto integer = [&](const char* key) -> uint64_t {
        const auto value = text(key);
        return value.empty() ? 0u : static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
    };
    auto numberList = [&](const char* key) -> std::vector<double> {
        std::vector<double> parsed;
        const auto value = text(key);
        size_t start = 0;
        while (start < value.size()) {
            const size_t comma = value.find(',', start);
            const auto token = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!token.empty()) {
                parsed.push_back(std::strtod(token.c_str(), nullptr));
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1u;
        }
        return parsed;
    };

    info.reachable = true;
    info.roundTripMs = std::chrono::duration<double, std::milli>(finished - started).count();
    info.vendor = text("vendor");
    info.model = text("model");
    info.version = text("version");
    info.hostname = text("hostname");
    info.macAddress = text("mac");
    info.cpuModel = text("cpu_model");
    info.cpuMhz = number("cpu_mhz");
    info.memoryMb = static_cast<uint32_t>(integer("memory_mb"));
    info.temperatureC = number("temperature_c");
    info.temperatureF = number("temperature_f");
    info.cpuCoreLoads = numberList("cpu_core_loads");
    info.nic = text("nic");
    info.audioPort = static_cast<uint16_t>(integer("audio_port"));
    info.monitorPort = static_cast<uint16_t>(integer("monitor_port"));
    info.channels = static_cast<uint32_t>(integer("channels"));
    info.coreCount = static_cast<uint32_t>(integer("core_count"));
    info.bufferProfiles = text("buffer_profiles");
    info.performanceModes = text("performance_modes");
    info.lpfc = text("lpfc");
    info.lpee = text("lpee");
    info.pluginId = text("plugin_id");
    info.pluginName = text("plugin_name");
    info.pluginCatalog = parseRemotePluginCatalog(text("plugin_catalog"));
    info.packetsIn = integer("packets_in");
    info.packetsOut = integer("packets_out");
    info.badPackets = integer("bad_packets");
    info.message = "RT DSP status server responded";
    return info;
}

std::vector<RemoteDspDiscoveryResult> discoverRemoteDspServers(const RemoteDspServerSettings& settings,
                                                               const std::vector<std::string>& broadcastHosts,
                                                               int timeoutMs) {
    std::vector<RemoteDspDiscoveryResult> results;

#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.ready) {
        return results;
    }
#endif

    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle == kInvalidSocket) {
        return results;
    }

    int enabled = 1;
    setsockopt(socketHandle,
               SOL_SOCKET,
               SO_BROADCAST,
               reinterpret_cast<const char*>(&enabled),
               static_cast<SocketLength>(sizeof(enabled)));

    // Always probe the broadcast address as well as any configured hosts. Previously a configured
    // host made `targets` non-empty and suppressed the broadcast, and the address was parsed with
    // inet_pton alone — which accepts numeric IPs only, so a hostname like "studio.local" was
    // skipped outright and the probe sent nothing at all while the node sat there answering.
    std::vector<std::string> targets = broadcastHosts;
    targets.push_back("255.255.255.255");
    const char request[] = "NA_DISCOVER\n";
    std::set<std::string> probed;
    for (const auto& host : targets) {
        if (host.empty() || !probed.insert(host).second) {
            continue;
        }
        sockaddr_in target {};
        target.sin_family = AF_INET;
        target.sin_port = htons(settings.statusPort);
        if (inet_pton(AF_INET, host.c_str(), &target.sin_addr) != 1) {
            // Not a literal address — resolve it (mDNS ".local" names included).
            addrinfo hints {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* resolved = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &resolved) != 0 || resolved == nullptr) {
                continue;
            }
            target.sin_addr = reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr;
            freeaddrinfo(resolved);
        }
        sendto(socketHandle,
               request,
               static_cast<int>(sizeof(request) - 1u),
               0,
               reinterpret_cast<const sockaddr*>(&target),
               static_cast<SocketLength>(sizeof(target)));
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(1, timeoutMs));
    std::set<std::string> seen;
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socketHandle, &readSet);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        timeval timeout {};
        timeout.tv_sec = static_cast<long>(std::max<int64_t>(0, remaining.count()) / 1000);
        timeout.tv_usec = static_cast<long>((std::max<int64_t>(0, remaining.count()) % 1000) * 1000);
#ifdef _WIN32
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) {
            break;
        }

        std::array<char, 65507> response {};
        sockaddr_in peer {};
        SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
        const int received = recvfrom(socketHandle,
                                      response.data(),
                                      static_cast<int>(response.size() - 1u),
                                      0,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peerLength);
        if (received <= 0) {
            continue;
        }
        response[static_cast<size_t>(received)] = '\0';

        std::map<std::string, std::string> values;
        std::istringstream lines(response.data());
        std::string line;
        while (std::getline(lines, line)) {
            const auto equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            values[line.substr(0, equals)] = line.substr(equals + 1u);
        }
        auto text = [&](const char* key) -> std::string {
            const auto found = values.find(key);
            return found == values.end() ? std::string() : found->second;
        };
        auto integer = [&](const char* key) -> uint64_t {
            const auto value = text(key);
            return value.empty() ? 0u : static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
        };
        if (text("vendor") != "Neuracoust") {
            continue;
        }
        std::array<char, INET_ADDRSTRLEN> peerText {};
        if (inet_ntop(AF_INET, &peer.sin_addr, peerText.data(), peerText.size()) == nullptr) {
            continue;
        }
        const std::string host = peerText.data();
        const uint16_t audioPort = static_cast<uint16_t>(integer("audio_port"));
        const uint16_t statusPort = static_cast<uint16_t>(integer("monitor_port"));
        const std::string dedupeKey = host + ":" + std::to_string(audioPort);
        if (audioPort == 0 || seen.find(dedupeKey) != seen.end()) {
            continue;
        }
        seen.insert(dedupeKey);

        RemoteDspDiscoveryResult result;
        result.info.reachable = true;
        result.info.vendor = text("vendor");
        result.info.model = text("model");
        result.info.version = text("version");
        result.info.hostname = text("hostname");
        result.info.audioPort = audioPort;
        result.info.monitorPort = statusPort;
        result.info.channels = static_cast<uint32_t>(integer("channels"));
        result.info.coreCount = static_cast<uint32_t>(integer("core_count"));
        result.info.cpuModel = text("cpu_model");
        result.info.cpuMhz = std::strtod(text("cpu_mhz").c_str(), nullptr);
        result.info.memoryMb = static_cast<uint32_t>(integer("memory_mb"));
        result.info.temperatureC = static_cast<double>(std::strtod(text("temperature_c").c_str(), nullptr));
        result.info.temperatureF = static_cast<double>(std::strtod(text("temperature_f").c_str(), nullptr));
        result.info.packetsIn = integer("packets_in");
        result.info.packetsOut = integer("packets_out");
        result.info.badPackets = integer("bad_packets");
        result.info.pluginId = text("plugin_id");
        result.info.pluginName = text("plugin_name");
        result.info.pluginCatalog = parseRemotePluginCatalog(text("plugin_catalog"));
        result.info.message = "RT DSP discovery response";

        result.node.enabled = true;
        result.node.id = result.info.hostname.empty() ? dedupeKey : result.info.hostname;
        result.node.displayName = result.info.hostname.empty() ? "Neuracoust Remote Core" : result.info.hostname;
        result.node.host = host;
        result.node.rtEnginePort = audioPort;
        result.node.reportedCoreCount = result.info.coreCount > 0
            ? static_cast<uint16_t>(std::min<uint32_t>(result.info.coreCount, 65535u))
            : settings.totalCoreHint;
        result.node.dedicatedLink = true;
        results.push_back(result);
    }

    closeSocket(socketHandle);
    return results;
}

RemoteDspNodeSelection selectRemoteDspNodeForTrackGroup(const std::vector<RemoteDspNodeStatus>& nodes) {
    RemoteDspNodeSelection best;
    double bestScore = std::numeric_limits<double>::infinity();

    for (size_t index = 0; index < nodes.size(); ++index) {
        const auto& status = nodes[index];
        const auto& node = status.node;
        const auto& probe = status.probe;
        if (!node.enabled || node.host.empty() || !probe.reachable || !probe.protocolMatched) {
            continue;
        }
        if (node.maxTrackGroups > 0 && node.assignedTrackGroups >= node.maxTrackGroups) {
            continue;
        }

        const double loadPenalty = std::clamp(node.estimatedDspLoad, 0.0, 1.0) * 60.0;
        const double assignmentPenalty = static_cast<double>(node.assignedTrackGroups) * 8.0;
        const double latencyPenalty = std::max(0.0, probe.roundTripMs) * 3.0;
        const double linkPenalty = node.linkCapacityMbps > 0.0 ? 1000.0 / node.linkCapacityMbps : 1.0;
        const double sharedLinkPenalty = node.dedicatedLink ? 0.0 : 5.0;
        const double score = loadPenalty + assignmentPenalty + latencyPenalty + linkPenalty + sharedLinkPenalty;

        if (score < bestScore) {
            bestScore = score;
            best.selected = true;
            best.nodeIndex = index;
            best.node = node;
            best.score = score;
        }
    }

    if (!best.selected) {
        best.message = "No reachable remote DSP node is available for this track group.";
        return best;
    }

    std::ostringstream message;
    message << "Auto-assigned track group to "
            << (best.node.displayName.empty() ? best.node.host : best.node.displayName)
            << " (" << best.node.host << ":" << best.node.rtEnginePort << ")";
    best.message = message.str();
    return best;
}

} // namespace neuracoust::daw
