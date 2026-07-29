#define _GNU_SOURCE

#include "na_rt_plugin.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NA_RT_MAGIC 0x4e415254u
#define NA_RT_VERSION 1u
#define NA_RT_AUDIO_PORT 20000
#define NA_RT_MONITOR_PORT 20001
#define NA_RT_SAMPLE_RATE 48000.0
#define NA_RT_DEFAULT_FRAMES 128u
#define NA_RT_FLAG_PARAMETERS 1u
/* Optional routing block right after the header: which module this packet is for, and which
 * SESSION of it. Two DAW streams processing two different channels through the same module must
 * not share filter memories and detector state — without sessions they silently did. A packet
 * with no route flag runs module 0, session 0: exactly the old single-module behaviour, so an
 * old DAW keeps working against a new engine. */
#define NA_RT_FLAG_ROUTE 2u
#define NA_RT_MODULE_ID_LEN 48u
#define NA_RT_ROUTE_BLOCK_LEN (NA_RT_MODULE_ID_LEN + 8u)
#define NA_RT_MAX_MODULES 8u
#define NA_RT_MAX_SESSIONS 64u
#define NA_RT_MAX_PACKET_PARAMS 64u

/* ── Remote mixer (M1): flat deterministic summing ─────────────────────────────────────────
 * The DAW sends one MIX packet per track (same sequence, same mix key), the engine banks each
 * track's stereo block, and when the LAST track of the set arrives it replies ONCE with the
 * sum — summed in ascending track_index order, because float addition order is audible to a
 * parity check even when it is not audible to an ear. One round trip per block, not one per
 * track. Master-chain processing stays on the existing per-module session path; this is the
 * summing bus only. */
#define NA_RT_FLAG_MIX 4u
#define NA_RT_MIX_BLOCK_LEN 16u          /* key(8) track_index(4) track_count(4) */
#define NA_RT_MAX_MIX_SESSIONS 4u
#define NA_RT_MAX_MIX_TRACKS 64u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t sequence;
    uint16_t channel_count;
    uint16_t frame_count;
    uint32_t flags;
} NaRtPacketHeader;

typedef struct {
    float gain;
} NaGainState;

typedef struct {
    const NaRtPlugin *plugin;
    void *state;
    void *module_handle;
} NaGraphNode;

typedef struct {
    uint32_t node_count;
    NaGraphNode nodes[NA_RT_MAX_MODULES];
} NaGraph;

/* One live (module, session) state. Allocated on the first packet that names the session —
 * a malloc on the audio path, but only once per stream start, which the stream's own warm-up
 * already absorbs; steady state never allocates. Slots recycle least-recently-used, so an
 * abandoned stream costs nothing and a returning one just warms up again. */
typedef struct {
    uint64_t key;          /* 0 = free */
    uint32_t module_index;
    void *state;
    uint32_t state_size;
    uint64_t stamp;
} NaSession;

static NaSession g_sessions[NA_RT_MAX_SESSIONS];
static uint64_t g_session_stamp = 0u;

typedef struct {
    uint64_t key;                        /* 0 = free */
    uint32_t sequence;
    uint32_t track_count;
    uint64_t received_mask;
    uint16_t frame_count;
    uint64_t stamp;
    float buffers[NA_RT_MAX_MIX_TRACKS][2u * NA_RT_MAX_FRAMES];
} NaMixSession;

static NaMixSession g_mix_sessions[NA_RT_MAX_MIX_SESSIONS];
static uint64_t g_mix_stamp = 0u;

typedef struct {
    _Atomic uint64_t packets_in;
    _Atomic uint64_t packets_out;
    _Atomic uint64_t bad_packets;
    _Atomic uint64_t overruns;
} NaStats;

static volatile sig_atomic_t g_running = 1;
static _Atomic(NaGraph *) g_active_graph;
static NaStats g_stats;
static uint16_t g_audio_port = NA_RT_AUDIO_PORT;
static uint16_t g_monitor_port = NA_RT_MONITOR_PORT;
static const char *g_loaded_plugin_id = "na.gain";
static const char *g_loaded_plugin_name = "Neuracoust Gain";

static void signal_handler(int signo) {
    (void)signo;
    g_running = 0;
}

static void gain_init(void *state, double sample_rate) {
    (void)sample_rate;
    ((NaGainState *)state)->gain = 1.0f;
}

