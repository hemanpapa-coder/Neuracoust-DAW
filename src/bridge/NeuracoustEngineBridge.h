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

// FFT spectrum bins for the analyzer (0..1, dB-scaled, low→high frequency). Cached on
// each nc_engine_status call, so poll status first, then read these.
int nc_spectrum_bin_count(NCEngine* engine);
bool nc_spectrum_bins(NCEngine* engine, float* out, int count);

// Goniometer L/R sample pairs (interleaved L,R,L,R…). Cached on each nc_engine_status.
int nc_goniometer_sample_count(NCEngine* engine);
bool nc_goniometer_samples(NCEngine* engine, float* out, int count);

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

/// New tracks land at the end of the list. Returns the new track's index, or -1.
int nc_track_add_audio(NCEngine* engine);
int nc_track_add_instrument(NCEngine* engine);

/// Loads a scanned plug-in into the track's instrument slot — the thing that turns
/// its MIDI notes into sound. `pluginIndex` addresses the filtered browser list.
bool nc_track_set_instrument(NCEngine* engine, int trackIndex, int pluginIndex);
// Remove the instrument from a track's instrument slot.
bool nc_track_clear_instrument(NCEngine* engine, int trackIndex);

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
void nc_track_instrument_name(NCEngine* engine, int trackIndex, char* out, size_t outLen);
int nc_track_add_midi(NCEngine* engine);

/// Deletes the track and, when `removeClips` is true, everything on it. Refuses on
/// Master and Monitor. Returns false when the track cannot go.
bool nc_track_delete(NCEngine* engine, int index, bool removeClips);

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

/// wav, wave, mp3, aif, aiff, m4a, caf.
bool nc_audio_import_supported(const char* path);

/// Imports `path` onto the track at `trackIndex`, starting at `startSeconds`.
/// Non-WAV sources are converted; the result lands in the project's Audio Files
/// folder, or a temporary folder when the project has no path yet.
bool nc_audio_import(NCEngine* engine, int trackIndex, const char* path, double startSeconds,
                     char* error, size_t errorLen);

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
bool nc_clip_trim_start(NCEngine* engine, const char* clipId, double newStartSeconds);
bool nc_clip_trim_end(NCEngine* engine, const char* clipId, double newEndSeconds);

/// Splits at `seconds`; the right-hand piece gets a new id. Records a step.
bool nc_clip_split(NCEngine* engine, const char* clipId, double seconds);
// Heal (re-join) adjacent same-source clips within a time range; returns how many
// resulting clips were glued. Pass the span of the selected clips or the edit range.
int  nc_clip_glue_range(NCEngine* engine, double startSeconds, double endSeconds);
bool nc_clip_delete(NCEngine* engine, const char* clipId);

/// -60…+12 dB. Continuous, like move and trim.
float nc_clip_gain_db(NCEngine* engine, int index);
bool nc_clip_set_gain_db(NCEngine* engine, const char* clipId, float gainDb);
/// Continuous preview: sets the gain field only, no graph rebuild (smooth drag).
/// Commit with nc_clip_set_gain_db to reconcile + record one step.
bool nc_clip_set_gain_db_preview(NCEngine* engine, const char* clipId, float gainDb);

/// Fades, in seconds from each end. Continuous, like move and trim.
double nc_clip_fade_in(NCEngine* engine, int index);
double nc_clip_fade_out(NCEngine* engine, int index);
bool nc_clip_set_fades(NCEngine* engine, const char* clipId, double fadeIn, double fadeOut);
bool nc_clip_set_fade_curves(NCEngine* engine, const char* clipId,
                             const char* inCurve, const char* outCurve);
/// Turn a same-track overlap around this clip into a crossfade (no history step).
bool nc_clip_apply_crossfades(NCEngine* engine, const char* clipId);
void nc_clip_fade_in_curve(NCEngine* engine, int index, char* out, size_t outLen);
void nc_clip_fade_out_curve(NCEngine* engine, int index, char* out, size_t outLen);

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

/// Renders a serialized document. Touches no shared state — safe to call from a
/// background thread while the user keeps editing.
bool nc_bounce_snapshot_to_wav(const char* projectText, const char* path, NCBounceResult* out);

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

/// Empty strings mean "no constraint". Returns the number of matches.
int nc_plugin_apply_filter(NCEngine* engine,
                           const char* text,
                           const char* brand,
                           const char* category,
                           const char* format);

int nc_plugin_count(NCEngine* engine);
void nc_plugin_name(NCEngine* engine, int index, char* out, size_t outLen);
void nc_plugin_brand(NCEngine* engine, int index, char* out, size_t outLen);
void nc_plugin_category(NCEngine* engine, int index, char* out, size_t outLen);
void nc_plugin_format(NCEngine* engine, int index, char* out, size_t outLen);
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
bool nc_track_remove_insert(NCEngine* engine, int trackIndex, int slot);
/// direction is -1 (earlier in the chain) or +1. Returns the new slot index, or -1.
int nc_track_move_insert(NCEngine* engine, int trackIndex, int slot, int direction);
int nc_track_move_insert_to_index(NCEngine* engine, int trackIndex, int fromSlot, int toSlot);
int nc_track_move_insert_to_slot(NCEngine* engine, int trackIndex, int fromSlot, int toSlot);

/// "NAT", "INT", "RINT" or "EXT" — what the insert will actually run on.
void nc_track_insert_mode_badge(NCEngine* engine, int trackIndex, int slot, char* out, size_t outLen);

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

bool nc_dsp_core_isolation(NCEngine* engine);
void nc_dsp_set_core_isolation(NCEngine* engine, bool enabled);
int nc_dsp_core_count(NCEngine* engine);
void nc_dsp_set_core_count(NCEngine* engine, int count);

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

// Monitor source: master (false, default) vs the computer's input source (true).
bool nc_monitor_listen_source(NCEngine* engine);
void nc_monitor_set_listen_source(NCEngine* engine, bool on);

// Reverb/delay tail rendered after stop, in seconds (0 = cut immediately).
double nc_insert_tail_on_stop_seconds(NCEngine* engine);
void nc_set_insert_tail_on_stop_seconds(NCEngine* engine, double seconds);
void nc_monitor_set_talkback(NCEngine* engine, bool on);

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
int  nc_speaker_output_route_count(void);
void nc_speaker_output_route(int index, char* out, size_t outLen);
void nc_monitor_set_speaker_model(NCEngine* engine, int slot, const char* model);
void nc_monitor_set_speaker_output(NCEngine* engine, int slot, const char* route);
void nc_monitor_set_speaker_room_eq(NCEngine* engine, int slot, bool enabled);

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
void nc_midi_pump_live_input(NCEngine* engine);
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

#ifdef __cplusplus
} // extern "C"
#endif
