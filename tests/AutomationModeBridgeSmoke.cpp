// Automation modes: per-track mode round-trips; value_at evaluates a lane; the live write
// adds/updates points without a history step.
#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
#include <cmath>
#include <string>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    NCEngine* e = nc_engine_create();

    auto mode = [&]() {
        char b[32]; nc_track_automation_mode(e, 0, b, sizeof b); return std::string(b);
    };
    check(mode() == "read", "default mode is read");
    nc_track_set_automation_mode(e, 0, "latch");
    check(mode() == "latch", "mode set to latch");
    nc_track_set_automation_mode(e, 0, "bogus");
    check(mode() == "latch", "invalid mode ignored");
    nc_track_set_automation_mode(e, 0, "off");
    check(mode() == "off", "mode set to off");

    // A volume lane: value_at interpolates between points, falls back with no lane.
    check(std::abs(nc_track_automation_value_at(e, 0, "track.volume", 1.0, -3.0f) - (-3.0f)) < 1e-4,
          "value_at falls back with no points");
    check(nc_track_automation_add(e, 0, "track.volume", 0.0, -12.0f), "add pt @0s = -12");
    check(nc_track_automation_add(e, 0, "track.volume", 2.0, 0.0f), "add pt @2s = 0");
    check(std::abs(nc_track_automation_value_at(e, 0, "track.volume", 1.0, 0.0f) - (-6.0f)) < 1e-3,
          "value_at midpoint = -6");

    // Live write adds a pan point (no history step of its own).
    const int before = nc_track_automation_count(e, 0, "track.pan");
    check(nc_track_automation_write(e, 0, "track.pan", 1.0, 0.4f), "live-write pan pt");
    check(nc_track_automation_count(e, 0, "track.pan") == before + 1, "pan lane gained a point");
    check(std::abs(nc_track_automation_value_at(e, 0, "track.pan", 1.0, 0.0f) - 0.4f) < 1e-3,
          "written pan reads back");

    nc_engine_destroy(e);
    printf(failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
