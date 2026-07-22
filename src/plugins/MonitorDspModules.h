#pragma once

#include "core/DawState.h"
#include <string>
#include <vector>

namespace neuracoust::daw {

struct MonitorDspModule {
    std::string id;
    std::string displayName;
    std::string stage;
    bool enabled = true;
    std::string realModel;
    std::string targetModelA;
    std::string targetModelB;
    std::string targetModelC;
    std::string speakerOutputA;
    std::string speakerOutputB;
    std::string speakerOutputC;
    std::string streamingPreview;
    int activeTargetSlot = 0;
    bool speakerRoomEqA = true;
    bool speakerRoomEqB = true;
    bool speakerRoomEqC = true;
    float speakerSimulationWeightA = 0.0f;
    float speakerSimulationWeightB = 0.0f;
    float speakerSimulationWeightC = 0.0f;
    std::vector<TrackInsertSlot> speakerInsertsA;
    std::vector<TrackInsertSlot> speakerInsertsB;
    std::vector<TrackInsertSlot> speakerInsertsC;
    // Passive modeled speaker's power amp + cable (name heuristic tone), per A/B/C slot.
    // Kept LAST so positional aggregate initializers (defaultMonitorDspModules) are undisturbed.
    std::string powerAmpA;
    std::string powerAmpB;
    std::string powerAmpC;
    std::string speakerCableA;
    std::string speakerCableB;
    std::string speakerCableC;
};

std::vector<MonitorDspModule> defaultMonitorDspModules();

} // namespace neuracoust::daw
