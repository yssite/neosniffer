// NeoSniffer.Mcp — stdio JSON-RPC 2.0 server exposing disk-usage analysis to AI agents.
// Read-only: no tool deletes or modifies files.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "core.h"

using namespace ss;

// ------------------------------------------------------------------ tiny JSON
namespace json {

struct Value
{
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value& operator[](const char* k) const
    {
        static Value empty;
        auto it = obj.find(k);
        return it == obj.end() ? empty : it->second;
    }
    std::string AsString() const { return type == Str ? str : ""; }
    double AsNum(double d = 0) const { return type == Num ? num : d; }
    bool AsBool(bool d = false) const { return type == Bool ? b : d; }
};

static void SkipWs(const std::string& s, size_t& i) { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }

static bool ParseString(const std::string& s, size_t& i, std::string& out)
{
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size())
    {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\' && i < s.size())
        {
            char e = s[i++];
            switch (e)
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u':
                {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e;
            }
        }
        else out += c;
    }
    return false;
}

// P0-3: guard against stack overflow on deeply nested JSON and DoS on huge containers.
static constexpr int kMaxDepth = 64;
static constexpr size_t kMaxElements = 65536;

static bool ParseValue(const std::string& s, size_t& i, Value& v, int depth, size_t& elemCount);

static bool ParseArray(const std::string& s, size_t& i, Value& v, int depth, size_t& elemCount)
{
    if (depth > kMaxDepth) return false;
    v.type = Value::Arr;
    ++i;
    SkipWs(s, i);
    if (i < s.size() && s[i] == ']') { ++i; return true; }
    while (i < s.size())
    {
        if (++elemCount > kMaxElements) return false;
        Value item;
        if (!ParseValue(s, i, item, depth, elemCount)) return false;
        v.arr.push_back(std::move(item));
        SkipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        return false;
    }
    return false;
}

static bool ParseObject(const std::string& s, size_t& i, Value& v, int depth, size_t& elemCount)
{
    if (depth > kMaxDepth) return false;
    v.type = Value::Obj;
    ++i;
    SkipWs(s, i);
    if (i < s.size() && s[i] == '}') { ++i; return true; }
    while (i < s.size())
    {
        if (++elemCount > kMaxElements) return false;
        SkipWs(s, i);
        std::string key;
        if (!ParseString(s, i, key)) return false;
        SkipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        Value val;
        if (!ParseValue(s, i, val, depth, elemCount)) return false;
        v.obj[std::move(key)] = std::move(val);
        SkipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        return false;
    }
    return false;
}

