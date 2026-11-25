# 待办（Gemini 本轮结束后按序执行）

## 1. 版本号 1.0.0
- [ ] `native/app.rc`：加 `VS_VERSION_INFO`（FileVersion / ProductVersion = 1.0.0.0，
      ProductName = "SpaceSniffer Neo"，CompanyName 留空或 "SpaceSniffer Neo"，
      FileDescription = "Disk space analyzer"），保证 **资源管理器属性页** 能看到
- [ ] 标题栏：`SpaceSniffer Neo 1.0.0`（**去掉 "—— 磁盘空间分析"**）
- [ ] 关于窗口：`SpaceSniffer Neo 1.0.0`，下面一行 `基于 SpaceSniffer 1.3.0.2 逆向重制`
- [ ] MCP `serverInfo.version` = `1.0.0`

## 2. 工具栏「细节 −/+」放大
- [ ] 当前 9pt 字形 + 26px 格子 → **12–13pt + 30px 格子**，保持正方形、共用 1px 边框、光学居中
- [ ] 截图像素级确认居中（不要只看整体）

## 3. 最终代码审查（与 Gemini）
- [ ] 通读 `native/src/*`：明显 bug、资源泄漏、UB、跨线程、Win32 参数误用
- [ ] 重点看本轮新增：`i18n.*`（key 覆盖、缺 key 回退）、`app.rc`/图标加载、
      管理员提示条（`runas` 重启失败路径）、设置页改动
- [ ] 输出"已验收 / 未验收 / 已知限制"清单

## 4. 收尾
- [ ] `tools/pack.ps1` 重新打包 `dist/native/`
- [ ] README：功能清单、快捷键、MCP 配置、版本 1.0.0
