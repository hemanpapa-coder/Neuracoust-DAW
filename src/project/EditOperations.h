#pragma once

#include "project/ProjectDocument.h"
#include <cstddef>
#include <string>
#include <vector>

namespace neuracoust::daw {

inline constexpr size_t kMaxTrackInsertSlots = 20;

struct TimelineGridLine {
    double timeSeconds = 0.0;
    bool bar = false;
    bool beat = false;
    int barNumber = 0;
    int beatInBar = 0;
};

struct ClipTimeScaleApplyResult {
    size_t changedClips = 0;
    size_t skippedClips = 0;
    std::vector<std::string> changedClipIds;
    std::string message;
};

enum class RecordedMidiEventKind {
    NoteOn,
    NoteOff,
    Controller,
    PitchBend,
    ProgramChange
};

struct RecordedMidiEvent {
    RecordedMidiEventKind kind = RecordedMidiEventKind::NoteOn;
    double timeSeconds = 0.0;
    int channel = 1;
    int pitch = 60;
    int velocity = 96;
    int controller = 0;
    int value = 0;
    int program = 0;
};

double projectTimelineQuantumSeconds(const ProjectDocument& project);
double projectVideoFrameRate(const ProjectDocument& project);
double projectFrameDurationSeconds(const ProjectDocument& project);
std::string projectTimecodeString(const ProjectDocument& project, double seconds);
double projectTempoAtSeconds(const ProjectDocument& project, double seconds);
double snapProjectTime(const ProjectDocument& project, double seconds);
std::vector<TimelineGridLine> projectMusicalGridLines(const ProjectDocument& project,
                                                      const std::string& gridUnit,
                                                      double visibleStartSeconds,
                                                      double visibleEndSeconds,
                                                      size_t maxLines = 640);
bool moveClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds);
bool moveVideoClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds);
bool shuffleMoveClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds, const std::string& newTrackName = {});
bool nudgeClip(ProjectDocument& project, const std::string& clipId, double deltaSeconds);
bool nextClipBoundaryAfter(const ProjectDocument& project,
                           double seconds,
                           double& boundarySeconds,
                           const std::string& trackName = {});
bool previousClipBoundaryBefore(const ProjectDocument& project,
                                double seconds,
                                double& boundarySeconds,
                                const std::string& trackName = {});
bool trimClipStart(ProjectDocument& project, const std::string& clipId, double newStartSeconds);
bool trimClipEnd(ProjectDocument& project, const std::string& clipId, double newEndSeconds);
bool trimVideoClipStart(ProjectDocument& project, const std::string& clipId, double newStartSeconds);
bool trimVideoClipEnd(ProjectDocument& project, const std::string& clipId, double newEndSeconds);
bool deleteVideoClip(ProjectDocument& project, const std::string& clipId);
bool setVideoClipMuted(ProjectDocument& project, const std::string& clipId, bool muted);
bool setVideoClipLocked(ProjectDocument& project, const std::string& clipId, bool locked);
bool setVideoClipName(ProjectDocument& project, const std::string& clipId, const std::string& name);
bool relinkVideoSource(ProjectDocument& project,
                       const std::string& sourceId,
                       const std::string& sourcePath,
                       const std::string& displayName = {});
bool splitVideoClip(ProjectDocument& project, const std::string& clipId, double splitSeconds, std::string& newClipId);
bool splitClip(ProjectDocument& project, const std::string& clipId, double splitSeconds, std::string& newClipId);
bool glueAdjacentClip(ProjectDocument& project, const std::string& clipId, std::string& gluedClipId);
bool glueSelectedAdjacentClips(ProjectDocument& project,
                               const std::string& firstClipId,
                               const std::string& secondClipId,
                               std::string& gluedClipId);
bool glueClipRange(ProjectDocument& project,
                   double rangeStartSeconds,
                   double rangeEndSeconds,
                   std::vector<std::string>& gluedClipIds);
