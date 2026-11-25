# NeoSniffer

[English](README.md) | **中文**

轻量原生 Win32 磁盘空间分析工具，支持 Direct2D 垂直同步矩形图、Per-Monitor V2 高 DPI、
深色模式、NTFS `$MFT` 直读和只读 MCP 服务。

[下载 Windows 版 NeoSniffer](https://github.com/yssite/neosniffer/releases/latest)

![NeoSniffer](docs/screenshot.webp)

## 特性

| 特性 | 说明 |
|---|---|
| 极致轻量 | 整个程序就是**一个约 1.7 MB 的 exe**：静态链接、免安装、无运行库、无第三方 DLL，只调用 Windows 系统 DLL。 |
| 高 DPI | Per-Monitor V2，每次绘制按当前客户区重算布局 |
| 深色 / 浅色 | 菜单栏、弹出菜单、提示卡片、对话框全部自绘 |
| NTFS `$MFT` 直读 | 直接解析 MFT run list；需管理员，失败自动回退到并行目录枚举 |
| 嵌套矩形图 | 支持硬件加速的设备默认使用 Direct2D 和垂直同步，可回退到 GDI |
| 过滤器 DSL | 例如 `*.iso;>1gb;>2weeks;:red` |
| 导出 | 模板 DSL（`{script}`、`if X ? {a} : {b}`、`leftpad/rightpad`） |
| 快照 | `.ssx`（自有）/ `.sns`（兼容 SpaceSniffer，只读）/ `.du`（文本） |
| 中英双语 | 默认跟随系统语言 |
| 资源管理器集成 | 文件夹 / 目录空白处 / 盘符右键「使用 NeoSniffer 分析占用」 |
| MCP 服务 | stdio JSON-RPC，11 个只读工具 |

## 环境要求

- Windows 10 / 11（x64 或 ARM64）
- 自行编译需要 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20.x（clang 20 + lld）

## 编译

```powershell
# x64
powershell -ExecutionPolicy Bypass -File build.ps1

# Windows on ARM（在 x64 上交叉编译）
powershell -ExecutionPolicy Bypass -File build.ps1 -Target arm64

# 单元测试
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
```

产物：`out\NeoSniffer.exe`、`out\NeoSniffer.Mcp.exe`。

## 使用

双击 exe 后选择驱动器，也可以把文件夹拖进窗口。
双击方块下钻，按 `Backspace` 返回上一层。
打开 **设置 → 外观与配色 → 矩形图渲染（重启生效）**，可在 Direct2D 和 GDI 之间切换。

命令行：

```powershell
NeoSniffer.exe scan D:\
NeoSniffer.exe scan D:\ scan E:\
NeoSniffer.exe load snapshot.ssx filter "*.iso;>1gb"
NeoSniffer.exe scan D:\ export "Summary" "report.txt" autoclose
```

## MCP 服务

`NeoSniffer.Mcp.exe` 通过 stdio 提供 JSON-RPC。工具：`scan`、`list_children`、
`largest_files`、`largest_folders`、`search`、`tree`、`file_types`、
`duplicates`、`cleanup_candidates`、`volume_info`、`export_report`。

从 1.1 起，`export_report` 接收 `path`、可选的 `config`（默认 `Summary`）和
`max_nodes`，通过 MCP 返回报告文本及截断标记 `truncated`。旧版 `output` 参数已移除，文件导出可通过桌面应用或命令行完成。
重复文件和清理结果仅供人工核对。扫描统计只计入已发现的文件，卷的已用、可用和总容量另行报告。

```json
{
  "mcpServers": {
    "neosniffer": {
      "command": "C:\\Tools\\NeoSniffer\\NeoSniffer.Mcp.exe",
      "args": []
    }
  }
}
```

## 已知限制

- 启动时取消 UAC，程序会以普通权限继续，并使用目录枚举。
- NTFS 解析器使用合成记录测试。原生 4Kn 磁盘和特殊 UNC 路径的实机验证有限。

## 免责声明

NeoSniffer 是**非官方、独立重新实现**，不含 Umberto Uderzo 原作 SpaceSniffer 的任何代码，
与原作者无隶属或背书关系；仅为实现互操作性而读取其快照与配置格式。

## 许可证

MIT，见 [LICENSE](LICENSE)。