static void gain_process(void *state, NaRtAudioBlock *block) {
    const float gain = ((NaGainState *)state)->gain;
    if (gain == 1.0f) {
        return;
    }

    for (uint32_t ch = 0; ch < block->channel_count; ++ch) {
        float *samples = block->channels[ch];
        for (uint32_t i = 0; i < block->frame_count; ++i) {
            samples[i] *= gain;
        }
    }
}

static void gain_set_param(void *state, uint32_t index, float value) {
    if (index == 0u) {
        ((NaGainState *)state)->gain = value;
    }
}

static const NaRtPlugin g_gain_plugin = {
    .info = {
        .abi_version = NA_RT_PLUGIN_ABI_VERSION,
        .id = "na.gain",
        .name = "Neuracoust Gain",
        .state_size = sizeof(NaGainState),
        .param_count = 1u,
    },
    .init = gain_init,
    .process = gain_process,
    .set_param = gain_set_param,
};

static int set_realtime(void) {
#ifdef __linux__
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = 80;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
    }

    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        perror("sched_setscheduler");
        return -1;
    }

    return 0;
#else
    return 0;
#endif
}

static NaGraph *create_initial_graph(void) {
    NaGraph *graph = calloc(1, sizeof(*graph));
    NaGainState *gain_state = calloc(1, sizeof(*gain_state));
    if (graph == NULL || gain_state == NULL) {
        free(graph);
        free(gain_state);
        return NULL;
    }

    g_gain_plugin.init(gain_state, NA_RT_SAMPLE_RATE);
    graph->node_count = 1u;
    graph->nodes[0].plugin = &g_gain_plugin;
    graph->nodes[0].state = gain_state;
    return graph;
}

static int append_plugin_to_graph(NaGraph *graph, const NaRtPlugin *plugin, void *module_handle) {
    if (plugin == NULL ||
        plugin->info.abi_version != NA_RT_PLUGIN_ABI_VERSION ||
        plugin->info.state_size == 0u ||
        plugin->init == NULL ||
        plugin->process == NULL ||
        plugin->set_param == NULL) {
        fprintf(stderr, "invalid RT plugin\n");
        return -1;
    }
    if (graph->node_count >= NA_RT_MAX_MODULES) {
        fprintf(stderr, "too many modules (max %u)\n", NA_RT_MAX_MODULES);
        return -1;
    }

    void *state = calloc(1, plugin->info.state_size);
    if (state == NULL) {
        return -1;
    }

    plugin->init(state, NA_RT_SAMPLE_RATE);
    for (uint32_t index = 0u; index < plugin->info.param_count; ++index) {
        float value = 0.0f;
        if (plugin->info.params != NULL) {
            value = plugin->info.params[index].default_value;
        }
        plugin->set_param(state, index, value);
    }

    NaGraphNode *node = &graph->nodes[graph->node_count];
    node->plugin = plugin;
    node->state = state;
    node->module_handle = module_handle;
    graph->node_count += 1u;
    if (graph->node_count == 1u) {
        /* Module 0 stays the legacy identity an un-routed packet and an old DAW see. */
        g_loaded_plugin_id = plugin->info.id != NULL ? plugin->info.id : "unknown";
        g_loaded_plugin_name = plugin->info.name != NULL ? plugin->info.name : "Unknown RT Plugin";
    }
    printf("loaded RT plugin [%u]: %s (%s)\n", graph->node_count - 1u, plugin->info.name, plugin->info.id);
    return 0;
}

static int load_plugin_into_graph(NaGraph *graph, const char *path) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return -1;
    }

    dlerror();
    NaRtGetPluginFn get_plugin = (NaRtGetPluginFn)dlsym(handle, "na_rt_get_plugin");
    const char *error = dlerror();
    if (error != NULL || get_plugin == NULL) {
        fprintf(stderr, "dlsym na_rt_get_plugin failed: %s\n", error != NULL ? error : "missing symbol");
        dlclose(handle);
        return -1;
    }

    if (append_plugin_to_graph(graph, get_plugin(), handle) != 0) {
        dlclose(handle);
        return -1;
    }
    return 0;
}

static NaGraph *load_plugin_graph_multi(const char *const *paths, uint32_t path_count) {
    NaGraph *graph = calloc(1, sizeof(*graph));
    if (graph == NULL) {
        return NULL;
    }
    for (uint32_t i = 0u; i < path_count; ++i) {
        if (load_plugin_into_graph(graph, paths[i]) != 0) {
            free(graph);   /* leaks the earlier handles/states, but we exit right after */
            return NULL;
        }
    }
    return graph;
}

static int bind_udp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    return fd;
}

