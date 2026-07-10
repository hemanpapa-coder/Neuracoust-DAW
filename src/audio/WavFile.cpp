#include "audio/WavFile.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace neuracoust::daw {

namespace {

uint32_t readU32(std::istream& in) {
    uint8_t b[4] {};
    in.read(reinterpret_cast<char*>(b), 4);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

uint16_t readU16(std::istream& in) {
    uint8_t b[2] {};
    in.read(reinterpret_cast<char*>(b), 2);
    return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}

uint16_t readU16FromBytes(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 2 > bytes.size()) {
        return 0;
    }
    return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8);
}

uint32_t readU32FromBytes(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return uint32_t(bytes[offset]) |
        (uint32_t(bytes[offset + 1]) << 8) |
        (uint32_t(bytes[offset + 2]) << 16) |
        (uint32_t(bytes[offset + 3]) << 24);
}

uint64_t readU64FromBytes(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 8 > bytes.size()) {
        return 0;
    }
    return uint64_t(readU32FromBytes(bytes, offset)) |
        (uint64_t(readU32FromBytes(bytes, offset + 4)) << 32);
}

bool guidMatchesWaveSubformat(const std::vector<uint8_t>& bytes, size_t offset, uint16_t tag) {
    static constexpr uint8_t kWaveTail[12] {
        0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xaa,
        0x00, 0x38, 0x9b, 0x71
    };
    if (offset + 16 > bytes.size()) {
        return false;
    }
    if (readU16FromBytes(bytes, offset) != tag || bytes[offset + 2] != 0 || bytes[offset + 3] != 0) {
        return false;
    }
    return std::equal(std::begin(kWaveTail), std::end(kWaveTail), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4));
}

void writeU32(std::ostream& out, uint32_t value) {
    const uint8_t b[4] {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>((value >> 16) & 0xff),
        static_cast<uint8_t>((value >> 24) & 0xff)
    };
    out.write(reinterpret_cast<const char*>(b), 4);
}

void writeU16(std::ostream& out, uint16_t value) {
    const uint8_t b[2] {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff)
    };
    out.write(reinterpret_cast<const char*>(b), 2);
}

void writePcm24Sample(std::ostream& out, int32_t value) {
    const uint8_t b[3] {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>((value >> 16) & 0xff)
    };
    out.write(reinterpret_cast<const char*>(b), 3);
}

void writeFloat32Sample(std::ostream& out, float value) {
    const float finite = std::isfinite(value) ? value : 0.0f;
    uint32_t raw = 0;
    std::memcpy(&raw, &finite, sizeof(raw));
    writeU32(out, raw);
}

void writeFloat64Sample(std::ostream& out, double value) {
    const double finite = std::isfinite(value) ? value : 0.0;
    uint64_t raw = 0;
    std::memcpy(&raw, &finite, sizeof(raw));
    writeU32(out, static_cast<uint32_t>(raw & 0xffffffffu));
    writeU32(out, static_cast<uint32_t>((raw >> 32) & 0xffffffffu));
}

bool writeWaveHeader(std::ofstream& out,
                     const WavAudioData& data,
                     int bits,
                     uint16_t formatTag,
                     uint32_t dataBytes,
                     std::string& error) {
    if (data.channels <= 0 || data.sampleRate <= 0) {
        error = "Invalid WAV data.";
        return false;
    }
    out.write("RIFF", 4);
    writeU32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, formatTag);
    writeU16(out, static_cast<uint16_t>(data.channels));
    writeU32(out, static_cast<uint32_t>(data.sampleRate));
    writeU32(out, static_cast<uint32_t>(data.sampleRate * data.channels * (bits / 8)));
    writeU16(out, static_cast<uint16_t>(data.channels * (bits / 8)));
    writeU16(out, static_cast<uint16_t>(bits));
    out.write("data", 4);
    writeU32(out, dataBytes);
    return true;
}

std::filesystem::path pathFromUtf8String(const std::string& path) {
#if defined(_WIN32)
    return std::filesystem::u8path(path);
#else
    return std::filesystem::path(path);
#endif
}

} // namespace

