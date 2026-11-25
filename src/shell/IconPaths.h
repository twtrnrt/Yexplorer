// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/IconPaths.h
//
// 右键菜单图标缓存目录。
//
// 旧方案把第三方 handler 的菜单项图标采成文件放在这里；新方案复用同一套
// 「现场采集 + 落盘」逻辑，所以目录位置保持不变：
//   %APPDATA%\FileExplorer\menu-icons
// ============================================================================

#pragma once

#include <string>

namespace shell
{

// 返回图标目录（不存在会创建）；失败返回空串。
std::wstring IconDir();

} // namespace shell
