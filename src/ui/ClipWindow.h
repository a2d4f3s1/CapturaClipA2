#pragma once

#include <windows.h>

#include <string>

#include "app/Settings.h"
#include "capture/DibBuffer.h"
#include "doc/History.h"
#include "render/Renderer.h"
#include "tool/ToolState.h"
#include "ui/ColorPreview.h"
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
                ccl::app::Settings& settings, POINT position,
                const std::wstring& sourceTitle, LONGLONG releasedAt) noexcept;

    void Run() noexcept;

    // Sizes the window to the image without letting it grow past the work
    // area. A capture always fits by construction, but a picture opened from a
    // file can be any size at all.
    void ResizeToImage() noexcept;

private:
    // A pixel of the picture to keep where it is while the zoom changes, and
    // the place on the desktop to keep it. Held in both spaces at once: the
    // point comes from the picture, but what must not move is where it lands
    // on screen, and the window may have to move for that to stay true.
    struct ZoomAnchor {
        D2D1_POINT_2F image{};
        POINT screen{};
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void Draw() noexcept;
    void OnWheel(int notches, WPARAM keys, POINT client) noexcept;
    void OnKeyDown(WPARAM key) noexcept;
    // Runs whatever the key is bound to. Returns false when it is bound to
    // nothing, leaving the modal keys -- the size keys, the zoom digits, the
    // arrows -- to the handling below.
    bool RunShortcut(WPARAM key) noexcept;
    void OnLeftDown(POINT client) noexcept;
    void OnMouseMove(POINT client) noexcept;
    void OnLeftUp() noexcept;

    // Which gesture a button press amounts to, given the modifiers held. The
    // button is one of the kButton values in the implementation.
    ccl::app::DragGesture GestureFor(int button) const noexcept;
    // Starts whatever the button, with the modifiers held, is assigned to.
    // Returns false when that combination is assigned to nothing, leaving the
    // press to whoever else wants it.
    bool BeginDragCommand(int button) noexcept;
    void BeginScrollDrag() noexcept;
    void BeginWindowMove() noexcept;

    // Takes the window off the screen for the time set in the settings. It
    // comes back on a timer rather than when a key is released, because a
    // hidden window has no keyboard focus and would never hear the release.
    void HideTemporarily() noexcept;
    void StopHiding() noexcept;

    // Resizes the window to the zoomed image (bounded by the work area),
    // re-clamps the scroll offset and repaints. With an anchor, the picture is
    // also slid -- and the window moved, if sliding is not enough -- so that
    // the anchored pixel stays exactly where it was on the desktop. The window
    // is free to hang off the edge of the screen doing it; the anchor is the
    // thing being kept, and the anchored pixel is under the pointer, so some
    // of the window is always still in view.
    void ApplyZoom(const ZoomAnchor* anchor = nullptr) noexcept;
    // An anchor holding whatever is at that point of the window.
    ZoomAnchor AnchorAt(POINT client) const noexcept;
    // The corner of the visible area, and its middle.
    ZoomAnchor CornerZoomAnchor() const noexcept;
    ZoomAnchor CenterZoomAnchor() const noexcept;
    // The anchor for a turn of the wheel, which is the only zoom with a
    // pointer behind it. Following the pointer, a run of notches is one
    // gesture and keeps the point it started on: reading the position afresh
    // every notch would let the anchor creep with the smallest movement of
    // the hand, which is the one thing zooming to a point has to not do.
    ZoomAnchor WheelZoomAnchor(POINT client) noexcept;
    // The anchor for the keyboard presets and the menu. They have no pointer,
    // so the cursor setting cannot apply to them and they hold the middle.
    ZoomAnchor KeyZoomAnchor() const noexcept;
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
    // Puts the eyedropper away: removes the hook it holds over the whole
    // screen, hides the magnifier and goes back to the previous tool.
    void EndEyedropper() noexcept;
    // The eyedropper watches the mouse everywhere, which a capture cannot do:
    // over another process's window the system only honours a capture while a
    // button is already down, so a click there would reach that window instead
    // of being taken as a sample. A low-level hook sees every event and can
    // swallow the ones it uses.
    bool InstallEyedropperHook() noexcept;
    void RemoveEyedropperHook() noexcept;
    static LRESULT CALLBACK EyedropperHookProc(int code, WPARAM wParam,
                                               LPARAM lParam);
    void UpdateColorPreview() noexcept;
    void SetColorPreviewActive(bool active) noexcept;

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
    void OpenSettings() noexcept;
    // Takes the window's own contents as the new picture, at the size they are
    // being shown. Zooming in and then doing this is how a detail is enlarged
    // for real rather than just magnified on screen.
    void CaptureSelf() noexcept;
    // Closes this capture and starts the program again from the area
    // selection, as if it had just been launched.
    void Recapture() noexcept;
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

    // Which parts of a frame the chosen window style has. Without a title bar
    // the window supplies its own outline and resize grips; without a frame at
    // all it does not even do that.
    bool HasTitleBar() const noexcept;
    bool HasWindowBorder() const noexcept;
    // Width of that outline, in pixels: one, or none when frameless.
    int BorderWidth() const noexcept;
    // Outer window size that shows `content` pixels of image.
    SIZE WindowSizeFor(SIZE content) const noexcept;

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
    // Not const: the settings window edits these in place, and what can be
    // applied without a restart is applied here.
    ccl::app::Settings* settings_ = nullptr;

    ccl::view::ViewState view_;
    ccl::tool::ToolState tool_;
    ccl::doc::History history_;

    std::wstring sourceTitle_;
    bool saved_ = false;
    // Set when the capture is being thrown away on purpose, which suppresses
    // the automatic save that closing would otherwise do.
    bool discarding_ = false;

    bool moving_ = false;
    POINT dragOrigin_{};
    RECT windowOrigin_{};

    bool scrolling_ = false;
    POINT scrollOrigin_{};
    POINT scrollStart_{};

    // Set while the right button is down and assigned to a drag. A press that
    // never moved is still a click, and a click has to open the menu: it is
    // the only way to reach everything the keys do not cover.
    bool rightDragging_ = false;
    bool rightDragMoved_ = false;
    POINT rightDragStart_{};

    // Non-zero while the window is hidden waiting to come back.
    UINT_PTR hideTimer_ = 0;

    // The point a run of wheel notches is zooming around, and when the last
    // one arrived. A gap long enough to be a pause starts a new gesture.
    ZoomAnchor zoomAnchor_{};
    bool zoomAnchorValid_ = false;
    ULONGLONG lastZoomTick_ = 0;
    // Set while the window is being rearranged for a new zoom, so the WM_SIZE
    // that causes does not paint a frame of its own.
    bool applyingZoom_ = false;

    bool drawing_ = false;
    bool straightLine_ = false;
    // How many points of the straight line have settled. Everything past this
    // is the stretch still following the pointer.
    //
    // Pressing Shift again part way through settles the end it has reached and
    // carries on from there, which is how a line turns a corner without the
    // button being let go. One means only the start is settled.
    size_t fixedPoints_ = 1;
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
    // Magnified view of what the eyedropper is over. A single pixel cannot be
    // aimed at without it.
    ColorPreview colorPreview_;
    // Keeps the magnifier honest while the cursor is still: the hook only
    // reports movement, and what is under the cursor can change on its own.
    UINT_PTR previewTimer_ = 0;
    HHOOK eyedropperHook_ = nullptr;
    // One movement message in flight at a time, so a fast mouse cannot fill
    // the queue faster than the magnifier can redraw.
    bool previewPending_ = false;

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