/* The state a routed packet should run through: the module's own base state for the legacy
 * (no-route) path, or this session's private copy. */
static void *resolve_session_state(NaGraph *graph, uint32_t module_index, uint64_t session_key) {
    if (module_index >= graph->node_count) {
        return NULL;
    }
    if (session_key == 0u) {
        return graph->nodes[module_index].state;
    }
    NaSession *slot = NULL;
    NaSession *oldest = &g_sessions[0];
    for (uint32_t i = 0u; i < NA_RT_MAX_SESSIONS; ++i) {
        NaSession *candidate = &g_sessions[i];
        if (candidate->key == session_key && candidate->module_index == module_index) {
            candidate->stamp = ++g_session_stamp;
            return candidate->state;
        }
        if (candidate->key == 0u) {
            if (slot == NULL) {
                slot = candidate;
            }
        } else if (candidate->stamp < oldest->stamp) {
            oldest = candidate;
        }
    }
    if (slot == NULL) {
        slot = oldest;   /* recycle the least-recently-used stream */
    }
    const NaRtPlugin *plugin = graph->nodes[module_index].plugin;
    if (slot->state == NULL || slot->state_size < plugin->info.state_size) {
        free(slot->state);
        slot->state = calloc(1u, plugin->info.state_size);
        slot->state_size = slot->state != NULL ? plugin->info.state_size : 0u;
        if (slot->state == NULL) {
            slot->key = 0u;
            return NULL;
        }
    } else {
        memset(slot->state, 0, plugin->info.state_size);
    }
    plugin->init(slot->state, NA_RT_SAMPLE_RATE);
    for (uint32_t index = 0u; index < plugin->info.param_count; ++index) {
        float value = 0.0f;
        if (plugin->info.params != NULL) {
            value = plugin->info.params[index].default_value;
        }
        plugin->set_param(slot->state, index, value);
    }
    slot->key = session_key;
    slot->module_index = module_index;
    slot->stamp = ++g_session_stamp;
    return slot->state;
}

/* Which loaded module a route block names; module 0 when the id is empty or unknown, so a
 * mis-addressed stream degrades to the legacy behaviour instead of silence. */
static uint32_t resolve_module_index(NaGraph *graph, const char *module_id) {
    if (module_id[0] == '\0') {
        return 0u;
    }
    for (uint32_t i = 0u; i < graph->node_count; ++i) {
        const char *id = graph->nodes[i].plugin->info.id;
        if (id != NULL && strncmp(id, module_id, NA_RT_MODULE_ID_LEN) == 0) {
            return i;
        }
    }
    return 0u;
}

static void process_block(float *payload, uint32_t channels, uint32_t frames,
                          uint32_t module_index, void *state) {
    NaRtAudioBlock block;
    memset(&block, 0, sizeof(block));
    block.channel_count = channels;
    block.frame_count = frames;
    block.sample_rate = NA_RT_SAMPLE_RATE;

    for (uint32_t ch = 0; ch < channels; ++ch) {
        block.channels[ch] = payload + ((size_t)ch * frames);
    }

    NaGraph *graph = atomic_load_explicit(&g_active_graph, memory_order_acquire);
    if (graph == NULL || module_index >= graph->node_count || state == NULL) {
        return;
    }

    /* One module per packet. The old loop chained every node, which was indistinguishable from
     * this while only one module could be loaded; with several loaded, chaining would run a
     * channel through every processor the node happens to host. */
    graph->nodes[module_index].plugin->process(state, &block);
}

