#pragma once

// Pure-C facade over the C++ engine so Swift can drive it through a bridging
// header. Every function here must be called on the main thread — the engine's
// public API is main-thread-only (see docs/legacy-ui-contract.md §1).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_MAX_TRACK_METERS 64
#define NC_TEXT_LEN 128

// Flat mirror of AudioEngineStatus, trimmed to what the shell and transport
// need. Grows as panels land — keep it a plain C struct so Swift sees it as a
// value type with no bridging cost at 30 Hz.
typedef struct {
    bool running;
    bool transportRunning;

    double sampleRate;
    int outputChannels;
    int requestedBufferSize;

    float outputPeakLeft;
    float outputPeakRight;
    float phaseCorrelation;
    float spectrumLow;
    float spectrumMid;
    float spectrumHigh;

    double playbackSeconds;

    bool delayCompensationEnabled;
    double delayCompensationMs;

    int inputChannels;
    float inputPeak;
    int recordArmedTrackCount;

    // Realtime telemetry. Wake jitter is meaningless on its own — judge severity
    // against render headroom, not the raw number (Waves SoundGrid delivers
    // callbacks in bursts, so an idle system still reads ~1 buffer period).
    uint64_t realtimeCallbackCount;
    double realtimeAverageWakeJitterUs;
    double realtimeMaxWakeJitterUs;
    double realtimeMaxRenderDurationUs;
    int realtimeLateWakeCount;

    bool remoteDspMonitorActive;
    double remoteDspRoundTripMs;

    int trackMeterCount;
    float trackPeakLeft[NC_MAX_TRACK_METERS];
    float trackPeakRight[NC_MAX_TRACK_METERS];

    char deviceName[NC_TEXT_LEN];
    char dspEngineName[NC_TEXT_LEN];
    char monitorDspPathMode[NC_TEXT_LEN];
    char message[NC_TEXT_LEN];
} NCEngineStatus;

typedef struct NCEngine NCEngine;

NCEngine* nc_engine_create(void);
void nc_engine_destroy(NCEngine* engine);

// Loads the default project into the engine, then opens the audio device.
// Returns false and fills `error` (up to `errorLen` bytes, NUL-terminated).
bool nc_engine_start(NCEngine* engine, char* error, size_t errorLen);
void nc_engine_stop(NCEngine* engine);

void nc_engine_status(NCEngine* engine, NCEngineStatus* out);

void nc_engine_set_transport_running(NCEngine* engine, bool running);
void nc_engine_set_recording(NCEngine* engine, bool active);
void nc_engine_seek(NCEngine* engine, double seconds);
void nc_engine_rewind(NCEngine* engine);

void nc_engine_set_metronome_enabled(NCEngine* engine, bool enabled);
void nc_engine_set_test_tone_enabled(NCEngine* engine, bool enabled);

// Project readouts. `out` receives a NUL-terminated string.
void nc_project_name(NCEngine* engine, char* out, size_t outLen);
void nc_project_timecode(NCEngine* engine, double seconds, char* out, size_t outLen);
int nc_project_tempo_bpm(NCEngine* engine);
int nc_project_time_signature_numerator(NCEngine* engine);
int nc_project_time_signature_denominator(NCEngine* engine);

// Bars|beats|ticks at `seconds`, 1-based bar and beat, 960 ticks per beat.
void nc_project_bars_beats(NCEngine* engine, double seconds, int* bar, int* beat, int* tick);

bool nc_project_loop_enabled(NCEngine* engine);
void nc_project_set_loop_enabled(NCEngine* engine, bool enabled);
double nc_project_loop_start(NCEngine* engine);
double nc_project_loop_end(NCEngine* engine);

// ---------------------------------------------------------------------------
// Monitor station
// ---------------------------------------------------------------------------