// Glue ONLY the clips in `clipIds` to each other (abutting, same-source). Neighbours outside the
// set are never absorbed — unlike glueClipRange, which catches any clip whose boundary lands in
// the range. This is what a selection-based Heal/Glue must use.
bool glueSelectedClips(ProjectDocument& project,
                       const std::vector<std::string>& clipIds,
                       std::vector<std::string>& gluedClipIds);
bool duplicateClip(ProjectDocument& project, const std::string& clipId, double newStartSeconds, std::string& newClipId);
bool duplicateClipToTrack(ProjectDocument& project,
                          const std::string& clipId,
                          double newStartSeconds,
                          const std::string& targetTrackName,
                          std::string& newClipId);
bool shuffleDuplicateClip(ProjectDocument& project,
                          const std::string& clipId,
                          double newStartSeconds,
                          const std::string& newTrackName,
                          std::string& newClipId);
bool pasteClip(ProjectDocument& project, const ClipState& sourceClip, double newStartSeconds, std::string& newClipId);
bool deleteClip(ProjectDocument& project, const std::string& clipId);
bool clearClipRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds);
// Clear a time range on ONE track only (split/trim its clips out of [start,end]) — a recorded punch
// overwrites the tape underneath it so the old and new clips do not double, analog-tape style.
bool clearTrackClipRange(ProjectDocument& project, const std::string& trackName,
                         double rangeStartSeconds, double rangeEndSeconds);
bool shuffleDeleteClipRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds);
// Shuffle-delete specific clips, rippling ONLY each clip's own track left (per-track ripple, the
// Pro Tools default) — not every track like the range version (which is "Ripple All").
bool shuffleDeleteClips(ProjectDocument& project, const std::vector<std::string>& clipIds);
bool separateClipRange(ProjectDocument& project,
                       double rangeStartSeconds,
                       double rangeEndSeconds,
                       std::vector<std::string>& newClipIds);
std::vector<ClipState> copyClipRange(const ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds);
bool cutClipRange(ProjectDocument& project,
                  double rangeStartSeconds,
                  double rangeEndSeconds,
                  std::vector<ClipState>& copiedClips);
bool pasteClipRange(ProjectDocument& project,
                    const std::vector<ClipState>& sourceClips,
                    double startSeconds,
                    std::vector<std::string>& newClipIds);
bool duplicateClipRange(ProjectDocument& project,
                        double rangeStartSeconds,
                        double rangeEndSeconds,
                        std::vector<std::string>& newClipIds);
bool trimClipRangeToSelection(ProjectDocument& project,
                              double rangeStartSeconds,
                              double rangeEndSeconds,
                              std::vector<std::string>& keptClipIds);
bool quantizeClipStartsInRange(ProjectDocument& project,
                               double rangeStartSeconds,
                               double rangeEndSeconds,
                               double quantumSeconds,
                               std::vector<std::string>& changedClipIds);
bool setEditSelectionRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds);
bool projectClipTimeRange(const ProjectDocument& project, double& rangeStartSeconds, double& rangeEndSeconds);
bool setEditSelectionToAdjacentClipBoundary(ProjectDocument& project,
                                            double seconds,
                                            bool forward,
                                            double& boundarySeconds,
                                            const std::string& trackName = {});
bool setEditSelectionToSurroundingClipBoundaries(ProjectDocument& project,
                                                 double seconds,
                                                 double& rangeStartSeconds,
                                                 double& rangeEndSeconds,
                                                 const std::string& trackName = {});
bool setEditSelectionToSurroundingMarkers(ProjectDocument& project,
                                          double seconds,
                                          double& rangeStartSeconds,
                                          double& rangeEndSeconds);
