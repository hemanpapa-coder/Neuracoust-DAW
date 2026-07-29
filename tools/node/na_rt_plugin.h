#pragma once

#include <stddef.h>
#include <stdint.h>

/* ABI 2 adds the optional meter() hook at the TAIL of NaRtPlugin, so a v1 module still loads
 * (the engine only calls meter() when abi_version >= 2 AND the pointer is set). GR telemetry
 * exists because a strip processed on the node otherwise leaves the DAW's gain-reduction
 * needles frozen — the local processor that used to feed them never ran. */
#define NA_RT_PLUGIN_ABI_VERSION 2u
#define NA_RT_MAX_METERS 8u
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
    /* ABI >= 2, optional (NULL fine): fill up to NA_RT_MAX_METERS values describing the last
     * processed block (a console strip reports [comp GR dB, gate GR dB]); return how many. */
    uint32_t (*meter)(void *state, float *values, uint32_t capacity);
} NaRtPlugin;

typedef const NaRtPlugin *(*NaRtGetPluginFn)(void);
