# SpaceSniffer 1.3.0.2 逆向分析报告

> 目标文件：`%USERPROFILE%\bin\SpaceSniffer.exe`
> SHA256：`FC0629D450F8A57BC93E1BA1CDEF0BFF49C1A4CF0725C2A1F52116FD67D9FE8E`
> MD5：`B310E7335EAE66A533E985B377E81612`
> 时间戳：2016-10-02 08:57:01 UTC　文件版本：1.3.0.2　大小：2,309,632 字节
> 分析日期：2026-11-25　分析方式：纯静态（Ghidra 12.2 全量反编译 7590 函数 + 自研 Delphi RTTI/DFM 解析器）+ 动态验证（命令行导出）

> **勘误（2026-11-25，由补丁路线实测反馈）**：本报告初版有三处不准确，已在正文修正：
> ① 原版**带 manifest**（两条 RT_MANIFEST，均无 dpiAware）；
> ② `FindFirstFileExW` 原版**已是** `FindExInfoBasic + LARGE_FETCH`，不是"标准枚举"；
> ③ `0x004386C0` 是 VA（RVA `0x386C0`）。
> 详见 `docs/PATCH_ROUTE.md` §1。

---

## 1. 二进制指纹

| 项目 | 值 | 说明 |
|---|---|---|
| 机器类型 | `0x014C` (i386) | 32 位 PE |
| 子系统 | 2 (GUI) | 无控制台 |
| ImageBase | `0x400000` | 未启用 ASLR / DEP（`DllCharacteristics = 0`） |
| EntryPoint | `0x187C` | Delphi `_start` → `WinMain` |
| SizeOfImage | `0x26C000` | |
| 编译器 | **Delphi 2010 (Unicode RTL)** | `DVCLAL = A2 8C DF 98 7B 3C 3A 79 26 71 3F 09 0F 2A 25 17`；`.data` 内字符串 `Embarcadero RAD Studio - Copyright 2009` |
| 编译器置 | 有 `TLS` 目录、`.tls`/`.didata`/`.edata` 节、`__GetExceptDLLinfo` | 典型 Delphi/Borland 布局 |
| 清单(manifest) | **有**，两条 `RT_MANIFEST`：`ID1/LANG1033`(752 B, DelphiApplication) 与 `ID1/LANG1040`(850 B, CodeGear RAD Studio) | 均声明 comctl32 v6 + `asInvoker`，但**都没有 dpiAware**——这是高 DPI 模糊的根因 |
| 链接器特征 | 34 个 `@@<unit>_source@Initialize/Finalize` 导出 | **单元名完整保留**——逆向的巨大捷径 |

节表：

```
.text    VA=0x001000  VSize=0x1B9000  RawSize=0x1B8400  entropy 6.49   ← 全部代码
.data    VA=0x1BA000  VSize=0x05A000  RawSize=0x027E00  entropy 4.98   ← 字符串 + VMT + RTTI
.tls     VA=0x214000  0x1000
.rdata   VA=0x215000  0x1000
.idata   VA=0x216000  0x4000
.didata  VA=0x21A000  0x1000
.edata   VA=0x21B000  0x1000（导出表）
.rsrc    VA=0x21C000  0x2F000（图标/光标/字符串表/DFM/DVCLAL）
.reloc   VA=0x24B000  0x21000
```

未加壳、未混淆；字符串基本明文（仅个别被拆成多段拼接）。

---

## 2. 源码结构还原（单元 → 类）

导出表直接给出了作者的全部工程单元（`.pas`）。单元名后缀 `_source` 说明该构建是由**重新组织的源码树**编译的（作者构建时对单元做了别名/重命名）。

