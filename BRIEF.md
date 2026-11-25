# SpaceSniffer 原生重制版 — 实现说明（给实现 Agent）

## 目标
把 `C:\Users\EDM\bin\SpaceSniffer.exe`（Delphi 2010 + VCL + Graphics32，2016 年停更）重制为
**原生 Win32 C++ 单文件 exe**，保持原版架构（原生窗口 + 自绘树图 + 26 个源单元的功能划分），并加上
原版没有的现代化能力：

1. 原生高 DPI（PerMonitorV2）
2. 原生深色模式（含标题栏）
3. 并行目录枚举 + **NTFS $MFT 直读**双扫描引擎
4. MCP 服务器（stdio JSON-RPC），供 AI Agent 分析占用
5. 完整兼容原版的过滤器 DSL / 导出模板 DSL / 配置文件

## 硬性约束
- **语言/工具链**：C++20 + clang++（llvm-mingw 20.1.2，已在 PATH）。
- **产物**：单个 exe，`-static`，**不得依赖 .NET / VC 运行库 / 任何第三方 DLL**（只允许系统 DLL：kernel32/user32/gdi32/comctl32/dwmapi/shell32/ole32/uxtheme/shlwapi/advapi32）。
- **界面语言**：简体中文（源码 UTF-8，用 `L"..."` 宽字符字面量）。
- **不要用 `#pragma comment(lib, ...)`**（clang/mingw 不支持），所有库在 `build.ps1` 的链接参数里给出。
- 只允许修改 `native/` 目录内的文件。

## 构建
```powershell
powershell -ExecutionPolicy Bypass -File native\build.ps1
```
产物：`native\out\SpaceSniffer.exe`（GUI）、`native\out\SpaceSniffer.Mcp.exe`（控制台）。
构建脚本已经写好，绝不要改它的链接库列表之外的东西；如需新增源文件，直接在 `native/src/` 里加（脚本用通配符收集 `src/*.cpp`，`mcp` 子目录单独一套）。

## 已有资产（必须复用，不要重新发明）
- `docs/ANALYSIS.md` — 完整逆向报告：单元/类清单、界面结构、过滤器 DSL、导出模板 DSL、
  配置 XML 结构、命令行、`.sns` 二进制格式、关键函数地址。
- `src/SpaceSniffer.Core/` — 我先写的 C# 参考实现（**只作为算法参考**，不要调用、不要引用）：
  - `Scan/FastScanner.cs` — 并行 `FindFirstFileExW`(LARGE_FETCH) 扫描、簇对齐磁盘大小、ADS 枚举、伪节点
  - `Scan/MftScanner.cs` — $MFT 直读：卷句柄 → FSCTL_GET_NTFS_VOLUME_DATA → 记录0 的 $DATA runlist
    → 顺序读整条 MFT → 解析 FILE 记录（$STANDARD_INFORMATION/$FILE_NAME/$DATA）→ 按父引用建树
  - `Treemap/TreemapLayout.cs` — squarified 布局（已验证的数学实现，可直接逐行翻译）
  - `Filtering/FilterEngine.cs` — 过滤器解析/求值（语法见 ANALYSIS.md §4.3）
  - `Exporting/ExportEngine.cs` — 导出模板解释器（`{script}` / `if X ? {a} : {b}` / `leftpad` / `<%tag%>` / `&br`）
  - `Config/AppConfig.cs` — `SpaceSnifferConfig.xml` 读写（元素名必须与 ANALYSIS.md §4.6 一致）
  - `Snapshot/SnapshotIO.cs` — `.sns` 读写、`.ssx`、`.du`
- `work/dfm/*.dfm` — 8 个窗体的文本 DFM（原版 UI 的权威布局来源：位置、尺寸、控件层级、快捷键）。

## 目录结构（请按此组织）
```
native/
  build.ps1                 (已存在，勿破坏)
  src/
    main.cpp                (已存在骨架：窗口、DPI、深色标题栏、菜单/工具栏/状态栏/标签页占位)
    app.h / app.cpp         (应用状态：配置、视图列表、日志)
    ui_theme.h/.cpp         深色调色板、DPI 缩放辅助、owner-draw 绘制
    ui_shell.cpp            菜单、工具栏、状态栏、标签页、开始页
    treemap.h/.cpp          squarified 布局 + GDI 双缓冲绘制 + 命中测试 + tooltip + 缩放动画
    core/model.h            节点树（节点池，避免每节点一次 malloc）
    core/scanner.h/.cpp     并行目录枚举 + MFT 直读（含自动回退）
    core/filter.h/.cpp      过滤器
    core/export.h/.cpp      导出模板
    core/config.h/.cpp      XML 配置
    core/snapshot.h/.cpp    .sns/.ssx/.du
    core/util.h/.cpp        大小/日期格式化、宽字符工具
  mcp/
    mcp_main.cpp            SpaceSniffer.Mcp.exe：stdio JSON-RPC(MCP) + 直接调用 core 扫描
  out/                      构建产物
```

