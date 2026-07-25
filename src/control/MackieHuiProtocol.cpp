#include "control/MackieHuiProtocol.h"

#include <algorithm>
#include <cmath>

namespace neuracoust::daw {
namespace {

std::optional<HuiAction> decodeSwitch(std::uint8_t zone, std::uint8_t portValue) {
    const bool pressed = (portValue & 0x40u) != 0;
    const int port = portValue & 0x07u;

    // Zones 0–7 are the eight channel strips. Ports 0–3 are the standard
    // Record/Solo/Mute/Select row used by HUI-compatible surfaces.
    if (zone <= 7) {
        static constexpr HuiActionType channelTypes[] = {
            HuiActionType::RecordArm, HuiActionType::Solo,
            HuiActionType::Mute, HuiActionType::Select
        };
        if (port < 4)
            return HuiAction{channelTypes[port], static_cast<int>(zone), pressed ? 1.0f : 0.0f, pressed};
    }

    // HUI transport zone.
    if (zone == 0x0e && port <= 4) {
        static constexpr HuiActionType transportTypes[] = {
            HuiActionType::Rewind, HuiActionType::FastForward,
            HuiActionType::Stop, HuiActionType::Play, HuiActionType::Record
        };
        return HuiAction{transportTypes[port], -1, pressed ? 1.0f : 0.0f, pressed};
    }

    // Window/bank navigation zone used by the original HUI layout.
    if (zone == 0x0a && port <= 3) {
        static constexpr HuiActionType navigationTypes[] = {
            HuiActionType::BankLeft, HuiActionType::BankRight,
            HuiActionType::ChannelLeft, HuiActionType::ChannelRight
        };
        return HuiAction{navigationTypes[port], -1, pressed ? 1.0f : 0.0f, pressed};
    }
    return std::nullopt;
}

} // namespace

std::optional<HuiAction> MackieHuiProtocol::decode(const std::vector<std::uint8_t>& m) {
    if (m.size() == 1 && m[0] == 0xfe)
        return HuiAction{HuiActionType::None, -1, 0.0f, true};
    if (m.size() < 3)
        return std::nullopt;

    const auto status = m[0];
    if ((status & 0xf0u) == 0xe0u && (status & 0x0fu) < 9) {
        const int raw = (static_cast<int>(m[2] & 0x7f) << 7) | (m[1] & 0x7f);
        return HuiAction{HuiActionType::Fader, status & 0x0f,
                         static_cast<float>(raw) / 16383.0f, true};
    }
    if ((status & 0xf0u) == 0xb0u && m[1] >= 0x10 && m[1] <= 0x17) {
        const int raw = m[2] & 0x7f;
        const int delta = raw == 0 ? 0 : ((raw & 0x40) ? -(raw & 0x3f) : raw);
        return HuiAction{HuiActionType::PanDelta, static_cast<int>(m[1] - 0x10),
                         static_cast<float>(delta), true};
    }
    if ((status & 0xf0u) == 0xb0u && m[1] == 0x0f) {
        selectedZone_ = m[2] & 0x7f;
        return std::nullopt;
    }
    if ((status & 0xf0u) == 0xb0u && m[1] == 0x2f && selectedZone_ != 0xff)
        return decodeSwitch(selectedZone_, m[2] & 0x7f);
    return std::nullopt;
}

HuiMidiMessage MackieHuiProtocol::fader(int channel, float normalized) {
    channel = std::clamp(channel, 0, 8);
    const int raw = std::clamp(static_cast<int>(std::lround(normalized * 16383.0f)), 0, 16383);
    return {{static_cast<std::uint8_t>(0xe0 | channel),
             static_cast<std::uint8_t>(raw & 0x7f),
             static_cast<std::uint8_t>((raw >> 7) & 0x7f)}};
}

HuiMidiMessage MackieHuiProtocol::switchLed(std::uint8_t zone, std::uint8_t port, bool on) {
    return {{0xb0, 0x0f, static_cast<std::uint8_t>(zone & 0x7f),
             0xb0, 0x2f, static_cast<std::uint8_t>((port & 0x07) | (on ? 0x40 : 0x00))}};
}

HuiMidiMessage MackieHuiProtocol::keepAlive() {
    return {{0xfe}};
}

HuiMidiMessage MackieHuiProtocol::displayText(int channel, const std::string& text) {
    channel = std::clamp(channel, 0, 7);
    std::vector<std::uint8_t> bytes{0xf0, 0x00, 0x00, 0x66, 0x05, 0x00,
                                    static_cast<std::uint8_t>(channel * 4)};
    for (int i = 0; i < 4; ++i) {
        const auto c = i < static_cast<int>(text.size()) ? text[static_cast<size_t>(i)] : ' ';
        bytes.push_back(static_cast<std::uint8_t>(c) & 0x7f);
    }
    bytes.push_back(0xf7);
    return {std::move(bytes)};
}

} // namespace neuracoust::daw
