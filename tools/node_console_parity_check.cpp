// Does a channel strip sound the same on the node as it does here?
//
// That is the entire promise of the remote-DSP path, and it is the one thing no unit test can
// answer: the wire mapping, the normalisation ranges, the channel-major conversion and the node's
// own compiler all sit between the two, and any of them could be subtly wrong while everything
// still "works". So this sends a tone to a real node and compares what comes back against the
// same tone processed locally, sample for sample.
//
//   neuracoust_node_console_parity_check <host> [audio-port] [status-port]
//
// Exits non-zero on a mismatch, so it can gate a deploy. Skips (exit 0, with a message) when no
// node answers — a machine without the appliance on its LAN should not fail a build.

#include "audio/ConsoleChannelProcessor.h"
#include "audio/RemoteDspServerClient.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace neuracoust::daw;

// A strip with every module doing something, and nothing at a default — a mapping that swapped two
// indices would still pass if both happened to be zero.
//
// `only` narrows it to one module, which is how you find WHICH parameter is wrong instead of
// merely learning that something is: "none" proves the audio path itself (interleave conversion,
// packet layout) before any processing is involved, then each module is added on its own.
ConsoleChannelState busyStrip(const std::string& only = "full") {
    ConsoleChannelState c;
    c.model = "4000e";
    c.moduleOrder = "filter,eq,gate,comp,saturator";
    const bool all = only == "full";
    if (all || only == "filter") {
    c.filterEnabled = true;
    c.highPassEnabled = true;
    c.highPassHz = 82.0f;
    c.lowPassEnabled = true;
    c.lowPassHz = 14500.0f;
    }
    if (all || only == "eq") {
    c.eqEnabled = true;
    c.eqHfGainDb = -4.5f;   c.eqHfHz = 11000.0f;
    c.eqHmfGainDb = 3.25f;  c.eqHmfHz = 2400.0f;  c.eqHmfQ = 2.5f;
    c.eqLmfGainDb = -2.0f;  c.eqLmfHz = 640.0f;   c.eqLmfQ = 0.8f;
    c.eqLfGainDb = 5.0f;    c.eqLfHz = 90.0f;
    c.eqCircuitMode = true;
    }
    if (all || only == "comp") {
    c.compEnabled = true;
    c.compThresholdDb = -22.5f; c.compRatio = 6.0f;
    c.compAttackMs = 12.0f;     c.compReleaseMs = 480.0f; c.compMix = 0.75f;
    c.compFastAttack = true;
    c.compCircuitMode = true;
    }
    if (all || only == "gate") {
    c.gateEnabled = true;
    c.gateThresholdDb = -44.0f; c.gateRangeDb = 32.0f;
    c.gateAttackMs = 2.5f;      c.gateReleaseMs = 250.0f;
    c.expanderMode = false;
    }
    if (all || only == "sat") {
    c.saturatorEnabled = true;
    c.saturatorDriveDb = 9.0f;  c.saturatorMix = 0.6f;
    c.saturatorCircuitMode = true;
    }
    if (all || only == "bias") {
    c.channelBiasSeed = 137;
    c.channelBiasDepth = 0.4f;
    }
    return c;
}

