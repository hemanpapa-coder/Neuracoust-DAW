#pragma once

// Undo/redo and dirty tracking for a ProjectDocument. Ported out of the old UI
// (DawWindowController.mm `setProjectDirty:`), where it was the only copy.
//
// History is snapshot-based: every recorded edit serializes the whole document and
// compares it against the current snapshot. Serializing costs ~0.06 ms, so the
// expense is not the point — the point is that a step is recorded only when the
// document actually changed, and that one user gesture must produce one step.
// Callers coalesce continuous gestures (a fader drag) by recording once, at the end.
//
// This class touches no files. Autosave is the caller's job; ask isDirty().

#include "project/ProjectDocument.h"

#include <cstddef>
#include <string>
#include <vector>

namespace neuracoust::daw {

class ProjectHistory {
public:
    /// The legacy stack capped at 100 steps; deeper history silently drops the oldest.
    explicit ProjectHistory(size_t maxDepth = 100);

    /// Forget everything and treat `project` as both the current and the saved state.
    /// Use on new-project and on open.
    void reset(const ProjectDocument& project);

    /// Record an edit. Serializes `project`, and if it differs from the current
    /// snapshot pushes the *previous* snapshot onto the undo stack under `stepName`
    /// and clears the redo stack. Returns true when a step was recorded.
    ///
    /// An empty `stepName` records as "Edit".
    bool recordEdit(const ProjectDocument& project, const std::string& stepName = {});

    /// The document has been written to disk: current becomes the saved state and
    /// isDirty() goes false. History is kept.
    void markSaved(const ProjectDocument& project);

    bool isDirty() const;

    bool canUndo() const;
    bool canRedo() const;
    size_t undoDepth() const;
    size_t redoDepth() const;

    /// Name of the step that undo() would reverse, or redo() would replay.
    std::string undoStepName() const;
    std::string redoStepName() const;

    /// Restores the previous snapshot into `project`. Returns false and fills
    /// `error` when there is nothing to undo, or the snapshot fails to parse.
    bool undo(ProjectDocument& project, std::string& error);
    bool redo(ProjectDocument& project, std::string& error);

private:
    size_t maxDepth_;
    std::string currentSnapshot_;
    std::string savedSnapshot_;
    std::vector<std::string> undoSnapshots_;
    std::vector<std::string> undoStepNames_;
    std::vector<std::string> redoSnapshots_;
    std::vector<std::string> redoStepNames_;
};

} // namespace neuracoust::daw
