// NeoSniffer — native Win32 UI (main window, tabs, toolbar, treemap view, dialogs).
#include "core.h"
#include "ui_treemap.h"
#include "ui_config.h"
#include "i18n.h"
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <shellscalingapi.h>
#include <ole2.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <initializer_list>
#include <utility>

#pragma comment(lib, "comctl32.lib")

using namespace ss;

// ---------------------------------------------------------------- constants
static const wchar_t* kMainClass = L"NeoSnifferMainWindow";
static const wchar_t* kViewClass = L"NeoSnifferViewWindow";
static const wchar_t* kCanvasClass = L"NeoSnifferTreemapCanvas";
static const wchar_t* kStartClass = L"NeoSnifferStartWindow";
static const wchar_t* kPopupMenuClass = L"NeoSnifferPopupMenu";
static const wchar_t* kMenuBarClass = L"NeoSnifferMenuBar";
static const wchar_t* kLogClass = L"NeoSnifferLogWindow";
static const wchar_t* kHelpClass = L"NeoSnifferHelpWindow";
static const wchar_t* kAboutClass = L"NeoSnifferAboutWindow";
static const wchar_t* AppTitle() { return ss::i18n::T(L"app.title"); }
static bool g_isElevated = false;

static bool IsProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD retLen = 0;
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &retLen))
        elevated = (elev.TokenIsElevated != 0);
    CloseHandle(token);
    return elevated;
}

static std::wstring MakeFileFilter(std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> pairs)
{
    std::wstring f;
    for (const auto& p : pairs)
    {
        f += p.first;
        f.push_back(L'\0');
        f += p.second;
        f.push_back(L'\0');
    }
    f.push_back(L'\0');
    return f;
}

// ---------------------------------------------------------------- icon font
static HFONT  g_fontIcon = nullptr;
static bool   g_hasIconFont = false;
static bool   g_toolGlyphCacheValid = false;
static wchar_t g_iconFace[64] = {};

static int CALLBACK EnumFontProcW(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp)
{
    if (lp) *(bool*)lp = true;
    return 0;
}

static void InitIconFont()
{
    const wchar_t* faces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    g_hasIconFont = false;
    for (const wchar_t* face : faces)
    {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcsncpy_s(lf.lfFaceName, LF_FACESIZE, face, _TRUNCATE);
        bool found = false;
        HDC dc = GetDC(nullptr);
        if (dc)
        {
            EnumFontFamiliesExW(dc, &lf, EnumFontProcW, (LPARAM)&found, 0);
            ReleaseDC(nullptr, dc);
        }
        if (found)
        {
            wcsncpy_s(g_iconFace, _countof(g_iconFace), face, _TRUNCATE);
            g_hasIconFont = true;
            break;
        }
    }
}

// true when the installed icon font really contains this glyph
static bool HasGlyph(wchar_t ch, HFONT font)
{
    if (!g_hasIconFont || !font) return false;
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    HGDIOBJ old = SelectObject(dc, font);
    WORD idx = 0;
    wchar_t c = ch;
    bool ok = GetGlyphIndicesW(dc, &c, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && idx != 0xFFFF;
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return ok;
}

// ---------------------------------------------------------------- theme
struct Palette
{
    COLORREF bg = RGB(0x1E, 0x1E, 0x1E);
    COLORREF panel = RGB(0x25, 0x25, 0x26);
    COLORREF panelAlt = RGB(0x2D, 0x2D, 0x30);
    COLORREF border = RGB(0x3F, 0x3F, 0x46);
    COLORREF text = RGB(0xE6, 0xE6, 0xE6);
    COLORREF textDim = RGB(0x9A, 0x9A, 0xA0);
    COLORREF accent = RGB(0x4C, 0x9A, 0xFF);
    COLORREF canvas = RGB(0x18, 0x18, 0x1A);
    bool dark = true;
};

static Palette g_pal;
static AppConfig g_cfg;
static HFONT g_fontUI = nullptr, g_fontUIBold = nullptr, g_fontMono = nullptr, g_fontSmall = nullptr, g_fontSeg = nullptr;
static HBRUSH g_editBrush = nullptr;
static COLORREF g_editBrushColor = CLR_INVALID;
static HBRUSH g_viewerBrush = nullptr;
static COLORREF g_viewerBrushColor = CLR_INVALID;
static int g_dpi = 96;

static HBRUSH GetEditBrush(COLORREF color)
{
    if (!g_editBrush || g_editBrushColor != color)
    {
        if (g_editBrush) DeleteObject(g_editBrush);
        g_editBrush = CreateSolidBrush(color);
        g_editBrushColor = color;
    }
    return g_editBrush;
}

static HBRUSH GetViewerBrush(COLORREF color)
{
    if (!g_viewerBrush || g_viewerBrushColor != color)
    {
        if (g_viewerBrush) DeleteObject(g_viewerBrush);
        g_viewerBrush = CreateSolidBrush(color);
        g_viewerBrushColor = color;
    }
    return g_viewerBrush;
}

static void CleanupGlobalGdi()
{
    auto delObj = [](auto& h) { if (h) { DeleteObject(h); h = nullptr; } };
    delObj(g_fontUI);
    delObj(g_fontUIBold);
    delObj(g_fontMono);
    delObj(g_fontSmall);
    delObj(g_fontSeg);
    delObj(g_fontIcon);
    delObj(g_editBrush);
    g_editBrushColor = CLR_INVALID;
    delObj(g_viewerBrush);
    g_viewerBrushColor = CLR_INVALID;
}

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void InitTheme(bool dark)
{
    g_pal.dark = dark;
    if (dark)
    {
        g_pal.bg = RGB(0x1E, 0x1E, 0x1E);
        g_pal.panel = RGB(0x25, 0x25, 0x26);
        g_pal.panelAlt = RGB(0x2D, 0x2D, 0x30);
        g_pal.border = RGB(0x3F, 0x3F, 0x46);
        g_pal.text = RGB(0xE6, 0xE6, 0xE6);
        g_pal.textDim = RGB(0x9A, 0x9A, 0xA0);
        g_pal.accent = RGB(0x4C, 0x9A, 0xFF);
        g_pal.canvas = RGB(0x18, 0x18, 0x1A);
    }
    else
    {
        g_pal.bg = RGB(0xF5, 0xF6, 0xF8);
        g_pal.panel = RGB(0xFF, 0xFF, 0xFF);
        g_pal.panelAlt = RGB(0xEC, 0xEE, 0xF1);
        g_pal.border = RGB(0xC9, 0xCD, 0xD3);
        g_pal.text = RGB(0x1E, 0x1E, 0x1E);
        g_pal.textDim = RGB(0x6B, 0x70, 0x76);
        g_pal.accent = RGB(0x1F, 0x6F, 0xEB);
        g_pal.canvas = RGB(0xE8, 0xEA, 0xEE);
    }
}

static void RebuildFonts()
{
    auto mk = [](int pt, bool bold, const wchar_t* face)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, g_dpi, 72);
        lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
        return CreateFontIndirectW(&lf);
    };
    if (g_fontUI) DeleteObject(g_fontUI);
    if (g_fontUIBold) DeleteObject(g_fontUIBold);
    if (g_fontMono) DeleteObject(g_fontMono);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontSeg) { DeleteObject(g_fontSeg); g_fontSeg = nullptr; }
    g_fontUI = mk(9, false, L"Segoe UI");
    g_fontUIBold = mk(9, true, L"Segoe UI");
    g_fontMono = mk(10, false, L"Consolas");
    g_fontSmall = mk(8, false, L"Segoe UI");
    g_fontSeg = mk(13, false, L"Segoe UI");
    if (g_fontIcon) { DeleteObject(g_fontIcon); g_fontIcon = nullptr; }
    if (g_hasIconFont)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(11, g_dpi, 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcsncpy_s(lf.lfFaceName, LF_FACESIZE, g_iconFace, _TRUNCATE);
        g_fontIcon = CreateFontIndirectW(&lf);
    }
    g_toolGlyphCacheValid = false;
}

static void Fill(HDC dc, const RECT& r, COLORREF c)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = (HBRUSH)GetStockObject(DC_BRUSH);
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, b);
}

static void Frame(HDC dc, const RECT& r, COLORREF c)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = (HBRUSH)GetStockObject(DC_BRUSH);
    SetDCBrushColor(dc, c);
    FrameRect(dc, &r, b);
}

static void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, bool drawBorder)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, drawBorder ? border : fill);
    if (!br || !pen)
    {
        if (br) DeleteObject(br);
        if (pen) DeleteObject(pen);
        return;
    }
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

static void TextAt(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt);
    SelectObject(dc, old);
}


// ---------------------------------------------------------------- window chrome (DWM)
// DWMWA_USE_IMMERSIVE_DARK_MODE 20/19, DWMWA_BORDER_COLOR 34,
// DWMWA_CAPTION_COLOR 35, DWMWA_TEXT_COLOR 36  (Windows 11; older builds ignore them)
static void ApplyDwmChrome(HWND hwnd)
{
    if (!hwnd) return;
    BOOL dark = g_pal.dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    COLORREF border = g_pal.dark ? g_pal.panel : g_pal.border;
    COLORREF caption = g_pal.dark ? RGB(0x1A, 0x1A, 0x1D) : RGB(0xF2, 0xF4, 0xF8);
    COLORREF text = g_pal.text;
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
}

// ---------------------------------------------------------------- treemap layout (implemented in ui_treemap.cpp)
struct ScanJob
{
    Tree tree;
    bool usedMft = false;
    bool ok = false;
};

struct ViewState
{
    HWND hwnd = nullptr;
    HWND canvas = nullptr;
    bool canvasTimer = false;
    ss::ui::TreemapRenderer renderer;
    HWND editFilter = nullptr;
    std::wstring path;
    std::wstring title;
    std::wstring status;
    Tree tree;
    Tree filtered;
    bool usingFiltered = false;
    uint32_t focus = 0;
    uint32_t selected = Node::kNone;
    uint32_t hover = Node::kNone;
    std::vector<uint32_t> back, forward;
    std::vector<ss::ui::TreemapBox> boxes;
    int mouseX = -1, mouseY = -1;
    bool trackingMouse = false;
    bool scanning = false;
    bool usedMft = false;
    std::thread worker;
    std::unique_ptr<ScanJob> scanJob;
    ScanProgress progress;
    int64_t lastReportedFiles = -1;
    int scrollY = 0;
    int lastW = -1, lastH = -1;
    bool filterVisible = false;
    std::wstring filterText;

    ~ViewState()
    {
        InterlockedExchange(&progress.state, 2);
        if (worker.joinable()) worker.join();
    }
};

static std::vector<std::unique_ptr<ViewState>> g_views;
static int g_activeView = -1;
static HWND g_main = nullptr;
static HWND g_start = nullptr;
static HWND g_log = nullptr;
struct HelpWindowEntry { HWND hwnd = nullptr; int which = 0; };
static std::vector<HelpWindowEntry> g_helpWindows;
static bool g_showStart = true;
static int  g_menuOpen = -1;      // index of the open menu-bar item, -1 = none
static int  g_menuHover = -1;     // hovered menu-bar item (edge-triggered repaint)
static bool g_menuTracking = false;
static HWND g_menuBar = nullptr;
static int  g_toolHoverId = 0;
static bool g_mainTrackMouse = false;
static bool g_startTrackMouse = false;
static IContextMenu2* g_shellCm2 = nullptr;
static IContextMenu3* g_shellCm3 = nullptr;

// layout rects (client coords of main window)
static RECT g_rcMenuBar, g_rcToolbar, g_rcTabs, g_rcContent, g_rcStatus;
static int g_lastClientW = -1, g_lastClientH = -1;

struct TabHit { int index; bool close; RECT rect; };
static std::vector<TabHit> g_tabHits;

struct ToolHit { int id; RECT rect; std::wstring text; };
static std::vector<ToolHit> g_toolHits;

enum
{
    CMD_NEW = 1000, CMD_EXPORT, CMD_SNAPSHOT, CMD_EXIT, CMD_CONFIG,
    CMD_BACK, CMD_FWD, CMD_UP, CMD_HOME, CMD_REFRESH, CMD_DETAIL_MINUS, CMD_DETAIL_PLUS,
    CMD_STYLE, CMD_FREE, CMD_UNKNOWN, CMD_DARK, CMD_LOG, CMD_HELP_FILTER, CMD_HELP_TAG,
    CMD_HELP_CLI, CMD_HELP_MCP, CMD_ABOUT, CMD_TAG1, CMD_TAG2, CMD_TAG3, CMD_TAG4, CMD_TAG0, CMD_STOP,
    CMD_FILTERBAR
};

