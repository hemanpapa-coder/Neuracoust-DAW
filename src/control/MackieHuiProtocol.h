#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct HuiMidiMessage {
    std::vector<std::uint8_t> bytes;
};

enum class HuiActionType {
    None,
    Fader,
    PanDelta,
    Select,
    Mute,
    Solo,
    RecordArm,
    Play,
    Stop,
    Record,
    Rewind,
    FastForward,
    BankLeft,
    BankRight,
    ChannelLeft,
    ChannelRight,
    Scrub,
};

struct HuiAction {
    HuiActionType type = HuiActionType::None;
    int channel = -1;
    float value = 0.0f;
    bool pressed = false;
};

/// Stateful decoder/encoder for the Mackie HUI MIDI protocol.
///
/// HUI switch messages arrive as a zone-select CC (0x0f), followed by a
/// port/value CC (0x2f). Faders are 14-bit pitch bend messages on channels 1–8.
/// Pan encoders use the eight relative controllers at 0x10–0x17.
class MackieHuiProtocol {
public:
    std::optional<HuiAction> decode(const std::vector<std::uint8_t>& message);

    static HuiMidiMessage fader(int channel, float normalized);
    static HuiMidiMessage switchLed(std::uint8_t zone, std::uint8_t port, bool on);
    static HuiMidiMessage keepAlive();
    static HuiMidiMessage displayText(int channel, const std::string& text);

private:
    std::uint8_t selectedZone_ = 0xff;
};

} // namespace neuracoust::daw
