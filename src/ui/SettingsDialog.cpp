#include "ui/SettingsDialog.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/Settings.h"
#include "util/Dpi.h"

namespace ccl::ui {
namespace {

constexpr wchar_t kWindowClass[] = L"CapturaClipA2SettingsWindow";

// Pages, in the order their tabs appear.
enum Page : int {
    kPageGeneral,
    kPageDrawing,
    kPageColors,
    kPageText,
    kPageSaving,
    kPageWindow,
    kPageShortcuts,
    kPageCount,
};

// Control identifiers. Only the ones read back or acted on need a name; labels
// and notes are placed and forgotten.
enum ControlId : UINT {
    kIdTabs = 100,
    kIdPreparation,
    kIdCopyOnCapture,
    kIdTitleFormat,
    kIdSmoothScaling,
    kIdZoomStep,
    kIdPaletteScale,

    kIdPenWidth,
    kIdPenColor,
    kIdEraserWidth,
    kIdUsePressure,
    kIdPressureMin,

    kIdFontFamily,
    kIdFontSize,
    kIdTextOutline,
    kIdTextShadow,

    kIdFormat,
    kIdJpegQuality,
    kIdAutoSaveFolder,
    kIdBrowseFolder,
    kIdHistoryDays,

    kIdFrameNormal,
    kIdFrameThin,
    kIdFrameNoTitle,
    kIdFrameNoFrame,

    kIdShortcutList,
    kIdAssignKey,
    kIdClearKey,

    kIdReset,

    // Eight consecutive ids for the quick colour swatches.
    kIdQuickColorBase = 200,
};

// Layout in 96-DPI units, scaled to the monitor when the window is built.
constexpr int kMargin = 12;
constexpr int kPagePad = 12;
constexpr int kLabelWidth = 200;
constexpr int kFieldWidth = 250;
constexpr int kNarrowField = 90;
constexpr int kRowHeight = 24;
constexpr int kRowGap = 6;
constexpr int kNoteHeight = 18;
constexpr int kButtonWidth = 96;
constexpr int kButtonHeight = 26;
constexpr int kBrowseWidth = 32;
constexpr int kSwatchSize = 40;
constexpr int kDialogWidth =
    2 * kMargin + 2 * kPagePad + kLabelWidth + kFieldWidth;

struct Dialog {
    // Edited here and copied back only once OK is pressed, so cancelling costs
    // nothing.
    ccl::app::Settings working;
    bool accepted = false;
    bool finished = false;

    HFONT font = nullptr;
    UINT dpi = ccl::dpi::kDefaultDpi;

    HWND window = nullptr;
    HWND tabs = nullptr;

    // Controls belong to a page and are shown only while that page is picked.
    // Kept as a list per page rather than inside a container window, because a
    // container would swallow the WM_COMMAND its children send.
    std::vector<HWND> pageControls[kPageCount];
    int page = kPageGeneral;

    // Where the next control goes while a page is being built.
    int cursorY = 0;
    int building = kPageGeneral;
    int pageTop = 0;
    int tallestPage = 0;
    int width = 0;

    // Live colour values for the swatch buttons, which draw themselves.
    ccl::doc::Color penColor;
    ccl::doc::QuickColors quickColors{};

    HWND Field(UINT id) const noexcept {
        return ::GetDlgItem(window, static_cast<int>(id));
    }
};

std::wstring TextOf(HWND control) noexcept {
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length), L'\0');
    ::GetWindowTextW(control, text.data(), length + 1);
    return text;
}

void SetNumber(HWND control, int value) noexcept {
    wchar_t text[32];
    ::swprintf_s(text, L"%d", value);
    ::SetWindowTextW(control, text);
}

void SetNumber(HWND control, float value) noexcept {
    wchar_t text[32];
    ::swprintf_s(text, L"%g", value);
    ::SetWindowTextW(control, text);
}

int ReadInt(HWND control, int fallback) noexcept {
    const std::wstring text = TextOf(control);
    wchar_t* end = nullptr;
    const long value = ::wcstol(text.c_str(), &end, 10);
    return end != text.c_str() ? static_cast<int>(value) : fallback;
}

float ReadFloat(HWND control, float fallback) noexcept {
    const std::wstring text = TextOf(control);
    wchar_t* end = nullptr;
    const float value = ::wcstof(text.c_str(), &end);
    return end != text.c_str() ? value : fallback;
}