static bool ParseValue(const std::string& s, size_t& i, Value& v, int depth, size_t& elemCount)
{
    if (depth > kMaxDepth) return false;
    SkipWs(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') { v.type = Value::Str; return ParseString(s, i, v.str); }
    if (c == '{') return ParseObject(s, i, v, depth + 1, elemCount);
    if (c == '[') return ParseArray(s, i, v, depth + 1, elemCount);
    if (c == 't') { if (s.compare(i, 4, "true") == 0) { v.type = Value::Bool; v.b = true; i += 4; return true; } }
    if (c == 'f') { if (s.compare(i, 5, "false") == 0) { v.type = Value::Bool; v.b = false; i += 5; return true; } }
    if (s.compare(i, 4, "null") == 0) { v.type = Value::Null; i += 4; return true; }
    char* end = nullptr;
    double d = strtod(s.c_str() + i, &end);
    if (end && end != s.c_str() + i) { v.type = Value::Num; v.num = d; i = (size_t)(end - s.c_str()); return true; }
    return false;
}

static bool ParseValue(const std::string& s, size_t& i, Value& v)
{
    size_t elemCount = 0;
    if (!ParseValue(s, i, v, 0, elemCount)) return false;
    SkipWs(s, i);
    return i == s.size();
}

static std::string Escape(const std::string& in)
{
    std::string o = "\"";
    for (unsigned char c : in)
    {
        switch (c)
        {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    o += "\"";
    return o;
}

static std::string W2U(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring U2W(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string Esc(const std::wstring& w) { return Escape(W2U(w)); }

} // namespace json

// ------------------------------------------------------------------ cache
struct CacheEntry
{
    Tree tree;
    std::wstring path;
    bool valid = false;
};

static std::map<std::wstring, CacheEntry> g_cache;
static AppConfig g_cfg;

static std::wstring NormalizePathForMatch(std::wstring p)
{
    for (auto& c : p) if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

static bool GetTree(const std::wstring& pathIn, Tree** out, std::wstring& error)
{
    std::wstring path = Trim(pathIn);
    if (path.empty()) { error = L"path is required"; return false; }
    std::wstring normKey = NormalizePathForMatch(path);
    for (auto& kv : g_cache)
    {
        if (_wcsicmp(kv.first.c_str(), normKey.c_str()) == 0 && kv.second.valid)
        {
            *out = &kv.second.tree;
            return true;
        }
    }
    auto& e = g_cache[normKey];
    ScanOptions opt;
    opt.mode = ScanOptions::Auto;
    ScanProgress prog;
    bool usedMft = false;
    if (!ScanPath(path, opt, e.tree, &prog, &usedMft))
    {
        g_cache.erase(normKey);
        error = L"scan failed: " + path;
        return false;
    }
    e.tree.ApplyGroups(g_cfg.classes);
    e.path = path;
    e.valid = true;
    *out = &e.tree;
    return true;
}

static uint32_t ResolveNode(Tree& t, const std::wstring& path, bool* isFile = nullptr)
{
    if (isFile) *isFile = false;
    std::wstring target = NormalizePathForMatch(Trim(path));
    if (target.empty()) return t.root;
    std::wstring rootPath = NormalizePathForMatch(t.Path(t.root));
    std::wstring rel = target;
    if (target.size() >= rootPath.size() && _wcsnicmp(target.c_str(), rootPath.c_str(), rootPath.size()) == 0)
        rel = target.substr(rootPath.size());
    uint32_t cur = t.root;
    size_t pos = 0;
    while (pos < rel.size())
    {
        size_t nx = rel.find_first_of(L"\\/", pos);
        std::wstring part = rel.substr(pos, nx == std::wstring::npos ? std::wstring::npos : nx - pos);
        if (!part.empty())
        {
            uint32_t child = t.FindChild(cur, part);
            if (child == Node::kNone) return Node::kNone;
            cur = child;
        }
        if (nx == std::wstring::npos) break;
        pos = nx + 1;
    }
    if (isFile) *isFile = t.nodes[cur].IsFile();
    return cur;
}

// ------------------------------------------------------------------ tool helpers
static std::string NodeJson(Tree& t, uint32_t n, bool withPath = true)
{
    const Node& nd = t.nodes[n];
    std::string o = "{";
    o += "\"name\":" + json::Esc(t.Name(n));
    const char* kind = nd.IsFile() ? "file" : (nd.kind == (uint8_t)Kind::Drive ? "drive" : "folder");
    o += ",\"type\":\"" + std::string(kind) + "\"";
    o += ",\"size\":" + std::to_string(nd.size);
    o += ",\"disk_size\":" + std::to_string(nd.disk);
    o += ",\"size_human\":" + json::Esc(FmtSize(nd.disk));
    o += ",\"attributes\":" + json::Esc(AttrFlags(nd.attrs));
    o += ",\"modified\":" + json::Esc(FmtDateTime(nd.modified));
    int kids = 0;
    for (uint32_t c = nd.firstChild; c != Node::kNone; c = t.nodes[c].nextSibling) kids++;
    o += ",\"children\":" + std::to_string(kids);
    if (withPath) o += ",\"path\":" + json::Esc(t.Path(n));
    if (nd.tag) o += ",\"tag\":" + std::to_string(nd.tag);
    o += "}";
    return o;
}

static std::string ToolResult(const std::string& text, bool isError = false)
{
    return std::string("{\"content\":[{\"type\":\"text\",\"text\":") + json::Escape(text) +
           "}]" + (isError ? ",\"isError\":true" : "") + "}";
}

static std::string JsonResult(const std::string& json) { return ToolResult(json); }

static std::vector<uint32_t> SortedChildren(Tree& t, uint32_t n, bool desc = true)
{
    std::vector<uint32_t> kids;
    for (uint32_t c = t.nodes[n].firstChild; c != Node::kNone; c = t.nodes[c].nextSibling) kids.push_back(c);
    std::sort(kids.begin(), kids.end(), [&](uint32_t a, uint32_t b)
    { return desc ? t.nodes[a].disk > t.nodes[b].disk : t.nodes[a].disk < t.nodes[b].disk; });
    return kids;
}

// P0-3: iterative traversal to avoid stack overflow on deep \\?\ directory trees.
static void CollectAll(Tree& t, uint32_t root, std::vector<uint32_t>& out)
{
    if (!t.Valid(root)) return;
    std::vector<uint32_t> stack;
    stack.push_back(root);
    while (!stack.empty())
    {
        uint32_t n = stack.back();
        stack.pop_back();
        out.push_back(n);
        for (uint32_t c = t.nodes[n].firstChild; c != Node::kNone && t.Valid(c); c = t.nodes[c].nextSibling)
            stack.push_back(c);
    }
}

// ------------------------------------------------------------------ tools
static std::string ToolScan(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    if (path.empty()) return ToolResult("path is required", true);
    std::wstring normKey = NormalizePathForMatch(Trim(path));
    for (auto it = g_cache.begin(); it != g_cache.end(); )
    {
        if (_wcsicmp(it->first.c_str(), normKey.c_str()) == 0) it = g_cache.erase(it);
        else ++it;
    }
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);

    int64_t files = 0, dirs = 0;
    std::vector<uint32_t> all;
    CollectAll(*t, t->root, all);
    for (uint32_t n : all) (t->nodes[n].IsFile() ? files : dirs)++;

    VolumeInfo vi = QueryVolume(path);
    std::string o = "{";
    o += "\"root\":" + json::Esc(t->Path(t->root));
    o += ",\"files\":" + std::to_string(files);
    o += ",\"folders\":" + std::to_string(dirs);
    o += ",\"total_size\":" + std::to_string(t->nodes[t->root].size);
    o += ",\"total_disk_size\":" + std::to_string(t->nodes[t->root].disk);
    o += ",\"total_human\":" + json::Esc(FmtSize(t->nodes[t->root].disk));
    o += ",\"volume\":{\"fs\":" + json::Esc(vi.fs) + ",\"cluster\":" + std::to_string(vi.bytesPerCluster) +
         ",\"total\":" + std::to_string(vi.total) + ",\"free\":" + std::to_string(vi.free) + "}";
    o += ",\"top_children\":[";
    auto kids = SortedChildren(*t, t->root);
    for (size_t i = 0; i < kids.size() && i < 10; ++i)
    {
        if (i) o += ",";
        o += NodeJson(*t, kids[i]);
    }
    o += "]}";
    return JsonResult(o);
}

static std::string ToolListChildren(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    int limit = args["limit"].AsNum(100);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);
    uint32_t n = ResolveNode(*t, path);
    if (n == Node::kNone) return ToolResult("path not found", true);

    std::string o = "{\"path\":" + json::Esc(t->Path(n)) + ",\"children\":[";
    auto kids = SortedChildren(*t, n);
    for (size_t i = 0; i < kids.size() && (int)i < limit; ++i)
    {
        if (i) o += ",";
        o += NodeJson(*t, kids[i]);
    }
    o += "]}";
    return JsonResult(o);
}

static std::string ToolLargest(const json::Value& args, bool files)
{
    std::wstring path = json::U2W(args["path"].AsString());
    int limit = (int)args["limit"].AsNum(50);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);

    std::vector<uint32_t> all;
    CollectAll(*t, t->root, all);
    std::vector<uint32_t> hits;
    for (uint32_t n : all)
    {
        if (n == t->root) continue;
        if (files && !t->nodes[n].IsFile()) continue;
        if (!files && !t->nodes[n].IsFolder()) continue;
        hits.push_back(n);
    }
    std::sort(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) { return t->nodes[a].disk > t->nodes[b].disk; });
    if ((int)hits.size() > limit) hits.resize((size_t)std::max(0, limit));

    std::string o = "{\"items\":[";
    for (size_t i = 0; i < hits.size(); ++i)
    {
        if (i) o += ",";
        o += NodeJson(*t, hits[i]);
    }
    o += "]}";
    return JsonResult(o);
}

static std::string ToolSearch(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    std::wstring expr = json::U2W(args["filter"].AsString());
    int limit = (int)args["limit"].AsNum(100);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);
    FilterExpr fx = FilterExpr::Parse(expr);
    if (fx.empty()) return ToolResult("filter is required", true);

    std::vector<uint32_t> all;
    CollectAll(*t, t->root, all);
    std::vector<uint32_t> hits;
    for (uint32_t n : all)
    {
        if (!t->nodes[n].IsFile()) continue;
        if (fx.Matches(*t, n, g_cfg.classes)) hits.push_back(n);
        if ((int)hits.size() > limit * 20) break;
    }
    std::sort(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) { return t->nodes[a].disk > t->nodes[b].disk; });
    if ((int)hits.size() > limit) hits.resize((size_t)std::max(0, limit));
    int64_t sum = 0;
    for (uint32_t n : hits) sum += t->nodes[n].disk;

    std::string o = "{\"filter\":" + json::Esc(expr) + ",\"count\":" + std::to_string(hits.size()) +
                    ",\"total_disk_size\":" + std::to_string(sum) + ",\"items\":[";
    for (size_t i = 0; i < hits.size(); ++i)
    {
        if (i) o += ",";
        o += NodeJson(*t, hits[i]);
    }
    o += "]}";
    return JsonResult(o);
}

