// Volume info, tree helpers, formatting, parallel directory scanner and NTFS $MFT reader.
#include "core.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cwctype>
#include <vector>

namespace ss {

static constexpr uint32_t kNone = Node::kNone;

// ============================================================ formatting
std::wstring Lowercase(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

int64_t FileTimeToSeconds(const FILETIME& ft)
{
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart == 0) return 0;
    return (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

static std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[128];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring FmtSize(int64_t bytes)
{
    if (bytes < 0) bytes = 0;
    if (bytes < 1024) return Fmt(L"%lld B", (long long)bytes);
    static const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = (double)bytes; int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    if (v >= 100) return Fmt(L"%.0f %s", v, u[i]);
    if (v >= 10)  return Fmt(L"%.1f %s", v, u[i]);
    return Fmt(L"%.2f %s", v, u[i]);
}

std::wstring FmtCount(int64_t n)
{
    std::wstring s = std::to_wstring(n);
    std::wstring out;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i)
    {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), L',');
    }
    return out;
}

std::wstring FmtPercent(int64_t part, int64_t total)
{
    if (total <= 0) return L"0%";
    return Fmt(L"%.1f%%", 100.0 * (double)part / (double)total);
}

static std::wstring SysTime(const FILETIME& ft, const wchar_t* fmt)
{
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return L"-";
    FILETIME local{};
    if (!FileTimeToLocalFileTime(&ft, &local)) return L"-";
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&local, &st)) return L"-";
    wchar_t buf[64] = {};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, fmt, buf, _countof(buf), nullptr) == 0)
        return L"-";
    return buf;
}

std::wstring FmtDate(const FILETIME& ft)
{
    return SysTime(ft, L"yyyy-MM-dd");
}

std::wstring FmtDateTime(const FILETIME& ft)
{
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return L"-";
    FILETIME local{};
    if (!FileTimeToLocalFileTime(&ft, &local)) return L"-";
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&local, &st)) return L"-";
    wchar_t buf[64] = {};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring FmtAge(const FILETIME& ft)
{
    int64_t t = FileTimeToSeconds(ft);
    if (t == 0) return L"-";
    int64_t now = (int64_t)time(nullptr);
    int64_t d = now - t;
    if (d < 0) d = 0;
    if (d < 60) return Fmt(L"%llds", (long long)d);
    if (d < 3600) return Fmt(L"%lldm", (long long)(d / 60));
    if (d < 86400) return Fmt(L"%lldh", (long long)(d / 3600));
    if (d < 86400LL * 31) return Fmt(L"%lldd", (long long)(d / 86400));
    if (d < 86400LL * 365) return Fmt(L"%lldmo", (long long)(d / (86400LL * 30)));
    return Fmt(L"%lldy", (long long)(d / (86400LL * 365)));
}

std::wstring AttrFlags(DWORD attrs)
{
    std::wstring s;
    s += (attrs & FILE_ATTRIBUTE_DIRECTORY) ? L'D' : L'-';
    if (attrs & FILE_ATTRIBUTE_READONLY) s += L'R';
    if (attrs & FILE_ATTRIBUTE_HIDDEN) s += L'H';
    if (attrs & FILE_ATTRIBUTE_SYSTEM) s += L'S';
    if (attrs & FILE_ATTRIBUTE_ARCHIVE) s += L'A';
    if (attrs & FILE_ATTRIBUTE_COMPRESSED) s += L'C';
    if (attrs & FILE_ATTRIBUTE_ENCRYPTED) s += L'E';
    if (attrs & FILE_ATTRIBUTE_SPARSE_FILE) s += L'P';
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) s += L'L';
    return s;
}

bool WildcardMatch(const std::wstring& text, const std::wstring& pattern)
{
    size_t ti = 0, pi = 0, starT = std::wstring::npos, starP = std::wstring::npos;
    while (ti < text.size())
    {
        if (pi < pattern.size() && (pattern[pi] == L'?' || towlower(pattern[pi]) == towlower(text[ti])))
        {
            ti++; pi++;
        }
        else if (pi < pattern.size() && pattern[pi] == L'*')
        {
            starP = pi++; starT = ti;
        }
        else if (starP != std::wstring::npos)
        {
            pi = starP + 1; ti = ++starT;
        }
        else return false;
    }
    while (pi < pattern.size() && pattern[pi] == L'*') pi++;
    return pi == pattern.size();
}

// ============================================================ tree helpers
std::wstring Tree::Path(uint32_t i) const
{
    std::vector<std::wstring> parts;
    uint32_t cur = i;
    for (int guard = 0; guard < 512 && Valid(cur); ++guard)
    {
        const Node& n = nodes[cur];
        if (n.parent == kNone || n.parent == cur) { parts.push_back(Name(cur)); break; }
        parts.push_back(Name(cur));
        cur = n.parent;
    }
    std::wstring out;
    for (size_t k = parts.size(); k-- > 0;)
    {
        if (!out.empty())
        {
            if (out.back() != L'\\') out += L'\\';
        }
        out += parts[k];
    }
    return out;
}

int Tree::Depth(uint32_t i) const
{
    int d = 0;
    uint32_t cur = i;
    while (Valid(cur) && nodes[cur].parent != kNone) { d++; cur = nodes[cur].parent; if (d > 512) break; }
    return d;
}

uint32_t Tree::FindChild(uint32_t parent, const std::wstring& name) const
{
    if (!Valid(parent)) return kNone;
    std::wstring low = Lowercase(name);
    for (uint32_t c = nodes[parent].firstChild; c != kNone; c = nodes[c].nextSibling)
        if (Lowercase(Name(c)) == low) return c;
    return kNone;
}

void Tree::Aggregate()
{
    // iterative post-order
    struct Frame { uint32_t idx; uint32_t child; };
    std::vector<Frame> stack;
    if (!Valid(root)) return;
    stack.push_back({ root, nodes[root].firstChild });

    while (!stack.empty())
    {
        Frame& f = stack.back();
        if (f.child != kNone && Valid(f.child) && stack.size() < 1024)
        {
            uint32_t c = f.child;
            f.child = nodes[c].nextSibling;
            stack.push_back({ c, nodes[c].firstChild });
            continue;
        }
        uint32_t idx = f.idx;
        stack.pop_back();
        Node& n = nodes[idx];
        if (n.IsFolder())
        {
            int64_t size = 0, disk = 0;
            for (uint32_t c = n.firstChild; c != kNone && Valid(c); c = nodes[c].nextSibling)
            {
                size += nodes[c].size;
                disk += nodes[c].disk;
            }
            n.size = size;
            n.disk = disk;
        }
        else if (n.IsVirtual())
        {
            // keep its own size
        }
    }
}

void Tree::ApplyGroups(const FileClasses& fc)
{
    for (auto& n : nodes)
    {
        if (!n.IsFile()) continue;
        std::wstring nm = Name(&n - nodes.data());
        size_t dot = nm.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= nm.size()) { n.group = -1; continue; }
        n.group = (int16_t)fc.IndexOf(nm.substr(dot + 1));
    }
}

