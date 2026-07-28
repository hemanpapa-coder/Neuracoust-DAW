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
    // Per-slot REAL speaker the user actually monitors on (correction = target − real). Empty
    // falls back to the single realModel (old projects / the removed device-button field).
    std::string realModelA;
    std::string realModelB;
    std::string realModelC;
    // The REAL speaker's own power amp + cable, per slot — only meaningful when that real speaker
    // is PASSIVE. Independent of the modeling speaker's powerAmp/speakerCable above: the real chain
    // is subtracted (flattened) while the modeled chain is added. Kept LAST for aggregate-init order.
    std::string realAmpA;
    std::string realAmpB;
    std::string realAmpC;
    std::string realCableA;
    std::string realCableB;
    std::string realCableC;
    // The HEADPHONE side's own physical output pair — independent of the A/B/C speaker slots,
    // which the headphone path silently rode until now (switching to 헤드폰 changed the DSP
    // context but kept playing out of whatever pair the active speaker slot used). "" / "None"
    // means the main pair, the same non-mute convention as the speaker routes. monitorToHeadphone
    // is the 스피커/헤드폰 tab made visible to the engine, which routes by it. Kept LAST for
    // aggregate-init order.
    std::string headphoneOutput;
    bool monitorToHeadphone = false;
};

std::vector<MonitorDspModule> defaultMonitorDspModules();

} // namespace neuracoust::daw
