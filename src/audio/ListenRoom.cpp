#include "audio/ListenRoom.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>

#if defined(NEURACOUST_DAW_HAS_OPUS)
#if __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neuracoust::daw {

namespace {

constexpr uint32_t kListenMagic = 0x4e4c5354u;
constexpr uint8_t kListenVersion = 1u;
constexpr uint8_t kListenVersionV2 = 2u;
constexpr uint8_t kListenChannels = 2u;
constexpr uint8_t kListenCodecOpus = 1u;
constexpr size_t kListenHeaderSize = 20u;
constexpr size_t kListenHeaderSizeV2 = 24u;
constexpr int kOpusFrameSamples = 960;
constexpr int kMaxOpusPayloadBytes = 1476;
constexpr int kDefaultMaxQueuedBlocks = 16;
constexpr int kDefaultFramesPerPacket = 184; // Keeps stable-mode v1 PCM packets near Ethernet MTU.

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

struct WinsockSession {
    WinsockSession() { ready = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinsockSession() {
        if (ready) {
            WSACleanup();
        }
    }
    WSADATA data {};
    bool ready = false;
};

void closeSocketHandle(SocketHandle socketHandle) {
    closesocket(socketHandle);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void closeSocketHandle(SocketHandle socketHandle) {
    close(socketHandle);
}
#endif

void appendLe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void appendLe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

bool sendAll(SocketHandle socketHandle, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        const int result = send(socketHandle,
                                reinterpret_cast<const char*>(data + sent),
                                static_cast<int>(std::min<size_t>(size - sent, 32768u)),
                                0);
#else
        const ssize_t result = send(socketHandle,
                                    data + sent,
                                    std::min<size_t>(size - sent, 32768u),
                                    0);
#endif
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool listenRoomWantsOpus(const ListenRoomSettings& settings) {
    return settings.quality == "opus_balanced" ||
        settings.quality == "opus_high" ||
        settings.quality == "opus_max";
}

std::vector<int16_t> floatToPcm16(const float* samples, size_t count) {
    std::vector<int16_t> out(count);
    for (size_t index = 0; index < count; ++index) {
        const float clamped = std::max(-1.0f, std::min(1.0f, samples[index]));
        out[index] = static_cast<int16_t>(clamped >= 0.0f ? clamped * 32767.0f + 0.5f : clamped * 32768.0f - 0.5f);
    }
    return out;
}

std::vector<float> resampleInterleavedStereoLinear(const std::vector<float>& input, double sourceRate, double targetRate) {
    const size_t inputFrames = input.size() / 2u;
    if (inputFrames == 0 || sourceRate <= 1000.0 || targetRate <= 1000.0) {
        return {};
    }
    if (std::abs(sourceRate - targetRate) < 1.0) {
        return input;
    }

    const size_t outputFrames = std::max<size_t>(1u, static_cast<size_t>(std::llround(static_cast<double>(inputFrames) * targetRate / sourceRate)));
    std::vector<float> output(outputFrames * 2u, 0.0f);
    const double sourceStep = sourceRate / targetRate;
    for (size_t frame = 0; frame < outputFrames; ++frame) {
        const double sourcePosition = static_cast<double>(frame) * sourceStep;
        const size_t leftFrame = std::min(inputFrames - 1u, static_cast<size_t>(sourcePosition));
        const size_t rightFrame = std::min(inputFrames - 1u, leftFrame + 1u);
        const float fraction = static_cast<float>(sourcePosition - static_cast<double>(leftFrame));
        for (size_t channel = 0; channel < 2u; ++channel) {
            const float a = input[leftFrame * 2u + channel];
            const float b = input[rightFrame * 2u + channel];
            output[frame * 2u + channel] = a + (b - a) * fraction;
        }
    }
    return output;
}

} // namespace

uint32_t listenRoomSessionId(const std::string& text) {
    uint32_t hash = 2166136261u;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

std::string listenRoomShareUrl(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    return listenRoomPublicShareUrl(safe);
}

namespace {

bool isLoopbackOrEmptyHost(const std::string& host) {
    return host.empty() || host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool isPrivateIPv4(const std::string& ip) {
    // The ranges a listener on the same WiFi would actually be able to reach.
    return ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0 ||
           ip.rfind("172.16.", 0) == 0 || ip.rfind("172.17.", 0) == 0 ||
           ip.rfind("172.18.", 0) == 0 || ip.rfind("172.19.", 0) == 0 ||
           ip.rfind("172.2", 0) == 0 || ip.rfind("172.30.", 0) == 0 ||
           ip.rfind("172.31.", 0) == 0;
}

/// The IPv4 the machine would use to reach the network, found by asking the routing
/// table which local address a socket bound for the gateway would get. No packet is
/// sent — connect on a UDP socket only picks the route. This lands on the interface
/// that carries the default route, not whichever getifaddrs lists first (which is
/// often a disconnected link-local one).
std::string outboundRouteIPv4() {
#if !defined(_WIN32)
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return {};
    }
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(9);  // discard; nothing is sent
    // A routable public address, only so the kernel picks the default-route interface.
    ::inet_pton(AF_INET, "203.0.113.1", &probe.sin_addr);
    std::string result;
    if (::connect(sock, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
            char buffer[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, &local.sin_addr, buffer, sizeof(buffer)) != nullptr) {
                result = buffer;
            }
        }
    }
    ::close(sock);
    if (isPrivateIPv4(result)) {
        return result;
    }
#endif
    return {};
}

/// Falls back to scanning every interface for a private-range address when there is
/// no default route (e.g. an isolated switch), then to link-local, then to nothing.
std::string scanInterfacesIPv4() {
#if !defined(_WIN32)
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        return {};
    }
    std::string privateHit;
    std::string linkLocalHit;
    for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char buffer[INET_ADDRSTRLEN] = {0};
        auto* addr = reinterpret_cast<sockaddr_in*>(entry->ifa_addr);
        if (::inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) == nullptr) {
            continue;
        }
        const std::string ip = buffer;
        if (isPrivateIPv4(ip) && privateHit.empty()) {
            privateHit = ip;
        } else if (ip.rfind("169.254.", 0) == 0 && linkLocalHit.empty()) {
            linkLocalHit = ip;
        }
    }
    ::freeifaddrs(interfaces);
    if (!privateHit.empty()) {
        return privateHit;
    }
    return linkLocalHit;
#else
    return {};
#endif
}

} // namespace

