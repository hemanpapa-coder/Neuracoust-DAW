#pragma once

#include "plugins/MonitorDspModules.h"
#include <string>
#include <vector>

namespace neuracoust::daw {

struct MonitorOutputRoute {
    int activeSlot = 0;
    int leftChannel = 0;
    int rightChannel = 1;
    bool assigned = true;
    bool available = true;
    std::string requestedRoute = "Main 1-2";
    std::string displayRoute = "Main 1-2";
    std::string description = "Speaker A -> Main 1-2";
};

int monitorOutputRequiredChannels(const std::vector<MonitorDspModule>& modules);
MonitorOutputRoute resolveMonitorOutputRoute(const std::vector<MonitorDspModule>& modules, int availableChannels);

/// Simultaneous speaker+headphone: the OTHER tab's output pair, valid only while the module's
/// simultaneousOutput flag is on (the 배타 switch, inverted) AND it lands on a different pair
/// than the primary — the same pair twice is one output, and writing it twice doubles the level.
struct MonitorSecondaryOutputRoute {
    MonitorOutputRoute route;
    bool active = false;
};
MonitorSecondaryOutputRoute resolveMonitorSecondaryOutputRoute(const std::vector<MonitorDspModule>& modules,
                                                               int availableChannels);

} // namespace neuracoust::daw