bool readPcmWavFile(const std::filesystem::path& path, WavAudioData& outData, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open WAV file.";
        return false;
    }

    char riff[4] {};
    in.read(riff, 4);
    const auto riffSize = readU32(in);
    (void)riffSize;
    char wave[4] {};
    in.read(wave, 4);
    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") {
        error = "Not a RIFF/WAVE file.";
        return false;
    }

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    std::vector<uint8_t> data;
    double embeddedTempoBpm = 0.0;
    bool hasBroadcastTimeReference = false;
    uint64_t broadcastTimeReferenceSamples = 0;

    while (in && !in.eof()) {
        char chunkId[4] {};
        in.read(chunkId, 4);
        if (in.gcount() != 4) {
            break;
        }
        const uint32_t chunkSize = readU32(in);
        const auto id = std::string(chunkId, 4);
        if (id == "fmt ") {
            std::vector<uint8_t> fmt(chunkSize);
            in.read(reinterpret_cast<char*>(fmt.data()), chunkSize);
            if (fmt.size() >= 16) {
                audioFormat = readU16FromBytes(fmt, 0);
                channels = readU16FromBytes(fmt, 2);
                sampleRate = readU32FromBytes(fmt, 4);
                bitsPerSample = readU16FromBytes(fmt, 14);
                if (audioFormat == 0xfffe && fmt.size() >= 40) {
                    if (guidMatchesWaveSubformat(fmt, 24, 1)) {
                        audioFormat = 1;
                    } else if (guidMatchesWaveSubformat(fmt, 24, 3)) {
                        audioFormat = 3;
                    }
                }
            }
        } else if (id == "data") {
            data.resize(chunkSize);
            in.read(reinterpret_cast<char*>(data.data()), chunkSize);
        } else if (id == "acid") {
            std::vector<uint8_t> acid(chunkSize);
            in.read(reinterpret_cast<char*>(acid.data()), chunkSize);
            if (acid.size() >= 24) {
                uint32_t rawTempo = readU32FromBytes(acid, 16);
                float tempo = 0.0f;
                std::memcpy(&tempo, &rawTempo, sizeof(float));
                if (std::isfinite(tempo) && tempo >= 20.0f && tempo <= 400.0f) {
                    embeddedTempoBpm = tempo;
                }
            }
        } else if (id == "bext") {
            std::vector<uint8_t> bext(chunkSize);
            in.read(reinterpret_cast<char*>(bext.data()), chunkSize);
            if (bext.size() >= 346) {
                broadcastTimeReferenceSamples = readU64FromBytes(bext, 338);
                hasBroadcastTimeReference = true;
            }
        } else {
            in.seekg(chunkSize, std::ios::cur);
        }
        if ((chunkSize & 1u) != 0u) {
            in.seekg(1, std::ios::cur);
        }
    }

    if ((audioFormat != 1 && audioFormat != 3) || channels == 0 || sampleRate == 0 || data.empty()) {
        error = "Only PCM or IEEE float WAV files are supported in this pass.";
        return false;
    }

    outData.channels = channels;
    outData.sampleRate = static_cast<int>(sampleRate);
    outData.bitsPerSample = bitsPerSample;
    outData.floatingPoint = audioFormat == 3;
    outData.embeddedTempoBpm = embeddedTempoBpm;
    outData.hasBroadcastTimeReference = hasBroadcastTimeReference;
    outData.broadcastTimeReferenceSamples = broadcastTimeReferenceSamples;
    outData.broadcastTimeReferenceSeconds = hasBroadcastTimeReference && sampleRate > 0
        ? static_cast<double>(broadcastTimeReferenceSamples) / static_cast<double>(sampleRate)
        : 0.0;
    outData.interleavedSamples.clear();

    if (audioFormat == 1 && bitsPerSample == 16) {
        const auto sampleCount = data.size() / sizeof(int16_t);
        outData.interleavedSamples.reserve(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            const auto offset = i * sizeof(int16_t);
            const auto value = static_cast<int16_t>(uint16_t(data[offset]) | (uint16_t(data[offset + 1]) << 8));
            outData.interleavedSamples.push_back(static_cast<float>(value / 32768.0f));
        }
    } else if (audioFormat == 1 && bitsPerSample == 24) {
        const auto sampleCount = data.size() / 3;
        outData.interleavedSamples.reserve(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            const auto offset = i * 3;
            int32_t value = int32_t(data[offset]) | (int32_t(data[offset + 1]) << 8) | (int32_t(data[offset + 2]) << 16);
            if ((value & 0x00800000) != 0) {
                value |= ~0x00ffffff;
            }
            outData.interleavedSamples.push_back(static_cast<float>(value / 8388608.0f));
        }
    } else if (audioFormat == 1 && bitsPerSample == 32) {
        const auto sampleCount = data.size() / sizeof(int32_t);
        outData.interleavedSamples.reserve(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            const auto offset = i * sizeof(int32_t);
            const auto unsignedValue = readU32FromBytes(data, offset);
            const auto value = static_cast<int32_t>(unsignedValue);
            outData.interleavedSamples.push_back(static_cast<float>(static_cast<double>(value) / 2147483648.0));
        }
    } else if (audioFormat == 3 && bitsPerSample == 32) {
        const auto sampleCount = data.size() / sizeof(float);
        outData.interleavedSamples.reserve(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            const auto raw = readU32FromBytes(data, i * sizeof(float));
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(float));
            outData.interleavedSamples.push_back(std::isfinite(value) ? std::max(-4.0f, std::min(4.0f, value)) : 0.0f);
        }
    } else if (audioFormat == 3 && bitsPerSample == 64) {
        const auto sampleCount = data.size() / sizeof(double);
        outData.interleavedSamples.reserve(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            const auto offset = i * sizeof(double);
            const uint64_t raw = static_cast<uint64_t>(readU32FromBytes(data, offset)) |
                (static_cast<uint64_t>(readU32FromBytes(data, offset + 4)) << 32);
            double value = 0.0;
            std::memcpy(&value, &raw, sizeof(double));
            outData.interleavedSamples.push_back(std::isfinite(value) ? static_cast<float>(std::max(-4.0, std::min(4.0, value))) : 0.0f);
        }
    } else {
        error = "Unsupported WAV sample format.";
        return false;
    }

    return true;
}

