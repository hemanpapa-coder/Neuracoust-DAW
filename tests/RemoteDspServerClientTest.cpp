#include "audio/RemoteDspServerClient.h"
#include "audio/RemoteDspPluginCatalog.h"
#include "audio/NeuracoustDspEngine.h"
#include "audio/WavFile.h"
#include "project/ProjectDocument.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

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

void setReceiveTimeout(SocketHandle socketHandle, int timeoutMs) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

uint16_t boundPort(SocketHandle socketHandle) {
    sockaddr_in addr {};
    SocketLength length = static_cast<SocketLength>(sizeof(addr));
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

uint16_t readNetworkU16(const uint8_t* data) {
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

uint32_t readNetworkU32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

neuracoust::daw::TrackInsertSlot makeTrackInsert(const std::string& name,
                                                 const std::string& format,
                                                 const std::string& path) {
    neuracoust::daw::TrackInsertSlot insert;
    insert.pluginName = name;
    insert.pluginFormat = format;
    insert.pluginPath = path;
    insert.bypassed = false;
    insert.enabled = true;
    return insert;
}

} // namespace

int main() {
#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.ready) {
        std::cerr << "Winsock unavailable\n";
        return 1;
    }
#endif

    SocketHandle serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket == kInvalidSocket) {
        std::cerr << "Could not create mock server socket\n";
        return 2;
    }
    setReceiveTimeout(serverSocket, 2000);

    sockaddr_in bindAddr {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bindAddr.sin_port = 0;
    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        closeSocket(serverSocket);
        std::cerr << "Could not bind mock server socket\n";
        return 3;
    }

    const uint16_t port = boundPort(serverSocket);
    if (port == 0) {
        closeSocket(serverSocket);
        std::cerr << "Could not read mock server port\n";
        return 4;
    }

    std::atomic<bool> handled {false};
    std::thread serverThread([&] {
        std::vector<uint8_t> buffer(4096);
        sockaddr_in peer {};
        SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
        const int received = recvfrom(serverSocket,
                                      reinterpret_cast<char*>(buffer.data()),
                                      static_cast<int>(buffer.size()),
                                      0,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peerLength);
        if (received > 0) {
            sendto(serverSocket,
                   reinterpret_cast<const char*>(buffer.data()),
                   received,
                   0,
                   reinterpret_cast<const sockaddr*>(&peer),
                   peerLength);
            handled = true;
        }
        closeSocket(serverSocket);
    });

    neuracoust::daw::RemoteDspServerSettings settings;
    settings.host = "127.0.0.1";
    settings.rtEnginePort = port;
    settings.channelCount = 2;
    settings.frameCount = 128;
    settings.timeoutMs = 1000;
    const auto result = neuracoust::daw::probeRemoteDspServer(settings);

    serverThread.join();

    if (!handled.load()) {
        std::cerr << "Mock server did not handle the probe\n";
        return 5;
    }
    if (!result.reachable || !result.protocolMatched) {
        std::cerr << "Probe did not validate RT protocol: " << result.message << "\n";
        return 6;
    }
    if (result.channelCount != 2 || result.frameCount != 128 || result.returnedPeak <= 0.0f) {
        std::cerr << "Probe response did not preserve expected audio block metadata\n";
        return 7;
    }

    SocketHandle processServerSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (processServerSocket == kInvalidSocket) {
        std::cerr << "Could not create mock process server socket\n";
        return 30;
    }
    setReceiveTimeout(processServerSocket, 2000);
    sockaddr_in processBindAddr {};
    processBindAddr.sin_family = AF_INET;
    processBindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    processBindAddr.sin_port = 0;
    if (bind(processServerSocket, reinterpret_cast<sockaddr*>(&processBindAddr), sizeof(processBindAddr)) != 0) {
        closeSocket(processServerSocket);
        std::cerr << "Could not bind mock process server socket\n";
        return 31;
    }
    const uint16_t processPort = boundPort(processServerSocket);
    if (processPort == 0) {
        closeSocket(processServerSocket);
        std::cerr << "Could not read mock process server port\n";
        return 32;
    }

    std::atomic<bool> processHandled {false};
    std::thread processServerThread([&] {
        std::vector<uint8_t> buffer(4096);
        sockaddr_in peer {};
        SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
        const int received = recvfrom(processServerSocket,
                                      reinterpret_cast<char*>(buffer.data()),
                                      static_cast<int>(buffer.size()),
                                      0,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peerLength);
        if (received > 20) {
            const uint16_t frames = readNetworkU16(buffer.data() + 14);
            const uint32_t flags = readNetworkU32(buffer.data() + 16);
            size_t audioOffset = 20u;
            if ((flags & 1u) != 0u && static_cast<size_t>(received) >= audioOffset + 4u) {
                const uint16_t parameterCount = readNetworkU16(buffer.data() + audioOffset);
                audioOffset += 4u + static_cast<size_t>(parameterCount) * 8u;
            }
            const size_t responseBytes = 20u + static_cast<size_t>(frames) * 2u * sizeof(float);
            std::vector<uint8_t> response(responseBytes, 0u);
            std::copy(buffer.begin(), buffer.begin() + 20, response.begin());
            response[16] = response[17] = response[18] = response[19] = 0;
            if (static_cast<size_t>(received) >= audioOffset + static_cast<size_t>(frames) * 2u * sizeof(float)) {
                const auto* input = reinterpret_cast<const float*>(buffer.data() + audioOffset);
                auto* output = reinterpret_cast<float*>(response.data() + 20u);
                for (uint16_t frame = 0; frame < frames; ++frame) {
                    output[frame] = input[frame] * 0.5f;
                    output[static_cast<size_t>(frames) + frame] =
                        input[static_cast<size_t>(frames) + frame] * 0.5f;
                }
                sendto(processServerSocket,
                       reinterpret_cast<const char*>(response.data()),
                       static_cast<int>(response.size()),
                       0,
                       reinterpret_cast<const sockaddr*>(&peer),
                       peerLength);
                processHandled = true;
            }
        }
        closeSocket(processServerSocket);
    });

    auto processSettings = settings;
    processSettings.rtEnginePort = processPort;
    processSettings.timeoutMs = 1000;
    std::vector<float> processInput {0.10f, -0.20f, 0.30f, -0.40f};
    std::vector<float> processOutput;
    const auto processResult = neuracoust::daw::processRemoteDspInterleavedStereo(
        processSettings,
        processInput,
        {{3u, 0.75f}},
        processOutput);
    processServerThread.join();
    if (!processHandled.load()) {
        std::cerr << "Mock process server did not handle the audio packet\n";
        return 33;
    }
    if (!processResult.processed ||
        !processResult.protocolMatched ||
        processOutput.size() != processInput.size() ||
        std::abs(processOutput[0] - 0.05f) > 0.000001f ||
        std::abs(processOutput[1] + 0.10f) > 0.000001f ||
        std::abs(processOutput[2] - 0.15f) > 0.000001f ||
        std::abs(processOutput[3] + 0.20f) > 0.000001f) {
        std::cerr << "Mock Neuracoust DSP server process contract failed: "
                  << processResult.message
                  << "\n";
        return 34;
    }

    SocketHandle discoveryServerSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (discoveryServerSocket == kInvalidSocket) {
        std::cerr << "Could not create mock discovery server socket\n";
        return 35;
    }
    setReceiveTimeout(discoveryServerSocket, 2000);
    sockaddr_in discoveryBindAddr {};
    discoveryBindAddr.sin_family = AF_INET;
    discoveryBindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    discoveryBindAddr.sin_port = 0;
    if (bind(discoveryServerSocket, reinterpret_cast<sockaddr*>(&discoveryBindAddr), sizeof(discoveryBindAddr)) != 0) {
        closeSocket(discoveryServerSocket);
        std::cerr << "Could not bind mock discovery server socket\n";
        return 36;
    }
    const uint16_t discoveryPort = boundPort(discoveryServerSocket);
    if (discoveryPort == 0) {
        closeSocket(discoveryServerSocket);
        std::cerr << "Could not read mock discovery server port\n";
        return 37;
    }

    std::atomic<bool> discoveryHandled {false};
    std::thread discoveryServerThread([&] {
        std::array<char, 256> buffer {};
        sockaddr_in peer {};
        SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));
        const int received = recvfrom(discoveryServerSocket,
                                      buffer.data(),
                                      static_cast<int>(buffer.size() - 1u),
                                      0,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peerLength);
        if (received > 0 && std::string(buffer.data()).rfind("NA_DISCOVER", 0) == 0) {
            const std::string response =
                "vendor=Neuracoust\n"
                "model=Remote Core DSP\n"
                "version=0.1.0\n"
                "hostname=loopback-remote-core\n"
                "audio_port=20000\n"
                "monitor_port=" + std::to_string(discoveryPort) + "\n"
                "channels=2\n"
                "core_count=8\n"
                "plugin_id=na.neuracoust.4001e\n"
                "plugin_name=Neuracoust 4001E RT\n"
                "plugin_catalog=na.neuracoust.4001e|Neuracoust+4001E+RT|RT|||;vst3%3Aabc123|GEQ+Modern+Stereo|VST3|%2FLibrary%2FAudio%2FPlug-Ins%2FVST3%2FGEQ.vst3|abc123|GEQ+Modern+Stereo\n";
            sendto(discoveryServerSocket,
                   response.data(),
                   static_cast<int>(response.size()),
                   0,
                   reinterpret_cast<const sockaddr*>(&peer),
                   peerLength);
            discoveryHandled = true;
        }
        closeSocket(discoveryServerSocket);
    });

    auto discoverySettings = settings;
    discoverySettings.statusPort = discoveryPort;
    const auto discovered = neuracoust::daw::discoverRemoteDspServers(discoverySettings, {"127.0.0.1"}, 1000);
    discoveryServerThread.join();
    if (!discoveryHandled.load() ||
        discovered.size() != 1 ||
        discovered.front().node.host != "127.0.0.1" ||
        discovered.front().node.rtEnginePort != 20000 ||
        discovered.front().node.reportedCoreCount != 8 ||
        discovered.front().info.pluginId != "na.neuracoust.4001e" ||
        discovered.front().info.pluginCatalog.size() != 2 ||
        discovered.front().info.pluginCatalog[1].pluginName != "GEQ Modern Stereo" ||
        discovered.front().info.pluginCatalog[1].pluginClassId != "abc123") {
        std::cerr << "Remote Core discovery did not return the mock LAN node\n";
        return 38;
    }

    const auto defaults = neuracoust::daw::defaultRemoteDspServerSettings();
    if (defaults.host != "studio.local" || defaults.rtEnginePort != 20000) {
        std::cerr << "Remote DSP server defaults no longer match the Intel Mac Remote Core profile\n";
        return 8;
    }

    auto nodes = neuracoust::daw::defaultRemoteDspServerNodes();
    const auto effectiveNodes = neuracoust::daw::effectiveRemoteDspServerNodes(defaults);
    if (nodes.size() != 1 ||
        effectiveNodes.size() != 1 ||
        !defaults.autoAssignTrackGroups ||
        nodes.front().id != "neuracoust-dsp-server" ||
        nodes.front().displayName != "누라쿠스트 DSP 서버" ||
        !nodes.front().dedicatedLink ||
        nodes.front().host != defaults.host ||
        effectiveNodes.front().rtEnginePort != defaults.rtEnginePort) {
        std::cerr << "Remote DSP node defaults do not preserve the legacy appliance endpoint\n";
        return 9;
    }
    if (std::getenv("NEURACOUST_DAW_REAL_REMOTE_DSP_PROBE") != nullptr) {
        auto realSettings = defaults;
        realSettings.timeoutMs = 1000;
        if (const char* hostOverride = std::getenv("NEURACOUST_DAW_REAL_REMOTE_DSP_HOST")) {
            if (hostOverride[0] != '\0') {
                realSettings.host = hostOverride;
                if (!realSettings.nodes.empty()) {
                    realSettings.nodes.front().host = hostOverride;
                }
            }
        }
        auto applyPortOverride = [&](const char* envName, uint16_t& value) {
            if (const char* text = std::getenv(envName)) {
                if (text[0] == '\0') {
                    return;
                }
                char* end = nullptr;
                const auto parsed = std::strtoul(text, &end, 10);
                if (end != text && *end == '\0' && parsed > 0u && parsed <= 65535u) {
                    value = static_cast<uint16_t>(parsed);
                }
            }
        };
        applyPortOverride("NEURACOUST_DAW_REAL_REMOTE_DSP_PORT", realSettings.rtEnginePort);
        applyPortOverride("NEURACOUST_DAW_REAL_REMOTE_DSP_STATUS_PORT", realSettings.statusPort);
        if (!realSettings.nodes.empty()) {
            realSettings.nodes.front().rtEnginePort = realSettings.rtEnginePort;
        }
        const auto realProbe = neuracoust::daw::probeRemoteDspServer(realSettings);
        if (!realProbe.reachable || !realProbe.protocolMatched) {
            std::cerr << "누라쿠스트 DSP 서버 실장비 probe 실패: " << realProbe.message << "\n";
            return 14;
        }
        const auto serverInfo = neuracoust::daw::queryRemoteDspServerInfo(realSettings);
        if (!serverInfo.reachable ||
            serverInfo.vendor != "Neuracoust" ||
            serverInfo.model.find("DSP") == std::string::npos ||
            serverInfo.coreCount == 0 ||
            serverInfo.channels < 2 ||
            serverInfo.macAddress.empty()) {
            std::cerr << "누라쿠스트 DSP 서버 상태 정보 조회 실패: "
                      << serverInfo.message
                      << " vendor="
                      << serverInfo.vendor
                      << " model="
                      << serverInfo.model
                      << "\n";
            return 19;
        }
        realSettings.loadedPluginIdHint = serverInfo.pluginId;
        std::vector<float> processInput(256, 0.0f);
        for (size_t i = 0; i < processInput.size(); i += 2) {
            processInput[i] = 0.05f;
            processInput[i + 1] = -0.05f;
        }
        std::vector<float> processOutput;
        const auto processResult = neuracoust::daw::processRemoteDspInterleavedStereo(realSettings, processInput, processOutput);
        if (!processResult.processed || processOutput.size() != processInput.size()) {
            std::cerr << "누라쿠스트 DSP 서버 실장비 audio process 실패: " << processResult.message << "\n";
            return 15;
        }
        neuracoust::daw::WavAudioData remoteTrackSource;
        remoteTrackSource.channels = 1;
        remoteTrackSource.sampleRate = 48000;
        remoteTrackSource.interleavedSamples.assign(256, 0.05f);
        std::string wavError;
        const auto remoteTrackSourcePath =
            (std::filesystem::temp_directory_path() / "neuracoust-daw-remote-track-insert.wav").string();
        if (!neuracoust::daw::writePcm16WavFile(remoteTrackSourcePath, remoteTrackSource, wavError)) {
            std::cerr << "원격 트랙 인서트 테스트 WAV 생성 실패: " << wavError << "\n";
            return 16;
        }
        auto remoteTrackProject = neuracoust::daw::defaultProject();
        remoteTrackProject.sampleRate = 48000.0;
        remoteTrackProject.clips.push_back({"remote-track-insert-clip", "Audio 1", remoteTrackSourcePath, 0.0, 0.005, 0.0, 0.0f});
        remoteTrackProject.tracks[0].inserts.push_back(makeTrackInsert(
            "Neuracoust 4001E",
            "VST3",
            "/Library/Audio/Plug-Ins/VST3/Newacoust4001E.vst3"));
        const auto real4001Capability =
            neuracoust::daw::remoteDspCapabilityForInsert(remoteTrackProject.tracks[0].inserts.front(), false, true);
        if (real4001Capability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteCapable ||
            real4001Capability.moduleId != "na.neuracoust.4001e") {
            std::cerr << "4001E real-track insert was not recognized as remote-capable before engine render\n";
            return 27;
        }
        remoteTrackProject.tracks[0].inserts.front().dspExecutionMode = "external";
        remoteTrackProject.tracks[0].inserts.front().assignedDspServerId = realSettings.host;
        remoteTrackProject.tracks[0].inserts.front().serverModuleId = real4001Capability.moduleId;
        neuracoust::daw::NeuracoustDspEngine remoteTrackEngine;
        neuracoust::daw::AudioEngineSettings remoteTrackSettings;
        remoteTrackSettings.sampleRate = 48000.0;
        remoteTrackSettings.bufferSize = 128;
        remoteTrackSettings.monitorDspEnabled = false;
        remoteTrackSettings.monitorDspPathMode = "external";
        remoteTrackSettings.remoteDspServer = realSettings;
        std::string engineError;
        if (!remoteTrackEngine.configure(remoteTrackSettings, remoteTrackSettings.bufferSize, engineError) ||
            !remoteTrackEngine.loadProject(remoteTrackProject, engineError)) {
            std::cerr << "원격 트랙 인서트 엔진 준비 실패: " << engineError << "\n";
            return 17;
        }
        std::vector<float> remoteTrackRender;
        remoteTrackEngine.renderInterleavedStereo(128, remoteTrackRender);
        const auto remoteTrackStatus = remoteTrackEngine.statusSnapshot();
        if (remoteTrackStatus.activeRemoteDspTrackInsertCount < 1 ||
            remoteTrackRender.size() != 256 ||
            remoteTrackStatus.message.find("Track insert routed through") == std::string::npos) {
            std::cerr << "원격 트랙 인서트가 서버 경로로 처리되지 않았습니다: "
                      << remoteTrackStatus.message
                      << " count="
                      << remoteTrackStatus.activeRemoteDspTrackInsertCount
                      << "\n";
            return 18;
        }
        auto mismatchedModuleProject = remoteTrackProject;
        mismatchedModuleProject.tracks[0].inserts.clear();
        mismatchedModuleProject.tracks[0].inserts.push_back(makeTrackInsert(
            "Neuracoust Compressor DM2C",
            "VST3",
            "/Library/Audio/Plug-Ins/VST3/Neuracoust Compressor DM2C.vst3"));
        mismatchedModuleProject.tracks[0].inserts.front().dspExecutionMode = "external";
        mismatchedModuleProject.tracks[0].inserts.front().assignedDspServerId = realSettings.host;
        mismatchedModuleProject.tracks[0].inserts.front().serverModuleId = "na.neuracoust.compressor.dm2c";
        neuracoust::daw::NeuracoustDspEngine mismatchedModuleEngine;
        if (!mismatchedModuleEngine.configure(remoteTrackSettings, remoteTrackSettings.bufferSize, engineError) ||
            !mismatchedModuleEngine.loadProject(mismatchedModuleProject, engineError)) {
            std::cerr << "모듈 불일치 트랙 인서트 엔진 준비 실패: " << engineError << "\n";
            return 20;
        }
        std::vector<float> mismatchedRender;
        mismatchedModuleEngine.renderInterleavedStereo(128, mismatchedRender);
        const auto mismatchedStatus = mismatchedModuleEngine.statusSnapshot();
        if (serverInfo.pluginId == "na.neuracoust.4001e" &&
            mismatchedStatus.activeRemoteDspTrackInsertCount != 0) {
            std::cerr << "서버가 4001E 모듈인데 다른 NDS 플러그인이 원격으로 라우팅되었습니다: count="
                      << mismatchedStatus.activeRemoteDspTrackInsertCount
                      << "\n";
            return 21;
        }
        std::cout << "누라쿠스트 DSP 서버 READY · "
                  << realProbe.roundTripMs
                  << " ms probe · "
                  << processResult.roundTripMs
                  << " ms audio · "
                  << remoteTrackStatus.remoteDspRoundTripMs
                  << " ms track-insert · "
                  << serverInfo.coreCount
                  << " cores · "
                  << serverInfo.temperatureC
                  << " C · "
                  << realSettings.host
                  << ":"
                  << defaults.rtEnginePort
                  << "\n";
    }

    neuracoust::daw::RemoteDspNodeStatus slowShared;
    slowShared.node.id = "shared-a";
    slowShared.node.displayName = "Shared switch DSP";
    slowShared.node.host = "10.10.1.2";
    slowShared.node.linkCapacityMbps = 1000.0;
    slowShared.node.estimatedDspLoad = 0.80;
    slowShared.node.assignedTrackGroups = 4;
    slowShared.probe.reachable = true;
    slowShared.probe.protocolMatched = true;
    slowShared.probe.roundTripMs = 1.2;

    neuracoust::daw::RemoteDspNodeStatus directIdle;
    directIdle.node.id = "direct-b";
    directIdle.node.displayName = "Direct DSP";
    directIdle.node.host = "10.10.2.2";
    directIdle.node.dedicatedLink = true;
    directIdle.node.linkCapacityMbps = 2500.0;
    directIdle.node.estimatedDspLoad = 0.20;
    directIdle.node.assignedTrackGroups = 1;
    directIdle.probe.reachable = true;
    directIdle.probe.protocolMatched = true;
    directIdle.probe.roundTripMs = 1.5;

    neuracoust::daw::RemoteDspNodeStatus offline;
    offline.node.id = "offline-c";
    offline.node.host = "10.10.3.2";
    offline.probe.reachable = false;

    const auto selection = neuracoust::daw::selectRemoteDspNodeForTrackGroup({slowShared, directIdle, offline});
    if (!selection.selected || selection.node.id != "direct-b") {
        std::cerr << "Remote DSP scheduler did not prefer the reachable lower-load direct node\n";
        return 10;
    }

    directIdle.node.assignedTrackGroups = directIdle.node.maxTrackGroups;
    const auto fallback = neuracoust::daw::selectRemoteDspNodeForTrackGroup({slowShared, directIdle});
    if (!fallback.selected || fallback.node.id != "shared-a") {
        std::cerr << "Remote DSP scheduler did not skip a saturated node\n";
        return 11;
    }

    auto fourKInsert = makeTrackInsert(
        "Neuracoust 4001E",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Newacoust4001E.vst3");
    const auto activeCapability = neuracoust::daw::remoteDspCapabilityForInsert(fourKInsert, true, true);
    if (activeCapability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        activeCapability.moduleId != "na.neuracoust.4001e") {
        std::cerr << "4001E insert should be marked as remote DSP active when the server is reachable\n";
        return 12;
    }
    fourKInsert.dspExecutionMode = "remote_internal";
    if (!neuracoust::daw::isRemoteInternalDspExecutionMode(fourKInsert.dspExecutionMode) ||
        !neuracoust::daw::isAnyInternalDspExecutionMode(fourKInsert.dspExecutionMode)) {
        std::cerr << "remote_internal should be classified as a remote internal DSP execution mode\n";
        return 29;
    }

    auto fourKTwoInsert = makeTrackInsert(
        "Neuracoust 4001-2 Channel Strip",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust 4001-2.vst3");
    const auto fourKTwoCapability = neuracoust::daw::remoteDspCapabilityForInsert(fourKTwoInsert, true, true);
    if (fourKTwoCapability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        fourKTwoCapability.moduleId != "na.neuracoust.4001e") {
        std::cerr << "4001-2 channel strip should route to the compatible 4001E RT module\n";
        return 20;
    }

    auto dm2cInsert = makeTrackInsert(
        "Neuracoust Compressor DM2C",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Compressor DM2C.vst3");
    const auto dm2cCapability = neuracoust::daw::remoteDspCapabilityForInsert(dm2cInsert, true, true);
    if (dm2cCapability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        dm2cCapability.moduleId != "na.neuracoust.compressor.dm2c") {
        std::cerr << "DM2C insert should route to the DM2C RT module when the server is reachable\n";
        return 21;
    }

    auto qf2dInsert = makeTrackInsert(
        "Neuracoust EQ QF2D",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust EQ QF2D.vst3");
    const auto qf2dCapability = neuracoust::daw::remoteDspCapabilityForInsert(qf2dInsert, true, true);
    if (qf2dCapability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        qf2dCapability.moduleId != "na.neuracoust.eq.qf2d") {
        std::cerr << "QF2D insert should route to the QF2D RT module when the server is reachable\n";
        return 22;
    }

    auto compressor99Insert = makeTrackInsert(
        "Neuracoust Compressor 99",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Compressor 99.vst3");
    const auto compressor99Capability = neuracoust::daw::remoteDspCapabilityForInsert(compressor99Insert, true, true);
    if (compressor99Capability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        compressor99Capability.moduleId != "na.neuracoust.compressor99") {
        std::cerr << "Compressor 99 insert should route to the Compressor 99 RT module when the server is reachable\n";
        return 24;
    }

    auto compressor201Insert = makeTrackInsert(
        "Neuracoust Compressor 201",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Compressor 201.vst3");
    const auto compressor201Capability = neuracoust::daw::remoteDspCapabilityForInsert(compressor201Insert, true, true);
    if (compressor201Capability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        compressor201Capability.moduleId != "na.neuracoust.compressor201") {
        std::cerr << "Compressor 201 insert should route to the Compressor 201 RT module when the server is reachable\n";
        return 25;
    }

    auto compLimiter340Insert = makeTrackInsert(
        "Neuracoust Comp Limiter 340",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Comp Limiter 340.vst3");
    const auto compLimiter340Capability = neuracoust::daw::remoteDspCapabilityForInsert(compLimiter340Insert, true, true);
    if (compLimiter340Capability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        compLimiter340Capability.moduleId != "na.neuracoust.comp-limiter-340") {
        std::cerr << "Comp Limiter 340 insert should route to the Comp Limiter 340 RT module when the server is reachable\n";
        return 26;
    }

    auto coAirInsert = makeTrackInsert(
        "Neuracoust CoAir2026",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/CoAir2026.vst3");
    const auto coAirCapability = neuracoust::daw::remoteDspCapabilityForInsert(coAirInsert, true, true);
    if (coAirCapability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        coAirCapability.moduleId != "na.neuracoust.coair2026") {
        std::cerr << "CoAir2026 insert should route to the CoAir2026 RT module when the server is reachable\n";
        return 22;
    }

    auto mirage991Insert = makeTrackInsert(
        "Neuracoust Mirage 991",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Mirage 991.vst3");
    const auto mirage991Capability = neuracoust::daw::remoteDspCapabilityForInsert(mirage991Insert, true, true);
    if (mirage991Capability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        mirage991Capability.moduleId != "na.neuracoust.mirage991") {
        std::cerr << "Mirage 991 insert should route to the Mirage 991 RT module when the server is reachable\n";
        return 23;
    }

    auto mirage8Insert = makeTrackInsert(
        "Neuracoust Mirage 8",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Mirage 8.vst3");
    const auto mirage8Capability = neuracoust::daw::remoteDspCapabilityForInsert(mirage8Insert, true, true);
    if (mirage8Capability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        mirage8Capability.moduleId != "na.neuracoust.mirage8") {
        std::cerr << "Mirage 8 insert should route to the Mirage 8 RT module when the server is reachable\n";
        return 24;
    }

    auto mirage901Insert = makeTrackInsert(
        "Neuracoust Mirage 901",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Mirage 901.vst3");
    const auto mirage901Capability = neuracoust::daw::remoteDspCapabilityForInsert(mirage901Insert, true, true);
    if (mirage901Capability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        mirage901Capability.moduleId != "na.neuracoust.mirage901") {
        std::cerr << "Mirage 901 insert should route to the Mirage 901 RT module when the server is reachable\n";
        return 28;
    }

    auto spaceSculptorInsert = makeTrackInsert(
        "Neuracoust Space Sculptor",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/Neuracoust Space Sculptor.vst3");
    const auto spaceSculptorCapability = neuracoust::daw::remoteDspCapabilityForInsert(spaceSculptorInsert, true, true);
    if (spaceSculptorCapability.mode != neuracoust::daw::RemoteDspInsertMode::RemoteActive ||
        spaceSculptorCapability.moduleId != "na.neuracoust.space-sculptor") {
        std::cerr << "Space Sculptor insert should route to the Space Sculptor RT module when the server is reachable\n";
        return 27;
    }

    auto thirdPartyInsert = makeTrackInsert(
        "FabFilter Pro-Q 4",
        "VST3",
        "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-Q 4.vst3");
    const auto thirdPartyCapability = neuracoust::daw::remoteDspCapabilityForInsert(thirdPartyInsert, true, true);
    if (thirdPartyCapability.mode != neuracoust::daw::RemoteDspInsertMode::LocalOnly) {
        std::cerr << "Third-party inserts must stay local-only for the Neuracoust DSP server\n";
        return 13;
    }

    return 0;
}
