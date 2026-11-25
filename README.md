# NeoSniffer

**A native Win32 C++20 single-executable disk space analyzer with Per-Monitor V2 High DPI, dark mode, direct NTFS `$MFT` scanning, and a built-in read-only MCP server for AI agents.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D6.svg)](#build)
[![Build](https://github.com/yssite/neosniffer/actions/workflows/build.yml/badge.svg)](https://github.com/yssite/neosniffer/actions/workflows/build.yml)

---

## Screenshots

| Dark Treemap | Start Page | Settings |
| :---: | :---: | :---: |
| ![Dark Treemap](docs/images/treemap-dark.png) | ![Start Page](docs/images/start-page.png) | ![Settings](docs/images/settings.png) |

---

## Features

| Feature | Details |
|---|---|
| **Single Native Binary** | C++20 + Win32 API compiled with `-static`; zero `.NET`, VC++ redistributable, or third-party DLL dependencies. |
| **Dark Mode & Per-Monitor V2 DPI** | Custom-drawn dark and light themes with DWM title-bar chrome; crisp rendering across 100%–200%+ displays. |
| **Direct NTFS `$MFT` Reader** | Reads `$MFT` sequentially when elevated (5–20× faster on large NTFS volumes); automatically falls back to parallel `FindFirstFileExW` (`LARGE_FETCH`) when non-elevated or on non-NTFS volumes. |
| **Nested Squarified Treemap** | Proportional nested containers, configurable detail levels, smooth zoom animations, file-class coloring, color tags, and custom tooltips. |
| **Filter DSL** | Compatible with SpaceSniffer 1.3.0.2 syntax: wildcards, size/age thresholds, tags, attributes, file classes, folder names, and `|` negation. |
| **Export Templates** | Built-in `Summary`, `File report`, `Simple file list`, `First level`, and `CSV` templates using `{script}` / `if` / `leftpad` / `<%tag%>` expressions. |
| **Snapshots** | Save and load `.ssx` snapshots; load legacy `.sns` (SpaceSniffer 1.3.0.2) and `.du` snapshots. |
| **Bilingual UI** | English and Simplified Chinese with `System` auto-detection and live preview in Settings. |
| **Built-in MCP Server** | `NeoSniffer.Mcp.exe` exposes **11 read-only tools** over stdio JSON-RPC 2.0 for Claude Desktop, Cursor, and other MCP hosts. |

---

## Build

### Prerequisites

- Windows 10 / 11 (x64)
- **llvm-mingw 20.x** (`clang++` and `windres` in `PATH`) — download from [mstorsjo/llvm-mingw releases](https://github.com/mstorsjo/llvm-mingw/releases)
- PowerShell 5.1+ or PowerShell 7+

### Compile

```powershell
# Build x64 binaries (out\NeoSniffer.exe and out\NeoSniffer.Mcp.exe)
powershell -ExecutionPolicy Bypass -File build.ps1 -Target x64

# Build x64 and run unit tests (out\mfttest.exe)
powershell -ExecutionPolicy Bypass -File build.ps1 -Target x64 -Test

# Cross-compile for ARM64 (requires aarch64-w64-mingw32 toolchain in PATH)
powershell -ExecutionPolicy Bypass -File build.ps1 -Target arm64
```

> **Note:** On first run, NeoSniffer reads `NeoSnifferConfig.xml` next to the executable. If it does not exist, it automatically falls back to reading a legacy `SpaceSnifferConfig.xml` if present, then saves future changes to `NeoSnifferConfig.xml`.

---

## Usage

### Command Line Interface

```powershell
# Open a scan view
NeoSniffer.exe scan "C:\"

# Load a snapshot (.ssx / .sns / .du)
NeoSniffer.exe load "snapshot.ssx"

# Apply a filter to the preceding scan/load
NeoSniffer.exe scan "D:\Projects" filter "*.iso;d>1gb"

# Headless scan + export + exit
NeoSniffer.exe scan "D:\Projects" export "Summary" "report.txt" autoclose

# Show command line help
NeoSniffer.exe help
```

### Explorer Right-Click Integration

Open **Edit → Settings → Behavior** and enable **Integrate into Explorer context menu**. This registers a per-user (`HKCU\Software\Classes`) context menu entry (**Analyze with NeoSniffer**) on folders, folder backgrounds, and drives—no administrator rights required. Uncheck it to cleanly remove all registry keys.

### Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+N` | Open Start page / new scan view |
| `F5` | Refresh current view |
| `Ctrl+L` | Show / hide filter bar |
| `Backspace` / `Shift+Backspace` | Navigate back / forward |
| `Ctrl+Up` / `Ctrl+Home` | Up one level / go to root |
| `Ctrl++` / `Ctrl+-` | More / less treemap detail |
| `Ctrl+T` | Toggle File Classes vs. Flat Colors |
| `Ctrl+F` / `Ctrl+U` | Toggle Free Space / Unknown Space |
| `Ctrl+1` .. `Ctrl+4` | Tag selected item (Red / Yellow / Green / Blue) |
| `Ctrl+0` | Clear all tags in current view |
| `F1` | Open Filter Help |

---

## MCP Server (AI Agent Integration)

`NeoSniffer.Mcp.exe` speaks **MCP (Model Context Protocol, `2024-11-05`)** over `stdio` JSON-RPC 2.0. All analysis tools are **read-only**—no tool deletes or modifies user files (only `export_report` writes a report to the requested output path).

### Claude Desktop / Cursor Configuration

Add the following to your `claude_desktop_config.json` or `.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "neosniffer": {
      "command": "C:\\path\\to\\NeoSniffer.Mcp.exe",
      "args": []
    }
  }
}
```

### Available Tools (11)

| Tool | Purpose |
|---|---|
| `scan` | Scan a drive or directory, cache the tree, and return summary stats + top 10 children. |
| `list_children` | List direct children of a directory sorted by allocated disk size. |
| `largest_files` | Return the largest files under the scanned path. |
| `largest_folders` | Return the largest folders under the scanned path. |
| `search` | Filter files using NeoSniffer's SpaceSniffer-compatible Filter DSL. |
| `tree` | Render an indented text tree with configurable `depth` and `max_nodes`. |
| `file_types` | Aggregate file counts and disk usage by extension. |
| `duplicates` | Find same-name, same-size duplicate candidates and estimate reclaimable space. |
| `cleanup_candidates` | Suggest temp/backup files and stale installers with risk levels (never deletes). |
| `volume_info` | Query filesystem type, cluster size, total/free bytes, and `$MFT` readability. |
| `export_report` | Generate a report using a built-in export template (`Summary`, `File report`, `Simple file list`, `First level`, `CSV`). |

---

## Known Limitations

Tracked from our static audit ([docs/CODE_REVIEW.md](docs/CODE_REVIEW.md)):

- **Global shortcuts & view focus:** Keyboard shortcuts are handled on the main frame (`MainProc::WM_KEYDOWN`); when a scan view child window holds focus, shortcuts (including `Ctrl+1..4` tagging) may not fire until focus returns to the main window.
- **Filter DSL edge cases:** Unprefixed size/age filters (e.g., `>1gb`, `>2weeks`), `:attr:` negation (`-s`), and parent-tag inheritance for `:tag:` are not yet fully wired—use explicit prefixes such as `d>1gb`, `s>1gb`, or `m>2weeks`.
- **Empty filter & virtual nodes:** A filter matching zero files currently falls back to displaying the unfiltered tree, and whole-volume virtual nodes (`Free Space` / `Unknown`) are included in root size aggregation.
- **Secondary window DPI changes:** Settings, Log, and Help windows scale to the owner's DPI when opened, but do not yet dynamically rescale on `WM_DPICHANGED` if dragged to a monitor with a different DPI.
- **Filter bar & CLI export:** Pressing `Enter` inside the filter input does not submit yet (click **Apply**); exports currently run against the unfiltered tree; CLI auto-export waits up to 30 seconds for a scan to complete.
- **Snapshot `.du` save/load:** Saving a `.du` snapshot currently writes `.ssx` binary format; loading `.du` text snapshots expects single-segment names.
- **Placeholder settings & elevation fallback:** Several compatibility settings (`refreshDelayMs`, `useRealZoom`, `scanADS`, `logPopup`/`logIssue`/`logWarning`) are persisted in XML but not yet wired to runtime behavior. If UAC elevation fails or is cancelled, NeoSniffer falls back to standard user privileges and directory enumeration (no retry prompt). A few core-layer fallback error strings may appear in Chinese when running in English mode.

---

## Disclaimer

NeoSniffer is an **unofficial, independent open-source reimplementation** written from scratch in C++20 / Win32. It is **not affiliated with, endorsed by, or derived from** Uderzo Umberto or the original SpaceSniffer software, and **contains no original SpaceSniffer source code or binaries**. Compatibility is limited to public file/syntax conventions (SpaceSniffer 1.3.0.2 filter syntax, export template syntax, snapshot files, and XML configuration schema).

---

## License

Released under the [MIT License](LICENSE).

---

<details>
<summary><strong>中文文档 / Chinese Documentation</strong>（点击展开）</summary>

# NeoSniffer

**原生 Win32 C++20 单文件磁盘空间分析工具：支持 Per-Monitor V2 高 DPI、深浅色主题、NTFS `$MFT` 直读加速，以及面向 AI Agent 的内置只读 MCP 服务器。**

---

## 界面截图

| 深色树图 | 开始页 | 设置页 |
| :---: | :---: | :---: |
| ![深色树图](docs/images/treemap-dark.png) | ![开始页](docs/images/start-page.png) | ![设置页](docs/images/settings.png) |

---

## 核心特性

| 特性 | 说明 |
|---|---|
| **原生单文件零依赖** | C++20 + 纯 Win32 API 静态链接（`-static`），不依赖 .NET、VC 运行库或第三方 DLL。 |
| **深浅色主题 + Per-Monitor V2** | 全自绘深色/浅色界面与 DWM 标题栏，在 100%–200%+ 高分屏下清晰锐利。 |
| **NTFS `$MFT` 直读加速** | 管理员权限下顺序直读 `$MFT`（大盘提速 5–20 倍）；非管理员或非 NTFS 卷自动回退并行 `FindFirstFileExW`（`LARGE_FETCH`）。 |
| **嵌套 Squarified 树图** | 容器嵌套层级可视化、可调细节层级、平滑缩放动画、文件类配色、颜色标签与自绘悬浮卡片。 |
| **过滤器 DSL** | 兼容 SpaceSniffer 1.3.0.2 语法：通配符、大小/年龄条件、标签、属性、文件类、目录名及 `|` 取反。 |
| **导出模板引擎** | 内置 `Summary`、`File report`、`Simple file list`、`First level`、`CSV` 模板，支持 `{script}` / `if` / `leftpad` / `<%tag%>`。 |
| **快照读写** | 支持保存与加载 `.ssx` 快照，支持加载原版 SpaceSniffer `.sns` 及 `.du` 快照。 |
| **中英双语** | 支持简体中文、English 与跟随系统自动切换，设置页支持即时预览。 |
| **内置 MCP 服务器** | `NeoSniffer.Mcp.exe` 通过 stdio JSON-RPC 2.0 提供 **11 个只读分析工具**，供 Claude Desktop / Cursor 等 AI 宿主调用。 |

---

## 构建

### 环境要求

- Windows 10 / 11 (x64)
- **llvm-mingw 20.x**（`clang++` 与 `windres` 已加入 `PATH`）— 可从 [mstorsjo/llvm-mingw releases](https://github.com/mstorsjo/llvm-mingw/releases) 下载
- PowerShell 5.1+ 或 PowerShell 7+

### 编译命令

```powershell
# 编译 x64 产物（out\NeoSniffer.exe 与 out\NeoSniffer.Mcp.exe）
powershell -ExecutionPolicy Bypass -File build.ps1 -Target x64

# 编译 x64 并运行单元测试（out\mfttest.exe）
powershell -ExecutionPolicy Bypass -File build.ps1 -Target x64 -Test

# 交叉编译 ARM64（需 PATH 中包含 aarch64-w64-mingw32 工具链）
powershell -ExecutionPolicy Bypass -File build.ps1 -Target arm64
```

> **配置兼容性提示**：首次启动优先读取同目录 `NeoSnifferConfig.xml`；若不存在，会自动回退读取旧版 `SpaceSnifferConfig.xml` 并平滑迁移至 `NeoSnifferConfig.xml`。

---

## 使用指南

### 命令行参数

```powershell
# 打开扫描视图
NeoSniffer.exe scan "C:\"

# 载入快照（.ssx / .sns / .du）
NeoSniffer.exe load "snapshot.ssx"

# 对前一个 scan/load 应用过滤器
NeoSniffer.exe scan "D:\Projects" filter "*.iso;d>1gb"

# 自动扫描、导出并在完成后退出
NeoSniffer.exe scan "D:\Projects" export "Summary" "report.txt" autoclose

# 查看命令行帮助
NeoSniffer.exe help
```

### 资源管理器右键菜单集成

在 **编辑 → 设置 → 行为** 中勾选 **集成到资源管理器右键菜单**，即可在当前用户范围（`HKCU\Software\Classes`，无需管理员权限）为文件夹、文件夹空白处和驱动器注册 **使用 NeoSniffer 分析占用** 菜单项；取消勾选即可完整卸载对应注册表键。

### 快捷键

| 快捷键 | 功能 |
|---|---|
| `Ctrl+N` | 打开开始页 / 新建扫描视图 |
| `F5` | 刷新当前视图 |
| `Ctrl+L` | 显示 / 隐藏过滤条 |
| `Backspace` / `Shift+Backspace` | 后退 / 前进 |
| `Ctrl+Up` / `Ctrl+Home` | 上升一层 / 回到根 |
| `Ctrl++` / `Ctrl+-` | 增加 / 减少树图细节层级 |
| `Ctrl+T` | 切换文件类着色 / 纯色样式 |
| `Ctrl+F` / `Ctrl+U` | 显示或隐藏剩余空间 / 未知空间 |
| `Ctrl+1` .. `Ctrl+4` | 给选中项打标签（红 / 黄 / 绿 / 蓝） |
| `Ctrl+0` | 清除当前视图全部标签 |
| `F1` | 打开过滤器帮助 |

---

## MCP 服务器用法

`NeoSniffer.Mcp.exe` 实现 **MCP（`2024-11-05`）** stdio JSON-RPC 2.0 协议。所有分析工具均为**只读**，不会删除或修改用户文件（仅 `export_report` 会将报告写入调用方指定的输出文件）。

### Claude Desktop / Cursor 配置示例

将以下片段写入 `claude_desktop_config.json` 或 `.cursor/mcp.json`：

```json
{
  "mcpServers": {
    "neosniffer": {
      "command": "C:\\path\\to\\NeoSniffer.Mcp.exe",
      "args": []
    }
  }
}
```

### 11 个内置工具

| 工具名 | 功能 |
|---|---|
| `scan` | 扫描指定驱动器或目录并缓存节点树，返回总体统计与前 10 大子项。 |
| `list_children` | 按磁盘占用降序列出目录直接子项。 |
| `largest_files` | 列出扫描范围内体积最大的文件。 |
| `largest_folders` | 列出扫描范围内占用最大的目录。 |
| `search` | 使用兼容 SpaceSniffer 的过滤器语法检索文件。 |
| `tree` | 按指定深度与节点上限输出缩进文本目录树。 |
| `file_types` | 按文件扩展名聚合数量与磁盘占用。 |
| `duplicates` | 按同名同大小查找潜在重复文件并估算可回收空间（只读）。 |
| `cleanup_candidates` | 给出临时/备份文件及陈旧安装包清理建议与风险等级（不删除）。 |
| `volume_info` | 查询文件系统、簇大小、容量/剩余空间及 `$MFT` 可读状态。 |
| `export_report` | 使用内置模板（`Summary` / `File report` / `Simple file list` / `First level` / `CSV`）导出报告。 |

---

## 已知限制

汇总自静态代码复审（详见 [docs/CODE_REVIEW.md](docs/CODE_REVIEW.md)）：

- **视图焦点与全局快捷键**：当前全局快捷键在主窗口 `MainProc::WM_KEYDOWN` 中处理；当扫描视图子窗口获得焦点时，快捷键（含 `Ctrl+1..4` 标签）可能无法直接触发。
- **过滤器 DSL 边角语法**：无前缀的大小/年龄条件（如 `>1gb`、`>2weeks`）、`:attr:` 取反（如 `-s`）以及 `:tag:` 的父目录标签继承尚未完全接通，建议暂用带前缀写法（如 `d>1gb`、`s>1gb`、`m>2weeks`）。
- **空过滤与伪节点汇总**：过滤结果为空时会回退显示未过滤树；整盘扫描时 `Free Space` / `Unknown` 伪节点会被计入根节点大小汇总。
- **子窗口跨屏 DPI 切换**：设置、日志与帮助窗口在打开时按宿主 DPI 缩放，但跨不同缩放比例显示器拖动时暂未处理 `WM_DPICHANGED` 动态重排。
- **过滤框与 CLI 导出**：过滤框暂不支持按回车提交（需点击“过滤”按钮）；导出当前未应用过滤投影；CLI 自动导出等待扫描完成的上限为 30 秒。
- **`.du` 快照读写**：保存 `.du` 当前实际写入 `.ssx` 二进制数据；载入 `.du` 文本快照时节点名未截断父路径。
- **占位设置项与提权回退**：部分配置项（`refreshDelayMs`、`useRealZoom`、`scanADS`、`logPopup`/`logIssue`/`logWarning`）仅为 XML 格式兼容保留，暂未驱动运行期行为；开启管理员重启但 UAC 被取消或失败时无重试兜底，将直接回退为普通权限与目录枚举模式；英文模式下核心层个别回退提示仍为中文。

---

## 免责声明

NeoSniffer 是一个从零编写的**非官方独立开源重制项目**，与原版 SpaceSniffer 及其作者（Uderzo Umberto）无任何隶属或授权关系，仓库中**不包含任何原版 SpaceSniffer 源码或二进制文件**。本项目仅出于互操作需要兼容其公开的过滤器语法、导出模板、快照与配置 XML 格式。

---

## 许可证

基于 [MIT License](LICENSE) 开源。

</details>
