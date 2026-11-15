// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellFolder.cpp
//
// 目录枚举实现。核心流程：
//
//   路径字符串
//     → SHCreateItemFromParsingName  得到 IShellItem
//     → BindToHandler(BHID_SFObject) 得到 IShellFolder
//     → EnumObjects                  得到 IEnumIDList
//     → Next() 逐个取子项 PIDL
//         → GetDisplayNameOf  取显示名
//         → GetAttributesOf   判断是不是文件夹
//         → SHCreateItemWithParent + IShellItem2  取大小、修改时间、绝对路径
//
// COM 释放说明：
//   PIDL 必须用 CoTaskMemFree 释放（不是 delete）；
//   接口指针全部用 wil::com_ptr 自动管理，避免漏掉 Release 导致泄漏。
// ============================================================================

#include "ShellFolder.h"
#include "../core/Logger.h"

// WIL：COM 智能指针与 HRESULT 工具
#include <wil/com.h>
#include <wil/result.h>

#include <shlobj.h>       // SHCreateItemFromParsingName 等
#include <shobjidl.h>     // IShellItem2 / IEnumIDList / IShellFolder
#include <shlwapi.h>      // StrRetToBufW
#include <propkey.h>      // PKEY_Size / PKEY_DateModified
#include <knownfolders.h> // FOLDERID_Desktop

#pragma comment(lib, "shlwapi.lib")

namespace
{

// ----------------------------------------------------------------------------
// 判断是不是盘符开头的普通文件系统路径（C:\… / D:\…）
// ----------------------------------------------------------------------------
bool IsDriveLetterPath(const std::wstring& path)
{
    return path.size() >= 2 && ::iswalpha(path[0]) && path[1] == L':';
}

// ----------------------------------------------------------------------------
// 判断某个路径是不是"物理桌面目录"（C:\Users\xxx\Desktop）
//
// 用来修一个坑：回收站、控制面板、网络、主文件夹这些顶层命名空间项，
// 系统给它们的父项是物理桌面目录，而资源管理器里它们的上一级是那个
// 装着「此电脑/主文件夹/音乐…」的「桌面」容器。所以一旦算出物理桌面，
// 就得换成顶层容器的伪路径。
// ----------------------------------------------------------------------------
bool IsPhysicalDesktopPath(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    static std::wstring cachedDesktopPath;   // 取一次就够，路径不会变
    static bool         fetched = false;

    if (!fetched)
    {
        wil::unique_cotaskmem_string desktopPath;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, desktopPath.put())) &&
            desktopPath != nullptr)
        {
            cachedDesktopPath = desktopPath.get();
        }
        fetched = true;
    }

    if (cachedDesktopPath.empty())
    {
        return false;
    }

    // 比较时忽略末尾反斜杠和大小写
    std::wstring a = path;
    std::wstring b = cachedDesktopPath;
    while (!a.empty() && (a.back() == L'\\' || a.back() == L'/'))
    {
        a.pop_back();
    }
    while (!b.empty() && (b.back() == L'\\' || b.back() == L'/'))
    {
        b.pop_back();
    }

    return ::_wcsicmp(a.c_str(), b.c_str()) == 0;
}

// ----------------------------------------------------------------------------
// 去掉一级目录（纯字符串层面）
//
//   "C:\Users\kf168\Downloads" → "C:\Users\kf168"
//   "C:\Users"                 → "C:\"
//   "C:\"                      → "C:\"（已经是盘根，原样返回，由调用方决定去哪）
// ----------------------------------------------------------------------------
std::wstring TrimOneLevel(const std::wstring& path)
{
    std::wstring trimmed = path;

    // 先去掉末尾多余的反斜杠，但保留 "C:\" 这种盘根的形态
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
    {
        trimmed.pop_back();
    }

    // 盘根
    if (trimmed.size() <= 3 && trimmed.size() >= 2 && trimmed[1] == L':')
    {
        return trimmed;
    }

    size_t lastSlash = trimmed.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos)
    {
        return trimmed;
    }

    // "C:\Windows" 的上一级是盘根 "C:\"
    if (lastSlash == 2 && trimmed.size() > 2 && trimmed[1] == L':')
    {
        return trimmed.substr(0, 3);
    }

    return trimmed.substr(0, lastSlash);
}

// ----------------------------------------------------------------------------
// 把 HRESULT 写进日志
// ----------------------------------------------------------------------------
void LogIfFailed(HRESULT hr, const wchar_t* what, const std::wstring& path)
{
    if (FAILED(hr))
    {
        Logger::Error(std::wstring(what) + L" 失败，路径=" + path +
                      L"，HRESULT=" + Logger::FormatHresult(hr));
    }
}

