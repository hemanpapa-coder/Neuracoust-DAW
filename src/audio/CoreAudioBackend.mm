#include "audio/AudioDeviceModel.h"

#if defined(__APPLE__)
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

namespace neuracoust::daw {

static std::string cfStringToStd(CFStringRef value) {
    if (value == nullptr) {
        return {};
    }
    char buffer[512] = {};
    if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return buffer;
    }
    return {};
}

static UInt32 countChannels(AudioObjectID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyStreamConfiguration,
        scope,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    std::vector<std::byte> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) {
        return 0;
    }
    UInt32 channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        channels += list->mBuffers[i].mNumberChannels;
    }
    return channels;
}

std::vector<AudioDeviceInfo> enumerateAudioDevices() {
    AudioObjectPropertyAddress devicesAddress {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &size) != noErr || size == 0) {
        return {};
    }

    std::vector<AudioObjectID> deviceIds(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &size, deviceIds.data()) != noErr) {
        return {};
    }

    std::vector<AudioDeviceInfo> devices;
    for (auto deviceId : deviceIds) {
        CFStringRef name = nullptr;
        UInt32 nameSize = sizeof(name);
        AudioObjectPropertyAddress nameAddress {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(deviceId, &nameAddress, 0, nullptr, &nameSize, &name);

        Float64 sampleRate = 48000.0;
        UInt32 sampleRateSize = sizeof(sampleRate);
        AudioObjectPropertyAddress sampleRateAddress {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(deviceId, &sampleRateAddress, 0, nullptr, &sampleRateSize, &sampleRate);

        AudioDeviceInfo info;
        info.id = std::to_string(deviceId);
        info.name = cfStringToStd(name);
        info.driver = AudioDriverKind::CoreAudio;
        info.inputChannels = static_cast<int>(countChannels(deviceId, kAudioDevicePropertyScopeInput));
        info.outputChannels = static_cast<int>(countChannels(deviceId, kAudioDevicePropertyScopeOutput));
        info.defaultSampleRate = sampleRate;
        info.defaultBufferSize = 256;
        info.available = true;
        if (!info.name.empty() && (info.inputChannels > 0 || info.outputChannels > 0)) {
            devices.push_back(info);
        }
        if (name != nullptr) {
            CFRelease(name);
        }
    }
    return devices;
}

} // namespace neuracoust::daw
#endif
