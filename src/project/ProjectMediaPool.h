#pragma once

#include "project/ProjectDocument.h"
#include <string>
#include <vector>

namespace neuracoust::daw {

struct MediaPoolSourceSummary {
    std::string sourceId;
    std::string path;
    std::string displayName;
    int channels = 0;
    double sampleRate = 0.0;
    int bitsPerSample = 0;
    bool floatingPoint = false;
    bool missing = false;
    bool unused = false;
    size_t regionCount = 0;
    size_t useCount = 0;
    std::vector<std::string> clipIds;
};

struct MediaPoolRegionSummary {
    std::string definitionId;
    std::string sourceId;
    std::string sourcePath;
    std::string name;
    double sourceOffsetSeconds = 0.0;
    double durationSeconds = 0.0;
    bool missing = false;
    bool unused = false;
    size_t useCount = 0;
    std::vector<std::string> clipIds;
};

struct MediaPoolUseSummary {
    std::string placementId;
    std::string legacyClipId;
    std::string definitionId;
    std::string sourceId;
    std::string sourcePath;
    std::string regionName;
    std::string trackName;
    std::string playlistName;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    bool missing = false;
};

struct MediaPoolSummary {
    std::vector<MediaPoolSourceSummary> sources;
    std::vector<MediaPoolRegionSummary> regions;
    std::vector<MediaPoolUseSummary> uses;
    size_t missingSources = 0;
    size_t unusedSources = 0;
    size_t missingUses = 0;
};

MediaPoolSummary buildProjectMediaPoolSummary(ProjectDocument project);
size_t removeMediaSourceFromProject(ProjectDocument& project, const std::string& sourceId);
size_t relinkMediaSource(ProjectDocument& project, const std::string& sourceId, const std::string& newPath);
size_t deleteUnusedMediaSources(ProjectDocument& project);

} // namespace neuracoust::daw
