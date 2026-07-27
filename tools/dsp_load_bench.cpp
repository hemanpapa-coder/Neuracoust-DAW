// Where does the realtime budget actually go?
//
// The DSP-load meter hits 100% with a handful of tracks, and the two candidates are the per-track
// console strip and the linear-phase monitor EQ (a 4096-tap FIR). This times both against the real
// deadline — one buffer must be finished within blockFrames / sampleRate seconds — so the decision
// to parallelise (or to replace the FIR with a partitioned convolution) rests on numbers.
//
// Not a ctest: it measures wall-clock, which no CI wants to assert on. Run it by hand.

#include "audio/ConsoleChannelProcessor.h"
#include "audio/MonitorFirEq.h"
#include "audio/ProjectAudioRenderer.h"
#include "project/ProjectDocument.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace neuracoust::daw;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockFrames = 256;                       // the app's current buffer
const double kBlockBudgetUs = 1e6 * kBlockFrames / kSampleRate;   // 5333 us at 48k/256

std::vector<float> makeNoiseBlock(int frames, unsigned seed) {
    std::vector<float> block(static_cast<size_t>(frames) * 2u);
    unsigned state = seed * 1664525u + 1013904223u;
    for (auto& sample : block) {
        state = state * 1664525u + 1013904223u;
        sample = 0.25f * (static_cast<float>(state >> 8 & 0xffffu) / 32768.0f - 1.0f);
    }
    return block;
}

/// A console channel with every module engaged — the worst case a strip can cost.
ConsoleChannelState fullyLoadedConsole() {
    ConsoleChannelState console;
    console.model = "4000e";
    console.filterEnabled = true;
    console.highPassEnabled = true;
    console.lowPassEnabled = true;
    console.highPassHz = 80.0f;
    console.lowPassHz = 16000.0f;
    console.eqEnabled = true;
    console.eqHfGainDb = 3.0f;
    console.eqHmfGainDb = -2.5f;
    console.eqLmfGainDb = 2.0f;
    console.eqLfGainDb = -1.5f;
    console.compEnabled = true;
    console.compThresholdDb = -24.0f;
    console.compRatio = 4.0f;
    console.gateEnabled = true;
    console.gateThresholdDb = -48.0f;
    console.saturatorEnabled = true;
    console.saturatorDriveDb = 6.0f;
    console.compCircuitMode = true;
    console.eqCircuitMode = true;
    console.saturatorCircuitMode = true;
    return console;
}

double gLastMaxUs = 0.0;   // worst single block of the last timing run

/// Mean per block, and (via gLastMaxUs) the worst single block. Jitter lives in the worst case:
/// a heap allocation on the audio thread is cheap on average and occasionally very expensive, so
/// the mean hides exactly the thing that drops audio.
double timeBlocksUs(int iterations, const std::function<void(int)>& body) {
    double total = 0.0, worst = 0.0;
    for (int i = 0; i < iterations; ++i) {
        const auto blockStart = std::chrono::steady_clock::now();
        body(i);
        const double us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - blockStart).count();
        total += us;
        if (i > 0 && us > worst) worst = us;   // skip the first, which warms caches and capacity
    }
    gLastMaxUs = worst;
    return total / iterations;
}

void report(const std::string& label, double perBlockUs, bool withWorst = false) {
    std::printf("%-46s %8.1f us/block   %6.2f%% of budget", label.c_str(),
                perBlockUs, 100.0 * perBlockUs / kBlockBudgetUs);
    if (withWorst) {
        std::printf("   worst %8.1f us  (%6.2f%%)", gLastMaxUs, 100.0 * gLastMaxUs / kBlockBudgetUs);
    }
    std::printf("\n");
}

} // namespace

int main() {
    std::printf("Block %d frames @ %.0f Hz -> budget %.0f us per block\n\n",
                kBlockFrames, kSampleRate, kBlockBudgetUs);

    // --- One console strip, all modules on -------------------------------------------------
    {
        ConsoleChannelProcessor processor;
        processor.reset(kSampleRate);
        const auto console = fullyLoadedConsole();
        auto scratch = makeNoiseBlock(kBlockFrames, 1);
        const auto source = scratch;
        const double perBlock = timeBlocksUs(2000, [&](int) {
            scratch = source;
            processor.processInterleavedStereo(scratch, console, kSampleRate);
        });
        report("console strip x1 (filter+EQ+comp+gate+sat)", perBlock);
        for (int tracks : {8, 24, 64, 128}) {
            report("  x" + std::to_string(tracks) + " tracks", perBlock * tracks);
        }
    }
    std::printf("\n");

    // --- The linear-phase monitor EQ, at the tap counts the engine uses --------------------
    {
        ResponseCurve target;
        for (double hz = 20.0; hz < 20000.0; hz *= 1.25) {
            target.push_back({hz, 4.0 * std::sin(std::log(hz))});   // something non-flat to fit
        }
        for (int taps : {1024, 2048, 4096}) {
            MonitorFirEq fir;
            fir.designFromCurve(kSampleRate, target, taps);
            if (!fir.active()) { std::printf("FIR %d taps: inactive\n", taps); continue; }
            auto block = makeNoiseBlock(kBlockFrames, 7);
            const double perBlock = timeBlocksUs(2000, [&](int) {
                fir.processInterleavedStereo(block.data(), kBlockFrames);
            });
            report("monitor FIR " + std::to_string(fir.numTaps()) + " taps (stereo)", perBlock);
        }
    }
    std::printf("\n");

    // --- The renderer itself, with N tracks --------------------------------------------------
    // The two sections above measure DSP in isolation, which is not what a session costs: the
    // route loop runs per track per block whether or not the track has audio on it. A session of
    // empty tracks is exactly the case that reported 100%, so measure that.
    for (int trackCount : {8, 32, 100, 200}) {
        ProjectDocument project;
        project.sampleRate = kSampleRate;
        TrackState master;
        master.name = "Master";
        master.trackType = "master";
        project.tracks.push_back(master);
        for (int i = 0; i < trackCount; ++i) {
            TrackState track;
            track.name = "Audio " + std::to_string(i + 1);
            track.trackType = "audio";
            track.outputBus = "Master";
            project.tracks.push_back(track);
        }
        ProjectAudioRenderPlan plan;
        std::string error;
        if (!makeProjectAudioRenderPlan(project, plan, error)) {
            std::printf("plan failed for %d tracks: %s\n", trackCount, error.c_str());
            continue;
        }
        ProjectAudioRenderState state;
        std::vector<float> out(static_cast<size_t>(kBlockFrames) * 2u, 0.0f);
        int64_t frame = 0;
        const double perBlock = timeBlocksUs(1000, [&](int) {
            renderProjectAudioBlockWithStateAndMeters(plan, state, frame, kBlockFrames, out, nullptr);
            frame += kBlockFrames;
        });
        report("renderer, " + std::to_string(trackCount) + " empty tracks", perBlock, true);
    }

    std::printf("\nBudget is one core. Anything at or over 100%% cannot run in time.\n");
    return 0;
}
