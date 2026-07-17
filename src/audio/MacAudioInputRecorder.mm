#include "audio/AudioInputRecorder.h"
#include "audio/RecordingTake.h"

#if defined(__APPLE__)
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

namespace neuracoust::daw {

class AudioInputRecorder::Impl {
public:
    ~Impl() {
        std::string error;
        stop(error);
    }

    bool start(const std::string& outputPath, int sampleRate, int channels, const std::string& inputDeviceId, int bitDepth) {
        std::string error;
        stop(error);
        if (outputPath.empty()) {
            setStoppedStatus(outputPath, "Recording output path is empty.", 0.0);
            return false;
        }
        if (sampleRate <= 0 || channels <= 0) {
            setStoppedStatus(outputPath, "Recording sample rate or channel count is invalid.", 0.0);
            return false;
        }

        outputPath_ = outputPath;
        bitDepth_ = bitDepth;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            take_ = std::make_unique<RecordingTake>(channels, sampleRate);
            status_ = {};
            status_.outputPath = outputPath_;
            status_.message = "Starting Core Audio input recording.";
        }

        format_ = {};
        format_.mSampleRate = sampleRate;
        format_.mFormatID = kAudioFormatLinearPCM;
        format_.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        format_.mBytesPerPacket = static_cast<UInt32>(channels * sizeof(int16_t));
        format_.mFramesPerPacket = 1;
        format_.mBytesPerFrame = static_cast<UInt32>(channels * sizeof(int16_t));
        format_.mChannelsPerFrame = static_cast<UInt32>(channels);
        format_.mBitsPerChannel = 16;

        const OSStatus queueStatus = AudioQueueNewInput(&format_, &Impl::inputCallback, this, nullptr, nullptr, 0, &queue_);
        if (queueStatus != noErr || queue_ == nullptr) {
            setStoppedStatus(outputPath_, "Could not create Core Audio input queue. Check microphone permission and input device availability.", 0.0);
            return false;
        }
        CFStringRef inputUid = deviceUidFromId(inputDeviceId);
        if (inputUid != nullptr) {
            AudioQueueSetProperty(queue_, kAudioQueueProperty_CurrentDevice, &inputUid, sizeof(inputUid));
            CFRelease(inputUid);
        }

        const UInt32 framesPerBuffer = static_cast<UInt32>(std::max(1, sampleRate / 20));
        const UInt32 bufferBytes = framesPerBuffer * format_.mBytesPerFrame;
        for (auto& buffer : buffers_) {
            if (AudioQueueAllocateBuffer(queue_, bufferBytes, &buffer) != noErr) {
                setStoppedStatus(outputPath_, "Could not allocate input recording buffer.", 0.0);
                cleanupQueue();
                return false;
            }
            AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
        }

        const OSStatus startStatus = AudioQueueStart(queue_, nullptr);
        if (startStatus != noErr) {
            setStoppedStatus(outputPath_, "Could not start Core Audio input recording.", 0.0);
            cleanupQueue();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.recording = true;
            status_.outputPath = outputPath_;
            status_.message = "Recording input.";
        }
        return true;
    }

    bool stop(std::string& error) {
        if (queue_ != nullptr) {
            AudioQueueStop(queue_, true);
            cleanupQueue();
        }

        std::unique_ptr<RecordingTake> takeToSave;
        std::string pathToSave;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!status_.recording && take_ == nullptr) {
                error = status_.message;
                return true;
            }
            status_.recording = false;
            pathToSave = outputPath_;
            takeToSave = std::move(take_);
        }

        if (takeToSave == nullptr || pathToSave.empty()) {
            setStoppedStatus(pathToSave, "Recording stopped without captured audio.", 0.0);
            error = "Recording stopped without captured audio.";
            return true;
        }

        const double duration = takeToSave->durationSeconds();
        if (duration <= 0.0) {
            setStoppedStatus(pathToSave, "Recording stopped without captured audio.", 0.0);
            error = "Recording stopped without captured audio.";
            return false;
        }
        if (!takeToSave->saveWav(pathToSave, bitDepth_, error)) {
            setStoppedStatus(pathToSave, error, duration);
            return false;
        }

        setStoppedStatus(pathToSave, "Recording saved.", duration);
        return true;
    }

    RecordingStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto copy = status_;
        if (take_ != nullptr && copy.recording) {
            copy.durationSeconds = take_->durationSeconds();
        }
        return copy;
    }

