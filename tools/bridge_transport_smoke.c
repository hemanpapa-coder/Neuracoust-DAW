// Drives the C facade the SwiftUI app uses: open the device, roll the transport,
// and confirm the playhead advances. Isolates engine/bridge faults from UI ones.

#include "bridge/NeuracoustEngineBridge.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    NCEngine* engine = nc_engine_create();
    if (engine == NULL) {
        fprintf(stderr, "FAIL: nc_engine_create returned NULL\n");
        return 1;
    }

    char error[256] = {0};
    if (!nc_engine_start(engine, error, sizeof(error))) {
        // No audio device in CI is not a bridge fault; say so and skip.
        fprintf(stderr, "SKIP: engine did not start: %s\n", error);
        nc_engine_destroy(engine);
        return 0;
    }

    NCEngineStatus status;
    nc_engine_status(engine, &status);
    printf("device=%s  rate=%.0f  buffer=%d  running=%d\n",
           status.deviceName, status.sampleRate, status.requestedBufferSize, status.running);

    if (!status.running) {
        fprintf(stderr, "FAIL: engine reports not running after start\n");
        nc_engine_destroy(engine);
        return 1;
    }

    nc_engine_seek(engine, 0.0);
    nc_engine_set_transport_running(engine, true);

    usleep(600 * 1000);

    nc_engine_status(engine, &status);
    printf("after 0.6s: transportRunning=%d  playbackSeconds=%.4f\n",
           status.transportRunning, status.playbackSeconds);

    int failures = 0;
    if (!status.transportRunning) {
        fprintf(stderr, "FAIL: transport did not start\n");
        failures++;
    }
    if (status.playbackSeconds < 0.3) {
        fprintf(stderr, "FAIL: playhead did not advance (%.4f s)\n", status.playbackSeconds);
        failures++;
    }

    nc_engine_set_transport_running(engine, false);

    int bar = 0, beat = 0, tick = 0;
    nc_project_bars_beats(engine, 4.0, &bar, &beat, &tick);
    printf("bars|beats at 4.0s @120bpm 4/4 = %d|%d|%d (expect 3|1|0)\n", bar, beat, tick);
    if (bar != 3 || beat != 1 || tick != 0) {
        fprintf(stderr, "FAIL: bars|beats conversion wrong\n");
        failures++;
    }

    char timecode[128] = {0};
    nc_project_timecode(engine, 71.5, timecode, sizeof(timecode));
    printf("timecode at 71.5s = %s\n", timecode);
    if (strlen(timecode) == 0) {
        fprintf(stderr, "FAIL: empty timecode\n");
        failures++;
    }

    nc_engine_stop(engine);
    nc_engine_destroy(engine);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("bridge transport smoke OK\n");
    return 0;
}
