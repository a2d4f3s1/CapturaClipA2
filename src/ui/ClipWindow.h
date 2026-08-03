#pragma once

#include <windows.h>

#include <string>

#include "app/Settings.h"
#include "capture/DibBuffer.h"
#include "doc/History.h"
#include "render/Renderer.h"
#include "tool/ToolState.h"
#include "util/Timing.h"
#include "view/ViewState.h"

namespace ccl::doc {
class Document;
}

namespace ccl::render {
class D2DContext;
}

namespace ccl::ui {

// The window that holds a capture on top of everything else.
//
// It has no title bar and is always topmost -- both are fixed behaviour rather
// than settings. With no title bar there is nothing to drag, so window movement
// is bound to the middle button, and what the left button does depends on the
// selected tool.
class ClipWindow {
public:
    bool Create(ccl::render::D2DContext& context, ccl::doc::Document& document,
                const ccl::app::Settings& settings, POINT position,
                const std::wstring& sourceTitle, LONGLONG releasedAt) noexcept;

    void Run() noexcept;

    // Sizes the window to the image without letting it grow past the work
    // area. A capture always fits by construction, but a picture opened from a
    // file can be any size at all.
    void ResizeToImage() noexcept;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void Draw() noexcept;
    void OnWheel(int notches, WPARAM keys) noexcept;
    void OnKeyDown(WPARAM key) noexcept;
    void OnLeftDown(POINT client) noexcept;
    void OnMouseMove(POINT client) noexcept;
    void OnLeftUp() noexcept;

    // Resizes the window to the zoomed image (bounded by the work area),
    // re-clamps the scroll offset and repaints.
    void ApplyZoom() noexcept;
    void ApplyOpacity() noexcept;
    void FitToImage() noexcept;
    void UpdateTitle() noexcept;
    void UpdateCursor() noexcept;

    void ShowContextMenu(POINT screen) noexcept;
    // Styling menu for text being edited, reached by right-clicking the editor.
    void ShowTextStyleMenu(POINT screen) noexcept;
    void OnCommand(int command) noexcept;

    void ChooseColorFromPicker() noexcept;
    // Reads the colour out of the captured image at that point.
    bool PickColorAt(POINT client) noexcept;
    void SelectTool(ccl::tool::Tool tool) noexcept;

    void SaveAs() noexcept;
    void CopyImage() noexcept;
    void OpenFile() noexcept;
    void PasteImage() noexcept;
    // Replaces the picture being shown, resetting everything tied to the old
    // one: annotations, history, zoom and the window size.
    void ReplaceImage(ccl::capture::DibBuffer image,
                      const std::wstring& title) noexcept;
    void Undo() noexcept;
    void Redo() noexcept;

    // Commits anything still in the editor and returns the picture with every
    // annotation drawn into it, ready to be reshaped.
    ccl::capture::DibBuffer FlattenForTransform() noexcept;
    // Puts a reshaped picture in place of the current one. The annotations are
    // already part of it by then, so they are dropped -- a stroke placed
    // against the old shape has no meaning against the new one. Undoable,
    // including the pixels.
    void ApplyTransform(ccl::capture::DibBuffer transformed) noexcept;
    void CropToSelection() noexcept;
    // Joins the image on the clipboard onto the right or the bottom.
    void ConcatenateClipboard(bool toRight) noexcept;

    // Saves automatically before the capture is discarded, unless the image has
    // already been saved or Shift is held to skip it.
    void AutoSaveBeforeClosing() noexcept;

    // Client pixels to image coordinates, undoing zoom and scroll.
    D2D1_POINT_2F ToImage(POINT client) const noexcept;

    // Text is typed into a real edit control positioned over the image rather
    // than handled key by key. Composing Japanese means driving an IME, and the
    // edit control already does that correctly.
    void BeginTextAt(POINT client) noexcept;
    // Creates the edit control itself; the position and the state around it are
    // set up by whichever of the two entry points called in.
    void OpenEditor(POINT client) noexcept;
    void CommitText() noexcept;
    void CancelText() noexcept;
    void DestroyEditor() noexcept;
    // Grows the edit box to fit what has been typed, so added lines are not
    // scrolled out of sight.
    void ResizeEditor() noexcept;
    void TurnOffIme() noexcept;
    bool EditingText() const noexcept { return editor_ != nullptr; }

    // Index of the text annotation under a point, or npos. Clicking existing
    // text reopens it for editing rather than starting a second one on top.
    size_t FindTextAt(D2D1_POINT_2F image) noexcept;
    // Reopens the text at an index, as opposed to starting a new one.
    void EditTextAt(size_t index) noexcept;
    // Applies the current styling to the selection, or to the whole text when
    // asked. With nothing selected the control applies it to what is typed
    // next, which is how styling ahead of typing works.
    void ApplyCharFormat(bool wholeText) noexcept;
    // Writes a single attribute, leaving everything else about the run alone.
    void ApplyTextEffect(DWORD mask, DWORD effect, bool enabled) noexcept;
    void ApplyTextColor() noexcept;
    // Scales the font size of the selection, or of what is typed next.
    void StepTextSize(int steps) noexcept;
    void SetTextFont(const std::wstring& family) noexcept;
    HMENU BuildFontMenu() noexcept;
    // Clears the indentation and paragraph spacing rich edit applies by
    // default, which do not exist in the drawn result.
    void ApplyParagraphFormat() noexcept;
    // Reads back the per-character styling as ranges.
    std::vector<ccl::doc::TextRun> ReadRuns(int length) noexcept;

