#include "ui_treemap.h"
#include "i18n.h"
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace ss::ui {
namespace {

struct ReleaseCom
{
    void operator()(IUnknown* object) const { object->Release(); }
};

template<typename T>
using ComPtr = std::unique_ptr<T, ReleaseCom>;

// ============================================================================
// 公共基础数学与几何工具（不依赖 <cmath>，规避 <windows.h> min/max 宏冲突）
// ============================================================================

template <typename T>
inline T MinVal(T a, T b) { return (a < b) ? a : b; }

template <typename T>
inline T MaxVal(T a, T b) { return (a > b) ? a : b; }

template <typename T>
inline T ClampVal(T v, T lo, T hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }

inline double AbsD(double v) { return (v < 0.0) ? -v : v; }

inline int FloorInt(double v)
{
    int i = static_cast<int>(v);
    return (v < static_cast<double>(i)) ? (i - 1) : i;
}

inline int RoundInt(double v)
{
    return (v >= 0.0) ? static_cast<int>(v + 0.5) : static_cast<int>(v - 0.5);
}

inline BYTE ClampByte(int v)
{
    return static_cast<BYTE>(ClampVal(v, 0, 255));
}

inline double SqrtD(double x)
{
    if (x <= 0.0) return 0.0;
    double g = (x >= 1.0) ? x : 1.0;
    for (int i = 0; i < 12; ++i) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

inline int ScalePx(int px, int dpi)
{
    return MulDiv(px, (dpi > 0) ? dpi : 96, 96);
}

inline int RectW(const RECT& r) { return r.right - r.left; }
inline int RectH(const RECT& r) { return r.bottom - r.top; }

inline bool RectIntersects(const RECT& a, const RECT& b)
{
    return a.left < b.right && a.right > b.left &&
           a.top < b.bottom && a.bottom > b.top;
}

// ============================================================================
// C++20 节点/路径/时间适配工具（零堆分配读取名称，兼容 core.h 字段）
// ============================================================================

struct WStrSpan
{
    const wchar_t* data = nullptr;
    size_t         len  = 0;
};

inline WStrSpan GetNodeNameSpan(const ss::Tree& tree, uint32_t node)
{
    if (!tree.Valid(node)) return {};
    const ss::Node& n = tree.nodes[node];
    size_t off = static_cast<size_t>(n.nameOff);
    size_t len = static_cast<size_t>(n.nameLen);
    if (off + len > tree.names.size()) return {};
    return { tree.names.data() + off, len };
}

inline std::wstring ToWString(const wchar_t* s)
{
    return s ? std::wstring(s) : std::wstring();
}

template <typename TStr>
inline std::wstring ToWString(const TStr& s)
{
    if constexpr (requires { std::wstring(s); }) {
        return std::wstring(s);
    } else if constexpr (requires { s.data(); s.size(); }) {
        return std::wstring(s.data(), s.size());
    } else {
        return std::wstring();
    }
}

template <typename K>
inline bool IsKindDrive(K k)
{
    if constexpr (std::is_integral_v<K>) return static_cast<ss::Kind>(k) == ss::Kind::Drive;
    else if constexpr (requires { K::Drive; }) return k == K::Drive;
    else return false;
}

template <typename K>
inline bool IsKindFolder(K k)
{
    if constexpr (std::is_integral_v<K>) return static_cast<ss::Kind>(k) == ss::Kind::Folder;
    else if constexpr (requires { K::Folder; }) return k == K::Folder;
    else return false;
}

template <typename K>
inline bool IsKindFree(K k)
{
    if constexpr (std::is_integral_v<K>) return static_cast<ss::Kind>(k) == ss::Kind::FreeSpace;
    else if constexpr (requires { K::FreeSpace; }) return k == K::FreeSpace;
    else return false;
}

template <typename K>
inline bool IsKindUnknown(K k)
{
    if constexpr (std::is_integral_v<K>) {
        auto sk = static_cast<ss::Kind>(k);
        return sk == ss::Kind::Unknown || sk == ss::Kind::Missing;
    } else if constexpr (requires { K::Unknown; }) {
        return k == K::Unknown;
    } else {
        return false;
    }
}

inline std::wstring GetNodeFullPath(const ss::Tree& tree, uint32_t node)
{
    return ToWString(tree.Path(node));
}

template <typename F>
inline uint64_t ToFileTimeU64(const F& ft)
{
    if constexpr (requires { ft.dwLowDateTime; ft.dwHighDateTime; }) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
               static_cast<uint64_t>(ft.dwLowDateTime);
    } else if constexpr (requires { static_cast<int64_t>(ft); }) {
        int64_t s = static_cast<int64_t>(ft);
        if (s <= 0) return 0;
        uint64_t raw = static_cast<uint64_t>(s);
        if (raw >= 10000000000000000ULL) return raw;
        if (raw >= 100000000000ULL)      return raw * 10000ULL + 116444736000000000ULL;
        return raw * 10000000ULL + 116444736000000000ULL;
    } else {
        return 0;
    }
}

template <typename TNode>
inline uint64_t GetNodeModTime(const TNode& n)
{
    if constexpr (requires { n.modified; }) return ToFileTimeU64(n.modified);
    else if constexpr (requires { n.mtime; }) return ToFileTimeU64(n.mtime);
    else return 0;
}

// ============================================================================
// 容器几何公式（布局模块与绘制模块共享同一套标题带与子区内缩参数）
// ============================================================================

// 标题带高度公式：
//   - depth == 1: S(19)   顶层容器留足呼吸感
//   - depth == 2: S(18)   二级容器紧凑清晰
//   - depth >= 3: S(17)   深层容器节约纵向空间，同时严格落于 S(17..19) 区间
//   按 DPI 缩放，保证至少 14px 以完整容纳 7pt 标题文字。
inline int ContainerHeaderHeight(int depth, int dpi)
{
    int basePx = (depth <= 1) ? 19 : ((depth == 2) ? 18 : 17);
    return MaxVal(14, ScalePx(basePx, dpi));
}

// 容器“外框”彩色边厚度（不含最外 1px 暗边框）：
//   - 浅层（depth <= 2）：S(2)（96dpi=2px, 120dpi=3px），盒子套盒子轮廓醒目
//   - 深层（depth >= 3）：1px（高 DPI 下至少 1px），避免深层嵌套过度吞噬子区
inline int ContainerRimPad(int depth, int dpi)
{
    if (depth <= 2) return MaxVal(2, ScalePx(2, dpi));
    return MaxVal(1, ScalePx(1, dpi));
}

struct ContainerGeometry
{
    RECT headerRect;     // 标题带矩形（位于外框 1px 边框内侧顶部）
    RECT childWellRect;  // 子区外边界（位于标题带下方且四周内缩，其边缘绘制 1px 深色内缝）
};

// 由容器外矩形计算标题带与子区：
//   - 最外侧 1px：容器外边框（分隔同级兄弟）
//   - 顶部紧接 headerH：标题带（背景为容器基色加深 8%）
//   - 标题带下方留 1px 内缩 + 左右底留 rimPad（容器自身填充即“外框”）
//   - 子项全部布局在 childWellRect 内，childWellRect 边线即 1px 深色内缝
inline ContainerGeometry ComputeContainerGeometry(const RECT& outer, int depth, int dpi)
{
    int headerH = ContainerHeaderHeight(depth, dpi);
    int rimPad  = ContainerRimPad(depth, dpi);
    int sideIn  = 1 + rimPad;            // 1px 外边框 + rimPad 彩色外框
    int topIn   = 1 + headerH + 1;       // 1px 上外框 + 标题带 + 1px 上内缩

    int hdrBot = MinVal(outer.bottom - 1, outer.top + 1 + headerH);
    RECT hdr{ outer.left + 1, outer.top + 1, outer.right - 1, hdrBot };
    RECT well{
        outer.left + sideIn,
        outer.top + topIn,
        outer.right - sideIn,
        outer.bottom - sideIn
    };
    return { hdr, well };
}

// ============================================================================
// 模块一：布局（Squarified 嵌套容器布局）
// ============================================================================

struct RectD
{
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

inline double NodeWeight(const ss::Tree& t, uint32_t n)
{
    const ss::Node& nd = t.nodes[n];
    return static_cast<double>(nd.disk > 0 ? nd.disk : nd.size);
}

void SquarifyLayout(const ss::Tree& t, const std::vector<uint32_t>& items, double total,
                    double ax, double ay, double aw, double ah, std::vector<RectD>& out)
{
    const size_t n = items.size();
    out.clear();
    if (n == 0 || aw <= 0.0 || ah <= 0.0 || total <= 0.0) return;

    const double area = aw * ah;
    std::vector<double> a(n);
    for (size_t i = 0; i < n; ++i) {
        a[i] = NodeWeight(t, items[i]) / total * area;
    }

    size_t idx = 0;
    double x = ax, y = ay, w = aw, h = ah;
    while (idx < n) {
        double side = MinVal(w, h);
        if (side <= 0.001) break;

        size_t rowEnd = idx;
        double rowSum = 0.0;
        double worstNow = 1e300;
        double curMx = 0.0;
        double curMn = 1e300;

        while (rowEnd < n) {
            double s = rowSum + a[rowEnd];
            double mx = MaxVal(curMx, a[rowEnd]);
            double mn = MinVal(curMn, a[rowEnd]);
            double worst = (mn <= 0.0 || s <= 0.0)
                ? 1e300
                : MaxVal(side * side * mx / (s * s), (s * s) / (side * side * mn));
            if (rowEnd > idx && worst > worstNow) break;
            rowSum = s;
            curMx = mx;
            curMn = mn;
            worstNow = worst;
            ++rowEnd;
        }
        if (rowEnd == idx) rowEnd = idx + 1;
        if (rowSum <= 0.0) break;

        bool vertical = (w >= h);
        double strip = vertical ? (rowSum / h) : (rowSum / w);
        if (strip < 0.0) strip = 0.0;
        double cursor = vertical ? y : x;
        for (size_t k = idx; k < rowEnd; ++k) {
            double extent = (strip > 0.0) ? (a[k] / strip) : 0.0;
            if (vertical) out.push_back({ x, cursor, strip, extent });
            else          out.push_back({ cursor, y, extent, strip });
            cursor += extent;
        }
        if (vertical) { x += strip; w -= strip; }
        else          { y += strip; h -= strip; }
        idx = rowEnd;
        if (w <= 0.5 || h <= 0.5) break;
    }
    while (out.size() < n) out.push_back({ 0.0, 0.0, 0.0, 0.0 });
}

void CollectNestedBoxes(const ss::Tree& t, uint32_t node, int depth,
                        double x, double y, double w, double h,
                        const TreemapLayoutOptions& opt,
                        std::vector<TreemapBox>& outBoxes)
{
    if (!t.Valid(node) || depth >= opt.maxDepth) return;
    if (static_cast<int>(outBoxes.size()) >= opt.maxBoxes) return;

    const int dpi = (opt.dpi > 0) ? opt.dpi : 96;
    const double minSz = MaxVal(1.0, opt.minElementSizePx);

    // 深度大于 0 时，当前节点本身是一个容器盒子：
    // 需在顶部预留标题带（S(17..19)）并四周内缩，再决定子区是否足以继续下钻。
    double cx = x, cy = y, cw = w, ch = h;
    if (depth > 0) {
        bool beyondBudget = (depth >= opt.detailLevel);
        if (beyondBudget) {
            if (!opt.autoDrill) return;
            if (w < static_cast<double>(ScalePx(88, dpi)) ||
                h < static_cast<double>(ScalePx(58, dpi))) {
                return;
            }
        }
        int headerH = ContainerHeaderHeight(depth, dpi);
        int rimPad  = ContainerRimPad(depth, dpi);
        double sideIn = static_cast<double>(1 + rimPad);
        double topIn  = static_cast<double>(1 + headerH + 1);
        cx = x + sideIn;
        cy = y + topIn;
        cw = w - sideIn * 2.0;
        ch = h - topIn - sideIn;

        double minChildW = MaxVal(minSz * 2.0, static_cast<double>(ScalePx(22, dpi)));
        double minChildH = MaxVal(minSz * 2.0, static_cast<double>(ScalePx(16, dpi)));
        if (cw < minChildW || ch < minChildH) return;
    } else {
        if (cw < minSz * 2.0 || ch < minSz * 2.0) return;
    }

    std::vector<uint32_t> kids;
    double total = 0.0;
    size_t guard = 0;
    const size_t maxNodes = t.nodes.size();
    for (uint32_t c = t.nodes[node].firstChild;
         c != ss::Node::kNone && t.Valid(c) && guard++ < maxNodes;
         c = t.nodes[c].nextSibling) {
        uint8_t k = t.nodes[c].kind;
        if (IsKindFree(k) && !opt.showFree) continue;
        if (IsKindUnknown(k) && !opt.showUnknown) continue;
        double wgt = NodeWeight(t, c);
        if (wgt <= 0.0) continue;
        kids.push_back(c);
        total += wgt;
    }
    if (kids.empty() || total <= 0.0) return;

    if (opt.sortBySize) {
        std::sort(kids.begin(), kids.end(), [&](uint32_t a, uint32_t b) {
            return NodeWeight(t, a) > NodeWeight(t, b);
        });
    }

    std::vector<RectD> rects;
    SquarifyLayout(t, kids, total, cx, cy, cw, ch, rects);

    for (size_t i = 0; i < kids.size() && i < rects.size(); ++i) {
        if (static_cast<int>(outBoxes.size()) >= opt.maxBoxes) break;
        const RectD& r = rects[i];
        if (r.w < minSz || r.h < minSz) continue;

        TreemapBox box{};
        box.node  = kids[i];
        box.x     = r.x;
        box.y     = r.y;
        box.w     = r.w;
        box.h     = r.h;
        box.depth = depth + 1;
        outBoxes.push_back(box);

        // 仅文件夹/驱动器继续递归下钻；若下钻后未产生任何可见子项，该节点自然保持为叶子绘制
        if (t.nodes[kids[i]].IsFolder() && t.nodes[kids[i]].firstChild != ss::Node::kNone) {
            CollectNestedBoxes(t, kids[i], depth + 1, r.x, r.y, r.w, r.h, opt, outBoxes);
        }
    }
}

// ============================================================================
// 模块二：绘制（色彩与层级提亮 + 四档字体与字宽缓存 + 嵌套容器与标签渲染）
// ============================================================================

struct HslColor
{
    double h; // 0..1
    double s; // 0..1
    double l; // 0..1
};

inline HslColor RgbToHsl(COLORREF c)
{
    double r = GetRValue(c) / 255.0;
    double g = GetGValue(c) / 255.0;
    double b = GetBValue(c) / 255.0;
    double cmax = MaxVal(r, MaxVal(g, b));
    double cmin = MinVal(r, MinVal(g, b));
    double delta = cmax - cmin;

    HslColor out{ 0.0, 0.0, 0.5 * (cmax + cmin) };
    if (delta > 1e-6) {
        out.s = (out.l > 0.5) ? (delta / (2.0 - cmax - cmin))
                              : (delta / (cmax + cmin));
        if (cmax == r) {
            out.h = (g - b) / delta + ((g < b) ? 6.0 : 0.0);
        } else if (cmax == g) {
            out.h = (b - r) / delta + 2.0;
        } else {
            out.h = (r - g) / delta + 4.0;
        }
        out.h /= 6.0;
    }
    return out;
}

inline double HueToRgb(double p, double q, double t)
{
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5)       return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

inline COLORREF HslToRgb(HslColor c)
{
    double s = ClampVal(c.s, 0.0, 1.0);
    double l = ClampVal(c.l, 0.0, 1.0);
    double r = l, g = l, b = l;
    if (s > 1e-6) {
        double q = (l < 0.5) ? (l * (1.0 + s)) : (l + s - l * s);
        double p = 2.0 * l - q;
        r = HueToRgb(p, q, c.h + 1.0 / 3.0);
        g = HueToRgb(p, q, c.h);
        b = HueToRgb(p, q, c.h - 1.0 / 3.0);
    }
    return RGB(ClampByte(RoundInt(r * 255.0)),
               ClampByte(RoundInt(g * 255.0)),
               ClampByte(RoundInt(b * 255.0)));
}

inline COLORREF BlendColor(COLORREF a, COLORREF b, double t)
{
    t = ClampVal(t, 0.0, 1.0);
    double inv = 1.0 - t;
    int r  = RoundInt(GetRValue(a) * inv + GetRValue(b) * t);
    int g  = RoundInt(GetGValue(a) * inv + GetGValue(b) * t);
    int bl = RoundInt(GetBValue(a) * inv + GetBValue(b) * t);
    return RGB(ClampByte(r), ClampByte(g), ClampByte(bl));
}

// 将容器基色在 HSL 亮度通道精确加深 deltaL（如 0.08 = 加深 8%）
inline COLORREF DarkenColorL(COLORREF col, double deltaL)
{
    HslColor hsl = RgbToHsl(col);
    hsl.l = ClampVal(hsl.l - deltaL, 0.04, 0.94);
    return HslToRgb(hsl);
}

// sRGB -> 线性亮度查找表（256 项初始化一次）
inline double SrgbByteToLinear(int ch)
{
    static double table[256];
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < 256; ++i) {
            double c = i / 255.0;
            if (c <= 0.04045) {
                table[i] = c / 12.92;
            } else {
                double x = (c + 0.055) / 1.055;
                double x2 = x * x;
                double s2 = SqrtD(x);
                double s4 = SqrtD(s2);
                double s8 = SqrtD(s4);
                double s16 = SqrtD(s8);
                double s32 = SqrtD(s16);
                double s64 = SqrtD(s32);
                double s128 = SqrtD(s64);
                table[i] = x2 * s4 * s8 * s64 * s128; // x^2.3984375 ≈ x^2.4
            }
        }
        initialized = true;
    }
    return table[ClampVal(ch, 0, 255)];
}

inline double RelativeLuminance(COLORREF c)
{
    return 0.2126 * SrgbByteToLinear(GetRValue(c)) +
           0.7152 * SrgbByteToLinear(GetGValue(c)) +
           0.0722 * SrgbByteToLinear(GetBValue(c));
}

inline double ContrastRatio(COLORREF c1, COLORREF c2)
{
    double y1 = RelativeLuminance(c1);
    double y2 = RelativeLuminance(c2);
    double hi = MaxVal(y1, y2);
    double lo = MinVal(y1, y2);
    return (hi + 0.05) / (lo + 0.05);
}

// ----------------------------------------------------------------------------
// 层级提色公式：
//   1. 依据节点类型选基色：Free Space 绿、Unknown 土黄、文件夹蓝、文件按分类或默认色。
//   2. 按深/浅色主题设定第 1 层基准亮度，使父容器偏深沉稳、文件相对明快。
//   3. 第 n 层（levels = depth - 1 >= 1）在父色基础上逐层提亮：
//        step = (lc == 0) ? 0.0 : (0.08 + 0.08 * (lc / 50.0))
//        （默认 levelContrast = 10 时 step = 9.6%，严格满足“8% + levelContrast 调节”）
//        totalLift = min(0.60, levels * step)   （总提亮幅度上限 60%）
//        L_n = min(maxL, L_base + totalLift * (1.0 - L_base))
//   4. 容器标题带再额外加深 8%（DarkenColorL(fill, 0.08)），因此视觉上严格形成：
//        容器标题带 < 容器外框 < 子项（越深越亮，层次一目了然）。
// ----------------------------------------------------------------------------
inline COLORREF ComputeBoxColor(const ss::Node& node, int depth, const TreemapStyle& style)
{
    bool isDrive   = IsKindDrive(node.kind);
    bool isFolder  = IsKindFolder(node.kind);
    bool isFree    = IsKindFree(node.kind);
    bool isUnknown = IsKindUnknown(node.kind);

    COLORREF base = style.fileColor;
    bool usedClassColor = false;
    if (isDrive) {
        base = style.driveColor;
    } else if (isFolder) {
        base = style.folderColor;
    } else if (isFree) {
        base = style.freeColor;
    } else if (isUnknown) {
        base = style.unknownColor;
    } else {
        base = style.fileColor;
        if (style.showFileClassColors && style.classes != nullptr) {
            int g = static_cast<int>(node.group);
            if (g >= 0 && static_cast<size_t>(g) < style.classes->size()) {
                base = (*style.classes)[static_cast<size_t>(g)].color;
                usedClassColor = true;
            }
        }
    }

    HslColor hsl = RgbToHsl(base);
    if (style.dark) {
        // 深色主题：压低顶层文件夹亮度以形成清晰“盒子底色”，保持文件相对明亮易区分
        if (isFolder || isDrive) {
            hsl.l = ClampVal(hsl.l * 0.70, 0.24, 0.42);
            hsl.s = ClampVal(hsl.s * 1.04, 0.0, 0.65);
        } else if (isFree || isUnknown) {
            hsl.l = ClampVal(hsl.l * 1.06, 0.25, 0.40);
            hsl.s = ClampVal(hsl.s * 0.96, 0.0, 0.55);
        } else {
            hsl.s *= 0.88;
            hsl.l = ClampVal(hsl.l * (usedClassColor ? 0.86 : 0.84), 0.32, 0.54);
        }
    } else {
        // 浅色主题：整体通透明快，Free Space 与 Unknown 适当提亮避免黑块感
        if (isFolder || isDrive) {
            hsl.l = ClampVal(hsl.l * 1.00, 0.44, 0.56);
            hsl.s = ClampVal(hsl.s * 1.02, 0.0, 0.68);
        } else if (isFree) {
            hsl.l = ClampVal(hsl.l * 1.55, 0.38, 0.52);
            hsl.s = ClampVal(hsl.s * 1.10, 0.0, 0.55);
        } else if (isUnknown) {
            hsl.l = ClampVal(hsl.l * 1.75, 0.44, 0.58);
            hsl.s = ClampVal(hsl.s * 1.10, 0.0, 0.55);
        } else {
            hsl.l = ClampVal(hsl.l * (usedClassColor ? 1.12 : 1.08), 0.52, 0.68);
        }
    }

    int lc = ClampVal(style.levelContrast, 0, 50);
    int levels = ClampVal(depth - 1, 0, 16);
    if (lc > 0 && levels > 0) {
        double step = 0.08 + 0.08 * (static_cast<double>(lc) / 50.0);
        double totalLift = MinVal(0.60, static_cast<double>(levels) * step);
        double maxL = style.dark ? 0.80 : 0.88;
        double lifted = hsl.l + totalLift * (1.0 - hsl.l);
        if (hsl.l < maxL) {
            hsl.l = MinVal(lifted, maxL);
        }
    }

    return HslToRgb(hsl);
}

// 自动计算与背景对比度 >= 4.5:1 的主文字色
inline COLORREF ComputeLabelPrimaryColor(COLORREF fillCol)
{
    const COLORREF kWhite = RGB(0xFF, 0xFF, 0xFF);
    const COLORREF kInk   = RGB(0x12, 0x14, 0x1A);
    const double kTargetCR = 4.75;

    if (ContrastRatio(kWhite, fillCol) >= 4.5) {
        double lo = 0.72;
        double hi = 1.0;
        COLORREF candLo = BlendColor(fillCol, kWhite, lo);
        if (ContrastRatio(candLo, fillCol) >= kTargetCR) return candLo;
        COLORREF best = kWhite;
        for (int i = 0; i < 6; ++i) {
            double mid = 0.5 * (lo + hi);
            COLORREF cand = BlendColor(fillCol, kWhite, mid);
            if (ContrastRatio(cand, fillCol) >= kTargetCR) {
                best = cand;
                hi = mid;
            } else {
                lo = mid;
            }
        }
        return best;
    }

    double lo = 0.70;
    double hi = 1.0;
    COLORREF best = kInk;
    for (int i = 0; i < 6; ++i) {
        double mid = 0.5 * (lo + hi);
        COLORREF cand = BlendColor(fillCol, kInk, mid);
        if (ContrastRatio(cand, fillCol) >= kTargetCR) {
            best = cand;
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return best;
}

// 次要文字色（大小标签）：与主文字拉开轻微层级，同时保持 >= 3.8:1 可读对比度
inline COLORREF ComputeLabelSecondaryColor(COLORREF fillCol, COLORREF primaryCol, COLORREF textDim)
{
    COLORREF mixed = BlendColor(primaryCol, textDim, 0.45);
    if (ContrastRatio(mixed, fillCol) >= 3.8) return mixed;
    double lo = 0.0, hi = 1.0;
    COLORREF best = primaryCol;
    for (int i = 0; i < 5; ++i) {
        double mid = 0.5 * (lo + hi);
        COLORREF cand = BlendColor(mixed, primaryCol, mid);
        if (ContrastRatio(cand, fillCol) >= 3.8) {
            best = cand;
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return best;
}

// ============================================================================
// 静态 GDI 资源缓存（四档字号 10/9/8/7pt + ASCII/CJK 字宽表 + DIB 缓冲）
// - 所有 CreateFontW / CreateCompatibleDC / CreateDIBSection 在析构时配对释放
// - 运行期零每帧字体创建、零 GDI 泄漏
// ============================================================================

constexpr int kFontTierCount = 4; // 0: 10pt, 1: 9pt, 2: 8pt, 3: 7pt

struct GdiCache
{
    int cachedDpi      = 0;
    int cachedBasePt   = 0;

    std::array<HFONT, kFontTierCount> regFont{};
    std::array<HFONT, kFontTierCount> semiFont{};
    std::array<int,   kFontTierCount> fontH{};
    std::array<int,   kFontTierCount> regEllipsisW{};
    std::array<int,   kFontTierCount> semiEllipsisW{};
    std::array<int,   kFontTierCount> regCjkW{};
    std::array<int,   kFontTierCount> semiCjkW{};
    std::array<int,   kFontTierCount> min3FloorW{};
    std::array<std::array<uint8_t, 128>, kFontTierCount> regCharW{};
    std::array<std::array<uint8_t, 128>, kFontTierCount> semiCharW{};

    HFONT fontTipTitle = nullptr;
    HFONT fontTipBody  = nullptr;
    std::array<HFONT, 2 * kFontTierCount + 2> writeFonts{};
    std::array<ComPtr<IDWriteTextFormat>, 2 * kFontTierCount + 2> writeFormats;
    int   hTipTitle    = 18;
    int   hTipBody     = 15;

    // 1x1 32-bit DIB 用于纯色半透明 AlphaBlend
    HDC       alphaDc     = nullptr;
    HBITMAP   alphaBmp    = nullptr;
    HGDIOBJ   alphaOld    = nullptr;
    uint32_t* alphaBits   = nullptr;

    // 可复用 32-bit 预乘 DIB，用于抗锯齿角标点与圆角阴影 Tooltip
    HDC       scratchDc   = nullptr;
    HBITMAP   scratchBmp  = nullptr;
    HGDIOBJ   scratchOld  = nullptr;
    uint32_t* scratchBits = nullptr;
    int       scratchW    = 0;
    int       scratchH    = 0;

    ~GdiCache()
    {
        DestroyFonts();
        if (alphaDc) {
            if (alphaOld) SelectObject(alphaDc, alphaOld);
            if (alphaBmp) DeleteObject(alphaBmp);
            DeleteDC(alphaDc);
            alphaDc = nullptr;
            alphaBmp = nullptr;
            alphaOld = nullptr;
            alphaBits = nullptr;
        }
        if (scratchDc) {
            if (scratchOld) SelectObject(scratchDc, scratchOld);
            if (scratchBmp) DeleteObject(scratchBmp);
            DeleteDC(scratchDc);
            scratchDc = nullptr;
            scratchBmp = nullptr;
            scratchOld = nullptr;
            scratchBits = nullptr;
        }
    }

    void DestroyFonts()
    {
        writeFonts.fill(nullptr);
        for (auto& format : writeFormats) format.reset();
        auto del = [](HFONT& f) {
            if (f) { DeleteObject(f); f = nullptr; }
        };
        for (int i = 0; i < kFontTierCount; ++i) {
            del(regFont[i]);
            del(semiFont[i]);
        }
        del(fontTipTitle);
        del(fontTipBody);
    }

    static HFONT MakeFont(int pt, int dpi, int weight)
    {
        return CreateFontW(
            -MulDiv(pt, dpi, 72), 0, 0, 0,
            weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }

    static int MeasureFontHeight(HDC dc, HFONT font, int fallback)
    {
        if (!font) return fallback;
        HGDIOBJ old = SelectObject(dc, font);
        TEXTMETRICW tm{};
        int h = GetTextMetricsW(dc, &tm) ? static_cast<int>(tm.tmHeight) : fallback;
        SelectObject(dc, old);
        return MaxVal(1, h);
    }

    static void FillCharWidthTable(HDC dc, HFONT font, int fallbackAvg,
                                   std::array<uint8_t, 128>& outAscii,
                                   int& outCjkW, int& outEllipsisW)
    {
        HGDIOBJ old = SelectObject(dc, font);
        int widths[128]{};
        if (GetCharWidth32W(dc, 0, 127, widths)) {
            for (int c = 0; c < 128; ++c) {
                int w = (widths[c] > 0) ? widths[c] : fallbackAvg;
                outAscii[static_cast<size_t>(c)] = static_cast<uint8_t>(ClampVal(w, 1, 255));
            }
        } else {
            TEXTMETRICW tm{};
            int avg = GetTextMetricsW(dc, &tm) ? MaxVal(3, static_cast<int>(tm.tmAveCharWidth)) : fallbackAvg;
            for (int c = 0; c < 128; ++c) {
                outAscii[static_cast<size_t>(c)] = static_cast<uint8_t>(ClampVal(avg, 1, 255));
            }
        }
        SIZE cjkSz{};
        wchar_t cjkSample = L'中';
        if (GetTextExtentPoint32W(dc, &cjkSample, 1, &cjkSz) && cjkSz.cx > 0) {
            outCjkW = static_cast<int>(cjkSz.cx);
        } else {
            outCjkW = fallbackAvg * 2;
        }
        SIZE ellSz{};
        if (GetTextExtentPoint32W(dc, L"...", 3, &ellSz) && ellSz.cx > 0) {
            outEllipsisW = static_cast<int>(ellSz.cx);
        } else {
            outEllipsisW = MaxVal(6, fallbackAvg);
        }
        SelectObject(dc, old);
    }

    void EnsureFonts(HDC dc, int dpi, int cfgFontPt)
    {
        dpi = (dpi > 0) ? dpi : 96;
        int basePt = (cfgFontPt > 0) ? ClampVal(cfgFontPt, 7, 24) : 9;
        if (dpi == cachedDpi && basePt == cachedBasePt && regFont[0] != nullptr) {
            return;
        }
        DestroyFonts();

        // 默认 fontSize == 9 时四档严格为 10 / 9 / 8 / 7pt；若用户在配置中调整字号则整体平移且不低于 7pt
        int shift = ClampVal(basePt - 9, -2, 6);
        const int tierPts[kFontTierCount] = {
            MaxVal(7, 10 + shift),
            MaxVal(7,  9 + shift),
            MaxVal(7,  8 + shift),
            MaxVal(7,  7 + shift)
        };

        for (int i = 0; i < kFontTierCount; ++i) {
            int pt = tierPts[i];
            regFont[i]  = MakeFont(pt, dpi, FW_NORMAL);
            semiFont[i] = MakeFont(pt, dpi, FW_SEMIBOLD);
            int fallbackH = MaxVal(9, ScalePx(pt + 5, dpi));
            fontH[i] = MeasureFontHeight(dc, regFont[i], fallbackH);
            int fallbackAvg = MaxVal(4, ScalePx(pt - 2, dpi));
            FillCharWidthTable(dc, regFont[i],  fallbackAvg, regCharW[i],  regCjkW[i],  regEllipsisW[i]);
            FillCharWidthTable(dc, semiFont[i], fallbackAvg, semiCharW[i], semiCjkW[i], semiEllipsisW[i]);
            // 快速剔除下限：3 个极窄字符的像素宽
            min3FloorW[i] = MaxVal(6, static_cast<int>(regCharW[i]['i']) * 3);
        }

        fontTipTitle = MakeFont(11, dpi, FW_SEMIBOLD);
        fontTipBody  = MakeFont(9,  dpi, FW_NORMAL);
        hTipTitle    = MeasureFontHeight(dc, fontTipTitle, ScalePx(19, dpi));
        hTipBody     = MeasureFontHeight(dc, fontTipBody,  ScalePx(15, dpi));

        cachedDpi    = dpi;
        cachedBasePt = basePt;
    }

    bool EnsureWriteFormats()
    {
        if (writeFormats.front()) return true;
        static auto factory = [] {
            IDWriteFactory* ptr = nullptr;
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(&ptr));
            return ComPtr<IDWriteFactory>(ptr);
        }();
        if (!factory) return false;

        for (int i = 0; i < kFontTierCount; ++i) {
            writeFonts[2 * i] = regFont[i];
            writeFonts[2 * i + 1] = semiFont[i];
        }
        writeFonts[2 * kFontTierCount] = fontTipTitle;
        writeFonts[2 * kFontTierCount + 1] = fontTipBody;

        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        decltype(writeFormats) formats;
        for (size_t i = 0; i < formats.size(); ++i) {
            LOGFONTW font{};
            if (!GetObjectW(writeFonts[i], sizeof(font), &font)) return false;
            IDWriteTextFormat* rawFormat = nullptr;
            if (FAILED(factory->CreateTextFormat(font.lfFaceName, nullptr,
                    static_cast<DWRITE_FONT_WEIGHT>(font.lfWeight), DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(-font.lfHeight),
                    L"", &rawFormat))) return false;
            formats[i].reset(rawFormat);
            IDWriteInlineObject* rawSign = nullptr;
            if (FAILED(factory->CreateEllipsisTrimmingSign(rawFormat, &rawSign))) return false;
            ComPtr<IDWriteInlineObject> sign(rawSign);
            if (FAILED(rawFormat->SetTrimming(&trim, sign.get())) ||
                FAILED(rawFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))) return false;
        }
        writeFormats = std::move(formats);
        return true;
    }

    bool EnsureAlphaPixel()
    {
        if (alphaDc && alphaBmp && alphaBits) return true;
        alphaDc = CreateCompatibleDC(nullptr);
        if (!alphaDc) return false;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = 1;
        bmi.bmiHeader.biHeight      = -1;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        alphaBmp = CreateDIBSection(alphaDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!alphaBmp || !bits) {
            if (alphaBmp) { DeleteObject(alphaBmp); alphaBmp = nullptr; }
            DeleteDC(alphaDc);
            alphaDc = nullptr;
            return false;
        }
        alphaBits = static_cast<uint32_t*>(bits);
        alphaOld  = SelectObject(alphaDc, alphaBmp);
        return true;
    }

    bool EnsureScratch(int w, int h)
    {
        if (w <= 0 || h <= 0) return false;
        if (scratchDc && scratchBmp && scratchBits && w <= scratchW && h <= scratchH) {
            return true;
        }
        int nw = MaxVal(w, scratchW);
        int nh = MaxVal(h, scratchH);
        if (!scratchDc) {
            scratchDc = CreateCompatibleDC(nullptr);
            if (!scratchDc) return false;
        } else if (scratchBmp) {
            SelectObject(scratchDc, scratchOld);
            DeleteObject(scratchBmp);
            scratchBmp  = nullptr;
            scratchOld  = nullptr;
            scratchBits = nullptr;
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = nw;
        bmi.bmiHeader.biHeight      = -nh;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        scratchBmp = CreateDIBSection(scratchDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!scratchBmp || !bits) {
            if (scratchBmp) { DeleteObject(scratchBmp); scratchBmp = nullptr; }
            scratchW = 0;
            scratchH = 0;
            return false;
        }
        scratchBits = static_cast<uint32_t*>(bits);
        scratchOld  = SelectObject(scratchDc, scratchBmp);
        scratchW    = nw;
        scratchH    = nh;
        return true;
    }
};

inline GdiCache& GetGdiCache()
{
    static GdiCache cache;
    return cache;
}

// 利用缓存字宽表在纯 CPU 侧 O(len) 估算像素宽（单帧零额外 GDI 度量调用）
inline int EstimateTextWidthPx(const GdiCache& cache, int tier, bool semibold,
                               const wchar_t* s, size_t len)
{
    if (!s || len == 0) return 0;
    const auto& tbl = semibold ? cache.semiCharW[tier] : cache.regCharW[tier];
    const int cjkW  = semibold ? cache.semiCjkW[tier]  : cache.regCjkW[tier];
    int sum = 0;
    for (size_t i = 0; i < len; ++i) {
        wchar_t ch = s[i];
        sum += (ch < 128) ? static_cast<int>(tbl[static_cast<size_t>(ch)]) : cjkW;
    }
    return sum;
}

// ----------------------------------------------------------------------------
// 标签字号分档策略（10 / 9 / 8 / 7pt）：
//   1. 高度 < 9px 直接不绘制，避免产生挤扁噪点。
//   2. 先按 10 -> 9 -> 8 -> 7pt 尝试能否放下“完整名字”（优先免省略号展示全名，
//      例如小格子自动缩到 8pt/7pt 完整显示 cas_18.cas）。
//   3. 若即使 7pt 也放不下完整名字，再按 10 -> 9 -> 8 -> 7pt 从大到小试：
//      - 10/9/8pt 需同时容纳前 3 个字符 + 省略号宽度且高度匹配，避免只露出 1 字符；
//      - 最低档 7pt 只要能放下前 min(3, len) 个字符且高度 >= 9px 即绘制（DT_END_ELLIPSIS）。
//   4. 若 7pt 下仍放不下 3 个字符，返回 -1（不绘制）。
// ----------------------------------------------------------------------------
inline int SelectLabelFontTier(const GdiCache& cache, bool semibold,
                               const wchar_t* s, size_t len,
                               int availW, int availH, int minTier = 0)
{
    if (!s || len == 0 || availH < 9 || availW < cache.min3FloorW[kFontTierCount - 1]) {
        return -1;
    }
    minTier = ClampVal(minTier, 0, kFontTierCount - 1);

    auto heightFits = [&](int t) -> bool {
        if (t == kFontTierCount - 1) return availH >= 9;
        return availH >= cache.fontH[t] - 1;
    };

    // Pass A：从大到小找能完整放下全名的最大字号
    for (int t = minTier; t < kFontTierCount; ++t) {
        if (!heightFits(t)) continue;
        if (EstimateTextWidthPx(cache, t, semibold, s, len) <= availW) {
            return t;
        }
    }

    // Pass B：全名放不下，按 10 -> 9 -> 8 -> 7pt 从大到小试能否放下 >= 3 字符
    const size_t prefixLen = MinVal<size_t>(3, len);
    for (int t = minTier; t < kFontTierCount; ++t) {
        if (!heightFits(t)) continue;
        int w3 = EstimateTextWidthPx(cache, t, semibold, s, prefixLen);
        int ellW = semibold ? cache.semiEllipsisW[t] : cache.regEllipsisW[t];
        int needW = (t < kFontTierCount - 1 && len > prefixLen) ? (w3 + ellW) : w3;
        if (needW <= availW) {
            return t;
        }
    }
    return -1;
}

// ============================================================================
// 高效零泄漏 GDI 绘制原语（DC_BRUSH + AlphaBlend）
// ============================================================================

struct FastBrushState
{
    HBRUSH   brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    COLORREF last  = CLR_INVALID;

    inline void Fill(HDC dc, const RECT& r, COLORREF color)
    {
        if (r.right <= r.left || r.bottom <= r.top) return;
        if (color != last) {
            SetDCBrushColor(dc, color);
            last = color;
        }
        FillRect(dc, &r, brush);
    }
};

inline void FillRectSolid(HDC dc, const RECT& r, COLORREF color)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, brush);
}

inline void FillRectAlpha(HDC dc, const RECT& r, COLORREF color, BYTE alpha)
{
    if (r.right <= r.left || r.bottom <= r.top || alpha == 0) return;
    if (alpha == 255) {
        FillRectSolid(dc, r, color);
        return;
    }
    GdiCache& cache = GetGdiCache();
    if (!cache.EnsureAlphaPixel()) return;

    *cache.alphaBits = (static_cast<uint32_t>(GetRValue(color)) << 16) |
                       (static_cast<uint32_t>(GetGValue(color)) << 8)  |
                        static_cast<uint32_t>(GetBValue(color));

    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat         = 0;
    AlphaBlend(dc, r.left, r.top, RectW(r), RectH(r),
               cache.alphaDc, 0, 0, 1, 1, bf);
}

inline void DrawSolidFrame(HDC dc, const RECT& r, COLORREF color, int thick)
{
    int w = RectW(r);
    int h = RectH(r);
    if (w <= 0 || h <= 0 || thick <= 0) return;
    int t = MinVal(thick, MinVal(w, h));
    if (w <= 2 * t || h <= 2 * t) {
        FillRectSolid(dc, r, color);
        return;
    }
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, color);
    RECT top    { r.left,      r.top,        r.right,    r.top + t    };
    RECT bottom { r.left,      r.bottom - t, r.right,    r.bottom     };
    RECT left   { r.left,      r.top + t,    r.left + t, r.bottom - t };
    RECT right  { r.right - t, r.top + t,    r.right,    r.bottom - t };
    FillRect(dc, &top, brush);
    FillRect(dc, &bottom, brush);
    FillRect(dc, &left, brush);
    FillRect(dc, &right, brush);
}

inline void DrawAlphaFrame1px(HDC dc, const RECT& r, COLORREF color, BYTE alpha)
{
    int w = RectW(r);
    int h = RectH(r);
    if (w <= 0 || h <= 0 || alpha == 0) return;
    if (w <= 2 || h <= 2) {
        FillRectAlpha(dc, r, color, alpha);
        return;
    }
    RECT top    { r.left,      r.top,        r.right,    r.top + 1    };
    RECT bottom { r.left,      r.bottom - 1, r.right,    r.bottom     };
    RECT left   { r.left,      r.top + 1,    r.left + 1, r.bottom - 1 };
    RECT right  { r.right - 1, r.top + 1,    r.right,    r.bottom - 1 };
    FillRectAlpha(dc, top, color, alpha);
    FillRectAlpha(dc, bottom, color, alpha);
    FillRectAlpha(dc, left, color, alpha);
    FillRectAlpha(dc, right, color, alpha);
}

// 支持小数像素宽（1.5px 悬停 / 2.5px 选中）的内描边
inline void DrawFractionalInnerStroke(HDC dc, const RECT& r, COLORREF color,
                                      double pxAt96, int dpi)
{
    double scaled = pxAt96 * static_cast<double>(dpi) / 96.0;
    int fullPx = FloorInt(scaled);
    double frac = scaled - static_cast<double>(fullPx);
    if (fullPx > 0) {
        DrawSolidFrame(dc, r, color, fullPx);
    }
    if (frac > 0.05) {
        RECT inner{ r.left + fullPx, r.top + fullPx, r.right - fullPx, r.bottom - fullPx };
        if (inner.right > inner.left && inner.bottom > inner.top) {
            DrawAlphaFrame1px(dc, inner, color, ClampByte(RoundInt(frac * 255.0)));
        }
    }
}

// 4x4 超采样抗锯齿标签角标圆点
inline void DrawTagDotAA(HDC dc, int dstX, int dstY, int dia, COLORREF fillCol, COLORREF rimCol)
{
    int surfSize = dia + 2;
    GdiCache& cache = GetGdiCache();
    if (!cache.EnsureScratch(surfSize, surfSize)) return;

    double cx = 0.5 * static_cast<double>(surfSize);
    double cy = 0.5 * static_cast<double>(surfSize);
    double rFill = 0.5 * static_cast<double>(dia) - 0.55;
    double rRim  = 0.5 * static_cast<double>(dia) + 0.35;
    double rFill2 = rFill * rFill;
    double rRim2  = rRim * rRim;

    double fr = GetRValue(fillCol), fg = GetGValue(fillCol), fb = GetBValue(fillCol);
    double rr = GetRValue(rimCol),  rg = GetGValue(rimCol),  rb = GetBValue(rimCol);

    for (int y = 0; y < surfSize; ++y) {
        uint32_t* row = cache.scratchBits + y * cache.scratchW;
        for (int x = 0; x < surfSize; ++x) {
            int inFill = 0;
            int inRim  = 0;
            for (int sy = 0; sy < 4; ++sy) {
                double py = static_cast<double>(y) + (sy + 0.5) * 0.25 - cy;
                for (int sx = 0; sx < 4; ++sx) {
                    double px = static_cast<double>(x) + (sx + 0.5) * 0.25 - cx;
                    double d2 = px * px + py * py;
                    if (d2 <= rFill2)     ++inFill;
                    else if (d2 <= rRim2) ++inRim;
                }
            }
            int total = inFill + inRim;
            if (total == 0) {
                row[x] = 0;
                continue;
            }
            double a  = static_cast<double>(total) / 16.0;
            double wf = static_cast<double>(inFill) / static_cast<double>(total);
            double wr = 1.0 - wf;
            BYTE ba = ClampByte(RoundInt(a * 255.0));
            BYTE br = MinVal(ba, ClampByte(RoundInt((fr * wf + rr * wr) * a)));
            BYTE bg = MinVal(ba, ClampByte(RoundInt((fg * wf + rg * wr) * a)));
            BYTE bb = MinVal(ba, ClampByte(RoundInt((fb * wf + rb * wr) * a)));
            row[x] = (static_cast<uint32_t>(ba) << 24) |
                     (static_cast<uint32_t>(br) << 16) |
                     (static_cast<uint32_t>(bg) << 8)  |
                      static_cast<uint32_t>(bb);
        }
    }

    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    AlphaBlend(dc, dstX - 1, dstY - 1, surfSize, surfSize,
               cache.scratchDc, 0, 0, surfSize, surfSize, bf);
}

inline COLORREF TagBadgeColor(int tag)
{
    switch (tag) {
        case 1:  return RGB(0xEF, 0x44, 0x44); // 红
        case 2:  return RGB(0xF5, 0xB8, 0x2E); // 黄
        case 3:  return RGB(0x34, 0xC7, 0x59); // 绿
        case 4:  return RGB(0x3B, 0x82, 0xF6); // 蓝
        default: return RGB(0x00, 0x00, 0x00);
    }
}

// ============================================================================
// 零每帧分配渲染暂存缓冲（SnappedBox + 开放定址节点哈希表）
// ============================================================================

struct SnappedBox
{
    uint32_t node;
    int      depth;
    RECT     rc;                // 整数像素外框
    COLORREF fill;              // 容器/叶子基色
    COLORREF headerBg;          // 容器标题带背景色（fill 加深 8%）
    bool     hasDrawnChildren;  // 是否有子节点在当前帧被绘制
};

struct FlatNodeMap
{
    std::vector<uint32_t> keys;
    std::vector<uint32_t> vals;
    uint32_t mask = 0;

    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

    static inline uint32_t HashU32(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    }

    void Reset(size_t expectedCount)
    {
        size_t cap = 64;
        while (cap < expectedCount * 2 + 8) cap <<= 1;
        keys.assign(cap, kEmpty);
        vals.resize(cap);
        mask = static_cast<uint32_t>(cap - 1);
    }

    inline void Insert(uint32_t key, uint32_t val)
    {
        uint32_t slot = HashU32(key) & mask;
        while (keys[slot] != kEmpty && keys[slot] != key) {
            slot = (slot + 1) & mask;
        }
        keys[slot] = key;
        vals[slot] = val;
    }

    inline bool Find(uint32_t key, uint32_t& outVal) const
    {
        if (keys.empty()) return false;
        uint32_t slot = HashU32(key) & mask;
        while (keys[slot] != kEmpty) {
            if (keys[slot] == key) {
                outVal = vals[slot];
                return true;
            }
            slot = (slot + 1) & mask;
        }
        return false;
    }
};

struct RenderScratch
{
    std::vector<SnappedBox> items;
    FlatNodeMap             nodeToItem;
};

inline RenderScratch& GetRenderScratch()
{
    static RenderScratch scratch;
    return scratch;
}

// ============================================================================
// Tooltip 卡片 SDF 圆角 + 柔和阴影渲染 + 时间/年龄格式化
// ============================================================================

inline double SdRoundBox(double px, double py, double cx, double cy,
                         double hx, double hy, double r)
{
    double dx = AbsD(px - cx) - (hx - r);
    double dy = AbsD(py - cy) - (hy - r);
    double ox = MaxVal(dx, 0.0);
    double oy = MaxVal(dy, 0.0);
    double outDist = (ox > 0.0 && oy > 0.0) ? SqrtD(ox * ox + oy * oy) : (ox + oy);
    double inDist  = MinVal(MaxVal(dx, dy), 0.0);
    return outDist + inDist - r;
}

inline void DrawTooltipCardSurface(HDC dc, const RECT& cardRect, int radiusPx,
                                   int borderPx, int shadowPx,
                                   COLORREF bgCol, COLORREF borderCol, bool dark)
{
    int cardW = RectW(cardRect);
    int cardH = RectH(cardRect);
    if (cardW <= 0 || cardH <= 0) return;

    int pad = shadowPx + 3;
    int surfW = cardW + pad * 2;
    int surfH = cardH + pad * 2;

    GdiCache& cache = GetGdiCache();
    if (!cache.EnsureScratch(surfW, surfH)) {
        FillRectSolid(dc, cardRect, bgCol);
        DrawSolidFrame(dc, cardRect, borderCol, borderPx);
        return;
    }

    double cx = static_cast<double>(pad) + 0.5 * static_cast<double>(cardW);
    double cy = static_cast<double>(pad) + 0.5 * static_cast<double>(cardH);
    double hx = 0.5 * static_cast<double>(cardW);
    double hy = 0.5 * static_cast<double>(cardH);
    double rad = MinVal(static_cast<double>(radiusPx), MinVal(hx, hy));
    double bPx = static_cast<double>(MaxVal(1, borderPx));

    double shOffY = static_cast<double>(MaxVal(1, shadowPx));
    double shSpread = static_cast<double>(shadowPx) + 2.5;
    double shMaxAlpha = dark ? 0.42 : 0.22;

    double bgR = GetRValue(bgCol), bgG = GetGValue(bgCol), bgB = GetBValue(bgCol);
    double bdR = GetRValue(borderCol), bdG = GetGValue(borderCol), bdB = GetBValue(borderCol);

    for (int y = 0; y < surfH; ++y) {
        uint32_t* row = cache.scratchBits + y * cache.scratchW;
        double py = static_cast<double>(y) + 0.5;
        for (int x = 0; x < surfW; ++x) {
            double px = static_cast<double>(x) + 0.5;

            double sdCard = SdRoundBox(px, py, cx, cy, hx, hy, rad);
            double cardA = ClampVal(0.5 - sdCard, 0.0, 1.0);

            double sdSh = SdRoundBox(px, py, cx, cy + shOffY, hx, hy, rad);
            double shA = 0.0;
            if (sdSh < shSpread) {
                double t = ClampVal(1.0 - MaxVal(sdSh, 0.0) / shSpread, 0.0, 1.0);
                shA = shMaxAlpha * t * t;
            }

            if (cardA <= 0.0 && shA <= 0.0) {
                row[x] = 0;
                continue;
            }

            double borderMix = ClampVal(sdCard + bPx + 0.5, 0.0, 1.0);
            double cr = bgR * (1.0 - borderMix) + bdR * borderMix;
            double cg = bgG * (1.0 - borderMix) + bdG * borderMix;
            double cb = bgB * (1.0 - borderMix) + bdB * borderMix;

            double outA = cardA + shA * (1.0 - cardA);
            BYTE ba = ClampByte(RoundInt(outA * 255.0));
            BYTE br = MinVal(ba, ClampByte(RoundInt(cr * cardA)));
            BYTE bg = MinVal(ba, ClampByte(RoundInt(cg * cardA)));
            BYTE bb = MinVal(ba, ClampByte(RoundInt(cb * cardA)));

            row[x] = (static_cast<uint32_t>(ba) << 24) |
                     (static_cast<uint32_t>(br) << 16) |
                     (static_cast<uint32_t>(bg) << 8)  |
                      static_cast<uint32_t>(bb);
        }
    }

    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    AlphaBlend(dc, cardRect.left - pad, cardRect.top - pad, surfW, surfH,
               cache.scratchDc, 0, 0, surfW, surfH, bf);
}

inline void AppendPaddedNum(std::wstring& out, unsigned int val, int width)
{
    wchar_t buf[16];
    int n = 0;
    do {
        buf[n++] = static_cast<wchar_t>(L'0' + (val % 10));
        val /= 10;
    } while (val > 0 && n < 16);
    for (int i = n; i < width; ++i) out.push_back(L'0');
    for (int i = n - 1; i >= 0; --i) out.push_back(buf[i]);
}

inline std::wstring FormatFileTimeDate(uint64_t ftU64)
{
    FILETIME ftUtc{};
    ftUtc.dwLowDateTime  = static_cast<DWORD>(ftU64 & 0xFFFFFFFFULL);
    ftUtc.dwHighDateTime = static_cast<DWORD>(ftU64 >> 32);
    FILETIME ftLocal{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ftUtc, &ftLocal) || !FileTimeToSystemTime(&ftLocal, &st)) {
        return {};
    }
    std::wstring s;
    s.reserve(20);
    AppendPaddedNum(s, st.wYear, 4);   s.push_back(L'-');
    AppendPaddedNum(s, st.wMonth, 2);  s.push_back(L'-');
    AppendPaddedNum(s, st.wDay, 2);    s.push_back(L' ');
    AppendPaddedNum(s, st.wHour, 2);   s.push_back(L':');
    AppendPaddedNum(s, st.wMinute, 2); s.push_back(L':');
    AppendPaddedNum(s, st.wSecond, 2);
    return s;
}

inline std::wstring FormatFileTimeAge(uint64_t ftU64)
{
    using ss::i18n::T;
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    uint64_t nowU64 = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) |
                      static_cast<uint64_t>(nowFt.dwLowDateTime);
    if (nowU64 <= ftU64) return T(L"tip.age.just_now");

    uint64_t sec = (nowU64 - ftU64) / 10000000ULL;
    if (sec < 60)       return T(L"tip.age.just_now");
    if (sec < 3600)     return std::to_wstring(sec / 60) + T(L"tip.age.min");
    if (sec < 86400)    return std::to_wstring(sec / 3600) + T(L"tip.age.hour");
    uint64_t days = sec / 86400;
    if (days < 30)      return std::to_wstring(days) + T(L"tip.age.day");
    if (days < 365)     return std::to_wstring(days / 30) + T(L"tip.age.month");
    uint64_t years  = days / 365;
    uint64_t months = (days % 365) / 30;
    if (months > 0) {
        return std::to_wstring(years) + T(L"tip.age.year_space") + std::to_wstring(months) + T(L"tip.age.month");
    }
    return std::to_wstring(years) + T(L"tip.age.year");
}

