#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct WavAudioData {
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    bool floatingPoint = false;
    double embeddedTempoBpm = 0.0;
    bool hasBroadcastTimeReference = false;
    uint64_t broadcastTimeReferenceSamples = 0;
    double broadcastTimeReferenceSeconds = 0.0;
    std::vector<float> interleavedSamples;

    int64_t frameCount() const {
        return channels > 0 ? static_cast<int64_t>(interleavedSamples.size() / channels) : 0;
    }
};

bool readPcmWavFile(const std::string& path, WavAudioData& outData, std::string& error);
bool readPcmWavFile(const std::filesystem::path& path, WavAudioData& outData, std::string& error);
bool writePcm16WavFile(const std::string& path, const WavAudioData& data, std::string& error);
bool writePcm16WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writePcm16WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writePcm24WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writePcm24WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writeFloat32WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writeFloat32WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writeFloat64WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writeFloat64WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error);
bool writeTestToneWavFile(const std::string& path, int sampleRate, double seconds, double frequencyHz);
bool writeTestToneWavFile(const std::filesystem::path& path, int sampleRate, double seconds, double frequencyHz);
WavAudioData resampleAudioDataLinear(const WavAudioData& data, int targetSampleRate);

} // namespace neuracoust::daw