bool setEditSelectionToClip(ProjectDocument& project, const std::string& clipId);
bool editSelectionMatchesClip(const ProjectDocument& project, const std::string& clipId, double epsilonSeconds = 0.001);
bool toggleEditSelectionToClip(ProjectDocument& project, const std::string& clipId, bool clearMatchingLoop = true);
bool clearEditSelection(ProjectDocument& project);
bool setLoopRange(ProjectDocument& project, double rangeStartSeconds, double rangeEndSeconds);
bool setLoopToEditSelection(ProjectDocument& project);
bool setLoopToClip(ProjectDocument& project, const std::string& clipId);
bool clearLoopRange(ProjectDocument& project);
bool setClipGainDb(ProjectDocument& project, const std::string& clipId, float gainDb);
bool adjustClipGainInRange(ProjectDocument& project,
                           double rangeStartSeconds,
                           double rangeEndSeconds,
                           float deltaDb,
                           std::vector<std::string>& changedClipIds);
bool setClipRegionName(ProjectDocument& project, const std::string& clipId, const std::string& regionName);
bool setClipFades(ProjectDocument& project, const std::string& clipId, double fadeInSeconds, double fadeOutSeconds);
bool setClipFadeCurvature(ProjectDocument& project,
                          const std::string& clipId,
                          double fadeInCurvature,
                          double fadeOutCurvature);
bool setClipFadeCurves(ProjectDocument& project,
                       const std::string& clipId,
                       const std::string& fadeInCurve,
                       const std::string& fadeOutCurve);
bool applyAutomaticClipCrossfades(ProjectDocument& project, const std::string& clipId);
bool applyFadeOrCrossfadeToClipRange(ProjectDocument& project,
                                     double rangeStartSeconds,
                                     double rangeEndSeconds,
                                     std::vector<std::string>& changedClipIds);
bool setClipColor(ProjectDocument& project, const std::string& clipId, const std::string& colorHex);
bool setClipMuted(ProjectDocument& project, const std::string& clipId, bool muted);
bool setClipReversed(ProjectDocument& project, const std::string& clipId, bool reversed);
bool toggleClipMuted(ProjectDocument& project, const std::string& clipId);
bool toggleClipReversed(ProjectDocument& project, const std::string& clipId);
bool toggleClipPolarityInverted(ProjectDocument& project, const std::string& clipId);
bool setClipMutedInRange(ProjectDocument& project,
                         double rangeStartSeconds,
                         double rangeEndSeconds,
                         bool muted,
                         std::vector<std::string>& changedClipIds);
bool setClipPolarityInverted(ProjectDocument& project, const std::string& clipId, bool inverted);
bool setClipPolarityInvertedInRange(ProjectDocument& project,
                                    double rangeStartSeconds,
                                    double rangeEndSeconds,
                                    bool inverted,
                                    std::vector<std::string>& changedClipIds);
bool setClipLocked(ProjectDocument& project, const std::string& clipId, bool locked);
bool setClipTrack(ProjectDocument& project, const std::string& clipId, const std::string& trackName);
bool slipClipSourceOffset(ProjectDocument& project,
                          const std::string& clipId,
                          double deltaSeconds,
                          std::string& message);
bool normalizeClipGainToPeak(ProjectDocument& project,
                             const std::string& clipId,
                             float targetPeakDb,
                             std::string& message);
bool normalizeClipGainInRange(ProjectDocument& project,
                              double rangeStartSeconds,
                              double rangeEndSeconds,
                              float targetPeakDb,
                              std::vector<std::string>& changedClipIds,
                              std::string& message);
bool applyClipTimeScaleToProjectTempo(ProjectDocument& project,
                                      const std::string& clipId,
                                      std::string& message);
bool applyPendingClipTimeScaleToProjectTempo(ProjectDocument& project,
                                             ClipTimeScaleApplyResult& result);
bool setClipSourcePath(ProjectDocument& project,
                       const std::string& clipId,
                       const std::string& sourcePath,
                       double durationSeconds);