std::string listenRoomShareHost(const ListenRoomSettings& settings) {
    // An explicitly configured host (a tunnel address, a hostname) wins.
    if (!isLoopbackOrEmptyHost(settings.relayHost)) {
        return settings.relayHost;
    }
    if (std::string routed = outboundRouteIPv4(); !routed.empty()) {
        return routed;
    }
    if (std::string scanned = scanInterfacesIPv4(); !scanned.empty()) {
        return scanned;
    }
    // Nothing reachable: loopback, which at least works on this machine.
    return "127.0.0.1";
}

std::string listenRoomPublicShareUrl(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    // The listener's URL must point at an address they can reach, not the loopback
    // the relay ingests on.
    std::string url = "http://" + listenRoomShareHost(safe) + ":" + std::to_string(safe.relayHttpPort) +
        "/?session=" + safe.sessionName +
        "&quality=" + safe.quality +
        "&latency=" + safe.latencyMode +
        "&transport=" + safe.transportMode +
        "&connect=" + (safe.transportMode == "relay" ? std::string("server") : std::string("direct"));
    if (!safe.accessToken.empty()) {
        url += "&token=" + safe.accessToken;
    }
    return url;
}

std::string listenRoomEffectiveCodec(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    if (safe.transportMode == "native_webrtc") {
        return NativeWebRtcSender::buildConfigured() ? "Native WebRTC Opus" : "Native WebRTC unavailable";
    }
    if (safe.quality == "pcm_lossless") {
        return "PCM float32";
    }
#if defined(NEURACOUST_DAW_HAS_OPUS)
    return "Opus";
#else
    return "PCM float32 relay ingest";
#endif
}