// ----------------------------------------------------------------------------
// 取子项显示名（相对于父文件夹，比如 "Windows"、"readme.txt"）
// ----------------------------------------------------------------------------
std::wstring GetDisplayName(IShellFolder* folder, LPCITEMIDLIST childPidl)
{
    STRRET strret = {};
    if (FAILED(folder->GetDisplayNameOf(childPidl, SHGDN_INFOLDER, &strret)))
    {
        return {};
    }

    // 用 512 字符缓冲足够常规文件名；超长名称会截断，后续阶段再优化
    wchar_t buffer[512] = {};
    if (FAILED(::StrRetToBufW(&strret, childPidl, buffer, ARRAYSIZE(buffer))))
    {
        return {};
    }
    return std::wstring(buffer);
}

// ----------------------------------------------------------------------------
// 判断是否为文件夹
// ----------------------------------------------------------------------------
bool IsFolder(IShellFolder* folder, LPCITEMIDLIST childPidl)
{
    SFGAOF attributes = SFGAO_FOLDER;
    if (FAILED(folder->GetAttributesOf(1, &childPidl, &attributes)))
    {
        return false;
    }
    return (attributes & SFGAO_FOLDER) != 0;
}

// ----------------------------------------------------------------------------
// 用 IShellItem2 补齐额外信息：绝对路径、大小、修改时间
// 说明：这些属性不是所有项都有（虚拟项就没有文件系统路径），取不到就留空。
// ----------------------------------------------------------------------------
void FillExtraInfo(IShellFolder* folder, LPCITEMIDLIST childPidl, shell::ShellEntry& entry)
{
    wil::com_ptr<IShellItem2> item2;
    HRESULT hr = ::SHCreateItemWithParent(nullptr, folder, childPidl, IID_PPV_ARGS(item2.put()));
    if (FAILED(hr))
    {
        // 虚拟项可能建不出 IShellItem2，属于正常情况，不记错误日志
        return;
    }

    // 绝对路径（仅文件系统项有）
    LPWSTR fileSystemPath = nullptr;
    if (SUCCEEDED(item2->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath)) && fileSystemPath != nullptr)
    {
        entry.fullPath = fileSystemPath;
        ::CoTaskMemFree(fileSystemPath);
    }
    else
    {
        // 虚拟项（例如「此电脑」里的各个盘、网络位置）没有文件系统路径。
        // 这里退一步取「桌面绝对解析名」（形如 ::{20D04FE0-...}\C:），
        // 它同样能用来导航（SHCreateItemFromParsingName 认这种写法），也能用来问图标。
        LPWSTR parsingName = nullptr;
        if (SUCCEEDED(item2->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsingName)) && parsingName != nullptr)
        {
            entry.fullPath = parsingName;
            ::CoTaskMemFree(parsingName);
        }
    }

    // 修改时间
    FILETIME modified = {};
    if (SUCCEEDED(item2->GetFileTime(PKEY_DateModified, &modified)))
    {
        entry.modified = modified;
    }

    // 大小（文件夹的大小没有意义，不取）
    if (!entry.isFolder)
    {
        UINT64 size = 0;
        if (SUCCEEDED(item2->GetUInt64(PKEY_Size, &size)))
        {
            entry.size = size;
        }
    }
}

} // namespace

namespace shell
{

EnumerateResult EnumerateFolder(const std::wstring& path,
                                const core::CancellationToken* token)
{
    EnumerateResult result;
    result.path = path;

    // ------------------------------------------------------------------
    // 初始化 COM
    //
    // 后台线程调用本函数时，线程本身还没初始化 COM，这里先初始化。
    // 用 STA：Shell 的很多外壳扩展只支持单线程套间。
    //
    // 返回值判断：
    //   S_OK / S_FALSE —— 初始化成功（S_FALSE 表示本线程已初始化过），需要配对 CoUninitialize
    //   RPC_E_CHANGED_MODE —— 本线程已被初始化成 MTA，此时不能改，也不能 CoUninitialize
    // ------------------------------------------------------------------
    HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needUninitialize = (hrInit == S_OK || hrInit == S_FALSE);

    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
    {
        result.hr = hrInit;
        LogIfFailed(hrInit, L"CoInitializeEx", path);
        return result;
    }

    try
    {
        // ---------------- 第 1-2 步：路径 → IShellFolder ----------------
        wil::com_ptr<IShellFolder> folder;

        if (IsTopDesktopPath(path))
        {
            // 命名空间最顶层那个「桌面」：没有解析名，只能直接拿桌面根文件夹
            THROW_IF_FAILED(::SHGetDesktopFolder(folder.put()));
        }
        else
        {
            // 路径 → IShellItem
            wil::com_ptr<IShellItem> shellItem;
            THROW_IF_FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                          IID_PPV_ARGS(shellItem.put())));

            // IShellItem → IShellFolder
            // BHID_SFObject 就是「给我这个项对应的 IShellFolder」
            THROW_IF_FAILED(shellItem->BindToHandler(nullptr, BHID_SFObject,
                                                     IID_PPV_ARGS(folder.put())));
        }

