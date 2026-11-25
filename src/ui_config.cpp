// ============================================================================
// SpaceSniffer Native Config Dialog (Compact + Anti-Aliased, Win32 Tool Style)
//
// Architecture (3 Layers):
//   1. Layout & Theme Layer   : DPI scaling, dark/light color tokens, window /
//                               sidebar / content / row / table / footer rects.
//   2. Control & State Layer  : C++20 config field adapters, tab/row schemas,
//                               hit-testing, slider/scrollbar capture, inline
//                               WM_CHAR cell editor, modal dialog loop.
//   3. Render Layer           : Offscreen double-buffered GDI (1px hairlines,
//                               ClearType text) + scoped GDI+ anti-aliasing
//                               for checkbox ticks and slider tracks/thumbs.
//
// Constraints: Win32 + GDI + GDI+ (-lgdiplus), C++20, no exceptions, no RTTI.
// ============================================================================

#include "ui_config.h"
#include "i18n.h"

#include <windowsx.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef NOMINMAX
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#endif
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace ss::ui {
namespace {

// ============================================================================
// Utilities & RAII Wrappers (Zero GDI / GDI+ Leaks)
// ============================================================================

constexpr int kMinInt(int a, int b) { return a < b ? a : b; }
constexpr int kMaxInt(int a, int b) { return a > b ? a : b; }
constexpr int kClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct ScopedFont {
    HFONT handle = nullptr;
    ScopedFont() = default;
    explicit ScopedFont(HFONT h) : handle(h) {}
    ~ScopedFont() { Reset(); }
    ScopedFont(const ScopedFont&) = delete;
    ScopedFont& operator=(const ScopedFont&) = delete;
    void Reset(HFONT h = nullptr) {
        if (handle) DeleteObject(handle);
        handle = h;
    }
    operator HFONT() const { return handle; }
};

struct ScopedPen {
    HPEN handle = nullptr;
    explicit ScopedPen(HPEN h = nullptr) : handle(h) {}
    ~ScopedPen() { if (handle) DeleteObject(handle); }
    ScopedPen(const ScopedPen&) = delete;
    ScopedPen& operator=(const ScopedPen&) = delete;
    operator HPEN() const { return handle; }
};

struct ScopedBrush {
    HBRUSH handle = nullptr;
    explicit ScopedBrush(HBRUSH h = nullptr) : handle(h) {}
    ~ScopedBrush() { if (handle) DeleteObject(handle); }
    ScopedBrush(const ScopedBrush&) = delete;
    ScopedBrush& operator=(const ScopedBrush&) = delete;
    operator HBRUSH() const { return handle; }
};

struct ScopedBitmap {
    HBITMAP handle = nullptr;
    explicit ScopedBitmap(HBITMAP h = nullptr) : handle(h) {}
    ~ScopedBitmap() { if (handle) DeleteObject(handle); }
    ScopedBitmap(const ScopedBitmap&) = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;
    operator HBITMAP() const { return handle; }
};

struct ScopedDC {
    HDC handle = nullptr;
    explicit ScopedDC(HDC h = nullptr) : handle(h) {}
    ~ScopedDC() { if (handle) DeleteDC(handle); }
    ScopedDC(const ScopedDC&) = delete;
    ScopedDC& operator=(const ScopedDC&) = delete;
    operator HDC() const { return handle; }
};

struct GdiplusSession {
    ULONG_PTR token = 0;
    bool active = false;

    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
            active = true;
        }
    }
    ~GdiplusSession() {
        if (active) {
            Gdiplus::GdiplusShutdown(token);
            active = false;
        }
    }
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
};

void ApplyTitleBarDarkMode(HWND hwnd, bool dark) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using FnDwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<FnDwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn) {
        BOOL val = dark ? TRUE : FALSE;
        // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+/Win11), 19 = older Win10
        if (FAILED(fn(hwnd, 20, &val, sizeof(val)))) {
            fn(hwnd, 19, &val, sizeof(val));
        }
        COLORREF border = dark ? RGB(0x23, 0x23, 0x27) : RGB(0xC9, 0xCD, 0xD3);
        COLORREF caption = dark ? RGB(0x1A, 0x1A, 0x1D) : RGB(0xF2, 0xF4, 0xF8);
        COLORREF text = dark ? RGB(0xEC, 0xEC, 0xEF) : RGB(0x1D, 0x1F, 0x23);
        fn(hwnd, 34, &border, sizeof(border));
        fn(hwnd, 35, &caption, sizeof(caption));
        fn(hwnd, 36, &text, sizeof(text));
    }
    FreeLibrary(dwm);
}

// ============================================================================
// C++20 Safe Adapters for ss::AppConfig Fields
// ============================================================================

namespace cfg_detail {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

template <typename T>
std::wstring ToWString(const T& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, std::wstring>) {
        return v;
    } else if constexpr (std::is_same_v<D, std::string>) {
        return Utf8ToWide(v);
    } else if constexpr (requires(const D& x) { x.wstring(); }) {
        return v.wstring();
    } else {
        return {};
    }
}

template <typename T>
void AssignFromWString(T& dst, const std::wstring& src) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, std::wstring>) {
        dst = src;
    } else if constexpr (std::is_same_v<D, std::string>) {
        dst = WideToUtf8(src);
    }
}

template <typename T>
COLORREF ReadColor(const T& c) {
    return static_cast<COLORREF>(c);
}

template <typename T>
void WriteColor(T& dst, COLORREF c) {
    dst = static_cast<std::decay_t<T>>(c);
}

template <typename T>
int ReadStyleIndex(const T& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_enum_v<D> || std::is_integral_v<D>) {
        return static_cast<int>(v) == 0 ? 0 : 1;
    } else {
        std::wstring s = ToWString(v);
        for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
        return (s.find(L"class") != std::wstring::npos || s == L"1") ? 1 : 0;
    }
}

template <typename T>
void WriteStyleIndex(T& dst, int idx) {
    using D = std::decay_t<T>;
    int clamped = idx == 0 ? 0 : 1;
    if constexpr (std::is_enum_v<D> || std::is_integral_v<D>) {
        dst = static_cast<D>(clamped);
    } else {
        AssignFromWString(dst, clamped == 0 ? L"Flat Colors" : L"File Classes");
    }
}

int ReadLanguageIndex(const std::wstring& v) {
    std::wstring s = v;
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    if (s == L"zh") return 1;
    if (s == L"en") return 2;
    return 0;  // "auto"
}

void WriteLanguageIndex(std::wstring& dst, int idx) {
    if (idx == 1) dst = L"zh";
    else if (idx == 2) dst = L"en";
    else dst = L"auto";
}

template <typename T>
std::wstring ExtensionsToWString(const T& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, std::wstring> || std::is_same_v<D, std::string>) {
        return ToWString(v);
    } else {
        std::wstring out;
        for (const auto& tok : v) {
            std::wstring w = ToWString(tok);
            if (w.empty()) continue;
            if (!out.empty()) out += L"; ";
            out += w;
        }
        return out;
    }
}

template <typename T>
void AssignExtensions(T& dst, const std::wstring& text) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, std::wstring> || std::is_same_v<D, std::string>) {
        AssignFromWString(dst, text);
    } else {
        dst.clear();
        std::wstring cur;
        auto flush = [&]() {
            size_t start = 0;
            while (start < cur.size() && (cur[start] == L'*' || cur[start] == L'.')) ++start;
            if (start < cur.size()) {
                typename D::value_type elem{};
                AssignFromWString(elem, cur.substr(start));
                dst.push_back(std::move(elem));
            }
            cur.clear();
        };
        for (wchar_t ch : text) {
            if (ch == L';' || ch == L',' || ch == L' ' || ch == L'\t') {
                flush();
            } else {
                cur.push_back(ch);
            }
        }
        flush();
    }
}

template <typename Item>
std::wstring GetClassName(const Item& item) {
    return ToWString(item.name);
}

template <typename Item>
void SetClassName(Item& item, const std::wstring& v) {
    AssignFromWString(item.name, v);
}

template <typename Item>
std::wstring GetClassExts(const Item& item) {
    if constexpr (requires { item.extensions; }) {
        return ExtensionsToWString(item.extensions);
    } else if constexpr (requires { item.exts; }) {
        return ExtensionsToWString(item.exts);
    } else if constexpr (requires { item.patterns; }) {
        return ExtensionsToWString(item.patterns);
    } else if constexpr (requires { item.pattern; }) {
        return ExtensionsToWString(item.pattern);
    } else {
        return {};
    }
}

template <typename Item>
void SetClassExts(Item& item, const std::wstring& v) {
    if constexpr (requires { item.extensions; }) {
        AssignExtensions(item.extensions, v);
    } else if constexpr (requires { item.exts; }) {
        AssignExtensions(item.exts, v);
    } else if constexpr (requires { item.patterns; }) {
        AssignExtensions(item.patterns, v);
    } else if constexpr (requires { item.pattern; }) {
        AssignExtensions(item.pattern, v);
    }
}

template <typename Item>
COLORREF GetClassColor(const Item& item) {
    return ReadColor(item.color);
}

template <typename Item>
void SetClassColor(Item& item, COLORREF c) {
    WriteColor(item.color, c);
}

template <typename Cfg>
std::wstring ResolveConfigPath(const Cfg& cfg) {
    if constexpr (requires { cfg.path; }) {
        auto s = ToWString(cfg.path);
        if (!s.empty()) return s;
    } else if constexpr (requires { cfg.configPath; }) {
        auto s = ToWString(cfg.configPath);
        if (!s.empty()) return s;
    } else if constexpr (requires { cfg.iniPath; }) {
        auto s = ToWString(cfg.iniPath);
        if (!s.empty()) return s;
    } else if constexpr (requires { cfg.filePath; }) {
        auto s = ToWString(cfg.filePath);
        if (!s.empty()) return s;
    } else if constexpr (requires { cfg.xmlPath; }) {
        auto s = ToWString(cfg.xmlPath);
        if (!s.empty()) return s;
    }

    std::wstring buf(4096, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) {
        buf.resize(n);
        auto slash = buf.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            buf.resize(slash + 1);
            return buf + L"SpaceSnifferConfig.xml";
        }
    }
    return L"SpaceSnifferConfig.xml";
}

}  // namespace cfg_detail

// ============================================================================
// LAYER 1: Layout & Theme
// ============================================================================

struct ThemePalette {
    COLORREF bg;
    COLORREF panel;
    COLORREF input;
    COLORREF border;
    COLORREF borderHover;
    COLORREF text;
    COLORREF textHover;
    COLORREF textMuted;
    COLORREF accent;
    COLORREF accentHover;
    COLORREF accentText;
    COLORREF rowSelected;
    COLORREF thumbBorder;
    COLORREF thumbBorderActive;
};

ThemePalette MakePalette(bool dark) {
    if (dark) {
        return ThemePalette{
            RGB(0x1B, 0x1B, 0x1F),  // bg
            RGB(0x23, 0x23, 0x27),  // panel
            RGB(0x2A, 0x2A, 0x30),  // input
            RGB(0x3A, 0x3A, 0x42),  // border
            RGB(0x5E, 0x5E, 0x6B),  // borderHover
            RGB(0xEC, 0xEC, 0xEF),  // text
            RGB(0xFF, 0xFF, 0xFF),  // textHover
            RGB(0xA0, 0xA0, 0xA8),  // textMuted
            RGB(0x4C, 0x9A, 0xFF),  // accent
            RGB(0x66, 0xAA, 0xFF),  // accentHover
            RGB(0xFF, 0xFF, 0xFF),  // accentText
            RGB(0x22, 0x34, 0x4E),  // rowSelected
            RGB(0x1B, 0x1B, 0x1F),  // thumbBorder
            RGB(0xEC, 0xEC, 0xEF),  // thumbBorderActive
        };
    }
    return ThemePalette{
        RGB(0xF7, 0xF8, 0xFA),  // bg
        RGB(0xFF, 0xFF, 0xFF),  // panel
        RGB(0xEE, 0xF0, 0xF4),  // input
        RGB(0xD5, 0xD9, 0xE0),  // border
        RGB(0x8C, 0x95, 0xA4),  // borderHover
        RGB(0x1D, 0x1F, 0x23),  // text
        RGB(0x00, 0x00, 0x00),  // textHover
        RGB(0x66, 0x6C, 0x76),  // textMuted
        RGB(0x1F, 0x6F, 0xEB),  // accent
        RGB(0x17, 0x5C, 0xC7),  // accentHover
        RGB(0xFF, 0xFF, 0xFF),  // accentText
        RGB(0xDC, 0xE8, 0xFA),  // rowSelected
        RGB(0xFF, 0xFF, 0xFF),  // thumbBorder
        RGB(0x1D, 0x1F, 0x23),  // thumbBorderActive
    };
}