bool setTrackRecordArmed(ProjectDocument& project, const std::string& trackName, bool armed);
bool setTrackInputMonitoring(ProjectDocument& project, const std::string& trackName, bool monitoring);
bool setTrackMuted(ProjectDocument& project, const std::string& trackName, bool muted);
bool setTrackSolo(ProjectDocument& project, const std::string& trackName, bool solo);
bool setTrackVolumeDb(ProjectDocument& project, const std::string& trackName, float volumeDb);
bool setTrackPan(ProjectDocument& project, const std::string& trackName, float pan);
bool setTrackColor(ProjectDocument& project, const std::string& trackName, const std::string& colorHex);
bool setTrackControlMaster(ProjectDocument& project, const std::string& trackName, const std::string& controlMasterTrackName);
bool addTrackInsertSlot(ProjectDocument& project, const std::string& trackName);
bool setTrackInsertSlot(ProjectDocument& project,
                        const std::string& trackName,
                        size_t insertIndex,
                        const TrackInsertSlot& insert);
bool toggleTrackInsertBypass(ProjectDocument& project, const std::string& trackName, size_t insertIndex);
bool removeTrackInsertSlot(ProjectDocument& project, const std::string& trackName, size_t insertIndex);
bool setTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName, const InstrumentSlotState& instrument);
bool setTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName, size_t slotIndex, const InstrumentSlotState& instrument);
bool toggleTrackInstrumentBypass(ProjectDocument& project, const std::string& trackName);
bool toggleTrackInstrumentBypass(ProjectDocument& project, const std::string& trackName, size_t slotIndex);
/// Per-layer solo: when any layer in the rack is soloed, only soloed layers sound.
bool toggleTrackInstrumentSlotSolo(ProjectDocument& project, const std::string& trackName, size_t slotIndex);
bool setTrackInstrumentMidiChannel(ProjectDocument& project, const std::string& trackName, int midiChannel);
bool setTrackInstrumentMidiChannel(ProjectDocument& project, const std::string& trackName, size_t slotIndex, int midiChannel);
bool clearTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName);
bool clearTrackInstrumentSlot(ProjectDocument& project, const std::string& trackName, size_t slotIndex);
int moveTrackInsertSlot(ProjectDocument& project, const std::string& trackName, size_t insertIndex, int direction);
int moveTrackInsertSlotToIndex(ProjectDocument& project,
                               const std::string& trackName,
                               size_t insertIndex,
                               size_t targetIndex);
bool addTrackSendSlot(ProjectDocument& project, const std::string& trackName, const TrackSendState& send);
bool setTrackSendSlot(ProjectDocument& project,
                      const std::string& trackName,
                      size_t sendIndex,
                      const TrackSendState& send);
bool setTrackSendEnabled(ProjectDocument& project, const std::string& trackName, size_t sendIndex, bool enabled);
bool toggleTrackSendPreFader(ProjectDocument& project, const std::string& trackName, size_t sendIndex);
bool toggleTrackSendStereo(ProjectDocument& project, const std::string& trackName, size_t sendIndex);
bool removeTrackSendSlot(ProjectDocument& project, const std::string& trackName, size_t sendIndex);
int moveTrackSendSlot(ProjectDocument& project, const std::string& trackName, size_t sendIndex, int direction);
int moveTrackSendSlotToIndex(ProjectDocument& project,
                             const std::string& trackName,
                             size_t sendIndex,
                             size_t targetIndex);
bool setTrackVolumeAutomationPoint(ProjectDocument& project, const std::string& trackName, double timeSeconds, float volumeDb);
bool moveTrackVolumeAutomationPoint(ProjectDocument& project, const std::string& trackName, size_t pointIndex, double timeSeconds, float volumeDb);
bool deleteTrackVolumeAutomationPoint(ProjectDocument& project, const std::string& trackName, size_t pointIndex);
size_t deleteTrackVolumeAutomationPointsInRange(ProjectDocument& project, const std::string& trackName, double rangeStartSeconds, double rangeEndSeconds);
bool setTrackAutomationLanePoint(ProjectDocument& project,
                                 const std::string& trackName,
                                 const std::string& parameterId,
                                 const std::string& displayName,
                                 double timeSeconds,
                                 float value);
bool moveTrackAutomationLanePoint(ProjectDocument& project,
                                  const std::string& trackName,
                                  const std::string& parameterId,
                                  size_t pointIndex,
                                  double timeSeconds,
                                  float value);