        // ---------------- 第 3 步：拿枚举器 ----------------
        // SHCONTF_FOLDERS   —— 要文件夹
        // SHCONTF_NONFOLDERS—— 要文件
        // 暂不包含隐藏项，保持和资源管理器默认行为一致
        wil::com_ptr<IEnumIDList> enumList;
        THROW_IF_FAILED(folder->EnumObjects(nullptr,
                                            SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                                            enumList.put()));

        // ---------------- 第 4 步：逐个取子项 ----------------
        LPITEMIDLIST childPidl = nullptr;
        ULONG fetched = 0;

        while (enumList->Next(1, &childPidl, &fetched) == S_OK && fetched == 1)
        {
            // 取消检查：用户切了目录就尽快退出，别把磁盘拖死
            if (token != nullptr && token->IsCancelled())
            {
                result.cancelled = true;
                ::CoTaskMemFree(childPidl);
                childPidl = nullptr;
                break;
            }

            ShellEntry entry;
            entry.isFolder = IsFolder(folder.get(), childPidl);
            entry.name     = GetDisplayName(folder.get(), childPidl);
            FillExtraInfo(folder.get(), childPidl, entry);

            // PIDL 是 COM 分配的内存，必须用 CoTaskMemFree 释放
            ::CoTaskMemFree(childPidl);
            childPidl = nullptr;
            fetched = 0;

            // 名称为空的项（罕见）跳过，避免列表里出现空白行
            if (entry.name.empty())
            {
                continue;
            }

            if (entry.isFolder)
            {
                ++result.folderCount;
            }
            else
            {
                ++result.fileCount;
            }
            result.entries.push_back(std::move(entry));
        }

        result.hr = S_OK;
    }
    catch (const wil::ResultException& e)
    {
        result.hr = e.GetErrorCode();
        LogIfFailed(static_cast<HRESULT>(result.hr), L"枚举目录", path);
    }
    catch (...)
    {
        result.hr = E_FAIL;
        Logger::Error(L"枚举目录出现未知异常，路径=" + path);
    }

    // 与 CoInitializeEx 配对
    if (needUninitialize)
    {
        ::CoUninitialize();
    }

    return result;
}