std::string listenRoomQualityLabel(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    if (safe.quality == "pcm_lossless") {
        return "Lossless PCM";
    }
    if (safe.quality == "opus_balanced") {
        return "Opus Balanced target";
    }
    if (safe.quality == "opus_max") {
        return "Opus Max target";
    }
    return "Opus High target";
}

int listenRoomTargetBitrateKbps(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    if (safe.quality == "opus_balanced") {
        return 128;
    }
    if (safe.quality == "opus_high") {
        return 192;
    }
    if (safe.quality == "opus_max") {
        return 256;
    }
    return 0;
}

int listenRoomLatencyTargetMs(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    if (safe.latencyMode == "low") {
        return 120;
    }
    if (safe.latencyMode == "video_sync") {
        return 800;
    }
    return 350;
}

int listenRoomPacketFrames(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    if (safe.latencyMode == "low") {
        return 96;
    }
    if (safe.latencyMode == "video_sync") {
        return 480;
    }
    return kDefaultFramesPerPacket;
}

int listenRoomMaxQueuedBlocks(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    if (safe.latencyMode == "low") {
        return 4;
    }
    if (safe.latencyMode == "video_sync") {
        return 32;
    }
    return kDefaultMaxQueuedBlocks;
}

ListenRoomSettings normalizedListenRoomSettings(ListenRoomSettings settings) {
    if (settings.sessionName.empty()) {
        settings.sessionName = "mix";
    }
    for (auto& ch : settings.sessionName) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_';
        if (!ok) {
            ch = '-';
        }
    }
    for (auto& ch : settings.accessToken) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_';
        if (!ok) {
            ch = '-';
        }
    }
    if (settings.source != "mix" && settings.source != "monitor") {
        settings.source = "monitor";
    }
    if (settings.quality != "pcm_lossless" &&
        settings.quality != "opus_balanced" &&
        settings.quality != "opus_high" &&
        settings.quality != "opus_max") {
        settings.quality = "opus_high";
    }
    if (settings.latencyMode != "low" &&
        settings.latencyMode != "stable" &&
        settings.latencyMode != "video_sync") {
        settings.latencyMode = "stable";
    }
    if (settings.transportMode != "direct" &&
        settings.transportMode != "relay" &&
        settings.transportMode != "native_webrtc" &&
        settings.transportMode != "direct_fallback") {
        settings.transportMode = "direct_fallback";
    }
    if (settings.relayHost.empty()) {
        settings.relayHost = "127.0.0.1";
    }
    settings.relayHttpPort = std::clamp(settings.relayHttpPort, 1, 65535);
    settings.relayTcpIngestPort = std::clamp(settings.relayTcpIngestPort, 1, 65535);
    return settings;
}

ListenRoomSender::ListenRoomSender() = default;

ListenRoomSender::~ListenRoomSender() {
    stop();
    resetOpusEncoderLocked();
}