// ============================================================ path helpers
// P0-5: unified extended-length path helper for >MAX_PATH and UNC targets.
std::wstring ToLongPath(const std::wstring& p)
{
    if (p.empty()) return p;
    std::wstring s = p;
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (s.rfind(L"\\\\?\\", 0) == 0 || s.rfind(L"\\\\.\\", 0) == 0)
        return s;
    // Collapse redundant backslashes (preserving leading UNC "\\") so \\?\ accepts the path.
    {
        std::wstring norm;
        norm.reserve(s.size());
        size_t start = 0;
        if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\')
        {
            norm += L"\\\\";
            start = 2;
        }
        for (size_t i = start; i < s.size(); ++i)
        {
            if (s[i] == L'\\' && !norm.empty() && norm.back() == L'\\') continue;
            norm += s[i];
        }
        s = std::move(norm);
    }
    if (s.size() < 240) return s;
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\')
        return L"\\\\?\\UNC\\" + s.substr(2);
    if (s.size() >= 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\')
        return L"\\\\?\\" + s;
    return s;
}

namespace {
std::wstring StripLongPrefix(const std::wstring& p)
{
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
    return p;
}

wchar_t ExtractDriveLetter(const std::wstring& p)
{
    std::wstring s = StripLongPrefix(p);
    if (s.size() >= 2 && iswalpha(s[0]) && s[1] == L':') return s[0];
    return 0;
}

bool IsWholeVolumePath(const std::wstring& p)
{
    std::wstring s = StripLongPrefix(p);
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (s.size() == 2 && iswalpha(s[0]) && s[1] == L':') return true;
    if (s.size() == 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\') return true;
    return false;
}
} // namespace

// ============================================================ volume
VolumeInfo QueryVolume(const std::wstring& path)
{
    VolumeInfo vi;
    // P0-5: resolve volume root via GetVolumePathNameW with 4096-wchar buffer + \\?\ prefix
    // so >MAX_PATH and UNC paths are never truncated by MAX_PATH.
    std::wstring lp = ToLongPath(path);
    std::wstring volRoot;
    if (!lp.empty())
    {
        std::wstring volBuf(4096, L'\0');
        if (GetVolumePathNameW(lp.c_str(), volBuf.data(), (DWORD)volBuf.size()))
        {
            volRoot = volBuf.c_str();
        }
    }
    if (volRoot.empty())
    {
        wchar_t drv = ExtractDriveLetter(path);
        if (drv)
        {
            wchar_t r[4] = { drv, L':', L'\\', 0 };
            volRoot = r;
        }
        else
        {
            volRoot = lp;
        }
    }
    if (!volRoot.empty() && volRoot.back() != L'\\') volRoot += L'\\';
    vi.root = StripLongPrefix(volRoot);

    wchar_t fs[MAX_PATH] = {}, label[MAX_PATH] = {};
    DWORD serial = 0, maxComp = 0, flags = 0;
    if (!volRoot.empty() &&
        GetVolumeInformationW(volRoot.c_str(), label, MAX_PATH, &serial, &maxComp, &flags, fs, MAX_PATH))
    {
        vi.fs = fs;
        vi.label = label;
    }
    vi.isNtfs = (_wcsicmp(vi.fs.c_str(), L"NTFS") == 0) || (_wcsicmp(vi.fs.c_str(), L"ReFS") == 0);

    DWORD spc = 0, bps = 0, freeClusters = 0, totalClusters = 0;
    if (!volRoot.empty() && GetDiskFreeSpaceW(volRoot.c_str(), &spc, &bps, &freeClusters, &totalClusters) && spc && bps)
    {
        vi.bytesPerCluster = (int)(spc * bps);
        vi.bytesPerSector = (int)bps;
    }
    ULARGE_INTEGER avail{}, total{}, totalFree{};
    if (!volRoot.empty() && GetDiskFreeSpaceExW(volRoot.c_str(), &avail, &total, &totalFree))
    {
        vi.total = (int64_t)total.QuadPart;
        vi.free = (int64_t)(totalFree.QuadPart ? totalFree.QuadPart : avail.QuadPart);
    }
    return vi;
}

int64_t AlignToCluster(int64_t v, int cluster)
{
    if (v <= 0) return 0;
    if (cluster <= 0) cluster = 4096;
    int64_t rem = v % cluster;
    return rem == 0 ? v : v + (cluster - rem);
}

void EnableBackupPrivilege()
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    const wchar_t* privs[] = { L"SeBackupPrivilege", L"SeRestorePrivilege" };
    for (auto p : privs)
    {
        LUID luid;
        if (LookupPrivilegeValueW(nullptr, p, &luid))
        {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        }
    }
    CloseHandle(token);
}

bool CanReadVolume(const std::wstring& path)
{
    wchar_t drv = ExtractDriveLetter(path);
    if (!drv) return false;
    wchar_t dev[16] = { L'\\', L'\\', L'.', L'\\', drv, L':', 0 };
    HANDLE h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// ============================================================ fast scanner
namespace {

struct Entry
{
    std::wstring name;
    DWORD attrs = 0;
    int64_t size = 0;
    FILETIME created{}, modified{}, accessed{};
};

struct WorkItem
{
    uint32_t node;
    std::wstring path;
};

struct NodeInit
{
    DWORD attrs = 0;
    int64_t size = 0;
    int64_t disk = 0;
    FILETIME created{}, modified{}, accessed{};
};

uint32_t AddNodeLocked(Tree& t, std::mutex& mtx, uint32_t parent, const std::wstring& name, Kind kind)
{
    std::lock_guard<std::mutex> lock(mtx);
    Node n;
    n.kind = (uint8_t)kind;
    n.parent = parent;
    t.SetName(n, name);
    t.nodes.push_back(n);
    uint32_t idx = (uint32_t)t.nodes.size() - 1;
    if (t.Valid(parent))
    {
        Node& p = t.nodes[parent];
        t.nodes[idx].nextSibling = p.firstChild;
        p.firstChild = idx;
    }
    return idx;
}

// Adds the node and fills all fields under the same lock, so another thread can never
// reallocate t.nodes while we still hold a Node reference.
uint32_t AddNodeFilled(Tree& t, std::mutex& mtx, uint32_t parent, const std::wstring& name, Kind kind, const NodeInit& init)
{
    std::lock_guard<std::mutex> lock(mtx);
    Node n;
    n.kind = (uint8_t)kind;
    n.parent = parent;
    n.attrs = init.attrs;
    n.size = init.size;
    n.disk = init.disk;
    n.created = init.created;
    n.modified = init.modified;
    n.accessed = init.accessed;
    t.SetName(n, name);
    t.nodes.push_back(n);
    uint32_t idx = (uint32_t)t.nodes.size() - 1;
    if (t.Valid(parent))
    {
        Node& p = t.nodes[parent];
        t.nodes[idx].nextSibling = p.firstChild;
        p.firstChild = idx;
    }
    return idx;
}


bool ScanDirectory(const std::wstring& dirPath, std::vector<Entry>& out, DWORD* outErr = nullptr)
{
    if (outErr) *outErr = ERROR_SUCCESS;
    std::wstring pattern = ToLongPath(dirPath);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (outErr) *outErr = GetLastError();
        return false;
    }
    do
    {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        Entry e;
        e.name = fd.cFileName;
        e.attrs = fd.dwFileAttributes;
        e.size = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.created = fd.ftCreationTime;
        e.modified = fd.ftLastWriteTime;
        e.accessed = fd.ftLastAccessTime;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

void AddPseudoNodes(Tree& t)
{
    if (!t.Valid(t.root)) return;
    bool wholeVolume = IsWholeVolumePath(t.rootPath);
    if (!wholeVolume) return;
    VolumeInfo vi = QueryVolume(t.rootPath);
    t.volumeTotal = vi.total;
    t.volumeFree = vi.free;
    uint32_t freeNode = t.AddNode(t.root, L"Free Space", Kind::FreeSpace);
    t.nodes[freeNode].size = vi.free;
    t.nodes[freeNode].disk = vi.free;
    int64_t unknown = vi.total - t.nodes[t.root].disk - vi.free;
    if (unknown > 1024 * 1024)
    {
        uint32_t un = t.AddNode(t.root, L"Unknown (not yet scanned) Space", Kind::Unknown);
        t.nodes[un].size = unknown;
        t.nodes[un].disk = unknown;
    }
}

bool ScanFast(const std::wstring& path, const ScanOptions& opt, Tree& t, ScanProgress* prog)
{
    // P0-5: validate target exists and is a directory (using \\?\ for >MAX_PATH) so
    // inaccessible or invalid targets fail explicitly instead of silently returning an empty tree.
    std::wstring longRoot = ToLongPath(path);
    DWORD rootAttrs = GetFileAttributesW(longRoot.c_str());
    if (rootAttrs == INVALID_FILE_ATTRIBUTES || !(rootAttrs & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    VolumeInfo vi = QueryVolume(path);
    t.bytesPerCluster = vi.bytesPerCluster;
    t.rootPath = path;
    t.volumeFs = vi.fs;

    t.nodes.reserve(1 << 16);
    t.names.reserve(1 << 20);
    std::mutex treeMutex;
    t.root = AddNodeLocked(t, treeMutex, kNone, path, Kind::Drive);
    if (!IsWholeVolumePath(path))
        t.nodes[t.root].kind = (uint8_t)Kind::Folder;

    // Keep the machine responsive: one thread per physical core is plenty for an
    // SSD/NVMe walk, and more threads mostly adds scheduler pressure.
    int hw = (int)std::thread::hardware_concurrency();
    if (hw <= 0) hw = 4;
    int threads = opt.threads > 0 ? opt.threads : std::max<int>(2, std::min<int>(12, hw));

    std::vector<WorkItem> queue;
    queue.push_back({ t.root, path });
    std::mutex queueMutex;
    std::atomic<int> pending{ 1 };
    std::atomic<bool> cancel{ false };
    std::atomic<bool> rootFailed{ false };

    auto worker = [&]()
    {
        std::vector<Entry> entries;
        for (;;)
        {
            if (cancel.load() || (prog && InterlockedCompareExchange(&prog->state, 0, 0) == 2))
            {
                cancel = true;
                return;
            }
            WorkItem item;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (queue.empty())
                {
                    if (pending.load() == 0) return;
                }
                else
                {
                    item = std::move(queue.back());
                    queue.pop_back();
                }
            }
            if (item.path.empty())
            {
                // queue drained but work may still be in flight
                if (pending.load() == 0) return;
                Sleep(0);
                continue;
            }

            entries.clear();
            DWORD dirErr = ERROR_SUCCESS;
            bool ok = ScanDirectory(item.path, entries, &dirErr);
            if (!ok)
            {
                if (item.node == t.root && dirErr != ERROR_FILE_NOT_FOUND)
                    rootFailed = true;
                std::lock_guard<std::mutex> lock(treeMutex);
                if (item.node < t.nodes.size()) t.nodes[item.node].attrs |= 0x80000000u;
            }

            std::vector<WorkItem> subdirs;
            for (auto& e : entries)
            {
                bool isDir = (e.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                NodeInit init;
                init.attrs = e.attrs;
                init.created = e.created;
                init.modified = e.modified;
                init.accessed = e.accessed;

                if (isDir)
                {
                    init.size = 0;
                    init.disk = 0;
                }
                else
                {
                    init.size = e.size;
                    bool compressed = (e.attrs & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0;
                    if (compressed && !opt.fastDiskSize)
                    {
                        std::wstring full = item.path;
                        if (!full.empty() && full.back() != L'\\') full += L'\\';
                        full += e.name;
                        std::wstring longFull = ToLongPath(full);
                        DWORD high = 0;
                        DWORD low = GetCompressedFileSizeW(longFull.c_str(), &high);
                        init.disk = (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
                                        ? AlignToCluster(e.size, vi.bytesPerCluster)
                                        : (int64_t)(((uint64_t)high << 32) | low);
                    }
                    else if (compressed)
                    {
                        init.disk = e.size;
                    }
                    else
                    {
                        init.disk = AlignToCluster(e.size, vi.bytesPerCluster);
                    }
                    if (prog)
                    {
                        InterlockedIncrement64(&prog->files);
                        InterlockedAdd64(&prog->bytes, e.size);
                    }
                }

                uint32_t idx = AddNodeFilled(t, treeMutex, item.node, e.name, isDir ? Kind::Folder : Kind::File, init);
                if (isDir)
                {
                    if (prog) InterlockedIncrement64(&prog->dirs);
                    if (!(e.attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !opt.fastDiskSize)
                    {
                        std::wstring sub = item.path;
                        if (!sub.empty() && sub.back() != L'\\') sub += L'\\';
                        sub += e.name;
                        subdirs.push_back({ idx, std::move(sub) });
                    }
                }
            }

            if (!subdirs.empty())
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                for (auto& s : subdirs)
                {
                    queue.push_back(std::move(s));
                    pending++;
                }
            }
            if (--pending == 0)
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (queue.empty()) return;
            }
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    if (rootFailed.load() || cancel || (prog && InterlockedCompareExchange(&prog->state, 0, 0) == 2)) return false;

    t.Aggregate();
    if (opt.includePseudo) AddPseudoNodes(t);
    t.Aggregate();
    return true;
}

} // namespace

// ============================================================ MFT scanner
namespace {

struct DataExtent
{
    uint64_t recNo = 0;
    int64_t  lowestVcn = 0;
    int64_t  realSize = 0;
    int64_t  allocSize = 0;
    int64_t  compSize = 0;
    int64_t  runAllocBytes = 0;
    int64_t  runTotalBytes = 0;
    uint16_t attrId = 0;
    bool     nonRes = false;
    bool     isComp = false;
    bool     isSparse = false;
    bool     hasRuns = false;
};

struct MftEntry
{
    uint64_t parentRef = 0;
    std::wstring name;
    bool isDir = false;
    bool hasName = false;
    bool hasStdInfo = false;
    bool hasAttrList = false;
    bool compressed = false;
    bool sparse = false;
    uint8_t namePrio = 0;       // 3 = Win32/Win32&DOS, 2 = POSIX, 1 = DOS
    int64_t alloc = 0;
    int64_t real = 0;
    int64_t wofAlloc = 0;
    DWORD attrs = 0;
    FILETIME created{}, modified{}, accessed{};
};

struct AttrListRef
{
    uint64_t baseRecNo = 0;
    uint64_t extRecNo = 0;
    DWORD    type = 0;
    int64_t  lowestVcn = 0;
    uint16_t attrId = 0;
    bool     unnamed = true;
    bool     isWof = false;
};

struct ExtRecordInfo
{
    uint64_t extRecNo = 0;
    uint64_t baseRecNo = 0;
    bool     hasStdInfo = false;
    DWORD    attrs = 0;
    FILETIME created{}, modified{}, accessed{};
    bool     hasName = false;
    uint8_t  namePrio = 0;
    uint64_t parentRef = 0;
    std::wstring name;
    int64_t  wofAlloc = 0;
    std::vector<DataExtent> dataExtents;
};

struct NtfsVolumeData
{
    LARGE_INTEGER VolumeSerialNumber;
    LARGE_INTEGER NumberSectors;
    LARGE_INTEGER TotalClusters;
    LARGE_INTEGER FreeClusters;
    LARGE_INTEGER TotalReserved;
    DWORD BytesPerSector;
    DWORD BytesPerCluster;
    DWORD BytesPerFileRecordSegment;
    DWORD ClustersPerFileRecordSegment;
    LARGE_INTEGER MftValidDataLength;
    LARGE_INTEGER MftStartLcn;
    LARGE_INTEGER Mft2StartLcn;
    LARGE_INTEGER MftZoneStart;
    LARGE_INTEGER MftZoneEnd;
};

constexpr DWORD FSCTL_GET_NTFS_VOLUME_DATA_CODE = 0x00090064;
constexpr DWORD ATTR_STANDARD_INFORMATION = 0x10;
constexpr DWORD ATTR_ATTRIBUTE_LIST = 0x20;
constexpr DWORD ATTR_FILE_NAME = 0x30;
constexpr DWORD ATTR_DATA = 0x80;
constexpr DWORD ATTR_END = 0xFFFFFFFF;
constexpr int64_t kMaxSaneSize = (int64_t)1 << 62;

bool ReadAt(HANDLE h, int64_t offset, void* buf, DWORD count)
{
    if (offset < 0) return false;
    LARGE_INTEGER li; li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD total = 0;
    while (total < count)
    {
        DWORD got = 0;
        if (!ReadFile(h, (uint8_t*)buf + total, count - total, &got, nullptr) || got == 0) return false;
        total += got;
    }
    return true;
}

// P0-6: overflow-checked cluster-to-byte conversion reused across MFT parsing.
bool ClustersToBytes(int64_t clusters, int clusterSize, int64_t& outBytes)
{
    if (clusters < 0 || clusterSize <= 0) return false;
    if (clusters > kMaxSaneSize / (int64_t)clusterSize) return false;
    outBytes = clusters * (int64_t)clusterSize;
    return true;
}

// P0-6: return false on USN mismatch / malformed fixup array so callers skip torn records.
// Minimal synthetic test records with usaOff == 0 && usaCount == 0 remain accepted.
bool ApplyFixup(uint8_t* rec, DWORD recordSize)
{
    if (recordSize < 8) return false;
    WORD usaOff = *(WORD*)(rec + 4);
    WORD usaCount = *(WORD*)(rec + 6);
    if (usaOff == 0 && usaCount == 0) return true;
    if (usaCount <= 1 || usaOff == 0 || (DWORD)usaOff + (DWORD)usaCount * 2 > recordSize) return false;
    WORD usn = *(WORD*)(rec + usaOff);
    for (WORD i = 1; i < usaCount; ++i)
    {
        DWORD end = (DWORD)i * 512 - 2;
        if (end + 2 > recordSize) return false;
        if (*(WORD*)(rec + end) != usn) return false;
        *(WORD*)(rec + end) = *(WORD*)(rec + usaOff + i * 2);
    }
    return true;
}

struct Run { int64_t vcn = 0, lcn = 0, clusters = 0; bool sparse = false; };

std::vector<Run> ParseRuns(const uint8_t* rec, DWORD limit, DWORD off)
{
    std::vector<Run> runs;
    int64_t vcn = 0, lcn = 0;
    DWORD p = off;
    while (p < limit)
    {
        uint8_t header = rec[p++];
        if (header == 0) break;
        int lenBytes = header & 0x0F;
        int offBytes = (header >> 4) & 0x0F;
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8 || (DWORD)(lenBytes + offBytes) > limit - p) break;
        uint64_t ulen = 0;
        for (int i = 0; i < lenBytes; ++i) ulen |= (uint64_t)rec[p + i] << (8 * i);
        p += (DWORD)lenBytes;
        int64_t length = (int64_t)ulen;
        if (length <= 0) break;
        if (offBytes == 0)
        {
            runs.push_back({ vcn, -1, length, true });
            vcn += length;
            continue;
        }
        uint64_t udelta = 0;
        for (int i = 0; i < offBytes; ++i) udelta |= (uint64_t)rec[p + i] << (8 * i);
        if (offBytes < 8 && (rec[p + offBytes - 1] & 0x80))
            udelta |= (~0ULL << (8 * offBytes));
        p += (DWORD)offBytes;
        lcn += (int64_t)udelta;
        runs.push_back({ vcn, lcn, length, false });
        vcn += length;
    }
    return runs;
}

void ParseAttributes(const uint8_t* rec, DWORD recSize, DWORD recOff,
                     const std::function<void(DWORD, bool, DWORD, DWORD)>& visit)
{
    if (recSize < 0x38 || recOff + 0x38 > recSize) return;
    WORD attrOff = *(const WORD*)(rec + recOff + 0x14);
    DWORD bytesInUse = *(const DWORD*)(rec + recOff + 0x18);
    if (bytesInUse > recSize - recOff) bytesInUse = recSize - recOff;
    if (attrOff < 0x20 || attrOff >= bytesInUse) return;
    DWORD p = recOff + attrOff;
    DWORD limit = recOff + bytesInUse;
    int guard = 0;
    while (p + 16 <= limit && guard++ < 1024)
    {
        DWORD type = *(const DWORD*)(rec + p);
        if (type == ATTR_END || type == 0) break;
        DWORD len = *(const DWORD*)(rec + p + 4);
        if (len < 16 || len > limit - p) break;
        bool nonResident = rec[p + 8] != 0;
        visit(type, nonResident, p, len);
        p += len;
    }
}

FILETIME ToFt(int64_t v)
{
    FILETIME ft;
    ULARGE_INTEGER u; u.QuadPart = (uint64_t)(v > 0 ? v : 0);
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

uint8_t FileNamePriority(uint8_t ns)
{
    if (ns == 1 || ns == 3) return 3;   // Win32 / Win32&DOS
    if (ns == 0) return 2;              // POSIX
    return 1;                           // DOS 8.3
}

bool IsNtfsMetadataName(const std::wstring& name)
{
    if (name.empty() || name[0] != L'$') return false;
    static const wchar_t* const kMeta[] = {
        L"$MFT", L"$MFTMirr", L"$LogFile", L"$Volume", L"$AttrDef", L"$Bitmap",
        L"$Boot", L"$BadClus", L"$Secure", L"$UpCase", L"$Extend", L"$ObjId",
        L"$Quota", L"$Reparse", L"$UsnJrnl", L"$RmMetadata", L"$Txf", L"$TxfLog",
        L"$Tops", L"$Repair", L"$Verify", L"$Config", L"$Map", L"$Bad"
    };
    for (const wchar_t* m : kMeta)
        if (_wcsicmp(name.c_str(), m) == 0) return true;
    return false;
}

bool IsWofAttrName(const uint8_t* rec, DWORD aoff, DWORD attrLen, uint8_t nameLen, WORD nameOff)
{
    if (nameLen != 17) return false;
    if ((DWORD)nameOff + 34 > attrLen) return false;
    return _wcsnicmp((const wchar_t*)(rec + aoff + nameOff), L"WofCompressedData", 17) == 0;
}

bool ExtractDataExtent(const uint8_t* rec, DWORD recSize, DWORD aoff, DWORD attrLen,
                       int cluster, uint64_t recNo, DataExtent& out, bool& isWof)
{
    isWof = false;
    if (aoff + attrLen > recSize || attrLen < 0x18) return false;
    uint8_t nameLen = rec[aoff + 9];
    WORD nameOff = *(const WORD*)(rec + aoff + 0x0A);
    if (nameLen != 0)
    {
        if (IsWofAttrName(rec, aoff, attrLen, nameLen, nameOff)) isWof = true;
        else return false;
    }

    bool nonRes = rec[aoff + 8] != 0;
    out = DataExtent{};
    out.recNo = recNo;
    out.nonRes = nonRes;
    out.attrId = *(const WORD*)(rec + aoff + 0x0E);

    if (!nonRes)
    {
        DWORD valueLen = *(const DWORD*)(rec + aoff + 0x10);
        out.realSize = (int64_t)valueLen;
        return true;
    }

    if (attrLen < 0x40) return false;
    WORD aflags = *(const WORD*)(rec + aoff + 0x0C);
    out.isComp = (aflags & 0x0001) != 0;
    out.isSparse = (aflags & 0x8000) != 0;
    out.lowestVcn = *(const int64_t*)(rec + aoff + 0x10);
    int64_t highestVcn = *(const int64_t*)(rec + aoff + 0x18);
    WORD runOff = *(const WORD*)(rec + aoff + 0x20);
    WORD compUnit = *(const WORD*)(rec + aoff + 0x22);

    if (out.lowestVcn == 0)
    {
        out.allocSize = *(const int64_t*)(rec + aoff + 0x28);
        out.realSize = *(const int64_t*)(rec + aoff + 0x30);
        if ((compUnit > 0 || out.isComp || out.isSparse) && runOff >= 0x48 && attrLen >= 0x48)
            out.compSize = *(const int64_t*)(rec + aoff + 0x40);
    }

    if (runOff >= 0x40 && runOff < attrLen)
    {
        auto runs = ParseRuns(rec, aoff + attrLen, aoff + runOff);
        if (!runs.empty()) out.hasRuns = true;
        for (const auto& r : runs)
        {
            int64_t bytes = 0;
            if (r.clusters <= 0 || !ClustersToBytes(r.clusters, cluster, bytes)) continue;
            out.runTotalBytes += bytes;
            if (!r.sparse) out.runAllocBytes += bytes;
        }
        if (runs.empty() && highestVcn >= out.lowestVcn && highestVcn - out.lowestVcn < (1LL << 40))
        {
            int64_t span = (highestVcn - out.lowestVcn + 1) * (int64_t)cluster;
            if (span > 0 && span < kMaxSaneSize) out.runTotalBytes = span;
        }
    }
    return true;
}

bool ReadAttrListPayload(HANDLE h, const uint8_t* rec, DWORD recSize, DWORD aoff, DWORD attrLen,
                         bool nonRes, int cluster, std::vector<uint8_t>& outBuf)
{
    outBuf.clear();
    if (aoff + attrLen > recSize) return false;
    if (!nonRes)
    {
        if (attrLen < 0x18) return false;
        DWORD valLen = *(const DWORD*)(rec + aoff + 0x10);
        WORD valOff = *(const WORD*)(rec + aoff + 0x14);
        if (valLen == 0 || valOff > attrLen || valLen > attrLen - valOff) return false;
        outBuf.assign(rec + aoff + valOff, rec + aoff + valOff + valLen);
        return true;
    }

    if (h == INVALID_HANDLE_VALUE || attrLen < 0x40 || cluster <= 0) return false;
    WORD runOff = *(const WORD*)(rec + aoff + 0x20);
    int64_t realSize = *(const int64_t*)(rec + aoff + 0x30);
    if (realSize <= 0 || realSize > 16LL * 1024 * 1024 || runOff < 0x40 || runOff >= attrLen) return false;
    auto runs = ParseRuns(rec, aoff + attrLen, aoff + runOff);
    if (runs.empty()) return false;

    outBuf.assign((size_t)realSize, 0);
    size_t written = 0;
    for (const auto& r : runs)
    {
        if (written >= outBuf.size()) break;
        int64_t rbytes = 0;
        if (r.clusters <= 0 || !ClustersToBytes(r.clusters, cluster, rbytes)) return false;
        size_t take = (size_t)std::min<int64_t>(rbytes, (int64_t)(outBuf.size() - written));
        if (!r.sparse && r.lcn >= 0)
        {
            int64_t diskOff = 0;
            if (!ClustersToBytes(r.lcn, cluster, diskOff)) return false;
            if (!ReadAt(h, diskOff, outBuf.data() + written, (DWORD)take))
                return false;
        }
        written += take;
    }
    return written > 0;
}

void ParseAttrListEntries(const uint8_t* buf, size_t len, uint64_t baseRecNo,
                          std::vector<AttrListRef>& outRefs)
{
    size_t pos = 0;
    int guard = 0;
    while (pos + 0x1A <= len && guard++ < 65536)
    {
        DWORD type = *(const DWORD*)(buf + pos);
        WORD rlen = *(const WORD*)(buf + pos + 4);
        if (rlen < 0x1A || rlen > len - pos) break;
        uint8_t nameLen = buf[pos + 6];
        uint8_t nameOff = buf[pos + 7];
        int64_t lowestVcn = *(const int64_t*)(buf + pos + 8);
        uint64_t segRef = *(const uint64_t*)(buf + pos + 0x10);
        WORD attrId = *(const WORD*)(buf + pos + 0x18);
        uint64_t extRecNo = segRef & 0xFFFFFFFFFFFFull;

        if (extRecNo != baseRecNo &&
            (type == ATTR_DATA || type == ATTR_FILE_NAME || type == ATTR_STANDARD_INFORMATION))
        {
            bool unnamed = (nameLen == 0);
            bool isWof = false;
            if (type == ATTR_DATA && nameLen == 17 && (size_t)nameOff + 34 <= (size_t)rlen)
            {
                isWof = _wcsnicmp((const wchar_t*)(buf + pos + nameOff), L"WofCompressedData", 17) == 0;
            }
            if (type != ATTR_DATA || unnamed || isWof)
            {
                outRefs.push_back({ baseRecNo, extRecNo, type, lowestVcn, attrId, unnamed, isWof });
            }
        }
        pos += rlen;
    }
}

void FinalizeMftEntrySizes(MftEntry& e, std::vector<DataExtent>& exts, int cluster)
{
    if (e.attrs & FILE_ATTRIBUTE_DIRECTORY) e.isDir = true;
    if (e.isDir)
    {
        e.real = 0;
        e.alloc = 0;
        return;
    }

    if (exts.size() > 1)
    {
        std::sort(exts.begin(), exts.end(), [](const DataExtent& a, const DataExtent& b)
        {
            if (a.lowestVcn != b.lowestVcn) return a.lowestVcn < b.lowestVcn;
            if (a.recNo != b.recNo) return a.recNo < b.recNo;
            return a.attrId < b.attrId;
        });
        auto last = std::unique(exts.begin(), exts.end(), [](const DataExtent& a, const DataExtent& b)
        {
            return a.lowestVcn == b.lowestVcn && a.recNo == b.recNo && a.attrId == b.attrId;
        });
        exts.erase(last, exts.end());
    }

    int64_t maxReal = 0;
    int64_t maxAlloc = 0;
    int64_t maxComp = 0;
    int64_t sumRunAlloc = 0;
    int64_t sumRunTotal = 0;
    bool hasNonRes = false;
    bool anyComp = false;
    bool anySparse = false;
    bool anyRuns = false;

    for (const auto& d : exts)
    {
        if (d.realSize > maxReal && d.realSize < kMaxSaneSize) maxReal = d.realSize;
        if (d.nonRes)
        {
            hasNonRes = true;
            anyComp |= d.isComp;
            anySparse |= d.isSparse;
            anyRuns |= d.hasRuns;
            if (d.allocSize > maxAlloc && d.allocSize < kMaxSaneSize) maxAlloc = d.allocSize;
            if (d.compSize > maxComp && d.compSize < kMaxSaneSize) maxComp = d.compSize;
            if (d.runAllocBytes > 0 && sumRunAlloc < kMaxSaneSize - d.runAllocBytes)
                sumRunAlloc += d.runAllocBytes;
            if (d.runTotalBytes > 0 && sumRunTotal < kMaxSaneSize - d.runTotalBytes)
                sumRunTotal += d.runTotalBytes;
        }
    }

    e.real = maxReal;
    if (e.real == 0 && sumRunTotal > 0) e.real = sumRunTotal;

    if (!hasNonRes)
    {
        e.alloc = AlignToCluster(e.real, cluster);
    }
    else if (anyComp || anySparse)
    {
        if (maxComp > 0) e.alloc = maxComp;
        else if (sumRunAlloc > 0) e.alloc = sumRunAlloc;
        else if (anySparse && anyRuns) e.alloc = 0;
        else if (maxAlloc > 0) e.alloc = maxAlloc;
        else e.alloc = AlignToCluster(e.real, cluster);
    }
    else
    {
        int64_t disk = std::max(maxAlloc, sumRunAlloc);
        int64_t aligned = AlignToCluster(e.real, cluster);
        if (disk < aligned) disk = aligned;
        e.alloc = disk;
    }

    if (e.alloc == 0 && e.wofAlloc > 0 && e.wofAlloc < kMaxSaneSize)
        e.alloc = e.wofAlloc;

    if (e.real < 0 || e.real > kMaxSaneSize) e.real = 0;
    if (e.alloc < 0 || e.alloc > kMaxSaneSize) e.alloc = 0;
    e.compressed = anyComp;
    e.sparse = anySparse;
}

bool ReadMftRecordByNumber(HANDLE h, const std::vector<Run>& mftRuns, int cluster, DWORD recSize,
                           uint64_t recNo, std::vector<uint8_t>& buf)
{
    if (cluster <= 0 || recSize < 0x38) return false;
    if (recNo > (uint64_t)(kMaxSaneSize / recSize)) return false;
    int64_t byteOff = (int64_t)recNo * (int64_t)recSize;
    if (byteOff < 0) return false;
    buf.assign(recSize, 0);

    DWORD done = 0;
    int64_t curVcnByte = 0;
    for (const auto& r : mftRuns)
    {
        int64_t runLen = 0;
        if (r.clusters <= 0 || !ClustersToBytes(r.clusters, cluster, runLen)) continue;
        if (curVcnByte > kMaxSaneSize - runLen) return false;
        int64_t runEnd = curVcnByte + runLen;
        int64_t wantPos = byteOff + done;
        if (wantPos >= curVcnByte && wantPos < runEnd)
        {
            DWORD take = (DWORD)std::min<int64_t>((int64_t)(recSize - done), runEnd - wantPos);
            if (r.sparse || r.lcn < 0) return false;
            int64_t lcnBase = 0;
            if (!ClustersToBytes(r.lcn, cluster, lcnBase)) return false;
            int64_t diskOff = lcnBase + (wantPos - curVcnByte);
            if (!ReadAt(h, diskOff, buf.data() + done, take)) return false;
            done += take;
            if (done == recSize) break;
        }
        curVcnByte = runEnd;
    }
    if (done != recSize) return false;
    if (buf[0] != 'F' || buf[1] != 'I' || buf[2] != 'L' || buf[3] != 'E') return false;
    if (!ApplyFixup(buf.data(), recSize)) return false;
    return true;
}

bool ReadMftRuns(HANDLE h, int64_t mftStartLcn, int cluster, DWORD recSize, std::vector<Run>& outRuns)
{
    outRuns.clear();
    int64_t mftStartOff = 0;
    if (!ClustersToBytes(mftStartLcn, cluster, mftStartOff)) return false;
    std::vector<uint8_t> rec0(recSize);
    if (!ReadAt(h, mftStartOff, rec0.data(), recSize)) return false;
    if (rec0[0] != 'F' || rec0[1] != 'I' || rec0[2] != 'L' || rec0[3] != 'E') return false;
    if (!ApplyFixup(rec0.data(), recSize)) return false;

    struct MftRunChunk { int64_t lowestVcn; std::vector<Run> runs; };
    std::vector<MftRunChunk> chunks;
    std::vector<AttrListRef> alRefs;

    ParseAttributes(rec0.data(), recSize, 0, [&](DWORD type, bool nonRes, DWORD aoff, DWORD attrLen)
    {
        if (type == ATTR_DATA && nonRes && rec0[aoff + 9] == 0 && attrLen >= 0x40)
        {
            int64_t lowestVcn = *(const int64_t*)(rec0.data() + aoff + 0x10);
            WORD runOff = *(const WORD*)(rec0.data() + aoff + 0x20);
            if (runOff >= 0x40 && runOff < attrLen)
            {
                auto r = ParseRuns(rec0.data(), aoff + attrLen, aoff + runOff);
                if (!r.empty()) chunks.push_back({ lowestVcn, std::move(r) });
            }
        }
        else if (type == ATTR_ATTRIBUTE_LIST)
        {
            std::vector<uint8_t> payload;
            if (ReadAttrListPayload(h, rec0.data(), recSize, aoff, attrLen, nonRes, cluster, payload))
                ParseAttrListEntries(payload.data(), payload.size(), 0, alRefs);
        }
    });

    auto rebuildRuns = [&]()
    {
        std::sort(chunks.begin(), chunks.end(), [](const MftRunChunk& a, const MftRunChunk& b)
        { return a.lowestVcn < b.lowestVcn; });
        outRuns.clear();
        int64_t nextVcn = 0;
        for (const auto& c : chunks)
        {
            if (c.lowestVcn < nextVcn) continue;
            for (auto r : c.runs)
            {
                r.vcn = nextVcn;
                nextVcn += r.clusters;
                outRuns.push_back(r);
            }
        }
    };
    rebuildRuns();

    // If $MFT (Record 0) itself has $ATTRIBUTE_LIST, follow its $DATA extension records in VCN order.
    if (!alRefs.empty() && !outRuns.empty())
    {
        std::vector<std::pair<int64_t, uint64_t>> dataExts;
        for (const auto& ref : alRefs)
            if (ref.type == ATTR_DATA && ref.unnamed && ref.extRecNo != 0)
                dataExts.push_back({ ref.lowestVcn, ref.extRecNo });
        std::sort(dataExts.begin(), dataExts.end());
        std::vector<uint64_t> seenExt;
        std::vector<uint8_t> extBuf;
        for (const auto& de : dataExts)
        {
            if (std::find(seenExt.begin(), seenExt.end(), de.second) != seenExt.end()) continue;
            seenExt.push_back(de.second);
            if (!ReadMftRecordByNumber(h, outRuns, cluster, recSize, de.second, extBuf)) continue;
            ParseAttributes(extBuf.data(), recSize, 0, [&](DWORD type, bool nonRes, DWORD aoff, DWORD attrLen)
            {
                if (type == ATTR_DATA && nonRes && extBuf[aoff + 9] == 0 && attrLen >= 0x40)
                {
                    int64_t lowestVcn = *(const int64_t*)(extBuf.data() + aoff + 0x10);
                    WORD runOff = *(const WORD*)(extBuf.data() + aoff + 0x20);
                    if (runOff >= 0x40 && runOff < attrLen)
                    {
                        auto r = ParseRuns(extBuf.data(), aoff + attrLen, aoff + runOff);
                        if (!r.empty()) chunks.push_back({ lowestVcn, std::move(r) });
                    }
                }
            });
            rebuildRuns();
        }
    }

    return !outRuns.empty();
}

// Parses a single extension record's attributes into ExtRecordInfo.
void ParseExtensionRecordAttrs(const uint8_t* rec, DWORD recSize, int cluster,
                               uint64_t extRecNo, uint64_t baseRecNo, ExtRecordInfo& ext)
{
    ext = ExtRecordInfo{};
    ext.extRecNo = extRecNo;
    ext.baseRecNo = baseRecNo;

    ParseAttributes(rec, recSize, 0, [&](DWORD type, bool nonRes, DWORD aoff, DWORD attrLen)
    {
        if (type == ATTR_STANDARD_INFORMATION && !nonRes && attrLen >= 0x18)
        {
            WORD vo = *(const WORD*)(rec + aoff + 0x14);
            DWORD v = aoff + vo;
            if (vo < attrLen && v + 0x24 <= aoff + attrLen)
            {
                ext.created = ToFt(*(const int64_t*)(rec + v));
                ext.modified = ToFt(*(const int64_t*)(rec + v + 8));
                ext.accessed = ToFt(*(const int64_t*)(rec + v + 24));
                ext.attrs = *(const DWORD*)(rec + v + 0x20);
                ext.hasStdInfo = true;
            }
        }
        else if (type == ATTR_FILE_NAME && !nonRes && attrLen >= 0x18)
        {
            WORD vo = *(const WORD*)(rec + aoff + 0x14);
            DWORD v = aoff + vo;
            if (vo < attrLen && v + 0x42 <= aoff + attrLen)
            {
                uint8_t ns = rec[v + 0x41];
                DWORD nl = rec[v + 0x40];
                if (nl > 0 && v + 0x42 + nl * 2 <= aoff + attrLen)
                {
                    uint8_t prio = FileNamePriority(ns);
                    if (!ext.hasName || prio > ext.namePrio)
                    {
                        ext.parentRef = *(const uint64_t*)(rec + v);
                        ext.name.assign((const wchar_t*)(rec + v + 0x42), nl);
                        ext.hasName = true;
                        ext.namePrio = prio;
                    }
                }
            }
        }
        else if (type == ATTR_DATA)
        {
            DataExtent d;
            bool isWof = false;
            if (ExtractDataExtent(rec, recSize, aoff, attrLen, cluster, extRecNo, d, isWof))
            {
                if (isWof)
                {
                    int64_t w = d.nonRes ? (d.compSize > 0 ? d.compSize : (d.runAllocBytes > 0 ? d.runAllocBytes : d.allocSize)) : d.realSize;
                    if (w > ext.wofAlloc && w < kMaxSaneSize) ext.wofAlloc = w;
                }
                else
                {
                    ext.dataExtents.push_back(d);
                }
            }
        }
    });
}

} // namespace

// The implementation above is superseded; see ScanMftEx below (kept single and correct).
namespace {

// Parses one standalone FILE record into an MftEntry. Returns false for records that are free,
// extension records, unnamed, or NTFS metadata ($MFT, $LogFile, ...).
// Shared by the unit test (see SS_TEST_HOOKS at the bottom).
[[maybe_unused]] bool ParseMftRecord(uint8_t* rec, DWORD recSize, int cluster, MftEntry& e)
{
    if (recSize < 0x38) return false;
    if (!ApplyFixup(rec, recSize)) return false;

    WORD flags = *(const WORD*)(rec + 0x16);
    if (!(flags & 0x1)) return false;              // free record
    uint64_t baseRef = *(const uint64_t*)(rec + 0x20);
    if ((baseRef & 0xFFFFFFFFFFFFull) != 0) return false;  // extension record

    e = MftEntry{};
    e.isDir = (flags & 0x2) != 0;
    e.attrs = FILE_ATTRIBUTE_NORMAL;
    std::vector<DataExtent> exts;

    ParseAttributes(rec, recSize, 0, [&](DWORD type, bool nonRes, DWORD aoff, DWORD attrLen)
    {
        if (type == ATTR_STANDARD_INFORMATION && !nonRes && attrLen >= 0x18)
        {
            WORD vo = *(const WORD*)(rec + aoff + 0x14);
            DWORD v = aoff + vo;
            if (vo < attrLen && v + 0x24 <= aoff + attrLen)
            {
                e.created = ToFt(*(const int64_t*)(rec + v));
                e.modified = ToFt(*(const int64_t*)(rec + v + 8));
                e.accessed = ToFt(*(const int64_t*)(rec + v + 24));
                e.attrs = *(const DWORD*)(rec + v + 0x20);
                e.hasStdInfo = true;
            }
        }
        else if (type == ATTR_FILE_NAME && !nonRes && attrLen >= 0x18)
        {
            WORD vo = *(const WORD*)(rec + aoff + 0x14);
            DWORD v = aoff + vo;
            if (vo < attrLen && v + 0x42 <= aoff + attrLen)
            {
                uint8_t ns = rec[v + 0x41];
                DWORD nl = rec[v + 0x40];
                if (nl > 0 && v + 0x42 + nl * 2 <= aoff + attrLen)
                {
                    uint8_t prio = FileNamePriority(ns);
                    if (!e.hasName || prio > e.namePrio)
                    {
                        e.parentRef = *(const uint64_t*)(rec + v);
                        e.name.assign((const wchar_t*)(rec + v + 0x42), nl);
                        e.hasName = true;
                        e.namePrio = prio;
                    }
                }
            }
        }
        else if (type == ATTR_DATA)
        {
            DataExtent d;
            bool isWof = false;
            if (ExtractDataExtent(rec, recSize, aoff, attrLen, cluster, 0, d, isWof))
            {
                if (isWof)
                {
                    int64_t w = d.nonRes ? (d.compSize > 0 ? d.compSize : (d.runAllocBytes > 0 ? d.runAllocBytes : d.allocSize)) : d.realSize;
                    if (w > e.wofAlloc && w < kMaxSaneSize) e.wofAlloc = w;
                }
                else
                {
                    exts.push_back(d);
                }
            }
        }
    });

    if (!e.hasName || e.name.empty()) return false;
    if (IsNtfsMetadataName(e.name)) return false;

    FinalizeMftEntrySizes(e, exts, cluster);
    return true;
}

bool ScanMftEx(const std::wstring& path, const ScanOptions& opt, Tree& t, ScanProgress* prog)
{
    wchar_t drv = ExtractDriveLetter(path);
    if (!drv) return false;
    wchar_t dev[16] = { L'\\', L'\\', L'.', L'\\', drv, L':', 0 };
    HANDLE h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD bytes = 0;
    NtfsVolumeData nvd{};
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA_CODE, nullptr, 0, &nvd, sizeof(nvd), &bytes, nullptr))
    {
        CloseHandle(h);
        return false;
    }

    DWORD recSize = nvd.BytesPerFileRecordSegment;
    if (recSize == 0)
    {
        int cpr = (int)nvd.ClustersPerFileRecordSegment;
        recSize = cpr > 0 ? (DWORD)(cpr * nvd.BytesPerCluster) : (cpr >= -16 ? (1u << (-cpr)) : 1024u);
    }
    int cluster = (int)nvd.BytesPerCluster;
    if (recSize < 512 || recSize > 65536 || cluster < 512 || cluster > 1024 * 1024)
    {
        CloseHandle(h);
        return false;
    }

    std::vector<Run> runs;
    if (!ReadMftRuns(h, nvd.MftStartLcn.QuadPart, cluster, recSize, runs))
    {
        CloseHandle(h);
        return false;
    }

    int64_t available = 0;
    for (const auto& r : runs)
    {
        int64_t rbytes = 0;
        if (r.clusters > 0 && ClustersToBytes(r.clusters, cluster, rbytes) && available <= kMaxSaneSize - rbytes)
            available += rbytes;
    }
    int64_t mftBytes = nvd.MftValidDataLength.QuadPart > 0
                           ? std::min<int64_t>(nvd.MftValidDataLength.QuadPart, available)
                           : available;
    // Align total MFT byte budget to whole FILE records (also preserves sector alignment).
    mftBytes = (mftBytes / recSize) * recSize;
    if (mftBytes <= 0) { CloseHandle(h); return false; }

    const size_t estRecords = (size_t)std::min<int64_t>(mftBytes / recSize + 16, 16LL * 1024 * 1024);
    std::vector<MftEntry> entries;
    entries.reserve(estRecords);
    std::vector<uint64_t> refs;
    refs.reserve(estRecords);

    // Base records that have $ATTRIBUTE_LIST (or multiple $DATA extents) store their
    // local $DATA extents here for deduplicated merging with extension records.
    std::vector<std::pair<uint32_t, DataExtent>> baseMultiExtents;
    std::vector<AttrListRef> attrListRefs;
    std::vector<ExtRecordInfo> extRecords;

    const DWORD chunkRecords = 8192;
    DWORD chunkBytes = chunkRecords * recSize;
    std::vector<uint8_t> chunk(chunkBytes);
    // P0-6: carry partial FILE record bytes across run/chunk boundaries (e.g. cluster < recSize)
    // so a FILE record is never truncated or misaligned.
    std::vector<uint8_t> carry;
    carry.reserve(recSize);
    std::vector<uint8_t> alPayload;
    std::vector<DataExtent> localExtents;
    int64_t recordIndex = 0;
    int64_t left = mftBytes;

    auto processRecord = [&](uint8_t* rec, int64_t recIdx)
    {
        if (rec[0] != 'F' || rec[1] != 'I' || rec[2] != 'L' || rec[3] != 'E') return;
        if (!ApplyFixup(rec, recSize)) return;

        WORD flags = *(const WORD*)(rec + 0x16);
        if (!(flags & 0x1)) return;   // free record

        uint64_t recNo = (uint64_t)recIdx;
        uint64_t baseRef = *(const uint64_t*)(rec + 0x20);
        uint64_t baseRecNo = baseRef & 0xFFFFFFFFFFFFull;

        if (baseRecNo != 0)
        {
            // Extension record: NEVER add as an independent directory-tree node.
            // Collect its attributes so we can merge them back into its base record.
            if (baseRecNo < 16 && baseRecNo != 5) return;
            ExtRecordInfo ext;
            ParseExtensionRecordAttrs(rec, recSize, cluster, recNo, baseRecNo, ext);
            if (ext.hasStdInfo || ext.hasName || !ext.dataExtents.empty() || ext.wofAlloc > 0)
                extRecords.push_back(std::move(ext));
            return;
        }

        // Base record: skip reserved NTFS metadata records 0..15 (except 5 = root dir).
        if (recNo < 16 && recNo != 5) return;

        MftEntry e;
        e.isDir = (flags & 0x2) != 0;
        e.attrs = FILE_ATTRIBUTE_NORMAL;
        localExtents.clear();

        ParseAttributes(rec, recSize, 0, [&](DWORD type, bool nonRes, DWORD aoff, DWORD attrLen)
        {
            if (type == ATTR_STANDARD_INFORMATION && !nonRes && attrLen >= 0x18)
            {
                WORD vo = *(const WORD*)(rec + aoff + 0x14);
                DWORD v = aoff + vo;
                if (vo < attrLen && v + 0x24 <= aoff + attrLen)
                {
                    e.created = ToFt(*(const int64_t*)(rec + v));
                    e.modified = ToFt(*(const int64_t*)(rec + v + 8));
                    e.accessed = ToFt(*(const int64_t*)(rec + v + 24));
                    e.attrs = *(const DWORD*)(rec + v + 0x20);
                    e.hasStdInfo = true;
                }
            }
            else if (type == ATTR_ATTRIBUTE_LIST)
            {
                e.hasAttrList = true;
                if (ReadAttrListPayload(h, rec, recSize, aoff, attrLen, nonRes, cluster, alPayload))
                    ParseAttrListEntries(alPayload.data(), alPayload.size(), recNo, attrListRefs);
            }
            else if (type == ATTR_FILE_NAME && !nonRes && attrLen >= 0x18)
            {
                WORD vo = *(const WORD*)(rec + aoff + 0x14);
                DWORD v = aoff + vo;
                if (vo < attrLen && v + 0x42 <= aoff + attrLen)
                {
                    uint8_t ns = rec[v + 0x41];
                    DWORD nl = rec[v + 0x40];
                    if (nl > 0 && v + 0x42 + nl * 2 <= aoff + attrLen)
                    {
                        uint8_t prio = FileNamePriority(ns);
                        if (!e.hasName || prio > e.namePrio)
                        {
                            e.parentRef = *(const uint64_t*)(rec + v);
                            e.name.assign((const wchar_t*)(rec + v + 0x42), nl);
                            e.hasName = true;
                            e.namePrio = prio;
                        }
                    }
                }
            }
            else if (type == ATTR_DATA)
            {
                DataExtent d;
                bool isWof = false;
                if (ExtractDataExtent(rec, recSize, aoff, attrLen, cluster, recNo, d, isWof))
                {
                    if (isWof)
                    {
                        int64_t w = d.nonRes ? (d.compSize > 0 ? d.compSize : (d.runAllocBytes > 0 ? d.runAllocBytes : d.allocSize)) : d.realSize;
                        if (w > e.wofAlloc && w < kMaxSaneSize) e.wofAlloc = w;
                    }
                    else
                    {
                        localExtents.push_back(d);
                    }
                }
            }
        });

        // Keep base records that either have a name, are record 5, or have $ATTRIBUTE_LIST
        // (their $FILE_NAME or $DATA may reside in an extension record).
        if (recNo != 5 && !e.hasName && !e.hasAttrList) return;
        if (recNo != 5 && e.hasName && IsNtfsMetadataName(e.name)) return;

        uint32_t baseIdx = (uint32_t)entries.size();
        if (e.hasAttrList)
        {
            for (const auto& d : localExtents)
                baseMultiExtents.push_back({ baseIdx, d });
        }
        else
        {
            FinalizeMftEntrySizes(e, localExtents, cluster);
        }

        refs.push_back(recNo);
        entries.push_back(std::move(e));
    };

    auto skipStreamBytes = [&](int64_t skipBytes)
    {
        int64_t totalSkip = (int64_t)carry.size() + skipBytes;
        recordIndex += totalSkip / recSize;
        carry.clear();
        size_t rem = (size_t)(totalSkip % recSize);
        if (rem > 0) carry.assign(rem, 0);
        left -= skipBytes;
    };

    for (const auto& run : runs)
    {
        if (left <= 0) break;
        int64_t runLen = 0;
        if (run.clusters <= 0 || !ClustersToBytes(run.clusters, cluster, runLen)) continue;
        int64_t runBytes = std::min(runLen, left);
        if (run.sparse || run.lcn < 0)
        {
            skipStreamBytes(runBytes);
            continue;
        }
        int64_t runDiskBase = 0;
        if (!ClustersToBytes(run.lcn, cluster, runDiskBase))
        {
            skipStreamBytes(runBytes);
            continue;
        }
        int64_t runOff = 0;
        while (runOff < runBytes)
        {
            if (prog && InterlockedCompareExchange(&prog->state, 0, 0) == 2)
            {
                CloseHandle(h);
                return false;
            }
            DWORD want = (DWORD)std::min<int64_t>(chunkBytes, runBytes - runOff);
            if (want == 0) break;
            if (!ReadAt(h, runDiskBase + runOff, chunk.data(), want))
            {
                // Advance recordIndex past the unreadable remainder of this run so
                // subsequent MFT record numbers remain exact.
                skipStreamBytes(runBytes - runOff);
                break;
            }

            DWORD pos = 0;
            if (!carry.empty())
            {
                DWORD need = recSize - (DWORD)carry.size();
                DWORD take = std::min(need, want);
                carry.insert(carry.end(), chunk.data(), chunk.data() + take);
                pos += take;
                if (carry.size() == recSize)
                {
                    processRecord(carry.data(), recordIndex++);
                    carry.clear();
                }
            }
            while (pos + recSize <= want)
            {
                processRecord(chunk.data() + pos, recordIndex++);
                pos += recSize;
            }
            if (pos < want)
            {
                carry.assign(chunk.data() + pos, chunk.data() + want);
            }

            runOff += want;
            left -= want;
        }
    }

    if (entries.empty()) { CloseHandle(h); return false; }

    // Index base records by 48-bit record number.
    const uint64_t mask = 0xFFFFFFFFFFFFull;
    std::vector<std::pair<uint64_t, uint32_t>> baseMap;
    baseMap.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        baseMap.push_back({ refs[i] & mask, (uint32_t)i });
    std::sort(baseMap.begin(), baseMap.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    auto lookupBase = [&](uint64_t ref) -> uint32_t
    {
        uint64_t key = ref & mask;
        auto it = std::lower_bound(baseMap.begin(), baseMap.end(), std::make_pair(key, (uint32_t)0));
        if (it != baseMap.end() && it->first == key) return it->second;
        return kNone;
    };

    // Index extension records seen during the linear scan.
    std::vector<std::pair<uint64_t, uint32_t>> extMap;
    extMap.reserve(extRecords.size());
    for (size_t i = 0; i < extRecords.size(); ++i)
        extMap.push_back({ extRecords[i].extRecNo & mask, (uint32_t)i });
    std::sort(extMap.begin(), extMap.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    auto lookupExt = [&](uint64_t recNo) -> uint32_t
    {
        uint64_t key = recNo & mask;
        auto it = std::lower_bound(extMap.begin(), extMap.end(), std::make_pair(key, (uint32_t)0));
        if (it != extMap.end() && it->first == key) return it->second;
        return kNone;
    };

    // On-demand fallback read for any extension record referenced by $ATTRIBUTE_LIST
    // that wasn't encountered during the linear pass (e.g., beyond MftValidDataLength).
    if (!attrListRefs.empty())
    {
        std::vector<uint64_t> missingExts;
        for (const auto& ref : attrListRefs)
        {
            if (lookupExt(ref.extRecNo) == kNone)
                missingExts.push_back(ref.extRecNo);
        }
        if (!missingExts.empty())
        {
            std::sort(missingExts.begin(), missingExts.end());
            missingExts.erase(std::unique(missingExts.begin(), missingExts.end()), missingExts.end());
            std::vector<uint8_t> tmpRec;
            bool addedAny = false;
            for (uint64_t extRecNo : missingExts)
            {
                if (!ReadMftRecordByNumber(h, runs, cluster, recSize, extRecNo, tmpRec)) continue;
                WORD flags = *(const WORD*)(tmpRec.data() + 0x16);
                if (!(flags & 0x1)) continue;
                uint64_t baseRecNo = *(const uint64_t*)(tmpRec.data() + 0x20) & mask;
                if (baseRecNo == 0) continue;
                ExtRecordInfo ext;
                ParseExtensionRecordAttrs(tmpRec.data(), recSize, cluster, extRecNo, baseRecNo, ext);
                if (ext.hasStdInfo || ext.hasName || !ext.dataExtents.empty() || ext.wofAlloc > 0)
                {
                    extRecords.push_back(std::move(ext));
                    addedAny = true;
                }
            }
            if (addedAny)
            {
                extMap.clear();
                extMap.reserve(extRecords.size());
                for (size_t i = 0; i < extRecords.size(); ++i)
                    extMap.push_back({ extRecords[i].extRecNo & mask, (uint32_t)i });
                std::sort(extMap.begin(), extMap.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            }
        }
    }
    CloseHandle(h);

    // Merge extension records back into their base records (both via $ATTRIBUTE_LIST
    // references and via extension record baseRef back-pointers, deduplicated).
    std::vector<uint8_t> extMerged(extRecords.size(), 0);

    auto mergeExtIntoBase = [&](uint32_t baseIdx, uint32_t extIdx)
    {
        if (extMerged[extIdx]) return;
        const ExtRecordInfo& ext = extRecords[extIdx];
        if ((ext.baseRecNo & mask) != (refs[baseIdx] & mask)) return;
        extMerged[extIdx] = 1;

        MftEntry& base = entries[baseIdx];
        if (!base.hasStdInfo && ext.hasStdInfo)
        {
            base.created = ext.created;
            base.modified = ext.modified;
            base.accessed = ext.accessed;
            base.attrs = ext.attrs;
            base.hasStdInfo = true;
        }
        if (ext.hasName && (!base.hasName || ext.namePrio > base.namePrio))
        {
            base.parentRef = ext.parentRef;
            base.name = ext.name;
            base.hasName = true;
            base.namePrio = ext.namePrio;
        }
        if (ext.wofAlloc > base.wofAlloc && ext.wofAlloc < kMaxSaneSize)
            base.wofAlloc = ext.wofAlloc;

        if (!ext.dataExtents.empty())
        {
            // If base wasn't marked hasAttrList, preserve any existing finalized sizes
            // as a synthetic extent before re-finalizing with extension extents.
            if (!base.hasAttrList)
            {
                base.hasAttrList = true;
                if (base.real > 0 || base.alloc > 0)
                {
                    DataExtent synth{};
                    synth.recNo = refs[baseIdx];
                    synth.lowestVcn = 0;
                    synth.realSize = base.real;
                    synth.allocSize = base.alloc;
                    synth.nonRes = (base.alloc > 0);
                    synth.isComp = base.compressed;
                    synth.isSparse = base.sparse;
                    baseMultiExtents.push_back({ baseIdx, synth });
                }
            }
            for (const auto& d : ext.dataExtents)
                baseMultiExtents.push_back({ baseIdx, d });
        }
    };

    for (const auto& ref : attrListRefs)
    {
        uint32_t bIdx = lookupBase(ref.baseRecNo);
        uint32_t eIdx = lookupExt(ref.extRecNo);
        if (bIdx != kNone && eIdx != kNone)
            mergeExtIntoBase(bIdx, eIdx);
    }
    for (uint32_t eIdx = 0; eIdx < (uint32_t)extRecords.size(); ++eIdx)
    {
        if (extMerged[eIdx]) continue;
        uint32_t bIdx = lookupBase(extRecords[eIdx].baseRecNo);
        if (bIdx != kNone)
            mergeExtIntoBase(bIdx, eIdx);
    }

    // Group all collected multi-extents by baseIdx and finalize sizes.
    std::sort(baseMultiExtents.begin(), baseMultiExtents.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    {
        size_t pos = 0;
        std::vector<DataExtent> perFile;
        while (pos < baseMultiExtents.size())
        {
            uint32_t bIdx = baseMultiExtents[pos].first;
            perFile.clear();
            while (pos < baseMultiExtents.size() && baseMultiExtents[pos].first == bIdx)
            {
                perFile.push_back(baseMultiExtents[pos].second);
                ++pos;
            }
            FinalizeMftEntrySizes(entries[bIdx], perFile, cluster);
        }
        // Any base with hasAttrList that had zero $DATA extents anywhere: finalize with empty.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].hasAttrList && entries[i].real == 0 && entries[i].alloc == 0 && entries[i].wofAlloc > 0)
            {
                perFile.clear();
                FinalizeMftEntrySizes(entries[i], perFile, cluster);
            }
        }
    }

    // Determine reachability from Record 5 (NTFS volume root).
    // This cleanly keeps $RECYCLE.BIN and all real user files while excluding
    // $Extend internal metadata ($UsnJrnl, $RmMetadata, ...) and deleted-parent orphans.
    const uint32_t rootRecEntryIdx = lookupBase(5);
    std::vector<uint8_t> reach(entries.size(), 0); // 0=unknown, 1=visiting, 2=reachable, 3=unreachable
    if (rootRecEntryIdx != kNone) reach[rootRecEntryIdx] = 2;

    std::vector<uint32_t> chain;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (reach[i] != 0) continue;
        const MftEntry& e = entries[i];
        if (!e.hasName || e.name.empty() || IsNtfsMetadataName(e.name))
        {
            reach[i] = 3;
            continue;
        }
        chain.clear();
        uint32_t cur = (uint32_t)i;
        uint8_t result = 3;
        for (int guard = 0; guard < 512; ++guard)
        {
            if (cur == kNone) { result = 3; break; }
            if (reach[cur] == 2) { result = 2; break; }
            if (reach[cur] == 3 || reach[cur] == 1) { result = 3; break; }
            const MftEntry& ce = entries[cur];
            if (!ce.hasName || ce.name.empty() || IsNtfsMetadataName(ce.name))
            {
                result = 3;
                break;
            }
            reach[cur] = 1;
            chain.push_back(cur);
            uint64_t pRef = ce.parentRef & mask;
            if (pRef == 5) { result = 2; break; }
            uint32_t pIdx = lookupBase(pRef);
            if (pIdx == kNone || pIdx == cur) { result = 3; break; }
            cur = pIdx;
        }
        for (uint32_t idx : chain) reach[idx] = result;
    }

    VolumeInfo vi = QueryVolume(path);
    t.bytesPerCluster = vi.bytesPerCluster;
    t.rootPath = path;
    t.volumeFs = vi.fs;
    t.volumeTotal = vi.total;
    t.volumeFree = vi.free;
    t.nodes.reserve(entries.size() + 8);
    t.names.reserve(entries.size() * 12);

    // Create synthetic root representing MFT Record 5 (named as volume root, e.g. C:\,
    // so re-rooting into a subfolder produces accurate full paths without duplication).
    wchar_t volRootName[4] = { drv, L':', L'\\', 0 };
    t.root = t.AddNode(kNone, volRootName, Kind::Drive);
    std::vector<uint32_t> nodeOf(entries.size(), kNone);
    if (rootRecEntryIdx != kNone) nodeOf[rootRecEntryIdx] = t.root;

    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (i == rootRecEntryIdx || reach[i] != 2) continue;
        const MftEntry& e = entries[i];
        uint32_t idx = t.AddNode(kNone, e.name, e.isDir ? Kind::Folder : Kind::File);
        nodeOf[i] = idx;
        Node& n = t.nodes[idx];
        n.size = e.isDir ? 0 : e.real;
        n.disk = e.isDir ? 0 : e.alloc;
        n.attrs = e.attrs;
        n.created = e.created;
        n.modified = e.modified;
        n.accessed = e.accessed;
        if (prog)
        {
            if (e.isDir) InterlockedIncrement64(&prog->dirs);
            else
            {
                InterlockedIncrement64(&prog->files);
                InterlockedAdd64(&prog->bytes, e.real);
            }
        }
    }

    // Link parents
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (i == rootRecEntryIdx || nodeOf[i] == kNone) continue;
        uint64_t pRef = entries[i].parentRef & mask;
        uint32_t self = nodeOf[i];
        if (pRef == 5)
        {
            t.nodes[self].parent = t.root;
            continue;
        }
        uint32_t parentIdx = lookupBase(pRef);
        if (parentIdx == kNone || parentIdx == i || nodeOf[parentIdx] == kNone)
            t.nodes[self].parent = t.root;
        else
            t.nodes[self].parent = nodeOf[parentIdx];
    }

    // Rebuild child lists in O(n)
    {
        std::vector<uint32_t> last(t.nodes.size(), kNone);
        for (size_t i = 0; i < t.nodes.size(); ++i)
        {
            uint32_t p = t.nodes[i].parent;
            if (p == kNone || p == i || p >= t.nodes.size()) continue;
            if (t.nodes[p].firstChild == kNone) t.nodes[p].firstChild = (uint32_t)i;
            else t.nodes[last[p]].nextSibling = (uint32_t)i;
            last[p] = (uint32_t)i;
        }
    }

    // Navigate to the requested sub-path (if any); strip \\?\ prefix first for >MAX_PATH targets.
    uint32_t focus = t.root;
    std::wstring cleanPath = StripLongPrefix(path);
    if (cleanPath.size() > 3)
    {
        std::wstring rel = cleanPath.substr(3);
        size_t pos = 0;
        bool foundAll = true;
        while (pos < rel.size())
        {
            size_t next = rel.find_first_of(L"\\/", pos);
            std::wstring part = rel.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
            if (!part.empty())
            {
                uint32_t child = t.FindChild(focus, part);
                if (child == kNone) { foundAll = false; break; }
                focus = child;
            }
            if (next == std::wstring::npos) break;
            pos = next + 1;
        }
        if (!foundAll) return false;
    }

    if (focus != t.root)
    {
        // Re-root the tree at the requested folder and keep its full path as root display name.
        std::wstring focusPath = t.Path(focus);
        t.rootPath = focusPath;
        t.nodes[focus].parent = kNone;
        t.SetName(t.nodes[focus], focusPath);
        t.root = focus;
    }

    t.Aggregate();
    if (opt.includePseudo) AddPseudoNodes(t);
    t.Aggregate();
    return true;
}

} // namespace

bool ScanPath(const std::wstring& path, const ScanOptions& opt, Tree& out,
              ScanProgress* progress, bool* usedMft)
{
    if (usedMft) *usedMft = false;
    out.Clear();
    // an empty/blank path (e.g. "%V" expanded to nothing) would spin the worker pool forever
    if (path.empty() || path.find_first_not_of(L" \t\r\n") == std::wstring::npos)
        return false;

    bool wantMft = opt.mode != ScanOptions::Fast;
    if (wantMft && CanReadVolume(path))
    {
        EnableBackupPrivilege();
        if (ScanMftEx(path, opt, out, progress))
        {
            // Whole-volume safety net: verify non-virtual accounted bytes against volume used
            // bytes; if a large share is still missing, fall back to directory enumeration.
            bool incomplete = false;
            if (IsWholeVolumePath(path) && out.Valid(out.root))
            {
                VolumeInfo vi = QueryVolume(path);
                int64_t used = vi.total - vi.free;
                int64_t accounted = 0;
                for (uint32_t c = out.nodes[out.root].firstChild; c != Node::kNone && out.Valid(c);
                     c = out.nodes[c].nextSibling)
                {
                    if (!out.nodes[c].IsVirtual()) accounted += out.nodes[c].disk;
                }
                incomplete = used > 0 && accounted < used * 7 / 10;
            }
            if (!incomplete)
            {
                if (usedMft) *usedMft = true;
                return true;
            }
            out.Clear();
            if (progress)
            {
                InterlockedExchange64(&progress->files, 0);
                InterlockedExchange64(&progress->dirs, 0);
                InterlockedExchange64(&progress->bytes, 0);
                if (progress->message[0] == 0)
                    wcsncpy_s(progress->message, _countof(progress->message), L"MFT 结果不完整，改用目录枚举", _TRUNCATE);
            }
        }
    }
    if (opt.mode == ScanOptions::Mft) return false;
    return ScanFast(path, opt, out, progress);
}

} // namespace ss

// ============================================================ test hooks
#ifdef SS_TEST_HOOKS
namespace ss { namespace test {

bool MftParseRecordForTest(void* record, unsigned recSize, int cluster,
                           wchar_t* nameOut, int nameCap, int* isDir,
                           long long* realSize, long long* allocSize,
                           unsigned long long* parentRef, unsigned* attrs)
{
    if (!record || recSize < 0x38) return false;
    MftEntry e;
    if (!ParseMftRecord((uint8_t*)record, recSize, cluster, e)) return false;
    if (nameOut && nameCap > 0)
    {
        int n = (int)e.name.size();
        if (n > nameCap - 1) n = nameCap - 1;
        for (int i = 0; i < n; ++i) nameOut[i] = e.name[i];
        nameOut[n] = 0;
    }
    if (isDir) *isDir = e.isDir ? 1 : 0;
    if (realSize) *realSize = e.real;
    if (allocSize) *allocSize = e.alloc;
    if (parentRef) *parentRef = e.parentRef;
    if (attrs) *attrs = e.attrs;
    return true;
}

int MftParseRunsForTest(const void* buffer, unsigned len, long long* out, int maxTriples)
{
    auto runs = ParseRuns((const uint8_t*)buffer, len, 0);
    int n = 0;
    for (auto& r : runs)
    {
        if (n + 1 > maxTriples) break;
        out[n * 3 + 0] = r.vcn;
        out[n * 3 + 1] = r.lcn;
        out[n * 3 + 2] = r.clusters;
        n++;
    }
    return n;
}

long long MftAlignForTest(long long v, int cluster) { return AlignToCluster(v, cluster); }

}} // namespace ss::test
#endif
