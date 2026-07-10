#include "audio/AudioDeviceModel.h"
#include "audio/AsioRuntimeAdapter.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>
#include <windows.h>
#include <algorithm>
#include <string>

namespace neuracoust::daw {

namespace {

const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

template <typename T>
void releaseIfSet(T*& ptr) {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string wideToUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string deviceFriendlyName(IMMDevice* device) {
    IPropertyStore* properties = nullptr;
    if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return {};
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(properties->GetValue(kPkeyDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = wideToUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    properties->Release();
    return name;
}

void fillMixFormat(IMMDevice* device, AudioDeviceInfo& info, bool capture) {
    IAudioClient* client = nullptr;
    if (device == nullptr || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
        return;
    }

    WAVEFORMATEX* format = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&format)) && format != nullptr) {
        const int channels = std::max<int>(0, format->nChannels);
        if (capture) {
            info.inputChannels = channels;
        } else {
            info.outputChannels = channels;
        }
        if (format->nSamplesPerSec > 0) {
            info.defaultSampleRate = static_cast<double>(format->nSamplesPerSec);
        }
        CoTaskMemFree(format);
    }
    client->Release();
}

void appendDevices(IMMDeviceEnumerator* enumerator, EDataFlow flow, std::vector<AudioDeviceInfo>& devices) {
    IMMDeviceCollection* collection = nullptr;
    if (enumerator == nullptr || FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
        return;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(index, &device)) || device == nullptr) {
            continue;
        }

        LPWSTR rawId = nullptr;
        std::string id;
        if (SUCCEEDED(device->GetId(&rawId)) && rawId != nullptr) {
            id = wideToUtf8(rawId);
            CoTaskMemFree(rawId);
        }

        AudioDeviceInfo info;
        const bool capture = flow == eCapture;
        info.id = std::string(capture ? "wasapi-input:" : "wasapi-output:") + (id.empty() ? std::to_string(index) : id);
        info.name = deviceFriendlyName(device);
        if (info.name.empty()) {
            info.name = capture ? "Windows Audio Input" : "Windows Audio Output";
        }
        info.driver = AudioDriverKind::WASAPI;
        info.defaultBufferSize = 256;
        info.available = true;
        fillMixFormat(device, info, capture);
        devices.push_back(std::move(info));
        device->Release();
    }

    collection->Release();
}

void appendAsioRegistryDevices(std::vector<AudioDeviceInfo>& devices) {
    for (const auto& driver : enumerateAsioDriverRegistrations()) {
        const auto status = asioAdapterStatusForDeviceId(driver.deviceId);
        AudioDeviceInfo info;
        info.id = driver.deviceId;
        info.name = !driver.description.empty() ? driver.description : driver.driverName;
        if (info.name.empty()) {
            info.name = "ASIO Driver";
        }
        info.driver = AudioDriverKind::ASIO;
        info.inputChannels = 2;
        info.outputChannels = 2;
        info.defaultSampleRate = 48000.0;
        info.defaultBufferSize = 128;
        info.available = status.runtimeAdapterLinked && status.driverRegistered && status.comServerFound;
        info.diagnosticNote = status.diagnosticSummary.empty() ? status.message : status.diagnosticSummary;
        devices.push_back(std::move(info));
    }
}

} // namespace

std::vector<AudioDeviceInfo> enumerateAudioDevices() {
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        return {};
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    std::vector<AudioDeviceInfo> devices;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                   nullptr,
                                   CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator)))) {
        appendDevices(enumerator, eRender, devices);
        appendDevices(enumerator, eCapture, devices);
    }
    appendAsioRegistryDevices(devices);

    releaseIfSet(enumerator);
    if (shouldUninitialize) {
        CoUninitialize();
    }
    return devices;
}

} // namespace neuracoust::daw
#elif !defined(__APPLE__)
namespace neuracoust::daw {

std::vector<AudioDeviceInfo> enumerateAudioDevices() {
    return {};
}

} // namespace neuracoust::daw
#endif
