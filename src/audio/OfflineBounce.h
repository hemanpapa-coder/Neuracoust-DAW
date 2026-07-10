#pragma once

#include "audio/ProjectAudioRenderer.h"
#include "audio/ProjectRenderTypes.h"
#include "project/ProjectDocument.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct AudioLevelStats {
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    float rmsLeft = 0.0f;
    float rmsRight = 0.0f;
    uint64_t clippedSampleCount = 0;
    bool clippingDetected = false;
    bool nearSilent = false;
};

struct BounceResult {
    bool ok = false;
    double durationSeconds = 0.0;
    AudioLevelStats levelStats;
    std::vector<std::string> missingMediaClipIds;
    std::string manifestPath;
    std::string message;
};

enum class BounceRenderMode {
    Offline,
    Realtime
};

enum class BounceRangeMode {
    FullProject,
    EditSelection
};

struct BounceOptions {
    BounceRenderMode renderMode = BounceRenderMode::Offline;
    BounceRangeMode rangeMode = BounceRangeMode::FullProject;
    bool ditherEnabled = false;
    int sourceBitDepth = 0;
    std::vector<ProjectExternalSidechainBus> externalSidechainBuses;
    bool externalSidechainDuckingEnabled = true;
    bool peakCeilingGuardEnabled = false;
    float peakCeilingDbfs = -1.0f;
};

struct StemExportResult {
    bool ok = false;
    size_t exportedStems = 0;
    double durationSeconds = 0.0;
    std::vector<AudioLevelStats> stemLevelStats;
    std::vector<std::string> missingMediaClipIds;
    std::vector<std::string> outputPaths;
    std::string manifestPath;
    std::string message;
};

BounceResult bounceProjectToWav(const ProjectDocument& project, const std::string& outputPath);
BounceResult bounceProjectToWav(const ProjectDocument& project, const std::string& outputPath, const BounceOptions& options);
StemExportResult exportProjectTrackStems(const ProjectDocument& project, const std::filesystem::path& outputDirectory);

} // namespace neuracoust::daw
