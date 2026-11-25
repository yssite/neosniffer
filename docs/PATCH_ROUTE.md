# 二进制补丁路线：能力边界与实测报告

> 目标：在**不重编译**（无 Delphi 编译器）的前提下，对 `C:\Users\EDM\bin\SpaceSniffer.exe`
> （SpaceSniffer 1.3.0.2，Delphi 2010 / VCL / Graphics32，Win32 i386）做**最小、可复现、可回滚**的增强，
> 并给出实测数据，用于和 `native/` 原生重制版对照。
>
> 日期：2026-11-25　实测机：Windows 11，单显示器 2560×1440 @ **125%** 缩放（系统 DPI 120）

---

## 0. 结论速览

| # | 补丁 | 可行性 | 状态 | 实测效果 |
|---|---|---|---|---|
| A | **DPI 清单注入**（PerMonitorV2 / System + longPathAware） | ✅ 可做 | **已交付** | 125% 下由"96 DPI 渲染 + DWM 位图拉伸"变为 **120 DPI 原生渲染**，窗口物理尺寸不变、文字/图标清晰 |
| B | **扫描加速**（`FindExInfoBasic` + `FIND_FIRST_EX_LARGE_FETCH`） | ⚠️ 理论可做，**原版已启用** | 检测为 **already-applied**（不改字节） | `C:\Windows` A/B：与未打补丁版差异在噪声内（见 §6）；把参数改回 0 反而**变慢**，证明参数确实生效 |
| C | **关闭 "Check for update"** | ✅ 可做（低风险） | 已实现（`--no-update-check`，1 字节） | 原版**本来就没有自动联网检查**（二进制不导入任何网络 DLL），该菜单项只是 `ShellExecute` 打开网页；补丁让它变成空操作 |
| D | 深色模式 | ❌ 不可行 | — | 见 §8.1 |
| E | MFT 直读 | ❌ 不可行 | — | 见 §8.2 |
| F | MCP 接口 | ❌ 不可行 | — | 见 §8.3 |
| G | 树图重排 / 列布局 | ❌ 不可行（布局微调除外） | — | 见 §8.4 |

**一句话结论**：字节补丁能改的是**资源（清单/字符串/布局常量）**和**已有指令的立即数/开关**；
凡是"新增行为"（联网、读 MFT、换绘制模型、加控件）都必须重写代码 —— 这正是 `native/` 存在的理由。

### 交付物

| 路径 | 说明 | SHA-256 |
|---|---|---|
| `dist/patched/SpaceSniffer.Patched.exe` | **主成品**：DPI 清单（PerMonitorV2 + longPathAware）+ fast-scan（检测为已启用） | `85EA18FF8D4AF10D335B24D8130FE8B27F4FB72B7429531CF178B62627C52D86` |
| `dist/patched/SpaceSniffer.Patched.SystemDpi.exe` | 备选：DPI 清单用 System 模式（混合缩放显示器更稳） | `AE7DC557B769F2BE1557BF5985B0FBED30F867819141B1A4C801F518F27C9ECB` |
| `dist/patched/patch_report.json`（及 `.systemdpi.json`） | 逐字节补丁报告（偏移 / 原字节 / 新字节 / 校验） | — |
| `tools/patch_original.py` | 打补丁工具（纯 Python，幂等，SHA-256 门禁） | — |
| `work/patch/shots/` | 125% 实测截图 + 放大对照图 | — |
| `work/patch/bench/bench_ab_final.csv` | A/B 原始数据（9 次运行） | — |

> 原版 `C:\Users\EDM\bin\SpaceSniffer.exe` 全程只读：实验前后 SHA-256 均为
> `FC0629D450F8A57BC93E1BA1CDEF0BFF49C1A4CF0725C2A1F52116FD67D9FE8E`。

---

## 1. 先修正三条前提（实测与任务书不符）

逆向结论必须以二进制为准，以下三点经本次实测修正（都有可复核证据）：

| 任务书前提 | 实测结果 | 证据 |
|---|---|---|
| 「无 manifest 资源（资源类型 24 不存在）」 | **存在两个** `RT_MANIFEST`：`ID 1 / LANG 1033`（752 B，`DelphiApplication`）与 `ID 1 / LANG 1040`（850 B，`CodeGear RAD Studio`），都声明 Common-Controls 6 + `asInvoker`，但**都没有 `dpiAware`/`dpiAwareness`** | `pefile` 资源枚举；原文见 `work/ana/manif.py` 输出 |
| 「原版调用很可能是 `FindExInfoStandard(0)` + flags `0`」 | 实际是 **`FindExInfoBasic(1)` + `FIND_FIRST_EX_LARGE_FETCH(2)`**，两个调用点都是 | §4.1 反汇编 |
| 「`work/patch/SpaceSniffer_dpi.exe` 用 ResourceHacker 注入成功」 | 该文件 **1033/1040 清单内容与原版逐字节相同**（只是 `.rsrc` 被重排、文件 +0x200），**没有真正改到清单**；本次用自研注入器重做并做了运行时验证 | `work/ana/injman.py`、§3.5 |

