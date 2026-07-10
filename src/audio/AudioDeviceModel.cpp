#include "audio/AudioDeviceModel.h"

namespace neuracoust::daw {

std::string audioDriverName(AudioDriverKind driver) {
    switch (driver) {
        case AudioDriverKind::CoreAudio: return "Core Audio";
        case AudioDriverKind::WASAPI: return "WASAPI";
        case AudioDriverKind::ASIO: return "ASIO";
        case AudioDriverKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::vector<AudioDriverSupport> supportedAudioDrivers() {
    return {
        {
            AudioDriverKind::CoreAudio,
            true,
#if defined(__APPLE__)
            true,
#else
            false,
#endif
            false,
            "Native macOS low-latency audio driver."
        },
        {
            AudioDriverKind::WASAPI,
            true,
#if defined(_WIN32)
            true,
#else
            false,
#endif
            false,
            "Native Windows shared/exclusive audio driver target."
        },
        {
            AudioDriverKind::ASIO,
            true,
#if defined(_WIN32)
            true,
#else
            false,
#endif
            true,
            "Optional low-latency ASIO driver target; requires an ASIO SDK or runtime adapter for this platform."
        }
    };
}

} // namespace neuracoust::daw
