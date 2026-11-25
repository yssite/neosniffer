// Config XML, filter DSL, export template engine and snapshot formats.
#include "core.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ss {

static constexpr uint32_t kNone = Node::kNone;

// ============================================================ tiny XML helpers
static std::wstring ReadWholeFile(const std::wstring& path)
{
    // P0-5: support >MAX_PATH files via \\?\ prefix.
    std::wstring lp = ToLongPath(path);
    HANDLE h = CreateFileW(lp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 256LL * 1024 * 1024)
    {
        CloseHandle(h);
        return L"";
    }
    std::string buf((size_t)size.QuadPart, '\0');
    size_t total = 0;
    while (total < buf.size())
    {
        DWORD want = (DWORD)std::min<size_t>(buf.size() - total, 16u * 1024 * 1024);
        DWORD got = 0;
        if (!ReadFile(h, buf.data() + total, want, &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    buf.resize(total);
    if (buf.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), nullptr, 0);
    if (need <= 0) return L"";
    std::wstring w((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), w.data(), need);
    return w;
}

static bool WriteWholeFile(const std::wstring& path, const std::wstring& text)
{
    std::string buf;
    if (!text.empty())
    {
        int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        if (need <= 0) return false;
        buf.resize((size_t)need);
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), buf.data(), need, nullptr, nullptr);
    }
    // P0-5: support >MAX_PATH output paths via \\?\ prefix.
    std::wstring lp = ToLongPath(path);
    HANDLE h = CreateFileW(lp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t total = 0;
    bool ok = true;
    while (total < buf.size())
    {
        DWORD want = (DWORD)std::min<size_t>(buf.size() - total, 16u * 1024 * 1024);
        DWORD wrote = 0;
        if (!WriteFile(h, buf.data() + total, want, &wrote, nullptr) || wrote == 0) { ok = false; break; }
        total += wrote;
    }
    CloseHandle(h);
    return ok;
}

// Extracts <name>value</name> from an XML blob (first match).
static std::wstring XmlValue(const std::wstring& xml, const std::wstring& name)
{
    std::wstring open = L"<" + name;
    size_t p = xml.find(open);
    if (p == std::wstring::npos) return L"";
    size_t gt = xml.find(L'>', p);
    if (gt == std::wstring::npos) return L"";
    // self closing?
    if (gt > 0 && xml[gt - 1] == L'/') return L"";
    std::wstring close = L"</" + name + L">";
    size_t e = xml.find(close, gt + 1);
    if (e == std::wstring::npos) return L"";
    std::wstring v = xml.substr(gt + 1, e - gt - 1);
    // unescape
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == L'&')
        {
            if (v.compare(i, 5, L"&amp;") == 0) { out += L'&'; i += 4; continue; }
            if (v.compare(i, 4, L"&lt;") == 0) { out += L'<'; i += 3; continue; }
            if (v.compare(i, 4, L"&gt;") == 0) { out += L'>'; i += 3; continue; }
            if (v.compare(i, 6, L"&quot;") == 0) { out += L'"'; i += 5; continue; }
            if (v.compare(i, 6, L"&apos;") == 0) { out += L'\''; i += 5; continue; }
        }
        out += v[i];
    }
    return out;
}

