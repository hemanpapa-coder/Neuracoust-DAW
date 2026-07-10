#pragma once

#include <memory>
#include <string>

namespace neuracoust::daw {

struct RecordingStatus {
    bool recording = false;
    double durationSeconds = 0.0;
    std::string outputPath;
    std::string message;
};

class AudioInputRecorder {
public:
    AudioInputRecorder();
    ~AudioInputRecorder();

    AudioInputRecorder(const AudioInputRecorder&) = delete;
    AudioInputRecorder& operator=(const AudioInputRecorder&) = delete;

    bool start(const std::string& outputPath,
               int sampleRate = 48000,
               int channels = 1,
               const std::string& inputDeviceId = {},
               int bitDepth = 16);
    bool stop(std::string& error);
    RecordingStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuracoust::daw
