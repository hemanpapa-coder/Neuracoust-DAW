#include "project/StemMagicBridge.h"
#include "audio/WavFile.h"
#include "project/EditOperations.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace neuracoust::daw {
namespace {

std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    result += ' ';
                } else {
                    result += ch;
                }
                break;
        }
    }
    return result;
}

std::string sanitizedFileStem(std::string value) {
    if (value.empty()) {
        value = "clip";
    }
    for (char& ch : value) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
        if (!allowed) {
            ch = '-';
        }
    }
    while (value.find("--") != std::string::npos) {
        value.replace(value.find("--"), 2, "-");
    }
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](char ch) { return ch != '-'; }));
    while (!value.empty() && value.back() == '-') {
        value.pop_back();
    }
    return value.empty() ? "clip" : value;
}

std::filesystem::path projectBaseDirectory(const std::filesystem::path& projectPath) {
    if (projectPath.empty()) {
        return std::filesystem::temp_directory_path() / "Neuracoust DAW";
    }
    const auto parent = projectPath.parent_path();
    return parent.empty() ? std::filesystem::current_path() : parent;
}

std::vector<std::filesystem::path> stemMagicAppCandidates() {
    return {
        "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/build-current/NeuracoustStemMagicMetal_artefacts/Release/Stem Magic Metal.app",
        "/Applications/Neuracoust/Stem Magic Metal.app",
        "/Applications/Stem Magic Metal.app",
        "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/release/Stem Magic Metal v260610.2207/Stem Magic Metal.app",
        "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/build/NeuracoustStemMagicMetal_artefacts/Release/Stem Magic Metal.app",
        "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/StemMagicMetal_Staging_260612.2356/Stem Magic Metal.app",
        "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/StemMagicMetal_Staging_260612.2345/Stem Magic Metal.app",
        "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/StemMagicMetal_Staging_260612.2341/Stem Magic Metal.app",
        "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/StemMagicMetal_Staging_260612.2336/Stem Magic Metal.app"
    };
}

bool fileContainsText(const std::filesystem::path& path, const std::string& needle) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find(needle) != std::string::npos;
}

std::filesystem::path currentExecutablePath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) {
        return {};
    }
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(buffer, error);
    return error ? std::filesystem::path(buffer) : canonical;
#else
    return {};
#endif
}

std::vector<std::filesystem::path> stemMagicBatchBridgeCandidates(const std::filesystem::path& stemMagicResourcesPath) {
    std::vector<std::filesystem::path> paths;
    const auto executable = currentExecutablePath();
    if (!executable.empty()) {
        const auto macOsDirectory = executable.parent_path();
        const auto contentsDirectory = macOsDirectory.parent_path();
        paths.push_back(contentsDirectory / "Resources" / "stem_magic_mps_batch.py");
        paths.push_back(macOsDirectory / "stem_magic_mps_batch.py");
    }
    if (!stemMagicResourcesPath.empty()) {
        paths.push_back(stemMagicResourcesPath / "stem_magic_mps_batch.py");
    }
    paths.push_back("/Volumes/Program Dev/DW/resources/stem_magic_mps_batch.py");
    return paths;
}

std::vector<std::filesystem::path> stemMagicCoreMlBatchBridgeCandidates(const std::filesystem::path& stemMagicResourcesPath) {
    std::vector<std::filesystem::path> paths;
    const auto executable = currentExecutablePath();
    if (!executable.empty()) {
        const auto macOsDirectory = executable.parent_path();
        const auto contentsDirectory = macOsDirectory.parent_path();
        paths.push_back(contentsDirectory / "Resources" / "stem_magic_coreml_batch.py");
        paths.push_back(macOsDirectory / "stem_magic_coreml_batch.py");
    }
    if (!stemMagicResourcesPath.empty()) {
        paths.push_back(stemMagicResourcesPath / "stem_magic_coreml_batch.py");
    }
    paths.push_back("/Volumes/Program Dev/DW/resources/stem_magic_coreml_batch.py");
    return paths;
}