| 单元 | 承载的类（RTTI 复原） | 职责 |
|---|---|---|
| `Filesystemnode` | `TFileSystemNode`(抽象基类) | 节点基类：类型谓词（IsFile/IsFolder/IsContainer…）、大小、属性、时间、导出/导入虚方法 |
| `Filenode` | `TFileNode` | 文件节点 |
| `Foldernode` | `TFolderNode` | 文件夹节点 |
| `Drivenode` | `TDriveNode` | 驱动器根节点（含卷容量/剩余空间） |
| `Freespacenode` | `TFreeSpaceNode` | “Free Space”伪节点（卷剩余空间） |
| `Unknownspacenode` | `TUnknownSpaceNode` | “Unknown (not yet scanned) Space”伪节点 |
| `Missingdatanode` | `TMissingDataNode` | 无权限/无法访问的空间 |
| `Array` | `TArray<T>` 容器 | 动态数组封装 |
| `Datacache` | 缓存池 | 节点对象池 / 内存复用 |
| `Filter` | `TFilter` | 过滤器表达式解析与求值 |
| `Ntfs` | `TNtfs` | 卷信息（簇大小、剩余空间、NTFS 判定、`GetCompressedFileSizeW`） |
| `Shell` | `TShell` | ShellExecute / SHBrowseForFolder / 剪贴板 / 打开资源管理器 |
| `Utils` | — | 格式化（大小/日期/时间/年龄）、路径处理、Base64、字符串 |
| `Basethread` | `TBaseThread` | 线程基类（含 WOW64 重定向处理、`SeBackupPrivilege` 提权） |
| `Snifthread` | `TSnifThread` | **主扫描线程**（`FindFirstFileExW` 递归） |
| `Filterthread` | `TFilterThread` | 过滤器异步计算线程 |
| `Watchthread` | `TWatchThread` | 文件系统变更监视（`ReadDirectoryChangesW`） |
| `Treemapnode` | `TTreemapNode` | 树图布局基类（矩形分割/排序/最小元素尺寸） |
| `Filetreemapnode` / `Foldertreemapnode` / `Drivetreemapnode` / `Freespacetreemapnode` / `Missingdatatreemapnode` / `Unknownspacetreemapnode` | 对应节点 | 各节点类型的绘制/布局特化 |
| `Tooltip` | `TTooltip`(748B) | 自绘悬浮提示窗（非系统 tooltip） |
| `Frmmain` | `TfrmMain`(1244B, 30 published methods) | MDI 主窗口、菜单、工具栏、状态栏、动作表 |
| `Frmstart` | `TfrmStart` | “Choose your Space” 启动窗（驱动器列表 / 快照拖放 / 路径输入） |
| `Frmview` | `TfrmView`(1124B, 31 methods) | **MDI 子窗口 = 一次扫描/一个视图**（树图 + 过滤条 + 进度） |
| `Frmconfig` | `TfrmConfig`(1332B, 20 methods) | 配置对话框（Geometry/Animation/Behavior/Look/Styles/File Classes） |
| `Frmexport` | `TfrmExport`(1428B, 12 methods) | 导出配置编辑器 + 预览 |
| `Frmconsole` | `TfrmConsole` | 日志控制台（含 Hints 面板） |
| `Frmhelp` / `Frmabout` | `TfrmHelp` / `TfrmAbout` | 帮助（只读等宽 Memo）/ 关于 |

**第三方库**

| 库 | 证据 | 用途 |
|---|---|---|
| **Graphics32 (GR32)** | `D:\Sorgenti\BCBPlugins\Graphics32\GR32_Resamplers.pas`、`GR32_Image.pas`、`GR32_Layers.pas`、`GR32_MicroTiles`、`TPaintBox32`+`RepaintMode=rmOptimizer`+`OnPaintBuffer`、`TCustomPanningWindow`、`TMemoryBackend`、`TShadowBitmap` | 树图渲染后端（32bpp 位图缓冲 + 快速重绘） |
| **PngComponents / PNG Delphi** | `TPngImageList`、`TPNGObject` | UI 图标（PNG 按钮图） |
| **Virtual Treeview 痕迹** | `Theme manager … 2001-2006 Mike Lischke`、`BDSUnthemedDesigner` | 配置页控件主题（第三方 VCL 主题管理器） |
| VCL | `TXPManifest`（XP 主题清单）、`TActionList`、`TWindowClose/TileHorizontal/...` 标准动作 | |

**类层次（RTTI 复原的 VMT 布局，Delphi 2010 Win32）**

```
vmtSelfPtr -76 | vmtIntfTable -72 | vmtAutoTable -68 | vmtInitTable -64
vmtTypeInfo -60 | vmtFieldTable -56 | vmtMethodTable -52 | vmtDynamicTable -48
vmtClassName -44 | vmtInstanceSize -40 | vmtParent -36
vmtEquals -32 | vmtGetHashCode -28 | vmtToString -24
vmtSafeCallException -20 | vmtAfterConstruction -16 | vmtBeforeDestruction -12
vmtDispatch -8 | vmtDefaultHandler -4 | vmtNewInstance +0 | vmtFreeInstance +4 | vmtDestroy +8
```

本构建的 `TMethodTable` 布局（与教科书版不同，实测）：

```
Word Count;  Count × { Word idx; DWORD address; ShortString name }
```

据此复原出 117 个 published 方法 → 代码地址的映射（`work/dump/methods.tsv`），其中表单类：

* `TfrmMain`(30) vmt=0x1C6AD8, instanceSize=1244
* `TfrmView`(31) vmt=0x1C9BA4, instanceSize=1124
* `TfrmExport`(12) vmt=0x1C3EE4, instanceSize=1428
* `TfrmConfig`(20) vmt=0x1BE054, instanceSize=1332
* `TfrmStart`(12) vmt=0x1C83E4, instanceSize=972
* `TfrmConsole`(4) vmt=0x1BEF80, instanceSize=972
* `TfrmAbout`(2) vmt=0x1BBFA4

