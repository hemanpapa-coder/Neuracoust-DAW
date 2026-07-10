// Pins the undo/redo and dirty-tracking rules ported out of the old UI.

#include "project/EditOperations.h"
#include "project/ProjectHistory.h"

#include <cstdio>
#include <string>

using namespace neuracoust::daw;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void setVolume(ProjectDocument& project, float db) {
    setTrackVolumeDb(project, "Audio 1", db);
}

/// findTrack is internal to EditOperations; the test walks the vector itself.
float volumeOf(const ProjectDocument& project, const std::string& name) {
    for (const auto& track : project.tracks) {
        if (track.name == name) {
            return track.volumeDb;
        }
    }
    return 999.0f;
}

} // namespace

int main() {
    ProjectDocument project = defaultProject();
    ProjectHistory history;

    // --- a fresh document is clean and has no history ------------------------
    history.reset(project);
    check(!history.isDirty(), "a reset document is clean");
    check(!history.canUndo(), "a reset document cannot undo");
    check(!history.canRedo(), "a reset document cannot redo");

    // --- recording an unchanged document records nothing ----------------------
    check(!history.recordEdit(project, "No-op"), "recording an unchanged document records no step");
    check(!history.isDirty(), "an unchanged document stays clean");
    check(history.undoDepth() == 0, "no step was pushed");

    // --- a real edit records one step -----------------------------------------
    setVolume(project, -6.0f);
    check(history.recordEdit(project, "Volume"), "a changed document records a step");
    check(history.isDirty(), "an edited document is dirty");
    check(history.undoDepth() == 1, "one step on the stack");
    check(history.undoStepName() == "Volume", "the step carries its name");

    // --- undo restores the previous document -----------------------------------
    std::string error;
    check(history.undo(project, error), "undo succeeds");
    check(volumeOf(project, "Audio 1") == 0.0f, "undo restored the volume");
    check(!history.isDirty(), "back at the saved state, the document is clean again");
    check(history.canRedo(), "undo enables redo");
    check(history.redoStepName() == "Volume", "redo carries the step name");
    check(!history.canUndo(), "the undo stack is empty again");

    // --- redo replays it --------------------------------------------------------
    check(history.redo(project, error), "redo succeeds");
    check(volumeOf(project, "Audio 1") == -6.0f, "redo reapplied the volume");
    check(history.isDirty(), "redo makes it dirty again");
    check(history.canUndo(), "redo enables undo");
    check(!history.canRedo(), "the redo stack is empty");

    // --- a new edit after an undo discards the redo stack -----------------------
    check(history.undo(project, error), "undo again");
    check(history.canRedo(), "redo is available");
    setVolume(project, -3.0f);
    check(history.recordEdit(project, "Volume again"), "a new edit records");
    check(!history.canRedo(), "the new edit discarded the redo stack");

    // --- markSaved clears dirty without clearing history ------------------------
    check(history.isDirty(), "still dirty before saving");
    history.markSaved(project);
    check(!history.isDirty(), "saving clears dirty");
    check(history.canUndo(), "saving keeps the undo history");

    // Undoing past a save makes the document dirty again — it no longer matches disk.
    check(history.undo(project, error), "undo past the save point");
    check(history.isDirty(), "undoing past a save is dirty");

    // --- an empty step name records as "Edit" -----------------------------------
    history.reset(project);
    setVolume(project, -9.0f);
    check(history.recordEdit(project), "record with no name");
    check(history.undoStepName() == "Edit", "an unnamed step is called Edit");

    // --- the stack is capped, dropping the oldest -------------------------------
    {
        ProjectDocument capped = defaultProject();
        ProjectHistory shallow(3);
        shallow.reset(capped);
        for (int step = 1; step <= 5; ++step) {
            setTrackVolumeDb(capped, "Audio 1", static_cast<float>(-step));
            shallow.recordEdit(capped, "Step " + std::to_string(step));
        }
        check(shallow.undoDepth() == 3, "the stack is capped at its depth");
        check(shallow.undoStepName() == "Step 5", "the newest step is on top");

        // Three undos are all that remain; the first two steps were dropped.
        std::string capError;
        check(shallow.undo(capped, capError), "undo 1");
        check(shallow.undo(capped, capError), "undo 2");
        check(shallow.undo(capped, capError), "undo 3");
        check(!shallow.canUndo(), "no fourth undo");
        check(volumeOf(capped, "Audio 1") == -2.0f,
              "unwinding a capped stack lands on the oldest retained state, not the original");
    }

    // --- undo/redo on an empty history fails cleanly -----------------------------
    {
        ProjectDocument fresh = defaultProject();
        ProjectHistory empty;
        empty.reset(fresh);
        std::string emptyError;
        check(!empty.undo(fresh, emptyError), "undo on empty history fails");
        check(!emptyError.empty(), "and says why");
        emptyError.clear();
        check(!empty.redo(fresh, emptyError), "redo on empty history fails");
        check(!emptyError.empty(), "and says why");
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("project history OK\n");
    return 0;
}
