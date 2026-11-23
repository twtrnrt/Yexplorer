// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuConfig.cpp
//
// 配置读写。说明见 MenuConfig.h。
// ============================================================================

#include "shell/MenuConfig.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <shlobj.h>

namespace shell
{

namespace
{

using nlohmann::json;

const int kConfigVersion = 1;

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
    int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty()) return std::wstring();
    int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
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

} // namespace

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

ConfigGroup* MenuConfig::FindGroup(const std::wstring& id)
{
    for (ConfigGroup& group : groups)
    {
        if (group.id == id) return &group;
    }
    return nullptr;
}

ConfigEntry* MenuConfig::FindEntry(const std::wstring& id)
{
    for (ConfigEntry& entry : entries)
    {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

const ConfigGroup* MenuConfig::FindGroup(const std::wstring& id) const
{
    for (const ConfigGroup& group : groups)
    {
        if (group.id == id) return &group;
    }
    return nullptr;
}

const ConfigEntry* MenuConfig::FindEntry(const std::wstring& id) const
{
    for (const ConfigEntry& entry : entries)
    {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

std::wstring MenuConfig::NewGroupId()
{
    int maxId = 0;
    for (const ConfigGroup& group : groups)
    {
        if (group.id.rfind(L"g", 0) == 0)
        {
            maxId = std::max(maxId, _wtoi(group.id.c_str() + 1));
        }
    }
    return L"g" + std::to_wstring(maxId + 1);
}

std::wstring MenuConfig::NewEntryId()
{
    int maxId = 0;
    for (const ConfigEntry& entry : entries)
    {
        if (entry.id.rfind(L"e", 0) == 0)
        {
            maxId = std::max(maxId, _wtoi(entry.id.c_str() + 1));
        }
    }
    return L"e" + std::to_wstring(maxId + 1);
}

// ---------------------------------------------------------------------------
// 路径
// ---------------------------------------------------------------------------

std::wstring MenuConfigPath()
{
    const std::wstring appData = AppDataDir();
    if (appData.empty()) return std::wstring();
    const std::wstring dir = appData + L"\\FileExplorer";
    ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\menu-config.json";
}

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------

bool LoadMenuConfig(MenuConfig& config)
{
    config = MenuConfig();

    const std::wstring path = MenuConfigPath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    json root;
    try { in >> root; }
    catch (const std::exception& e)
    {
        Logger::Warn(std::wstring(L"菜单配置解析失败：") + FromUtf8(e.what()));
        return false;
    }

    if (root.value("version", 0) != kConfigVersion) return false;

    if (root.contains("groups") && root["groups"].is_array())
    {
        for (const auto& j : root["groups"])
        {
            ConfigGroup group;
            group.id       = FromUtf8(j.value("id", std::string()));
            group.title    = FromUtf8(j.value("title", std::string()));
            group.iconFile = FromUtf8(j.value("icon", std::string()));
            group.order    = j.value("order", 0);
            if (!group.id.empty()) config.groups.push_back(std::move(group));
        }
    }

    if (root.contains("entries") && root["entries"].is_array())
    {
        for (const auto& j : root["entries"])
        {
            ConfigEntry entry;
            entry.id            = FromUtf8(j.value("id", std::string()));
            entry.groupId       = FromUtf8(j.value("group", std::string()));
            entry.parentEntryId = FromUtf8(j.value("parent", std::string()));
            entry.pathKey       = FromUtf8(j.value("pathKey", std::string()));
            entry.brand         = FromUtf8(j.value("brand", std::string()));
            entry.sourceText    = FromUtf8(j.value("sourceText", std::string()));
            entry.displayName   = FromUtf8(j.value("name", std::string()));
            entry.iconFile      = FromUtf8(j.value("icon", std::string()));
            entry.order         = j.value("order", 0);
            entry.scope.scenes  = j.value("scenes", 0u);
            if (j.contains("exts") && j["exts"].is_array())
            {
                for (const auto& e : j["exts"]) entry.scope.exts.push_back(FromUtf8(e.get<std::string>()));
            }
            if (!entry.id.empty()) config.entries.push_back(std::move(entry));
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 写
// ---------------------------------------------------------------------------

bool SaveMenuConfig(const MenuConfig& config)
{
    const std::wstring path = MenuConfigPath();
    if (path.empty()) return false;

    json root;
    root["version"] = kConfigVersion;

    json groups = json::array();
    for (const ConfigGroup& group : config.groups)
    {
        json j;
        j["id"]    = ToUtf8(group.id);
        j["title"] = ToUtf8(group.title);
        j["icon"]  = ToUtf8(group.iconFile);
        j["order"] = group.order;
        groups.push_back(j);
    }
    root["groups"] = groups;

    json entries = json::array();
    for (const ConfigEntry& entry : config.entries)
    {
        json j;
        j["id"]        = ToUtf8(entry.id);
        j["group"]     = ToUtf8(entry.groupId);
        j["parent"]    = ToUtf8(entry.parentEntryId);
        j["pathKey"]   = ToUtf8(entry.pathKey);
        j["brand"]     = ToUtf8(entry.brand);
        j["sourceText"] = ToUtf8(entry.sourceText);
        j["name"]      = ToUtf8(entry.displayName);
        j["icon"]      = ToUtf8(entry.iconFile);
        j["order"]     = entry.order;
        j["scenes"]    = entry.scope.scenes;
        json exts = json::array();
        for (const std::wstring& ext : entry.scope.exts) exts.push_back(ToUtf8(ext));
        j["exts"] = exts;
        entries.push_back(j);
    }
    root["entries"] = entries;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string text = root.dump(2);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

} // namespace shell
