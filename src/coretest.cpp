// Console harness for quick core verification (not part of the shipped app).
#include "../src/core.h"
#include <cstdio>
#include <string>

using namespace ss;

int wmain(int argc, wchar_t** argv)
{
    std::wstring path = argc > 1 ? argv[1] : L"G:\\Reverse-Enginnering-AI\\SpaceSniffer_Reverse\\work\\testdata";
    ScanOptions opt;
    opt.mode = ScanOptions::Fast;
    Tree t;
    ScanProgress prog;
    bool mft = false;
    if (!ScanPath(path, opt, t, &prog, &mft))
    {
        wprintf(L"scan failed\n");
        return 1;
    }
    wprintf(L"nodes=%zu names=%zu root=%u mft=%d\n", t.nodes.size(), t.names.size(), t.root, (int)mft);
    wprintf(L"root name=%s disk=%lld size=%lld\n", t.Name(t.root).c_str(),
            (long long)t.nodes[t.root].disk, (long long)t.nodes[t.root].size);
    int n = 0;
    for (uint32_t c = t.nodes[t.root].firstChild; c != Node::kNone; c = t.nodes[c].nextSibling)
    {
        wprintf(L"  child[%d] %s kind=%u disk=%lld size=%lld\n", n++, t.Name(c).c_str(),
                (unsigned)t.nodes[c].kind, (long long)t.nodes[c].disk, (long long)t.nodes[c].size);
        if (n > 20) break;
    }
    return 0;
}