bool deleteTrackAutomationLanePoint(ProjectDocument& project,
                                    const std::string& trackName,
                                    const std::string& parameterId,
                                    size_t pointIndex);
size_t deleteTrackAutomationLanePointsInRange(ProjectDocument& project,
                                             const std::string& trackName,
                                             const std::string& parameterId,
                                             double rangeStartSeconds,
                                             double rangeEndSeconds);
bool normalizeMonitorStationProjectState(ProjectDocument& project);
bool renameTrack(ProjectDocument& project, const std::string& oldName, const std::string& newName);
bool duplicateTrackWithClips(ProjectDocument& project,
                             const std::string& sourceTrackName,
                             std::string& newTrackName,
                             std::vector<std::string>& newClipIds);
std::string recordingTargetTrackName(const ProjectDocument& project);
int inputChannelCountForBusName(const std::string& inputBusName);
int recordingInputChannelCount(const ProjectDocument& project);
std::string appendAudioClip(ProjectDocument& project,
                            const std::string& trackName,
                            const std::string& sourcePath,
                            double durationSeconds);
std::string appendAudioClipAt(ProjectDocument& project,
                              const std::string& trackName,
                              const std::string& sourcePath,
                              double startSeconds,
                              double durationSeconds);
std::string appendVideoReferenceClip(ProjectDocument& project,
                                     const std::string& sourcePath,
                                     double startSeconds,
                                     double durationSeconds,
                                     double frameRate = 30.0,
                                     int width = 0,
                                     int height = 0,
                                     bool hasAudio = false);
bool appendRecordedTakeClip(ProjectDocument& project,
                            const std::string& trackName,
                            const std::string& sourcePath,
                            double startSeconds,
                            double durationSeconds,
                            std::string& newClipId,
                            std::string& message);
bool appendRecordedMidiTakeRegion(ProjectDocument& project,
                                  const std::string& trackName,
                                  const std::vector<RecordedMidiEvent>& events,
                                  double startSeconds,
                                  double durationSeconds,
                                  std::string& newRegionId,
                                  std::string& message,
                                  const std::string& mode = "new-region",
                                  double punchStartSeconds = -1.0,
                                  double punchEndSeconds = -1.0);
std::string addAudioTrack(ProjectDocument& project);
std::string addInstrumentTrack(ProjectDocument& project);
std::string addFolderTrack(ProjectDocument& project);
std::string addBusFolderTrack(ProjectDocument& project, const std::string& inputBusName);
std::string addVcaTrack(ProjectDocument& project);
bool setTrackFolderCollapsed(ProjectDocument& project, const std::string& trackName, bool collapsed);
int moveTrack(ProjectDocument& project, const std::string& trackName, int direction);
bool moveTrackNearTrack(ProjectDocument& project,
                        const std::string& trackName,
                        const std::string& targetTrackName,
                        bool insertAfterTarget);
bool moveTrackIntoFolder(ProjectDocument& project, const std::string& trackName, const std::string& folderName);
size_t trackTimelineItemCount(const ProjectDocument& project, const std::string& trackName);
size_t folderChildTrackCount(const ProjectDocument& project, const std::string& folderName);
bool deleteTrack(ProjectDocument& project,
                 const std::string& trackName,
                 bool removeTimelineItems,
                 bool deleteFolderChildren);