COLORREF ToColorRef(const ccl::doc::Color& color) noexcept {
    const auto channel = [](float value) -> int {
        return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

ccl::doc::Color FromColorRef(COLORREF value) noexcept {
    return ccl::doc::Color{static_cast<float>(GetRValue(value)) / 255.0f,
                           static_cast<float>(GetGValue(value)) / 255.0f,
                           static_cast<float>(GetBValue(value)) / 255.0f, 1.0f};
}

// The font other applications use for their dialogs, so this one does not look
// out of place next to them.
HFONT CreateMessageFont(UINT dpi) noexcept {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                      &metrics, 0, dpi)) {
        return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
    return ::CreateFontIndirectW(&metrics.lfMessageFont);
}

int CALLBACK CollectFont(const LOGFONTW* logical, const TEXTMETRICW*, DWORD,
                         LPARAM parameter) {
    auto* names = reinterpret_cast<std::set<std::wstring>*>(parameter);
    // Names starting with @ are the vertical-writing variants, which are not
    // what anyone is looking for in a font list.
    if (logical->lfFaceName[0] != L'@') {
        names->insert(logical->lfFaceName);
    }
    return 1;
}

std::vector<std::wstring> InstalledFonts() noexcept {
    std::set<std::wstring> names;

    const HDC screen = ::GetDC(nullptr);
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    ::EnumFontFamiliesExW(screen, &query, CollectFont,
                          reinterpret_cast<LPARAM>(&names), 0);
    ::ReleaseDC(nullptr, screen);

    return std::vector<std::wstring>(names.begin(), names.end());
}

int Scaled(const Dialog& dialog, int value) noexcept {
    return ccl::dpi::Scale(value, dialog.dpi);
}

HWND Add(Dialog& dialog, const wchar_t* className, const wchar_t* text,
         DWORD style, int x, int y, int width, int height, UINT id,
         bool onPage = true) noexcept {
    const HWND control = ::CreateWindowExW(
        0, className, text, WS_CHILD | style, x, y, width, height,
        dialog.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(
            ::GetWindowLongPtrW(dialog.window, GWLP_HINSTANCE)),
        nullptr);
    if (control == nullptr) {
        return nullptr;
    }

    ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog.font),
                   TRUE);
    if (onPage) {
        dialog.pageControls[dialog.building].push_back(control);
    } else {
        ::ShowWindow(control, SW_SHOW);
    }
    return control;
}

// A labelled control on its own line. `heightRows` is the height the control is
// created with, which for a combo box sizes its dropped-down list; the row
// itself is always one high.
HWND AddRow(Dialog& dialog, const wchar_t* label, const wchar_t* className,
            DWORD style, UINT id, int fieldWidth = kFieldWidth,
            int heightRows = 1) noexcept {
    const int left = Scaled(dialog, kMargin + kPagePad);
    const int rowHeight = Scaled(dialog, kRowHeight);

    if (label != nullptr) {
        Add(dialog, L"STATIC", label, SS_LEFT | SS_CENTERIMAGE, left,
            dialog.cursorY, Scaled(dialog, kLabelWidth), rowHeight, 0);
    }

    const HWND control =
        Add(dialog, className, L"", style | WS_TABSTOP,
            left + Scaled(dialog, kLabelWidth), dialog.cursorY,
            Scaled(dialog, fieldWidth), rowHeight * heightRows, id);

    dialog.cursorY += rowHeight + Scaled(dialog, kRowGap);
    return control;
}

// A checkbox reads as its own sentence, so it spans the row rather than sitting
// in the value column with an empty label beside it.
HWND AddCheck(Dialog& dialog, const wchar_t* label, UINT id,
              DWORD style = BS_AUTOCHECKBOX) noexcept {
    const int left = Scaled(dialog, kMargin + kPagePad);
    const int rowHeight = Scaled(dialog, kRowHeight);

    const HWND control =
        Add(dialog, L"BUTTON", label, style | WS_TABSTOP, left, dialog.cursorY,
            dialog.width - 2 * left, rowHeight, id);

    dialog.cursorY += rowHeight + Scaled(dialog, kRowGap);
    return control;
}

// Explanatory text under a control. Given a real height per line, because
// squeezing two lines into one row is what made the first version unreadable.
void AddNote(Dialog& dialog, const wchar_t* text, bool indent = true,
             int lines = 1) noexcept {
    const int left =
        Scaled(dialog, kMargin + kPagePad + (indent ? kLabelWidth : 0));
    const int height = Scaled(dialog, kNoteHeight) * lines;

    Add(dialog, L"STATIC", text, SS_LEFT, left, dialog.cursorY,
        dialog.width - left - Scaled(dialog, kMargin + kPagePad), height, 0);
    dialog.cursorY += height + Scaled(dialog, kRowGap);
}

void BeginPage(Dialog& dialog, int page) noexcept {
    dialog.building = page;
    dialog.cursorY = dialog.pageTop;
}

void EndPage(Dialog& dialog) noexcept {
    dialog.tallestPage = (std::max)(dialog.tallestPage, dialog.cursorY);
}

void BuildGeneral(Dialog& dialog) noexcept {
    BeginPage(dialog, kPageGeneral);
    AddRow(dialog, L"準備時間 (ミリ秒)", L"EDIT",
           ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, kIdPreparation, kNarrowField);
    AddNote(dialog, L"メニューが消えるのを待ってから撮ります");
    AddCheck(dialog, L"キャプチャしたら自動でコピーする", kIdCopyOnCapture);
    AddRow(dialog, L"タイトルの書式", L"EDIT", ES_AUTOHSCROLL | WS_BORDER,
           kIdTitleFormat);
    AddNote(dialog,
            L"%y 年  %m 月  %d 日  %h 時  %n 分  %s 秒\n"
            L"%t 取得元のウィンドウ名   %% パーセント記号",
            true, 2);
    AddCheck(dialog,
             L"拡大縮小をなめらかにする：切ると 1 ピクセルずつそのまま拡大します",
             kIdSmoothScaling);
    AddRow(dialog, L"ホイール 1 段の拡大率 (%)", L"EDIT",
           ES_AUTOHSCROLL | WS_BORDER, kIdZoomStep, kNarrowField);
    AddRow(dialog, L"パレットの大きさ (%)", L"EDIT",
           ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, kIdPaletteScale, kNarrowField);
    EndPage(dialog);
}