static std::wstring XmlEscape(const std::wstring& s)
{
    std::wstring o;
    for (wchar_t c : s)
    {
        switch (c)
        {
            case L'&': o += L"&amp;"; break;
            case L'<': o += L"&lt;"; break;
            case L'>': o += L"&gt;"; break;
            case L'"': o += L"&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

static int ToInt(const std::wstring& s, int def)
{
    if (s.empty()) return def;
    return (int)wcstol(s.c_str(), nullptr, 10);
}

static bool ToBool(const std::wstring& s, bool def)
{
    if (s.empty()) return def;
    std::wstring v = Lowercase(Trim(s));
    return v == L"true" || v == L"1" || v == L"yes";
}

static COLORREF ParseColor(const std::wstring& s, COLORREF def)
{
    std::wstring t = Trim(s);
    if (t.empty()) return def;
    if (t[0] == L'#') t = t.substr(1);
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(t.c_str(), &end, 16);
    if (end == t.c_str()) return def;
    // #AARRGGBB -> RGB
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static std::wstring ColorText(COLORREF c)
{
    wchar_t buf[16];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"#FF%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

// ============================================================ file classes
FileClasses FileClasses::Default()
{
    FileClasses fc;
    auto add = [&](const wchar_t* name, COLORREF color, std::vector<std::wstring> exts)
    {
        FileClass c; c.name = name; c.color = color; c.extensions = std::move(exts);
        fc.items.push_back(std::move(c));
    };
    add(L"Video/Movies", RGB(0x3F, 0x7F, 0xBF),
        { L"qt", L"mov", L"mpeg", L"mpg", L"mpe", L"m1s", L"mp4", L"avi", L"mts", L"vob", L"wmv", L"rm", L"3gp", L"mkv" });
    add(L"Executables/System files", RGB(0xBF, 0x5F, 0x3F),
        { L"exe", L"dll", L"sys", L"ini", L"msi", L"log" });
    add(L"Archives/Disk images", RGB(0xBF, 0x9F, 0x3F),
        { L"zip", L"7zip", L"7z", L"rar", L"iso", L"gz", L"bz2", L"xz", L"tar" });
    add(L"Pictures/Images", RGB(0x3F, 0xAF, 0x6F),
        { L"jpg", L"jpeg", L"gif", L"png", L"tif", L"tiff", L"nef", L"raw", L"rw2", L"cr2", L"bmp", L"psd", L"webp", L"svg" });
    add(L"Audio/Music", RGB(0x9F, 0x5F, 0xBF),
        { L"mp2", L"mp3", L"wav", L"ogg", L"aif", L"alp", L"au", L"3gp", L"ac3", L"amr", L"aac", L"m4a",
          L"wma", L"aiff", L"flac", L"voc", L"opus" });
    return fc;
}

int FileClasses::IndexOf(const std::wstring& ext) const
{
    std::wstring low = Lowercase(ext);
    for (size_t i = 0; i < items.size(); ++i)
        for (auto& e : items[i].extensions)
            if (Lowercase(e) == low) return (int)i;
    return -1;
}

int FileClasses::IndexOfName(const std::wstring& name) const
{
    for (size_t i = 0; i < items.size(); ++i)
        if (_wcsicmp(items[i].name.c_str(), name.c_str()) == 0) return (int)i;
    return -1;
}

// ============================================================ default config
static std::vector<ExportConfig> DefaultExports()
{
    std::vector<ExportConfig> v;
    auto mk = [&](const wchar_t* name, const wchar_t* file, const wchar_t* ext,
                  const wchar_t* block, const wchar_t* fine, const wchar_t* script)
    {
        ExportConfig c;
        c.name = name; c.preferredFileName = file; c.preferredExtension = ext;
        c.blockSorting = block; c.fineSorting = fine; c.builtIn = true; c.script = script;
        v.push_back(std::move(c));
    };
    mk(L"Summary", L"NeoSniffer-summary", L".txt", L"folders", L"size",
       L"{script}Total size: <%disksize%>\r\nFolders   : <%foldercount%>\r\nFiles     : <%filecount%>\r\n"
       L"Path      : <%pathfile%>\r\n");
    mk(L"Simple file list", L"NeoSniffer-files", L".txt", L"files", L"size",
       L"{script}{ if {<%isfile%>} == {true} ? {<%disksize%>\t<%pathfile%>{&br}}}");
    mk(L"File report", L"NeoSniffer-report", L".txt", L"files", L"size",
       L"{script}{   if {<%isfile%>} == {true}\r\n"
       L"  ? {  [{leftpad{<%disksize%>}{ }{8}}] <%file%>{&br}}\r\n"
       L"  : {   if {<%containsfiles%>} == {true}\r\n"
       L"      ? {{&br}<%pathfile%> [<%disksize%>]{&br}}\r\n"
       L"    }\r\n}");
    mk(L"First level", L"NeoSniffer-level1", L".txt", L"name", L"size",
       L"{script}{ if {<%nestinglevel%>} == {1} ? {<%disksize%>\t<%name%>{&br}}}");
    mk(L"CSV", L"NeoSniffer-export", L".csv", L"files", L"size",
       L"{script}\"<%pathfile%>\",\"<%disksizebytes%>\",\"<%sizebytes%>\",\"<%nestinglevel%>\"{&br}");
    return v;
}

AppConfig AppConfig::Default()
{
    AppConfig c;
    c.classes = FileClasses::Default();
    c.exports = DefaultExports();
    return c;
}

bool AppConfig::Load(const std::wstring& file)
{
    path = file;
    std::wstring xml = ReadWholeFile(file);
    if (xml.empty()) return false;

    refreshDelayMs = ToInt(XmlValue(xml, L"refreshDelayMs"), refreshDelayMs);
    zoomTimeMs = ToInt(XmlValue(xml, L"zoomTimeMs"), zoomTimeMs);
    useRealZoom = ToBool(XmlValue(xml, L"useRealZoom"), useRealZoom);
    useDynaSmooth = ToBool(XmlValue(xml, L"useDynaSmooth"), useDynaSmooth);
    treemapDirect2D = ToBool(XmlValue(xml, L"treemapDirect2D"), treemapDirect2D);
    minElementSizePx = ToInt(XmlValue(xml, L"minElementSizePx"), minElementSizePx);
    initialDetailLevel = ToInt(XmlValue(xml, L"initialDetailLevel"), initialDetailLevel);
    fontSize = ToInt(XmlValue(xml, L"fontSize"), fontSize);
    proportions = ToInt(XmlValue(xml, L"proportions"), proportions);
    levelContrast = ToInt(XmlValue(xml, L"levelContrast"), levelContrast);
    borderContrast = ToInt(XmlValue(xml, L"borderContrast"), borderContrast);
    showFreeSpace = ToBool(XmlValue(xml, L"showFreeSpace"), showFreeSpace);
    showUnknownSpace = ToBool(XmlValue(xml, L"showUnknownSpace"), showUnknownSpace);
    useSorting = ToBool(XmlValue(xml, L"useSorting"), useSorting);
    scanAds = ToBool(XmlValue(xml, L"scanADS"), scanAds);
    darkMode = ToBool(XmlValue(xml, L"darkMode"), darkMode);
    followSystemTheme = ToBool(XmlValue(xml, L"followSystemTheme"), followSystemTheme);
    useMftScanner = ToBool(XmlValue(xml, L"useMftScanner"), useMftScanner);
    hideAdminHint = ToBool(XmlValue(xml, L"hideAdminHint"), hideAdminHint);
    alwaysRunAsAdmin = ToBool(XmlValue(xml, L"alwaysRunAsAdmin"), alwaysRunAsAdmin);
    shellIntegration = ToBool(XmlValue(xml, L"shellIntegration"), shellIntegration);
    {
        std::wstring lang = Lowercase(Trim(XmlValue(xml, L"language")));
        if (lang == L"auto" || lang == L"zh" || lang == L"en") language = lang;
    }
    showTooltipDate = ToBool(XmlValue(xml, L"showFileTooltipDateTime"), showTooltipDate);
    showTooltipAge = ToBool(XmlValue(xml, L"showFileTooltipAge"), showTooltipAge);
    mouseHover = ToBool(XmlValue(xml, L"mouseHover"), mouseHover);
    logPopup = ToBool(XmlValue(xml, L"logPopup"), logPopup);
    logIssue = ToBool(XmlValue(xml, L"logIssue"), logIssue);
    logWarning = ToBool(XmlValue(xml, L"logWarning"), logWarning);

    std::wstring style = XmlValue(xml, L"initialStyle");
    if (!style.empty()) initialStyle = style;

    driveColor = ParseColor(XmlValue(xml, L"driveColor"), driveColor);
    folderColor = ParseColor(XmlValue(xml, L"folderColor"), folderColor);
    fileColor = ParseColor(XmlValue(xml, L"fileColor"), fileColor);
    freeColor = ParseColor(XmlValue(xml, L"freeColor"), freeColor);
    unknownColor = ParseColor(XmlValue(xml, L"unknownColor"), unknownColor);

    // file classes
    size_t gp = xml.find(L"<fileGroups>");
    if (gp != std::wstring::npos)
    {
        FileClasses fc;
        size_t p = gp;
        for (;;)
        {
            size_t s = xml.find(L"<fileGroup>", p);
            if (s == std::wstring::npos) break;
            size_t e = xml.find(L"</fileGroup>", s);
            if (e == std::wstring::npos) break;
            std::wstring block = xml.substr(s, e - s);
            FileClass c;
            c.name = XmlValue(block, L"name");
            c.color = ParseColor(XmlValue(block, L"color"), RGB(0x80, 0x80, 0x80));
            std::wstring exts = XmlValue(block, L"extensions");
            size_t pos = 0;
            while (pos <= exts.size())
            {
                size_t nx = exts.find(L';', pos);
                std::wstring one = Trim(exts.substr(pos, nx == std::wstring::npos ? std::wstring::npos : nx - pos));
                if (!one.empty()) c.extensions.push_back(one);
                if (nx == std::wstring::npos) break;
                pos = nx + 1;
            }
            if (!c.name.empty()) fc.items.push_back(std::move(c));
            p = e + 12;
        }
        if (!fc.items.empty()) classes = std::move(fc);
    }
    return true;
}

bool AppConfig::Save() const
{
    if (path.empty()) return false;
    std::wstring x;
    x += L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<SpaceSnifferConfig>\r\n";
    auto num = [&](const wchar_t* n, int v) { x += L"  <" + std::wstring(n) + L">" + std::to_wstring(v) + L"</" + n + L">\r\n"; };
    auto flag = [&](const wchar_t* n, bool v)
    { x += L"  <" + std::wstring(n) + L">" + (v ? L"true" : L"false") + L"</" + n + L">\r\n"; };

    num(L"refreshDelayMs", refreshDelayMs);
    num(L"zoomTimeMs", zoomTimeMs);
    flag(L"useRealZoom", useRealZoom);
    flag(L"useDynaSmooth", useDynaSmooth);
    flag(L"treemapDirect2D", treemapDirect2D);
    num(L"minElementSizePx", minElementSizePx);
    num(L"initialDetailLevel", initialDetailLevel);
    num(L"fontSize", fontSize);
    num(L"proportions", proportions);
    flag(L"showFreeSpace", showFreeSpace);
    flag(L"showUnknownSpace", showUnknownSpace);
    flag(L"useSorting", useSorting);
    flag(L"scanADS", scanAds);
    flag(L"darkMode", darkMode);
    flag(L"followSystemTheme", followSystemTheme);
    flag(L"useMftScanner", useMftScanner);
    flag(L"hideAdminHint", hideAdminHint);
    flag(L"alwaysRunAsAdmin", alwaysRunAsAdmin);
    flag(L"shellIntegration", shellIntegration);
    x += L"  <language>" + XmlEscape(language) + L"</language>\r\n";
    flag(L"showFileTooltipDateTime", showTooltipDate);
    flag(L"showFileTooltipAge", showTooltipAge);
    flag(L"mouseHover", mouseHover);
    flag(L"logPopup", logPopup);
    flag(L"logIssue", logIssue);
    flag(L"logWarning", logWarning);
    x += L"  <initialStyle>" + XmlEscape(initialStyle) + L"</initialStyle>\r\n";
    x += L"  <colors>\r\n";
    auto col = [&](const wchar_t* n, COLORREF c) { x += L"    <" + std::wstring(n) + L">" + ColorText(c) + L"</" + n + L">\r\n"; };
    col(L"driveColor", driveColor);
    col(L"folderColor", folderColor);
    col(L"fileColor", fileColor);
    col(L"freeColor", freeColor);
    col(L"unknownColor", unknownColor);
    x += L"    <levelContrast>" + std::to_wstring(levelContrast) + L"</levelContrast>\r\n";
    x += L"    <borderContrast>" + std::to_wstring(borderContrast) + L"</borderContrast>\r\n";
    x += L"  </colors>\r\n";

    x += L"  <fileGroups>\r\n";
    for (auto& c : classes.items)
    {
        std::wstring exts;
        for (size_t i = 0; i < c.extensions.size(); ++i)
        {
            if (i) exts += L';';
            exts += c.extensions[i];
        }
        x += L"    <fileGroup><name>" + XmlEscape(c.name) + L"</name><extensions>" + XmlEscape(exts) +
             L"</extensions><color>" + ColorText(c.color) + L"</color></fileGroup>\r\n";
    }
    x += L"  </fileGroups>\r\n";
    x += L"</SpaceSnifferConfig>\r\n";
    return WriteWholeFile(path, x);
}

// ============================================================ filter
static bool ParseUnit(const std::wstring& u, double& factor)
{
    std::wstring s = Lowercase(u);
    if (s.empty() || s == L"b" || s == L"byte" || s == L"bytes") { factor = 1; return true; }
    if (s == L"k" || s == L"kb") { factor = 1024.0; return true; }
    if (s == L"m" || s == L"mb") { factor = 1024.0 * 1024; return true; }
    if (s == L"g" || s == L"gb") { factor = 1024.0 * 1024 * 1024; return true; }
    if (s == L"t" || s == L"tb") { factor = 1024.0 * 1024 * 1024 * 1024; return true; }
    return false;
}

static bool ParseAgeUnit(const std::wstring& u, double& seconds)
{
    std::wstring s = Lowercase(u);
    if (s == L"s" || s == L"sec" || s == L"secs" || s == L"second" || s == L"seconds") { seconds = 1; return true; }
    if (s == L"m" || s == L"min" || s == L"mins" || s == L"minute" || s == L"minutes") { seconds = 60; return true; }
    if (s == L"h" || s == L"hr" || s == L"hrs" || s == L"hour" || s == L"hours") { seconds = 3600; return true; }
    if (s == L"d" || s == L"day" || s == L"days") { seconds = 86400; return true; }
    if (s == L"w" || s == L"week" || s == L"weeks") { seconds = 604800; return true; }
    if (s == L"mo" || s == L"month" || s == L"months") { seconds = 2592000; return true; }
    if (s == L"y" || s == L"year" || s == L"years") { seconds = 31536000; return true; }
    return false;
}

static DWORD AttrBit(const std::wstring& in)
{
    std::wstring s = Lowercase(in);
    if (s == L"a" || s == L"archive") return FILE_ATTRIBUTE_ARCHIVE;
    if (s == L"s" || s == L"system") return FILE_ATTRIBUTE_SYSTEM;
    if (s == L"r" || s == L"readonly") return FILE_ATTRIBUTE_READONLY;
    if (s == L"h" || s == L"hidden") return FILE_ATTRIBUTE_HIDDEN;
    if (s == L"c" || s == L"compressed") return FILE_ATTRIBUTE_COMPRESSED;
    if (s == L"e" || s == L"encrypted") return FILE_ATTRIBUTE_ENCRYPTED;
    if (s == L"o" || s == L"offline") return FILE_ATTRIBUTE_OFFLINE;
    if (s == L"t" || s == L"temporary") return FILE_ATTRIBUTE_TEMPORARY;
    if (s == L"ni" || s == L"notindexed") return FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if (s == L"sp" || s == L"sparse") return FILE_ATTRIBUTE_SPARSE_FILE;
    if (s == L"ads") return 0x80000000u;
    return 0;
}

static void SplitList(const std::wstring& s, std::vector<std::wstring>& out, const wchar_t* seps)
{
    size_t pos = 0;
    while (pos <= s.size())
    {
        size_t nx = s.find_first_of(seps, pos);
        std::wstring one = Trim(s.substr(pos, nx == std::wstring::npos ? std::wstring::npos : nx - pos));
        if (!one.empty()) out.push_back(one);
        if (nx == std::wstring::npos) break;
        pos = nx + 1;
    }
}

FilterExpr FilterExpr::Parse(const std::wstring& text)
{
    FilterExpr fx;
    std::vector<std::wstring> raw;
    SplitList(text, raw, L";");
    for (auto& termText : raw)
    {
        std::wstring t = Trim(termText);
        if (t.empty()) continue;
        FilterTerm term;
        term.raw = t;
        if (t[0] == L'|') { term.negate = true; t = Trim(t.substr(1)); if (t.empty()) continue; }

        if (Lowercase(t).rfind(L":tag:", 0) == 0)
        {
            term.target = FilterTerm::Tag;
            std::wstring v = Lowercase(Trim(t.substr(5)));
            if (v == L"all") term.tagAny = true;
            else
            {
                std::vector<std::wstring> parts;
                SplitList(v, parts, L"+,- ");
                for (auto& pt : parts)
                {
                    if (pt == L"r" || pt == L"red") term.tagMask |= 1;
                    else if (pt == L"y" || pt == L"yellow") term.tagMask |= 2;
                    else if (pt == L"g" || pt == L"green") term.tagMask |= 4;
                    else if (pt == L"b" || pt == L"blue") term.tagMask |= 8;
                }
            }
        }
        else if (Lowercase(t).rfind(L":attr:", 0) == 0)
        {
            term.target = FilterTerm::Attr;
            size_t pos = 6;
            while ((pos = t.find_first_not_of(L"+, \t", pos)) != std::wstring::npos)
            {
                bool neg = t[pos] == L'-';
                if (neg) ++pos;
                size_t end = t.find_first_of(L"+,- \t", pos);
                DWORD bit = AttrBit(t.substr(pos, end == std::wstring::npos ? end : end - pos));
                if (neg) term.attrNotMask |= bit; else term.attrMask |= bit;
                pos = end;
            }
        }
        else if (Lowercase(t).rfind(L":class:", 0) == 0)
        {
            term.target = FilterTerm::FileClass;
            term.className = Trim(t.substr(7));
            if (term.className.size() >= 2 && term.className.front() == L'"' && term.className.back() == L'"')
                term.className = term.className.substr(1, term.className.size() - 2);
        }
        else if (t[0] == L':')
        {
            term.target = FilterTerm::Tag;
            std::wstring v = Lowercase(Trim(t.substr(1)));
            if (v == L"all") term.tagAny = true;
            else
            {
                std::vector<std::wstring> parts;
                SplitList(v, parts, L"+,- ");
                for (auto& pt : parts)
                {
                    if (pt == L"r" || pt == L"red") term.tagMask |= 1;
                    else if (pt == L"y" || pt == L"yellow") term.tagMask |= 2;
                    else if (pt == L"g" || pt == L"green") term.tagMask |= 4;
                    else if (pt == L"b" || pt == L"blue") term.tagMask |= 8;
                }
            }
        }
        else if (t[0] == L'\\')
        {
            term.target = FilterTerm::FolderName;
            term.mask = t.substr(1);
        }
        else
        {
            // size / age / name
            size_t i = 0;
            std::wstring src;
            while (i < t.size() && iswalpha(t[i])) { src += t[i]; ++i; }
            while (i < t.size() && iswspace(t[i])) ++i;
            if (i < t.size() && (t[i] == L'<' || t[i] == L'>'))
            {
                const wchar_t* start = t.c_str() + i + 1;
                wchar_t* end = nullptr;
                double value = std::wcstod(start, &end);
                std::wstring unit = Trim(end);
                double factor = 0;
                bool size = ParseUnit(unit, factor);
                bool age = !size && ParseAgeUnit(unit, factor);
                double threshold = value * factor;
                if (end != start && (size || age) && threshold >= 0 && threshold < 0x1p63) {
                    std::wstring source = Lowercase(src);
                    term.target = size ? FilterTerm::Size : FilterTerm::Age;
                    term.greater = t[i] == L'>';
                    term.threshold = static_cast<int64_t>(threshold);
                    term.sizeLogical = source == L"s" || source == L"size" || source == L"filesize" ||
                                       source == L"logicalsize" || source == L"logical";
                    term.ageSource = source.empty() || source[0] == L'm' ? 0 : (source[0] == L'c' ? 1 : 2);
                    fx.terms.push_back(std::move(term));
                    continue;
                }
            }
            term.target = FilterTerm::FileName;
            term.mask = t;
        }
        fx.terms.push_back(std::move(term));
    }
    return fx;
}

bool FilterExpr::Matches(const Tree& t, uint32_t node, const FileClasses& fc) const
{
    if (terms.empty()) return true;
    if (!t.Valid(node)) return false;
    const Node& n = t.nodes[node];
    std::wstring name = t.Name(node);
    std::wstring lowerName = Lowercase(name);
    int64_t now = (int64_t)time(nullptr);

    for (auto& term : terms)
    {
        bool hit = false;
        switch (term.target)
        {
            case FilterTerm::FileName:
                hit = n.IsFile() && WildcardMatch(name, term.mask);
                break;
            case FilterTerm::FolderName:
            {
                uint32_t p = n.parent;
                for (int guard = 0; guard < 512 && t.Valid(p); ++guard)
                {
                    if (WildcardMatch(t.Name(p), term.mask)) { hit = true; break; }
                    p = t.nodes[p].parent;
                }
                break;
            }
            case FilterTerm::Size:
            {
                int64_t v = term.sizeLogical ? n.size : n.disk;
                hit = term.greater ? v > term.threshold : v < term.threshold;
                break;
            }
            case FilterTerm::Age:
            {
                const FILETIME& ft = term.ageSource == 1 ? n.created : term.ageSource == 2 ? n.accessed : n.modified;
                int64_t ts = FileTimeToSeconds(ft);
                if (ts == 0) { hit = false; break; }
                int64_t age = now - ts;
                hit = term.greater ? age > term.threshold : age < term.threshold;
                break;
            }
            case FilterTerm::Tag: {
                uint32_t current = node;
                for (int depth = 0; depth < 512 && t.Valid(current); ++depth) {
                    uint8_t tag = t.nodes[current].tag;
                    if (tag >= 1 && tag <= 4) {
                        hit = term.tagAny || (term.tagMask & (1 << (tag - 1))) != 0;
                        break;
                    }
                    current = t.nodes[current].parent;
                }
                break;
            }
            case FilterTerm::Attr:
            {
                DWORD a = n.attrs;
                if (term.attrMask && (a & term.attrMask) != term.attrMask) hit = false;
                else if (term.attrNotMask && (a & term.attrNotMask) != 0) hit = false;
                else hit = true;
                break;
            }
            case FilterTerm::FileClass:
            {
                size_t dot = lowerName.find_last_of(L'.');
                if (!n.IsFile() || dot == std::wstring::npos) { hit = false; break; }
                int ci = fc.IndexOf(lowerName.substr(dot + 1));
                hit = ci >= 0 && _wcsicmp(fc.items[ci].name.c_str(), term.className.c_str()) == 0;
                break;
            }
        }
        if (term.negate) hit = !hit;
        if (!hit) return false;
    }
    return true;
}

static uint32_t ProjectNode(const Tree& src, uint32_t node, const FilterExpr& fx, const FileClasses& fc,
                            Tree& out, uint32_t parentOut, int depth)
{
    if (!src.Valid(node) || src.nodes[node].IsVirtual() || depth > 512) return kNone;
    const Node& n = src.nodes[node];
    if (n.IsFile())
    {
        if (!fx.Matches(src, node, fc)) return kNone;
        uint32_t idx = out.AddNode(parentOut, src.Name(node), Kind::File);
        Node& d = out.nodes[idx];
        d.size = n.size; d.disk = n.disk; d.attrs = n.attrs; d.tag = n.tag;
        d.created = n.created; d.modified = n.modified; d.accessed = n.accessed;
        return idx;
    }

    size_t namesBefore = out.names.size();
    uint32_t idx = out.AddNode(parentOut, src.Name(node), (Kind)n.kind);
    out.nodes[idx].attrs = n.attrs; out.nodes[idx].tag = n.tag; out.nodes[idx].created = n.created;
    out.nodes[idx].modified = n.modified; out.nodes[idx].accessed = n.accessed;
    bool any = false;
    for (uint32_t c = n.firstChild; c != kNone; c = src.nodes[c].nextSibling)
    {
        uint32_t sub = ProjectNode(src, c, fx, fc, out, idx, depth + 1);
        if (sub != kNone) any = true;
    }
    if (!any && parentOut != kNone)
    {
        out.nodes[parentOut].firstChild = out.nodes[idx].nextSibling;
        out.nodes.pop_back();
        out.names.resize(namesBefore);
        return kNone;
    }
    return idx;
}

bool ProjectFilter(const Tree& src, uint32_t node, const FilterExpr& fx, const FileClasses& fc, Tree& out)
{
    out.Clear();
    if (!src.Valid(node)) return false;
    out.bytesPerCluster = src.bytesPerCluster;
    uint32_t idx = ProjectNode(src, node, fx, fc, out, kNone, 0);
    if (idx == kNone) return false;
    out.root = idx;
    out.rootPath = src.Path(node);
    if (node != src.root) out.SetName(out.nodes[idx], out.rootPath);
    out.nodes[idx].parent = kNone;
    out.Aggregate();
    return true;
}

// ============================================================ export engine
namespace {

enum class TokKind { Literal, If, Pad };

struct Tok
{
    TokKind kind = TokKind::Literal;
    std::wstring text;
    // If
    std::vector<Tok> left, right, thenBranch, elseBranch;
    std::wstring op;
    // Pad
    std::vector<Tok> value;
    std::wstring padChar;
    int width = 0;
    bool padRight = false;
};

struct RenderCtx
{
    const Tree* t = nullptr;
    uint32_t node = kNone;
    uint32_t root = kNone;
    int nesting = 0;
    const FileClasses* fc = nullptr;
};

std::wstring ExpandTag(const std::wstring& tag, const RenderCtx& c);

// P0-3: iterative counting to prevent stack overflow on deep \\?\ trees.
int64_t CountFiles(const Tree& t, uint32_t root)
{
    if (!t.Valid(root)) return 0;
    int64_t count = 0;
    std::vector<uint32_t> stack;
    stack.push_back(root);
    while (!stack.empty())
    {
        uint32_t cur = stack.back();
        stack.pop_back();
        for (uint32_t k = t.nodes[cur].firstChild; k != kNone && t.Valid(k); k = t.nodes[k].nextSibling)
        {
            if (t.nodes[k].IsFile()) count++;
            else stack.push_back(k);
        }
    }
    return count;
}

int64_t CountFolders(const Tree& t, uint32_t root)
{
    if (!t.Valid(root)) return 0;
    int64_t count = 0;
    std::vector<uint32_t> stack;
    stack.push_back(root);
    while (!stack.empty())
    {
        uint32_t cur = stack.back();
        stack.pop_back();
        for (uint32_t k = t.nodes[cur].firstChild; k != kNone && t.Valid(k); k = t.nodes[k].nextSibling)
        {
            if (t.nodes[k].IsFolder())
            {
                count++;
                stack.push_back(k);
            }
        }
    }
    return count;
}

std::wstring ExpandTag(const std::wstring& tagIn, const RenderCtx& c)
{
    if (!c.t || !c.t->Valid(c.node)) return L"";
    std::wstring tag = Lowercase(Trim(tagIn));
    const Node& n = c.t->nodes[c.node];
    std::wstring name = c.t->Name(c.node);
    if (tag == L"filename" || tag == L"file" || tag == L"name") return name;
    if (tag == L"fileext")
    {
        size_t dot = name.find_last_of(L'.');
        return (dot == std::wstring::npos || dot == 0) ? L"" : Lowercase(name.substr(dot + 1));
    }
    if (tag == L"pathfile" || tag == L"path" || tag == L"fullpath") return c.t->Path(c.node);
    if (tag == L"filecreatedate") return FmtDate(n.created);
    if (tag == L"filecreatetime")
    {
        if (n.created.dwLowDateTime == 0 && n.created.dwHighDateTime == 0) return L"-";
        FILETIME l{}; SYSTEMTIME st{};
        if (!FileTimeToLocalFileTime(&n.created, &l) || !FileTimeToSystemTime(&l, &st)) return L"-";
        wchar_t b[16] = {}; _snwprintf_s(b, _countof(b), _TRUNCATE, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond); return b;
    }
    if (tag == L"filecreateage") return FmtAge(n.created);
    if (tag == L"filemodifydate") return FmtDate(n.modified);
    if (tag == L"filemodifytime")
    {
        if (n.modified.dwLowDateTime == 0 && n.modified.dwHighDateTime == 0) return L"-";
        FILETIME l{}; SYSTEMTIME st{};
        if (!FileTimeToLocalFileTime(&n.modified, &l) || !FileTimeToSystemTime(&l, &st)) return L"-";
        wchar_t b[16] = {}; _snwprintf_s(b, _countof(b), _TRUNCATE, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond); return b;
    }
    if (tag == L"filemodifyage") return FmtAge(n.modified);
    if (tag == L"fileaccessdate") return FmtDate(n.accessed);
    if (tag == L"fileaccessage") return FmtAge(n.accessed);
    if (tag == L"sizebytes") return std::to_wstring(n.size);
    if (tag == L"disksizebytes") return std::to_wstring(n.disk);
    if (tag == L"disksize" || tag == L"size") return FmtSize(n.disk);
    if (tag == L"isfile") return n.IsFile() ? L"true" : L"false";
    if (tag == L"isfolder") return n.IsFolder() ? L"true" : L"false";
    if (tag == L"iscontainer") return n.IsFolder() ? L"true" : L"false";
    if (tag == L"nestinglevel") return std::to_wstring(c.nesting);
    if (tag == L"count" || tag == L"localcount")
    {
        int64_t k = 0;
        for (uint32_t x = n.firstChild; x != kNone; x = c.t->nodes[x].nextSibling) k++;
        return std::to_wstring(k);
    }
    if (tag == L"filecount") return std::to_wstring(CountFiles(*c.t, c.node));
    if (tag == L"foldercount") return std::to_wstring(CountFolders(*c.t, c.node));
    if (tag == L"containsfiles") return CountFiles(*c.t, c.node) > 0 ? L"true" : L"false";
    if (tag == L"containsfolders") return CountFolders(*c.t, c.node) > 0 ? L"true" : L"false";
    if (tag == L"percent") return c.t->Valid(c.root) ? FmtPercent(n.disk, c.t->nodes[c.root].disk) : L"0%";
    if (tag == L"tag") return std::to_wstring(n.tag);
    return L"";
}

// P0-3: cap recursion depth for nested export template expressions.
static constexpr int kMaxTemplateDepth = 64;

void RenderToks(const std::vector<Tok>& toks, const RenderCtx& c, std::wstring& out, int depth = 0);

std::wstring RenderOne(const Tok& tk, const RenderCtx& c, int depth = 0)
{
    if (depth > kMaxTemplateDepth) return L"";
    std::wstring s;
    switch (tk.kind)
    {
        case TokKind::Literal:
        {
            const std::wstring& raw = tk.text;
            for (size_t i = 0; i < raw.size();)
            {
                if (raw.compare(i, 8, L"&curlyop") == 0) { s += L'{'; i += 8; continue; }
                if (raw.compare(i, 8, L"&curlycl") == 0) { s += L'}'; i += 8; continue; }
                if (raw.compare(i, 8, L"&angleop") == 0) { s += L'<'; i += 8; continue; }
                if (raw.compare(i, 8, L"&anglecl") == 0) { s += L'>'; i += 8; continue; }
                if (raw.compare(i, 3, L"&br") == 0) { s += L"\r\n"; i += 3; continue; }
                if (raw[i] == L'<' && i + 1 < raw.size() && raw[i + 1] == L'%')
                {
                    size_t e = raw.find(L"%>", i + 2);
                    if (e != std::wstring::npos)
                    {
                        s += ExpandTag(raw.substr(i + 2, e - i - 2), c);
                        i = e + 2;
                        continue;
                    }
                }
                s += raw[i++];
            }
            break;
        }
        case TokKind::If:
        {
            std::wstring l, r;
            RenderToks(tk.left, c, l, depth + 1);
            RenderToks(tk.right, c, r, depth + 1);
            bool eq = _wcsicmp(Trim(l).c_str(), Trim(r).c_str()) == 0;
            bool cond = (tk.op == L"!=") ? !eq : eq;
            RenderToks(cond ? tk.thenBranch : tk.elseBranch, c, s, depth + 1);
            break;
        }
        case TokKind::Pad:
        {
            std::wstring v;
            RenderToks(tk.value, c, v, depth + 1);
            int width = std::min(tk.width, 1024);
            if ((int)v.size() >= width) { s = v; break; }
            std::wstring pad((size_t)(width - (int)v.size()), tk.padChar.empty() ? L' ' : tk.padChar[0]);
            s = tk.padRight ? v + pad : pad + v;
            break;
        }
    }
    return s;
}

void RenderToks(const std::vector<Tok>& toks, const RenderCtx& c, std::wstring& out, int depth)
{
    if (depth > kMaxTemplateDepth) return;
    for (auto& t : toks) out += RenderOne(t, c, depth);
}

class Parser
{
public:
    explicit Parser(const std::wstring& s, int depth = 0) : s_(s), depth_(depth) {}

    std::vector<Tok> ParseSeq(wchar_t end)
    {
        std::vector<Tok> seq;
        if (depth_ > kMaxTemplateDepth)
        {
            SkipBalancedToClose(end);
            return seq;
        }
        std::wstring lit;
        auto flush = [&]()
        {
            if (!lit.empty()) { Tok t; t.kind = TokKind::Literal; t.text = lit; seq.push_back(std::move(t)); lit.clear(); }
        };
        while (i_ < s_.size())
        {
            wchar_t c = s_[i_];
            if (end && c == end) { ++i_; break; }
            if (c == L'}') { ++i_; break; }
            if (c == L'{') { flush(); ++i_; seq.push_back(ParseGroup()); continue; }
            lit += c; ++i_;
        }
        flush();
        return seq;
    }

private:
    void SkipWs() { while (i_ < s_.size() && iswspace(s_[i_])) ++i_; }
    void SkipBalancedToClose(wchar_t end)
    {
        int nested = 0;
        while (i_ < s_.size())
        {
            wchar_t c = s_[i_++];
            if (c == L'{') ++nested;
            else if (c == L'}') { if (nested == 0) break; --nested; }
            else if (end && c == end && nested == 0) break;
        }
    }
    bool Starts(const wchar_t* tok)
    {
        size_t n = wcslen(tok);
        return i_ + n <= s_.size() && _wcsnicmp(s_.c_str() + i_, tok, n) == 0;
    }

    Tok ParseGroup()
    {
        if (depth_ >= kMaxTemplateDepth)
        {
            SkipBalancedToClose(L'\0');
            return Tok{};
        }
        ++depth_;
        size_t save = i_;
        SkipWs();
        Tok res;
        if (Starts(L"if")) { i_ += 2; res = ParseIf(); --depth_; return res; }
        if (Starts(L"leftpad") || Starts(L"rightpad"))
        {
            bool right = Starts(L"rightpad");
            i_ += right ? 8 : 7;
            res = ParsePad(right);
            --depth_;
            return res;
        }
        i_ = save;
        res.kind = TokKind::Literal;
        auto seq = ParseSeq(L'\0');
        for (auto& x : seq) res.text += x.text;
        --depth_;
        return res;
    }

    Tok ParseIf()
    {
        Tok tk;
        tk.kind = TokKind::If;
        size_t condStart = i_;
        int depth = 0;
        size_t q = std::wstring::npos;
        for (size_t k = i_; k < s_.size(); ++k)
        {
            wchar_t c = s_[k];
            if (c == L'{') depth++;
            else if (c == L'}') { if (depth == 0) break; depth--; }
            else if (c == L'?' && depth == 0) { q = k; break; }
        }
        if (q == std::wstring::npos) return tk;
        std::wstring cond = s_.substr(condStart, q - condStart);
        size_t opPos = cond.find(L"==");
        tk.op = L"==";
        if (opPos == std::wstring::npos) { opPos = cond.find(L"!="); tk.op = L"!="; }
        if (opPos != std::wstring::npos)
        {
            Parser pl(cond.substr(0, opPos), depth_ + 1), pr(cond.substr(opPos + 2), depth_ + 1);
            tk.left = pl.ParseSeq(L'\0');
            tk.right = pr.ParseSeq(L'\0');
        }
        i_ = q + 1;
        SkipWs();
        if (i_ < s_.size() && s_[i_] == L'{') { ++i_; tk.thenBranch = ParseSeq(L'\0'); }
        SkipWs();
        if (i_ < s_.size() && s_[i_] == L':')
        {
            ++i_; SkipWs();
            if (i_ < s_.size() && s_[i_] == L'{') { ++i_; tk.elseBranch = ParseSeq(L'\0'); }
        }
        SkipWs();
        if (i_ < s_.size() && s_[i_] == L'}') ++i_;
        return tk;
    }

    Tok ParsePad(bool right)
    {
        Tok tk;
        tk.kind = TokKind::Pad;
        tk.padRight = right;
        SkipWs();
        if (i_ < s_.size() && s_[i_] == L'{') { ++i_; tk.value = ParseSeq(L'\0'); }
        SkipWs();
        if (i_ < s_.size() && s_[i_] == L'{')
        {
            ++i_;
            size_t start = i_;
            int depth = 0;
            while (i_ < s_.size())
            {
                wchar_t c = s_[i_];
                if (c == L'{') depth++;
                else if (c == L'}') { if (depth == 0) break; depth--; }
                ++i_;
            }
            tk.padChar = s_.substr(start, i_ - start);
            if (i_ < s_.size() && s_[i_] == L'}') ++i_;
        }
        SkipWs();
        if (i_ < s_.size() && s_[i_] == L'{')
        {
            ++i_;
            size_t start = i_;
            while (i_ < s_.size() && s_[i_] != L'}') ++i_;
            tk.width = ToInt(s_.substr(start, i_ - start), 0);
            if (i_ < s_.size() && s_[i_] == L'}') ++i_;
        }
        SkipWs();
        if (i_ < s_.size() && s_[i_] == L'}') ++i_;
        return tk;
    }

    const std::wstring& s_;
    size_t i_ = 0;
    int depth_ = 0;
};

std::vector<Tok> ParseTemplate(const std::wstring& srcIn)
{
    std::wstring src = srcIn;
    size_t marker = Lowercase(src).find(L"{script}");
    if (marker != std::wstring::npos) src = src.substr(marker + 8);
    Parser p(src);
    return p.ParseSeq(L'\0');
}

} // namespace

std::wstring ExportPreview(const Tree& t, uint32_t root, const ExportConfig& cfg, const FileClasses& fc,
                           size_t maxNodes, size_t maxChars, bool& truncated)
{
    truncated = false;
    if (!t.Valid(root)) return L"";
    auto toks = ParseTemplate(cfg.script);
    std::wstring out;
    size_t count = 0;

    std::function<void(uint32_t, int)> walk = [&](uint32_t n, int level)
    {
        if (truncated || !t.Valid(n)) return;
        if (count >= maxNodes || level > 512) { truncated = true; return; }
        RenderCtx c{ &t, n, root, level, &fc };
        for (auto& tk : toks) {
            std::wstring text = RenderOne(tk, c);
            if (text.size() > maxChars - out.size()) { truncated = true; return; }
            out += text;
        }
        count++;
        std::vector<uint32_t> kids;
        for (uint32_t k = t.nodes[n].firstChild; k != kNone; k = t.nodes[k].nextSibling) kids.push_back(k);
        std::sort(kids.begin(), kids.end(), [&](uint32_t a, uint32_t b)
        { return t.nodes[a].disk > t.nodes[b].disk; });
        for (uint32_t k : kids) {
            if (truncated) break;
            walk(k, level + 1);
        }
    };
    walk(root, 0);
    return out;
}

ExportResult ExportRun(const Tree& t, uint32_t root, const ExportConfig& cfg, const FileClasses& fc,
                       const std::wstring& outPath, std::function<void(int64_t, int64_t)> progress)
{
    ExportResult r;
    if (!t.Valid(root))
    {
        r.message = L"无效的视图根节点";
        return r;
    }
    auto toks = ParseTemplate(cfg.script);
    std::wstring body;
    int64_t total = (int64_t)t.nodes.size(), done = 0;

    std::function<void(uint32_t, int)> walk = [&](uint32_t n, int level)
    {
        if (level > 512 || !t.Valid(n)) return;
        RenderCtx c{ &t, n, root, level, &fc };
        for (auto& tk : toks) body += RenderOne(tk, c);
        done++;
        if (progress && (done & 0x3FF) == 0) progress(done, total);
        std::vector<uint32_t> kids;
        for (uint32_t k = t.nodes[n].firstChild; k != kNone; k = t.nodes[k].nextSibling) kids.push_back(k);
        if (cfg.fineSorting == L"size")
            std::sort(kids.begin(), kids.end(), [&](uint32_t a, uint32_t b)
            { return cfg.fineDescending ? t.nodes[a].disk > t.nodes[b].disk : t.nodes[a].disk < t.nodes[b].disk; });
        else if (cfg.fineSorting == L"name")
            std::sort(kids.begin(), kids.end(), [&](uint32_t a, uint32_t b)
            { return _wcsicmp(t.Name(a).c_str(), t.Name(b).c_str()) < 0; });
        for (uint32_t k : kids) walk(k, level + 1);
    };
    walk(root, 0);
    if (progress) progress(done, done);

    if (!WriteWholeFile(outPath, body))
    {
        r.message = L"无法写入文件: " + outPath;
        return r;
    }
    r.ok = true;
    r.bytes = (int64_t)body.size() * 2;
    r.message = L"导出完成";
    return r;
}

// ============================================================ snapshots
namespace {
void PutU8(std::string& b, uint8_t v) { b.push_back((char)v); }
void PutU32(std::string& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF)); }
void PutU64(std::string& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF)); }
void PutStr(std::string& b, const std::wstring& s)
{
    PutU32(b, (uint32_t)s.size());
    for (wchar_t c : s) { b.push_back((char)(c & 0xFF)); b.push_back((char)((c >> 8) & 0xFF)); }
}
uint32_t GetU32(const std::string& b, size_t& p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4 && p < b.size(); ++i) v |= (uint32_t)(uint8_t)b[p++] << (8 * i);
    return v;
}
uint64_t GetU64(const std::string& b, size_t& p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8 && p < b.size(); ++i) v |= (uint64_t)(uint8_t)b[p++] << (8 * i);
    return v;
}
std::wstring GetStr(const std::string& b, size_t& p)
{
    uint32_t n = GetU32(b, p);
    std::wstring s;
    for (uint32_t i = 0; i < n && p + 1 < b.size(); ++i)
    {
        wchar_t c = (wchar_t)(uint8_t)b[p] | ((wchar_t)(uint8_t)b[p + 1] << 8);
        p += 2;
        s += c;
    }
    return s;
}
uint64_t FtToU64(const FILETIME& ft) { ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; return u.QuadPart; }
FILETIME U64ToFt(uint64_t v) { FILETIME ft; ULARGE_INTEGER u; u.QuadPart = v; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart; return ft; }

