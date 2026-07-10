#include "project/ProjectMediaPool.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace neuracoust::daw {

namespace {

bool fileMissing(const std::string& path) {
    if (path.empty()) {
        return true;
    }
    std::error_code error;
    return !std::filesystem::exists(std::filesystem::path(path), error);
}

std::string fileNameForPath(const std::string& path) {
    const std::filesystem::path fsPath(path);
    const auto name = fsPath.filename().string();
    return name.empty() ? path : name;
}

} // namespace

MediaPoolSummary buildProjectMediaPoolSummary(ProjectDocument project) {
    normalizeProjectEditModel(project);
    MediaPoolSummary summary;
    summary.sources.reserve(project.mediaSources.size());
    summary.regions.reserve(project.clipDefinitions.size());

    std::map<std::string, MediaPoolSourceSummary*> sourceById;
    for (const auto& source : project.mediaSources) {
        MediaPoolSourceSummary row;
        row.sourceId = source.id;
        row.path = source.path;
        row.displayName = source.displayName.empty() ? fileNameForPath(source.path) : source.displayName;
        row.channels = source.channels;
        row.sampleRate = source.sampleRate;
        row.bitsPerSample = source.bitsPerSample;
        row.floatingPoint = source.floatingPoint;
        row.missing = fileMissing(source.path);
        summary.sources.push_back(row);
        sourceById[summary.sources.back().sourceId] = &summary.sources.back();
    }

    std::map<std::string, MediaPoolRegionSummary*> regionById;
    for (const auto& definition : project.clipDefinitions) {
        MediaPoolRegionSummary row;
        row.definitionId = definition.id;
        row.sourceId = definition.sourceId;
        row.name = definition.name;
        row.sourceOffsetSeconds = definition.sourceOffsetSeconds;
        row.durationSeconds = definition.durationSeconds;
        auto sourceIt = sourceById.find(definition.sourceId);
        if (sourceIt != sourceById.end()) {
            row.sourcePath = sourceIt->second->path;
            row.missing = sourceIt->second->missing;
            sourceIt->second->regionCount += 1;
        } else {
            row.missing = true;
        }
        summary.regions.push_back(row);
        regionById[summary.regions.back().definitionId] = &summary.regions.back();
    }

    for (const auto& playlist : project.trackPlaylists) {
        if (!playlist.active) {
            continue;
        }
        for (const auto& placement : playlist.placements) {
            auto regionIt = regionById.find(placement.clipDefinitionId);
            MediaPoolUseSummary use;
            use.placementId = placement.id;
            use.legacyClipId = placement.legacyClipId;
            use.definitionId = placement.clipDefinitionId;
            use.trackName = playlist.trackName;
            use.playlistName = playlist.name;
            use.startSeconds = placement.startSeconds;
            if (regionIt != regionById.end()) {
                auto& region = *regionIt->second;
                use.sourceId = region.sourceId;
                use.sourcePath = region.sourcePath;
                use.regionName = region.name;
                use.durationSeconds = region.durationSeconds;
                use.missing = region.missing;
                region.useCount += 1;
                const std::string clipId = placement.legacyClipId.empty() ? placement.id : placement.legacyClipId;
                if (!clipId.empty()) {
                    region.clipIds.push_back(clipId);
                }
                auto sourceIt = sourceById.find(region.sourceId);
                if (sourceIt != sourceById.end()) {
                    sourceIt->second->useCount += 1;
                    if (!clipId.empty()) {
                        sourceIt->second->clipIds.push_back(clipId);
                    }
                }
            } else {
                use.regionName = placement.clipDefinitionId;
                use.missing = true;
            }
            summary.uses.push_back(use);
        }
    }

    for (auto& region : summary.regions) {
        region.unused = region.useCount == 0;
    }
    for (auto& source : summary.sources) {
        source.unused = source.useCount == 0;
        if (source.missing) {
            summary.missingSources += 1;
        }
        if (source.unused) {
            summary.unusedSources += 1;
        }
    }
    summary.missingUses = static_cast<size_t>(std::count_if(summary.uses.begin(), summary.uses.end(), [](const MediaPoolUseSummary& use) {
        return use.missing;
    }));

    std::sort(summary.sources.begin(), summary.sources.end(), [](const auto& left, const auto& right) {
        if (left.missing != right.missing) {
            return left.missing > right.missing;
        }
        if (left.unused != right.unused) {
            return left.unused > right.unused;
        }
        return left.displayName < right.displayName;
    });
    std::sort(summary.regions.begin(), summary.regions.end(), [](const auto& left, const auto& right) {
        if (left.missing != right.missing) {
            return left.missing > right.missing;
        }
        if (left.unused != right.unused) {
            return left.unused > right.unused;
        }
        return left.name < right.name;
    });
    std::sort(summary.uses.begin(), summary.uses.end(), [](const auto& left, const auto& right) {
        if (left.trackName != right.trackName) {
            return left.trackName < right.trackName;
        }
        if (left.startSeconds == right.startSeconds) {
            return left.regionName < right.regionName;
        }
        return left.startSeconds < right.startSeconds;
    });
    return summary;
}

