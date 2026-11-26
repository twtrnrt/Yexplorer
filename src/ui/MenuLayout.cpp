// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MenuLayout.cpp
//
// 「自定义右键菜单」的配置读写、默认布局、顺序解析。
// 纯数据处理，不碰窗口、不碰 COM、不查注册表。
// ============================================================================

#include "ui/MenuLayout.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include <shlobj.h>

namespace ui
{

namespace
{

using nlohmann::json;

const int kLayoutVersion = 1;

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty()) return std::wstring();
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length);
    return result;
}

std::wstring AppDataDir()
{
    LPWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) || !appData)
    {
        return std::wstring();
    }
    std::wstring path = appData;
    ::CoTaskMemFree(appData);
    return path;
}

std::wstring ToLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return text;
}

std::wstring Trim(const std::wstring& text)
{
    const wchar_t* spaces = L" \t\r\n";
    const size_t first = text.find_first_not_of(spaces);
    if (first == std::wstring::npos) return std::wstring();
    const size_t last = text.find_last_not_of(spaces);
    return text.substr(first, last - first + 1);
}

// MenuScene 的枚举值 0..3 正好对应位 0..3，但不能直接强转成位掩码。
unsigned SceneBitFor(MenuScene scene)
{
    return 1u << static_cast<unsigned>(scene);
}

// ---------------------------------------------------------------------------
// 默认顺序
//
// 每一条都和改造前 Build*Menu 里的 AppendMenu 顺序逐项对应；
// 中间那些 sep 就是原来的分隔线。改这里等于改默认菜单，必须同步改测试。
// ---------------------------------------------------------------------------

const wchar_t* const kBackgroundDefault[] =
{
    L"view", L"sort", L"refresh", L"sep",
    L"paste", L"sep",
    L"undo", L"redo", L"sep",
    L"newFolder", L"new", L"sep",
    L"terminal", L"sep",
    L"actions", L"sep",
    L"properties",
};

const wchar_t* const kFileDefault[] =
{
    L"open", L"openInNewWindow", L"openWith", L"sep",
    L"cut", L"copy", L"copyPath", L"sep",
    L"undo", L"redo", L"sep",
    L"delete", L"sep",
    L"rename", L"sep",
    L"actions", L"sep",
    L"properties",
};

const wchar_t* const kFolderDefault[] =
{
    L"open", L"openInNewWindow", L"sep",
    L"cut", L"copy", L"copyPath", L"sep",
    L"undo", L"redo", L"sep",
    L"delete", L"sep",
    L"rename", L"sep",
    L"terminal", L"sep",
    L"actions", L"sep",
    L"properties",
};

const wchar_t* const kMultipleDefault[] =
{
    L"open", L"sep",
    L"cut", L"copy", L"copyPath", L"sep",
    L"undo", L"redo", L"sep",
    L"delete", L"sep",
    L"batchRename", L"sep",
    L"actions", L"sep",
    L"properties",
};

template <size_t N>
std::vector<std::wstring> FromArray(const wchar_t* const (&items)[N])
{
    return std::vector<std::wstring>(items, items + N);
}

std::vector<std::wstring> Dedupe(std::vector<std::wstring> order)
{
    std::vector<std::wstring> result;
    std::set<std::wstring> seen;
    for (std::wstring& token : order)
    {
        if (!seen.insert(token).second)
        {
            continue;
        }
        result.push_back(std::move(token));
    }
    return result;
}