bool WriteBinaryFile(const std::wstring& path, const std::string& data)
{
    // P0-5: support >MAX_PATH output paths via \\?\ prefix.
    std::wstring lp = ToLongPath(path);
    HANDLE h = CreateFileW(lp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t total = 0;
    bool ok = true;
    while (total < data.size())
    {
        DWORD want = (DWORD)std::min<size_t>(data.size() - total, 16u * 1024 * 1024);
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + total, want, &wrote, nullptr) || wrote == 0) { ok = false; break; }
        total += wrote;
    }
    CloseHandle(h);
    return ok;
}

bool ReadBinaryFile(const std::wstring& path, std::string& data)
{
    // P0-5: support >MAX_PATH snapshot paths via \\?\ prefix.
    std::wstring lp = ToLongPath(path);
    HANDLE h = CreateFileW(lp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 512LL * 1024 * 1024)
    {
        CloseHandle(h);
        return false;
    }
    data.resize((size_t)sz.QuadPart);
    size_t total = 0;
    bool ok = true;
    while (total < data.size())
    {
        DWORD want = (DWORD)std::min<size_t>(data.size() - total, 16u * 1024 * 1024);
        DWORD got = 0;
        if (!ReadFile(h, data.data() + total, want, &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        total += got;
    }
    data.resize(total);
    CloseHandle(h);
    return ok;
}

// P0-3: iterative pre-order serialization with depth cap matching DeserializeNode (512).
static constexpr int kMaxSnapshotDepth = 512;

void SerializeNode(const Tree& t, uint32_t root, std::string& b)
{
    struct Frame { uint32_t node; int depth; };
    std::vector<Frame> stack;
    if (t.Valid(root)) stack.push_back({ root, 0 });
    while (!stack.empty())
    {
        Frame f = stack.back();
        stack.pop_back();
        if (!t.Valid(f.node)) continue;
        const Node& nd = t.nodes[f.node];
        PutU8(b, nd.kind);
        PutStr(b, t.Name(f.node));
        PutStr(b, t.Path(f.node));
        PutU64(b, (uint64_t)nd.size);
        PutU64(b, (uint64_t)nd.disk);
        PutU32(b, nd.attrs);
        PutU64(b, FtToU64(nd.created));
        PutU64(b, FtToU64(nd.modified));
        PutU64(b, FtToU64(nd.accessed));
        PutU8(b, nd.tag);
        int16_t group = nd.group;
        b.push_back((char)(group & 0xFF));
        b.push_back((char)((group >> 8) & 0xFF));
        std::vector<uint32_t> kids;
        if (f.depth < kMaxSnapshotDepth)
        {
            for (uint32_t k = nd.firstChild; k != kNone && t.Valid(k); k = t.nodes[k].nextSibling)
                kids.push_back(k);
        }
        PutU32(b, (uint32_t)kids.size());
        for (size_t i = kids.size(); i-- > 0;)
            stack.push_back({ kids[i], f.depth + 1 });
    }
}

void DeserializeNode(const std::string& b, size_t& p, Tree& t, uint32_t parent, int depth)
{
    if (depth > kMaxSnapshotDepth || p >= b.size()) return;
    Kind kind = (Kind)(uint8_t)b[p++];
    std::wstring name = GetStr(b, p);
    std::wstring path = GetStr(b, p);
    if (p + 8 + 8 + 4 + 8 + 8 + 8 + 1 + 2 + 4 > b.size()) return;
    uint32_t idx = t.AddNode(parent, name, kind);
    Node& n = t.nodes[idx];
    n.size = (int64_t)GetU64(b, p);
    n.disk = (int64_t)GetU64(b, p);
    n.attrs = GetU32(b, p);
    n.created = U64ToFt(GetU64(b, p));
    n.modified = U64ToFt(GetU64(b, p));
    n.accessed = U64ToFt(GetU64(b, p));
    n.tag = (uint8_t)b[p++];
    n.group = (int16_t)((uint8_t)b[p] | ((uint8_t)b[p + 1] << 8));
    p += 2;
    uint32_t count = GetU32(b, p);
    for (uint32_t i = 0; i < count && p < b.size(); ++i) DeserializeNode(b, p, t, idx, depth + 1);
    (void)path;
}

// --- .sns ---------------------------------------------------------------
// P0-3: iterative pre-order .sns writer with depth cap to avoid stack overflow on deep trees.
void PutSnsRecord(std::string& b, const Tree& t, uint32_t root, bool rootFlag)
{
    struct SnsFrame { uint32_t node; bool isRoot; int depth; };
    std::vector<SnsFrame> stack;
    if (t.Valid(root)) stack.push_back({ root, rootFlag, 0 });
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    while (!stack.empty())
    {
        SnsFrame f = stack.back();
        stack.pop_back();
        if (!t.Valid(f.node)) continue;
        const Node& nd = t.nodes[f.node];
        uint8_t type = nd.IsFile() ? 2 : 3;
        std::wstring name = f.isRoot ? t.Path(f.node) : t.Name(f.node);
        std::string utf8;
        if (!name.empty())
        {
            int need = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), nullptr, 0, nullptr, nullptr);
            if (need > 0)
            {
                utf8.resize((size_t)need);
                WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), utf8.data(), need, nullptr, nullptr);
            }
        }

        std::string enc;
        for (size_t i = 0; i < utf8.size(); i += 3)
        {
            uint32_t v = (uint8_t)utf8[i] << 16;
            if (i + 1 < utf8.size()) v |= (uint8_t)utf8[i + 1] << 8;
            if (i + 2 < utf8.size()) v |= (uint8_t)utf8[i + 2];
            enc += b64[(v >> 18) & 63];
            enc += b64[(v >> 12) & 63];
            enc += (i + 1 < utf8.size()) ? b64[(v >> 6) & 63] : '=';
            enc += (i + 2 < utf8.size()) ? b64[v & 63] : '=';
        }
        std::string nameField = enc;
        if (f.isRoot)
        {
            // wrap at 72 chars with CRLF, like the original
            std::string wrapped;
            for (size_t i = 0; i < enc.size(); i += 72)
                wrapped += enc.substr(i, std::min<size_t>(72, enc.size() - i)) + "\r\n";
            nameField = wrapped;
        }

        PutU8(b, 2);
        PutU8(b, type);
        PutU32(b, (uint32_t)nameField.size());
        b += nameField;

        if (type == 2)
        {
            PutU64(b, (uint64_t)nd.size);
            PutU32(b, (uint32_t)std::max<int64_t>(0, nd.disk - nd.size));
            PutU32(b, nd.attrs);
            PutU64(b, FtToU64(nd.created));
            PutU64(b, FtToU64(nd.modified));
            PutU64(b, FtToU64(nd.accessed));
            b.push_back(0); b.push_back(0);
            b.push_back(1); b.push_back(0);
            b.push_back(1); b.push_back(0);
        }
        else
        {
            PutU64(b, (uint64_t)nd.size);
            PutU64(b, (uint64_t)nd.disk);
            for (int i = 0; i < 32; ++i) b.push_back(0);
        }
        if (f.depth < kMaxSnapshotDepth)
        {
            std::vector<uint32_t> kids;
            for (uint32_t k = nd.firstChild; k != kNone && t.Valid(k); k = t.nodes[k].nextSibling)
                kids.push_back(k);
            for (size_t i = kids.size(); i-- > 0;)
                stack.push_back({ kids[i], false, f.depth + 1 });
        }
    }
}

