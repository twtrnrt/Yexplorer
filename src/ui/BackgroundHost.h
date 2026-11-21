// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/BackgroundHost.h
//
// 后台宿主进程（FileExplorer.exe --background）：
//   · 一个隐藏窗口 + 托盘图标
//   · 可选安装 Win+E 全局钩子
//   · 保留一个隐藏的 MainWindow 作为"热窗口"，Win+E 时直接显示
// 由设置里的「开机时后台启动 / 接管 Win+E」控制。
// ============================================================================

#pragma once

#include <windows.h>

namespace ui
{

// --background 入口：跑后台宿主的消息循环
int RunBackgroundHost(HINSTANCE instance);

// 启动一个后台宿主进程（已经有一个就直接返回 true）。
bool StartBackgroundHost();

// 让后台宿主退出（移除托盘/钩子/热窗口）。
void StopBackgroundHost();

// 无参数启动（任务栏/开始菜单/双击 exe）时，把请求交给后台宿主：
// 隐藏窗口直接显示，已显示则拉到前台；不新开进程。
void ActivateBackgroundHost();

// 设置变了：让后台宿主重新读配置（钩子开/关、热窗口路径）。
void ReloadBackgroundHost();

bool IsBackgroundHostRunning();

} // namespace ui