inline int MeasureTextWidth(HDC dc, HFONT font, const std::wstring& s)
{
    if (s.empty()) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    return static_cast<int>(sz.cx);
}

// ============================================================================
// 模块三：缩放动画（180ms ease-out，节点匹配插值，>3000 盒自动跳过）
//
// 动画插值与跳过条件说明：
//   - 跳过条件：durationMs <= 0（用户关闭动画）、旧/新布局为空，或任一侧盒数 > 3000
//     （规模保护，避免超大目录缩放掉帧）。
//   - 缓动曲线：三次 ease-out：easeT = 1 - (1 - t)^3，t ∈ [0, 1]。
//   - 节点匹配（以树节点索引 node 为 ID）：
//       1. 旧新共存节点：rc = lerp(oldRc, newRc, easeT)。
//       2. 仅新布局节点（展开进入）：
//          - 若 node == oldFocus（返回上层）：从整个画布缩回新位置；
//          - 若某祖先存在于旧布局：按比例映射进该祖先旧矩形内，形成平滑镜头放大展开；
//          - 否则若父节点已在新布局中：随父节点起始矩形同步展开；
//          - 其余顶层新兄弟：从自身中心展开。
//       3. 仅旧布局节点（缩小淡出）：
//          - 向新布局中最近可见祖先的中心（或旧父矩形/自身中心）缩小至 0×0，
//            透明度按 (1 - easeT) 线性淡出。
//   - 动画期间不重算布局，只对预构建轨迹做线性插值；结束后立即销毁 Session 释放内存。
// ============================================================================