void ListenRoomSender::configure(double sampleRate, ListenRoomSettings settings) {
    settings = normalizedListenRoomSettings(std::move(settings));
    std::unique_lock<std::mutex> lock(mutex_);
    const bool wasRunning = worker_.joinable();
    if (wasRunning) {
        stopRequested_ = true;
        condition_.notify_all();
        auto worker = std::move(worker_);
        lock.unlock();
        worker.join();
        lock.lock();
    }

    settings_ = std::move(settings);
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    queue_.clear();
    stopRequested_ = false;
    configured_ = settings_.enabled;
    sequence_ = 0;
    pendingOpusSamples_.clear();
    resetOpusEncoderLocked();
    auto nativeSettings = nativeWebRtcSettingsFromListenRoom(settings_);
    nativeSettings.enabled = configured_ && (settings_.transportMode == "native_webrtc" || settings_.transportMode == "direct_fallback");
    nativeWebRtcSender_.configure(sampleRate_, nativeSettings);
#if defined(NEURACOUST_DAW_HAS_OPUS)
    if (configured_ && listenRoomWantsOpus(settings_)) {
        int error = OPUS_OK;
        opusEncoder_ = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
        if (opusEncoder_ != nullptr && error == OPUS_OK) {
            opusBitrate_ = std::max(64000, listenRoomTargetBitrateKbps(settings_) * 1000);
            opus_encoder_ctl(static_cast<OpusEncoder*>(opusEncoder_), OPUS_SET_BITRATE(opusBitrate_));
            opus_encoder_ctl(static_cast<OpusEncoder*>(opusEncoder_), OPUS_SET_COMPLEXITY(5));
            opus_encoder_ctl(static_cast<OpusEncoder*>(opusEncoder_), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        } else {
            opusEncoder_ = nullptr;
            opusBitrate_ = 0;
        }
    }
#endif
    relayReachable_.store(false);
    message_ = configured_ ? "Listen Room sender armed." : "Listen Room disabled.";
    if (configured_) {
        worker_ = std::thread(&ListenRoomSender::workerLoop, this);
    }
}

void ListenRoomSender::stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (worker_.joinable()) {
        stopRequested_ = true;
        condition_.notify_all();
        auto worker = std::move(worker_);
        lock.unlock();
        worker.join();
        lock.lock();
    }
    queue_.clear();
    pendingOpusSamples_.clear();
    nativeWebRtcSender_.stop();
    configured_ = false;
    stopRequested_ = false;
    senderRunning_.store(false);
    relayReachable_.store(false);
    message_ = "Listen Room idle.";
}

void ListenRoomSender::resetOpusEncoderLocked() {
#if defined(NEURACOUST_DAW_HAS_OPUS)
    if (opusEncoder_ != nullptr) {
        opus_encoder_destroy(static_cast<OpusEncoder*>(opusEncoder_));
        opusEncoder_ = nullptr;
    }
#else
    opusEncoder_ = nullptr;
#endif
    opusBitrate_ = 0;
}

void ListenRoomSender::pushInterleavedStereo(const float* samples, int64_t frameCount) {
    if (samples == nullptr || frameCount <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        return;
    }
    const size_t maxQueuedBlocks = static_cast<size_t>(std::max(1, listenRoomMaxQueuedBlocks(settings_)));
    if (queue_.size() >= maxQueuedBlocks) {
        queue_.pop_front();
        packetsDropped_.fetch_add(1);
    }
    Block block;
    block.sampleRateWire = static_cast<uint32_t>(sampleRate_ * 1000.0 + 0.5);
    block.samples.assign(samples, samples + static_cast<size_t>(frameCount) * 2u);
    queue_.push_back(std::move(block));
    packetsQueued_.fetch_add(1);
    condition_.notify_one();
}

ListenRoomStatus ListenRoomSender::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ListenRoomStatus status;
    status.enabled = settings_.enabled;
    status.senderRunning = senderRunning_.load();
    status.relayReachable = relayReachable_.load();
    status.sessionId = listenRoomSessionId(settings_.sessionName);
    status.packetsQueued = packetsQueued_.load();
    status.packetsSent = packetsSent_.load();
    status.packetsDropped = packetsDropped_.load();
    status.sendFailures = sendFailures_.load();
    status.queuedBlocks = static_cast<int>(queue_.size());
    status.maxQueuedBlocks = listenRoomMaxQueuedBlocks(settings_);
    status.packetFrames = listenRoomPacketFrames(settings_);
    status.latencyTargetMs = listenRoomLatencyTargetMs(settings_);
    status.targetBitrateKbps = listenRoomTargetBitrateKbps(settings_);
    const auto nativeStatus = nativeWebRtcSender_.status();
    status.nativeWebRtcAvailable = nativeStatus.available;
    status.nativeWebRtcActive = nativeStatus.enabled && nativeStatus.available;
    status.nativeWebRtcConnected = nativeStatus.connected;
    status.nativeWebRtcOfferReady = nativeStatus.offerReady;
    status.nativeWebRtcFramesQueued = nativeStatus.framesQueued;
    status.nativeWebRtcPacketsDropped = nativeStatus.packetsDropped;
    status.nativeWebRtcSendFailures = nativeStatus.sendFailures;
    if ((settings_.transportMode == "native_webrtc" || settings_.transportMode == "direct_fallback") &&
        status.nativeWebRtcFramesQueued == 0 &&
        !queue_.empty()) {
        status.nativeWebRtcFramesQueued = static_cast<uint64_t>(queue_.size()) *
            static_cast<uint64_t>(std::max(1, status.packetFrames));
    }
    if ((settings_.transportMode == "native_webrtc" || settings_.transportMode == "direct_fallback") &&
        status.nativeWebRtcFramesQueued == 0 &&
        status.packetsQueued > 0) {
        status.nativeWebRtcFramesQueued = status.packetsQueued *
            static_cast<uint64_t>(std::max(1, status.packetFrames));
    }
    status.shareUrl = listenRoomPublicShareUrl(settings_);
    status.nativeWebRtcSignalingUrl = nativeStatus.signalingUrl;
    status.activeCodec = listenRoomEffectiveCodec(settings_);
    status.qualityLabel = listenRoomQualityLabel(settings_);
    status.transportMode = settings_.transportMode;
    status.message = message_;
    return status;
}

