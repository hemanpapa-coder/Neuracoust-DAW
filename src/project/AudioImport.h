#pragma once

// Audio import orchestration, ported out of the old UI (DawWindowController.mm).
// The primitives — WAV reading, media-folder copies, clip creation, musical
// analysis — already live in the engine; what lived only in the UI was the policy:
// where a converted file goes, when to convert at all, and where the clip lands.
//
// Nothing here touches AppKit. Conversion shells out to /usr/bin/afconvert, the
// same tool the old UI used.

#include "project/ProjectDocument.h"

#include <filesystem>
#include <string>

namespace neuracoust::daw {

struct AudioImportResult {
    std::string clipId;
    std::string clipSourcePath;   // the WAV the clip actually points at
    double durationSeconds = 0.0;
    bool converted = false;       // true when the source was not already a WAV
    bool copiedIntoProject = false;
    std::string message;
};

/// wav, wave, mp3, aif, aiff, m4a, caf — the set the old UI accepted.
bool isSupportedImportAudioExtension(const std::filesystem::path& path);

/// `<project folder>/Audio Files`, or the current directory when the project has
/// no path yet.
std::filesystem::path projectAudioFilesDirectory(const std::filesystem::path& projectPath);

/// Where imports go before the project has been saved anywhere.
std::filesystem::path temporaryImportAudioFilesDirectory();

/// Converts `sourceWav` to the format `spec` names at `targetPath`, via afconvert.
/// Specs: "wav16" / "wav24" / "wavf32" / "aiff24" / "flac" / "aac", optionally with a
/// ":<sampleRate>" suffix (absent or 0 keeps the source rate). The clip-export menu's presets
/// are exactly these strings. Returns false with `error` filled when afconvert refuses.
bool convertAudioFileToSpec(const std::filesystem::path& sourceWav,
                            const std::filesystem::path& targetPath,
                            const std::string& spec,
                            std::string& error);

/// Converts any supported audio file to a float32 WAV inside `mediaDirectory`,
/// choosing a filename that does not collide. Returns the written path.
bool convertAudioFileToWavInDirectory(const std::filesystem::path& sourcePath,
                                      const std::filesystem::path& mediaDirectory,
                                      std::string& convertedPath,
                                      std::string& error);

/// Imports `sourcePath` onto `trackName` at `startSeconds`.
///
/// A WAV already inside the project's media folder is used where it lies. Anything
/// else is converted (or copied, if already a WAV) into the project's Audio Files
/// folder — or into a temporary folder when `projectPath` is empty, because an
/// unsaved project has nowhere of its own to put media.
///
/// The clip's musical metadata is analysed on the way in, the same as the old UI.
/// `analyze` runs the tempo / key / chord / marker detection; `applyToTimeline` decides
/// whether that detection is written into the project (tempo map, markers, key, chords) or
/// only reported. `analyze == false` imports the audio raw and touches none of it.
bool importAudioFile(ProjectDocument& project,
                     const std::filesystem::path& projectPath,
                     const std::string& trackName,
                     const std::filesystem::path& sourcePath,
                     double startSeconds,
                     AudioImportResult& result,
                     std::string& error,
                     bool analyze = true,
                     bool applyToTimeline = true);

} // namespace neuracoust::daw
