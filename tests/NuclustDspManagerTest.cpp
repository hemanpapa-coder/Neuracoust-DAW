#include "nuclust/NuclustDspManager.h"

#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace neuracoust::nuclust;

int main() {
#if defined(_WIN32)
    _putenv_s("NUCLUST_DSP_MANAGER_HOME", "build/nuclust-test-settings");
    _putenv_s("NUCLUST_DSP_MOCK_SERVERS", "10.0.10.20,10.0.11.20");
#else
    setenv("NUCLUST_DSP_MANAGER_HOME", "build/nuclust-test-settings", 1);
    setenv("NUCLUST_DSP_MOCK_SERVERS", "10.0.10.20,10.0.11.20", 1);
#endif
    NuclustDspManagerCore core;
    auto snapshot = core.snapshot();
    assert(snapshot.servers.size() >= 2);
    assert(!snapshot.interfaces.empty());

    PluginRegistrationRequest req;
    req.instanceId = "test-instance";
    req.pluginId = "neuracoust.test";
    req.pluginVersion = "260704.0000";
    req.requestedMode = DspExecutionMode::ExternalDsp;
    req.serverModuleId = "neuracoust.test.external";
    req.channelCount = 2;
    auto response = core.registerPlugin(req);
    assert(response.assignedMode == DspExecutionMode::ExternalDsp);
    assert(response.status == PluginAssignmentStatus::Ready);
    assert(!response.assignedServerId.empty());

    req.instanceId = "missing-module";
    req.serverModuleId = "neuracoust.missing.module";
    response = core.registerPlugin(req);
    assert(response.assignedMode == DspExecutionMode::Native);
    assert(response.status == PluginAssignmentStatus::NativeFallback);

    auto serverId = core.snapshot().servers.front().serverId;
    core.clearJitterLatch(serverId);
    core.setDefaultBufferFrames(512);
    snapshot = core.snapshot();
    assert(snapshot.settings.defaultBufferFrames == 512);
    assert(snapshot.servers.front().networkBufferFrames == 512);

    core.unregisterPlugin("test-instance");
    core.unregisterPlugin("missing-module");
    std::cout << "Neuracoust DSP Manager core smoke passed\n";
    return 0;
}