// 把配置里不认识的、或者不属于当前场景的 ID 丢掉，避免一个笔误让整份配置失效。
std::vector<std::wstring> NormalizeMenuOrder(const std::vector<std::wstring>& order,
                                             MenuScene scene)
{
    std::vector<std::wstring> result;
    std::set<std::wstring> seen;

    for (const std::wstring& token : order)
    {
        // 分隔线是位置标记，允许出现多次；以前把 sep 也塞进 seen，
        // 结果配置文件里第二条以后的分隔线在加载时就被吃掉了。
        if (token == L"sep")
        {
            result.push_back(token);
            continue;
        }

        if (token == L"actions")
        {
            if (seen.insert(token).second) result.push_back(token);
            continue;
        }

        const MenuItemInfo* info = nullptr;
        for (const MenuItemInfo& candidate : MenuItemCatalog())
        {
            if (candidate.id == token)
            {
                info = &candidate;
                break;
            }
        }

        if (info == nullptr || (info->scenes & SceneBitFor(scene)) == 0)
        {
            Logger::Warn(L"右键菜单配置：忽略不适用于当前场景的项 " + token);
            continue;
        }

        if (seen.insert(token).second) result.push_back(token);
    }

    return result;
}

// ---------------------------------------------------------------------------
// 缓存
// ---------------------------------------------------------------------------

struct MenuLayoutCacheState
{
    MenuLayout layout;
    bool       loaded = false;
};

MenuLayoutCacheState& Cache()
{
    static MenuLayoutCacheState state;
    return state;
}

} // namespace

// ---------------------------------------------------------------------------
// 目录 / 场景键
// ---------------------------------------------------------------------------

const std::vector<MenuItemInfo>& MenuItemCatalog()
{
    static const std::vector<MenuItemInfo> catalog =
    {
        { L"view",             L"查看",             SceneBitBackground },
        { L"sort",             L"排序",             SceneBitBackground },
        { L"refresh",          L"刷新",             SceneBitBackground },
        { L"paste",            L"粘贴",             SceneBitBackground },
        { L"newFolder",        L"新建文件夹",       SceneBitBackground },
        { L"new",              L"新建",             SceneBitBackground },
        { L"terminal",         L"在终端中打开",     SceneBitBackground | SceneBitFolder },

        { L"open",             L"打开",             SceneBitFile | SceneBitFolder | SceneBitMultiple },
        { L"openInNewWindow",  L"在新窗口中打开",   SceneBitFile | SceneBitFolder },
        { L"openWith",         L"打开方式",         SceneBitFile },
        { L"cut",              L"剪切",             SceneBitFile | SceneBitFolder | SceneBitMultiple },
        { L"copy",             L"复制",             SceneBitFile | SceneBitFolder | SceneBitMultiple },
        { L"copyPath",         L"复制文件路径",     SceneBitFile | SceneBitFolder | SceneBitMultiple },
        { L"delete",           L"删除",             SceneBitFile | SceneBitFolder | SceneBitMultiple },
        { L"rename",           L"重命名",           SceneBitFile | SceneBitFolder },
        { L"batchRename",      L"批量重命名",       SceneBitMultiple },

        { L"undo",             L"撤销",             SceneBitAll },
        { L"redo",             L"恢复",             SceneBitAll },
        { L"properties",       L"属性",             SceneBitAll },
        { L"actions",          L"快捷动作",         SceneBitAll },
    };
    return catalog;
}

const wchar_t* MenuSceneKey(MenuScene scene)
{
    switch (scene)
    {
    case MenuScene::Background: return L"background";
    case MenuScene::File:       return L"file";
    case MenuScene::Folder:     return L"folder";
    case MenuScene::Multiple:   return L"multiple";
    }
    return L"background";
}

bool ParseMenuSceneKey(const std::string& key, MenuScene& out)
{
    if (key == "background") { out = MenuScene::Background; return true; }
    if (key == "file")       { out = MenuScene::File;       return true; }
    if (key == "folder")     { out = MenuScene::Folder;     return true; }
    if (key == "multiple")   { out = MenuScene::Multiple;   return true; }
    return false;
}

// ---------------------------------------------------------------------------
// 默认布局
// ---------------------------------------------------------------------------

std::vector<std::wstring> DefaultMenuOrder(MenuScene scene)
{
    switch (scene)
    {
    case MenuScene::Background: return FromArray(kBackgroundDefault);
    case MenuScene::File:       return FromArray(kFileDefault);
    case MenuScene::Folder:     return FromArray(kFolderDefault);
    case MenuScene::Multiple:   return FromArray(kMultipleDefault);
    }
    return std::vector<std::wstring>();
}

