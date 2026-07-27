#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/NativeWebRtcSender.h"

namespace neuracoust::daw {

struct ListenRoomSettings {
    bool enabled = false;
    std::string sessionName = "mix";
    std::string source = "monitor";
    std::string quality = "opus_high";
    std::string latencyMode = "stable";
    std::string transportMode = "direct_fallback";
    std::string relayHost = "127.0.0.1";
    std::string accessToken;
    int relayHttpPort = 8787;
    int relayTcpIngestPort = 8791;
};

struct ListenRoomStatus {
    bool enabled = false;
    bool senderRunning = false;
    bool relayReachable = false;
    uint32_t sessionId = 0;
    uint64_t packetsQueued = 0;
    uint64_t packetsSent = 0;
    uint64_t packetsDropped = 0;
    uint64_t sendFailures = 0;
    int queuedBlocks = 0;
    int maxQueuedBlocks = 0;
    int packetFrames = 0;
    int latencyTargetMs = 0;
    int targetBitrateKbps = 0;
    bool nativeWebRtcAvailable = false;
    bool nativeWebRtcActive = false;
    bool nativeWebRtcConnected = false;
    bool nativeWebRtcOfferReady = false;
    uint64_t nativeWebRtcFramesQueued = 0;
    uint64_t nativeWebRtcPacketsDropped = 0;
    uint64_t nativeWebRtcSendFailures = 0;
    std::string shareUrl;
    std::string nativeWebRtcSignalingUrl;
    std::string activeCodec;
    std::string qualityLabel;
    std::string transportMode;
    std::string message;
};

uint32_t listenRoomSessionId(const std::string& text);
std::string listenRoomShareUrl(const ListenRoomSettings& settings);
/// The best address a listener could reach the relay at: the default-route LAN IP,
/// else any private-range interface, else link-local, else loopback. An explicitly
/// set relayHost (e.g. a tunnel hostname) is used as-is.
std::string listenRoomShareHost(const ListenRoomSettings& settings);
std::string listenRoomPublicShareUrl(const ListenRoomSettings& settings);
std::string listenRoomEffectiveCodec(const ListenRoomSettings& settings);
std::string listenRoomQualityLabel(const ListenRoomSettings& settings);
int listenRoomTargetBitrateKbps(const ListenRoomSettings& settings);
int listenRoomLatencyTargetMs(const ListenRoomSettings& settings);
int listenRoomPacketFrames(const ListenRoomSettings& settings);
int listenRoomMaxQueuedBlocks(const ListenRoomSettings& settings);
ListenRoomSettings normalizedListenRoomSettings(ListenRoomSettings settings);

class ListenRoomSender {
public:
    ListenRoomSender();
    ~ListenRoomSender();

    ListenRoomSender(const ListenRoomSender&) = delete;
    ListenRoomSender& operator=(const ListenRoomSender&) = delete;

    void configure(double sampleRate, ListenRoomSettings settings);
    void stop();
    void pushInterleavedStereo(const float* samples, int64_t frameCount);
    ListenRoomStatus status() const;

private:
    struct Block {
        std::vector<float> samples;
        uint32_t sampleRateWire = 48000000;
    };

    void workerLoop();
    bool sendBlock(const Block& block);
    bool shouldUseRelayPathLocked() const;
    bool sendPcmBlock(const Block& block, const ListenRoomSettings& settings, uint32_t sequenceBase);
    bool sendOpusBlock(const Block& block, const ListenRoomSettings& settings);
    bool sendFramedPacket(const std::vector<uint8_t>& packet, const ListenRoomSettings& settings);
    void resetOpusEncoderLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    ListenRoomSettings settings_;
    /// The share URL is rebuilt only when the settings that shape it change. Building it resolves
    /// the machine's LAN address by opening a UDP socket, and status() is polled at ~30 Hz — doing
    /// that per poll cost as much CPU as the entire audio render.
    mutable std::string shareUrlCache_;
    mutable std::string shareUrlKey_;
    std::deque<Block> queue_;
    std::thread worker_;
    double sampleRate_ = 48000.0;
    bool stopRequested_ = false;
    bool configured_ = false;
    uint32_t sequence_ = 0;
    void* opusEncoder_ = nullptr;
    int opusBitrate_ = 0;
    std::vector<float> pendingOpusSamples_;
    NativeWebRtcSender nativeWebRtcSender_;
    std::atomic<bool> senderRunning_ {false};
    std::atomic<bool> relayReachable_ {false};
    std::atomic<uint64_t> packetsQueued_ {0};
    std::atomic<uint64_t> packetsSent_ {0};
    std::atomic<uint64_t> packetsDropped_ {0};
    std::atomic<uint64_t> sendFailures_ {0};
    std::string message_ = "Listen Room idle.";
};

} // namespace neuracoust::daw