非持久类（无 published 方法、但有 VMT）：`TSnifThread`(100B)、`TFilterThread`(108B)、`TWatchThread`(92B)、`TBaseThread`(76B)、`TTooltip`(748B)、`TShadowBitmap`(244B)、`TTagBitmap`(244B)。

---

## 3. 界面结构（DFM 二进制 → 文本，100% 还原）

自研 `tools/dfm.py` 实现了 `TReader` 的完整语义（`[prefix] str(class) str(name) prop* 0 obj* 0`、`vaList/vaCollection/vaSet/vaBinary/vaExtended` 等），已把 8 个窗体资源完整反编译为 `work/dfm/*.dfm`。

### 3.1 主窗口 `frmMain`（`FormStyle = fsMDIForm`，MDI 容器）

```
frmMain
├─ status: TStatusBar                    (1 panel，显示状态文本/进度)
├─ ToolBar1: TToolBar (AutoSize, ButtonHeight=38)
│  btnNew(actShowStartWindow) | sep | btnGoBackward(actGoBack) btnGoForward(actGoForward)
│  btnZoomOut(actZoomOut) btnGoHome(actGoHome) | sep
│  btnRefresh(actRefresh) btnFocusRefresh(actFocusRefresh) | sep
│  btnMoreDetail(actLessDetail) btnLessDetail(actMoreDetail) | sep
│  btnShowFileGroups(actShowFileGroups, tbsCheck) btnShowFreeSpace(actShowFreeSpace, tbsCheck)
│  btnShowUnknownSpace(actShowUnknownSpace, tbsCheck) | sep | btnDonate(actDonate)
└─ mnuMain: TMainMenu
   ├─ File : &New(Ctrl+N) | --- | Export to file | --- | E&xit
   ├─ Edit : Configure
   ├─ Windows : Show Log Console | --- | Tile H / Tile V / Cascade / Minimize All / Close | ---（MDI 窗口列表）
   └─ Help : Filtering help | Tagging help | Command line help | --- | Check for update | Support this project! | About
```

动作表 `actViewActions`（含 Hint = 快捷键说明）：

| Action | Caption | Hint（原文） |
|---|---|---|
| `actRefresh` | — | Refresh current view |
| `actGoHome` | — | Go to home (Ctrl+Home) |
| `actZoomOut` | — | Go to upper level (Ctrl+Up) |
| `actLessDetail` | — | Less detail (Ctrl '-') |
| `actMoreDetail` | — | More detail (Ctrl '+') |
| `actShowFreeSpace` | — | Show free space (Ctrl+F) |
| `actShowUnknownSpace` | — | Show unknown space (Ctrl+U) |
| `actGoBack` | Go back | Go back (Backspace) |
| `actGoForward` | Go forward | Go forward (Shift+Backspace) |
| `actShowStartWindow` | &New | New view，**ShortCut=16462 (Ctrl+N)** |
| `actFocusRefresh` | — | Refresh zoomed view |
| `actExit` | E&xit | |
| `actExport` | Export to file | Export current view focus to file |
| `actShowFileGroups` | S&tyle | Use File Classes style (Ctrl+T) |
| `actAbout` / `actDonate` / `actTaggingHelp` / `actFilteringHelp` / `actCommandLineHelp` | | |

其它：`activeViewTimer`、`statusTimer`、`startTimer`（三个 TTimer 驱动状态刷新与视图切换）、`imgViewActions: TPngImageList`。

### 3.2 视图 `frmView`（`fsMDIChild`，一次扫描 = 一个 MDI 子窗）

```
frmView (KeyPreview, OnShortCut, OnKeyDown/Up/Resize)
├─ pnlGraph: TPaintBox32        ← 树图绘制区（Graphics32 32bpp 缓冲，rmOptimizer）
│    events: OnPaintBuffer / OnMouseDown / OnMouseMove / OnMouseLeave / OnDblClick
├─ pnlFilter: TPanel (alTop, 高 25)
│  ├─ pnlFilterHeaderLeft : Label "   Filter " + edtFilter(Edit) + btnFilter(&Filter)
│  └─ pnlFilterHeaderRight: lblProgress(':::progress:::', 9pt) + ToolButton(停止) + ToolButton(重扫)
```

状态文本（运行时字符串）：`Ready`、`Master status:`、`(ADS scan active)`、`Stop current master scan`、`Perform a new master scan`、`Stop current zoomed view scan`、`To scan entire view use master scan`、`Perform a new scan of the zoomed view`。

### 3.3 启动窗 `frmStart`（`Choose your Space`）

```
frmStart
├─ tabs: TPageControl
│  ├─ tabFileSystem "Drives or Paths" : lstDrives(TListView, Hint='Choose a drive to scan')
│  └─ tabFileDump   "Snapshots"       : Label 'Drop here a snapshot, or choose one'
├─ pnlBottom: btnStart "Start" | btnPath "&Path:" | path(TEdit, ';' 分隔多路径，可拖放)
├─ imgDrives: TPngImageList(7 icons)  ← 驱动器类型图标
├─ drivesTimer / bringerTimer / dlgOpenFile
└─ dlgOpenFile filter: '2SpaceSniffer Snapshot|*.sns|Disk Usage output|*.du'
```