static uint16_t read_u16_network(const uint8_t *data) {
    uint16_t value = 0u;
    memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

static uint32_t read_u32_network(const uint8_t *data) {
    uint32_t value = 0u;
    memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

static float read_float_network(const uint8_t *data) {
    const uint32_t bits = read_u32_network(data);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void apply_param_to_state(NaGraph *graph, uint32_t module_index, void *state,
                                 uint32_t index, float value) {
    if (!isfinite(value) || graph == NULL || module_index >= graph->node_count || state == NULL) {
        return;
    }
    if (value < 0.0f) {
        value = 0.0f;
    } else if (value > 1.0f) {
        value = 1.0f;
    }
    const NaRtPlugin *plugin = graph->nodes[module_index].plugin;
    if (plugin != NULL && plugin->set_param != NULL && index < plugin->info.param_count) {
        plugin->set_param(state, index, value);
    }
}

static int run_self_test(const char *const *module_paths, uint32_t module_count) {
    NaGraph *graph = module_count > 0u ? load_plugin_graph_multi(module_paths, module_count)
                                       : create_initial_graph();
    if (graph == NULL) {
        return 1;
    }
    atomic_store_explicit(&g_active_graph, graph, memory_order_release);

    /* Every module, and both the base state and a private session state — the session pool is
     * exactly the new machinery, so the self-test must run through it. */
    for (uint32_t m = 0u; m < graph->node_count; ++m) {
        float audio[2u * NA_RT_DEFAULT_FRAMES];
        for (size_t i = 0; i < sizeof(audio) / sizeof(audio[0]); ++i) {
            audio[i] = 0.25f;
        }
        if (module_count == 0u) {
            g_gain_plugin.set_param(graph->nodes[m].state, 0u, 0.5f);
        }
        process_block(audio, 2u, NA_RT_DEFAULT_FRAMES, m, graph->nodes[m].state);
        if (!isfinite(audio[0]) || fabsf(audio[0]) < 0.000001f) {
            fprintf(stderr, "self-test failed (module %u, base state): %f\n", m, audio[0]);
            return 1;
        }
        void *session_state = resolve_session_state(graph, m, 0x5e551d0000000001ull + m);
        if (session_state == NULL) {
            fprintf(stderr, "self-test failed: no session state for module %u\n", m);
            return 1;
        }
        for (size_t i = 0; i < sizeof(audio) / sizeof(audio[0]); ++i) {
            audio[i] = 0.25f;
        }
        if (module_count == 0u) {
            g_gain_plugin.set_param(session_state, 0u, 0.5f);
        }
        process_block(audio, 2u, NA_RT_DEFAULT_FRAMES, m, session_state);
        if (!isfinite(audio[0]) || fabsf(audio[0]) < 0.000001f) {
            fprintf(stderr, "self-test failed (module %u, session state): %f\n", m, audio[0]);
            return 1;
        }
    }

    printf("self-test ok (%u module(s), sessions ok)\n", graph->node_count);
    return 0;
}

static NaMixSession *resolve_mix_session(uint64_t key) {
    NaMixSession *oldest = &g_mix_sessions[0];
    for (uint32_t i = 0u; i < NA_RT_MAX_MIX_SESSIONS; ++i) {
        NaMixSession *slot = &g_mix_sessions[i];
        if (slot->key == key) {
            slot->stamp = ++g_mix_stamp;
            return slot;
        }
        if (slot->key == 0u) {
            oldest = slot;
            break;
        }
        if (slot->stamp < oldest->stamp) {
            oldest = slot;
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->key = key;
    oldest->stamp = ++g_mix_stamp;
    return oldest;
}

/* One track of a mix set. Replies with the deterministic sum when the set completes. */
static void handle_mix_packet(int audio_fd, const struct sockaddr_in *peer, socklen_t peer_len,
                              uint8_t *packet, size_t got, uint32_t frames) {
    const size_t mix_offset = sizeof(NaRtPacketHeader);
    const size_t audio_offset = mix_offset + NA_RT_MIX_BLOCK_LEN;
    const size_t audio_bytes = (size_t)2u * frames * sizeof(float);
    if (got != audio_offset + audio_bytes) {
        atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
        return;
    }
    const uint64_t key = ((uint64_t)read_u32_network(packet + mix_offset) << 32) |
                         read_u32_network(packet + mix_offset + 4u);
    const uint32_t track_index = read_u32_network(packet + mix_offset + 8u);
    const uint32_t track_count = read_u32_network(packet + mix_offset + 12u);
    NaRtPacketHeader *header = (NaRtPacketHeader *)packet;
    const uint32_t sequence = ntohl(header->sequence);
    if (key == 0u || track_count == 0u || track_count > NA_RT_MAX_MIX_TRACKS ||
        track_index >= track_count) {
        atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
        return;
    }

    NaMixSession *mix = resolve_mix_session(key);
    if (mix->sequence != sequence || mix->track_count != track_count ||
        mix->frame_count != (uint16_t)frames) {
        /* A new block starts (or the set shape changed): any incomplete previous set is
         * abandoned — the DAW's own timeout already re-rendered that block locally. */
        if (mix->received_mask != 0u &&
            mix->received_mask != ((track_count >= 64u) ? ~0ull : ((1ull << mix->track_count) - 1ull))) {
            atomic_fetch_add_explicit(&g_stats.overruns, 1u, memory_order_relaxed);
        }
        mix->sequence = sequence;
        mix->track_count = track_count;
        mix->frame_count = (uint16_t)frames;
        mix->received_mask = 0u;
    }
    memcpy(mix->buffers[track_index], packet + audio_offset, audio_bytes);
    mix->received_mask |= (1ull << track_index);
    atomic_fetch_add_explicit(&g_stats.packets_in, 1u, memory_order_relaxed);

    const uint64_t full = (track_count >= 64u) ? ~0ull : ((1ull << track_count) - 1ull);
    if (mix->received_mask != full) {
        return;
    }

    /* Complete: sum ascending (the deterministic order the parity gate pins) and reply once. */
    float *out = (float *)(packet + sizeof(NaRtPacketHeader));
    memcpy(out, mix->buffers[0], audio_bytes);
    for (uint32_t trk = 1u; trk < track_count; ++trk) {
        const float *src = mix->buffers[trk];
        for (uint32_t i = 0u; i < 2u * frames; ++i) {
            out[i] += src[i];
        }
    }
    mix->received_mask = 0u;
    header->flags = htonl(NA_RT_FLAG_MIX);
    const size_t reply_bytes = sizeof(NaRtPacketHeader) + audio_bytes;
    if (sendto(audio_fd, packet, reply_bytes, 0,
               (const struct sockaddr *)peer, peer_len) == (ssize_t)reply_bytes) {
        atomic_fetch_add_explicit(&g_stats.packets_out, 1u, memory_order_relaxed);
    }
}

static void run_engine(void) {
    uint8_t packet[sizeof(NaRtPacketHeader) +
                   4u + (NA_RT_MAX_PACKET_PARAMS * 8u) +
                   (sizeof(float) * NA_RT_MAX_CHANNELS * NA_RT_MAX_FRAMES)];

    int audio_fd = bind_udp(g_audio_port);
    if (audio_fd < 0) {
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    setsockopt(audio_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    printf("neuracoust-rt-engine listening on UDP %u\n", g_audio_port);

    while (g_running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t got = recvfrom(audio_fd, packet, sizeof(packet), 0,
                               (struct sockaddr *)&peer, &peer_len);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            perror("recvfrom");
            break;
        }

        if ((size_t)got < sizeof(NaRtPacketHeader)) {
            atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
            continue;
        }

        NaRtPacketHeader *header = (NaRtPacketHeader *)packet;
        const uint32_t channels = ntohs(header->channel_count);
        const uint32_t frames = ntohs(header->frame_count);
        const uint32_t flags = ntohl(header->flags);
        const size_t payload_bytes = (size_t)channels * frames * sizeof(float);
        size_t payload_offset = sizeof(NaRtPacketHeader);

        if (ntohl(header->magic) != NA_RT_MAGIC ||
            ntohs(header->version) != NA_RT_VERSION ||
            channels == 0u || channels > NA_RT_MAX_CHANNELS ||
            frames == 0u || frames > NA_RT_MAX_FRAMES) {
            atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
            continue;
        }

        if ((flags & NA_RT_FLAG_MIX) != 0u) {
            if (channels == 2u) {
                handle_mix_packet(audio_fd, &peer, peer_len, packet, (size_t)got, frames);
            } else {
                atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
            }
            continue;
        }

        /* Route block first (module id + session), then parameters, then audio. */
        char module_id[NA_RT_MODULE_ID_LEN + 1u] = {0};
        uint64_t session_key = 0u;
        if ((flags & NA_RT_FLAG_ROUTE) != 0u) {
            if ((size_t)got < payload_offset + NA_RT_ROUTE_BLOCK_LEN) {
                atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
                continue;
            }
            memcpy(module_id, packet + payload_offset, NA_RT_MODULE_ID_LEN);
            module_id[NA_RT_MODULE_ID_LEN] = '\0';
            session_key = ((uint64_t)read_u32_network(packet + payload_offset + NA_RT_MODULE_ID_LEN) << 32) |
                          read_u32_network(packet + payload_offset + NA_RT_MODULE_ID_LEN + 4u);
            payload_offset += NA_RT_ROUTE_BLOCK_LEN;
        }

        NaGraph *route_graph = atomic_load_explicit(&g_active_graph, memory_order_acquire);
        if (route_graph == NULL) {
            atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
            continue;
        }
        const uint32_t module_index = resolve_module_index(route_graph, module_id);
        void *session_state = resolve_session_state(route_graph, module_index, session_key);
        if (session_state == NULL) {
            atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
            continue;
        }

        if ((flags & NA_RT_FLAG_PARAMETERS) != 0u) {
            if ((size_t)got < payload_offset + 4u) {
                atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
                continue;
            }
            const uint16_t param_count = read_u16_network(packet + payload_offset);
            payload_offset += 4u;
            if (param_count > NA_RT_MAX_PACKET_PARAMS ||
                (size_t)got < payload_offset + ((size_t)param_count * 8u)) {
                atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
                continue;
            }
            for (uint16_t i = 0u; i < param_count; ++i) {
                const uint32_t index = read_u32_network(packet + payload_offset);
                const float value = read_float_network(packet + payload_offset + 4u);
                apply_param_to_state(route_graph, module_index, session_state, index, value);
                payload_offset += 8u;
            }
        }

        if ((size_t)got != payload_offset + payload_bytes) {
            atomic_fetch_add_explicit(&g_stats.bad_packets, 1u, memory_order_relaxed);
            continue;
        }

        atomic_fetch_add_explicit(&g_stats.packets_in, 1u, memory_order_relaxed);
        process_block((float *)(packet + payload_offset), channels, frames, module_index, session_state);

        if (payload_offset != sizeof(NaRtPacketHeader)) {
            memmove(packet + sizeof(NaRtPacketHeader), packet + payload_offset, payload_bytes);
            got = (ssize_t)(sizeof(NaRtPacketHeader) + payload_bytes);
            header = (NaRtPacketHeader *)packet;
            header->flags = htonl(0u);
        }

        if (sendto(audio_fd, packet, (size_t)got, 0,
                   (struct sockaddr *)&peer, peer_len) == got) {
            atomic_fetch_add_explicit(&g_stats.packets_out, 1u, memory_order_relaxed);
        }
    }

    close(audio_fd);
}

static void read_first_line(const char *path, char *out, size_t out_size) {
    if (out_size == 0u) {
        return;
    }
    out[0] = '\0';
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }
    if (fgets(out, (int)out_size, file) != NULL) {
        out[strcspn(out, "\r\n")] = '\0';
    }
    fclose(file);
}

static void read_cpu_model(char *out, size_t out_size) {
    read_first_line("/proc/cpuinfo", out, out_size);
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (file == NULL) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "model name", 10) == 0) {
            char *colon = strchr(line, ':');
            if (colon != NULL) {
                ++colon;
                while (*colon == ' ' || *colon == '\t') {
                    ++colon;
                }
                snprintf(out, out_size, "%s", colon);
                out[strcspn(out, "\r\n")] = '\0';
                break;
            }
        }
    }
    fclose(file);
}

static long read_mem_total_mb(void) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        return 0;
    }
    char key[64];
    long kb = 0;
    char unit[32];
    while (fscanf(file, "%63s %ld %31s", key, &kb, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) {
            fclose(file);
            return kb / 1024;
        }
    }
    fclose(file);
    return 0;
}

