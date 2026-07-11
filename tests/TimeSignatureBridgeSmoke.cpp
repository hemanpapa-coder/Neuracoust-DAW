// Positional time-signature edits over the bridge: add sorts, replace-at-time, and the
// anchor at t=0 cannot be moved or deleted.
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

    // Fresh project carries a single anchor at t=0.
    check(nc_time_sig_count(e) >= 1, "anchor present");

    check(nc_time_sig_add(e, 16.0, 6, 8), "add 6/8 @16");
    check(nc_time_sig_add(e, 8.0, 3, 4), "add 3/4 @8 (out of order)");

    // Sorted by time: anchor(0), 3/4(8), 6/8(16).
    int n = nc_time_sig_count(e);
    check(n == 3, "count == 3");
    check(std::abs(nc_time_sig_time(e, 1) - 8.0) < 1e-6 && nc_time_sig_numerator(e, 1) == 3, "sorted: 3/4 at index 1");
    check(std::abs(nc_time_sig_time(e, 2) - 16.0) < 1e-6 && nc_time_sig_denominator(e, 2) == 8, "sorted: 6/8 at index 2");

    // Adding at an existing time replaces rather than duplicating.
    check(nc_time_sig_add(e, 8.0, 5, 4) && nc_time_sig_count(e) == 3, "replace at 8s, no dup");
    check(nc_time_sig_numerator(e, 1) == 5, "replaced to 5/4");

    // Move a change; anchor is immovable.
    check(nc_time_sig_move(e, 16.0, 0.25, 20.0), "move 6/8 to 20s");
    check(!nc_time_sig_move(e, 0.0, 0.25, 4.0), "anchor refuses move");

    // Delete a change; anchor is undeletable.
    check(nc_time_sig_delete(e, 8.0, 0.25), "delete 5/4");
    check(!nc_time_sig_delete(e, 0.0, 0.25), "anchor refuses delete");
    check(nc_time_sig_count(e) == 2, "count == 2 after delete");

    nc_engine_destroy(e);
    printf(failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