### 3.4 配置 `frmConfig`（`SpaceSniffer Configuration`）

页签：**Geometry / Animation / Behavior / Look / Styles / Flat Colors / File Classes**

| 控件 | 范围/默认 | 含义 |
|---|---|---|
| `trkFontSize` | 4..20, pos=4 | 元素字体大小 |
| `trkMinElementSize` | 3..20, pos=10 | 最小元素尺寸(px) |
| `trkProportions` | -60..60 | 元素比例（Tall↔Wide） |
| `trkInitialDetailLevel` | 1..15, pos=1 | 初始嵌套层级 |
| `trkRefreshDelay` | 1..100, pos=50 | 刷新间隔 ms（越小越流畅） |
| `trkZoomTime` | pos=5 | 缩放动画时长 |
| `trkLevelContrast` | 0..50, pos=10, freq=5 | 层级亮度对比（Flat↔Deep） |
| `trkBorderContrast` | 0..50, pos=10, freq=5 | 层级边框对比（Smooth↔Hard） |
| `trkHilightHaloLevels` | pos=3 | 高亮光晕层数 |
| `chkShowFreeSpace` | True | 新视图默认显示剩余空间 |
| `chkShowUnknownSpace` | True | 新视图默认显示未知空间 |
| `chkUseSorting` | True | 按大小排序元素 |
| `chkRealZoom` | | DynaZoom（缩放时实时重排） |
| `chkDynaSmooth` | | DynaSmooth（按交互调节帧率） |
| `chkADS` | | 扫描 NTFS 备用数据流（慢） |
| `chkWatch` | | 文件系统变更检测（需重启） |
| `chkLogPopup/LogWarning/LogDebug/LogDebugFine/LogIssue` | | 日志级别与自动弹出 |
| `chkFlashWindow` | | 扫描结束时隐藏则闪烁任务栏 |
| `chkKeepTooltip` / `chkShowFileTooltipDateTime` / `chkShowFileTooltipAge` | | 悬浮窗行为 |
| `chkMouseHover` / `chkMouseTrail` / `chkHilightShadow` | | 鼠标悬停高亮/轨迹/阴影 |
| `cbxDefaultView` | 'Flat Colors' | 默认视图风格 |
| 颜色面板 | `clrDrive/clrFolder/clrFile/clrFree/clrUnknown/clrGroup` + `TColorDialog` | 扁平配色 & 文件类配色 |

### 3.5 导出 `frmExport`（`SpaceSniffer - Export current view focus`）

左侧 `edtConfig`（多行模板编辑器）+ 右侧 `edtPreview`（`Preview (may show partial data)` / `This is an hard coded binary configuration, edit and preview not available`）+ `progress`。
工具栏：`Export` / `SaveConfig` / `DeleteConfig` / `ImportConfig` / `ExportConfig` / `Config`。

### 3.6 日志 `frmConsole`（`SpaceSniffer Log Console`）

`edtLog: TMemo` + `pnlHints{ lblHints 'Hints', edtHints: TMemo }` + `splitter` + `btnClear(actCut 'Cut log')` + `btnDismiss`（关闭后不再自动弹出）。

---

## 4. 核心功能与算法

### 4.1 扫描引擎（`TSnifThread`，`Snifthread.pas`）

**入口与 API 证据**（导入表 + 反编译交叉验证；补丁路线的动态实验已交叉确认）

* `FindFirstFileExW` / `FindNextFileW` / `FindClose` —— 目录枚举主循环（唯一调用者 `0x004386C0`，反编译 23 KB）
* `FindFirstFileExW` **已经**使用 `FindExInfoBasic(1)` + `FIND_FIRST_EX_LARGE_FETCH(2)`（两个调用点，由 `[0x5E9D39]` 的 Win7+ 判定门控）——即原版的目录枚举参数已是 Windows 上的最优组合，补丁路线无法靠改这两个参数再提速（实测改回 0/0 后 `C:\Windows` 扫描 CPU +23%）。
* `GetCompressedFileSizeW`（唯一调用者 `0x00402ED0`）—— 压缩/稀疏文件的**实际占用**；失败时写日志 `Unable to get compressed file size for <path>`
* `GetFileSizeEx` —— 逻辑大小
* `GetDiskFreeSpaceExW` / `GetDiskFreeSpaceW`（`0x00429B00`）—— 卷容量/簇大小/剩余空间
* `GetVolumeInformationW`、`GetDriveTypeA` —— 卷类型/文件系统（NTFS 判定）
* `CreateFileW` + `::$DATA`（`0x004058DC`、`0x00432594`）—— **NTFS 备用数据流(ADS)** 枚举，配置项 `scanADS` 控制
* `ReadDirectoryChangesW`（`0x0043DCB4`）—— `TWatchThread` 实时变更监视
* `Wow64DisableWow64FsRedirection` / `Wow64RevertWow64FsRedirection` —— 64 位系统上的 FS 重定向处理
* `LookupPrivilegeValueA`/`AdjustTokenPrivileges` + `SeBackupPrivilege` —— 尝试提权以读取受限目录，失败时日志给出 `HINT: Try running SpaceSniffer as Administrator`