private:
    void setStoppedStatus(const std::string& outputPath, const std::string& message, double durationSeconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.recording = false;
        status_.outputPath = outputPath;
        status_.durationSeconds = durationSeconds;
        status_.message = message;
        if (message != "Recording saved.") {
            take_.reset();
        }
    }

    static CFStringRef deviceUidFromId(const std::string& inputDeviceId) {
        if (inputDeviceId.empty()) {
            return nullptr;
        }
        // Stored identity is a stable UID; resolve it (or a legacy numeric id) to the
        // current AudioObjectID, then read the device's UID back for the AudioQueue.
        AudioObjectID device = kAudioObjectUnknown;
        CFStringRef cfUid = CFStringCreateWithCString(nullptr, inputDeviceId.c_str(), kCFStringEncodingUTF8);
        if (cfUid != nullptr) {
            AudioValueTranslation translation {};
            translation.mInputData = &cfUid;
            translation.mInputDataSize = sizeof(cfUid);
            translation.mOutputData = &device;
            translation.mOutputDataSize = sizeof(device);
            UInt32 tsize = sizeof(translation);
            AudioObjectPropertyAddress taddr {
                kAudioHardwarePropertyDeviceForUID,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &taddr, 0, nullptr, &tsize, &translation) != noErr) {
                device = kAudioObjectUnknown;
            }
            CFRelease(cfUid);
        }
        if (device == kAudioObjectUnknown) {
            char* end = nullptr;
            const auto parsed = std::strtoul(inputDeviceId.c_str(), &end, 10);
            if (end == inputDeviceId.c_str() || parsed == 0) {
                return nullptr;
            }
            device = static_cast<AudioObjectID>(parsed);
        }
        CFStringRef uid = nullptr;
        UInt32 size = sizeof(uid);
        AudioObjectPropertyAddress address {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &uid) != noErr || uid == nullptr) {
            return nullptr;
        }
        return uid;
    }

    static void inputCallback(void* userData,
                              AudioQueueRef queue,
                              AudioQueueBufferRef buffer,
                              const AudioTimeStamp* startTime,
                              UInt32 packetCount,
                              const AudioStreamPacketDescription* packetDescriptions) {
        (void)startTime;
        (void)packetDescriptions;
        auto* self = static_cast<Impl*>(userData);
        self->handleInput(queue, buffer, packetCount);
    }

    void handleInput(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 packetCount) {
        if (buffer == nullptr) {
            return;
        }
        const auto frames = packetCount > 0
            ? static_cast<int>(packetCount)
            : static_cast<int>(buffer->mAudioDataByteSize / format_.mBytesPerFrame);
        bool shouldReenqueue = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_.recording && take_ != nullptr && frames > 0) {
                take_->appendInterleavedInt16(static_cast<const int16_t*>(buffer->mAudioData), frames);
                status_.durationSeconds = take_->durationSeconds();
                shouldReenqueue = true;
            }
        }
        if (shouldReenqueue) {
            AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        }
    }

    void cleanupQueue() {
        if (queue_ != nullptr) {
            AudioQueueDispose(queue_, true);
            queue_ = nullptr;
        }
        for (auto& buffer : buffers_) {
            buffer = nullptr;
        }
    }

    AudioStreamBasicDescription format_ {};
    AudioQueueRef queue_ = nullptr;
    AudioQueueBufferRef buffers_[3] {};
    std::unique_ptr<RecordingTake> take_;
    mutable std::mutex mutex_;
    RecordingStatus status_;
    std::string outputPath_;
    int bitDepth_ = 16;
};

AudioInputRecorder::AudioInputRecorder() : impl_(std::make_unique<Impl>()) {}
AudioInputRecorder::~AudioInputRecorder() = default;
bool AudioInputRecorder::start(const std::string& outputPath, int sampleRate, int channels, const std::string& inputDeviceId, int bitDepth) { return impl_->start(outputPath, sampleRate, channels, inputDeviceId, bitDepth); }
bool AudioInputRecorder::stop(std::string& error) { return impl_->stop(error); }
RecordingStatus AudioInputRecorder::status() const { return impl_->status(); }

} // namespace neuracoust::daw
#endif
