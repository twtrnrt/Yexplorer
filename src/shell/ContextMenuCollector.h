// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuCollector.h
//
// 运行时采集：把注册表查不到、藏在 DLL 里的真实右键菜单项文字读出来。
//
// 做法（和资源管理器弹出菜单走同一套 COM 接口）：
//   1. 准备临时样本文件 / 文件夹 / 背景目录；
//   2. SHCreateItemFromParsingName → BindToHandler(BHID_SFUIObject) 拿 IContextMenu；
//   3. QueryContextMenu 填进临时 HMENU，递归遍历菜单树，取真实文字、层级、动词；
//   4. 子菜单先用 IContextMenu2::HandleMenuMsg(WM_INITMENUPOPUP) 触发懒填充。
//
// 一期边界（已拍板）：
//   · 纯采集展示，不调用任何 InvokeCommand、不改注册表；
//   · 多选场景一期不采（构造 IDataObject 留二期）；
//   · 图标一期不采（二期再接 IExtractIcon）；
//   · 逐处理器精确归属留二期；一期按菜单一级文字分组展示。
// ============================================================================

#pragma once

#include <string>

#include "shell/ContextMenuData.h"

namespace shell
{

// ---------------------------------------------------------------------------
// 在 EnumerateRegistry() 产出的报告基础上，运行时采集 shellex 真实菜单项并补全。
//
// report 会被就地修改：采集到的扩展项按菜单一级文字归组写进 report.groups；
// 采集失败的场景 / 处理器在对应条目上标注 failed 和 failReason。
// ---------------------------------------------------------------------------
void CollectDynamicItems(ContextMenuReport& report);

// ---------------------------------------------------------------------------
// 采集结果缓存：%APPDATA%\FileExplorer\menu-cache.json
//
// 开窗先读缓存展示；点「刷新」才重新采集。缓存只是展示副本，二期配置不落这里。
// ---------------------------------------------------------------------------
bool LoadCache(ContextMenuReport& report);
void SaveCache(const ContextMenuReport& report);

} // namespace shell
