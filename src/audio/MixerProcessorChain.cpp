#include "audio/MixerProcessorChain.h"
#include "audio/MixMath.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace neuracoust::daw {

namespace {

// The old linear balance: centre = unity in both channels, the opposite side attenuates.
std::pair<float, float> balanceGains(float clamped) {
    return { clamped > 0.0f ? 1.0f - clamped : 1.0f,
             clamped < 0.0f ? 1.0f + clamped : 1.0f };
}

// Pan gains under a law. Stereo tracks always use the linear balance (so centred stereo
// stays at unity); only mono tracks take a constant-power law, which keeps the perceived
// loudness even across the sweep by pulling the centre down (−3 / −4.5 / −6 dB).
std::pair<float, float> panGains(float pan, const std::string& law, bool isMono) {
    const float clamped = std::max(-1.0f, std::min(1.0f, pan));
    if (law == "legacy" || !isMono) return balanceGains(clamped);
    const float p = (clamped + 1.0f) * 0.5f;                 // 0 left … 1 right
    const float cpL = std::cos(p * 1.57079632679f);
    const float cpR = std::sin(p * 1.57079632679f);
    if (law == "-3dB") return { cpL, cpR };
    if (law == "-6dB") return { 1.0f - p, p };
    return { std::sqrt((1.0f - p) * cpL), std::sqrt(p * cpR) };   // −4.5 dB default
}

MixerStereoFrame foldToMono(MixerStereoFrame frame) {
    const float mono = 0.5f * (frame.left + frame.right);
    return {mono, mono};
}

} // namespace

MixerStereoFrame applyMixerGainPan(MixerStereoFrame frame, float gainDb, float pan,
                                   const std::string& panLaw, bool isMonoTrack) {
    const auto [panLeft, panRight] = panGains(pan, panLaw, isMonoTrack);
    const float gain = dbToLinearGain(gainDb);
    return {frame.left * gain * panLeft, frame.right * gain * panRight};
}

MixerStereoFrame applyMixerMasterGainBalance(MixerStereoFrame frame, float gainDb, float balance) {
    // Master is a balance, not a pan — always the linear law, never pulled down at centre.
    const auto [leftTrim, rightTrim] = balanceGains(std::max(-1.0f, std::min(1.0f, balance)));
    const float gain = dbToLinearGain(gainDb);
    return {frame.left * gain * leftTrim, frame.right * gain * rightTrim};
}

MixerRouteProcessorOutput processMixerRouteProcessors(const MixerRouteProcessorInput& input) {
    MixerRouteProcessorOutput output;
    output.postFader = input.applyRouteFaderPan
        ? applyMixerGainPan(input.input, input.gainDb, input.pan, input.panLaw, input.isMonoTrack)
        : input.input;

    for (const auto& send : input.sends) {
        if (!send.enabled || send.busName.empty()) {
            continue;
        }
        MixerStereoFrame sendFrame = send.preFader ? input.input : output.postFader;
        sendFrame = applyMixerGainPan(sendFrame, send.gainDb, send.pan, input.panLaw, input.isMonoTrack);
        if (!send.stereo) {
            sendFrame = foldToMono(sendFrame);
        }
        output.sendTaps.push_back({send.busName, sendFrame});
    }

    return output;
}

} // namespace neuracoust::daw
