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
        char canonical[128] = {0};
        const int kind = nc_dsp_probe_node_kind(argv[i], 120, canonical, sizeof(canonical));
        const int carried = nc_dsp_probe_node_audio(argv[i], 60, 8);
        const bool ndsOk = kind == 2 && carried > 0;
        usable += ndsOk ? 1 : 0;
        std::printf("%-26s 상태=%-6s 종류=%-24s 오디오=%d/8  → NDS %s\n",
                    argv[i], answered ? "응답" : "무응답",
                    kind == 2 ? "NDS 어플라이언스" : kind == 1 ? "외부 노드(빌린 컴퓨터)" : "없음",
                    carried, ndsOk ? "체결 가능" : "거부");
        if (kind != 0 && canonical[0] != '\0') std::printf("%-26s   사용할 주소: %s\n", "", canonical);
    }
    return usable > 0 ? 0 : 1;
}
