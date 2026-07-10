#include "audio/NativeWebRtcSender.h"

#include "audio/ListenRoom.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_remote_description_observer_interface.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neuracoust::daw {

namespace {

constexpr uint32_t kListenMagic = 0x4e4c5354u;
constexpr uint8_t kListenVersion = 1u;
constexpr uint8_t kListenChannels = 2u;
constexpr size_t kListenHeaderSize = 20u;
constexpr size_t kMaxNativeQueuedPackets = 96u;

std::string nativeSignalingUrl(const NativeWebRtcSenderSettings& settings) {
    std::string url = "http://" + settings.relayHost + ":" + std::to_string(std::clamp(settings.relayHttpPort, 1, 65535)) +
        "/api/native-webrtc/offer?session=" + settings.sessionName +
        "&quality=" + settings.quality +
        "&latency=" + settings.latencyMode;
    if (!settings.accessToken.empty()) {
        url += "&token=" + settings.accessToken;
    }
    return url;
}

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

std::string urlEncode(const std::string& text) {
    std::ostringstream out;
    static constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char ch : text) {
        const bool safe = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_' ||
            ch == '.';
        if (safe) {
            out << static_cast<char>(ch);
        } else {
            out << '%' << hex[(ch >> 4u) & 0xfu] << hex[ch & 0xfu];
        }
    }
    return out.str();
}

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16u);
    for (const unsigned char ch : text) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch < 0x20u ? ' ' : static_cast<char>(ch);
            break;
        }
    }
    return out;
}

std::string environmentText(const char* name) {
    if (const char* value = std::getenv(name)) {
        return std::string(value);
    }
    return {};
}

std::optional<std::string> jsonStringValue(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find('"', pos + 1u);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::string out;
    bool escaping = false;
    for (++pos; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escaping) {
            if (ch == 'n') {
                out += '\n';
            } else if (ch == 'r') {
                out += '\r';
            } else if (ch == 't') {
                out += '\t';
            } else {
                out += ch;
            }
            escaping = false;
        } else if (ch == '\\') {
            escaping = true;
        } else if (ch == '"') {
            return out;
        } else {
            out += ch;
        }
    }
    return std::nullopt;
}

struct HttpResponse {
    int status = 0;
    std::string body;
};

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

bool sendAll(SocketHandle socketHandle, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        const int result = send(socketHandle, data + sent, static_cast<int>(std::min<size_t>(size - sent, 32768u)), 0);
#else
        const ssize_t result = send(socketHandle, data + sent, std::min<size_t>(size - sent, 32768u), 0);
#endif
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

std::optional<HttpResponse> httpRequest(const std::string& host, int port, const std::string& method, const std::string& path, const std::string& body, const std::string& token) {
#ifdef _WIN32
    static WinsockSession winsock;
    if (!winsock.ready) {
        return std::nullopt;
    }
#endif
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(std::clamp(port, 1, 65535));
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) {
        return std::nullopt;
    }

    SocketHandle socketHandle = kInvalidSocket;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        socketHandle = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (socketHandle == kInvalidSocket) {
            continue;
        }
        if (connect(socketHandle, item->ai_addr, static_cast<socklen_t>(item->ai_addrlen)) == 0) {
            break;
        }
        closeSocketHandle(socketHandle);
        socketHandle = kInvalidSocket;
    }
    freeaddrinfo(result);
    if (socketHandle == kInvalidSocket) {
        return std::nullopt;
    }

    std::string request;
    request += method + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + portText + "\r\n";
    request += "Connection: close\r\n";
    request += "Cache-Control: no-store\r\n";
    if (!token.empty()) {
        request += "X-Listen-Token: " + token + "\r\n";
    }
    if (method == "POST") {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    if (!sendAll(socketHandle, request.data(), request.size())) {
        closeSocketHandle(socketHandle);
        return std::nullopt;
    }

    std::string response;
    char buffer[8192];
    for (;;) {
#ifdef _WIN32
        const int received = recv(socketHandle, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        const ssize_t received = recv(socketHandle, buffer, sizeof(buffer), 0);
#endif
        if (received <= 0) {
            break;
        }
        response.append(buffer, buffer + static_cast<size_t>(received));
        if (response.size() > 512u * 1024u) {
            break;
        }
    }
    closeSocketHandle(socketHandle);

    const size_t statusStart = response.find(' ');
    const size_t statusEnd = statusStart == std::string::npos ? std::string::npos : response.find(' ', statusStart + 1u);
    if (statusStart == std::string::npos || statusEnd == std::string::npos) {
        return std::nullopt;
    }
    HttpResponse parsed;
    parsed.status = std::atoi(response.substr(statusStart + 1u, statusEnd - statusStart - 1u).c_str());
    const size_t bodyStart = response.find("\r\n\r\n");
    parsed.body = bodyStart == std::string::npos ? std::string() : response.substr(bodyStart + 4u);
    return parsed;
}

std::vector<uint8_t> buildPcmPacket(const std::vector<float>& samples, uint32_t sampleRateWire, uint32_t sessionId, uint32_t sequence) {
    const auto frameCount = static_cast<uint16_t>(std::min<size_t>(samples.size() / 2u, 65535u));
    std::vector<uint8_t> packet;
    packet.reserve(kListenHeaderSize + static_cast<size_t>(frameCount) * 2u * sizeof(float));
    appendLe32(packet, kListenMagic);
    packet.push_back(kListenVersion);
    packet.push_back(kListenChannels);
    appendLe16(packet, frameCount);
    appendLe32(packet, sequence);
    appendLe32(packet, sampleRateWire);
    appendLe32(packet, sessionId);
    const auto* bytes = reinterpret_cast<const uint8_t*>(samples.data());
    packet.insert(packet.end(), bytes, bytes + static_cast<size_t>(frameCount) * 2u * sizeof(float));
    return packet;
}

#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
std::string rtcMessage(webrtc::RTCError error) {
    const char* message = error.message();
    return message != nullptr ? std::string(message) : std::string();
}
#endif

} // namespace

