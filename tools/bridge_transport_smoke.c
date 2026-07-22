// Drives the C facade the SwiftUI app uses: open the device, roll the transport,
// and confirm the playhead advances. Isolates engine/bridge faults from UI ones.

#include "bridge/NeuracoustEngineBridge.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
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
    printf("after 0.6s: transportRunning=%d  playbackSeconds=%.4f  trackMeters=%d\n",
           status.transportRunning, status.playbackSeconds, status.trackMeterCount);
    for (int i = 0; i < status.trackMeterCount && i < 8; ++i) {
        printf("   meter[%d] %-12s L=%.4f R=%.4f\n",
               i, status.trackMeterNames[i], status.trackPeakLeft[i], status.trackPeakRight[i]);
    }

    int failures = 0;
    if (!status.transportRunning) {
        fprintf(stderr, "FAIL: transport did not start\n");
        failures++;
    }
    if (status.playbackSeconds < 0.3) {
        fprintf(stderr, "FAIL: playhead did not advance (%.4f s)\n", status.playbackSeconds);
        failures++;
    }
    // Only loadProject seeds the DSP engine's meter arrays, and the DSP engine does
    // not exist until start(). Master and Monitor are excluded by the engine.
    if (status.trackMeterCount != 2) {
        fprintf(stderr, "FAIL: expected 2 track meters (Audio 1, Audio 2), got %d\n",
                status.trackMeterCount);
        failures++;
    } else if (strcmp(status.trackMeterNames[0], "Audio 1") != 0) {
        fprintf(stderr, "FAIL: meter[0] is '%s', expected 'Audio 1'\n", status.trackMeterNames[0]);
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

    // ---- tracks / mixer ----
    const int trackCount = nc_track_count(engine);
    printf("tracks: %d\n", trackCount);
    if (trackCount < 3) {
        fprintf(stderr, "FAIL: default project should carry audio + master + monitor tracks\n");
        failures++;
    }

    char trackName[128] = {0};
    nc_track_name(engine, 0, trackName, sizeof(trackName));
    if (strcmp(trackName, "Audio 1") != 0) {
        fprintf(stderr, "FAIL: track 0 is '%s', expected 'Audio 1'\n", trackName);
        failures++;
    }

    nc_track_set_volume_db(engine, 0, -12.0f);
    if (nc_track_volume_db(engine, 0) < -12.5f || nc_track_volume_db(engine, 0) > -11.5f) {
        fprintf(stderr, "FAIL: track volume readback %.2f, expected -12\n", nc_track_volume_db(engine, 0));
        failures++;
    }
    nc_track_set_volume_db(engine, 0, 0.0f);

    nc_track_set_pan(engine, 0, 0.5f);
    if (nc_track_pan(engine, 0) < 0.45f || nc_track_pan(engine, 0) > 0.55f) {
        fprintf(stderr, "FAIL: track pan readback %.2f, expected 0.5\n", nc_track_pan(engine, 0));
        failures++;
    }
    nc_track_set_pan(engine, 0, 0.0f);

    nc_track_set_muted(engine, 0, true);
    if (!nc_track_muted(engine, 0)) {
        fprintf(stderr, "FAIL: track mute did not latch\n");
        failures++;
    }
    nc_track_set_muted(engine, 0, false);

    // Solo is additive, not exclusive: soloing two tracks leaves both soloed.
    nc_track_set_solo(engine, 0, true);
    nc_track_set_solo(engine, 1, true);
    if (!nc_track_solo(engine, 0) || !nc_track_solo(engine, 1)) {
        fprintf(stderr, "FAIL: solo should be additive across tracks\n");
        failures++;
    }
    nc_track_set_solo(engine, 0, false);
    nc_track_set_solo(engine, 1, false);

    // ---- plugin browser + inserts ----
    const int scanned = nc_plugin_scan(engine);
    printf("plugins scanned: %d\n", scanned);
    if (scanned <= 0) {
        printf("(no plug-ins installed — skipping insert checks)\n");
    } else {
        printf("facets: brands=%d categories=%d formats=%d\n",
               nc_plugin_facet_count(engine, NC_FACET_BRAND),
               nc_plugin_facet_count(engine, NC_FACET_CATEGORY),
               nc_plugin_facet_count(engine, NC_FACET_FORMAT));

        // The FX-insert browser excludes instruments (picking one is rejected anyway).
        const int instruments = nc_plugin_apply_filter(engine, "", "", "Instrument", "VST3", "");
        const int withoutInstruments = nc_plugin_apply_filter(engine, "", "", "", "VST3", "Instrument");
        const int everything = nc_plugin_apply_filter(engine, "", "", "", "VST3", "");
        if (withoutInstruments + instruments != everything) {
            fprintf(stderr, "FAIL: exclude filter dropped %d, expected the %d instruments\n",
                    everything - withoutInstruments, instruments);
            failures++;
        }
        nc_plugin_apply_filter(engine, "", "", "", "VST3", "Instrument");
        for (int i = 0; i < withoutInstruments; ++i) {
            char cat[128] = {0};
            nc_plugin_category(engine, i, cat, sizeof cat);
            if (strcmp(cat, "Instrument") == 0) {
                fprintf(stderr, "FAIL: an Instrument survived the exclude filter\n");
                failures++;
                break;
            }
        }

        // Narrow to one VST3 so the insert we add is one the engine can host.
        const int matches = nc_plugin_apply_filter(engine, "", "", "", "VST3", "");
        printf("VST3 matches: %d\n", matches);
        if (matches <= 0) {
            fprintf(stderr, "FAIL: no VST3 plug-ins after filtering\n");
            failures++;
        } else {
            char pluginName[128] = {0};
            nc_plugin_name(engine, 0, pluginName, sizeof(pluginName));

            const int before = nc_track_insert_count(engine, 0);
            if (!nc_track_add_insert(engine, 0, 0)) {
                fprintf(stderr, "FAIL: could not add '%s' to track 0\n", pluginName);
                failures++;
            } else {
                const int after = nc_track_insert_count(engine, 0);
                char slotName[128] = {0};
                char badge[128] = {0};
                nc_track_insert_name(engine, 0, 0, slotName, sizeof(slotName));
                nc_track_insert_mode_badge(engine, 0, 0, badge, sizeof(badge));
                printf("insert added: '%s' badge=%s  slots %d -> %d\n",
                       slotName, badge, before, after);

                if (strcmp(slotName, pluginName) != 0) {
                    fprintf(stderr, "FAIL: slot holds '%s', expected '%s'\n", slotName, pluginName);
                    failures++;
                }
                // Core isolation is on in the default project, so a loaded VST3
                // must land on the isolated core rather than the audio thread.
                if (strcmp(badge, "INT") != 0) {
                    fprintf(stderr, "FAIL: badge is '%s', expected 'INT'\n", badge);
                    failures++;
                }

                // Prove the engine really hosts it: roll the transport and ask how
                // many inserts it is running.
                nc_engine_set_transport_running(engine, true);
                usleep(1500 * 1000);
                nc_engine_status(engine, &status);
                nc_engine_set_transport_running(engine, false);
                printf("active inserts: realtime=%d remote=%d offline=%d\n",
                       status.activeRealtimeVst3TrackInserts,
                       status.activeRemoteDspTrackInserts,
                       status.activeOfflineVst3TrackInserts);
                const int hosted = status.activeRealtimeVst3TrackInserts +
                                   status.activeRemoteDspTrackInserts +
                                   status.activeOfflineVst3TrackInserts;
                if (hosted < 1) {
                    fprintf(stderr, "FAIL: engine hosts no insert after adding one\n");
                    failures++;
                }

                // What the editor host needs on its command line.
                char pluginPath[512] = {0};
                nc_track_insert_plugin_path(engine, 0, 0, pluginPath, sizeof(pluginPath));
                if (pluginPath[0] == '\0') {
                    fprintf(stderr, "FAIL: insert has no plug-in path for the editor host\n");
                    failures++;
                }

                // A knob turn in the editor arrives as one of these.
                const int paramsBefore = nc_track_insert_param_count(engine, 0, 0);
                if (!nc_track_set_vst3_parameter(engine, 0, 0, 7, "Gain", 0.75)) {
                    fprintf(stderr, "FAIL: could not set VST3 parameter 7\n");
                    failures++;
                } else if (nc_track_insert_param_count(engine, 0, 0) != paramsBefore + 1) {
                    fprintf(stderr, "FAIL: parameter 7 was not appended\n");
                    failures++;
                }
                // The same id again updates in place rather than appending a duplicate.
                nc_track_set_vst3_parameter(engine, 0, 0, 7, "Gain", 0.25);
                if (nc_track_insert_param_count(engine, 0, 0) != paramsBefore + 1) {
                    fprintf(stderr, "FAIL: re-sending parameter 7 appended a duplicate\n");
                    failures++;
                }
                const int last = nc_track_insert_param_count(engine, 0, 0) - 1;
                if (nc_track_insert_param_id(engine, 0, 0, last) != 7 ||
                    nc_track_insert_param_value(engine, 0, 0, last) != 0.25) {
                    fprintf(stderr, "FAIL: parameter 7 reads back as id=%u value=%f\n",
                            nc_track_insert_param_id(engine, 0, 0, last),
                            nc_track_insert_param_value(engine, 0, 0, last));
                    failures++;
                }
                // Out-of-range values must never reach a plug-in.
                nc_track_set_vst3_parameter(engine, 0, 0, 8, "Wild", 4.5);
                const int wild = nc_track_insert_param_count(engine, 0, 0) - 1;
                if (nc_track_insert_param_value(engine, 0, 0, wild) != 1.0) {
                    fprintf(stderr, "FAIL: parameter 8 was not clamped to 1.0\n");
                    failures++;
                }
                if (nc_track_set_vst3_parameter(engine, 0, 9, 1, "None", 0.5)) {
                    fprintf(stderr, "FAIL: setting a parameter on an empty slot succeeded\n");
                    failures++;
                }
                // A nameless edit from the editor still gets a label.
                nc_track_set_vst3_parameter(engine, 0, 0, 11, NULL, 0.5);
                const int nameless = nc_track_insert_param_count(engine, 0, 0) - 1;
                char paramName[128] = {0};
                nc_track_insert_param_name(engine, 0, 0, nameless, paramName, sizeof(paramName));
                if (strcmp(paramName, "Param 11") != 0) {
                    fprintf(stderr, "FAIL: nameless parameter labelled '%s'\n", paramName);
                    failures++;
                }
                printf("vst3 parameters: %d stored on slot 0\n",
                       nc_track_insert_param_count(engine, 0, 0));

                nc_track_set_insert_bypassed(engine, 0, 0, true);
                if (!nc_track_insert_bypassed(engine, 0, 0)) {
                    fprintf(stderr, "FAIL: insert bypass did not latch\n");
                    failures++;
                }

                if (!nc_track_remove_insert(engine, 0, 0)) {
                    fprintf(stderr, "FAIL: could not remove the insert\n");
                    failures++;
                }
            }
        }
    }

    // ---- history + autosave ----
    {
        // Autosave only fires for a document that has a home on disk.
        const char* projectPath = "/tmp/neuracoust-smoke-project.ndaw";
        const char* autosavePath = "/tmp/neuracoust-smoke-project.ndaw.autosave";
        remove(autosavePath);
        nc_project_set_path(engine, projectPath);
        // Earlier sections of this test edited the document; start from a clean slate.
        nc_history_reset(engine);

        if (nc_project_dirty(engine)) {
            fprintf(stderr, "FAIL: a fresh document should not be dirty\n");
            failures++;
        }
        if (nc_history_can_undo(engine)) {
            fprintf(stderr, "FAIL: a fresh document should have no history\n");
            failures++;
        }

        // A discrete edit records a step on its own.
        nc_track_set_muted(engine, 0, true);
        if (!nc_history_can_undo(engine) || !nc_project_dirty(engine)) {
            fprintf(stderr, "FAIL: muting did not record a step\n");
            failures++;
        }
        char stepName[128] = {0};
        nc_history_undo_step_name(engine, stepName, sizeof(stepName));
        printf("history: depth=%d step='%s' dirty=%d\n",
               nc_history_undo_depth(engine), stepName, nc_project_dirty(engine));
        if (strcmp(stepName, "Mute") != 0) {
            fprintf(stderr, "FAIL: step name is '%s', expected 'Mute'\n", stepName);
            failures++;
        }

        FILE* autosave = fopen(autosavePath, "r");
        if (autosave == NULL) {
            fprintf(stderr, "FAIL: no autosave file at %s\n", autosavePath);
            failures++;
        } else {
            fclose(autosave);
            printf("autosave written to %s\n", autosavePath);
        }

        // A fader drag is continuous: many set calls, then one recorded step.
        const int depthBefore = nc_history_undo_depth(engine);
        for (int frame = 0; frame < 30; ++frame) {
            nc_track_set_volume_db(engine, 0, -1.0f * (float)frame);
        }
        if (nc_history_undo_depth(engine) != depthBefore) {
            fprintf(stderr, "FAIL: dragging a fader recorded %d steps by itself\n",
                    nc_history_undo_depth(engine) - depthBefore);
            failures++;
        }
        if (!nc_history_record_gesture(engine, "Volume")) {
            fprintf(stderr, "FAIL: the gesture recorded no step\n");
            failures++;
        }
        if (nc_history_undo_depth(engine) != depthBefore + 1) {
            fprintf(stderr, "FAIL: a drag should record exactly one step\n");
            failures++;
        }
        // Recording again with nothing changed must not push an empty step.
        if (nc_history_record_gesture(engine, "Volume")) {
            fprintf(stderr, "FAIL: an unchanged document recorded a step\n");
            failures++;
        }

        // Undo the volume, then the mute.
        if (!nc_history_undo(engine) || nc_track_volume_db(engine, 0) != 0.0f) {
            fprintf(stderr, "FAIL: undo did not restore the volume (%.2f)\n",
                    nc_track_volume_db(engine, 0));
            failures++;
        }
        if (!nc_history_undo(engine) || nc_track_muted(engine, 0)) {
            fprintf(stderr, "FAIL: undo did not restore the mute\n");
            failures++;
        }
        if (nc_project_dirty(engine)) {
            fprintf(stderr, "FAIL: undoing back to the start should be clean\n");
            failures++;
        }
        if (nc_history_can_undo(engine)) {
            fprintf(stderr, "FAIL: the undo stack should be empty\n");
            failures++;
        }

        // Redo replays them.
        if (!nc_history_redo(engine) || !nc_track_muted(engine, 0)) {
            fprintf(stderr, "FAIL: redo did not reapply the mute\n");
            failures++;
        }
        if (!nc_history_redo(engine) || nc_track_volume_db(engine, 0) > -28.0f) {
            fprintf(stderr, "FAIL: redo did not reapply the volume (%.2f)\n",
                    nc_track_volume_db(engine, 0));
            failures++;
        }

        // A new edit after undo discards the redo stack.
        nc_history_undo(engine);
        if (!nc_history_can_redo(engine)) {
            fprintf(stderr, "FAIL: undo should enable redo\n");
            failures++;
        }
        nc_track_set_solo(engine, 1, true);
        if (nc_history_can_redo(engine)) {
            fprintf(stderr, "FAIL: a new edit should discard the redo stack\n");
            failures++;
        }
        nc_track_set_solo(engine, 1, false);

        char autosaveError[128] = {0};
        nc_project_autosave_error(engine, autosaveError, sizeof(autosaveError));
        if (strlen(autosaveError) > 0) {
            fprintf(stderr, "FAIL: autosave error '%s'\n", autosaveError);
            failures++;
        }

        // Leave nothing behind.
        nc_project_set_path(engine, "");
        remove(autosavePath);
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

    // --- monitor listen state: the cycles ported from the old UI ---------------
    {
        char mode[16] = {0};
        // Stereo button cycles Stereo -> Left -> Right -> Stereo.
        nc_monitor_set_listen_mode(engine, "LR");
        nc_monitor_cycle_stereo(engine);
        nc_monitor_listen_mode(engine, mode, sizeof(mode));
        if (strcmp(mode, "L") != 0) { fprintf(stderr, "FAIL: stereo cycle LR->%s, expected L\n", mode); failures++; }
        nc_monitor_cycle_stereo(engine);
        nc_monitor_listen_mode(engine, mode, sizeof(mode));
        if (strcmp(mode, "R") != 0) { fprintf(stderr, "FAIL: stereo cycle L->%s, expected R\n", mode); failures++; }
        nc_monitor_cycle_stereo(engine);
        nc_monitor_listen_mode(engine, mode, sizeof(mode));
        if (strcmp(mode, "LR") != 0) { fprintf(stderr, "FAIL: stereo cycle R->%s, expected LR\n", mode); failures++; }

        // Mono button turns mono on and cycles the summed side.
        nc_monitor_cycle_mono(engine);
        if (!nc_monitor_mono(engine)) { fprintf(stderr, "FAIL: mono cycle did not engage mono\n"); failures++; }

        // M/S toggle: on gives Mid, and the Stereo/Mono buttons then pick Mid/Side.
        nc_monitor_set_listen_mode(engine, "LR");
        nc_monitor_set_mono(engine, 0);
        nc_monitor_toggle_mid_side(engine);
        if (!nc_monitor_mid_side(engine)) { fprintf(stderr, "FAIL: M/S toggle did not engage\n"); failures++; }
        nc_monitor_listen_mode(engine, mode, sizeof(mode));
        if (strcmp(mode, "M") != 0) { fprintf(stderr, "FAIL: M/S on gave %s, expected M\n", mode); failures++; }
        nc_monitor_cycle_mono(engine);   // in M/S, the mono button selects Side
        nc_monitor_listen_mode(engine, mode, sizeof(mode));
        if (strcmp(mode, "S") != 0) { fprintf(stderr, "FAIL: M/S mono button gave %s, expected S\n", mode); failures++; }
        nc_monitor_toggle_mid_side(engine);  // off returns to stereo
        if (nc_monitor_mid_side(engine)) { fprintf(stderr, "FAIL: M/S toggle off still M/S\n"); failures++; }

        // Phase button cycles Off -> ØL -> ØR -> ØLR -> Off.
        while (nc_monitor_invert_left(engine) || nc_monitor_invert_right(engine)) nc_monitor_cycle_phase(engine);
        nc_monitor_cycle_phase(engine);
        if (!nc_monitor_invert_left(engine) || nc_monitor_invert_right(engine)) { fprintf(stderr, "FAIL: phase step 1 not ØL\n"); failures++; }
        nc_monitor_cycle_phase(engine);
        if (nc_monitor_invert_left(engine) || !nc_monitor_invert_right(engine)) { fprintf(stderr, "FAIL: phase step 2 not ØR\n"); failures++; }
        nc_monitor_cycle_phase(engine);
        if (!nc_monitor_invert_left(engine) || !nc_monitor_invert_right(engine)) { fprintf(stderr, "FAIL: phase step 3 not ØLR\n"); failures++; }
        nc_monitor_cycle_phase(engine);
        if (nc_monitor_invert_left(engine) || nc_monitor_invert_right(engine)) { fprintf(stderr, "FAIL: phase step 4 not Off\n"); failures++; }
        printf("monitor listen state cycles OK\n");
    }

    // --- DSP core allocation ---------------------------------------------------
    {
        // Default: isolation on, 4 cores.
        if (!nc_dsp_core_isolation(engine)) { fprintf(stderr, "FAIL: core isolation off by default\n"); failures++; }
        if (nc_dsp_core_count(engine) != 4) { fprintf(stderr, "FAIL: default core count %d, expected 4\n", nc_dsp_core_count(engine)); failures++; }

        nc_dsp_set_core_count(engine, 8);
        if (nc_dsp_core_count(engine) != 8) { fprintf(stderr, "FAIL: set 8 -> %d\n", nc_dsp_core_count(engine)); failures++; }
        nc_dsp_set_core_count(engine, 99);
        if (nc_dsp_core_count(engine) != 16) { fprintf(stderr, "FAIL: 99 not clamped to 16 (%d)\n", nc_dsp_core_count(engine)); failures++; }

        // No isolation floor: the user can drop the internal reserve as low as 1 even with
        // isolation on (was floored at 4; the floor was removed on request so a light session can
        // hand cores back to the OS).
        nc_dsp_set_core_count(engine, 1);
        if (nc_dsp_core_count(engine) != 1) { fprintf(stderr, "FAIL: isolation-on count did not reach 1 (%d)\n", nc_dsp_core_count(engine)); failures++; }

        // Off, it can also go to 1.
        nc_dsp_set_core_isolation(engine, false);
        nc_dsp_set_core_count(engine, 1);
        if (nc_dsp_core_count(engine) != 1) { fprintf(stderr, "FAIL: isolation off floor (%d)\n", nc_dsp_core_count(engine)); failures++; }

        // Re-enabling isolation leaves the chosen count as-is (no floor to lift it to).
        nc_dsp_set_core_isolation(engine, true);
        if (nc_dsp_core_count(engine) != 1) { fprintf(stderr, "FAIL: re-enable changed count (%d)\n", nc_dsp_core_count(engine)); failures++; }
        printf("dsp core allocation OK\n");
    }

    // --- External DSP Manager core reserve -------------------------------------
    {
        // Default 4, independent of the internal reserve and its isolation floor.
        if (nc_dsp_external_core_count(engine) != 4) { fprintf(stderr, "FAIL: default external core %d, expected 4\n", nc_dsp_external_core_count(engine)); failures++; }
        nc_dsp_set_external_core_count(engine, 8);
        if (nc_dsp_external_core_count(engine) != 8) { fprintf(stderr, "FAIL: external set 8 -> %d\n", nc_dsp_external_core_count(engine)); failures++; }
        // No isolation floor here: the external reserve can go to 1 even with internal isolation on.
        nc_dsp_set_external_core_count(engine, 1);
        if (nc_dsp_external_core_count(engine) != 1) { fprintf(stderr, "FAIL: external floor to 1 (%d)\n", nc_dsp_external_core_count(engine)); failures++; }
        nc_dsp_set_external_core_count(engine, 99);
        if (nc_dsp_external_core_count(engine) != 16) { fprintf(stderr, "FAIL: external 99 not clamped to 16 (%d)\n", nc_dsp_external_core_count(engine)); failures++; }
        // Internal reserve is unchanged by external edits (left at 1 from the block above).
        if (nc_dsp_core_count(engine) != 1) { fprintf(stderr, "FAIL: external edit disturbed internal core (%d)\n", nc_dsp_core_count(engine)); failures++; }
        printf("external dsp core reserve OK\n");
    }

    // --- Physical monitor models + speaker/headphone exclusivity ----------------
    {
        char buf[128];
        if (nc_headphone_model_count() < 10) { fprintf(stderr, "FAIL: headphone catalog too small (%d)\n", nc_headphone_model_count()); failures++; }
        if (!nc_monitor_output_exclusive(engine)) { fprintf(stderr, "FAIL: exclusive not on by default\n"); failures++; }
        nc_monitor_set_physical_speaker_model(engine, "Genelec 8040B (NF)");
        nc_monitor_physical_speaker_model(engine, buf, sizeof buf);
        if (strcmp(buf, "Genelec 8040B (NF)") != 0) { fprintf(stderr, "FAIL: physical speaker '%s'\n", buf); failures++; }
        nc_monitor_set_physical_headphone_model(engine, "Sennheiser HD 650");
        nc_monitor_physical_headphone_model(engine, buf, sizeof buf);
        if (strcmp(buf, "Sennheiser HD 650") != 0) { fprintf(stderr, "FAIL: physical headphone '%s'\n", buf); failures++; }
        nc_monitor_set_output_exclusive(engine, false);
        if (nc_monitor_output_exclusive(engine)) { fprintf(stderr, "FAIL: exclusive stayed on\n"); failures++; }
        printf("physical monitor models OK\n");
    }

    // --- Master auto fade-out ---------------------------------------------------
    {
        char buf[64];
        if (nc_master_auto_fade_seconds(engine) != 0.0) { fprintf(stderr, "FAIL: auto fade not off by default\n"); failures++; }
        nc_master_set_auto_fade_seconds(engine, 5.0);
        if (nc_master_auto_fade_seconds(engine) != 5.0) { fprintf(stderr, "FAIL: auto fade seconds %.1f\n", nc_master_auto_fade_seconds(engine)); failures++; }
        nc_master_set_auto_fade_curve(engine, "linear");
        nc_master_auto_fade_curve(engine, buf, sizeof buf);
        if (strcmp(buf, "linear") != 0) { fprintf(stderr, "FAIL: auto fade curve '%s'\n", buf); failures++; }
        // The curve amplitude falls from ~1 at the start to ~0 at the end.
        if (nc_auto_fade_amplitude("linear", 0.0) < 0.99f) { fprintf(stderr, "FAIL: fade start amp\n"); failures++; }
        if (nc_auto_fade_amplitude("linear", 1.0) > 0.01f) { fprintf(stderr, "FAIL: fade end amp\n"); failures++; }
        nc_master_set_auto_fade_seconds(engine, 0.0);
        if (nc_master_auto_fade_seconds(engine) != 0.0) { fprintf(stderr, "FAIL: auto fade off\n"); failures++; }
        printf("master auto fade-out OK\n");
    }

    // --- Speaker set: model catalog + model/output setters ---------------------
    {
        char buf[256];
        if (nc_speaker_model_count() < 100) { fprintf(stderr, "FAIL: model catalog too small (%d)\n", nc_speaker_model_count()); failures++; }
        if (nc_speaker_output_route_count() != 18) { fprintf(stderr, "FAIL: output routes %d, expected 18\n", nc_speaker_output_route_count()); failures++; }
        nc_speaker_output_route(0, buf, sizeof buf);
        if (strcmp(buf, "None") != 0) { fprintf(stderr, "FAIL: route[0] '%s', expected None\n", buf); failures++; }

        // Assign a model to slot A: stored as "Speaker A: <name>", output stays None.
        nc_monitor_set_speaker_model(engine, 0, "Genelec 8040B (NF)");
        nc_monitor_speaker_model(engine, 0, buf, sizeof buf);
        if (strcmp(buf, "Speaker A: Genelec 8040B (NF)") != 0) { fprintf(stderr, "FAIL: model set -> '%s'\n", buf); failures++; }
        nc_monitor_speaker_output(engine, 0, buf, sizeof buf);
        if (strcmp(buf, "None") != 0) { fprintf(stderr, "FAIL: model set left output '%s', expected None\n", buf); failures++; }

        // Routing slot A to a physical pair forces its model to Flat and room EQ off.
        nc_monitor_set_speaker_room_eq(engine, 0, true);
        nc_monitor_set_speaker_output(engine, 0, "Output 3-4");
        nc_monitor_speaker_output(engine, 0, buf, sizeof buf);
        if (strcmp(buf, "Output 3-4") != 0) { fprintf(stderr, "FAIL: output set -> '%s'\n", buf); failures++; }
        nc_monitor_speaker_model(engine, 0, buf, sizeof buf);
        if (strcmp(buf, "Speaker A: Flat") != 0) { fprintf(stderr, "FAIL: physical output did not force Flat (%s)\n", buf); failures++; }
        if (nc_monitor_speaker_room_eq(engine, 0)) { fprintf(stderr, "FAIL: physical output left room EQ on\n"); failures++; }

        // Slot B is untouched by slot A edits.
        nc_monitor_speaker_output(engine, 1, buf, sizeof buf);
        if (strcmp(buf, "Output 3-4") == 0) { fprintf(stderr, "FAIL: slot A output leaked to slot B\n"); failures++; }
        printf("speaker set model/output OK\n");
    }

    // --- Instrument editor reverse monitor: lifecycle + shm handoff -------------
    {
        char shm[128] = {0};
        int maxBlock = 0;
        double rate = 0.0;
        // An audio track never gets the ring.
        if (nc_track_instrument_editor_opened(engine, 0, shm, sizeof shm, &maxBlock, &rate)) {
            fprintf(stderr, "FAIL: editor monitor opened on an audio track\n"); failures++;
        }
        const int inst = nc_track_add_instrument(engine);
        if (inst < 0) { fprintf(stderr, "FAIL: could not add an instrument track\n"); failures++; }
        if (!nc_track_instrument_editor_opened(engine, inst, shm, sizeof shm, &maxBlock, &rate)) {
            fprintf(stderr, "FAIL: editor monitor did not open on an instrument track\n"); failures++;
        }
        if (shm[0] != '/' || maxBlock <= 0 || rate <= 1000.0) {
            fprintf(stderr, "FAIL: editor monitor handoff ('%s', %d, %.0f)\n", shm, maxBlock, rate); failures++;
        }
        // The segment must exist for the editor host to attach to.
        int fd = shm_open(shm, O_RDWR, 0600);
        if (fd < 0) { fprintf(stderr, "FAIL: monitor shm '%s' missing after open\n", shm); failures++; }
        else { close(fd); }
        // Reopening (a newer editor) recreates it; closing tears down and unlinks.
        if (!nc_track_instrument_editor_opened(engine, inst, shm, sizeof shm, &maxBlock, &rate)) {
            fprintf(stderr, "FAIL: editor monitor reopen failed\n"); failures++;
        }
        nc_track_instrument_editor_closed(engine, inst);
        fd = shm_open(shm, O_RDWR, 0600);
        if (fd >= 0) { fprintf(stderr, "FAIL: monitor shm survived close\n"); failures++; close(fd); }
        printf("instrument editor monitor lifecycle OK\n");
    }

    // Controller (CC) + pitch-bend lane round trip: add, filter-by-controller, get, move, delete.
    {
        char region[128] = {0};
        const int mtrack = nc_track_add_instrument(engine);
        if (mtrack < 0 ||
            !nc_midi_region_add(engine, mtrack, 0.0, 4.0, region, sizeof region) || region[0] == 0) {
            fprintf(stderr, "FAIL: could not add MIDI region for CC test\n"); failures++;
        }
        char id1[128] = {0};
        if (!nc_midi_cc_add(engine, region, 1, 0.5, 100, id1, sizeof id1) || id1[0] == 0) {
            fprintf(stderr, "FAIL: nc_midi_cc_add cc1\n"); failures++;
        }
        char id2[128] = {0};
        if (!nc_midi_cc_add(engine, region, 10, 1.0, 64, id2, sizeof id2)) {
            fprintf(stderr, "FAIL: nc_midi_cc_add cc10\n"); failures++;
        }
        // Each lane only sees its own controller number.
        if (nc_midi_cc_count(engine, region, 1) != 1 || nc_midi_cc_count(engine, region, 10) != 1) {
            fprintf(stderr, "FAIL: cc counts (%d, %d)\n",
                    nc_midi_cc_count(engine, region, 1), nc_midi_cc_count(engine, region, 10)); failures++;
        }
        char gotId[128] = {0}; double gotBeat = 0; int gotVal = 0;
        if (!nc_midi_cc_get(engine, region, 1, 0, gotId, sizeof gotId, &gotBeat, &gotVal)
            || gotVal != 100 || gotBeat < 0.49 || gotBeat > 0.51) {
            fprintf(stderr, "FAIL: cc1 get (%s,%.3f,%d)\n", gotId, gotBeat, gotVal); failures++;
        }
        if (!nc_midi_cc_move(engine, region, id1, 2.0, 40)) {
            fprintf(stderr, "FAIL: cc move\n"); failures++;
        }
        nc_midi_cc_get(engine, region, 1, 0, gotId, sizeof gotId, &gotBeat, &gotVal);
        if (gotVal != 40 || gotBeat < 1.99 || gotBeat > 2.01) {
            fprintf(stderr, "FAIL: cc after move (%.3f,%d)\n", gotBeat, gotVal); failures++;
        }
        if (!nc_midi_cc_delete(engine, region, id1) || nc_midi_cc_count(engine, region, 1) != 0) {
            fprintf(stderr, "FAIL: cc delete\n"); failures++;
        }
        char pb[128] = {0};
        if (!nc_midi_pb_add(engine, region, 0.25, 12000, pb, sizeof pb)
            || nc_midi_pb_count(engine, region) != 1) {
            fprintf(stderr, "FAIL: pb add/count\n"); failures++;
        }
        if (!nc_midi_pb_move(engine, region, pb, 0.75, 4000)) {
            fprintf(stderr, "FAIL: pb move\n"); failures++;
        }
        double pbBeat = 0; int pbVal = 0; char pbId[128] = {0};
        nc_midi_pb_get(engine, region, 0, pbId, sizeof pbId, &pbBeat, &pbVal);
        if (pbVal != 4000 || pbBeat < 0.74 || pbBeat > 0.76) {
            fprintf(stderr, "FAIL: pb after move (%.3f,%d)\n", pbBeat, pbVal); failures++;
        }
        if (!nc_midi_pb_delete(engine, region, pb) || nc_midi_pb_count(engine, region) != 0) {
            fprintf(stderr, "FAIL: pb delete\n"); failures++;
        }
        printf("controller (CC + pitch bend) lane round trip OK\n");
    }

    // Single monitor EQ, context-driven (the "one EQ, values swap" design). nc_monitor_eq_sync
    // imposes a measured model's curve, and clears the bands when there is neither a curve nor
    // room tuning — so a physical route or a non-measured model leaves the one EQ flat.
    {
        int measuredCount = nc_virtual_monitor_count(engine);
        if (measuredCount <= 0) {
            fprintf(stderr, "FAIL: no measured speaker profiles for eq sync\n"); failures++;
        } else {
            char measured[256] = {0};
            nc_virtual_monitor_name(engine, 0, measured, sizeof measured);
            // A measured slot model → the single EQ picks up bands.
            nc_monitor_eq_sync(engine, measured, "", false);
            if (nc_monitor_eq_band_count(engine) <= 0) {
                fprintf(stderr, "FAIL: eq sync '%s' produced no bands\n", measured); failures++;
            }
            // Empty slot + empty correction, no room → the single EQ is cleared flat.
            nc_monitor_eq_sync(engine, "", "", false);
            if (nc_monitor_eq_band_count(engine) != 0) {
                fprintf(stderr, "FAIL: eq sync empty left %d bands\n",
                        nc_monitor_eq_band_count(engine)); failures++;
            }
            printf("single monitor EQ context sync OK (model '%s')\n", measured);
        }
    }

    // --- Built-in test signal generator (track source) -------------------------------------------
    {
        const int t = 0;   // track 0 exists in the default project
        if (nc_track_test_signal_generator_slot(engine, t) != -1) {
            fprintf(stderr, "FAIL: generator present before add\n"); failures++;
        }
        if (!nc_track_add_test_signal_generator(engine, t)) {
            fprintf(stderr, "FAIL: could not add signal generator\n"); failures++;
        }
        if (nc_track_test_signal_generator_slot(engine, t) < 0) {
            fprintf(stderr, "FAIL: generator not present after add\n"); failures++;
        }
        // Defaults: enabled, sine, 1 kHz, -6 dB, stereo.
        if (!nc_track_test_signal_enabled(engine, t)) { fprintf(stderr, "FAIL: gen not enabled by default\n"); failures++; }
        if (nc_track_test_signal_waveform(engine, t) != 0) { fprintf(stderr, "FAIL: default waveform not sine\n"); failures++; }
        double f0 = nc_track_test_signal_frequency_hz(engine, t);
        if (f0 < 980.0 || f0 > 1020.0) { fprintf(stderr, "FAIL: default freq %.1f not ~1kHz\n", f0); failures++; }
        // Real-unit round-trips.
        nc_track_test_signal_set_frequency_hz(engine, t, 440.0);
        double f1 = nc_track_test_signal_frequency_hz(engine, t);
        if (f1 < 436.0 || f1 > 444.0) { fprintf(stderr, "FAIL: freq round-trip 440 -> %.2f\n", f1); failures++; }
        nc_track_test_signal_set_level_db(engine, t, -12.0);
        double lv = nc_track_test_signal_level_db(engine, t);
        if (lv < -12.5 || lv > -11.5) { fprintf(stderr, "FAIL: level round-trip -12 -> %.2f\n", lv); failures++; }
        nc_track_test_signal_set_waveform(engine, t, 6);   // Sweep
        if (nc_track_test_signal_waveform(engine, t) != 6) { fprintf(stderr, "FAIL: waveform round-trip\n"); failures++; }
        nc_track_test_signal_set_channel(engine, t, 2);    // Right
        if (nc_track_test_signal_channel(engine, t) != 2) { fprintf(stderr, "FAIL: channel round-trip\n"); failures++; }
        nc_track_test_signal_set_polarity(engine, t, true);
        if (!nc_track_test_signal_polarity(engine, t)) { fprintf(stderr, "FAIL: polarity round-trip\n"); failures++; }
        // Adding again is idempotent (one per track).
        nc_track_add_test_signal_generator(engine, t);
        if (nc_track_remove_test_signal_generator(engine, t) &&
            nc_track_test_signal_generator_slot(engine, t) != -1) {
            fprintf(stderr, "FAIL: generator still present after remove\n"); failures++;
        }
        printf("test signal generator OK\n");
    }

    nc_engine_stop(engine);
    nc_engine_destroy(engine);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("bridge transport + monitor smoke OK\n");
    return 0;
}