std::filesystem::path resolveClipSourcePath(const std::string& sourcePath,
                                            const std::filesystem::path& baseDirectory) {
    std::filesystem::path resolved(sourcePath);
    if (resolved.is_relative()) {
        resolved = baseDirectory / resolved;
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(resolved, error);
    if (!error && !canonical.empty()) {
        return canonical;
    }
    return resolved.lexically_normal();
}

std::string modeName(StemMagicJobMode mode) {
    return mode == StemMagicJobMode::DrumSplit ? "drum-split" : "four-stem";
}

std::string uniqueStemTrackName(std::set<std::string>& existingTrackNames, const std::string& base) {
    std::string candidate = base;
    int suffix = 2;
    while (existingTrackNames.find(candidate) != existingTrackNames.end()) {
        candidate = base + " " + std::to_string(suffix++);
    }
    existingTrackNames.insert(candidate);
    return candidate;
}

TrackState* findMutableTrack(ProjectDocument& project, const std::string& trackName) {
    auto it = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    return it == project.tracks.end() ? nullptr : &(*it);
}

std::string nextStemMagicBusName(const ProjectDocument& project) {
    std::set<std::string> used;
    for (const auto& track : project.tracks) {
        used.insert(track.inputBus);
        used.insert(track.outputBus);
        for (const auto& send : track.sends) {
            used.insert(send.busName);
        }
    }
    for (int bus = 1; bus <= 64; ++bus) {
        const std::string candidate = "Bus " + std::to_string(bus * 2 - 1) + "-" + std::to_string(bus * 2);
        if (used.find(candidate) == used.end()) {
            return candidate;
        }
    }
    return "Bus 1-2";
}

bool writeTextFileAtomically(const std::filesystem::path& path, const std::string& text, std::string& error) {
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    if (fsError) {
        error = "Could not create Stem Magic job folder: " + fsError.message();
        return false;
    }
    const auto tempPath = path.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not open Stem Magic job manifest for writing.";
            return false;
        }
        out << text;
        if (!out) {
            error = "Could not write Stem Magic job manifest.";
            return false;
        }
    }
    std::filesystem::rename(tempPath, path, fsError);
    if (fsError) {
        std::filesystem::remove(path, fsError);
        fsError.clear();
        std::filesystem::rename(tempPath, path, fsError);
        if (fsError) {
            error = "Could not finalize Stem Magic job manifest: " + fsError.message();
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::string> stemMagicExpectedStemNames(StemMagicJobMode mode) {
    if (mode == StemMagicJobMode::DrumSplit) {
        return {"Kick", "Snare", "Cymbals", "Toms"};
    }
    return {"Drums", "Bass", "Other", "Vocals"};
}

std::filesystem::path findStemMagicMetalApp() {
    for (const auto& candidate : stemMagicAppCandidates()) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

StemMagicRuntimeResources resolveStemMagicRuntimeResources(const std::filesystem::path& preferredAppPath) {
    StemMagicRuntimeResources resources;
    const auto candidates = preferredAppPath.empty()
        ? stemMagicAppCandidates()
        : std::vector<std::filesystem::path> {preferredAppPath};
    std::string lastMessage = "Stem Magic Metal app was not found.";
    for (const auto& appCandidate : candidates) {
        std::error_code appError;
        if (!std::filesystem::is_directory(appCandidate, appError)) {
            continue;
        }

        StemMagicRuntimeResources candidateResources;
        candidateResources.appPath = appCandidate;
        candidateResources.resourcesPath = candidateResources.appPath / "Contents" / "Resources";
        const std::vector<std::filesystem::path> pythonCandidates {
            candidateResources.resourcesPath / "python",
            candidateResources.resourcesPath / "venv" / "bin" / "python",
            candidateResources.resourcesPath / "venv" / "bin" / "python3"
        };
        const std::vector<std::filesystem::path> scriptCandidates {
            candidateResources.resourcesPath / "run_demucs_mps.py",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/scripts/run_demucs_mps.py"
        };
        const std::vector<std::filesystem::path> modelCandidates {
            candidateResources.resourcesPath / "demucs_mps.pt",
            candidateResources.resourcesPath / "Models" / "demucs_mps.pt",
            candidateResources.resourcesPath / "demucs.pt",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/demucs_mps.pt",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/demucs.pt"
        };
        const std::vector<std::filesystem::path> coreMlModelCandidates {
            candidateResources.resourcesPath / "demucs.mlpackage",
            candidateResources.resourcesPath / "Models" / "demucs.mlpackage",
            candidateResources.resourcesPath / "demucs.mlmodelc",
            candidateResources.resourcesPath / "Models" / "demucs.mlmodelc",
            candidateResources.resourcesPath / "demucs.mlmodel",
            candidateResources.resourcesPath / "Models" / "demucs.mlmodel",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/Assets/demucs.mlpackage",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/Assets/demucs.mlmodelc",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/demucs.mlpackage",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/demucs.mlmodelc"
        };
        const std::vector<std::filesystem::path> pythonExtraCandidates {
            candidateResources.resourcesPath / "venv" / "lib" / "python3.9" / "site-packages"
        };

        auto firstExistingFile = [](const std::vector<std::filesystem::path>& paths) {
            for (const auto& path : paths) {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error)) {
                    return path;
                }
            }
            return std::filesystem::path {};
        };
        auto firstExistingPath = [](const std::vector<std::filesystem::path>& paths) {
            for (const auto& path : paths) {
                std::error_code error;
                if (std::filesystem::exists(path, error)) {
                    return path;
                }
            }
            return std::filesystem::path {};
        };
        auto firstSitePackagesWithStemRuntime = [](const std::vector<std::filesystem::path>& paths) {
            for (const auto& path : paths) {
                std::error_code error;
                if (std::filesystem::is_directory(path / "torchgen", error) ||
                    std::filesystem::is_directory(path / "coremltools", error)) {
                    return path;
                }
            }
            return std::filesystem::path {};
        };

        candidateResources.pythonPath = firstExistingFile(pythonCandidates);
        candidateResources.pythonExtraPath = firstSitePackagesWithStemRuntime(pythonExtraCandidates);
        candidateResources.scriptPath = firstExistingFile(scriptCandidates);
        candidateResources.batchBridgePath = firstExistingFile(stemMagicBatchBridgeCandidates(candidateResources.resourcesPath));
        candidateResources.coreMlBatchBridgePath = firstExistingFile(stemMagicCoreMlBatchBridgeCandidates(candidateResources.resourcesPath));
        candidateResources.mpsServerPath = firstExistingFile({
            candidateResources.resourcesPath / "mps_inference_server.py",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/scripts/mps_inference_server.py"
        });
        candidateResources.drumSplitServerPath = firstExistingFile({
            candidateResources.resourcesPath / "drumsep_inference_server.py",
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/scripts/drumsep_inference_server.py"
        });
        candidateResources.modelPath = firstExistingFile(modelCandidates);
        candidateResources.coreMlModelPath = firstExistingPath(coreMlModelCandidates);
        const bool hasTorchRuntimeSupport = !candidateResources.pythonExtraPath.empty() &&
                                            std::filesystem::is_directory(candidateResources.pythonExtraPath / "torchgen");
        const bool hasCoreMlRuntimeSupport = !candidateResources.pythonExtraPath.empty() &&
                                             std::filesystem::is_directory(candidateResources.pythonExtraPath / "coremltools");
        const bool supportsMpsBridgeBatch = !candidateResources.pythonPath.empty() &&
                                            hasTorchRuntimeSupport &&
                                            !candidateResources.batchBridgePath.empty() &&
                                            !candidateResources.mpsServerPath.empty();
        const bool supportsCoreMlBatch = !candidateResources.pythonPath.empty() &&
                                         hasCoreMlRuntimeSupport &&
                                         !candidateResources.coreMlBatchBridgePath.empty() &&
                                         !candidateResources.coreMlModelPath.empty();
        const bool supportsLegacyBatch = !candidateResources.pythonPath.empty() &&
                                         hasTorchRuntimeSupport &&
                                         !candidateResources.scriptPath.empty() &&
                                         !candidateResources.modelPath.empty();
        if (supportsMpsBridgeBatch) {
            candidateResources.scriptPath = candidateResources.batchBridgePath;
        }
        candidateResources.supportsBatchFourStem = supportsMpsBridgeBatch || supportsCoreMlBatch || supportsLegacyBatch;
        candidateResources.supportsMpsServer = !candidateResources.pythonPath.empty() &&
                                               hasTorchRuntimeSupport &&
                                               !candidateResources.mpsServerPath.empty();
        candidateResources.supportsDrumSplitServer = !candidateResources.pythonPath.empty() &&
                                                     hasTorchRuntimeSupport &&
                                                     !candidateResources.drumSplitServerPath.empty();
        candidateResources.supportsCoreMlAne = supportsCoreMlBatch;
        candidateResources.batchUsesMetalAcceleration = supportsMpsBridgeBatch ||
            (candidateResources.supportsBatchFourStem &&
             (fileContainsText(candidateResources.scriptPath, "torch.device(\"mps\")") ||
             fileContainsText(candidateResources.scriptPath, "torch.device('mps')") ||
             fileContainsText(candidateResources.scriptPath, "device = \"mps\"") ||
              fileContainsText(candidateResources.scriptPath, "device = 'mps'")));
        candidateResources.batchUsesAneAcceleration = supportsCoreMlBatch;

        if (candidateResources.pythonPath.empty()) {
            lastMessage = "Stem Magic Metal bundled Python was not found.";
        } else if (!hasTorchRuntimeSupport && !hasCoreMlRuntimeSupport) {
            lastMessage = "Stem Magic bundled Python is missing torchgen or coremltools site-packages required by stem acceleration.";
        } else if (candidateResources.supportsBatchFourStem) {
            candidateResources.ok = true;
            candidateResources.message = candidateResources.supportsCoreMlAne
                ? "Stem Magic CoreML/ANE and Metal/MPS batch runtimes resolved."
                : (candidateResources.batchUsesMetalAcceleration
                ? "Stem Magic Metal/MPS batch runtime resolved."
                : "Stem Magic Metal batch runtime resolved; current batch runner uses CPU/AMX for HTDemucs.");
            return candidateResources;
        } else if (candidateResources.supportsMpsServer || candidateResources.supportsDrumSplitServer) {
            lastMessage = "Stem Magic server scripts were found, but a DAW batch runner is not bundled in this app.";
        } else if (candidateResources.scriptPath.empty()) {
            lastMessage = "Stem Magic Metal runner script was not found.";
        } else if (candidateResources.modelPath.empty()) {
            lastMessage = "Stem Magic Metal Demucs model was not found.";
        }
    }
    resources.message = lastMessage;
    return resources;
}

StemMagicJobResult writeStemMagicJobManifest(const ClipState& clip,
                                             const std::filesystem::path& projectPath,
                                             StemMagicJobMode mode) {
    StemMagicJobResult result;
    if (clip.id.empty()) {
        result.message = "Stem Magic needs a selected clip.";
        return result;
    }
    if (clip.sourcePath.empty()) {
        result.message = "Selected clip has no source audio path.";
        return result;
    }

    const auto baseDirectory = projectBaseDirectory(projectPath);
    const auto resolvedSourcePath = resolveClipSourcePath(clip.sourcePath, baseDirectory);
    const auto jobDirectory = baseDirectory / "Stem Magic Jobs";
    const auto outputDirectory = jobDirectory / (sanitizedFileStem(clip.id) + "-" + modeName(mode));
    const auto manifestPath = outputDirectory / "Neuracoust DAW Stem Magic Job.json";
    const auto progressPath = outputDirectory / "Neuracoust DAW Stem Magic Progress.json";
    const auto expected = stemMagicExpectedStemNames(mode);
    const auto runtimeResources = resolveStemMagicRuntimeResources();
    const auto runtimeApp = runtimeResources.appPath;
    const auto sourceBaseName = resolvedSourcePath.stem().string();
    std::error_code cleanupError;
    std::filesystem::create_directories(outputDirectory, cleanupError);
    for (const auto& stemName : expected) {
        cleanupError.clear();
        std::filesystem::remove(outputDirectory / (sourceBaseName + "_" + stemName + ".wav"), cleanupError);
    }

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"com.neuracoust.daw.stem-magic-job.v1\",\n";
    out << "  \"status\": \"queued-for-internal-render\",\n";
    out << "  \"engine\": \"Neuracoust Stem Magic Metal\",\n";
    out << "  \"engineMode\": \"internal-background\",\n";
    out << "  \"mode\": \"" << modeName(mode) << "\",\n";
    out << "  \"clipId\": \"" << jsonEscape(clip.id) << "\",\n";
    out << "  \"trackName\": \"" << jsonEscape(clip.trackName) << "\",\n";
    out << "  \"regionName\": \"" << jsonEscape(clip.regionName) << "\",\n";
    out << "  \"sourcePath\": \"" << jsonEscape(resolvedSourcePath.string()) << "\",\n";
    out << "  \"originalSourcePath\": \"" << jsonEscape(std::filesystem::path(clip.sourcePath).lexically_normal().generic_string()) << "\",\n";
    out << "  \"projectPath\": \"" << jsonEscape(projectPath.string()) << "\",\n";
    out << "  \"outputDirectory\": \"" << jsonEscape(outputDirectory.string()) << "\",\n";
    out << "  \"progressPath\": \"" << jsonEscape(progressPath.string()) << "\",\n";
    out << "  \"clipStartSeconds\": " << clip.startSeconds << ",\n";
    out << "  \"clipDurationSeconds\": " << clip.durationSeconds << ",\n";
    out << "  \"sourceOffsetSeconds\": " << clip.sourceOffsetSeconds << ",\n";
    out << "  \"sourceChannels\": " << clip.sourceChannels << ",\n";
    out << "  \"sourceSampleRate\": " << clip.sourceSampleRate << ",\n";
    out << "  \"sourceBitsPerSample\": " << clip.sourceBitsPerSample << ",\n";
    out << "  \"runtimeAppPath\": \"" << jsonEscape(runtimeApp.string()) << "\",\n";
    out << "  \"runtimePythonPath\": \"" << jsonEscape(runtimeResources.pythonPath.string()) << "\",\n";
    out << "  \"runtimePythonExtraPath\": \"" << jsonEscape(runtimeResources.pythonExtraPath.string()) << "\",\n";
    out << "  \"runtimeScriptPath\": \"" << jsonEscape(runtimeResources.scriptPath.string()) << "\",\n";
    out << "  \"runtimeBatchBridgePath\": \"" << jsonEscape(runtimeResources.batchBridgePath.string()) << "\",\n";
    out << "  \"runtimeCoreMlBatchBridgePath\": \"" << jsonEscape(runtimeResources.coreMlBatchBridgePath.string()) << "\",\n";
    out << "  \"runtimeMpsServerPath\": \"" << jsonEscape(runtimeResources.mpsServerPath.string()) << "\",\n";
    out << "  \"runtimeDrumSplitServerPath\": \"" << jsonEscape(runtimeResources.drumSplitServerPath.string()) << "\",\n";
    out << "  \"runtimeModelPath\": \"" << jsonEscape(runtimeResources.modelPath.string()) << "\",\n";
    out << "  \"runtimeCoreMlModelPath\": \"" << jsonEscape(runtimeResources.coreMlModelPath.string()) << "\",\n";
    out << "  \"supportsBatchFourStem\": " << (runtimeResources.supportsBatchFourStem ? "true" : "false") << ",\n";
    out << "  \"supportsMpsServer\": " << (runtimeResources.supportsMpsServer ? "true" : "false") << ",\n";
    out << "  \"supportsDrumSplitServer\": " << (runtimeResources.supportsDrumSplitServer ? "true" : "false") << ",\n";
    out << "  \"supportsCoreMlAne\": " << (runtimeResources.supportsCoreMlAne ? "true" : "false") << ",\n";
    out << "  \"batchUsesMetalAcceleration\": " << (runtimeResources.batchUsesMetalAcceleration ? "true" : "false") << ",\n";
    out << "  \"batchUsesAneAcceleration\": " << (runtimeResources.batchUsesAneAcceleration ? "true" : "false") << ",\n";
    out << "  \"expectedStems\": [";
    for (size_t index = 0; index < expected.size(); ++index) {
        out << (index == 0 ? "" : ", ") << "\"" << jsonEscape(expected[index]) << "\"";
    }
    out << "]\n";
    out << "}\n";

    std::string error;
    if (!writeTextFileAtomically(manifestPath, out.str(), error)) {
        result.message = error;
        return result;
    }

    std::ostringstream progress;
    progress << "{\n";
    progress << "  \"schema\": \"com.neuracoust.daw.stem-magic-progress.v1\",\n";
    progress << "  \"status\": \"queued\",\n";
    progress << "  \"progress\": 0.0,\n";
    progress << "  \"message\": \"Stem Magic render is queued inside Neuracoust DAW. Tracks are created only after rendered stem WAV files exist.\",\n";
    progress << "  \"manifestPath\": \"" << jsonEscape(manifestPath.string()) << "\"\n";
    progress << "}\n";
    if (!writeTextFileAtomically(progressPath, progress.str(), error)) {
        result.message = error;
        return result;
    }

    result.ok = true;
    result.runtimeFound = runtimeResources.ok;
    result.runtimeUsesMetalAcceleration = runtimeResources.batchUsesMetalAcceleration;
    result.runtimeUsesAneAcceleration = runtimeResources.batchUsesAneAcceleration;
    result.supportsMpsServer = runtimeResources.supportsMpsServer;
    result.supportsDrumSplitServer = runtimeResources.supportsDrumSplitServer;
    result.supportsCoreMlAne = runtimeResources.supportsCoreMlAne;
    result.manifestPath = manifestPath;
    result.progressPath = progressPath;
    result.outputDirectory = outputDirectory;
    result.runtimeAppPath = runtimeApp;
    result.runtimePythonPath = runtimeResources.pythonPath;
    result.runtimePythonExtraPath = runtimeResources.pythonExtraPath;
    result.runtimeScriptPath = runtimeResources.scriptPath;
    result.runtimeCoreMlScriptPath = runtimeResources.coreMlBatchBridgePath;
    result.runtimeMpsServerPath = runtimeResources.mpsServerPath;
    result.runtimeDrumSplitServerPath = runtimeResources.drumSplitServerPath;
    result.runtimeModelPath = runtimeResources.modelPath;
    result.runtimeCoreMlModelPath = runtimeResources.coreMlModelPath;
    result.resolvedSourcePath = resolvedSourcePath;
    result.message = result.runtimeFound
        ? "Stem Magic job manifest created."
        : "Stem Magic job manifest created, but " + runtimeResources.message;
    return result;
}

StemMagicApplyResult applyStemMagicRenderedStems(ProjectDocument& project,
                                                 const StemMagicJobResult& job,
                                                 const ClipState& sourceClip,
                                                 StemMagicJobMode mode,
                                                 StemMagicTrackLayout layout) {
    StemMagicApplyResult result;
    if (!job.ok) {
        result.message = "Stem Magic job is not ready.";
        return result;
    }
    if (job.outputDirectory.empty() || job.resolvedSourcePath.empty()) {
        result.message = "Stem Magic output path is missing.";
        return result;
    }

    const ClipState sourceClipSnapshot = sourceClip;
    const auto stemNames = stemMagicExpectedStemNames(mode);
    std::set<std::string> existingTrackNames;
    for (const auto& track : project.tracks) {
        existingTrackNames.insert(track.name);
    }

    static const char* kStemColors[] = {"#E05A47", "#4D9BE6", "#D3A72F", "#8E63D8", "#3EB489"};
    const auto sourceBaseName = job.resolvedSourcePath.stem().string();
    std::vector<std::filesystem::path> readyStemPaths;
    readyStemPaths.reserve(stemNames.size());
    for (size_t stemIndex = 0; stemIndex < stemNames.size(); ++stemIndex) {
        const auto stemPath = job.outputDirectory / (sourceBaseName + "_" + stemNames[stemIndex] + ".wav");
        std::error_code error;
        if (!std::filesystem::is_regular_file(stemPath, error)) {
            result.missingStemPaths.push_back(stemPath);
            continue;
        }
        const auto byteSize = std::filesystem::file_size(stemPath, error);
        if (error || byteSize <= 44) {
            result.missingStemPaths.push_back(stemPath);
            continue;
        }
        WavAudioData renderedStem;
        std::string wavError;
        if (!readPcmWavFile(stemPath, renderedStem, wavError) ||
            renderedStem.channels <= 0 ||
            renderedStem.sampleRate <= 0 ||
            renderedStem.frameCount() <= 0) {
            result.missingStemPaths.push_back(stemPath);
            continue;
        }
        readyStemPaths.push_back(stemPath);
    }

    if (!result.missingStemPaths.empty()) {
        result.message = "Stem Magic render is not complete yet; no partial stem tracks were added.";
        return result;
    }

    const std::string layoutBaseName = sourceClipSnapshot.regionName.empty()
        ? (sourceClipSnapshot.trackName.empty() ? "Stem Magic" : "Stem Magic " + sourceClipSnapshot.trackName)
        : "Stem Magic " + sourceClipSnapshot.regionName;
    std::string containerTrackName;
    std::string busInputName;
    if (layout == StemMagicTrackLayout::FolderWithOriginal) {
        containerTrackName = addFolderTrack(project);
        if (auto* folder = findMutableTrack(project, containerTrackName)) {
            folder->name = uniqueStemTrackName(existingTrackNames, layoutBaseName);
            folder->colorHex = "#7CCB5E";
            containerTrackName = folder->name;
        }
        moveTrackIntoFolder(project, sourceClipSnapshot.trackName, containerTrackName);
    } else if (layout == StemMagicTrackLayout::BusFolder) {
        busInputName = nextStemMagicBusName(project);
        containerTrackName = addBusFolderTrack(project, busInputName);
        if (auto* busFolder = findMutableTrack(project, containerTrackName)) {
            busFolder->name = uniqueStemTrackName(existingTrackNames, layoutBaseName + " Bus");
            busFolder->inputBus = busInputName;
            busFolder->outputBus = "Master";
            busFolder->colorHex = "#F0B84D";
            containerTrackName = busFolder->name;
        }
        if (moveTrackIntoFolder(project, sourceClipSnapshot.trackName, containerTrackName)) {
            if (auto* sourceTrack = findMutableTrack(project, sourceClipSnapshot.trackName)) {
                sourceTrack->outputBus = busInputName;
            }
        }
    }

    for (size_t stemIndex = 0; stemIndex < stemNames.size(); ++stemIndex) {
        const auto& stemPath = readyStemPaths[stemIndex];

        const std::string provisionalTrackName = addAudioTrack(project);
        auto trackIt = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
            return track.name == provisionalTrackName;
        });
        if (trackIt == project.tracks.end()) {
            result.missingStemPaths.push_back(stemPath);
            continue;
        }

        trackIt->name = uniqueStemTrackName(existingTrackNames, "Stem Magic " + stemNames[stemIndex]);
        trackIt->outputBus = "Master";
        trackIt->inputBus = "None";
        const std::string stemColor = kStemColors[result.createdTracks % (sizeof(kStemColors) / sizeof(kStemColors[0]))];
        trackIt->colorHex = stemColor;
        const std::string createdTrackName = trackIt->name;

        const std::string clipId = appendAudioClipAt(project,
                                                     createdTrackName,
                                                     stemPath.string(),
                                                     sourceClipSnapshot.startSeconds,
                                                     sourceClipSnapshot.durationSeconds);
        if (layout == StemMagicTrackLayout::FolderWithOriginal && !containerTrackName.empty()) {
            moveTrackIntoFolder(project, createdTrackName, containerTrackName);
        } else if (layout == StemMagicTrackLayout::BusFolder && !containerTrackName.empty()) {
            moveTrackIntoFolder(project, createdTrackName, containerTrackName);
            auto* createdTrack = findMutableTrack(project, createdTrackName);
            if (createdTrack != nullptr && !busInputName.empty()) {
                createdTrack->outputBus = busInputName;
            }
        }

        auto clipIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
            return clip.id == clipId;
        });
        if (clipIt != project.clips.end()) {
            clipIt->regionName = stemNames[stemIndex];
            clipIt->sourceOffsetSeconds = sourceClipSnapshot.sourceOffsetSeconds;
            clipIt->timeScale = sourceClipSnapshot.timeScale;
            clipIt->tempoSyncPolicy = sourceClipSnapshot.tempoSyncPolicy;
            clipIt->pendingTimeStretchToProject = sourceClipSnapshot.pendingTimeStretchToProject;
            clipIt->colorHex = stemColor;
        }

        ++result.createdTracks;
        result.createdTrackNames.push_back(createdTrackName);
    }

    result.ok = result.createdTracks > 0;
    if (result.ok) {
        if (layout == StemMagicTrackLayout::FolderWithOriginal && !containerTrackName.empty()) {
            result.message = "Stem Magic rendered stems added in a folder with the original track.";
        } else if (layout == StemMagicTrackLayout::BusFolder && !containerTrackName.empty()) {
            result.message = "Stem Magic rendered stems added in a routed bus folder.";
        } else {
            result.message = "Stem Magic rendered stems added to the project.";
        }
    } else {
        result.message = "No rendered Stem Magic WAV files were found.";
    }
    return result;
}

} // namespace neuracoust::daw