std::vector<float> tone(size_t frames, double sampleRate) {
    std::vector<float> block(frames * 2u);
    for (size_t f = 0; f < frames; ++f) {
        // Two tones and a little level, so the EQ bands, the compressor and the saturator all have
        // something to act on.
        const double t = static_cast<double>(f) / sampleRate;
        const float x = static_cast<float>(0.35 * std::sin(2.0 * M_PI * 220.0 * t) +
                                           0.20 * std::sin(2.0 * M_PI * 3100.0 * t));
        block[f * 2u] = x;
        block[f * 2u + 1u] = x * 0.8f;
    }
    return block;
}

} // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "192.168.0.198";
    const uint16_t audioPort = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 20000;
    const uint16_t statusPort = argc > 3 ? static_cast<uint16_t>(std::atoi(argv[3])) : 20001;

    constexpr size_t kFrames = 256;      // NA_RT_MAX_FRAMES — one packet, no reassembly to confuse
    constexpr double kSampleRate = 48000.0;

    RemoteDspServerSettings settings = defaultRemoteDspServerSettings();
    settings.nodes.clear();
    settings.host = host;
    settings.rtEnginePort = audioPort;
    settings.statusPort = statusPort;
    settings.channelCount = 2;
    settings.frameCount = static_cast<uint16_t>(kFrames);
    settings.sampleRate = kSampleRate;
    settings.timeoutMs = 400;

    const auto info = queryRemoteDspServerInfo(settings);
    if (!info.reachable) {
        std::cout << "no node at " << host << ':' << statusPort << " — skipping parity check\n";
        return 0;
    }
    std::cout << "node " << host << " loaded module: " << info.pluginId << '\n';
    if (info.pluginId != "na.neuracoust.console.channel") {
        std::cout << "node is hosting a different module — skipping (start it with "
                     "--module na_console_channel.so to run this check)\n";
        return 0;
    }

    const std::string mode = argc > 4 ? argv[4] : "full";
    const auto console = busyStrip(mode);
    std::cout << "strip: " << mode << '\n';
    std::vector<RemoteDspParameterValue> parameters;
    for (const auto& parameter : consoleChannelParameterValues(console)) {
        parameters.push_back({static_cast<uint32_t>(parameter.index), parameter.normalized});
    }

    // The strip is stateful — filter memories, compressor and gate detectors, and coefficients
    // that RAMP toward their targets rather than stepping. The node's module keeps that state for
    // as long as its instance lives, while a fresh local processor starts from reset, so the two
    // do not begin in the same place and the first blocks differ enormously through no fault of
    // the port. Both are driven with the same signal until they converge, and only the settled
    // blocks are compared. (A run that compared block 0 reported a 0.39 mismatch and was measuring
    // its own setup.)
    // Long enough to pass the slowest time constant in the strip several times over: a 480 ms
    // compressor release is 23,040 samples, and 48 blocks of 256 is only half of one — a shorter
    // warm-up was measuring where the two sides happened to be on the same convergence curve, and
    // the answer moved by 100x between runs.
    //
    // IMPORTANT: the node's module keeps its state for the life of its process, so a fair run
    // needs a freshly started instance. tools/node/parity-run.sh does that.
    constexpr int kWarmupBlocks = 400;
    constexpr int kComparedBlocks = 32;

    ConsoleChannelProcessor local;
    local.reset(kSampleRate);

    double worst = 0.0;
    double localEnergy = 0.0;
    for (int pass = 0; pass < kWarmupBlocks + kComparedBlocks; ++pass) {
        const bool warming = pass < kWarmupBlocks;
        auto block = tone(kFrames, kSampleRate);
        auto expected = block;
        local.processInterleavedStereo(expected, console, kSampleRate);

        std::vector<float> returned;
        const auto result = processRemoteDspInterleavedStereo(settings, block, parameters, returned);
        if (!result.processed || returned.size() != expected.size()) {
            std::cerr << "node did not process block " << pass << ": " << result.message << '\n';
            return 2;
        }
        double passWorst = 0.0;
        for (size_t i = 0; i < expected.size(); ++i) {
            passWorst = std::max(passWorst, std::abs(static_cast<double>(expected[i] - returned[i])));
            if (!warming) {
                localEnergy = std::max(localEnergy, std::abs(static_cast<double>(expected[i])));
            }
        }
        if (warming) {
            // The shape of the warm-up tells you what you are looking at: an error that FALLS is
            // two differently-initialised copies converging, which is expected. One that GROWS is
            // the two arithmetics diverging. One that stays flat and large is a parameter arriving
            // as the wrong value.
            if (pass % 100 == 0) {
                std::cout << "  warm-up block " << pass << ": " << passWorst << '\n';
            }
            continue;
        }
        worst = std::max(worst, passWorst);
    }

    const double relativeDb = worst > 0.0 ? 20.0 * std::log10(worst / localEnergy) : -400.0;
    std::cout << "peak level " << localEnergy << ", worst sample difference " << worst
              << "  (" << relativeDb << " dB)\n";
    if (localEnergy < 0.01) {
        std::cerr << "the strip produced near-silence — the comparison would prove nothing\n";
        return 3;
    }

    // The bar is inaudibility, not bit-exactness, and the difference between those two is a
    // measured fact rather than a shrug.
    //
    // With a fresh node and a proper warm-up, the pass-through, gate and bias paths come back
    // EXACTLY equal, and the compressor and saturator land on float32 epsilon (~1.2e-7). Only the
    // biquad-based modules differ: the filter at about -85 dB, the EQ at about -70 dB. Those
    // coefficients are built from tan/cos/exp, and macOS libm and glibc disagree on those by one
    // or two ULP — which a filter whose pole sits near the unit circle at 90 Hz amplifies. It is
    // not a mapping error: a wrong parameter shows up as tens of dB, not seventy below.
    //
    // So: -60 dB is the line. Anything worse is a real defect. If bit-exactness is ever needed —
    // a bounce rendered on the node would want it — the coefficient math has to stop calling libm
    // and use our own approximations, which is a different piece of work.
    constexpr double kAudibilityFloorDb = -60.0;
    if (relativeDb > kAudibilityFloorDb) {
        std::cerr << "MISMATCH — the node's strip does not match this one\n";
        return 1;
    }
    std::cout << "match — the node's console strip is inaudibly identical to the local one\n";
    return 0;
}
