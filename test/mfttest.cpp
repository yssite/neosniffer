// Unit test for the NTFS $MFT record parser and run-list decoder.
// Builds synthetic FILE records (the same layout the reader sees on disk) and checks
// that name / parent reference / sizes / attributes / data-run decoding are correct.
// This verifies the algorithm without needing administrator rights or a raw volume read.
#include "../src/core.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// hooks exposed by core_scan.cpp when compiled with -DSS_TEST_HOOKS
namespace ss::test {
bool MftParseRecordForTest(void* record, unsigned recSize, int cluster,
                           wchar_t* nameOut, int nameCap, int* isDir,
                           long long* realSize, long long* allocSize,
                           unsigned long long* parentRef, unsigned* attrs);
int MftParseRunsForTest(const void* buffer, unsigned len, long long* out, int maxTriples);
long long MftAlignForTest(long long v, int cluster);
}

static int g_failures = 0;

static void Check(bool cond, const char* what)
{
    if (cond) std::printf("  [ok]   %s\n", what);
    else { std::printf("  [FAIL] %s\n", what); ++g_failures; }
}

static void W32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
static void W16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
static void W64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

// ---- builder helpers -------------------------------------------------------
struct Builder
{
    std::vector<uint8_t> buf;
    explicit Builder(size_t size) : buf(size, 0) {}

    // resident attribute: type/len/nameLen/nameOff/flags/id/residentHdr/vlen/voff/flags/resid
    size_t AddResident(uint32_t type, const void* payload, uint32_t payloadLen, uint16_t id)
    {
        size_t p = FindFree();
        uint32_t total = 0x18 + payloadLen;
        total = (total + 7) & ~7u;
        W32(&buf[p], type);
        W32(&buf[p + 4], total);
        buf[p + 8] = 0;                 // resident
        buf[p + 9] = 0;                 // name len
        W16(&buf[p + 10], 0);           // name offset
        W16(&buf[p + 12], 0);           // flags
        W16(&buf[p + 14], id);
        W32(&buf[p + 16], payloadLen);  // value length
        W16(&buf[p + 20], 0x18);        // value offset
        buf[p + 22] = 0;                // indexed flag
        buf[p + 23] = 0;
        std::memcpy(&buf[p + 0x18], payload, payloadLen);
        m_end = p + total;
        return p;
    }

    // non-resident $DATA: allocated / real / compressed sizes + run list
    size_t AddNonResidentData(int64_t allocated, int64_t real, int64_t compressed,
                              uint16_t attrFlags, const std::vector<uint8_t>& runs, uint16_t id)
    {
        size_t p = FindFree();
        uint32_t hdr = 0x48;          // 0x40 fields + CompressedSize at 0x40
        uint32_t total = hdr + (uint32_t)runs.size();
        total = (total + 7) & ~7u;
        W32(&buf[p], 0x80);
        W32(&buf[p + 4], total);
        buf[p + 8] = 1;                 // non-resident
        W16(&buf[p + 12], attrFlags);
        W16(&buf[p + 14], id);
        W64(&buf[p + 16], 0);           // start VCN
        W64(&buf[p + 24], 0);           // last VCN
        W16(&buf[p + 32], hdr);         // run list offset
        W64(&buf[p + 40], (uint64_t)allocated);
        W64(&buf[p + 48], (uint64_t)real);
        W64(&buf[p + 64], (uint64_t)compressed);   // CompressedSize (header >= 0x48)
        std::memcpy(&buf[p + hdr], runs.data(), runs.size());
        m_end = p + total;
        return p;
    }

    size_t FindFree() const { return m_end; }
    size_t m_end = 0x38;
};

static void Finalize(Builder& b, uint16_t flags, uint16_t usaOffset, uint16_t usaCount)
{
    std::memcpy(&b.buf[0], "FILE", 4);
    W16(&b.buf[4], usaOffset);
    W16(&b.buf[6], usaCount);
    W16(&b.buf[0x14], 0x38);            // first attribute offset
    W16(&b.buf[0x16], flags);           // 0x1 in use, 0x2 directory
    W32(&b.buf[0x18], (uint32_t)b.m_end);
    W32(&b.buf[0x1C], (uint32_t)b.buf.size());
    W32(&b.buf[0x2C], 0);               // record number

    // update sequence array: USN + one replacement per sector
    uint16_t usn = 0x0042;
    W16(&b.buf[usaOffset], usn);
    for (uint16_t i = 1; i < usaCount; ++i)
    {
        size_t end = (size_t)i * 512 - 2;
        uint16_t saved = (uint16_t)(0xA000 + i);
        W16(&b.buf[usaOffset + i * 2], saved);
        W16(&b.buf[end], usn);          // what is on disk
    }
}

