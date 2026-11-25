# 只读代码复审报告（NeoSniffer）

> **执行说明**：严格只读，未修改/创建/删除任何文件，未执行任何终端命令；以下所有结论均基于对范围内全部 9 个文件及 4 个关联头文件（[core.h](../src/core.h)、[ui_treemap.h](../src/ui_treemap.h)、[ui_config.h](../src/ui_config.h)、[i18n.h](../src/i18n.h)）的完整静态阅读。

---

## P0：崩溃 / 挂死 / 数据损坏类（6 条）

1. **[mcp_main.cpp:373](../src/mcp_main.cpp#L373)、[:403-406](../src/mcp_main.cpp#L403-L406)、[:470](../src/mcp_main.cpp#L470)、[:506](../src/mcp_main.cpp#L506)、[:591](../src/mcp_main.cpp#L591)** — **P0 会崩**
   - **问题**：负数 `limit`/`top` 直接传入 `std::vector::resize((size_t)limit)`，触发超大内存分配导致进程崩溃；`ToolSearch` 中 `limit * 20` 存在有符号整数溢出。
   - **触发条件**：MCP 客户端调用 `largest_files` / `largest_folders` / `search` / `file_types` / `duplicates` / `cleanup_candidates` 时传入 `limit: -1`（或 `top: -1`），`(int)vec.size() > -1` 为真后执行 `resize(SIZE_MAX)`。
   - **建议修法**：统一将 `limit`/`top` 钳制到 `[0, 10000]` 之类的安全区间，并将 `limit * 20` 改为 `int64_t` 或固定采样上限。

2. **[core_scan.cpp:474](../src/core_scan.cpp#L474)、[:502-508](../src/core_scan.cpp#L502-L508)** — **P0 挂死**
   - **问题**：向 `ScanFast` 传入空路径会令所有扫描工作线程永久活锁（100% CPU 空转）。
   - **触发条件**：通过 CLI（如 `SpaceSniffer.exe scan ""`）或右键资源管理器虚拟目录背景（`%V` 展开为空）进入 `ScanFast(L"", ...)`，`pending` 初始化为 `1`，弹出后因 `item.path.empty()` 走 `Sleep(0); continue;` 且永不递减 `pending`。
   - **建议修法**：在 `ScanPath`/`ScanFast` 入口校验 `path` 非空（为空直接返回 `false`）；工作线程用独立 `bool gotItem` 区分“队列暂时为空”和“取到空路径”。

3. **[mcp_main.cpp:80-139](../src/mcp_main.cpp#L80-L139)、[:291-295](../src/mcp_main.cpp#L291-L295) 与 [core_features.cpp:706-722](../src/core_features.cpp#L706-L722)、[:1130-1151](../src/core_features.cpp#L1130-L1151)、[:1177-1240](../src/core_features.cpp#L1177-L1240)** — **P0 会崩**
   - **问题**：多处递归遍历缺少深度上限，存在栈溢出崩溃风险。
   - **触发条件**：向 MCP 输入深层嵌套 JSON（`ParseValue`/`ParseArray`/`ParseObject` 无深度限制）；或扫描经 `\\?\` 长路径可达的超深目录后，执行导出（`CountFiles`/`CountFolders`）、保存快照（`SerializeNode`/`PutSnsRecord`，每层还在栈上构造 `std::wstring path`）或 MCP `CollectAll`。
   - **建议修法**：JSON 解析增加最大嵌套深度（如 64）；树遍历/序列化改为显式 `std::vector` 栈迭代或增加 `depth` 上限（参考 `Tree::Aggregate`）。

4. **[mcp_main.cpp:723-728](../src/mcp_main.cpp#L723-L728)** — **P0 栈大缓冲/请求丢弃**
   - **问题**：`wmain` 栈上分配 64KB 固定缓冲 `char buf[65536]`，且 `fgets` 在单行超过 65535 字节时会直接截断并拆裂 JSON 报文。
   - **触发条件**：MCP 客户端发送超长 JSON-RPC 请求（>64KB）时，前后段分别解析失败被静默丢弃，调用方永久超时挂起。
   - **建议修法**：改用 `std::getline(std::cin, line)` 或堆上按块拼接直至 `\n`，并设置合理单行上限（如 4MB）。

5. **[mcp_main.cpp:715-717](../src/mcp_main.cpp#L715-L717)** — **P0 越界/未初始化读**
   - **问题**：`wchar_t exe[MAX_PATH];` 未初始化且未检查 `GetModuleFileNameW` 返回值，失败时读取栈垃圾、超长路径时静默截断。
   - **触发条件**：`SpaceSniffer.Mcp.exe` 部署在超 `MAX_PATH` 目录或系统调用失败时。
   - **建议修法**：改用 4096 宽字符的 `std::wstring` 并校验 `len > 0 && len < buf.size()`（与 [main.cpp:3191-3193](../src/main.cpp#L3191-L3193) 一致）。

6. **[core_scan.cpp:713-727](../src/core_scan.cpp#L713-L727)、[:917](../src/core_scan.cpp#L917)、[:1062](../src/core_scan.cpp#L1062)、[:1355](../src/core_scan.cpp#L1355)、[:1385](../src/core_scan.cpp#L1385)** — **P0 数据损坏/溢出 UB**
   - **问题**：`ApplyFixup` 检测到 USN 校验失败后静默返回（调用方仍继续解析损坏扇区）；多处 `r.clusters * (int64_t)cluster` 和 `r.lcn * (int64_t)cluster` 缺少溢出校验（而 `ExtractDataExtent:875` 已做校验）。
   - **触发条件**：扫描存在损坏 MFT 记录的 NTFS 卷，或在磁盘并发写入过程中读到半更新的 MFT 扇区。
   - **建议修法**：让 `ApplyFixup` 返回 `bool`，校验失败直接跳过该记录；抽取带上限/溢出校验的 `ClustersToBytes` 辅助函数复用。

---

## P1：功能不对类（16 条）

1. **[main.cpp:762-950](../src/main.cpp#L762-L950)、[:849](../src/main.cpp#L849)、[:2555](../src/main.cpp#L2555)、[:2992-3020](../src/main.cpp#L2992-L3020)、[:3386-3390](../src/main.cpp#L3386-L3390)** — **P1 功能不对**
   - **问题**：视图激活后全部全局快捷键（`Ctrl+1..4`/`Ctrl+0` 标签、`Backspace` 后退、`Ctrl+Up`/`Home`/`F`/`U`/`T`/`L`/`+/-`、`F5`/`F1`）完全失效，导致无菜单入口的**标签功能彻底不可用**。
   - **触发条件**：打开或点击任意扫描/快照视图时，`ShowView` 与 `ViewProc::WM_LBUTTONDOWN` 把焦点设在子窗口 `v->hwnd`，`WM_KEYDOWN` 发往 `ViewProc` 后被 `DefWindowProcW` 丢弃，根本到不了 `MainProc`；消息循环也未做加速键拦截。
   - **建议修法**：在 `wWinMain` 消息循环中统一拦截主窗口快捷键（或引入 `TranslateAcceleratorW`），当焦点不在 `EDIT` 控件时直接分发给 `DoCommand`。

2. **[core_scan.cpp:1757](../src/core_scan.cpp#L1757)、[:1839-1845](../src/core_scan.cpp#L1839-L1845)** — **P1 功能不对（严重）**
   - **问题**：MFT 模式扫描非卷根目录（子目录）时，重根后根节点与所有子节点的完整路径丢失盘符和父路径，且 `t.rootPath` 路径段被重复拼接。
   - **触发条件**：以管理员身份扫描如 `D:\Projects\Demo`：`t.root` 初始名为 `D:\Projects\Demo`，重根时 `t.rootPath = t.Path(focus)` 产出 `D:\Projects\Demo\Projects\Demo`，随后把 `focus` 设为新根但 `Name` 仍是 `Demo`；此后 `t.Path(child)` 全部变成 `Demo\foo.txt`，导致右键系统菜单（`SHParseDisplayName`）、面包屑、标签标题、导出 `<%pathfile%>`、快照及 MCP `list_children`/`tree` 全部失效。
   - **建议修法**：MFT 初始根命名为卷根（如 `C:\`），重根前取真实 `fullPath`，重根后将 `t.nodes[focus]` 重命名为 `fullPath`（或把 `t.rootPath` 与 `Tree::Path` 根前缀解耦）。

3. **[core_features.cpp:504-545](../src/core_features.cpp#L504-L545)** — **P1 功能不对（严重）**
   - **问题**：所有无前缀的大小与年龄过滤语法（`>1gb`、`>100kb`、`>2weeks`、`<10mb`）完全失效。
   - **触发条件**：在 GUI 过滤框或 MCP `search` 输入文档/提示框示例 `*.iso;>1gb;>2weeks`；行 504 `if (!src.empty())` 要求 `<`/`>` 前必须有字母前缀，导致 `>1gb`、`>2weeks` 全部落入 `FilterTerm::FileName`，匹配 0 个文件。
   - **建议修法**：移除 `if (!src.empty())` 外层限制，只要 `j < t.size() && (t[j] == L'<' || t[j] == L'>')` 即进入大小/年龄解析（行 532 本身已兼容 `lsrc.empty()`）。

4. **[core_features.cpp:458-466](../src/core_features.cpp#L458-L466)** — **P1 功能不对**
   - **问题**：属性过滤器 `:attr:` 的取反语法（`-s`、`-h`）完全失效，反被当作正向匹配。
   - **触发条件**：输入文档示例 `:attr:a+h-s` 或 `:attr:a,h,-s`；行 458 `SplitList(t.substr(6), parts, L"+,- ")` 把 `-` 当分隔符剥离，导致行 461 `p[0] == L'-'` 恒为假，所有属性都被写入 `attrMask` 而非 `attrNotMask`。
   - **建议修法**：不要把 `-` 作为 `SplitList` 分隔符，按 `L"+, "` 拆分并额外处理紧贴前项的 `-`（如 `a+h-s` 解析为 `a`、`h`、`-s`）。

5. **[core_features.cpp:593-597](../src/core_features.cpp#L593-L597)** — **P1 功能不对**
   - **问题**：`:tag:` 过滤未实现父目录标签继承，与帮助文档说明矛盾。
   - **触发条件**：给目录打上红色标签后在过滤器输入 `:red`；`FilterExpr::Matches` 仅检查文件自身的 `n.tag`，未沿 `n.parent` 向上查找继承标签，结果为 0。
   - **建议修法**：在 `FilterTerm::Tag` 分支向上遍历 `n.parent` 直到找到首个非 0 的 `tag`（加深度守卫）。

6. **[main.cpp:433](../src/main.cpp#L433)、[:2644-2649](../src/main.cpp#L2644-L2649) 与 [core_features.cpp:642-649](../src/core_features.cpp#L642-L649)** — **P1 功能不对**
   - **问题**：过滤结果为空时静默回退展示整个未过滤树；扫描整盘后过滤会把 `Free Space`/`Unknown` 伪节点无条件带入结果。
   - **触发条件**：(a) 子目录视图下输入不匹配任何文件的过滤词，`filtered.Clear()` 后 `ActiveTree(v)` 因 `filtered.Valid(root)` 为假退回 `v->tree`；(b) 卷根视图过滤 `*.mp4` 时 `ProjectNode` 强行复制虚拟节点并置 `any = true`，即使无匹配文件仍显示剩余/未知空间并计入总大小。
   - **建议修法**：`ActiveTree(v)` 直接按 `v->usingFiltered` 返回 `v->filtered`（空树显示无匹配提示）；`ProjectNode` 过滤时跳过 `IsVirtual()` 节点。

7. **[core_scan.cpp:601-603](../src/core_scan.cpp#L601-L603)、[:1847-1849](../src/core_scan.cpp#L1847-L1849) 与 [main.cpp:646-659](../src/main.cpp#L646-L659)、[:1554](../src/main.cpp#L1554)** — **P1 功能不对**
   - **问题**：伪节点（`Free Space`/`Unknown`）被再次 `Aggregate()` 累加进根节点占用，导致“磁盘占用”误报为整盘总容量；`UpdateViewStatus` 线性扫描 `t.nodes` 统计文件/目录数严重失真。
   - **触发条件**：(a) 扫描整盘（默认关闭显示剩余空间），状态栏、面包屑、导出 `<%percent%>`/`Summary` 与 MCP `scan` 均把可用空间计入已用占用，切换 `Ctrl+F`/`Ctrl+U` 也不更新；(b) MFT 扫子目录（`t.nodes` 保留全盘其余节点）或过滤后（被 unlink 的空目录仍在 `t.nodes`），状态栏显示全盘文件数/把虚拟节点算作目录。
   - **建议修法**：`Tree::Aggregate()` 汇总目录时不累加 `IsVirtual()` 节点（或单独记录已用/卷总量）；`UpdateViewStatus` 从 `t.root` 遍历可达的非虚拟节点计数。

8. **[main.cpp:2225-2241](../src/main.cpp#L2225-L2241)、[:2261-2336](../src/main.cpp#L2261-L2336)、[:3306](../src/main.cpp#L3306) 与 [ui_config.cpp:1576-1588](../src/ui_config.cpp#L1576-L1588)** — **P1 功能不对**
   - **问题**：`SHChangeNotify` 每次启动无条件触发导致桌面闪烁；注册表部分写入失败不回滚；已注册检查未校验当前 exe 路径；取消勾选后错误徽章残留。
   - **触发条件**：(a) 每次启动应用（即使默认 `shellIntegration = false` 且注册表本无键，或操作失败）都会调用 `SHChangeNotify(SHCNE_ASSOCCHANGED, ...)`；(b) 3 个注册表键写到第 2 个失败时不回滚且配置保持启用；(c) 移动 exe 后仍显示“已注册”；(d) 注册失败后在设置页取消勾选，仍展示红色失败错误码。
   - **建议修法**：先比对当前注册表实际值（含 exe 路径）与目标状态，仅在确实发生写入/删除且有改动时调用 `SHChangeNotify`；部分失败时回滚已建键；`ui_config.cpp` 仅在勾选状态与失败操作匹配时显示错误徽章。

9. **[main.cpp:2287](../src/main.cpp#L2287)、[:3319-3326](../src/main.cpp#L3319-L3326)** — **P1 功能不对**
   - **问题**：`scan "%V"` 依赖 `sanitizeCliPath` 仅修复末尾引号，在带后续 CLI 参数时被 `\"` 转义破坏。
   - **触发条件**：命令行执行 `SpaceSniffer.exe scan "C:\" filter *.mp4` 时，Windows `CommandLineToArgvW` 将 `\"` 解析为字面引号并吞并后续参数，`s.back() == L'"'` 不成立，目标路径损坏。
   - **建议修法**：注册表右键命令使用 `"%V."`（规范化后自动去除 `.` 且永无末尾 `\"` 冲突），并在 CLI 解析后对路径调用 `GetFullPathNameW` 规范化、拒绝空路径。

10. **[ui_config.cpp:2273-2427](../src/ui_config.cpp#L2273-L2427) 与 [main.cpp:2064](../src/main.cpp#L2064)、[:3045](../src/main.cpp#L3045)、[:3182-3188](../src/main.cpp#L3182-L3188)** — **P1 功能不对**
    - **问题**：设置/日志/帮助三个顶层窗口均未处理 `WM_DPICHANGED`，多显示器跨 DPI 拖动时界面缩放失调。
    - **触发条件**：Per-Monitor V2 下将设置对话框、日志窗口或帮助窗口从 100% 屏拖到 150%/200% 屏，标题栏按新 DPI 放大但客户区 `state.m.dpi`、字体和窗口大小不更新，控件被裁切或极小；启动 DPI 用 `GetDC(nullptr)` 也只取主屏。
    - **建议修法**：为 `ConfigWndProc`、`LogProc`、`HelpProc` 补齐 `WM_DPICHANGED` 处理（更新 DPI、重建字体、按 `lParam` 建议矩形 `SetWindowPos` 并重排）；打开子对话框时用 `GetDpiForWindow(owner)`。

11. **[core_scan.cpp:253-283](../src/core_scan.cpp#L253-L283)、[:349-358](../src/core_scan.cpp#L349-L358)、[:436](../src/core_scan.cpp#L436) 与 [core_features.cpp:16](../src/core_features.cpp#L16)、[:53](../src/core_features.cpp#L53)、[:1089](../src/core_features.cpp#L1089)、[:1106](../src/core_features.cpp#L1106)** — **P1 功能不对**
    - **问题**：长路径（`>MAX_PATH`）与 UNC 路径在卷查询、斜杠规范化、伪节点判断和文件读写处多点断裂。
    - **触发条件**：扫描 `\\server\share\sub` 或 `\\?\C:\...` 时 `QueryVolume` 的 `root[MAX_PATH]` 截断且未提取卷根导致簇/容量查询失败；相对短路径（如 `.`）误触 `size() <= 3` 走整盘伪节点逻辑；含 `/` 的长路径加 `\\?\` 后被 Win32 拒收；在超 `MAX_PATH` 路径读写配置/快照/导出时 `CreateFileW` 失败。
    - **建议修法**：`QueryVolume` 改用 `GetVolumePathNameW`；`ToLongPath` 先将 `/` 转为 `\` 并在 `core_features.cpp` 所有 `CreateFileW` 路径复用；整盘判断改为比对规范化路径与卷根是否相等。

12. **[main.cpp:543-558](../src/main.cpp#L543-L558)、[:771-777](../src/main.cpp#L771-L777)、[:1045-1166](../src/main.cpp#L1045-L1166)** — **P1 功能不对**
    - **问题**：过滤框不支持回车提交且状态文本被输入框遮挡；开始页缺少滚动与窄窗裁剪，多盘符时按钮出界不可点击。
    - **触发条件**：(a) 在过滤框输入完按 Enter 只发蜂鸣不触发过滤，且行 556 绘制的 `v->status` 被 `editFilter` 子窗口完全盖住；(b) 本机挂载 10+ 盘符或缩小主窗高度时，开始页的“浏览…”和管理员提示卡片落到可视区下方无法点击，宽度 `< S(468)` 时表头列矩形翻转。
    - **建议修法**：子类化 `editFilter` 在 `VK_RETURN` 调用 `ApplyFilter`，删除过滤行被遮挡的 `v->status` 绘制；为开始页驱动器列表加垂直滚动支持并保护窄窗列宽。

13. **[main.cpp:3141](../src/main.cpp#L3141)、[:3161](../src/main.cpp#L3161)、[:3369-3378](../src/main.cpp#L3369-L3378)** — **P1 功能不对**
    - **问题**：导出始终忽略当前过滤器；CLI 自动导出 30 秒硬超时且等待期间会吞掉 `WM_QUIT`。
    - **触发条件**：(a) 过滤视图后点导出或 CLI `scan ... filter ... export ...`，传入的是 `v->tree` 而非 `ActiveTree(v)`；(b) 大盘扫描耗时 >30s 时提前跳出循环导出空树；(c) CLI 扫描期间用户关窗，`PeekMessageW` 消费 `WM_QUIT` 未重新投递，随后进入主 `GetMessageW` 永久滞留后台。
    - **建议修法**：导出改为传 `ActiveTree(v)`；CLI 等待循环不设 30s 截断，捕获 `WM_QUIT` 后立即 `PostQuitMessage` 并中止后续导出。

14. **[core_features.cpp:1279-1293](../src/core_features.cpp#L1279-L1293)、[:1417-1422](../src/core_features.cpp#L1417-L1422) 与 [main.cpp:2679-2698](../src/main.cpp#L2679-L2698)** — **P1 功能不对**
    - **问题**：保存 `.du` 快照实际写入二进制 `SSX1`；加载真实 `.du` 文本快照时节点名存了完整路径，导致路径重复拼接、层级失效。
    - **触发条件**：保存快照对话框选择 `*.du`（`SaveSnapshot` 无 `.du` 写出分支且 `native` 恒为 `false`）；或加载标准 `.du` 文本，行 1418 将 `pathPart` 全路径作为 `Name`，导致 `out.Path(stack[k])` 翻倍拼接。
    - **建议修法**：补齐 `.du` 写出逻辑（或先从保存过滤器移除 `*.du`）；`.du` 加载时截取父路径后的末段文件名作为节点名。

15. **[main.cpp:380-418](../src/main.cpp#L380-L418)、[:510](../src/main.cpp#L510)、[:699](../src/main.cpp#L699)、[:714](../src/main.cpp#L714)、[:1005-1016](../src/main.cpp#L1005-L1016) 与 [ui_config.cpp:676-700](../src/ui_config.cpp#L676-L700)、[:2121-2126](../src/ui_config.cpp#L2121-L2126)、[:2604](../src/ui_config.cpp#L2604)** — **P1 功能不对**
    - **问题**：多项设置项（`refreshDelayMs`、`useRealZoom`、`scanAds`、`logPopup`/`Issue`/`Warning`）为空壳不生效；设置页预览切换语言后点“取消”主窗残留错误语言；开始页“以管理员重启”丢失命令行参数且 UAC 取消/失败静默无提示。
    - **触发条件**：修改上述设置后无实际行为变化（`zoomTimeMs == 250` 还被行 510 暗改 180ms）；设置页切语言预览再点取消，主窗口若曾重绘则不会恢复原语言；带参数启动后在开始页点击“以管理员重启”丢失参数且拒绝 UAC 无日志/提示。
    - **建议修法**：接入或隐藏未实现选项；`ShowConfigDialog` 取消后若切换过语言则刷新主窗；`RestartAsAdmin` 统一复用 `RelaunchElevated(g_main)` 并记录失败/取消日志。

16. **[core_scan.cpp:1894](../src/core_scan.cpp#L1894)、[core_features.cpp:1012](../src/core_features.cpp#L1012)、[:1041](../src/core_features.cpp#L1041)、[:1046](../src/core_features.cpp#L1046)、[:1298-1306](../src/core_features.cpp#L1298-L1306)、[:1378](../src/core_features.cpp#L1378)、[:1424](../src/core_features.cpp#L1424) 与 [mcp_main.cpp:258](../src/mcp_main.cpp#L258)、[:302](../src/mcp_main.cpp#L302)、[:339-340](../src/mcp_main.cpp#L339-L340)、[:487](../src/mcp_main.cpp#L487)、[:632-640](../src/mcp_main.cpp#L632-L640)** — **P1 功能不对**
    - **问题**：英文模式下核心层多处硬编码中文文案渗出；MCP 缓存刷新大小写敏感且不复用父盘缓存、虚拟节点标为 `"folder"`、`min_size_mb` 小数截断为 0、`export_report` schema 声明 `config` 可选但省略必报错（且允许写入任意路径与文件头“Read-only”声明不符）。
    - **触发条件**：英文模式下触发 MFT 回退/快照或导出失败看到中文；MCP 先 `scan("c:\\")` 再 `scan("C:\\")` 命中旧缓存，或传 `min_size_mb: 0.5` 被截断成 0，或调用 `export_report` 不传 `config` 直接失败。
    - **建议修法**：核心层错误文案接入 `ss::i18n::T()` 或返回错误码；MCP 缓存键统一转小写规范化并支持按前缀复用父树；`NodeJson` 区分虚拟节点；`min_size_mb` 先按 `double` 乘再转 `int64_t`；给 `export_report` 的 `config` 设默认值或标为 `required`。

---

## P2：可维护性 / 设计 / 健壮性类（7 条）

1. **[build.ps1:9](../build.ps1#L9)、[:24-25](../build.ps1#L24-L25)、[:52-53](../build.ps1#L52-L53)、[:69-102](../build.ps1#L69-L102) 与 [app.rc:1](../app.rc#L1)** — **P2 构建健壮性**
   - **问题**：`.rc` 编译缺少失败回退（`windres` 失败直接中断不尝试 `llvm-rc`，`llvm-rc` 分支直接把 `.res` 传给链接器且未给预处理传 `-I $root`，缺 `python` 且缺 `app.ico` 时静默跳过导致后续报错）；`app.rc` 包含完整 `<windows.h>` 且缺应用清单；未停止运行中的 `SpaceSniffer.Mcp`；`-luxtheme` 重复两次且控制台版 MCP 无差别链接全套 GUI 库；`$coreSources` 为死变量。
   - **触发条件**：在仅装部分工具链、MCP 正被宿主占用或重新拉取无 `app.ico` 的环境中执行 `build.ps1`。
   - **建议修法**：`app.rc` 改用 `#include <winver.h>` 并补嵌入式清单（ComCtl32 v6 / `longPathAware`）；`build.ps1` 停止 `SpaceSniffer.Mcp`、拆出独立 `$mcpLibs`、`windres` 失败时回退 `llvm-rc`+`llvm-cvtres` 或提供降级告警继续编译。

2. **[main.cpp:132](../src/main.cpp#L132)、[:148-158](../src/main.cpp#L148-L158)、[:205](../src/main.cpp#L205)、[:210](../src/main.cpp#L210)、[:319](../src/main.cpp#L319)、[:935-937](../src/main.cpp#L935-L937)、[:962](../src/main.cpp#L962)、[:1425](../src/main.cpp#L1425)、[:1428-1444](../src/main.cpp#L1428-L1444) 与 [core.h:83](../src/core.h#L83)** — **P2 死代码与未配对 GDI 句柄**
   - **问题**：`g_fontSeg` 在 `RebuildFonts()` 中创建但从未被使用，且 `CleanupGlobalGdi()` 漏掉释放（退出时泄漏 1 个 HFONT）；另有 `ViewState::scrollY`、`BuildDriveRows` 的 `buf[MAX_PATH]`、子窗口 `ViewProc` 的 `WM_DPICHANGED`、`DrawToolbar` 行 1429 不可达的 `if (isSegment)` 分支及 `(g_pal.dark ? g_pal.text : g_pal.text)`、`Tree::AddNode` 行 83 冗余赋值。
   - **触发条件**：常规运行即创建废弃字体；阅读与维护时易误导。
   - **建议修法**：删除 `g_fontSeg` 及上述死代码/冗余语句。

3. **[ui_treemap.cpp:473-474](../src/ui_treemap.cpp#L473-L474)、[:899-903](../src/ui_treemap.cpp#L899-L903)、[:1218-1222](../src/ui_treemap.cpp#L1218-L1222)、[:1430-1434](../src/ui_treemap.cpp#L1430-L1434)、[core_features.cpp:1244-1245](../src/core_features.cpp#L1244-L1245)、[main.cpp:1367](../src/main.cpp#L1367)、[:2431](../src/main.cpp#L2431)** — **P2 `static` 可变全局可重入/并发隐患**
   - **问题**：多处函数内/模块级 `static` 可变状态（`GdiCache`、`RenderScratch`、`GetAnimSessions`、`SrgbByteToLinear` 表、`B64Decode` 表、`cachedOk[64]`、`s_inMenu`）不可重入且无同步。
   - **触发条件**：未来引入多线程渲染/解码或在单测中并发调用 `B64Decode`/`DrawTreemap` 时产生数据竞争。
   - **建议修法**：纯查找表改为 `constexpr` 或 `const static` 初始化；渲染缓存和动画会话收拢到显式上下文对象。

4. **[main.cpp:273](../src/main.cpp#L273) / [ui_config.cpp:124](../src/ui_config.cpp#L124)、[core_scan.cpp:1169-1505](../src/core_scan.cpp#L1169-L1505)、[core_scan.cpp:515](../src/core_scan.cpp#L515) / [core_features.cpp:406](../src/core_features.cpp#L406)** — **P2 重复代码、超大函数与魔数冲突**
   - **问题**：DWM 标题栏设置、UTF-8/UTF-16 转换（3 份）、取 exe 目录（5 处）、MFT 属性解析（3 份）均存在复制粘贴；`ScanMftEx`(535 行)/`DrawTreemap`(365 行)/`RebuildLayout`(235 行) 过长；`0x80000000u` 在 `attrs` 中同时代表“目录访问被拒”和“ADS 属性”；DWM 属性 `19/20/34/35/36` 与定时器 ID `1/2/9` 均为散落魔数。
   - **触发条件**：修改其中一处（如 `ApplyDwmChrome` 或 MFT 属性解析）极易遗漏副本导致行为分叉。
   - **建议修法**：抽取共用工具函数与命名常量，拆分超大函数；为“访问被拒”和“ADS”分配独立标志位。

5. **[ui_treemap.h:9-14](../src/ui_treemap.h#L9-L14)、[ui_treemap.cpp:393](../src/ui_treemap.cpp#L393)、[:1150](../src/ui_treemap.cpp#L1150)、[core_scan.cpp:335](../src/core_scan.cpp#L335)、[ui_config.cpp:372](../src/ui_config.cpp#L372)、[main.cpp:351](../src/main.cpp#L351)、[:954](../src/main.cpp#L954)、[:3151](../src/main.cpp#L3151)、[mcp_main.cpp:501](../src/mcp_main.cpp#L501)、[:536](../src/mcp_main.cpp#L536)** — **P2 结构体缺少成员默认初始化**
   - **问题**：上述 10 个结构体的标量/RECT 成员无类内默认初始化器（且 `build.ps1:18` 关闭了 `-Wno-missing-field-initializers`），例如 [core_scan.cpp:489](../src/core_scan.cpp#L489) `WorkItem item;` 的 `item.node` 处于未初始化态。
   - **触发条件**：新增调用方忘记用 `{}` 值初始化或新增字段时，可能读到未初始化内存。
   - **建议修法**：为所有结构体的标量/RECT 成员添加 `= 0` / `{}` 默认初始化。

6. **[main.cpp:2042-2062](../src/main.cpp#L2042-L2062)、[i18n.cpp:389-402](../src/i18n.cpp#L389-L402)、[core_features.cpp:69-97](../src/core_features.cpp#L69-L97)、[:1028-1045](../src/core_features.cpp#L1028-L1045)、[core_scan.cpp:57-68](../src/core_scan.cpp#L57-L68)** — **P2 性能与边角健壮性**
   - **问题**：(a) `LogLine` 每次全量拼接最多 4000 行并 `SetWindowTextW`（O(N²)），还会覆盖 `CurrentView()->status` 冲掉视图统计摘要；(b) `i18n::T()` 每帧高频线性 `wcscmp` 遍历 150+ 项，且缺 `en` 译文时默认回退中文；(c) `XmlValue` 搜索 `<name` 未校验标签名边界；(d) `ExportRun` 忽略 `blockSorting`、`r.bytes` 报 UTF-16 字节数而非 UTF-8 实际写入量；(e) `FmtCount` 遇到负数会产生 `-,100`。
   - **触发条件**：长时间运行大量日志输出、打开含近似前缀标签的 XML、使用自定义导出模板。
   - **建议修法**：`LogLine` 用 `EM_REPLACESEL` 追加且不再覆盖视图统计；`XmlValue` 校验标签结束符（`>`/`/`/空白）；`ExportRun` 返回实际写入字节数；`FmtCount` 先处理符号位。

7. **[ui_config.cpp:1232](../src/ui_config.cpp#L1232)、[:2417-2420](../src/ui_config.cpp#L2417-L2420)、[:2456](../src/ui_config.cpp#L2456) 与 [main.cpp:2736](../src/main.cpp#L2736)、[:2748](../src/main.cpp#L2748)** — **P2 边界防御不足**
   - **问题**：`ui_config.cpp` 未检查 `gdiplus.active` 直接构造 `Gdiplus::Graphics`、`WM_NCDESTROY` 未置 `st->closed = true`（窗口被外部销毁时会卡在模态循环）、表格滚动条 `thumbH` 未限制 `<= trackH`；`CMD_UP`/`CMD_HOME` 导航后未清空 `v->forward`。
   - **触发条件**：GDI+ 初始化异常、极小窗口高度、或先“后退”再点“上一层/回到根”后发现“前进”仍残留旧历史。
   - **建议修法**：补齐上述边界钳制、`closed = true` 兜底及 `v->forward.clear()`。

---

## 总结与上线建议

### 1. 建议**上线前必须修复**的条目（共 **16 条**：6 条 P0 + 10 条核心 P1）

- **全部 6 条 P0**（MCP 负 `limit` 崩溃、空路径 `ScanFast` 活锁、递归栈溢出、MCP 64KB 截断、`mcp_main` 未初始化路径缓冲、MFT fixup/溢出校验）：直接影响进程稳定性与防挂死。
- **10 条高影响 P1（P1 #1 ~ #10）**：
  - **P1 #1**（快捷键与标签全失效）与 **P1 #3/#4/#5**（过滤语法 `>1gb`、`:attr:-s`、`:tag:` 继承失效）：属于核心交互与文档承诺功能大面积不可用；
  - **P1 #2**（MFT 子目录重根路径损坏）：管理员模式下扫任何文件夹都会破坏右键菜单、导出和 MCP；
  - **P1 #6/#7**（空过滤回退原树、伪节点污染总大小与文件数）：直接导致用户看到错误的空间统计；
  - **P1 #8/#9**（每次启动刷 `SHChangeNotify`、Shell 集成错误处理与 `%V` 展开）：每次启动闪桌面且右键入口易踩坑；
  - **P1 #10**（设置/日志/帮助缺 `WM_DPICHANGED`）：双屏高 DPI 下设置弹窗直接错位。
- 其余 P1（#11~#16：长路径/UNC 细节、开始页滚动、`.du` 读写、空壳设置项清理、英文模式核心硬编码文案、MCP 缓存/字段细节）建议视发布范围排入紧随其后的迭代（若本次同时发布英文版或 MCP，建议将 **P1 #16** 一并纳入上线前修复）。

### 2. 已检查但**确认没问题**的关键点

- **无 `goto` / 无 `std::regex`**：范围内全部源码均未出现 `goto` 跳过初始化问题，也未使用 `std::regex`（通配符为手写双指针 `WildcardMatch`）。
- **核心 GDI / GDI+ 生命周期配对良好**：
  - `ui_config.cpp` 中 `GdiplusSession` 覆盖整个对话框生命周期，`Gdiplus::Graphics` 严格在 `BitBlt` 与 `ScopedDC`/`ScopedBitmap` 析构前作用域退出；`ScopedFont`/`ScopedPen`/`ScopedBrush` 与 `SelectObject` 均正确还原。
  - `ui_treemap.cpp` 的 `GdiCache` 在析构时配对释放全部 `HFONT`/`HBITMAP`/`HDC`，`DrawTreemap`/`DrawTreemapTooltip` 使用 `SaveDC`/`RestoreDC` 成对恢复裁剪区与字体。
  - `main.cpp` 中 `ShowShellContextMenu` 的 `IContextMenu`/`2`/`3`、`IShellFolder`、`PIDL`、`HMENU` 全部正确释放，全局 `g_shellCm2`/`g_shellCm3` 在 `TrackPopupMenu` 返回后立即清零；`ViewProc`/`StartProc`/`MainProc`/`MenuBarProc`/`PopupMenuProc` 的双缓冲 `DC`/`HBITMAP` 全部配对释放。
- **扫描线程跨线程同步安全**：工作线程只写独立 `ScanJob` 和 `Interlocked*` 统计字段（`progress.message` 仅在 `worker.join()` 之后由 UI 线程读取），不碰 `ViewState::tree`、`HWND` 或 `g_cfg`；`ScanFast` 内 `AddNodeFilled` 在同一把 `treeMutex` 锁内完成节点构造与写入，不存在 `t.nodes` 扩容导致的悬垂引用；`ViewState::~ViewState()`、`CloseView`、`MainProc::WM_CLOSE` 均先设取消标志再 `join()` 线程，且 `MainProc::WM_CLOSE` 先 `DestroyWindow` 再 `g_views.clear()` 避免 `ViewProc` UAF。
- **i18n 已有 key 完整性**：核对了 `main.cpp`、`ui_treemap.cpp`、`ui_config.cpp` 中全部 `ss::i18n::T(L"...")` 调用，所有 key 均存在于 `i18n.cpp` 的 `kTable` 且均有非空英文译文（无悬空 key）。
- **设置页滚动区裁剪与命中一致**：`ui_config.cpp` 的 `DrawSettingsBody` 同时设置了 GDI `IntersectClipRect` 和 GDI+ `SetClip` 且配对恢复，`HitTest` 先校验 `PtInRect(&layout.bodyRect, pt)` 防止点击被裁切行。
- **启动自动提权回退安全**：`wWinMain` 中 `alwaysRunAsAdmin` 触发 `RelaunchElevated` 失败或用户取消 UAC 时，不会崩溃也不会死循环重启，而是回退为普通权限继续启动。
- **工具栏/标签页点击遍历无迭代器失效**：`MainProc::WM_LBUTTONDOWN` 在触发 `DoCommand`/`CloseView`/`ShowView` 后立即 `return 0;` 退出循环。

### 3. 尚未覆盖的区域（诚实说明）

- **未编译与动态运行验证**：按任务要求未执行任何终端命令，未进行实机编译（clang++ / windres / llvm-rc）、ASAN/UBSAN 检测、真实 NTFS 卷（含压缩/稀疏/WOF/大 `$ATTRIBUTE_LIST`）扫描精度实测及高 DPI 多屏 GUI 实机交互测试。
- **范围外文件未检查**：`tools/`（如 `tools/make_icon.py`）、`docs/`、已有单元测试以及可能存在的其他脚本/资源文件未纳入本次阅读范围。
- **变更文件清单**：无（严格只读，0 个文件被修改）。
- **阻塞项**：无。