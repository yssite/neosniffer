#pragma once

#include "core.h"
#include "ui_theme.h"
#include <thread>
#include <vector>
#include <string>
#include <memory>

namespace ss::ui {

inline constexpr const wchar_t* kMainClass  = L"SSMainWindow";
inline constexpr const wchar_t* kViewClass  = L"SSViewWindow";
inline constexpr const wchar_t* kStartClass = L"SSStartWindow";
inline constexpr const wchar_t* kLogClass   = L"SSLogWindow";
inline constexpr const wchar_t* kHelpClass  = L"SSHelpWindow";
inline constexpr const wchar_t* kAppName    = L"SpaceSniffer 2.0 — 磁盘空间分析";

inline constexpr UINT WM_APP_SCAN_DONE     = WM_APP + 1;
inline constexpr UINT WM_APP_SCAN_PROGRESS = WM_APP + 2;

enum CommandId
{
    CMD_NEW = 1000,
    CMD_EXPORT,
    CMD_SNAPSHOT,
    CMD_EXIT,
    CMD_CONFIG,
    CMD_BACK,
    CMD_FWD,
    CMD_UP,
    CMD_HOME,
    CMD_REFRESH,
    CMD_DETAIL_MINUS,
    CMD_DETAIL_PLUS,
    CMD_STYLE,
    CMD_FREE,
    CMD_UNKNOWN,
    CMD_DARK,
    CMD_LOG,
    CMD_HELP_FILTER,
    CMD_HELP_TAG,
    CMD_HELP_CLI,
    CMD_ABOUT,
    CMD_TAG1,
    CMD_TAG2,
    CMD_TAG3,
    CMD_TAG4,
    CMD_TAG0,
    CMD_STOP
};

struct Box
{
    uint32_t node = 0;
    double x = 0, y = 0, w = 0, h = 0;
    int depth = 0;
    bool isContainer = false; // true if children were laid out inside this box
    int headerH = 0;          // height of container header strip (0 if none)
};

struct ViewState
{
    HWND hwnd = nullptr;
    HWND editFilter = nullptr;
    WNDPROC origEditProc = nullptr;
    bool filterFocused = false;
    bool filterBtnHot = false;
    bool filterBtnDown = false;
    bool clearBtnHot = false;
    bool clearBtnDown = false;
    bool trackingMouse = false;

    RECT rcFilterInput{};
    RECT rcFilterBtn{};
    RECT rcClearBtn{};
    RECT rcBreadcrumb{};
    RECT rcCanvas{};

    std::wstring path;
    std::wstring title;
    std::wstring status;      // left status message
    std::wstring statsRight;  // right status summary: "N 文件 · N 目录 · X GB · 模式"
    std::wstring filterText;

    Tree tree;
    Tree filtered;
    bool usingFiltered = false;

    uint32_t focus = 0;
    uint32_t selected = Node::kNone;
    uint32_t hover = Node::kNone;
    std::vector<uint32_t> back, forward;
    std::vector<Box> boxes;

    int mouseX = -1, mouseY = -1;
    bool scanning = false;
    bool usedMft = false;
    std::thread worker;
    ScanProgress progress;
    int lastW = -1, lastH = -1;
};

// Globals defined in main.cpp
extern AppConfig g_cfg;
extern std::vector<std::unique_ptr<ViewState>> g_views;
extern int g_activeView;
extern HWND g_main;
extern HWND g_start;
extern HWND g_log;
extern bool g_showStart;

// Chrome layout rects (client coords of g_main)
extern RECT g_rcMenuBar;
extern RECT g_rcToolbar;
extern RECT g_rcTabs;
extern RECT g_rcContent;
extern RECT g_rcStatus;

// View helpers
ViewState* CurrentView();
Tree& ActiveTree(ViewState* v);
void UpdateViewStatus(ViewState* v);
void RelayoutView(ViewState* v);
void ApplyFilter(ViewState* v);
void ClearFilter(ViewState* v);
void StartScan(ViewState* v, const std::wstring& path);
void RefreshMain();
void LayoutMain(HWND hwnd);
void LogLine(const std::wstring& s);

// Navigation / view lifecycle
void ShowView(int index);
void OpenScanView(const std::wstring& path, const std::wstring& filter);
void OpenSnapshotView(const std::wstring& file);
void CloseView(int index);
void DoCommand(int cmd);

// Window procs (implemented in dedicated modules)
LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK StartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Start page helpers
void BuildDriveRows();

// Chrome (menu bar, owner-drawn popups, toolbar, tabs, status bar)
void InitChrome();
void PaintMainChrome(HWND hwnd, HDC dc);
bool HandleMainMouseMove(HWND hwnd, int x, int y);
bool HandleMainMouseLeave(HWND hwnd);
bool HandleMainLButtonDown(HWND hwnd, int x, int y);
bool HandleMainLButtonUp(HWND hwnd, int x, int y);
bool HandleMainSysKeyDown(HWND hwnd, WPARAM wp);
bool HandleMenuMeasureItem(HWND hwnd, MEASUREITEMSTRUCT* mis);
bool HandleMenuDrawItem(HWND hwnd, DRAWITEMSTRUCT* dis);

// Dialogs
void ShowConfigDlg(HWND owner);
void ShowExportDlg(HWND owner);
void ShowLogWindow(HWND owner);
void ShowHelpDlg(HWND owner, int which);
void ShowAboutDlg(HWND owner);
void UpdateLogWindowTheme();

} // namespace ss::ui
