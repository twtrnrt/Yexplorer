// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/Settings.cpp
//
// 统一的配置文件读写（%APPDATA%\FileExplorer\settings.json）。
// 现在的结构：
//   {
//     "version": 1,
//     "pinned": [
//       { "name": "项目文档", "path": "C:\\git\\explorer\\docs", "isFolder": true }
//     ]
//   }
// 以后加窗口大小、列宽这些，就在最外层再放一节（"window": {...}），
// 不影响已经存着的 pinned。
// ============================================================================

#include "core/Settings.h"

#include "core/Logger.h"
#include "shell/ShellFolder.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <fstream>
#include <iterator>
#include <wctype.h>

namespace core
{

namespace
{
// 配置放在这个文件夹里：%APPDATA% 下面的 FileExplorer 目录
const wchar_t* kConfigFolderName = L"FileExplorer";
const wchar_t* kConfigFileName   = L"settings.json";
}

std::wstring OpenWithKey(const OpenWithItem& item)
{
    if (item.kind == L"uwp")
    {
        // 商店应用：AUMID 就是身份证
        return L"uwp:" + item.appId;
    }

    if (item.kind == L"self")
    {
        // 「用本程序打开」这一项（压缩包容器双击时用）：整台机器只有一个本程序，
        // 身份串写死成常量，不跟 exe 路径走——程序挪目录也不用重设账本。
        return L"self:yexplorer";
    }

    // 普通程序：exe 全路径转小写（Windows 路径不区分大小写）
    std::wstring key = item.path;
    for (wchar_t& ch : key)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return key;
}

// ============================================================================
// 单例
// ============================================================================

Settings& Settings::Instance()
{
    static Settings instance;
    return instance;
}

// ============================================================================
// 路径
// ============================================================================

std::wstring Settings::ConfigFilePath()
{
    // 只算一次（每次拖放都要用，不要反复去问系统）
    static std::wstring cachedPath;

    if (!cachedPath.empty())
    {
        return cachedPath;
    }

    LPWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) ||
        appData == nullptr)
    {
        return std::wstring();
    }

    cachedPath = std::wstring(appData) + L"\\" + kConfigFolderName + L"\\" + kConfigFileName;

    ::CoTaskMemFree(appData);
    return cachedPath;
}

bool Settings::EnsureDirectory(const std::wstring& filePath)
{
    // 取文件所在目录
    size_t lastSlash = filePath.find_last_of(L'\\');
    if (lastSlash == std::wstring::npos)
    {
        return false;
    }

    std::wstring folder = filePath.substr(0, lastSlash);

    // SHCreateDirectoryExW 会一层层建，目录已存在时返回 ERROR_ALREADY_EXISTS（也算成功）
    int result = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

// ============================================================================
// 编码转换
// ============================================================================

std::string Settings::ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return std::string();
    }

    int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return std::string();
    }

    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Settings::FromUtf8(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }

    int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return std::wstring();
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length);
    return result;
}

// ============================================================================
// 读
// ============================================================================

