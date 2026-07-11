// Verifies the spectrum data reaches the bridge: start engine, tone on, render, read bins.
#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
#include <thread>
#include <chrono>

int main() {
    NCEngine* e = nc_engine_create();
    char err[256] = {0};
    nc_engine_start(e, err, sizeof err);
    nc_engine_set_test_tone_enabled(e, true);
    for (int i = 0; i < 60; ++i) {
        NCEngineStatus s;
        nc_engine_status(e, &s);   // caches lastSpectrumBins
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    int n = nc_spectrum_bin_count(e);
    printf("spectrum_bin_count=%d\n", n);
    if (n > 0) {
        float buf[1024] = {0};
        nc_spectrum_bins(e, buf, n < 1024 ? n : 1024);
        float maxv = 0; int maxi = 0;
        for (int i = 0; i < n && i < 1024; ++i) { if (buf[i] > maxv) { maxv = buf[i]; maxi = i; } }
        printf("max bin=%d value=%.3f\n", maxi, maxv);
    }
    nc_engine_destroy(e);
    return 0;
}