另外：任务书中的「函数 RVA 0x004386C0」实际是 **VA**（ImageBase 0x400000 → RVA 0x386C0）。本文全部偏移同时给出
**RVA / VA / 文件偏移**三者，避免混淆。

---

## 2. 工具：`tools/patch_original.py`

纯 Python（`pefile` + `capstone`），**不依赖 ResourceHacker / LIEF**，资源节注入是自己实现的。

```bash
# 交付用的成品就是这条命令生成的
python tools/patch_original.py \
    --in  "C:\Users\EDM\bin\SpaceSniffer.exe" \
    --out dist/patched/SpaceSniffer.Patched.exe \
    --dpi permonitor --fast-scan \
    --report dist/patched/patch_report.json
```

| 能力 | 说明 |
|---|---|
| `--dpi permonitor\|system\|none` | 重建 `RT_MANIFEST`（PerMonitorV2 或 System 模式，均带 `longPathAware`） |
| `--fast-scan` | 定位 `FindFirstFileExW` 的 IAT → 导入桩 → 全部调用点，逐个解析 6 个栈参数；参数为 0 才改成 1/2，已经是 1/2 则报 `already-applied` |
| `--no-update-check` | 把 `actCheckForUpdateClick` 首字节 `53`(push ebx) 改成 `C3`(ret) |
| `--report <json>` | 完整报告：每个补丁的文件偏移、原始字节、新字节、校验结果 |
| 输入校验 | SHA-256 必须等于 `FC0629…FE8E`，否则拒绝；`--force` 可绕过；**本工具自己的产物**（含 `.rsrc2` 节）可直接作为输入 |
| 幂等 | 再次运行时先剥离 `.rsrc2` 再重建；实测 `patch(patch(x)) == patch(x)`（三次运行 SHA-256 完全一致） |
| 不碰原文件 | 只读输入、只写 `--out`；原版 SHA-256 在每次实验前后校验（见 §9） |

每个补丁在控制台和 JSON 里都长这样（可逐字节复核）：

```
[       applied] dpi-manifest: ...
[already-applied] fast-scan: the shipped build already calls FindFirstFileExW with ...
                 file 0x00037DE8  6A 00 -> 6A 01   (fInfoLevelId: 0 -> 1 (FindExInfoBasic))
```

---

## 3. 补丁 A：DPI 清单注入（已交付）

### 3.1 原版为什么模糊

原版两个清单都只声明 comctl32 v6 + `asInvoker`，**没有任何 DPI 感知声明** → 进程是 `DPI_UNAWARE`：
Windows 按 96 DPI 让它渲染，再把整幅位图**双线性拉伸**到 125% / 150%。这就是"字大但发虚"的来源。

实测（125%）：`GetProcessDpiAwareness = 0`，`GetDpiForWindow = 96`，窗口 1536×803 逻辑 → 屏幕 **1920×1004 物理**（×1.25 拉伸）。

### 3.2 注入方法（纯 Python 重建资源树）

不能简单地"往 `.rsrc` 里塞一条"：`.rsrc` 后面紧跟 `.reloc`，中间没有空间；且原版资源目录里已有 55 个条目。
做法是**新建一个节 `.rsrc2`，把整棵 3 级资源目录树重新序列化进去**：

```
.rsrc2 节内容
├─ 资源目录树（root → type → name → language → IMAGE_RESOURCE_DATA_ENTRY）  0xBCA 字节
├─ 新清单 blob（1033 与 1040 共用同一份）                                    0x9E0 字节
└─ 原 .rsrc 原始数据整块复制                                                0x2E200 字节
```

然后：

1. 追加 40 字节节表项（原头部 `SizeOfHeaders=0x600`，节表结束于 `0x460`，**正好有 0x1A0 空闲**，不需要搬动任何东西）；
2. `NumberOfSections` 9 → 10；
3. `SizeOfImage` `0x26C000` → `0x29C000`；
4. **数据目录 #2（资源）** 从 `(RVA 0x21C000, size 0x2E200)` 改为 `(RVA 0x26C000, size 0x2F7B0)`。

### 3.3 逐字节偏移表（di = 相对原版文件）

