SpaceSniffer 2.0（原生重制版）
=================================

SpaceSniffer.exe      磁盘空间分析 GUI（原生 Win32，单文件，无运行库依赖）
SpaceSniffer.Mcp.exe  MCP stdio 服务器，供 AI Agent 调用

用法
----
双击 SpaceSniffer.exe，在开始页选择驱动器或输入路径并回车；树图按占用面积展示，
矩形越大占用越多，双击进入子目录，顶部 Filter 栏可用过滤器语法筛选。

命令行（与原版兼容）：
  SpaceSniffer.exe scan "D:\" export "File report" "D:\report.txt" autoclose
  SpaceSniffer.exe load "snapshot.ssx" filter "*.iso;>1gb"

快捷键：
  Ctrl+N 新建视图        Backspace 后退        Shift+Backspace 前进
  Ctrl+Home 回到根       Ctrl+Up 上一层        Ctrl +/- 细节层级
  Ctrl+F 剩余空间        Ctrl+U 未知空间       Ctrl+T 文件类着色
  Ctrl+1..4 打标签       Ctrl+0 清除标签       F5 刷新   F1 帮助

MCP 客户端配置示例
------------------
{
  "mcpServers": {
    "spacesniffer": { "command": "<本目录>\\SpaceSniffer.Mcp.exe" }
  }
}

提供的 MCP 工具（全部只读，不会删除文件）：
  scan, list_children, largest_files, largest_folders, search, tree,
  file_types, duplicates, cleanup_candidates, volume_info, export_report

配置文件
--------
首次运行会在同目录生成 SpaceSnifferConfig.xml（元素名与原版 1.3.0.2 一致），
可直接编辑，重启生效：
  darkMode            深色模式
  useMftScanner       使用 NTFS $MFT 直读（需管理员权限）
  scanADS             扫描 NTFS 备用数据流
  showFreeSpace / showUnknownSpace
  minElementSizePx    最小元素尺寸 3-20
  initialDetailLevel  初始嵌套层级 1-15
  fontSize            元素字体 4-20
  levelContrast / borderContrast  层级/边框对比度 0-50

NTFS $MFT 直读
--------------
以管理员身份运行时会自动尝试直接解析 $MFT（WizTree 同款思路），比目录枚举快数倍；
非管理员或非 NTFS 卷自动回退并行目录枚举，功能不变。