// ============================================================================
// 取上一级（资源管理器「向上」按钮的同一套逻辑）
//
// 用 IShellItem::GetParent 让 Shell 自己去算父项，这样：
//   · 普通文件夹 → 物理父目录
//   · C:\        → 此电脑
//   · 此电脑      → 命名空间顶层的「桌面」（里面含此电脑、主文件夹、音乐等）
//   · 顶层「桌面」→ GetParent 返回失败，表示没有上一级（向上按钮应变灰）
// 最后把父项换成「绝对解析名」（形如 ::{GUID}\C:\ 或 ::{GUID}），
// 这个字符串我们程序能直接拿去导航和枚举。
// ============================================================================
std::wstring GetParentPath(const std::wstring& path)
{
    if (path.empty())
    {
        return L"";
    }

    // 已经在命名空间最顶层那个「桌面」下：再往上就没有了（资源管理器里按钮是灰的）
    if (IsTopDesktopPath(path))
    {
        return L"";
    }

    // ------------------------------------------------------------------
    // 1) 「此电脑」的上一级 = 顶层「桌面」
    // ------------------------------------------------------------------
    if (::_wcsicmp(path.c_str(), kThisPcPath) == 0)
    {
        return kTopDesktopPath;
    }

    // ------------------------------------------------------------------
    // 2) 普通文件系统路径（C:\…）：按目录层级往上走，到盘根再往上 = 此电脑
    //
    //    C:\Users\kf168\Downloads → C:\Users\kf168     （资源管理器就是这样）
    //    C:\Users\kf168\Desktop   → C:\Users\kf168
    //    C:\Users\kf168           → C:\Users
    //    C:\Users                 → C:\
    //    C:\                      → 此电脑
    //
    //    这里必须用路径层级，不能调 IShellItem::GetParent：
    //    系统把它算成「下载/桌面 → 桌面容器」，和资源管理器给出的目标不一致
    //    （资源管理器状态下，下载的向上按钮明明白白写着「上移到 kf168」）。
    // ------------------------------------------------------------------
    if (IsDriveLetterPath(path))
    {
        std::wstring parent = TrimOneLevel(path);

        if (::_wcsicmp(parent.c_str(), path.c_str()) == 0)
        {
            // 已经在盘根（C:\）了再往上就是此电脑
            return kThisPcPath;
        }
        return parent;
    }

    // ------------------------------------------------------------------
    // 3) 其余都是虚拟项（回收站、网络位置、控制面板之类）：交给 Shell 自己算
    // ------------------------------------------------------------------
    HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool    needUninitialize = SUCCEEDED(initHr);

    std::wstring parentPath;

    try
    {
        wil::com_ptr<IShellItem> item;
        THROW_IF_FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                      IID_PPV_ARGS(&item)));

        wil::com_ptr<IShellItem> parent;
        HRESULT hr = item->GetParent(&parent);

        if (FAILED(hr) || parent == nullptr)
        {
            // 没有父项 → 当作到了顶层
            parentPath = kTopDesktopPath;
        }
        else
        {
            wil::unique_cotaskmem_string name;

            if (SUCCEEDED(parent->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name)) &&
                name != nullptr)
            {
                parentPath = name.get();
            }

            if (parentPath.empty())
            {
                wil::unique_cotaskmem_string fsPath;
                if (SUCCEEDED(parent->GetDisplayName(SIGDN_FILESYSPATH, &fsPath)) && fsPath != nullptr)
                {
                    parentPath = fsPath.get();
                }
            }

            // 系统把这一层折成了「桌面」，解析名跟当前项一样 → 用伪路径表示顶层桌面
            if (parentPath.empty() || ::_wcsicmp(parentPath.c_str(), path.c_str()) == 0)
            {
                parentPath = kTopDesktopPath;
            }

            // 系统给回收站 / 控制面板 / 网络这些顶层命名空间项算出来的父是"物理桌面目录"，
            // 但资源管理器里它们的上一级是那个装着「此电脑/主文件夹/音乐…」的「桌面」容器。
            // 这里统一换成伪路径，不然点向上会掉到 C:\Users\xxx\Desktop（只有桌面上的文件）。
            if (!parentPath.empty() && IsPhysicalDesktopPath(parentPath))
            {
                parentPath = kTopDesktopPath;
            }
        }
    }
    catch (const wil::ResultException& e)
    {
        LogIfFailed(e.GetErrorCode(), L"取上一级", path);
        parentPath.clear();
    }
    catch (...)
    {
        Logger::Error(L"取上一级出现未知异常，路径=" + path);
        parentPath.clear();
    }

    if (needUninitialize)
    {
        ::CoUninitialize();
    }

    return parentPath;
}

// ============================================================================
// 判断 Shell 项是不是容器（决定双击是"进去"还是"打开"）
// ============================================================================

bool ShellItemIsContainer(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    // 普通文件系统目录：肯定能进去（空文件夹也一样，进去显示空列表）
    if (::PathIsDirectoryW(path.c_str()))
    {
        return true;
    }

    HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool    needUninitialize = SUCCEEDED(initHr);

    bool isContainer = false;

    try
    {
        wil::com_ptr<IShellItem> item;

        // 优先按解析名取；取不到再用 PIDL 兜底（有些虚拟项只认 PIDL）
        HRESULT hr = ::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));

        if (FAILED(hr))
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            SFGAOF           attrs = 0;

            if (SUCCEEDED(::SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, &attrs)) &&
                pidl != nullptr)
            {
                hr = ::SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
                ::CoTaskMemFree(pidl);
            }
        }

        if (SUCCEEDED(hr) && item != nullptr)
        {
            wil::com_ptr<IShellFolder> folder;

            // 能绑定出 IShellFolder 的才算容器；applet 这一步就会失败
            if (SUCCEEDED(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder))) &&
                folder != nullptr)
            {
                wil::com_ptr<IEnumIDList> enumList;
                const DWORD              flags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;

                // 枚举接口拿得到就算容器——注意这里不看"有没有子项"：
                // 空回收站、空库都是"能进去但是空的"，必须让用户进去看到空列表，
                // 而不是被当成 applet 弹一个系统窗口出来。
                if (SUCCEEDED(folder->EnumObjects(nullptr, flags, enumList.put())) &&
                    enumList != nullptr)
                {
                    isContainer = true;
                }
            }
        }
    }
    catch (const wil::ResultException& e)
    {
        LogIfFailed(e.GetErrorCode(), L"探测是否为容器", path);
        isContainer = false;
    }
    catch (...)
    {
        Logger::Error(L"探测是否为容器出现未知异常，路径=" + path);
        isContainer = false;
    }

    if (needUninitialize)
    {
        ::CoUninitialize();
    }

    return isContainer;
}

} // namespace shell
