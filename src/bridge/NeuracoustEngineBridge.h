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
void nc_track_input_bus(NCEngine* engine, int index, char* out, size_t outLen);
void nc_track_output_bus(NCEngine* engine, int index, char* out, size_t outLen);

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

int nc_track_send_count(NCEngine* engine, int index);
void nc_track_send_bus(NCEngine* engine, int index, int slot, char* out, size_t outLen);
float nc_track_send_gain_db(NCEngine* engine, int index, int slot);

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

bool nc_clip_move(NCEngine* engine, const char* clipId, double newStartSeconds);
bool nc_clip_trim_start(NCEngine* engine, const char* clipId, double newStartSeconds);
bool nc_clip_trim_end(NCEngine* engine, const char* clipId, double newEndSeconds);

/// Splits at `seconds`; the right-hand piece gets a new id. Records a step.
bool nc_clip_split(NCEngine* engine, const char* clipId, double seconds);
bool nc_clip_delete(NCEngine* engine, const char* clipId);

/// -60…+12 dB. Continuous, like move and trim.
float nc_clip_gain_db(NCEngine* engine, int index);
bool nc_clip_set_gain_db(NCEngine* engine, const char* clipId, float gainDb);

/// Fades, in seconds from each end. Continuous, like move and trim.
double nc_clip_fade_in(NCEngine* engine, int index);
double nc_clip_fade_out(NCEngine* engine, int index);
bool nc_clip_set_fades(NCEngine* engine, const char* clipId, double fadeIn, double fadeOut);

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

int nc_track_automation_count(NCEngine* engine, int trackIndex, const char* parameterId);
double nc_track_automation_time(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex);
float nc_track_automation_value(NCEngine* engine, int trackIndex, const char* parameterId, int pointIndex);

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

#define NC_WAVEFORM_BUCKETS 2048

/// Reads (or reuses) the peaks for `path`, writing NC_WAVEFORM_BUCKETS values into
/// each of `mins` and `maxs`, both in -1…1. Returns false when the file cannot be
/// read. Channels are summed to mono.
bool nc_waveform_peaks(NCEngine* engine, const char* path, float* mins, float* maxs);

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
