# Changelog

All notable changes to NeoSniffer will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.1.0] - 2026-11-26

### Added

- Hardware-accelerated Direct2D treemap with V-Sync and automatic GDI fallback. Select the renderer in Settings → Appearance and restart to apply it.

### Fixed

- Keep free and unknown space in the treemap while excluding them from file counts and aggregated scan sizes.
- Support bare size and age expressions, negative attributes, and inherited tags in filters.
- Restore shortcuts while the treemap has focus. Update settings, log, and help layouts for per-monitor DPI changes.
- Bound MFT run arithmetic and read buffers. Reject scan totals above volume capacity and fall back to enumeration in automatic mode.
- Enable CI on the default `master` branch.
- Return MCP tool discovery as a single JSON-RPC line and preserve numeric request IDs.

### Changed

- MCP `export_report` returns bounded report text through stdio. Clients should remove the former `output` argument.
- MCP caches the two most recently used scans and caps result limits. Directory requests use enumeration. Duplicate and cleanup results are candidates for manual review.

## [1.0.0] - 2026-11-25

### Added
- **Native Win32 C++20 Rewrite (`NeoSniffer.exe`):** Standalone statically linked executable with no third-party DLL or runtime dependencies.
- **Modern UI & High DPI:** Per-Monitor V2 DPI scaling, custom-drawn Dark and Light themes with DWM title-bar integration, tabbed multi-view workspace, start page with drive overview, and custom Settings/Log/Help/About dialogs.
- **Dual Scanning Engine:**
  - Direct NTFS `$MFT` sequential reader (with `$ATTRIBUTE_LIST`, extension record merging, fixup validation, compressed/sparse/WOF handling, and automatic safety-net fallback).
  - Parallel `FindFirstFileExW` (`FindExInfoBasic` + `FIND_FIRST_EX_LARGE_FETCH`) directory scanner for non-elevated and non-NTFS targets, with `\\?\` long-path and UNC support.
- **Nested Squarified Treemap:** Depth-shaded nested containers, configurable detail levels, smooth zoom transitions, File Classes vs. Flat Colors styles, color tags (`Ctrl+1..4`), and rounded custom tooltips.
- **SpaceSniffer 1.3.0.2 Format Compatibility:** Filter DSL, Export Template DSL (`Summary`, `File report`, `Simple file list`, `First level`, `CSV`), `.ssx` / `.sns` / `.du` snapshot loading, and `NeoSnifferConfig.xml` with automatic fallback read from `SpaceSnifferConfig.xml`.
- **Bilingual Localization:** Full English and Simplified Chinese UI with system language auto-detection.
- **Explorer Context Menu Integration:** Optional per-user (`HKCU`) right-click integration for folders, directory backgrounds, and drives.
- **Built-in MCP Server (`NeoSniffer.Mcp.exe`):** Read-only stdio JSON-RPC 2.0 server providing 11 disk-analysis tools for AI agents.
- **Unit Tests & CI:** Synthetic NTFS `$MFT` parser unit tests (`test/mfttest.cpp`) integrated with `build.ps1 -Test` and GitHub Actions (`windows-latest`).
