#include "i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>

namespace ss::i18n {
namespace {

struct Entry
{
    const wchar_t* key;
    const wchar_t* zh;
    const wchar_t* en;
};

Lang g_lang = Lang::Zh;

constexpr Entry kTable[] = {
    // ---- window & dialog titles ----
    { L"app.title",          L"NeoSniffer 1.0.0",                  L"NeoSniffer 1.0.0" },
    { L"log.title",          L"NeoSniffer 日志",                   L"NeoSniffer Log" },
    { L"cfg.window_title",   L"设置 - NeoSniffer",                 L"Settings - NeoSniffer" },
    { L"help.filter.title",  L"过滤器帮助",                        L"Filter Help" },
    { L"help.tag.title",     L"标签帮助",                          L"Tag Help" },
    { L"help.cli.title",     L"命令行帮助",                        L"Command Line Help" },
    { L"about.title",        L"关于 NeoSniffer",                   L"About NeoSniffer" },
    { L"export.title",       L"NeoSniffer 导出",                   L"NeoSniffer Export" },

    // ---- menu bar ----
    { L"menu.file",          L"文件",                              L"File" },
    { L"menu.edit",          L"编辑",                              L"Edit" },
    { L"menu.view",          L"视图",                              L"View" },
    { L"menu.window",        L"窗口",                              L"Window" },
    { L"menu.help",          L"帮助",                              L"Help" },

    // ---- menu items ----
    { L"menu.file.new",      L"新建扫描视图",                      L"New Scan View" },
    { L"menu.file.export",   L"导出当前视图…",                     L"Export Current View..." },
    { L"menu.file.snapshot", L"保存快照…",                         L"Save Snapshot..." },
    { L"menu.file.exit",     L"退出",                              L"Exit" },
    { L"menu.edit.config",   L"设置…",                             L"Settings..." },
    { L"menu.view.refresh",  L"刷新当前视图",                      L"Refresh Current View" },
    { L"menu.view.filter",   L"显示/隐藏过滤器",                   L"Show/Hide Filter" },
    { L"menu.view.stop",     L"停止扫描",                          L"Stop Scan" },
    { L"menu.view.back",     L"后退",                              L"Back" },
    { L"menu.view.forward",  L"前进",                              L"Forward" },
    { L"menu.view.up",       L"上升一层",                          L"Up One Level" },
    { L"menu.view.home",     L"回到根",                            L"Go to Root" },
    { L"menu.view.detail_plus",  L"更多细节",                      L"More Detail" },
    { L"menu.view.detail_minus", L"更少细节",                      L"Less Detail" },
    { L"menu.view.style",    L"按文件类着色",                      L"Color by File Class" },
    { L"menu.view.free",     L"显示剩余空间",                      L"Show Free Space" },
    { L"menu.view.unknown",  L"显示未知空间",                      L"Show Unknown Space" },
    { L"menu.view.dark",     L"深色模式",                          L"Dark Mode" },
    { L"menu.window.log",    L"日志窗口",                          L"Log Window" },
    { L"menu.help.filter",   L"过滤器帮助",                        L"Filter Help" },
    { L"menu.help.tag",      L"标签帮助",                          L"Tag Help" },
    { L"menu.help.cli",      L"命令行帮助",                        L"Command Line Help" },
    { L"menu.help.about",    L"关于…",                             L"About..." },

    // ---- toolbar buttons & tooltips ----
    { L"tb.new",             L"新建",                              L"New" },
    { L"tb.new.tip",         L"新建扫描视图 (Ctrl+N)",             L"New Scan View (Ctrl+N)" },
    { L"tb.back",            L"后退",                              L"Back" },
    { L"tb.back.tip",        L"后退 (Backspace)",                  L"Back (Backspace)" },
    { L"tb.fwd",             L"前进",                              L"Forward" },
    { L"tb.fwd.tip",         L"前进 (Shift+Backspace)",            L"Forward (Shift+Backspace)" },
    { L"tb.up",              L"上层",                              L"Up" },
    { L"tb.up.tip",          L"上升一层 (Ctrl+Up)",                L"Up One Level (Ctrl+Up)" },
    { L"tb.home",            L"根",                                L"Root" },
    { L"tb.home.tip",        L"回到根 (Ctrl+Home)",                L"Go to Root (Ctrl+Home)" },
    { L"tb.refresh",         L"刷新",                              L"Refresh" },
    { L"tb.refresh.tip",     L"刷新当前视图 (F5)",                 L"Refresh Current View (F5)" },
    { L"tb.filter",          L"过滤",                              L"Filter" },
    { L"tb.filter.tip",      L"显示/隐藏过滤器 (Ctrl+L)",          L"Show/Hide Filter (Ctrl+L)" },
    { L"tb.detail",          L"细节",                              L"Detail" },
    { L"tb.detail_minus",    L"−",                                 L"−" },
    { L"tb.detail_minus.tip",L"更少细节 (Ctrl+-)",                 L"Less Detail (Ctrl+-)" },
    { L"tb.detail_plus",     L"+",                                 L"+" },
    { L"tb.detail_plus.tip", L"更多细节 (Ctrl++)",                 L"More Detail (Ctrl++)" },
    { L"tb.style",           L"文件类",                            L"Classes" },
    { L"tb.style.tip",       L"按文件类着色 (Ctrl+T)",             L"Color by File Class (Ctrl+T)" },
    { L"tb.free",            L"剩余",                              L"Free" },
    { L"tb.free.tip",        L"显示剩余空间 (Ctrl+F)",             L"Show Free Space (Ctrl+F)" },
    { L"tb.unknown",         L"未知",                              L"Unknown" },
    { L"tb.unknown.tip",     L"显示未知空间 (Ctrl+U)",             L"Show Unknown Space (Ctrl+U)" },
    { L"tb.export",          L"导出",                              L"Export" },
    { L"tb.export.tip",      L"导出当前视图",                      L"Export Current View" },
    { L"tb.snapshot",        L"快照",                              L"Snapshot" },
    { L"tb.snapshot.tip",    L"保存快照",                          L"Save Snapshot" },
    { L"tb.dark",            L"主题",                              L"Theme" },
    { L"tb.dark.tip",        L"切换深色模式",                      L"Toggle Dark Mode" },

    // ---- tabs ----
    { L"tab.start",          L"开始",                              L"Start" },
    { L"tab.new_view",       L"新视图",                            L"New View" },

    // ---- start page & admin hint banner ----
    { L"start.title",        L"选择要分析的空间",                  L"Select a Drive or Folder to Analyze" },
    { L"start.col.drive",    L"驱动器",                            L"Drive" },
    { L"start.col.label",    L"卷标",                              L"Label" },
    { L"start.col.fs",       L"文件系统",                          L"File System" },
    { L"start.col.space",    L"可用 / 总量",                       L"Free / Total" },
    { L"start.browse",       L"浏览…",                             L"Browse..." },
    { L"start.browse_title", L"选择要分析的文件夹",                L"Select Folder to Analyze" },
    { L"start.admin_title",  L"建议以管理员身份运行",              L"Run as Administrator Recommended" },
    { L"start.admin_desc",   L"可启用 NTFS $MFT 直读，扫描更快、更准确。", L"Enables direct NTFS $MFT reading for faster and more accurate scans." },
    { L"start.admin_restart",L"以管理员重启",                      L"Restart as Admin" },
    { L"start.admin_dismiss",L"不再提示",                          L"Don't Show Again" },

    // ---- view, filter & status bar ----
    { L"view.filter_label",  L"过滤",                              L"Filter" },
    { L"view.filter_btn",    L"过滤",                              L"Apply" },
    { L"view.scanning_prefix",   L"正在扫描 ",                     L"Scanning " },
    { L"view.scanning_ellipsis", L" …",                            L"..." },
    { L"view.status.scanning",       L"扫描中…",                   L"Scanning..." },
    { L"view.status.scanning_files", L" 文件",                     L" files" },
    { L"view.title.scanning",L"扫描中: ",                          L"Scanning: " },
    { L"view.title.scan",    L"扫描: ",                            L"Scan: " },
    { L"view.title.snapshot",L"快照: ",                            L"Snapshot: " },
    { L"view.status.files",  L" 文件 · ",                          L" files · " },
    { L"view.status.dirs",   L" 目录 · ",                          L" folders · " },
    { L"view.mode.mft",      L"MFT 直读",                          L"MFT Direct" },
    { L"view.mode.enum",     L"目录枚举",                          L"Dir Enum" },
    { L"view.status.scan_failed", L"扫描失败或被取消",             L"Scan failed or cancelled" },
    { L"view.status.load_failed", L"载入失败: ",                   L"Load failed: " },
    { L"status.ready",       L"就绪 — 选择驱动器或浏览文件夹",     L"Ready — Select a drive or browse for a folder" },
    { L"status.disk_usage",  L"磁盘占用 ",                         L"Disk Usage " },

    // ---- log messages ----
    { L"log.started",            L"NeoSniffer 已启动",             L"NeoSniffer started" },
    { L"log.opened",             L"日志窗口已打开",                L"Log window opened" },
    { L"log.cfg_saved",          L"配置已保存到 ",                 L"Settings saved to " },
    { L"log.scan_start",         L"开始扫描: ",                    L"Scan started: " },
    { L"log.scan_done",          L"扫描完成: ",                    L"Scan completed: " },
    { L"log.scan_failed",        L"扫描失败: ",                    L"Scan failed: " },
    { L"log.scan_stop_req",      L"已请求停止扫描",                L"Stop scan requested" },
    { L"log.snap_loaded",        L"已载入快照: ",                  L"Snapshot loaded: " },
    { L"log.snap_load_failed",   L"载入快照失败: ",                L"Failed to load snapshot: " },
    { L"log.snap_none",          L"没有可保存的视图",              L"No active view to save" },
    { L"log.snap_saved",         L"快照已保存: ",                  L"Snapshot saved: " },
    { L"log.snap_save_failed",   L"保存快照失败",                  L"Failed to save snapshot" },
    { L"log.filter_applied",     L"应用过滤器: ",                  L"Filter applied: " },
    { L"log.detail_level",       L"细节层级: ",                    L"Detail level: " },
    { L"log.style",              L"样式: ",                        L"Style: " },
    { L"log.export_done",        L"导出完成: ",                    L"Export completed: " },
    { L"log.export_failed",      L"导出失败: ",                    L"Export failed: " },
    { L"log.auto_export_done",   L"自动导出完成: ",                L"Auto export completed: " },
    { L"log.auto_export_failed", L"自动导出失败: ",                L"Auto export failed: " },
    { L"log.shell_reg_ok",       L"资源管理器右键菜单已注册",      L"Explorer context menu registered" },
    { L"log.shell_unreg_ok",     L"资源管理器右键菜单已移除",      L"Explorer context menu removed" },
    { L"log.shell_reg_failed",   L"资源管理器右键菜单操作失败 (错误码 ", L"Explorer context menu operation failed (error " },
    { L"log.runas_cancelled",    L"已取消以管理员身份重启，继续以当前权限运行", L"Administrator restart cancelled; continuing with current privileges" },
    { L"log.runas_failed",       L"以管理员身份重启失败 (错误码 ",  L"Failed to restart as administrator (error " },
    { L"shell.menu_label",       L"使用 NeoSniffer 分析占用",      L"Analyze with NeoSniffer" },

    // ---- file dialogs & export messages ----
    { L"dlg.snap_filter_ssx",    L"NeoSniffer 快照 (*.ssx)",       L"NeoSniffer Snapshot (*.ssx)" },
    { L"dlg.snap_filter_du",     L"DU 文本 (*.du)",                L"DU Text (*.du)" },
    { L"dlg.export_filter_txt",  L"文本 (*.txt)",                  L"Text (*.txt)" },
    { L"dlg.export_filter_csv",  L"CSV (*.csv)",                   L"CSV (*.csv)" },
    { L"dlg.export_filter_all",  L"所有文件 (*.*)",                L"All Files (*.*)" },
    { L"export.no_view",         L"当前没有可导出的视图。",        L"There is no active view to export." },
    { L"export.done_msg",        L"导出完成:\n",                   L"Export completed:\n" },
    { L"export.fail_msg",        L"导出失败:\n",                   L"Export failed:\n" },
    { L"export.cfg_not_found",   L"找不到导出配置: ",              L"Export configuration not found: " },

    // ---- treemap tooltip ----
    { L"tip.disk_size",          L"磁盘占用 ",                     L"Disk " },
    { L"tip.logical_size",       L" · 逻辑大小 ",                  L" · Logical " },
    { L"tip.children_suffix",    L" 个子项",                       L" items" },
    { L"tip.modified",           L"修改时间 ",                     L"Modified " },
    { L"tip.age_open",           L"（年龄 ",                       L" (Age " },
    { L"tip.age_close",          L"）",                            L")" },
    { L"tip.age_prefix",         L"年龄 ",                         L"Age " },
    { L"tip.age.just_now",       L"刚刚",                          L"just now" },
    { L"tip.age.min",            L" 分钟",                         L" min" },
    { L"tip.age.hour",           L" 小时",                         L" hr" },
    { L"tip.age.day",            L" 天",                           L" d" },
    { L"tip.age.month",          L" 个月",                         L" mo" },
    { L"tip.age.year",           L" 年",                           L" yr" },
    { L"tip.age.year_space",     L" 年 ",                          L" yr " },

    // ---- settings dialog: tabs & page titles ----
    { L"cfg.tab.geometry",       L"几何",                          L"Geometry" },
    { L"cfg.tab.animation",      L"动画",                          L"Animation" },
    { L"cfg.tab.behavior",       L"行为",                          L"Behavior" },
    { L"cfg.tab.appearance",     L"外观与配色",                    L"Appearance" },
    { L"cfg.tab.classes",        L"文件类",                        L"File Classes" },

    { L"cfg.title.geometry",     L"几何布局",                      L"Geometry & Layout" },
    { L"cfg.title.animation",    L"动画与渲染",                    L"Animation & Rendering" },
    { L"cfg.title.behavior",     L"扫描与行为",                    L"Scanning & Behavior" },
    { L"cfg.title.appearance",   L"外观与配色",                    L"Appearance & Colors" },
    { L"cfg.title.classes",      L"文件类规则",                    L"File Class Rules" },

    // ---- settings dialog: group headers ----
    { L"cfg.grp.geometry",       L"树图几何",                      L"TREEMAP GEOMETRY" },
    { L"cfg.grp.timing",         L"时间参数",                      L"TIMING" },
    { L"cfg.grp.smoothing",      L"平滑与缩放",                    L"SMOOTHING" },
    { L"cfg.grp.scanning",       L"扫描与显示",                    L"SCANNING & DISPLAY" },
    { L"cfg.grp.logging",        L"日志记录",                      L"LOGGING" },
    { L"cfg.grp.tooltips",       L"悬浮提示",                      L"TOOLTIPS" },
    { L"cfg.grp.theme",          L"主题与语言",                    L"THEME & LANGUAGE" },
    { L"cfg.grp.colors",         L"元素配色",                      L"ELEMENT COLORS" },

    // ---- settings dialog: row labels ----
    { L"cfg.min_elem_size",      L"最小元素尺寸",                  L"Min element size" },
    { L"cfg.init_detail",        L"初始细节层级",                  L"Initial detail level" },
    { L"cfg.font_size",          L"区块字体大小",                  L"Tile font size" },
    { L"cfg.proportions",        L"区块比例偏移",                  L"Tile proportion bias" },
    { L"cfg.refresh_delay",      L"刷新延迟",                      L"Refresh delay" },
    { L"cfg.zoom_time",          L"缩放动画时长",                  L"Zoom animation duration" },
    { L"cfg.real_zoom",          L"启用真实缩放动画",              L"Enable real zoom" },
    { L"cfg.dyna_smooth",        L"启用动态平滑过渡",              L"Enable dynamic smoothing" },
    { L"cfg.scan_ads",           L"扫描备用数据流 (ADS)",          L"Scan Alternate Data Streams" },
    { L"cfg.use_mft",            L"使用 MFT 快速扫描",             L"Use fast NTFS $MFT scanner" },
    { L"cfg.hide_admin_hint",    L"隐藏管理员运行提示",            L"Hide administrator prompt" },
    { L"cfg.always_admin",       L"始终以管理员身份运行",          L"Always run as administrator" },
    { L"cfg.shell_integration",  L"集成到资源管理器右键菜单",      L"Integrate into Explorer context menu" },
    { L"cfg.shell_registered",   L"已注册",                        L"Registered" },
    { L"cfg.shell_reg_failed_prefix", L"注册失败(错误码 ",         L"Registration failed (error " },
    { L"cfg.shell_reg_failed_suffix", L")",                        L")" },
    { L"cfg.show_free",          L"显示可用空间",                  L"Show free space" },
    { L"cfg.show_unknown",       L"显示未知空间",                  L"Show unknown space" },
    { L"cfg.use_sorting",        L"按体积大小排序",                L"Sort items by size" },
    { L"cfg.log_popup",          L"自动弹出日志窗口",              L"Auto-popup log window" },
    { L"cfg.log_issue",          L"记录访问异常",                  L"Log access issues" },
    { L"cfg.log_warning",        L"记录警告信息",                  L"Log warnings" },
    { L"cfg.tip_date",           L"提示中显示修改日期",            L"Show modified date in tooltip" },
    { L"cfg.tip_age",            L"提示中显示文件年龄",            L"Show file age in tooltip" },
    { L"cfg.dark_mode",          L"深色主题",                      L"Dark theme" },
    { L"cfg.language",           L"语言",                          L"Language" },
    { L"cfg.init_style",         L"初始视图样式",                  L"Initial view style" },
    { L"cfg.color.drive",        L"驱动器颜色",                    L"Drive color" },
    { L"cfg.color.folder",       L"文件夹颜色",                    L"Folder color" },
    { L"cfg.color.file",         L"文件颜色",                      L"File color" },
    { L"cfg.color.free",         L"可用空间颜色",                  L"Free space color" },
    { L"cfg.color.unknown",      L"未知空间颜色",                  L"Unknown space color" },

    // ---- settings dialog: segmented labels, table & buttons ----
    { L"cfg.lang.auto",          L"跟随系统",                      L"System" },
    { L"cfg.lang.zh",            L"中文",                          L"中文" },
    { L"cfg.lang.en",            L"English",                       L"English" },
    { L"cfg.style.flat",         L"纯色",                          L"Flat Colors" },
    { L"cfg.style.classes",      L"文件类",                        L"File Classes" },
    { L"cfg.enabled",            L"已启用",                        L"Enabled" },
    { L"cfg.disabled",           L"已禁用",                        L"Disabled" },
    { L"cfg.btn.add",            L"+ 新增",                        L"+ Add" },
    { L"cfg.btn.delete",         L"删除",                          L"Delete" },
    { L"cfg.classes.hint",       L"双击单元格编辑 · 点击色块改色",  L"Double-click to edit · Click swatch for color" },
    { L"cfg.classes.col.name",   L"名称",                          L"Name" },
    { L"cfg.classes.col.ext",    L"扩展名",                        L"Extensions" },
    { L"cfg.classes.col.color",  L"颜色",                          L"Color" },
    { L"cfg.classes.empty",      L"暂无文件类规则，点击“+ 新增”创建。", L"No file class rules. Click \"+ Add\" to create one." },
    { L"cfg.classes.new_name",   L"新分类",                        L"New Class" },
    { L"cfg.btn.ok",             L"确定",                          L"OK" },
    { L"cfg.btn.cancel",         L"取消",                          L"Cancel" },

    // ---- help & about text bodies ----
    {
        L"help.filter.body",
        L"NeoSniffer 过滤器语法（兼容原版 SpaceSniffer 1.3.0.2）\r\n\r\n"
        L"多个条件用分号分隔，条件之间是“与”关系；条件前加 | 表示取反。\r\n\r\n"
        L"按文件名      *.jpg            通配符 ?  *\r\n"
        L"按大小        >100kb  s>1mb  d<16k\r\n"
        L"              前缀 disksize|clustersize = 磁盘占用；filesize|logicalsize|size = 逻辑大小\r\n"
        L"按年龄        >2weeks  c>1month  m<10s\r\n"
        L"              前缀 create|access|modify\r\n"
        L"按标签        :red  :tag:red+yellow  :tag:all\r\n"
        L"按属性        :attr:a+h-s   :attr:a,h,-s\r\n"
        L"              短写 a s r h c e o t ni sp ads\r\n"
        L"按文件类      :class:Audio/Music\r\n"
        L"按文件夹名    \\temp   \\progr*\r\n\r\n"
        L"示例\r\n"
        L"  *.jpg;>100kb;>2weeks\r\n"
        L"  :red;:yellow\r\n"
        L"  |*.exe;|*.dll;\\*temp*;\\*windows*\r\n"
        L"  :attr:a+h-s\r\n\r\n"
        L"标签快捷键：Ctrl+1/2/3/4 红/黄/绿/蓝，Ctrl+0 清除，子元素继承父标签（继承色不渲染）\r\n",

        L"NeoSniffer Filter Syntax (compatible with SpaceSniffer 1.3.0.2)\r\n\r\n"
        L"Separate multiple conditions with semicolons (AND); prefix a condition with | to negate.\r\n\r\n"
        L"By filename    *.jpg            wildcards ?  *\r\n"
        L"By size        >100kb  s>1mb  d<16k\r\n"
        L"               prefix disksize|clustersize = allocated; filesize|logicalsize|size = logical\r\n"
        L"By age         >2weeks  c>1month  m<10s\r\n"
        L"               prefix create|access|modify\r\n"
        L"By tag         :red  :tag:red+yellow  :tag:all\r\n"
        L"By attribute   :attr:a+h-s   :attr:a,h,-s\r\n"
        L"               short forms: a s r h c e o t ni sp ads\r\n"
        L"By file class  :class:Audio/Music\r\n"
        L"By folder name \\temp   \\progr*\r\n\r\n"
        L"Examples\r\n"
        L"  *.jpg;>100kb;>2weeks\r\n"
        L"  :red;:yellow\r\n"
        L"  |*.exe;|*.dll;\\*temp*;\\*windows*\r\n"
        L"  :attr:a+h-s\r\n\r\n"
        L"Tag shortcuts: Ctrl+1/2/3/4 Red/Yellow/Green/Blue, Ctrl+0 clears all tags.\r\n"
    },
    {
        L"help.tag.body",
        L"标签\r\n\r\n"
        L"Ctrl+1 红色   Ctrl+2 黄色   Ctrl+3 绿色   Ctrl+4 蓝色\r\n"
        L"再次按下同一颜色可取消；Ctrl+0 清除当前视图内的全部标签。\r\n\r\n"
        L"子元素会继承父目录的标签颜色：继承色不绘制（避免视觉噪音），但参与 :tag: 过滤。\r\n"
        L"用标签可以隔离/排除元素，配合过滤器：\r\n"
        L"  :red          只看红色\r\n"
        L"  |:red         排除红色\r\n",

        L"Tags\r\n\r\n"
        L"Ctrl+1 Red   Ctrl+2 Yellow   Ctrl+3 Green   Ctrl+4 Blue\r\n"
        L"Press the same color again to clear; Ctrl+0 clears all tags in the active view.\r\n\r\n"
        L"Child elements inherit parent tag colors for :tag: filtering without visual clutter.\r\n"
        L"Combine tags with the filter bar to isolate or exclude items:\r\n"
        L"  :red          show only red items\r\n"
        L"  |:red         exclude red items\r\n"
    },
    {
        L"help.cli.body",
        L"命令行\r\n\r\n"
        L"  NeoSniffer.exe scan \"<path>\"                打开扫描视图\r\n"
        L"  NeoSniffer.exe load \"<file>\"                打开快照（.ssx/.sns/.du）\r\n"
        L"  NeoSniffer.exe filter \"<filter>\"            对前一个 scan/load 应用过滤\r\n"
        L"  NeoSniffer.exe export \"<name>\" \"<path>\"     导出（配置名区分大小写）\r\n"
        L"  NeoSniffer.exe autoclose                     导出后自动退出\r\n"
        L"  NeoSniffer.exe help                          本帮助\r\n\r\n"
        L"快捷：Ctrl+N 新建 · Backspace 后退 · Shift+Backspace 前进 · Ctrl+Home 根\r\n"
        L"      Ctrl+Up 上一层 · Ctrl+/- 细节 · Ctrl+F 剩余 · Ctrl+U 未知 · Ctrl+T 文件类\r\n",

        L"Command Line\r\n\r\n"
        L"  NeoSniffer.exe scan \"<path>\"                Open a scan view\r\n"
        L"  NeoSniffer.exe load \"<file>\"                Open a snapshot (.ssx/.sns/.du)\r\n"
        L"  NeoSniffer.exe filter \"<filter>\"            Apply filter to previous scan/load\r\n"
        L"  NeoSniffer.exe export \"<name>\" \"<path>\"     Export using template name\r\n"
        L"  NeoSniffer.exe autoclose                     Exit automatically after export\r\n"
        L"  NeoSniffer.exe help                          Show this help\r\n\r\n"
        L"Shortcuts: Ctrl+N New · Backspace Back · Shift+Backspace Forward · Ctrl+Home Root\r\n"
        L"           Ctrl+Up Up · Ctrl+/- Detail · Ctrl+F Free · Ctrl+U Unknown · Ctrl+T Classes\r\n"
    },
    { L"about.heading",      L"NeoSniffer",                                L"NeoSniffer" },
    { L"about.license",      L"MIT License · v1.0.0",                      L"MIT License · v1.0.0" },
    { L"about.repo_label",   L"开源仓库：",                                L"Repository: " },
    { L"about.repo_url",     L"https://github.com/yssite/neosniffer",      L"https://github.com/yssite/neosniffer" },
    { L"about.ok",           L"确定",                                      L"OK" },
    {
        L"about.body",
        L"原生 Win32 磁盘空间分析工具（非官方独立重制，不含原版代码）：\n"
        L"  · 原生高 DPI（PerMonitorV2）与深色 / 浅色模式\n"
        L"  · 并行目录枚举 + NTFS $MFT 直读双引擎\n"
        L"  · 兼容 SpaceSniffer 1.3.0.2 过滤器、导出模板、快照与配置格式\n"
        L"  · 内置 MCP 服务器供 AI Agent 只读分析磁盘占用",

        L"Native Win32 disk space analyzer (unofficial independent remake, no original code):\n"
        L"  · Native Per-Monitor V2 High DPI and Dark / Light Mode\n"
        L"  · Dual scan engine: parallel directory enum + direct NTFS $MFT reader\n"
        L"  · Compatible with SpaceSniffer 1.3.0.2 filters, export templates, snapshots & config\n"
        L"  · Built-in MCP server for read-only AI Agent disk analysis"
    },
};

} // namespace

void Init(const std::wstring& setting)
{
    if (_wcsicmp(setting.c_str(), L"zh") == 0)
    {
        g_lang = Lang::Zh;
        return;
    }
    if (_wcsicmp(setting.c_str(), L"en") == 0)
    {
        g_lang = Lang::En;
        return;
    }
    // "auto" or any unrecognized value: follow system UI language
    LANGID lid = GetUserDefaultUILanguage();
    g_lang = (PRIMARYLANGID(lid) == LANG_CHINESE) ? Lang::Zh : Lang::En;
}

Lang Current()
{
    return g_lang;
}

const wchar_t* T(const wchar_t* key)
{
    if (!key) return L"";
    for (const auto& e : kTable)
    {
        if (wcscmp(e.key, key) == 0)
        {
            if (g_lang == Lang::En)
            {
                // P1: never leak Chinese or return empty string in English mode.
                return (e.en && e.en[0] != L'\0') ? e.en : key;
            }
            if (e.zh && e.zh[0] != L'\0') return e.zh;
            if (e.en && e.en[0] != L'\0') return e.en;
            return key;
        }
    }
    // Unknown key: return key itself so English mode never gets Chinese or an empty label.
    return key;
}

} // namespace ss::i18n