struct LayoutMetrics {
    int dpi = 96;

    int S(int px) const { return MulDiv(px, dpi, 96); }
    float Sf(float px) const { return px * static_cast<float>(dpi) / 96.0f; }

    int WindowW() const { return S(720); }
    int WindowH() const { return S(560); }
    int NavW() const { return S(160); }
    int FooterH() const { return S(52); }

    int PageHeaderH() const { return S(32); }
    int GroupHeaderH() const { return S(22); }
    int GroupHeaderBottomGap() const { return S(6); }
    int GroupGap() const { return S(8); }

    int PadX() const { return S(12); }
    int PadTop() const { return S(8); }
    int InlineGap() const { return S(10); }

    int RowH() const { return S(27); }
    int LabelW() const { return S(185); }
    int CtrlH() const { return S(18); }

    int CheckboxSize() const { return S(16); }
    int SwatchSize() const { return S(18); }
    int SliderTrackH() const { return kMaxInt(2, S(2)); }
    int SliderThumbSize() const { return kMaxInt(8, S(10)); }
    int SliderValueW() const { return S(52); }

    int SegItemW() const { return S(96); }
    int SegLangItemW() const { return S(84); }
    int SegItemH() const { return S(21); }

    int NavItemH() const { return S(28); }

    int FooterBtnW() const { return S(80); }
    int FooterBtnH() const { return S(24); }
    int FooterBtnGap() const { return S(6); }

    int ToolbarH() const { return S(30); }
    int ToolBtnW() const { return S(62); }
    int ToolBtnH() const { return S(22); }
    int TableRowH() const { return S(22); }
    int TableHeaderH() const { return S(22); }
    int TableNameColW() const { return S(170); }
    int TableColorColW() const { return S(50); }  // Fits 18px swatch + unclipped "颜色"/"Color" header
    int TableSwatchSize() const { return S(16); }
    int ScrollbarW() const { return S(8); }
    int BodyScrollbarW() const { return kMaxInt(4, S(6)); }
};

struct TableLayout {
    RECT toolbarRect{};
    RECT btnAddRect{};
    RECT btnDelRect{};
    RECT hintRect{};

    RECT frameRect{};
    RECT headerRect{};
    RECT bodyRect{};
    RECT scrollTrackRect{};
    RECT scrollThumbRect{};

    int colNameX = 0;
    int colNameW = 0;
    int colExtX = 0;
    int colExtW = 0;
    int colColorX = 0;
    int colColorW = 0;

    int visibleRows = 0;
    bool hasScrollbar = false;

    RECT RowRect(int visIdx, int rowH) const {
        int y = bodyRect.top + visIdx * rowH;
        return RECT{bodyRect.left, y, colColorX + colColorW, y + rowH};
    }
    RECT CellRect(int visIdx, int col, int rowH) const {
        int y = bodyRect.top + visIdx * rowH;
        if (col == 0) return RECT{colNameX, y, colNameX + colNameW, y + rowH};
        if (col == 1) return RECT{colExtX, y, colExtX + colExtW, y + rowH};
        return RECT{colColorX, y, colColorX + colColorW, y + rowH};
    }
    RECT SwatchRect(int visIdx, int rowH, int swatchSz) const {
        RECT cell = CellRect(visIdx, 2, rowH);
        int cx = (cell.left + cell.right) / 2;
        int cy = (cell.top + cell.bottom) / 2;
        int half = swatchSz / 2;
        return RECT{cx - half, cy - half, cx - half + swatchSz, cy - half + swatchSz};
    }
};

struct DialogLayout {
    RECT clientRect{};
    RECT navRect{};
    RECT contentRect{};
    RECT pageTitleRect{};
    RECT bodyRect{};
    RECT footerRect{};

    RECT okBtnRect{};
    RECT cancelBtnRect{};
    RECT pathTextRect{};

    bool bodyHasScrollbar = false;
    int bodyContentH = 0;
    int bodyViewportH = 0;
    RECT bodyScrollTrackRect{};
    RECT bodyScrollThumbRect{};

    TableLayout table{};
};

// ============================================================================
// LAYER 2: Control Model & Interaction State
// ============================================================================

enum class TabId : int {
    Geometry = 0,
    Animation = 1,
    Behavior = 2,
    Appearance = 3,
    FileClasses = 4,
    Count = 5,
};

constexpr const wchar_t* kTabNameKeys[static_cast<int>(TabId::Count)] = {
    L"cfg.tab.geometry",
    L"cfg.tab.animation",
    L"cfg.tab.behavior",
    L"cfg.tab.appearance",
    L"cfg.tab.classes",
};

constexpr const wchar_t* kTabTitleKeys[static_cast<int>(TabId::Count)] = {
    L"cfg.title.geometry",
    L"cfg.title.animation",
    L"cfg.title.behavior",
    L"cfg.title.appearance",
    L"cfg.title.classes",
};

enum class RowKind {
    GroupHeader,
    Slider,
    Checkbox,
    ColorSwatch,
    Segmented,
};

enum class FieldId {
    None,
    // Geometry
    MinElementSizePx,
    InitialDetailLevel,
    FontSize,
    Proportions,
    // Animation
    RefreshDelayMs,
    ZoomTimeMs,
    UseRealZoom,
    UseDynaSmooth,
    // Behavior
    ScanAds,
    UseMftScanner,
    HideAdminHint,
    AlwaysRunAsAdmin,
    ShellIntegration,
    ShowFreeSpace,
    ShowUnknownSpace,
    UseSorting,
    LogPopup,
    LogIssue,
    LogWarning,
    ShowTooltipDate,
    ShowTooltipAge,
    // Appearance & Colors
    DarkMode,
    Language,
    InitialStyle,
    DriveColor,
    FolderColor,
    FileColor,
    FreeColor,
    UnknownColor,
};

struct SettingRow {
    RowKind kind = RowKind::GroupHeader;
    FieldId field = FieldId::None;
    const wchar_t* label = L"";
    int minVal = 0;
    int maxVal = 100;
    const wchar_t* suffix = L"";
    bool showSign = false;
};

struct RowGeometry {
    RECT rowRect{};
    RECT labelRect{};
    RECT controlRect{};
    RECT valueRect{};
    // For segmented buttons (2 or 3 segments)
    RECT segRects[3]{};
    int segCount = 0;
};

enum class HitKind {
    None,
    NavItem,
    RowControl,
    RowSegButton,
    FooterOk,
    FooterCancel,
    TableAdd,
    TableDelete,
    TableRow,
    TableSwatch,
    TableScrollThumb,
    TableScrollTrack,
    BodyScrollThumb,
    BodyScrollTrack,
};

struct HitTarget {
    HitKind kind = HitKind::None;
    int index = -1;
    int subIndex = -1;

    bool operator==(const HitTarget& o) const {
        return kind == o.kind && index == o.index && subIndex == o.subIndex;
    }
    bool operator!=(const HitTarget& o) const { return !(*this == o); }
};

std::vector<SettingRow> BuildRowsForTab(TabId tab) {
    using ss::i18n::T;
    std::vector<SettingRow> rows;
    switch (tab) {
        case TabId::Geometry:
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.geometry")});
            rows.push_back({RowKind::Slider, FieldId::MinElementSizePx, T(L"cfg.min_elem_size"), 3, 20, L" px", false});
            rows.push_back({RowKind::Slider, FieldId::InitialDetailLevel, T(L"cfg.init_detail"), 1, 15, L"", false});
            rows.push_back({RowKind::Slider, FieldId::FontSize, T(L"cfg.font_size"), 4, 20, L" pt", false});
            rows.push_back({RowKind::Slider, FieldId::Proportions, T(L"cfg.proportions"), -60, 60, L"", true});
            break;

        case TabId::Animation:
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.timing")});
            rows.push_back({RowKind::Slider, FieldId::RefreshDelayMs, T(L"cfg.refresh_delay"), 1, 100, L" ms", false});
            rows.push_back({RowKind::Slider, FieldId::ZoomTimeMs, T(L"cfg.zoom_time"), 0, 600, L" ms", false});
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.smoothing")});
            rows.push_back({RowKind::Checkbox, FieldId::UseRealZoom, T(L"cfg.real_zoom")});
            rows.push_back({RowKind::Checkbox, FieldId::UseDynaSmooth, T(L"cfg.dyna_smooth")});
            break;

        case TabId::Behavior:
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.scanning")});
            rows.push_back({RowKind::Checkbox, FieldId::ScanAds, T(L"cfg.scan_ads")});
            rows.push_back({RowKind::Checkbox, FieldId::UseMftScanner, T(L"cfg.use_mft")});
            rows.push_back({RowKind::Checkbox, FieldId::HideAdminHint, T(L"cfg.hide_admin_hint")});
            rows.push_back({RowKind::Checkbox, FieldId::AlwaysRunAsAdmin, T(L"cfg.always_admin")});
            rows.push_back({RowKind::Checkbox, FieldId::ShellIntegration, T(L"cfg.shell_integration")});
            rows.push_back({RowKind::Checkbox, FieldId::ShowFreeSpace, T(L"cfg.show_free")});
            rows.push_back({RowKind::Checkbox, FieldId::ShowUnknownSpace, T(L"cfg.show_unknown")});
            rows.push_back({RowKind::Checkbox, FieldId::UseSorting, T(L"cfg.use_sorting")});
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.logging")});
            rows.push_back({RowKind::Checkbox, FieldId::LogPopup, T(L"cfg.log_popup")});
            rows.push_back({RowKind::Checkbox, FieldId::LogIssue, T(L"cfg.log_issue")});
            rows.push_back({RowKind::Checkbox, FieldId::LogWarning, T(L"cfg.log_warning")});
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.tooltips")});
            rows.push_back({RowKind::Checkbox, FieldId::ShowTooltipDate, T(L"cfg.tip_date")});
            rows.push_back({RowKind::Checkbox, FieldId::ShowTooltipAge, T(L"cfg.tip_age")});
            break;

        case TabId::Appearance:
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.theme")});
            rows.push_back({RowKind::Checkbox, FieldId::DarkMode, T(L"cfg.dark_mode")});
            rows.push_back({RowKind::Segmented, FieldId::Language, T(L"cfg.language")});
            rows.push_back({RowKind::Segmented, FieldId::InitialStyle, T(L"cfg.init_style")});
            rows.push_back({RowKind::GroupHeader, FieldId::None, T(L"cfg.grp.colors")});
            rows.push_back({RowKind::ColorSwatch, FieldId::DriveColor, T(L"cfg.color.drive")});
            rows.push_back({RowKind::ColorSwatch, FieldId::FolderColor, T(L"cfg.color.folder")});
            rows.push_back({RowKind::ColorSwatch, FieldId::FileColor, T(L"cfg.color.file")});
            rows.push_back({RowKind::ColorSwatch, FieldId::FreeColor, T(L"cfg.color.free")});
            rows.push_back({RowKind::ColorSwatch, FieldId::UnknownColor, T(L"cfg.color.unknown")});
            break;

        case TabId::FileClasses:
        default:
            break;
    }
    return rows;
}

int GetIntField(const ss::AppConfig& c, FieldId id) {
    switch (id) {
        case FieldId::MinElementSizePx: return static_cast<int>(c.minElementSizePx);
        case FieldId::InitialDetailLevel: return static_cast<int>(c.initialDetailLevel);
        case FieldId::FontSize: return static_cast<int>(c.fontSize);
        case FieldId::Proportions: return static_cast<int>(c.proportions);
        case FieldId::RefreshDelayMs: return static_cast<int>(c.refreshDelayMs);
        case FieldId::ZoomTimeMs: return static_cast<int>(c.zoomTimeMs);
        default: return 0;
    }
}

