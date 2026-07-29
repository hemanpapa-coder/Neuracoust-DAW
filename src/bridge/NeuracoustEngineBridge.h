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

// UI localization (global, no engine handle). Set the language from an OS locale tag
// once at launch; tr() then resolves a key in that language, falling back to English,
// then to the key itself. Languages: Korean, English, Japanese, Chinese (Simplified).
void nc_set_ui_language(const char* localeTag);
void nc_tr(const char* key, char* out, size_t outLen);

#define NC_NAME_LEN 64
#define NC_MAX_INSERT_SLOTS 10
#define NC_MAX_SEND_SLOTS 5

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
    /// Post master fader, pre monitor path — the mixer's Master meter (outputPeak* follows the
    /// monitor volume knob and must not drive it).
    /// Monitor bus before the monitor level — the transport L/R meter (see the engine).
    float monitorPrePeakLeft;
    float monitorPrePeakRight;
    float masterBusPeakLeft;
    float masterBusPeakRight;
    float phaseCorrelation;
    float spectrumLow;
    float spectrumMid;
    float spectrumHigh;

    // ITU-R BS.1770 loudness. True-peak is 4× oversampled (inter-sample peaks).
    float momentaryLufs;
    float shortTermLufs;
    float integratedLufs;
    float loudnessRange;
    float truePeakDb;

    double playbackSeconds;

    bool delayCompensationEnabled;
    double delayCompensationMs;

    int inputChannels;
    float inputPeak;
    float inputPeakLeft;
    float inputPeakRight;
    int recordArmedTrackCount;

    // Realtime telemetry. Wake jitter is meaningless on its own — judge severity
    // against render headroom, not the raw number (Waves SoundGrid delivers
    // callbacks in bursts, so an idle system still reads ~1 buffer period).
    uint64_t realtimeCallbackCount;
    double realtimeAverageWakeJitterUs;
    double realtimeMaxWakeJitterUs;
    double realtimeMaxRenderDurationUs;
    /// Worst recent render as a fraction of the period that callback was allowed (see the engine).
    double realtimeMaxRenderLoad;
    /// The same load, split: what the Mac actually computed vs. time the render callback spent
    /// WAITING on remote DSP nodes (network round trips fill the wall clock while the CPU idles).
    double realtimeMaxLocalRenderLoad;
    double realtimeMaxRemoteWaitLoad;
    /// Server-side timing wobble: EMA of block-to-block remote round-trip variation (µs).
    double remoteDspRoundTripJitterUs;
    int realtimeLateWakeCount;
    // Reference-tap ("다른 앱") FIFO faults. The wake-jitter figures above describe the OUTPUT
    // render thread ONLY — a tap capture that starves or overflows crackles while the render
    // reads perfectly clean. These are that blind spot, made visible.
    uint64_t referenceUnderrunBlocks;
    uint64_t referenceOverrunDrops;

    bool remoteDspMonitorActive;
    double remoteDspRoundTripMs;
    // Remote-mixer provenance: buses the node sums right now + cumulative sums/misses (a miss
    // is bit-identical locally — an honesty meter, not a quality alarm).
    unsigned int remoteMixBusCount;
    unsigned long long remoteMixSums;
    unsigned long long remoteMixMisses;

    // How many inserts the engine is actually running, and where. The only honest
    // way to tell that a plug-in really loaded.
    int activeRealtimeVst3TrackInserts;
    int activeRealtimeVst3MasterInserts;
    int activeRemoteDspTrackInserts;
    int activeOfflineVst3TrackInserts;

    // Meters arrive as parallel arrays keyed by track name, not by track index.
    int trackMeterCount;
    char trackMeterNames[NC_MAX_TRACK_METERS][NC_NAME_LEN];
    float trackPeakLeft[NC_MAX_TRACK_METERS];
    float trackPeakRight[NC_MAX_TRACK_METERS];
    float trackConsoleGainReductionDb[NC_MAX_TRACK_METERS];
    float trackConsoleGateGainReductionDb[NC_MAX_TRACK_METERS];

    char deviceName[NC_TEXT_LEN];
    char dspEngineName[NC_TEXT_LEN];
    char monitorDspPathMode[NC_TEXT_LEN];
    char message[NC_TEXT_LEN];
} NCEngineStatus;

typedef struct NCEngine NCEngine;

typedef struct NCHuiEvent {
    int type;       // 0 none, 1 fader, 2 pan, 3 select, 4 mute, 5 solo, 6 arm,
                    // 7 play, 8 stop, 9 record, 10 rewind, 11 fast-forward
    int trackIndex;
    float value;
    bool pressed;
} NCHuiEvent;

NCEngine* nc_engine_create(void);
void nc_engine_destroy(NCEngine* engine);

// Loads the default project into the engine, then opens the audio device.
// Returns false and fills `error` (up to `errorLen` bytes, NUL-terminated).
bool nc_engine_start(NCEngine* engine, char* error, size_t errorLen);
void nc_engine_stop(NCEngine* engine);

