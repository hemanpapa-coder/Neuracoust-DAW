// Automation storage is generic: pan and a plug-in lane ("insert.<slot>.<paramId>") both
// round-trip through the same add/read/move/delete path, independently.
#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
#include <cmath>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    NCEngine* e = nc_engine_create();

    check(nc_automation_parameter_supported("track.volume"), "volume supported");
    check(nc_automation_parameter_supported("track.pan"), "pan supported");
    check(nc_automation_parameter_supported("insert.2.4242"), "plugin lane supported");
    check(!nc_automation_parameter_supported("bogus.param"), "bogus refused");

    const char* pluginLane = "insert.1.777";
    check(nc_track_automation_add(e, 0, pluginLane, 1.0, 0.2f), "add plugin pt @1s");
    check(nc_track_automation_add(e, 0, pluginLane, 3.0, 0.8f), "add plugin pt @3s");
    check(nc_track_automation_count(e, 0, pluginLane) == 2, "plugin lane has 2 pts");

    // Pan lane is a separate lane, unaffected by the plugin lane.
    check(nc_track_automation_add(e, 0, "track.pan", 2.0, -0.5f), "add pan pt");
    check(nc_track_automation_count(e, 0, "track.pan") == 1, "pan lane has 1 pt");
    check(nc_track_automation_count(e, 0, pluginLane) == 2, "plugin lane still 2 pts");

    // Values read back exactly.
    check(std::abs(nc_track_automation_value(e, 0, pluginLane, 0) - 0.2f) < 1e-4, "pt0 value 0.2");
    check(std::abs(nc_track_automation_value(e, 0, pluginLane, 1) - 0.8f) < 1e-4, "pt1 value 0.8");

    check(nc_track_automation_move(e, 0, pluginLane, 0, 1.0, 0.35f), "move pt0 value");
    check(std::abs(nc_track_automation_value(e, 0, pluginLane, 0) - 0.35f) < 1e-4, "pt0 moved to 0.35");

    check(nc_track_automation_delete(e, 0, pluginLane, 0), "delete pt0");
    check(nc_track_automation_count(e, 0, pluginLane) == 1, "plugin lane has 1 pt after delete");
    check(nc_track_automation_count(e, 0, "track.pan") == 1, "pan lane untouched by delete");

    // Applying automation with no loaded plug-in must not crash.
    nc_apply_plugin_automation(e, 2.0);
    check(true, "apply with no plugin is safe");

    nc_engine_destroy(e);
    printf(failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