void BuildDrawing(Dialog& dialog) noexcept {
    BeginPage(dialog, kPageDrawing);
    AddRow(dialog, L"ブラシの太さ (px)", L"EDIT", ES_AUTOHSCROLL | WS_BORDER,
           kIdPenWidth, kNarrowField);
    AddRow(dialog, L"ブラシの色", L"BUTTON", BS_OWNERDRAW, kIdPenColor,
           kSwatchSize);
    AddNote(dialog, L"蛍光マーカーもこの太さと色を使います");
    AddRow(dialog, L"消しゴムの太さ (px)", L"EDIT", ES_AUTOHSCROLL | WS_BORDER,
           kIdEraserWidth, kNarrowField);
    AddNote(dialog, L"消しゴムだけ太さを別に覚えます");
    AddCheck(dialog, L"筆圧を使う", kIdUsePressure);
    AddRow(dialog, L"筆圧が最小のときの太さ (%)", L"EDIT",
           ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, kIdPressureMin, kNarrowField);
    AddNote(dialog, L"0 にすると、軽く触れた所は消えます");
    EndPage(dialog);
}

void BuildColors(Dialog& dialog) noexcept {
    BeginPage(dialog, kPageColors);
    AddNote(dialog,
            L"Shift+1 〜 Shift+8 で選べる色。パレットのいちばん上の段にも"
            L"並びます。",
            false);

    const int left = Scaled(dialog, kMargin + kPagePad);
    const int swatch = Scaled(dialog, kSwatchSize);
    const int gap = Scaled(dialog, kRowGap);
    const int labelHeight = Scaled(dialog, kNoteHeight);

    for (int i = 0; i < 8; ++i) {
        const int x = left + i * (swatch + gap);
        Add(dialog, L"STATIC", std::to_wstring(i + 1).c_str(),
            SS_CENTER | SS_CENTERIMAGE, x, dialog.cursorY, swatch, labelHeight,
            0);
        Add(dialog, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, x,
            dialog.cursorY + labelHeight, swatch, swatch,
            kIdQuickColorBase + static_cast<UINT>(i));
    }
    dialog.cursorY += labelHeight + swatch + gap;

    AddNote(dialog, L"押すと色を選び直せます。", false);
    EndPage(dialog);
}

void BuildText(Dialog& dialog) noexcept {
    BeginPage(dialog, kPageText);
    const HWND font =
        AddRow(dialog, L"フォント", L"COMBOBOX",
               CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL, kIdFontFamily,
               kFieldWidth, 12);
    for (const std::wstring& name : InstalledFonts()) {
        ComboBox_AddString(font, name.c_str());
    }

    AddRow(dialog, L"大きさ (px)", L"EDIT", ES_AUTOHSCROLL | WS_BORDER,
           kIdFontSize, kNarrowField);
    AddCheck(dialog, L"縁取りをつける", kIdTextOutline);
    AddCheck(dialog, L"影をつける", kIdTextShadow);
    AddNote(dialog,
            L"どちらもスクリーンショットの上で読めるようにするためのもので、"
            L"両方いっしょに使えます。",
            false);
    EndPage(dialog);
}

void BuildSaving(Dialog& dialog) noexcept {
    BeginPage(dialog, kPageSaving);
    const HWND format =
        AddRow(dialog, L"標準の形式", L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL,
               kIdFormat, kNarrowField + 30, 6);
    ComboBox_AddString(format, L"PNG");
    ComboBox_AddString(format, L"JPEG");
    ComboBox_AddString(format, L"BMP");

    AddRow(dialog, L"JPEG の品質 (0-100)", L"EDIT",
           ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, kIdJpegQuality, kNarrowField);
    AddNote(dialog, L"PNG に品質の設定はありません");

    const HWND folder = AddRow(dialog, L"自動保存のフォルダ", L"EDIT",
                               ES_AUTOHSCROLL | WS_BORDER, kIdAutoSaveFolder,
                               kFieldWidth - kBrowseWidth - 4);

    // Placed against the folder box rather than on a row of its own.
    RECT bounds{};
    ::GetWindowRect(folder, &bounds);
    POINT origin{bounds.right, bounds.top};
    ::ScreenToClient(dialog.window, &origin);
    Add(dialog, L"BUTTON", L"...", BS_PUSHBUTTON | WS_TABSTOP,
        origin.x + Scaled(dialog, 4), origin.y, Scaled(dialog, kBrowseWidth),
        Scaled(dialog, kRowHeight), kIdBrowseFolder);

    AddNote(dialog, L"空にすると自動保存しません");
    AddRow(dialog, L"残す日数 (0 で消さない)", L"EDIT",
           ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, kIdHistoryDays, kNarrowField);
    AddNote(dialog, L"過ぎたファイルはゴミ箱へ送ります");
    EndPage(dialog);
}

