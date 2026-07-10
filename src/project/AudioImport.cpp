#include "project/AudioImport.h"

#include "audio/WavFile.h"
#include "project/AudioImportAnalysis.h"
#include "project/EditOperations.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace neuracoust::daw {

namespace {

std::string lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isWavExtension(const std::filesystem::path& path) {
    const std::string extension = lowercased(path.extension().string());
    return extension == ".wav" || extension == ".wave";
}

/// Never overwrite an existing file: "Take.wav", then "Take 2.wav", and so on.
std::filesystem::path uniqueConvertedWavPath(const std::filesystem::path& mediaDirectory,
                                             const std::filesystem::path& sourcePath) {
    std::string stem = sourcePath.stem().string();
    if (stem.empty()) {
        stem = "Imported Audio";
    }

    std::filesystem::path candidate = mediaDirectory / (stem + ".wav");
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
        candidate = mediaDirectory / (stem + " " + std::to_string(suffix) + ".wav");
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

/// Runs afconvert and waits. Returns the exit status, or -1 if it never started.
int runAfconvert(const std::string& source, const std::string& target) {
    const char* afconvert = "/usr/bin/afconvert";
    std::array<const char*, 8> argv{
        afconvert, source.c_str(), target.c_str(),
        "-f", "WAVE",
        "-d", "LEF32",
        nullptr,
    };

    pid_t pid = 0;
    // afconvert's diagnostics go to stderr; let them through to the app's log.
    if (posix_spawn(&pid, afconvert, nullptr, nullptr,
                    const_cast<char* const*>(argv.data()), environ) != 0) {
        return -1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

bool isSupportedImportAudioExtension(const std::filesystem::path& path) {
    static const std::array<const char*, 7> kSupported{
        ".wav", ".wave", ".mp3", ".aif", ".aiff", ".m4a", ".caf",
    };
    const std::string extension = lowercased(path.extension().string());
    return std::find(kSupported.begin(), kSupported.end(), extension) != kSupported.end();
}

std::filesystem::path projectAudioFilesDirectory(const std::filesystem::path& projectPath) {
    const auto parent = projectPath.parent_path();
    return (parent.empty() ? std::filesystem::current_path() : parent) / "Audio Files";
}

std::filesystem::path temporaryImportAudioFilesDirectory() {
    const char* tmp = std::getenv("TMPDIR");
    const std::filesystem::path root = tmp != nullptr ? std::filesystem::path(tmp)
                                                      : std::filesystem::path("/tmp");
    return root / "Neuracoust DAW Unsaved Imports";
}

bool convertAudioFileToWavInDirectory(const std::filesystem::path& sourcePath,
                                      const std::filesystem::path& mediaDirectory,
                                      std::string& convertedPath,
                                      std::string& error) {
    error.clear();
    convertedPath.clear();

    std::error_code fsError;
    std::filesystem::create_directories(mediaDirectory, fsError);
    if (fsError) {
        error = "Could not create media folder: " + fsError.message();
        return false;
    }

    const auto targetPath = uniqueConvertedWavPath(mediaDirectory, sourcePath);
    if (targetPath.empty()) {
        error = "Could not create a unique converted WAV filename.";
        return false;
    }

    const int status = runAfconvert(sourcePath.string(), targetPath.string());
    if (status < 0) {
        error = "Could not launch afconvert.";
        return false;
    }
    if (status != 0) {
        error = "afconvert failed (exit " + std::to_string(status) + ").";
        std::filesystem::remove(targetPath, fsError);
        return false;
    }

    convertedPath = targetPath.string();
    return true;
}

bool importAudioFile(ProjectDocument& project,
                     const std::filesystem::path& projectPath,
                     const std::string& trackName,
                     const std::filesystem::path& sourcePath,
                     double startSeconds,
                     AudioImportResult& result,
                     std::string& error) {
    result = {};
    error.clear();

    if (!std::filesystem::exists(sourcePath)) {
        error = "File not found: " + sourcePath.string();
        return false;
    }
    if (!isSupportedImportAudioExtension(sourcePath)) {
        error = "Unsupported audio format: " + sourcePath.extension().string();
        return false;
    }

    // An unsaved project has nowhere of its own to keep media.
    const auto mediaDirectory = projectPath.empty() ? temporaryImportAudioFilesDirectory()
                                                    : projectAudioFilesDirectory(projectPath);
    const bool alreadyInMediaFolder =
        isWavExtension(sourcePath) && sourcePath.parent_path() == mediaDirectory;

    std::string clipSourcePath;
    if (alreadyInMediaFolder) {
        clipSourcePath = sourcePath.string();
    } else {
        // afconvert copies a WAV as readily as it transcodes an mp3, and it also
        // normalises the sample format, so there is no separate copy path.
        if (!convertAudioFileToWavInDirectory(sourcePath, mediaDirectory, clipSourcePath, error)) {
            return false;
        }
        result.converted = !isWavExtension(sourcePath);
        result.copiedIntoProject = !projectPath.empty();
    }

    WavAudioData audio;
    if (!readPcmWavFile(clipSourcePath, audio, error)) {
        return false;
    }
    if (audio.sampleRate <= 0 || audio.channels <= 0 || audio.interleavedSamples.empty()) {
        error = "Imported WAV has no audio.";
        return false;
    }

    const double durationSeconds =
        static_cast<double>(audio.frameCount()) / static_cast<double>(audio.sampleRate);

    const std::string clipId =
        appendAudioClipAt(project, trackName, clipSourcePath, std::max(0.0, startSeconds), durationSeconds);
    if (clipId.empty()) {
        error = "Could not place the clip on track '" + trackName + "'.";
        return false;
    }

    result.clipId = clipId;
    result.clipSourcePath = clipSourcePath;
    result.durationSeconds = durationSeconds;
    // A WAV can carry its own tempo; the analysis trusts it when present.
    result.message = analyzeImportedAudioIntoProject(project, audio,
                                                     std::max(0.0, startSeconds),
                                                     durationSeconds,
                                                     audio.embeddedTempoBpm > 0.0);
    return true;
}

} // namespace neuracoust::daw
