#pragma once

#include <windows.h>

#include <string>

#include "app/Settings.h"
#include "capture/DibBuffer.h"
// By value in SetAside, so the whole definition is needed rather than the
// forward declaration that the document pointer alone would want.
#include "doc/Document.h"
#include "doc/History.h"
#include "doc/Selection.h"
#include "render/Renderer.h"
#include "render/SelectionGeometry.h"
#include "tool/ToolState.h"
#include "ui/ColorPreview.h"
#include "ui/ToolCursors.h"
#include "util/Timing.h"
#include "view/ViewState.h"

// The Text Object Model view of a rich edit control. Declared here so the
// header does not have to pull in <tom.h>.
struct ITextDocument;

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
    // Puts the picture a replacement set aside back, history and all. Reached
    // from Undo once the marks on the current picture are exhausted.
    void RestoreSetAside() noexcept;

    // Commits anything still in the editor and returns the picture with every
    // annotation drawn into it, ready to be reshaped.
    ccl::capture::DibBuffer FlattenForTransform() noexcept;
    // Puts a reshaped picture in place of the current one. The annotations are
    // already part of it by then, so they are dropped -- a stroke placed
    // against the old shape has no meaning against the new one. Undoable,
    // including the pixels.
    void ApplyTransform(ccl::capture::DibBuffer transformed) noexcept;
    void CropToSelection() noexcept;
    // Turns the picture by an angle chosen in a dialog. Kept apart from the
    // quarter turns because it resamples: those move pixels about and lose
    // nothing, this one cannot.
    void RotateFreely() noexcept;
    void OpenSettings() noexcept;
    // Takes the window's own contents as the new picture, at the size they are
    // being shown. Zooming in and then doing this is how a detail is enlarged
    // for real rather than just magnified on screen.
    void CaptureSelf() noexcept;
    // Closes this capture and starts the program again from the area
    // selection, as if it had just been launched.
    void Recapture() noexcept;
    // Joins the image on the clipboard onto the right or the bottom.
    void ConcatenateClipboard() noexcept;

    // Why the picture is about to go away, which decides how much the program
    // is allowed to do about a save that fails.
    enum class Departure {
        Window,      // this window is being closed or its picture replaced
        SessionEnd,  // Windows is ending the session and will not wait long
    };

    // Saves automatically before the capture is discarded, unless the image has
    // already been saved or Shift is held to skip it.
    //
    // Returns false when the picture could not be written and the window should
    // stay open rather than take it with it. Always true when the session is
    // ending: refusing to go would not save anything and would hold up the
    // shutdown.
    bool AutoSaveBeforeClosing(Departure departure) noexcept;

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

    // What the next piece of text will be given. Everything that needs the
    // size or the face goes through these, so that a value changed during the
    // session cannot be read back out of the settings file by mistake -- the
    // two used to be the same store, and there were a dozen places reading it.
    float CurrentTextSize() const noexcept { return tool_.textFontSize; }
    const std::wstring& CurrentTextFont() const noexcept {
        return tool_.textFontFamily;
    }

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
    // Resizes the text the pointer is over, which is the one drawn with an
    // outline round it. A run of steps counts as one thing done: the state
    // before the first is what undo returns to.
    void ResizeHoveredText(int steps) noexcept;

    // A number given outright rather than stepped with the bracket keys. The
    // box appears where the menu item was, on this window -- there is no
    // dialog. Acts on the piece being pointed at, or, with nothing pointed at,
    // on what the next piece will be given.
    // The values the little box that opens under the pointer can edit. Kept
    // beside a table of what each one allows (`kNumberFields`), so that adding
    // one is adding a row rather than another arm to four separate branches.
    enum class NumberKind {
        FontSize,
        OutlineWidth,
        ShadowLength,
        ShadowOpacity,
        kCount
    };
    // What one of those values allows, and whether changing it means the
    // glyphs have to be traced again.
    struct NumberField {
        float low;
        float high;
        bool reshapes;
    };
    static const NumberField& FieldFor(NumberKind kind) noexcept;

    void BeginNumberEntry(NumberKind kind) noexcept;
    // Reads what has been typed and shows it on the text as it goes.
    void UpdateNumberEntry() noexcept;
    // `keep` false puts back what was there before the box opened.
    void EndNumberEntry(bool keep) noexcept;
    void ApplyNumber(float value) noexcept;
    static LRESULT CALLBACK NumberProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR reference);
    // Paints the whole of one piece of text, ranges included. Whole rather
    // than partly: outside the editor there is no selection to aim at.
    void PaintText(size_t index, const ccl::doc::Color& colour) noexcept;
    // The text being pointed at, which is what a change made from outside the
    // editor lands on -- a colour, a size, an outline. -1 for none.
    size_t HoveredTextTarget() const noexcept;
    // Works out afresh what is under the pointer. Undo and redo drop what was
    // being pointed at, and nothing puts it back until the pointer moves, so a
    // menu opened in between would otherwise find no target while the text sits
    // right under the cursor.
    void RefreshHoveredText() noexcept;
    // Turns the outline or the shadow on the text being pointed at, or -- with
    // nothing pointed at -- on whatever is typed next.
    void ToggleTextOutline() noexcept;
    void ToggleTextShadow() noexcept;
    // Which way the shadow is thrown, on the text being pointed at or -- with
    // nothing pointed at -- on whatever is typed next. 8 leaves it underneath,
    // where only its spread shows.
    void SetShadowDirection(int way) noexcept;
    // The colour the shadow is cast in. How strong it is stays as it was: the
    // palette has nowhere to show a strength, so that is typed separately.
    // `owner` is what the palette is opened under -- see ChooseOutlineColor.
    void ChooseShadowColor(HWND owner) noexcept;
    // The same, for the edge. Kept apart rather than parameterised: the shadow
    // has to hold its strength aside while a colour is mixed, and the edge has
    // no strength to hold.
    //
    // `owner` is what the palette is opened under. The decoration panel passes
    // itself, so that the palette counts as one of its own and does not read
    // as "the pointer went elsewhere".
    void ChooseOutlineColor(HWND owner) noexcept;

    // The eight edge-and-shadow settings on one panel, so that dressing a
    // piece of text does not mean opening the menu once per setting. Every row
    // goes back through the call the menu would have made, so there is one
    // rule for what a change means and one place that records it.
    void OpenDecorPanel() noexcept;
    // A value from the panel, put through the same apply as the number box.
    void ApplyDecorNumber(NumberKind kind, float value) noexcept;
    // Flips one of the four switches on the text being hovered. Says whether
    // there was a text to flip it on: with none, the caller carries on and
    // changes what the next piece of text will be given instead.
    //
    // The whole piece takes the new value, ranges included. Outside the editor
    // there is no selection to aim at, so there is nothing finer to act on --
    // the same bargain the colour already makes.
    bool StyleHoveredText(bool ccl::doc::TextAnnotation::* whole,
                          bool ccl::doc::TextRun::* part) noexcept;
    // The same for the font, which is a name rather than a switch.
    bool RefontHoveredText(const std::wstring& family) noexcept;
    void SetTextFont(const std::wstring& family) noexcept;
    // Which family a change would be replacing -- what is pointed at, else the
    // selection in the box, else what the next piece will be given. Empty when
    // nothing is in force, which is what a selection spanning two faces means.
    std::wstring FontInForce() const noexcept;
    // One place that decides what a chosen family lands on, so the menu and the
    // picker cannot drift apart about it.
    void ApplyFontChoice(const std::wstring& family) noexcept;
    // The installed families as a window with a box to type into, because
    // several hundred of them as menu columns fill the screen and a menu item
    // cannot hold anything to type into.
    void OpenFontPicker() noexcept;
    // Clears the indentation and paragraph spacing rich edit applies by
    // default, which do not exist in the drawn result.
    // Pins the line pitch to what the picture will use for text of this size.
    // The size is the largest in the box, so the tallest line is not cut off.
    void ApplyParagraphFormat(const ccl::doc::TextAnnotation& shown) noexcept;
    // What is in the editor, shaped like the annotation it will become, so it
    // can be measured with the same code that will draw it.
    ccl::doc::TextAnnotation EditorSnapshot() noexcept;
    // Reads back the per-character styling as ranges.
    std::vector<ccl::doc::TextRun> ReadRuns(int length) noexcept;
    // The same, read through the Text Object Model, which leaves the selection
    // where it is. False if the control would not answer, in which case the
    // caller falls back to walking the selection.
    bool ReadRunsByTom(int length,
                       std::vector<ccl::doc::TextRun>& runs) noexcept;
    // The original way: selects each character in turn and asks about it. Kept
    // for when there is no Text Object Model to be had.
    std::vector<ccl::doc::TextRun> ReadRunsBySelection(int length) noexcept;

    float WidthForPressure(float pressure) const noexcept;
    void BeginStroke(POINT client, float pressure) noexcept;
    void ContinueStroke(POINT client, float pressure) noexcept;
    void EndStroke() noexcept;
    // Puts an arrowhead on the line being drawn, where it has got to. Does
    // nothing unless a line is being drawn and has somewhere to point: the
    // first point of a stroke has nothing behind it to give it a direction.
    void InsertArrowhead() noexcept;
    // Turns the head just placed, in steps of the angle set for it. Points at
    // the line being drawn, or at the one just finished.
    void TurnArrowhead(int steps) noexcept;
    // True while there is a head for the arrow keys to turn.
    bool HasAdjustableArrow() const noexcept;
    // Forgets the line just drawn, which is what makes R and the arrow keys
    // stop pointing at it. Called wherever something else has been done.
    void ForgetRecentStroke() noexcept;
    // The stroke R and the arrow keys act on, or null when there is none. The
    // line being drawn takes precedence over the one just finished.
    const ccl::doc::Stroke* RecentStroke() const noexcept;
    // For callers that go on to change it: taking this counts as having changed
    // the picture, so asking a question does not go through here.
    ccl::doc::Stroke* MutableRecentStroke() noexcept;
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
    // Puts the current colour down over the selected area. `width` of zero
    // fills it, anything else draws round its edge at that width; `opacity`
    // tells the plain entries from the highlighter ones.
    void PaintSelection(float opacity, float width) noexcept;
    // The selection broken into the separate patches it covers, so that paint
    // laid over several places can be rubbed out one place at a time.
    //
    // Patches that touch are kept together: laid down separately, the overlap
    // would take the colour twice, and a highlighter that darkens where it
    // crosses itself is not a highlighter.
    std::vector<ccl::doc::SelectionShapes> SelectionPieces() const noexcept;
    // Adjusts the effect just placed, so its strength can be judged against the
    // result rather than guessed at in advance.
    void StepEffectStrength(int steps) noexcept;
    bool HasSelection() const noexcept;
    // Smallest rectangle holding the selection. Only meaningful when there is
    // one, which is what HasSelection answers: an area taken apart again
    // reports a box of infinities rather than an empty one.
    D2D1_RECT_F SelectionBounds() const noexcept;
    // The pieces as they stand: those settled on, plus the one being dragged
    // out, if any.
    ccl::doc::SelectionShapes CurrentShapes() const noexcept;
    // Folds those pieces into the shape everything else is asked of. Called
    // wherever the pieces change, rather than on every frame.
    void RefreshSelection() noexcept;
    void ClearSelection() noexcept;
    // True when the selection is one rectangle and nothing else, which is what
    // the operations that can only produce a rectangle are offered for.
    bool SelectionIsSingleRect() const noexcept;
    // Tools whose business is the selected area. They share it: moving between
    // them keeps what is selected, since picking a different way to draw the
    // same area is not finishing with it.
    static bool IsSelectionTool(ccl::tool::Tool tool) noexcept;
    // Tools whose business is the things drawn on the picture rather than an
    // area of it. Kept apart from the pair above: the two kinds of selecting
    // do not share what they hold, and switching between kinds is finishing
    // with one of them.
    static bool IsObjectTool(ccl::tool::Tool tool) noexcept;
    // The box a piece of annotation occupies, in image coordinates. False when
    // it has no extent to speak of -- an empty stroke, text that cannot be
    // measured.
    bool AnnotationBounds(const ccl::doc::Annotation& annotation,
                          D2D1_RECT_F& bounds) noexcept;
    // True when the point falls on the text, allowing for how far it is turned.
    // The point is turned back rather than the box being turned forward: a
    // turned box is no longer a box, and the comparison would need a polygon.
    // `slack` widens the box all round, in picture units, for the sake of a
    // press that has to be aimed by hand.
    bool TextHit(const ccl::doc::TextAnnotation& text, D2D1_POINT_2F at,
                 float slack = 0.0f) noexcept;
    // The text's box as it actually sits -- corners, middle and edge midpoints,
    // turned. Used to ask whether a band drawn on the picture reaches it.
    bool TextOutlinePoints(const ccl::doc::TextAnnotation& text,
                           std::vector<D2D1_POINT_2F>& out) noexcept;
    // True when the band just dragged out reaches any part of the annotation.
    // Taken at the annotation's own points rather than at its box, so that a
    // lasso drawn between two strokes does not pick up both.
    // `slack` reaches that far past the annotation, in picture units. The band
    // passes zero -- what it encloses is what the eye enclosed -- and only a
    // press asks for room to be off by a little.
    bool AnnotationTouched(const ccl::doc::SelectionShapes& band,
                           const ccl::doc::Annotation& annotation,
                           float slack) noexcept;
    // Folds the band into what is already picked, the way the modifier keys
    // asked for, and settles the result.
    // How far past a piece a press still takes hold of it, in screen pixels.
    // From the settings, so that widening the reach widens the mark drawn
    // round what is picked with it -- the two are drawn and tested from this
    // one number, which is what keeps them from disagreeing.
    float GrabSlack() const noexcept;
    // Everything a press at this point would reach, listed from the piece on
    // top downwards. `ObjectAt` is the first of these; this is what repeated
    // presses in the same place walk through.
    void ObjectsAt(D2D1_POINT_2F image, std::vector<size_t>& out) noexcept;
    void ApplyObjectBand(ccl::doc::SelectionOp op) noexcept;
    // Puts a step on the history when the picked set came out different from
    // what is passed in. Called after the change, with the set as it was.
    void RecordPickedChange(const std::vector<unsigned int>& before) noexcept;
    // Adds or takes out one piece, for a modified press that turned out to be
    // a click rather than a band.
    void PickOne(unsigned int id, ccl::doc::SelectionOp op) noexcept;
    void ClearPicked() noexcept;
    // The topmost piece under a point, searched from the front as the eye
    // reads it. `size_t(-1)` when the point is over nothing. Reaches a little
    // way past the ink -- a line three pixels wide is not a target anyone can
    // hit -- and the cursor asks this same question, so that a four-way arrow
    // and a press that takes hold can never disagree.
    size_t ObjectAt(D2D1_POINT_2F image) noexcept;
    // Takes hold of what is picked, so that a drag moves it. The originals are
    // kept whole rather than as a starting offset: a piece is moved from where
    // it was when the button went down, not by however far the last message
    // happened to be from the one before it.
    void BeginPickedDrag(POINT client) noexcept;
    void ContinuePickedDrag(POINT client) noexcept;
    void EndPickedDrag() noexcept;
    // Moves what is picked through the stack. `toward` is +1 for the front and
    // -1 for the back; `allTheWay` sends it past everything rather than one
    // place. Several pieces keep their order relative to each other, so a
    // group sent forward arrives looking the same as it left.
    void ReorderPicked(int toward, bool allTheWay) noexcept;
    // Asks how far to turn what is picked, showing the result on the picture
    // while the angle is chosen, as turning the whole picture does.
    void RotatePicked() noexcept;
    // Puts the pieces back as they were and turns them by `degrees` about
    // `about`. Rebuilt from the copies every time rather than turned again and
    // again, so that walking the angle up and back down lands exactly where it
    // started.
    void ApplyPickedTurn(const std::vector<ccl::doc::Annotation>& originals,
                         D2D1_POINT_2F about, float degrees) noexcept;
    // The box round everything picked, in image coordinates.
    bool PickedBounds(D2D1_RECT_F& bounds) noexcept;
    // Drops anything picked that no longer exists, which is what undo and the
    // eraser can leave behind.
    void PrunePicked() noexcept;
    // The tool an undo step should remember. While the eyedropper is armed it
    // is the tool underneath, since the eyedropper is somewhere the program
    // passes through rather than somewhere it is.
    ccl::tool::Tool ToolForHistory() const noexcept;
    // Puts a tool back as part of stepping through the history. Deliberately
    // not SelectTool: that throws away a selected area on the way out of the
    // tool that made it, which would undo the area the same step just restored.
    void RestoreTool(ccl::tool::Tool tool) noexcept;
    // Adds a point to the lasso being dragged, no nearer the last one than the
    // spacing allows, and coarsens that spacing once the run gets long.
    void ExtendLasso(POINT client) noexcept;

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

    // The picture a replacement pushed aside, kept whole: the image, the marks
    // on it, and the history that belongs to those marks. Opening a file or
    // pasting lands on the only copy of what is on screen, and pressing either
    // by mistake used to be the end of it -- the history went out with the
    // picture, so there was nothing left to undo.
    //
    // One deep. The next replacement lets the previous one go.
    struct SetAside {
        ccl::doc::Document document;
        ccl::doc::History history;
        std::wstring title;
        float zoom = 1.0f;
        POINT scroll{};
        bool saved = false;
        bool valid = false;
    };
    SetAside setAside_;

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

    // Set between this window's own right press and its release.
    //
    // The menu opens on the release, so a release has to be told apart from one
    // this window never saw the press for. That happens when the eyedropper's
    // hook has swallowed the press, and when a press that began in another
    // window is let go over this one -- neither is a click here.
    bool rightButtonDown_ = false;

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

    // The line just finished, which R can still put a head on and the arrow
    // keys can still turn one on -- until anything but moving the mouse is
    // done. The same idea as the effect the size keys stay pointed at.
    size_t recentStrokeIndex_ = static_cast<size_t>(-1);

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

    // The selected area, in image coordinates: the pieces settled on so far,
    // and the one being dragged out while the button is down. The two are kept
    // apart so that letting go part way through can drop the one in hand
    // without disturbing the rest.
    ccl::doc::SelectionShapes selection_;
    bool selecting_ = false;
    ccl::doc::SelectionShape pending_;
    // What was selected when the drag began. Taken at the press because a
    // plain drag throws the old area away there and then, leaving nothing to
    // step back to by the time the button comes up.
    ccl::doc::SelectionShapes selectionBeforeDrag_;
    // How far apart a lasso's points are kept, in client pixels. Measured on
    // screen rather than in the picture: how finely a hand moves is a fact
    // about the screen, not about the zoom. Doubled while a very long lasso is
    // being drawn, since what it costs to fold and to outline grows with the
    // count and a drag that goes on must not slow down.
    float lassoSpacing_ = 0.0f;
    ccl::render::SelectionGeometry selectionGeometry_;
    // The piece being dragged out while it is being taken away, shown on its
    // own so that what is disappearing is visible. Only built for that: adding
    // shows itself, since the outline grows to include what is being dragged.
    ccl::render::SelectionGeometry removingGeometry_;

    bool spaceHeld_ = false;
    // Tool to return to once the eyedropper has taken a sample.
    ccl::tool::Tool toolBeforeEyedropper_ = ccl::tool::Tool::Pen;

    POINT lastCursor_{};
    bool cursorInside_ = false;
    bool trackingLeave_ = false;

    HWND editor_ = nullptr;
    // The little box a number is typed into, and what it is acting on. Null
    // whenever one is not open.
    HWND numberBox_ = nullptr;
    NumberKind numberKind_ = NumberKind::FontSize;
    size_t numberTarget_ = static_cast<size_t>(-1);
    unsigned int numberId_ = 0;
    float numberBefore_ = 0.0f;
    HFONT editorFont_ = nullptr;
    // Held for as long as the editor is open, so that reading the styling back
    // costs one interface call rather than one per keystroke.
    ITextDocument* editorDoc_ = nullptr;
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
    // True while the decoration panel is up. What it acts on is settled when
    // it opens, so the piece being pointed at is held still for as long as it
    // is: without this, walking the pointer past the panel would move the
    // outline onto another piece while the panel went on changing the first.
    bool decorOpen_ = false;
    // True while the font picker is up, for the same reason as above: the piece
    // the choice will land on is settled when the window opens.
    bool fontPickerOpen_ = false;
    // The text whose size is being stepped, so that a run of presses records
    // one undo step rather than one per press. Zero while nothing is.
    unsigned int resizingTextId_ = 0;
    // The size the editor's line pitch is currently pinned to. Pinning it runs
    // through a select-all, which throws away the format waiting for the next
    // character typed, so it is only done when the size actually changes.
    // The line heights the editing box was last given, so a keystroke that
    // leaves them alone does not set them all again.
    std::vector<float> pinnedLineHeights_;

    size_t movingTextIndex_ = static_cast<size_t>(-1);
    POINT textDragStart_{};
    float textDragOriginX_ = 0.0f;
    float textDragOriginY_ = 0.0f;
    bool textDragMoved_ = false;

    // What is picked out, by id rather than by position in the list: the list
    // is reordered by the very commands this exists to serve, and ids are
    // handed out once and never reused.
    std::vector<unsigned int> pickedIds_;
    // The pointers the four selecting tools show. Drawn on first use rather
    // than at startup: the time before the first selection can be made is the
    // one cost this program will not pay.
    ccl::ui::ToolCursors toolCursors_;

    // A press with Shift or Alt that landed on a piece. What it means cannot
    // be known until the button comes up: moved, it is a band drawn from on
    // top of something, which is an ordinary way to reach its neighbours; not
    // moved, it is that one piece being added or taken out. Zero for a press
    // that landed on nothing, since ids are never zero.
    unsigned int bandClickId_ = 0;
    POINT bandClickStart_{};

    // Walking down through pieces stacked in one place. Where the last press
    // landed and when, and how far down the stack it had got to by then.
    // Pressing the same place again works down through whatever is stacked
    // there. One piece is acted on at a time and which one moves down the
    // stack, rather than each press adding to the last: pressing twice over
    // three pieces means the second one, not the first two.
    //
    // Held from the press that began the run: the set as it was, and the ids
    // the run walks through. Every press puts the set back to how it started
    // before acting, which is what takes back the step before it. What is
    // walked through depends on the modifier -- Shift passes over what was
    // already picked, Alt over what was not -- and is settled once, at the
    // start, so the ground does not move while the walk is going on.
    POINT cycleAt_{};
    ULONGLONG cycleWhen_ = 0;
    size_t cycleDepth_ = 0;
    ccl::doc::SelectionOp cycleOp_ = ccl::doc::SelectionOp::Replace;
    std::vector<unsigned int> cycleBase_;
    std::vector<unsigned int> cycleTargets_;

    // Moving what is picked. The copies are of the pieces as they were when
    // the button went down.
    bool movingPicked_ = false;
    bool pickedDragMoved_ = false;
    POINT pickedDragStart_{};
    std::vector<ccl::doc::Annotation> pickedOriginals_;

    LONGLONG releasedAt_ = 0;
    bool reportedFirstFrame_ = false;
    ccl::timing::FrameStats drawStats_;
};

}  // namespace ccl::ui
