// The API 525A compressor as a standalone module for the node's rt-engine — the NDS half of the
// 525A plugin family (the VST3/AU halves ship separately and route here via the plugin catalog).
//
// Same rule as the console-channel module: NOT a reimplementation — the actual
// ConsoleChannelProcessor from src/audio, restricted to its compressor stage and voiced as the
// 525A (compType carries the API family + the CEILING coupling). A second implementation would
// drift the day either is touched.
//
// Params are the 525A's own controls, normalized 0..1 on the wire and denormalised here:
//   0 IN (bypass inverse)   1 THRESH −20…+10 dB   2 MAKE-UP 0…20 dB
//   3 ATTACK 7 detents (15µ…15 ms)   4 RELEASE 4-step ladder (.05/.2/.5/2.0 s)
//   5 RATIO (C 2:1 / L 20:1)         6 CEILING 0…20 dB (threshold down + make-up up)
//
// ABI 2: meter() reports the compressor's gain reduction so the plate's needle keeps moving
// when the plugin runs here instead of in the host.
//
// Build (on the node): tools/node/build-console-module.sh builds this alongside the strip.

#include "audio/ConsoleChannelProcessor.h"
#include "na_rt_plugin.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

using neuracoust::daw::ConsoleChannelProcessor;
using neuracoust::daw::ConsoleChannelState;

struct Api525AState {
    ConsoleChannelProcessor processor;
    ConsoleChannelState parameters;
    std::vector<float> scratch;
    double sampleRate = 48000.0;
};

constexpr uint32_t kParamCount = 7u;
const char* const kParamNames[kParamCount] = {
    "In", "Thresh", "Make-Up", "Attack", "Release", "Ratio", "Ceiling",
};
constexpr float kAttackStepsMs[] = {0.1f, 0.25f, 1.0f, 2.0f, 5.0f, 10.0f, 15.0f};
constexpr float kReleaseStepsSec[] = {0.05f, 0.2f, 0.5f, 2.0f};

NaRtParamInfo g_params[kParamCount];
char g_paramIds[kParamCount][8];

void buildParamTable() {
    for (uint32_t i = 0; i < kParamCount; ++i) {
        std::snprintf(g_paramIds[i], sizeof(g_paramIds[i]), "p%u", i);
        g_params[i].index = i;
        g_params[i].id = g_paramIds[i];
        g_params[i].name = kParamNames[i];
        g_params[i].type = NA_RT_PARAM_FLOAT;
        g_params[i].default_value = i == 0 ? 1.0f : 0.0f;
        g_params[i].min_value = 0.0f;
        g_params[i].max_value = 1.0f;
        g_params[i].choice_count = 0u;
    }
}

void moduleInit(void* ptr, double sampleRate) {
    auto* state = new (ptr) Api525AState();
    state->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    // Voiced as the 525A: API family character, comp stage only. Strings assigned once, here —
    // never in process()/set_param().
    state->parameters.compType = "API 525A";
    state->parameters.moduleOrder = "comp";
    state->parameters.compEnabled = true;
    state->parameters.compRatio = 2.0f;
    state->parameters.compMix = 1.0f;
    state->scratch.assign(static_cast<size_t>(NA_RT_MAX_FRAMES) * 2u, 0.0f);
    state->processor.reset(state->sampleRate);
}

void moduleSetParam(void* ptr, uint32_t index, float value) {
    auto* state = static_cast<Api525AState*>(ptr);
    auto& p = state->parameters;
    const float v = std::max(0.0f, std::min(1.0f, value));
    switch (index) {
        case 0: p.compEnabled = v >= 0.5f; break;
        case 1: p.compThresholdDb = -20.0f + v * 30.0f; break;
        case 2: p.compMakeupDb = v * 20.0f; break;
        case 3: {
            const int step = static_cast<int>(v * 6.0f + 0.5f);
            p.compAttackMs = kAttackStepsMs[std::max(0, std::min(6, step))];
            break;
        }
        case 4: {
            const int step = static_cast<int>(v * 3.0f + 0.5f);
            p.compReleaseMs = kReleaseStepsSec[std::max(0, std::min(3, step))] * 1000.0f;
            break;
        }
        case 5: p.compRatio = v >= 0.5f ? 20.0f : 2.0f; break;
        case 6: p.compCeilingDb = v * 20.0f; break;
        default: break;
    }
}

void moduleProcess(void* ptr, NaRtAudioBlock* block) {
    auto* state = static_cast<Api525AState*>(ptr);
    if (block == nullptr || block->frame_count == 0u || block->channel_count == 0u) {
        return;
    }
    const uint32_t frames = std::min<uint32_t>(block->frame_count, NA_RT_MAX_FRAMES);
    if (block->sample_rate > 0.0 && block->sample_rate != state->sampleRate) {
        state->sampleRate = block->sample_rate;
        state->processor.reset(state->sampleRate);
    }
    float* left = block->channels[0];
    float* right = block->channel_count > 1u ? block->channels[1] : block->channels[0];
    if (left == nullptr || right == nullptr) {
        return;
    }
    state->scratch.resize(static_cast<size_t>(frames) * 2u);
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
}

uint32_t moduleMeter(void* ptr, float* values, uint32_t capacity) {
    auto* state = static_cast<Api525AState*>(ptr);
    if (values == nullptr || capacity < 1u) {
        return 0u;
    }
    values[0] = state->processor.compressorGainReductionDb();
    return 1u;
}

const NaRtPlugin g_plugin = {
    {
        NA_RT_PLUGIN_ABI_VERSION,
        "na.neuracoust.api525a",
        "Neuracoust 525A",
        static_cast<uint32_t>(sizeof(Api525AState)),
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