void SetIntField(ss::AppConfig& c, FieldId id, int val) {
    switch (id) {
        case FieldId::MinElementSizePx: c.minElementSizePx = static_cast<decltype(c.minElementSizePx)>(val); break;
        case FieldId::InitialDetailLevel: c.initialDetailLevel = static_cast<decltype(c.initialDetailLevel)>(val); break;
        case FieldId::FontSize: c.fontSize = static_cast<decltype(c.fontSize)>(val); break;
        case FieldId::Proportions: c.proportions = static_cast<decltype(c.proportions)>(val); break;
        case FieldId::RefreshDelayMs: c.refreshDelayMs = static_cast<decltype(c.refreshDelayMs)>(val); break;
        case FieldId::ZoomTimeMs: c.zoomTimeMs = static_cast<decltype(c.zoomTimeMs)>(val); break;
        default: break;
    }
}

bool GetBoolField(const ss::AppConfig& c, FieldId id) {
    switch (id) {
        case FieldId::UseRealZoom: return static_cast<bool>(c.useRealZoom);
        case FieldId::UseDynaSmooth: return static_cast<bool>(c.useDynaSmooth);
        case FieldId::ScanAds: return static_cast<bool>(c.scanAds);
        case FieldId::UseMftScanner: return static_cast<bool>(c.useMftScanner);
        case FieldId::HideAdminHint: return static_cast<bool>(c.hideAdminHint);
        case FieldId::AlwaysRunAsAdmin: return static_cast<bool>(c.alwaysRunAsAdmin);
        case FieldId::ShellIntegration: return static_cast<bool>(c.shellIntegration);
        case FieldId::ShowFreeSpace: return static_cast<bool>(c.showFreeSpace);
        case FieldId::ShowUnknownSpace: return static_cast<bool>(c.showUnknownSpace);
        case FieldId::UseSorting: return static_cast<bool>(c.useSorting);
        case FieldId::LogPopup: return static_cast<bool>(c.logPopup);
        case FieldId::LogIssue: return static_cast<bool>(c.logIssue);
        case FieldId::LogWarning: return static_cast<bool>(c.logWarning);
        case FieldId::ShowTooltipDate: return static_cast<bool>(c.showTooltipDate);
        case FieldId::ShowTooltipAge: return static_cast<bool>(c.showTooltipAge);
        case FieldId::DarkMode: return static_cast<bool>(c.darkMode);
        default: return false;
    }
}

void SetBoolField(ss::AppConfig& c, FieldId id, bool val) {
    switch (id) {
        case FieldId::UseRealZoom: c.useRealZoom = static_cast<decltype(c.useRealZoom)>(val); break;
        case FieldId::UseDynaSmooth: c.useDynaSmooth = static_cast<decltype(c.useDynaSmooth)>(val); break;
        case FieldId::ScanAds: c.scanAds = static_cast<decltype(c.scanAds)>(val); break;
        case FieldId::UseMftScanner: c.useMftScanner = static_cast<decltype(c.useMftScanner)>(val); break;
        case FieldId::HideAdminHint: c.hideAdminHint = static_cast<decltype(c.hideAdminHint)>(val); break;
        case FieldId::AlwaysRunAsAdmin: c.alwaysRunAsAdmin = static_cast<decltype(c.alwaysRunAsAdmin)>(val); break;
        case FieldId::ShellIntegration: c.shellIntegration = static_cast<decltype(c.shellIntegration)>(val); break;
        case FieldId::ShowFreeSpace: c.showFreeSpace = static_cast<decltype(c.showFreeSpace)>(val); break;
        case FieldId::ShowUnknownSpace: c.showUnknownSpace = static_cast<decltype(c.showUnknownSpace)>(val); break;
        case FieldId::UseSorting: c.useSorting = static_cast<decltype(c.useSorting)>(val); break;
        case FieldId::LogPopup: c.logPopup = static_cast<decltype(c.logPopup)>(val); break;
        case FieldId::LogIssue: c.logIssue = static_cast<decltype(c.logIssue)>(val); break;
        case FieldId::LogWarning: c.logWarning = static_cast<decltype(c.logWarning)>(val); break;
        case FieldId::ShowTooltipDate: c.showTooltipDate = static_cast<decltype(c.showTooltipDate)>(val); break;
        case FieldId::ShowTooltipAge: c.showTooltipAge = static_cast<decltype(c.showTooltipAge)>(val); break;
        case FieldId::DarkMode: c.darkMode = static_cast<decltype(c.darkMode)>(val); break;
        default: break;
    }
}

COLORREF GetColorField(const ss::AppConfig& c, FieldId id) {
    switch (id) {
        case FieldId::DriveColor: return cfg_detail::ReadColor(c.driveColor);
        case FieldId::FolderColor: return cfg_detail::ReadColor(c.folderColor);
        case FieldId::FileColor: return cfg_detail::ReadColor(c.fileColor);
        case FieldId::FreeColor: return cfg_detail::ReadColor(c.freeColor);
        case FieldId::UnknownColor: return cfg_detail::ReadColor(c.unknownColor);
        default: return RGB(128, 128, 128);
    }
}

void SetColorField(ss::AppConfig& c, FieldId id, COLORREF color) {
    switch (id) {
        case FieldId::DriveColor: cfg_detail::WriteColor(c.driveColor, color); break;
        case FieldId::FolderColor: cfg_detail::WriteColor(c.folderColor, color); break;
        case FieldId::FileColor: cfg_detail::WriteColor(c.fileColor, color); break;
        case FieldId::FreeColor: cfg_detail::WriteColor(c.freeColor, color); break;
        case FieldId::UnknownColor: cfg_detail::WriteColor(c.unknownColor, color); break;
        default: break;
    }
}

constexpr UINT_PTR kCaretTimerId = 1;

struct GdiFonts {
    ScopedFont ui;          // 9.5pt regular
    ScopedFont title;       // 12pt semibold
    ScopedFont group;       // 11pt semibold
    ScopedFont tableHeader; // 9pt semibold
    ScopedFont small;       // 9pt regular

    void Create(int dpi) {
        auto make = [&](int tenthPt, int weight) -> HFONT {
            int height = -MulDiv(tenthPt, dpi, 720);
            return CreateFontW(
                height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        };
        ui.Reset(make(95, FW_NORMAL));
        title.Reset(make(120, FW_SEMIBOLD));
        group.Reset(make(110, FW_SEMIBOLD));
        tableHeader.Reset(make(90, FW_SEMIBOLD));
        small.Reset(make(90, FW_NORMAL));
    }
};

struct DialogState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    LayoutMetrics m{};
    ThemePalette pal{};
    GdiFonts fonts{};

    ss::AppConfig draft{};
    bool darkMode = true;
    std::wstring configPath;

    TabId activeTab = TabId::Geometry;
    std::vector<SettingRow> rows;
    std::vector<RowGeometry> rowGeoms;
    DialogLayout layout{};

    HitTarget hover{};
    bool trackingMouse = false;

    // Slider drag capture
    int dragSliderRow = -1;

    // Settings body scroll state (tabs 0..3)
    int bodyScrollY = 0;
    bool draggingBodyScroll = false;
    int bodyScrollDragOffsetY = 0;

    // Cached shell integration registry presence on dialog open
    bool shellRegistered = false;

    // File classes table state
    int selectedClass = -1;
    int tableScroll = 0;
    bool draggingScroll = false;
    int scrollDragOffsetY = 0;

    // Inline cell editor (WM_CHAR driven)
    int editRow = -1;
    int editCol = -1;  // 0 = name, 1 = extensions
    std::wstring editBuffer;
    std::wstring editOriginal;
    size_t editCaret = 0;
    bool caretVisible = true;

    // Persistent custom colors for ChooseColorW
    std::array<COLORREF, 16> customColors{};

    // Modal loop state
    bool accepted = false;
    bool closed = false;

    bool IsEditingCell() const { return editRow >= 0 && (editCol == 0 || editCol == 1); }

    int ClassCount() const { return static_cast<int>(draft.classes.items.size()); }

    int MaxTableScroll() const {
        int excess = ClassCount() - layout.table.visibleRows;
        return excess > 0 ? excess : 0;
    }

    void ClampTableScroll() {
        tableScroll = kClampInt(tableScroll, 0, MaxTableScroll());
    }

    void EnsureRowVisible(int rowIdx) {
        if (rowIdx < 0 || layout.table.visibleRows <= 0) return;
        if (rowIdx < tableScroll) {
            tableScroll = rowIdx;
        } else if (rowIdx >= tableScroll + layout.table.visibleRows) {
            tableScroll = rowIdx - layout.table.visibleRows + 1;
        }
        ClampTableScroll();
    }

    int MaxBodyScroll() const {
        int excess = layout.bodyContentH - layout.bodyViewportH;
        return excess > 0 ? excess : 0;
    }

    void ClampBodyScroll() {
        bodyScrollY = kClampInt(bodyScrollY, 0, MaxBodyScroll());
    }

    void SwitchTab(TabId tab) {
        if (IsEditingCell()) CommitCellEdit();
        activeTab = tab;
        rows = BuildRowsForTab(tab);
        dragSliderRow = -1;
        bodyScrollY = 0;
        draggingBodyScroll = false;
        draggingScroll = false;
        hover = {};
        if (tab == TabId::FileClasses) {
            if (selectedClass >= ClassCount()) selectedClass = ClassCount() - 1;
            if (selectedClass < 0 && ClassCount() > 0) selectedClass = 0;
        }
        RebuildLayout();
    }

    void BeginCellEdit(int rowIdx, int col) {
        if (rowIdx < 0 || rowIdx >= ClassCount() || (col != 0 && col != 1)) return;
        if (IsEditingCell()) CommitCellEdit();
        selectedClass = rowIdx;
        EnsureRowVisible(rowIdx);
        editRow = rowIdx;
        editCol = col;
        const auto& item = draft.classes.items[static_cast<size_t>(rowIdx)];
        editOriginal = (col == 0) ? cfg_detail::GetClassName(item) : cfg_detail::GetClassExts(item);
        editBuffer = editOriginal;
        editCaret = editBuffer.size();
        caretVisible = true;
        if (hwnd) SetTimer(hwnd, kCaretTimerId, 530, nullptr);
        RebuildLayout();
    }

    void CommitCellEdit() {
        if (!IsEditingCell()) return;
        if (editRow >= 0 && editRow < ClassCount()) {
            auto& item = draft.classes.items[static_cast<size_t>(editRow)];
            if (editCol == 0) {
                cfg_detail::SetClassName(item, editBuffer.empty() ? editOriginal : editBuffer);
            } else {
                cfg_detail::SetClassExts(item, editBuffer);
            }
        }
        editRow = -1;
        editCol = -1;
        editBuffer.clear();
        editOriginal.clear();
        editCaret = 0;
        if (hwnd) KillTimer(hwnd, kCaretTimerId);
    }

    void CancelCellEdit() {
        if (!IsEditingCell()) return;
        editRow = -1;
        editCol = -1;
        editBuffer.clear();
        editOriginal.clear();
        editCaret = 0;
        if (hwnd) KillTimer(hwnd, kCaretTimerId);
    }

    void AddFileClass() {
        if (IsEditingCell()) CommitCellEdit();
        using ItemT = typename decltype(draft.classes.items)::value_type;
        ItemT item{};
        cfg_detail::SetClassName(item, ss::i18n::T(L"cfg.classes.new_name"));
        cfg_detail::SetClassExts(item, L"ext");
        cfg_detail::SetClassColor(item, pal.accent);
        draft.classes.items.push_back(std::move(item));
        selectedClass = ClassCount() - 1;
        RebuildLayout();
        EnsureRowVisible(selectedClass);
        RebuildLayout();
        BeginCellEdit(selectedClass, 0);
    }

    void DeleteSelectedFileClass() {
        if (IsEditingCell()) CancelCellEdit();
        if (selectedClass < 0 || selectedClass >= ClassCount()) return;
        draft.classes.items.erase(draft.classes.items.begin() + selectedClass);
        if (selectedClass >= ClassCount()) selectedClass = ClassCount() - 1;
        RebuildLayout();
        ClampTableScroll();
        RebuildLayout();
    }