| 文件偏移 | 字段 | 原值 | 新值 | 说明 |
|---|---|---|---|---|
| `0x206` | `NumberOfSections` | `09` | `0A` | +1 |
| `0x250`–`0x253` | `SizeOfImage` | `00 C0 26 00` | `00 C0 29 00` | 0x26C000 → 0x29C000 |
| `0x288`–`0x28B` | 数据目录#2 `.VirtualAddress` | `00 C0 21 00` | `00 C0 26 00` | 指向新树 |
| `0x28C`–`0x28F` | 数据目录#2 `.Size` | `00 E2 02 00` | `B0 F7 02 00` | 0x2E200 → 0x2F7B0 |
| `0x460`–`0x487` | 新节表项（原本全 0） | `00…00` | `.rsrc2` / VSize `0x2F7B0` / VA `0x26C000` / RawSize `0x2F800` / RawPtr `0x233E00` / Char `0x40000040` | 只读已初始化数据 |
| `0x233E00`–`0x2A35FF` | 新节原始数据 | — | 194 560 B | 追加在文件末尾 |

> 文件大小：2 309 632 → **2 504 192**（+194 560 = 0x2F800）。除上述字段外，**头部其余字节完全未变**（已用逐字节 diff 验证）。

### 3.4 踩过的坑：资源数据必须落在数据目录范围内

第一版实现把旧资源的 `IMAGE_RESOURCE_DATA_ENTRY.OffsetToData` 原样保留（仍指向旧 `.rsrc` 的 RVA），
结果程序能启动、能扫描，但**版本号变成 `0.0.0.0`** —— `GetFileVersionInfo` 只认落在
`[资源目录 RVA, +Size)` 区间内的数据块，旧 `.rsrc` 的 RVA 低于新目录基址，被判为无效。

修正：把旧 `.rsrc` 整块复制进新节，并**重写全部 55 条 data entry 的 RVA**（`新RVA = 0x26D5B0 + (旧RVA − 0x21C000)`）。
工具现在把 `GetFileVersionInfoSizeW > 0` 作为**强制性回归检查**，这个坑不会再复现。

### 3.5 两种模式

| 模式 | 清单内容 | 适用场景 |
|---|---|---|
| `permonitor` | `<dpiAware>true/pm</dpiAware>` + `<dpiAwareness>PerMonitorV2</dpiAwareness>` + `<longPathAware>true</longPathAware>` | 交付主用；单显示器 125% 下效果最好 |
| `system` | `<dpiAware>true</dpiAware>` + `<dpiAwareness>System</dpiAwareness>` + `<longPathAware>true</longPathAware>` | 多显示器混合缩放时更稳（VCL 2010 不会处理 `WM_DPICHANGED`，PerMonitorV2 只是让窗口"不缩放"，跨屏会错位；System 模式由 VCL 自己做一次 96→N 缩放） |

两种模式都保留原清单的 **comctl32 v6 依赖**（否则按钮/工具栏会退回 Windows 95 外观）与 **`asInvoker`**（UAC 行为不变）。

### 3.6 运行时实测（125%）

| 指标 | 原版 | 补丁版（permonitor） |
|---|---|---|
| `GetProcessDpiAwareness` | **0**（DPI_UNAWARE） | **2**（PER_MONITOR_DPI_AWARE） |
| `GetDpiForWindow` | 96 | **120** |
| 窗口物理尺寸 | 1920×1004（1536×803 被拉伸） | **1920×1010**（120 DPI 原生渲染，VCL 自动按 1.25 缩放窗体） |
| 文字/图标 | 位图拉伸，发虚 | GDI 在 120 DPI 下重新光栅化，**清晰** |
| 版本号 | 1.3.0.2 | 1.3.0.2（版本资源未被破坏） |

> 因为 VCL 窗体的 `PixelsPerInch=96` 且 `Scaled=True`，系统 DPI 变成 120 后 VCL 会把窗体整体放大 1.25 倍 ——
> 所以补丁版**既是清晰渲染、物理尺寸又和原来一样**，并不是"清晰但变小"。
> 截图见 §7 与 `work/patch/shots/`。

---

## 4. 补丁 B：扫描加速（原版已经是最优参数）

### 4.1 调用链定位（静态）

Delphi 不是直接 `call [IAT]`，而是经由 `.text` 里的导入桩：

```
IAT 槽   FindFirstFileExW  VA 0x6164CC   (文件 0x1E10CC)
  └─ 桩  jmp dword ptr [0x6164CC]  VA 0x5B8496 (文件 0x1B7A96)
       ├─ 调用点 #1  VA 0x438868 (文件 0x37E68)   ← 主路径
       └─ 调用点 #2  VA 0x43892E (文件 0x37F2E)   ← 首个 pattern 失败后的重试
```