**大小模型**

* 每个文件记录两个量：**逻辑大小**（`GetFileSizeEx`）与**磁盘大小**（`GetCompressedFileSizeW`，未压缩时 = 按簇向上取整）。
  `.sns` 中文件记录只存 `size:u64` + `slack:u32`（= 磁盘大小 − 逻辑大小），验证：1 字节→4095、1000 字节→3096、5000 字节→3192（4 KiB 簇）。
* 文件夹大小 = 子树聚合；同时聚合“未知空间”（父大小 − 已扫描子项和）与“无法访问空间”。
* 卷根额外生成两个伪节点：`Free Space`（`GetDiskFreeSpaceExW` 可用字节）与 `Unknown (not yet scanned) Space`。

**线程模型**：`TBaseThread` 派生 `TSnifThread`（扫描）、`TFilterThread`（过滤）、`TWatchThread`（监视）。扫描线程与 UI 通过 `Synchronize`/消息队列 + 定时器刷新（`refreshDelayMs` 默认 50ms）解耦；支持“master scan”（整盘）与“zoomed view scan”（当前视图焦点重扫：`actFocusRefresh`）。

**进度模型**：状态栏 1 panel + 视图内 `lblProgress`（`:::progress:::` 占位符），扫描中可“Stop”，停止后剩余区域显示为 `Unknown (not yet scanned) Space`。

### 4.2 树图（treemap）渲染

* 后端 Graphics32 `TPaintBox32`（32bpp DIB，`rmOptimizer` 重绘调度，`OnPaintBuffer` 拿到底层位图直接绘制）。
* 布局：`TTreemapNode` 体系按“父矩形 → 子矩形”递归分割；子项**按大小降序**（`useSorting`）后做**squarified** 分割（保持接近正方形的长宽比），再由 `minElementSizePx` 决定是否继续下钻，`initialDetailLevel` 决定初始展开层数。
* 视觉：按层级调整亮度（`levelContrast`：Flat↔Deep）、边框对比（`borderContrast`：Smooth↔Hard）、高亮光晕（`hilightHaloLevels`）、阴影（`hilightShadow`）、鼠标悬停/轨迹淡出。
* 配色：`driveColor/folderColor/fileColor/freeColor/unknownColor` + 按扩展名分组的 `fileGroup.color`；`actShowFileGroups`(Ctrl+T) 切换“文件类”着色。
* 交互：单击选中、双击放大进入、`Backspace` 后退、`Ctrl+Up` 上升一层、`Ctrl+Home` 回到根、`Ctrl+/-` 改变细节层级、`Ctrl+F`/`Ctrl+U` 切换剩余/未知空间、`Ctrl+1..4` 打标签、`Ctrl+0` 清除。
* 悬浮提示自绘 `TTooltip`：路径、大小（磁盘/逻辑）、修改时间、年龄（可配置）。

### 4.3 过滤器 DSL（`TFilter`，**原文规格**）

分号 `;` 分隔多个条件；每个条件以 `|` 开头表示**取反**。

| 维度 | 语法 | 说明 |
|---|---|---|
| 文件名 | `*.jpg` / `?abc*` | 通配符掩码 |
| 大小 | `[<\|>] N [b\|kb\|mb\|gb\|tb]` | 默认按**磁盘大小**；前缀限定 `disksize\|clustersize\|filesize\|logicalsize\|size`（disk/cluster 同义，file/logical/size 同义）；例：`s>1mb`、`d<16k` |
| 年龄 | `[<\|>] N [seconds\|minutes\|hours\|days\|weeks\|months\|years]` | 默认按**修改时间**；前缀 `create\|access\|modify`；例：`c>1month`、`m<1day`、`m<10s`（实时文件） |
| 标签 | `:tag:[red\|yellow\|green\|blue\|all]`、`:red` | 支持 `+`/`-` 组合，如 `:tag:red+yellow`、`:attr:a+h-s` |
| 属性 | `:attr:[archive\|system\|readonly\|hidden\|compressed\|encrypted\|offline\|temporary\|notindexed\|sparse\|alternatedatastream]` | 短写 `a\|s\|r\|h\|c\|e\|o\|t\|ni\|sp\|ads`；`+`/`-` 组合，逗号亦可 |
| 文件类 | `:class:Audio/Music` | 类名来自配置（允许空格，无需引号） |
| 文件夹名 | `\temp`、`\progr*` | 反斜杠前缀；匹配**路径中任意层级**的目录名；命中的目录用**粗体**渲染；比文件掩码慢 |

