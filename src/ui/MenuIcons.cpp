// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MenuIcons.cpp
//
// 右键菜单小图标缓存。说明见 MenuIcons.h。
// ============================================================================

#include "ui/MenuIcons.h"

#include "core/Logger.h"

#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")

namespace
{

// 把各种来源的图标统一成 16x16（系统给的可能带点尺寸差异，菜单里必须齐）
HICON NormalizeIcon(HICON source, int size)
{
    if (source == nullptr)
    {
        return nullptr;
    }

    // 注意：变量千万别叫 small —— rpcndr.h 里有 `#define small char` 这个经典宏，
    // 会把 "HICON small" 变成 "HICON char"。
    HICON resized = reinterpret_cast<HICON>(::CopyImage(source, IMAGE_ICON, size, size, 0));
    ::DestroyIcon(source);

    return resized;
}

// 菜单里图标统一尺寸（96 DPI 下 16px，高 DPI 由系统缩放菜单时一起放大）
const int kMenuIconSize = 16;

} // 匿名命名空间

MenuIconCache::~MenuIconCache()
{
    Shutdown();

    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& pair : m_icons)
    {
        if (pair.second != nullptr)
        {
            ::DestroyIcon(pair.second);
        }
    }
    m_icons.clear();
}

std::wstring MenuIconCache::KeyForProgram(const core::OpenWithItem& item)
{
    // 和账本的身份串保持一致：exe 是小写全路径，商店应用是 "uwp:<AUMID>"
    return L"program:" + core::OpenWithKey(item);
}

std::wstring MenuIconCache::KeyForNewItem(const shell::ShellNewItem& item)
{
    // 用注册表键名做键：扩展名（".zip"）和 ProgID（"Folder"）都不会撞
    return L"new:" + item.menuKey;
}

// ---------------------------------------------------------------------------
// 解析 "C:\Windows\system32\shell32.dll,-16769" 这种图标引用，取出 16px 图标
// ---------------------------------------------------------------------------
HICON LoadIconFromSpec(const std::wstring& spec)
{
    if (spec.empty())
    {
        return nullptr;
    }

    std::wstring text = spec;

    // 去掉资源字符串前缀和引号
    if (!text.empty() && text.front() == L'@')
    {
        text.erase(text.begin());
    }
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
    {
        text = text.substr(1, text.size() - 2);
    }

    // 最后一个逗号后面是索引（负数表示资源 ID）
    const size_t comma = text.find_last_of(L',');
    if (comma == std::wstring::npos)
    {
        return nullptr;
    }

    const std::wstring filePart = text.substr(0, comma);
    const int index = _wtoi(text.substr(comma + 1).c_str());

    HICON smallIcon = nullptr;
    if (SUCCEEDED(::SHDefExtractIconW(filePart.c_str(), index, 0, nullptr, &smallIcon,
                                      kMenuIconSize)) &&
        smallIcon != nullptr)
    {
        return NormalizeIcon(smallIcon, kMenuIconSize);
    }

    return nullptr;
}

HICON MenuIconCache::LoadProgramIcon(const core::OpenWithItem& item)
{
    // ---- Win32 程序：直接问它的 exe ----
    if (item.kind != L"uwp")
    {
        if (item.path.empty())
        {
            return nullptr;
        }

        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(item.path.c_str(), 0, &info, sizeof(info),
                             SHGFI_ICON | SHGFI_SMALLICON))
        {
            return NormalizeIcon(info.hIcon, kMenuIconSize);
        }

        return nullptr;
    }

    // ---- 商店应用：先解析成 shell 项，再问图标 ----
    if (item.appId.empty())
    {
        return nullptr;
    }

    const std::wstring parsingName = L"shell:AppsFolder\\" + item.appId;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(::SHParseDisplayName(parsingName.c_str(), nullptr, &pidl, 0, nullptr)) ||
        pidl == nullptr)
    {
        return nullptr;
    }

    SHFILEINFOW info = {};
    HICON icon = nullptr;

    if (::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof(info),
                         SHGFI_ICON | SHGFI_SMALLICON | SHGFI_PIDL))
    {
        icon = NormalizeIcon(info.hIcon, kMenuIconSize);
    }

    ::CoTaskMemFree(pidl);
    return icon;
}