    void RebuildLayout() {
        if (!hwnd) return;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        layout.clientRect = rc;

        const int navW = m.NavW();
        const int footerH = m.FooterH();
        const int upperBottom = kMaxInt(0, rc.bottom - footerH);

        layout.navRect = RECT{0, 0, navW, upperBottom};
        layout.contentRect = RECT{navW + 1, 0, rc.right, upperBottom};
        layout.footerRect = RECT{0, upperBottom + 1, rc.right, rc.bottom};

        // Page title + body inside content pane
        layout.pageTitleRect = RECT{
            layout.contentRect.left,
            layout.contentRect.top,
            layout.contentRect.right,
            layout.contentRect.top + m.PageHeaderH(),
        };
        layout.bodyRect = RECT{
            layout.contentRect.left,
            layout.pageTitleRect.bottom + 1,
            layout.contentRect.right,
            layout.contentRect.bottom,
        };

        // Footer buttons (right-aligned, 80x24, 6px gap)
        const int btnW = m.FooterBtnW();
        const int btnH = m.FooterBtnH();
        const int btnGap = m.FooterBtnGap();
        const int padX = m.PadX();
        const int btnY = layout.footerRect.top + (layout.footerRect.bottom - layout.footerRect.top - btnH) / 2;

        layout.cancelBtnRect = RECT{
            layout.footerRect.right - padX - btnW,
            btnY,
            layout.footerRect.right - padX,
            btnY + btnH,
        };
        layout.okBtnRect = RECT{
            layout.cancelBtnRect.left - btnGap - btnW,
            btnY,
            layout.cancelBtnRect.left - btnGap,
            btnY + btnH,
        };
        layout.pathTextRect = RECT{
            layout.footerRect.left + padX,
            layout.footerRect.top,
            kMaxInt(layout.footerRect.left + padX, layout.okBtnRect.left - m.InlineGap()),
            layout.footerRect.bottom,
        };

        // Setting rows layout (for tabs 0..3)
        rowGeoms.clear();
        layout.bodyHasScrollbar = false;
        layout.bodyContentH = 0;
        layout.bodyViewportH = kMaxInt(0, layout.bodyRect.bottom - layout.bodyRect.top);
        layout.bodyScrollTrackRect = RECT{};
        layout.bodyScrollThumbRect = RECT{};

        if (activeTab != TabId::FileClasses) {
            const int labelW = m.LabelW();
            const int gap = m.InlineGap();
            const int rowH = m.RowH();
            const int ctrlH = m.CtrlH();
            const int padTop = m.PadTop();
            const int padBottom = m.S(14); // P1: clear margin so last row is never occluded by footer divider

            // First pass: measure total content height to decide scrollbar & clamp scroll offset
            int contentH = padTop;
            {
                bool seenGroup = false;
                for (const auto& row : rows) {
                    if (row.kind == RowKind::GroupHeader) {
                        if (seenGroup) contentH += m.GroupGap();
                        seenGroup = true;
                        contentH += m.GroupHeaderH() + m.GroupHeaderBottomGap();
                    } else {
                        contentH += rowH;
                    }
                }
            }
            contentH += padBottom;

            layout.bodyContentH = contentH;
            layout.bodyHasScrollbar = (layout.bodyContentH > layout.bodyViewportH && layout.bodyViewportH > 0);
            ClampBodyScroll();

            int sbReserve = 0;
            if (layout.bodyHasScrollbar) {
                const int sbW = m.BodyScrollbarW();
                const int sbMargin = m.S(3);
                sbReserve = sbW + sbMargin + m.S(3);
                layout.bodyScrollTrackRect = RECT{
                    layout.bodyRect.right - sbMargin - sbW,
                    layout.bodyRect.top + m.S(4),
                    layout.bodyRect.right - sbMargin,
                    layout.bodyRect.bottom - m.S(4),
                };
                int trackH = kMaxInt(1, layout.bodyScrollTrackRect.bottom - layout.bodyScrollTrackRect.top);
                int minThumb = kMinInt(m.S(20), trackH);
                int thumbH = kClampInt(MulDiv(trackH, layout.bodyViewportH, kMaxInt(1, layout.bodyContentH)),
                                       minThumb, trackH);
                int maxScroll = kMaxInt(1, MaxBodyScroll());
                int thumbTop = layout.bodyScrollTrackRect.top + MulDiv(trackH - thumbH, bodyScrollY, maxScroll);
                layout.bodyScrollThumbRect = RECT{
                    layout.bodyScrollTrackRect.left,
                    thumbTop,
                    layout.bodyScrollTrackRect.right,
                    thumbTop + thumbH,
                };
            }

            const int innerL = layout.bodyRect.left + padX;
            const int innerR = kMaxInt(innerL, layout.bodyRect.right - padX - sbReserve);
            int y = layout.bodyRect.top + padTop - bodyScrollY;

            bool seenGroup = false;
            for (const auto& row : rows) {
                RowGeometry g{};
                if (row.kind == RowKind::GroupHeader) {
                    if (seenGroup) y += m.GroupGap();
                    seenGroup = true;
                    const int gh = m.GroupHeaderH();
                    g.rowRect = RECT{innerL, y, innerR, y + gh};
                    g.labelRect = g.rowRect;
                    y += gh + m.GroupHeaderBottomGap();
                } else {
                    g.rowRect = RECT{innerL, y, innerR, y + rowH};
                    g.labelRect = RECT{innerL, y, kMinInt(innerL + labelW, innerR), y + rowH};
                    const int ctrlX = innerL + labelW + gap;
                    const int ctrlTop = y + (rowH - ctrlH) / 2;

                    switch (row.kind) {
                        case RowKind::Checkbox: {
                            int sz = m.CheckboxSize();
                            int cy = y + (rowH - sz) / 2;
                            g.controlRect = RECT{ctrlX, cy, ctrlX + sz, cy + sz};
                            g.valueRect = RECT{g.controlRect.right + gap, y, innerR, y + rowH};
                            break;
                        }
                        case RowKind::Slider: {
                            int valW = m.SliderValueW();
                            int valL = kMaxInt(ctrlX + m.S(60), innerR - valW);
                            g.valueRect = RECT{valL, y, innerR, y + rowH};
                            g.controlRect = RECT{ctrlX, ctrlTop, kMaxInt(ctrlX + m.S(40), valL - gap), ctrlTop + ctrlH};
                            break;
                        }
                        case RowKind::ColorSwatch: {
                            int sz = m.SwatchSize();
                            int cy = y + (rowH - sz) / 2;
                            g.controlRect = RECT{ctrlX, cy, ctrlX + sz, cy + sz};
                            g.valueRect = RECT{g.controlRect.right + gap, y, innerR, y + rowH};
                            break;
                        }
                        case RowKind::Segmented: {
                            int count = (row.field == FieldId::Language) ? 3 : 2;
                            int sw = (row.field == FieldId::Language) ? m.SegLangItemW() : m.SegItemW();
                            int sh = m.SegItemH();
                            int sy = y + (rowH - sh) / 2;
                            g.segCount = count;
                            for (int s = 0; s < count; ++s) {
                                int sx = ctrlX + s * (sw - 1);
                                g.segRects[s] = RECT{sx, sy, sx + sw, sy + sh};
                            }
                            g.controlRect = RECT{g.segRects[0].left, sy, g.segRects[count - 1].right, sy + sh};
                            break;
                        }
                        case RowKind::GroupHeader:
                            break;
                    }
                    y += rowH;
                }
                rowGeoms.push_back(g);
            }
        } else {
            // File classes table layout
            TableLayout& tl = layout.table;
            const int innerL = layout.bodyRect.left + padX;
            const int innerR = layout.bodyRect.right - padX;
            const int topY = layout.bodyRect.top + m.PadTop();

            tl.toolbarRect = RECT{innerL, topY, innerR, topY + m.ToolbarH()};
            const int tBtnW = m.ToolBtnW();
            const int tBtnH = m.ToolBtnH();
            const int tBtnY = tl.toolbarRect.top + (m.ToolbarH() - tBtnH) / 2;

            tl.btnAddRect = RECT{innerL, tBtnY, innerL + tBtnW, tBtnY + tBtnH};
            tl.btnDelRect = RECT{tl.btnAddRect.right + m.FooterBtnGap(), tBtnY,
                                 tl.btnAddRect.right + m.FooterBtnGap() + tBtnW, tBtnY + tBtnH};
            tl.hintRect = RECT{tl.btnDelRect.right + m.InlineGap(), tl.toolbarRect.top,
                               innerR, tl.toolbarRect.bottom};

            const int frameTop = tl.toolbarRect.bottom + m.S(4);
            // P1: clamp table frame strictly inside bodyRect so the last row is never occluded by footer.
            const int maxFrameBottom = kMaxInt(frameTop, layout.bodyRect.bottom - m.S(6));
            const int frameBottom = kMinInt(maxFrameBottom,
                                            kMaxInt(frameTop + m.TableHeaderH() + m.TableRowH(),
                                                    layout.bodyRect.bottom - m.S(10)));
            tl.frameRect = RECT{innerL, frameTop, innerR, frameBottom};

            const int innerLeft = tl.frameRect.left + 1;
            const int innerRight = tl.frameRect.right - 1;
            const int headerH = m.TableHeaderH();
            const int rowH = m.TableRowH();

            tl.headerRect = RECT{innerLeft, tl.frameRect.top + 1, innerRight, tl.frameRect.top + 1 + headerH};
            tl.bodyRect = RECT{innerLeft, tl.headerRect.bottom + 1, innerRight, tl.frameRect.bottom - 1};

            int bodyH = kMaxInt(0, tl.bodyRect.bottom - tl.bodyRect.top);
            tl.visibleRows = kMaxInt(1, bodyH / rowH);
            ClampTableScroll();

            const int totalItems = ClassCount();
            tl.hasScrollbar = totalItems > tl.visibleRows;
            const int sbW = tl.hasScrollbar ? m.ScrollbarW() : 0;

            const int usableW = kMaxInt(0, (innerRight - innerLeft) - sbW);
            tl.colNameW = kMinInt(m.TableNameColW(), usableW / 2);
            tl.colColorW = kMinInt(m.TableColorColW(), kMaxInt(0, usableW - tl.colNameW));
            tl.colExtW = kMaxInt(0, usableW - tl.colNameW - tl.colColorW);

            tl.colNameX = innerLeft;
            tl.colExtX = tl.colNameX + tl.colNameW;
            tl.colColorX = tl.colExtX + tl.colExtW;

            if (tl.hasScrollbar) {
                tl.scrollTrackRect = RECT{innerRight - sbW, tl.bodyRect.top, innerRight, tl.bodyRect.bottom};
                int trackH = kMaxInt(1, tl.scrollTrackRect.bottom - tl.scrollTrackRect.top);
                int minThumb = kMinInt(m.S(18), trackH);
                int thumbH = kClampInt(MulDiv(trackH, tl.visibleRows, totalItems), minThumb, trackH);
                int maxScroll = kMaxInt(1, totalItems - tl.visibleRows);
                int thumbTop = tl.scrollTrackRect.top + MulDiv(trackH - thumbH, tableScroll, maxScroll);
                tl.scrollThumbRect = RECT{tl.scrollTrackRect.left + 1, thumbTop,
                                          tl.scrollTrackRect.right - 1, thumbTop + thumbH};
            } else {
                tl.scrollTrackRect = RECT{};
                tl.scrollThumbRect = RECT{};
            }
        }
    }

    RECT NavItemRect(int idx) const {
        const int top = layout.navRect.top + m.PadTop() + idx * m.NavItemH();
        return RECT{layout.navRect.left, top, layout.navRect.right, top + m.NavItemH()};
    }

    HitTarget HitTest(int x, int y) const {
        POINT pt{x, y};

        // Footer buttons
        if (PtInRect(&layout.okBtnRect, pt)) return {HitKind::FooterOk, 0, 0};
        if (PtInRect(&layout.cancelBtnRect, pt)) return {HitKind::FooterCancel, 0, 0};

        // Left navigation items
        if (PtInRect(&layout.navRect, pt)) {
            for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
                RECT nr = NavItemRect(i);
                if (PtInRect(&nr, pt)) return {HitKind::NavItem, i, 0};
            }
            return {};
        }

