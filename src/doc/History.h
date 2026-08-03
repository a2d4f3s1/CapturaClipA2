#pragma once

#include <vector>

#include "capture/DibBuffer.h"
#include "doc/Annotation.h"

namespace ccl::doc {

// Undo / redo for the document.
//
// Whole-list snapshots rather than individual commands: annotations are vector
// data, so a snapshot is a few hundred bytes per stroke, and every operation --
// including erasing, which splits and removes strokes -- becomes undoable
// without needing an inverse for each one. There is no step limit.
//
// A transform (crop, rotate, concatenate) also replaces the picture itself, so
// those steps carry a copy of the pixels as well. That is megabytes rather than
// bytes, which is why it is only done for the steps that need it.
struct HistoryStep {
    AnnotationList annotations;
    // Empty unless this step changed the picture.
    ccl::capture::DibBuffer image;

    HistoryStep() = default;
    HistoryStep(HistoryStep&&) noexcept = default;
    HistoryStep& operator=(HistoryStep&&) noexcept = default;
};

class History {
public:
    // Call with the state as it was *before* an edit.
    void Record(const AnnotationList& before);
    // For an edit that replaces the picture. `before` is copied.
    void RecordWithImage(const AnnotationList& annotations,
                         const ccl::capture::DibBuffer& image);

    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }

    // `image` is exchanged with the stored one when the step carries a picture,
    // and left alone otherwise.
    bool Undo(AnnotationList& current, ccl::capture::DibBuffer& image);
    bool Redo(AnnotationList& current, ccl::capture::DibBuffer& image);

    // True when the step that would be undone or redone also swaps the picture,
    // so the caller knows to resize and rebuild what depends on it.
    bool NextUndoChangesImage() const noexcept;
    bool NextRedoChangesImage() const noexcept;

private:
    std::vector<HistoryStep> undo_;
    std::vector<HistoryStep> redo_;
};

}  // namespace ccl::doc
