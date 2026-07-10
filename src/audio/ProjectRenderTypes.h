#pragma once

#include "audio/WavFile.h"
#include <string>

namespace neuracoust::daw {

struct ProjectExternalSidechainBus {
    std::string name = "External Sidechain";
    WavAudioData source;
    double startSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
};

} // namespace neuracoust::daw