bool Settings::Load()
{
    // 读是幂等的：多处都会顺手调一次，已经读过就直接返回，不再碰磁盘
    if (m_loaded)
    {
        return true;
    }

    m_pinnedItems.clear();

    std::wstring path = ConfigFilePath();
    if (path.empty())
    {
        Logger::Error(L"配置文件路径取不出来（%APPDATA% 拿不到）");
        return false;
    }

    // MSVC 的 fstream 直接支持宽字符路径
    std::ifstream file(path, std::ios::binary);

    if (!file.is_open())
    {
        // 首次运行还没有配置文件，这是正常情况，不用记错误
        Logger::Info(L"没有配置文件，按空配置启动：" + path);
        m_loaded = true;
        return true;
    }

    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();

    try
    {
        nlohmann::json json = nlohmann::json::parse(text);

        if (json.contains("pinned") && json["pinned"].is_array())
        {
            for (const auto& one : json["pinned"])
            {
                PinnedItem item;
                item.name     = FromUtf8(one.value("name", std::string()));
                item.path     = FromUtf8(one.value("path", std::string()));
                item.isFolder = one.value("isFolder", true);

                if (item.path.empty())
                {
                    continue;
                }

                if (item.name.empty())
                {
                    // 名字没存就退一步用路径末段，至少列表里不是空的
                    size_t lastSlash = item.path.find_last_of(L'\\');
                    item.name = (lastSlash == std::wstring::npos)
                                ? item.path
                                : item.path.substr(lastSlash + 1);
                }

                m_pinnedItems.push_back(std::move(item));
            }
        }

        // ---------------- 窗口位置与大小 ----------------
        if (json.contains("window") && json["window"].is_object())
        {
            const nlohmann::json& win = json["window"];

            WindowState state;
            state.x         = win.value("x", 0);
            state.y         = win.value("y", 0);
            state.width     = win.value("width", 0);
            state.height    = win.value("height", 0);
            state.maximized = win.value("maximized", false);

            // 宽高必须像个正常窗口，否则宁可不采用（免得配置文件被改坏后开出一个看不见的窗口）
            state.valid     = (state.width >= 320 && state.height >= 240);

            m_window = state;
        }

        // ---------------- 左栏宽度 ----------------
        m_paneWidth = json.value("paneWidth", 0);

        // ---------------- 三列固定列宽度 ----------------
        if (json.contains("columns") && json["columns"].is_object())
        {
            const nlohmann::json& cols = json["columns"];

            ColumnWidths widths;
            widths.time  = cols.value("time", 0);
            widths.type  = cols.value("type", 0);
            widths.size  = cols.value("size", 0);
            widths.valid = (widths.time > 20 && widths.type > 20 && widths.size > 20);

            m_columns = widths;
        }

        // ---------------- 查看模式 ----------------
        if (json.contains("view") && json["view"].is_object())
        {
            const nlohmann::json& view = json["view"];
            m_viewMode       = view.value("mode", -1);
            m_viewModeManual = view.value("manual", false);

            // 按位置单独记的视图（此电脑 / 控制面板）
            // 结构：{ "thispc": 4, "controlpanel": 4 }
            if (view.contains("locations") && view["locations"].is_object())
            {
                m_viewByLocation.clear();
                for (auto it = view["locations"].begin(); it != view["locations"].end(); ++it)
                {
                    if (it.value().is_number_integer())
                    {
                        m_viewByLocation[FromUtf8(it.key())] = it.value().get<int>();
                    }
                }
            }
        }

        // ---------------- 上次停留的目录 ----------------
        m_lastFolder = FromUtf8(json.value("lastFolder", std::string()));

        // ---------------- 启动位置 ----------------
        const std::string startupLocation = json.value("startupLocation", std::string("last"));
        if (startupLocation == "thispc")
        {
            m_startupLocation = StartupLocation::ThisPc;
        }
        else if (startupLocation == "home")
        {
            m_startupLocation = StartupLocation::Home;
        }
        else if (startupLocation == "custom")
        {
            m_startupLocation = StartupLocation::Custom;
        }
        else
        {
            m_startupLocation = StartupLocation::LastFolder;
        }
        m_customStartupFolder = FromUtf8(json.value("customStartupFolder", std::string()));

        // ---------------- 快速启动 ----------------
        m_backgroundStartup = json.value("backgroundStartup", false);
        m_takeOverWinE = json.value("takeOverWinE", false);
        m_tabsEnabled = json.value("tabsEnabled", true);
        m_singleInstance = json.value("singleInstance", true);

        // 重启恢复的标签
        m_tabSession.clear();
        m_activeTabIndex = json.value("activeTab", 0);
        if (json.contains("tabs") && json["tabs"].is_array())
        {
            for (const auto& item : json["tabs"])
            {
                if (!item.is_object())
                {
                    continue;
                }
                TabSessionEntry entry;
                entry.path = FromUtf8(item.value("path", std::string()));
                entry.viewMode = item.value("viewMode", 3);
                entry.viewModeManual = item.value("viewModeManual", false);
                entry.sortColumn = item.value("sortColumn", 0);
                entry.sortAscending = item.value("sortAscending", true);
                if (!entry.path.empty())
                {
                    m_tabSession.push_back(std::move(entry));
                }
            }
        }

        // ---------------- 界面开关 ----------------
        // 右键菜单显示图标：没记过就是关（保持原来的纯文字菜单）
        m_menuIcons = json.value("menuIcons", false);

        // 显示隐藏文件 / 系统文件：没记过就是关（和资源管理器默认一致）
        m_showHiddenFiles = json.value("showHiddenFiles", false);
        m_showSystemFiles = json.value("showSystemFiles", false);

        // Shell 窗口集成（实验性）：默认关
        m_shellWindowIntegration = json.value("shellWindowIntegration", false);

        // ---------------- 「打开方式」账本 ----------------
        // 结构：{ ".txt": { "default": "…", "items": [ {...}, … ] }, … }
        if (json.contains("openWith") && json["openWith"].is_object())
        {
            for (auto it = json["openWith"].begin(); it != json["openWith"].end(); ++it)
            {
                OpenWithEntry entry;
                entry.ext = FromUtf8(it.key());

                if (entry.ext.empty())
                {
                    continue;
                }

                const nlohmann::json& one = it.value();

                entry.defaultKey = FromUtf8(one.value("default", std::string()));

                if (one.contains("items") && one["items"].is_array())
                {
                    for (const auto& raw : one["items"])
                    {
                        OpenWithItem item;
                        item.kind    = FromUtf8(raw.value("kind", std::string("exe")));
                        item.path    = FromUtf8(raw.value("path", std::string()));
                        item.appId   = FromUtf8(raw.value("appId", std::string()));
                        item.progId  = FromUtf8(raw.value("progId", std::string()));
                        item.name    = FromUtf8(raw.value("name", std::string()));
                        item.args    = FromUtf8(raw.value("args", std::string()));
                        item.enabled = raw.value("enabled", true);
                        item.alive   = raw.value("alive", -1);

                        // 既没有 exe 路径又没有 AUMID 的条目是废的，直接丢
                        if (item.path.empty() && item.appId.empty())
                        {
                            continue;
                        }

                        // 「本程序」这一项统一叫 "Yexplorer"：
                        // 旧账本里存过 "Yexplorer（本程序）"，加载时顺手纠正，
                        // 下次保存写回新名字（身份串由 OpenWithKey 决定，与名字无关）
                        if (item.kind == L"self")
                        {
                            item.name = L"Yexplorer";
                        }

                        entry.items.push_back(std::move(item));
                    }
                }

                m_openWith.push_back(std::move(entry));
            }
        }
    }
    catch (const std::exception& error)
    {
        // 文件坏了：改名留档，不让它下次保存时被悄悄覆盖掉
        std::wstring badPath = path + L".bad";
        ::MoveFileExW(path.c_str(), badPath.c_str(), MOVEFILE_REPLACE_EXISTING);

        Logger::Error(L"配置文件解析失败，已改名留档：" + badPath + L"，原因：" +
                      FromUtf8(error.what()));
        m_loaded = true;
        return false;
    }

    m_loaded = true;

    Logger::Info(L"配置已加载，固定项 " + std::to_wstring(m_pinnedItems.size()) + L" 条" +
                 L"，上次目录=" + (m_lastFolder.empty() ? L"（无）" : m_lastFolder) +
                 L"，窗口=" + (m_window.valid
                                  ? (std::to_wstring(m_window.width) + L"x" +
                                     std::to_wstring(m_window.height))
                                  : std::wstring(L"（无）")));
    return true;
}

