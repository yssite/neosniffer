#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include "core.h"

namespace ss::ui {

struct TreemapBox                 // 由调用方算好（squarified 布局），只负责渲染
{
    uint32_t node;                // Tree 节点索引
    double   x, y, w, h;          // 视图像素坐标
    int      depth;               // 1 = 顶层子项
};

struct TreemapStyle
{
    bool     dark          = true;
    COLORREF bg            = RGB(0x16,0x16,0x1A);
    COLORREF driveColor    = RGB(0x2D,0x6C,0xA2);
    COLORREF folderColor   = RGB(0x4A,0x7E,0xBB);
    COLORREF fileColor     = RGB(0x6D,0x8F,0xB0);
    COLORREF freeColor     = RGB(0x2F,0x5D,0x3A);
    COLORREF unknownColor  = RGB(0x5A,0x4A,0x2F);
    COLORREF accent        = RGB(0x4C,0x9A,0xFF);
    COLORREF text          = RGB(0xEC,0xEC,0xEF);
    COLORREF textDim       = RGB(0xA0,0xA0,0xA8);
    int      levelContrast = 10;      // 0..50
    int      borderContrast= 10;      // 0..50
    int      fontSize      = 9;       // pt
    int      dpi           = 96;
    bool     showFileClassColors = true;
    const std::vector<ss::FileClass>* classes = nullptr;   // 与 node.group 对应
};

// 嵌套容器布局参数（供 LayoutTreemap 使用）
struct TreemapLayoutOptions
{
    double minElementSizePx = 10.0;   // 最小可渲染矩形边长（像素）
    int    detailLevel      = 3;      // 强制下钻深度预算
    bool   autoDrill        = true;   // 超过预算但矩形仍足够大时继续自动下钻
    int    maxDepth         = 9;      // 最大递归深度
    int    maxBoxes         = 9000;   // 单帧盒子数量上限
    bool   sortBySize       = true;   // 按占用大小降序排列子项
    bool   showFree         = true;   // 是否包含 FreeSpace 虚拟节点
    bool   showUnknown      = true;   // 是否包含 Unknown 虚拟节点
    int    dpi              = 96;     // 当前视图 DPI
};

// 生成“容器嵌套结构”Squarified 树图布局（父容器顶部预留标题带、子区四周内缩）
void LayoutTreemap(const ss::Tree& tree, uint32_t focusNode,
                   double x, double y, double w, double h,
                   const TreemapLayoutOptions& opt,
                   std::vector<TreemapBox>& outBoxes);

// 画树图：背景、每层填色、分隔线、标签（名字 + 大小）、悬停描边、选中描边、标签角标
void DrawTreemap(HDC dc, const RECT& area, const ss::Tree& tree,
                 const std::vector<TreemapBox>& boxes, const TreemapStyle& style,
                 uint32_t hoverNode, uint32_t selectedNode);

// 画悬浮提示卡片（在 area 内自动避让边缘），鼠标位置用 (mouseX, mouseY)
void DrawTreemapTooltip(HDC dc, const RECT& area, const ss::Tree& tree, uint32_t node,
                        int mouseX, int mouseY, const TreemapStyle& style,
                        bool showDateTime, bool showAge);

// ----------------------------------------------------------------------------
// 焦点缩放平滑动画接口（180ms ease-out，>3000 盒自动跳过，按 HWND 隔离多标签）
// ----------------------------------------------------------------------------

// 启动一次焦点切换动画；若被跳过（如关闭动画或盒数 > 3000）返回 false。
bool TreemapAnimStart(HWND hwnd, const ss::Tree& tree,
                      uint32_t oldFocus, uint32_t newFocus,
                      const RECT& layoutArea,
                      const std::vector<TreemapBox>& oldBoxes,
                      const std::vector<TreemapBox>& newBoxes,
                      uint64_t nowMs, int durationMs = 180);

// 定时器驱动动画步进（建议 16ms）；返回 true 表示仍需继续，false 表示已结束并释放中间数据。
bool TreemapAnimTick(HWND hwnd, uint64_t nowMs);

// 立即取消并释放指定窗口的动画中间数据（用于缩放/过滤/刷新/窗口销毁）。
void TreemapAnimCancel(HWND hwnd);

// 若指定窗口处于动画中，返回当前帧已插值的盒子数组指针；否则返回 nullptr。
const std::vector<TreemapBox>* TreemapAnimCurrentBoxes(HWND hwnd);

// 动画期间在主树图之上绘制正在向父矩形中心缩小淡出的旧节点（非动画态为空操作）。
void DrawTreemapAnimExiting(HDC dc, HWND hwnd, const RECT& area,
                            const ss::Tree& tree, const TreemapStyle& style);

} // namespace ss::ui
