# Contributing to NeoSniffer

Thank you for considering contributing to NeoSniffer!

## Repository Layout

```
NeoSniffer/
├── src/                        # Application, core scanner/features, UI, i18n, and MCP server
│   ├── main.cpp                # Win32 window shell, tabs, toolbar, menus, CLI, About dialog
│   ├── core.h                  # Shared node tree, scanner, filter, export, snapshot, config types
│   ├── core_scan.cpp           # Parallel FindFirstFileExW + NTFS $MFT reader
│   ├── core_features.cpp       # Config XML, filter DSL, export templates, snapshots
│   ├── ui_treemap.h/.cpp       # Nested squarified layout, GDI renderer, tooltip, zoom animation
│   ├── ui_config.h/.cpp        # Settings dialog (GDI + GDI+) and Explorer shell integration
│   ├── i18n.h/.cpp             # Simplified Chinese / English localization table
│   └── mcp_main.cpp            # NeoSniffer.Mcp.exe stdio JSON-RPC 2.0 server
├── test/                       # Unit tests
│   └── mfttest.cpp             # Synthetic NTFS FILE record & data-run decoder tests
├── docs/                       # Reverse-engineering analysis, code review, screenshots
├── app.rc                      # Win32 version info & icon resource
├── build.ps1                   # Build & test script (-Target x64|arm64, -Test)
└── .github/workflows/build.yml # Windows CI workflow
```

## Building Locally

### Toolchain

Install **llvm-mingw 20.x** (`ucrt-x86_64`) from [mstorsjo/llvm-mingw releases](https://github.com/mstorsjo/llvm-mingw/releases) and ensure its `bin/` folder is on your `PATH`.

### Build Commands

```powershell
# Build GUI (out\NeoSniffer.exe) and MCP server (out\NeoSniffer.Mcp.exe)
powershell -ExecutionPolicy Bypass -File build.ps1 -Target x64

# Build and run unit tests (out\mfttest.exe)
powershell -ExecutionPolicy Bypass -File build.ps1 -Target x64 -Test

# Optional: cross-compile for ARM64
powershell -ExecutionPolicy Bypass -File build.ps1 -Target arm64
```

## Running Unit Tests

Running `build.ps1 -Test` compiles `test/mfttest.cpp` with `-DSS_TEST_HOOKS` against `src/core_scan.cpp` and `src/core_features.cpp` and executes `out\mfttest.exe`. Tests run without administrator privileges using synthetic NTFS `$MFT` records.

## Code Style & Constraints

- **Language & Compiler:** C++20 with `clang++` (`-std=c++20 -O2 -municode -static -Wall -Wextra`).
- **Zero External Runtime Dependencies:** Use Windows system DLLs only. The treemap also uses system Direct2D and DirectWrite.
- **Unicode & Encoding:** Keep source files UTF-8 and use `wchar_t` / `L"..."` Win32 Unicode APIs (`*W`).
- **Resource Safety:** Pair all GDI/Win32 handles (`CreateCompatibleDC`, `CreateCompatibleBitmap`, `CreateFontW`, `CreatePen`, `CreateSolidBrush`, `CreateFileW`, `RegOpenKeyExW`) using RAII wrappers or guaranteed cleanup paths.
- **Safety & Simplicity:** Avoid `goto` and `std::regex`; bound all recursion depths and untrusted buffer sizes.
- **Localization:** Every user-facing string must be registered in `src/i18n.cpp` with both Chinese (`zh`) and English (`en`) entries.
- **DPI & Themes:** Scale all pixel metrics via `S(...)` / DPI helpers and verify both Dark and Light modes.

## Pull Request Checklist

- [ ] Builds cleanly with `powershell -ExecutionPolicy Bypass -File build.ps1 -Target x64 -Test` with zero new warnings.
- [ ] All unit tests pass.
- [ ] Tested in both Dark and Light themes and in both English and Simplified Chinese.
- [ ] No personal paths, credentials, or local machine artifacts introduced.
- [ ] Relevant documentation (`README.md`, `CHANGELOG.md`, `docs/`) updated when behavior changes.