// ============================================================================
// 写
// ============================================================================

// ----------------------------------------------------------------------------
// 配置保存的跨进程互斥
//
// 为什么需要：多开几个窗口时，几个实例可能同时保存同一份 settings.json。
// 原来它们都写同一个 settings.json.tmp 再各自覆盖，极端情况下会写出
// "两份 JSON 叠在一起"的坏文件（2026-11-19 真实发生过一次，配置被判坏、
// 被改名成 settings.json.bad，用户丢了 4 个固定项和十几个扩展名账本）。
//
// 这里用命名互斥把保存串行化；拿不到锁（5 秒超时）也照常保存，属于尽力而为。
// ----------------------------------------------------------------------------
namespace
{
class SettingsSaveLock
{
public:
    SettingsSaveLock()
    {
        m_handle = ::CreateMutexW(nullptr, FALSE, L"Local\\FileExplorer.SettingsSave");

        if (m_handle != nullptr)
        {
            const DWORD wait = ::WaitForSingleObject(m_handle, 5000);
            m_owned = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
        }
    }

    ~SettingsSaveLock()
    {
        if (m_handle != nullptr)
        {
            if (m_owned)
            {
                ::ReleaseMutex(m_handle);
            }

            ::CloseHandle(m_handle);
        }
    }

    SettingsSaveLock(const SettingsSaveLock&) = delete;
    SettingsSaveLock& operator=(const SettingsSaveLock&) = delete;

private:
    HANDLE m_handle = nullptr;
    bool   m_owned  = false;
};
} // namespace

