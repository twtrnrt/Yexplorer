// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuApps.h
//
// 把 owner 缓存（一个 handler 一条）归成「应用 + 来源类型」：
//
//   WPS Office（经典）  ← kwpsshellext64.dll 等经典 handler
//   WPS Office（现代）  ← wpsappext1 现代包
//   7-Zip（经典）
//   TortoiseSVN（经典 / 现代）
//
// 归组只到应用层，不采功能项、不碰扩展名；功能项在右键展开时 live capture。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "shell/MenuInventory.h"

namespace shell
{

// 来源类型（设置里括号中的文字；右键菜单里不显示）
enum class AppSourceKind
{
    Classic = 0,
    Modern = 1,
};

struct AppSourceRow
{
    std::wstring key;        // 稳定键：appName + "|" + source
    std::wstring appName;    // 应用名（WPS Office / 7-Zip / …）
    AppSourceKind source = AppSourceKind::Classic;
    unsigned int scenes = 0; // 该来源在哪些场景出现（owner 的并集）
    std::vector<DynamicOwner> owners; // 该来源下可用的 owner（probe ok）
};

// 从 owner 缓存生成应用行。纯函数（不读盘、不调 COM）。
std::vector<AppSourceRow> BuildAppSourceRows(const std::vector<DynamicOwner>& owners);

// 运行时缓存：首次调用读 cache；没有 cache 时尝试重建一次。
// 右键弹出只读内存，不重新扫描。
const std::vector<AppSourceRow>& CachedAppSourceRows();
void InvalidateAppSourceRows();

// 按 key 查一行（找不到返回 nullptr）
const AppSourceRow* FindAppSourceRow(const std::wstring& key);

// 应用名 + 来源 → key
std::wstring AppSourceKey(const std::wstring& appName, AppSourceKind source);

} // namespace shell