bool readPcmWavFile(const std::string& path, WavAudioData& outData, std::string& error) {
    return readPcmWavFile(pathFromUtf8String(path), outData, error);
}

bool writeTestToneWavFile(const std::filesystem::path& path, int sampleRate, double seconds, double frequencyHz) {
    const int channels = 2;
    const int bits = 16;
    const int frames = static_cast<int>(seconds * sampleRate);
    const int dataBytes = frames * channels * (bits / 8);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write("RIFF", 4);
    writeU32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, channels);
    writeU32(out, static_cast<uint32_t>(sampleRate));
    writeU32(out, static_cast<uint32_t>(sampleRate * channels * (bits / 8)));
    writeU16(out, static_cast<uint16_t>(channels * (bits / 8)));
    writeU16(out, bits);
    out.write("data", 4);
    writeU32(out, static_cast<uint32_t>(dataBytes));
    for (int frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / sampleRate;
        const auto sample = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979323846 * frequencyHz * t) * 3000.0);
        writeU16(out, static_cast<uint16_t>(sample));
        writeU16(out, static_cast<uint16_t>(sample));
    }
    return true;
}

bool writeTestToneWavFile(const std::string& path, int sampleRate, double seconds, double frequencyHz) {
    return writeTestToneWavFile(pathFromUtf8String(path), sampleRate, seconds, frequencyHz);
}

WavAudioData resampleAudioDataLinear(const WavAudioData& data, int targetSampleRate) {
    if (data.channels <= 0 || data.sampleRate <= 0 || targetSampleRate <= 0 || data.interleavedSamples.empty()) {
        return data;
    }
    if (data.sampleRate == targetSampleRate) {
        WavAudioData copy = data;
        copy.sampleRate = targetSampleRate;
        return copy;
    }

    const int64_t sourceFrames = data.frameCount();
    if (sourceFrames <= 0) {
        WavAudioData copy = data;
        copy.sampleRate = targetSampleRate;
        return copy;
    }

    WavAudioData out;
    out.channels = data.channels;
    out.sampleRate = targetSampleRate;
    out.bitsPerSample = data.bitsPerSample;
    out.floatingPoint = data.floatingPoint;
    out.embeddedTempoBpm = data.embeddedTempoBpm;
    const double durationSeconds = static_cast<double>(sourceFrames) / static_cast<double>(data.sampleRate);
    const int64_t targetFrames = std::max<int64_t>(1, static_cast<int64_t>(std::llround(durationSeconds * targetSampleRate)));
    out.interleavedSamples.assign(static_cast<size_t>(targetFrames) * static_cast<size_t>(out.channels), 0.0f);

    const double sourceRate = static_cast<double>(data.sampleRate);
    const double targetRate = static_cast<double>(targetSampleRate);
    for (int64_t frame = 0; frame < targetFrames; ++frame) {
        const double sourcePosition = static_cast<double>(frame) * sourceRate / targetRate;
        const int64_t leftFrame = std::max<int64_t>(0, std::min<int64_t>(sourceFrames - 1, static_cast<int64_t>(std::floor(sourcePosition))));
        const int64_t rightFrame = std::min<int64_t>(sourceFrames - 1, leftFrame + 1);
        const float fraction = static_cast<float>(sourcePosition - static_cast<double>(leftFrame));
        for (int channel = 0; channel < out.channels; ++channel) {
            const auto leftIndex = static_cast<size_t>(leftFrame * data.channels + channel);
            const auto rightIndex = static_cast<size_t>(rightFrame * data.channels + channel);
            const float left = leftIndex < data.interleavedSamples.size() ? data.interleavedSamples[leftIndex] : 0.0f;
            const float right = rightIndex < data.interleavedSamples.size() ? data.interleavedSamples[rightIndex] : left;
            const auto outIndex = static_cast<size_t>(frame * out.channels + channel);
            out.interleavedSamples[outIndex] = left + (right - left) * fraction;
        }
    }
    return out;
}