static double read_cpu_mhz(void) {
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (file == NULL) {
        return 0.0;
    }
    char line[512];
    double mhz = 0.0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "cpu MHz", 7) == 0) {
            char *colon = strchr(line, ':');
            if (colon != NULL) {
                mhz = strtod(colon + 1, NULL);
                break;
            }
        }
    }
    fclose(file);
    return mhz;
}

static double read_temperature_c(void) {
    const char *paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/hwmon/hwmon0/temp1_input",
        "/sys/class/hwmon/hwmon1/temp1_input",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        char value[64];
        read_first_line(paths[i], value, sizeof(value));
        if (value[0] != '\0') {
            const double raw = strtod(value, NULL);
            return raw > 1000.0 ? raw / 1000.0 : raw;
        }
    }
    return -1.0;
}

static void read_mac_address(char *out, size_t out_size) {
    read_first_line("/sys/class/net/enp3s0/address", out, out_size);
    if (out[0] == '\0') {
        read_first_line("/sys/class/net/eth0/address", out, out_size);
    }
}

static uint32_t read_cpu_core_loads(double *loads, uint32_t max_loads) {
    static uint64_t prev_total[64];
    static uint64_t prev_idle[64];
    static int initialized = 0;

    if (loads == NULL || max_loads == 0u) {
        return 0u;
    }
    for (uint32_t i = 0; i < max_loads; ++i) {
        loads[i] = 0.0;
    }

    FILE *file = fopen("/proc/stat", "r");
    if (file == NULL) {
        return 0u;
    }

    char line[512];
    uint32_t count = 0u;
    while (fgets(line, sizeof(line), file) != NULL && count < max_loads) {
        unsigned int cpu_index = 0u;
        unsigned long long user = 0u;
        unsigned long long nice = 0u;
        unsigned long long system = 0u;
        unsigned long long idle = 0u;
        unsigned long long iowait = 0u;
        unsigned long long irq = 0u;
        unsigned long long softirq = 0u;
        unsigned long long steal = 0u;
        if (sscanf(line,
                   "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
                   &cpu_index,
                   &user,
                   &nice,
                   &system,
                   &idle,
                   &iowait,
                   &irq,
                   &softirq,
                   &steal) != 9) {
            continue;
        }
        if (cpu_index >= 64u) {
            continue;
        }
        const uint64_t idle_all = (uint64_t)idle + (uint64_t)iowait;
        const uint64_t total = (uint64_t)user + (uint64_t)nice + (uint64_t)system +
                               (uint64_t)idle + (uint64_t)iowait + (uint64_t)irq +
                               (uint64_t)softirq + (uint64_t)steal;
        if (initialized) {
            const uint64_t total_delta = total >= prev_total[cpu_index] ? total - prev_total[cpu_index] : 0u;
            const uint64_t idle_delta = idle_all >= prev_idle[cpu_index] ? idle_all - prev_idle[cpu_index] : 0u;
            if (total_delta > 0u) {
                loads[count] = 100.0 * (double)(total_delta > idle_delta ? total_delta - idle_delta : 0u) / (double)total_delta;
            }
        }
        prev_total[cpu_index] = total;
        prev_idle[cpu_index] = idle_all;
        ++count;
    }
    fclose(file);
    initialized = 1;
    return count;
}