size_t removeMediaSourceFromProject(ProjectDocument& project, const std::string& sourceId) {
    if (sourceId.empty()) {
        return 0;
    }
    normalizeProjectEditModel(project);
    std::set<std::string> removedDefinitionIds;
    project.clipDefinitions.erase(
        std::remove_if(project.clipDefinitions.begin(), project.clipDefinitions.end(), [&](const ClipDefinitionState& definition) {
            const bool remove = definition.sourceId == sourceId;
            if (remove) {
                removedDefinitionIds.insert(definition.id);
            }
            return remove;
        }),
        project.clipDefinitions.end());
    if (removedDefinitionIds.empty()) {
        return 0;
    }
    size_t removedActivePlacements = 0;
    for (auto& playlist : project.trackPlaylists) {
        const auto before = playlist.placements.size();
        playlist.placements.erase(
            std::remove_if(playlist.placements.begin(), playlist.placements.end(), [&](const PlaylistClipPlacementState& placement) {
                return removedDefinitionIds.find(placement.clipDefinitionId) != removedDefinitionIds.end();
            }),
            playlist.placements.end());
        if (playlist.active) {
            removedActivePlacements += before - playlist.placements.size();
        }
    }
    project.mediaSources.erase(
        std::remove_if(project.mediaSources.begin(), project.mediaSources.end(), [&](const MediaSourceState& source) {
            return source.id == sourceId;
        }),
        project.mediaSources.end());
    rebuildProjectClipsFromActivePlaylists(project);
    std::set<std::string> remainingSourceIds;
    std::set<std::string> remainingSourcePaths;
    for (const auto& source : project.mediaSources) {
        if (!source.id.empty()) {
            remainingSourceIds.insert(source.id);
        }
        if (!source.path.empty()) {
            remainingSourcePaths.insert(source.path);
        }
    }
    project.clips.erase(
        std::remove_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
            if (!clip.sourceFileUid.empty() && remainingSourceIds.find(clip.sourceFileUid) == remainingSourceIds.end()) {
                return true;
            }
            return !clip.sourcePath.empty() && remainingSourcePaths.find(clip.sourcePath) == remainingSourcePaths.end();
        }),
        project.clips.end());
    return removedActivePlacements;
}

size_t relinkMediaSource(ProjectDocument& project, const std::string& sourceId, const std::string& newPath) {
    if (sourceId.empty() || newPath.empty()) {
        return 0;
    }
    normalizeProjectEditModel(project);
    size_t changed = 0;
    for (auto& source : project.mediaSources) {
        if (source.id == sourceId) {
            source.path = newPath;
            source.displayName = fileNameForPath(newPath);
            changed = 1;
            break;
        }
    }
    if (changed == 0) {
        return 0;
    }
    rebuildProjectClipsFromActivePlaylists(project);
    return changed;
}

size_t deleteUnusedMediaSources(ProjectDocument& project) {
    auto summary = buildProjectMediaPoolSummary(project);
    std::set<std::string> unusedSourceIds;
    for (const auto& source : summary.sources) {
        if (source.unused) {
            unusedSourceIds.insert(source.sourceId);
        }
    }
    if (unusedSourceIds.empty()) {
        return 0;
    }
    const auto before = project.mediaSources.size();
    project.mediaSources.erase(
        std::remove_if(project.mediaSources.begin(), project.mediaSources.end(), [&](const MediaSourceState& source) {
            return unusedSourceIds.find(source.id) != unusedSourceIds.end();
        }),
        project.mediaSources.end());
    return before - project.mediaSources.size();
}

} // namespace neuracoust::daw
