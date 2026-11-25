#include "ui_theme.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

namespace ss::ui {

Palette g_pal;
ThemeFonts g_fonts;
int g_dpi = 96;

int S(int px) { return MulDiv(px, g_dpi, 96); }

static void FreeBrushes()
{
    if (g_pal.brSurface2) { DeleteObject(g_pal.brSurface2); g_pal.brSurface2 = nullptr; }
    if (g_pal.brSurface)  { DeleteObject(g_pal.brSurface);  g_pal.brSurface  = nullptr; }
    if (g_pal.brMenuBg)   { DeleteObject(g_pal.brMenuBg);   g_pal.brMenuBg   = nullptr; }
}

void InitTheme(bool dark)
{
    FreeBrushes();
    g_pal.dark = dark;
    if (dark)
    {
        g_pal.bg         = RGB(0x1B, 0x1B, 0x1F);
        g_pal.surface    = RGB(0x23, 0x23, 0x27);
        g_pal.surface2   = RGB(0x2A, 0x2A, 0x30);
        g_pal.pressed    = RGB(0x35, 0x35, 0x3E);
        g_pal.border     = RGB(0x3A, 0x3A, 0x42);
        g_pal.text       = RGB(0xEC, 0xEC, 0xEF);
        g_pal.textDim    = RGB(0xA0, 0xA0, 0xA8);
        g_pal.accent     = RGB(0x4C, 0x9A, 0xFF);
        g_pal.accentDim  = RGB(0x2F, 0x6F, 0xD0);
        g_pal.accentSoft = RGB(0x24, 0x36, 0x52);
        g_pal.canvas     = RGB(0x15, 0x15, 0x19);
        g_pal.shadow     = RGB(0x0B, 0x0B, 0x0E);
        g_pal.danger     = RGB(0xE0, 0x56, 0x56);
    }
    else
    {
        g_pal.bg         = RGB(0xF7, 0xF8, 0xFA);
        g_pal.surface    = RGB(0xFF, 0xFF, 0xFF);
        g_pal.surface2   = RGB(0xEE, 0xF0, 0xF4);
        g_pal.pressed    = RGB(0xDF, 0xE3, 0xEC);
        g_pal.border     = RGB(0xD5, 0xD9, 0xE0);
        g_pal.text       = RGB(0x1D, 0x1F, 0x23);
        g_pal.textDim    = RGB(0x66, 0x6C, 0x76);
        g_pal.accent     = RGB(0x1F, 0x6F, 0xEB);
        g_pal.accentDim  = RGB(0x15, 0x55, 0xBA);
        g_pal.accentSoft = RGB(0xE3, 0xEE, 0xFE);
        g_pal.canvas     = RGB(0xEC, 0xEE, 0xF3);
        g_pal.shadow     = RGB(0xC6, 0xCA, 0xD4);
        g_pal.danger     = RGB(0xCF, 0x22, 0x2E);
    }
    g_pal.brSurface2 = CreateSolidBrush(g_pal.surface2);
    g_pal.brSurface  = CreateSolidBrush(g_pal.surface);
    g_pal.brMenuBg   = CreateSolidBrush(g_pal.surface);
}

static void DeleteFontSafe(HFONT& f)
{
    if (f) { DeleteObject(f); f = nullptr; }
}

void DestroyThemeResources()
{
    FreeBrushes();
    DeleteFontSafe(g_fonts.ui);
    DeleteFontSafe(g_fonts.uiBold);
    DeleteFontSafe(g_fonts.uiSmall);
    DeleteFontSafe(g_fonts.uiSmallBold);
    DeleteFontSafe(g_fonts.treeFolder);
    DeleteFontSafe(g_fonts.treeFile);
    DeleteFontSafe(g_fonts.treeSize);
    DeleteFontSafe(g_fonts.treeHeader);
    DeleteFontSafe(g_fonts.startTitle);
    DeleteFontSafe(g_fonts.startSub);
    DeleteFontSafe(g_fonts.startRow);
    DeleteFontSafe(g_fonts.startRowBld);
    DeleteFontSafe(g_fonts.tipTitle);
    DeleteFontSafe(g_fonts.tipBody);
    DeleteFontSafe(g_fonts.mono);
    DeleteFontSafe(g_fonts.icon);
    g_fonts.hasIconFont = false;
}

static bool FontFamilyExists(HDC dc, const wchar_t* face)
{
    struct Probe { bool found = false; } probe;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    auto cb = [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
        ((Probe*)lp)->found = true;
        return 0;
    };
    EnumFontFamiliesExW(dc, &lf, cb, (LPARAM)&probe, 0);
    return probe.found;
}

static HFONT MakeFontPt(double pt, int weight, const wchar_t* face)
{
    LOGFONTW lf{};
    lf.lfHeight = -(int)std::lround(pt * g_dpi / 72.0);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

static HFONT MakeFontPx(int pxAt96, int weight, const wchar_t* face)
{
    LOGFONTW lf{};
    lf.lfHeight = -S(pxAt96);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

void RebuildFonts(int baseTreePt)
{
    DeleteFontSafe(g_fonts.ui);
    DeleteFontSafe(g_fonts.uiBold);
    DeleteFontSafe(g_fonts.uiSmall);
    DeleteFontSafe(g_fonts.uiSmallBold);
    DeleteFontSafe(g_fonts.treeFolder);
    DeleteFontSafe(g_fonts.treeFile);
    DeleteFontSafe(g_fonts.treeSize);
    DeleteFontSafe(g_fonts.treeHeader);
    DeleteFontSafe(g_fonts.startTitle);
    DeleteFontSafe(g_fonts.startSub);
    DeleteFontSafe(g_fonts.startRow);
    DeleteFontSafe(g_fonts.startRowBld);
    DeleteFontSafe(g_fonts.tipTitle);
    DeleteFontSafe(g_fonts.tipBody);
    DeleteFontSafe(g_fonts.mono);
    DeleteFontSafe(g_fonts.icon);
    g_fonts.hasIconFont = false;

    HDC dc = GetDC(nullptr);
    const wchar_t* uiFace = FontFamilyExists(dc, L"Microsoft YaHei UI") ? L"Microsoft YaHei UI" : L"Segoe UI";
    const wchar_t* iconFace = nullptr;
    if (FontFamilyExists(dc, L"Segoe Fluent Icons")) iconFace = L"Segoe Fluent Icons";
    else if (FontFamilyExists(dc, L"Segoe MDL2 Assets")) iconFace = L"Segoe MDL2 Assets";
    ReleaseDC(nullptr, dc);

    int treePt = std::clamp(baseTreePt, 8, 18);
    // Ensure default 9/10 yields crisp 10pt name + 9pt size per design spec
    double titlePt = std::max(10.0, (double)treePt);
    double subPt   = std::max(8.5, titlePt - 1.0);

    g_fonts.ui          = MakeFontPx(13, FW_NORMAL,   uiFace);
    g_fonts.uiBold      = MakeFontPx(13, FW_SEMIBOLD, uiFace);
    g_fonts.uiSmall     = MakeFontPx(12, FW_NORMAL,   uiFace);
    g_fonts.uiSmallBold = MakeFontPx(12, FW_SEMIBOLD, uiFace);

    g_fonts.treeFolder  = MakeFontPt(titlePt, FW_SEMIBOLD, uiFace);
    g_fonts.treeFile    = MakeFontPt(titlePt, FW_NORMAL,   uiFace);
    g_fonts.treeSize    = MakeFontPt(subPt,   FW_NORMAL,   uiFace);
    g_fonts.treeHeader  = MakeFontPt(9.0,     FW_SEMIBOLD, uiFace);

    g_fonts.startTitle  = MakeFontPt(20.0, FW_BOLD,     uiFace);
    g_fonts.startSub    = MakeFontPt(10.0, FW_NORMAL,   uiFace);
    g_fonts.startRow    = MakeFontPt(10.0, FW_NORMAL,   uiFace);
    g_fonts.startRowBld = MakeFontPt(10.0, FW_SEMIBOLD, uiFace);

    g_fonts.tipTitle    = MakeFontPt(11.0, FW_SEMIBOLD, uiFace);
    g_fonts.tipBody     = MakeFontPt(9.0,  FW_NORMAL,   uiFace);

    g_fonts.mono        = MakeFontPt(9.5,  FW_NORMAL,   L"Consolas");

    if (iconFace)
    {
        g_fonts.icon = MakeFontPx(13, FW_NORMAL, iconFace);
        g_fonts.hasIconFont = (g_fonts.icon != nullptr);
    }
}

void ApplyWindowTitleBarTheme(HWND hwnd, bool dark)
{
    if (!hwnd) return;
    BOOL val = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val))))
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
}