static std::string ToolTree(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    int depth = std::clamp((int)args["depth"].AsNum(2), 0, 64);
    int maxNodes = std::clamp((int)args["max_nodes"].AsNum(500), 0, 10000);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);
    uint32_t start = ResolveNode(*t, path);
    if (start == Node::kNone) return ToolResult("path not found", true);

    std::string out;
    int count = 0;
    std::function<void(uint32_t, int)> walk = [&](uint32_t n, int level)
    {
        if (count++ > maxNodes) return;
        out.append((size_t)level * 2, ' ');
        out += "- " + json::W2U(t->Name(n)) + "  (" + json::W2U(FmtSize(t->nodes[n].disk)) + ")\n";
        if (level >= depth) return;
        for (uint32_t c : SortedChildren(*t, n)) walk(c, level + 1);
    };
    walk(start, 0);
    return ToolResult(out);
}

static std::string ToolFileTypes(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    int top = (int)args["top"].AsNum(20);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);

    struct St { int64_t count = 0, size = 0; };
    std::map<std::wstring, St> map;
    std::vector<uint32_t> all;
    CollectAll(*t, t->root, all);
    for (uint32_t n : all)
    {
        if (!t->nodes[n].IsFile()) continue;
        std::wstring name = t->Name(n);
        size_t dot = name.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"(none)" : Lowercase(name.substr(dot + 1));
        auto& st = map[ext];
        st.count++;
        st.size += t->nodes[n].disk;
    }
    std::vector<std::pair<std::wstring, St>> list(map.begin(), map.end());
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.second.size > b.second.size; });
    if ((int)list.size() > top) list.resize((size_t)std::max(0, top));

    std::string o = "{\"types\":[";
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (i) o += ",";
        o += "{\"extension\":" + json::Esc(list[i].first) + ",\"count\":" + std::to_string(list[i].second.count) +
             ",\"disk_size\":" + std::to_string(list[i].second.size) +
             ",\"size_human\":" + json::Esc(FmtSize(list[i].second.size)) + "}";
    }
    o += "]}";
    return JsonResult(o);
}

