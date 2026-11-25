// SpaceSniffer 2.0 — core model, scanner, features (port of the reverse-engineered
// original plus the .NET reference implementation).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <strsafe.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace ss {

// ---------------------------------------------------------------- node model
enum class Kind : uint8_t { Drive = 1, Folder = 2, File = 3, FreeSpace = 4, Unknown = 5, Missing = 6 };

struct Node
{
    uint32_t nameOff = 0;      // offset into Tree::names (wchar units)
    uint16_t nameLen = 0;
    uint8_t  kind = (uint8_t)Kind::File;
    uint8_t  tag = 0;
    int16_t  group = -1;
    uint32_t parent = kNone;
    uint32_t firstChild = kNone;
    uint32_t nextSibling = kNone;
    int64_t  size = 0;         // logical bytes
    int64_t  disk = 0;         // allocated bytes
    FILETIME created{}, modified{}, accessed{};
    DWORD    attrs = 0;

    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    bool IsFile() const { return kind == (uint8_t)Kind::File; }
    bool IsFolder() const { return kind == (uint8_t)Kind::Folder || kind == (uint8_t)Kind::Drive; }
    bool IsVirtual() const { return kind >= (uint8_t)Kind::FreeSpace; }
};

struct Tree
{
    std::vector<Node> nodes;
    std::vector<wchar_t> names;    // arena
    uint32_t root = Node::kNone;
    int bytesPerCluster = 4096;
    std::wstring rootPath;
    std::wstring volumeFs;
    int64_t volumeTotal = 0;
    int64_t volumeFree = 0;

    std::wstring Name(uint32_t i) const
    {
        if (i >= nodes.size()) return L"";
        const Node& n = nodes[i];
        if ((size_t)n.nameOff + (size_t)n.nameLen > names.size()) return L"";
        return std::wstring(names.data() + n.nameOff, n.nameLen);
    }
    void SetName(Node& n, const std::wstring& s)
    {
        size_t len = s.size();
        if (len > 65535) len = 65535;
        n.nameOff = (uint32_t)names.size();
        n.nameLen = (uint16_t)len;
        names.insert(names.end(), s.begin(), s.begin() + len);
    }
    bool Valid(uint32_t i) const { return i != Node::kNone && i < nodes.size(); }
    uint32_t AddNode(uint32_t parent, const std::wstring& name, Kind kind)
    {
        Node n;
        n.kind = (uint8_t)kind;
        n.parent = parent;
        SetName(n, name);
        nodes.push_back(n);
        uint32_t idx = (uint32_t)nodes.size() - 1;
        if (Valid(parent))
        {
            // O(1) prepend; sibling order is not significant (views sort by size)
            Node& p = nodes[parent];
            n.nextSibling = p.firstChild;
            nodes[idx].nextSibling = p.firstChild;
            p.firstChild = idx;
        }
        return idx;
    }
    std::wstring Path(uint32_t i) const;
    int Depth(uint32_t i) const;
    void Aggregate();                        // recompute folder sizes bottom-up
    uint32_t FindChild(uint32_t parent, const std::wstring& name) const;
    void ApplyGroups(const struct FileClasses& fc);
    void Clear() { nodes.clear(); names.clear(); root = Node::kNone; }
};

// ---------------------------------------------------------------- volume info
struct VolumeInfo
{
    std::wstring root;
    std::wstring fs;
    std::wstring label;
    bool isNtfs = false;
    int  bytesPerCluster = 4096;
    int  bytesPerSector = 512;
    int64_t total = 0;
    int64_t free = 0;
};

VolumeInfo QueryVolume(const std::wstring& path);
int64_t AlignToCluster(int64_t v, int cluster_size);

// ---------------------------------------------------------------- scanner
struct ScanOptions
{
    enum Mode { Auto, Fast, Mft } mode = Auto;
    bool scanAds = false;
    bool fastDiskSize = true;
    int  threads = 0;             // 0 = auto
    bool includePseudo = true;
};

struct ScanProgress
{
    volatile LONG64 files = 0;
    volatile LONG64 dirs = 0;
    volatile LONG64 bytes = 0;
    volatile LONG state = 0;      // 0 = running, 1 = done, 2 = cancelled, 3 = failed
    wchar_t  message[256] = {};
};

// Runs a scan of `path` into `out`. `progress` may be null.
bool ScanPath(const std::wstring& path, const ScanOptions& opt, Tree& out,
              ScanProgress* progress, bool* usedMft);

bool CanReadVolume(const std::wstring& path);
void EnableBackupPrivilege();

