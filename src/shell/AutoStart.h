// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/AutoStart.h
//
// 开机启动项（HKCU\...\Run），用户级，不需要管理员。
// 写入的命令是：  "本程序全路径" --background
// ============================================================================

#pragma once

namespace shell
{

// 当前用户的开机启动项是不是指向本程序（且带 --background）
bool IsAutoStartEnabled();

// 勾选：写 Run 项；取消：删除 Run 项。
bool SetAutoStart(bool enabled);

} // namespace shell