static void build_status_payload(char *out, size_t out_size) {
    /* Every loaded module, comma-joined. Doubles as the capability signal: a DAW only sends the
     * route block once it has seen plugin_ids, so an old engine never receives packets it would
     * count as bad. */
    char plugin_ids[NA_RT_MAX_MODULES * (NA_RT_MODULE_ID_LEN + 1u)] = "";
    NaGraph *status_graph = atomic_load_explicit(&g_active_graph, memory_order_acquire);
    if (status_graph != NULL) {
        for (uint32_t i = 0u; i < status_graph->node_count; ++i) {
            const char *id = status_graph->nodes[i].plugin->info.id;
            if (id == NULL) {
                continue;
            }
            if (plugin_ids[0] != '\0') {
                strncat(plugin_ids, ",", sizeof(plugin_ids) - strlen(plugin_ids) - 1u);
            }
            strncat(plugin_ids, id, sizeof(plugin_ids) - strlen(plugin_ids) - 1u);
        }
    }
    char hostname[128] = "debian";
    char cpu_model[512] = "unknown";
    char mac[64] = "unknown";
    double cpu_loads[64];
    char cpu_core_loads[512] = "";
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1u] = '\0';
    read_cpu_model(cpu_model, sizeof(cpu_model));
    read_mac_address(mac, sizeof(mac));
    const long mem_mb = read_mem_total_mb();
    const double cpu_mhz = read_cpu_mhz();
    const double temp_c = read_temperature_c();
    const double temp_f = temp_c >= 0.0 ? (temp_c * 9.0 / 5.0) + 32.0 : -1.0;
    const uint32_t cpu_load_count = read_cpu_core_loads(cpu_loads, 64u);
    for (uint32_t i = 0u; i < cpu_load_count; ++i) {
        char chunk[24];
        snprintf(chunk, sizeof(chunk), "%s%.1f", i == 0u ? "" : ",", cpu_loads[i]);
        strncat(cpu_core_loads, chunk, sizeof(cpu_core_loads) - strlen(cpu_core_loads) - 1u);
    }
    const uint64_t packets_in = atomic_load_explicit(&g_stats.packets_in, memory_order_relaxed);
    const uint64_t packets_out = atomic_load_explicit(&g_stats.packets_out, memory_order_relaxed);
    const uint64_t bad_packets = atomic_load_explicit(&g_stats.bad_packets, memory_order_relaxed);
    snprintf(out,
             out_size,
             "vendor=Neuracoust\n"
             "model=누라쿠스트 DSP 서버\n"
             "version=260703.1500\n"
             "hostname=%s\n"
             "mac=%s\n"
             "cpu_model=%s\n"
             "cpu_mhz=%.1f\n"
             "memory_mb=%ld\n"
             "temperature_c=%.1f\n"
	             "temperature_f=%.1f\n"
	             "cpu_core_loads=%s\n"
	             "nic=enp3s0\n"
             "audio_port=%u\n"
             "monitor_port=%u\n"
             "channels=128\n"
             "core_count=4\n"
             "buffer_profiles=40:0.8,56:1.2,80:1.7,112:2.3,160:3.3,224:4.7\n"
             "performance_modes=latency,dsp\n"
             "lpfc=off\n"
             "lpee=on\n"
             "plugin_id=%s\n"
             "plugin_name=%s\n"
             "plugin_ids=%s\n"
             "packets_in=%llu\n"
             "packets_out=%llu\n"
             "bad_packets=%llu\n",
             hostname,
             mac,
             cpu_model,
             cpu_mhz,
             mem_mb,
	             temp_c,
	             temp_f,
	             cpu_core_loads,
	             g_audio_port,
             g_monitor_port,
             g_loaded_plugin_id,
             g_loaded_plugin_name,
             plugin_ids,
             (unsigned long long)packets_in,
             (unsigned long long)packets_out,
             (unsigned long long)bad_packets);
}

