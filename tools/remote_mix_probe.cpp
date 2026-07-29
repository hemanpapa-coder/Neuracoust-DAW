// The remote-mixer M1 gate, against a real node: the summing bus must be BIT-EXACT.
//
// Deterministic per-track blocks are summed locally in ascending track order, then the same
// blocks go to the node's mix session — one packet per track, one reply — and the two sums are
// compared with memcmp, not a dB threshold: addition is the one operation both sides can and
// therefore must perform identically, so any difference at all is a protocol or ordering bug.
//
//   neuracoust_remote_mix_probe <host[:port]> [trackCount] [blocks]
//
// Also reports the round-trip per block, which is the number the realtime budget (5.3 ms at
// 256/48k) cares about. Not in ctest — needs the appliance on the LAN.

#include "audio/RemoteDspServerClient.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace neuracoust::daw;

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "192.168.0.198:20002";
    const size_t trackCount = argc > 2 ? static_cast<size_t>(std::stoi(argv[2])) : 32;
    const int blocks = argc > 3 ? std::stoi(argv[3]) : 200;
    constexpr size_t kFrames = 256;

    auto settings = defaultRemoteDspServerSettings();
    settings.nodes.clear();
    settings.host = host;
    applyRemoteDspHostPort(settings);
    settings.timeoutMs = 50;

    RemoteMixSession session;
    std::vector<std::vector<float>> tracks(trackCount);
    std::vector<float> localSum(kFrames * 2u);
    std::vector<float> remoteSum;

    double worstMs = 0.0;
    double totalMs = 0.0;
    int mismatches = 0;
    int misses = 0;

    for (int block = 0; block < blocks; ++block) {
        // Deterministic, block-varying content — every track different, nothing zero.
        for (size_t track = 0; track < trackCount; ++track) {
            auto& buffer = tracks[track];
            buffer.resize(kFrames * 2u);
            for (size_t frame = 0; frame < kFrames; ++frame) {
                const double phase = 0.001 * static_cast<double>(block * 977 + frame) *
                                     static_cast<double>(track + 1);
                buffer[frame * 2u] = static_cast<float>(0.03 * std::sin(phase));
                buffer[frame * 2u + 1u] = static_cast<float>(0.03 * std::cos(phase * 1.3));
            }
        }
        // The reference: ascending-order float addition, the exact order the node promises.
        std::fill(localSum.begin(), localSum.end(), 0.0f);
        for (size_t track = 0; track < trackCount; ++track) {
            for (size_t i = 0; i < localSum.size(); ++i) {
                localSum[i] += tracks[track][i];
            }
        }

        const auto result = session.mix(settings, tracks, remoteSum);
        if (!result.processed) {
            ++misses;
            if (misses <= 3) {
                std::cerr << "block " << block << " miss: " << result.message << '\n';
            }
            continue;
        }
        totalMs += result.roundTripMs;
        worstMs = std::max(worstMs, result.roundTripMs);
        if (remoteSum.size() != localSum.size() ||
            std::memcmp(remoteSum.data(), localSum.data(), localSum.size() * sizeof(float)) != 0) {
            ++mismatches;
        }
    }

    const int completed = blocks - misses;
    std::cout << trackCount << " tracks x " << blocks << " blocks: "
              << completed << " summed, " << misses << " missed, "
              << mismatches << " MISMATCHED\n";
    if (completed > 0) {
        std::cout << "round trip avg " << (totalMs / completed) << " ms, worst " << worstMs
                  << " ms (budget 5.33 ms at 256/48k)\n";
    }
    if (mismatches > 0) {
        std::cerr << "REMOTE SUM IS NOT BIT-EXACT — ordering or protocol bug\n";
        return 2;
    }
    if (completed == 0) {
        std::cerr << "no block completed — is the appliance up?\n";
        return 3;
    }
    std::cout << "bit-exact: every completed block matched the local sum exactly\n";
    return 0;
}
