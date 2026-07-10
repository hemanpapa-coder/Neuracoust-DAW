#include "audio/MixerProcessorChain.h"
#include "audio/MixMath.h"
#include <algorithm>

namespace neuracoust::daw {

namespace {

std::pair<float, float> panGains(float pan) {
    const float clamped = std::max(-1.0f, std::min(1.0f, pan));
    const float leftTrim = clamped > 0.0f ? 1.0f - clamped : 1.0f;
    const float rightTrim = clamped < 0.0f ? 1.0f + clamped : 1.0f;
    return {leftTrim, rightTrim};
}

MixerStereoFrame foldToMono(MixerStereoFrame frame) {
    const float mono = 0.5f * (frame.left + frame.right);
    return {mono, mono};
}

} // namespace

MixerStereoFrame applyMixerGainPan(MixerStereoFrame frame, float gainDb, float pan) {
    const auto [panLeft, panRight] = panGains(pan);
    const float gain = dbToLinearGain(gainDb);
    return {frame.left * gain * panLeft, frame.right * gain * panRight};
}

MixerStereoFrame applyMixerMasterGainBalance(MixerStereoFrame frame, float gainDb, float balance) {
    const auto [leftTrim, rightTrim] = panGains(balance);
    const float gain = dbToLinearGain(gainDb);
    return {frame.left * gain * leftTrim, frame.right * gain * rightTrim};
}

MixerRouteProcessorOutput processMixerRouteProcessors(const MixerRouteProcessorInput& input) {
    MixerRouteProcessorOutput output;
    output.postFader = input.applyRouteFaderPan
        ? applyMixerGainPan(input.input, input.gainDb, input.pan)
        : input.input;

    for (const auto& send : input.sends) {
        if (!send.enabled || send.busName.empty()) {
            continue;
        }
        MixerStereoFrame sendFrame = send.preFader ? input.input : output.postFader;
        sendFrame = applyMixerGainPan(sendFrame, send.gainDb, send.pan);
        if (!send.stereo) {
            sendFrame = foldToMono(sendFrame);
        }
        output.sendTaps.push_back({send.busName, sendFrame});
    }

    return output;
}

} // namespace neuracoust::daw