bool deleteTrackIfEmpty(ProjectDocument& project, const std::string& trackName);
std::string createTrackPlaylist(ProjectDocument& project, const std::string& trackName, const std::string& playlistName = {});
std::string duplicateActiveTrackPlaylist(ProjectDocument& project, const std::string& trackName, const std::string& playlistName = {});
bool activateTrackPlaylist(ProjectDocument& project, const std::string& playlistId);
bool renameTrackPlaylist(ProjectDocument& project, const std::string& playlistId, const std::string& playlistName);
bool deleteTrackPlaylist(ProjectDocument& project, const std::string& playlistId);
bool copyPlaylistPlacementToActivePlaylist(ProjectDocument& project, const std::string& playlistId, const std::string& placementId);
std::string addMarkerAt(ProjectDocument& project, double timeSeconds);
bool renameNearestMarker(ProjectDocument& project, double timeSeconds, double toleranceSeconds, const std::string& name);
bool moveNearestMarker(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds);
bool deleteNearestMarker(ProjectDocument& project, double timeSeconds, double toleranceSeconds);
std::string addChordEventAt(ProjectDocument& project, double timeSeconds, const std::string& name);
bool renameNearestChordEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds, const std::string& name);
bool moveNearestChordEvent(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds);
bool deleteNearestChordEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds);
struct LyricTranscriptionSegment {
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    std::string text;
};

struct LyricTranscriptionApplyResult {
    bool ok = false;
    size_t addedEvents = 0;
    size_t removedEvents = 0;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    std::string message;
};

std::string addLyricEventAt(ProjectDocument& project, double timeSeconds, const std::string& text);
LyricTranscriptionApplyResult applyLyricTranscription(ProjectDocument& project,
                                                      const std::vector<LyricTranscriptionSegment>& segments,
                                                      double projectTimeOffsetSeconds = 0.0,
                                                      bool replaceOverlappingEvents = true);
bool renameNearestLyricEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds, const std::string& text);
bool moveNearestLyricEvent(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds);
bool deleteNearestLyricEvent(ProjectDocument& project, double timeSeconds, double toleranceSeconds);
std::string addMidiTrack(ProjectDocument& project);
std::string addMidiRegion(ProjectDocument& project,
                          const std::string& trackName,
                          double startSeconds,
                          double durationSeconds,
                          const std::string& name = "MIDI Region");
bool moveMidiRegion(ProjectDocument& project,
                    const std::string& regionId,
                    const std::string& trackName,
                    double startSeconds);
bool resizeMidiRegion(ProjectDocument& project,
                      const std::string& regionId,
                      double durationSeconds);
bool trimMidiRegionStart(ProjectDocument& project,
                         const std::string& regionId,
                         double newStartSeconds);
bool splitMidiRegion(ProjectDocument& project,
                     const std::string& regionId,
                     double splitSeconds,
                     std::string& newRegionId);
bool duplicateMidiRegion(ProjectDocument& project,
                         const std::string& regionId,
                         double newStartSeconds,
                         std::string& newRegionId);
bool setMidiRegionName(ProjectDocument& project,
                       const std::string& regionId,
                       const std::string& name);
bool setMidiRegionColor(ProjectDocument& project,
                        const std::string& regionId,
                        const std::string& colorHex);
bool setMidiRegionMuted(ProjectDocument& project,
                        const std::string& regionId,
                        bool muted);
bool setMidiRegionLocked(ProjectDocument& project,
                         const std::string& regionId,
                         bool locked);
bool setMidiRegionLoopEnabled(ProjectDocument& project,
                              const std::string& regionId,
                              bool loopEnabled);
bool transposeMidiRegion(ProjectDocument& project,
                         const std::string& regionId,
                         int semitones,
                         std::vector<std::string>& changedNoteIds);
bool humanizeMidiRegion(ProjectDocument& project,
                        const std::string& regionId,
                        double maxTimingBeats,
                        int maxVelocityDelta,
                        unsigned int seed,
                        std::vector<std::string>& changedNoteIds);
bool deleteMidiRegion(ProjectDocument& project, const std::string& regionId);
std::string addMidiNote(ProjectDocument& project,
                        const std::string& regionId,
                        int pitch,
                        double startBeats,
                        double durationBeats,
                        int velocity,
                        int channel = 1);
std::string addMidiControllerEvent(ProjectDocument& project,
                                   const std::string& regionId,
                                   double beat,
                                   int controller,
                                   int value,
                                   int channel = 1);
std::string addMidiSustainEvent(ProjectDocument& project,
                                const std::string& regionId,
                                double beat,
                                bool down,
                                int channel = 1);
