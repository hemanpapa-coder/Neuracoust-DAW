#include "audio/AudioInputRecorder.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "audio/RecordingTake.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace neuracoust::daw {

namespace {

template <typename T>
void releaseIfSet(T*& ptr) {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (count <= 1) {
        return {};
    }
    std::wstring result(static_cast<size_t>(count - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), count);
    return result;
}

std::string stripWasapiInputPrefix(const std::string& id) {
    constexpr const char* prefix = "wasapi-input:";
    return id.rfind(prefix, 0) == 0 ? id.substr(std::strlen(prefix)) : id;
}

std::string hresultMessage(const char* prefix, HRESULT hr) {
    return std::string(prefix) + " HRESULT=0x" + std::to_string(static_cast<unsigned long>(hr));
}

bool isFloatFormat(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

bool isPcmFormat(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    return false;
}

void appendSilence(std::vector<float>& out, uint32_t frames, int channels) {
    out.insert(out.end(), static_cast<size_t>(frames) * static_cast<size_t>(channels), 0.0f);
}

void appendFloat32(const BYTE* data, std::vector<float>& out, uint32_t frames, int channels) {
    const auto* samples = reinterpret_cast<const float*>(data);
    const auto count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    out.insert(out.end(), samples, samples + count);
}

void appendPcm16(const BYTE* data, std::vector<float>& out, uint32_t frames, int channels) {
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const auto count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    out.reserve(out.size() + count);
    for (size_t index = 0; index < count; ++index) {
        out.push_back(static_cast<float>(samples[index] / 32768.0f));
    }
}

void appendPcm24(const BYTE* data, std::vector<float>& out, uint32_t frames, int channels) {
    const auto count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    out.reserve(out.size() + count);
    for (size_t index = 0; index < count; ++index) {
        const auto offset = index * 3;
        int32_t value = int32_t(data[offset]) | (int32_t(data[offset + 1]) << 8) | (int32_t(data[offset + 2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= ~0x00ffffff;
        }
        out.push_back(static_cast<float>(value / 8388608.0f));
    }
}

void appendPcm32(const BYTE* data, std::vector<float>& out, uint32_t frames, int channels) {
    const auto* samples = reinterpret_cast<const int32_t*>(data);
    const auto count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    out.reserve(out.size() + count);
    for (size_t index = 0; index < count; ++index) {
        out.push_back(static_cast<float>(samples[index] / 2147483648.0));
    }
}

bool appendCaptureBuffer(const BYTE* data,
                         uint32_t frames,
                         DWORD flags,
                         const WAVEFORMATEX* format,
                         std::vector<float>& scratch) {
    if (format == nullptr || frames == 0 || format->nChannels == 0) {
        return false;
    }
    const int channels = static_cast<int>(format->nChannels);
    scratch.clear();
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        appendSilence(scratch, frames, channels);
        return true;
    }
    if (isFloatFormat(format) && format->wBitsPerSample == 32) {
        appendFloat32(data, scratch, frames, channels);
        return true;
    }
    if (isPcmFormat(format)) {
        if (format->wBitsPerSample == 16) {
            appendPcm16(data, scratch, frames, channels);
            return true;
        }
        if (format->wBitsPerSample == 24) {
            appendPcm24(data, scratch, frames, channels);
            return true;
        }
        if (format->wBitsPerSample == 32) {
            appendPcm32(data, scratch, frames, channels);
            return true;
        }
    }
    return false;
}

} // namespace

class AudioInputRecorder::Impl {
public:
    ~Impl() {
        std::string ignored;
        stop(ignored);
    }

    bool start(const std::string& outputPath, int sampleRate, int, const std::string& inputDeviceId, int bitDepth) {
        std::string ignored;
        stop(ignored);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = {};
            status_.outputPath = outputPath;
            status_.message = "Starting WASAPI input recording.";
            startupComplete_ = false;
            startupOk_ = false;
        }

        running_.store(true);
        worker_ = std::thread(&Impl::captureThread, this, outputPath, sampleRate, inputDeviceId, bitDepth);

        std::unique_lock<std::mutex> lock(mutex_);
        startupCv_.wait(lock, [&] { return startupComplete_; });
        return startupOk_;
    }

    bool stop(std::string& error) {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
        const auto copy = status();
        error = copy.message;
        return !copy.recording && !copy.outputPath.empty() && copy.message == "Recording saved.";
    }

    RecordingStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

private:
    void setStartupResult(bool ok, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        startupOk_ = ok;
        startupComplete_ = true;
        status_.recording = ok;
        status_.message = message;
        startupCv_.notify_all();
    }

    void updateDuration(double seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.durationSeconds = seconds;
    }

    void finishRecording(bool saved, const std::string& message, double duration) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.recording = false;
        status_.durationSeconds = duration;
        status_.message = message.empty() ? (saved ? "Recording saved." : "Recording failed.") : message;
    }

