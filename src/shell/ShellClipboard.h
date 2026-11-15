// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellClipboard.h
//
// 系统剪贴板：文件和资源管理器互通。
//
// 【为什么不能只记在程序内存里】
//   只记在自己内存里的话，从资源管理器复制的东西粘不进来，
//   在本程序复制的东西也粘不出去。用户会立刻觉得"这软件不全"。
//
// 【用的什么格式】
//   CF_HDROP：Windows 放"一堆文件路径"的标准格式，资源管理器、
//             桌面、各种程序都认它，就这一个格式就能打通绝大部分场景。
//   "Preferred DropEffect"：一个 DWORD，告诉接收方这次是"复制"还是"剪切"。
//             没有它，接收方默认按复制处理，剪切就失效了。
//             （资源管理器按 Ctrl+X 时放的是 DROPEFFECT_MOVE）
// ============================================================================

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace shellclip
{

// 把一批路径放进系统剪贴板
// isCut：true = 剪切（接收方粘贴完会删源），false = 复制
bool WriteFiles(const std::vector<std::wstring>& paths, bool isCut, HWND owner);

// 把一段纯文字放进系统剪贴板（CF_UNICODETEXT）。
// 现在给右键「复制文件路径」用：多选时几条路径按行拼成一段文字。
bool WriteText(const std::wstring& text, HWND owner);

// 从系统剪贴板读出一批文件路径
// 剪贴板里不是文件（比如是一段文字）就返回 false
// isCut：读到的"首选效果"是不是移动
bool ReadFiles(std::vector<std::wstring>& paths, bool& isCut, HWND owner);

// 剪贴板里现在有没有文件（右键菜单判断"粘贴"要不要变灰用）
bool HasFiles();

// 清空剪贴板（剪切粘贴成功之后用，和资源管理器行为一致）
bool Clear(HWND owner);

} // namespace shellclip