// Mackie HUI control surface. Input and output are deliberately separate because
// most HUI devices expose a matched pair of virtual CoreMIDI ports.
int  nc_hui_input_count(NCEngine* engine);
int  nc_hui_output_count(NCEngine* engine);
void nc_hui_input_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_hui_input_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_hui_output_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_hui_output_name(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_hui_connect(NCEngine* engine, const char* inputId, const char* outputId);
void nc_hui_disconnect(NCEngine* engine);
bool nc_hui_connected(NCEngine* engine);
void nc_hui_status(NCEngine* engine, char* out, size_t outLen);
bool nc_hui_next_event(NCEngine* engine, NCHuiEvent* out);
void nc_hui_sync(NCEngine* engine, bool transportRunning, bool recording);

void nc_engine_status(NCEngine* engine, NCEngineStatus* out);

// FFT spectrum bins for the analyzer (0..1, dB-scaled, low→high frequency). Cached on
// each nc_engine_status call, so poll status first, then read these.
int nc_spectrum_bin_count(NCEngine* engine);
bool nc_spectrum_bins(NCEngine* engine, float* out, int count);

// Goniometer L/R sample pairs (interleaved L,R,L,R…). Cached on each nc_engine_status.
int nc_goniometer_sample_count(NCEngine* engine);
bool nc_goniometer_samples(NCEngine* engine, float* out, int count);

void nc_engine_set_transport_running(NCEngine* engine, bool running);
void nc_engine_set_recording(NCEngine* engine, bool active);
// Audio recording-to-disk (V1). Begin captures the record-armed track's input (physical pair or
// the "다른 앱" reference tap); end saves a WAV and drops a clip at the record-start position.
bool nc_engine_begin_audio_record(NCEngine* engine);
bool nc_engine_finish_audio_record(NCEngine* engine, char* outPath, size_t pathLen,
                                   char* outError, size_t errLen);
bool nc_engine_add_take_clip(NCEngine* engine, const char* path, double clipStartSeconds,
                             double sourceOffsetSeconds, double durationSeconds,
                             char* outClipId, size_t outLen, char* outError, size_t errLen);
bool nc_engine_audio_recording_active(NCEngine* engine);
void nc_engine_discard_audio_record(NCEngine* engine);
double nc_recording_live_seconds(NCEngine* engine);
int nc_recording_live_peak_count(NCEngine* engine);
int nc_recording_live_peaks_since(NCEngine* engine, int fromBucket, float* outLR, int maxBuckets);
int nc_recording_channels(NCEngine* engine);
int nc_recording_peak_samples(NCEngine* engine);
void nc_engine_seek(NCEngine* engine, double seconds);
void nc_engine_rewind(NCEngine* engine);

void nc_engine_set_metronome_enabled(NCEngine* engine, bool enabled);
void nc_engine_set_metronome_subdivision(NCEngine* engine, const char* subdivision);
void nc_engine_set_metronome_gain(NCEngine* engine, float gain);
void nc_engine_set_metronome_sound(NCEngine* engine, const char* sound);
void nc_engine_set_groove(NCEngine* engine, const char* feel, float swingAmount);
void nc_engine_set_metronome_accent_first(NCEngine* engine, bool accent);
void nc_engine_set_metronome_pattern(NCEngine* engine, const float* gains, int count);
void nc_engine_set_metronome_genre(NCEngine* engine, const char* genre);
// Getters — the UI reloads metronome settings from an opened project (they persist in the .ndaw).
float nc_metronome_gain(NCEngine* engine);
void  nc_metronome_sound(NCEngine* engine, char* out, size_t outLen);
bool  nc_metronome_accent_first(NCEngine* engine);
void  nc_metronome_genre(NCEngine* engine, char* out, size_t outLen);
/// Renders the current metronome sound/groove into a new audio track. When
/// `loopRangeOnly` is true the current loop/edit range is printed; otherwise the
/// complete session extent is printed. The generated WAV is kept in Audio Files.
bool nc_metronome_print_to_track(NCEngine* engine, bool loopRangeOnly,
                                  char* error, size_t errorLen);
void nc_engine_set_test_tone_enabled(NCEngine* engine, bool enabled);

// Project readouts. `out` receives a NUL-terminated string.
void nc_project_name(NCEngine* engine, char* out, size_t outLen);
void nc_project_timecode(NCEngine* engine, double seconds, char* out, size_t outLen);
int nc_project_tempo_bpm(NCEngine* engine);
int nc_project_time_signature_numerator(NCEngine* engine);
int nc_project_time_signature_denominator(NCEngine* engine);
/// Set the base tempo / time signature (the transport TEMPO and SIG fields). Each keeps the
/// t=0 conductor anchor in sync so the transport and the conductor lanes always agree.
bool nc_project_set_tempo_bpm(NCEngine* engine, int bpm);
bool nc_project_set_time_signature(NCEngine* engine, int numerator, int denominator);

// Bars|beats|ticks at `seconds`, 1-based bar and beat, 960 ticks per beat.
void nc_project_bars_beats(NCEngine* engine, double seconds, int* bar, int* beat, int* tick);

bool nc_project_loop_enabled(NCEngine* engine);
void nc_project_set_loop_enabled(NCEngine* engine, bool enabled);
double nc_project_loop_start(NCEngine* engine);
double nc_project_loop_end(NCEngine* engine);
double nc_project_pre_roll(NCEngine* engine);
double nc_project_post_roll(NCEngine* engine);
void nc_project_set_pre_post_roll(NCEngine* engine, double preRollSeconds, double postRollSeconds);

// ---------------------------------------------------------------------------
// Tracks / mixer
//
// Tracks are addressed by index into ProjectDocument::tracks. Mutations edit the
// project model and push the change through the engine's fine-grained realtime
// setters — never a full loadProject.
// ---------------------------------------------------------------------------

int nc_track_count(NCEngine* engine);

void nc_track_name(NCEngine* engine, int index, char* out, size_t outLen);
/// "audio", "instrument", "midi", "aux", "vca", "folder", "bus", "master", "monitor"
void nc_track_type(NCEngine* engine, int index, char* out, size_t outLen);
/// "#RRGGBB", possibly empty.
void nc_track_color(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_folder(NCEngine* engine, int index, char* out, size_t outLen);
// A free-text channel memo (no audio effect). The setter records an undo step.
void nc_track_notes(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_set_notes(NCEngine* engine, int index, const char* notes);
void nc_track_input_bus(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_output_bus(NCEngine* engine, int index, char* out, size_t outLen);
// Route a track's input/output; both reconcile the mixer graph. Output options are
// Master plus any aux/bus tracks (count then name, cached between the two calls).
void nc_track_set_input_bus(NCEngine* engine, int index, const char* bus);
void nc_track_set_output_bus(NCEngine* engine, int index, const char* bus);
// Track channel format: "mono" or "stereo". The renderer sums a mono track to one channel
// panned into the field; setting it reconciles the graph.
void nc_track_channel_format(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_set_channel_format(NCEngine* engine, int index, const char* format);
int  nc_track_output_option_count(NCEngine* engine, int index);
void nc_track_output_option(NCEngine* engine, int index, int i, char* out, size_t outLen);

float nc_track_volume_db(NCEngine* engine, int index);
float nc_track_pan(NCEngine* engine, int index);
bool nc_track_muted(NCEngine* engine, int index);
bool nc_track_solo(NCEngine* engine, int index);
bool nc_track_record_armed(NCEngine* engine, int index);
bool nc_track_input_monitoring(NCEngine* engine, int index);

void nc_track_set_volume_db(NCEngine* engine, int index, float db);
void nc_track_set_pan(NCEngine* engine, int index, float pan);
void nc_track_set_muted(NCEngine* engine, int index, bool muted);
void nc_track_set_solo(NCEngine* engine, int index, bool solo);
void nc_track_set_record_armed(NCEngine* engine, int index, bool armed);
void nc_track_set_input_monitoring(NCEngine* engine, int index, bool monitoring);
void nc_track_console_model(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_set_console_model(NCEngine* engine, int index, const char* name);
void nc_track_console_harmonics(NCEngine* engine, int index, float* out, int count);
int nc_track_console_bias_seed(NCEngine* engine, int index);
float nc_track_console_bias_depth(NCEngine* engine, int index);
void nc_console_bias_auto(NCEngine* engine, float depth);
void nc_console_bias_off(NCEngine* engine);
void nc_track_set_console_bias_seed(NCEngine* engine, int index, int seed);
void nc_track_console_comp_type(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_set_console_comp_type(NCEngine* engine, int index, const char* name);
void nc_track_console_gate_type(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_set_console_gate_type(NCEngine* engine, int index, const char* name);
void nc_track_console_module_order(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_set_console_module_order(NCEngine* engine, int index, const char* order);
bool nc_track_console_bool(NCEngine* engine, int index, const char* parameter);
float nc_track_console_value(NCEngine* engine, int index, const char* parameter);
void nc_track_set_console_bool(NCEngine* engine, int index, const char* parameter, bool value);
void nc_track_set_console_value(NCEngine* engine, int index, const char* parameter, float value);
// Apply one flag to several selected tracks in one undo step. flag: 0=mute 1=solo 2=armed 3=inputMon.
bool nc_track_set_flag_many(NCEngine* engine, const int* indices, int count, int flag, bool value);

/// New tracks land at the end of the list. Returns the new track's index, or -1.
int nc_track_add_audio(NCEngine* engine);
int nc_track_add_instrument(NCEngine* engine);

/// Loads a scanned plug-in into the track's instrument slot — the thing that turns
/// its MIDI notes into sound. `pluginIndex` addresses the filtered browser list.
bool nc_track_set_instrument(NCEngine* engine, int trackIndex, int pluginIndex);
// Remove the instrument from a track's instrument slot.
bool nc_track_clear_instrument(NCEngine* engine, int trackIndex);

// Instrument rack (layering): up to 8 instruments on one track, all fed the same MIDI and
// summed. Slot 0 is the primary instrument; higher slots are layers.
int  nc_track_instrument_slot_count(NCEngine* engine, int trackIndex);
void nc_track_instrument_slot_name(NCEngine* engine, int trackIndex, int slotIndex, char* out, size_t outLen);
bool nc_track_set_instrument_slot(NCEngine* engine, int trackIndex, int slotIndex, int pluginIndex);
bool nc_track_remove_instrument_slot(NCEngine* engine, int trackIndex, int slotIndex);
/// Per-layer mute (bypass) and solo within the instrument rack.
bool nc_track_instrument_slot_bypassed(NCEngine* engine, int trackIndex, int slotIndex);
bool nc_track_instrument_slot_soloed(NCEngine* engine, int trackIndex, int slotIndex);
bool nc_track_toggle_instrument_slot_bypass(NCEngine* engine, int trackIndex, int slotIndex);
bool nc_track_toggle_instrument_slot_solo(NCEngine* engine, int trackIndex, int slotIndex);

/// Duplicates a track with all its settings (inserts, sends, routing, clips), optionally
/// excluding clips/inserts/sends. Returns the new track's index, or -1 on failure.
int nc_track_duplicate(NCEngine* engine, int trackIndex,
                       bool includeClips, bool includeInserts, bool includeSends);

/// Reorder a mixer channel next to another track (before/after), by name.
bool nc_track_move_near(NCEngine* engine, const char* sourceName, const char* targetName, bool after);

/// Shuffle (ripple) edit mode. A shuffle move drops the clip and slides its
/// neighbours to close/open the gap; a shuffle delete removes the range and pulls
/// later clips left to fill it. Both record their own step.
bool nc_clip_shuffle_move(NCEngine* engine, const char* clipId, double newStartSeconds);
int nc_clip_shuffle_delete_range(NCEngine* engine, double startSeconds, double endSeconds);
int nc_clip_shuffle_delete_many(NCEngine* engine, const char* const* clipIds, int count);
void nc_track_instrument_name(NCEngine* engine, int trackIndex, char* out, size_t outLen);
int nc_track_add_midi(NCEngine* engine);

/// Deletes the track and, when `removeClips` is true, everything on it. Refuses on
/// Master and Monitor. Returns false when the track cannot go.
bool nc_track_delete(NCEngine* engine, int index, bool removeClips);
// Delete several tracks (by index) in a single undo step. Indices are resolved to names before any
// deletion so the shift from removing one does not misaddress the rest.
bool nc_track_delete_many(NCEngine* engine, const int* indices, int count, bool removeClips);

/// Fails when the name is empty or already taken.
bool nc_track_rename(NCEngine* engine, int index, const char* newName);

int nc_track_insert_count(NCEngine* engine, int index);
void nc_track_insert_name(NCEngine* engine, int index, int slot, char* out, size_t outLen);
bool nc_track_insert_bypassed(NCEngine* engine, int index, int slot);
void nc_track_set_insert_bypassed(NCEngine* engine, int index, int slot, bool bypassed);

/// Everything the out-of-process editor host needs on its command line.
void nc_track_insert_plugin_path(NCEngine* engine, int index, int slot, char* out, size_t outLen);
void nc_track_insert_plugin_format(NCEngine* engine, int index, int slot, char* out, size_t outLen);
void nc_track_insert_class_id(NCEngine* engine, int index, int slot, char* out, size_t outLen);
void nc_track_insert_class_name(NCEngine* engine, int index, int slot, char* out, size_t outLen);

/// Stored plug-in parameters. The editor is authoritative while it is open; these
/// are what gets restored when it reopens, and what the project file keeps.
int nc_track_insert_param_count(NCEngine* engine, int index, int slot);
uint32_t nc_track_insert_param_id(NCEngine* engine, int index, int slot, int paramIndex);
double nc_track_insert_param_value(NCEngine* engine, int index, int slot, int paramIndex);
void nc_track_insert_param_name(NCEngine* engine, int index, int slot, int paramIndex,
                                char* out, size_t outLen);

/// True when this insert runs in the sandboxed realtime bridge rather than on the
/// audio thread. Those editors must be pointed at the bridge's shared memory, or
/// the plug-in's own meters sit dead while audio plays through it.
/// Fills the observer shm name, block size and sample rate for the editor host.
bool nc_track_insert_observer(NCEngine* engine, int index, int slot,
                              char* shmName, size_t shmNameLen,
                              int* maxBlock, double* sampleRate);

/// Upserts a parameter into the project and pushes it into the running graph.
/// Continuous: the editor sends a stream of these while a knob is turned.
bool nc_track_set_vst3_parameter(NCEngine* engine, int index, int slot,
                                 uint32_t parameterId, const char* displayName,
                                 double normalizedValue);

/// The instrument slot, addressed the way the inserts are. A track has at most one,
/// and it is what turns its MIDI notes into sound — so its editor matters as much as
/// any insert's.
void nc_track_instrument_plugin_path(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_instrument_plugin_format(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_instrument_class_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_instrument_class_name(NCEngine* engine, int index, char* out, size_t outLen);

int nc_track_instrument_param_count(NCEngine* engine, int index);
uint32_t nc_track_instrument_param_id(NCEngine* engine, int index, int paramIndex);
double nc_track_instrument_param_value(NCEngine* engine, int index, int paramIndex);

/// Unlike an insert, the renderer reads an instrument's parameters straight out of
/// the plan, so this reconciles the project rather than pushing into a live chain.
bool nc_track_set_instrument_vst3_parameter(NCEngine* engine, int index, uint32_t parameterId,
                                            const char* displayName, double normalizedValue);

// Per-rack-slot instrument access (slot 0 = primary). Lets a layer's editor open and be
// addressed on its own. The slot-0 forms above route through slot 0 of these.
void nc_track_instrument_slot_plugin_path(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen);
void nc_track_instrument_slot_plugin_format(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen);
void nc_track_instrument_slot_class_id(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen);
void nc_track_instrument_slot_class_name(NCEngine* engine, int index, int slotIndex, char* out, size_t outLen);
int nc_track_instrument_slot_param_count(NCEngine* engine, int index, int slotIndex);
uint32_t nc_track_instrument_slot_param_id(NCEngine* engine, int index, int slotIndex, int paramIndex);
double nc_track_instrument_slot_param_value(NCEngine* engine, int index, int slotIndex, int paramIndex);
bool nc_track_set_instrument_slot_vst3_parameter(NCEngine* engine, int index, int slotIndex,
                                                 uint32_t parameterId, const char* displayName,
                                                 double normalizedValue);

// ---------------------------------------------------------------------------
// Master inserts
//
// A separate chain from any track's, applied to the mix on its way out. The engine
// keeps it in project.masterInserts, not on the Master track.
// ---------------------------------------------------------------------------

int nc_master_insert_count(NCEngine* engine);
void nc_master_insert_name(NCEngine* engine, int slot, char* out, size_t outLen);
bool nc_master_insert_bypassed(NCEngine* engine, int slot);
void nc_master_insert_plugin_path(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_master_insert_plugin_format(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_master_insert_class_id(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_master_insert_class_name(NCEngine* engine, int slot, char* out, size_t outLen);

bool nc_master_add_insert(NCEngine* engine, int pluginIndex);
bool nc_master_remove_insert(NCEngine* engine, int slot);
bool nc_master_set_insert_bypassed(NCEngine* engine, int slot, bool bypassed);
/// -1 moves the insert earlier in the chain, +1 later. Returns the new slot, or -1.
int nc_master_move_insert(NCEngine* engine, int slot, int direction);

int nc_master_insert_param_count(NCEngine* engine, int slot);
uint32_t nc_master_insert_param_id(NCEngine* engine, int slot, int paramIndex);
double nc_master_insert_param_value(NCEngine* engine, int slot, int paramIndex);
bool nc_master_set_vst3_parameter(NCEngine* engine, int slot, uint32_t parameterId,
                                  const char* displayName, double normalizedValue);

int nc_track_send_count(NCEngine* engine, int index);
void nc_track_send_bus(NCEngine* engine, int index, int slot, char* out, size_t outLen);
float nc_track_send_gain_db(NCEngine* engine, int index, int slot);
// Sends route a copy of the track to an aux/bus. nc_track_add_aux makes a destination
// bus. Send options are the aux/bus tracks (count then name, cached between the calls).
// add / set-gain / remove all reconcile the mixer graph.
int  nc_track_add_aux(NCEngine* engine);
int  nc_track_send_option_count(NCEngine* engine, int index);
void nc_track_send_option(NCEngine* engine, int index, int i, char* out, size_t outLen);
bool nc_track_add_send(NCEngine* engine, int index, const char* busName);
void nc_track_set_send_gain_db(NCEngine* engine, int index, int slot, float db);
float nc_track_send_pan(NCEngine* engine, int index, int slot);
void nc_track_set_send_pan(NCEngine* engine, int index, int slot, float pan);
bool nc_track_send_pre_fader(NCEngine* engine, int index, int slot);
void nc_track_set_send_pre_fader(NCEngine* engine, int index, int slot, bool pre);
void nc_track_remove_send(NCEngine* engine, int index, int slot);

// ---------------------------------------------------------------------------
// History (undo / redo / dirty / autosave)
//
// Every mutating call in this facade records a step automatically, EXCEPT the
// continuous ones — track volume and pan. A fader drag would otherwise push a
// hundred steps. Those are recorded by nc_history_record_gesture once the gesture
// ends, and the caller must call it.
// ---------------------------------------------------------------------------

/// Records the current document under `stepName` if it differs from the last
/// snapshot. Returns true when a step was actually pushed. Also triggers autosave.
bool nc_history_record_gesture(NCEngine* engine, const char* stepName);

bool nc_history_can_undo(NCEngine* engine);
bool nc_history_can_redo(NCEngine* engine);
int nc_history_undo_depth(NCEngine* engine);
void nc_history_undo_step_name(NCEngine* engine, char* out, size_t outLen);
void nc_history_redo_step_name(NCEngine* engine, char* out, size_t outLen);

/// Restores the document and reconciles it into the running engine.
bool nc_history_undo(NCEngine* engine);
bool nc_history_redo(NCEngine* engine);

/// Forget all history and treat the current document as the saved state. Use on
/// new-project and after opening a file.
void nc_history_reset(NCEngine* engine);

/// The document was written to disk: dirty goes false, history is kept.
void nc_history_mark_saved(NCEngine* engine);

bool nc_project_dirty(NCEngine* engine);

/// Where autosave writes. Empty (the default) disables autosave entirely; the
/// document still tracks dirty state. Mirrors the legacy behaviour of only
/// autosaving projects that already have a path on disk.
void nc_project_set_path(NCEngine* engine, const char* path);
void nc_project_path(NCEngine* engine, char* out, size_t outLen);

/// The last autosave error, or empty. Cleared on the next successful write.
void nc_project_autosave_error(NCEngine* engine, char* out, size_t outLen);

// ---------------------------------------------------------------------------
// Project file I/O and audio import
// ---------------------------------------------------------------------------

/// Discards the document and starts from defaultProject(). History is reset.
void nc_project_new(NCEngine* engine);

/// True when an autosave sitting beside `path` is newer than the project itself —
/// the app crashed or quit with unsaved work. Ask the user before recovering.
bool nc_project_autosave_is_newer(const char* path);

/// Opens `path`. When `preferAutosave` is true and one exists, the autosave is
/// loaded instead; otherwise a stale autosave beside the project is removed.
/// Sets the project path, reconciles the engine, and resets history.
bool nc_project_open(NCEngine* engine, const char* path, bool preferAutosave,
                     char* error, size_t errorLen);

/// Writes to the current project path, keeping a backup of what was there.
/// Fails when the document has no path yet — call nc_project_save_as.
bool nc_project_save(NCEngine* engine, char* error, size_t errorLen);
bool nc_project_save_as(NCEngine* engine, const char* path, char* error, size_t errorLen);

/// Copies every clip source that lives outside the project's `Audio Files` folder into
/// it (temp imports made before the first save, files dragged in from elsewhere) and
/// rewrites the clip paths, so a saved project folder is self-contained. Returns the
/// number of files copied, or -1 on error. Requires the project to have a path.
int nc_project_consolidate_media(NCEngine* engine, char* error, size_t errorLen);

/// Save a fully self-contained COPY to `path` (a project folder), collecting all external media
/// (audio + video) into its Audio Files, WITHOUT changing the working session (its path, bindings,
/// or dirty state). Returns the number of media files gathered, or -1 on error.
int nc_project_save_copy(NCEngine* engine, const char* path, char* error, size_t errorLen);

/// wav, wave, mp3, aif, aiff, m4a, caf.
bool nc_audio_import_supported(const char* path);

/// Imports `path` onto the track at `trackIndex`, starting at `startSeconds`.
/// Non-WAV sources are converted; the result lands in the project's Audio Files
/// folder, or a temporary folder when the project has no path yet.
/// This convenience always analyses and applies to the timeline (the historical behaviour).
bool nc_audio_import(NCEngine* engine, int trackIndex, const char* path, double startSeconds,
                     char* error, size_t errorLen);

/// Import with explicit control over the musical analysis: `analyze` runs tempo / key /
/// chord / marker detection, `applyToTimeline` commits it to the project (vs. detect-only).
/// On success `error` carries the analysis summary string for the UI to surface.
bool nc_audio_import_analyzed(NCEngine* engine, int trackIndex, const char* path, double startSeconds,
                             bool analyze, bool applyToTimeline, char* error, size_t errorLen);

/// Clips currently in the document, and their placement. Enough for a timeline to
/// draw against; the waveform comes from the source file.
int nc_clip_count(NCEngine* engine);
void nc_clip_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_clip_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_clip_track(NCEngine* engine, int index, char* out, size_t outLen);
void nc_clip_source_path(NCEngine* engine, int index, char* out, size_t outLen);
double nc_clip_start_seconds(NCEngine* engine, int index);
double nc_clip_duration_seconds(NCEngine* engine, int index);
/// Where inside its source file the clip's audio begins. Trimming moves this.
double nc_clip_source_offset_seconds(NCEngine* engine, int index);
void nc_clip_color(NCEngine* engine, int index, char* out, size_t outLen);

// ---------------------------------------------------------------------------
// Clip editing
//
// Move and trim are continuous: they push the model and the graph but record no
// history. The caller records one step with nc_history_record_gesture when the
// drag ends. Split and delete are discrete and record themselves.
// ---------------------------------------------------------------------------

/// Snaps `seconds` to the project's grid. It always snaps — there is no "snap
/// enabled" flag inside. Decide whether to call it.
double nc_project_snap_time(NCEngine* engine, double seconds);
void nc_project_set_edit_mode(NCEngine* engine, const char* mode);
void nc_project_set_grid_unit(NCEngine* engine, const char* unit);
void nc_project_grid_unit(NCEngine* engine, char* out, size_t outLen);
double nc_project_grid_quantum_seconds(NCEngine* engine);
void nc_project_pan_law(NCEngine* engine, char* out, size_t outLen);
void nc_project_set_pan_law(NCEngine* engine, const char* law);

bool nc_clip_move(NCEngine* engine, const char* clipId, double newStartSeconds);
// Lightweight live-drag move (in-place render slide, no reconcile) + a one-shot commit reconcile.
bool nc_clip_update_start(NCEngine* engine, const char* clipId, double startSeconds);
int nc_clip_update_start_many(NCEngine* engine, const char* const* clipIds, int count, double deltaSeconds);
void nc_project_reconcile(NCEngine* engine);
bool nc_clip_trim_start(NCEngine* engine, const char* clipId, double newStartSeconds);
bool nc_clip_trim_end(NCEngine* engine, const char* clipId, double newEndSeconds);
// Lightweight live-drag trim: patches the clip's bounds in the render plan in place (no rebuild), so
// stretching a clip during playback never stops the sound. Commit once on drop via nc_project_reconcile.
bool nc_clip_update_trim_start(NCEngine* engine, const char* clipId, double newStartSeconds);
bool nc_clip_update_trim_end(NCEngine* engine, const char* clipId, double newEndSeconds);
// Roll edit: slide the shared boundary of two abutting clips together (one clamped boundary). Live.
bool nc_clip_roll_boundary(NCEngine* engine, const char* leftId, const char* rightId, double boundarySeconds);

/// Splits at `seconds`; the right-hand piece gets a new id. Records a step.
bool nc_clip_split(NCEngine* engine, const char* clipId, double seconds);
// Heal (re-join) adjacent same-source clips within a time range; returns how many
// resulting clips were glued. Pass the span of the selected clips or the edit range.
int  nc_clip_glue_range(NCEngine* engine, double startSeconds, double endSeconds);
// Heal only the given clips to each other (abutting same-source), never their unselected neighbours.
int  nc_clip_glue_selection(NCEngine* engine, const char* const* clipIds, int count);
bool nc_clip_delete(NCEngine* engine, const char* clipId);

/// -60…+12 dB. Continuous, like move and trim.
float nc_clip_gain_db(NCEngine* engine, int index);
/// Non-destructive processing state (by clip index), so the timeline can reflect it in the waveform.
bool nc_clip_muted(NCEngine* engine, int index);
bool nc_clip_reversed(NCEngine* engine, int index);
bool nc_clip_polarity(NCEngine* engine, int index);
bool nc_clip_set_gain_db(NCEngine* engine, const char* clipId, float gainDb);
/// Continuous preview: sets the gain field only, no graph rebuild (smooth drag).
/// Commit with nc_clip_set_gain_db to reconcile + record one step.
bool nc_clip_set_gain_db_preview(NCEngine* engine, const char* clipId, float gainDb);

/// Non-destructive clip processing the renderer honours directly (no new file); each records one
/// undo step. Reverse plays the source window back-to-front; polarity flips the sign; mute silences
/// the clip; normalize bakes a clip gain that brings the source peak to just under 0 dBFS.
bool nc_clip_toggle_reversed(NCEngine* engine, const char* clipId);
bool nc_clip_toggle_muted(NCEngine* engine, const char* clipId);
bool nc_clip_toggle_polarity(NCEngine* engine, const char* clipId);
bool nc_clip_normalize(NCEngine* engine, const char* clipId);

/// Offline time-stretch + pitch-shift PRINT (Serato phase vocoder). Renders the clip's played window
/// to a new WAV in the project's Audio Files folder and repoints the clip at it — timeRatio (0.125..8)
/// changes length independently of semitones (±24) which change pitch. Returns false with `error` set.
/// formantPreserve != 0 keeps the timbre through a pitch shift (WORLD-style source/filter separation),
/// so a shifted vocal/instrument does not chipmunk; 0 = raw shift. Ignored when semitones == 0.
bool nc_clip_apply_time_pitch(NCEngine* engine, const char* clipId,
                              double timeRatio, double semitones, int formantPreserve,
                              char* error, size_t errorLen);

/// Piecewise time remap PRINT (Serato anchor time map). sourceAnchors/destAnchors are matched
/// normalized [0,1] positions; each segment stretches independently to its dest span, at the global
/// pitch. anchorCount 0 behaves like nc_clip_apply_time_pitch. formantPreserve as above.
bool nc_clip_apply_time_map(NCEngine* engine, const char* clipId, double timeRatio, double semitones,
                            const double* sourceAnchors, const double* destAnchors, int anchorCount,
                            int formantPreserve, char* error, size_t errorLen);

/// Vocal alignment PRINT (VocAlign-style): time-warp `dubClipId` onto `refClipId`'s timing via MFCC-DTW,
/// then print+repoint (offline). strength 0..1 blends between no change (0) and full alignment (1).
/// formantPreserve keeps the dub's timbre through the warp. One undo step. Returns false with `error`.
bool nc_clip_align_to_reference(NCEngine* engine, const char* dubClipId, const char* refClipId,
                                double strength, int formantPreserve, char* error, size_t errorLen);

// Melodyne-mode pitch editing. detect runs YIN + note segmentation on the clip window and caches the
// notes on the engine (returns the count). The editor reads each note, sets a per-note semitone offset,
// then applies — rendering a new WAV and repointing the clip (length preserved). One undo step.
// mode: 0 = Melodic (monophonic pitch), 1 = Polyphonic (chords; falls back to Melodic for now),
// 2 = Percussive (onset/transient events, no pitch).
int nc_clip_detect_notes(NCEngine* engine, const char* clipId, int mode);
int nc_clip_note_count(NCEngine* engine);
double nc_clip_note_start_seconds(NCEngine* engine, int index);
double nc_clip_note_duration_seconds(NCEngine* engine, int index);
double nc_clip_note_detected_midi(NCEngine* engine, int index);
double nc_clip_note_offset_semitones(NCEngine* engine, int index);
double nc_clip_note_time_offset_seconds(NCEngine* engine, int index);
double nc_clip_note_duration_scale(NCEngine* engine, int index);
double nc_clip_note_confidence(NCEngine* engine, int index);
void nc_clip_note_set_offset(NCEngine* engine, int index, double semitones);
void nc_clip_note_set_time_offset(NCEngine* engine, int index, double seconds);
void nc_clip_note_set_duration_scale(NCEngine* engine, int index, double scale);
/// The rest of the Melodyne palette, per note. Amplitude/mute are exact; the formant tool moves the
/// spectral envelope without moving the pitch; attack speed time-warps the note's own attack envelope
/// (1.0 is exactly transparent). All are applied by the same offline print as the pitch edits.
double nc_clip_note_gain_db(NCEngine* engine, int index);
bool nc_clip_note_muted(NCEngine* engine, int index);
double nc_clip_note_formant_semitones(NCEngine* engine, int index);
double nc_clip_note_attack_speed(NCEngine* engine, int index);
void nc_clip_note_set_gain_db(NCEngine* engine, int index, double gainDb);
void nc_clip_note_set_muted(NCEngine* engine, int index, bool muted);
void nc_clip_note_set_formant_semitones(NCEngine* engine, int index, double semitones);
void nc_clip_note_set_attack_speed(NCEngine* engine, int index, double speed);
/// Pitch-modulation and pitch-drift tools: scale the note's own vibrato / slow pitch movement.
/// 1 = as recorded, 0 = flat, 2 = twice as much.
double nc_clip_note_modulation_scale(NCEngine* engine, int index);
double nc_clip_note_drift_scale(NCEngine* engine, int index);
void nc_clip_note_set_modulation_scale(NCEngine* engine, int index, double scale);
void nc_clip_note_set_drift_scale(NCEngine* engine, int index, double scale);
/// Puts one note back to untouched.
void nc_clip_note_reset(NCEngine* engine, int index);
bool nc_clip_note_split(NCEngine* engine, int index, double localSeconds);
bool nc_clip_apply_note_edits(NCEngine* engine, const char* clipId, char* error, size_t errorLen);

// Polyphonic detection helpers: write the clip window as-is (to feed the separator), then reset the
// note cache and append the notes found in each stem file (they accumulate, sorted by time).
bool nc_clip_export_raw_window(NCEngine* engine, const char* clipId, const char* outPath, char* error, size_t errorLen);

// Repoint a clip at an externally-produced WAV that spans exactly its played window (same length): the
// file is copied into the project's Audio Files folder, the clip's source is swapped to it with offset 0
// and its duration/fades unchanged, and one undo step is recorded under `label`. Used by the neural
// denoiser (offline print): export the window → denoise it into a WAV → repoint here.
bool nc_clip_repoint_to_window_wav(NCEngine* engine, const char* clipId, const char* wavPath,
                                   const char* label, char* error, size_t errorLen);
// --- ARA (Melodyne 등) -----------------------------------------------------------------------
// An ARA plug-in edits a clip's audio in place with full random access, so it is hosted OFFLINE and
// IN-PROCESS: open a session on a clip, show the plug-in's own editor, then commit — which archives
// the plug-in's edits onto the clip (so they stay re-editable) and prints the result to a new WAV the
// clip points at. Nothing ARA ever touches the audio thread. One session at a time.
//
// The clip keeps the unedited window it was first opened against, so re-opening always edits the
// original audio rather than stacking a second pass on the print.
/// The installed ARA-capable plug-ins, independent of the plug-in browser's current filter.
int nc_ara_plugin_count(NCEngine* engine);
void nc_ara_plugin_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_ara_plugin_path(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_ara_open(NCEngine* engine, const char* clipId, const char* pluginName, const char* pluginPath,
                 char* error, size_t errorLen);
bool nc_ara_is_open(NCEngine* engine);
void nc_ara_open_clip_id(NCEngine* engine, char* out, size_t outLen);
/// Creates the plug-in's editor inside `nsView` and reports the size it wants. Main thread.
bool nc_ara_attach_editor(NCEngine* engine, void* nsView, int* widthOut, int* heightOut,
                          char* error, size_t errorLen);
/// Takes the plug-in's editor down without ending the session. Must be called before the NSView it
/// was attached to goes away — the plug-in is still drawing into it.
void nc_ara_detach_editor(NCEngine* engine);
/// Archives the edits onto the clip and prints them into its audio. One undo step. Blocking.
bool nc_ara_commit(NCEngine* engine, char* error, size_t errorLen);
/// Drops the session without committing anything.
void nc_ara_close(NCEngine* engine);
bool nc_clip_has_ara_edits(NCEngine* engine, const char* clipId);
/// Points the clip back at its unedited window and forgets the archive. One undo step.
bool nc_clip_clear_ara_edits(NCEngine* engine, const char* clipId, char* error, size_t errorLen);

void nc_detect_notes_reset(NCEngine* engine);
int nc_detect_notes_add_from_file(NCEngine* engine, const char* wavPath, int mode);
/// Append one externally detected polyphonic note to the editor cache.
void nc_detect_notes_add_note(NCEngine* engine, double startSeconds, double durationSeconds,
                              double midiPitch, double confidence);
void nc_detect_notes_bind_clip(NCEngine* engine, const char* clipId);
// Segment an external pitch track (from the CREPE neural detector helper) into cached notes.
int nc_segment_pitch_track(NCEngine* engine, const double* times, const double* hzs,
                           const double* confs, int count);

// Export the processed result to a standalone WAV at `outPath` — the clip/project is NOT changed.
// note_edits uses the cached Melodyne edits; time_map uses the Serato anchor remap (+ ratio/pitch).
bool nc_clip_export_note_edits(NCEngine* engine, const char* clipId, const char* outPath,
                               char* error, size_t errorLen);
bool nc_clip_export_time_map(NCEngine* engine, const char* clipId, double timeRatio, double semitones,
                             const double* sourceAnchors, const double* destAnchors, int anchorCount,
                             const char* outPath, char* error, size_t errorLen);

/// Fades, in seconds from each end. Continuous, like move and trim.
double nc_clip_fade_in(NCEngine* engine, int index);
double nc_clip_fade_out(NCEngine* engine, int index);
bool nc_clip_set_fades(NCEngine* engine, const char* clipId, double fadeIn, double fadeOut);
bool nc_clip_set_fade_curves(NCEngine* engine, const char* clipId,
                             const char* inCurve, const char* outCurve);
/// Turn a same-track overlap around this clip into a crossfade (no history step).
bool nc_clip_apply_crossfades(NCEngine* engine, const char* clipId);
// Consolidate: render each track's selected clips (gain/fades/crossfades baked) into one new WAV
// and replace them with a single clip. One undo step. Returns false with a message on failure.
bool nc_clip_consolidate(NCEngine* engine, const char* const* clipIds, int count,
                         char* outError, size_t errLen);
void nc_clip_fade_in_curve(NCEngine* engine, int index, char* out, size_t outLen);
void nc_clip_fade_out_curve(NCEngine* engine, int index, char* out, size_t outLen);
/// Continuous fade shape bend, [-1, 1], 0 = the named curve unchanged. The fade editor's middle handle.
double nc_clip_fade_in_curvature(NCEngine* engine, int index);
double nc_clip_fade_out_curvature(NCEngine* engine, int index);
bool nc_clip_set_fade_curvature(NCEngine* engine, const char* clipId,
                                double inCurvature, double outCurvature);

/// Moves a clip onto another track at `startSeconds`, leaving every other clip
/// where it is. (`shuffleMoveClip` would ripple its neighbours — that is Shuffle
/// edit mode, not a plain drag.) Discrete: it records its own step, and the clip
/// gets a new id, returned in `out`.
bool nc_clip_move_to_track(NCEngine* engine, const char* clipId, int trackIndex,
                           double startSeconds, char* out, size_t outLen);

/// Clipboard. Copy stores the clip; paste places a copy at `startSeconds` on the
/// clip's original track. Cut copies then deletes. All record their own step.
bool nc_clip_copy(NCEngine* engine, const char* clipId);
bool nc_clip_cut(NCEngine* engine, const char* clipId);
bool nc_clipboard_has_clip(NCEngine* engine);
void nc_clipboard_clip_name(NCEngine* engine, char* out, size_t outLen);
/// Returns the new clip's id in `out`, or empty on failure.
bool nc_clip_paste(NCEngine* engine, double startSeconds, char* out, size_t outLen);
bool nc_clip_duplicate(NCEngine* engine, const char* clipId, char* out, size_t outLen);

/// Batch edits over a timeline selection. Each records exactly one undo step for
/// the whole selection — a five-clip delete is one ⌘Z, not five.
///
/// Ids that name no clip are skipped; the return value says how many clips the
/// edit actually touched. Clips created by an edit are read back with
/// `nc_result_count` / `nc_result_id`, which the next batch call overwrites.
int nc_result_count(NCEngine* engine);
void nc_result_id(NCEngine* engine, int index, char* out, size_t outLen);

/// Continuous, like `nc_clip_move`: records nothing. If the selection would cross
/// zero the whole thing is held back so their spacing survives the drag.
int nc_clip_move_many(NCEngine* engine, const char* const* clipIds, int count, double deltaSeconds);

/// Where the clip FIRST landed on the timeline (import), the Pro-Tools "original
/// time stamp". Moves/trims never change it; a split offsets the right half.
/// Returns -1 when unknown (clips from projects saved before the field existed).
double nc_clip_original_start_seconds(NCEngine* engine, const char* clipId);
/// Spot: move each clip back to ITS OWN original position (no common delta), so a
/// scattered selection re-forms the imported layout. Clips without a stored
/// original stay put. Returns how many moved; records ONE undo step.
int nc_clip_spot_to_original_many(NCEngine* engine, const char* const* clipIds, int count);

int nc_clip_delete_many(NCEngine* engine, const char* const* clipIds, int count);
int nc_clip_split_many(NCEngine* engine, const char* const* clipIds, int count, double seconds);
/// Places the copies one selection-width to the right, so they do not overlap.
int nc_clip_duplicate_many(NCEngine* engine, const char* const* clipIds, int count);

/// The clipboard holds a whole selection. Paste lands the earliest clip at
/// `startSeconds` and keeps the others' offsets from it.
bool nc_clip_copy_many(NCEngine* engine, const char* const* clipIds, int count);
int nc_clip_cut_many(NCEngine* engine, const char* const* clipIds, int count);
int nc_clipboard_clip_count(NCEngine* engine);
int nc_clip_paste_all(NCEngine* engine, double startSeconds);

// ---------------------------------------------------------------------------
// MIDI regions and notes
//
// A region sits on an instrument or midi track and holds notes. Region times are
// **seconds** on the timeline; note times are **beats from the region's start**.
// A note only makes a sound if its track carries an instrument plug-in — the
// renderer turns notes into VST3 events and mixes whatever the instrument returns.
// A region on a bare midi track is silent by design.
//
// The renderer copies `midiRegions` straight into its plan, so unlike audio clips
// there is no playlist to rebuild.
// ---------------------------------------------------------------------------

int nc_midi_region_count(NCEngine* engine);
void nc_midi_region_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_midi_region_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_midi_region_track(NCEngine* engine, int index, char* out, size_t outLen);
double nc_midi_region_start_seconds(NCEngine* engine, int index);
double nc_midi_region_duration_seconds(NCEngine* engine, int index);
bool nc_midi_region_muted(NCEngine* engine, int index);

bool nc_midi_region_add(NCEngine* engine, int trackIndex, double startSeconds,
                        double durationSeconds, char* out, size_t outLen);
/// Import a Standard MIDI File onto an existing instrument/midi track as ONE region at startSeconds,
/// flattening all its notes (relative beat timing preserved; plays at the host tempo). One undo step.
/// Backs the drum MIDI library's drag-drop + insert. Returns the new region id via out.
bool nc_midi_import_file_to_track(NCEngine* engine, const char* midiPath, int trackIndex,
                                  double startSeconds, char* out, size_t outLen);
/// Smart MIDI import: a single-track file (a loop) lands as one region on `preferredTrackIndex` (or a new
/// instrument track if < 0); a MULTI-track song lands as one new instrument track PER source track, each
/// named from the source track, so a full song splits into its parts. One undo step. Returns tracks made.
int nc_midi_import_file_auto(NCEngine* engine, const char* midiPath, int preferredTrackIndex,
                             double startSeconds, char* error, size_t errorLen);
/// Continuous, like a clip drag: records nothing. Pass trackIndex < 0 to stay put.
bool nc_midi_region_move(NCEngine* engine, const char* regionId, int trackIndex, double startSeconds);
bool nc_midi_region_resize(NCEngine* engine, const char* regionId, double durationSeconds);
bool nc_midi_region_delete(NCEngine* engine, const char* regionId);

/// Region tools. Each records one step for the whole region. `beatQuantum` is in
/// beats: 0.25 is a sixteenth. Humanize takes a seed so the same call twice gives the
/// same result — a random result could not be tested.
int nc_midi_region_quantize(NCEngine* engine, const char* regionId, double beatQuantum);
int nc_midi_region_transpose(NCEngine* engine, const char* regionId, int semitones);
int nc_midi_region_humanize(NCEngine* engine, const char* regionId, double maxTimingBeats,
                            int maxVelocityDelta, unsigned int seed);
bool nc_midi_region_split(NCEngine* engine, const char* regionId, double splitSeconds,
                          char* out, size_t outLen);
bool nc_midi_region_duplicate(NCEngine* engine, const char* regionId, char* out, size_t outLen);
// Cubase Glue: merge two or more MIDI regions on the same track into one part (notes + CC + pitch
// bend + program changes rebased onto the merged timeline). Writes the new region id to `out`.
// Returns false for fewer than two mergeable regions or regions spanning different tracks.
bool nc_midi_regions_merge(NCEngine* engine, const char* const* regionIds, int count,
                           char* out, size_t outLen);

int nc_midi_note_count(NCEngine* engine, const char* regionId);
void nc_midi_note_id(NCEngine* engine, const char* regionId, int noteIndex, char* out, size_t outLen);
int nc_midi_note_pitch(NCEngine* engine, const char* regionId, int noteIndex);
double nc_midi_note_start_beats(NCEngine* engine, const char* regionId, int noteIndex);
double nc_midi_note_duration_beats(NCEngine* engine, const char* regionId, int noteIndex);
int nc_midi_note_velocity(NCEngine* engine, const char* regionId, int noteIndex);

bool nc_midi_note_add(NCEngine* engine, const char* regionId, int pitch, double startBeats,
                      double durationBeats, int velocity, char* out, size_t outLen);
/// Continuous: dragging a note across the grid records nothing.
bool nc_midi_note_move(NCEngine* engine, const char* regionId, const char* noteId,
                       int pitch, double startBeats);
bool nc_midi_note_resize(NCEngine* engine, const char* regionId, const char* noteId, double durationBeats);
/// Continuous: records nothing. Commit with nc_history_record_gesture.
bool nc_midi_note_set_velocity(NCEngine* engine, const char* regionId, const char* noteId, int velocity);
bool nc_midi_note_delete(NCEngine* engine, const char* regionId, const char* noteId);
// Cubase Key Editor functions. Pass no note ids (nullptr/0) to act on the whole region.
/// Legato: stretch each note to meet the next that starts later, less gapBeats. One undo step.
bool nc_midi_notes_legato(NCEngine* engine, const char* regionId,
                          const char* const* noteIds, int count, double gapBeats);
/// Delete Overlaps: shorten a note that runs past the next note of the SAME pitch.
bool nc_midi_notes_delete_overlaps(NCEngine* engine, const char* regionId,
                                   const char* const* noteIds, int count);
/// Fixed Lengths: set every target note to one length in beats.
bool nc_midi_notes_set_length(NCEngine* engine, const char* regionId,
                              const char* const* noteIds, int count, double lengthBeats);

/// Cubase Glue for notes: joins the given notes of each pitch into one long note (gaps absorbed).
/// One undo step. False when fewer than two notes of any single pitch were given.
bool nc_midi_notes_merge(NCEngine* engine, const char* regionId,
                         const char* const* noteIds, int count);

// Controller (CC) lanes. Counts/reads are filtered to one controller number (0-127); values
// are 0-127. The engine already renders these to the instrument, so an edited curve is heard.
int nc_midi_cc_count(NCEngine* engine, const char* regionId, int controller);
bool nc_midi_cc_get(NCEngine* engine, const char* regionId, int controller, int index,
                    char* outId, size_t idLen, double* outBeat, int* outValue);
bool nc_midi_cc_add(NCEngine* engine, const char* regionId, int controller, double beat, int value,
                    char* outId, size_t idLen);
/// Continuous: dragging a CC point records nothing. Commit with nc_history_record_gesture.
bool nc_midi_cc_move(NCEngine* engine, const char* regionId, const char* eventId, double beat, int value);
bool nc_midi_cc_delete(NCEngine* engine, const char* regionId, const char* eventId);

// Pitch-bend lane. Values are 0-16383, centre 8192.
int nc_midi_pb_count(NCEngine* engine, const char* regionId);
bool nc_midi_pb_get(NCEngine* engine, const char* regionId, int index,
                    char* outId, size_t idLen, double* outBeat, int* outValue);
bool nc_midi_pb_add(NCEngine* engine, const char* regionId, double beat, int value,
                    char* outId, size_t idLen);
/// Continuous: records nothing. Commit with nc_history_record_gesture.
bool nc_midi_pb_move(NCEngine* engine, const char* regionId, const char* eventId, double beat, int value);
bool nc_midi_pb_delete(NCEngine* engine, const char* regionId, const char* eventId);

// ---------------------------------------------------------------------------
// Markers
//
// Navigation only: nothing in the audio path reads them. They stay sorted by time,
// so an index is only good until the next edit. The engine addresses a marker by
// the time you clicked, within a tolerance — which is what a UI has anyway.
// ---------------------------------------------------------------------------

int nc_marker_count(NCEngine* engine);
double nc_marker_time(NCEngine* engine, int index);
void nc_marker_name(NCEngine* engine, int index, char* out, size_t outLen);

bool nc_marker_add(NCEngine* engine, double timeSeconds, char* out, size_t outLen);
bool nc_marker_rename(NCEngine* engine, double timeSeconds, double toleranceSeconds, const char* name);
/// Continuous, for dragging a marker: records nothing. Pass the marker's *current*
/// time each frame, not the time the drag started from.
bool nc_marker_move(NCEngine* engine, double fromSeconds, double toleranceSeconds, double toSeconds);
bool nc_marker_delete(NCEngine* engine, double timeSeconds, double toleranceSeconds);

/// The stretch between the markers on either side of `seconds`, for setting the
/// edit range from a marker.
bool nc_marker_surrounding_range(NCEngine* engine, double seconds, double* start, double* end);

// Conductor / global track: chords, lyrics, tempo markers.
int nc_chord_count(NCEngine* engine);
double nc_chord_time(NCEngine* engine, int index);
void nc_chord_name(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_chord_add(NCEngine* engine, double timeSeconds, const char* name);
bool nc_chord_rename(NCEngine* engine, double timeSeconds, double tol, const char* name);
bool nc_chord_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds);
bool nc_chord_delete(NCEngine* engine, double timeSeconds, double tol);

int nc_lyric_count(NCEngine* engine);
double nc_lyric_time(NCEngine* engine, int index);
void nc_lyric_text(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_lyric_add(NCEngine* engine, double timeSeconds, const char* text);
bool nc_lyric_rename(NCEngine* engine, double timeSeconds, double tol, const char* text);
bool nc_lyric_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds);
bool nc_lyric_delete(NCEngine* engine, double timeSeconds, double tol);

// Song-form / arrangement sections — per-project (stored in the project document, not app-global).
int nc_song_section_count(NCEngine* engine);
double nc_song_section_time(NCEngine* engine, int index);
void nc_song_section_name(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_song_section_add(NCEngine* engine, double timeSeconds, const char* name);
bool nc_song_section_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds);
bool nc_song_section_delete(NCEngine* engine, double timeSeconds, double tol);
// Delete all conductor events (marker/chord/lyric/song section/tempo/meter) in [start,end], one
// undo step. Returns the count removed. Tempo/meter keep their t=0 anchor.
int  nc_conductor_clear_range(NCEngine* engine, double start, double end);

int nc_tempo_marker_count(NCEngine* engine);
double nc_tempo_marker_time(NCEngine* engine, int index);
double nc_tempo_marker_bpm(NCEngine* engine, int index);
bool nc_tempo_marker_add(NCEngine* engine, double timeSeconds, double bpm);
bool nc_tempo_marker_delete(NCEngine* engine, double timeSeconds, double tol);
bool nc_tempo_marker_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds);

int nc_time_sig_count(NCEngine* engine);
double nc_time_sig_time(NCEngine* engine, int index);
int nc_time_sig_numerator(NCEngine* engine, int index);
int nc_time_sig_denominator(NCEngine* engine, int index);
bool nc_time_sig_add(NCEngine* engine, double timeSeconds, int numerator, int denominator);
bool nc_time_sig_move(NCEngine* engine, double fromSeconds, double tol, double toSeconds);
bool nc_time_sig_delete(NCEngine* engine, double timeSeconds, double tol);

// ---------------------------------------------------------------------------
// Automation
//
// The renderer honours exactly two parameters, and both the realtime mixer and the
// offline bounce read them through the same per-frame code. Anything else stored in
// a track's automation lanes is kept by the project and ignored by the sound, so the
// UI must not offer it.
//
//   "track.volume"  dB, -120…+24, the fallback is the track's fader
//   "track.pan"     -1…+1, the fallback is the track's pan knob
//
// Points are kept sorted by time; `pointIndex` addresses that order. Moving a point
// past its neighbours re-sorts them, so read the list back afterwards.
// ---------------------------------------------------------------------------

/// False for a parameter the sound would ignore.
bool nc_automation_parameter_supported(const char* parameterId);
/// Drive all plugin-insert automation lanes ("insert.<slot>.<paramId>") to the given time,
/// pushing values into the live graph. Call each tick while the transport runs.
void nc_apply_plugin_automation(NCEngine* engine, double timeSeconds);

int nc_track_automation_count(NCEngine* engine, int trackIndex, const char* parameterId);
double nc_track_automation_time(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex);
float nc_track_automation_value(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex);

// Automation modes (per track) + live write / evaluate for touch/latch/write and fader-follow.
void nc_track_automation_mode(NCEngine* engine, int trackIndex, char* out, size_t outLen);
void nc_track_set_automation_mode(NCEngine* engine, int trackIndex, const char* mode);
float nc_track_automation_value_at(NCEngine* engine, int trackIndex,
                                   const char* parameterId, double timeSeconds, float fallback);
bool nc_track_automation_write(NCEngine* engine, int trackIndex,
                               const char* parameterId, double timeSeconds, float value);
bool nc_track_automation_write_sweep(NCEngine* engine, int trackIndex, const char* parameterId,
                                     double fromExclusive, double toInclusive, float value);

/// Adds a point, or replaces the one already sitting at that time. Records a step.
bool nc_track_automation_add(NCEngine* engine, int trackIndex, const char* parameterId,
                             double timeSeconds, float value);
/// Continuous, for dragging a point. Records nothing; commit with nc_history_record_gesture.
bool nc_track_automation_move(NCEngine* engine, int trackIndex, const char* parameterId,
                              int pointIndex, double timeSeconds, float value);
bool nc_track_automation_delete(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex);
/// Clears the points inside the edit range. Returns how many went.
int nc_track_automation_clear_range(NCEngine* engine, int trackIndex, const char* parameterId,
                                    double startSeconds, double endSeconds);

// ---------------------------------------------------------------------------
// Range editing
//
// The loop range doubles as the edit range, the way the old UI used it. A range
// edit slices clips at its edges rather than treating them whole: cutting 2–3 s
// out of a ten-second clip leaves the rest of it playing.
// ---------------------------------------------------------------------------

bool nc_project_set_loop_range(NCEngine* engine, double startSeconds, double endSeconds);

/// Copies the slice of every clip the range overlaps onto the clipboard, keeping
/// each one's offset from the range start. Paste with `nc_clip_paste_all`.
int nc_range_copy(NCEngine* engine, double startSeconds, double endSeconds);
int nc_range_cut(NCEngine* engine, double startSeconds, double endSeconds);
/// Deletes what lies inside the range and leaves a hole; the clipboard is untouched.
bool nc_range_clear(NCEngine* engine, double startSeconds, double endSeconds);
/// Splits every clip at both range edges, so the range becomes clips of its own.
int nc_range_separate(NCEngine* engine, double startSeconds, double endSeconds);
int nc_range_duplicate(NCEngine* engine, double startSeconds, double endSeconds);

// ---------------------------------------------------------------------------
// Bounce (offline export)
//
// Renders the whole document through the same plan the realtime engine builds.
// It blocks: see the timing the smoke test prints before deciding whether the UI
// needs a progress sheet.
// ---------------------------------------------------------------------------

typedef struct {
    bool ok;
    double durationSeconds;
    float peakLeft;
    float peakRight;
    float rmsLeft;
    float rmsRight;
    bool clippingDetected;
    bool nearSilent;
    int missingMediaClipCount;
    char message[NC_TEXT_LEN];
} NCBounceResult;

/// Bounces the engine's current document. Blocks; roughly 25x realtime with no
/// plug-ins, so a three-minute song freezes the caller for several seconds.
bool nc_bounce_to_wav(NCEngine* engine, const char* path, NCBounceResult* out);

/// Serializes the document into `out`. Returns the number of bytes the document
/// needs (excluding the terminator), so a short buffer can be retried. The text is
/// self-contained: `nc_bounce_snapshot_to_wav` renders it without touching the
/// engine, which is what makes an off-thread bounce safe.
int nc_project_serialize(NCEngine* engine, char* out, size_t outLen);
/// Apply only the monitor-station configuration from a serialized project onto the current
/// one — the "전체 설정 저장" template a new session inherits. Returns false if it won't parse.
bool nc_apply_monitor_template(NCEngine* engine, const char* serialized);

/// Renders a serialized document. Touches no shared state — safe to call from a
/// background thread while the user keeps editing.
bool nc_bounce_snapshot_to_wav(const char* projectText, const char* path, NCBounceResult* out);
// Same render, but the DSP-role-assigned processing (console strips, master inserts) runs ON THE
// NODE, and STRICTLY: any block the node misses fails the bounce with an error instead of
// quietly finishing on the local processors. Fails up front when nothing is assigned remote.
bool nc_bounce_snapshot_to_wav_remote(const char* projectText, const char* path, NCBounceResult* out);

// AAF session import (libAAF). Reads audio tracks, clips and markers from a Pro Tools / Media
// Composer / Resolve AAF and REPLACES the open document with it — an AAF is a whole session.
// Media is referenced where the AAF points; anything missing is reported in the message so it can
// be relinked. Effects and automation are not imported: AAF's model does not map onto ours and
// guessing would misrepresent the session.
bool nc_import_aaf(NCEngine* engine, const char* path, char* msgOut, size_t msgLen);
/// False when this build has no AAF reader, so the UI can hide the command.
bool nc_aaf_import_available(void);

/// Renders one WAV per track into `folderPath` — the session-interchange path that every DAW
/// accepts. Each stem is the full session length starting at 00:00 (rendered with that track
/// soloed), so dropping them at zero in another DAW lines the session up exactly. Returns how many
/// stems were written; `errOut` explains a zero. Blocks, like the other bounces.
int nc_bounce_stems(NCEngine* engine, const char* folderPath, char* errOut, size_t errLen);

// ---------------------------------------------------------------------------
// Waveform peaks
//
// Decoding a WAV costs real time, so the engine caches one peak set per file at a
// fixed bucket count. The timeline resamples that cache when it zooms rather than
// re-reading the file.
// ---------------------------------------------------------------------------



/// each of `mins` and `maxs`, both in -1…1. Returns false when the file cannot be
/// read. Channels are summed to mono.
/// Peaks are stored at a fixed sample resolution, so their count scales with the
/// file's length rather than being clamped to a bucket total — a long clip no longer
/// smears when zoomed. Analyzes and caches the file on first call.
int nc_waveform_peak_count(NCEngine* engine, const char* path);
/// Copies up to `count` peaks (min/max per slice) into the caller's buffers.
bool nc_waveform_peaks(NCEngine* engine, const char* path, float* mins, float* maxs, int count);
/// The whole file's length. The buckets span the file, not the clip, so a trimmed
/// clip has to know where inside the file its own audio starts and ends.
/// Zero until `nc_waveform_peaks` has read the file.
double nc_waveform_duration_seconds(NCEngine* engine, const char* path);

/// Channel count for the drawing: 1 (mono → one envelope) or 2 (stereo → L/R envelopes).
int nc_waveform_channel_count(NCEngine* engine, const char* path);
/// Per-channel peaks (channel 0 = L, 1 = R). Falls back to the mono envelope when the
/// channel is absent, so it is always safe to call for either channel.
bool nc_waveform_channel_peaks(NCEngine* engine, const char* path, int channel,
                               float* mins, float* maxs, int count);

// ---------------------------------------------------------------------------
// Plugin browser
//
// Scanning ~1000 plug-ins costs about 90 ms, so it runs once and caches. The
// filtered view is recomputed by nc_plugin_apply_filter; every accessor below
// indexes into that filtered list.
// ---------------------------------------------------------------------------

/// Scans installed plug-ins into the engine's cache. Returns the total found.
int nc_plugin_scan(NCEngine* engine);
/// Force a fresh scan from disk (bypasses the cache) — for when a new plug-in was installed.
int nc_plugin_rescan(NCEngine* engine);
/// True if the installed .vst3 set changed since the last scan (browser auto-rescans on open).
bool nc_plugin_locations_changed(NCEngine* engine);

/// Empty strings mean "no constraint". `excludeCategory` drops candidates OF that
/// category — "Instrument" while browsing for an FX insert, where picking an
/// instrument would only be rejected. Returns the number of matches.
int nc_plugin_apply_filter(NCEngine* engine,
                           const char* text,
                           const char* brand,
                           const char* category,
                           const char* format,
                           const char* excludeCategory);

int nc_plugin_count(NCEngine* engine);
void nc_plugin_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_plugin_brand(NCEngine* engine, int index, char* out, size_t outLen);
void nc_plugin_category(NCEngine* engine, int index, char* out, size_t outLen);
void nc_plugin_format(NCEngine* engine, int index, char* out, size_t outLen);
/// True when the filtered plug-in at `index` is an ARA plug-in (its VST3 factory advertises an ARA
/// Main Factory). The browser badges these: they are not realtime effects and cannot be inserted.
bool nc_plugin_is_ara(NCEngine* engine, int index);
/// What the plug-in's own ARA factory reports (name, version, manufacturer, supported ARA
/// generations) — opens the plug-in, so call it for ONE plug-in on demand, never across a scan.
void nc_plugin_ara_info(NCEngine* engine, int index, char* out, size_t outLen);
void nc_plugin_path(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_plugin_exists(NCEngine* engine, int index);

/// Facet kinds for the browser's filter columns.
#define NC_FACET_BRAND 0
#define NC_FACET_CATEGORY 1
#define NC_FACET_FORMAT 2
#define NC_FACET_SCOPE 3

/// Facets describe the full scan, not the filtered view, so the columns stay stable.
int nc_plugin_facet_count(NCEngine* engine, int kind);
void nc_plugin_facet_name(NCEngine* engine, int kind, int index, char* out, size_t outLen);
/// How many of the scanned plug-ins carry this facet value.
int nc_plugin_facet_tally(NCEngine* engine, int kind, int index);

/// Adds the filtered plug-in at `pluginIndex` to the first free insert slot on the
/// track, appending a slot if needed. The slot's DSP execution mode comes from
/// InsertDspPolicy. Returns false when the track refuses inserts or is full.
bool nc_track_add_insert(NCEngine* engine, int trackIndex, int pluginIndex);
/// Why the last insert-add was refused, when it was refused for a reason worth showing (an ARA
/// plug-in kept out of the realtime chain). Empty when there is nothing to say.
void nc_last_plugin_message(NCEngine* engine, char* out, size_t outLen);
bool nc_track_remove_insert(NCEngine* engine, int trackIndex, int slot);

// Built-in high-accuracy test signal generator as a track SOURCE (band-limited sine/square/triangle/
// saw, white/pink noise, log/lin sweep). It voices a silent track, so add it to an empty track and
// enable it. Setters take real units (Hz, dBFS); waveform 0..6 = sine/square/triangle/saw/white/pink/
// sweep; channel 0=L, 1=Stereo, 2=R. Frequency/level setters record no undo step (continuous); the
// discrete ones do. One generator per track.
bool nc_track_add_test_signal_generator(NCEngine* engine, int trackIndex);
bool nc_track_remove_test_signal_generator(NCEngine* engine, int trackIndex);
int nc_track_test_signal_generator_slot(NCEngine* engine, int trackIndex);   // -1 if none
void nc_track_test_signal_set_enabled(NCEngine* engine, int trackIndex, bool enabled);
void nc_track_test_signal_set_waveform(NCEngine* engine, int trackIndex, int waveform);
void nc_track_test_signal_set_frequency_hz(NCEngine* engine, int trackIndex, double hz);
void nc_track_test_signal_set_level_db(NCEngine* engine, int trackIndex, double db);
void nc_track_test_signal_set_channel(NCEngine* engine, int trackIndex, int channel);
void nc_track_test_signal_set_polarity(NCEngine* engine, int trackIndex, bool inverted);
bool nc_track_test_signal_enabled(NCEngine* engine, int trackIndex);
int nc_track_test_signal_waveform(NCEngine* engine, int trackIndex);
double nc_track_test_signal_frequency_hz(NCEngine* engine, int trackIndex);
double nc_track_test_signal_level_db(NCEngine* engine, int trackIndex);
int nc_track_test_signal_channel(NCEngine* engine, int trackIndex);
bool nc_track_test_signal_polarity(NCEngine* engine, int trackIndex);
/// direction is -1 (earlier in the chain) or +1. Returns the new slot index, or -1.
int nc_track_move_insert(NCEngine* engine, int trackIndex, int slot, int direction);
int nc_track_move_insert_to_index(NCEngine* engine, int trackIndex, int fromSlot, int toSlot);
int nc_track_move_insert_to_slot(NCEngine* engine, int trackIndex, int fromSlot, int toSlot);
/// Copy / move an insert (with its parameters) to a slot on the same or a different track —
/// mixer drag-and-drop (Option-drag copies, plain drag moves). dstSlot < 0 appends.
bool nc_track_copy_insert(NCEngine* engine, int srcTrackIndex, int srcSlot, int dstTrackIndex, int dstSlot);
bool nc_track_move_insert_across(NCEngine* engine, int srcTrackIndex, int srcSlot, int dstTrackIndex, int dstSlot);

/// "NAT", "INT", "RINT" or "EXT" — what the insert will actually run on.
void nc_track_insert_mode_badge(NCEngine* engine, int trackIndex, int slot, char* out, size_t outLen);

/// Set a track/master insert's DSP execution mode from the channel UI. Only "native" (in-process,
/// audio thread) and "internal" (out-of-process on the isolated performance core) are accepted;
/// returns false for anything else or if unchanged. Rebuilds the chain through a declick.
bool nc_track_insert_set_dsp_mode(NCEngine* engine, int trackIndex, int slot, const char* mode);
bool nc_master_insert_set_dsp_mode(NCEngine* engine, int slot, const char* mode);

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

// ---------------------------------------------------------------------------
// DSP core allocation
//
// The engine reserves performance cores for its realtime DSP as a QoS hint. The
// count defaults to 4 and is clamped to 1..16; with isolation on the engine keeps a
// floor of 4. These are read when the audio engine starts, so changing them restarts
// it — a brief dropout, the way a buffer-size change would.
// ---------------------------------------------------------------------------

bool nc_delay_compensation_enabled(NCEngine* engine);
void nc_delay_compensation_set(NCEngine* engine, bool enabled);
double nc_delay_compensation_ms(NCEngine* engine);
int nc_delay_compensation_samples(NCEngine* engine);
int nc_track_delay_compensation_samples(NCEngine* engine, int index);

bool nc_dsp_core_isolation(NCEngine* engine);
void nc_dsp_set_core_isolation(NCEngine* engine, bool enabled);
int nc_dsp_core_count(NCEngine* engine);
void nc_dsp_set_core_count(NCEngine* engine, int count);

/// Audio I/O buffer size in frames (the project's requested size). Setting it restarts the
/// audio engine to apply — a brief dropout. The device may clamp it; the granted size is in
/// nc_engine_status().requestedBufferSize. Smaller = lower latency, more CPU / dropout risk.
int nc_buffer_size(NCEngine* engine);
void nc_set_buffer_size(NCEngine* engine, int frames);

// Cores DW asks the external DSP Manager to reserve (1..16). This applies live through
// the monitor DSP path — no audio restart. A connected node's own reported core count
// still takes precedence; this is the request/fallback hint.
int nc_dsp_external_core_count(NCEngine* engine);
void nc_dsp_set_external_core_count(NCEngine* engine, int count);

// The remote DSP node the engine streams monitor audio to (host or IPv4). Empty falls
// back to "studio.local". Setting it re-applies the monitor path live — no restart.
// nc_dsp_discover_remote_host broadcast-probes the LAN and returns a node address (or
// "" if none answered) so the UI can fill the field without typing an IP.
void nc_dsp_remote_host(NCEngine* engine, char* out, size_t outLen);
void nc_dsp_set_remote_host(NCEngine* engine, const char* host);
void nc_dsp_discover_remote_host(NCEngine* engine, char* out, size_t outLen);
// Inventory scan (engine-free, blocking ~1 s — background thread): every answering server,
// newline-joined; appliance engines come back as host:20002, legacy cores as plain hosts.
void nc_dsp_scan_lan(char* out, size_t outLen);
// Waves-style server options: wire buffer per remote stream (frames, 64–1024, live) and the
// remote-mixer channel capacity ladder (8/16/32/64, stored intent until the remote mixer lands).
int nc_dsp_network_buffer_frames(NCEngine* engine);
void nc_dsp_set_network_buffer_frames(NCEngine* engine, int frames);
int nc_dsp_mixer_channels(NCEngine* engine);
void nc_dsp_set_mixer_channels(NCEngine* engine, int channels);
// Two performance modes, SoundGrid-style: MIXING (roomy buffer, DSP optimized) and TRACKING
// (tight buffer, latency optimized — floor 40 frames = 0.83 ms at 48k). "auto" follows arming:
// any record-armed or input-monitoring track selects the tracking buffer live.
// nc_dsp_network_buffer_frames above is the MIXING buffer; this pair is the mode + tracking buffer.
void nc_dsp_latency_mode(NCEngine* engine, char* out, size_t outLen);
void nc_dsp_set_latency_mode(NCEngine* engine, const char* mode);
int nc_dsp_tracking_buffer_frames(NCEngine* engine);
void nc_dsp_set_tracking_buffer_frames(NCEngine* engine, int frames);

// ---------------------------------------------------------------------------
// DSP role assignment
//
// Which machine handles each job: "internal" (this Mac), "nds" (the dedicated appliance) or
// "external" (a general-purpose node computer). Assignment is explicit by default — a path that
// changes under you mid-take is worse than one that runs short predictably — and nc_dsp_auto_overflow
// turns the assignments into starting points that spill internal -> NDS -> external as each fills.
// Roles: "monitor", "channelStrip", "master", "inserts".
// ---------------------------------------------------------------------------
void nc_dsp_role(NCEngine* engine, const char* role, char* out, size_t outLen);
void nc_dsp_set_role(NCEngine* engine, const char* role, const char* machine);
int  nc_dsp_auto_overflow(NCEngine* engine);
void nc_dsp_set_auto_overflow(NCEngine* engine, int enabled);
void nc_dsp_nds_host(NCEngine* engine, char* out, size_t outLen);
void nc_dsp_set_nds_host(NCEngine* engine, const char* host);
int  nc_dsp_nds_enabled(NCEngine* engine);
void nc_dsp_set_nds_enabled(NCEngine* engine, int enabled);

// A discovered/queried remote DSP node's identity + hardware specs, for the Remote Core panel.
typedef struct {
    int reachable;          // 1 if a node answered, else 0 (other fields undefined when 0)
    double roundTripMs;     // status-query round-trip, ms
    char host[128];         // resolved address / host
    char model[128];        // node product string ("Remote Core DSP")
    char cpuModel[192];     // CPU brand string; "unknown" if the node can't report it
    double cpuMhz;          // CPU clock in MHz; 0 if unknown (e.g. Apple Silicon)
    int memoryMb;           // physical RAM in MB; 0 if unknown
    int coreCount;          // reported logical cores
    // Live telemetry, so the monitor station can show what the NODE is doing rather than only
    // what this Mac is doing. The node reports per-core load, not a single figure.
    double cpuLoadPercent;  // busiest reported core, 0..100; -1 when the node does not report
    double temperatureC;    // node CPU temperature; 0 if unknown
    unsigned long long packetsIn;
    unsigned long long packetsOut;
    unsigned long long badPackets;   // malformed/rejected — the node-side dropout signal
} NCRemoteNodeInfo;

// Probe the current remote host for its identity + specs. Returns 1 and fills `out` if a node
// answered, else 0. Blocks briefly on the network — call off the UI hot path.
int nc_dsp_remote_node_info(NCEngine* engine, NCRemoteNodeInfo* out);

// Probe an arbitrary address, with no engine involved. This takes no NCEngine precisely so it can
// be called from a background queue: the main-thread-only rule exists because engine calls touch
// the project, and this one touches nothing but a socket. `timeoutMs` bounds the wait so a poll
// against a dead address costs a known amount of time rather than the client default.
int nc_dsp_probe_node_info(const char* host, int timeoutMs, NCRemoteNodeInfo* out);

// Per-item overrides of the project-wide DSP assignment: which machine runs THIS channel's console
// strip, or THIS insert. Empty string follows the project assignment; otherwise "internal" | "nds" |
// "external". A session is rarely uniform, so the global rows are defaults, not verdicts.
void nc_track_console_dsp_machine(NCEngine* engine, int trackIndex, char* out, size_t outLen);
void nc_track_set_console_dsp_machine(NCEngine* engine, int trackIndex, const char* machine);
void nc_track_insert_dsp_machine(NCEngine* engine, int trackIndex, int slot, char* out, size_t outLen);
void nc_track_set_insert_dsp_machine(NCEngine* engine, int trackIndex, int slot, const char* machine);
// The same per-slot override for the MASTER chain. The chain is serial, so the engine only
// offloads when every active slot resolves to a Neuracoust module on one machine.
void nc_master_insert_dsp_machine(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_master_insert_set_dsp_machine(NCEngine* engine, int slot, const char* machine);

// The "use this node" master switch: whether the external DSP node participates at all. Off gates the
// node out of the monitor/DAW/plugin core plan regardless of the requested reserve. Applies live.
int nc_dsp_external_enabled(NCEngine* engine);
void nc_dsp_set_external_enabled(NCEngine* engine, int enabled);

// ---------------------------------------------------------------------------
// Output device
//
// Which physical output the engine opens. An empty id means the system default; the
// device is read at start(), so changing it restarts the engine. nc_output_device_*
// enumerate the output-capable devices (call count first, it refreshes the cache).
// ---------------------------------------------------------------------------

int nc_output_device_count(NCEngine* engine);
void nc_output_device_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_output_device_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_current_output_device_id(NCEngine* engine, char* out, size_t outLen);
void nc_active_output_device_name(NCEngine* engine, char* out, size_t outLen);
void nc_set_output_device(NCEngine* engine, const char* deviceId);

// Input device — the reference/monitor input (e.g. BlackHole for reference music).
// Empty id = system default; read at start(), so changing it restarts the engine.
// nc_input_device_* enumerate the input-capable devices (call count first to refresh).
int nc_input_device_count(NCEngine* engine);
void nc_input_device_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_input_device_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_current_input_device_id(NCEngine* engine, char* out, size_t outLen);
void nc_set_input_device(NCEngine* engine, const char* deviceId);

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
float nc_monitor_dim_db(NCEngine* engine);
void nc_monitor_set_dim_db(NCEngine* engine, float db);

// Monitor source: master (false, default) vs the computer's input source (true).
bool nc_monitor_listen_source(NCEngine* engine);
void nc_monitor_set_listen_source(NCEngine* engine, bool on);
// Reference-hold arming: run the tap + mute the tapped apps while armed, so A/B-ing to the
// master never leaks their sound out of the computer. Disarm also clears the listening state.
bool nc_monitor_reference_armed(NCEngine* engine);
void nc_monitor_set_reference_armed(NCEngine* engine, bool on);
// Auto-input punch: hear the tap on the master only while punched in.
void nc_monitor_set_tap_input_monitor(NCEngine* engine, bool on);
// Input-Monitor toggle on a tap-input track: run + hear the tap continuously.
void nc_monitor_set_tap_input_hold(NCEngine* engine, bool on);

// Reverb/delay tail rendered after stop, in seconds (0 = cut immediately).
double nc_insert_tail_on_stop_seconds(NCEngine* engine);
void nc_set_insert_tail_on_stop_seconds(NCEngine* engine, double seconds);
void nc_monitor_set_talkback(NCEngine* engine, bool on);
// Talkback destination: "monitor_bus", "listen_room" (default, remote listeners only), "all".
void nc_monitor_talkback_route(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_talkback_route(NCEngine* engine, const char* route);
// Talkback mic input channel (1-based) + the picker's live-channel helpers.
int   nc_monitor_talkback_channel(NCEngine* engine);
void  nc_monitor_set_talkback_channel(NCEngine* engine, int oneBased);
int   nc_talkback_channel_count(NCEngine* engine);
float nc_talkback_channel_activity(NCEngine* engine, int oneBased);

// The monitor listen state, ported whole from the old UI. It is not four exclusive
// modes: it is a listen mode string ("LR"/"L"/"R"/"M"/"S"), a separate mono flag, and
// independent L/R phase inverts, kept consistent by normalizeMonitorStationProjectState.
//
//   Stereo button — cycles Stereo -> Left-only -> Right-only (or Mid, in M/S mode)
//   Mono button   — cycles Mono -> L-into-both -> R-into-both (or Side, in M/S mode)
//   M/S button    — toggles Mid/Side monitoring
//   Ø button      — cycles phase invert Off -> ØL -> ØR -> ØLR
void nc_monitor_listen_mode(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_listen_mode(NCEngine* engine, const char* mode);
void nc_monitor_cycle_stereo(NCEngine* engine);
void nc_monitor_cycle_mono(NCEngine* engine);
void nc_monitor_toggle_mid_side(NCEngine* engine);
void nc_monitor_cycle_phase(NCEngine* engine);
/// Swap left/right in the monitor path (speakers wired backwards). No effect in Mid/Side.
bool nc_monitor_swap_left_right(NCEngine* engine);
void nc_monitor_toggle_swap_left_right(NCEngine* engine);
bool nc_monitor_mid_side(NCEngine* engine);
bool nc_monitor_invert_left(NCEngine* engine);
bool nc_monitor_invert_right(NCEngine* engine);

// A/B/C speaker sets live on the "speaker-simulation" module. Slot is 0, 1 or 2.
int nc_monitor_active_speaker_slot(NCEngine* engine);
void nc_monitor_set_active_speaker_slot(NCEngine* engine, int slot);
void nc_monitor_speaker_model(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_monitor_speaker_output(NCEngine* engine, int slot, char* out, size_t outLen);
float nc_monitor_speaker_sim_weight(NCEngine* engine, int slot);
bool nc_monitor_speaker_room_eq(NCEngine* engine, int slot);

// Per-slot (A=0/B=1/C=2) setters. Model is a bare catalog name ("Genelec 8040B (NF)");
// the bridge stores it as "Speaker X: <name>". Output is a route ("None", "Main 1-2",
// "Output 3-4", ...) — picking a hardware route forces the slot's model to Flat and
// room EQ off (physical passthrough), matching the reference. All apply live via
// pushModules(). The catalog getters back the pickers.
int  nc_speaker_model_count(void);
void nc_speaker_model_name(int index, char* out, size_t outLen);
// Headphone model catalog, and the **physical** speaker/headphone the user monitors on
// (a definition of real hardware, not a simulation). Plus the speaker/headphone
// exclusivity flag — when set, one output is active at a time.
int  nc_headphone_model_count(void);
void nc_headphone_model_name(int index, char* out, size_t outLen);
void nc_monitor_physical_speaker_model(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_speaker_model(NCEngine* engine, const char* model);
void nc_monitor_physical_headphone_model(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_headphone_model(NCEngine* engine, const char* model);
// A passive speaker runs off an external power amp + speaker cable; an active monitor has
// the amp built in. is_passive drives whether the amp/cable pickers are enabled.
bool nc_speaker_model_is_passive(const char* name);
int  nc_power_amp_model_count(void);
void nc_power_amp_model_name(int index, char* out, size_t outLen);
int  nc_speaker_cable_model_count(void);
void nc_speaker_cable_model_name(int index, char* out, size_t outLen);
int  nc_power_cable_model_count(void);
void nc_power_cable_model_name(int index, char* out, size_t outLen);
int  nc_connector_model_count(void);
void nc_connector_model_name(int index, char* out, size_t outLen);
void nc_monitor_physical_power_amp_model(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_power_amp_model(NCEngine* engine, const char* model);
void nc_monitor_physical_speaker_cable_model(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_speaker_cable_model(NCEngine* engine, const char* model);
void nc_monitor_physical_power_cable_model(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_power_cable_model(NCEngine* engine, const char* model);
void nc_monitor_physical_connector_model(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_connector_model(NCEngine* engine, const char* model);
// Whether a passive speaker's amp / cable name-heuristic actually colours the monitor sound.
bool nc_power_amp_tone_active(NCEngine* engine);
bool nc_speaker_cable_tone_active(NCEngine* engine);
// Audio-interface D/A output-stage model (catalog + measurement status only; no audio effect yet).
int  nc_audio_interface_model_count(void);
void nc_audio_interface_model_name(int index, char* out, size_t outLen);
bool nc_audio_interface_model_measured(const char* name);
void nc_monitor_physical_audio_interface_model(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_audio_interface_model(NCEngine* engine, const char* model);
// Purpose 2: render the physical interface AS a different model (A->B). Stored intent only.
void nc_monitor_physical_audio_interface_target(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_physical_audio_interface_target(NCEngine* engine, const char* model);
// True only when a raw-measured A->B transform is actually applied to audio (never from specs).
bool nc_audio_interface_transform_active(NCEngine* engine);
// Optional 2단계 harmonic (waveshaper) modeling of the interface's measured nonlinear character.
bool nc_monitor_interface_modeling_enabled(NCEngine* engine);
void nc_monitor_set_interface_modeling_enabled(NCEngine* engine, bool enabled);

// Measurement microphone selection (drives absolute vs relative-only room correction).
int  nc_measurement_mic_model_count(void);
void nc_measurement_mic_model_name(int index, char* out, size_t outLen);
bool nc_measurement_mic_has_calibration(const char* name);
void nc_measurement_mic_model(NCEngine* engine, char* out, size_t outLen);
void nc_set_measurement_mic_model(NCEngine* engine, const char* model);

// Monitor parametric EQ (0–64 bands, added on demand; monitor path only, never printed).
// type is "peaking" / "low_shelf" / "high_shelf" / "high_pass" / "low_pass" / "notch".
int  nc_monitor_eq_band_count(NCEngine* engine);
bool nc_monitor_eq_band(NCEngine* engine, int index, bool* enabled, char* typeOut, size_t typeLen,
                        double* freq, double* gain, double* q);
int  nc_monitor_eq_add_band(NCEngine* engine, const char* type, double freq, double gain, double q);
bool nc_monitor_eq_set_band(NCEngine* engine, int index, bool enabled, const char* type,
                            double freq, double gain, double q);
bool nc_monitor_eq_remove_band(NCEngine* engine, int index);
void nc_monitor_eq_clear(NCEngine* engine);
void nc_monitor_eq_response(NCEngine* engine, double* outMagsDb, int count, double minHz, double maxHz);
// Linear-phase (FIR) monitor EQ. When on, the monitor EQ is a FIR that matches the target curve
// across the whole band (no biquad ripple/cramping) at the cost of latency. Toggling requires a
// re-sync (reloadMonitorState) to rebuild through the chosen path.
bool nc_monitor_eq_linear_phase(NCEngine* engine);
void nc_monitor_eq_set_linear_phase(NCEngine* engine, bool enabled);
double nc_monitor_eq_latency_ms(NCEngine* engine);
// Optional: reference a headphone model to the Harman OE target (removes the raw ear-gain baseline).
bool nc_monitor_eq_headphone_oe_target(NCEngine* engine);
void nc_monitor_eq_set_headphone_oe_target(NCEngine* engine, bool enabled);
// The room-tuning correction curve (Harman − measured, fitted), sampled like nc_monitor_eq_response
// without touching the live EQ. Flat until a room measurement exists; returns false when unmeasured.
bool nc_monitor_room_correction_response(NCEngine* engine, int channel, double* outMagsDb, int count, double minHz, double maxHz);

// Acoustic measurement (②b): play a sweep out a channel (0=L,1=R), capture the mic, deconvolve
// to the in-room response curve. Requires input monitoring / a mic input for samples to arrive.
bool   nc_measure_start(NCEngine* engine, int channel);
bool   nc_measure_active(NCEngine* engine);
double nc_measure_progress(NCEngine* engine);
void   nc_measure_cancel(NCEngine* engine);
bool   nc_measure_finish(NCEngine* engine, int channel);
bool   nc_measure_has_curve(NCEngine* engine, int channel);
void   nc_measure_curve_response(NCEngine* engine, int channel, double* out, int count, double minHz, double maxHz);

// VR / headset-worn monitor correction. Measure the room with the headset OFF → capture_baseline,
// then with it ON → capture_worn builds (baseline − worn) and enables it. Added to the monitor EQ.
bool   nc_vr_capture_baseline(NCEngine* engine);
bool   nc_vr_capture_worn(NCEngine* engine);
bool   nc_vr_correction_enabled(NCEngine* engine);
bool   nc_vr_correction_active(NCEngine* engine);
bool   nc_vr_has_baseline(NCEngine* engine);
void   nc_vr_set_correction_enabled(NCEngine* engine, bool on);
void   nc_vr_clear_correction(NCEngine* engine);
void   nc_vr_correction_response(NCEngine* engine, double* out, int count, double minHz, double maxHz);

// Audio-interface loopback measurement (②d): patch the interface DAC output back to its ADC
// input, run nc_measure_start (channel 0), then nc_measure_interface_finish. One ESS capture
// yields the D/A frequency response AND the harmonic coefficients, stored for the currently
// selected physical interface (project.physicalAudioInterfaceModel) and persisted per-model.
// A live measurement overrides the offline baked profile. Requires input monitoring on.
bool   nc_measure_interface_start(NCEngine* engine);   // loopback: sweep on measure-output channel
bool   nc_measure_interface_finish(NCEngine* engine);  // holds the result as PENDING for review
// Pending-measurement review + commit/discard (quality verdict before saving over a good profile).
bool   nc_measure_interface_pending(NCEngine* engine);
double nc_measure_interface_pending_thd(NCEngine* engine);
float  nc_measure_interface_pending_peak(NCEngine* engine);   // sweep peak 0..1 (>=~0.99 clipped)
void   nc_measure_interface_pending_name(NCEngine* engine, char* out, size_t outLen);
void   nc_measure_interface_commit(NCEngine* engine);         // save + apply the pending measurement
void   nc_measure_interface_discard(NCEngine* engine);
// Multi-level auto run (A단계): drive level per sweep, accumulate per-level results, read the curve.
void   nc_measure_set_sweep_amplitude(NCEngine* engine, double amplitude);   // 0.001..0.99
void   nc_measure_interface_reset_levels(NCEngine* engine);
void   nc_measure_interface_record_level(NCEngine* engine, double returnDbfs);
int    nc_measure_interface_level_count(NCEngine* engine);
double nc_measure_interface_level_dbfs(NCEngine* engine, int index);
double nc_measure_interface_level_thd(NCEngine* engine, int index);
bool   nc_measure_interface_has_profile(NCEngine* engine, const char* name);
// Loopback measurement channel selection (1-based physical channels). Output = DAC channel the
// sweep exits; input = ADC channel the loopback is patched into. Counts come from the open device.
void   nc_measure_level_check(NCEngine* engine, bool on);   // live input meter for gain setup
float  nc_measure_input_level(NCEngine* engine);            // linear peak of the chosen ADC channel, 0..1
void   nc_measure_set_output_channel(NCEngine* engine, int oneBased);
void   nc_measure_set_input_channel(NCEngine* engine, int oneBased);
int    nc_measure_output_channel(NCEngine* engine);
int    nc_measure_input_channel(NCEngine* engine);
int    nc_measure_output_channel_count(NCEngine* engine);
int    nc_measure_input_channel_count(NCEngine* engine);
double nc_measure_interface_thd(NCEngine* engine, const char* name);   // total harmonic distortion, %
void   nc_measure_interface_harmonics(NCEngine* engine, const char* name, double* out, int count);  // [c2..] linear
void   nc_measure_interface_curve_response(NCEngine* engine, const char* name, double* out, int count, double minHz, double maxHz);
void   nc_measure_interface_clear(NCEngine* engine, const char* name); // drop the measurement, revert to baked

// Virtual monitor: model a target speaker (by catalog name) on the physical monitor by loading
// its fitted correction curve into the monitor EQ.
int  nc_virtual_monitor_count(NCEngine* engine);
void nc_virtual_monitor_name(NCEngine* engine, int index, char* out, size_t outLen);
// Headphone models with a measured curve (feeds the headphone modeller + the "측정" badge).
int  nc_headphone_profile_count(NCEngine* engine);
void nc_headphone_profile_name(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_headphone_profile_response(NCEngine* engine, const char* name, double* outMagsDb, int count, double minHz, double maxHz);
bool nc_speaker_profile_response(NCEngine* engine, const char* name, double* outMagsDb, int count, double minHz, double maxHz);
bool nc_audio_interface_profile_response(NCEngine* engine, const char* name, double* outMagsDb, int count, double minHz, double maxHz);
bool nc_monitor_eq_apply_virtual_monitor(NCEngine* engine, const char* catalogName);
// Room correction (③): flatten the measured in-room curve toward the Harman target.
bool nc_monitor_eq_apply_room_correction(NCEngine* engine, int channel);
// Rebuild the single monitor EQ from the active context: the given model's measured curve
// (empty = none) plus room-tuning correction (applyRoom). Records no history — derived state.
void nc_monitor_eq_sync(NCEngine* engine, const char* slotModel, const char* correctionHeadphone, bool applyRoom);
bool nc_monitor_output_exclusive(NCEngine* engine);
void nc_monitor_set_output_exclusive(NCEngine* engine, bool exclusive);

// Auto fade-out: a fade written into the Master track's volume automation over the last
// N seconds of content (0 = off). Setting either regenerates it (auto-fade owns the
// master automation). Curve: linear / equal_power / exponential / logarithmic.
// nc_auto_fade_amplitude gives the 0..1 amplitude at a normalized position for the UI
// curve preview.
double nc_master_auto_fade_seconds(NCEngine* engine);
void   nc_master_set_auto_fade_seconds(NCEngine* engine, double seconds);
void   nc_master_auto_fade_curve(NCEngine* engine, char* out, size_t outLen);
void   nc_master_set_auto_fade_curve(NCEngine* engine, const char* curve);
float  nc_auto_fade_amplitude(const char* curve, double t);
// Physical output pairs exposed by the currently opened CoreAudio device. The list
// contains the modelled path ("None"), Main 1-2, then only the additional pairs the
// hardware actually reports (3-4, 5-6, ...). It is never padded to an arbitrary size.
int  nc_speaker_output_route_count(NCEngine* engine);
void nc_speaker_output_route(NCEngine* engine, int index, char* out, size_t outLen);
void nc_monitor_set_speaker_model(NCEngine* engine, int slot, const char* model);
void nc_monitor_set_speaker_real_model(NCEngine* engine, int slot, const char* model);
void nc_monitor_speaker_real_model(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_monitor_set_speaker_output(NCEngine* engine, int slot, const char* route);
// The headphone side's OWN physical output pair, independent of the A/B/C speaker slots
// ("" / "None" = the main pair). The engine routes to it whenever the monitor destination is
// headphone — which nc_monitor_set_output_to_headphone reports from the 스피커/헤드폰 tab.
void nc_monitor_headphone_output(NCEngine* engine, char* out, size_t outLen);
void nc_monitor_set_headphone_output(NCEngine* engine, const char* route);
bool nc_monitor_output_to_headphone(NCEngine* engine);
void nc_monitor_set_output_to_headphone(NCEngine* engine, bool headphone);
void nc_monitor_set_speaker_room_eq(NCEngine* engine, int slot, bool enabled);
// Per-slot amp/cable for a passive modeled speaker (heuristic tone folded into the monitor EQ).
void nc_monitor_speaker_amp(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_monitor_speaker_cable(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_monitor_set_speaker_amp(NCEngine* engine, int slot, const char* model);
void nc_monitor_set_speaker_cable(NCEngine* engine, int slot, const char* model);
// Per-slot amp/cable for a passive REAL speaker (subtracted — flattens the chain you hear on).
void nc_monitor_speaker_real_amp(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_monitor_speaker_real_cable(NCEngine* engine, int slot, char* out, size_t outLen);
void nc_monitor_set_speaker_real_amp(NCEngine* engine, int slot, const char* model);
void nc_monitor_set_speaker_real_cable(NCEngine* engine, int slot, const char* model);

// Live MIDI input: monitor a keyboard through armed/input-monitoring instrument tracks.
// Enumerate sources, start on one (empty = first available), then call
// nc_midi_pump_live_input each UI tick to drain events into the instruments. Live notes
// sound whether or not the transport is running.
int  nc_midi_input_count(NCEngine* engine);
void nc_midi_input_id(NCEngine* engine, int index, char* out, size_t outLen);
void nc_midi_input_name(NCEngine* engine, int index, char* out, size_t outLen);
bool nc_midi_live_start(NCEngine* engine, const char* sourceId);
void nc_midi_live_stop(NCEngine* engine);
bool nc_midi_live_active(NCEngine* engine);
/// One pumped live-MIDI event as raw MIDI bytes, so the UI can mirror the stream to an
/// open plug-in editor process (whose GUI keyboard/wheel otherwise never sees the input).
typedef struct NCMidiLiveEvent {
    unsigned char status; ///< status byte incl. channel (0x9n note-on, 0x8n off, 0xBn CC, 0xEn bend, 0xCn program)
    unsigned char data1;
    unsigned char data2;
} NCMidiLiveEvent;
/// Drains pending keyboard input into the armed/monitored instrument tracks (as before) and
/// also copies up to maxEvents of the drained batch into outEvents. Returns how many were
/// written; pass NULL/0 to just pump.
int nc_midi_pump_live_input(NCEngine* engine, NCMidiLiveEvent* outEvents, int maxEvents);
/// MIDI recording. Begin a take on a track at the transport position, feed it the pumped
/// keyboard events each tick (the same batch nc_midi_pump_live_input returns, stamped at the
/// current playhead), and commit on stop to create a region with the recorded notes. The
/// recording path is independent of what the monitor station is listening to.
bool nc_midi_record_begin(NCEngine* engine, int trackIndex, double startSeconds);
bool nc_midi_record_active(NCEngine* engine);
void nc_midi_record_feed(NCEngine* engine, const NCMidiLiveEvent* events, int count,
                         double playheadSeconds);
/// Finish the take: closes still-held notes, creates the region + notes (one undo step), and
/// returns the new region id (empty if nothing was recorded).
bool nc_midi_record_commit(NCEngine* engine, char* outRegionId, size_t outRegionIdLen);

// Low-latency monitoring. The linear-phase monitor EQ buys its exact curve with numTaps/2
// samples of pure delay — 42.7 ms at the default 4096 taps and 48 kHz — on everything you
// hear. Correct for judging a mix, unplayable for performing. With this on (the default) the
// EQ falls back to the minimum-phase fit of the same curve whenever a track is record-armed
// or input-monitoring, which adds no delay. Call nc_monitor_eq_sync afterwards to rebuild.
bool nc_monitor_eq_low_latency_monitoring(NCEngine* engine);
void nc_monitor_eq_set_low_latency_monitoring(NCEngine* engine, bool enabled);
/// Whether the fallback is engaged right now, so the UI can say why the curve is not linear phase.
bool nc_monitor_eq_low_latency_active(NCEngine* engine);
/// What the monitor EQ is adding right now, in samples. 0 on the minimum-phase path.
int nc_monitor_eq_latency_samples(NCEngine* engine);

/// Sounds one note on a track's instrument without recording anything — the piano roll's
/// keyboard, and any other place that auditions a pitch. Rides the same live-MIDI queue the
/// keyboard uses, so it needs no transport and no region. Call again with noteOn=false to
/// release; an un-released note sustains, exactly like a held key.
void nc_midi_preview_note(NCEngine* engine, int trackIndex, int pitch, int velocity, bool noteOn);
/// Releases every note this track's preview may still be holding — for a mouse-up that
/// landed somewhere else, or a view going away mid-drag.
void nc_midi_preview_all_notes_off(NCEngine* engine, int trackIndex);

// Which controllers a MIDI take captures. A keyboard sends far more than a part needs, so
// the region only receives the CC numbers switched on here; everything else is still heard
// live and simply not written down. Defaults: sustain (64), modulation (1), pitch bend.
// Project state, so a take records what the song was set up to record.
bool nc_midi_record_controller_enabled(NCEngine* engine, int controller);
void nc_midi_record_set_controller_enabled(NCEngine* engine, int controller, bool enabled);
bool nc_midi_record_pitch_bend_enabled(NCEngine* engine);
void nc_midi_record_set_pitch_bend_enabled(NCEngine* engine, bool enabled);

/// Route the keyboard to the selected instrument track even when it is not record-armed
/// (Logic/Live convention). Pass the track index, or -1 to clear. Transient, no undo.
void nc_set_live_midi_target(NCEngine* engine, int trackIndex);
/// Instrument editor reverse-audio monitor. While an instrument editor is open, its own
/// plug-in instance renders GUI keyboard clicks and the forwarded live MIDI; this creates
/// the shared-memory ring the editor publishes into, mixes it into the monitor path, and
/// hands the track's live-MIDI path to the editor instance (the render instance stops
/// hearing the keyboard so nothing doubles). One editor at a time — opening another moves
/// the monitor. Pass the returned shm name/block/rate to the editor host (--monitor-shm).
bool nc_track_instrument_editor_opened(NCEngine* engine, int index,
                                       char* shmName, size_t shmNameLen,
                                       int* maxBlock, double* sampleRate);
/// Tears the ring down and returns the live-MIDI path to the render instance. Safe to
/// call for an editor that never owned the monitor (a newer editor's ring survives).
void nc_track_instrument_editor_closed(NCEngine* engine, int index);

// Instrument patch handoff (the plug-in's own VST3 component state).
//
// A workstation instrument keeps its selected program in its component state, not in its
// parameters — KORG TRITON publishes 2,573 parameters and not one of them selects the
// program. Mirroring parameters alone therefore loses the patch the moment the editor
// closes and the render instance takes the sound back. These two calls move the blob
// between the project and the editor-host process through a file, because a sampler's
// state is far too large for either an argv entry or a pipe line.
//
// Writes the slot's stored patch to `path` as raw bytes for the editor host's
// --state-file. False when the slot has no stored patch (nothing was written).
bool nc_track_instrument_slot_write_state_file(NCEngine* engine, int index, int slotIndex,
                                               const char* path);
/// Reads the patch the editor host left at `path` into the slot and rebuilds the render
/// instance on it, so the sound the editor was making is the sound that remains. Records
/// one undo step. False when the file is missing/empty or the patch is unchanged.
bool nc_track_instrument_slot_read_state_file(NCEngine* engine, int index, int slotIndex,
                                              const char* path);
// Peak MIDI-input activity (0..1) since the last call; reading it resets it, so the UI
// applies its own decay. Bump it by calling nc_midi_pump_live_input first each tick.
float nc_midi_input_activity(NCEngine* engine);

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

/// Regenerates the share token, invalidating any link already handed out. The caller
/// restarts the relay if it is running, so a listener on the old link is dropped.
void nc_listen_reset_token(NCEngine* engine);

void nc_listen_quality(NCEngine* engine, char* out, size_t outLen);
void nc_listen_set_quality(NCEngine* engine, const char* quality);
void nc_listen_latency_mode(NCEngine* engine, char* out, size_t outLen);
void nc_listen_set_latency_mode(NCEngine* engine, const char* mode);

/// Share link for the local relay, and the token-bearing invite link.
void nc_listen_share_url(NCEngine* engine, char* out, size_t outLen);
/// The LAN address a listener on the same network reaches the relay at.
void nc_listen_public_share_url(NCEngine* engine, char* out, size_t outLen);
/// The public invite link a listener anywhere reaches through the external page, the
/// way the old UI built it: a base URL (env NEURACOUST_LISTEN_EXTERNAL_URL, then
/// ~/.neuracoust/listen_external_url, then the tplinkdns default) with
/// external/profile/session/quality/latency/transport/connect/token appended. This is
/// what Copy and QR hand out — the LAN address only works on the same network.
void nc_listen_external_share_url(NCEngine* engine, char* out, size_t outLen);

// --- AI assistant (Phase 0) ---
/// The project snapshot the assistant reasons over (tracks, tempo, health), serialized.
void nc_ai_project_context(NCEngine* engine, char* out, size_t outLen);
/// Build the Ollama /api/chat request body (JSON) for a user message: system prompt +
/// command schema + current project snapshot + the user's text. Swift POSTs this. Pass a
/// large buffer (the snapshot can be several KB).
void nc_ai_build_request(NCEngine* engine, const char* model, const char* userText,
                         char* out, size_t outLen);
/// Validate and apply one assistant-proposed command (SetTrackGain/Pan/Mute/Solo,
/// ArmTrackForRecording, AddMarker), recording a single undo step. Returns false with a
/// reason in msg if validation or the edit fails.
bool nc_ai_apply_command(NCEngine* engine, const char* typeStr, const char* trackName,
                         float gainDb, float pan, bool enabled, double timeSeconds,
                         const char* label, char* msg, size_t msgLen);

#ifdef __cplusplus
} // extern "C"
#endif