## 各阶段验收标准

### A. 外壳（先做这个，能跑起来截图）
- 主窗口 1240x800，菜单栏（文件/编辑/视图/窗口/帮助）、工具栏（自绘按钮+悬停/按下态）、
  状态栏（左状态文本 + 右统计）、标签页条（开始页 + 每个扫描视图一页，可关闭）。
- 深色主题：背景 #1E1E1E、面板 #252526、边框 #3F3F46、文本 #E6E6E6、强调 #4C9AFF；
  标题栏深色用 `DwmSetWindowAttribute(hwnd, 20, ...)`；浅色主题可切换，配置里持久化。
- 高 DPI：`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`；所有尺寸走 `S(x)=x*dpi/96`；
  字体按 pt 创建并随 DPI 重建；`WM_DPICHANGED` 时重排。
- 开始页：驱动器列表（卷标/文件系统/可用/总量，双击扫描）、路径输入框 + 浏览 + 开始按钮、
  快照列表（.sns/.ssx/.du，双击打开）。

### B. 扫描 + 树图
- 并行扫描（线程数 = min(32, cores*2)），进度实时反馈到状态栏与视图内进度标签。
- 文件节点同时记录逻辑大小与磁盘大小（簇对齐；压缩/稀疏用 `GetCompressedFileSizeW`）。
- 卷根扫描时追加 “Free Space” 与 “Unknown (not yet scanned) Space” 伪节点。
- 树图：squarified，按大小降序；最小元素尺寸、初始层级、比例可配置；
  层级亮度渐变；边框对比度；悬停高亮 + 白边；选中用强调色描边；
  自绘 tooltip（路径、磁盘/逻辑大小、修改时间、年龄）；双击放大；标签角标。
- 导航：后退/前进/上层/回到根；面包屑路径显示在视图顶部。

### C. NTFS $MFT 直读
- 管理员权限下自动走 MFT（比目录枚举快 5–20 倍），非管理员/非 NTFS 自动回退并记录日志。
- 解析：`FILE` 记录、fixup、`$STANDARD_INFORMATION`、`$FILE_NAME`(优先 Win32 命名空间)、
  未命名 `$DATA` 的 AllocatedSize/RealSize；稀疏/压缩按实际占用。
- 用 `native\bench` 之类的方式对比两种模式耗时（命令行 `sscli` 可选做）。

### D. 功能面
- 过滤器：完整 DSL（见 ANALYSIS.md §4.3），回车应用；应用后重新投影树并显示命中占比。
- 导出：`File report` / `Summary` / `Simple file list` / `First level` / `CSV` / 快照；
  模板编辑器 + 预览 + 导出。
- 配置对话框：几何/动画/行为/外观/配色/文件类六页，字段对应配置 XML。
- 快照：写 `.ssx`（自有，gzip+二进制）与 `.du`；读 `.sns`(尽力)/`.ssx`/`.du`。
- 命令行：`scan/load/filter/export/autoclose/help`，与 ANALYSIS.md §4.7 一致。
- 标签：Ctrl+1..4 着色、Ctrl+0 清除；标签参与 `:tag:` 过滤。
- 日志窗口：带时间戳、级别、清空、不再自动弹出。

### E. MCP 服务器
`SpaceSniffer.Mcp.exe`（控制台，stdio JSON-RPC 2.0），工具：
`scan`, `list_children`, `largest_files`, `largest_folders`, `search`, `tree`,
`file_types`, `duplicates`, `old_files`, `cleanup_candidates`, `volume_info`, `export_report`。
- 只读分析，不删除任何文件；`cleanup_candidates` 只给“建议 + 风险等级”。
- 每个工具的 inputSchema 用 JSON Schema；结果返回结构化 JSON（含 size/disk_size/路径）。

## 质量要求
- 无明显内存泄漏（节点池/RAII）；百万节点量级下内存 < 300 MB。
- 1M 文件的磁盘（fast 模式）应在 ~5 s 量级完成（SSD）；MFT 模式 ~1–3 s。
- 关闭窗口不崩溃；扫描中可取消。
- 界面不能出现：控件重叠、文字被裁切、滚动条错位、深色模式下白底残留。
- 每个阶段结束都要能 `build.ps1` 编译通过，并实际运行截图自查。

## 交付
每阶段完成后在回复里说明：改了哪些文件、如何验证、已知限制。
