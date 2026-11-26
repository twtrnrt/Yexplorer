// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuFlatten.h
//
// 第三方动态组菜单树的拍平/合并。
//
// 规则（用户定稿）：
//   · 2级里多个同名目录先合并子项（A）；
//   · 只有2级“唯一一个目录、没有任何其他项目”时才拍平；
//   · 有叶子、有多个目录、有多个项目 → 一律不拍平；
//   · 拍平后父项始终显示别名/应用名，不再拼「1级 · 2级」；
//   · 拍平后子项如果又是唯一一个目录，继续拍，最多 3 层。
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
