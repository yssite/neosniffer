# 当前交付状态（2026-11-25）

## 已完成

### 1. 逆向分析（docs/ANALYSIS.md）
- PE/Delphi 2010 指纹、34 个导出单元 → 源码结构还原、117 个 `类.方法 → 地址`（RTTI/VMT 复原）
- 8 个窗体的二进制 DFM 全部反编译为文本（`work/dfm/*.dfm`）
- 过滤器 DSL、导出模板 DSL、`SpaceSnifferConfig.xml` 结构、命令行、`.sns` 二进制格式（用原版自导样本逐字段验证）
- Ghidra 全量反编译 7590 函数 + 关键函数地址索引

### 2. 原生重制版（native/，主线）
- `native/out/SpaceSniffer.exe`（约 0.94 MB，静态链接，无运行库依赖）
- `native/out/SpaceSniffer.Mcp.exe`（约 0.97 MB，stdio JSON-RPC，11 个工具）
- 能力：
  - 原生 Win32 自绘：深色/浅色主题、DWM 深色标题栏、owner-draw 菜单栏
  - PerMonitorV2 高 DPI（布局自愈：每次绘制按当前 client 重算）
  - 并行 `FindFirstFileExW`（`FindExInfoBasic` + `LARGE_FETCH`）+ NTFS `$MFT` 直读（自动回退）
  - squarified 树图：自适应下钻（默认 3 层，矩形够大继续下钻）、层级提亮、名字+大小双行标签、遮挡感知（不画被覆盖的半截字）、文件类着色、悬停/选中描边、标签角标
  - 过滤器（完整原版 DSL）、导出模板、快照（`.ssx`/`.sns`/`.du`）、标签 Ctrl+1..4、命令行 `scan/load/filter/export/autoclose`
  - 开始页（驱动器卡片列表/路径输入/快照列表）

实测：`C:\Windows` 163,772 文件 / 122,827 目录 / 48.0 GB，MCP `scan` 冷启动含扫描 1.41 s，进程内存约 70 MB。

### 3. .NET 对照版（src/ + dist/dotnet）
- 自包含单文件 49 MB，功能与算法参考实现，已跑通。

## 进行中

### 4. 原版二进制补丁线（已完成）
- `dist/patched/SpaceSniffer.Patched.exe`（2,504,192 B）：注入 PerMonitorV2 清单后
  `GetProcessDpiAwareness` 0→2、`GetDpiForWindow` 96→120；125% 下窗口尺寸不变、文字变清晰
- 同时产出 `SystemDpi` 变体、两份 `patch_report*.json`、`docs/PATCH_ROUTE.md`、截图对比
- **结论**：补丁路线只能解决"DIP 模糊"；扫描参数原版已最优（A/B 9 次无可测差异）；
  深色模式 / `$MFT` / MCP / 树图重排均不可行（依据见报告"不可行清单"）
- 顺带纠正了 ANALYSIS.md 初版三处事实错误（原版带 manifest、Find 参数已最优、RVA/VA 标注）

### 5. Gemini 渲染 / 对话框模块（已接入）
- `native/src/ui_treemap.{h,cpp}`（51 KB，Gemini 4 Neon 生成）：层级提亮公式、标签遮挡避让、
  卡片式 tooltip；已替换 `DrawView` 内原绘制段落
- `native/src/ui_config.{h,cpp}`（82 KB，Gemini 生成）：六分类自绘配置对话框
  （几何/动画/行为/外观/配色/文件类），滑杆拖拽、开关、颜色选择、文件类增删、Enter/Esc、模态循环
- 调用方式：`tools/gemini.py`（改编自用户 codex skill）—— `stdin=DEVNULL` + 有限 `--print-timeout`
  + `--settings` 权限声明；**关键教训**：单轮超过 4 分钟会被判超时，长任务要给足 20 分钟；
  带 workspace 的多轮工具回合在本机代理下容易卡住，改为"无工具单轮出代码 + 我方落盘接线"最稳
- 接线时补齐了原版配置项 `zoomTimeMs / useRealZoom / useDynaSmooth`（XML 读写已同步）

### 6. NTFS `$MFT` 解析器单元验证（无管理员权限）
- 把记录解析抽成 `ParseMftRecord()`（扫描器与测试共用同一份实现）
- `native/test/mfttest.cpp` 用**合成 FILE 记录**验证 20 项断言，全部通过：
  fixup、`$STANDARD_INFORMATION`、`$FILE_NAME`(UTF-16/命名空间)、父引用、逻辑/分配大小、
  压缩文件取 CompressedSize、元数据与空闲记录跳过、runlist 解码（含负偏移）、簇对齐
- 复现：`clang++ ... -DSS_TEST_HOOKS -o out\mfttest.exe test\mfttest.cpp src\core_scan.cpp src\core_features.cpp`
  然后 `out\mfttest.exe`

## 已知限制
- 配置对话框为精简版：直接编辑 `SpaceSnifferConfig.xml`（元素名与原版一致）
- `.sns` 只做读取兼容；写入标记实验性；自有 `.ssx` 无损
- `$MFT` 直读需管理员权限，非管理员自动回退
- 帮助/日志窗口为基础实现（可读、可清空、能复制）

### 7. 管理员权限下的 MFT 结果错误 —— 根因与修复（2026-11-25）

用户以管理员身份实测时，`G:\` 显示 538 TB、每个目录 4 TB，结果完全不可用。定位到
`ParseMftRecord()` 里两个真实缺陷：

1. **常驻 `$DATA`**：把数据内容的**前 4 字节**当成了文件大小。正确值是属性头里的
   `valueLength`（+0x10）。文本文件以 `﻿[Se` 开头时会被读成 ~4 TB 的"大小"，再被目录聚合放大。
2. **压缩文件的 `CompressedSize`**：位于属性头 +0x40，但**只有头部 ≥ 0x48 时才存在**；
   否则那里是 runlist，被当成 int64 读出来同样是天文数字。

修复：常驻流改用 `valueLength`；非驻留流按 `runListOffset` 推断头部长度后再取 CompressedSize；
另加防御性钳制（|size| 超出 2^62 直接归零），避免未来偏移错误再次污染整棵树。

`native/test/mfttest.cpp` 的单测已覆盖这两种情况（常驻大小断言、压缩头守卫）。

### 8. UI 细节修复（2026-11-25）

- **自绘菜单栏**：不再给窗口挂系统 HMENU，改为自绘子窗口（间距紧凑、悬停/展开态、
  主题切换即时重绘），下拉菜单加 `MNS_NOCHECK` 去掉左侧空槽，分隔线也改自绘 → 深色完整
- **过滤条默认收起**：工具栏「过滤」按钮 / `Ctrl+L` 展开；展开后才有输入框+过滤+清除
- **默认不显示 Free Space**（`showFreeSpace=false`），需要时工具栏「剩余」或 `Ctrl+F` 打开
- **右键 = 系统壳菜单**（`IShellFolder::GetUIObjectOf(IID_IContextMenu)` + `TrackPopupMenu`），
  与原版行为一致；右键时先清掉悬浮提示（`v->hover = kNone`）
- **小块标签**：宽度不足时退回小一号字体 + 省略号截断，而不是整块不画（阈值 26px，避免 1-2 字符噪音）
- 工具栏「主题」按钮文字修正（此前的 `☼/☾` 在缺字形时显示成 `☼/C`）
