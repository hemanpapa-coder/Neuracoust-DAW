// Reproduces the "외부 DSP" (external DSP) button crash: start the engine, switch the
// monitor DSP path to remote_external, and pump status/render for a moment.
#include "bridge/NeuracoustEngineBridge.h"

#include <cstdio>
#include <thread>
#include <chrono>

int main() {
    NCEngine* engine = nc_engine_create();
    if (engine == nullptr) { printf("no engine\n"); return 1; }
    char err[256] = {0};
    bool started = nc_engine_start(engine, err, sizeof err);
    NCEngineStatus s0; nc_engine_status(engine, &s0);
    printf("started=%d running=%d err='%s'\n", started ? 1 : 0, s0.running ? 1 : 0, err);

    // Flow a signal so the remote path actually streams (it skips silent blocks).
    nc_engine_set_test_tone_enabled(engine, true);
    for (const char* mode : {"auto", "remote_external", "nds"}) {
        nc_monitor_set_path_mode(engine, mode);
        for (int i = 0; i < 40; ++i) {
            NCEngineStatus status;
            nc_engine_status(engine, &status);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    nc_engine_set_test_tone_enabled(engine, false);
    nc_monitor_set_path_mode(engine, "internal");
    nc_engine_destroy(engine);
    printf("EXTERNAL_DSP_OK\n");
    return 0;
}