std::string B64Decode(const std::string& in)
{
    static int rev[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i) rev[i] = -1;
        const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) rev[(uint8_t)tbl[i]] = i;
        init = true;
    }
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in)
    {
        if (c == '=' ) break;
        int d = rev[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (need <= 0) return L"";
    std::wstring w((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), need);
    return w;
}

} // namespace

bool SaveSnapshot(const Tree& t, const std::wstring& path, bool nativeSns)
{
    if (!t.Valid(t.root)) return false;
    std::wstring lower = Lowercase(path);
    if (!nativeSns || (lower.size() > 4 && lower.substr(lower.size() - 4) == L".ssx"))
    {
        std::string b = "SSX1";
        SerializeNode(t, t.root, b);
        return WriteBinaryFile(path, b);
    }
    std::string b;
    PutSnsRecord(b, t, t.root, true);
    b.push_back(1); b.push_back(0); b.push_back(1); b.push_back(0); b.push_back(1); b.push_back(0);
    return WriteBinaryFile(path, b);
}

bool LoadSnapshot(const std::wstring& path, Tree& out, std::wstring& error)
{
    std::string d;
    if (!ReadBinaryFile(path, d)) { error = L"无法打开文件"; return false; }
    if (d.empty()) { error = L"空快照"; return false; }

    if (d.size() > 4 && d.compare(0, 4, "SSX1") == 0)
    {
        out.Clear();
        size_t p = 4;
        DeserializeNode(d, p, out, kNone, 0);
        if (out.nodes.empty()) { error = L"快照数据损坏"; return false; }
        out.root = 0;
        out.nodes[0].parent = kNone;
        out.Aggregate();
        out.rootPath = out.Path(out.root);
        return true;
    }

    if ((uint8_t)d[0] == 2)
    {
        // legacy .sns: best-effort with size containment
        struct Flat { uint8_t type; std::wstring name; int64_t size, disk; DWORD attrs; FILETIME c, m, a; };
        std::vector<Flat> flat;
        size_t p = 0;
        while (p + 6 <= d.size())
        {
            if ((uint8_t)d[p] != 2) { ++p; continue; }
            uint8_t type = (uint8_t)d[p + 1];
            if (type < 1 || type > 6) { ++p; continue; }
            size_t hdr = p + 2; uint32_t nlen = GetU32(d, hdr);
            if (nlen == 0 || nlen > 4096 || p + 6 + nlen > d.size()) { ++p; continue; }
            std::string nameField = d.substr(p + 6, nlen);
            std::string raw = B64Decode(nameField);
            std::wstring name = Utf8ToW(raw);
            size_t qStart = p + 6 + nlen;
            size_t q = qStart;
            Flat f{};
            f.type = type;
            f.name = name;
            if (type == 2)
            {
                if (qStart + 46 > d.size()) break;
                f.size = (int64_t)GetU64(d, q);
                uint32_t slack = GetU32(d, q);
                f.disk = f.size + slack;
                p = qStart + 46;
            }
            else
            {
                if (qStart + 48 > d.size()) break;
                f.size = (int64_t)GetU64(d, q);
                f.disk = (int64_t)GetU64(d, q);
                p = qStart + 48;
            }
            flat.push_back(std::move(f));
        }
        if (flat.empty()) { error = L"Bad file format"; return false; }

        out.Clear();
        out.nodes.reserve(flat.size());
        out.names.reserve(flat.size() * 8);
        out.root = out.AddNode(kNone, flat[0].name, flat[0].name.size() <= 3 ? Kind::Drive : Kind::Folder);
        out.nodes[out.root].size = flat[0].size;
        out.nodes[out.root].disk = flat[0].disk;
        std::vector<uint32_t> stack{ out.root };
        for (size_t i = 1; i < flat.size(); ++i)
        {
            while (stack.size() > 1 && out.nodes[stack.back()].disk < flat[i].disk) stack.pop_back();
            uint32_t parent = stack.back();
            Kind kind = flat[i].type == 2 ? Kind::File : Kind::Folder;
            uint32_t idx = out.AddNode(parent, flat[i].name, kind);
            out.nodes[idx].size = flat[i].size;
            out.nodes[idx].disk = flat[i].disk;
            if (kind != Kind::File) stack.push_back(idx);
        }
        out.Aggregate();
        out.rootPath = out.Path(out.root);
        return true;
    }

    // .du text
    std::wstring text = ReadWholeFile(path);
    if (text.empty()) { error = L"无法识别快照格式"; return false; }
    out.Clear();
    std::vector<uint32_t> stack;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() : nl + 1;
        line = Trim(line);
        if (line.empty()) continue;
        bool isDir = false;
        uint64_t size = 0;
        std::wstring pathPart;
        if (line.rfind(L"<dir>", 0) == 0)
        {
            isDir = true;
            std::wstring rest = Trim(line.substr(5));
            size = _wcstoui64(rest.c_str(), nullptr, 10);
            size_t sp = rest.find(L' ');
            pathPart = sp == std::wstring::npos ? rest : Trim(rest.substr(sp + 1));
        }
        else
        {
            size = _wcstoui64(line.c_str(), nullptr, 10);
            size_t sp = line.find(L' ');
            pathPart = sp == std::wstring::npos ? line : Trim(line.substr(sp + 1));
        }
        if (pathPart.empty()) continue;
        uint32_t parent = kNone;
        if (!out.Valid(out.root)) parent = kNone;
        else
        {
            for (size_t k = stack.size(); k-- > 0;)
            {
                std::wstring base = out.Path(stack[k]);
                if (pathPart.rfind(base, 0) == 0) { parent = stack[k]; break; }
            }
        }
        Kind kind = isDir ? (out.Valid(out.root) ? Kind::Folder : Kind::Drive) : Kind::File;
        uint32_t idx = out.AddNode(parent, pathPart, kind);
        out.nodes[idx].size = (int64_t)size;
        out.nodes[idx].disk = (int64_t)size;
        if (!out.Valid(out.root)) out.root = idx;
        if (isDir) { while (!stack.empty() && out.Path(stack.back()).size() >= pathPart.size()) stack.pop_back(); stack.push_back(idx); }
    }
    if (!out.Valid(out.root)) { error = L"空快照"; return false; }
    out.Aggregate();
    out.rootPath = out.Path(out.root);
    return true;
}

} // namespace ss