bool writePcm16WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    const int bits = 16;
    const auto sampleCount = data.interleavedSamples.size();
    const auto dataBytes = static_cast<uint32_t>(sampleCount * sizeof(int16_t));
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Could not create WAV file.";
        return false;
    }

    if (!writeWaveHeader(out, data, bits, 1, dataBytes, error)) {
        return false;
    }

    for (float sample : data.interleavedSamples) {
        const float clipped = std::max(-1.0f, std::min(1.0f, sample));
        const auto pcm = static_cast<int16_t>(clipped * 32767.0f);
        writeU16(out, static_cast<uint16_t>(pcm));
    }
    out.close();
    if (!out) {
        error = "Could not finish writing WAV file.";
        return false;
    }
    return true;
}

bool writePcm16WavFile(const std::string& path, const WavAudioData& data, std::string& error) {
    return writePcm16WavFile(pathFromUtf8String(path), data, error);
}

bool writePcm24WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    const int bits = 24;
    const auto sampleCount = data.interleavedSamples.size();
    const auto dataBytes = static_cast<uint32_t>(sampleCount * 3);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Could not create WAV file.";
        return false;
    }

    if (!writeWaveHeader(out, data, bits, 1, dataBytes, error)) {
        return false;
    }

    for (float sample : data.interleavedSamples) {
        const double clipped = std::max(-1.0, std::min(1.0, static_cast<double>(sample)));
        const auto pcm = static_cast<int32_t>(std::round(clipped * 8388607.0));
        writePcm24Sample(out, std::max<int32_t>(-8388608, std::min<int32_t>(8388607, pcm)));
    }
    out.close();
    if (!out) {
        error = "Could not finish writing WAV file.";
        return false;
    }
    return true;
}

bool writeFloat32WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    const int bits = 32;
    const auto sampleCount = data.interleavedSamples.size();
    const auto dataBytes = static_cast<uint32_t>(sampleCount * sizeof(float));
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Could not create WAV file.";
        return false;
    }
    if (!writeWaveHeader(out, data, bits, 3, dataBytes, error)) {
        return false;
    }
    for (float sample : data.interleavedSamples) {
        writeFloat32Sample(out, sample);
    }
    out.close();
    if (!out) {
        error = "Could not finish writing WAV file.";
        return false;
    }
    return true;
}

bool writeFloat64WavFile(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    const int bits = 64;
    const auto sampleCount = data.interleavedSamples.size();
    const auto dataBytes = static_cast<uint32_t>(sampleCount * sizeof(double));
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Could not create WAV file.";
        return false;
    }
    if (!writeWaveHeader(out, data, bits, 3, dataBytes, error)) {
        return false;
    }
    for (float sample : data.interleavedSamples) {
        writeFloat64Sample(out, static_cast<double>(sample));
    }
    out.close();
    if (!out) {
        error = "Could not finish writing WAV file.";
        return false;
    }
    return true;
}

bool writeWavFileAtomically(const std::filesystem::path& path,
                            const WavAudioData& data,
                            bool (*writer)(const std::filesystem::path&, const WavAudioData&, std::string&),
                            std::string& error) {
    if (path.empty()) {
        error = "WAV path is empty.";
        return false;
    }

    std::error_code fsError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), fsError);
        if (fsError) {
            error = "Could not create WAV output directory: " + fsError.message();
            return false;
        }
    }

    std::ostringstream suffix;
    suffix << ".writing."
           << std::chrono::steady_clock::now().time_since_epoch().count()
           << "."
           << reinterpret_cast<std::uintptr_t>(&error);

    auto tempPath = path;
    tempPath += suffix.str();
    if (!writer(tempPath, data, error)) {
        std::filesystem::remove(tempPath, fsError);
        return false;
    }

    std::filesystem::rename(tempPath, path, fsError);
    if (fsError) {
        std::filesystem::remove(path, fsError);
        fsError.clear();
        std::filesystem::rename(tempPath, path, fsError);
    }
    if (fsError) {
        const auto message = fsError.message();
        std::filesystem::remove(tempPath, fsError);
        error = "Could not replace WAV file: " + message;
        return false;
    }

    error.clear();
    return true;
}

bool writePcm16WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    return writeWavFileAtomically(path, data, writePcm16WavFile, error);
}

bool writePcm24WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    return writeWavFileAtomically(path, data, writePcm24WavFile, error);
}

bool writeFloat32WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    return writeWavFileAtomically(path, data, writeFloat32WavFile, error);
}

bool writeFloat64WavFileAtomically(const std::filesystem::path& path, const WavAudioData& data, std::string& error) {
    return writeWavFileAtomically(path, data, writeFloat64WavFile, error);
}

} // namespace neuracoust::daw