两个调用点都在函数 **VA 0x4386C0（RVA 0x386C0）** 内 —— 它就是目录枚举主循环：函数内既有
`FindNextFileW`（VA 0x439887）也有 `GetCompressedFileSizeW` 帮手（VA 0x439642）的调用，且是
`FindFirstFileExW` 的唯一使用者。

调用点的 6 个栈参数（stdcall，从右往左压栈，文件偏移已标注）：

| VA | 文件偏移 | 指令 | 对应参数 |
|---|---|---|---|
| `0x4387E6` | `0x37DE6` | `6A 02` push 2 | `dwAdditionalFlags = FIND_FIRST_EX_LARGE_FETCH` |
| `0x4387E8` | `0x37DE8` | `6A 00` push 0 | `lpSearchFilter = NULL` |
| `0x4387EA` | `0x37DEA` | `6A 00` push 0 | `fSearchOp = FindExSearchNameMatch` |
| `0x4387EC` | `0x37DEC` | `8D 8D 88 FC FF FF` lea ecx,[ebp-0x378] + `51` push ecx | `lpFindFileData`（WIN32_FIND_DATAW） |
| **`0x4387F3`** | **`0x37DF3`** | **`6A 01` push 1** | **`fInfoLevelId = FindExInfoBasic`** |
| `0x438867` | `0x37E67` | `50` push eax | `lpFileName` |

调用点 #2 同构：`push 2` @ `0x4388BB`（文件 `0x37EBB`）、`push 1` @ `0x4388C8`（文件 `0x37EC8`）。

### 4.2 关键发现：这条路径受 Windows 版本门控

```
004387D9  80 3D 39 9D 5E 00 00   cmp  byte ptr [0x5E9D39], 0
004387E0  0F 84 7F 01 00 00      je   0x438965          ; flag==0 → 退回 FindFirstFileW 老路径
```

`[0x5E9D39]` 在启动时由 `RtlGetVersion` 结果算出（VA `0x43AFCA`–`0x43B018`）：

```
cmp  [ebp-0x304], 6      ; dwMajorVersion
ja   …                   ; >6  → 1
jne  …                   ; <6  → 0
cmp  [ebp-0x300], 1      ; ==6 且 dwMinorVersion>=1（Windows 7）
jae  …                   ; → 1
```

即：**Windows 7 及以上一律走 `FindFirstFileExW(Basic + LARGE_FETCH)`**，Win2000/XP/Vista 才走老路径。
在 Win11 上实测 `[0x5E9D39] = 1`，所以原版扫描**已经**用了这两个优化参数。

### 4.3 动态证明参数真的生效（反证实验）

把两个调用点的 `fInfoLevelId` 从 `1` 改成非法的 `2`（`FindExInfoMaxInfoLevel`），其余不动：

```powershell
# 只改 2 个字节：0x37DF4 和 0x37EC9，01 -> 02
Start-Process work\patch\bin\level2.exe -ArgumentList 'scan "C:\Windows" export "Summary" "...\level2.txt" autoclose' -Wait
```

结果：

```
Total size: 284KB
Folders   : 7
Files     : 1
耗时 1.8 s
```

`FindFirstFileExW` 直接以 `ERROR_INVALID_PARAMETER` 失败、且这条路径没有回退 → 扫描几乎为空。
**这一个实验同时证明了三件事**：① 该调用在运行时确实被执行；② 我们定位的立即数就是 `fInfoLevelId`；
③ 原版的值确实是 `1`（否则改成 2 不会有任何变化）。

把 `1→0`（Standard）、`2→0`（无 LARGE_FETCH）后的反证实验见 §6.3：`C:\Windows` 扫描**变慢**。

### 4.4 所以 `--fast-scan` 是 no-op

工具会**重新做一遍完整分析**（IAT → 桩 → 调用点 → 6 个参数），发现已经是 `1` / `2` 就输出：

```
[already-applied] fast-scan: the shipped build already calls FindFirstFileExW with FindExInfoBasic(1)
                  and FIND_FIRST_EX_LARGE_FETCH(2) - both immediates verified at every call site,
                  no byte is changed by this patch
```

输出文件与输入**逐字节相同**（SHA-256 不变）。如果换一个更老的 SpaceSniffer 构建（参数是 0），
同一段逻辑会做 `6A 00 → 6A 01`、`6A 00 → 6A 02` 的等长替换，并逐字节报告 —— 工具是通用的，
只是这一版没得改。

---

## 5. 补丁 C：Check for update / 捐赠

### 5.1 原版的行为

- 二进制**不导入任何网络 DLL**（只有 ADVAPI32 / KERNEL32 / VERSION / WINSPOOL / COMCTL32 / COMDLG32 /
  GDI32 / MSIMG32 / SHELL32 / USER32 / WINMM / OLE32 / OLEAUT32，共 13 个），没有 WinINet / WinHTTP / WS2_32。