示例（原文）：`*.jpg;>100kb;>2weeks`、`:red;:yellow`、`:tag:r+y`、`|*.exe;|*.dll`、`|*.exe;|*.dll;\*temp*;|\*windows*`、`:attr:a+h-s`、`|:attr:a`、`:attr:-a`

### 4.4 导出模板 DSL（`frmExport` / 41 KB 的解释器 `0x00416BC0`）

```
{ ... }                       脚本块（{script} 起始标记）
if <cond> ? { ... } : { ... } 条件表达式（可嵌套）
leftpad{value}{padchar}{width} / rightpad{...}
&br  &curlyop &curlycl &angleop &anglecl    转义
<%tag%>                       占位符
```

占位符全集：

```
filename fileext filecreatedate filecreatetime filecreateage
filemodifydate filemodifytime filemodifyage
fileaccessdate fileaccesstime fileaccessage
pathfile sizebytes disksize disksizebytes
isfile isfolder iscontainer
count localcount filecount localfilecount foldercount localfoldercount
containercount localcontainercount
containsfiles containsfolders containscontainers
nestinglevel
```

内置导出配置（fields: `version, description, preferredFileName, preferredFileExtension, headerFormat, detailFormat, footerFormat, blockSorting, fineSorting, fineSortingDirection`）：

* **SpaceSniffer binary snapshot**（`*.sns`，二进制快照）
* **SpaceSniffer snapshot**（文本快照，`{script}` 模板，含 `Disk Usage output (only folders)`）
* **File report** / **Grouped by folder** / **First level folders** / **First level**
* **Simple file list** / **File list** / **Summary**

最终导出配置以 **UTF-8 XML**（根 `exportConfig`）保存/导入，含 `description/preferredFileName/...` + 模板文本；内置配置为“硬编码”，需先 `SaveConfig` 落地才能编辑/删除。

### 4.5 快照格式 `.sns`（二进制，实测解析）

由 `SpaceSniffer binary snapshot` 导出得到真实样本（`work/out/test2.sns`，776 B），结构：

```
记录流（无全局魔数，逐条记录）：
  [0x02][type:u8][nameLen:u32][name: base64][payload]
  type: 2 = File, 3 = Folder（另有 Drive/Free/Unknown/Missing 类型值）
  root 记录的 name 内含 CRLF 分隔的两段 base64：上级路径 + 节点名
  File   payload(46B): size:u64 | slack:u32 | attributes:u32 | ctime:u64 | mtime:u64 | atime:u64 | u16 u16 u16
  Folder payload(48B): size:u64 | 聚合量:u64 | 32B 保留(0)
  结尾：00 00 01 00 01 00（结束标记）
```

* `slack = 磁盘大小 − 逻辑大小`（未压缩时即簇内碎片）；实测 1 B→4095、1000 B→3096、5000 B→3192（4096 B 簇）完全吻合。
* 校验值：`Bad file format`、`Unexpected end of file`、`Bad structure (parent node must be a drive)`、`Unrecognized file system node type on import/export`；版本字段读到 1 或 2 才接受，否则 `Bad file format`。
* 另支持导入 **`.du`（Disk Usage / WinDirStat 文本格式）**。
* 本报告的解析实现见 `tools/sns_parse3.py`；`.sns` 的“目录第二个聚合量”语义未完全确定（在 4 KiB 簇下不等于磁盘大小之和，疑为历史遗留的近似聚合），重写版采用 `size + slack` 精确重算。

### 4.6 配置文件 `SpaceSnifferConfig.xml`（UTF-8 XML）

```
<SpaceSnifferConfig>
  refreshDelayMs zoomTimeMs minElementSizePx showFreeSpace showUnknownSpace initialStyle
  showFileTooltipDateTime showFileTooltipAge useRealZoom useDynaSmooth useSorting
  scanADS watchDog logPopup logWarning logDebug logDebugFine logIssue
  flashWindow keepTooltip proportions initialDetailLevel fontSize
  <colors> driveColor folderColor fileColor freeColor unknownColor
           levelContrast borderContrast mouseHover mouseTrail hilightShadow hilightHaloLevels </colors>
  <exportConfigs> <exportConfig>…</exportConfig> </exportConfigs>
  <fileGroups> <fileGroup><name/><extensions/><color/></fileGroup> </fileGroups>
</SpaceSnifferConfig>
```

默认文件类（原文）：

| 名称 | 扩展名 | 
|---|---|
| Video/Movies | `qt;mov;mpeg;mpg;mpe;m1s;mp4;avi;mts;vob;wmv;rm;3gp;mkv` |
| Executables/System files | `exe;dll;sys;ini;msi;log` |
| Archives/Disk images | `zip;7zip;rar;iso` |
| Pictures/Images | `jpg;jpeg;gif;png;tif;tiff;nef;raw;rw2;cr2;bmp;psd` |
| Audio/Music | `mp2;mp3;wav;ogg;aif;alp;au;3gp;ac3;amr;aac;m4a;wma;aiff;flac;m4a;voc` |

