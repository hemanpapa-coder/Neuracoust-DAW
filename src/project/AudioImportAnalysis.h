#pragma once

#include "audio/WavFile.h"
#include "project/ProjectDocument.h"
#include <functional>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct ProjectMusicReanalysisReport {
    size_t analyzedClips = 0;
    size_t skippedClips = 0;
    size_t reusedSourceFiles = 0;
    std::vector<std::string> messages;
    std::string summary;
};

std::string analyzeImportedAudioIntoProject(ProjectDocument& project,
                                            const WavAudioData& data,
                                            double clipStartSeconds,
                                            double clipDurationSeconds,
                                            bool hasEmbeddedTempo);
std::string reanalyzeClipMusicalMetadata(ProjectDocument& project,
                                         const std::string& clipId,
                                         const WavAudioData& sourceData,
                                         std::string& error);
ProjectMusicReanalysisReport reanalyzeProjectMusicalMetadata(ProjectDocument& project,
                                                             const std::function<bool(const std::string& path,
                                                                                      WavAudioData& data,
                                                                                      std::string& error)>& loadSourceAudio);

} // namespace neuracoust::daw
