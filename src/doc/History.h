#pragma once

#include <vector>

#include "doc/Annotation.h"

namespace ccl::doc {

// Undo / redo for annotations.
//
// Whole-list snapshots rather than individual commands: annotations are vector
// data, so a snapshot is a few hundred bytes per stroke, and every operation --
// including erasing, which splits and removes strokes -- becomes undoable
// without needing an inverse for each one. There is no step limit.
class History {
public:
    // Call with the state as it was *before* an edit.
    void Record(const AnnotationList& before);

    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }

    bool Undo(AnnotationList& current);
    bool Redo(AnnotationList& current);

private:
    std::vector<AnnotationList> undo_;
    std::vector<AnnotationList> redo_;
};

}  // namespace ccl::doc