constexpr size_t kAnimMaxBoxes = 3000;

struct AnimTrack
{
    uint32_t node = ss::Node::kNone;
    int      depth = 1;
    RectD    start{};
    RectD    end{};
};

struct AnimSession
{
    HWND     hwnd       = nullptr;
    double startMs = 0;
    int durationMs = 180;
    double   easeT      = 0.0;

    std::vector<AnimTrack>  activeTracks; // 匹配节点 + 新展开节点
    std::vector<AnimTrack>  exitTracks;   // 旧布局消失节点（向父中心缩小淡出）
    std::vector<TreemapBox> currentBoxes; // 当前帧插值结果（复用于零拷贝绘制）
};

inline std::vector<AnimSession>& GetAnimSessions()
{
    static std::vector<AnimSession> sessions;
    return sessions;
}

inline AnimSession* FindAnimSession(HWND hwnd)
{
    if (!hwnd) return nullptr;
    auto& sessions = GetAnimSessions();
    for (auto& s : sessions) {
        if (s.hwnd == hwnd) return &s;
    }
    return nullptr;
}

inline void EraseAnimSession(HWND hwnd)
{
    if (!hwnd) return;
    auto& sessions = GetAnimSessions();
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i].hwnd == hwnd) {
            sessions.erase(sessions.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

inline RectD BoxToRectD(const TreemapBox& b)
{
    return { b.x, b.y, b.w, b.h };
}

inline RectD MapProportional(const RectD& childNew, const RectD& parentNew, const RectD& parentStart)
{
    if (parentNew.w <= 1e-3 || parentNew.h <= 1e-3) {
        return { parentStart.x + parentStart.w * 0.5, parentStart.y + parentStart.h * 0.5, 0.0, 0.0 };
    }
    double rx = (childNew.x - parentNew.x) / parentNew.w;
    double ry = (childNew.y - parentNew.y) / parentNew.h;
    double rw = childNew.w / parentNew.w;
    double rh = childNew.h / parentNew.h;
    return {
        parentStart.x + rx * parentStart.w,
        parentStart.y + ry * parentStart.h,
        rw * parentStart.w,
        rh * parentStart.h
    };
}

inline RectD CollapseToCenter(const RectD& r)
{
    return { r.x + r.w * 0.5, r.y + r.h * 0.5, 0.0, 0.0 };
}

inline double LerpD(double a, double b, double t)
{
    return a + (b - a) * t;
}

inline void UpdateAnimCurrentBoxes(AnimSession& sess, double easeT)
{
    sess.easeT = easeT;
    sess.currentBoxes.resize(sess.activeTracks.size());
    for (size_t i = 0; i < sess.activeTracks.size(); ++i) {
        const AnimTrack& tr = sess.activeTracks[i];
        TreemapBox& dst = sess.currentBoxes[i];
        dst.node  = tr.node;
        dst.depth = tr.depth;
        dst.x     = LerpD(tr.start.x, tr.end.x, easeT);
        dst.y     = LerpD(tr.start.y, tr.end.y, easeT);
        dst.w     = MaxVal(0.0, LerpD(tr.start.w, tr.end.w, easeT));
        dst.h     = MaxVal(0.0, LerpD(tr.start.h, tr.end.h, easeT));
    }
}

} // anonymous namespace

// ============================================================================
// 公开接口实现
// ============================================================================

void LayoutTreemap(const ss::Tree& tree, uint32_t focusNode,
                   double x, double y, double w, double h,
                   const TreemapLayoutOptions& opt,
                   std::vector<TreemapBox>& outBoxes)
{
    outBoxes.clear();
    if (!tree.Valid(focusNode) || w <= 0.0 || h <= 0.0) return;
    CollectNestedBoxes(tree, focusNode, 0, x, y, w, h, opt, outBoxes);
}

namespace {

D2D1_RECT_F FloatRect(const RECT& r)
{
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                       static_cast<float>(r.right), static_cast<float>(r.bottom));
}

struct TreemapPaint
{
    HDC dc;
    ID2D1RenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    FastBrushState gdiBrush;
    HFONT font = nullptr;
    COLORREF textColor = CLR_INVALID;
    IDWriteTextFormat* format = nullptr;

    void SetBrush(COLORREF color, BYTE alpha = 255)
    {
        brush->SetColor(D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                                    GetBValue(color) / 255.0f, alpha / 255.0f));
    }

    void Fill(const RECT& r, COLORREF color)
    {
        if (target) {
            SetBrush(color);
            target->FillRectangle(FloatRect(r), brush);
        } else {
            gdiBrush.Fill(dc, r, color);
        }
    }

    void FillAlpha(const RECT& r, COLORREF color, BYTE alpha)
    {
        if (target) {
            SetBrush(color, alpha);
            target->FillRectangle(FloatRect(r), brush);
        } else {
            FillRectAlpha(dc, r, color, alpha);
        }
    }

    void Stroke(const RECT& r, COLORREF color, double width, int dpi)
    {
        if (!target) {
            DrawFractionalInnerStroke(dc, r, color, width, dpi);
            return;
        }
        float half = static_cast<float>(width * dpi / 192.0);
        if (RectW(r) <= 4 * half || RectH(r) <= 4 * half) {
            Fill(r, color);
            return;
        }
        auto rect = FloatRect(r);
        rect.left += half;
        rect.top += half;
        rect.right -= half;
        rect.bottom -= half;
        SetBrush(color);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target->DrawRectangle(rect, brush, 2 * half);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    }

    void Tag(int x, int y, int diameter, COLORREF color, COLORREF rim)
    {
        if (!target) {
            DrawTagDotAA(dc, x, y, diameter, color, rim);
            return;
        }
        float radius = diameter / 2.0f;
        auto center = D2D1::Point2F(x + radius, y + radius);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        SetBrush(rim);
        target->FillEllipse(D2D1::Ellipse(center, radius, radius), brush);
        SetBrush(color);
        target->FillEllipse(D2D1::Ellipse(center, radius - 1, radius - 1), brush);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    }

    void TooltipSurface(const RECT& r, int radius, int border, int shadow,
                        COLORREF background, COLORREF outline, bool dark)
    {
        if (!target) {
            DrawTooltipCardSurface(dc, r, radius, border, shadow, background, outline, dark);
            return;
        }
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto rect = FloatRect(r);
        auto shadowRect = D2D1::RectF(rect.left - shadow, rect.top + shadow,
                                     rect.right + shadow, rect.bottom + 2 * shadow);
        SetBrush(RGB(0, 0, 0), dark ? 105 : 48);
        target->FillRoundedRectangle(D2D1::RoundedRect(shadowRect, radius, radius), brush);
        SetBrush(outline);
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
        auto inner = D2D1::RectF(rect.left + border, rect.top + border,
                                rect.right - border, rect.bottom - border);
        float innerRadius = static_cast<float>(MaxVal(0, radius - border));
        SetBrush(background);
        target->FillRoundedRectangle(D2D1::RoundedRect(inner, innerRadius, innerRadius), brush);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    }

    void Font(HFONT value)
    {
        if (value == font || !value) return;
        font = value;
        if (!target) {
            SelectObject(dc, value);
            return;
        }
        auto& cache = GetGdiCache();
        auto it = std::find(cache.writeFonts.begin(), cache.writeFonts.end(), value);
        format = it == cache.writeFonts.end() ? nullptr
            : cache.writeFormats[it - cache.writeFonts.begin()].get();
    }

    void TextColor(COLORREF value)
    {
        if (value == textColor) return;
        textColor = value;
        if (!target) SetTextColor(dc, value);
    }

    void Text(const wchar_t* value, int length, RECT r, UINT flags)
    {
        if (!target) {
            DrawTextW(dc, value, length, &r, flags);
            return;
        }
        if (!format || !length || r.right <= r.left || r.bottom <= r.top) return;
        format->SetTextAlignment(flags & DT_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER
            : flags & DT_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(flags & DT_VCENTER ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
            : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        SetBrush(textColor);
        // Preserve the tail of long paths in the tooltip.
        if (flags & DT_PATH_ELLIPSIS) {
            DWRITE_TRIMMING trim{};
            ComPtr<IDWriteInlineObject> sign;
            IDWriteInlineObject* rawSign = nullptr;
            format->GetTrimming(&trim, &rawSign);
            sign.reset(rawSign);
            trim.delimiter = L'\\';
            trim.delimiterCount = 1;
            if (sign) format->SetTrimming(&trim, sign.get());
            target->DrawText(value, length, format, FloatRect(r), brush,
                             D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_GDI_CLASSIC);
            trim.delimiter = 0;
            trim.delimiterCount = 0;
            if (sign) format->SetTrimming(&trim, sign.get());
            return;
        }
        target->DrawText(value, length, format, FloatRect(r), brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_GDI_CLASSIC);
    }
};

void DrawTreemap(TreemapPaint& paint, const RECT& area, const ss::Tree& tree,
                 const std::vector<TreemapBox>& boxes, const TreemapStyle& style,
                 uint32_t hoverNode, uint32_t selectedNode)
{
    paint.Fill(area, style.bg);
    if (boxes.empty()) return;

    const int dpi = (style.dpi > 0) ? style.dpi : 96;
    GdiCache& cache = GetGdiCache();

    // 1) 过滤可见矩形、整数对齐（Round(x+w) 消除相邻拼缝），并复用静态暂存缓冲
    RenderScratch& scratch = GetRenderScratch();
    auto& items = scratch.items;
    items.clear();
    items.reserve(boxes.size());

    for (size_t i = 0; i < boxes.size(); ++i) {
        const TreemapBox& b = boxes[i];
        if (!tree.Valid(b.node)) continue;
        if (!(b.w > 0.0 && b.h > 0.0 && b.w < 1e7 && b.h < 1e7 &&
              b.x > -1e7 && b.x < 1e7 && b.y > -1e7 && b.y < 1e7)) {
            continue;
        }

        RECT rc{
            RoundInt(b.x),
            RoundInt(b.y),
            RoundInt(b.x + b.w),
            RoundInt(b.y + b.h)
        };
        if (rc.right <= rc.left || rc.bottom <= rc.top) continue;
        if (!RectIntersects(rc, area)) continue;

        int depth = MaxVal(1, b.depth);
        const ss::Node& n = tree.nodes[b.node];
        COLORREF fill = ComputeBoxColor(n, depth, style);

        SnappedBox sb{};
        sb.node             = b.node;
        sb.depth            = depth;
        sb.rc               = rc;
        sb.fill             = fill;
        sb.headerBg         = DarkenColorL(fill, 0.08); // 标题带背景：容器基色加深 8%
        sb.hasDrawnChildren = false;
        items.push_back(sb);
    }

    if (items.empty()) return;

    // 按 depth 升序稳定排序：确保父容器先画、子项后画
    std::stable_sort(items.begin(), items.end(), [](const SnappedBox& a, const SnappedBox& b) {
        return a.depth < b.depth;
    });

    // O(N) 标记哪些容器本帧有子项被绘制（复用开放定址哈希表，零每帧大分配）
    scratch.nodeToItem.Reset(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        scratch.nodeToItem.Insert(items[i].node, static_cast<uint32_t>(i));
    }
    for (size_t i = 0; i < items.size(); ++i) {
        uint32_t parentNode = tree.nodes[items[i].node].parent;
        if (parentNode == ss::Node::kNone) continue;
        uint32_t parentIdx = 0;
        if (scratch.nodeToItem.Find(parentNode, parentIdx)) {
            items[parentIdx].hasDrawnChildren = true;
        }
    }

    // ------------------------------------------------------------------------
    // Pass 1：盒体填色 + 外框 + 标题带 + 子区内侧 1px 深色缝（“盒子套盒子”）
    //   性能优化：普通矩形仅需 2 次 FillRect（先画外边框色再画内缩 1px 填充色），
    //   比 1 填色 + 4 边框减少 60% GDI 调用，9000 盒单帧稳控在 < 15ms。
    // ------------------------------------------------------------------------
    const int bc = ClampVal(style.borderContrast, 0, 50);
    const double outerSeamStr = (bc <= 0) ? 0.0 : (0.34 + 0.50 * (static_cast<double>(bc) / 50.0));
    const double innerSeamStr = (bc <= 0) ? 0.22 : MinVal(0.90, outerSeamStr + 0.14);
    const COLORREF seamTarget = style.dark ? RGB(0x0A, 0x0B, 0x0F) : RGB(0x1E, 0x22, 0x2A);

    int hoverItemIdx = -1;
    int selectedItemIdx = -1;

    for (size_t i = 0; i < items.size(); ++i) {
        const SnappedBox& sb = items[i];
        if (sb.node == hoverNode && hoverNode != ss::Node::kNone) {
            hoverItemIdx = static_cast<int>(i);
        }
        if (sb.node == selectedNode && selectedNode != ss::Node::kNone) {
            selectedItemIdx = static_cast<int>(i);
        }

        int w = RectW(sb.rc);
        int h = RectH(sb.rc);

        if (bc > 0 && w >= 3 && h >= 3) {
            COLORREF borderCol = BlendColor(sb.fill, seamTarget, outerSeamStr);
            paint.Fill(sb.rc, borderCol);
            RECT inner{ sb.rc.left + 1, sb.rc.top + 1, sb.rc.right - 1, sb.rc.bottom - 1 };
            paint.Fill(inner, sb.fill);
        } else {
            paint.Fill(sb.rc, sb.fill);
        }

        if (sb.hasDrawnChildren && w >= 8 && h >= 18) {
            ContainerGeometry geom = ComputeContainerGeometry(sb.rc, sb.depth, dpi);

            // 1) 顶部标题带（加深 8%）
            if (geom.headerRect.right > geom.headerRect.left &&
                geom.headerRect.bottom > geom.headerRect.top) {
                paint.Fill(geom.headerRect, sb.headerBg);
            }

            // 2) 容器子区边界 1px 深色缝（紧贴外框彩色边内侧，与子项外框对齐）
            int ww = RectW(geom.childWellRect);
            int wh = RectH(geom.childWellRect);
            if (ww >= 3 && wh >= 3) {
                COLORREF innerSeamCol = BlendColor(sb.fill, seamTarget, innerSeamStr);
                paint.Fill(geom.childWellRect, innerSeamCol);
                RECT wellInner{
                    geom.childWellRect.left + 1,
                    geom.childWellRect.top + 1,
                    geom.childWellRect.right - 1,
                    geom.childWellRect.bottom - 1
                };
                COLORREF wellBg = BlendColor(sb.fill, style.bg, style.dark ? 0.16 : 0.10);
                paint.Fill(wellInner, wellBg);
            }
        }
    }

    // ------------------------------------------------------------------------
    // Pass 2：四档自适应标签绘制与标签角标（tag 1..4）
    //   - 容器（hasDrawnChildren）：标题带左对齐 semibold 名字 + 右对齐小一号大小
    //   - 叶子（!hasDrawnChildren）：整块垂直+水平居中绘制（名字 + 可选第二行大小）
    // ------------------------------------------------------------------------

    const int hdrPadL     = ScalePx(4, dpi);   // 标题带左侧 4px 内边距
    const int hdrPadR     = ScalePx(4, dpi);
    const int hdrSizeGap  = ScalePx(6, dpi);
    const int hdrSizeMinSpare = ScalePx(60, dpi); // 右侧大小要求至少 60px 余量
    const int dotDia      = MaxVal(5, ScalePx(7, dpi));
    const int dotMargin   = ScalePx(4, dpi);
    const COLORREF dotRim = style.dark ? RGB(0x10, 0x10, 0x16) : RGB(0xFF, 0xFF, 0xFF);

    for (size_t i = 0; i < items.size(); ++i) {
        const SnappedBox& sb = items[i];
        const ss::Node& n = tree.nodes[sb.node];
        int w = RectW(sb.rc);
        int h = RectH(sb.rc);
        int tag = static_cast<int>(n.tag);

        WStrSpan nameSpan = GetNodeNameSpan(tree, sb.node);

        if (sb.hasDrawnChildren) {
            // ---- 容器：仅在顶部标题带绘制名字、右侧大小与可选标签角标 ----
            // 若动画缩放过程中容器被压扁至不足以绘制标题带，则跳过以免文字覆盖子项
            if (w < 8 || h < 18) continue;
            ContainerGeometry geom = ComputeContainerGeometry(sb.rc, sb.depth, dpi);
            const RECT& hdr = geom.headerRect;
            int hdrW = RectW(hdr);
            int hdrH = RectH(hdr);
            if (hdrW <= 0 || hdrH < 9) continue;

            int rightEdge = hdr.right - hdrPadR;
            if (tag >= 1 && tag <= 4 && hdrW >= dotDia + hdrPadR * 2 + 8 && hdrH >= dotDia + 2) {
                int bx = hdr.right - hdrPadR - dotDia;
                int by = hdr.top + (hdrH - dotDia) / 2;
                paint.Tag(bx, by, dotDia, TagBadgeColor(tag), dotRim);
                rightEdge = bx - hdrPadR;
            }

            int leftEdge = hdr.left + hdrPadL;
            int availW = rightEdge - leftEdge;
            if (availW < cache.min3FloorW[kFontTierCount - 1] || nameSpan.len == 0) continue;

            // 容器标题带优先使用 9/8/7pt（顶层 19px 时可上到 10pt），永远尝试画名字
            int minTier = (sb.depth <= 1 && hdrH >= cache.fontH[0]) ? 0 : 1;
            int nameTier = SelectLabelFontTier(cache, true, nameSpan.data, nameSpan.len,
                                               availW, hdrH, minTier);
            if (nameTier < 0 && minTier > 0) {
                nameTier = SelectLabelFontTier(cache, true, nameSpan.data, nameSpan.len,
                                               availW, hdrH, 0);
            }
            if (nameTier < 0) continue;

            COLORREF primaryCol = ComputeLabelPrimaryColor(sb.headerBg);

            // 检查是否绘制右对齐大小（无 60px 余量时省略）
            int nameRight = rightEdge;
            if (availW >= hdrSizeMinSpare + ScalePx(28, dpi)) {
                int sizeTier = MinVal(kFontTierCount - 1, nameTier + 1);
                std::wstring szText = ss::FmtSize(n.disk);
                int szW = EstimateTextWidthPx(cache, sizeTier, false, szText.c_str(), szText.size());
                int szBoxW = szW + ScalePx(4, dpi); // 留出 ClearType 微小余量避免左边缘裁剪
                int fullNameW = EstimateTextWidthPx(cache, nameTier, true, nameSpan.data, nameSpan.len);

                bool enoughSpare =
                    (availW >= szBoxW + hdrSizeGap + hdrSizeMinSpare) ||
                    (availW >= fullNameW + hdrSizeGap + szBoxW && (availW - fullNameW) >= hdrSizeMinSpare);

                if (enoughSpare && szW > 0 && szBoxW < availW - hdrSizeGap) {
                    int szLeft = MaxVal(leftEdge, rightEdge - szBoxW);
                    RECT szRc{ szLeft, hdr.top, rightEdge, hdr.bottom };
                    COLORREF secCol = ComputeLabelSecondaryColor(sb.headerBg, primaryCol, style.textDim);
                    paint.Font(cache.regFont[sizeTier]);
                    paint.TextColor(secCol);
                    paint.Text(szText.c_str(), static_cast<int>(szText.size()), szRc,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    nameRight = szRc.left - hdrSizeGap;
                }
            }

            // 若因绘制大小导致名字区变窄，重新确保名字字号适配剩余宽度
            int nameAvailW = nameRight - leftEdge;
            if (nameAvailW < availW) {
                int refitTier = SelectLabelFontTier(cache, true, nameSpan.data, nameSpan.len,
                                                    nameAvailW, hdrH, minTier);
                if (refitTier >= 0) nameTier = refitTier;
            }

            if (nameAvailW >= cache.min3FloorW[kFontTierCount - 1]) {
                RECT nameRc{ leftEdge, hdr.top, nameRight, hdr.bottom };
                paint.Font(cache.semiFont[nameTier]);
                paint.TextColor(primaryCol);
                paint.Text(nameSpan.data, static_cast<int>(nameSpan.len), nameRc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            continue;
        }

        // ---- 叶子节点：标签角标（右上角）+ 名字与大小整块垂直居中绘制 ----
        bool drewLeafBadge = false;
        if (tag >= 1 && tag <= 4 && w >= dotDia + dotMargin * 2 && h >= dotDia + dotMargin * 2) {
            int bx = sb.rc.right - dotMargin - dotDia;
            int by = sb.rc.top + dotMargin;
            paint.Tag(bx, by, dotDia, TagBadgeColor(tag), dotRim);
            drewLeafBadge = true;
        }

        // 快速高度/宽度剪枝（高度 < 9px 或极窄直接跳过，不创建任何字符串或计算色彩）
        int padX = (w >= 44) ? ScalePx(4, dpi) : ((w >= 24) ? 2 : 1);
        int padY = (h >= 24) ? ScalePx(2, dpi) : 1;
        int innerL = sb.rc.left + padX;
        int innerR = sb.rc.right - padX;
        if (drewLeafBadge && h < (dotDia + dotMargin) * 2 + cache.fontH[kFontTierCount - 1]) {
            innerR -= (dotDia + dotMargin);
        }
        int innerT = sb.rc.top + padY;
        int innerB = sb.rc.bottom - padY;
        int availW = innerR - innerL;
        int availH = innerB - innerT;
        if (availH < 9 || availW < cache.min3FloorW[kFontTierCount - 1] || nameSpan.len == 0) {
            continue;
        }

        bool isFolderLeaf = IsKindFolder(n.kind) || IsKindDrive(n.kind);
        int nameTier = SelectLabelFontTier(cache, isFolderLeaf, nameSpan.data, nameSpan.len,
                                           availW, availH, 0);
        if (nameTier < 0) continue;

        COLORREF primaryCol = ComputeLabelPrimaryColor(sb.fill);
        HFONT nameFont = isFolderLeaf ? cache.semiFont[nameTier] : cache.regFont[nameTier];
        int nameH = cache.fontH[nameTier];

        // 判断是否有足够纵向与横向空间显示第二行大小（小一号字号）
        int sizeTier = MinVal(kFontTierCount - 1, nameTier + 1);
        int sizeH    = cache.fontH[sizeTier];
        int lineGap  = (availH >= nameH + sizeH + 4) ? ScalePx(1, dpi) : 0;
        bool drawTwoLines = false;
        std::wstring szText;

        if (availH >= nameH + lineGap + sizeH && availW >= ScalePx(28, dpi)) {
            szText = ss::FmtSize(n.disk);
            if (!szText.empty()) {
                int szW = EstimateTextWidthPx(cache, sizeTier, false, szText.c_str(), szText.size());
                // 若当前字号装不下完整大小，则再降一档到 7pt 尝试
                if (szW > availW && sizeTier < kFontTierCount - 1) {
                    sizeTier = kFontTierCount - 1;
                    sizeH = cache.fontH[sizeTier];
                    szW = EstimateTextWidthPx(cache, sizeTier, false, szText.c_str(), szText.size());
                }
                if (szW <= availW + 4 && availH >= nameH + lineGap + sizeH) {
                    drawTwoLines = true;
                }
            }
        }

        if (drawTwoLines) {
            int totalH = nameH + lineGap + sizeH;
            int startY = innerT + (availH - totalH) / 2;

            RECT r1{ innerL, startY, innerR, startY + nameH };
            paint.Font(nameFont);
            paint.TextColor(primaryCol);
            paint.Text(nameSpan.data, static_cast<int>(nameSpan.len), r1,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            RECT r2{ innerL, startY + nameH + lineGap, innerR, startY + totalH };
            COLORREF secCol = ComputeLabelSecondaryColor(sb.fill, primaryCol, style.textDim);
            paint.Font(cache.regFont[sizeTier]);
            paint.TextColor(secCol);
            paint.Text(szText.c_str(), static_cast<int>(szText.size()), r2,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            // 单行垂直+水平居中
            RECT r1{ innerL, innerT, innerR, innerB };
            paint.Font(nameFont);
            paint.TextColor(primaryCol);
            paint.Text(nameSpan.data, static_cast<int>(nameSpan.len), r1,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    // ------------------------------------------------------------------------
    // Pass 3：悬停（轻度高光 + 1.5px 白/黑描边）与选中（2.5px 强调色描边）
    // ------------------------------------------------------------------------
    if (hoverItemIdx >= 0) {
        const SnappedBox& hb = items[static_cast<size_t>(hoverItemIdx)];
        RECT clipped{
            MaxVal(hb.rc.left, area.left),
            MaxVal(hb.rc.top, area.top),
            MinVal(hb.rc.right, area.right),
            MinVal(hb.rc.bottom, area.bottom)
        };
        COLORREF tintCol   = style.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
        BYTE     tintAlpha = style.dark ? 26 : 18;
        paint.FillAlpha(clipped, tintCol, tintAlpha);

        COLORREF hoverStroke = style.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x14, 0x14, 0x18);
        paint.Stroke(hb.rc, hoverStroke, 1.5, dpi);
    }

    if (selectedItemIdx >= 0) {
        const SnappedBox& sb = items[static_cast<size_t>(selectedItemIdx)];
        paint.Stroke(sb.rc, style.accent, 2.5, dpi);
    }

}

void DrawTreemapTooltip(TreemapPaint& paint, const RECT& area, const ss::Tree& tree, uint32_t node,
                        int mouseX, int mouseY, const TreemapStyle& style,
                        bool showDateTime, bool showAge)
{
    if (node == ss::Node::kNone || !tree.Valid(node)) return;
    if (area.right <= area.left || area.bottom <= area.top) return;

    const int dpi = (style.dpi > 0) ? style.dpi : 96;
    GdiCache& cache = GetGdiCache();

    const ss::Node& n = tree.nodes[node];

    std::wstring line1 = ToWString(tree.Name(node));
    std::wstring line2 = GetNodeFullPath(tree, node);

    uint32_t childCount = 0;
    uint32_t c = n.firstChild;
    while (c != ss::Node::kNone && tree.Valid(c) && childCount < tree.nodes.size()) {
        ++childCount;
        c = tree.nodes[c].nextSibling;
    }

    using ss::i18n::T;
    std::wstring line3 = std::wstring(T(L"tip.disk_size")) + ToWString(ss::FmtSize(n.disk)) +
                         T(L"tip.logical_size") + ToWString(ss::FmtSize(n.size));
    if (IsKindFolder(n.kind) || IsKindDrive(n.kind) || childCount > 0) {
        line3 += L" · " + std::to_wstring(childCount) + T(L"tip.children_suffix");
    }

    std::wstring line4;
    if (showDateTime || showAge) {
        uint64_t mtime = GetNodeModTime(n);
        if (mtime != 0) {
            std::wstring dtStr  = showDateTime ? FormatFileTimeDate(mtime) : std::wstring();
            std::wstring ageStr = showAge      ? FormatFileTimeAge(mtime)  : std::wstring();
            if (!dtStr.empty() && !ageStr.empty()) {
                line4 = std::wstring(T(L"tip.modified")) + dtStr + T(L"tip.age_open") + ageStr + T(L"tip.age_close");
            } else if (!dtStr.empty()) {
                line4 = std::wstring(T(L"tip.modified")) + dtStr;
            } else if (!ageStr.empty()) {
                line4 = std::wstring(T(L"tip.age_prefix")) + ageStr;
            }
        }
    }

    const int edgeMargin = ScalePx(8, dpi);
    const int padX       = ScalePx(12, dpi);
    const int padY       = ScalePx(10, dpi);
    const int gap12      = ScalePx(3, dpi);
    const int gap23      = ScalePx(6, dpi);
    const int gap34      = ScalePx(3, dpi);
    const int radiusPx   = ScalePx(6, dpi);
    const int borderPx   = MaxVal(1, ScalePx(1, dpi));
    const int shadowPx   = MaxVal(1, ScalePx(2, dpi));
    const int offX       = ScalePx(14, dpi);
    const int offY       = ScalePx(18, dpi);

    int areaW = RectW(area);
    int areaH = RectH(area);
    if (areaW < ScalePx(60, dpi) || areaH < ScalePx(40, dpi)) return;
    int maxCardW = MinVal(ScalePx(460, dpi), MaxVal(ScalePx(120, dpi), areaW - edgeMargin * 2));

    int tag = static_cast<int>(n.tag);
    bool showTagDot = (tag >= 1 && tag <= 4);
    int dotDia = MaxVal(5, ScalePx(7, dpi));
    int titleExtraW = showTagDot ? (dotDia + ScalePx(8, dpi)) : 0;

    int w1 = MeasureTextWidth(paint.dc, cache.fontTipTitle, line1) + titleExtraW;
    int w2 = MeasureTextWidth(paint.dc, cache.fontTipBody,  line2);
    int w3 = MeasureTextWidth(paint.dc, cache.fontTipBody,  line3);
    int w4 = MeasureTextWidth(paint.dc, cache.fontTipBody,  line4);

    int contentW = MaxVal(MaxVal(w1, w2), MaxVal(w3, w4));
    int cardW = ClampVal(contentW + padX * 2, MinVal(ScalePx(180, dpi), maxCardW), maxCardW);

    int cardH = padY * 2 + cache.hTipTitle;
    if (!line2.empty()) cardH += gap12 + cache.hTipBody;
    cardH += gap23 + cache.hTipBody;
    if (!line4.empty()) cardH += gap34 + cache.hTipBody;

    int x = mouseX + offX;
    int y = mouseY + offY;
    if (x + cardW > area.right - edgeMargin)  x = mouseX - offX - cardW;
    if (y + cardH > area.bottom - edgeMargin) y = mouseY - offY - cardH;

    int minX = area.left + edgeMargin;
    int maxX = area.right - edgeMargin - cardW;
    int minY = area.top + edgeMargin;
    int maxY = area.bottom - edgeMargin - cardH;
    x = (maxX >= minX) ? ClampVal(x, minX, maxX) : area.left;
    y = (maxY >= minY) ? ClampVal(y, minY, maxY) : area.top;

    RECT cardRect{ x, y, x + cardW, y + cardH };

    COLORREF panelBg = style.dark
        ? BlendColor(style.bg, RGB(0x2A, 0x2C, 0x35), 0.78)
        : BlendColor(style.bg, RGB(0xFF, 0xFF, 0xFF), 0.88);
    COLORREF panelBorder = style.dark
        ? BlendColor(panelBg, RGB(0xFF, 0xFF, 0xFF), 0.16)
        : BlendColor(panelBg, RGB(0x00, 0x00, 0x00), 0.16);
    COLORREF titleCol = style.text;
    if (ContrastRatio(titleCol, panelBg) < 4.5) {
        titleCol = style.dark ? RGB(0xF4, 0xF4, 0xF7) : RGB(0x16, 0x18, 0x1E);
    }

    paint.TooltipSurface(cardRect, radiusPx, borderPx, shadowPx,
                           panelBg, panelBorder, style.dark);

    int textL = cardRect.left + padX;
    int textR = cardRect.right - padX;
    int curY  = cardRect.top + padY;

    RECT r1{ textL, curY, textR - titleExtraW, curY + cache.hTipTitle };
    paint.Font(cache.fontTipTitle);
    paint.TextColor(titleCol);
    paint.Text(line1.c_str(), static_cast<int>(line1.size()), r1,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (showTagDot) {
        int bx = textR - dotDia;
        int by = curY + (cache.hTipTitle - dotDia) / 2;
        paint.Tag(bx, by, dotDia, TagBadgeColor(tag), panelBg);
    }
    curY += cache.hTipTitle;

    paint.Font(cache.fontTipBody);
    if (!line2.empty()) {
        curY += gap12;
        RECT r2{ textL, curY, textR, curY + cache.hTipBody };
        paint.TextColor(style.textDim);
        paint.Text(line2.c_str(), static_cast<int>(line2.size()), r2,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_END_ELLIPSIS | DT_NOPREFIX);
        curY += cache.hTipBody;
    }

    curY += gap23;
    RECT r3{ textL, curY, textR, curY + cache.hTipBody };
    paint.TextColor(titleCol);
    paint.Text(line3.c_str(), static_cast<int>(line3.size()), r3,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    curY += cache.hTipBody;

    if (!line4.empty()) {
        curY += gap34;
        RECT r4{ textL, curY, textR, curY + cache.hTipBody };
        paint.TextColor(style.textDim);
        paint.Text(line4.c_str(), static_cast<int>(line4.size()), r4,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

}

} // namespace

// ============================================================================
// 缩放动画公开接口实现
// ============================================================================

bool TreemapAnimStart(HWND hwnd, const ss::Tree& tree,
                      uint32_t oldFocus, uint32_t newFocus,
                      const RECT& layoutArea,
                      const std::vector<TreemapBox>& oldBoxes,
                      const std::vector<TreemapBox>& newBoxes,
                      double nowMs, int durationMs)
{
    EraseAnimSession(hwnd);

    // 跳过条件：关闭动画 / 无效窗口 / 任一布局为空 / 盒数 > 3000 规模保护
    if (!hwnd || durationMs <= 0) return false;
    if (oldBoxes.empty() || newBoxes.empty()) return false;
    if (oldBoxes.size() > kAnimMaxBoxes || newBoxes.size() > kAnimMaxBoxes) return false;

    RectD canvasD{
        static_cast<double>(layoutArea.left),
        static_cast<double>(layoutArea.top),
        static_cast<double>(MaxVal(1, RectW(layoutArea))),
        static_cast<double>(MaxVal(1, RectH(layoutArea)))
    };

    FlatNodeMap oldMap;
    oldMap.Reset(oldBoxes.size());
    for (size_t i = 0; i < oldBoxes.size(); ++i) {
        oldMap.Insert(oldBoxes[i].node, static_cast<uint32_t>(i));
    }

    FlatNodeMap newMap;
    newMap.Reset(newBoxes.size());
    for (size_t i = 0; i < newBoxes.size(); ++i) {
        newMap.Insert(newBoxes[i].node, static_cast<uint32_t>(i));
    }

    // 预先按深度排序新盒子，保证计算展开起始矩形时父节点总先于子节点处理
    std::vector<uint32_t> newOrder(newBoxes.size());
    for (size_t i = 0; i < newBoxes.size(); ++i) newOrder[i] = static_cast<uint32_t>(i);
    std::stable_sort(newOrder.begin(), newOrder.end(), [&](uint32_t a, uint32_t b) {
        return newBoxes[a].depth < newBoxes[b].depth;
    });

    AnimSession sess{};
    sess.hwnd       = hwnd;
    sess.startMs    = nowMs;
    sess.durationMs = ClampVal(durationMs, 30, 2000);
    sess.activeTracks.reserve(newBoxes.size());

    FlatNodeMap trackMap;
    trackMap.Reset(newBoxes.size());

    // 1) 为所有新布局盒子构造轨迹（匹配节点平滑插值，新节点从父矩形展开）
    for (uint32_t idx : newOrder) {
        const TreemapBox& nb = newBoxes[idx];
        if (!tree.Valid(nb.node)) continue;

        AnimTrack tr{};
        tr.node  = nb.node;
        tr.depth = nb.depth;
        tr.end   = BoxToRectD(nb);

        uint32_t oldIdx = 0;
        if (oldMap.Find(nb.node, oldIdx)) {
            // 共存节点：直接从旧矩形插值到新矩形
            tr.start = BoxToRectD(oldBoxes[oldIdx]);
        } else if (nb.node == oldFocus) {
            // 返回上层：原焦点目录从整个画布缩小至新位置
            tr.start = canvasD;
        } else {
            // 新出现节点：向上寻找旧布局中的最近可见祖先，按比例从其旧矩形展开
            bool resolved = false;
            uint32_t anc = tree.nodes[nb.node].parent;
            for (int guard = 0; guard < 64 && anc != ss::Node::kNone && tree.Valid(anc); ++guard) {
                uint32_t ancOldIdx = 0;
                if (oldMap.Find(anc, ancOldIdx)) {
                    RectD ancOld = BoxToRectD(oldBoxes[ancOldIdx]);
                    RectD ancNew = canvasD;
                    uint32_t ancNewIdx = 0;
                    if (newMap.Find(anc, ancNewIdx)) {
                        ancNew = BoxToRectD(newBoxes[ancNewIdx]);
                    } else if (anc != newFocus) {
                        ancNew = tr.end;
                    }
                    tr.start = MapProportional(tr.end, ancNew, ancOld);
                    resolved = true;
                    break;
                }
                if (anc == oldFocus) {
                    uint32_t ancNewIdx = 0;
                    if (newMap.Find(anc, ancNewIdx)) {
                        tr.start = MapProportional(tr.end, BoxToRectD(newBoxes[ancNewIdx]), canvasD);
                        resolved = true;
                    }
                    break;
                }
                anc = tree.nodes[anc].parent;
            }

            if (!resolved) {
                // 若父节点已分配展开轨迹，则随父节点起始矩形同步展开；否则从自身中心展开
                uint32_t parentNode = tree.nodes[nb.node].parent;
                uint32_t parentTrackIdx = 0;
                if (parentNode != ss::Node::kNone && trackMap.Find(parentNode, parentTrackIdx)) {
                    const AnimTrack& pTr = sess.activeTracks[parentTrackIdx];
                    tr.start = MapProportional(tr.end, pTr.end, pTr.start);
                } else {
                    tr.start = CollapseToCenter(tr.end);
                }
            }
        }

        uint32_t trackIdx = static_cast<uint32_t>(sess.activeTracks.size());
        sess.activeTracks.push_back(tr);
        trackMap.Insert(tr.node, trackIdx);
    }

    // 2) 为仅存在于旧布局的节点构造缩小淡出轨迹（向父矩形中心收敛）
    sess.exitTracks.reserve(oldBoxes.size() / 2);
    for (const TreemapBox& ob : oldBoxes) {
        if (!tree.Valid(ob.node)) continue;
        uint32_t dummy = 0;
        if (newMap.Find(ob.node, dummy)) continue;

        AnimTrack tr{};
        tr.node  = ob.node;
        tr.depth = ob.depth;
        tr.start = BoxToRectD(ob);

        bool foundTarget = false;
        uint32_t anc = tree.nodes[ob.node].parent;
        for (int guard = 0; guard < 64 && anc != ss::Node::kNone && tree.Valid(anc); ++guard) {
            uint32_t ancNewIdx = 0;
            if (newMap.Find(anc, ancNewIdx)) {
                tr.end = CollapseToCenter(BoxToRectD(newBoxes[ancNewIdx]));
                foundTarget = true;
                break;
            }
            anc = tree.nodes[anc].parent;
        }
        if (!foundTarget) {
            uint32_t parentNode = tree.nodes[ob.node].parent;
            uint32_t parentOldIdx = 0;
            if (parentNode != ss::Node::kNone && oldMap.Find(parentNode, parentOldIdx)) {
                tr.end = CollapseToCenter(BoxToRectD(oldBoxes[parentOldIdx]));
            } else {
                tr.end = CollapseToCenter(tr.start);
            }
        }
        sess.exitTracks.push_back(tr);
    }

    UpdateAnimCurrentBoxes(sess, 0.0);
    GetAnimSessions().push_back(std::move(sess));
    return true;
}

bool TreemapAnimTick(HWND hwnd, double nowMs)
{
    AnimSession* sess = FindAnimSession(hwnd);
    if (!sess) return false;

    double elapsed = nowMs - sess->startMs;
    if (elapsed >= sess->durationMs) {
        EraseAnimSession(hwnd);
        return false;
    }

    double t = elapsed / sess->durationMs;
    double inv = 1.0 - ClampVal(t, 0.0, 1.0);
    double easeOut = 1.0 - inv * inv * inv; // 1 - (1 - t)^3
    UpdateAnimCurrentBoxes(*sess, easeOut);
    return true;
}

void TreemapAnimCancel(HWND hwnd)
{
    EraseAnimSession(hwnd);
}

const std::vector<TreemapBox>* TreemapAnimCurrentBoxes(HWND hwnd)
{
    AnimSession* sess = FindAnimSession(hwnd);
    return sess ? &sess->currentBoxes : nullptr;
}

namespace {

void DrawTreemapAnimExiting(TreemapPaint& paint, HWND hwnd, const RECT& area,
                            const ss::Tree& tree, const TreemapStyle& style)
{
    AnimSession* sess = FindAnimSession(hwnd);
    if (!sess || sess->exitTracks.empty()) return;

    double fade = 1.0 - sess->easeT;
    if (fade <= 0.02) return;

    BYTE alpha = ClampByte(RoundInt(fade * 190.0));
    if (alpha < 4) return;


    const double et = sess->easeT;
    for (const AnimTrack& tr : sess->exitTracks) {
        if (!tree.Valid(tr.node)) continue;
        double x = LerpD(tr.start.x, tr.end.x, et);
        double y = LerpD(tr.start.y, tr.end.y, et);
        double w = LerpD(tr.start.w, tr.end.w, et);
        double h = LerpD(tr.start.h, tr.end.h, et);
        if (w < 2.0 || h < 2.0) continue;

        RECT rc{
            RoundInt(x),
            RoundInt(y),
            RoundInt(x + w),
            RoundInt(y + h)
        };
        if (!RectIntersects(rc, area)) continue;

        COLORREF col = ComputeBoxColor(tree.nodes[tr.node], MaxVal(1, tr.depth), style);
        paint.FillAlpha(rc, col, alpha);
    }
}

void PaintFrame(TreemapPaint& paint, HWND view, const RECT& area, const ss::Tree& tree,
                const std::vector<TreemapBox>& boxes, const TreemapStyle& style,
                const TreemapOverlay& overlay)
{
    if (const auto* animated = TreemapAnimCurrentBoxes(view)) {
        DrawTreemap(paint, area, tree, *animated, style, Node::kNone, Node::kNone);
        DrawTreemapAnimExiting(paint, view, area, tree, style);
    } else {
        DrawTreemap(paint, area, tree, boxes, style, overlay.hover, overlay.selected);
        if (overlay.mouseX >= 0)
            DrawTreemapTooltip(paint, area, tree, overlay.hover, overlay.mouseX, overlay.mouseY,
                               style, overlay.showDateTime, overlay.showAge);
    }
}

} // namespace

void DrawTreemapFrame(HDC dc, HWND view, const RECT& area, const ss::Tree& tree,
                      const std::vector<TreemapBox>& boxes, const TreemapStyle& style,
                      const TreemapOverlay& overlay)
{
    if (!dc || RectW(area) <= 0 || RectH(area) <= 0) return;
    int saved = SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
    SetBkMode(dc, TRANSPARENT);
    GetGdiCache().EnsureFonts(dc, style.dpi > 0 ? style.dpi : 96, style.fontSize);
    TreemapPaint paint{ dc };
    PaintFrame(paint, view, area, tree, boxes, style, overlay);
    RestoreDC(dc, saved);
}

TreemapRenderer::~TreemapRenderer()
{
    Reset();
}

void TreemapRenderer::Reset()
{
    if (brush_) brush_->Release();
    if (target_) target_->Release();
    brush_ = nullptr;
    target_ = nullptr;
}

TreemapPresent TreemapRenderer::Draw(HWND canvas, HDC metrics, HWND view, const RECT& area,
                                    const ss::Tree& tree, const std::vector<TreemapBox>& boxes,
                                    const TreemapStyle& style, const TreemapOverlay& overlay)
{
    if (!metrics || RectW(area) <= 0 || RectH(area) <= 0)
        return TreemapPresent::Fallback;

    auto size = D2D1::SizeU(RectW(area), RectH(area));
    if (!target_) {
        if (GetTickCount64() < retryAfter_) return TreemapPresent::Fallback;
        static auto factory = [] {
            ID2D1Factory* ptr = nullptr;
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &ptr);
            return ComPtr<ID2D1Factory>(ptr);
        }();
        if (!factory) return TreemapPresent::Fallback;
        // Layout and hit testing already use physical pixels.
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        auto hwndProps = D2D1::HwndRenderTargetProperties(canvas, size, D2D1_PRESENT_OPTIONS_NONE);
        if (FAILED(factory->CreateHwndRenderTarget(props, hwndProps, &target_)) ||
            FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0), &brush_))) {
            Reset();
            retryAfter_ = GetTickCount64() + 5000;
            return TreemapPresent::Fallback;
        }
        target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    }

    auto oldSize = target_->GetPixelSize();
    if ((oldSize.width != size.width || oldSize.height != size.height) &&
        FAILED(target_->Resize(size))) {
        Reset();
        return TreemapPresent::Fallback;
    }
    if (target_->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED)
        return TreemapPresent::Occluded;

    GdiCache& cache = GetGdiCache();
    cache.EnsureFonts(metrics, style.dpi > 0 ? style.dpi : 96, style.fontSize);
    if (!cache.EnsureWriteFormats()) {
        Reset();
        retryAfter_ = GetTickCount64() + 5000;
        return TreemapPresent::Fallback;
    }

    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(area.left),
                                                       -static_cast<float>(area.top)));
    TreemapPaint paint{ metrics, target_, brush_ };
    PaintFrame(paint, view, area, tree, boxes, style, overlay);
    if (FAILED(target_->EndDraw())) {
        Reset();
        return TreemapPresent::Fallback;
    }
    // Consume the previous present's occlusion result before the next pre-draw check.
    return (target_->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED)
        ? TreemapPresent::Occluded : TreemapPresent::Presented;
}

} // namespace ss::ui
