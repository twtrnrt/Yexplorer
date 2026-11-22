// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/TraceCleanup.cpp
//
// 实现见头文件。
// ============================================================================

#include "shell/TraceCleanup.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

#include "core/Logger.h"

namespace shell
{

namespace
{

// 配置目录名（与 core/Settings.cpp 里的 kConfigFolderName 一致）
const wchar_t* kConfigFolderName = L"FileExplorer";

// 本软件在注册表里自己的地盘（默认管理器原值备份键就在这下面）
const wchar_t* kRegistryRoot = L"Software\\FileExplorer";

// 解压缓存临时根（与 shell/ContainerExtract.cpp 的 kTempRootName 一致）
const wchar_t* kTempRootName = L"Yexp";

// 删除一个目录；不存在算成功。返回是否删干净了。
bool RemoveDirectoryIfPresent(const std::wstring& path)
{
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return true;   // 本来就没有
    }

    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error)
    {
        Logger::Warn(L"痕迹清理：目录删除失败 " + path + L"，错误=" +
                     std::to_wstring(error.value()));
        return false;
    }

    Logger::Info(L"痕迹清理：已删除目录 " + path);
    return true;
}

} // namespace

bool DeleteAllUserTraces()
{
    bool ok = true;

    // ---- 1. %APPDATA%\FileExplorer 整个目录 ----
    LPWSTR appData = nullptr;
    if (::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData) == S_OK &&
        appData != nullptr)
    {
        const std::wstring configDir = std::wstring(appData) + L"\\" + kConfigFolderName;
        ::CoTaskMemFree(appData);
        ok = RemoveDirectoryIfPresent(configDir) && ok;
    }
    else
    {
        Logger::Warn(L"痕迹清理：取不到 RoamingAppData 目录，跳过配置目录");
        ok = false;
    }

    // ---- 2. HKCU\Software\FileExplorer 整棵注册表键 ----
    // RegDeleteTreeW 递归删子键和值；键不存在也算成功。
    const LONG regResult = ::RegDeleteTreeW(HKEY_CURRENT_USER, kRegistryRoot);
    if (regResult == ERROR_SUCCESS || regResult == ERROR_FILE_NOT_FOUND)
    {
        Logger::Info(L"痕迹清理：已删除注册表键 HKCU\\" + std::wstring(kRegistryRoot));
    }
    else
    {
        Logger::Warn(L"痕迹清理：注册表键删除失败，错误=" + std::to_wstring(regResult));
        ok = false;
    }

    // ---- 3. %TEMP%\Yexp 解压缓存 ----
    wchar_t tempBuffer[MAX_PATH] = {};
    const DWORD tempLength = ::GetTempPathW(ARRAYSIZE(tempBuffer), tempBuffer);
    if (tempLength != 0 && tempLength < ARRAYSIZE(tempBuffer))
    {
        std::wstring tempRoot(tempBuffer);
        if (tempRoot.back() != L'\\')
        {
            tempRoot.push_back(L'\\');
        }
        tempRoot += kTempRootName;
        ok = RemoveDirectoryIfPresent(tempRoot) && ok;
    }
    else
    {
        Logger::Warn(L"痕迹清理：取不到临时目录，跳过解压缓存");
        ok = false;
    }

    return ok;
}

} // namespace shell