void ApplyDefaultMenuLayout(MenuLayout& layout)
{
    layout.version = kLayoutVersion;
    layout.scenes.clear();
    layout.thirdParty.clear();
    layout.aliases.clear();
    for (MenuScene scene : { MenuScene::Background, MenuScene::File,
                             MenuScene::Folder, MenuScene::Multiple })
    {
        layout.scenes[MenuSceneKey(scene)] = DefaultMenuOrder(scene);
    }
}

// ---------------------------------------------------------------------------
// 路径 / 读写
// ---------------------------------------------------------------------------

std::wstring MenuLayoutPath()
{
    const std::wstring appData = AppDataDir();
    if (appData.empty()) return std::wstring();
    return appData + L"\\FileExplorer\\context-menu.json";
}

bool ValidateMenuOrder(const std::vector<std::wstring>& order, MenuScene scene)
{
    for (const std::wstring& token : order)
    {
        if (token == L"sep" || token == L"actions") continue;

        bool found = false;
        for (const MenuItemInfo& info : MenuItemCatalog())
        {
            if (info.id == token && (info.scenes & SceneBitFor(scene)) != 0)
            {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

namespace
{

bool ParseActions(const json& root, std::vector<CustomAction>& out)
{
    if (!root.contains("actions") || !root["actions"].is_array())
    {
        return true;
    }

    int nextId = 1;
    std::set<std::wstring> usedIds;

    for (const json& item : root["actions"])
    {
        if (!item.is_object()) continue;

        CustomAction action;
        action.id        = FromUtf8(item.value("id", std::string()));
        action.title     = FromUtf8(item.value("title", std::string()));
        action.program   = FromUtf8(item.value("program", std::string()));
        action.arguments = FromUtf8(item.value("arguments", std::string("%1")));
        action.startIn   = FromUtf8(item.value("startIn", std::string("%dir%")));
        action.icon      = FromUtf8(item.value("icon", std::string()));
        action.enabled   = item.value("enabled", true);
        action.runAsAdmin = item.value("runAsAdmin", false);
        action.order     = item.value("order", 0);

        if (action.title.empty()) action.title = L"（未命名动作）";
        if (action.program.empty())
        {
            Logger::Warn(L"右键菜单配置：动作没有程序路径，已跳过：" + action.title);
            continue;
        }

        // 场景：数组；缺省给「文件/文件夹/多选」
        unsigned scenes = 0;
        if (item.contains("scenes") && item["scenes"].is_array())
        {
            for (const json& sceneValue : item["scenes"])
            {
                if (!sceneValue.is_string()) continue;
                const std::string key = sceneValue.get<std::string>();
                MenuScene scene;
                if (!ParseMenuSceneKey(key, scene)) continue;
                switch (scene)
                {
                case MenuScene::Background: scenes |= SceneBitBackground; break;
                case MenuScene::File:       scenes |= SceneBitFile;       break;
                case MenuScene::Folder:     scenes |= SceneBitFolder;     break;
                case MenuScene::Multiple:   scenes |= SceneBitMultiple;   break;
                }
            }
        }
        action.scenes = scenes != 0 ? scenes
                                    : (SceneBitFile | SceneBitFolder | SceneBitMultiple);

        // 扩展名：统一小写、补前导点；"*" 等于全部
        if (item.contains("extensions") && item["extensions"].is_array())
        {
            for (const json& extValue : item["extensions"])
            {
                if (!extValue.is_string()) continue;
                std::wstring ext = ToLower(Trim(FromUtf8(extValue.get<std::string>())));
                if (ext.empty() || ext == L"*") continue;
                if (ext[0] != L'.') ext.insert(ext.begin(), L'.');
                action.extensions.push_back(std::move(ext));
            }
        }

        // ID：缺失/重复就补一个
        if (action.id.empty() || usedIds.count(action.id) != 0)
        {
            do
            {
                action.id = L"a" + std::to_wstring(nextId++);
            } while (usedIds.count(action.id) != 0);
        }
        usedIds.insert(action.id);

        if (action.order == 0)
        {
            action.order = static_cast<int>(out.size()) * 10;
        }

        out.push_back(std::move(action));
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const CustomAction& a, const CustomAction& b) { return a.order < b.order; });
    return true;
}

// 解析 thirdParty：{ "file": [ {"key":"...","app":"WPS Office","source":"classic"} ] }
void ParseThirdParty(const json& root, MenuLayout& layout)
{
    if (!root.contains("thirdParty") || !root["thirdParty"].is_object())
    {
        return;
    }

    for (MenuScene scene : { MenuScene::Background, MenuScene::File,
                             MenuScene::Folder, MenuScene::Multiple })
    {
        const std::string sceneKey = ToUtf8(MenuSceneKey(scene));
        if (!root["thirdParty"].contains(sceneKey) ||
            !root["thirdParty"][sceneKey].is_array())
        {
            continue;
        }

        std::vector<ThirdPartySelection> selections;
        for (const json& item : root["thirdParty"][sceneKey])
        {
            if (!item.is_object()) continue;

            ThirdPartySelection selection;
            selection.appName = FromUtf8(item.value("app", std::string()));
            selection.key     = FromUtf8(item.value("key", std::string()));

            const std::string source = item.value("source", std::string("classic"));
            selection.source = (source == "modern") ? 1 : 0;
            if (selection.appName.empty()) continue;

            if (selection.key.empty())
            {
                selection.key = selection.appName + L"|" +
                                (selection.source == 1 ? L"1" : L"0");
            }
            selections.push_back(std::move(selection));
        }

        layout.thirdParty[MenuSceneKey(scene)] = std::move(selections);
    }
}

void ParseAliases(const json& root, MenuLayout& layout)
{
    layout.aliases.clear();
    if (!root.contains("aliases") || !root["aliases"].is_object()) return;

    for (auto it = root["aliases"].begin(); it != root["aliases"].end(); ++it)
    {
        if (!it.value().is_string()) continue;
        const std::wstring key = FromUtf8(it.key());
        const std::wstring value = FromUtf8(it.value().get<std::string>());
        if (key.empty() || value.empty()) continue;
        layout.aliases[key] = value;
    }
}

} // namespace

bool LoadMenuLayoutFrom(const std::wstring& path, MenuLayout& layout)
{
    layout = MenuLayout();
    ApplyDefaultMenuLayout(layout);

    if (path.empty())
    {
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;   // 没配置过：默认布局
    }

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty())
    {
        Logger::Warn(L"右键菜单配置为空，使用默认布局：" + path);
        return false;
    }

    json root;
    try
    {
        root = json::parse(text);
    }
    catch (const std::exception& e)
    {
        Logger::Warn(std::wstring(L"右键菜单配置解析失败，使用默认布局：") +
                     FromUtf8(e.what()));

        // 留一份坏档，方便用户/我们事后看是什么写坏了
        const std::wstring badPath = path + L".bad";
        ::CopyFileW(path.c_str(), badPath.c_str(), FALSE);
        return false;
    }

    if (!root.is_object())
    {
        Logger::Warn(L"右键菜单配置根节点不是对象，使用默认布局");
        return false;
    }

    layout.version = root.value("version", kLayoutVersion);
    if (layout.version != kLayoutVersion)
    {
        Logger::Warn(L"右键菜单配置版本不是 1，已按当前格式尽力读取");
    }

    // scenes：缺的用默认；有的逐项清洗
    if (root.contains("scenes") && root["scenes"].is_object())
    {
        for (MenuScene scene : { MenuScene::Background, MenuScene::File,
                                 MenuScene::Folder, MenuScene::Multiple })
        {
            const std::string key = ToUtf8(MenuSceneKey(scene));
            if (!root["scenes"].contains(key) || !root["scenes"][key].is_array())
            {
                continue;
            }

            std::vector<std::wstring> order;
            for (const json& token : root["scenes"][key])
            {
                if (token.is_string())
                {
                    order.push_back(FromUtf8(token.get<std::string>()));
                }
            }

            std::vector<std::wstring> normalized = NormalizeMenuOrder(order, scene);

            // 写了内容但一个合法项都没有：多半是 ID 拼错了，回默认，别让菜单整片空掉。
            if (!order.empty() && normalized.empty())
            {
                Logger::Warn(L"右键菜单配置里这个场景没有合法项，使用默认布局：" +
                             std::wstring(MenuSceneKey(scene)));
                normalized = DefaultMenuOrder(scene);
            }

            layout.scenes[MenuSceneKey(scene)] = std::move(normalized);
        }
    }

    ParseThirdParty(root, layout);
    ParseActions(root, layout.actions);
    ParseAliases(root, layout);
    return true;
}

static bool SaveMenuLayoutInternal(const std::wstring& path, const MenuLayout& layout)
{
    if (path.empty()) return false;

    json root;
    root["version"] = kLayoutVersion;

    json scenes = json::object();
    for (MenuScene scene : { MenuScene::Background, MenuScene::File,
                             MenuScene::Folder, MenuScene::Multiple })
    {
        const std::wstring key = MenuSceneKey(scene);
        auto it = layout.scenes.find(key);
        const std::vector<std::wstring> order =
            (it != layout.scenes.end()) ? it->second : DefaultMenuOrder(scene);

        json array = json::array();
        for (const std::wstring& token : order)
        {
            array.push_back(ToUtf8(token));
        }
        scenes[ToUtf8(key)] = array;
    }
    root["scenes"] = scenes;

    json thirdParty = json::object();
    for (MenuScene scene : { MenuScene::Background, MenuScene::File,
                             MenuScene::Folder, MenuScene::Multiple })
    {
        const std::wstring sceneKey = MenuSceneKey(scene);
        auto it = layout.thirdParty.find(sceneKey);
        if (it == layout.thirdParty.end() || it->second.empty())
        {
            continue;
        }

        json array = json::array();
        for (const ThirdPartySelection& selection : it->second)
        {
            json item;
            item["key"]    = ToUtf8(selection.key);
            item["app"]    = ToUtf8(selection.appName);
            item["source"] = (selection.source == 1) ? "modern" : "classic";
            array.push_back(std::move(item));
        }
        thirdParty[ToUtf8(sceneKey)] = array;
    }
    root["thirdParty"] = thirdParty;

    json aliases = json::object();
    for (const auto& pair : layout.aliases)
    {
        if (pair.first.empty() || pair.second.empty()) continue;
        aliases[ToUtf8(pair.first)] = ToUtf8(pair.second);
    }
    root["aliases"] = aliases;

    json actions = json::array();
    for (const CustomAction& action : layout.actions)
    {
        json item;
        item["id"]        = ToUtf8(action.id);
        item["title"]     = ToUtf8(action.title);
        item["program"]   = ToUtf8(action.program);
        item["arguments"] = ToUtf8(action.arguments);
        item["startIn"]   = ToUtf8(action.startIn);
        item["icon"]      = ToUtf8(action.icon);
        item["enabled"]   = action.enabled;
        item["runAsAdmin"] = action.runAsAdmin;
        item["order"]     = action.order;

        json sceneArray = json::array();
        if (action.scenes & SceneBitBackground) sceneArray.push_back("background");
        if (action.scenes & SceneBitFile)       sceneArray.push_back("file");
        if (action.scenes & SceneBitFolder)     sceneArray.push_back("folder");
        if (action.scenes & SceneBitMultiple)   sceneArray.push_back("multiple");
        item["scenes"] = sceneArray;

        json extArray = json::array();
        for (const std::wstring& ext : action.extensions)
        {
            extArray.push_back(ToUtf8(ext));
        }
        item["extensions"] = extArray;

        actions.push_back(std::move(item));
    }
    root["actions"] = actions;

    // 目录不存在就建（首次保存）
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        const std::wstring dir = path.substr(0, slash);
        ::CreateDirectoryW(dir.c_str(), nullptr);
    }

    const std::wstring tempPath = path + L".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        const std::string text = root.dump(2);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
    }

    // 旧文件留一份 .bak，配置写坏时还能翻出来对照
    ::CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);

    if (!::MoveFileExW(tempPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        ::DeleteFileW(tempPath.c_str());
        Logger::Warn(L"右键菜单配置保存失败：" + path);
        return false;
    }

    return true;
}

