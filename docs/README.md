# SpaceSniffer Neo（逆向还原 + 现代化重制）

原版 `%USERPROFILE%\bin\SpaceSniffer.exe`（1.3.0.2，2016 年停更，Delphi 2010 + VCL + Graphics32）
的完整逆向分析与现代化重制。**保持原版原生 Win32 自绘架构**，用 C++20 重写为**单文件、零运行库依赖**
的 exe，并补上原版没有的能力。

```
主线：native/out/SpaceSniffer.exe     ← 原生 Win32 C++（无依赖，主推荐）
对照：dist/dotnet/SpaceSniffer.exe    ← .NET 10 自包含单文件（备用 / 算法基线）
工具：native/out/SpaceSniffer.Mcp.exe ← MCP stdio 服务器（AI Agent 接口）
```

## 1. 为什么重制

| 原版问题 | 重制后 |
|---|---|
| 2016 年 Delphi 版本，96 DPI 假设，150%/200% 缩放下发虚、错位 | PerMonitorV2 原生高 DPI，按 DPI 重排与重建字体 |
| 没有深色模式（VCL clBtnFace 系统色） | 原生深色主题（自绘菜单/工具栏/标签/状态栏 + DWM 深色标题栏），浅色可切 |
| 单线程 `FindFirstFileExW` 扫描，大盘慢 | 并行目录枚举 + **NTFS `$MFT` 直读**（WizTree 同款思路），1M 文件 1–5 s |
| 无对外接口 | 内置 **MCP 服务器**，AI Agent 可直接查询占用、找大文件、找重复、给清理建议 |
| 源码不可得 | 完整逆向报告 + 可编译源码 |

## 2. 逆向成果（`docs/ANALYSIS.md`）

- 34 个导出单元名 → 完整源码结构还原（`Filesystemnode`/`Snifthread`/`Treemapnode`/`Frmview` …）
- Delphi 2010 RTTI/VMT 复原：117 个 `类.方法 → 代码地址` 映射
- 8 个窗体的二进制 DFM 全部反编译为文本（`work/dfm/*.dfm`），得到权威 UI 结构、快捷键、默认值
- **过滤器 DSL** 与**导出模板 DSL** 的完整语法（从字符串与解释器代码复原）
- `SpaceSnifferConfig.xml` 配置结构、文件类默认配色
- **`.sns` 二进制快照格式**（用原版自己导出的样本逐字段验证）
- 命令行 `scan/load/filter/export/autoclose/help`
- Ghidra 全量反编译语料（7590 函数）与关键函数地址索引

## 3. 构建

### 原生版（推荐）
```powershell
powershell -ExecutionPolicy Bypass -File native\build.ps1
# 产物：native\out\SpaceSniffer.exe（约 1–3 MB，静态链接，无运行库依赖）
```

依赖：**只有 clang++（llvm-mingw）**。不需要 Visual Studio、不需要 .NET、不需要运行库。