struct NativeWebRtcSender::Impl {
    ~Impl() { stop(); }

    void configure(double sampleRate, NativeWebRtcSenderSettings settings) {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings_ = std::move(settings);
            sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
            stopRequested_ = false;
            connected_ = false;
            offerReady_ = false;
            message_ = "Native WebRTC sender starting.";
        }
#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
        worker_ = std::thread(&Impl::workerLoop, this);
#else
        setMessage("Native WebRTC unavailable in this build.");
#endif
    }

    void stop() {
        std::unique_lock<std::mutex> lock(mutex);
        stopRequested_ = true;
        condition.notify_all();
        auto worker = std::move(worker_);
        lock.unlock();
        if (worker.joinable()) {
            worker.join();
        }
#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
        closePeerConnection();
#endif
        lock.lock();
        pendingPackets_.clear();
        connected_ = false;
        offerReady_ = false;
        message_ = "Native WebRTC sender idle.";
    }

    bool enqueue(const float* samples, int64_t frameCount) {
        if (samples == nullptr || frameCount <= 0) {
            return true;
        }
        std::vector<float> block(samples, samples + static_cast<size_t>(frameCount) * 2u);
        std::lock_guard<std::mutex> lock(mutex);
        const uint32_t sampleRateWire = static_cast<uint32_t>(sampleRate_ * 1000.0 + 0.5);
        auto packet = buildPcmPacket(block, sampleRateWire, listenRoomSessionId(settings_.sessionName), sequence_++);
        if (pendingPackets_.size() >= kMaxNativeQueuedPackets) {
            pendingPackets_.pop_front();
            ++packetsDropped_;
        }
        pendingPackets_.push_back(std::move(packet));
        condition.notify_all();
        return true;
    }

    bool connected() const {
        std::lock_guard<std::mutex> lock(mutex);
        return connected_;
    }

    bool offerReady() const {
        std::lock_guard<std::mutex> lock(mutex);
        return offerReady_;
    }

    std::string message() const {
        std::lock_guard<std::mutex> lock(mutex);
        return message_;
    }

    uint64_t packetsDropped() const {
        std::lock_guard<std::mutex> lock(mutex);
        return packetsDropped_;
    }

private:
    void setMessage(std::string message) {
        std::lock_guard<std::mutex> lock(mutex);
        message_ = std::move(message);
        condition.notify_all();
    }

    bool stopRequested() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stopRequested_;
    }

