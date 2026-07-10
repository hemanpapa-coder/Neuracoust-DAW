#pragma once

#include "audio/MixerGraph.h"
#include "core/DawState.h"
#include <string>
#include <vector>

namespace neuracoust::daw {

struct MixerStereoFrame {
    float left = 0.0f;
    float right = 0.0f;
};

struct MixerSendTapFrame {
    std::string busName;
    MixerStereoFrame frame;
};

struct MixerRouteProcessorInput {
    MixerStereoFrame input;
    MixerRouteKind routeKind = MixerRouteKind::Unknown;
    float gainDb = 0.0f;
    float pan = 0.0f;
    bool applyRouteFaderPan = true;
    std::vector<TrackSendState> sends;
};

struct MixerRouteProcessorOutput {
    MixerStereoFrame postFader;
    std::vector<MixerSendTapFrame> sendTaps;
};

MixerStereoFrame applyMixerGainPan(MixerStereoFrame frame, float gainDb, float pan);
MixerStereoFrame applyMixerMasterGainBalance(MixerStereoFrame frame, float gainDb, float balance);
MixerRouteProcessorOutput processMixerRouteProcessors(const MixerRouteProcessorInput& input);

} // namespace neuracoust::daw