void ListenRoomSender::workerLoop() {
    senderRunning_.store(true);
    for (;;) {
        Block block;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return stopRequested_ || !queue_.empty(); });
            if (stopRequested_ && queue_.empty()) {
                break;
            }
            block = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!sendBlock(block)) {
            sendFailures_.fetch_add(1);
        }
    }
    senderRunning_.store(false);
}

bool ListenRoomSender::sendBlock(const Block& block) {
#ifdef _WIN32
    static WinsockSession winsock;
    if (!winsock.ready) {
        return false;
    }
#endif
    ListenRoomSettings settings;
    uint32_t sequenceBase = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
        sequenceBase = sequence_;
        if (settings.transportMode == "native_webrtc" || settings.transportMode == "direct_fallback") {
            nativeWebRtcSender_.pushInterleavedStereo(block.samples.data(), static_cast<int64_t>(block.samples.size() / 2u));
        }
    }

    if (settings.transportMode == "native_webrtc") {
        const auto nativeStatus = nativeWebRtcSender_.status();
        const bool ok = nativeStatus.available;
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = nativeStatus.message.empty()
            ? (ok ? "Listen Room native WebRTC sender ready." : "Listen Room native WebRTC sender unavailable.")
            : nativeStatus.message;
        return ok;
    }

    const bool ok = listenRoomWantsOpus(settings)
        ? sendOpusBlock(block, settings)
        : sendPcmBlock(block, settings, sequenceBase);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = ok ? "Listen Room streaming to relay." : "Listen Room relay ingest failed.";
    }
    return ok;
}

bool ListenRoomSender::sendFramedPacket(const std::vector<uint8_t>& packet, const ListenRoomSettings& settings) {
    SocketHandle socketHandle = socket(AF_INET, SOCK_STREAM, 0);
    if (socketHandle == kInvalidSocket) {
        relayReachable_.store(false);
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(settings.relayTcpIngestPort));
    if (inet_pton(AF_INET, settings.relayHost.c_str(), &address.sin_addr) != 1) {
        closeSocketHandle(socketHandle);
        relayReachable_.store(false);
        return false;
    }

    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocketHandle(socketHandle);
        relayReachable_.store(false);
        return false;
    }
    relayReachable_.store(true);

    std::vector<uint8_t> framed;
    appendLe32(framed, static_cast<uint32_t>(packet.size()));
    framed.insert(framed.end(), packet.begin(), packet.end());
    const bool ok = sendAll(socketHandle, framed.data(), framed.size());
    closeSocketHandle(socketHandle);
    return ok;
}