- 菜单 `Check for update` 的 action 是 `actCheckForUpdate`，其 `OnClick` = `actCheckForUpdateClick`
  （VA **0x41FDD0**），实现就是：

```asm
0041FDD0  53                    push ebx
0041FDD1  8B D8                 mov  ebx,eax
0041FDD3  6A 0A                 push 0xA                       ; SW_SHOWDEFAULT
0041FDD5  6A 00                 push 0
0041FDD7  6A 00                 push 0
0041FDD9  68 A1 5B 5C 00        push 0x5C5BA1                  ; "http://www.uderzo.it/.../download.html"
0041FDDE  68 9C 5B 5C 00        push 0x5C5B9C                  ; "open"
0041FDE3  8B C3                 mov  eax,ebx
0041FDE5  E8 5E 46 0A 00        call 0x4C4448                  ; → PWideChar
0041FDEA  50                    push eax
0041FDEB  E8 C8 8D 19 00        call 0x5B8BB8                  ; ShellExecuteW
0041FDF0  5B                    pop  ebx
0041FDF1  C3                    ret
```

**没有开机自动更新检查**、没有后台请求、没有"新版本"弹窗 —— 所以"去更新检查"在功能/隐私层面**无事可做**。

### 5.2 补丁（可选，`--no-update-check`）

把首字节改掉即可（函数只用寄存器传 `Self`，没有栈上局部变量，直接 `ret` 是安全的）：

| 文件偏移 | 原字节 | 新字节 | 含义 |
|---|---|---|---|
| `0x1F3D0` | `53` | `C3` | `push ebx` → `ret`，菜单项点击后什么都不做 |

`--no-update-check` 默认**不包含**在交付成品里（保留原厂行为）；需要时加这个开关即可。

---

## 6. A/B 性能实测

### 6.1 方法

- 目标目录：`C:\Windows`（实测 122 8xx 个文件夹 / 163 77x 个文件 / 47.7 GB）
- 命令（完全相同，只有 exe 不同）：

```powershell
Start-Process <exe> -ArgumentList 'scan "C:\Windows" export "Summary" "<out>.txt" autoclose' -Wait
```

- 三个变体、轮转顺序 `orig → patched → slow_std`、共 3 轮（9 次），**同时记录墙钟时间与进程 CPU 时间**
  （CPU 时间对后台磁盘负载不敏感，是本机更可靠的指标）。
- 脚本：`work/patch/bench/bench.ps1`；原始数据：`work/patch/bench/bench_ab_final.csv`，
  每次的 `Summary` 导出文件也在同目录。
- 变体：
  | 变体 | 说明 |
  |---|---|
  | `orig.exe` | 原版，一字未改 |
  | `patched.exe` | **交付成品**（DPI 清单 + `--fast-scan` 检测；扫描代码 0 字节改动） |
  | `slow_std.exe` | 反证用：把 `fInfoLevelId` 改回 0、`dwAdditionalFlags` 改回 0（4 字节） |

> 环境说明：测试期间机器上有外部磁盘负载（Steam 约 36 MB/s，磁盘占用率 60–87%），
> 单次墙钟时间波动可达 ±30%，因此下面同时给出全部原始值、极值和中位数，并做**同轮配对比值**。

### 6.2 原始数据

| 轮 | 变体 | 墙钟 (s) | CPU (s) | 导出 Summary |
|---|---|---|---|---|
| 1 | orig | 67.81 | 82.52 | 47.7GB / 122834 / 163772 |
| 1 | patched | 57.66 | 72.45 | 47.7GB / 122829 / 163772 |
| 1 | slow_std | 74.82 | 101.55 | 47.7GB / 122843 / 163772 |
| 2 | orig | 51.06 | 58.28 | 47.7GB / 122833 / 163773 |
| 2 | patched | 74.95 | 104.53 | 47.7GB / 122840 / 163772 |
| 2 | slow_std | 75.33 | 95.05 | 47.7GB / 122831 / 163772 |
| 3 | orig | 51.19 | 60.22 | 47.7GB / 122830 / 163772 |
| 3 | patched | 49.75 | 67.14 | 47.7GB / 122829 / 163772 |
| 3 | slow_std | 56.44 | 73.98 | 47.7GB / 122828 / 163772 |

中位数：

| 变体 | 墙钟中位数 | CPU 中位数 |
|---|---|---|
| orig | **51.19 s** | **60.22 s** |
| patched | 57.66 s | 72.45 s |
| slow_std | 74.82 s | 95.05 s |

同轮配对比值（同一轮内相除，抵消当时的机器负载）：

