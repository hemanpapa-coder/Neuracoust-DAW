#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
int main(int argc, char** argv) {
    char report[2048] = {0};
    const bool ok = nc_remote_jitter_probe(argc > 1 ? argv[1] : "192.168.0.198:20002",
                                           report, sizeof(report));
    printf("ok=%d\n%s\n", ok ? 1 : 0, report);
    return ok ? 0 : 1;
}
