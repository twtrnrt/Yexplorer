// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuCollector.h
//
// 按场景采集系统右键菜单快照，并把每条菜单项渲染成位图（图标 + 文字）。
//
// 为什么这样做：
//   实测本机 434 个菜单项里只有 124 个（约 29%）在 QueryContextMenu 时自带
//   图标位图，WPS、7-Zip 的项全是空的。真实菜单的图标是外壳扩展在
//   WM_DRAWITEM 里自己画的（IContextMenu3::HandleMenuMsg2）。
//   所以这里不猜图标，直接让扩展把整行画到我们的内存 DC 上 —— 图标、文字、
//   灰态、加粗默认项，全都和系统菜单一致。
//
// 一期边界：纯采集展示，不执行命令、不写注册表。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "shell/ContextMenuData.h"

namespace shell
{

// 采集全部场景：文件（txt/docx/rar/zip 四个样本）、文件夹、空白区、多选。
// 结果写进 report，同时在 menu-icons 目录里生成每条菜单项的渲染图。
void CollectScenes(ContextMenuReport& report);

// 渲染图目录：%APPDATA%\FileExplorer\menu-icons
std::wstring IconDirPath();

// 采集结果缓存：%APPDATA%\FileExplorer\menu-cache.json
bool LoadCache(ContextMenuReport& report);
void SaveCache(const ContextMenuReport& report);

} // namespace shell
