#pragma once

#include "project/ProjectDocument.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct EdlExportResult {
    bool ok = false;
    std::string text;
    size_t eventCount = 0;
    size_t tempoEventCount = 0;
    size_t markerCount = 0;
    size_t chordEventCount = 0;
    size_t lyricEventCount = 0;
    std::string message;
};

struct FcpxmlExportResult {
    bool ok = false;
    std::string text;
    size_t clipCount = 0;
    size_t assetCount = 0;
    size_t tempoEventCount = 0;
    size_t markerCount = 0;
    size_t chordEventCount = 0;
    size_t lyricEventCount = 0;
    std::string message;
};

struct InterchangeReferenceExportResult {
    bool ok = false;
    std::string text;
    size_t clipCount = 0;
    size_t trackCount = 0;
    size_t chordEventCount = 0;
    size_t lyricEventCount = 0;
    std::string message;
};

struct InterchangeReferenceImportResult {
    bool ok = false;
    ProjectDocument project;
    size_t clipCount = 0;
    size_t trackCount = 0;
    size_t chordEventCount = 0;
    size_t lyricEventCount = 0;
    std::string message;
};

enum class TimelineInterchangeProfile {
    FinalCutPro,
    DaVinciResolve
};

const char* timelineInterchangeProfileName(TimelineInterchangeProfile profile);

enum class VideoDeliveryPreset {
    YouTube1080p,
    YouTube4k,
    SharePreview720p
};

struct VideoDeliveryClipPlan {
    std::string clipId;
    std::string sourcePath;
    double timelineStartSeconds = 0.0;
    double durationSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
};

struct VideoDeliveryPlan {
    bool ok = false;
    std::string presetId;
    std::string presetName;
    std::string container;
    std::string videoCodec;
    std::string audioCodec;
    int width = 0;
    int height = 0;
    int videoBitrateKbps = 0;
    int audioBitrateKbps = 0;
    double frameRate = 30.0;
    double durationSeconds = 0.0;
    std::vector<VideoDeliveryClipPlan> clips;
    std::string ffmpegCommand;
    std::string message;
};

struct MidiExportResult {
    bool ok = false;
    std::vector<uint8_t> data;
    size_t tempoEventCount = 0;
    size_t markerCount = 0;
    size_t chordEventCount = 0;
    size_t lyricEventCount = 0;
    size_t regionCueCount = 0;
    size_t midiTrackCount = 0;
    size_t midiNoteCount = 0;
    size_t midiControllerEventCount = 0;
    size_t midiPitchBendEventCount = 0;
    size_t midiProgramChangeEventCount = 0;
    std::string outputPath;
    std::string message;
};

struct MidiImportResult {
    bool ok = false;
    ProjectDocument project;
    size_t trackCount = 0;
    size_t regionCount = 0;
    size_t noteCount = 0;
    size_t controllerEventCount = 0;
    size_t pitchBendEventCount = 0;
    size_t programChangeEventCount = 0;
    int ticksPerQuarter = 480;
    std::string message;
};

EdlExportResult exportProjectToCmx3600Edl(const ProjectDocument& project,
                                          double frameRate = 30.0,
                                          TimelineInterchangeProfile profile = TimelineInterchangeProfile::FinalCutPro);
FcpxmlExportResult exportProjectToFcpxml(const ProjectDocument& project,
                                         TimelineInterchangeProfile profile = TimelineInterchangeProfile::FinalCutPro);
InterchangeReferenceExportResult exportProjectToAafReference(const ProjectDocument& project);
InterchangeReferenceExportResult exportProjectToOmfReference(const ProjectDocument& project);
VideoDeliveryPlan makeVideoDeliveryPlan(const ProjectDocument& project,
                                        VideoDeliveryPreset preset,
                                        const std::filesystem::path& outputPath,
                                        const std::filesystem::path& mixedAudioPath = {});
InterchangeReferenceImportResult importProjectFromCmx3600EdlText(const std::string& text, double frameRate = 30.0);
InterchangeReferenceImportResult importProjectFromFcpxmlText(const std::string& text);
InterchangeReferenceImportResult importProjectFromAafReferenceText(const std::string& text);
InterchangeReferenceImportResult importProjectFromOmfReferenceText(const std::string& text);
MidiExportResult exportProjectTempoMapToMidi(const ProjectDocument& project, int ticksPerQuarter = 480);
MidiExportResult writeProjectMidiFile(const ProjectDocument& project,
                                      const std::filesystem::path& outputPath,
                                      int ticksPerQuarter = 480);
MidiImportResult importProjectFromMidiData(const std::vector<uint8_t>& data);
MidiImportResult readProjectMidiFile(const std::filesystem::path& inputPath);

} // namespace neuracoust::daw