bool HasIconGlyph(wchar_t glyph)
{
    if (!g_fonts.hasIconFont || !g_fonts.icon || glyph == 0) return false;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, g_fonts.icon);
    WORD idx = 0xFFFF;
    DWORD r = GetGlyphIndicesW(dc, &glyph, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return r == 1 && idx != 0xFFFF;
}

COLORREF BlendColor(COLORREF base, COLORREF overlay, double alpha)
{
    alpha = std::clamp(alpha, 0.0, 1.0);
    auto mix = [&](int a, int b) {
        return (BYTE)std::clamp((int)std::lround(a * (1.0 - alpha) + b * alpha), 0, 255);
    };
    return RGB(mix(GetRValue(base), GetRValue(overlay)),
               mix(GetGValue(base), GetGValue(overlay)),
               mix(GetBValue(base), GetBValue(overlay)));
}

COLORREF ShadeColor(COLORREF c, double factor)
{
    auto ch = [&](int v) {
        double out = factor >= 0.0 ? (v + (255.0 - v) * factor) : (v * (1.0 + factor));
        return (BYTE)std::clamp((int)std::lround(out), 0, 255);
    };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

double Luminance(COLORREF c)
{
    return (0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c)) / 255.0;
}

COLORREF TextColorOn(COLORREF bg)
{
    return Luminance(bg) > 0.62 ? RGB(0x16, 0x18, 0x1D) : RGB(0xF4, 0xF5, 0xF8);
}