int wmain()
{
    const int kCluster = 4096;
    const DWORD kRecSize = 1024;

    std::printf("MFT record parser unit test (synthetic FILE records)\n");

    // ---------------------------------------------------------------- case 1: regular file
    {
        Builder b(kRecSize);

        // $STANDARD_INFORMATION (resident, 0x48 bytes)
        std::vector<uint8_t> si(0x48, 0);
        int64_t ftCreate = 132000000000000000LL;   // arbitrary FILETIME
        int64_t ftModify = 132000000010000000LL;
        int64_t ftAccess = 132000000020000000LL;
        std::memcpy(&si[0], &ftCreate, 8);
        std::memcpy(&si[8], &ftModify, 8);
        std::memcpy(&si[24], &ftAccess, 8);
        uint32_t fileAttrs = 0x20;                 // ARCHIVE
        std::memcpy(&si[0x20], &fileAttrs, 4);
        b.AddResident(0x10, si.data(), (uint32_t)si.size(), 0);

        // $FILE_NAME (resident)
        const wchar_t* name = L"test.bin";
        uint32_t nameChars = 8;
        std::vector<uint8_t> fn(0x42 + nameChars * 2, 0);
        uint64_t parentRef = (5ull << 48) | 5ull;  // record 5 = volume root
        std::memcpy(&fn[0], &parentRef, 8);
        std::memcpy(&fn[8], &ftCreate, 8);
        std::memcpy(&fn[16], &ftModify, 8);
        int64_t allocSz = 16384, realSz = 12345;
        std::memcpy(&fn[40], &allocSz, 8);
        std::memcpy(&fn[48], &realSz, 8);
        fn[0x40] = (uint8_t)nameChars;             // name length in chars
        fn[0x41] = 1;                              // Win32 namespace
        std::memcpy(&fn[0x42], name, nameChars * 2);
        b.AddResident(0x30, fn.data(), (uint32_t)fn.size(), 1);

        // $DATA (non-resident): allocated 16384, real 12345
        std::vector<uint8_t> runs = { 0x11, 0x04, 0x20, 0x00 };  // 4 clusters @ LCN 0x20
        b.AddNonResidentData(allocSz, realSz, 0, 0x0000, runs, 2);

        // terminate attribute list
        W32(&b.buf[b.m_end], 0xFFFFFFFF);
        Finalize(b, 0x0001, 0x30, 0x03);

        wchar_t outName[128] = {};
        int isDir = -1;
        long long realOut = 0, allocOut = 0;
        unsigned long long parentOut = 0;
        unsigned attrsOut = 0;
        bool ok = ss::test::MftParseRecordForTest(b.buf.data(), kRecSize, kCluster,
                                                 outName, 128, &isDir, &realOut, &allocOut,
                                                 &parentOut, &attrsOut);
        Check(ok, "record parsed");
        Check(std::wstring(outName) == L"test.bin", "file name decoded (UTF-16, namespace 1)");
        Check(isDir == 0, "type = file");
        Check(realOut == 12345, "logical size = 12345");
        Check(allocOut == 16384, "allocated size = 16384 (from $DATA)");
        Check(parentOut == parentRef, "parent reference = record 5");
        Check(attrsOut == 0x20, "attributes = ARCHIVE");
    }

    // ---------------------------------------------------------------- case 2: directory
    {
        Builder b(kRecSize);
        std::vector<uint8_t> si(0x48, 0);
        uint32_t fa = 0x10;                        // DIRECTORY
        std::memcpy(&si[0x20], &fa, 4);
        b.AddResident(0x10, si.data(), (uint32_t)si.size(), 0);

        const wchar_t* name = L"src";
        uint32_t n = 3;
        std::vector<uint8_t> fn(0x42 + n * 2, 0);
        uint64_t parentRef = (42ull << 48) | 42ull;
        std::memcpy(&fn[0], &parentRef, 8);
        fn[0x40] = (uint8_t)n;
        fn[0x41] = 3;                              // Win32&DOS
        std::memcpy(&fn[0x42], name, n * 2);
        b.AddResident(0x30, fn.data(), (uint32_t)fn.size(), 1);
        W32(&b.buf[b.m_end], 0xFFFFFFFF);
        Finalize(b, 0x0003, 0x30, 0x03);           // in use + directory

        wchar_t outName[128] = {};
        int isDir = -1;
        long long realOut = -1, allocOut = -1;
        bool ok = ss::test::MftParseRecordForTest(b.buf.data(), kRecSize, kCluster,
                                                 outName, 128, &isDir, &realOut, &allocOut,
                                                 nullptr, nullptr);
        Check(ok, "directory record parsed");
        Check(std::wstring(outName) == L"src", "directory name decoded");
        Check(isDir == 1, "type = directory");
        Check(realOut == 0 && allocOut == 0, "directory sizes are 0");
    }

    // ---------------------------------------------------------------- case 3: metadata + free records are skipped
    {
        Builder b(kRecSize);
        const wchar_t* name = L"$MFT";
        uint32_t n = 4;
        std::vector<uint8_t> fn(0x42 + n * 2, 0);
        fn[0x40] = (uint8_t)n;
        fn[0x41] = 1;
        std::memcpy(&fn[0x42], name, n * 2);
        b.AddResident(0x30, fn.data(), (uint32_t)fn.size(), 0);
        W32(&b.buf[b.m_end], 0xFFFFFFFF);
        Finalize(b, 0x0001, 0x30, 0x03);
        wchar_t out[64] = {};
        Check(!ss::test::MftParseRecordForTest(b.buf.data(), kRecSize, kCluster, out, 64,
                                               nullptr, nullptr, nullptr, nullptr, nullptr),
              "$MFT metadata record skipped");

        Builder f(kRecSize);
        std::memcpy(&f.buf[0], "FILE", 4);
        W16(&f.buf[0x14], 0x38);
        W16(&f.buf[0x16], 0x0000);                 // not in use
        W32(&f.buf[0x1C], kRecSize);
        Check(!ss::test::MftParseRecordForTest(f.buf.data(), kRecSize, kCluster, out, 64,
                                               nullptr, nullptr, nullptr, nullptr, nullptr),
              "free record skipped");
    }

    // ---------------------------------------------------------------- case 4: compressed file
    {
        Builder b(kRecSize);
        std::vector<uint8_t> si(0x48, 0);
        b.AddResident(0x10, si.data(), (uint32_t)si.size(), 0);
        const wchar_t* name = L"packed.dat";
        uint32_t n = 10;
        std::vector<uint8_t> fn(0x42 + n * 2, 0);
        fn[0x40] = (uint8_t)n;
        fn[0x41] = 1;
        std::memcpy(&fn[0x42], name, n * 2);
        b.AddResident(0x30, fn.data(), (uint32_t)fn.size(), 1);
        std::vector<uint8_t> runs = { 0x11, 0x02, 0x40, 0x00 };  // 2 clusters @ LCN 0x40
        b.AddNonResidentData(8192, 100000, 3000, 0x0001, runs, 2);   // attr flag COMPRESSED
        W32(&b.buf[b.m_end], 0xFFFFFFFF);
        Finalize(b, 0x0001, 0x30, 0x03);

        wchar_t out[64] = {};
        long long realOut = 0, allocOut = 0;
        bool ok = ss::test::MftParseRecordForTest(b.buf.data(), kRecSize, kCluster, out, 64,
                                                  nullptr, &realOut, &allocOut, nullptr, nullptr);
        Check(ok, "compressed record parsed");
        Check(realOut == 100000, "logical size = 100000");
        Check(allocOut == 3000, "compressed -> on-disk size = compressed size (3000)");
    }

    // ---------------------------------------------------------------- case 5: run-list decoder
    {
        // run 1: length 4  at LCN +0x20       -> vcn 0,  lcn 0x20
        // run 2: length 3  at LCN +0x10       -> vcn 4,  lcn 0x30
        // run 3: length 2  at LCN -0x08 (neg) -> vcn 7,  lcn 0x28
        std::vector<uint8_t> runs = {
            0x11, 0x04, 0x20,          // length 4, offset +0x20
            0x11, 0x03, 0x10,          // length 3, offset +0x10
            0x11, 0x02, 0xF8,          // length 2, offset -8 (0xF8 as int8)
            0x00                       // terminator
        };
        long long out[9] = {};
        int n = ss::test::MftParseRunsForTest(runs.data(), (unsigned)runs.size(), out, 3);
        Check(n == 3, "3 runs decoded");
        Check(out[0] == 0 && out[1] == 0x20 && out[2] == 4, "run1: vcn0 lcn0x20 len4");
        Check(out[3] == 4 && out[4] == 0x30 && out[5] == 3, "run2: vcn4 lcn0x30 len3");
        Check(out[6] == 7 && out[7] == 0x28 && out[8] == 2, "run3: negative delta handled (lcn 0x28)");
    }

    // ---------------------------------------------------------------- case 6: cluster alignment
    {
        Check(ss::test::MftAlignForTest(0, 4096) == 0, "align(0) = 0");
        Check(ss::test::MftAlignForTest(1, 4096) == 4096, "align(1) = 4096");
        Check(ss::test::MftAlignForTest(4096, 4096) == 4096, "align(4096) = 4096");
        Check(ss::test::MftAlignForTest(200002, 4096) == 200704, "align(200002) = 200704");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