### 4.7 命令行（`showCommandLineHelp` @ `0x0043A7A0`）

```
scan   "<path>"            打开新扫描视图；可多个、可用 ';' 分隔多路径
load   "<file>"            打开快照视图（.sns/.du）；同上
filter "<filter>"          对"前一个" scan/load 应用过滤器
export "<name>" "<path>"   对"前一个" scan/load 应用命名导出配置（区分大小写）
autoclose                  所有自动导出完成后自动退出
help                       显示帮助
```

* 单实例 + 进程间转发（`_requestedScans` 导出、`GlobalAddAtomW/GlobalFindAtomW`），内部规范化参数形如 `path:"…"`、`load:"…"`、`filter:"…"`、`exportName:"…"`、`exportPath:"…"`。
* 任何 `filter/stop` 等 UI 操作都会取消 auto-export / autoclose。
* 启动时检查 OS ≥ Windows 2000 SP4（`SpaceSniffer - OS Version Check`），有“测试构建”警告框（`This is a test build.`）。

### 4.8 其它行为

* **标签( tagging )**：Ctrl+1..4 = 红/黄/绿/蓝；Ctrl+0 清除当前焦点内所有标签；子元素继承父标签（继承的不渲染，避免视觉噪音，但参与 `:tag:` 过滤）。
* **查看方式**：Flat Colors（扁平配色）与 File Classes（按文件类着色）两种 style。
* **快照/日志/关于**：`SpaceSniffer Log Console`、`About SpaceSniffer`（`ver.` + `http://www.uderzo.it`）、`http://www.uderzo.it/main_products/space_sniffer/donate.html`。
* **更新检查**：`Check for update`（联网，本重写版不实现）。
* 主要字符串资源位于 `.data 0x1BA000–0x1D1000`，已导出 `work/dump/strings.txt`（26,991 条）。

---

## 5. 关键函数地址索引（供进一步研究）

| 地址(RVA) | 大小(反编译) | 说明 |
|---|---|---|
| `0x004386C0`(VA) / `0x386C0`(RVA) | 23,370 B | **目录扫描主循环**（`FindFirstFileExW` 两个调用点所在函数；无直接 `E8` 调用者，是线程/回调入口） |
| `0x00402ED0` | 3,087 B | 取文件大小（`GetCompressedFileSizeW`，压缩/稀疏处理） |
| `0x00431B0` / `0x0043245C` | 1.5 KB | 卷/路径辅助（`CreateFileW`） |
| `0x004058DC` / `0x00432594` | | ADS (`::$DATA`) 枚举 |
| `0x00406C60` | 6,976 B | **`.sns` 读取器**（含 `Bad file format` 等校验） |
| `0x0040681C` | | 节点导出分派（6 个虚谓词 → `Unrecognized file system node type on export`） |
| `0x00421810` | 3,765 B | 快照/DU 文本输出（`SpaceSniffer scan dump`） |
| `0x00401AD4` | | 生成 `Free Space` 节点 |
| `0x00429B00` | 2,059 B | `GetDiskFreeSpaceEx` 卷信息 |
| `0x0043DCB4` | 3,810 B | `TWatchThread`（`ReadDirectoryChangesW`） |
| `0x00416BC0` | 41,729 B | **导出模板解释器**（最大函数） |
| `0x0043A7A0` | | `showCommandLineHelp` |
| `0x0042D558` | | 过滤器解析（`clustersize/size` 等关键字） |
| `0x0041E6BC` | 14,744 B | `TfrmMain.actFilteringHelpExecute`（帮助文本构造） |

完整反编译语料：`work/decomp/`（7,590 个函数，`_index.tsv` 为地址索引）。

---

## 6. 复刻与现代化路线（本仓库实现）

| 目标 | 原版 | 重写版（本项目） |
|---|---|---|
| 语言/框架 | Delphi 2010 + VCL + Graphics32 | **C# / .NET 10 + WinForms 自绘**（`net10.0-windows`） |
| 高 DPI | 96 DPI 假设，`PixelsPerInch=96` | **PerMonitorV2**，全部尺寸按 DPI 缩放，字体按点值缩放 |
| 深色模式 | 无（仅 clBtnFace 系统色） | **原生深色主题**：自绘标题栏/菜单/工具栏/面板 + `DWMWA_USE_IMMERSIVE_DARK_MODE` |
| 扫描 | 单线程 `FindFirstFileExW` | **并行 `FindFirstFileExW`（LARGE_FETCH）+ NTFS `$MFT` 直读**（WizTree 路线） |
| 树图渲染 | Graphics32 位图缓冲 | 双缓冲 GDI+ 自绘 + squarified 布局 + 动画缩放 |
| 过滤器 | `TFilter` 语法 | 完整兼容同一 DSL（含 `|` 取反、属性/标签/文件类/文件夹名） |
| 导出 | 模板 DSL | 兼容同一 DSL + 内置配置 + UTF-8 XML 导入导出 |
| 快照 | `.sns`（二进制）+ `.du` | 读 `.sns`/`.du`；写 `.ssx`（文档化 JSON+zlib）+ `.sns` 兼容写出 |
| 配置 | `SpaceSnifferConfig.xml` | 同名同结构 XML |
| AI 分析 | 无 | **MCP 服务器**（stdio JSON-RPC）：`scan/list_children/largest_files/search/file_types/duplicates/cleanup_candidates/export_report` 等 |

