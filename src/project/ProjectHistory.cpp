#include "project/ProjectHistory.h"

#include "project/EditOperations.h"

namespace neuracoust::daw {

namespace {

/// Routing normalization runs before every snapshot so that two documents which
/// differ only in derived routing state compare equal and do not record a step.
std::string snapshotOf(const ProjectDocument& project) {
    ProjectDocument copy = project;
    normalizeProjectRouting(copy);
    return serializeProject(copy);
}

const char* kDefaultStepName = "Edit";

} // namespace

ProjectHistory::ProjectHistory(size_t maxDepth)
    : maxDepth_(maxDepth == 0 ? 1 : maxDepth) {}

void ProjectHistory::reset(const ProjectDocument& project) {
    currentSnapshot_ = snapshotOf(project);
    savedSnapshot_ = currentSnapshot_;
    undoSnapshots_.clear();
    undoStepNames_.clear();
    redoSnapshots_.clear();
    redoStepNames_.clear();
}

bool ProjectHistory::recordEdit(const ProjectDocument& project, const std::string& stepName) {
    const std::string snapshot = snapshotOf(project);

    // First edit before any reset(): adopt it silently rather than inventing a step.
    if (currentSnapshot_.empty()) {
        currentSnapshot_ = snapshot;
        return false;
    }
    if (snapshot == currentSnapshot_) {
        return false;
    }

    undoSnapshots_.push_back(currentSnapshot_);
    undoStepNames_.push_back(stepName.empty() ? kDefaultStepName : stepName);

    while (undoSnapshots_.size() > maxDepth_) {
        undoSnapshots_.erase(undoSnapshots_.begin());
        undoStepNames_.erase(undoStepNames_.begin());
    }

    redoSnapshots_.clear();
    redoStepNames_.clear();
    currentSnapshot_ = snapshot;
    return true;
}

void ProjectHistory::markSaved(const ProjectDocument& project) {
    currentSnapshot_ = snapshotOf(project);
    savedSnapshot_ = currentSnapshot_;
}

bool ProjectHistory::isDirty() const {
    return currentSnapshot_ != savedSnapshot_;
}

bool ProjectHistory::canUndo() const {
    return !undoSnapshots_.empty();
}

bool ProjectHistory::canRedo() const {
    return !redoSnapshots_.empty();
}

size_t ProjectHistory::undoDepth() const {
    return undoSnapshots_.size();
}

size_t ProjectHistory::redoDepth() const {
    return redoSnapshots_.size();
}

std::string ProjectHistory::undoStepName() const {
    return undoStepNames_.empty() ? std::string{} : undoStepNames_.back();
}

std::string ProjectHistory::redoStepName() const {
    return redoStepNames_.empty() ? std::string{} : redoStepNames_.back();
}

bool ProjectHistory::undo(ProjectDocument& project, std::string& error) {
    if (undoSnapshots_.empty()) {
        error = "nothing to undo";
        return false;
    }

    const std::string snapshot = undoSnapshots_.back();
    ProjectDocument restored;
    if (!deserializeProject(snapshot, restored, error)) {
        return false;
    }

    redoSnapshots_.push_back(currentSnapshot_);
    redoStepNames_.push_back(undoStepNames_.back());
    undoSnapshots_.pop_back();
    undoStepNames_.pop_back();

    currentSnapshot_ = snapshot;
    project = std::move(restored);
    return true;
}

bool ProjectHistory::redo(ProjectDocument& project, std::string& error) {
    if (redoSnapshots_.empty()) {
        error = "nothing to redo";
        return false;
    }

    const std::string snapshot = redoSnapshots_.back();
    ProjectDocument restored;
    if (!deserializeProject(snapshot, restored, error)) {
        return false;
    }

    undoSnapshots_.push_back(currentSnapshot_);
    undoStepNames_.push_back(redoStepNames_.back());
    redoSnapshots_.pop_back();
    redoStepNames_.pop_back();

    currentSnapshot_ = snapshot;
    project = std::move(restored);
    return true;
}

} // namespace neuracoust::daw
