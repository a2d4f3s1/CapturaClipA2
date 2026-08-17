#include "doc/History.h"

#include <utility>

namespace ccl::doc {
namespace {

// Moves one step's contents into the document and the document's into the step,
// so the caller's state and the step trade places. The picture is only touched
// when the step carries one; the rest of the time it belongs to no step and has
// to stay where it is.
void Exchange(HistoryStep& step, AnnotationList& annotations,
              ccl::capture::DibBuffer& image, SelectionShapes& selection,
              ccl::tool::Tool& tool) {
    std::swap(step.annotations, annotations);
    std::swap(step.tool, tool);
    if (step.image.IsValid()) {
        ccl::capture::DibBuffer held = std::move(step.image);
        step.image = std::move(image);
        image = std::move(held);
    }
    // Traded rather than assigned, so that going the other way puts back what
    // was there. The flag rides along with it: a step that once carried a
    // selection still does after it has been stepped over.
    if (step.selectionChanged) {
        std::swap(step.selection, selection);
    }
}

}  // namespace

void History::Record(const AnnotationList& before, ccl::tool::Tool tool) {
    HistoryStep step;
    step.annotations = before;
    step.tool = tool;
    undo_.push_back(std::move(step));
    // A fresh edit invalidates anything that was undone.
    redo_.clear();
}

void History::RecordWithImage(const AnnotationList& annotations,
                              const ccl::capture::DibBuffer& image,
                              const SelectionShapes& selection,
                              ccl::tool::Tool tool) {
    HistoryStep step;
    step.annotations = annotations;
    step.image = image.Clone();
    step.selectionChanged = true;
    step.selection = selection;
    step.tool = tool;
    undo_.push_back(std::move(step));
    redo_.clear();
}

bool History::Undo(AnnotationList& current, ccl::capture::DibBuffer& image,
                   SelectionShapes& selection, ccl::tool::Tool& tool) {
    if (undo_.empty()) {
        return false;
    }
    HistoryStep step = std::move(undo_.back());
    undo_.pop_back();
    Exchange(step, current, image, selection, tool);
    redo_.push_back(std::move(step));
    return true;
}

bool History::Redo(AnnotationList& current, ccl::capture::DibBuffer& image,
                   SelectionShapes& selection, ccl::tool::Tool& tool) {
    if (redo_.empty()) {
        return false;
    }
    HistoryStep step = std::move(redo_.back());
    redo_.pop_back();
    Exchange(step, current, image, selection, tool);
    undo_.push_back(std::move(step));
    return true;
}

void History::RecordSelection(const AnnotationList& annotations,
                              const SelectionShapes& selection,
                              ccl::tool::Tool tool) {
    HistoryStep step;
    step.annotations = annotations;
    step.selectionChanged = true;
    step.selection = selection;
    step.tool = tool;
    undo_.push_back(std::move(step));
    redo_.clear();
}

bool History::NextUndoChangesImage() const noexcept {
    return !undo_.empty() && undo_.back().image.IsValid();
}

bool History::NextRedoChangesImage() const noexcept {
    return !redo_.empty() && redo_.back().image.IsValid();
}

}  // namespace ccl::doc
