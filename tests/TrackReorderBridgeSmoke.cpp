// Dragging a mixer channel reorders it: nc_track_move_near moves a track before/after
// another by name.
#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    NCEngine* e = nc_engine_create();
    nc_track_add_audio(e);          // ensure a few audio tracks exist
    nc_track_add_audio(e);
    nc_track_add_audio(e);

    int n = nc_track_count(e);
    check(n >= 3, "have >= 3 tracks");

    auto name = [&](int i) {
        static char buf[128];
        nc_track_name(e, i, buf, sizeof buf);
        return std::string(buf);
    };
    const std::string first = name(0), third = name(2);
    printf("before: 0=%s 2=%s\n", first.c_str(), third.c_str());

    // Move the first track to sit after the third.
    check(nc_track_move_near(e, first.c_str(), third.c_str(), true), "move first after third");

    // The moved track should now be at or past the third's old slot, and order changed.
    bool orderChanged = (name(0) != first);
    check(orderChanged, "order changed (first no longer at 0)");
    // The moved track still exists somewhere.
    bool stillPresent = false;
    for (int i = 0; i < nc_track_count(e); ++i) if (name(i) == first) stillPresent = true;
    check(stillPresent, "moved track still present");

    // Refuses a no-op (same source/target).
    check(!nc_track_move_near(e, third.c_str(), third.c_str(), true), "no-op refused");

    nc_engine_destroy(e);
    printf(failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