bool Settings::Save()
{
    // 整个写入过程串行化：别的实例正在保存就等它写完（最多 5 秒）
    SettingsSaveLock saveLock;

    std::wstring path = ConfigFilePath();
    if (path.empty())
    {
        Logger::Error(L"配置文件路径取不出来，保存失败");
        return false;
    }

    if (!EnsureDirectory(path))
    {
        Logger::Error(L"配置文件目录建不出来：" + path);
        return false;
    }

    nlohmann::json json;
    json["version"] = 1;

    nlohmann::json pinned = nlohmann::json::array();
    for (const PinnedItem& item : m_pinnedItems)
    {
        nlohmann::json one;
        one["name"]     = ToUtf8(item.name);
        one["path"]     = ToUtf8(item.path);
        one["isFolder"] = item.isFolder;
        pinned.push_back(std::move(one));
    }
    json["pinned"] = std::move(pinned);

    // ---------------- 窗口位置与大小 ----------------
    if (m_window.valid)
    {
        nlohmann::json win;
        win["x"]         = m_window.x;
        win["y"]         = m_window.y;
        win["width"]     = m_window.width;
        win["height"]    = m_window.height;
        win["maximized"] = m_window.maximized;
        json["window"]   = std::move(win);
    }

    // ---------------- 左栏宽度 ----------------
    if (m_paneWidth > 0)
    {
        json["paneWidth"] = m_paneWidth;
    }

    // ---------------- 三列固定列宽度 ----------------
    if (m_columns.valid)
    {
        nlohmann::json cols;
        cols["time"] = m_columns.time;
        cols["type"] = m_columns.type;
        cols["size"] = m_columns.size;
        json["columns"] = std::move(cols);
    }

    // ---------------- 查看模式 ----------------
    // 全局那份（普通文件夹用）和按位置记的那份（此电脑 / 控制面板）写在同一段里，
    // 位置那部分为空就不写，保持配置文件干净。
    if (m_viewMode >= 0 || !m_viewByLocation.empty())
    {
        nlohmann::json view;

        // 全局那份只在内存里有值时才写。站在「此电脑」「控制面板」关闭时，
        // 程序只更新位置那份，m_viewMode 可能是 -1（配置里本来就没记过全局），
        // 这时候不能把 -1 写进文件。
        if (m_viewMode >= 0)
        {
            view["mode"]   = m_viewMode;
            view["manual"] = m_viewModeManual;
        }

        if (!m_viewByLocation.empty())
        {
            nlohmann::json locations = nlohmann::json::object();
            for (const auto& item : m_viewByLocation)
            {
                locations[ToUtf8(item.first)] = item.second;
            }
            view["locations"] = std::move(locations);
        }

        json["view"] = std::move(view);
    }

    // ---------------- 上次停留的目录 ----------------
    if (!m_lastFolder.empty())
    {
        json["lastFolder"] = ToUtf8(m_lastFolder);
    }

    // ---------------- 启动位置 ----------------
    switch (m_startupLocation)
    {
    case StartupLocation::ThisPc: json["startupLocation"] = "thispc"; break;
    case StartupLocation::Home:   json["startupLocation"] = "home"; break;
    case StartupLocation::Custom: json["startupLocation"] = "custom"; break;
    case StartupLocation::LastFolder:
    default:                      json["startupLocation"] = "last"; break;
    }

    if (!m_customStartupFolder.empty())
    {
        json["customStartupFolder"] = ToUtf8(m_customStartupFolder);
    }

    // ---------------- 快速启动 ----------------
    if (m_backgroundStartup)
    {
        json["backgroundStartup"] = true;
    }
    if (m_takeOverWinE)
    {
        json["takeOverWinE"] = true;
    }
    if (!m_tabsEnabled)
    {
        json["tabsEnabled"] = false;
    }
    if (!m_singleInstance)
    {
        json["singleInstance"] = false;
    }

    if (!m_tabSession.empty())
    {
        nlohmann::json tabs = nlohmann::json::array();
        for (const TabSessionEntry& entry : m_tabSession)
        {
            tabs.push_back({
                {"path", ToUtf8(entry.path)},
                {"viewMode", entry.viewMode},
                {"viewModeManual", entry.viewModeManual},
                {"sortColumn", entry.sortColumn},
                {"sortAscending", entry.sortAscending},
            });
        }
        json["tabs"] = std::move(tabs);
        json["activeTab"] = m_activeTabIndex;
    }

    // ---------------- 界面开关 ----------------
    // 只在"开"的时候写，关就是默认值，不往文件里塞多余的字段
    if (m_menuIcons)
    {
        json["menuIcons"] = true;
    }

    // 显示隐藏文件 / 系统文件：同样只在开的时候写
    if (m_showHiddenFiles)
    {
        json["showHiddenFiles"] = true;
    }
    if (m_showSystemFiles)
    {
        json["showSystemFiles"] = true;
    }
    if (m_shellWindowIntegration)
    {
        json["shellWindowIntegration"] = true;
    }

    // ---------------- 「打开方式」账本 ----------------
    if (!m_openWith.empty())
    {
        nlohmann::json openWith = nlohmann::json::object();

        for (const OpenWithEntry& entry : m_openWith)
        {
            nlohmann::json one;

            if (!entry.defaultKey.empty())
            {
                one["default"] = ToUtf8(entry.defaultKey);
            }

            nlohmann::json items = nlohmann::json::array();

            for (const OpenWithItem& item : entry.items)
            {
                nlohmann::json raw;
                raw["kind"] = ToUtf8(item.kind.empty() ? L"exe" : item.kind);

                if (!item.path.empty())
                {
                    raw["path"] = ToUtf8(item.path);
                }

                if (!item.appId.empty())
                {
                    raw["appId"] = ToUtf8(item.appId);
                }

                if (!item.progId.empty())
                {
                    raw["progId"] = ToUtf8(item.progId);
                }

                raw["name"]    = ToUtf8(item.name);
                raw["enabled"] = item.enabled;
                raw["alive"]   = item.alive;

                if (!item.args.empty())
                {
                    raw["args"] = ToUtf8(item.args);
                }

                items.push_back(std::move(raw));
            }

            one["items"] = std::move(items);
            openWith[ToUtf8(entry.ext)] = std::move(one);
        }

        json["openWith"] = std::move(openWith);
    }

    // 先写临时文件再替换：万一写一半失败（断电、磁盘满），原配置还是好的
    std::wstring tempPath = path + L".tmp";

    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        Logger::Error(L"配置文件写不进去：" + tempPath);
        return false;
    }

    out << json.dump(2);   // 缩进 2 空格，方便用记事本直接看
    out.close();

    if (!::MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        Logger::Error(L"配置文件替换失败：" + path);
        return false;
    }

    return true;
}

