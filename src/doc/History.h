#pragma once

#include <vector>

#include "capture/DibBuffer.h"
#include "doc/Annotation.h"
#include "doc/Selection.h"
#include "tool/Tool.h"

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
    // Set when this step changed what was selected rather than what was drawn.
    // A flag rather than an empty list standing for "no change": having nothing
    // selected is itself a state worth stepping back to.
    bool selectionChanged = false;
    SelectionShapes selection;
    // The tool the edit was made with. Every step carries one, unlike the
    // picture: stepping back to a mark without going back to the tool that
    // made it leaves what changed invisible -- the outline of a selected area
    // is only drawn by the tool that selects.
    ccl::tool::Tool tool = ccl::tool::Tool::View;

    HistoryStep() = default;
    HistoryStep(HistoryStep&&) noexcept = default;
    HistoryStep& operator=(HistoryStep&&) noexcept = default;
};

class History {
public:
    // Call with the state as it was *before* an edit.
    void Record(const AnnotationList& before, ccl::tool::Tool tool);
    // For an edit that replaces the picture. `before` is copied. The selected
    // area goes in as well: the picture it was measured against comes back with
    // the step, so the area means what it did again.
    void RecordWithImage(const AnnotationList& annotations,
                         const ccl::capture::DibBuffer& image,
                         const SelectionShapes& selection,
                         ccl::tool::Tool tool);
    // For a change to the selected area, which draws nothing. Building an area
    // up piece by piece is work like any other, and a piece taken away by
    // mistake should be recoverable without starting again.
    void RecordSelection(const AnnotationList& annotations,
                         const SelectionShapes& selection,
                         ccl::tool::Tool tool);

    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }

    // `image` is exchanged with the stored one when the step carries a picture,
    // and left alone otherwise. So is `selection`. `tool` is always exchanged.
    bool Undo(AnnotationList& current, ccl::capture::DibBuffer& image,
              SelectionShapes& selection, ccl::tool::Tool& tool);
    bool Redo(AnnotationList& current, ccl::capture::DibBuffer& image,
              SelectionShapes& selection, ccl::tool::Tool& tool);

    // True when the step that would be undone or redone also swaps the picture,
    // so the caller knows to resize and rebuild what depends on it.
    bool NextUndoChangesImage() const noexcept;
    bool NextRedoChangesImage() const noexcept;

private:
    std::vector<HistoryStep> undo_;
    std::vector<HistoryStep> redo_;
};

}  // namespace ccl::doc
