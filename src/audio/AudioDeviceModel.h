#pragma once

#include <string>
#include <vector>

namespace neuracoust::daw {

enum class AudioDriverKind {
    CoreAudio,
    WASAPI,
    ASIO,
    Unknown
};

struct AudioDeviceInfo {
    /// Stable identity persisted by the UI: the CoreAudio device UID, which survives
    /// re-plug / re-enumeration (unlike the ephemeral numeric AudioObjectID). Falls
    /// back to the numeric id only when a device exposes no UID.
    std::string id;
    /// The CoreAudio device UID (same value as id on macOS; kept explicit for clarity).
    std::string uid;
    std::string name;
    AudioDriverKind driver = AudioDriverKind::Unknown;
    int inputChannels = 0;
    int outputChannels = 0;
    double defaultSampleRate = 48000.0;
    int defaultBufferSize = 256;
    bool available = false;
    std::string diagnosticNote;
};

struct AudioDriverSupport {
    AudioDriverKind driver = AudioDriverKind::Unknown;
    bool targetSupported = false;
    bool availableOnThisPlatform = false;
    bool requiresExternalSdk = false;
    std::string note;
};

std::vector<AudioDeviceInfo> enumerateAudioDevices();
std::vector<AudioDriverSupport> supportedAudioDrivers();
std::string audioDriverName(AudioDriverKind driver);

} // namespace neuracoust::daw
