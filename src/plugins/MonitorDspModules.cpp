#include "plugins/MonitorDspModules.h"

namespace neuracoust::daw {

std::vector<MonitorDspModule> defaultMonitorDspModules() {
    return {
        {"speaker-simulation", "Speaker Simulation", "monitor", true,
         "Real Speaker: Flat", "Speaker A: Flat", "Speaker B: Yamaha NS-10M Studio (NF)", "Speaker C: Auratone 5C Sound Cube (NF)",
         "Main 1-2", "None", "None", "Off", 0},
        {"headphone-simulation", "Headphone Simulation", "monitor", false,
         "Real Headphones: Off", "Headphone A: Flat", "Headphone B: Speaker A", "Headphone C: Speaker B",
         "", "", "", "Off", 0},
        {"graphic-eq", "Graphic EQ", "monitor", false},
        {"room-correction", "Room Correction", "monitor", false},
        {"crossfeed", "Crossfeed", "monitor", false},
        {"spectrum-meter", "Spectrum Meter", "metering", true},
        {"phase-meter", "Phase Meter", "metering", true},
        {"oscilloscope", "Oscilloscope", "metering", false}
    };
}

} // namespace neuracoust::daw
