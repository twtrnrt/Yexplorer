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

#pragma comment(lib, "shlwapi.lib")

namespace
{

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
        // ---------------- 第 1 步：路径 → IShellItem ----------------
        wil::com_ptr<IShellItem> shellItem;
        THROW_IF_FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                      IID_PPV_ARGS(shellItem.put())));

        // ---------------- 第 2 步：IShellItem → IShellFolder ----------------
        // BHID_SFObject 就是「给我这个项对应的 IShellFolder」
        wil::com_ptr<IShellFolder> folder;
        THROW_IF_FAILED(shellItem->BindToHandler(nullptr, BHID_SFObject,
                                                 IID_PPV_ARGS(folder.put())));

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

} // namespace shell
