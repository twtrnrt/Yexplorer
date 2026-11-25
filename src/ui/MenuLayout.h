// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MenuLayout.h
//
// 「自定义右键菜单」的配置模型。
//
// 【这一版只做什么】
//   · 决定本程序自己的右键菜单里，哪些内置项显示、按什么顺序显示；
//   · 用户自己添加的快捷动作（程序 + 参数 + 适用场景）。
//
// 【明确不做什么】
//   · 不采集、不镜像、不调用第三方 Shell 扩展的右键菜单；
//   · 不做「系统菜单」兜底入口；
//   · 不写注册表、不调 IContextMenu、不做菜单项归属/动态组。
//   第三方软件的复杂右键菜单不属于本功能；需要时用「用某程序打开」
//   或程序自己的压缩包/终端/打开方式功能。
//
// 【数据】
//   独立文件 %APPDATA%\FileExplorer\context-menu.json，不碰 settings.json。
//   每个场景一份有序清单，清单里的 ID 就是「要显示什么、按什么顺序」：
//     · 内置项 ID（见 MenuItemCatalog）
//     · "sep"     分隔线
//     · "actions" 快捷动作整组
//   某个内置项不在清单里 = 这一项被隐藏。
//
// 【默认值】
//   默认清单和改造前的菜单逐项一致；配置丢失/损坏时直接回默认。
// ============================================================================

#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace ui
{

// 可自定义的四个场景（盘符/虚拟项、压缩包内部、左栏不参与）
enum class MenuScene
{
    Background = 0,
    File,
    Folder,
    Multiple,
};

enum MenuSceneBits : unsigned
{
    SceneBitBackground = 1u << 0,
    SceneBitFile       = 1u << 1,
    SceneBitFolder     = 1u << 2,
    SceneBitMultiple   = 1u << 3,

    SceneBitAll        = SceneBitBackground | SceneBitFile | SceneBitFolder | SceneBitMultiple,
};

// 内置项目录：配置窗口用它列清单，运行时用它校验 ID 合法性。
struct MenuItemInfo
{
    std::wstring id;
    std::wstring label;
    unsigned     scenes = 0;   // MenuSceneBits
};

const std::vector<MenuItemInfo>& MenuItemCatalog();

// 场景 ↔ JSON 键
const wchar_t* MenuSceneKey(MenuScene scene);
bool ParseMenuSceneKey(const std::string& key, MenuScene& out);

// 一条用户自定义动作。
//
// 参数模板里的占位符由 ActionRunner 展开，规则见 docs/自定义右键设计.md：
//   %1    第一个选中项（空白区 = 当前目录），自动加引号
//   %sel% 全部选中项（空白区 = 当前目录），每个自动加引号
//   %dir% 当前文件夹，自动加引号
struct CustomAction
{
    std::wstring id;
    std::wstring title;
    std::wstring program;                    // exe / bat / cmd / ps1 / lnk，可含环境变量
    std::wstring arguments = L"%1";          // 参数模板
    std::wstring startIn = L"%dir%";         // 起始目录
    std::wstring icon;                       // 空 = 用程序自己的图标
    unsigned     scenes = SceneBitFile | SceneBitFolder | SceneBitMultiple;
    std::vector<std::wstring> extensions;    // 空 = 该场景内全部（只对文件/多选生效）
    bool         enabled = true;
    bool         runAsAdmin = false;
    int          order = 0;
};

struct MenuLayout
{
    int version = 1;

    // key = MenuSceneKey（"background" / "file" / "folder" / "multiple"）
    // value = 有序 token 清单（内置项 ID / "sep" / "actions"）
    std::map<std::wstring, std::vector<std::wstring>> scenes;

    std::vector<CustomAction> actions;
};

// 默认文件位置：%APPDATA%\FileExplorer\context-menu.json
std::wstring MenuLayoutPath();

// 读写。路径参数单独给出，方便测试不碰真实配置。
bool LoadMenuLayout(MenuLayout& layout);                 // 读默认路径；失败时回默认布局
bool SaveMenuLayout(const MenuLayout& layout);           // 写默认路径（临时文件 + 替换）
bool LoadMenuLayoutFrom(const std::wstring& path, MenuLayout& layout);
bool SaveMenuLayoutTo(const std::wstring& path, const MenuLayout& layout);

// 运行时缓存：右键弹出只读内存，不再每次读盘。
// 配置窗口保存后调用 Invalidate，让下次右键重新读。
const MenuLayout& CachedMenuLayout();
void InvalidateMenuLayoutCache();

// 默认布局（和改造前的菜单逐项一致）
std::vector<std::wstring> DefaultMenuOrder(MenuScene scene);
void ApplyDefaultMenuLayout(MenuLayout& layout);

// 纯函数：按配置顺序解析出这次真正要渲染的 token 序列。
//   available        —— 本次场景里可用的内置项 ID（来自 MenuSpec.available）
//   actionsAvailable —— 本次场景是否有匹配的快捷动作
// 结果会：跳过不可用项、折叠连续 sep、去掉首尾 sep。
std::vector<std::wstring> ResolveMenuOrder(const MenuLayout& layout,
                                           MenuScene scene,
                                           const std::vector<std::wstring>& available,
                                           bool actionsAvailable);

// 配置里某个场景的清单是否合法（ID 都在目录里，或者是 sep/actions）。
// 不合法时 Load 会整份回默认，避免把一份坏配置带进右键。
bool ValidateMenuOrder(const std::vector<std::wstring>& order, MenuScene scene);

// 生成新的动作 ID（"a1"、"a2"…）
std::wstring NewActionId(const MenuLayout& layout);

} // namespace ui