#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
    struct PeerObserver final : public webrtc::PeerConnectionObserver {
        explicit PeerObserver(Impl& owner) : owner_(owner) {}
        void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
        void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
        void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override { owner_.onIceGatheringChange(state); }
        void OnIceCandidate(const webrtc::IceCandidate*) override {}
        void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override { owner_.onConnectionChange(state); }
    private:
        Impl& owner_;
    };

    struct ChannelObserver final : public webrtc::DataChannelObserver {
        explicit ChannelObserver(Impl& owner) : owner_(owner) {}
        void OnStateChange() override { owner_.onDataChannelState(); }
        void OnMessage(const webrtc::DataBuffer&) override {}
    private:
        Impl& owner_;
    };

    struct OfferObserver : public webrtc::CreateSessionDescriptionObserver {
        explicit OfferObserver(Impl& owner) : owner_(owner) {}
        void OnSuccess(webrtc::SessionDescriptionInterface* desc) override { owner_.onOffer(desc); }
        void OnFailure(webrtc::RTCError error) override { owner_.onOfferFailure(rtcMessage(std::move(error))); }
    private:
        Impl& owner_;
    };

    struct SetLocalObserver : public webrtc::SetSessionDescriptionObserver {
        explicit SetLocalObserver(Impl& owner) : owner_(owner) {}
        void OnSuccess() override { owner_.onLocalDescriptionSet(); }
        void OnFailure(webrtc::RTCError error) override { owner_.onOfferFailure(rtcMessage(std::move(error))); }
    private:
        Impl& owner_;
    };

    struct SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
        explicit SetRemoteObserver(Impl& owner) : owner_(owner) {}
        void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override { owner_.onRemoteDescriptionSet(std::move(error)); }
    private:
        Impl& owner_;
    };

    void workerLoop() {
        static std::once_flag sslOnce;
        std::call_once(sslOnce, [] { webrtc::InitializeSSL(); });
        while (!stopRequested()) {
            if (startPeerConnection() && publishOfferAndWaitForAnswer()) {
                sendLoop();
            }
            closePeerConnection();
            if (!stopRequested()) {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(900), [&] { return stopRequested_; });
            }
        }
    }

    bool startPeerConnection() {
        NativeWebRtcSenderSettings settings;
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings = settings_;
            message_ = "Native WebRTC creating PeerConnection.";
            offerReady_ = false;
            connected_ = false;
            localDescriptionReady_ = false;
            iceGatheringComplete_ = false;
            remoteDescriptionComplete_ = false;
            remoteDescriptionOk_ = false;
            offerSdp_.clear();
            offerError_.clear();
        }

        networkThread_ = webrtc::Thread::CreateWithSocketServer();
        workerThread_ = webrtc::Thread::Create();
        signalingThread_ = webrtc::Thread::Create();
        if (!networkThread_ || !workerThread_ || !signalingThread_ ||
            !networkThread_->Start() || !workerThread_->Start() || !signalingThread_->Start()) {
            setMessage("Native WebRTC failed to start libwebrtc threads.");
            return false;
        }

        factory_ = webrtc::CreatePeerConnectionFactory(networkThread_.get(),
                                                       workerThread_.get(),
                                                       signalingThread_.get(),
                                                       nullptr,
                                                       webrtc::CreateBuiltinAudioEncoderFactory(),
                                                       webrtc::CreateBuiltinAudioDecoderFactory(),
                                                       nullptr,
                                                       nullptr,
                                                       nullptr,
                                                       nullptr);
        if (!factory_) {
            setMessage("Native WebRTC failed to create PeerConnectionFactory.");
            return false;
        }

        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        if (!settings.stunUrl.empty()) {
            webrtc::PeerConnectionInterface::IceServer stun;
            stun.urls.push_back(settings.stunUrl);
            config.servers.push_back(stun);
        }
        if (!settings.turnUrl.empty()) {
            webrtc::PeerConnectionInterface::IceServer turn;
            turn.urls.push_back(settings.turnUrl);
            turn.username = settings.turnUsername;
            turn.password = settings.turnPassword;
            config.servers.push_back(turn);
        }

        peerObserver_ = std::make_unique<PeerObserver>(*this);
        webrtc::PeerConnectionDependencies deps(peerObserver_.get());
        auto pcOrError = factory_->CreatePeerConnectionOrError(config, std::move(deps));
        if (!pcOrError.ok()) {
            setMessage("Native WebRTC failed to create PeerConnection: " + rtcMessage(pcOrError.error()));
            return false;
        }
        peerConnection_ = pcOrError.MoveValue();

        webrtc::DataChannelInit dataConfig;
        dataConfig.ordered = true;
        dataConfig.protocol = "neuracoust-listen";
        auto channelOrError = peerConnection_->CreateDataChannelOrError("listen-audio", &dataConfig);
        if (!channelOrError.ok()) {
            setMessage("Native WebRTC failed to create DataChannel: " + rtcMessage(channelOrError.error()));
            return false;
        }
        channelObserver_ = std::make_unique<ChannelObserver>(*this);
        dataChannel_ = channelOrError.MoveValue();
        dataChannel_->RegisterObserver(channelObserver_.get());

        peerConnection_->CreateOffer(webrtc::make_ref_counted<OfferObserver>(*this).get(),
                                     webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());

        std::unique_lock<std::mutex> lock(mutex);
        condition.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::seconds(8), [&] {
            return stopRequested_ || localDescriptionReady_ || !offerError_.empty();
        });
        if (stopRequested_) {
            return false;
        }
        if (!offerError_.empty()) {
            message_ = "Native WebRTC offer failed: " + offerError_;
            return false;
        }
        condition.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::seconds(3), [&] {
            return stopRequested_ || iceGatheringComplete_;
        });
        if (const auto* local = peerConnection_->local_description()) {
            local->ToString(&offerSdp_);
        }
        if (offerSdp_.empty()) {
            message_ = "Native WebRTC local offer SDP is empty.";
            return false;
        }
        message_ = "Native WebRTC offer ready.";
        return true;
    }

    bool publishOfferAndWaitForAnswer() {
        NativeWebRtcSenderSettings settings;
        std::string offerSdp;
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings = settings_;
            offerSdp = offerSdp_;
        }
        std::string path = "/api/native-webrtc/offer?session=" + urlEncode(settings.sessionName) +
            "&quality=" + urlEncode(settings.quality) +
            "&latency=" + urlEncode(settings.latencyMode);
        if (!settings.accessToken.empty()) {
            path += "&token=" + urlEncode(settings.accessToken);
        }
        const std::string body = "{\"type\":\"offer\",\"sdp\":\"" + jsonEscape(offerSdp) +
            "\",\"quality\":\"" + jsonEscape(settings.quality) +
            "\",\"latency\":\"" + jsonEscape(settings.latencyMode) + "\"}";

        const auto posted = httpRequest(settings.relayHost, settings.relayHttpPort, "POST", path, body, settings.accessToken);
        if (!posted || posted->status < 200 || posted->status >= 300) {
            setMessage("Native WebRTC offer POST failed.");
            return false;
        }
        const auto offerId = jsonStringValue(posted->body, "id");
        if (!offerId || offerId->empty()) {
            setMessage("Native WebRTC relay did not return an offer id.");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            offerId_ = *offerId;
            offerReady_ = true;
            message_ = "Native WebRTC offer published; waiting for browser answer.";
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (!stopRequested() && std::chrono::steady_clock::now() < deadline) {
            std::string answerPath = "/api/native-webrtc/answer?id=" + urlEncode(*offerId);
            if (!settings.accessToken.empty()) {
                answerPath += "&token=" + urlEncode(settings.accessToken);
            }
            const auto answer = httpRequest(settings.relayHost, settings.relayHttpPort, "GET", answerPath, "", settings.accessToken);
            if (answer && answer->status == 200) {
                const auto sdp = jsonStringValue(answer->body, "sdp");
                const auto type = jsonStringValue(answer->body, "type");
                if (sdp && type && *type == "answer") {
                    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, *sdp);
                    if (!desc) {
                        setMessage("Native WebRTC answer SDP parse failed.");
                        return false;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        remoteDescriptionComplete_ = false;
                        remoteDescriptionOk_ = false;
                    }
                    peerConnection_->SetRemoteDescription(std::move(desc), webrtc::make_ref_counted<SetRemoteObserver>(*this));
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait_for(lock, std::chrono::seconds(5), [&] { return stopRequested_ || remoteDescriptionComplete_; });
                    if (remoteDescriptionOk_) {
                        message_ = "Native WebRTC answer applied; waiting for DataChannel.";
                        return true;
                    }
                    message_ = "Native WebRTC answer rejected.";
                    return false;
                }
            }
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait_for(lock, std::chrono::milliseconds(250), [&] { return stopRequested_; });
        }
        setMessage("Native WebRTC answer timed out.");
        return false;
    }

    void sendLoop() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopRequested_) {
            condition.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return stopRequested_ ||
                    (dataChannel_ && dataChannel_->state() == webrtc::DataChannelInterface::kOpen && !pendingPackets_.empty());
            });
            if (stopRequested_) {
                break;
            }
            if (!dataChannel_ || dataChannel_->state() != webrtc::DataChannelInterface::kOpen) {
                continue;
            }
            auto packet = std::move(pendingPackets_.front());
            pendingPackets_.pop_front();
            auto channel = dataChannel_;
            lock.unlock();
            webrtc::CopyOnWriteBuffer buffer(packet.data(), packet.size());
            const bool ok = channel->Send(webrtc::DataBuffer(buffer, true));
            lock.lock();
            if (!ok) {
                pendingPackets_.push_front(std::move(packet));
                message_ = "Native WebRTC DataChannel send backpressure.";
                condition.wait_for(lock, std::chrono::milliseconds(60), [&] { return stopRequested_; });
            } else {
                message_ = "Native WebRTC streaming over DataChannel.";
            }
        }
    }

    void closePeerConnection() {
        webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;
        webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
        std::unique_ptr<ChannelObserver> channelObserver;
        std::unique_ptr<PeerObserver> peerObserver;
        std::unique_ptr<webrtc::Thread> signalingThread;
        std::unique_ptr<webrtc::Thread> workerThread;
        std::unique_ptr<webrtc::Thread> networkThread;
        {
            std::lock_guard<std::mutex> lock(mutex);
            dataChannel = std::move(dataChannel_);
            peerConnection = std::move(peerConnection_);
            factory = std::move(factory_);
            channelObserver = std::move(channelObserver_);
            peerObserver = std::move(peerObserver_);
            signalingThread = std::move(signalingThread_);
            workerThread = std::move(workerThread_);
            networkThread = std::move(networkThread_);
            connected_ = false;
            offerReady_ = false;
        }
        if (dataChannel) {
            dataChannel->UnregisterObserver();
        }
        if (peerConnection) {
            peerConnection->Close();
        }
        dataChannel = nullptr;
        peerConnection = nullptr;
        factory = nullptr;
        channelObserver.reset();
        peerObserver.reset();
        signalingThread.reset();
        workerThread.reset();
        networkThread.reset();
    }

    void onOffer(webrtc::SessionDescriptionInterface* desc) {
        if (desc == nullptr || !peerConnection_) {
            onOfferFailure("empty offer");
            return;
        }
        peerConnection_->SetLocalDescription(webrtc::make_ref_counted<SetLocalObserver>(*this).get(), desc);
    }

    void onOfferFailure(std::string error) {
        std::lock_guard<std::mutex> lock(mutex);
        offerError_ = std::move(error);
        condition.notify_all();
    }

    void onLocalDescriptionSet() {
        std::lock_guard<std::mutex> lock(mutex);
        localDescriptionReady_ = true;
        condition.notify_all();
    }

    void onRemoteDescriptionSet(webrtc::RTCError error) {
        std::lock_guard<std::mutex> lock(mutex);
        remoteDescriptionComplete_ = true;
        remoteDescriptionOk_ = error.ok();
        if (!error.ok()) {
            message_ = "Native WebRTC remote description failed: " + rtcMessage(std::move(error));
        }
        condition.notify_all();
    }

    void onIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
            iceGatheringComplete_ = true;
            condition.notify_all();
        }
    }

    void onConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
        std::lock_guard<std::mutex> lock(mutex);
        connected_ = state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected;
        condition.notify_all();
    }

    void onDataChannelState() {
        std::lock_guard<std::mutex> lock(mutex);
        if (dataChannel_ && dataChannel_->state() == webrtc::DataChannelInterface::kOpen) {
            connected_ = true;
            message_ = "Native WebRTC DataChannel open.";
        }
        condition.notify_all();
    }

    bool localDescriptionReady_ = false;
    bool iceGatheringComplete_ = false;
    bool remoteDescriptionComplete_ = false;
    bool remoteDescriptionOk_ = false;
    std::string offerSdp_;
    std::string offerError_;
    std::string offerId_;
    std::unique_ptr<webrtc::Thread> networkThread_;
    std::unique_ptr<webrtc::Thread> workerThread_;
    std::unique_ptr<webrtc::Thread> signalingThread_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel_;
    std::unique_ptr<PeerObserver> peerObserver_;
    std::unique_ptr<ChannelObserver> channelObserver_;
