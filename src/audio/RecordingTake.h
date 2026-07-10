#pragma once

#include "audio/WavFile.h"
#include <mutex>
#include <string>

namespace neuracoust::daw {

class RecordingTake {
public:
    RecordingTake(int channels, int sampleRate);

    void appendInterleavedFloat(const float* samples, int frameCount);
    void appendInterleavedInt16(const int16_t* samples, int frameCount);
    bool saveWav(const std::string& path, std::string& error) const;
    bool saveWav(const std::string& path, int bitDepth, std::string& error) const;
    void clear();

    int channels() const;
    int sampleRate() const;
    int64_t frameCount() const;
    double durationSeconds() const;

private:
    mutable std::mutex mutex_;
    WavAudioData data_;
};

} // namespace neuracoust::daw
