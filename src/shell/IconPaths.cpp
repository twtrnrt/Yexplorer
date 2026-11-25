// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/IconPaths.cpp
//
// 右键菜单图标缓存目录。说明见 IconPaths.h。
// ============================================================================

#include "shell/IconPaths.h"

#include <windows.h>
#include <shlobj.h>

namespace shell
{

namespace
{

std::wstring AppDataSubDir(const wchar_t* child)
{
    LPWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) || !appData)
    {
        return std::wstring();
    }

    const std::wstring base = appData;
    ::CoTaskMemFree(appData);

    const std::wstring root = base + L"\\FileExplorer";
    ::CreateDirectoryW(root.c_str(), nullptr);

    const std::wstring dir = root + L"\\" + child;
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

} // namespace

std::wstring IconDir()
{
    return AppDataSubDir(L"menu-icons");
}

} // namespace shell