    float WidthForPressure(float pressure) const noexcept;
    void BeginStroke(POINT client, float pressure) noexcept;
    void ContinueStroke(POINT client, float pressure) noexcept;
    void EndStroke() noexcept;
    void EraseAt(POINT client) noexcept;

    // Pen input arrives as pointer messages, which carry pressure. Returns
    // false for anything that is not a pen so it falls through to the ordinary
    // mouse handling.
    bool HandlePointerMessage(UINT msg, WPARAM wParam) noexcept;

    SIZE ContentSize() const noexcept;
    SIZE ViewportSize() const noexcept;
    void ClampScroll() noexcept;

    // Obscures the selected area. Kept as an annotation so it can be undone.
    void ApplyEffectToSelection(ccl::doc::EffectKind kind) noexcept;
    // Adjusts the effect just placed, so its strength can be judged against the
    // result rather than guessed at in advance.
    void StepEffectStrength(int steps) noexcept;
    bool HasSelection() const noexcept;
    D2D1_RECT_F SelectionRect() const noexcept;

    // True when the left button should scroll rather than use the active tool.
    bool ScrollingWithLeftButton() const noexcept;
    // True when the brush size outline should follow the cursor.
    bool ShowsBrushCursor() const noexcept;
    void TrackMouseLeave() noexcept;

    HWND hwnd_ = nullptr;
    ccl::render::Renderer renderer_;
    ccl::render::D2DContext* context_ = nullptr;
    ccl::doc::Document* document_ = nullptr;
    const ccl::app::Settings* settings_ = nullptr;

    ccl::view::ViewState view_;
    ccl::tool::ToolState tool_;
    ccl::doc::History history_;

    std::wstring sourceTitle_;
    bool saved_ = false;

    bool moving_ = false;
    POINT dragOrigin_{};
    RECT windowOrigin_{};

    bool scrolling_ = false;
    POINT scrollOrigin_{};
    POINT scrollStart_{};

    bool drawing_ = false;
    bool straightLine_ = false;
    ccl::doc::Stroke activeStroke_;

    // Set while a pen is in contact, so pointer and mouse messages for the
    // same gesture are not both acted on.
    bool penActive_ = false;
    // Tool to restore after drawing with the eraser end of a pen.
    ccl::tool::Tool toolBeforePenEraser_ = ccl::tool::Tool::Pen;
    bool penEraserActive_ = false;

    bool erasing_ = false;
    bool erasedAny_ = false;

    // Eyedropper drag: the pointer is captured so the sample can come from
    // anywhere on screen, including other applications.
    bool sampling_ = false;

    // Effect whose strength the size keys currently adjust: the one just
    // placed, until something else is done.
    size_t adjustingEffectIndex_ = static_cast<size_t>(-1);

    // Rectangular selection, in image coordinates.
    bool selecting_ = false;
    bool hasSelection_ = false;
    D2D1_POINT_2F selectionAnchor_{};
    D2D1_POINT_2F selectionCursor_{};

    bool spaceHeld_ = false;
    // Tool to return to once the eyedropper has taken a sample.
    ccl::tool::Tool toolBeforeEyedropper_ = ccl::tool::Tool::Pen;

    POINT lastCursor_{};
    bool cursorInside_ = false;
    bool trackingLeave_ = false;

    HWND editor_ = nullptr;
    HFONT editorFont_ = nullptr;
    HBRUSH editorBackground_ = nullptr;
    // Where the text will sit, in image coordinates.
    float editorX_ = 0.0f;
    float editorY_ = 0.0f;
    // Set while re-editing existing text, so that committing restores its
    // properties rather than applying the current ones.
    bool editingExisting_ = false;
    ccl::doc::TextAnnotation editingOriginal_;

    // Non-zero while a menu or the palette is up. Those take focus away from
    // the editor, which would otherwise be read as clicking away and commit
    // the text out from under the user.
    int suppressCommitDepth_ = 0;

    // Dragging existing text to reposition it. A press that does not move far
    // enough is treated as a click and opens the text for editing instead.
    // Text under the pointer, outlined so it is clear what a click would edit.
    size_t hoveredTextIndex_ = static_cast<size_t>(-1);

    size_t movingTextIndex_ = static_cast<size_t>(-1);
    POINT textDragStart_{};
    float textDragOriginX_ = 0.0f;
    float textDragOriginY_ = 0.0f;
    bool textDragMoved_ = false;

    LONGLONG releasedAt_ = 0;
    bool reportedFirstFrame_ = false;
    ccl::timing::FrameStats drawStats_;
};

}  // namespace ccl::ui
