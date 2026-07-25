#include "core/Base64.h"

#include <array>

namespace neuracoust::daw {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<int8_t, 256> makeReverseTable() {
    std::array<int8_t, 256> table {};
    table.fill(-1);
    for (int index = 0; index < 64; ++index) {
        table[static_cast<unsigned char>(kAlphabet[index])] = static_cast<int8_t>(index);
    }
    return table;
}

const std::array<int8_t, 256>& reverseTable() {
    static const std::array<int8_t, 256> table = makeReverseTable();
    return table;
}

bool isSkippableWhitespace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

} // namespace

std::string encodeBase64(const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string encoded;
    if (bytes == nullptr || length == 0) {
        return encoded;
    }
    encoded.reserve(((length + 2) / 3) * 4);
    size_t index = 0;
    while (index + 2 < length) {
        const uint32_t triple = (static_cast<uint32_t>(bytes[index]) << 16) |
                                (static_cast<uint32_t>(bytes[index + 1]) << 8) |
                                static_cast<uint32_t>(bytes[index + 2]);
        encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        encoded.push_back(kAlphabet[triple & 0x3F]);
        index += 3;
    }
    const size_t remaining = length - index;
    if (remaining == 1) {
        const uint32_t triple = static_cast<uint32_t>(bytes[index]) << 16;
        encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (remaining == 2) {
        const uint32_t triple = (static_cast<uint32_t>(bytes[index]) << 16) |
                                (static_cast<uint32_t>(bytes[index + 1]) << 8);
        encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        encoded.push_back('=');
    }
    return encoded;
}

std::string encodeBase64(const std::vector<uint8_t>& bytes) {
    return encodeBase64(bytes.data(), bytes.size());
}

bool decodeBase64(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    const auto& table = reverseTable();
    uint32_t accumulator = 0;
    int accumulatedSextets = 0;
    int padding = 0;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (isSkippableWhitespace(byte)) {
            continue;
        }
        if (byte == '=') {
            ++padding;
            if (padding > 2) {
                out.clear();
                return false;
            }
            continue;
        }
        if (padding > 0) {
            // Data after the padding: the blob is not what it claims to be.
            out.clear();
            return false;
        }
        const int8_t value = table[byte];
        if (value < 0) {
            out.clear();
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        if (++accumulatedSextets == 4) {
            out.push_back(static_cast<uint8_t>((accumulator >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((accumulator >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(accumulator & 0xFF));
            accumulator = 0;
            accumulatedSextets = 0;
        }
    }
    if (accumulatedSextets == 1) {
        // A lone sextet carries no whole byte — the text was truncated mid-group.
        out.clear();
        return false;
    }
    if (accumulatedSextets == 2) {
        out.push_back(static_cast<uint8_t>((accumulator >> 4) & 0xFF));
    } else if (accumulatedSextets == 3) {
        out.push_back(static_cast<uint8_t>((accumulator >> 10) & 0xFF));
        out.push_back(static_cast<uint8_t>((accumulator >> 2) & 0xFF));
    }
    return true;
}

} // namespace neuracoust::daw