// Monitor DSP module chain, seeded from defaultMonitorDspModules().
int nc_monitor_module_count(NCEngine* engine);
void nc_monitor_module_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_monitor_module_detail(NCEngine* engine, int index, char* out, size_t outLen);
void nc_monitor_module_stage(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_monitor_module_enabled(NCEngine* engine, int index);
void nc_monitor_set_module_enabled(NCEngine* engine, int index, bool enabled);

bool nc_monitor_dsp_enabled(NCEngine* engine);
void nc_monitor_set_dsp_enabled(NCEngine* engine, bool enabled);

void nc_monitor_path_mode(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_path_mode(NCEngine* engine, const char* mode);

// Station controls. Every setter re-pushes the whole control set to the engine,
// which is the only entry point it offers.
float nc_monitor_volume_db(NCEngine* engine);
void nc_monitor_set_volume_db(NCEngine* engine, float db);

bool nc_monitor_mono(NCEngine* engine);
bool nc_monitor_mute(NCEngine* engine);
bool nc_monitor_dim(NCEngine* engine);
bool nc_monitor_talkback(NCEngine* engine);
void nc_monitor_set_mono(NCEngine* engine, bool on);
void nc_monitor_set_mute(NCEngine* engine, bool on);
void nc_monitor_set_dim(NCEngine* engine, bool on);
void nc_monitor_set_talkback(NCEngine* engine, bool on);

// "LR", "MS", "L", "R" — mirrors ProjectDocument::monitorStationListenMode.
void nc_monitor_listen_mode(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_listen_mode(NCEngine* engine, const char* mode);

// A/B/C speaker sets live on the "speaker-simulation" module. Slot is 0, 1 or 2.
int nc_monitor_active_speaker_slot(NCEngine* engine);
void nc_monitor_set_active_speaker_slot(NCEngine* engine, int slot);
void nc_monitor_speaker_model(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_monitor_speaker_output(NCEngine* engine, int slot, char* out, size_t outLen);
float nc_monitor_speaker_sim_weight(NCEngine* engine, int slot);
bool nc_monitor_speaker_room_eq(NCEngine* engine, int slot);

// ---------------------------------------------------------------------------
// Listen Room
//
// The engine owns the sender (encode + push to the relay). The relay itself is a
// separate Python process the app must launch; see ListenRoom.swift.
// ---------------------------------------------------------------------------

typedef struct {
    bool enabled;
    bool senderRunning;
    bool relayReachable;
    bool nativeWebRtcOfferReady;
    bool nativeWebRtcConnected;
    uint64_t packetsQueued;
    uint64_t packetsSent;
    uint64_t packetsDropped;
    uint64_t sendFailures;
    int queuedBlocks;
    int latencyTargetMs;
    int targetBitrateKbps;
    char shareUrl[256];
    char activeCodec[NC_TEXT_LEN];
    char qualityLabel[NC_TEXT_LEN];
    char transportMode[NC_TEXT_LEN];
    char message[NC_TEXT_LEN];
} NCListenStatus;

void nc_listen_status(NCEngine* engine, NCListenStatus* out);

bool nc_listen_enabled(NCEngine* engine);
/// Enabling mints an access token if the project has none, then pushes settings
/// to the engine. Launching the relay process is the caller's job.
void nc_listen_set_enabled(NCEngine* engine, bool enabled);

void nc_listen_session_name(NCEngine* engine, char* out, size_t outLen);
void nc_listen_access_token(NCEngine* engine, char* out, size_t outLen);
void nc_listen_relay_host(NCEngine* engine, char* out, size_t outLen);
int nc_listen_relay_http_port(NCEngine* engine);
int nc_listen_relay_tcp_ingest_port(NCEngine* engine);

void nc_listen_quality(NCEngine* engine, char* out, size_t outLen);
void nc_listen_set_quality(NCEngine* engine, const char* quality);
void nc_listen_latency_mode(NCEngine* engine, char* out, size_t outLen);
void nc_listen_set_latency_mode(NCEngine* engine, const char* mode);

/// Share link for the local relay, and the token-bearing invite link.
void nc_listen_share_url(NCEngine* engine, char* out, size_t outLen);
void nc_listen_public_share_url(NCEngine* engine, char* out, size_t outLen);

#ifdef __cplusplus
} // extern "C"
#endif
