// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuConfig.h
//
// 「我们的右键菜单」配置模型（界面阶段的产出，独立存 menu-config.json，
// 不碰 settings.json）。
//
// 结构：
//   组（Group）         —— 用户自建的一级菜单（如 WPS、7-Zip）
//   条目（Entry）       —— 从左侧拖进来的第三方项：
//                          · identity（身份键）：谁家的哪一项，跨语言/换版本也能对上
//                          · scope（范围）：在哪些场景、哪些扩展名生效
//                          · displayName（显示名）：可改名
//                          · iconFile（图标）：可换成自己的图
//
// 注意：落库只存身份与显示参数，**不存命令 ID**（命令 ID 每次生成都变）。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace shell
{

// 生效范围
struct ConfigScope
{
    unsigned int scenes = 0;              // MenuScene 位（文件/多选/文件夹/空白区）
    std::vector<std::wstring> exts;       // 扩展名（空 = 该场景内全部）
};

// 一条被配置进来的菜单项
struct ConfigEntry
{
    std::wstring id;              // 配置内唯一 ID（自增字符串）
    std::wstring groupId;         // 所属组
    std::wstring parentEntryId;   // 父条目（允许嵌套子菜单，空=组的直接子项）
    std::wstring pathKey;         // 身份键（来自普查）
    std::wstring brand;           // 软件品牌
    std::wstring sourceText;      // 原始文字模板（用于"恢复原名"）
    std::wstring displayName;     // 显示名（空=用 sourceText）
    std::wstring iconFile;        // 图标小图文件名（空=没有）
    ConfigScope  scope;           // 生效范围
    int          order = 0;       // 组内顺序
};

// 用户自建的一级组
struct ConfigGroup
{
    std::wstring id;
    std::wstring title;           // 组名（默认取软件名）
    std::wstring iconFile;        // 组图标（空=没有）
    int          order = 0;
};

// 整份配置
struct MenuConfig
{
    std::vector<ConfigGroup> groups;
    std::vector<ConfigEntry> entries;

    ConfigGroup* FindGroup(const std::wstring& id);
    ConfigEntry* FindEntry(const std::wstring& id);
    const ConfigGroup* FindGroup(const std::wstring& id) const;
    const ConfigEntry* FindEntry(const std::wstring& id) const;

    // 生成一个新的组 ID / 条目 ID
    std::wstring NewGroupId();
    std::wstring NewEntryId();
};

// 缓存文件：%APPDATA%\FileExplorer\menu-config.json
std::wstring MenuConfigPath();
bool LoadMenuConfig(MenuConfig& config);
bool SaveMenuConfig(const MenuConfig& config);

} // namespace shell
