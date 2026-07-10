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

} // namespace neuracoust::daw
