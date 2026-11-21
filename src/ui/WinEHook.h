// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/WinEHook.h
//
// 全局低级键盘钩子：只拦 Win+E，其它键原样放行。
// 钩子装在独立线程上，避免主线程忙（枚举大目录）时拖慢全系统键盘。
// ============================================================================

#pragma once

#include <windows.h>

namespace ui
{

// 钩子命中后给宿主窗口发这条消息（无参数）
constexpr UINT kWinEMessage = WM_APP + 40;

// 启动钩子；notify 是接收 kWinEMessage 的窗口。
// 重复调用是安全的；失败返回 false（例如被安全软件拦截）。
bool StartWinEHook(HWND notify);

// 卸载钩子并等线程退出；没启动也算成功。
void StopWinEHook();

} // namespace ui