static void LogLine(const std::wstring& s);
static void ApplyFilter(ViewState* v);
static void RelayoutView(ViewState* v);
static void UpdateViewStatus(ViewState* v);
static void RefreshMain();
static void DoCommand(int cmd);
static void ShowShellContextMenu(HWND owner, const std::wstring& path, POINT screenPt);
static void OpenScanView(const std::wstring& path, const std::wstring& filter);
static void OpenSnapshotView(const std::wstring& file);
static void ResetStartAdminHintState();
static bool RelaunchElevated(HWND owner, DWORD* outErr = nullptr);
static void SyncShellIntegration(bool enable, bool logSuccess);
static void ApplyThemeToEditWindow(HWND hwnd, HWND edit);
static void RefreshAuxWindows(bool updateText);
static void ShowConfigDlg(HWND owner)
{
    bool prevShell = g_cfg.shellIntegration;
    if (ss::ui::ShowConfigDialog(owner, g_cfg, g_dpi, g_pal.dark))
    {
        g_cfg.Save();
        ss::i18n::Init(g_cfg.language);
        g_isElevated = IsProcessElevated();
        InitTheme(g_cfg.darkMode);
        RebuildFonts();
        ApplyDwmChrome(g_main);
        if (g_main) SetWindowTextW(g_main, AppTitle());
        // push the new settings into every open view
        for (auto& vp : g_views)
        {
            if (!vp->hwnd) continue;
            if (!vp->scanning)
            {
                if (vp->tree.Valid(vp->tree.root)) vp->tree.ApplyGroups(g_cfg.classes);
                if (vp->usingFiltered && vp->filtered.Valid(vp->filtered.root))
                    vp->filtered.ApplyGroups(g_cfg.classes);
                UpdateViewStatus(vp.get());
            }
            if (vp->editFilter) SendMessageW(vp->editFilter, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
            RelayoutView(vp.get());
            InvalidateRect(vp->hwnd, nullptr, TRUE);
        }
        ResetStartAdminHintState();
        if (g_menuBar) InvalidateRect(g_menuBar, nullptr, TRUE);
        if (g_start)
        {
            InvalidateRect(g_start, nullptr, TRUE);
            UpdateWindow(g_start);
        }
        RefreshAuxWindows(true);
        LogLine(std::wstring(ss::i18n::T(L"log.cfg_saved")) + g_cfg.path);
        SyncShellIntegration(g_cfg.shellIntegration, g_cfg.shellIntegration || prevShell);
        if (g_main)
            RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        RefreshMain();
    }
    else
    {
        // P1: if the user previewed another language and clicked Cancel, repaint main UI
        // so it reflects the restored language cleanly.
        if (g_menuBar) InvalidateRect(g_menuBar, nullptr, TRUE);
        if (g_start) InvalidateRect(g_start, nullptr, TRUE);
        RefreshMain();
    }
}

static void ShowExportDlg(HWND owner);
static void ShowLogWindow(HWND owner);
static void ShowHelpDlg(HWND owner, int which);
static void ShowAboutDlg(HWND owner);

// ---------------------------------------------------------------- view helpers
static ViewState* CurrentView()
{
    if (g_activeView < 0 || g_activeView >= (int)g_views.size()) return nullptr;
    return g_views[g_activeView].get();
}

static Tree& ActiveTree(ViewState* v) { return (v->usingFiltered && v->filtered.Valid(v->filtered.root)) ? v->filtered : v->tree; }

static int ViewCanvasTop(const ViewState* v) { return v->filterVisible ? S(64) : S(30); }

static double AnimationTimeMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static void SetCanvasTimer(ViewState* v, bool running)
{
    if (!v->canvas || running == v->canvasTimer) return;
    if (running)
        v->canvasTimer = SetTimer(v->canvas, 1, 16, nullptr) != 0;
    else {
        KillTimer(v->canvas, 1);
        v->canvasTimer = false;
    }
}

static void CancelViewAnimation(ViewState* v)
{
    SetCanvasTimer(v, false);
    ss::ui::TreemapAnimCancel(v->hwnd);
}

static void SyncCanvas(ViewState* v, const RECT& rc)
{
    if (!v->canvas) return;
    int top = ViewCanvasTop(v);
    if (v->scanning || rc.right <= 0 || rc.bottom <= top) {
        ShowWindow(v->canvas, SW_HIDE);
        v->renderer.Reset();
        return;
    }
    SetWindowPos(v->canvas, nullptr, 0, top, rc.right, rc.bottom - top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static RECT ComputeViewLayoutArea(ViewState* v, const RECT& clientRc)
{
    int pad = S(8);
    return RECT{ pad, ViewCanvasTop(v), clientRc.right - pad, clientRc.bottom - pad };
}

static void RelayoutView(ViewState* v)
{
    if (!v) return;
    if (v->hwnd) CancelViewAnimation(v);
    v->boxes.clear();
    if (!v->hwnd) return;
    RECT rc;
    GetClientRect(v->hwnd, &rc);
    SyncCanvas(v, rc);
    v->lastW = rc.right;
    v->lastH = rc.bottom;
    RECT lay = ComputeViewLayoutArea(v, rc);
    double w = lay.right - lay.left;
    double h = lay.bottom - lay.top;
    if (w <= 0 || h <= 0) return;

    int propClamped = std::clamp(g_cfg.proportions, -60, 60);
    double warp = std::pow(2.0, propClamped / 100.0);
    double cx = lay.left + w / 2.0, cy = lay.top + h / 2.0;
    double ax = cx - w * warp / 2.0, ay = cy - h / warp / 2.0;
    double aw = w * warp, ah = h / warp;

    if (v->scanning) return;
    Tree& t = ActiveTree(v);
    if (!t.Valid(v->focus)) return;

    ss::ui::TreemapLayoutOptions opt;
    opt.minElementSizePx = (double)std::max(1, S(std::clamp(g_cfg.minElementSizePx, 3, 64)));
    opt.detailLevel      = std::clamp(g_cfg.initialDetailLevel, 1, 15);
    opt.autoDrill        = true;
    opt.maxDepth         = 9;
    opt.maxBoxes         = 9000;
    opt.sortBySize       = g_cfg.useSorting;
    opt.showFree         = g_cfg.showFreeSpace;
    opt.showUnknown      = g_cfg.showUnknownSpace;
    opt.dpi              = g_dpi;

    ss::ui::LayoutTreemap(t, v->focus, ax, ay, aw, ah, opt, v->boxes);
}

static void NavigateFocus(ViewState* v, uint32_t newFocus)
{
    if (!v || v->scanning) return;
    Tree& t = ActiveTree(v);
    if (!t.Valid(newFocus)) return;

    uint32_t oldFocus = v->focus;
    std::vector<ss::ui::TreemapBox> oldBoxes;
    if (v->hwnd)
    {
        if (const auto* curAnim = ss::ui::TreemapAnimCurrentBoxes(v->hwnd))
            oldBoxes = *curAnim;
        else
            oldBoxes = v->boxes;
    }

    v->focus = newFocus;
    v->selected = Node::kNone;
    v->hover = Node::kNone;
    RelayoutView(v);

    if (v->hwnd)
    {
        int animMs = (!g_cfg.useDynaSmooth || g_cfg.zoomTimeMs <= 0)
            ? 0
            : (g_cfg.zoomTimeMs == 250 ? 180 : std::clamp(g_cfg.zoomTimeMs, 50, 1000));
        if (v->canvas && animMs > 0 && !oldBoxes.empty() && !v->boxes.empty())
        {
            RECT rc{};
            GetClientRect(v->hwnd, &rc);
            RECT lay = ComputeViewLayoutArea(v, rc);
            ss::ui::TreemapAnimStart(v->hwnd, t, oldFocus, v->focus, lay,
                                     oldBoxes, v->boxes, AnimationTimeMs(), animMs);
        }
        InvalidateRect(v->hwnd, nullptr, FALSE);
    }
}

static ss::ui::TreemapStyle ViewTreemapStyle()
{
    ss::ui::TreemapStyle st;
    st.dark = g_pal.dark;
    st.bg = g_pal.canvas;
    st.driveColor = g_cfg.driveColor;
    st.folderColor = g_cfg.folderColor;
    st.fileColor = g_cfg.fileColor;
    st.freeColor = g_cfg.freeColor;
    st.unknownColor = g_cfg.unknownColor;
    st.accent = g_pal.accent;
    st.text = g_pal.text;
    st.textDim = g_pal.textDim;
    st.levelContrast = g_cfg.levelContrast;
    st.borderContrast = g_cfg.borderContrast;
    st.fontSize = g_cfg.fontSize;
    st.dpi = g_dpi;
    st.showFileClassColors = (g_cfg.initialStyle == L"File Classes");
    st.classes = &g_cfg.classes.items;
    return st;
}

static ss::ui::TreemapOverlay ViewTreemapOverlay(const ViewState* v)
{
    return { v->hover, v->selected, v->mouseX, v->mouseY, g_cfg.showTooltipDate, g_cfg.showTooltipAge };
}

static void DrawView(ViewState* v, HDC target, RECT rc)
{
    if (v->canvas && !v->scanning)
        rc.bottom = std::min<int>(rc.bottom, ViewCanvasTop(v));
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    if (!mem || !bmp)
    {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);

    Fill(mem, rc, g_pal.bg);

    // ---- filter row (hidden by default; toggled from the toolbar / Ctrl+L)
    if (v->filterVisible)
    {
        TextAt(mem, ss::i18n::T(L"view.filter_label"), RECT{ S(8), S(10), S(8) + S(50), S(38) }, g_pal.textDim, g_fontUI,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        RECT editBox{ S(78), S(12), W - S(196), S(38) };
        Frame(mem, editBox, g_pal.border);
        RECT btnFilter{ W - S(188), S(12), W - S(104), S(38) };
        Fill(mem, btnFilter, g_pal.accent);
        TextAt(mem, ss::i18n::T(L"view.filter_btn"), btnFilter, RGB(255, 255, 255), g_fontUI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT btnClear{ W - S(96), S(12), W - S(64), S(38) };
        Frame(mem, btnClear, g_pal.border);
        TextAt(mem, L"×", btnClear, g_pal.textDim, g_fontUIBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!v->status.empty())
            TextAt(mem, v->status, RECT{ W - S(560), S(10), W - S(196), S(38) }, g_pal.textDim, g_fontSmall,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // ---- scanning placeholder
    if (v->scanning)
    {
        std::wstring msg = std::wstring(ss::i18n::T(L"view.scanning_prefix")) + v->path + ss::i18n::T(L"view.scanning_ellipsis");
        if (!v->status.empty()) msg += L"  (" + v->status + L")";
        TextAt(mem, msg, RECT{ S(20), S(120), W - S(20), S(160) },
               g_pal.textDim, g_fontUI, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        BitBlt(target, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        return;
    }

    Tree& t = ActiveTree(v);
    std::wstring crumb = t.Valid(v->focus) ? t.Path(v->focus) : v->path;
    if (t.Valid(v->focus))
        crumb += L"    [" + FmtSize(t.nodes[v->focus].disk) + L" / " + FmtSize(t.nodes[v->focus].size) + L"]";
    {
        int cy = v->filterVisible ? S(40) : S(4);
        int right = W - S(10);
        if (!v->filterVisible && !v->status.empty())
        {
            TextAt(mem, v->status, RECT{ right - S(320), cy, right, cy + S(20) }, g_pal.textDim,
                   g_fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            right -= S(330);
        }
        TextAt(mem, crumb, RECT{ S(10), cy, right, cy + S(20) }, g_pal.textDim, g_fontSmall);
    }

    if (!v->canvas)
        ss::ui::DrawTreemapFrame(mem, v->hwnd, RECT{ 0, ViewCanvasTop(v), W, H },
                                 t, v->boxes, ViewTreemapStyle(), ViewTreemapOverlay(v));

    BitBlt(target, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static uint32_t HitTest(ViewState* v, int x, int y)
{
    uint32_t best = Node::kNone;
    int bestDepth = -1;
    for (auto& b : v->boxes)
    {
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
        {
            if (b.depth > bestDepth) { bestDepth = b.depth; best = b.node; }
        }
    }
    return best;
}

static void UpdateViewStatus(ViewState* v)
{
    Tree& t = ActiveTree(v);
    if (!t.Valid(t.root)) { v->status = L""; return; }
    int64_t files = 0, dirs = 0;
    for (size_t i = 0; i < t.nodes.size(); ++i)
    {
        if (t.nodes[i].kind == (uint8_t)Kind::File) files++;
        else dirs++;
    }
    std::wstring mode = v->usedMft ? ss::i18n::T(L"view.mode.mft") : ss::i18n::T(L"view.mode.enum");
    v->status = FmtCount(files) + ss::i18n::T(L"view.status.files") + FmtCount(dirs) + ss::i18n::T(L"view.status.dirs") +
                FmtSize(t.nodes[t.root].disk) + L" · " + mode;
}

// ---------------------------------------------------------------- scanning thread
// The worker thread ONLY writes to its private ScanJob and ScanProgress; it never
// touches ViewState::tree, any HWND, or g_cfg.
static void StartScan(ViewState* v, const std::wstring& path)
{
    if (!v || v->scanning) return;
    if (v->worker.joinable()) v->worker.join();

    v->path = path;
    v->tree.Clear();
    v->filtered.Clear();
    v->usingFiltered = false;
    v->back.clear();
    v->forward.clear();
    v->scanning = true;
    v->usedMft = false;
    v->status = ss::i18n::T(L"view.status.scanning");
    v->title = std::wstring(ss::i18n::T(L"view.title.scanning")) + path;
    v->focus = 0;
    v->selected = Node::kNone;
    v->hover = Node::kNone;
    if (v->hwnd)
    {
        CancelViewAnimation(v);
        RECT rc{};
        GetClientRect(v->hwnd, &rc);
        SyncCanvas(v, rc);
        InvalidateRect(v->hwnd, nullptr, FALSE);
    }
    v->boxes.clear();
    v->lastReportedFiles = -1;

    InterlockedExchange64(&v->progress.files, 0);
    InterlockedExchange64(&v->progress.dirs, 0);
    InterlockedExchange64(&v->progress.bytes, 0);
    InterlockedExchange(&v->progress.state, 0);
    v->progress.message[0] = 0;

    // Capture all config options by value on the UI thread before spawning worker.
    ScanOptions opt;
    opt.mode = g_cfg.useMftScanner ? ScanOptions::Auto : ScanOptions::Fast;
    opt.scanAds = g_cfg.scanAds;

    v->scanJob = std::make_unique<ScanJob>();
    ScanJob* job = v->scanJob.get();
    ScanProgress* prog = &v->progress;

    v->worker = std::thread([path, opt, job, prog]()
    {
        bool usedMft = false;
        bool ok = ScanPath(path, opt, job->tree, prog, &usedMft);
        job->usedMft = usedMft;
        job->ok = ok;
        LONG finalState = ok ? 1 : (InterlockedCompareExchange(&prog->state, 0, 0) == 2 ? 2 : 3);
        InterlockedExchange(&prog->state, finalState);
    });
    if (v->hwnd) SetTimer(v->hwnd, 1, 80, nullptr);
}

static void FinishScan(ViewState* v)
{
    if (!v || !v->scanning) return;
    v->scanning = false;
    if (v->hwnd) KillTimer(v->hwnd, 1);
    if (v->worker.joinable()) v->worker.join();

    bool ok = false;
    if (v->scanJob)
    {
        ok = v->scanJob->ok;
        v->usedMft = v->scanJob->usedMft;
        if (ok) v->tree = std::move(v->scanJob->tree);
        v->scanJob.reset();
    }
    if (v->progress.message[0] != 0)
        LogLine(std::wstring(v->progress.message));

    if (ok && v->tree.Valid(v->tree.root))
    {
        v->tree.ApplyGroups(g_cfg.classes);
        v->focus = v->tree.root;
        v->title = v->tree.Path(v->tree.root);
        UpdateViewStatus(v);
        LogLine(std::wstring(ss::i18n::T(L"log.scan_done")) + v->tree.Path(v->tree.root) + L"  (" + v->status + L")");
        if (!v->filterText.empty())
        {
            ApplyFilter(v);
            return;
        }
    }
    else
    {
        v->status = ss::i18n::T(L"view.status.scan_failed");
        LogLine(std::wstring(ss::i18n::T(L"log.scan_failed")) + v->path);
    }
    RelayoutView(v);
    if (v->hwnd) InvalidateRect(v->hwnd, nullptr, FALSE);
    RefreshMain();
}

static void DrawCanvasGdi(HDC dc, ViewState* v, const RECT& area, const Tree& tree,
                          const ss::ui::TreemapStyle& style, const ss::ui::TreemapOverlay& overlay)
{
    int width = area.right;
    int height = area.bottom - area.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = mem ? CreateCompatibleBitmap(dc, width, height) : nullptr;
    HGDIOBJ previous = bitmap ? SelectObject(mem, bitmap) : nullptr;
    HDC target = previous ? mem : dc;
    POINT origin{};
    SetViewportOrgEx(target, 0, -area.top, &origin);
    ss::ui::DrawTreemapFrame(target, v->hwnd, area, tree, v->boxes, style, overlay);
    SetViewportOrgEx(target, origin.x, origin.y, nullptr);
    if (previous) {
        BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (mem) DeleteDC(mem);
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* v = reinterpret_cast<ViewState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
        case WM_NCCREATE:
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (wp == 1) {
                if (v && v->canvasTimer) InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            bool nextFrame = false;
            auto present = ss::ui::TreemapPresent::Occluded;
            if (v && !v->scanning && rc.right > 0 && rc.bottom > 0 &&
                IsWindowVisible(hwnd) && !IsIconic(GetAncestor(hwnd, GA_ROOT))) {
                nextFrame = ss::ui::TreemapAnimTick(v->hwnd, AnimationTimeMs());
                int top = ViewCanvasTop(v);
                RECT area{ 0, top, rc.right, top + rc.bottom };
                const Tree& tree = ActiveTree(v);
                auto style = ViewTreemapStyle();
                auto overlay = ViewTreemapOverlay(v);
                present = v->renderer.Draw(hwnd, dc, v->hwnd, area, tree, v->boxes, style, overlay);
                if (present == ss::ui::TreemapPresent::Fallback)
                    DrawCanvasGdi(dc, v, area, tree, style, overlay);
            }
            EndPaint(hwnd, &ps);
            bool vsynced = present == ss::ui::TreemapPresent::Presented;
            if (v) SetCanvasTimer(v, nextFrame && !vsynced);
            // EndDraw presents at the display refresh. Keep one paint pending during zoom.
            if (nextFrame && vsynced) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            if (v) {
                SetCanvasTimer(v, false);
                v->renderer.Reset();
                v->canvas = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- view window proc
static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ViewState* v = (ViewState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
        case WM_CREATE:
        {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            if (!cs || !cs->lpCreateParams) return -1;
            v = (ViewState*)cs->lpCreateParams;
            v->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)v);
            v->editFilter = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                            S(80), S(14), 100, S(22), hwnd, (HMENU)1,
                                            GetModuleHandleW(nullptr), nullptr);
            SendMessageW(v->editFilter, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
            SendMessageW(v->editFilter, EM_SETCUEBANNER, TRUE, (LPARAM)L"*.iso;>1gb;>2weeks;:red");
            ShowWindow(v->editFilter, SW_HIDE);
            v->canvas = CreateWindowExW(0, kCanvasClass, L"", WS_CHILD, 0, 0, 0, 0,
                                        hwnd, nullptr, cs->hInstance, v);
            return 0;
        }
        case WM_SIZE:
        {
            if (v && v->editFilter)
            {
                int w = LOWORD(lp);
                SetWindowPos(v->editFilter, nullptr, S(80), S(14), std::max(50, w - S(200) - S(80)), S(22),
                             SWP_NOZORDER);
            }
            if (v) { RelayoutView(v); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (v)
            {
                if (rc.right != v->lastW || rc.bottom != v->lastH) RelayoutView(v);
                DrawView(v, dc, rc);
            }
            EndPaint(hwnd, &ps);
            if (v && v->canvas && !v->scanning) InvalidateRect(v->canvas, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SHOWWINDOW:
            if (!wp && v && v->canvas) {
                CancelViewAnimation(v);
                v->renderer.Reset();
            }
            break;

        case WM_CTLCOLOREDIT:
        {
            HDC dc = (HDC)wp;
            COLORREF bg = g_pal.dark ? RGB(0x33, 0x33, 0x36) : RGB(0xFF, 0xFF, 0xFF);
            SetTextColor(dc, g_pal.text);
            SetBkColor(dc, bg);
            return (LRESULT)GetEditBrush(bg);
        }

        case WM_MOUSEMOVE:
        {
            if (!v) return 0;
            if (!v->trackingMouse)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                v->trackingMouse = true;
            }
            if (v->scanning) return 0;
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            bool moved = (x != v->mouseX || y != v->mouseY);
            v->mouseX = x; v->mouseY = y;
            uint32_t hit = HitTest(v, x, y);
            if (hit != v->hover || (moved && hit != Node::kNone)) {
                v->hover = hit;
                InvalidateRect(v->canvas ? v->canvas : hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (v)
            {
                v->trackingMouse = false;
                if (v->hover != Node::kNone || v->mouseX != -1)
                {
                    v->hover = Node::kNone;
                    v->mouseX = -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;

        case WM_LBUTTONDOWN:
        {
            if (!v) return 0;
            SetFocus(hwnd);
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (v->filterVisible && y >= S(12) && y <= S(38))
            {
                if (x >= rc.right - S(188) && x <= rc.right - S(104)) { ApplyFilter(v); return 0; }
                if (x >= rc.right - S(96) && x <= rc.right - S(64))
                {
                    if (v->editFilter) SetWindowTextW(v->editFilter, L"");
                    v->filterText.clear();
                    ApplyFilter(v);
                    return 0;
                }
            }
            int canvasTop = v->filterVisible ? S(64) : S(30);
            if (v->scanning || y < canvasTop) return 0;
            v->selected = HitTest(v, x, y);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
        {
            if (!v || v->scanning) return 0;
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int canvasTop = v->filterVisible ? S(64) : S(30);
            if (y < canvasTop) return 0;
            uint32_t hit = HitTest(v, x, y);
            Tree& t = ActiveTree(v);
            if (t.Valid(hit) && t.nodes[hit].IsFolder())
            {
                v->back.push_back(v->focus);
                v->forward.clear();
                NavigateFocus(v, hit);
            }
            return 0;
        }
        case WM_RBUTTONUP:
        {
            if (!v || v->scanning) return 0;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int canvasTop = v->filterVisible ? S(64) : S(30);
            if (pt.y < canvasTop) return 0;
            uint32_t hit = HitTest(v, pt.x, pt.y);
            Tree& t = ActiveTree(v);
            if (!t.Valid(hit)) return 0;
            v->selected = hit;
            v->hover = Node::kNone;      // the tooltip must disappear under the menu
            v->mouseX = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);

            std::wstring nodePath = t.Path(hit);
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            ShowShellContextMenu(g_main, nodePath, screen);
            return 0;
        }
        case WM_TIMER:
            if (v && wp == 1 && v->scanning)
            {
                LONG st = InterlockedCompareExchange(&v->progress.state, 0, 0);
                if (st != 0)
                {
                    FinishScan(v);
                }
                else
                {
                    int64_t files = InterlockedCompareExchange64(&v->progress.files, 0, 0);
                    if (files != v->lastReportedFiles)
                    {
                        v->lastReportedFiles = files;
                        v->status = std::wstring(ss::i18n::T(L"view.status.scanning")) + L" " + FmtCount(files) + ss::i18n::T(L"view.status.scanning_files");
                        InvalidateRect(hwnd, nullptr, FALSE);
                        if (g_main) InvalidateRect(g_main, &g_rcStatus, FALSE);
                    }
                }
            }
            return 0;

        case WM_DPICHANGED:
            if (v) { RelayoutView(v); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            if (v) {
                CancelViewAnimation(v);
                v->hwnd = nullptr;
            }
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- start page
struct DriveRow { std::wstring root, label, fs, freeTotal; RECT rect; };

static std::vector<DriveRow> g_driveRows;
static int g_startHover = -1;
static int g_startScrollY = 0;
static int g_startContentH = 0;

static void BuildDriveRows()
{
    g_driveRows.clear();
    wchar_t buf[MAX_PATH];
    DWORD mask = GetLogicalDrives();
    int y = S(120);
    for (int i = 0; i < 26; ++i)
    {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        wchar_t label[MAX_PATH] = {}, fs[MAX_PATH] = {};
        GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH);
        ULARGE_INTEGER avail{}, total{};
        GetDiskFreeSpaceExW(root, &avail, &total, nullptr);
        DriveRow r;
        r.root = root;
        r.label = label;
        r.fs = fs;
        r.freeTotal = FmtSize((int64_t)avail.QuadPart) + L" / " + FmtSize((int64_t)total.QuadPart);
        r.rect = RECT{ S(20), y, 0, y + S(34) };
        y += S(36);
        g_driveRows.push_back(r);
        (void)buf;
    }
}

static RECT g_browseRect = { 0, 0, 0, 0 };
static bool g_browseHover = false;
static RECT g_adminRestartRect = { 0, 0, 0, 0 };
static RECT g_adminDismissRect = { 0, 0, 0, 0 };
static bool g_adminRestartHover = false;
static bool g_adminDismissHover = false;

static void ResetStartAdminHintState()
{
    g_adminRestartHover = false;
    g_adminDismissHover = false;
    if (g_cfg.hideAdminHint || g_isElevated)
    {
        g_adminRestartRect = { 0, 0, 0, 0 };
        g_adminDismissRect = { 0, 0, 0, 0 };
    }
}

static void RestartAsAdmin()
{
    // P1: reuse RelaunchElevated (preserving CLI args), only close on success, and log
    // UAC cancel / failure without leaving a half-closed state.
    DWORD err = ERROR_SUCCESS;
    if (RelaunchElevated(g_main, &err))
    {
        if (g_main) PostMessageW(g_main, WM_CLOSE, 0, 0);
        return;
    }
    g_adminRestartHover = false;
    if (g_start) InvalidateRect(g_start, nullptr, FALSE);
    if (err == ERROR_CANCELLED)
    {
        LogLine(ss::i18n::T(L"log.runas_cancelled"));
    }
    else
    {
        LogLine(std::wstring(ss::i18n::T(L"log.runas_failed")) + std::to_wstring(err) + L")");
    }
}

static void StartBrowseForFolder(HWND owner)
{
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))) || !dlg)
        return;
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(ss::i18n::T(L"start.browse_title"));
    if (SUCCEEDED(dlg->Show(owner)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item)
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            {
                OpenScanView(path, L"");
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

static void DrawStart(HDC target, const RECT& rc)
{
    using ss::i18n::T;
    if (rc.right <= 0 || rc.bottom <= 0) return;
    // double buffer: the previous version painted straight to the window DC, which is why
    // fast hover movement flickered
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    if (!dc || !bmp)
    {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    // P1: compute full start-page height and clamp vertical scroll so the last drive row,
    // Browse button, and Admin card are reachable and never clipped by the bottom edge/footer.
    int totalH = S(62) + S(26) + (int)g_driveRows.size() * (S(32) + 1) + S(12) + S(28);
    if (!g_isElevated && !g_cfg.hideAdminHint)
        totalH += S(16) + S(56);
    totalH += S(20); // bottom breathing room above status bar
    g_startContentH = totalH;
    int maxScroll = std::max(0, g_startContentH - (int)rc.bottom);
    g_startScrollY = std::clamp(g_startScrollY, 0, maxScroll);
    bool hasScroll = maxScroll > 0;
    int sbReserve = hasScroll ? S(12) : 0;
    int rightEdge = std::max(S(20), (int)rc.right - S(20) - sbReserve);

    Fill(dc, rc, g_pal.bg);
    TextAt(dc, T(L"start.title"),
           RECT{ S(24), S(18) - g_startScrollY, rightEdge, S(52) - g_startScrollY },
           g_pal.text, g_fontUIBold);

    int listTop = S(62) - g_startScrollY;
    RECT hdr{ S(20), listTop, rightEdge, listTop + S(26) };
    Fill(dc, hdr, g_pal.panelAlt);
    int c1R = std::min(S(120), rightEdge);
    int c2R = std::min(S(320), rightEdge);
    int c3R = std::min(S(430), rightEdge);
    int c4R = std::max(S(28), rightEdge - S(8));
    if (S(28) < c1R) TextAt(dc, T(L"start.col.drive"), RECT{ S(28), hdr.top, c1R, hdr.bottom }, g_pal.textDim, g_fontSmall);
    if (S(130) < c2R) TextAt(dc, T(L"start.col.label"), RECT{ S(130), hdr.top, c2R, hdr.bottom }, g_pal.textDim, g_fontSmall);
    if (S(330) < c3R) TextAt(dc, T(L"start.col.fs"), RECT{ S(330), hdr.top, c3R, hdr.bottom }, g_pal.textDim, g_fontSmall);
    if (S(440) < c4R) TextAt(dc, T(L"start.col.space"), RECT{ S(440), hdr.top, c4R, hdr.bottom }, g_pal.textDim, g_fontSmall);

    int y = hdr.bottom;
    for (size_t i = 0; i < g_driveRows.size(); ++i)
    {
        RECT r{ S(20), y, rightEdge, y + S(32) };
        g_driveRows[i].rect = r;
        if (r.bottom > 0 && r.top < rc.bottom)
        {
            if ((int)i == g_startHover) Fill(dc, r, g_pal.panelAlt);
            if (S(28) < c1R) TextAt(dc, g_driveRows[i].root, RECT{ S(28), r.top, c1R, r.bottom }, g_pal.text, g_fontUI);
            if (S(130) < c2R) TextAt(dc, g_driveRows[i].label, RECT{ S(130), r.top, c2R, r.bottom }, g_pal.text, g_fontUI);
            if (S(330) < c3R) TextAt(dc, g_driveRows[i].fs, RECT{ S(330), r.top, c3R, r.bottom }, g_pal.textDim, g_fontUI);
            if (S(440) < c4R) TextAt(dc, g_driveRows[i].freeTotal, RECT{ S(440), r.top, c4R, r.bottom },
                                     g_pal.textDim, g_fontUI);
            Frame(dc, RECT{ r.left, r.bottom, r.right, r.bottom + 1 }, g_pal.border);
        }
        y = r.bottom + 1;
    }

    // Browse… button under the drive list
    const wchar_t* browseLabel = T(L"start.browse");
    SIZE browseSz{};
    {
        HGDIOBJ oldF = SelectObject(dc, g_fontUI);
        GetTextExtentPoint32W(dc, browseLabel, (int)wcslen(browseLabel), &browseSz);
        SelectObject(dc, oldF);
    }
    int browseW = std::max(S(104), (int)browseSz.cx + S(28));
    g_browseRect = RECT{ S(20), y + S(12), S(20) + browseW, y + S(12) + S(28) };
    Fill(dc, g_browseRect, g_browseHover ? g_pal.panelAlt : g_pal.panel);
    Frame(dc, g_browseRect, g_browseHover ? g_pal.accent : g_pal.border);
    TextAt(dc, browseLabel, g_browseRect, g_pal.text, g_fontUI,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Non-administrator hint card (Start page only, when not elevated and not dismissed)
    g_adminRestartRect = { 0, 0, 0, 0 };
    g_adminDismissRect = { 0, 0, 0, 0 };
    if (!g_isElevated && !g_cfg.hideAdminHint)
    {
        int cardTop = g_browseRect.bottom + S(16);
        int cardH = S(56);
        RECT card{ S(20), cardTop, rightEdge, cardTop + cardH };
        if (card.right - card.left > S(220))
        {
            COLORREF cardBg = g_pal.dark ? RGB(0x1D, 0x3A, 0x5F) : RGB(0xE8, 0xF1, 0xFD);
            COLORREF cardBorder = g_pal.dark ? RGB(0x2B, 0x52, 0x82) : RGB(0xB8, 0xD2, 0xF5);
            FillRoundRect(dc, card, S(2), cardBg, cardBorder, true);
            // Left 1px accent vertical bar
            Fill(dc, RECT{ card.left, card.top + 1, card.left + 1, card.bottom - 1 }, g_pal.accent);

            const wchar_t* restartTxt = T(L"start.admin_restart");
            const wchar_t* dismissTxt = L"×";
            SIZE szRestart{}, szDismiss{};
            {
                HGDIOBJ oldF = SelectObject(dc, g_fontUIBold);
                GetTextExtentPoint32W(dc, restartTxt, (int)wcslen(restartTxt), &szRestart);
                SelectObject(dc, g_fontUI);
                GetTextExtentPoint32W(dc, dismissTxt, (int)wcslen(dismissTxt), &szDismiss);
                SelectObject(dc, oldF);
            }
            int btnH = S(28);
            int btnY = card.top + (cardH - btnH) / 2;
            int dismissW = (int)szDismiss.cx + S(18);
            int restartW = (int)szRestart.cx + S(22);

            g_adminDismissRect = RECT{ card.right - S(12) - dismissW, btnY, card.right - S(12), btnY + btnH };
            g_adminRestartRect = RECT{ g_adminDismissRect.left - S(8) - restartW, btnY,
                                       g_adminDismissRect.left - S(8), btnY + btnH };

            // Restart as Admin text button
            COLORREF rBg = g_adminRestartHover
                ? g_pal.accent
                : (g_pal.dark ? RGB(0x27, 0x4E, 0x7E) : RGB(0xD4, 0xE5, 0xFC));
            COLORREF rFg = g_adminRestartHover
                ? RGB(255, 255, 255)
                : (g_pal.dark ? RGB(0xEC, 0xF4, 0xFF) : g_pal.accent);
            FillRoundRect(dc, g_adminRestartRect, S(2), rBg, g_pal.accent, true);
            TextAt(dc, restartTxt, g_adminRestartRect, rFg, g_fontUIBold,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Dismiss text button
            if (g_adminDismissHover)
            {
                COLORREF dBg = g_pal.dark ? RGB(0x26, 0x48, 0x73) : RGB(0xD7, 0xE6, 0xFA);
                FillRoundRect(dc, g_adminDismissRect, S(2), dBg, cardBorder, false);
            }
            TextAt(dc, dismissTxt, g_adminDismissRect,
                   g_adminDismissHover ? g_pal.text : g_pal.textDim, g_fontUI,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Two text lines on the left
            int textL = card.left + S(14);
            int textR = std::max(textL, (int)(g_adminRestartRect.left - S(12)));
            RECT line1Rc{ textL, card.top + S(8), textR, card.top + S(28) };
            RECT line2Rc{ textL, card.top + S(28), textR, card.bottom - S(8) };
            TextAt(dc, T(L"start.admin_title"), line1Rc, g_pal.text, g_fontUIBold);
            TextAt(dc, T(L"start.admin_desc"), line2Rc, g_pal.textDim, g_fontSmall);
        }
    }

    if (hasScroll)
    {
        int trackTop = S(6);
        int trackBot = std::max(trackTop + 1, (int)rc.bottom - S(6));
        int trackH = trackBot - trackTop;
        int minThumb = std::min(S(24), trackH);
        int thumbH = std::clamp(MulDiv(trackH, (int)rc.bottom, std::max(1, g_startContentH)), minThumb, trackH);
        int thumbTop = trackTop + MulDiv(trackH - thumbH, g_startScrollY, std::max(1, maxScroll));
        RECT track{ rc.right - S(9), trackTop, rc.right - S(3), trackBot };
        RECT thumb{ track.left, thumbTop, track.right, thumbTop + thumbH };
        Fill(dc, track, g_pal.panel);
        Fill(dc, thumb, g_pal.border);
    }

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK StartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE: BuildDriveRows(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            DrawStart(dc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_startTrackMouse)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_startTrackMouse = true;
            }
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            POINT pt{ x, y };
            auto updateBtnHover = [&](const RECT& r, bool& state)
            {
                bool over = (r.right > r.left && r.bottom > r.top) && (PtInRect(&r, pt) != 0);
                if (over != state)
                {
                    state = over;
                    RECT inv = r;
                    InflateRect(&inv, S(2), S(2));
                    InvalidateRect(hwnd, &inv, FALSE);
                }
            };
            updateBtnHover(g_browseRect, g_browseHover);
            updateBtnHover(g_adminRestartRect, g_adminRestartHover);
            updateBtnHover(g_adminDismissRect, g_adminDismissHover);
            int hover = -1;
            for (size_t i = 0; i < g_driveRows.size(); ++i)
                if (y >= g_driveRows[i].rect.top && y < g_driveRows[i].rect.bottom) hover = (int)i;
            if (hover != g_startHover)
            {
                // repaint only the two rows whose highlight changed
                if (g_startHover >= 0 && g_startHover < (int)g_driveRows.size())
                    InvalidateRect(hwnd, &g_driveRows[g_startHover].rect, FALSE);
                g_startHover = hover;
                if (hover >= 0) InvalidateRect(hwnd, &g_driveRows[hover].rect, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_startTrackMouse = false;
            if (g_startHover != -1 || g_browseHover || g_adminRestartHover || g_adminDismissHover)
            {
                g_startHover = -1;
                g_browseHover = false;
                g_adminRestartHover = false;
                g_adminDismissHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
        {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&g_browseRect, pt))
            {
                StartBrowseForFolder(hwnd);
                return 0;
            }
            if (g_adminRestartRect.right > g_adminRestartRect.left && PtInRect(&g_adminRestartRect, pt))
            {
                RestartAsAdmin();
                return 0;
            }
            if (g_adminDismissRect.right > g_adminDismissRect.left && PtInRect(&g_adminDismissRect, pt))
            {
                g_cfg.hideAdminHint = true;
                g_cfg.Save();
                g_adminRestartHover = false;
                g_adminDismissHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
        {
            int y = GET_Y_LPARAM(lp);
            for (auto& r : g_driveRows)
                if (y >= r.rect.top && y < r.rect.bottom) { OpenScanView(r.root, L""); return 0; }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int maxScroll = std::max(0, g_startContentH - (int)rc.bottom);
            if (maxScroll > 0)
            {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                int prev = g_startScrollY;
                g_startScrollY = std::clamp(g_startScrollY + (delta > 0 ? -S(48) : S(48)), 0, maxScroll);
                if (g_startScrollY != prev) InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- main window chrome
struct ToolDef
{
    int id;
    const wchar_t* textKey;
    const wchar_t* tipKey;
    wchar_t glyph;      // icon-font codepoint, 0 = none
    bool iconOnly;      // navigation group: icon without label
    bool isLabel;       // static caption, not clickable
};

static const ToolDef kTools[] = {
    { CMD_NEW,            L"tb.new",          L"tb.new.tip",          0xE710, false, false },
    { 0, nullptr, nullptr, 0, false, false },
    { CMD_BACK,           L"tb.back",         L"tb.back.tip",         0xE72B, true , false },
    { CMD_FWD,            L"tb.fwd",          L"tb.fwd.tip",          0xE72A, true , false },
    { CMD_UP,             L"tb.up",           L"tb.up.tip",           0xE74A, true , false },
    { CMD_HOME,           L"tb.home",         L"tb.home.tip",         0xE80F, true , false },
    { 0, nullptr, nullptr, 0, false, false },
    { CMD_REFRESH,        L"tb.refresh",      L"tb.refresh.tip",      0xE72C, true , false },
    { 0, nullptr, nullptr, 0, false, false },
    { CMD_FILTERBAR,      L"tb.filter",       L"tb.filter.tip",       0xE71C, false, false },
    { 0,                  L"tb.detail",       nullptr,                0,      false, true  },
    { CMD_DETAIL_MINUS,   L"tb.detail_minus", L"tb.detail_minus.tip", 0xE738, true , false },
    { CMD_DETAIL_PLUS,    L"tb.detail_plus",  L"tb.detail_plus.tip",  0xE710, true , false },
    { 0, nullptr, nullptr, 0, false, false },
    { CMD_STYLE,          L"tb.style",        L"tb.style.tip",        0xE8FD, false, false },
    { CMD_FREE,           L"tb.free",         L"tb.free.tip",         0xE9D9, false, false },
    { CMD_UNKNOWN,        L"tb.unknown",      L"tb.unknown.tip",      0xE7BA, false, false },
    { 0, nullptr, nullptr, 0, false, false },
    { CMD_EXPORT,         L"tb.export",       L"tb.export.tip",       0xE74E, false, false },
    { CMD_SNAPSHOT,       L"tb.snapshot",     L"tb.snapshot.tip",     0xE722, false, false },
    { CMD_DARK,           L"tb.dark",         L"tb.dark.tip",         0xE793, false, false },
};

static void LayoutMain(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int mbH = S(26), tbH = S(40), tabH = S(30), stH = S(24);
    g_rcMenuBar = { 0, 0, rc.right, mbH };
    g_rcToolbar = { 0, mbH, rc.right, mbH + tbH };
    g_rcTabs = { 0, mbH + tbH, rc.right, mbH + tbH + tabH };
    g_rcStatus = { 0, rc.bottom - stH, rc.right, rc.bottom };
    g_rcContent = { 0, mbH + tbH + tabH, rc.right, rc.bottom - stH };

    if (g_menuBar)
        SetWindowPos(g_menuBar, nullptr, g_rcMenuBar.left, g_rcMenuBar.top,
                     g_rcMenuBar.right - g_rcMenuBar.left, g_rcMenuBar.bottom - g_rcMenuBar.top,
                     SWP_NOZORDER);
    if (g_start)
        SetWindowPos(g_start, nullptr, g_rcContent.left, g_rcContent.top,
                     g_rcContent.right - g_rcContent.left, g_rcContent.bottom - g_rcContent.top,
                     SWP_NOZORDER);
    for (auto& v : g_views)
        if (v->hwnd)
            SetWindowPos(v->hwnd, nullptr, g_rcContent.left, g_rcContent.top,
                         g_rcContent.right - g_rcContent.left, g_rcContent.bottom - g_rcContent.top,
                         SWP_NOZORDER);
}

static void DrawToolbar(HDC dc)
{
    Fill(dc, g_rcToolbar, g_pal.panelAlt);
    g_toolHits.clear();
    int x = S(6);
    int y = g_rcToolbar.top + S(6);
    int h = g_rcToolbar.bottom - g_rcToolbar.top - S(12);
    for (auto& t : kTools)
    {
        if (!t.textKey)
        {
            RECT sep{ x + S(4), y + S(4), x + S(5), y + h - S(4) };
            Fill(dc, sep, g_pal.border);
            x += S(14);
            continue;
        }

        const wchar_t* label = ss::i18n::T(t.textKey);

        if (t.isLabel)
        {
            SIZE ls{};
            HGDIOBJ oldL = SelectObject(dc, g_fontUI);
            GetTextExtentPoint32W(dc, label, (int)wcslen(label), &ls);
            SelectObject(dc, oldL);
            RECT lr{ x + S(4), y, x + S(4) + ls.cx, y + h };
            TextAt(dc, label, lr, g_pal.textDim, g_fontUI, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x = lr.right + S(4);
            continue;
        }

        static bool cachedOk[64] = {};
        if (!g_toolGlyphCacheValid)
        {
            for (size_t gi = 0; gi < _countof(kTools) && gi < 64; ++gi)
                cachedOk[gi] = g_hasIconFont && kTools[gi].glyph && HasGlyph(kTools[gi].glyph, g_fontIcon);
            g_toolGlyphCacheValid = true;
        }
        size_t toolIndex = (size_t)(&t - kTools);
        bool hasGlyph = toolIndex < 64 ? cachedOk[toolIndex] : false;
        SIZE textSz{ 0, 0 };
        HGDIOBJ old = SelectObject(dc, g_fontUI);
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &textSz);
        SelectObject(dc, old);

        int iconW = hasGlyph ? S(16) : 0;
        int gap = (hasGlyph && !t.iconOnly) ? S(5) : 0;
        int contentW = t.iconOnly ? (hasGlyph ? iconW : textSz.cx) : iconW + gap + textSz.cx;
        bool isSegment = (t.id == CMD_DETAIL_MINUS || t.id == CMD_DETAIL_PLUS);
        int padX = isSegment ? S(8) : (t.iconOnly ? S(11) : S(10));
        RECT r{ x, y, x + contentW + padX * 2, y + h };
        if (isSegment)
        {
            // true square cell, vertically centred inside the toolbar row
            int cell = std::min(h, S(30));
            int top = y + (h - cell) / 2;
            r = RECT{ x, top, x + cell, top + cell };
        }

        bool hot = (t.id != 0 && t.id == g_toolHoverId);

        if (isSegment)
        {
            // shared 1px frame, filled on hover
            Fill(dc, r, hot ? (g_pal.dark ? RGB(0x3A, 0x3A, 0x40) : RGB(0xD8, 0xE8, 0xFC)) : g_pal.panelAlt);
            Frame(dc, r, g_pal.border);
        }
        else if (hot)
        {
            FillRoundRect(dc, r, S(4), g_pal.dark ? RGB(0x3A, 0x3A, 0x40) : RGB(0xD8, 0xE8, 0xFC),
                          g_pal.border, false);
        }

        if (isSegment)
        {
            // plain UI-font glyph, optically centred (icon-font side bearings biased it left)
            TextAt(dc, label, r, hot ? g_pal.accent : g_pal.text, g_fontUI,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            g_toolHits.push_back({ t.id, r, label });
            x = r.right;
            continue;
        }

        int cx = r.left + padX;
        if (hasGlyph)
        {
            HGDIOBJ oldIcon = SelectObject(dc, g_fontIcon);
            SetBkMode(dc, TRANSPARENT);
            // two-tone by theme: accent when active, muted otherwise
            SetTextColor(dc, hot ? g_pal.accent : (g_pal.dark ? g_pal.text : g_pal.text));
            // segments: centre by the measured glyph box inside the 1px frame, otherwise the
            // optical centre drifts left by half a pixel after GDI rounding
            RECT ir;
            if (isSegment)
            {
                SIZE gs{ 0, 0 };
                GetTextExtentPoint32W(dc, &t.glyph, 1, &gs);
                int innerL = r.left + 1, innerR = r.right - 1;
                int gx = innerL + ((innerR - innerL) - gs.cx) / 2;
                if (gx < innerL) gx = innerL;
                ir = RECT{ gx, r.top + 1, gx + gs.cx, r.bottom - 1 };
            }
            else
            {
                ir = RECT{ cx, r.top, cx + iconW, r.bottom };
            }
            wchar_t g2[2] = { t.glyph, 0 };
            DrawTextW(dc, g2, 1, &ir,
                      (isSegment ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldIcon);
            cx += iconW + gap;
        }
        if (!t.iconOnly)
        {
            RECT tr{ cx, r.top, r.right - padX, r.bottom };
            TextAt(dc, label, tr, g_pal.text, g_fontUI, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        else if (!hasGlyph)
        {
            TextAt(dc, label, r, g_pal.text, g_fontUI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        g_toolHits.push_back({ t.id, r, label });
        x = isSegment ? r.right : r.right + S(2);
    }
    Frame(dc, RECT{ 0, g_rcToolbar.bottom - 1, g_rcToolbar.right, g_rcToolbar.bottom }, g_pal.border);
}

static std::wstring TabTitle(int i)
{
    if (i == 0) return ss::i18n::T(L"tab.start");
    auto& v = g_views[i - 1];
    std::wstring t = v->title.empty() ? std::wstring(ss::i18n::T(L"tab.new_view")) : v->title;
    if (t.size() > 34) t = t.substr(0, 33) + L"…";
    return L"  " + t + L"  ";
}

static void DrawTabs(HDC dc)
{
    Fill(dc, g_rcTabs, g_pal.panelAlt);
    g_tabHits.clear();
    int x = 0;
    int count = 1 + (int)g_views.size();
    for (int i = 0; i < count; ++i)
    {
        bool closable = (i != 0);
        std::wstring title = TabTitle(i);
        SIZE sz{};
        HGDIOBJ old = SelectObject(dc, g_fontUI);
        GetTextExtentPoint32W(dc, title.c_str(), (int)title.size(), &sz);
        SelectObject(dc, old);
        // Start tab (i == 0) does not have a close cross, so do not reserve space for it.
        int w = closable ? (sz.cx + S(34)) : (sz.cx + S(28));
        RECT r{ x, g_rcTabs.top, x + w, g_rcTabs.bottom };
        bool active = (i == 0 && g_showStart) || (i > 0 && !g_showStart && g_activeView == i - 1);
        Fill(dc, r, active ? g_pal.panel : g_pal.panelAlt);
        if (closable)
        {
            TextAt(dc, title, RECT{ r.left + S(8), r.top, r.right - S(24), r.bottom },
                   active ? g_pal.text : g_pal.textDim, g_fontUI);
            RECT cr{ r.right - S(20), r.top + S(9), r.right - S(8), r.top + S(21) };
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), g_pal.textDim);
            if (pen)
            {
                HGDIOBJ oldp = SelectObject(dc, pen);
                MoveToEx(dc, cr.left, cr.top, nullptr);
                LineTo(dc, cr.right, cr.bottom);
                MoveToEx(dc, cr.right, cr.top, nullptr);
                LineTo(dc, cr.left, cr.bottom);
                SelectObject(dc, oldp);
                DeleteObject(pen);
            }
        }
        else
        {
            TextAt(dc, title, r, active ? g_pal.text : g_pal.textDim, g_fontUI,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (active)
        {
            RECT line{ r.left, r.bottom - S(2), r.right, r.bottom };
            Fill(dc, line, g_pal.accent);
        }
        Frame(dc, RECT{ r.right - 1, r.top + S(4), r.right, r.bottom - S(4) }, g_pal.border);
        if (closable)
            g_tabHits.push_back({ i, true, RECT{ r.right - S(24), r.top, r.right, r.bottom } });
        g_tabHits.push_back({ i, false, r });
        x = r.right;
    }
    Frame(dc, RECT{ 0, g_rcTabs.bottom - 1, g_rcTabs.right, g_rcTabs.bottom }, g_pal.border);
}

static void DrawStatus(HDC dc)
{
    Fill(dc, g_rcStatus, g_pal.panelAlt);
    Frame(dc, RECT{ 0, g_rcStatus.top, g_rcStatus.right, g_rcStatus.top + 1 }, g_pal.border);
    std::wstring left;
    if (g_toolHoverId != 0)
    {
        for (const auto& t : kTools)
        {
            if (t.id == g_toolHoverId && t.tipKey)
            {
                left = ss::i18n::T(t.tipKey);
                break;
            }
        }
    }
    ViewState* v = CurrentView();
    if (left.empty())
    {
        if (g_showStart) left = ss::i18n::T(L"status.ready");
        else if (v) left = v->status;
    }
    TextAt(dc, left, RECT{ S(10), g_rcStatus.top, g_rcStatus.right - S(320), g_rcStatus.bottom },
           g_pal.textDim, g_fontSmall);
    std::wstring right;
    if (v && v->tree.Valid(v->tree.root))
        right = std::wstring(ss::i18n::T(L"status.disk_usage")) + FmtSize(v->tree.nodes[v->tree.root].disk);
    TextAt(dc, right, RECT{ g_rcStatus.right - S(320), g_rcStatus.top, g_rcStatus.right - S(12), g_rcStatus.bottom },
           g_pal.textDim, g_fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void PaintMain(HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    if (!mem || !bmp)
    {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    Fill(mem, rc, g_pal.bg);
    DrawToolbar(mem);
    DrawTabs(mem);
    DrawStatus(mem);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------- custom dropdown menu panel
// Replaces Win32 MF_OWNERDRAW / TrackPopupMenu for app menus: eliminates raw pointer
// itemData hazards, system white border, GDI brush leaks, and menu-loop hangs.
struct PopupMenuItem
{
    int cmd = 0;
    std::wstring label;
    std::wstring accel;
    bool separator = false;
    bool checked = false;
    bool enabled = true;
};

struct PopupMenuState
{
    const std::vector<PopupMenuItem>* items = nullptr;
    std::vector<RECT> itemRects;
    int hoverIdx = -1;
    int width = 0;
    int height = 0;
    bool trackingMouse = false;
};

static int PopupMenuItemAt(const PopupMenuState& st, int y)
{
    if (!st.items) return -1;
    for (size_t i = 0; i < st.itemRects.size() && i < st.items->size(); ++i)
    {
        const RECT& r = st.itemRects[i];
        if (y >= r.top && y < r.bottom)
        {
            if ((*st.items)[i].separator || !(*st.items)[i].enabled) return -1;
            return (int)i;
        }
    }
    return -1;
}

static int PopupMenuNextItem(const PopupMenuState& st, int cur, int dir)
{
    if (!st.items || st.items->empty()) return -1;
    int n = (int)st.items->size();
    int start = (cur < 0 || cur >= n) ? (dir > 0 ? -1 : n) : cur;
    for (int step = 0; step < n; ++step)
    {
        int idx = (start + dir * (step + 1)) % n;
        if (idx < 0) idx += n;
        if (!(*st.items)[idx].separator && (*st.items)[idx].enabled) return idx;
    }
    return -1;
}

static LRESULT CALLBACK PopupMenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = (CREATESTRUCTW*)lp;
        if (!cs || !cs->lpCreateParams) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    auto* st = (PopupMenuState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (rc.right > 0 && rc.bottom > 0)
            {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                if (mem && bmp)
                {
                    HGDIOBJ old = SelectObject(mem, bmp);
                    Fill(mem, rc, g_pal.panel);
                    Frame(mem, rc, g_pal.border);
                    if (st->items)
                    {
                        for (size_t i = 0; i < st->items->size() && i < st->itemRects.size(); ++i)
                        {
                            const PopupMenuItem& it = (*st->items)[i];
                            const RECT& r = st->itemRects[i];
                            if (it.separator)
                            {
                                int y = (r.top + r.bottom) / 2;
                                Fill(mem, RECT{ r.left + S(10), y, r.right - S(10), y + 1 }, g_pal.border);
                                continue;
                            }
                            bool hot = ((int)i == st->hoverIdx && it.enabled);
                            if (hot)
                            {
                                RECT hi{ r.left + S(4), r.top + S(1), r.right - S(4), r.bottom - S(1) };
                                Fill(mem, hi, g_pal.accent);
                            }
                            COLORREF fg = !it.enabled ? g_pal.textDim : (hot ? RGB(255, 255, 255) : g_pal.text);
                            if (it.checked)
                            {
                                RECT ck{ r.left + S(8), r.top, r.left + S(24), r.bottom };
                                TextAt(mem, L"✓", ck, hot ? RGB(255, 255, 255) : g_pal.accent, g_fontUIBold,
                                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }
                            RECT tr{ r.left + S(28), r.top, r.right - S(14), r.bottom };
                            TextAt(mem, it.label, tr, fg, g_fontUI, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                            if (!it.accel.empty())
                            {
                                COLORREF afg = hot ? RGB(255, 255, 255) : g_pal.textDim;
                                RECT ar{ r.left + S(28), r.top, r.right - S(14), r.bottom };
                                TextAt(mem, it.accel, ar, afg, g_fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                            }
                        }
                    }
                    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                }
                if (bmp) DeleteObject(bmp);
                if (mem) DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!st->trackingMouse)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                st->trackingMouse = true;
            }
            int idx = PopupMenuItemAt(*st, GET_Y_LPARAM(lp));
            if (idx != st->hoverIdx)
            {
                st->hoverIdx = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            st->trackingMouse = false;
            if (st->hoverIdx != -1)
            {
                st->hoverIdx = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int MenuBarItemIndexAt(HWND hwnd, int x, int y);

static std::vector<PopupMenuItem> GetTopMenuItems(int index)
{
    using ss::i18n::T;
    std::vector<PopupMenuItem> items;
    auto add = [&](int cmd, const wchar_t* label, const wchar_t* accel = L"", bool checked = false, bool enabled = true)
    {
        items.push_back({ cmd, label, accel, false, checked, enabled });
    };
    auto sep = [&]() { items.push_back({ 0, L"", L"", true, false, false }); };

    ViewState* v = CurrentView();
    bool hasView = (v != nullptr && !v->scanning && v->tree.Valid(v->tree.root));
    bool isScanning = (v != nullptr && v->scanning);

    switch (index)
    {
        case 0: // File
            add(CMD_NEW, T(L"menu.file.new"), L"Ctrl+N");
            sep();
            add(CMD_EXPORT, T(L"menu.file.export"), L"", false, hasView);
            add(CMD_SNAPSHOT, T(L"menu.file.snapshot"), L"", false, hasView);
            sep();
            add(CMD_EXIT, T(L"menu.file.exit"));
            break;
        case 1: // Edit
            add(CMD_CONFIG, T(L"menu.edit.config"));
            break;
        case 2: // View
            add(CMD_REFRESH, T(L"menu.view.refresh"), L"F5", false, v != nullptr && !isScanning);
            add(CMD_FILTERBAR, T(L"menu.view.filter"), L"Ctrl+L", v && v->filterVisible, v != nullptr);
            add(CMD_STOP, T(L"menu.view.stop"), L"", false, isScanning);
            sep();
            add(CMD_BACK, T(L"menu.view.back"), L"Backspace", false, v && !v->back.empty());
            add(CMD_FWD, T(L"menu.view.forward"), L"Shift+Backspace", false, v && !v->forward.empty());
            add(CMD_UP, T(L"menu.view.up"), L"Ctrl+Up", false, hasView);
            add(CMD_HOME, T(L"menu.view.home"), L"Ctrl+Home", false, hasView);
            sep();
            add(CMD_DETAIL_PLUS, T(L"menu.view.detail_plus"), L"Ctrl++");
            add(CMD_DETAIL_MINUS, T(L"menu.view.detail_minus"), L"Ctrl+-");
            add(CMD_STYLE, T(L"menu.view.style"), L"Ctrl+T", g_cfg.initialStyle == L"File Classes");
            add(CMD_FREE, T(L"menu.view.free"), L"Ctrl+F", g_cfg.showFreeSpace);
            add(CMD_UNKNOWN, T(L"menu.view.unknown"), L"Ctrl+U", g_cfg.showUnknownSpace);
            sep();
            add(CMD_DARK, T(L"menu.view.dark"), L"", g_cfg.darkMode);
            break;
        case 3: // Window
            add(CMD_LOG, T(L"menu.window.log"));
            break;
        case 4: // Help
            add(CMD_HELP_FILTER, T(L"menu.help.filter"), L"F1");
            add(CMD_HELP_TAG, T(L"menu.help.tag"));
            add(CMD_HELP_CLI, T(L"menu.help.cli"));
            add(CMD_HELP_MCP, T(L"menu.help.mcp"));
            sep();
            add(CMD_ABOUT, T(L"menu.help.about"));
            break;
    }
    return items;
}

// Displays a custom-drawn dropdown menu panel with full keyboard + mouse support.
// Returns selected command ID (> 0), or 0 if dismissed. If switchMenuIdx != nullptr and
// the user navigates Left/Right or hovers another menu bar title, *switchMenuIdx is set.
static int TrackCustomPopupMenu(HWND owner, const std::vector<PopupMenuItem>& items,
                                POINT screenPt, HWND menuBarHwnd, int currentMenuIdx,
                                int* switchMenuIdx)
{
    if (switchMenuIdx) *switchMenuIdx = -1;
    if (items.empty()) return 0;

    PopupMenuState st{};
    st.items = &items;

    int maxLabelW = S(100);
    int maxAccelW = 0;
    HDC dc = GetDC(owner);
    if (dc)
    {
        HGDIOBJ old = SelectObject(dc, g_fontUI);
        for (const auto& it : items)
        {
            if (it.separator) continue;
            SIZE sz{};
            if (!it.label.empty())
            {
                GetTextExtentPoint32W(dc, it.label.c_str(), (int)it.label.size(), &sz);
                if (sz.cx > maxLabelW) maxLabelW = sz.cx;
            }
        }
        SelectObject(dc, g_fontSmall);
        for (const auto& it : items)
        {
            if (it.separator || it.accel.empty()) continue;
            SIZE sz{};
            GetTextExtentPoint32W(dc, it.accel.c_str(), (int)it.accel.size(), &sz);
            if (sz.cx > maxAccelW) maxAccelW = sz.cx;
        }
        SelectObject(dc, old);
        ReleaseDC(owner, dc);
    }

    int itemH = S(26);
    int sepH = S(9);
    int padY = S(4);
    st.width = S(28) + maxLabelW + (maxAccelW > 0 ? S(24) + maxAccelW : 0) + S(18);
    int y = padY;
    st.itemRects.reserve(items.size());
    for (const auto& it : items)
    {
        int h = it.separator ? sepH : itemH;
        st.itemRects.push_back(RECT{ 1, y, st.width - 1, y + h });
        y += h;
    }
    st.height = y + padY;

    // Clamp to nearest monitor work area
    RECT work{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    int px = screenPt.x;
    int py = screenPt.y;
    if (px + st.width > work.right) px = std::max((int)work.left, (int)work.right - st.width);
    if (py + st.height > work.bottom) py = std::max((int)work.top, (int)work.bottom - st.height);
    if (px < work.left) px = work.left;
    if (py < work.top) py = work.top;

    HWND popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 kPopupMenuClass, L"", WS_POPUP | WS_CLIPSIBLINGS,
                                 px, py, st.width, st.height, owner, nullptr,
                                 GetModuleHandleW(nullptr), &st);
    if (!popup) return 0;

    ShowWindow(popup, SW_SHOWNOACTIVATE);
    UpdateWindow(popup);

    int chosenCmd = 0;
    bool done = false;
    bool gotQuit = false;
    WPARAM quitCode = 0;

    MSG msg{};
    while (!done)
    {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { gotQuit = true; quitCode = msg.wParam; break; }
        if (r < 0) break;

        if (!IsWindow(popup) || (owner && !IsWindow(owner))) break;

        switch (msg.message)
        {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            {
                if (msg.wParam == VK_ESCAPE)
                {
                    done = true;
                    continue;
                }
                if (msg.wParam == VK_DOWN)
                {
                    st.hoverIdx = PopupMenuNextItem(st, st.hoverIdx, +1);
                    InvalidateRect(popup, nullptr, FALSE);
                    continue;
                }
                if (msg.wParam == VK_UP)
                {
                    st.hoverIdx = PopupMenuNextItem(st, st.hoverIdx, -1);
                    InvalidateRect(popup, nullptr, FALSE);
                    continue;
                }
                if (switchMenuIdx && currentMenuIdx >= 0 && (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT))
                {
                    int delta = (msg.wParam == VK_RIGHT) ? 1 : -1;
                    *switchMenuIdx = (currentMenuIdx + delta + 5) % 5;
                    done = true;
                    continue;
                }
                if (msg.wParam == VK_RETURN || msg.wParam == VK_SPACE)
                {
                    if (st.hoverIdx >= 0 && st.hoverIdx < (int)items.size() &&
                        !items[st.hoverIdx].separator && items[st.hoverIdx].enabled)
                    {
                        chosenCmd = items[st.hoverIdx].cmd;
                    }
                    done = true;
                    continue;
                }
                continue;
            }
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_NCLBUTTONDOWN:
            case WM_NCRBUTTONDOWN:
            {
                POINT sp{};
                GetCursorPos(&sp);
                RECT pr{};
                GetWindowRect(popup, &pr);
                if (PtInRect(&pr, sp))
                {
                    POINT cl = sp;
                    ScreenToClient(popup, &cl);
                    int idx = PopupMenuItemAt(st, cl.y);
                    if (idx >= 0)
                    {
                        chosenCmd = items[idx].cmd;
                        done = true;
                    }
                    continue;
                }
                if (menuBarHwnd && IsWindow(menuBarHwnd))
                {
                    RECT mbr{};
                    GetWindowRect(menuBarHwnd, &mbr);
                    if (PtInRect(&mbr, sp))
                    {
                        POINT mbc = sp;
                        ScreenToClient(menuBarHwnd, &mbc);
                        int idx = MenuBarItemIndexAt(menuBarHwnd, mbc.x, mbc.y);
                        if (idx >= 0 && idx != currentMenuIdx && switchMenuIdx)
                            *switchMenuIdx = idx;
                        done = true;
                        continue;   // eat click so toggle-off doesn't reopen
                    }
                }
                done = true;
                if (msg.message == WM_NCLBUTTONDOWN || msg.message == WM_NCRBUTTONDOWN)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                continue;
            }
            case WM_LBUTTONUP:
            {
                POINT sp{};
                GetCursorPos(&sp);
                RECT pr{};
                GetWindowRect(popup, &pr);
                if (PtInRect(&pr, sp))
                {
                    POINT cl = sp;
                    ScreenToClient(popup, &cl);
                    int idx = PopupMenuItemAt(st, cl.y);
                    if (idx >= 0)
                    {
                        chosenCmd = items[idx].cmd;
                        done = true;
                        continue;
                    }
                }
                break;
            }
            case WM_MOUSEMOVE:
            {
                if (menuBarHwnd && switchMenuIdx && IsWindow(menuBarHwnd))
                {
                    POINT sp{};
                    GetCursorPos(&sp);
                    RECT mbr{};
                    GetWindowRect(menuBarHwnd, &mbr);
                    if (PtInRect(&mbr, sp))
                    {
                        POINT mbc = sp;
                        ScreenToClient(menuBarHwnd, &mbc);
                        int idx = MenuBarItemIndexAt(menuBarHwnd, mbc.x, mbc.y);
                        if (idx >= 0 && idx != currentMenuIdx)
                        {
                            *switchMenuIdx = idx;
                            done = true;
                            continue;
                        }
                    }
                }
                break;
            }
            case WM_ACTIVATEAPP:
                if (msg.wParam == FALSE) done = true;
                break;
            case WM_CANCELMODE:
            case WM_CLOSE:
                done = true;
                break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(popup)) DestroyWindow(popup);
    if (gotQuit) PostQuitMessage((int)quitCode);
    return chosenCmd;
}

// ---------------------------------------------------------------- log window
static HWND g_logEdit = nullptr;
static std::vector<std::wstring> g_logLines;

static void LogLine(const std::wstring& s)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    _snwprintf_s(buf, 32, _TRUNCATE, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    std::wstring line = buf + s;
    g_logLines.push_back(line);
    if (g_logLines.size() > 4000) g_logLines.erase(g_logLines.begin(), g_logLines.begin() + 1000);
    if (g_logEdit)
    {
        std::wstring all;
        for (auto& l : g_logLines) { all += l; all += L"\r\n"; }
        SetWindowTextW(g_logEdit, all.c_str());
        SendMessageW(g_logEdit, EM_SETSEL, (WPARAM)-2, (LPARAM)-1);
        SendMessageW(g_logEdit, EM_SCROLLCARET, 0, 0);
    }
    ViewState* v = CurrentView();
    if (v) { v->status = s; if (v->hwnd) InvalidateRect(v->hwnd, nullptr, FALSE); }
    if (g_main) InvalidateRect(g_main, &g_rcStatus, FALSE);
}

static LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            g_logEdit = CreateWindowExW(0, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                            ES_READONLY | ES_AUTOVSCROLL,
                                        0, 0, 100, 100, hwnd, (HMENU)1, GetModuleHandleW(nullptr), nullptr);
            if (g_logEdit)
            {
                SetWindowTheme(g_logEdit, g_pal.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                SendMessageW(g_logEdit, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
                SendMessageW(g_logEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(8), S(8)));
            }
            LogLine(ss::i18n::T(L"log.opened"));
            return 0;
        }
        case WM_SIZE:
        {
            if (g_logEdit)
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int pad = S(8);
                int w = std::max(0, (int)rc.right - (pad + 1) * 2);
                int h = std::max(0, (int)rc.bottom - (pad + 1) * 2);
                SetWindowPos(g_logEdit, nullptr, pad + 1, pad + 1, w, h, SWP_NOZORDER);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = (HDC)wp;
            COLORREF bg = g_pal.panel;
            SetTextColor(dc, g_pal.text);
            SetBkColor(dc, bg);
            return (LRESULT)GetViewerBrush(bg);
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Fill(dc, rc, g_pal.bg);
            int pad = S(8);
            if (rc.right > pad * 2 + 2 && rc.bottom > pad * 2 + 2)
                Frame(dc, RECT{ pad, pad, rc.right - pad, rc.bottom - pad }, g_pal.border);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        case WM_DESTROY:
            g_log = nullptr;
            g_logEdit = nullptr;
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowLogWindow(HWND owner)
{
    if (g_log && IsWindow(g_log))
    {
        ApplyThemeToEditWindow(g_log, g_logEdit);
        ShowWindow(g_log, SW_SHOW);
        SetForegroundWindow(g_log);
        return;
    }
    g_log = CreateWindowExW(WS_EX_TOOLWINDOW, kLogClass, ss::i18n::T(L"log.title"),
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(900), S(420),
                            owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_log)
    {
        ApplyDwmChrome(g_log);
        ShowWindow(g_log, SW_SHOW);
        UpdateWindow(g_log);
    }
}




// ---------------------------------------------------------------- shell context menu
// The original shows the *system* shell menu for the right-clicked element; do the same
// instead of a hand-rolled popup.
static void ShowShellContextMenu(HWND owner, const std::wstring& path, POINT screenPt)
{
    if (!owner || path.empty()) return;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder, (void**)&parent, &child)) && parent && child)
    {
        IContextMenu* cm = nullptr;
        if (SUCCEEDED(parent->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, (void**)&cm)) && cm)
        {
            IContextMenu2* cm2 = nullptr;
            IContextMenu3* cm3 = nullptr;
            cm->QueryInterface(IID_IContextMenu3, (void**)&cm3);
            if (!cm3) cm->QueryInterface(IID_IContextMenu2, (void**)&cm2);

            HMENU menu = CreatePopupMenu();
            if (menu && SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL)))
            {
                g_shellCm2 = cm2;
                g_shellCm3 = cm3;
                SetForegroundWindow(owner);
                int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                              screenPt.x, screenPt.y, 0, owner, nullptr);
                PostMessageW(owner, WM_NULL, 0, 0);
                g_shellCm2 = nullptr;
                g_shellCm3 = nullptr;

                if (cmd >= 1 && cmd <= 0x7FFF)
                {
                    CMINVOKECOMMANDINFOEX info{};
                    info.cbSize = sizeof(info);
                    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK | CMIC_MASK_PTINVOKE;
                    info.hwnd = owner;
                    info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                    info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                    info.ptInvoke = screenPt;
                    info.nShow = SW_SHOWNORMAL;
                    cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
                }
            }
            if (menu) DestroyMenu(menu);
            if (cm3) cm3->Release();
            if (cm2) cm2->Release();
            cm->Release();
        }
        parent->Release();
    }
    CoTaskMemFree(pidl);
}


// ---------------------------------------------------------------- elevation / shell integration

static const wchar_t* ExtractCmdLineArgs(const wchar_t* cmd)
{
    if (!cmd) return L"";
    while (*cmd == L' ' || *cmd == L'\t') ++cmd;
    if (*cmd == L'"')
    {
        ++cmd;
        while (*cmd && *cmd != L'"') ++cmd;
        if (*cmd == L'"') ++cmd;
    }
    else
    {
        while (*cmd && *cmd != L' ' && *cmd != L'\t') ++cmd;
    }
    while (*cmd == L' ' || *cmd == L'\t') ++cmd;
    return cmd;
}

static bool RelaunchElevated(HWND owner, DWORD* outErr)
{
    if (outErr) *outErr = ERROR_SUCCESS;
    std::wstring exe(4096, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (n == 0 || n >= exe.size())
    {
        if (outErr) *outErr = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
        return false;
    }
    exe.resize(n);

    const wchar_t* args = ExtractCmdLineArgs(GetCommandLineW());
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = (args && args[0] != L'\0') ? args : nullptr;
    sei.nShow = SW_SHOWNORMAL;
    SetLastError(0);
    if (!ShellExecuteExW(&sei))
    {
        if (outErr) *outErr = GetLastError();
        return false;
    }
    return true;
}

namespace ss::ui {

static constexpr const wchar_t* kShellRegBases[] = {
    L"Software\\Classes\\Directory\\shell\\NeoSniffer",
    L"Software\\Classes\\Directory\\Background\\shell\\NeoSniffer",
    L"Software\\Classes\\Drive\\shell\\NeoSniffer",
};

static ShellIntegrationStatus g_shellStatus{};

static bool ReadRegStringValue(HKEY root, const wchar_t* subKey, const wchar_t* valName, std::wstring& out)
{
    out.clear();
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = 0;
    LSTATUS st = RegQueryValueExW(k, valName, nullptr, &type, nullptr, &cb);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb < sizeof(wchar_t))
    {
        RegCloseKey(k);
        return false;
    }
    std::wstring buf(cb / sizeof(wchar_t) + 1, L'\0');
    st = RegQueryValueExW(k, valName, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &cb);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS) return false;
    out = buf.c_str();
    return !out.empty();
}

const ShellIntegrationStatus& GetShellIntegrationStatus()
{
    return g_shellStatus;
}

bool IsShellIntegrationRegistered()
{
    // P1: verify all 3 registry keys exist and point to the current executable path.
    std::wstring exe(4096, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (n == 0 || n >= exe.size()) return false;
    exe.resize(n);
    std::wstring expectedPrefix = L"\"" + exe + L"\"";

    for (const wchar_t* base : kShellRegBases)
    {
        std::wstring cmdPath = std::wstring(base) + L"\\command";
        std::wstring val;
        if (!ReadRegStringValue(HKEY_CURRENT_USER, cmdPath.c_str(), L"", val))
            return false;
        if (val.size() < expectedPrefix.size() ||
            _wcsnicmp(val.c_str(), expectedPrefix.c_str(), expectedPrefix.size()) != 0)
            return false;
    }
    return true;
}

ShellIntegrationStatus ApplyShellIntegration(bool enable)
{
    ShellIntegrationStatus res{};
    res.applied = true;
    res.enabled = enable;
    res.ok = true;
    res.errorCode = ERROR_SUCCESS;

    auto recordErr = [&](LSTATUS st, const wchar_t* op, const std::wstring& keyPath)
    {
        if (res.ok)
        {
            res.ok = false;
            res.errorCode = st;
            res.detail = std::wstring(op) + L" " + keyPath;
        }
    };

    if (!enable)
    {
        bool anyDeleted = false;
        for (const wchar_t* base : kShellRegBases)
        {
            LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, base);
            if (st == ERROR_SUCCESS)
                anyDeleted = true;
            else if (st != ERROR_FILE_NOT_FOUND && st != ERROR_PATH_NOT_FOUND)
                recordErr(st, L"RegDeleteTreeW", base);
        }
        // P1: only notify Explorer when keys were actually removed (avoids desktop flicker on startup).
        if (anyDeleted)
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        g_shellStatus = res;
        return res;
    }

    std::wstring exe(4096, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (n == 0 || n >= exe.size())
    {
        DWORD err = GetLastError();
        recordErr(err ? (LSTATUS)err : ERROR_GEN_FAILURE, L"GetModuleFileNameW", L"");
        g_shellStatus = res;
        return res;
    }
    exe.resize(n);

    const wchar_t* label = ss::i18n::T(L"shell.menu_label");
    std::wstring icon = L"\"" + exe + L"\",0";
    std::wstring cmd = L"\"" + exe + L"\" scan \"%V\"";

    // P1: skip registry writes & SHChangeNotify if all keys already match desired values.
    bool alreadyExact = true;
    for (const wchar_t* base : kShellRegBases)
    {
        std::wstring curLabel, curIcon, curCmd;
        std::wstring cmdPath = std::wstring(base) + L"\\command";
        if (!ReadRegStringValue(HKEY_CURRENT_USER, base, L"", curLabel) || curLabel != label ||
            !ReadRegStringValue(HKEY_CURRENT_USER, base, L"Icon", curIcon) || curIcon != icon ||
            !ReadRegStringValue(HKEY_CURRENT_USER, cmdPath.c_str(), L"", curCmd) || curCmd != cmd)
        {
            alreadyExact = false;
            break;
        }
    }
    if (alreadyExact)
    {
        g_shellStatus = res;
        return res;
    }

    std::vector<const wchar_t*> writtenBases;
    for (const wchar_t* base : kShellRegBases)
    {
        HKEY key = nullptr;
        LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, base, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                     KEY_WRITE, nullptr, &key, nullptr);
        if (st != ERROR_SUCCESS)
        {
            recordErr(st, L"RegCreateKeyExW", base);
            break;
        }
        writtenBases.push_back(base);

        st = RegSetValueExW(key, L"", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(label),
                            static_cast<DWORD>((wcslen(label) + 1) * sizeof(wchar_t)));
        if (st != ERROR_SUCCESS)
            recordErr(st, L"RegSetValueExW(Default)", base);

        if (res.ok)
        {
            st = RegSetValueExW(key, L"Icon", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(icon.c_str()),
                                static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));
            if (st != ERROR_SUCCESS)
                recordErr(st, L"RegSetValueExW(Icon)", base);
        }

        if (res.ok)
        {
            HKEY cmdKey = nullptr;
            std::wstring cmdKeyPath = std::wstring(base) + L"\\command";
            st = RegCreateKeyExW(key, L"command", 0, nullptr, REG_OPTION_NON_VOLATILE,
                                 KEY_WRITE, nullptr, &cmdKey, nullptr);
            if (st != ERROR_SUCCESS)
            {
                recordErr(st, L"RegCreateKeyExW", cmdKeyPath);
            }
            else
            {
                st = RegSetValueExW(cmdKey, L"", 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(cmd.c_str()),
                                    static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
                if (st != ERROR_SUCCESS)
                    recordErr(st, L"RegSetValueExW(Default)", cmdKeyPath);
                RegCloseKey(cmdKey);
            }
        }

        RegCloseKey(key);
        if (!res.ok) break;
    }

    if (!res.ok)
    {
        // P1: roll back partially written registry keys on failure so no half-registered state remains.
        for (const wchar_t* base : writtenBases)
            RegDeleteTreeW(HKEY_CURRENT_USER, base);
    }
    else
    {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    g_shellStatus = res;
    return res;
}

} // namespace ss::ui

static void SyncShellIntegration(bool enable, bool logSuccess)
{
    ss::ui::ShellIntegrationStatus st = ss::ui::ApplyShellIntegration(enable);
    if (!st.ok)
    {
        std::wstring msg = std::wstring(ss::i18n::T(L"log.shell_reg_failed")) +
                           std::to_wstring(st.errorCode) + L")";
        if (!st.detail.empty()) msg += L" [" + st.detail + L"]";
        LogLine(msg);
        // P1: notify user explicitly when interactive registration/removal fails.
        if (logSuccess && g_main && IsWindow(g_main))
        {
            MessageBoxW(g_main, msg.c_str(), ss::i18n::T(L"app.title"), MB_OK | MB_ICONWARNING);
        }
    }
    else if (logSuccess)
    {
        LogLine(ss::i18n::T(enable ? L"log.shell_reg_ok" : L"log.shell_unreg_ok"));
    }
}

// ---------------------------------------------------------------- menu bar (custom drawn)
static const wchar_t* kMenuTitleKeys[5] = { L"menu.file", L"menu.edit", L"menu.view", L"menu.window", L"menu.help" };

static int MenuBarItemIndexAt(HWND hwnd, int x, int y)
{
    HDC dc = GetDC(hwnd);
    if (!dc) return -1;
    HGDIOBJ old = SelectObject(dc, g_fontUI);
    int cx = S(12);
    for (int i = 0; i < 5; ++i)
    {
        const wchar_t* title = ss::i18n::T(kMenuTitleKeys[i]);
        SIZE sz{};
        GetTextExtentPoint32W(dc, title, (int)wcslen(title), &sz);
        int w = sz.cx + S(24);
        if (x >= cx - S(6) && x < cx - S(6) + w && y >= 0 && y < S(26))
        {
            SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
            return i;
        }
        cx += w;
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return -1;
}

static void MenuBarPaint(HWND hwnd, HDC target, const RECT& rc)
{
    (void)hwnd;
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    if (!mem || !bmp)
    {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    Fill(mem, rc, g_pal.panel);
    int hover = g_menuHover;

    int cx = S(12);
    for (int i = 0; i < 5; ++i)
    {
        const wchar_t* title = ss::i18n::T(kMenuTitleKeys[i]);
        SIZE sz{};
        HGDIOBJ oldf = SelectObject(mem, g_fontUI);
        GetTextExtentPoint32W(mem, title, (int)wcslen(title), &sz);
        SelectObject(mem, oldf);
        int w = sz.cx + S(24);
        RECT r{ cx - S(6), S(2), cx - S(6) + w, S(24) };
        if (i == g_menuOpen)
        {
            Fill(mem, r, g_pal.dark ? RGB(0x33, 0x33, 0x3B) : RGB(0xE2, 0xE8, 0xF2));
        }
        else if (i == hover)
        {
            Fill(mem, r, g_pal.panelAlt);
        }
        TextAt(mem, title, r, g_pal.text, g_fontUI,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        cx += w;
    }
    Frame(mem, RECT{ 0, rc.bottom - 1, rc.right, rc.bottom }, g_pal.border);
    BitBlt(target, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void MenuBarOpen(HWND hwnd, int index)
{
    static bool s_inMenu = false;
    if (s_inMenu || index < 0 || index >= 5) return;
    s_inMenu = true;

    int cmd = 0;
    int cur = index;
    while (cur >= 0 && cur < 5 && IsWindow(hwnd))
    {
        int cx = S(12);
        HDC dc = GetDC(hwnd);
        if (dc)
        {
            HGDIOBJ old = SelectObject(dc, g_fontUI);
            for (int i = 0; i < cur; ++i)
            {
                const wchar_t* title = ss::i18n::T(kMenuTitleKeys[i]);
                SIZE sz{};
                GetTextExtentPoint32W(dc, title, (int)wcslen(title), &sz);
                cx += sz.cx + S(24);
            }
            SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
        }

        POINT pt{ cx - S(6), S(26) };
        ClientToScreen(hwnd, &pt);
        g_menuOpen = cur;
        g_menuHover = cur;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);

        std::vector<PopupMenuItem> items = GetTopMenuItems(cur);
        int nextIdx = -1;
        cmd = TrackCustomPopupMenu(g_main, items, pt, hwnd, cur, &nextIdx);
        if (cmd > 0 || nextIdx < 0) break;
        cur = nextIdx;
    }

    g_menuOpen = -1;
    if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
    s_inMenu = false;

    if (cmd > 0) DoCommand(cmd);
}

static LRESULT CALLBACK MenuBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            MenuBarPaint(hwnd, dc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_menuTracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_menuTracking = true;
            }
            int idx = MenuBarItemIndexAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            // repaint only on a real state change -- invalidating on every move made the
            // message loop spin (move -> paint -> move ...) and froze the window
            if (idx != g_menuHover)
            {
                g_menuHover = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
        {
            g_menuTracking = false;
            if (g_menuHover != -1)
            {
                g_menuHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        {
            int idx = MenuBarItemIndexAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (idx >= 0) { MenuBarOpen(hwnd, idx); return 0; }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- commands
static void RefreshMain()
{
    if (!g_main) return;
    InvalidateRect(g_main, nullptr, FALSE);
    ViewState* v = CurrentView();
    if (v && v->hwnd) { InvalidateRect(v->hwnd, nullptr, FALSE); }
}

static ViewState* NewViewObject()
{
    auto v = std::make_unique<ViewState>();
    ViewState* raw = v.get();
    g_views.push_back(std::move(v));
    return raw;
}

static void ShowView(int index)
{
    g_showStart = index < 0;
    if (g_start) ShowWindow(g_start, g_showStart ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < (int)g_views.size(); ++i)
        if (g_views[i]->hwnd) ShowWindow(g_views[i]->hwnd, (!g_showStart && i == index) ? SW_SHOW : SW_HIDE);
    g_activeView = index;
    LayoutMain(g_main);
    if (index >= 0 && index < (int)g_views.size() && g_views[index]->hwnd)
        SetFocus(g_views[index]->hwnd);
    RefreshMain();
}

static void OpenScanView(const std::wstring& path, const std::wstring& filter)
{
    ViewState* v = NewViewObject();
    v->path = path;
    v->title = std::wstring(ss::i18n::T(L"view.title.scan")) + path;
    v->hwnd = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 100, 100, g_main, nullptr, GetModuleHandleW(nullptr), v);
    if (v->editFilter && !filter.empty())
    {
        SetWindowTextW(v->editFilter, filter.c_str());
        v->filterText = filter;
    }
    LayoutMain(g_main);
    ShowView((int)g_views.size() - 1);
    StartScan(v, path);
    LogLine(std::wstring(ss::i18n::T(L"log.scan_start")) + path);
}

static void OpenSnapshotView(const std::wstring& file)
{
    ViewState* v = NewViewObject();
    v->path = file;
    v->title = std::wstring(ss::i18n::T(L"view.title.snapshot")) + file;
    v->hwnd = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 100, 100, g_main, nullptr, GetModuleHandleW(nullptr), v);
    std::wstring err;
    if (LoadSnapshot(file, v->tree, err))
    {
        v->tree.ApplyGroups(g_cfg.classes);
        v->focus = v->tree.root;
        v->title = v->tree.Path(v->tree.root);
        UpdateViewStatus(v);
        LogLine(std::wstring(ss::i18n::T(L"log.snap_loaded")) + file);
    }
    else
    {
        v->status = std::wstring(ss::i18n::T(L"view.status.load_failed")) + err;
        LogLine(std::wstring(ss::i18n::T(L"log.snap_load_failed")) + err);
    }
    LayoutMain(g_main);
    ShowView((int)g_views.size() - 1);
    RelayoutView(v);
    RefreshMain();
}

static void CloseView(int index)
{
    if (index < 0 || index >= (int)g_views.size()) return;
    ViewState* v = g_views[index].get();
    InterlockedExchange(&v->progress.state, 2);
    if (v->worker.joinable()) v->worker.join();
    v->scanning = false;
    if (v->hwnd) DestroyWindow(v->hwnd);
    g_views.erase(g_views.begin() + index);
    if (g_views.empty()) { ShowView(-1); return; }
    ShowView(std::min(index, (int)g_views.size() - 1));
}


static void ApplyFilter(ViewState* v)
{
    if (!v || v->scanning) return;
    wchar_t buf[1024] = {};
    if (v->editFilter) GetWindowTextW(v->editFilter, buf, _countof(buf));
    v->filterText = buf;
    v->back.clear();
    v->forward.clear();
    v->selected = Node::kNone;
    v->hover = Node::kNone;
    FilterExpr fx = FilterExpr::Parse(buf);
    if (fx.empty())
    {
        v->usingFiltered = false;
        v->focus = v->tree.Valid(v->tree.root) ? v->tree.root : 0;
    }
    else
    {
        Tree out;
        if (ProjectFilter(v->tree, v->tree.root, fx, g_cfg.classes, out))
        {
            v->filtered = std::move(out);
            v->filtered.ApplyGroups(g_cfg.classes);
            v->usingFiltered = true;
            v->focus = v->filtered.root;
        }
        else
        {
            v->usingFiltered = true;
            v->filtered.Clear();
            v->focus = 0;
        }
    }
    RelayoutView(v);
    LogLine(std::wstring(ss::i18n::T(L"log.filter_applied")) + std::wstring(buf));
    RefreshMain();
}

static void DoCommand(int cmd)
{
    ViewState* v = CurrentView();
    switch (cmd)
    {
        case CMD_NEW:
            ShowView(-1);
            if (!g_start)
            {
                g_start = CreateWindowExW(0, kStartClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100,
                                          g_main, nullptr, GetModuleHandleW(nullptr), nullptr);
                BuildDriveRows();
            }
            LayoutMain(g_main);
            ShowView(-1);
            break;
        case CMD_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
        case CMD_CONFIG: ShowConfigDlg(g_main); break;
        case CMD_EXPORT: ShowExportDlg(g_main); break;
        case CMD_SNAPSHOT:
        {
            using ss::i18n::T;
            if (!v || v->scanning || !v->tree.Valid(v->tree.root)) { LogLine(T(L"log.snap_none")); break; }
            std::vector<wchar_t> fileBuf(4096, L'\0');
            std::wstring filter = MakeFileFilter({
                { T(L"dlg.snap_filter_ssx"), L"*.ssx" },
                { T(L"dlg.snap_filter_du"),  L"*.du" },
            });
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_main;
            ofn.lpstrFilter = filter.c_str();
            ofn.lpstrFile = fileBuf.data();
            ofn.nMaxFile = (DWORD)fileBuf.size();
            ofn.lpstrDefExt = L"ssx";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn))
            {
                bool native = false;
                if (SaveSnapshot(v->tree, fileBuf.data(), native))
                    LogLine(std::wstring(T(L"log.snap_saved")) + std::wstring(fileBuf.data()));
                else
                    LogLine(T(L"log.snap_save_failed"));
            }
            break;
        }
        case CMD_BACK:
            if (v && !v->scanning && !v->back.empty())
            {
                Tree& t = ActiveTree(v);
                uint32_t target = v->back.back();
                v->back.pop_back();
                if (t.Valid(target))
                {
                    v->forward.push_back(v->focus);
                    NavigateFocus(v, target);
                    RefreshMain();
                }
            }
            break;
        case CMD_FWD:
            if (v && !v->scanning && !v->forward.empty())
            {
                Tree& t = ActiveTree(v);
                uint32_t target = v->forward.back();
                v->forward.pop_back();
                if (t.Valid(target))
                {
                    v->back.push_back(v->focus);
                    NavigateFocus(v, target);
                    RefreshMain();
                }
            }
            break;
        case CMD_UP:
            if (v && !v->scanning)
            {
                Tree& t = ActiveTree(v);
                if (t.Valid(v->focus) && t.Valid(t.nodes[v->focus].parent))
                {
                    v->back.push_back(v->focus);
                    NavigateFocus(v, t.nodes[v->focus].parent);
                    RefreshMain();
                }
            }
            break;
        case CMD_HOME:
            if (v && !v->scanning)
            {
                Tree& t = ActiveTree(v);
                if (t.Valid(t.root) && v->focus != t.root)
                {
                    v->back.push_back(v->focus);
                    NavigateFocus(v, t.root);
                    RefreshMain();
                }
            }
            break;
        case CMD_REFRESH:
            if (v && !v->scanning) { StartScan(v, v->path); }
            break;
        case CMD_FILTERBAR:
            if (v)
            {
                v->filterVisible = !v->filterVisible;
                if (v->editFilter)
                {
                    ShowWindow(v->editFilter, v->filterVisible ? SW_SHOW : SW_HIDE);
                    if (v->filterVisible) SetFocus(v->editFilter);
                }
                RelayoutView(v);
                InvalidateRect(v->hwnd, nullptr, TRUE);
            }
            break;
        case CMD_STOP:
            if (v && v->scanning) { InterlockedExchange(&v->progress.state, 2); LogLine(ss::i18n::T(L"log.scan_stop_req")); }
            break;
        case CMD_DETAIL_PLUS:
            g_cfg.initialDetailLevel = std::min(15, g_cfg.initialDetailLevel + 1);
            if (v) { RelayoutView(v); RefreshMain(); }
            LogLine(std::wstring(ss::i18n::T(L"log.detail_level")) + std::to_wstring(g_cfg.initialDetailLevel));
            break;
        case CMD_DETAIL_MINUS:
            g_cfg.initialDetailLevel = std::max(1, g_cfg.initialDetailLevel - 1);
            if (v) { RelayoutView(v); RefreshMain(); }
            LogLine(std::wstring(ss::i18n::T(L"log.detail_level")) + std::to_wstring(g_cfg.initialDetailLevel));
            break;
        case CMD_STYLE:
            g_cfg.initialStyle = (g_cfg.initialStyle == L"File Classes") ? L"Flat Colors" : L"File Classes";
            if (v) { RelayoutView(v); RefreshMain(); }
            LogLine(std::wstring(ss::i18n::T(L"log.style")) + g_cfg.initialStyle);
            break;
        case CMD_FREE:
            g_cfg.showFreeSpace = !g_cfg.showFreeSpace;
            if (v) { RelayoutView(v); RefreshMain(); }
            break;
        case CMD_UNKNOWN:
            g_cfg.showUnknownSpace = !g_cfg.showUnknownSpace;
            if (v) { RelayoutView(v); RefreshMain(); }
            break;
        case CMD_DARK:
            g_cfg.darkMode = !g_cfg.darkMode;
            g_cfg.followSystemTheme = false;
            InitTheme(g_cfg.darkMode);
            RebuildFonts();
            if (g_menuBar) InvalidateRect(g_menuBar, nullptr, TRUE);
            if (g_start) InvalidateRect(g_start, nullptr, TRUE);
            for (auto& vp : g_views)
                if (vp->hwnd) InvalidateRect(vp->hwnd, nullptr, TRUE);
            ApplyDwmChrome(g_main);
            RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
            RefreshAuxWindows(false);
            break;
        case CMD_LOG: ShowLogWindow(g_main); break;
        case CMD_HELP_FILTER: ShowHelpDlg(g_main, 0); break;
        case CMD_HELP_TAG: ShowHelpDlg(g_main, 1); break;
        case CMD_HELP_CLI: ShowHelpDlg(g_main, 2); break;
        case CMD_HELP_MCP: ShowHelpDlg(g_main, 3); break;
        case CMD_ABOUT: ShowAboutDlg(g_main); break;
        case CMD_TAG1: case CMD_TAG2: case CMD_TAG3: case CMD_TAG4:
            if (v && !v->scanning)
            {
                Tree& t = ActiveTree(v);
                if (t.Valid(v->selected))
                {
                    uint8_t tag = (uint8_t)(cmd - CMD_TAG1 + 1);
                    t.nodes[v->selected].tag = (t.nodes[v->selected].tag == tag) ? 0 : tag;
                    RefreshMain();
                }
            }
            break;
        case CMD_TAG0:
            if (v && !v->scanning)
            {
                Tree& t = ActiveTree(v);
                for (auto& n : t.nodes) n.tag = 0;
                RefreshMain();
            }
            break;
    }
}

// ---------------------------------------------------------------- main proc
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Forward menu messages ONLY while a Shell IContextMenu is actively tracking.
    if (g_shellCm3 && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM ||
                       msg == WM_MEASUREITEM || msg == WM_MENUCHAR))
    {
        LRESULT lr = 0;
        if (SUCCEEDED(g_shellCm3->HandleMenuMsg2(msg, wp, lp, &lr)))
            return lr;
    }
    else if (g_shellCm2 && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM))
    {
        if (SUCCEEDED(g_shellCm2->HandleMenuMsg(msg, wp, lp)))
            return (msg == WM_INITMENUPOPUP) ? 0 : TRUE;
    }

    switch (msg)
    {
        case WM_CREATE:
        {
            g_main = hwnd;
            SetTimer(hwnd, 9, 400, nullptr);
            return 0;
        }
        case WM_SIZE:
            LayoutMain(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_WINDOWPOSCHANGED:
        {
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            RECT crc;
            if (GetClientRect(hwnd, &crc) &&
                (crc.right != g_lastClientW || crc.bottom != g_lastClientH))
            {
                g_lastClientW = crc.right;
                g_lastClientH = crc.bottom;
                LayoutMain(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return r;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT crc;
            GetClientRect(hwnd, &crc);
            if (g_lastClientW != crc.right || g_lastClientH != crc.bottom)
            {
                g_lastClientW = crc.right;
                g_lastClientH = crc.bottom;
                LayoutMain(hwnd);
            }
            PaintMain(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            DoCommand(LOWORD(wp));
            return 0;
        case WM_TIMER:
            if (wp == 9)
            {
                RECT crc;
                if (GetClientRect(hwnd, &crc) &&
                    (crc.right != g_lastClientW || crc.bottom != g_lastClientH))
                {
                    g_lastClientW = crc.right;
                    g_lastClientH = crc.bottom;
                    LayoutMain(hwnd);
                }
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            for (const auto& t : g_toolHits)
            {
                if (PtInRect(&t.rect, p))
                {
                    int cmd = t.id;
                    DoCommand(cmd);
                    return 0;
                }
            }
            for (const auto& t : g_tabHits)
            {
                if (!PtInRect(&t.rect, p)) continue;
                if (t.close)
                {
                    if (t.index > 0) CloseView(t.index - 1);
                    return 0;
                }
                ShowView(t.index == 0 ? -1 : t.index - 1);
                return 0;
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_mainTrackMouse)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_mainTrackMouse = true;
            }
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int nextHover = 0;
            if (PtInRect(&g_rcToolbar, p))
            {
                for (const auto& t : g_toolHits)
                {
                    if (PtInRect(&t.rect, p)) { nextHover = t.id; break; }
                }
            }
            if (nextHover != g_toolHoverId)
            {
                g_toolHoverId = nextHover;
                InvalidateRect(hwnd, &g_rcToolbar, FALSE);
                InvalidateRect(hwnd, &g_rcStatus, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_mainTrackMouse = false;
            if (g_toolHoverId != 0)
            {
                g_toolHoverId = 0;
                InvalidateRect(hwnd, &g_rcToolbar, FALSE);
                InvalidateRect(hwnd, &g_rcStatus, FALSE);
            }
            return 0;
        case WM_DPICHANGED:
        {
            int newDpi = HIWORD(wp);
            g_dpi = newDpi > 0 ? newDpi : 96;
            RebuildFonts();
            BuildDriveRows();
            for (auto& vp : g_views)
            {
                if (vp->editFilter) SendMessageW(vp->editFilter, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
            }
            RefreshAuxWindows(false);
            RECT* r = (RECT*)lp;
            if (r)
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutMain(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl)
            {
                switch (wp)
                {
                    case 'N': DoCommand(CMD_NEW); return 0;
                    case VK_HOME: DoCommand(CMD_HOME); return 0;
                    case VK_UP: DoCommand(CMD_UP); return 0;
                    case VK_OEM_PLUS: case VK_ADD: DoCommand(CMD_DETAIL_PLUS); return 0;
                    case VK_OEM_MINUS: case VK_SUBTRACT: DoCommand(CMD_DETAIL_MINUS); return 0;
                    case 'F': DoCommand(CMD_FREE); return 0;
                    case 'L': DoCommand(CMD_FILTERBAR); return 0;
                    case 'U': DoCommand(CMD_UNKNOWN); return 0;
                    case 'T': DoCommand(CMD_STYLE); return 0;
                    case '1': DoCommand(CMD_TAG1); return 0;
                    case '2': DoCommand(CMD_TAG2); return 0;
                    case '3': DoCommand(CMD_TAG3); return 0;
                    case '4': DoCommand(CMD_TAG4); return 0;
                    case '0': DoCommand(CMD_TAG0); return 0;
                }
            }
            if (wp == VK_F5) { DoCommand(CMD_REFRESH); return 0; }
            if (wp == VK_F1) { DoCommand(CMD_HELP_FILTER); return 0; }
            if (wp == VK_BACK) { DoCommand(shift ? CMD_FWD : CMD_BACK); return 0; }
            return 0;
        }
        case WM_CLOSE:
            for (auto& v : g_views)
            {
                InterlockedExchange(&v->progress.state, 2);
                if (v->worker.joinable()) v->worker.join();
                v->scanning = false;
            }
            if (g_log && IsWindow(g_log)) DestroyWindow(g_log);
            {
                auto helpCopy = g_helpWindows;
                for (const auto& e : helpCopy)
                    if (e.hwnd && IsWindow(e.hwnd)) DestroyWindow(e.hwnd);
                g_helpWindows.clear();
            }
            DestroyWindow(hwnd);
            g_views.clear();
            g_activeView = -1;
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 9);
            g_menuBar = nullptr;
            g_start = nullptr;
            g_main = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- dialogs (simplified but functional)
static void GetHelpContent(int which, const wchar_t*& title, const wchar_t*& body)
{
    using ss::i18n::T;
    switch (which)
    {
        case 0:  title = T(L"help.filter.title"); body = T(L"help.filter.body"); break;
        case 1:  title = T(L"help.tag.title");    body = T(L"help.tag.body");    break;
        case 2:  title = T(L"help.cli.title");    body = T(L"help.cli.body");    break;
        case 3:  title = T(L"help.mcp.title");    body = T(L"help.mcp.body");    break;
        default: title = T(L"help.filter.title"); body = T(L"help.filter.body"); break;
    }
}

static void ApplyThemeToEditWindow(HWND hwnd, HWND edit)
{
    if (!hwnd || !IsWindow(hwnd)) return;
    ApplyDwmChrome(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (edit && IsWindow(edit))
    {
        SetWindowTheme(edit, g_pal.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(edit, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(8), S(8)));
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int pad = S(8);
        int w = std::max(0, (int)rc.right - (pad + 1) * 2);
        int h = std::max(0, (int)rc.bottom - (pad + 1) * 2);
        SetWindowPos(edit, nullptr, pad + 1, pad + 1, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

static void RefreshAuxWindows(bool updateText)
{
    g_helpWindows.erase(
        std::remove_if(g_helpWindows.begin(), g_helpWindows.end(),
                       [](const HelpWindowEntry& e) { return !e.hwnd || !IsWindow(e.hwnd); }),
        g_helpWindows.end());

    if (g_log && IsWindow(g_log))
    {
        if (updateText) SetWindowTextW(g_log, ss::i18n::T(L"log.title"));
        ApplyThemeToEditWindow(g_log, g_logEdit);
    }
    for (const auto& e : g_helpWindows)
    {
        HWND edit = GetDlgItem(e.hwnd, 1);
        if (updateText)
        {
            const wchar_t* title = nullptr;
            const wchar_t* body = nullptr;
            GetHelpContent(e.which, title, body);
            SetWindowTextW(e.hwnd, title);
            if (edit && IsWindow(edit)) SetWindowTextW(edit, body);
        }
        ApplyThemeToEditWindow(e.hwnd, edit);
    }
}

static LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            auto* cs = (CREATESTRUCTW*)lp;
            const wchar_t* text = (cs && cs->lpCreateParams) ? (const wchar_t*)cs->lpCreateParams : L"";
            HWND edit = CreateWindowExW(0, L"EDIT", text,
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                        0, 0, 100, 100, hwnd, (HMENU)1, GetModuleHandleW(nullptr), nullptr);
            if (edit)
            {
                SetWindowTheme(edit, g_pal.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                SendMessageW(edit, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
                SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(8), S(8)));
            }
            return 0;
        }
        case WM_SIZE:
        {
            HWND edit = GetDlgItem(hwnd, 1);
            if (edit)
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int pad = S(8);
                int w = std::max(0, (int)rc.right - (pad + 1) * 2);
                int h = std::max(0, (int)rc.bottom - (pad + 1) * 2);
                SetWindowPos(edit, nullptr, pad + 1, pad + 1, w, h, SWP_NOZORDER);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = (HDC)wp;
            COLORREF bg = g_pal.panel;
            SetTextColor(dc, g_pal.text);
            SetBkColor(dc, bg);
            return (LRESULT)GetViewerBrush(bg);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Fill(dc, rc, g_pal.bg);
            int pad = S(8);
            if (rc.right > pad * 2 + 2 && rc.bottom > pad * 2 + 2)
                Frame(dc, RECT{ pad, pad, rc.right - pad, rc.bottom - pad }, g_pal.border);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
        {
            g_helpWindows.erase(
                std::remove_if(g_helpWindows.begin(), g_helpWindows.end(),
                               [hwnd](const HelpWindowEntry& e) { return e.hwnd == hwnd; }),
                g_helpWindows.end());
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowHelpDlg(HWND owner, int which)
{
    g_helpWindows.erase(
        std::remove_if(g_helpWindows.begin(), g_helpWindows.end(),
                       [](const HelpWindowEntry& e) { return !e.hwnd || !IsWindow(e.hwnd); }),
        g_helpWindows.end());

    for (const auto& e : g_helpWindows)
    {
        if (e.which == which)
        {
            ApplyThemeToEditWindow(e.hwnd, GetDlgItem(e.hwnd, 1));
            ShowWindow(e.hwnd, SW_SHOW);
            SetForegroundWindow(e.hwnd);
            return;
        }
    }

    const wchar_t* title = nullptr;
    const wchar_t* body = nullptr;
    GetHelpContent(which, title, body);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, kHelpClass, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(820), S(640), owner, nullptr,
                             GetModuleHandleW(nullptr), (LPVOID)body);
    if (h)
    {
        g_helpWindows.push_back({ h, which });
        ApplyDwmChrome(h);
        ShowWindow(h, SW_SHOW);
        UpdateWindow(h);
    }
}

struct AboutState
{
    HWND hwnd = nullptr;
    RECT linkRect{};
    RECT okRect{};
    bool linkHover = false;
    bool okHover = false;
    bool trackingMouse = false;
    bool closed = false;
};

static void PaintAboutDlg(HWND hwnd, AboutState& st)
{
    using ss::i18n::T;
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0)
    {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    if (!dc || !bmp)
    {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    Fill(dc, rc, g_pal.bg);

    int padX = S(22);
    int footerH = S(48);
    RECT footer{ 0, rc.bottom - footerH, rc.right, rc.bottom };
    Fill(dc, footer, g_pal.panel);
    Fill(dc, RECT{ 0, footer.top - 1, rc.right, footer.top }, g_pal.border);

    // Header: product name + license / version line
    RECT headRc{ padX, S(18), rc.right - padX, S(40) };
    TextAt(dc, T(L"about.heading"), headRc, g_pal.text, g_fontUIBold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT licRc{ padX, S(40), rc.right - padX, S(58) };
    TextAt(dc, T(L"about.license"), licRc, g_pal.textDim, g_fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    Fill(dc, RECT{ padX, S(66), rc.right - padX, S(67) }, g_pal.border);

    // Body text
    RECT bodyRc{ padX, S(78), rc.right - padX, footer.top - S(42) };
    TextAt(dc, T(L"about.body"), bodyRc, g_pal.text, g_fontUI, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    // Clickable repository link row
    const wchar_t* repoLabel = T(L"about.repo_label");
    const wchar_t* repoUrl = T(L"about.repo_url");
    int linkY = footer.top - S(34);
    int linkH = S(22);
    SIZE lblSz{}, urlSz{};
    {
        HGDIOBJ oldF = SelectObject(dc, g_fontUI);
        GetTextExtentPoint32W(dc, repoLabel, (int)wcslen(repoLabel), &lblSz);
        GetTextExtentPoint32W(dc, repoUrl, (int)wcslen(repoUrl), &urlSz);
        SelectObject(dc, oldF);
    }
    RECT lblRc{ padX, linkY, padX + lblSz.cx, linkY + linkH };
    TextAt(dc, repoLabel, lblRc, g_pal.textDim, g_fontUI, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    int urlX = lblRc.right;
    int urlR = std::min((int)(rc.right - padX), urlX + (int)urlSz.cx);
    st.linkRect = RECT{ urlX, linkY, urlR, linkY + linkH };
    COLORREF linkCol = st.linkHover
        ? (g_pal.dark ? RGB(0x74, 0xB4, 0xFF) : RGB(0x0D, 0x4F, 0xB8))
        : g_pal.accent;
    TextAt(dc, repoUrl, st.linkRect, linkCol, g_fontUI, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    // 1px underline below the URL text so clickability is obvious
    int ulY = st.linkRect.bottom - S(3);
    Fill(dc, RECT{ st.linkRect.left, ulY, st.linkRect.right, ulY + 1 }, linkCol);

    // Footer OK button
    int btnW = S(84);
    int btnH = S(26);
    int btnY = footer.top + (footerH - btnH) / 2;
    st.okRect = RECT{ rc.right - padX - btnW, btnY, rc.right - padX, btnY + btnH };
    COLORREF okBg = st.okHover
        ? (g_pal.dark ? RGB(0x66, 0xAA, 0xFF) : RGB(0x17, 0x5C, 0xC7))
        : g_pal.accent;
    Fill(dc, st.okRect, okBg);
    Frame(dc, st.okRect, st.okHover ? g_pal.text : g_pal.accent);
    TextAt(dc, T(L"about.ok"), st.okRect, RGB(255, 255, 255), g_fontUI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = (CREATESTRUCTW*)lp;
        if (!cs || !cs->lpCreateParams) return FALSE;
        auto* st = (AboutState*)cs->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* st = (AboutState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            PaintAboutDlg(hwnd, *st);
            return 0;
        case WM_MOUSEMOVE:
        {
            if (!st->trackingMouse)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                st->trackingMouse = true;
            }
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool overLink = PtInRect(&st->linkRect, pt) != 0;
            bool overOk = PtInRect(&st->okRect, pt) != 0;
            if (overLink != st->linkHover || overOk != st->okHover)
            {
                st->linkHover = overLink;
                st->okHover = overOk;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            st->trackingMouse = false;
            if (st->linkHover || st->okHover)
            {
                st->linkHover = false;
                st->okHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT)
            {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&st->linkRect, pt) || PtInRect(&st->okRect, pt))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP:
        {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&st->linkRect, pt))
            {
                const wchar_t* url = ss::i18n::T(L"about.repo_url");
                ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (PtInRect(&st->okRect, pt))
            {
                st->closed = true;
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            st->closed = true;
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            st->hwnd = nullptr;
            st->closed = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowAboutDlg(HWND owner)
{
    using ss::i18n::T;
    AboutState st{};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    RECT wr{ 0, 0, S(560), S(304) };
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    RECT work{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    RECT anchor = work;
    if (owner && IsWindow(owner)) GetWindowRect(owner, &anchor);
    int x = std::clamp((int)(anchor.left + ((anchor.right - anchor.left) - winW) / 2),
                       (int)work.left, std::max((int)work.left, (int)work.right - winW));
    int y = std::clamp((int)(anchor.top + ((anchor.bottom - anchor.top) - winH) / 2),
                       (int)work.top, std::max((int)work.top, (int)work.bottom - winH));

    bool hasOwner = (owner != nullptr && IsWindow(owner));
    bool ownerWasEnabled = hasOwner && (IsWindowEnabled(owner) != FALSE);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);

    HWND dlg = CreateWindowExW(exStyle, kAboutClass, T(L"about.title"), style,
                               x, y, winW, winH, owner, nullptr,
                               GetModuleHandleW(nullptr), &st);
    if (!dlg)
    {
        if (ownerWasEnabled && IsWindow(owner)) EnableWindow(owner, TRUE);
        return;
    }

    ApplyDwmChrome(dlg);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetFocus(dlg);

    MSG msg{};
    bool gotQuit = false;
    WPARAM quitCode = 0;
    while (!st.closed)
    {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { gotQuit = true; quitCode = msg.wParam; break; }
        if (r < 0) break;
        if (msg.hwnd == dlg && msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN || msg.wParam == VK_SPACE))
        {
            st.closed = true;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (ownerWasEnabled && IsWindow(owner)) EnableWindow(owner, TRUE);
    if (IsWindow(dlg)) DestroyWindow(dlg);
    if (hasOwner && IsWindow(owner)) SetActiveWindow(owner);
    if (gotQuit) PostQuitMessage((int)quitCode);
}

static void ShowExportDlg(HWND owner)
{
    using ss::i18n::T;
    ViewState* v = CurrentView();
    if (!v || v->scanning || !v->tree.Valid(v->tree.root))
    {
        MessageBoxW(owner, T(L"export.no_view"), T(L"export.title"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_cfg.exports.empty()) return;

    std::vector<PopupMenuItem> items;
    items.reserve(g_cfg.exports.size());
    for (size_t i = 0; i < g_cfg.exports.size(); ++i)
        items.push_back({ (int)(i + 1), g_cfg.exports[i].name, L"", false, false, true });

    POINT pt{ 0, 0 };
    GetCursorPos(&pt);                 // open where the user clicked, not at the window corner
    int sel = TrackCustomPopupMenu(owner, items, pt, nullptr, -1, nullptr);
    if (sel <= 0 || (size_t)(sel - 1) >= g_cfg.exports.size()) return;
    const ExportConfig cfg = g_cfg.exports[(size_t)(sel - 1)];

    std::wstring defName = cfg.preferredFileName + cfg.preferredExtension;
    std::vector<wchar_t> fileBuf(4096, L'\0');
    wcsncpy_s(fileBuf.data(), fileBuf.size(), defName.c_str(), _TRUNCATE);

    std::wstring filter = MakeFileFilter({
        { T(L"dlg.export_filter_txt"), L"*.txt" },
        { T(L"dlg.export_filter_csv"), L"*.csv" },
        { T(L"dlg.export_filter_all"), L"*.*" },
    });

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = (DWORD)fileBuf.size();
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // Re-fetch current view safely after modal file dialog
    v = CurrentView();
    if (!v || v->scanning || !v->tree.Valid(v->tree.root)) return;
    std::wstring outPath(fileBuf.data());
    ExportResult r = ExportRun(v->tree, v->tree.root, cfg, g_cfg.classes, outPath,
                               [](int64_t done, int64_t total) { (void)done; (void)total; });
    {
        std::wstring msg2 = std::wstring(r.ok ? T(L"export.done_msg") : T(L"export.fail_msg")) + outPath;
        MessageBoxW(owner, msg2.c_str(), T(L"export.title"), MB_OK | (r.ok ? MB_ICONINFORMATION : MB_ICONERROR));
    }
    LogLine(std::wstring(r.ok ? T(L"log.export_done") : T(L"log.export_failed")) + outPath);
}

// ---------------------------------------------------------------- startup / command line
struct CmdRequest { bool load; std::wstring target; std::wstring filter, exportName, exportPath; };

static void DoAutoExport(const std::wstring& name, const std::wstring& path)
{
    using ss::i18n::T;
    for (auto& e : g_cfg.exports)
    {
        if (_wcsicmp(e.name.c_str(), name.c_str()) != 0) continue;
        ViewState* v = CurrentView();
        if (!v) return;
        ExportResult r = ExportRun(v->tree, v->tree.root, e, g_cfg.classes, path, nullptr);
        LogLine(std::wstring(r.ok ? T(L"log.auto_export_done") : T(L"log.auto_export_failed")) + path);
        return;
    }
    std::wstring warn = std::wstring(T(L"export.cfg_not_found")) + name;
    MessageBoxW(g_main, warn.c_str(), T(L"export.title"), MB_OK | MB_ICONWARNING);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show)
{
    (void)cmdLine;
    bool oleInited = SUCCEEDED(OleInitialize(nullptr));
    {
        using SetCtxFn = BOOL (WINAPI*)(void*);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto setCtx = user32 ? (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : nullptr;
        if (setCtx) setCtx((void*)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        else SetProcessDPIAware();
    }
    InitCommonControls();

    HDC screen = GetDC(nullptr);
    if (screen)
    {
        g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    if (g_dpi <= 0) g_dpi = 96;

    g_cfg = AppConfig::Default();
    std::wstring exeBuf(4096, L'\0');
    DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf.data(), (DWORD)exeBuf.size());
    std::wstring exeDir = (exeLen > 0 && exeLen < exeBuf.size()) ? exeBuf.substr(0, exeLen) : L"";
    size_t slash = exeDir.find_last_of(L'\\');
    if (slash != std::wstring::npos) exeDir = exeDir.substr(0, slash);
    std::wstring newCfgPath = exeDir.empty() ? L"NeoSnifferConfig.xml" : (exeDir + L"\\NeoSnifferConfig.xml");
    std::wstring legacyCfgPath = exeDir.empty() ? L"SpaceSnifferConfig.xml" : (exeDir + L"\\SpaceSnifferConfig.xml");
    if (!g_cfg.Load(newCfgPath))
    {
        g_cfg.Load(legacyCfgPath);
        g_cfg.path = newCfgPath;
    }
    // first run (or migrated from legacy config): persist to NeoSnifferConfig.xml
    if (GetFileAttributesW(g_cfg.path.c_str()) == INVALID_FILE_ATTRIBUTES) g_cfg.Save();

    g_isElevated = IsProcessElevated();
    bool startupRunasFailed = false;
    DWORD startupRunasErr = ERROR_SUCCESS;
    if (g_cfg.alwaysRunAsAdmin && !g_isElevated)
    {
        if (RelaunchElevated(nullptr, &startupRunasErr))
        {
            if (oleInited) OleUninitialize();
            return 0;
        }
        // P1: UAC cancelled or elevation failed -> continue cleanly as non-elevated.
        startupRunasFailed = true;
    }

    ss::i18n::Init(g_cfg.language);
    InitIconFont();
    InitTheme(g_cfg.darkMode);
    RebuildFonts();

    HICON hIconBig = LoadIconW(inst, MAKEINTRESOURCEW(1));
    HICON hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                      LR_DEFAULTCOLOR | LR_SHARED);
    if (!hIconSm) hIconSm = hIconBig;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSm;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    wc.hIcon = nullptr;
    wc.hIconSm = nullptr;
    wc.lpfnWndProc = ViewProc;
    wc.lpszClassName = kViewClass;
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = CanvasProc;
    wc.lpszClassName = kCanvasClass;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = StartProc;
    wc.lpszClassName = kStartClass;
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = MenuBarProc;
    wc.lpszClassName = kMenuBarClass;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PopupMenuProc;
    wc.lpszClassName = kPopupMenuClass;
    wc.style = CS_DROPSHADOW;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSm;
    wc.lpfnWndProc = LogProc;
    wc.lpszClassName = kLogClass;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = HelpProc;
    wc.lpszClassName = kHelpClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = AboutProc;
    wc.lpszClassName = kAboutClass;
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int winW = std::min((int)(work.right - work.left) - S(40), S(1240));
    int winH = std::min((int)(work.bottom - work.top) - S(40), S(800));
    int winX = work.left + ((work.right - work.left) - winW) / 2;
    int winY = work.top + ((work.bottom - work.top) - winH) / 2;
    HWND main = CreateWindowExW(0, kMainClass, AppTitle(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                winX, winY, winW, winH, nullptr, nullptr, inst, nullptr);
    if (!main)
    {
        CleanupGlobalGdi();
        if (oleInited) OleUninitialize();
        return 1;
    }

    if (hIconBig) SendMessageW(main, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSm)  SendMessageW(main, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);

    ApplyDwmChrome(main);
    ShowWindow(main, show);
    UpdateWindow(main);
    {
        RECT crc;
        GetClientRect(main, &crc);
        g_lastClientW = crc.right;
        g_lastClientH = crc.bottom;
    }

    g_menuBar = CreateWindowExW(0, kMenuBarClass, L"", WS_CHILD | WS_VISIBLE,
                                0, 0, 100, 30, main, nullptr, inst, nullptr);

    // start page
    g_start = CreateWindowExW(0, kStartClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, main, nullptr, inst, nullptr);
    BuildDriveRows();
    LayoutMain(main);
    ShowView(-1);
    LogLine(ss::i18n::T(L"log.started"));
    if (startupRunasFailed)
    {
        if (startupRunasErr == ERROR_CANCELLED)
            LogLine(ss::i18n::T(L"log.runas_cancelled"));
        else
            LogLine(std::wstring(ss::i18n::T(L"log.runas_failed")) + std::to_wstring(startupRunasErr) + L")");
    }
    SyncShellIntegration(g_cfg.shellIntegration, false);

    // ---- command line ----
    std::vector<std::wstring> args;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
            LocalFree(argv);
        }
    }
    auto sanitizeCliPath = [](std::wstring s) -> std::wstring
    {
        // Explorer expands "%V" for a drive root as "C:\", and CommandLineToArgvW
        // parses the trailing \" as a literal quote (yielding C:"). Restore the backslash.
        if (!s.empty() && s.back() == L'"')
            s.back() = L'\\';
        return s;
    };
    bool autoClose = false;
    std::vector<CmdRequest> requests;
    for (size_t i = 0; i < args.size(); ++i)
    {
        std::wstring a = Lowercase(args[i]);
        if (a == L"scan" || a == L"path") { if (i + 1 < args.size()) { CmdRequest r{}; r.load = false; r.target = sanitizeCliPath(args[++i]); requests.push_back(r); } }
        else if (a == L"load") { if (i + 1 < args.size()) { CmdRequest r{}; r.load = true; r.target = sanitizeCliPath(args[++i]); requests.push_back(r); } }
        else if (a == L"filter") { if (i + 1 < args.size() && !requests.empty()) requests.back().filter = args[++i]; else if (i + 1 < args.size()) ++i; }
        else if (a == L"export")
        {
            if (i + 2 < args.size() && !requests.empty()) { requests.back().exportName = args[++i]; requests.back().exportPath = args[++i]; }
            else i = std::min(args.size(), i + 2);
        }
        else if (a == L"autoclose") autoClose = true;
        else if (a == L"help" || a == L"/?" || a == L"-h")
        {
            ShowHelpDlg(main, 2);
        }
        else if (!a.empty() && a[0] != L'-' && a[0] != L'/')
        {
            CmdRequest r{};
            std::wstring p = sanitizeCliPath(args[i]);
            DWORD attrs = GetFileAttributesW(p.c_str());
            r.load = attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
            r.target = std::move(p);
            requests.push_back(r);
        }
    }
    for (auto& r : requests)
    {
        if (r.load)
        {
            OpenSnapshotView(r.target);
            if (!r.filter.empty()) { auto v = CurrentView(); if (v && v->editFilter) { SetWindowTextW(v->editFilter, r.filter.c_str()); ApplyFilter(v); } }
        }
        else
        {
            OpenScanView(r.target, r.filter);
        }
        if (!r.exportName.empty())
        {
            // wait for the scan to finish
            for (int spin = 0; spin < 600; ++spin)
            {
                ViewState* v = CurrentView();
                if (v && !v->scanning) break;
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                Sleep(50);
            }
            DoAutoExport(r.exportName, r.exportPath);
        }
    }
    if (autoClose)
    {
        PostMessageW(main, WM_CLOSE, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_views.clear();
    g_cfg.Save();
    CleanupGlobalGdi();
    if (oleInited) OleUninitialize();
    return 0;
}