static std::string ToolDuplicates(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    int64_t minSize = (int64_t)args["min_size_mb"].AsNum(1) * 1024 * 1024;
    int limit = (int)args["limit"].AsNum(30);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);

    std::map<std::pair<std::wstring, int64_t>, std::vector<uint32_t>> groups;
    std::vector<uint32_t> all;
    CollectAll(*t, t->root, all);
    for (uint32_t n : all)
    {
        if (!t->nodes[n].IsFile() || t->nodes[n].size < minSize) continue;
        groups[{ Lowercase(t->Name(n)), t->nodes[n].size }].push_back(n);
    }
    struct Group { int64_t size; std::vector<uint32_t> nodes; };
    std::vector<Group> out;
    for (auto& kv : groups)
        if (kv.second.size() > 1) out.push_back({ kv.first.second, kv.second });
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.size * (a.nodes.size() - 1) > b.size * (b.nodes.size() - 1); });
    if ((int)out.size() > limit) out.resize((size_t)std::max(0, limit));

    int64_t wasted = 0;
    for (auto& g : out) wasted += g.size * (int64_t)(g.nodes.size() - 1);

    std::string o = "{\"wasted_bytes\":" + std::to_string(wasted) +
                    ",\"wasted_human\":" + json::Esc(FmtSize(wasted)) + ",\"groups\":[";
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (i) o += ",";
        o += "{\"size\":" + std::to_string(out[i].size) + ",\"paths\":[";
        for (size_t k = 0; k < out[i].nodes.size(); ++k)
        {
            if (k) o += ",";
            o += json::Esc(t->Path(out[i].nodes[k]));
        }
        o += "]}";
    }
    o += "]}";
    return JsonResult(o);
}