void BuildWindow(Dialog& dialog) noexcept {
    BeginPage(dialog, kPageWindow);
    AddNote(dialog, L"※ 次のキャプチャから反映されます", false);

    AddCheck(dialog, L"通常：タイトルバーと枠のある普通のウィンドウ",
             kIdFrameNormal, BS_AUTORADIOBUTTON | WS_GROUP);
    AddCheck(dialog,
             L"細いタイトルバー：閉じるボタンだけ。タスクバーに出ません",
             kIdFrameThin, BS_AUTORADIOBUTTON);
    AddCheck(dialog, L"タイトルバーなし：細い枠だけ。移動は中ボタンのドラッグ",
             kIdFrameNoTitle, BS_AUTORADIOBUTTON);
    AddCheck(dialog,
             L"タイトルバー・枠なし：背景に溶けこんで見失うことがあります",
             kIdFrameNoFrame, BS_AUTORADIOBUTTON);
    EndPage(dialog);
}

// A window that does nothing but wait for a key. Assigning a shortcut has to
// swallow the key press rather than let it act, and inside the settings window
// most keys already mean something to the controls.
class KeyCatcher {
public:
    static std::optional<ccl::app::Binding> Run(HWND owner, HFONT font,
                                                UINT dpi) noexcept;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);

    ccl::app::Binding binding;
    bool finished = false;
    bool accepted = false;
};