static void *run_monitor_server(void *arg) {
    (void)arg;
    int monitor_fd = bind_udp(g_monitor_port);
    if (monitor_fd < 0) {
        return NULL;
    }
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    setsockopt(monitor_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    printf("neuracoust-rt-engine status listening on UDP %u\n", g_monitor_port);
    while (g_running) {
        char request[128];
        char response[2048];
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t got = recvfrom(monitor_fd, request, sizeof(request) - 1u, 0,
                               (struct sockaddr *)&peer, &peer_len);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }
        request[got] = '\0';
        /* NA_STATUS is the direct query; NA_DISCOVER is the DAW's LAN broadcast. Both deserve
         * the same status reply — answering only the first is why the DAW's 검색 button never
         * found this node while a typed-in address worked fine. */
        if (strstr(request, "NA_STATUS") == NULL && strstr(request, "NA_DISCOVER") == NULL) {
            continue;
        }
        build_status_payload(response, sizeof(response));
        sendto(monitor_fd, response, strlen(response), 0, (struct sockaddr *)&peer, peer_len);
    }
    close(monitor_fd);
    return NULL;
}

int main(int argc, char **argv) {
    const char *module_paths[NA_RT_MAX_MODULES];
    uint32_t module_count = 0u;
    int self_test = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            self_test = 1;
        } else if (strcmp(argv[i], "--module") == 0 && i + 1 < argc) {
            if (module_count >= NA_RT_MAX_MODULES) {
                fprintf(stderr, "too many --module arguments (max %u)\n", NA_RT_MAX_MODULES);
                return 2;
            }
            module_paths[module_count++] = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            long port = strtol(argv[++i], NULL, 10);
            if (port > 0 && port <= 65535) {
                g_audio_port = (uint16_t)port;
            }
        } else if (strcmp(argv[i], "--monitor-port") == 0 && i + 1 < argc) {
            long port = strtol(argv[++i], NULL, 10);
            if (port > 0 && port <= 65535) {
                g_monitor_port = (uint16_t)port;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: neuracoust-rt-engine [--module a.so] [--module b.so ...] [--self-test] [--port UDP_PORT] [--monitor-port UDP_PORT]\n");
            return 0;
        } else {
            fprintf(stderr, "usage: neuracoust-rt-engine [--module a.so] [--module b.so ...] [--self-test] [--port UDP_PORT] [--monitor-port UDP_PORT]\n");
            return 2;
        }
    }
    const char *env_port = getenv("NA_RT_AUDIO_PORT");
    if (env_port != NULL && env_port[0] != '\0') {
        long port = strtol(env_port, NULL, 10);
        if (port > 0 && port <= 65535) {
            g_audio_port = (uint16_t)port;
        }
    }

    if (self_test) {
        return run_self_test(module_paths, module_count);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    NaGraph *graph = module_count > 0u ? load_plugin_graph_multi(module_paths, module_count)
                                       : create_initial_graph();
    if (graph == NULL) {
        fprintf(stderr, "failed to allocate initial graph\n");
        return 1;
    }
    atomic_store_explicit(&g_active_graph, graph, memory_order_release);

    (void)set_realtime();
    pthread_t monitor_thread;
    int monitor_started = pthread_create(&monitor_thread, NULL, run_monitor_server, NULL) == 0;
    run_engine();
    if (monitor_started) {
        pthread_join(monitor_thread, NULL);
    }
    return 0;
}
