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

// Detects tempo / meter / key / chords / section markers from imported audio. When
// `applyToTimeline` is true it writes them into the project (tempo map, time signature,
// key, chord events, section markers); when false it only detects and returns the summary,
// leaving the timeline untouched. The caller decides whether to analyze at all.
std::string analyzeImportedAudioIntoProject(ProjectDocument& project,
                                            const WavAudioData& data,
                                            double clipStartSeconds,
                                            double clipDurationSeconds,
                                            bool hasEmbeddedTempo,
                                            bool applyToTimeline = true);
std::string reanalyzeClipMusicalMetadata(ProjectDocument& project,
                                         const std::string& clipId,
                                         const WavAudioData& sourceData,
                                         std::string& error);
ProjectMusicReanalysisReport reanalyzeProjectMusicalMetadata(ProjectDocument& project,
                                                             const std::function<bool(const std::string& path,
                                                                                      WavAudioData& data,
                                                                                      std::string& error)>& loadSourceAudio);

} // namespace neuracoust::daw
