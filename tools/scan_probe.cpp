// LAN/direct-link inventory scan CLI: prints every Neuracoust DSP server that answers the
// discovery broadcast — now sent on EVERY attached segment (direct cables included).
#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
int main() {
    char out[4096] = {0};
    nc_dsp_scan_lan(out, sizeof(out));
    std::printf("%s\n", out[0] ? out : "(no servers answered)");
    return 0;
}