    void captureThread(std::string outputPath, int requestedSampleRate, std::string inputDeviceId, int bitDepth) {
        const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(initResult);
        if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
            setStartupResult(false, hresultMessage("Could not initialize COM for WASAPI input.", initResult));
            return;
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* captureClient = nullptr;
        WAVEFORMATEX* mixFormat = nullptr;
        std::unique_ptr<RecordingTake> take;
        std::vector<float> scratch;
        std::string finalMessage;
        bool saved = false;

        auto cleanup = [&] {
            if (client != nullptr) {
                client->Stop();
            }
            if (mixFormat != nullptr) {
                CoTaskMemFree(mixFormat);
                mixFormat = nullptr;
            }
            releaseIfSet(captureClient);
            releaseIfSet(client);
            releaseIfSet(device);
            releaseIfSet(enumerator);
            if (shouldUninitialize) {
                CoUninitialize();
            }
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                      nullptr,
                                      CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            setStartupResult(false, hresultMessage("Could not create WASAPI input enumerator.", hr));
            cleanup();
            return;
        }

        const auto requestedDeviceId = stripWasapiInputPrefix(inputDeviceId);
        if (!requestedDeviceId.empty()) {
            const auto wideId = utf8ToWide(requestedDeviceId);
            hr = enumerator->GetDevice(wideId.c_str(), &device);
        } else {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
        if (FAILED(hr) || device == nullptr) {
            setStartupResult(false, hresultMessage("No usable WASAPI input device is available.", hr));
            cleanup();
            return;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (FAILED(hr)) {
            setStartupResult(false, hresultMessage("Could not activate WASAPI input client.", hr));
            cleanup();
            return;
        }
        hr = client->GetMixFormat(&mixFormat);
        if (FAILED(hr) || mixFormat == nullptr) {
            setStartupResult(false, hresultMessage("Could not read WASAPI input mix format.", hr));
            cleanup();
            return;
        }

        const REFERENCE_TIME bufferDuration100ns = 1000000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                bufferDuration100ns,
                                0,
                                mixFormat,
                                nullptr);
        if (FAILED(hr)) {
            setStartupResult(false, hresultMessage("Could not initialize WASAPI shared input.", hr));
            cleanup();
            return;
        }
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr)) {
            setStartupResult(false, hresultMessage("Could not get WASAPI capture client.", hr));
            cleanup();
            return;
        }
        hr = client->Start();
        if (FAILED(hr)) {
            setStartupResult(false, hresultMessage("Could not start WASAPI input.", hr));
            cleanup();
            return;
        }

        const int channels = std::max<int>(1, mixFormat->nChannels);
        const int sampleRate = mixFormat->nSamplesPerSec > 0 ? static_cast<int>(mixFormat->nSamplesPerSec) : std::max(1, requestedSampleRate);
        take = std::make_unique<RecordingTake>(channels, sampleRate);
        setStartupResult(true, "Recording input.");

        while (running_.load()) {
            UINT32 packetFrames = 0;
            hr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                finalMessage = hresultMessage("Could not read WASAPI input packet size.", hr);
                break;
            }
            if (packetFrames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    finalMessage = hresultMessage("Could not read WASAPI input buffer.", hr);
                    running_.store(false);
                    break;
                }

                if (appendCaptureBuffer(data, frames, flags, mixFormat, scratch)) {
                    take->appendInterleavedFloat(scratch.data(), static_cast<int>(frames));
                    updateDuration(take->durationSeconds());
                } else {
                    finalMessage = "Unsupported WASAPI input sample format.";
                    running_.store(false);
                }

                captureClient->ReleaseBuffer(frames);
                if (!running_.load()) {
                    break;
                }
                hr = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(hr)) {
                    finalMessage = hresultMessage("Could not advance WASAPI input packet.", hr);
                    running_.store(false);
                    break;
                }
            }
        }

        const double duration = take ? take->durationSeconds() : 0.0;
        if (take && duration > 0.0) {
            std::string error;
            saved = take->saveWav(outputPath, bitDepth, error);
            if (!saved && finalMessage.empty()) {
                finalMessage = error;
            }
        } else if (finalMessage.empty()) {
            finalMessage = "Recording stopped without captured audio.";
        }
        cleanup();
        finishRecording(saved, saved ? "Recording saved." : finalMessage, duration);
    }

    mutable std::mutex mutex_;
    std::condition_variable startupCv_;
    RecordingStatus status_;
    std::atomic<bool> running_ {false};
    std::thread worker_;
    bool startupComplete_ = false;
    bool startupOk_ = false;
};

AudioInputRecorder::AudioInputRecorder() : impl_(std::make_unique<Impl>()) {}
AudioInputRecorder::~AudioInputRecorder() = default;
bool AudioInputRecorder::start(const std::string& outputPath, int sampleRate, int channels, const std::string& inputDeviceId, int bitDepth) { return impl_->start(outputPath, sampleRate, channels, inputDeviceId, bitDepth); }
bool AudioInputRecorder::stop(std::string& error) { return impl_->stop(error); }
RecordingStatus AudioInputRecorder::status() const { return impl_->status(); }

} // namespace neuracoust::daw

#elif !defined(__APPLE__)

namespace neuracoust::daw {

class AudioInputRecorder::Impl {
public:
    bool start(const std::string& outputPath, int, int, const std::string&, int) {
        status_.outputPath = outputPath;
        status_.message = "Input recording is not implemented on this platform yet.";
        return false;
    }

    bool stop(std::string& error) {
        error = status_.message;
        status_.recording = false;
        return false;
    }

    RecordingStatus status() const { return status_; }

private:
    RecordingStatus status_;
};

AudioInputRecorder::AudioInputRecorder() : impl_(std::make_unique<Impl>()) {}
AudioInputRecorder::~AudioInputRecorder() = default;
bool AudioInputRecorder::start(const std::string& outputPath, int sampleRate, int channels, const std::string& inputDeviceId, int bitDepth) { return impl_->start(outputPath, sampleRate, channels, inputDeviceId, bitDepth); }
bool AudioInputRecorder::stop(std::string& error) { return impl_->stop(error); }
RecordingStatus AudioInputRecorder::status() const { return impl_->status(); }

} // namespace neuracoust::daw

#endif