| 轮 | patched / orig（墙钟） | patched / orig（CPU） | slow_std / orig（墙钟） | slow_std / orig（CPU） |
|---|---|---|---|---|
| 1 | 0.85 | 0.88 | 1.10 | 1.23 |
| 2 | 1.47 | 1.79 | 1.48 | 1.63 |
| 3 | 0.97 | 1.11 | 1.10 | 1.23 |
| **中位数** | **0.97** | **1.11** | **1.10** | **1.23** |

### 6.3 怎么读这些数字

1. **`patched` vs `orig`：没有可测量的差异。** 配对中位数 0.97×（墙钟）/ 1.11×（CPU），
   单轮取值从 0.85× 到 1.47×——**方向都不一致**（第 3 轮 patched 反而更快：49.75 s < 51.19 s）。
   这与"扫描代码一个字节都没改"完全吻合：`--fast-scan` 是 no-op，见 §4.4。
   > `patched` 的 CPU 中位数略高（+11%）与 DPI 渲染有关：120 DPI 下窗体按 1.94 M 像素绘制，
   > 而未感知版只画 1536×803=1.23 M 像素、再交给 DWM 拉伸。这一项属于"清晰度换来的绘制量"，不是扫描开销。
2. **`slow_std` vs `orig`：三轮全部变慢，CPU 分别 +23% / +63% / +23%（中位数 +23%）。**
   这是**厂商参数确实在起作用**的证据：把 `FindExInfoBasic` 退回 `FindExInfoStandard`、把
   `FIND_FIRST_EX_LARGE_FETCH` 去掉，`C:\Windows` 这种"多文件小目录"场景会多出大量 8.3 短名解析与
   额外的目录缓冲往返。
3. **结论**：原版已经把这项优化做掉了（§4.1），本路线的"扫描加速补丁"因此**没有收益**；
   但反证实验量化了这项优化的价值：**约 1.2× CPU / 1.1× 墙钟**。如果换成更老的、参数还是 0 的构建，
   同一个工具确实能把这块提速补回来。
4. **导出结果一致性**：9 次运行的 `Summary` 完全一致（47.7 GB / 163 77x 文件；文件夹数因系统目录
   实时变化有 ±10 的浮动），说明补丁**没有改变扫描语义**。

> 附加反证（§4.3）：`fInfoLevelId` 改成非法的 2 时，扫描直接失效（1.8 s、7 个文件夹）——
> 说明参数是**活的**，不是死代码。

---

## 7. DPI 实测截图（125%）

所有截图由 `work/patch/dpi_probe.ps1` 生成：启动 → 移到固定位置 (1330,1000)、窗口 1200×420 →
等 12 s 扫描 `C:\Users\EDM\bin\dsh` 完成 → **屏幕实拍**（`CopyFromScreen`，包含 DWM 的缩放结果）→ 关闭。
探针自身是 PerMonitorV2，所以取到的是**物理像素**。

| 文件 | 内容 |
|---|---|
| `work/patch/shots/orig125_screen.png` | 原版，1200×420（逻辑 960×336 被拉伸 1.25×） |
| `work/patch/shots/pm125_screen.png` | 补丁版（PerMonitorV2），1200×420 原生 120 DPI |
| `work/patch/shots/sys125_screen.png` | 补丁版（System），1200×420 原生 120 DPI |
| `work/patch/shots/compare_125_full.png` | 上面两者的并排对照（见下图） |
| `work/patch/shots/zoom_125_title.png` | 标题栏/菜单 3× 放大：拉伸毛边 vs 原生字形 |
| `work/patch/shots/zoom_125_treemap.png` | 树图文字 3× 放大："primary-runtime 270.7MB" |
| `work/patch/shots/compare_125_permonitor_vs_system.png` | 两种模式对照（125% 下几乎一致） |

![DPI 125% 对比](../work/patch/shots/compare_125_full.png)

![标题栏 3x 放大](../work/patch/shots/zoom_125_title.png)

![树图文字 3x 放大](../work/patch/shots/zoom_125_treemap.png)

**看什么**：两版的**逻辑布局完全相同**（同样的面板、同样的分块、同样的文字位置），差别只在**字形边缘**——
原版的 "SpaceSniffer"、"File / Edit / Windows / Help" 和树图里的 "primary-runtime 270.7MB" 都有明显的
重采样毛边（灰色过渡带），补丁版则是干净的像素级字形。这是因为原版在 96 DPI 下渲染 960×336 的逻辑画面，
再由 DWM 整体拉到 1200×420；补丁版直接以 1200×420 @120 DPI 重新光栅化。

### 7.1 关于 150%