void FillSolid(HDC dc, const RECT& r, COLORREF c)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void Frame1px(HDC dc, const RECT& r, COLORREF c)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

void HLine(HDC dc, int x1, int x2, int y, COLORREF c)
{
    RECT r{ x1, y, x2, y + 1 };
    FillSolid(dc, r, c);
}

void VLine(HDC dc, int x, int y1, int y2, COLORREF c)
{
    RECT r{ x, y1, x + 1, y2 };
    FillSolid(dc, r, c);
}

void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, bool drawBorder)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = drawBorder ? CreatePen(PS_SOLID, 1, border) : (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ oldB = SelectObject(dc, br);
    HGDIOBJ oldP = SelectObject(dc, pen);
    int d = std::max(2, radius * 2);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, d, d);
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    if (drawBorder) DeleteObject(pen);
    DeleteObject(br);
}

void DrawTextLine(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt)
{
    if (s.empty() || r.right <= r.left || r.bottom <= r.top) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt);
    SelectObject(dc, old);
}

SIZE MeasureText(HDC dc, const std::wstring& s, HFONT f)
{
    SIZE sz{ 0, 0 };
    if (s.empty()) return sz;
    HGDIOBJ old = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, old);
    return sz;
}

void DrawCloseGlyph(HDC dc, const RECT& r, COLORREF c)
{
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    int half = std::max(3, S(4));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), c);
    HGDIOBJ oldP = SelectObject(dc, pen);
    MoveToEx(dc, cx - half, cy - half, nullptr);
    LineTo(dc, cx + half + 1, cy + half + 1);
    MoveToEx(dc, cx + half, cy - half, nullptr);
    LineTo(dc, cx - half - 1, cy + half + 1);
    SelectObject(dc, oldP);
    DeleteObject(pen);
}

} // namespace ss::ui