HICON MenuIconCache::LoadNewItemIcon(const shell::ShellNewItem& item)
{
    // ---- 1. ShellNew 的 IconPath（系统菜单用的就是它，最准）----
    if (!item.iconPath.empty())
    {
        HICON fromSpec = LoadIconFromSpec(item.iconPath);
        if (fromSpec != nullptr)
        {
            return fromSpec;
        }
    }

    // ---- 2. 有模板文件就用模板本身的图标（比如 WinRAR 的 zipnew.dat）----
    if (!item.templatePath.empty())
    {
        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(item.templatePath.c_str(), 0, &info, sizeof(info),
                             SHGFI_ICON | SHGFI_SMALLICON))
        {
            return NormalizeIcon(info.hIcon, kMenuIconSize);
        }
    }

    // ---- 3. 文件夹：直接用系统的文件夹图标 ----
    if (item.kind == shell::ShellNewKind::Folder)
    {
        SHSTOCKICONINFO stock = {};
        stock.cbSize = sizeof(stock);

        if (SUCCEEDED(::SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &stock)) &&
            stock.hIcon != nullptr)
        {
            return NormalizeIcon(stock.hIcon, kMenuIconSize);
        }
    }

    // ---- 4. 按扩展名问系统（假文件名，不碰真实文件）----
    std::wstring fakeName;

    if (!item.extension.empty())
    {
        fakeName = L"x" + item.extension;
    }
    else if (!item.menuKey.empty() && item.menuKey.front() == L'.')
    {
        fakeName = L"x" + item.menuKey;
    }

    if (!fakeName.empty())
    {
        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(fakeName.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                             SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES))
        {
            return NormalizeIcon(info.hIcon, kMenuIconSize);
        }
    }

    // ---- 5. 还是没有：用该 ProgID 的 DefaultIcon ----
    if (!item.menuKey.empty())
    {
        HKEY rowKey = nullptr;
        if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, (item.menuKey + L"\\DefaultIcon").c_str(), 0,
                            KEY_READ | KEY_WOW64_64KEY, &rowKey) == ERROR_SUCCESS)
        {
            std::wstring spec;

            wchar_t buffer[512] = {};
            DWORD   size = sizeof(buffer);
            if (::RegQueryValueExW(rowKey, nullptr, nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
                buffer[0] != L'\0')
            {
                spec = buffer;
            }

            ::RegCloseKey(rowKey);

            if (!spec.empty())
            {
                return LoadIconFromSpec(spec);
            }
        }
    }

    return nullptr;
}

void MenuIconCache::Store(const std::wstring& key, HICON icon)
{
    if (key.empty() || icon == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_icons.find(key) != m_icons.end())
    {
        // 已经有了（并发重复取），这份多余的直接丢掉
        ::DestroyIcon(icon);
        return;
    }

    m_icons.emplace(key, icon);
}

HICON MenuIconCache::Get(const std::wstring& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_icons.find(key);
    return (it != m_icons.end()) ? it->second : nullptr;
}

void MenuIconCache::PrefetchPrograms(const std::vector<core::OpenWithItem>& programs)
{
    if (programs.empty())
    {
        return;
    }

    // 先看有没有缺的，全都有就不必开线程
    bool needWork = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const core::OpenWithItem& item : programs)
        {
            if (m_icons.find(KeyForProgram(item)) == m_icons.end())
            {
                needWork = true;
                break;
            }
        }
    }

    if (!needWork)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingPrograms.insert(m_pendingPrograms.end(), programs.begin(), programs.end());
    }

    StartWorker();
}

void MenuIconCache::PrefetchNewItems(const std::vector<shell::ShellNewItem>& items)
{
    if (items.empty())
    {
        return;
    }

    bool needWork = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const shell::ShellNewItem& item : items)
        {
            if (m_icons.find(KeyForNewItem(item)) == m_icons.end())
            {
                needWork = true;
                break;
            }
        }
    }

    if (!needWork)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingNewItems.insert(m_pendingNewItems.end(), items.begin(), items.end());
    }

    StartWorker();
}

void MenuIconCache::StartWorker()
{
    if (m_workerRunning)
    {
        return;   // 已经有一个在跑了，它会把新加进来的活一起干完
    }

    m_workerRunning = true;

    // 上一个线程对象已经结束的话先回收
    if (m_worker.joinable())
    {
        m_worker.join();
    }

    m_worker = std::thread([this]()
    {
        // 后台线程也要初始化 COM：SHParseDisplayName / SHGetFileInfo 都要用
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        while (true)
        {
            std::vector<core::OpenWithItem>   programs;
            std::vector<shell::ShellNewItem>  newItems;

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (m_pendingPrograms.empty() && m_pendingNewItems.empty())
                {
                    m_workerRunning = false;
                    break;
                }

                programs.swap(m_pendingPrograms);
                newItems.swap(m_pendingNewItems);
            }

            for (const core::OpenWithItem& item : programs)
            {
                Store(KeyForProgram(item), LoadProgramIcon(item));
            }

            for (const shell::ShellNewItem& item : newItems)
            {
                Store(KeyForNewItem(item), LoadNewItemIcon(item));
            }
        }

        ::CoUninitialize();
    });
}

void MenuIconCache::Shutdown()
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }

    m_workerRunning = false;
}
