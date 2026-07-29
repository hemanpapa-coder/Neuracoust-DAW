// The console channel strip, as a module for the node's rt-engine.
//
// This is the whole point of the remote-DSP path: a channel assigned to a node must sound exactly
// as it does here, so the node runs THE SAME CODE. Not a reimplementation of the strip in C — the
// actual ConsoleChannelProcessor from src/audio, compiled for the node. A second implementation
// would drift from the first the day either one is touched, and the drift would be inaudible until
// it was not.
//
// Wire format is shared too: the DAW packs parameters with consoleChannelParameterValues() and
// this unpacks them with applyConsoleChannelParameter(), one mapping, one set of ranges.
//
// Build (on the node, which has g++ but no cmake):
//   tools/node/build-console-module.sh
//
// Load:
//   neuracoust-rt-engine --module na_console_channel.so
//
// Realtime rules from the SDK — no allocation, no locks, no I/O in process() — are met by doing
// every allocation once in init(): the interleave scratch is sized for NA_RT_MAX_FRAMES there and
// never grows, and the strip's own state holds no heap of its own.

#include "audio/ConsoleChannelProcessor.h"
#include "na_rt_plugin.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace {

using neuracoust::daw::ConsoleChannelProcessor;
using neuracoust::daw::ConsoleChannelState;

// The engine allocates state_size bytes and hands us the pointer; we placement-new into it.
struct ConsoleModuleState {
    ConsoleChannelProcessor processor;
    ConsoleChannelState parameters;
    // Interleaved scratch: the processor works on interleaved stereo, the engine hands us
    // channel-major planes. Sized once, in init.
    std::vector<float> scratch;
    double sampleRate = 48000.0;
};

// One entry per wire index. Everything is declared 0..1 because that is what the NART packet
// carries — the engine passes the wire value through untouched and the module denormalises, the
// same contract na_4001e follows.
constexpr uint32_t kParamCount = 39u;

const char* const kParamNames[kParamCount] = {
    "Filter In",     "HPF In",        "LPF In",        "HPF Freq",      "LPF Freq",
    "EQ In",
    "HF Gain",       "HF Freq",
    "HMF Gain",      "HMF Freq",      "HMF Q",
    "LMF Gain",      "LMF Freq",      "LMF Q",
    "LF Gain",       "LF Freq",
    "Comp In",       "Comp Threshold","Comp Ratio",    "Comp Attack",   "Comp Release", "Comp Mix",
    "Gate In",       "Gate Threshold","Gate Range",    "Gate Attack",   "Gate Release",
    "Sat In",        "Sat Drive",     "Sat Mix",
    "EQ E-Mode",     "Expander",
    "Comp Fast",     "Gate Fast",
    "Comp Circuit",  "EQ Circuit",    "Sat Circuit",
    "Bias Seed",     "Bias Depth",
};

NaRtParamInfo g_params[kParamCount];
char g_paramIds[kParamCount][8];

void buildParamTable() {
    for (uint32_t i = 0; i < kParamCount; ++i) {
        std::snprintf(g_paramIds[i], sizeof(g_paramIds[i]), "p%u", i);
        g_params[i].index = i;
        g_params[i].id = g_paramIds[i];
        g_params[i].name = kParamNames[i];
        g_params[i].type = NA_RT_PARAM_FLOAT;
        g_params[i].default_value = 0.0f;
        g_params[i].min_value = 0.0f;
        g_params[i].max_value = 1.0f;
        g_params[i].choice_count = 0u;
    }
}

void moduleInit(void* ptr, double sampleRate) {
    auto* state = new (ptr) ConsoleModuleState();
    state->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    state->parameters.model = "4000e";
    // Assigned once, here: this string is long enough to heap-allocate, which must not happen
    // during process() or set_param().
    state->parameters.moduleOrder = "filter,eq,gate,comp,saturator";
    state->scratch.assign(static_cast<size_t>(NA_RT_MAX_FRAMES) * 2u, 0.0f);
    state->processor.reset(state->sampleRate);
}

void moduleSetParam(void* ptr, uint32_t index, float value) {
    auto* state = static_cast<ConsoleModuleState*>(ptr);
    // The one mapping the DAW packed with. Denormalising here rather than on the wire is what
    // keeps a 14.5 kHz low-pass from arriving as 1.0.
    neuracoust::daw::applyConsoleChannelParameter(state->parameters, static_cast<int>(index), value);
}

void moduleProcess(void* ptr, NaRtAudioBlock* block) {
    auto* state = static_cast<ConsoleModuleState*>(ptr);
    if (block == nullptr || block->frame_count == 0u || block->channel_count == 0u) {
        return;
    }
    const uint32_t frames = std::min<uint32_t>(block->frame_count, NA_RT_MAX_FRAMES);
    if (block->sample_rate > 0.0 && block->sample_rate != state->sampleRate) {
        state->sampleRate = block->sample_rate;
        state->processor.reset(state->sampleRate);
    }

    // Channel-major planes in, interleaved stereo through the strip, planes back out. A mono
    // block is duplicated so the strip still sees the stereo pair it is written for, then only
    // the left result is written back — a mono channel must not become stereo on the way home.
    float* left = block->channels[0];
    float* right = block->channel_count > 1u ? block->channels[1] : block->channels[0];
    if (left == nullptr || right == nullptr) {
        return;
    }
    state->scratch.resize(static_cast<size_t>(frames) * 2u);   // capacity was reserved in init
    for (uint32_t f = 0; f < frames; ++f) {
        state->scratch[f * 2u] = left[f];
        state->scratch[f * 2u + 1u] = right[f];
    }
    state->processor.processInterleavedStereo(state->scratch, state->parameters, state->sampleRate);
    for (uint32_t f = 0; f < frames; ++f) {
        left[f] = state->scratch[f * 2u];
        if (block->channel_count > 1u) {
            right[f] = state->scratch[f * 2u + 1u];
        }
    }

    // Any channels above the first pair pass through untouched: the strip is a stereo channel,
    // and silently zeroing the rest would look like a routing fault on the DAW side.
}

// GR telemetry (ABI 2): the DAW's gain-reduction needles read THESE numbers when the strip
// runs here — the local processor that used to feed them is skipped on the remote path.
uint32_t moduleMeter(void* state, float* values, uint32_t capacity) {
    if (capacity < 2u) return 0u;
    auto* module = static_cast<ConsoleModuleState*>(state);
    values[0] = module->processor.compressorGainReductionDb();
    values[1] = module->processor.gateGainReductionDb();
    return 2u;
}

const NaRtPlugin g_plugin = {
    {
        NA_RT_PLUGIN_ABI_VERSION,
        "na.neuracoust.console.channel",
        "Neuracoust Console Channel",
        static_cast<uint32_t>(sizeof(ConsoleModuleState)),
        kParamCount,
        g_params,
    },
    moduleInit,
    moduleProcess,
    moduleSetParam,
    moduleMeter,
};

struct ParamTableBuilder {
    ParamTableBuilder() { buildParamTable(); }
};
const ParamTableBuilder g_paramTableBuilder;

} // namespace

extern "C" const NaRtPlugin* na_rt_get_plugin(void) {
    return &g_plugin;
}