        // Content area
        if (activeTab != TabId::FileClasses) {
            if (!PtInRect(&layout.bodyRect, pt)) return {};
            if (layout.bodyHasScrollbar) {
                RECT thumbHit = layout.bodyScrollThumbRect;
                RECT trackHit = layout.bodyScrollTrackRect;
                thumbHit.left -= m.S(2);
                thumbHit.right += m.S(2);
                trackHit.left -= m.S(2);
                trackHit.right += m.S(2);
                if (PtInRect(&thumbHit, pt)) return {HitKind::BodyScrollThumb, 0, 0};
                if (PtInRect(&trackHit, pt)) return {HitKind::BodyScrollTrack, 0, 0};
            }
            const int rowCount = kMinInt(static_cast<int>(rows.size()), static_cast<int>(rowGeoms.size()));
            for (int i = 0; i < rowCount; ++i) {
                const auto& row = rows[static_cast<size_t>(i)];
                const auto& g = rowGeoms[static_cast<size_t>(i)];
                if (row.kind == RowKind::GroupHeader) continue;
                if (g.rowRect.bottom <= layout.bodyRect.top || g.rowRect.top >= layout.bodyRect.bottom) continue;

                if (row.kind == RowKind::Segmented) {
                    for (int s = 0; s < g.segCount && s < 3; ++s) {
                        if (PtInRect(&g.segRects[s], pt)) return {HitKind::RowSegButton, i, s};
                    }
                } else if (row.kind == RowKind::Checkbox) {
                    RECT hitBox{g.rowRect.left, g.rowRect.top, g.rowRect.right, g.rowRect.bottom};
                    if (PtInRect(&hitBox, pt)) return {HitKind::RowControl, i, 0};
                } else if (row.kind == RowKind::ColorSwatch) {
                    RECT hitBox = g.controlRect;
                    hitBox.right = kMinInt(g.rowRect.right, g.controlRect.right + m.S(84));
                    if (PtInRect(&hitBox, pt)) return {HitKind::RowControl, i, 0};
                } else if (row.kind == RowKind::Slider) {
                    if (PtInRect(&g.controlRect, pt)) return {HitKind::RowControl, i, 0};
                }
            }
        } else {
            const auto& tl = layout.table;
            if (PtInRect(&tl.btnAddRect, pt)) return {HitKind::TableAdd, 0, 0};
            if (PtInRect(&tl.btnDelRect, pt)) return {HitKind::TableDelete, 0, 0};
            if (tl.hasScrollbar) {
                if (PtInRect(&tl.scrollThumbRect, pt)) return {HitKind::TableScrollThumb, 0, 0};
                if (PtInRect(&tl.scrollTrackRect, pt)) return {HitKind::TableScrollTrack, 0, 0};
            }
            if (PtInRect(&tl.bodyRect, pt)) {
                int rowH = kMaxInt(1, m.TableRowH());
                int visIdx = (y - tl.bodyRect.top) / rowH;
                int rowIdx = tableScroll + visIdx;
                if (visIdx >= 0 && visIdx < tl.visibleRows && rowIdx >= 0 && rowIdx < ClassCount()) {
                    RECT sw = tl.SwatchRect(visIdx, rowH, m.TableSwatchSize());
                    if (PtInRect(&sw, pt)) {
                        return {HitKind::TableSwatch, rowIdx, 2};
                    }
                    int col = (x < tl.colExtX) ? 0 : ((x < tl.colColorX) ? 1 : 2);
                    return {HitKind::TableRow, rowIdx, col};
                }
            }
        }
        return {};
    }

    bool UpdateSliderFromX(int rowIdx, int x) {
        if (rowIdx < 0 || rowIdx >= static_cast<int>(rows.size()) ||
            rowIdx >= static_cast<int>(rowGeoms.size())) return false;
        const auto& row = rows[static_cast<size_t>(rowIdx)];
        const auto& g = rowGeoms[static_cast<size_t>(rowIdx)];
        if (row.kind != RowKind::Slider) return false;

        int halfThumb = m.SliderThumbSize() / 2;
        int trackL = g.controlRect.left + halfThumb;
        int trackR = g.controlRect.right - halfThumb;
        if (trackR <= trackL) return false;

        int clampedX = kClampInt(x, trackL, trackR);
        double ratio = static_cast<double>(clampedX - trackL) / static_cast<double>(trackR - trackL);
        int range = row.maxVal - row.minVal;
        int newVal = row.minVal + static_cast<int>(ratio * range + (ratio >= 0.0 ? 0.5 : -0.5));
        newVal = kClampInt(newVal, row.minVal, row.maxVal);

        int oldVal = GetIntField(draft, row.field);
        if (newVal == oldVal) return false;
        SetIntField(draft, row.field, newVal);
        return true;
    }

    bool PickColor(COLORREF initial, COLORREF& outColor) {
        if (GetCapture() == hwnd) ReleaseCapture();
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = hwnd;
        cc.rgbResult = initial;
        cc.lpCustColors = customColors.data();
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            outColor = cc.rgbResult;
            return true;
        }
        return false;
    }
};

// ============================================================================
// LAYER 3: Rendering (Double-Buffered GDI + Targeted GDI+ Anti-Aliasing)
// ============================================================================

inline Gdiplus::Color ToGpColor(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

void FillSolidRect(HDC hdc, const RECT& rc, COLORREF color) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    ScopedBrush br(CreateSolidBrush(color));
    FillRect(hdc, &rc, br);
}

void DrawRectBorder1px(HDC hdc, const RECT& rc, COLORREF color) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    ScopedPen pen(CreatePen(PS_SOLID, 1, color));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
}

void DrawLine1px(HDC hdc, int x1, int y1, int x2, int y2, COLORREF color) {
    ScopedPen pen(CreatePen(PS_SOLID, 1, color));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, oldPen);
}

void DrawTextClipped(HDC hdc, HFONT font, COLORREF color, const wchar_t* text, int len,
                     const RECT& rc, UINT flags) {
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetTextColor(hdc, color);
    RECT copy = rc;
    DrawTextW(hdc, text, len, &copy, flags);
    SelectObject(hdc, oldFont);
}

void DrawCheckboxControl(HDC hdc, Gdiplus::Graphics& g, const DialogState& st,
                         const RECT& box, bool checked, bool hovered) {
    const auto& pal = st.pal;
    if (checked) {
        FillSolidRect(hdc, box, hovered ? pal.accentHover : pal.accent);
        DrawRectBorder1px(hdc, box, hovered ? pal.textHover : pal.accent);

        // Anti-aliased 2px white checkmark (diagonal strokes via GDI+)
        const float x = static_cast<float>(box.left);
        const float y = static_cast<float>(box.top);
        const float w = static_cast<float>(box.right - box.left);
        const float h = static_cast<float>(box.bottom - box.top);

        Gdiplus::PointF pts[3] = {
            {x + w * 0.22f, y + h * 0.52f},
            {x + w * 0.42f, y + h * 0.72f},
            {x + w * 0.78f, y + h * 0.28f},
        };
        float strokeW = (st.m.Sf(2.0f) < 1.8f) ? 1.8f : st.m.Sf(2.0f);
        Gdiplus::Pen pen(ToGpColor(pal.accentText), strokeW);
        pen.SetStartCap(Gdiplus::LineCapSquare);
        pen.SetEndCap(Gdiplus::LineCapSquare);
        pen.SetLineJoin(Gdiplus::LineJoinMiter);

        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.DrawLines(&pen, pts, 3);
    } else {
        FillSolidRect(hdc, box, pal.input);
        DrawRectBorder1px(hdc, box, hovered ? pal.accent : pal.border);
    }
}

void DrawSliderControl(Gdiplus::Graphics& g, const DialogState& st,
                       const RECT& rc, int val, int minVal, int maxVal,
                       bool hovered, bool dragging) {
    const auto& pal = st.pal;
    const int thumbSz = st.m.SliderThumbSize();
    const int halfThumb = thumbSz / 2;
    const int trackH = st.m.SliderTrackH();

    const float trackL = static_cast<float>(rc.left + halfThumb);
    const float trackR = static_cast<float>(rc.right - halfThumb);
    const float cy = static_cast<float>(rc.top + rc.bottom) * 0.5f;
    const float trackTop = cy - static_cast<float>(trackH) * 0.5f;

    float ratio = 0.0f;
    if (maxVal > minVal) {
        ratio = static_cast<float>(kClampInt(val, minVal, maxVal) - minVal) /
                static_cast<float>(maxVal - minVal);
    }
    const float thumbCx = trackL + ratio * (trackR - trackL);

    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    // Unfilled track (sharp square ends, #3A3A42 in dark mode)
    Gdiplus::SolidBrush unfilledBr(ToGpColor(pal.border));
    g.FillRectangle(&unfilledBr, trackL, trackTop, trackR - trackL, static_cast<float>(trackH));

    // Filled track (sharp square ends, accent)
    float fillW = thumbCx - trackL;
    if (fillW > 0.0f) {
        Gdiplus::SolidBrush filledBr(ToGpColor(pal.accent));
        g.FillRectangle(&filledBr, trackL, trackTop, fillW, static_cast<float>(trackH));
    }

    // 10px square thumb (accent fill + 1px border)
    const float tx = thumbCx - static_cast<float>(thumbSz) * 0.5f;
    const float ty = cy - static_cast<float>(thumbSz) * 0.5f;
    Gdiplus::SolidBrush thumbBr(ToGpColor((hovered || dragging) ? pal.accentHover : pal.accent));
    g.FillRectangle(&thumbBr, tx, ty, static_cast<float>(thumbSz), static_cast<float>(thumbSz));

    Gdiplus::Pen thumbPen(ToGpColor((hovered || dragging) ? pal.thumbBorderActive : pal.thumbBorder), 1.0f);
    g.DrawRectangle(&thumbPen, tx + 0.5f, ty + 0.5f,
                    static_cast<float>(thumbSz) - 1.0f, static_cast<float>(thumbSz) - 1.0f);
}

