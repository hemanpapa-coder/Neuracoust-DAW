#pragma once

// Reads an AAF session (Pro Tools, Media Composer, Resolve, Nuendo…) into a ProjectDocument.
//
// AAF is a structured-storage container with a large object model, so this leans on libAAF rather
// than parsing it here. libAAF is GPL v2+ — linking it makes the built DAW GPL, which is accepted
// for this personal build. If the DAW is ever distributed commercially this has to be replaced.
//
// What is imported: audio tracks, their clips (position, length, source offset, gain, mute) and the
// session's markers. Video, effects and automation are not — an AAF's plug-in and automation model
// does not map onto ours, and inventing a mapping would silently misrepresent the session.

#include "project/ProjectDocument.h"

#include <filesystem>
#include <string>

namespace neuracoust::daw {

struct AafImportResult {
    bool ok = false;
    size_t trackCount = 0;
    size_t clipCount = 0;
    size_t markerCount = 0;
    /// Clips whose media file could not be found on disk. They are still placed on the timeline so
    /// the arrangement is visible; the paths are reported so the user can relink them.
    size_t missingMediaCount = 0;
    std::string message;
};

/// Builds a project from `path`. `project` is replaced wholesale on success.
AafImportResult importAafSession(const std::filesystem::path& path, ProjectDocument& project);

/// True when this build can read AAF at all (libAAF present).
bool aafImportAvailable();

} // namespace neuracoust::daw