bool LoadMenuLayout(MenuLayout& layout)
{
    return LoadMenuLayoutFrom(MenuLayoutPath(), layout);
}

bool SaveMenuLayout(const MenuLayout& layout)
{
    return SaveMenuLayoutInternal(MenuLayoutPath(), layout);
}

bool SaveMenuLayoutTo(const std::wstring& path, const MenuLayout& layout)
{
    return SaveMenuLayoutInternal(path, layout);
}

// ---------------------------------------------------------------------------
// 运行时缓存
// ---------------------------------------------------------------------------

const MenuLayout& CachedMenuLayout()
{
    MenuLayoutCacheState& state = Cache();
    if (!state.loaded)
    {
        LoadMenuLayout(state.layout);
        state.loaded = true;
    }
    return state.layout;
}

void InvalidateMenuLayoutCache()
{
    Cache().loaded = false;
}

// ---------------------------------------------------------------------------
// 顺序解析
// ---------------------------------------------------------------------------

std::vector<std::wstring> ResolveMenuOrder(const MenuLayout& layout,
                                           MenuScene scene,
                                           const std::vector<std::wstring>& available,
                                           bool actionsAvailable)
{
    auto it = layout.scenes.find(MenuSceneKey(scene));
    const std::vector<std::wstring> order =
        (it != layout.scenes.end()) ? it->second : DefaultMenuOrder(scene);

    const std::set<std::wstring> availableSet(available.begin(), available.end());

    std::vector<std::wstring> result;
    auto pushSeparator = [&result]()
    {
        if (!result.empty() && result.back() != L"sep")
        {
            result.push_back(L"sep");
        }
    };

    for (const std::wstring& token : order)
    {
        if (token == L"sep")
        {
            pushSeparator();
            continue;
        }

        if (token == L"actions")
        {
            if (!actionsAvailable) continue;
            pushSeparator();
            result.push_back(L"actions");
            continue;
        }

        if (availableSet.count(token) == 0) continue;
        result.push_back(token);
    }

    while (!result.empty() && result.back() == L"sep")
    {
        result.pop_back();
    }

    return result;
}

// ---------------------------------------------------------------------------
// 新动作 ID
// ---------------------------------------------------------------------------

std::wstring NewActionId(const MenuLayout& layout)
{
    int maxId = 0;
    for (const CustomAction& action : layout.actions)
    {
        if (action.id.size() > 1 && action.id[0] == L'a')
        {
            try
            {
                maxId = std::max(maxId, std::stoi(action.id.substr(1)));
            }
            catch (...)
            {
            }
        }
    }
    return L"a" + std::to_wstring(maxId + 1);
}

std::wstring BuiltinAliasKey(const std::wstring& id)
{
    return L"builtin:" + id;
}

std::wstring ThirdPartyAliasKey(const std::wstring& appKey)
{
    return L"third:" + appKey;
}

std::wstring ManualAliasKey(const std::wstring& id)
{
    return L"manual:" + id;
}

std::wstring MenuAlias(const MenuLayout& layout, const std::wstring& key,
                       const std::wstring& fallback)
{
    auto it = layout.aliases.find(key);
    if (it != layout.aliases.end())
    {
        const std::wstring alias = Trim(it->second);
        if (!alias.empty()) return alias;
    }
    return fallback;
}

} // namespace ui
