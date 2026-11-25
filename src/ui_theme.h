#pragma once

#include "core.h"

namespace ss::ui {

struct Palette
{
    COLORREF bg         = RGB(0x1B, 0x1B, 0x1F);
    COLORREF surface    = RGB(0x23, 0x23, 0x27);
    COLORREF surface2   = RGB(0x2A, 0x2A, 0x30);
    COLORREF pressed    = RGB(0x33, 0x33, 0x3B);
    COLORREF border     = RGB(0x3A, 0x3A, 0x42);
    COLORREF text       = RGB(0xEC, 0xEC, 0xEF);
    COLORREF textDim    = RGB(0xA0, 0xA0, 0xA8);
    COLORREF accent     = RGB(0x4C, 0x9A, 0xFF);
    COLORREF accentDim  = RGB(0x2F, 0x6F, 0xD0);
    COLORREF accentSoft = RGB(0x22, 0x34, 0x4E);
    COLORREF canvas     = RGB(0x16, 0x16, 0x1A);
    COLORREF shadow     = RGB(0x0E, 0x0E, 0x12);
    COLORREF danger     = RGB(0xE0, 0x56, 0x56);
    bool dark           = true;

    HBRUSH brSurface2   = nullptr;
    HBRUSH brSurface    = nullptr;
    HBRUSH brMenuBg     = nullptr;
};

struct ThemeFonts
{
    HFONT ui          = nullptr; // 9.5pt regular (toolbar, tabs, menus, inputs)
    HFONT uiBold      = nullptr; // 9.5pt semibold
    HFONT uiSmall     = nullptr; // 9pt (12px at 96dpi) status bar, labels, breadcrumbs
    HFONT uiSmallBold = nullptr; // 9pt semibold
    HFONT treeFolder  = nullptr; // 10pt semibold (treemap folder label)
    HFONT treeFile    = nullptr; // 10pt regular (treemap file label)
    HFONT treeSize    = nullptr; // 9pt regular (treemap size sub-label)
    HFONT treeHeader  = nullptr; // 9pt semibold (container folder header)
    HFONT startTitle  = nullptr; // 20pt bold
    HFONT startSub    = nullptr; // 10pt regular
    HFONT startRow    = nullptr; // 10pt regular
    HFONT startRowBld = nullptr; // 10pt semibold
    HFONT tipTitle    = nullptr; // 11pt semibold
    HFONT tipBody     = nullptr; // 9pt regular
    HFONT mono        = nullptr; // 9.5pt Consolas
    HFONT icon        = nullptr; // Segoe Fluent Icons / Segoe MDL2 Assets (nullptr if unavailable)
    bool  hasIconFont = false;
};

extern Palette g_pal;
extern ThemeFonts g_fonts;
extern int g_dpi;

int S(int px);

void InitTheme(bool dark);
void DestroyThemeResources();
void RebuildFonts(int baseTreePt);
void ApplyWindowTitleBarTheme(HWND hwnd, bool dark);

bool HasIconGlyph(wchar_t glyph);

// Color helpers
COLORREF BlendColor(COLORREF base, COLORREF overlay, double alpha);
COLORREF ShadeColor(COLORREF c, double factor);
double   Luminance(COLORREF c);
COLORREF TextColorOn(COLORREF bg);

// GDI drawing helpers
void FillSolid(HDC dc, const RECT& r, COLORREF c);
void Frame1px(HDC dc, const RECT& r, COLORREF c);
void HLine(HDC dc, int x1, int x2, int y, COLORREF c);
void VLine(HDC dc, int x, int y1, int y2, COLORREF c);
void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, bool drawBorder);
void DrawTextLine(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f,
                  UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
SIZE MeasureText(HDC dc, const std::wstring& s, HFONT f);
void DrawCloseGlyph(HDC dc, const RECT& r, COLORREF c);

} // namespace ss::ui
