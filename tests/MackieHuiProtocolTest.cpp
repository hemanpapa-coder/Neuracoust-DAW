#include "control/MackieHuiProtocol.h"

#include <cassert>
#include <cmath>

using namespace neuracoust::daw;

int main() {
    MackieHuiProtocol hui;

    auto fader = hui.decode({0xe3, 0x7f, 0x7f});
    assert(fader && fader->type == HuiActionType::Fader && fader->channel == 3);
    assert(std::abs(fader->value - 1.0f) < 0.0001f);

    auto zone = hui.decode({0xb0, 0x0f, 0x02});
    assert(!zone);
    auto mute = hui.decode({0xb0, 0x2f, 0x42});
    assert(mute && mute->type == HuiActionType::Mute && mute->channel == 2 && mute->pressed);
    auto muteUp = hui.decode({0xb0, 0x2f, 0x02});
    assert(muteUp && muteUp->type == HuiActionType::Mute && !muteUp->pressed);

    hui.decode({0xb0, 0x0f, 0x0e});
    auto play = hui.decode({0xb0, 0x2f, 0x43});
    assert(play && play->type == HuiActionType::Play && play->pressed);

    auto panRight = hui.decode({0xb0, 0x14, 0x03});
    assert(panRight && panRight->type == HuiActionType::PanDelta &&
           panRight->channel == 4 && panRight->value == 3.0f);
    auto panLeft = hui.decode({0xb0, 0x14, 0x41});
    assert(panLeft && panLeft->value == -1.0f);

    const auto motor = MackieHuiProtocol::fader(7, 0.5f).bytes;
    assert(motor.size() == 3 && motor[0] == 0xe7);
    const auto led = MackieHuiProtocol::switchLed(1, 2, true).bytes;
    assert(led.size() == 6 && led[2] == 1 && led[5] == 0x42);
    const auto display = MackieHuiProtocol::displayText(1, "Bass").bytes;
    assert(display.front() == 0xf0 && display.back() == 0xf7);
}