bool ListenRoomSender::sendPcmBlock(const Block& block, const ListenRoomSettings& settings, uint32_t sequenceBase) {
    const int frameCount = static_cast<int>(block.samples.size() / 2u);
    bool ok = true;
    const int maxFramesPerPacket = std::max(1, listenRoomPacketFrames(settings));
    for (int offset = 0; offset < frameCount && ok; offset += maxFramesPerPacket) {
        const int frames = std::min(maxFramesPerPacket, frameCount - offset);
        std::vector<uint8_t> packet;
        packet.reserve(kListenHeaderSize + static_cast<size_t>(frames) * 2u * sizeof(float));
        appendLe32(packet, kListenMagic);
        packet.push_back(kListenVersion);
        packet.push_back(kListenChannels);
        appendLe16(packet, static_cast<uint16_t>(frames));
        appendLe32(packet, sequenceBase++);
        appendLe32(packet, block.sampleRateWire);
        appendLe32(packet, listenRoomSessionId(settings.sessionName));
        const auto* bytes = reinterpret_cast<const uint8_t*>(block.samples.data() + static_cast<size_t>(offset) * 2u);
        packet.insert(packet.end(), bytes, bytes + static_cast<size_t>(frames) * 2u * sizeof(float));
        ok = sendFramedPacket(packet, settings);
        if (ok) {
            packetsSent_.fetch_add(1);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sequence_ = sequenceBase;
    }
    return ok;
}

bool ListenRoomSender::sendOpusBlock(const Block& block, const ListenRoomSettings& settings) {
#if defined(NEURACOUST_DAW_HAS_OPUS)
    OpusEncoder* encoder = nullptr;
    uint32_t fallbackSequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        encoder = static_cast<OpusEncoder*>(opusEncoder_);
        fallbackSequence = sequence_;
        if (encoder == nullptr) {
            encoder = nullptr;
        } else {
            const double blockRate = static_cast<double>(block.sampleRateWire) / 1000.0;
            const auto opusSamples = resampleInterleavedStereoLinear(block.samples, blockRate, 48000.0);
            pendingOpusSamples_.insert(pendingOpusSamples_.end(), opusSamples.begin(), opusSamples.end());
            if (pendingOpusSamples_.size() < static_cast<size_t>(kOpusFrameSamples) * 2u) {
                message_ = "Listen Room Opus buffering.";
                return true;
            }
        }
    }
    if (encoder == nullptr) {
        return sendPcmBlock(block, settings, fallbackSequence);
    }

    bool ok = true;
    for (;;) {
        std::vector<float> frame;
        uint32_t sequence = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingOpusSamples_.size() < static_cast<size_t>(kOpusFrameSamples) * 2u) {
                break;
            }
            frame.assign(pendingOpusSamples_.begin(), pendingOpusSamples_.begin() + static_cast<size_t>(kOpusFrameSamples) * 2u);
            pendingOpusSamples_.erase(pendingOpusSamples_.begin(), pendingOpusSamples_.begin() + static_cast<size_t>(kOpusFrameSamples) * 2u);
            sequence = sequence_++;
        }

        const auto pcm16 = floatToPcm16(frame.data(), frame.size());
        std::vector<uint8_t> opusPayload(static_cast<size_t>(kMaxOpusPayloadBytes));
        const int opusBytes = opus_encode(encoder,
                                          pcm16.data(),
                                          kOpusFrameSamples,
                                          opusPayload.data(),
                                          kMaxOpusPayloadBytes);
        if (opusBytes <= 0) {
            return false;
        }
        opusPayload.resize(static_cast<size_t>(opusBytes));

        std::vector<uint8_t> packet;
        packet.reserve(kListenHeaderSizeV2 + opusPayload.size());
        appendLe32(packet, kListenMagic);
        packet.push_back(kListenVersionV2);
        packet.push_back(kListenCodecOpus);
        packet.push_back(kListenChannels);
        packet.push_back(0u);
        appendLe16(packet, static_cast<uint16_t>(kOpusFrameSamples));
        appendLe16(packet, static_cast<uint16_t>(opusPayload.size()));
        appendLe32(packet, sequence);
        appendLe32(packet, 48000000u);
        appendLe32(packet, listenRoomSessionId(settings.sessionName));
        packet.insert(packet.end(), opusPayload.begin(), opusPayload.end());
        ok = sendFramedPacket(packet, settings);
        if (ok) {
            packetsSent_.fetch_add(1);
        } else {
            break;
        }
    }
    return ok;
#else
    return sendPcmBlock(block, settings, sequence_);
#endif
}

} // namespace neuracoust::daw