// ============================================================================
// 固定区增删
// ============================================================================

bool Settings::AddPinned(const PinnedItem& item)
{
    if (item.path.empty())
    {
        return false;
    }

    if (IsPinned(item.path))
    {
        return false;   // 已经在里面了，不重复加
    }

    m_pinnedItems.push_back(item);
    return true;
}

bool Settings::RemovePinned(const std::wstring& path)
{
    for (size_t i = 0; i < m_pinnedItems.size(); ++i)
    {
        if (::_wcsicmp(m_pinnedItems[i].path.c_str(), path.c_str()) == 0)
        {
            m_pinnedItems.erase(m_pinnedItems.begin() + static_cast<ptrdiff_t>(i));
            return true;
        }
    }

    return false;
}

bool Settings::IsPinned(const std::wstring& path) const
{
    for (const PinnedItem& item : m_pinnedItems)
    {
        if (::_wcsicmp(item.path.c_str(), path.c_str()) == 0)
        {
            return true;
        }
    }

    return false;
}

// ============================================================================
// 按位置单独记的查看模式（此电脑 / 控制面板）
//
// 只操作内存里的那张表，不碰磁盘：启动时一次性读进来，导航时查内存，
// 关闭窗口时才 Save 写盘。所以切文件夹、弹右键菜单都没有 IO 开销。
// ============================================================================

