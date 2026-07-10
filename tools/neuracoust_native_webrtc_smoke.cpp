#include "audio/ListenRoom.h"
#include "audio/NativeWebRtcSender.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#endif

int main(int argc, char** argv) {
    bool requireAvailable = false;
    int waitOfferReadyMs = 0;
    int streamMs = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index] != nullptr ? argv[index] : "";
        if (arg == "--require-available") {
            requireAvailable = true;
        } else if (arg == "--wait-offer-ready-ms" && index + 1 < argc) {
            waitOfferReadyMs = std::max(0, std::atoi(argv[++index]));
        } else if (arg == "--stream-ms" && index + 1 < argc) {
            streamMs = std::max(0, std::atoi(argv[++index]));
        }
    }

    neuracoust::daw::ListenRoomSettings listen;
    listen.enabled = true;
    listen.sessionName = "native-smoke";
    listen.transportMode = "native_webrtc";
    listen.quality = "opus_high";
    listen.latencyMode = "low";
    listen.relayHost = "127.0.0.1";
    listen.accessToken = "native-token";

    auto settings = neuracoust::daw::nativeWebRtcSettingsFromListenRoom(listen);
    neuracoust::daw::NativeWebRtcSender sender;
    sender.configure(44100.0, settings);
    std::vector<float> block(128 * 2, 0.01f);
    sender.pushInterleavedStereo(block.data(), 128);

    auto status = sender.status();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitOfferReadyMs);
    while (waitOfferReadyMs > 0 && !status.offerReady && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        status = sender.status();
    }
    assert(status.enabled);
    assert(status.signalingUrl.find("/api/native-webrtc/offer") != std::string::npos);
    assert(status.signalingUrl.find("native-smoke") != std::string::npos);
    assert(status.framesQueued == 128);
    assert(status.blocksQueued == 1);
    if (waitOfferReadyMs > 0 && !status.offerReady) {
        std::cerr << "Native WebRTC offer was not published before timeout: " << status.message << "\n";
        return 3;
    }

    const auto streamDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(streamMs);
    while (streamMs > 0 && std::chrono::steady_clock::now() < streamDeadline) {
        sender.pushInterleavedStereo(block.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        status = sender.status();
    }

    if (requireAvailable && !status.available) {
        std::cerr << "Native WebRTC was required but this build has no libwebrtc implementation.\n";
        return 2;
    }

#if defined(NEURACOUST_DAW_HAS_NATIVE_WEBRTC)
    auto audioEncoderFactory = webrtc::CreateBuiltinAudioEncoderFactory();
    auto audioDecoderFactory = webrtc::CreateBuiltinAudioDecoderFactory();
    assert(audioEncoderFactory);
    assert(audioDecoderFactory);
#endif

    std::cout << "Native WebRTC smoke: available=" << (status.available ? "true" : "false")
              << " url=" << status.signalingUrl << "\n";
    return 0;
}
