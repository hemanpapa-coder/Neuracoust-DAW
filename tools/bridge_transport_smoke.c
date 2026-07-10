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

    // ---- monitor station ----
    const int moduleCount = nc_monitor_module_count(engine);
    printf("monitor modules: %d\n", moduleCount);
    if (moduleCount < 1) {
        fprintf(stderr, "FAIL: monitor module chain is empty\n");
        failures++;
    }

    char moduleName[128] = {0};
    nc_monitor_module_name(engine, 0, moduleName, sizeof(moduleName));
    if (strcmp(moduleName, "Speaker Simulation") != 0) {
        fprintf(stderr, "FAIL: module 0 is '%s', expected 'Speaker Simulation'\n", moduleName);
        failures++;
    }

    const bool wasEnabled = nc_monitor_module_enabled(engine, 0);
    nc_monitor_set_module_enabled(engine, 0, !wasEnabled);
    if (nc_monitor_module_enabled(engine, 0) == wasEnabled) {
        fprintf(stderr, "FAIL: module enable did not toggle\n");
        failures++;
    }
    nc_monitor_set_module_enabled(engine, 0, wasEnabled);

    nc_monitor_set_volume_db(engine, -18.0f);
    if (nc_monitor_volume_db(engine) < -18.5f || nc_monitor_volume_db(engine) > -17.5f) {
        fprintf(stderr, "FAIL: monitor volume readback %.2f, expected -18\n", nc_monitor_volume_db(engine));
        failures++;
    }
    // Clamped to the -60…+6 dB range the station accepts.
    nc_monitor_set_volume_db(engine, 999.0f);
    if (nc_monitor_volume_db(engine) > 6.0f) {
        fprintf(stderr, "FAIL: monitor volume not clamped (%.2f)\n", nc_monitor_volume_db(engine));
        failures++;
    }
    nc_monitor_set_volume_db(engine, -6.0f);

    nc_monitor_set_dim(engine, true);
    if (!nc_monitor_dim(engine)) {
        fprintf(stderr, "FAIL: dim did not latch\n");
        failures++;
    }
    nc_monitor_set_dim(engine, false);

    // Switching the active speaker set must change the model the module reports.
    char modelA[128] = {0};
    char modelB[128] = {0};
    nc_monitor_set_active_speaker_slot(engine, 0);
    nc_monitor_module_detail(engine, 0, modelA, sizeof(modelA));
    nc_monitor_set_active_speaker_slot(engine, 1);
    nc_monitor_module_detail(engine, 0, modelB, sizeof(modelB));
    printf("speaker A detail='%s'  B detail='%s'\n", modelA, modelB);
    if (nc_monitor_active_speaker_slot(engine) != 1) {
        fprintf(stderr, "FAIL: active speaker slot did not change\n");
        failures++;
    }
    if (strcmp(modelA, modelB) == 0) {
        fprintf(stderr, "FAIL: module detail did not follow the active speaker slot\n");
        failures++;
    }
    nc_monitor_set_active_speaker_slot(engine, 0);

    nc_engine_stop(engine);
    nc_engine_destroy(engine);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("bridge transport + monitor smoke OK\n");
    return 0;
}
