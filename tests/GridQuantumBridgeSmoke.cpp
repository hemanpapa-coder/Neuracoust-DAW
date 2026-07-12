// Grid resolution: setting the edit mode to Grid and choosing a unit changes the snap
// quantum accordingly (at the default 120 BPM / 4/4).
#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
#include <cmath>

static int failures = 0;
static void near(double got, double want, const char* what) {
    bool ok = std::abs(got - want) < 1e-4;
    printf("%s %s: got %.4f want %.4f\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) ++failures;
}

int main() {
    NCEngine* e = nc_engine_create();
    nc_project_set_edit_mode(e, "Grid");

    nc_project_set_grid_unit(e, "1 beat");      near(nc_project_grid_quantum_seconds(e), 0.5, "1 beat");
    nc_project_set_grid_unit(e, "1 bar");       near(nc_project_grid_quantum_seconds(e), 2.0, "1 bar");
    nc_project_set_grid_unit(e, "1/4 beat");    near(nc_project_grid_quantum_seconds(e), 0.125, "1/4 beat");
    nc_project_set_grid_unit(e, "1/8 beat");    near(nc_project_grid_quantum_seconds(e), 0.0625, "1/8 beat");
    nc_project_set_grid_unit(e, "1/16 beat");   near(nc_project_grid_quantum_seconds(e), 0.03125, "1/16 beat");
    nc_project_set_grid_unit(e, "0.1s");        near(nc_project_grid_quantum_seconds(e), 0.1, "0.1s");

    nc_engine_destroy(e);
    printf(failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