// ---------------------------------------------------------------- formatting
std::wstring FmtSize(int64_t bytes);
std::wstring FmtCount(int64_t n);
std::wstring FmtDate(const FILETIME& ft);
std::wstring FmtDateTime(const FILETIME& ft);
std::wstring FmtAge(const FILETIME& ft);
std::wstring FmtPercent(int64_t part, int64_t total);
int64_t FileTimeToSeconds(const FILETIME& ft);
std::wstring AttrFlags(DWORD attrs);
std::wstring Lowercase(std::wstring s);
std::wstring Trim(const std::wstring& s);
// Normalizes slashes and adds \\?\ (or \\?\UNC\) prefix for extended-length Win32 paths.
std::wstring ToLongPath(const std::wstring& path);
bool WildcardMatch(const std::wstring& text, const std::wstring& pattern);

// ---------------------------------------------------------------- file classes
struct FileClass
{
    std::wstring name;
    std::vector<std::wstring> extensions;
    COLORREF color = RGB(0x80, 0x80, 0x80);
};
struct FileClasses
{
    std::vector<FileClass> items;
    static FileClasses Default();
    int IndexOf(const std::wstring& ext) const;
    int IndexOfName(const std::wstring& name) const;
};

// ---------------------------------------------------------------- config
struct ExportConfig
{
    std::wstring name;
    std::wstring preferredFileName = L"SpaceSniffer-export";
    std::wstring preferredExtension = L".txt";
    std::wstring blockSorting = L"folders";
    std::wstring fineSorting = L"size";
    bool fineDescending = true;
    bool builtIn = false;
    std::wstring script;
};

struct AppConfig
{
    int refreshDelayMs = 50;
    int zoomTimeMs = 250;
    bool useRealZoom = false;
    bool useDynaSmooth = true;
    int minElementSizePx = 10;
    int initialDetailLevel = 3;
    int fontSize = 8;
    int proportions = 0;
    int levelContrast = 10;
    int borderContrast = 10;
    bool showFreeSpace = false;
    bool showUnknownSpace = true;
    bool useSorting = true;
    bool scanAds = false;
    bool darkMode = true;
    bool followSystemTheme = true;
    bool useMftScanner = true;
    bool hideAdminHint = false;
    bool alwaysRunAsAdmin = false;
    bool shellIntegration = false;
    std::wstring language = L"auto";   // "auto" | "zh" | "en"
    bool showTooltipDate = true;
    bool showTooltipAge = true;
    bool mouseHover = true;
    bool logPopup = true;
    bool logIssue = true;
    bool logWarning = true;
    std::wstring initialStyle = L"File Classes";
    COLORREF driveColor = RGB(0x2D, 0x6C, 0xA2);
    COLORREF folderColor = RGB(0x4A, 0x7E, 0xBB);
    COLORREF fileColor = RGB(0x6D, 0x8F, 0xB0);
    COLORREF freeColor = RGB(0x2F, 0x5D, 0x3A);
    COLORREF unknownColor = RGB(0x5A, 0x4A, 0x2F);
    std::vector<ExportConfig> exports;
    FileClasses classes;
    std::wstring path;

    static AppConfig Default();
    bool Load(const std::wstring& path);
    bool Save() const;
};

// ---------------------------------------------------------------- filter
struct FilterTerm
{
    enum Target { FileName, FolderName, Size, Age, Tag, Attr, FileClass } target = FileName;
    bool negate = false;
    std::wstring mask;
    bool greater = true;
    int64_t threshold = 0;
    bool sizeLogical = false;
    int ageSource = 0;             // 0 modified, 1 created, 2 accessed
    int tagMask = 0;
    bool tagAny = false;
    DWORD attrMask = 0, attrNotMask = 0;
    std::wstring className;
    std::wstring raw;
};

struct FilterExpr
{
    std::vector<FilterTerm> terms;
    bool empty() const { return terms.empty(); }
    static FilterExpr Parse(const std::wstring& text);
    bool Matches(const Tree& t, uint32_t node, const FileClasses& fc) const;
};

// Returns a projected tree (files not matching dropped, empty folders dropped).
bool ProjectFilter(const Tree& src, uint32_t node, const FilterExpr& fx, const FileClasses& fc, Tree& out);

// ---------------------------------------------------------------- export
struct ExportResult { bool ok = false; std::wstring message; int64_t bytes = 0; };
ExportResult ExportRun(const Tree& t, uint32_t root, const ExportConfig& cfg, const FileClasses& fc,
                       const std::wstring& outPath, std::function<void(int64_t,int64_t)> progress);
std::wstring ExportPreview(const Tree& t, uint32_t root, const ExportConfig& cfg, const FileClasses& fc,
                           size_t maxNodes = 3000);

// ---------------------------------------------------------------- snapshot
bool SaveSnapshot(const Tree& t, const std::wstring& path, bool nativeSns);
bool LoadSnapshot(const std::wstring& path, Tree& out, std::wstring& error);

// ---------------------------------------------------------------- equality/dup helpers
struct DuplicateGroup { int64_t size = 0; std::vector<std::wstring> paths; };

} // namespace ss