static std::string ToolCleanup(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    int limit = (int)args["limit"].AsNum(100);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);

    struct Item { std::wstring cat, reason, risk, path; int64_t size; };
    std::vector<Item> items;
    std::vector<uint32_t> all;
    CollectAll(*t, t->root, all);
    int64_t now = (int64_t)time(nullptr);

    for (uint32_t n : all)
    {
        if (!t->nodes[n].IsFile()) continue;
        std::wstring name = t->Name(n);
        std::wstring lower = Lowercase(t->Path(n));
        bool hit = false;
        Item it;
        it.path = t->Path(n);
        it.size = t->nodes[n].disk;

        if (WildcardMatch(name, L"*.tmp") || WildcardMatch(name, L"*.temp") || WildcardMatch(name, L"~$*") ||
            WildcardMatch(name, L"*.bak") || WildcardMatch(name, L"*.dmp") || WildcardMatch(name, L"*.crdownload") ||
            WildcardMatch(name, L"*.part"))
        {
            hit = true;
            it.cat = L"临时/备份文件";
            it.reason = L"扩展名符合临时文件模式";
            it.risk = L"low";
        }
        else if (lower.find(L"\\windows\\temp\\") != std::wstring::npos ||
                 lower.find(L"\\appdata\\local\\temp\\") != std::wstring::npos)
        {
            hit = true;
            it.cat = L"临时目录";
            it.reason = L"位于 Temp 目录内";
            it.risk = L"low";
        }
        else if (t->nodes[n].size > 100LL * 1024 * 1024)
        {
            int64_t age = now - FileTimeToSeconds(t->nodes[n].modified);
            bool pkg = lower.size() > 4 && (lower.substr(lower.size() - 4) == L".iso" ||
                                            lower.substr(lower.size() - 4) == L".msi" ||
                                            lower.substr(lower.size() - 4) == L".exe" ||
                                            lower.substr(lower.size() - 4) == L".zip");
            if (pkg && age > 365LL * 86400)
            {
                hit = true;
                it.cat = L"陈旧安装包/镜像";
                it.reason = L"超过 1 年未修改的大体积安装包";
                it.risk = L"medium";
            }
        }
        if (hit) items.push_back(it);
        if (items.size() > 20000) break;
    }

    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.size > b.size; });
    int64_t total = 0;
    for (auto& i : items) total += i.size;
    if ((int)items.size() > limit) items.resize((size_t)std::max(0, limit));

    std::string o = "{\"total_candidate_bytes\":" + std::to_string(total) +
                    ",\"total_human\":" + json::Esc(FmtSize(total)) + ",\"note\":\"" +
                    json::W2U(L"仅建议，不会删除任何文件") + "\",\"items\":[";
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i) o += ",";
        o += "{\"category\":" + json::Esc(items[i].cat) + ",\"reason\":" + json::Esc(items[i].reason) +
             ",\"risk\":\"" + json::W2U(items[i].risk) + "\",\"size\":" + std::to_string(items[i].size) +
             ",\"path\":" + json::Esc(items[i].path) + "}";
    }
    o += "]}";
    return JsonResult(o);
}