LRESULT CALLBACK KeyCatcher::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
    auto* state =
        reinterpret_cast<KeyCatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (state == nullptr) {
                break;
            }
            const UINT key = static_cast<UINT>(wParam);
            // A modifier on its own is half of a combination, not a key.
            if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU ||
                key == VK_LWIN || key == VK_RWIN) {
                return 0;
            }
            if (key == VK_ESCAPE) {
                state->finished = true;
                return 0;
            }
            state->binding = ccl::app::Binding{
                key, (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
                (::GetKeyState(VK_SHIFT) & 0x8000) != 0,
                (::GetKeyState(VK_MENU) & 0x8000) != 0};
            state->accepted = true;
            state->finished = true;
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            ::SetBkColor(reinterpret_cast<HDC>(wParam),
                         ::GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_3DFACE));

        case WM_CLOSE:
        case WM_KILLFOCUS:
            if (state != nullptr) {
                state->finished = true;
            }
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::optional<ccl::app::Binding> KeyCatcher::Run(HWND owner, HFONT font,
                                                 UINT dpi) noexcept {
    static constexpr wchar_t kClass[] = L"CapturaClipA2KeyCatcher";
    const auto instance =
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(owner, GWLP_HINSTANCE));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = KeyCatcher::WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_3DFACE);
    wc.lpszClassName = kClass;
    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return std::nullopt;
    }

    KeyCatcher state;

    const int width = ccl::dpi::Scale(340, dpi);
    const int height = ccl::dpi::Scale(120, dpi);
    RECT ownerBounds{};
    ::GetWindowRect(owner, &ownerBounds);

    const HWND window = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kClass, L"キーの割り当て",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        ownerBounds.left + (ownerBounds.right - ownerBounds.left - width) / 2,
        ownerBounds.top + (ownerBounds.bottom - ownerBounds.top - height) / 2,
        width, height, owner, nullptr, instance, &state);
    if (window == nullptr) {
        return std::nullopt;
    }

    const int pad = ccl::dpi::Scale(16, dpi);
    RECT client{};
    ::GetClientRect(window, &client);
    const HWND label = ::CreateWindowExW(
        0, L"STATIC",
        L"割り当てるキーを押してください。\n"
        L"Esc でやめます。",
        WS_CHILD | WS_VISIBLE | SS_CENTER, pad, pad,
        client.right - 2 * pad, client.bottom - 2 * pad, window, nullptr,
        instance, nullptr);
    ::SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    ::EnableWindow(owner, FALSE);
    ::ShowWindow(window, SW_SHOW);
    ::SetForegroundWindow(window);
    ::SetFocus(window);

    MSG msg{};
    while (!state.finished && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
    ::DestroyWindow(window);

    if (msg.message == WM_QUIT) {
        ::PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (!state.accepted) {
        return std::nullopt;
    }
    return state.binding;
}

void BuildShortcuts(Dialog& dialog) noexcept {
    BeginPage(dialog, kPageShortcuts);
    AddNote(dialog,
            L"行を選んで「キーを設定」を押し、割り当てたいキーを押します。",
            false);

    const int left = Scaled(dialog, kMargin + kPagePad);
    const int listWidth = dialog.width - 2 * left;
    const int listHeight = Scaled(dialog, 260);

    const HWND list = Add(dialog, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                              WS_BORDER | WS_TABSTOP,
                          left, dialog.cursorY, listWidth, listHeight,
                          kIdShortcutList);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = listWidth * 2 / 3;
    column.pszText = const_cast<wchar_t*>(L"操作");
    ListView_InsertColumn(list, 0, &column);
    column.cx = listWidth / 3 - Scaled(dialog, 20);
    column.pszText = const_cast<wchar_t*>(L"キー");
    ListView_InsertColumn(list, 1, &column);

    dialog.cursorY += listHeight + Scaled(dialog, kRowGap);

    const int buttonWidth = Scaled(dialog, kButtonWidth);
    const int buttonHeight = Scaled(dialog, kButtonHeight);
    const int gap = Scaled(dialog, kRowGap);
    Add(dialog, L"BUTTON", L"キーを設定", BS_PUSHBUTTON | WS_TABSTOP, left,
        dialog.cursorY, buttonWidth, buttonHeight, kIdAssignKey);
    Add(dialog, L"BUTTON", L"解除", BS_PUSHBUTTON | WS_TABSTOP,
        left + buttonWidth + gap, dialog.cursorY, buttonWidth, buttonHeight,
        kIdClearKey);
    dialog.cursorY += buttonHeight + gap;

    AddNote(dialog,
            L"サイズ変更の [ ]、ズームの 1〜5、色の Shift+1〜8、"
            L"スクロールのスペースと矢印は、\n"
            L"押している間や描いている最中で意味が変わるため、"
            L"割り当ては変えられません。",
            false, 2);
    EndPage(dialog);
}

// Refills the shortcut list from the working bindings.
void FillShortcutList(Dialog& dialog) noexcept {
    const HWND list = dialog.Field(kIdShortcutList);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(list);

    for (int i = 0; i < static_cast<int>(ccl::app::Command::Count); ++i) {
        const auto command = static_cast<ccl::app::Command>(i);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText =
            const_cast<wchar_t*>(ccl::app::CommandLabel(command));
        ListView_InsertItem(list, &item);

        const std::wstring key =
            ccl::app::BindingText(dialog.working.shortcuts.For(command));
        ListView_SetItemText(list, i, 1, const_cast<wchar_t*>(key.c_str()));
    }

    if (selected >= 0) {
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
}

void AssignShortcut(Dialog& dialog, bool clear) noexcept {
    const HWND list = dialog.Field(kIdShortcutList);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0) {
        return;
    }
    const auto command = static_cast<ccl::app::Command>(selected);

    if (clear) {
        dialog.working.shortcuts.Clear(command);
        FillShortcutList(dialog);
        return;
    }

    const auto binding = KeyCatcher::Run(dialog.window, dialog.font, dialog.dpi);
    if (!binding.has_value()) {
        return;
    }

    // Assign takes the key off whatever had it, so the whole list is refilled
    // rather than just the row that was edited.
    dialog.working.shortcuts.Assign(command, *binding);
    FillShortcutList(dialog);
}

// Writes the working values into the controls. Separate from building them so
// that the reset button can refill everything without rebuilding the window.
void Populate(Dialog& dialog) noexcept {
    const ccl::app::Settings& values = dialog.working;

    SetNumber(dialog.Field(kIdPreparation),
              static_cast<int>(values.preparationMs));
    Button_SetCheck(dialog.Field(kIdCopyOnCapture),
                    values.copyOnCapture ? BST_CHECKED : BST_UNCHECKED);
    ::SetWindowTextW(dialog.Field(kIdTitleFormat), values.titleFormat.c_str());
    Button_SetCheck(dialog.Field(kIdSmoothScaling),
                    values.smoothScaling ? BST_CHECKED : BST_UNCHECKED);
    SetNumber(dialog.Field(kIdZoomStep), values.zoomStepPercent);
    SetNumber(dialog.Field(kIdPaletteScale), values.paletteScalePercent);

    SetNumber(dialog.Field(kIdPenWidth), values.penWidth);
    SetNumber(dialog.Field(kIdEraserWidth), values.eraserWidth);
    Button_SetCheck(dialog.Field(kIdUsePressure),
                    values.usePenPressure ? BST_CHECKED : BST_UNCHECKED);
    SetNumber(dialog.Field(kIdPressureMin),
              static_cast<int>(values.pressureMinScale * 100.0f + 0.5f));

    dialog.penColor = values.penColor;
    dialog.quickColors = values.quickColors;
    ::InvalidateRect(dialog.Field(kIdPenColor), nullptr, TRUE);
    for (UINT i = 0; i < 8; ++i) {
        ::InvalidateRect(dialog.Field(kIdQuickColorBase + i), nullptr, TRUE);
    }

    const HWND font = dialog.Field(kIdFontFamily);
    if (ComboBox_SelectString(font, -1, values.textFontFamily.c_str()) ==
        CB_ERR) {
        ComboBox_SetCurSel(font, 0);
    }
    SetNumber(dialog.Field(kIdFontSize), values.textFontSize);
    Button_SetCheck(dialog.Field(kIdTextOutline),
                    values.textOutline ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(dialog.Field(kIdTextShadow),
                    values.textShadow ? BST_CHECKED : BST_UNCHECKED);

    ComboBox_SetCurSel(dialog.Field(kIdFormat),
                       static_cast<int>(values.defaultFormat));
    SetNumber(dialog.Field(kIdJpegQuality), values.jpegQuality);
    ::SetWindowTextW(dialog.Field(kIdAutoSaveFolder),
                     values.autoSaveFolder.c_str());
    SetNumber(dialog.Field(kIdHistoryDays), values.autoSaveHistoryDays);

    const UINT frames[] = {kIdFrameNormal, kIdFrameThin, kIdFrameNoTitle,
                           kIdFrameNoFrame};
    for (size_t i = 0; i < ARRAYSIZE(frames); ++i) {
        const bool chosen =
            static_cast<size_t>(values.windowFrame) == i;
        Button_SetCheck(dialog.Field(frames[i]),
                        chosen ? BST_CHECKED : BST_UNCHECKED);
    }

    FillShortcutList(dialog);
}

void Collect(Dialog& dialog) noexcept {
    ccl::app::Settings& values = dialog.working;

    values.preparationMs = static_cast<UINT>(
        (std::max)(0, ReadInt(dialog.Field(kIdPreparation),
                              static_cast<int>(values.preparationMs))));
    values.copyOnCapture =
        Button_GetCheck(dialog.Field(kIdCopyOnCapture)) == BST_CHECKED;
    values.titleFormat = TextOf(dialog.Field(kIdTitleFormat));
    values.smoothScaling =
        Button_GetCheck(dialog.Field(kIdSmoothScaling)) == BST_CHECKED;
    values.zoomStepPercent =
        ReadFloat(dialog.Field(kIdZoomStep), values.zoomStepPercent);
    values.paletteScalePercent =
        ReadInt(dialog.Field(kIdPaletteScale), values.paletteScalePercent);

    values.penWidth = ReadFloat(dialog.Field(kIdPenWidth), values.penWidth);
    values.penColor = dialog.penColor;
    values.eraserWidth =
        ReadFloat(dialog.Field(kIdEraserWidth), values.eraserWidth);
    values.usePenPressure =
        Button_GetCheck(dialog.Field(kIdUsePressure)) == BST_CHECKED;
    values.pressureMinScale =
        static_cast<float>(ReadInt(
            dialog.Field(kIdPressureMin),
            static_cast<int>(values.pressureMinScale * 100.0f + 0.5f))) /
        100.0f;

    values.quickColors = dialog.quickColors;

    const std::wstring family = TextOf(dialog.Field(kIdFontFamily));
    if (!family.empty()) {
        values.textFontFamily = family;
    }
    values.textFontSize =
        ReadFloat(dialog.Field(kIdFontSize), values.textFontSize);
    values.textOutline =
        Button_GetCheck(dialog.Field(kIdTextOutline)) == BST_CHECKED;
    values.textShadow =
        Button_GetCheck(dialog.Field(kIdTextShadow)) == BST_CHECKED;

    switch (ComboBox_GetCurSel(dialog.Field(kIdFormat))) {
        case 1: values.defaultFormat = ccl::app::ImageFormat::Jpeg; break;
        case 2: values.defaultFormat = ccl::app::ImageFormat::Bmp; break;
        default: values.defaultFormat = ccl::app::ImageFormat::Png; break;
    }
    values.jpegQuality = ReadInt(dialog.Field(kIdJpegQuality), values.jpegQuality);
    values.autoSaveFolder = TextOf(dialog.Field(kIdAutoSaveFolder));
    values.autoSaveHistoryDays =
        ReadInt(dialog.Field(kIdHistoryDays), values.autoSaveHistoryDays);

    if (Button_GetCheck(dialog.Field(kIdFrameNormal)) == BST_CHECKED) {
        values.windowFrame = ccl::app::WindowFrame::Normal;
    } else if (Button_GetCheck(dialog.Field(kIdFrameThin)) == BST_CHECKED) {
        values.windowFrame = ccl::app::WindowFrame::ThinTitleBar;
    } else if (Button_GetCheck(dialog.Field(kIdFrameNoFrame)) == BST_CHECKED) {
        values.windowFrame = ccl::app::WindowFrame::NoFrame;
    } else {
        values.windowFrame = ccl::app::WindowFrame::NoTitleBar;
    }

    values.Clamp();
}

void ShowPage(Dialog& dialog, int page) noexcept {
    dialog.page = page;
    for (int i = 0; i < kPageCount; ++i) {
        const int command = i == page ? SW_SHOW : SW_HIDE;
        for (HWND control : dialog.pageControls[i]) {
            ::ShowWindow(control, command);
        }
    }
}

// Swatch buttons paint themselves; a normal button cannot show a colour.
void DrawSwatch(Dialog& dialog, const DRAWITEMSTRUCT& item) noexcept {
    const ccl::doc::Color* color = nullptr;
    if (item.CtlID == kIdPenColor) {
        color = &dialog.penColor;
    } else if (item.CtlID >= kIdQuickColorBase &&
               item.CtlID < kIdQuickColorBase + 8) {
        color = &dialog.quickColors[item.CtlID - kIdQuickColorBase];
    }
    if (color == nullptr) {
        return;
    }

    const HBRUSH fill = ::CreateSolidBrush(ToColorRef(*color));
    ::FillRect(item.hDC, &item.rcItem, fill);
    ::DeleteObject(fill);

    // A focused or pressed swatch gets a frame, since the colour alone gives no
    // sign of either.
    const bool marked = (item.itemState & (ODS_SELECTED | ODS_FOCUS)) != 0;
    ::FrameRect(item.hDC, &item.rcItem,
                ::GetSysColorBrush(marked ? COLOR_HIGHLIGHT : COLOR_3DSHADOW));
}

bool PickColor(Dialog& dialog, ccl::doc::Color& color) noexcept {
    // Kept between calls so that a colour mixed for one swatch is still there
    // for the next.
    static COLORREF custom[16] = {};

    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = dialog.window;
    choose.rgbResult = ToColorRef(color);
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;

    if (!::ChooseColorW(&choose)) {
        return false;
    }
    color = FromColorRef(choose.rgbResult);
    return true;
}

void BrowseForFolder(Dialog& dialog) noexcept {
    IFileDialog* picker = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&picker)))) {
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(picker->GetOptions(&options))) {
        picker->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    }

    if (SUCCEEDED(picker->Show(dialog.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(picker->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                ::SetWindowTextW(dialog.Field(kIdAutoSaveFolder), path);
                ::CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    picker->Release();
}

void ResetToDefaults(Dialog& dialog) noexcept {
    if (::MessageBoxW(dialog.window,
                      L"すべての設定を初期値に戻します。よろしいですか。",
                      L"設定のリセット",
                      MB_ICONQUESTION | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK) {
        return;
    }
    dialog.working.ResetToDefaults();
    Populate(dialog);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* dialog =
        reinterpret_cast<Dialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }

        case WM_DRAWITEM:
            if (dialog != nullptr) {
                DrawSwatch(*dialog,
                           *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;

        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (dialog != nullptr && header->hwndFrom == dialog->tabs &&
                header->code == TCN_SELCHANGE) {
                ShowPage(*dialog, TabCtrl_GetCurSel(dialog->tabs));
                return 0;
            }
            break;
        }

        case WM_COMMAND: {
            if (dialog == nullptr) {
                break;
            }
            const UINT id = LOWORD(wParam);
            if (id == kIdPenColor) {
                if (PickColor(*dialog, dialog->penColor)) {
                    ::InvalidateRect(dialog->Field(kIdPenColor), nullptr, TRUE);
                }
                return 0;
            }
            if (id >= kIdQuickColorBase && id < kIdQuickColorBase + 8) {
                if (PickColor(*dialog,
                              dialog->quickColors[id - kIdQuickColorBase])) {
                    ::InvalidateRect(dialog->Field(id), nullptr, TRUE);
                }
                return 0;
            }
            switch (id) {
                case IDOK:
                    Collect(*dialog);
                    dialog->accepted = true;
                    dialog->finished = true;
                    return 0;
                case IDCANCEL:
                    dialog->finished = true;
                    return 0;
                case kIdBrowseFolder:
                    BrowseForFolder(*dialog);
                    return 0;
                case kIdAssignKey:
                    AssignShortcut(*dialog, false);
                    return 0;
                case kIdClearKey:
                    AssignShortcut(*dialog, true);
                    return 0;
                case kIdReset:
                    ResetToDefaults(*dialog);
                    return 0;
                default:
                    break;
            }
            break;
        }

        // Controls draw their own background from the parent's brush, which is
        // grey by default and does not match the dialog face colour.
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            ::SetBkColor(reinterpret_cast<HDC>(wParam),
                         ::GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_3DFACE));

        case WM_CLOSE:
            if (dialog != nullptr) {
                dialog->finished = true;
            }
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterWindowClass(HINSTANCE instance) noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_3DFACE);
    wc.lpszClassName = kWindowClass;

    return ::RegisterClassExW(&wc) != 0 ||
           ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void AddTab(HWND tabs, int index, const wchar_t* label) noexcept {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(label);
    ::SendMessageW(tabs, TCM_INSERTITEMW, static_cast<WPARAM>(index),
                   reinterpret_cast<LPARAM>(&item));
}

}  // namespace

bool ShowSettingsDialog(HWND owner, ccl::app::Settings& settings) noexcept {
    const auto instance =
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!RegisterWindowClass(instance)) {
        return false;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC =
        ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&controls);

    Dialog dialog;
    dialog.working = settings;
    dialog.dpi = ccl::dpi::ForWindow(owner);
    dialog.font = CreateMessageFont(dialog.dpi);
    dialog.width = ccl::dpi::Scale(kDialogWidth, dialog.dpi);

    // Created at a provisional height: how tall it needs to be is only known
    // once the tallest page has been laid out.
    dialog.window = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kWindowClass, L"設定",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, dialog.width,
        ccl::dpi::Scale(200, dialog.dpi), owner, nullptr, instance, &dialog);
    if (dialog.window == nullptr) {
        ::DeleteObject(dialog.font);
        return false;
    }

    const int margin = ccl::dpi::Scale(kMargin, dialog.dpi);
    dialog.tabs = ::CreateWindowExW(
        0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, margin,
        margin, dialog.width - 2 * margin, ccl::dpi::Scale(100, dialog.dpi),
        dialog.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdTabs)),
        instance, nullptr);
    if (dialog.tabs == nullptr) {
        ::DestroyWindow(dialog.window);
        ::DeleteObject(dialog.font);
        return false;
    }
    ::SendMessageW(dialog.tabs, WM_SETFONT, reinterpret_cast<WPARAM>(dialog.font),
                   TRUE);

    AddTab(dialog.tabs, kPageGeneral, L"全般");
    AddTab(dialog.tabs, kPageDrawing, L"描画");
    AddTab(dialog.tabs, kPageColors, L"色");
    AddTab(dialog.tabs, kPageText, L"文字");
    AddTab(dialog.tabs, kPageSaving, L"保存");
    AddTab(dialog.tabs, kPageWindow, L"ウィンドウ");
    AddTab(dialog.tabs, kPageShortcuts, L"ショートカット");

    // Where a page's contents may start. Only the top matters here; the height
    // is settled once the pages are built.
    RECT display{margin, margin, dialog.width - margin,
                 margin + ccl::dpi::Scale(1000, dialog.dpi)};
    ::SendMessageW(dialog.tabs, TCM_ADJUSTRECT, FALSE,
                   reinterpret_cast<LPARAM>(&display));
    dialog.pageTop = display.top + ccl::dpi::Scale(kRowGap, dialog.dpi);

    BuildGeneral(dialog);
    BuildDrawing(dialog);
    BuildColors(dialog);
    BuildText(dialog);
    BuildSaving(dialog);
    BuildWindow(dialog);
    BuildShortcuts(dialog);

    Populate(dialog);
    ShowPage(dialog, kPageGeneral);

    // The tab control grows to hold the tallest page; the buttons sit below it.
    const int pageBottom =
        dialog.tallestPage + ccl::dpi::Scale(kPagePad, dialog.dpi);
    ::SetWindowPos(dialog.tabs, nullptr, 0, 0, dialog.width - 2 * margin,
                   pageBottom - margin, SWP_NOMOVE | SWP_NOZORDER);

    const int buttonWidth = ccl::dpi::Scale(kButtonWidth, dialog.dpi);
    const int buttonHeight = ccl::dpi::Scale(kButtonHeight, dialog.dpi);
    const int gap = ccl::dpi::Scale(kRowGap, dialog.dpi);
    const int buttonY = pageBottom + gap;
    const int right = dialog.width - margin;

    Add(dialog, L"BUTTON", L"設定をリセット", BS_PUSHBUTTON | WS_TABSTOP, margin,
        buttonY, buttonWidth + gap, buttonHeight, kIdReset, false);
    Add(dialog, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
        right - 2 * buttonWidth - gap, buttonY, buttonWidth, buttonHeight, IDOK,
        false);
    Add(dialog, L"BUTTON", L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP,
        right - buttonWidth, buttonY, buttonWidth, buttonHeight, IDCANCEL,
        false);

    // The client area has to end up as tall as the controls made it, so the
    // frame is added on top of that rather than taken out of it.
    RECT wanted{0, 0, dialog.width, buttonY + buttonHeight + margin};
    ::AdjustWindowRectExForDpi(&wanted, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                               WS_EX_DLGMODALFRAME, dialog.dpi);

    const int width = wanted.right - wanted.left;
    const int height = wanted.bottom - wanted.top;

    // Centred on the capture it belongs to, then pushed back onto the monitor
    // if that put it off the edge.
    RECT ownerBounds{};
    ::GetWindowRect(owner, &ownerBounds);

    RECT work{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (::GetMonitorInfoW(::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST),
                          &monitor)) {
        work = monitor.rcWork;
    }

    int x = ownerBounds.left + (ownerBounds.right - ownerBounds.left - width) / 2;
    int y = ownerBounds.top + (ownerBounds.bottom - ownerBounds.top - height) / 2;
    if (work.right > work.left) {
        x = (std::min)((std::max)(x, static_cast<int>(work.left)),
                       static_cast<int>(work.right) - width);
        y = (std::min)((std::max)(y, static_cast<int>(work.top)),
                       static_cast<int>(work.bottom) - height);
    }

    ::SetWindowPos(dialog.window, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);

    // Modal by hand: the owner is disabled for the duration, and messages are
    // pumped here rather than by the caller's loop.
    ::EnableWindow(owner, FALSE);
    ::ShowWindow(dialog.window, SW_SHOW);
    ::SetForegroundWindow(dialog.window);

    MSG msg{};
    while (!dialog.finished && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dialog.window, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    // Re-enabled before the window goes, so that focus lands back on the owner
    // rather than on some other application.
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
    ::DestroyWindow(dialog.window);
    ::DeleteObject(dialog.font);

    // A WM_QUIT taken out of the queue here would never reach the caller's
    // loop, so it is put back.
    if (msg.message == WM_QUIT) {
        ::PostQuitMessage(static_cast<int>(msg.wParam));
    }

    if (!dialog.accepted) {
        return false;
    }

    settings = dialog.working;
    settings.Save();
    return true;
}

}  // namespace ccl::ui
