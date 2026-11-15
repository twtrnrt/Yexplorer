// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/DefaultManager.h
//
// 「把本程序注册成默认文件管理器」。
//
// 【这是哪一种"默认文件管理器"】
//   只接管「双击文件夹 / 双击磁盘」这一个动作：
//     · HKCU\Software\Classes\Directory\shell 的默认动词设成 "open"
//     · HKCU\Software\Classes\Directory\shell\open\command 设成本程序
//     · Drive 类同样两处（对应双击磁盘）
//
//   【为什么要改"默认动词"】
//     Win10 里 HKLM 的 Directory\shell、Drive\shell 默认值都是 "none"，
//     也就是这类东西没有默认动作、交给 Folder 类（explorer.exe）去打开。
//     只改 open\command 的话 shell 根本不会走到那条命令，
//     实测双击文件夹照样开系统资源管理器。必须先把默认动词设成 "open"。
//
//   【不影响的东西】
//     Folder 类（桌面、控制面板、库那些）一个值都没动，
//     所以桌面和任务栏照旧由 explorer.exe 提供；
//     Win+E、任务栏上那个「文件资源管理器」按钮、运行框里输路径回车
//     仍然打开系统资源管理器 —— 这些走的是另一条路，改这里管不到。
//
// 【为什么能一键恢复】
//   只写 HKCU（用户级覆盖），不需要管理员权限。
//   写之前把 4 个原本的值（有没有 + 是什么）备份到
//   HKCU\Software\FileExplorer\DefaultFileManager；「恢复系统默认」就是原样写回去，
//   原本没有的值就把我们写的删掉，系统自动回落到 HKLM 里的 explorer.exe。
//
// 【改完要通知系统】
//   ShellExecute 找默认动词时走缓存，写完调一次 SHChangeNotify(SHCNE_ASSOCCHANGED)
//   让 shell 立刻重新读，不用重启。
// ============================================================================

#pragma once

#include <windows.h>

namespace shell
{

// 本程序当前是不是"双击文件夹打开的那个程序"
bool IsDefaultFileManager();

// 注册为默认文件管理器（已经注册过就直接返回 true）
// 返回 false 表示写注册表失败，原因写进日志。
bool RegisterDefaultFileManager();

// 恢复成系统默认（写回备份；没有备份就删掉我们建的键，让系统回落 explorer.exe）
bool RestoreSystemFileManager();

} // namespace shell