static std::string ToolVolumeInfo(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    if (path.empty()) path = L"C:\\";
    VolumeInfo vi = QueryVolume(path);
    bool canRead = CanReadVolume(path);
    std::string o = "{";
    o += "\"root\":" + json::Esc(vi.root);
    o += ",\"fs\":" + json::Esc(vi.fs);
    o += ",\"label\":" + json::Esc(vi.label);
    o += ",\"ntfs\":" + std::string(vi.isNtfs ? "true" : "false");
    o += ",\"cluster_bytes\":" + std::to_string(vi.bytesPerCluster);
    o += ",\"total\":" + std::to_string(vi.total);
    o += ",\"free\":" + std::to_string(vi.free);
    o += ",\"total_human\":" + json::Esc(FmtSize(vi.total));
    o += ",\"free_human\":" + json::Esc(FmtSize(vi.free));
    o += ",\"mft_readable\":" + std::string(canRead ? "true" : "false");
    o += ",\"scan_mode\":\"" + std::string(canRead && vi.isNtfs ? "mft" : "fast") + "\"";
    o += "}";
    return JsonResult(o);
}

static std::string ToolExport(const json::Value& args)
{
    std::wstring path = json::U2W(args["path"].AsString());
    std::wstring name = json::U2W(args["config"].AsString());
    std::wstring outFile = json::U2W(args["output"].AsString());
    if (outFile.empty()) return ToolResult("output is required", true);
    Tree* t = nullptr;
    std::wstring err;
    if (!GetTree(path, &t, err)) return ToolResult(json::W2U(err), true);
    const ExportConfig* cfg = nullptr;
    for (auto& e : g_cfg.exports) if (_wcsicmp(e.name.c_str(), name.c_str()) == 0) cfg = &e;
    if (!cfg) return ToolResult("unknown export config: " + json::W2U(name), true);
    ExportResult r = ExportRun(*t, t->root, *cfg, g_cfg.classes, outFile, nullptr);
    std::string o = "{\"ok\":" + std::string(r.ok ? "true" : "false") + ",\"output\":" + json::Esc(outFile) +
                    ",\"bytes\":" + std::to_string(r.bytes) + "}";
    return JsonResult(o);
}

