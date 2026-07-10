#pragma once

#include <stddef.h>
#include <stdint.h>

#define NA_RT_PLUGIN_ABI_VERSION 1u
#define NA_RT_MAX_PARAMS 64u
#define NA_RT_MAX_CHANNELS 64u
#define NA_RT_MAX_FRAMES 256u

typedef enum {
    NA_RT_PARAM_FLOAT = 0,
    NA_RT_PARAM_BOOL = 1,
    NA_RT_PARAM_CHOICE = 2
} NaRtParamType;

typedef struct {
    uint32_t index;
    const char *id;
    const char *name;
    NaRtParamType type;
    float default_value;
    float min_value;
    float max_value;
    uint32_t choice_count;
} NaRtParamInfo;

typedef struct {
    uint32_t abi_version;
    const char *id;
    const char *name;
    uint32_t state_size;
    uint32_t param_count;
    const NaRtParamInfo *params;
} NaRtPluginInfo;

typedef struct {
    float *channels[NA_RT_MAX_CHANNELS];
    uint32_t channel_count;
    uint32_t frame_count;
    double sample_rate;
} NaRtAudioBlock;

typedef struct {
    NaRtPluginInfo info;
    void (*init)(void *state, double sample_rate);
    void (*process)(void *state, NaRtAudioBlock *block);
    void (*set_param)(void *state, uint32_t index, float value);
} NaRtPlugin;

typedef const NaRtPlugin *(*NaRtGetPluginFn)(void);
