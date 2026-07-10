// A freshly instantiated VST3's processor does not necessarily hold the values its
// own controller was initialised with. FabFilter Micro's low-pass sits at 4.8 Hz
// while its controller reports 4166 Hz; nothing synced the two, so every insert in
// this DAW processed with every parameter at zero and quietly destroyed the signal.
//
// This measures a 1 kHz tone through the plug-in. It is a sound test, not a state
// test: reading parameters back would have agreed with the bug.

#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char* kPluginPath = "/Library/Audio/Plug-Ins/VST3/FabFilter Micro.vst3";
constexpr const char* kPluginName = "FabFilter Micro";

} // namespace

int main() {
    if (!std::filesystem::exists(kPluginPath)) {
        std::printf("SKIP: %s is not installed.\n", kPluginName);
        return 0;
    }

    const auto descriptor =
        neuracoust::daw::resolveVst3PluginDescriptorForInsert(kPluginName, kPluginPath, "", "");

    neuracoust::daw::Vst3RealtimeProcessor processor;
    std::string message;
    if (!processor.prepare(descriptor, 48000.0, 512, message)) {
        std::printf("SKIP: could not prepare %s: %s\n", kPluginName, message.c_str());
        return 0;
    }

    // Micro's controller comes up as a 4166 Hz low-pass, so 1 kHz should pass almost
    // untouched. With the parameters left at zero the cutoff is 4.8 Hz and the tone
    // comes out four decades down.
    double phase = 0.0;
    float outputPeak = 0.0f;
    std::vector<float> block(512 * 2);
    for (int index = 0; index < 40; ++index) {
        for (int frame = 0; frame < 512; ++frame) {
            const auto sample = static_cast<float>(0.5 * std::sin(phase));
            phase += 2.0 * M_PI * 1000.0 / 48000.0;
            block[frame * 2] = sample;
            block[frame * 2 + 1] = sample;
        }
        std::string processMessage;
        const auto result = processor.processInterleavedStereo(block.data(), 512, processMessage);
        if (!result.processed) {
            std::fprintf(stderr, "FAIL: %s did not process: %s\n", kPluginName, processMessage.c_str());
            return 1;
        }
        // Skip the first blocks: the filter needs a moment to settle.
        if (index >= 20) {
            for (float value : block) {
                outputPeak = std::max(outputPeak, std::fabs(value));
            }
        }
    }

    const float ratio = outputPeak / 0.5f;
    std::printf("1 kHz through %s: peak %.4f of 0.5 (ratio %.4f)\n", kPluginName, outputPeak, ratio);
    if (ratio < 0.5f) {
        std::fprintf(stderr,
                     "FAIL: the tone came out at %.4f of its level — the plug-in is running with "
                     "unset parameters.\n", ratio);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