void DrawButtonFlat(HDC hdc, const DialogState& st, const RECT& rc, const wchar_t* label,
                    bool primary, bool hovered, bool enabled = true) {
    const auto& pal = st.pal;
    COLORREF bg = pal.input;
    COLORREF bd = pal.border;
    COLORREF fg = pal.text;

    if (!enabled) {
        bg = pal.panel;
        bd = pal.border;
        fg = pal.textMuted;
    } else if (primary) {
        bg = hovered ? pal.accentHover : pal.accent;
        bd = hovered ? pal.textHover : pal.accent;
        fg = pal.accentText;
    } else {
        bg = pal.panel;
        bd = hovered ? pal.accent : pal.border;
        fg = hovered ? pal.textHover : pal.text;
    }

    FillSolidRect(hdc, rc, bg);
    DrawRectBorder1px(hdc, rc, bd);
    DrawTextClipped(hdc, st.fonts.ui, fg, label, -1, rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawSidebar(HDC hdc, const DialogState& st) {
    const auto& pal = st.pal;
    const auto& nav = st.layout.navRect;

    FillSolidRect(hdc, nav, pal.panel);
    DrawLine1px(hdc, nav.right, nav.top, nav.right, nav.bottom, pal.border);

    for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
        RECT itemRc = st.NavItemRect(i);
        bool active = (static_cast<int>(st.activeTab) == i);
        bool hovered = (st.hover.kind == HitKind::NavItem && st.hover.index == i);

        if (active) {
            FillSolidRect(hdc, itemRc, pal.bg);
            RECT indicator{itemRc.left, itemRc.top, itemRc.left + st.m.S(2), itemRc.bottom};
            FillSolidRect(hdc, indicator, pal.accent);
        }

        COLORREF fg = active ? pal.text : (hovered ? pal.textHover : pal.textMuted);
        RECT textRc{itemRc.left + st.m.PadX(), itemRc.top, itemRc.right - st.m.S(8), itemRc.bottom};
        DrawTextClipped(hdc, st.fonts.ui, fg, ss::i18n::T(kTabNameKeys[i]), -1, textRc,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void DrawSettingsBody(HDC hdc, Gdiplus::Graphics& g, const DialogState& st) {
    const auto& pal = st.pal;
    const RECT& bodyRc = st.layout.bodyRect;
    if (bodyRc.right <= bodyRc.left || bodyRc.bottom <= bodyRc.top) return;

    int savedDc = SaveDC(hdc);
    IntersectClipRect(hdc, bodyRc.left, bodyRc.top, bodyRc.right, bodyRc.bottom);
    Gdiplus::GraphicsState gpState = g.Save();
    g.SetClip(Gdiplus::Rect(bodyRc.left, bodyRc.top,
                            bodyRc.right - bodyRc.left, bodyRc.bottom - bodyRc.top));

    const int rowCount = kMinInt(static_cast<int>(st.rows.size()), static_cast<int>(st.rowGeoms.size()));
    for (int i = 0; i < rowCount; ++i) {
        const auto& row = st.rows[static_cast<size_t>(i)];
        const auto& rg = st.rowGeoms[static_cast<size_t>(i)];
        if (rg.rowRect.bottom <= bodyRc.top || rg.rowRect.top >= bodyRc.bottom) continue;

        if (row.kind == RowKind::GroupHeader) {
            DrawTextClipped(hdc, st.fonts.group, pal.textMuted, row.label, -1, rg.labelRect,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawLine1px(hdc, rg.rowRect.left, rg.rowRect.bottom - 1,
                        rg.rowRect.right, rg.rowRect.bottom - 1, pal.border);
            continue;
        }

        const bool ctrlHover = (st.hover.kind == HitKind::RowControl && st.hover.index == i);
        const bool isDragging = (st.dragSliderRow == i);

        // Label column (never washes row white)
        COLORREF labelColor = (ctrlHover || isDragging) ? pal.textHover : pal.text;
        DrawTextClipped(hdc, st.fonts.ui, labelColor, row.label, -1, rg.labelRect,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        switch (row.kind) {
            case RowKind::Checkbox: {
                bool val = GetBoolField(st.draft, row.field);
                DrawCheckboxControl(hdc, g, st, rg.controlRect, val, ctrlHover);
                const wchar_t* stateStr = ss::i18n::T(val ? L"cfg.enabled" : L"cfg.disabled");
                COLORREF valColor = ctrlHover ? pal.text : pal.textMuted;
                DrawTextClipped(hdc, st.fonts.small, valColor, stateStr, -1, rg.valueRect,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (row.field == FieldId::ShellIntegration) {
                    const auto& sh = GetShellIntegrationStatus();
                    std::wstring badge;
                    COLORREF badgeColor = pal.textMuted;
                    // P1: only display the failure badge while the checkbox matches the failed action.
                    if (sh.applied && !sh.ok && (val == sh.enabled)) {
                        badge = std::wstring(ss::i18n::T(L"cfg.shell_reg_failed_prefix")) +
                                std::to_wstring(sh.errorCode) +
                                ss::i18n::T(L"cfg.shell_reg_failed_suffix");
                        badgeColor = st.darkMode ? RGB(0xF8, 0x71, 0x71) : RGB(0xD1, 0x24, 0x2F);
                    } else if (val && ((sh.applied && sh.enabled && sh.ok) || st.shellRegistered)) {
                        badge = ss::i18n::T(L"cfg.shell_registered");
                        badgeColor = st.darkMode ? RGB(0x4A, 0xDE, 0x80) : RGB(0x1A, 0x7F, 0x37);
                    }
                    if (!badge.empty()) {
                        SIZE stateSz{};
                        HGDIOBJ oldF = SelectObject(hdc, st.fonts.small);
                        GetTextExtentPoint32W(hdc, stateStr, static_cast<int>(std::wcslen(stateStr)), &stateSz);
                        SelectObject(hdc, oldF);
                        RECT badgeRc{
                            rg.valueRect.left + stateSz.cx + st.m.S(12),
                            rg.valueRect.top,
                            rg.valueRect.right,
                            rg.valueRect.bottom,
                        };
                        if (badgeRc.right > badgeRc.left) {
                            DrawTextClipped(hdc, st.fonts.small, badgeColor, badge.c_str(),
                                            static_cast<int>(badge.size()), badgeRc,
                                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        }
                    }
                }
                break;
            }

            case RowKind::Slider: {
                int val = GetIntField(st.draft, row.field);
                DrawSliderControl(g, st, rg.controlRect, val, row.minVal, row.maxVal, ctrlHover, isDragging);

                wchar_t numBuf[32]{};
                if (row.showSign && val > 0) {
                    std::swprintf(numBuf, 32, L"+%d%ls", val, row.suffix);
                } else {
                    std::swprintf(numBuf, 32, L"%d%ls", val, row.suffix);
                }
                COLORREF valColor = (ctrlHover || isDragging) ? pal.textHover : pal.textMuted;
                DrawTextClipped(hdc, st.fonts.small, valColor, numBuf, -1, rg.valueRect,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }

            case RowKind::ColorSwatch: {
                COLORREF c = GetColorField(st.draft, row.field);
                FillSolidRect(hdc, rg.controlRect, c);
                DrawRectBorder1px(hdc, rg.controlRect, ctrlHover ? pal.accent : pal.border);

                wchar_t hexBuf[16]{};
                std::swprintf(hexBuf, 16, L"#%02X%02X%02X",
                              static_cast<unsigned>(GetRValue(c)),
                              static_cast<unsigned>(GetGValue(c)),
                              static_cast<unsigned>(GetBValue(c)));
                COLORREF valColor = ctrlHover ? pal.textHover : pal.textMuted;
                DrawTextClipped(hdc, st.fonts.small, valColor, hexBuf, -1, rg.valueRect,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }

            case RowKind::Segmented: {
                using ss::i18n::T;
                const wchar_t* labels[3]{};
                int count = 2;
                int activeIdx = 0;
                if (row.field == FieldId::Language) {
                    count = 3;
                    activeIdx = cfg_detail::ReadLanguageIndex(st.draft.language);
                    labels[0] = T(L"cfg.lang.auto");
                    labels[1] = T(L"cfg.lang.zh");
                    labels[2] = T(L"cfg.lang.en");
                } else {
                    count = 2;
                    activeIdx = cfg_detail::ReadStyleIndex(st.draft.initialStyle);
                    labels[0] = T(L"cfg.style.flat");
                    labels[1] = T(L"cfg.style.classes");
                }
                for (int s = 0; s < count && s < rg.segCount; ++s) {
                    bool sel = (activeIdx == s);
                    bool segHover = (st.hover.kind == HitKind::RowSegButton &&
                                     st.hover.index == i && st.hover.subIndex == s);
                    COLORREF bg = sel ? pal.accent : pal.input;
                    COLORREF bd = sel ? pal.accent : (segHover ? pal.accent : pal.border);
                    COLORREF fg = sel ? pal.accentText : (segHover ? pal.textHover : pal.text);
                    FillSolidRect(hdc, rg.segRects[s], bg);
                    DrawRectBorder1px(hdc, rg.segRects[s], bd);
                    DrawTextClipped(hdc, st.fonts.small, fg, labels[s], -1, rg.segRects[s],
                                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                // Redraw selected segment border on top of shared center seam
                if (activeIdx >= 0 && activeIdx < rg.segCount) {
                    DrawRectBorder1px(hdc, rg.segRects[activeIdx], pal.accent);
                }
                break;
            }

            case RowKind::GroupHeader:
                break;
        }

        // 1px divider between setting rows (0px row gap)
        DrawLine1px(hdc, rg.rowRect.left, rg.rowRect.bottom - 1,
                    rg.rowRect.right, rg.rowRect.bottom - 1, pal.border);
    }

    // Thin 6px dark scrollbar without arrows on the right edge of the body
    if (st.layout.bodyHasScrollbar) {
        COLORREF trackCol = st.darkMode ? RGB(0x22, 0x22, 0x26) : RGB(0xE8, 0xEA, 0xEE);
        bool thumbHot = st.draggingBodyScroll || st.hover.kind == HitKind::BodyScrollThumb;
        COLORREF thumbCol = thumbHot
            ? pal.borderHover
            : (st.darkMode ? RGB(0x4C, 0x4C, 0x56) : RGB(0xB4, 0xBA, 0xC4));
        FillSolidRect(hdc, st.layout.bodyScrollTrackRect, trackCol);
        FillSolidRect(hdc, st.layout.bodyScrollThumbRect, thumbCol);
    }

    g.Restore(gpState);
    if (savedDc) RestoreDC(hdc, savedDc);
}

void DrawInlineEditorCell(HDC hdc, const DialogState& st, const RECT& cellRc) {
    const auto& pal = st.pal;
    FillSolidRect(hdc, cellRc, pal.input);
    DrawRectBorder1px(hdc, cellRc, pal.accent);

    RECT inner{cellRc.left + st.m.S(5), cellRc.top + 1, cellRc.right - st.m.S(5), cellRc.bottom - 1};
    if (inner.right <= inner.left) return;

    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, inner.left, inner.top, inner.right, inner.bottom);

    HGDIOBJ oldFont = SelectObject(hdc, st.fonts.ui);
    SetTextColor(hdc, pal.text);

    SIZE caretExt{};
    if (st.editCaret > 0) {
        size_t prefixLen = st.editCaret < st.editBuffer.size() ? st.editCaret : st.editBuffer.size();
        GetTextExtentPoint32W(hdc, st.editBuffer.data(), static_cast<int>(prefixLen), &caretExt);
    }
    int availW = inner.right - inner.left;
    int scrollX = 0;
    if (caretExt.cx > availW - 2) {
        scrollX = caretExt.cx - (availW - 2);
    }

    RECT textRc{inner.left - scrollX, inner.top, inner.right + scrollX + st.m.S(200), inner.bottom};
    DrawTextW(hdc, st.editBuffer.c_str(), static_cast<int>(st.editBuffer.size()),
              &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (st.caretVisible) {
        int cx = inner.left + caretExt.cx - scrollX;
        int padY = st.m.S(3);
        DrawLine1px(hdc, cx, inner.top + padY, cx, inner.bottom - padY, pal.text);
    }

    SelectObject(hdc, oldFont);
    if (saved) RestoreDC(hdc, saved);
}

void DrawFileClassesBody(HDC hdc, const DialogState& st) {
    const auto& pal = st.pal;
    const auto& tl = st.layout.table;
    const RECT& bodyRc = st.layout.bodyRect;
    if (bodyRc.right <= bodyRc.left || bodyRc.bottom <= bodyRc.top) return;

    // P1: clip entire FileClasses body to bodyRect so nothing can bleed over the footer bar.
    int savedBody = SaveDC(hdc);
    IntersectClipRect(hdc, bodyRc.left, bodyRc.top, bodyRc.right, bodyRc.bottom);

    // Toolbar
    using ss::i18n::T;
    DrawButtonFlat(hdc, st, tl.btnAddRect, T(L"cfg.btn.add"), false,
                   st.hover.kind == HitKind::TableAdd, true);
    DrawButtonFlat(hdc, st, tl.btnDelRect, T(L"cfg.btn.delete"), false,
                   st.hover.kind == HitKind::TableDelete, st.selectedClass >= 0);
    DrawTextClipped(hdc, st.fonts.small, pal.textMuted, T(L"cfg.classes.hint"), -1,
                    tl.hintRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Table frame
    FillSolidRect(hdc, tl.frameRect, pal.bg);
    DrawRectBorder1px(hdc, tl.frameRect, pal.border);

    // Table header (9pt semibold + 1px bottom border)
    FillSolidRect(hdc, tl.headerRect, pal.panel);
    DrawLine1px(hdc, tl.headerRect.left, tl.headerRect.bottom,
                tl.headerRect.right, tl.headerRect.bottom, pal.border);

    const int cellPad = st.m.S(6);
    RECT hName{tl.colNameX + cellPad, tl.headerRect.top, tl.colNameX + tl.colNameW - cellPad, tl.headerRect.bottom};
    RECT hExt{tl.colExtX + cellPad, tl.headerRect.top, tl.colExtX + tl.colExtW - cellPad, tl.headerRect.bottom};
    RECT hColor{tl.colColorX, tl.headerRect.top, tl.colColorX + tl.colColorW, tl.headerRect.bottom};

    DrawTextClipped(hdc, st.fonts.tableHeader, pal.textMuted, T(L"cfg.classes.col.name"), -1, hName,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextClipped(hdc, st.fonts.tableHeader, pal.textMuted, T(L"cfg.classes.col.ext"), -1, hExt,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextClipped(hdc, st.fonts.tableHeader, pal.textMuted, T(L"cfg.classes.col.color"), -1, hColor,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Column vertical hairlines in header
    DrawLine1px(hdc, tl.colExtX, tl.headerRect.top, tl.colExtX, tl.headerRect.bottom, pal.border);
    DrawLine1px(hdc, tl.colColorX, tl.headerRect.top, tl.colColorX, tl.headerRect.bottom, pal.border);

    // Table rows (22px height, no zebra striping, 1px separators, #22344E selected)
    const int rowH = st.m.TableRowH();
    const int count = st.ClassCount();

    if (count == 0) {
        DrawTextClipped(hdc, st.fonts.small, pal.textMuted, T(L"cfg.classes.empty"), -1,
                        tl.bodyRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        int saved = SaveDC(hdc);
        IntersectClipRect(hdc, tl.bodyRect.left, tl.bodyRect.top,
                          tl.colColorX + tl.colColorW, tl.bodyRect.bottom);

        for (int vis = 0; vis < tl.visibleRows; ++vis) {
            int idx = st.tableScroll + vis;
            if (idx >= count) break;

            const auto& item = st.draft.classes.items[static_cast<size_t>(idx)];
            RECT rowRc = tl.RowRect(vis, rowH);
            bool selected = (idx == st.selectedClass);
            bool rowHovered = (st.hover.kind == HitKind::TableRow && st.hover.index == idx);

            if (selected) {
                FillSolidRect(hdc, rowRc, pal.rowSelected);
            }

            // Cell 0: Name
            RECT nameCell = tl.CellRect(vis, 0, rowH);
            if (st.editRow == idx && st.editCol == 0) {
                DrawInlineEditorCell(hdc, st, nameCell);
            } else {
                std::wstring name = cfg_detail::GetClassName(item);
                RECT textRc{nameCell.left + cellPad, nameCell.top, nameCell.right - cellPad, nameCell.bottom};
                COLORREF fg = (rowHovered && st.hover.subIndex == 0) ? pal.textHover : pal.text;
                DrawTextClipped(hdc, st.fonts.ui, fg, name.c_str(), static_cast<int>(name.size()), textRc,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Cell 1: Extensions
            RECT extCell = tl.CellRect(vis, 1, rowH);
            if (st.editRow == idx && st.editCol == 1) {
                DrawInlineEditorCell(hdc, st, extCell);
            } else {
                std::wstring exts = cfg_detail::GetClassExts(item);
                RECT textRc{extCell.left + cellPad, extCell.top, extCell.right - cellPad, extCell.bottom};
                COLORREF fg = (rowHovered && st.hover.subIndex == 1)
                                  ? pal.textHover
                                  : (selected ? pal.text : pal.textMuted);
                DrawTextClipped(hdc, st.fonts.small, fg, exts.c_str(), static_cast<int>(exts.size()), textRc,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Cell 2: Color swatch
            RECT swRc = tl.SwatchRect(vis, rowH, st.m.TableSwatchSize());
            bool swHover = (st.hover.kind == HitKind::TableSwatch && st.hover.index == idx);
            FillSolidRect(hdc, swRc, cfg_detail::GetClassColor(item));
            DrawRectBorder1px(hdc, swRc, swHover ? pal.accent : pal.border);

            // Row bottom 1px divider
            DrawLine1px(hdc, rowRc.left, rowRc.bottom - 1, rowRc.right, rowRc.bottom - 1, pal.border);
        }

        // Subtle column separators across body
        DrawLine1px(hdc, tl.colExtX, tl.bodyRect.top, tl.colExtX, tl.bodyRect.bottom, pal.border);
        DrawLine1px(hdc, tl.colColorX, tl.bodyRect.top, tl.colColorX, tl.bodyRect.bottom, pal.border);

        if (saved) RestoreDC(hdc, saved);
    }

    // Compact scrollbar
    if (tl.hasScrollbar) {
        FillSolidRect(hdc, tl.scrollTrackRect, pal.panel);
        DrawLine1px(hdc, tl.scrollTrackRect.left, tl.scrollTrackRect.top,
                    tl.scrollTrackRect.left, tl.scrollTrackRect.bottom, pal.border);
        bool thumbHot = st.draggingScroll || st.hover.kind == HitKind::TableScrollThumb;
        FillSolidRect(hdc, tl.scrollThumbRect, thumbHot ? pal.borderHover : pal.border);
    }

    if (savedBody) RestoreDC(hdc, savedBody);
}

void DrawFooter(HDC hdc, const DialogState& st) {
    const auto& pal = st.pal;
    const auto& f = st.layout.footerRect;

    FillSolidRect(hdc, f, pal.panel);
    DrawLine1px(hdc, f.left, f.top - 1, f.right, f.top - 1, pal.border);

    DrawTextClipped(hdc, st.fonts.small, pal.textMuted,
                    st.configPath.c_str(), static_cast<int>(st.configPath.size()),
                    st.layout.pathTextRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    DrawButtonFlat(hdc, st, st.layout.okBtnRect, ss::i18n::T(L"cfg.btn.ok"), true,
                   st.hover.kind == HitKind::FooterOk, true);
    DrawButtonFlat(hdc, st, st.layout.cancelBtnRect, ss::i18n::T(L"cfg.btn.cancel"), false,
                   st.hover.kind == HitKind::FooterCancel, true);
}

void PaintDialog(HWND hwnd, const DialogState& st) {
    PAINTSTRUCT ps{};
    HDC paintDC = BeginPaint(hwnd, &ps);
    if (!paintDC) return;

    const int cw = st.layout.clientRect.right - st.layout.clientRect.left;
    const int ch = st.layout.clientRect.bottom - st.layout.clientRect.top;
    if (cw <= 0 || ch <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    ScopedDC memDC(CreateCompatibleDC(paintDC));
    ScopedBitmap bmp(CreateCompatibleBitmap(paintDC, cw, ch));
    if (!memDC || !bmp) {
        EndPaint(hwnd, &ps);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memDC, bmp);

    SetBkMode(memDC, TRANSPARENT);

    // Base fill
    FillSolidRect(memDC, st.layout.clientRect, st.pal.bg);

    // Sidebar
    DrawSidebar(memDC, st);

    // Page title bar (12pt semibold + 1px bottom border)
    RECT titleTextRc{
        st.layout.pageTitleRect.left + st.m.PadX(),
        st.layout.pageTitleRect.top,
        st.layout.pageTitleRect.right - st.m.PadX(),
        st.layout.pageTitleRect.bottom,
    };
    DrawTextClipped(memDC, st.fonts.title, st.pal.text,
                    ss::i18n::T(kTabTitleKeys[static_cast<int>(st.activeTab)]), -1,
                    titleTextRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawLine1px(memDC, st.layout.pageTitleRect.left, st.layout.pageTitleRect.bottom,
                st.layout.pageTitleRect.right, st.layout.pageTitleRect.bottom, st.pal.border);

    // Content body (scoped Gdiplus::Graphics destroyed before Footer / BitBlt / DeleteDC)
    {
        Gdiplus::Graphics gp(memDC);
        if (st.activeTab != TabId::FileClasses) {
            DrawSettingsBody(memDC, gp, st);
        } else {
            DrawFileClassesBody(memDC, st);
        }
    }

    // Footer drawn after body so nothing can ever overlap the footer bar
    DrawFooter(memDC, st);

    BitBlt(paintDC, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    EndPaint(hwnd, &ps);
}

// ============================================================================
// Window Procedure & Input Handling
// ============================================================================

void PasteClipboardIntoEditor(DialogState& st) {
    if (!st.IsEditingCell()) return;
    if (!OpenClipboard(st.hwnd)) return;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(hData));
        if (text) {
            std::wstring clean;
            for (const wchar_t* p = text; *p; ++p) {
                if (*p >= 0x20 && *p != 0x7F) clean.push_back(*p);
            }
            st.editBuffer.insert(st.editCaret, clean);
            st.editCaret += clean.size();
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
}

void HandleKeyDown(DialogState& st, WPARAM vk) {
    if (st.IsEditingCell()) {
        st.caretVisible = true;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (vk) {
            case VK_LEFT:
                if (st.editCaret > 0) --st.editCaret;
                break;
            case VK_RIGHT:
                if (st.editCaret < st.editBuffer.size()) ++st.editCaret;
                break;
            case VK_HOME:
                st.editCaret = 0;
                break;
            case VK_END:
                st.editCaret = st.editBuffer.size();
                break;
            case VK_DELETE:
                if (st.editCaret < st.editBuffer.size()) {
                    st.editBuffer.erase(st.editCaret, 1);
                }
                break;
            case VK_TAB: {
                int r = st.editRow;
                int nextCol = (st.editCol == 0) ? 1 : 0;
                st.CommitCellEdit();
                st.BeginCellEdit(r, nextCol);
                break;
            }
            case 'V':
                if (ctrl) PasteClipboardIntoEditor(st);
                break;
            default:
                return;
        }
        InvalidateRect(st.hwnd, nullptr, FALSE);
        return;
    }

    if (st.activeTab == TabId::FileClasses) {
        switch (vk) {
            case VK_UP:
                if (st.selectedClass > 0) {
                    --st.selectedClass;
                    st.EnsureRowVisible(st.selectedClass);
                    st.RebuildLayout();
                    InvalidateRect(st.hwnd, nullptr, FALSE);
                }
                break;
            case VK_DOWN:
                if (st.selectedClass + 1 < st.ClassCount()) {
                    ++st.selectedClass;
                    st.EnsureRowVisible(st.selectedClass);
                    st.RebuildLayout();
                    InvalidateRect(st.hwnd, nullptr, FALSE);
                }
                break;
            case VK_F2:
                if (st.selectedClass >= 0) {
                    st.BeginCellEdit(st.selectedClass, 0);
                    InvalidateRect(st.hwnd, nullptr, FALSE);
                }
                break;
            case VK_INSERT:
                st.AddFileClass();
                InvalidateRect(st.hwnd, nullptr, FALSE);
                break;
            case VK_DELETE:
                st.DeleteSelectedFileClass();
                InvalidateRect(st.hwnd, nullptr, FALSE);
                break;
            default:
                break;
        }
    }
}

void HandleChar(DialogState& st, WPARAM ch) {
    if (!st.IsEditingCell()) return;
    st.caretVisible = true;
    wchar_t wc = static_cast<wchar_t>(ch);

    if (wc == L'\b') {
        if (st.editCaret > 0) {
            st.editBuffer.erase(st.editCaret - 1, 1);
            --st.editCaret;
            InvalidateRect(st.hwnd, nullptr, FALSE);
        }
        return;
    }
    if (wc < 0x20 || wc == 0x7F) return;

    st.editBuffer.insert(st.editCaret, 1, wc);
    ++st.editCaret;
    InvalidateRect(st.hwnd, nullptr, FALSE);
}

void HandleLButtonDown(DialogState& st, int x, int y) {
    SetFocus(st.hwnd);
    HitTarget hit = st.HitTest(x, y);

    // If clicking outside the currently edited cell, commit first
    if (st.IsEditingCell()) {
        bool sameCell = (hit.kind == HitKind::TableRow &&
                         hit.index == st.editRow && hit.subIndex == st.editCol);
        if (!sameCell) {
            st.CommitCellEdit();
        } else {
            // Reposition caret to end on click inside editing cell
            st.editCaret = st.editBuffer.size();
            st.caretVisible = true;
            InvalidateRect(st.hwnd, nullptr, FALSE);
            return;
        }
    }

    switch (hit.kind) {
        case HitKind::NavItem:
            st.SwitchTab(static_cast<TabId>(hit.index));
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;

        case HitKind::FooterOk:
            st.accepted = true;
            st.closed = true;
            break;

        case HitKind::FooterCancel:
            st.accepted = false;
            st.closed = true;
            break;

        case HitKind::RowControl: {
            if (hit.index < 0 || hit.index >= static_cast<int>(st.rows.size())) break;
            const auto& row = st.rows[static_cast<size_t>(hit.index)];
            if (row.kind == RowKind::Checkbox) {
                bool next = !GetBoolField(st.draft, row.field);
                SetBoolField(st.draft, row.field, next);
                if (row.field == FieldId::DarkMode) {
                    st.darkMode = next;
                    st.pal = MakePalette(st.darkMode);
                    ApplyTitleBarDarkMode(st.hwnd, st.darkMode);
                }
                InvalidateRect(st.hwnd, nullptr, FALSE);
            } else if (row.kind == RowKind::Slider) {
                st.dragSliderRow = hit.index;
                SetCapture(st.hwnd);
                st.UpdateSliderFromX(hit.index, x);
                InvalidateRect(st.hwnd, nullptr, FALSE);
            } else if (row.kind == RowKind::ColorSwatch) {
                FieldId field = row.field;
                COLORREF picked = GetColorField(st.draft, field);
                if (st.PickColor(picked, picked)) {
                    SetColorField(st.draft, field, picked);
                    if (IsWindow(st.hwnd)) InvalidateRect(st.hwnd, nullptr, FALSE);
                }
            }
            break;
        }

        case HitKind::RowSegButton: {
            if (hit.index < 0 || hit.index >= static_cast<int>(st.rows.size())) break;
            const auto& row = st.rows[static_cast<size_t>(hit.index)];
            if (row.field == FieldId::Language) {
                cfg_detail::WriteLanguageIndex(st.draft.language, hit.subIndex);
                ss::i18n::Init(st.draft.language);
                st.rows = BuildRowsForTab(st.activeTab);
                st.RebuildLayout();
                if (st.hwnd) SetWindowTextW(st.hwnd, ss::i18n::T(L"cfg.window_title"));
            } else {
                cfg_detail::WriteStyleIndex(st.draft.initialStyle, hit.subIndex);
            }
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;
        }

        case HitKind::TableAdd:
            st.AddFileClass();
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;

        case HitKind::TableDelete:
            st.DeleteSelectedFileClass();
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;

        case HitKind::TableSwatch: {
            int rowIdx = hit.index;
            if (rowIdx >= 0 && rowIdx < st.ClassCount()) {
                st.selectedClass = rowIdx;
                COLORREF picked = cfg_detail::GetClassColor(st.draft.classes.items[static_cast<size_t>(rowIdx)]);
                if (st.PickColor(picked, picked) && rowIdx >= 0 && rowIdx < st.ClassCount()) {
                    cfg_detail::SetClassColor(st.draft.classes.items[static_cast<size_t>(rowIdx)], picked);
                }
                if (IsWindow(st.hwnd)) InvalidateRect(st.hwnd, nullptr, FALSE);
            }
            break;
        }

        case HitKind::TableRow:
            st.selectedClass = hit.index;
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;

        case HitKind::TableScrollThumb:
            st.draggingScroll = true;
            st.scrollDragOffsetY = y - st.layout.table.scrollThumbRect.top;
            SetCapture(st.hwnd);
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;

        case HitKind::TableScrollTrack: {
            const auto& tl = st.layout.table;
            if (y < tl.scrollThumbRect.top) {
                st.tableScroll -= tl.visibleRows;
            } else {
                st.tableScroll += tl.visibleRows;
            }
            st.ClampTableScroll();
            st.RebuildLayout();
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;
        }

        case HitKind::BodyScrollThumb:
            st.draggingBodyScroll = true;
            st.bodyScrollDragOffsetY = y - st.layout.bodyScrollThumbRect.top;
            SetCapture(st.hwnd);
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;

        case HitKind::BodyScrollTrack: {
            int page = kMaxInt(st.m.RowH(), st.layout.bodyViewportH - st.m.RowH());
            if (y < st.layout.bodyScrollThumbRect.top) {
                st.bodyScrollY -= page;
            } else {
                st.bodyScrollY += page;
            }
            st.ClampBodyScroll();
            st.RebuildLayout();
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;
        }

        case HitKind::None:
            InvalidateRect(st.hwnd, nullptr, FALSE);
            break;
    }
}

void HandleLButtonDblClk(DialogState& st, int x, int y) {
    HitTarget hit = st.HitTest(x, y);
    if (hit.kind == HitKind::TableRow && (hit.subIndex == 0 || hit.subIndex == 1)) {
        st.BeginCellEdit(hit.index, hit.subIndex);
        InvalidateRect(st.hwnd, nullptr, FALSE);
    }
}

void HandleMouseMove(DialogState& st, int x, int y) {
    if (!st.trackingMouse) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = st.hwnd;
        TrackMouseEvent(&tme);
        st.trackingMouse = true;
    }

    if (st.dragSliderRow >= 0) {
        if (st.UpdateSliderFromX(st.dragSliderRow, x)) {
            InvalidateRect(st.hwnd, nullptr, FALSE);
        }
        return;
    }

    if (st.draggingBodyScroll && st.layout.bodyHasScrollbar) {
        const auto& track = st.layout.bodyScrollTrackRect;
        const auto& thumb = st.layout.bodyScrollThumbRect;
        int trackH = track.bottom - track.top;
        int thumbH = thumb.bottom - thumb.top;
        int travel = kMaxInt(1, trackH - thumbH);
        int relY = kClampInt(y - track.top - st.bodyScrollDragOffsetY, 0, travel);
        int maxScroll = st.MaxBodyScroll();
        int nextScroll = MulDiv(relY, maxScroll, travel);
        if (nextScroll != st.bodyScrollY) {
            st.bodyScrollY = nextScroll;
            st.RebuildLayout();
            InvalidateRect(st.hwnd, nullptr, FALSE);
        }
        return;
    }

    if (st.draggingScroll && st.layout.table.hasScrollbar) {
        const auto& tl = st.layout.table;
        int trackH = tl.scrollTrackRect.bottom - tl.scrollTrackRect.top;
        int thumbH = tl.scrollThumbRect.bottom - tl.scrollThumbRect.top;
        int travel = kMaxInt(1, trackH - thumbH);
        int relY = kClampInt(y - tl.scrollTrackRect.top - st.scrollDragOffsetY, 0, travel);
        int maxScroll = st.MaxTableScroll();
        int nextScroll = MulDiv(relY, maxScroll, travel);
        if (nextScroll != st.tableScroll) {
            st.tableScroll = nextScroll;
            st.RebuildLayout();
            InvalidateRect(st.hwnd, nullptr, FALSE);
        }
        return;
    }

    HitTarget nextHover = st.HitTest(x, y);
    if (nextHover != st.hover) {
        st.hover = nextHover;
        InvalidateRect(st.hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (!cs || !cs->lpCreateParams) return FALSE;
        auto* st = static_cast<DialogState*>(cs->lpCreateParams);
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        // Must forward to DefWindowProcW so Win32 stores cs->lpszName as the window caption.
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            st->RebuildLayout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_PAINT:
            PaintDialog(hwnd, *st);
            return 0;

        case WM_MOUSEMOVE:
            HandleMouseMove(*st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSELEAVE:
            st->trackingMouse = false;
            if (st->hover.kind != HitKind::None) {
                st->hover = {};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            HandleLButtonDown(*st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONDBLCLK:
            HandleLButtonDblClk(*st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            if (st->dragSliderRow >= 0 || st->draggingScroll || st->draggingBodyScroll) {
                st->dragSliderRow = -1;
                st->draggingScroll = false;
                st->draggingBodyScroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            st->dragSliderRow = -1;
            st->draggingScroll = false;
            st->draggingBodyScroll = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL:
            if (st->activeTab == TabId::FileClasses && st->layout.table.hasScrollbar) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int steps = (delta > 0) ? -2 : 2;
                int prev = st->tableScroll;
                st->tableScroll += steps;
                st->ClampTableScroll();
                if (st->tableScroll != prev) {
                    st->RebuildLayout();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (st->activeTab != TabId::FileClasses && st->layout.bodyHasScrollbar) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int stepPx = st->m.RowH() * 2;
                int prev = st->bodyScrollY;
                st->bodyScrollY += (delta > 0) ? -stepPx : stepPx;
                st->ClampBodyScroll();
                if (st->bodyScrollY != prev) {
                    st->RebuildLayout();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;

        case WM_KEYDOWN:
            HandleKeyDown(*st, wParam);
            return 0;

        case WM_CHAR:
            HandleChar(*st, wParam);
            return 0;

        case WM_TIMER:
            if (wParam == kCaretTimerId && st->IsEditingCell()) {
                st->caretVisible = !st->caretVisible;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && st->IsEditingCell()) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                HitTarget hit = st->HitTest(pt.x, pt.y);
                if (hit.kind == HitKind::TableRow && hit.index == st->editRow && hit.subIndex == st->editCol) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                    return TRUE;
                }
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                if (st->IsEditingCell()) st->CommitCellEdit();
                st->accepted = true;
                st->closed = true;
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                if (st->IsEditingCell()) {
                    st->CancelCellEdit();
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    st->accepted = false;
                    st->closed = true;
                }
                return 0;
            }
            break;

        case WM_CLOSE:
            st->accepted = false;
            st->closed = true;
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kCaretTimerId);
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            st->hwnd = nullptr;
            st->closed = true;
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

POINT ComputeCenteredOrigin(HWND owner, int winW, int winH) {
    RECT workArea{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        workArea = mi.rcWork;
    }

    RECT anchor = workArea;
    if (owner && IsWindow(owner)) {
        RECT ownerRc{};
        if (GetWindowRect(owner, &ownerRc)) {
            anchor = ownerRc;
        }
    }

    int x = anchor.left + ((anchor.right - anchor.left) - winW) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - winH) / 2;
    x = kClampInt(x, workArea.left, kMaxInt(workArea.left, workArea.right - winW));
    y = kClampInt(y, workArea.top, kMaxInt(workArea.top, workArea.bottom - winH));
    return POINT{x, y};
}

}  // namespace

bool ShowConfigDialog(HWND owner, ss::AppConfig& cfg, int dpi, bool darkMode) {
    GdiplusSession gdiplus;
    (void)gdiplus;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    constexpr const wchar_t* kClassName = L"SpaceSnifferConfigDialogCompact";

    WNDCLASSEXW existing{};
    if (!GetClassInfoExW(hInst, kClassName, &existing)) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = ConfigWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
        wc.hIconSm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                   LR_DEFAULTCOLOR | LR_SHARED));
        if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    DialogState state{};
    state.owner = owner;
    state.m.dpi = (dpi > 0) ? dpi : 96;
    state.darkMode = darkMode;
    state.pal = MakePalette(darkMode);
    state.fonts.Create(state.m.dpi);
    state.draft = cfg;
    state.shellRegistered = IsShellIntegrationRegistered();
    state.configPath = cfg_detail::ResolveConfigPath(cfg);
    state.customColors.fill(RGB(0x4C, 0x9A, 0xFF));
    state.rows = BuildRowsForTab(state.activeTab);

    const int winW = state.m.WindowW();
    const int winH = state.m.WindowH();
    POINT origin = ComputeCenteredOrigin(owner, winW, winH);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;

    const bool hasOwner = (owner != nullptr && IsWindow(owner));
    const bool ownerWasEnabled = hasOwner && (IsWindowEnabled(owner) != FALSE);
    if (ownerWasEnabled) {
        EnableWindow(owner, FALSE);
    }

    HWND hwnd = CreateWindowExW(
        exStyle, kClassName, ss::i18n::T(L"cfg.window_title"), style,
        origin.x, origin.y, winW, winH,
        owner, nullptr, hInst, &state);

    if (!hwnd) {
        if (ownerWasEnabled && IsWindow(owner)) {
            EnableWindow(owner, TRUE);
        }
        return false;
    }

    ApplyTitleBarDarkMode(hwnd, state.darkMode);
    state.RebuildLayout();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(hwnd);

    // Modal loop with IsDialogMessageW + clean Escape / Enter / inline-edit dispatch
    MSG msg{};
    bool gotQuit = false;
    WPARAM quitCode = 0;

    while (!state.closed) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {
            gotQuit = true;
            quitCode = msg.wParam;
            break;
        }
        if (r < 0) break;

        if (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd)) {
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_ESCAPE) {
                    if (state.IsEditingCell()) {
                        state.CancelCellEdit();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else {
                        state.accepted = false;
                        state.closed = true;
                    }
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    if (state.IsEditingCell()) {
                        state.CommitCellEdit();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else {
                        state.accepted = true;
                        state.closed = true;
                    }
                    continue;
                }
                // Let inline cell editor receive arrows/Tab/Home/End/Delete directly
                if (state.IsEditingCell()) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    continue;
                }
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }

    if (state.IsEditingCell()) {
        if (state.accepted) {
            state.CommitCellEdit();
        } else {
            state.CancelCellEdit();
        }
    }

    // Restore owner BEFORE DestroyWindow to preserve Z-order / focus cleanly
    if (ownerWasEnabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
    }
    if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    if (hasOwner && IsWindow(owner)) {
        SetActiveWindow(owner);
    }
    if (gotQuit) {
        PostQuitMessage(static_cast<int>(quitCode));
    }

    if (state.accepted) {
        cfg = state.draft;
        ss::i18n::Init(cfg.language);
        return true;
    }
    ss::i18n::Init(cfg.language);
    return false;
}

}  // namespace ss::ui
