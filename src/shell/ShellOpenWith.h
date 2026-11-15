// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellOpenWith.h
//
// 「打开方式」的全部底层能力：
//   1. 从系统读出"谁能打开这个扩展名"的候选清单（Win32 程序 + 商店应用）；
//   2. 判断某条程序现在还在不在（卸载了没、磁盘拔了没）；
//   3. 用指定的程序打开一个文件（两种启动方式：CreateProcess / AUMID 激活）；
//   4. 按命令行模板拼参数（系统里的模板长什么样都有，不能硬拼）。
//
// 【什么时候调用 —— 这条很重要】
//   只有两个时候允许调本文件的"读系统"函数：
//     C. 用户打开「打开方式」设置窗口时（EnumerateCandidates / QuerySystemDefault）；
//     D. 真正打开文件时（IsAvailable / Launch）。
//   右键菜单弹出时绝对不要调任何读注册表、碰文件系统的函数——
//   用户要求"所有影响右键菜单打开速度的事情非必要坚决不要做"。
//   菜单里已卸载的程序靠读取配置文件里记好的 alive 字段来剔除，零成本。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/Settings.h"
#include "shell/ShellIcon.h"

namespace shell
{

// ---------------------------------------------------------------------------
// 扩展名规范化
//
// 文件路径 → 账本用的钥匙：
//   "C:\a\b\报告.TXT"  →  ".txt"（统一转小写，Windows 扩展名不分大小写）
//   "C:\a\b\README"    →  "(无扩展名)"
// ---------------------------------------------------------------------------
std::wstring ExtensionKeyForPath(const std::wstring& filePath);

// ---------------------------------------------------------------------------
// 这个扩展名要不要走「打开方式」
//
// 下列类型不接管，双击/右键照旧直接执行（否则双击 exe 会弹出"选择程序"）：
//   .exe .com .bat .cmd .msi .msp .scr .ps1 .vbs .js .jse .wsf .lnk .url .pif
// ---------------------------------------------------------------------------
bool IsExtensionHandled(const std::wstring& extension);

// ---------------------------------------------------------------------------
// 从系统读候选（只在打开设置窗口 / 首次询问时调）
//
// 读的来源（和系统自己"打开方式"面板用的那几处一致）：
//   1. HKCR\<扩展名> 的默认值（这个类型的默认 ProgID）
//   2. HKCR\<扩展名>\OpenWithProgids 里的 ProgID
//   3. HKCU\…\FileExts\<扩展名>\OpenWithProgids
//   4. HKCU\…\FileExts\<扩展名>\OpenWithList（值内容是 exe 文件名）
//   5. HKCR\Applications\<exe>\SupportedTypes 里声明支持这个扩展名的程序
//
// 能识别的两类程序：
//   Win32：HKCR\<ProgID>\shell\open\command 有默认值 → 取 exe + 参数模板
//   商店应用：没有默认值，但 \Application 或 \shell\open 里有 AppUserModelID
// 两者都没有的（打不开的壳）直接丢掉，不进候选。
// ---------------------------------------------------------------------------
std::vector<core::OpenWithItem> EnumerateCandidates(const std::wstring& extension);

// ---------------------------------------------------------------------------
// 系统当前默认程序（HKCU\…\FileExts\<扩展名>\UserChoice 的 ProgId）
//
// 找不到 / 解析不出可启动的程序就返回 false（比如系统默认是个商店应用而我们
// 只能从 ProgID 拿不到 AUMID 的情况——那种确实没法用）。
// ---------------------------------------------------------------------------
bool QuerySystemDefault(const std::wstring& extension, core::OpenWithItem& out);

// ---------------------------------------------------------------------------
// 程序还在不在
//
//   Win32：exe 路径可解析（SHCreateItemFromParsingName）
//   商店应用：shell:AppsFolder\<AUMID> 可解析
//
// 为什么要用 SHCreateItemFromParsingName 而不是 GetFileAttributes：
//   WindowsApps 目录普通用户没有读权限，GetFileAttributes 会返回"拒绝访问"，
//   会被误判成"卸载了"。走 shell 的解析路径不受这个 ACL 影响。
//
// diskMissing 输出：是不是"像是磁盘没连"（U 盘拔了、移动硬盘没插），
// 这种不能告诉用户"程序被卸载了"。网络路径一律不探测（断网时会卡几十秒）。
// ---------------------------------------------------------------------------
bool IsAvailable(const core::OpenWithItem& program, bool& diskMissing);

// ---------------------------------------------------------------------------
// 用指定程序打开文件
//
//   kind = "exe"：CreateProcess(exe, 按模板拼好的参数)，工作目录 = exe 所在目录
//   kind = "uwp"：IApplicationActivationManager::ActivateForFile(AUMID, 文件, "open")
//
// 失败时 error 里写一句能给用户看的原因（不含技术术语）。
// ---------------------------------------------------------------------------
bool Launch(HWND owner,
            const core::OpenWithItem& program,
            const std::wstring& filePath,
            std::wstring& error);

// ---------------------------------------------------------------------------
// 按系统默认关联打开（用户点"取消"、或还没设置默认程序时的兜底）
// ---------------------------------------------------------------------------
bool LaunchWithSystemDefault(HWND owner, const std::wstring& filePath);

// ---------------------------------------------------------------------------
// 取一条程序的图标索引（塞进 IconCache 的列表里用）
//   返回 -1 表示取不到（界面画个空白位，不崩）
// ---------------------------------------------------------------------------
int IconIndexFor(const core::OpenWithItem& program,
                 IconCache& icons,
                 IconCache::IconSize size = IconCache::IconSize::Small);

} // namespace shell