#endif

    mutable std::mutex mutex;
    std::condition_variable condition;
    NativeWebRtcSenderSettings settings_;
    double sampleRate_ = 48000.0;
    bool stopRequested_ = false;
    bool connected_ = false;
    bool offerReady_ = false;
    uint32_t sequence_ = 0;
    std::string message_ = "Native WebRTC sender idle.";
    std::deque<std::vector<uint8_t>> pendingPackets_;
    uint64_t packetsDropped_ = 0;
    std::thread worker_;
};

NativeWebRtcSender::NativeWebRtcSender() = default;

NativeWebRtcSender::~NativeWebRtcSender() {
    stop();
}

bool NativeWebRtcSender::buildConfigured() {
#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
    return true;
#else
    return false;
#endif
}

void NativeWebRtcSender::configure(double sampleRate, NativeWebRtcSenderSettings settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = std::move(settings);
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    configured_ = settings_.enabled;
    framesQueued_.store(0);
    blocksQueued_.store(0);
    sendFailures_.store(0);
#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
    if (configured_) {
        if (!impl_) {
            impl_ = std::make_unique<Impl>();
        }
        impl_->configure(sampleRate_, settings_);
        message_ = "Native WebRTC sender starting.";
    } else {
        if (impl_) {
            impl_->stop();
        }
        message_ = "Native WebRTC sender disabled.";
    }
#else
    message_ = configured_
        ? "Native WebRTC unavailable in this build; using Listen relay fallback when allowed."
        : "Native WebRTC sender disabled.";
#endif
}

