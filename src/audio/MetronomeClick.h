#pragma once

#include "audio/RealtimeAudioEngine.h"
#include <cstdint>

namespace neuracoust::daw {

float renderMetronomeClickSampleAtFrame(int64_t frame, const AudioEngineSettings& settings);

} // namespace neuracoust::daw
