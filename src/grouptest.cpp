// Console harness: verify group assignment and filter parsing.
#include "../src/core.h"
#include <cstdio>
#include <string>

using namespace ss;

int wmain(int argc, wchar_t** argv)
{
    std::wstring path = argc > 1 ? argv[1] : L"G:\\Reverse-Enginnering-AI\\SpaceSniffer_Reverse\\work\\testdata";

    AppConfig cfg = AppConfig::Default();
    wprintf(L"default classes=%zu exports=%zu initialStyle=%s\n",
            cfg.classes.items.size(), cfg.exports.size(), cfg.initialStyle.c_str());
    for (auto& c : cfg.classes.items)
        wprintf(L"  class %-30s ext=%zu color=%06X\n", c.name.c_str(), c.extensions.size(),
                (unsigned)c.color);

    ScanOptions opt;
    opt.mode = ScanOptions::Fast;
    Tree t;
    ScanProgress prog;
    bool mft = false;
    if (!ScanPath(path, opt, t, &prog, &mft)) { wprintf(L"scan failed\n"); return 1; }
    t.ApplyGroups(cfg.classes);

    int shown = 0;
    for (size_t i = 0; i < t.nodes.size() && shown < 12; ++i)
    {
        const Node& n = t.nodes[i];
        if (!n.IsFile()) continue;
        std::wstring name = t.Name((uint32_t)i);
        wprintf(L"  file %-24s ext=%-5s group=%d\n", name.c_str(),
                (name.find(L'.') == std::wstring::npos ? L"-" : name.substr(name.find_last_of(L'.') + 1).c_str()),
                (int)n.group);
        shown++;
    }
    return 0;
}
