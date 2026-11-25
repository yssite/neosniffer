# NeoSniffer 1.1.0 交付状态

- **Windows 10 / 11**。桌面程序与 MCP 服务各为独立 EXE，发布包支持 x64 和 ARM64。
- **矩形图**。支持硬件加速时默认使用 Direct2D 和垂直同步。创建渲染目标失败时回退 GDI，也可以在「设置 → 外观」选择 GDI，重启后生效。
- **空间统计**。卷的总容量、已用和可用空间来自系统查询。扫描总量统计实际发现的文件，矩形图可单独展示剩余和未知空间。
- **MCP**。11 个 stdio 工具用于只读查询。报告文本通过 `export_report` 返回，桌面端和 CLI 支持生成报告文件。
- **验证**。`build.ps1 -Test` 运行合成 NTFS FILE 记录、run list、簇对齐、聚合统计和过滤器回归测试。GitHub Actions 在 `master` 的 push 与 PR 上执行 x64 验证。

合成记录验证覆盖普通文件、目录、修复序列校验、压缩长度与数据 run 解码。物理 4Kn 磁盘、特殊 UNC 路径及跨屏 DPI 行为仍依赖实机验证。

[CHANGELOG](../CHANGELOG.md) · [历史代码审查](CODE_REVIEW.md) · [历史格式分析](ANALYSIS.md)