std::string addMidiPitchBendEvent(ProjectDocument& project,
                                  const std::string& regionId,
                                  double beat,
                                  int value,
                                  int channel = 1);
std::string addMidiProgramChangeEvent(ProjectDocument& project,
                                      const std::string& regionId,
                                      double beat,
                                      int program,
                                      int channel = 1);
bool deleteMidiControllerEvent(ProjectDocument& project,
                               const std::string& regionId,
                               const std::string& eventId);
bool deleteMidiPitchBendEvent(ProjectDocument& project,
                              const std::string& regionId,
                              const std::string& eventId);
bool deleteMidiProgramChangeEvent(ProjectDocument& project,
                                  const std::string& regionId,
                                  const std::string& eventId);
bool moveMidiControllerEvent(ProjectDocument& project,
                             const std::string& regionId,
                             const std::string& eventId,
                             double beat,
                             int value);
bool moveMidiPitchBendEvent(ProjectDocument& project,
                            const std::string& regionId,
                            const std::string& eventId,
                            double beat,
                            int value);
bool moveMidiProgramChangeEvent(ProjectDocument& project,
                                const std::string& regionId,
                                const std::string& eventId,
                                double beat,
                                int program);
bool moveMidiNote(ProjectDocument& project,
                  const std::string& regionId,
                  const std::string& noteId,
                  int pitch,
                  double startBeats);
bool moveMidiNotes(ProjectDocument& project,
                   const std::string& regionId,
                   const std::vector<std::string>& noteIds,
                   int pitchDelta,
                   double startBeatDelta,
                   std::vector<std::string>& changedNoteIds);
bool resizeMidiNote(ProjectDocument& project,
                    const std::string& regionId,
                    const std::string& noteId,
                    double durationBeats);
bool resizeMidiNotes(ProjectDocument& project,
                     const std::string& regionId,
                     const std::vector<std::string>& noteIds,
                     double durationBeatDelta,
                     std::vector<std::string>& changedNoteIds);
bool setMidiNoteVelocity(ProjectDocument& project,
                         const std::string& regionId,
                         const std::string& noteId,
                         int velocity);
bool adjustMidiNoteVelocities(ProjectDocument& project,
                              const std::string& regionId,
                              const std::vector<std::string>& noteIds,
                              int velocityDelta,
                              std::vector<std::string>& changedNoteIds);
bool setMidiNoteMuted(ProjectDocument& project,
                      const std::string& regionId,
                      const std::string& noteId,
                      bool muted);
bool deleteMidiNote(ProjectDocument& project,
                    const std::string& regionId,
                    const std::string& noteId);
bool deleteMidiNotes(ProjectDocument& project,
                     const std::string& regionId,
                     const std::vector<std::string>& noteIds,
                     std::vector<std::string>& deletedNoteIds);
bool quantizeMidiRegion(ProjectDocument& project,
                        const std::string& regionId,
                        double beatQuantum,
                        std::vector<std::string>& changedNoteIds);
bool addTempoMarkerAt(ProjectDocument& project, double timeSeconds, double bpm);
bool setNearestTempoMarkerBpm(ProjectDocument& project, double timeSeconds, double toleranceSeconds, double bpm);
bool moveNearestTempoMarker(ProjectDocument& project, double originalTimeSeconds, double toleranceSeconds, double newTimeSeconds, double bpm);
bool deleteNearestTempoMarker(ProjectDocument& project, double timeSeconds, double toleranceSeconds);
bool addMasterVst3Insert(ProjectDocument& project, const InsertState& insert);
bool addMasterInsertSlot(ProjectDocument& project);
bool toggleMasterVst3InsertBypass(ProjectDocument& project, size_t insertIndex);
bool removeMasterVst3Insert(ProjectDocument& project, size_t insertIndex);
size_t clearMasterVst3Inserts(ProjectDocument& project);
int moveMasterInsert(ProjectDocument& project, size_t insertIndex, int direction);

} // namespace neuracoust::daw
