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
#include "nlohmann/json.hpp"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <iterator>

namespace core
{

namespace
{
// 配置放在这个文件夹里：%APPDATA% 下面的 FileExplorer 目录
const wchar_t* kConfigFolderName = L"FileExplorer";
const wchar_t* kConfigFileName   = L"settings.json";
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
    }
    catch (const std::exception& error)
    {
        // 文件坏了：改名留档，不让它下次保存时被悄悄覆盖掉
        std::wstring badPath = path + L".bad";
        ::MoveFileExW(path.c_str(), badPath.c_str(), MOVEFILE_REPLACE_EXISTING);

        Logger::Error(L"配置文件解析失败，已改名留档：" + badPath + L"，原因：" +
                      FromUtf8(error.what()));
        return false;
    }

    Logger::Info(L"配置已加载，固定项 " + std::to_wstring(m_pinnedItems.size()) + L" 条");
    return true;
}

// ============================================================================
// 写
// ============================================================================

bool Settings::Save()
{
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

} // namespace core
