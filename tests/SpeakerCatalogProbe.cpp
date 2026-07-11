#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>
int main() {
    int n = nc_speaker_model_count();
    printf("speaker_model_count=%d\n", n);
    char buf[256];
    for (int i = 0; i < 3 && i < n; ++i) { nc_speaker_model_name(i, buf, sizeof buf); printf("  [%d]=%s\n", i, buf); }
    int h = nc_headphone_model_count();
    printf("headphone_model_count=%d\n", h);
    return 0;
}