### 高 DPI + 深色模式要点

1. `app.manifest` 声明 `PerMonitorV2`；`Application.SetHighDpiMode(HighDpiMode.PerMonitorV2)`。
2. 全部布局使用 `LogicalToDeviceUnits()`；自绘组件在 `DpiChanged` 时重算字号与边距（GDI+ 用 `Font(size * Dpi/96)`）。
3. 深色：统一 `Theme` 类提供调色板（背景 #1E1E1E / 面板 #252526 / 边框 #3F3F46 / 文本 #E6E6E6 / 强调色），自绘菜单与工具栏，`DwmSetWindowAttribute(hwnd, 20, &TRUE, 4)` 让标题栏跟随。

### NTFS `$MFT` 直读（加速核心）

1. `CreateFileW(\\.\C:, GENERIC_READ)` + `FSCTL_GET_NTFS_VOLUME_DATA` 取 `BytesPerCluster / BytesPerFileRecord / MftStartLcn / ClustersPerMftRecord`。
2. 依 `MftStartLcn` 从卷上读出 `$MFT` 的**记录 0**，解析其未命名 `$DATA` 的 runlist，得到 `$MFT` 全量数据流的簇分布。
3. 顺序读取整条 `$MFT` 流（大缓冲 8–16 MB，`FILE_FLAG_SEQUENTIAL_SCAN`），逐条解析 `FILE` 记录：`$STANDARD_INFORMATION`（时间/属性）、`$FILE_NAME`（父引用 + 名字 + 逻辑大小 + 分配大小）、未命名 `$DATA`（`AllocatedSize/RealSize`）。
4. 用父引用在内存里建树（`Dictionary<ulong, Node>`），HashMap 聚合尺寸，O(N) 一次性完成；1 M 文件通常 1–3 秒。
5. 需管理员权限；失败（非 NTFS/无权限/卷非固定盘）时自动回退到并行 `FindFirstFileExW` 扫描器。`SeBackupPrivilege` 以 `AdjustTokenPrivileges` 提权。

---

## 7. 复刻取舍与已知差异

* **不复刻**：自动更新检查、捐赠链接跳转（保留菜单项但只打开 URL）、MDI 子窗口（改为标签式视图，行为等价且更现代）。
* **未完全确定**：`.sns` 目录记录第二个聚合量的精确语义（见 4.5），重写版按 `size + slack` 精确重算，因此导入旧 `.sns` 后“磁盘大小”可能与原版显示略有差异（逻辑大小与树结构完全一致）。
* **Graphics32 的 `useDynaSmooth` 帧率调度**：重写版用 `refreshDelayMs` 作为节流，动画以固定 60fps 计时器驱动。
* 原版 PixelsPerInch=96 的固定布局在高 DPI 下会发虚/错位，重写版全部改用逻辑单位 + 缩放，这是本次“现代化适配”的主要收益。

---

## 附：本仓库分析工具

| 工具 | 作用 |
|---|---|
| `tools/extract_res.py` | 提取全部 PE 资源（图标/光标/字符串表/DFM/DVCLAL） |
| `tools/dfm.py` | **Delphi 二进制 DFM → 文本 DFM**（完整 `TReader` 语义） |
| `tools/dfm_tree.py` | 打印窗体控件树 + Caption/Hint/ShortCut |
| `tools/delphi_classes.py` | VMT/MethodTable 解析 → 类名/父类/实例大小/published 方法地址 |
| `tools/strings_pe.py` / `str_range.py` | 字符串提取与按区段导出 |
| `tools/ghidra_dump.py` / `ghidra_label2.py` | Ghidra（PyGhidra）符号注入 + 全量反编译导出 |
| `tools/dex.py` | 反编译语料索引与查询（按字符串/调用/地址） |
| `tools/sns_parse*.py` | `.sns` 快照格式解析实验 |
| `work/dfm/*.dfm` | 8 个窗体的文本 DFM |
| `work/decomp/` | 7,590 个函数反编译结果 |
| `work/dump/methods.tsv` | 117 个 `类.方法 → 地址` 映射 |