// ------------------------------------------------------------------ protocol
static std::string Dispatch(const std::string& method, const json::Value& params)
{
    if (method == "initialize")
    {
        return "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
               "\"serverInfo\":{\"name\":\"NeoSniffer\",\"version\":\"1.0.0\"}}";
    }
    if (method == "tools/list")
    {
        const char* tools = R"({"tools":[
{"name":"scan","description":"扫描一个驱动器或目录，返回总体统计与前 10 大子项。结果会缓存供后续工具使用。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"要扫描的路径，例如 C:\\ 或 D:\\Projects"}},"required":["path"]}},
{"name":"list_children","description":"列出某个目录的直接子项（按占用降序）。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"limit":{"type":"number","default":100}},"required":["path"]}},
{"name":"largest_files","description":"列出最大的文件。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"limit":{"type":"number","default":50}},"required":["path"]}},
{"name":"largest_folders","description":"列出占用最大的目录。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"limit":{"type":"number","default":50}},"required":["path"]}},
{"name":"search","description":"用 NeoSniffer 过滤器语法（兼容 SpaceSniffer 1.3.0.2）搜索文件，例如 *.iso;>1gb;>1month。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"filter":{"type":"string"},"limit":{"type":"number","default":100}},"required":["path","filter"]}},
{"name":"tree","description":"以缩进文本打印目录树（限制深度与节点数）。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"depth":{"type":"number","default":2},"max_nodes":{"type":"number","default":500}},"required":["path"]}},
{"name":"file_types","description":"按扩展名聚合占用。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"top":{"type":"number","default":20}},"required":["path"]}},
{"name":"duplicates","description":"查找同名同大小的重复文件，估算可回收空间（只读，不删除）。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"min_size_mb":{"type":"number","default":1},"limit":{"type":"number","default":30}},"required":["path"]}},
{"name":"cleanup_candidates","description":"给出清理建议（临时文件、陈旧安装包等）与风险等级；不会删除任何文件。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"limit":{"type":"number","default":100}},"required":["path"]}},
{"name":"volume_info","description":"卷信息：文件系统、簇大小、容量、可用空间、是否可用 $MFT 直读。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"}}}},
{"name":"export_report","description":"用内置导出模板生成报告文件（Summary / File report / Simple file list / First level / CSV）。",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"config":{"type":"string"},"output":{"type":"string"}},"required":["path","output"]}}
]})";
        return tools;
    }
    if (method == "tools/call")
    {
        std::string name = params["name"].AsString();
        const json::Value& args = params["arguments"];
        if (name == "scan") return ToolScan(args);
        if (name == "list_children") return ToolListChildren(args);
        if (name == "largest_files") return ToolLargest(args, true);
        if (name == "largest_folders") return ToolLargest(args, false);
        if (name == "search") return ToolSearch(args);
        if (name == "tree") return ToolTree(args);
        if (name == "file_types") return ToolFileTypes(args);
        if (name == "duplicates") return ToolDuplicates(args);
        if (name == "cleanup_candidates") return ToolCleanup(args);
        if (name == "volume_info") return ToolVolumeInfo(args);
        if (name == "export_report") return ToolExport(args);
        return ToolResult("unknown tool: " + name, true);
    }
    return "";
}

static void WriteLine(const std::string& s)
{
    fwrite(s.data(), 1, s.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// P0-4: accumulate full JSON-RPC lines up to a safe cap instead of truncating at 64KB.
static constexpr size_t kMaxRequestBytes = 4 * 1024 * 1024;

static bool ReadLineBounded(FILE* in, std::string& out, bool& tooLarge)
{
    out.clear();
    tooLarge = false;
    char chunk[8192];
    bool gotAny = false;
    while (fgets(chunk, sizeof(chunk), in))
    {
        gotAny = true;
        size_t len = strlen(chunk);
        bool hasNl = (len > 0 && chunk[len - 1] == '\n');
        if (!tooLarge)
        {
            if (out.size() + len > kMaxRequestBytes)
            {
                tooLarge = true;
                out.clear();
            }
            else
            {
                out.append(chunk, len);
            }
        }
        if (hasNl) break;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return gotAny;
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    g_cfg = AppConfig::Default();
    // P0-5: use 4096-wchar buffer and check GetModuleFileNameW return value to avoid MAX_PATH truncation / UB.
    std::wstring exe(4096, L'\0');
    DWORD exeLen = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (exeLen > 0 && exeLen < exe.size())
    {
        exe.resize(exeLen);
        size_t sl = exe.find_last_of(L'\\');
        std::wstring dirPrefix = (sl != std::wstring::npos) ? (exe.substr(0, sl) + L"\\") : L"";
        if (!g_cfg.Load(dirPrefix + L"NeoSnifferConfig.xml"))
            g_cfg.Load(dirPrefix + L"SpaceSnifferConfig.xml");
    }

    std::string line;
    bool tooLarge = false;
    while (ReadLineBounded(stdin, line, tooLarge))
    {
        if (tooLarge)
        {
            // P0-4: explicitly report oversized line instead of silently dropping / hanging client.
            WriteLine("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Request too large\"}}");
            continue;
        }
        if (line.empty()) continue;

        json::Value req;
        size_t i = 0;
        if (!json::ParseValue(line, i, req))
        {
            // P0-3: return JSON-RPC Parse error (-32700) on malformed JSON or depth/element overflow.
            WriteLine("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
            continue;
        }
        if (req.type != json::Value::Obj)
        {
            WriteLine("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"}}");
            continue;
        }

        std::string id;
        const json::Value& idv = req["id"];
        if (idv.type == json::Value::Str) id = json::Escape(idv.str);
        else if (idv.type == json::Value::Num) id = std::to_string((long long)idv.num);

        std::string method = req["method"].AsString();
        if (method.empty())
        {
            if (!id.empty())
            {
                WriteLine("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                          ",\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"}}");
            }
            continue;
        }

        // notifications (no id) need no response
        if (id.empty()) continue;

        std::string result = Dispatch(method, req["params"]);
        if (result.empty())
        {
            WriteLine("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                      ",\"error\":{\"code\":-32601,\"message\":\"method not found\"}}");
            continue;
        }
        WriteLine("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
    }
    return 0;
}