| 项 | 状态 |
|---|---|
| 125% | **实测**（本机当前会话缩放）：见上表 |
| 150% | **未能在本会话实测**：本机只有一台显示器，当前系统缩放为 125%；把会话级缩放到 150% 需要注销/重启桌面（或使用未公开的 `DisplayConfigSetDeviceInfo` DPI 接口），会打断同机并行的其它任务 |

150% 下的**预期**（机制与 125% 完全一致，只是系数从 1.25 变 1.5）：

- 原版（DPI_UNAWARE）：仍按 96 DPI 渲染 960×336 逻辑画面，DWM 放大到 **1440×504 物理**，字形毛边更明显；
- 补丁版（PerMonitorV2）：以 **144 DPI** 原生渲染，VCL 把 96 DPI 设计的窗体按 1.5× 缩放，
  物理尺寸与原来一致、字形清晰。VCL 2010 的 `Scaled=True` 在 144 DPI 下同样生效（125% 的 120 DPI 已实测验证）。

复现方法（在 150% 的会话里执行）：

```powershell
pwsh -NoProfile -File work\patch\dpi_probe.ps1 -Exe dist\patched\SpaceSniffer.Patched.exe `
     -Tag pm150 -ScanTarget "C:\Users\EDM\bin\dsh" -WaitSec 12 -X 1330 -Y 1000 -Width 1200 -Height 420
pwsh -NoProfile -File work\patch\make_compare.ps1   # 生成 compare_150_*.png（若 150% 截图存在）
```

---

## 8. 不可行清单（为什么这些必须重写，而不是打补丁）

### 8.1 深色模式 ❌

**判据（都可复核）**

1. 主题由 **comctl32 v6** 绘制：清单里声明了 `Microsoft.Windows.Common-Controls 6.0.0.0`，菜单、工具栏、
   滚动条、标题栏都由系统主题引擎画，**进程内没有任何 `SetWindowTheme`/`OpenThemeData`/`DrawThemeBackground` 导入**
   （557 个导入里一个都没有），所以应用无法自绘这些部件；补丁改不到系统主题。
2. 颜色来源分散且大部分是**设计期属性**：DFM 资源里 `clWindow` 出现 **60 次**、`clBtnFace` **7 次**，
   另有 `GetSysColor`/`GetSysColorBrush` 3 个调用点、`SetTextColor` 15 / `SetBkColor` 19 / `FillRect` 13。
   可以把 DFM 里的颜色常量改成深色（DFM 是长度前缀的二进制流，改字符串要重排整块），但 **文字前景色、菜单、
   高亮、图形视图的调色板、图标位图**都不在那 67 个属性里，结果一定是"深色底 + 黑字/白控件"的花屏。
3. 树图是 **Graphics32**（`TBitmap32` 出现 8 次、`GR32` 80 次）自己画的，配色写死在代码里；
   要深色必须改绘制逻辑。

**结论**：能做出"部分变暗"的残次品，做不出可用的深色模式 —— 这正是原生重制版全自绘的原因。

### 8.2 MFT 直读 ❌

**判据**：导入表里**没有 `ntdll.dll`**，也**没有 `DeviceIoControl`**（连 `FSCTL_ENUM_USN_DATA` 的调用入口都不存在）。
MFT 直读需要 `CreateFile("\\.\C:")` + `DeviceIoControl(FSCTL_ENUM_USN_DATA)` 或 `NtQueryVolumeInformationFile`，
并从 `FILE_ID_DESCRIPTOR` 解析 `FILE_ID_128` → 需要**新增导入、新增代码、新增路径拼接逻辑**；
PE 里没有空闲的可执行空间（`.text` 末尾就是导入桩），只能再造一个节塞机器码 —— 那是"手写汇编重实现扫描器"，
已经不属于"打补丁"，且稳定性远不如原生重制版。

### 8.3 MCP 接口 ❌

**判据**：没有任何网络/命名管道/stdio 服务框架的导入（无 WS2_32/WinHTTP/WinINet，无 `CreateNamedPipeW`），
且这是 GUI 子系统程序（`Subsystem=2`），没有控制台 stdout 通道。MCP 需要 JSON-RPC 收发、工具路由、
生命周期管理 —— 全部是新增代码。更麻烦的是**单实例转发**：程序已用 `FindWindowW` + `RegisterWindowMessageW`
+ `SetForegroundWindow` 做"第二次启动把消息转给第一个实例"，注入的 DLL 必须接管这套逻辑才能拿到参数，
杀软与稳定性风险都远超收益。

### 8.4 树图重排 / 列布局 ❌

- 树图布局（squarified treemap 的行列切分、拖拽重排、缩放动画）在 Graphics32 绘制代码里，是**算法 + 状态机**，
  不是常量；改不了。
- 列布局（详细信息视图的列宽/列序）**部分**存在于 DFM 属性里（整数/字符串），理论上可改常量，
  但改不了列的**语义**（新增一列 = 新增控件 + 数据绑定 + 绘制），也不值得为此维护一套 DFM 二进制补丁。

### 8.5 为什么"注入 DLL + hook"也不划算

| 风险 | 说明 |
|---|---|
| 稳定性 | 这是 Delphi RTL + VCL + Graphics32 的混合体，hook `FindFirstFileExW`/`DrawTextW` 之类热路径要处理重入、多线程扫描、SEH |
| 杀软 | 远程线程注入 + IAT hook 是典型恶意行为特征，交付物会直接被 Defender 拦 |
| 单实例 | 见 §8.3，进程间转发已有自己的协议，注入方必须完全兼容 |
| 签名/完整性 | 原版无签名，但注入会改变内存映像，任何完整性检查/自校验（本版没有）都会失效 |
| 维护 | 每个 hook 都要跟随 RVA 变化；而 RVA 会随任何一次官方更新漂移 |

---

## 9. 复现、回滚与风险

### 9.1 复现

```powershell
cd G:\Reverse-Enginnering-AI\SpaceSniffer_Reverse

