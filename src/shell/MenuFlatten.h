// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuFlatten.h
//
// 第三方动态组菜单树的拍平/合并。
//
// 规则（用户定稿）：
//   · 2级里多个同名目录先合并子项；
//   · 合并后数“目录”（有子项的项），不数叶子；
//   · 只剩一个目录且名字和1级一样 → 去掉2级，子项直接挂1级；
//   · 只剩一个目录但名字不一样 → 1级名字改成「1级 · 2级」，子项直接挂1级；
//   · 多个目录 → 保留，只有同名目录合并。
// ============================================================================

#pragma once

#include <string>
#include <vector>

#include "shell/MenuInventory.h"

namespace shell
{

struct FlattenResult
{
    std::vector<MenuInventoryItem> items;
    std::wstring                   title;   // 最终1级显示名（可能变成「1级 · 2级」）
};

// displayTitle：右键上准备显示的名字（有别名时是别名）
// appName：应用原始名，用来判断 handler 返回的同名包装
FlattenResult NormalizeAppMenu(std::vector<MenuInventoryItem> items,
                               const std::wstring& displayTitle,
                               const std::wstring& appName,
                               int depth = 0);

} // namespace shell