void NativeWebRtcSender::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_) {
        impl_->stop();
    }
    configured_ = false;
    message_ = "Native WebRTC sender idle.";
}

void NativeWebRtcSender::pushInterleavedStereo(const float* samples, int64_t frameCount) {
    if (samples == nullptr || frameCount <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        return;
    }
    blocksQueued_.fetch_add(1);
    framesQueued_.fetch_add(static_cast<uint64_t>(frameCount));
    if (!buildConfigured() || !impl_ || !impl_->enqueue(samples, frameCount)) {
        sendFailures_.fetch_add(1);
    }
}

NativeWebRtcSenderStatus NativeWebRtcSender::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    NativeWebRtcSenderStatus status;
    status.enabled = settings_.enabled;
    status.available = buildConfigured();
    status.connected = impl_ != nullptr && impl_->connected();
    status.offerReady = impl_ != nullptr ? impl_->offerReady() : (buildConfigured() && configured_);
    status.framesQueued = framesQueued_.load();
    status.blocksQueued = blocksQueued_.load();
    status.packetsDropped = impl_ != nullptr ? impl_->packetsDropped() : 0;
    status.sendFailures = sendFailures_.load();
    status.signalingUrl = nativeSignalingUrl(settings_);
    status.message = impl_ != nullptr ? impl_->message() : message_;
    return status;
}

NativeWebRtcSenderSettings nativeWebRtcSettingsFromListenRoom(const ListenRoomSettings& settings) {
    const auto safe = normalizedListenRoomSettings(settings);
    NativeWebRtcSenderSettings native;
    native.enabled = safe.enabled;
    native.sessionName = safe.sessionName;
    native.relayHost = safe.relayHost;
    native.accessToken = safe.accessToken;
    native.quality = safe.quality;
    native.latencyMode = safe.latencyMode;
    native.relayHttpPort = safe.relayHttpPort;
    if (const auto stunUrl = environmentText("NEURACOUST_LISTEN_STUN_URL"); !stunUrl.empty()) {
        native.stunUrl = stunUrl;
    }
    native.turnUrl = environmentText("NEURACOUST_LISTEN_TURN_URL");
    native.turnUsername = environmentText("NEURACOUST_LISTEN_TURN_USERNAME");
    native.turnPassword = environmentText("NEURACOUST_LISTEN_TURN_PASSWORD");
    return native;
}

} // namespace neuracoust::daw