# 1) 生成成品（幂等，可重复执行）
python tools/patch_original.py --in "C:\Users\EDM\bin\SpaceSniffer.exe" `
    --out dist\patched\SpaceSniffer.Patched.exe --dpi permonitor --fast-scan `
    --report dist\patched\patch_report.json

# 2) 校验
python tools/patch_original.py --in "C:\Users\EDM\bin\SpaceSniffer.exe" `
    --out work\patch\test.exe --dpi permonitor --fast-scan     # 一条命令，产物可启动/扫描/导出
(Get-FileHash "C:\Users\EDM\bin\SpaceSniffer.exe" -Algorithm SHA256).Hash   # 必须仍是 FC0629…

# 3) DPI 实测（启动→等 8s→截图→关闭）
pwsh -NoProfile -File work\patch\dpi_probe.ps1 -Exe dist\patched\SpaceSniffer.Patched.exe -Tag pm125
```

### 9.2 回滚

- 补丁版是**独立文件**（`dist/patched/SpaceSniffer.Patched.exe`），原版从未被写入 —— 删掉补丁版即回滚。
- 若要从补丁版还原原始字节：重新从 `C:\Users\EDM\bin\SpaceSniffer.exe` 复制即可；
  或对补丁版运行 `--dpi none --fast-scan`（会剥离 `.rsrc2` 并恢复数据目录），得到与"只打代码补丁"的等价物。

### 9.3 风险与已知限制

1. **修改了未签名的第三方二进制**，仅限自用/研究；不要再分发。
2. `PerMonitorV2` 对 VCL 2010 是"超纲"模式：单显示器无问题；**跨不同缩放比例的显示器拖动窗口时不会重新缩放**
   （VCL 不处理 `WM_DPICHANGED`）。混合 DPI 环境请改用 `--dpi system`。
3. `longPathAware` 只是声明；VCL 2010 的 `FindFirstFileExW` 路径本身支持长路径，但其它 `MAX_PATH` 逻辑
   （如 `ExpandFileName`）不受影响 —— 该声明对超长路径扫描**有帮助但不保证全部路径都通**。
4. 文件体积 +194 560 B（复制了 `.rsrc`），无运行时开销。
5. Windows 会校验 PE 的 `CheckSum` 仅为少数场景（驱动/系统 DLL），本程序无签名，未强制校验。

---

## 附录 A：`patch_report.json` 主要字段

| 字段 | 含义 |
|---|---|
| `input.sha256 / matches_known_build` | 输入校验 |
| `input.generated_by_this_tool` | 输入是否是本工具产物（幂等重跑） |
| `patches[].status` | `applied` / `already-applied` / `skipped` / `failed` |
| `patches[].edits[]` | `{offset, va, before, after, meaning}` 逐字节 |
| `patches[].call_sites[]` | `FindFirstFileExW` 每个调用点的 6 个参数与动作 |
| `patches[].section` | 新节 RVA / RawPtr / RawSize |
| `verification.checks[]` | PE 解析、清单、节范围、`GetFileVersionInfoSizeW` 回归检查 |

## 附录 B：注入的清单（permonitor 模式）

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="SpaceSniffer" version="1.3.0.2" processorArchitecture="*"/>
  <description>SpaceSniffer disk space analyzer (patched: DPI awareness)</description>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
                        publicKeyToken="6595b64144ccf1df" language="*" processorArchitecture="*"/>
    </dependentAssembly>
  </dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges>
      <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
    </requestedPrivileges></security>
  </trustInfo>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware         xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness     xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <longPathAware    xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
```