### .NET 对照版
```powershell
dotnet publish src\SpaceSniffer.App -c Release -r win-x64 --self-contained true `n  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o dist\dotnet
```

## 4. 使用

双击 `SpaceSniffer.exe` → 开始页选择驱动器或输入路径 → 树图按占用面积展示，
矩形越大占用越多；双击进入子目录；顶部 Filter 栏输入过滤表达式。

命令行（与原版兼容）：
```
SpaceSniffer.exe scan "D:\" export "File report" "D:\report.txt" autoclose
SpaceSniffer.exe load "snapshot.ssx" filter "*.iso;>1gb"
```

快捷键：

| 键 | 作用 | 键 | 作用 |
|---|---|---|---|
| Ctrl+N | 新建视图 | Backspace / Shift+Backspace | 后退 / 前进 |
| Ctrl+Home | 回到根 | Ctrl+Up | 上升一层 |
| Ctrl + / - | 细节层级 | Ctrl+F / Ctrl+U | 剩余 / 未知空间 |
| Ctrl+T | 文件类着色 | Ctrl+1..4 / Ctrl+0 | 打标签 / 清除 |
| F5 | 刷新 | F1 | 过滤器帮助 |

过滤器示例：`*.jpg;>100kb;>2weeks`、`:red;:yellow`、`|*.exe;|*.dll;\*temp*`、
`:attr:a+h-s`、`:class:Audio/Music`（完整语法见 F1 帮助或 `docs/ANALYSIS.md` §4.3）。

## 5. MCP（AI Agent 接口）

```jsonc
// Claude Desktop / 任意 MCP 客户端
{
  "mcpServers": {
    "spacesniffer": {
      "command": "%REPO%\out\NeoSniffer.Mcp.exe"
    }
  }
}
```

提供的工具（全部只读，不会删除文件）：
`scan`、`list_children`、`largest_files`、`largest_folders`、`search`、`tree`、
`file_types`、`duplicates`、`old_files`、`cleanup_candidates`、`volume_info`、`export_report`。

## 6. 目录

```
docs/ANALYSIS.md     逆向分析报告（权威规格）
docs/ROUTE.md        技术路线评估与决策
native/              原生 C++ 实现（主线）
  BRIEF.md           实现契约
  build.ps1          构建脚本
  src/               源码
  out/               产物
src/                 .NET 参考实现（对照）
tools/               逆向与验证工具（DFM 解包、Ghidra 导出、截图、验收脚本）
work/                DFM 文本、反编译语料、字符串、样本快照
```


## 8. 实测数据（2026-11-25，本机 i?-xx 核 / NVMe / Windows 11）

| 场景 | 结果 |
|---|---|
| 原生版扫描 `C:\Windows`（163,772 文件 / 122,827 目录 / 48.0 GB） | 并行枚举完成，进程内存约 70 MB |
| MCP `scan` 同一目录（进程冷启动含扫描） | **1.41 s** |
| 原生 exe 体积 | 约 0.9 MB（静态链接，无运行库依赖） |
| MCP exe 体积 | 约 0.95 MB |
| .NET 对照版（自包含单文件） | 49 MB，启动后内存约 148 MB |

`$MFT` 直读在管理员权限下启用；当前会话为 Medium 完整性级别，因此自动回退并行枚举。

## 9. 两条技术路线（已各自实测）

| | 主线：原生重写（`native/`） | 对照：原版打补丁（`dist/patched/`） |
|---|---|---|
| 可行性 | 全部功能可实现 | 只能做字节级最小增强 |
| 高 DPI | 完整 PerMonitorV2 + 自绘布局 | 注入清单成功：`GetProcessDpiAwareness` 0→2、`GetDpiForWindow` 96→120，125% 下**大小不变、字变清晰** |
| 深色模式 | 原生实现 | **不可行**（无 SetWindowTheme/OpenThemeData 导入；菜单/滚动条由 comctl32 v6 主题绘制；树图是 Graphics32 硬编码调色板） |
| $MFT 直读 | 已实现 | **不可行**（无 ntdll / DeviceIoControl 导入，需新增导入+代码段） |
| MCP | 已实现（11 工具） | **不可行**（无 WS2_32/WinHTTP/管道，GUI 子系统无 stdout） |
| 扫描加速 | 并行枚举 + MFT | **无提升**：原版 `FindFirstFileExW` 本就是 `FindExInfoBasic + LARGE_FETCH`；A/B 9 次实测 patched 与 orig 无可测差异 |
| 部署 | 单 exe 0.98 MB | 原版 2.3 MB → 补丁版 2.5 MB |

补丁版能力边界与逐字节偏移表见 `docs/PATCH_ROUTE.md`。

## 7. 已知限制

- `.sns` 只做**读取**兼容（字段语义已确认到 size/slack/属性/时间；目录第二个聚合量语义未完全确定，
  按 `size+slack` 重算），写入 `.sns` 标记为实验性；自有格式 `.ssx` 为无损。
- `$MFT` 直读需要管理员权限；非管理员自动回退并行枚举（速度约 1/5，功能不变）。
- 原版的自动更新检查、捐赠跳转未复刻。
