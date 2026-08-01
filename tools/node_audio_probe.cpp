// "Answers" and "carries audio" are different questions, and only the second one matters before
// the NDS switch engages. This prints both for each address given, which is what the switch now
// asks before it will turn on.
//
//   neuracoust_node_audio_probe 192.168.0.198:20002 studio.local
#include "bridge/NeuracoustEngineBridge.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("사용법: %s <주소> [주소 ...]\n", argv[0]);
        return 2;
    }
    int usable = 0;
    for (int i = 1; i < argc; ++i) {
        NCRemoteNodeInfo info{};
        const int answered = nc_dsp_probe_node_info(argv[i], 120, &info);
        const int carried = nc_dsp_probe_node_audio(argv[i], 60, 8);
        usable += carried > 0 ? 1 : 0;
        std::printf("%-26s 상태=%-6s 오디오=%d/8  → NDS %s\n",
                    argv[i], answered ? "응답" : "무응답", carried,
                    carried > 0 ? "체결 가능" : "거부");
    }
    return usable > 0 ? 0 : 1;
}