int Settings::LocationViewMode(const std::wstring& key) const
{
    const auto it = m_viewByLocation.find(key);
    return (it == m_viewByLocation.end()) ? -1 : it->second;
}

void Settings::SetLocationViewMode(const std::wstring& key, int mode)
{
    m_viewByLocation[key] = mode;
}

// ============================================================================
// 「打开方式」账本
// ============================================================================

const OpenWithEntry* Settings::FindOpenWith(const std::wstring& ext) const
{
    for (const OpenWithEntry& entry : m_openWith)
    {
        if (::_wcsicmp(entry.ext.c_str(), ext.c_str()) == 0)
        {
            return &entry;
        }
    }

    return nullptr;
}

OpenWithEntry& Settings::EnsureOpenWith(const std::wstring& ext)
{
    for (OpenWithEntry& entry : m_openWith)
    {
        if (::_wcsicmp(entry.ext.c_str(), ext.c_str()) == 0)
        {
            return entry;
        }
    }

    OpenWithEntry fresh;
    fresh.ext = ext;
    m_openWith.push_back(std::move(fresh));
    return m_openWith.back();
}

std::wstring Settings::ResolveStartupPath() const
{    switch (m_startupLocation)
    {
    case StartupLocation::ThisPc:
        return shell::kThisPcPath;

    case StartupLocation::Home:
        return shell::kHomeFolderPath;

    case StartupLocation::Custom:
    {
        if (m_customStartupFolder.empty())
        {
            return shell::kHomeFolderPath;
        }

        // 支持“此电脑”“主文件夹”等 Shell 虚拟位置
        const bool isShellNamespace =
            (m_customStartupFolder.compare(0, 2, L"::") == 0) ||
            (m_customStartupFolder.compare(0, 6, L"shell:") == 0);

        if (isShellNamespace)
        {
            IShellItem* item = nullptr;
            const bool parsed = SUCCEEDED(::SHCreateItemFromParsingName(
                m_customStartupFolder.c_str(), nullptr, IID_PPV_ARGS(&item)));
            if (item != nullptr)
            {
                item->Release();
            }
            if (parsed)
            {
                return m_customStartupFolder;
            }
        }
        else
        {
            const DWORD attributes = ::GetFileAttributesW(m_customStartupFolder.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return m_customStartupFolder;
            }
        }

        Logger::Warn(L"启动位置：自定义目录不可用，回退到主文件夹：" + m_customStartupFolder);
        return shell::kHomeFolderPath;
    }

    case StartupLocation::LastFolder:
    default:
        return m_lastFolder.empty() ? std::wstring(shell::kHomeFolderPath) : m_lastFolder;
    }
}

void Settings::ReloadFromDisk()
{
    m_loaded = false;
    Load();
}

} // namespace core
