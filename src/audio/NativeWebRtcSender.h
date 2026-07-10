#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace neuracoust::daw {

struct NativeWebRtcSenderSettings {
    bool enabled = false;
    std::string sessionName = "mix";
    std::string relayHost = "127.0.0.1";
    std::string accessToken;
    std::string quality = "opus_high";
    std::string latencyMode = "stable";
    std::string stunUrl = "stun:stun.l.google.com:19302";
    std::string turnUrl;
    std::string turnUsername;
    std::string turnPassword;
    int relayHttpPort = 8787;
};

struct NativeWebRtcSenderStatus {
    bool enabled = false;
    bool available = false;
    bool connected = false;
    bool offerReady = false;
    uint64_t framesQueued = 0;
    uint64_t blocksQueued = 0;
    uint64_t packetsDropped = 0;
    uint64_t sendFailures = 0;
    std::string signalingUrl;
    std::string message;
};

class NativeWebRtcSender {
public:
    NativeWebRtcSender();
    ~NativeWebRtcSender();

    NativeWebRtcSender(const NativeWebRtcSender&) = delete;
    NativeWebRtcSender& operator=(const NativeWebRtcSender&) = delete;

    void configure(double sampleRate, NativeWebRtcSenderSettings settings);
    void stop();
    void pushInterleavedStereo(const float* samples, int64_t frameCount);
    NativeWebRtcSenderStatus status() const;

    static bool buildConfigured();

private:
    struct Impl;

    mutable std::mutex mutex_;
    NativeWebRtcSenderSettings settings_;
    double sampleRate_ = 48000.0;
    bool configured_ = false;
    std::atomic<uint64_t> framesQueued_ {0};
    std::atomic<uint64_t> blocksQueued_ {0};
    std::atomic<uint64_t> sendFailures_ {0};
    std::string message_ = "Native WebRTC sender idle.";
    std::unique_ptr<Impl> impl_;
};

NativeWebRtcSenderSettings nativeWebRtcSettingsFromListenRoom(const struct ListenRoomSettings& settings);

} // namespace neuracoust::daw
