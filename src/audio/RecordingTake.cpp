#include "audio/RecordingTake.h"

namespace neuracoust::daw {

RecordingTake::RecordingTake(int channels, int sampleRate) {
    data_.channels = channels;
    data_.sampleRate = sampleRate;
}

void RecordingTake::appendInterleavedFloat(const float* samples, int frameCount) {
    if (samples == nullptr || frameCount <= 0 || data_.channels <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto count = static_cast<size_t>(frameCount * data_.channels);
    data_.interleavedSamples.insert(data_.interleavedSamples.end(), samples, samples + count);
}

void RecordingTake::appendInterleavedInt16(const int16_t* samples, int frameCount) {
    if (samples == nullptr || frameCount <= 0 || data_.channels <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto count = static_cast<size_t>(frameCount * data_.channels);
    data_.interleavedSamples.reserve(data_.interleavedSamples.size() + count);
    for (size_t i = 0; i < count; ++i) {
        data_.interleavedSamples.push_back(static_cast<float>(samples[i] / 32768.0f));
    }
}

bool RecordingTake::saveWav(const std::string& path, std::string& error) const {
    return saveWav(path, 16, error);
}

bool RecordingTake::saveWav(const std::string& path, int bitDepth, std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (bitDepth) {
        case 16:
            return writePcm16WavFileAtomically(path, data_, error);
        case 24:
            return writePcm24WavFileAtomically(path, data_, error);
        case 32:
            return writeFloat32WavFileAtomically(path, data_, error);
        case 64:
            return writeFloat64WavFileAtomically(path, data_, error);
        default:
            return writePcm24WavFileAtomically(path, data_, error);
    }
}

void RecordingTake::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.interleavedSamples.clear();
}

int RecordingTake::channels() const {
    return data_.channels;
}

int RecordingTake::sampleRate() const {
    return data_.sampleRate;
}

int64_t RecordingTake::frameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.frameCount();
}

double RecordingTake::durationSeconds() const {
    const auto frames = frameCount();
    return data_.sampleRate > 0 ? static_cast<double>(frames) / data_.sampleRate : 0.0;
}

} // namespace neuracoust::daw
