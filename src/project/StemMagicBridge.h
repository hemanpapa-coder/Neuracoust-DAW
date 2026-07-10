#pragma once

#include "project/ProjectDocument.h"
#include <filesystem>
#include <string>
#include <vector>

namespace neuracoust::daw {

enum class StemMagicJobMode {
    FourStem,
    DrumSplit
};

enum class StemMagicTrackLayout {
    IndividualTracks,
    FolderWithOriginal,
    BusFolder
};

enum class StemMagicComputeBackend {
    Auto,
    MetalMps,
    CoreMlAne
};

struct StemMagicJobResult {
    bool ok = false;
    bool runtimeFound = false;
    bool runtimeUsesMetalAcceleration = false;
    bool runtimeUsesAneAcceleration = false;
    bool supportsMpsServer = false;
    bool supportsDrumSplitServer = false;
    bool supportsCoreMlAne = false;
    std::filesystem::path manifestPath;
    std::filesystem::path progressPath;
    std::filesystem::path outputDirectory;
    std::filesystem::path runtimeAppPath;
    std::filesystem::path runtimePythonPath;
    std::filesystem::path runtimePythonExtraPath;
    std::filesystem::path runtimeScriptPath;
    std::filesystem::path runtimeCoreMlScriptPath;
    std::filesystem::path runtimeMpsServerPath;
    std::filesystem::path runtimeDrumSplitServerPath;
    std::filesystem::path runtimeModelPath;
    std::filesystem::path runtimeCoreMlModelPath;
    std::filesystem::path resolvedSourcePath;
    std::string message;
};

struct StemMagicRuntimeResources {
    bool ok = false;
    bool supportsBatchFourStem = false;
    bool supportsMpsServer = false;
    bool supportsDrumSplitServer = false;
    bool supportsCoreMlAne = false;
    bool batchUsesMetalAcceleration = false;
    bool batchUsesAneAcceleration = false;
    std::filesystem::path appPath;
    std::filesystem::path resourcesPath;
    std::filesystem::path pythonPath;
    std::filesystem::path pythonExtraPath;
    std::filesystem::path scriptPath;
    std::filesystem::path batchBridgePath;
    std::filesystem::path coreMlBatchBridgePath;
    std::filesystem::path mpsServerPath;
    std::filesystem::path drumSplitServerPath;
    std::filesystem::path modelPath;
    std::filesystem::path coreMlModelPath;
    std::string message;
};

struct StemMagicApplyResult {
    bool ok = false;
    size_t createdTracks = 0;
    std::vector<std::string> createdTrackNames;
    std::vector<std::filesystem::path> missingStemPaths;
    std::string message;
};

std::vector<std::string> stemMagicExpectedStemNames(StemMagicJobMode mode);
std::filesystem::path findStemMagicMetalApp();
StemMagicRuntimeResources resolveStemMagicRuntimeResources(const std::filesystem::path& preferredAppPath = {});

StemMagicJobResult writeStemMagicJobManifest(const ClipState& clip,
                                             const std::filesystem::path& projectPath,
                                             StemMagicJobMode mode);
StemMagicApplyResult applyStemMagicRenderedStems(ProjectDocument& project,
                                                 const StemMagicJobResult& job,
                                                 const ClipState& sourceClip,
                                                 StemMagicJobMode mode,
                                                 StemMagicTrackLayout layout = StemMagicTrackLayout::IndividualTracks);

} // namespace neuracoust::daw
