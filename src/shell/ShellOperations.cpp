// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellOperations.cpp
//
// IFileOperation 封装实现。
// ============================================================================

#include "ShellOperations.h"

#include <windows.h>
#include <shobjidl.h>       // IFileOperation / IShellItem / SHCreateShellItemArrayFromIDLists
#include <shobjidl_core.h>
#include <shlobj.h>         // SHParseDisplayName / SHCreateItemFromParsingName
#include <shlobj_core.h>    // FOF_ALLOWUNDO / FOF_NOCONFIRMATION 等标志位
#include <shellapi.h>

#include <wil/com.h>

#include <vector>

namespace
{

// ----------------------------------------------------------------------------
// 取路径的父目录
// 例：C:\a\b  →  C:\a
//     C:\a    →  C:\
// ----------------------------------------------------------------------------
std::wstring ParentDirectoryOf(const std::wstring& path)
{
    std::wstring trimmed = path;

    // 先去掉末尾多余的反斜杠（但保留 "C:\" 这种盘符根）
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
    {
        trimmed.pop_back();
    }

    size_t lastSeparator = trimmed.find_last_of(L"\\/");
    if (lastSeparator == std::wstring::npos)
    {
        return L"";
    }
    if (lastSeparator <= 2)
    {
        return trimmed.substr(0, 3);   // 盘符根
    }
    return trimmed.substr(0, lastSeparator);
}

// ----------------------------------------------------------------------------
// 两个目录是否同一个（忽略大小写和末尾反斜杠）
// ----------------------------------------------------------------------------
bool IsSameDirectory(const std::wstring& left, const std::wstring& right)
{
    std::wstring a = left;
    std::wstring b = right;

    while (a.size() > 3 && (a.back() == L'\\' || a.back() == L'/'))
    {
        a.pop_back();
    }
    while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/'))
    {
        b.pop_back();
    }

    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// ----------------------------------------------------------------------------
// 这批来源是不是全都在目标目录里（也就是「在同一目录里复制」）
// ----------------------------------------------------------------------------
bool AllSourcesInTargetDirectory(const std::vector<std::wstring>& sourcePaths,
                                 const std::wstring& targetDirectory)
{
    if (sourcePaths.empty())
    {
        return false;
    }

    for (const std::wstring& path : sourcePaths)
    {
        if (!IsSameDirectory(ParentDirectoryOf(path), targetDirectory))
        {
            return false;
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 取路径最后一段的名字（文件名或文件夹名）
// ----------------------------------------------------------------------------
std::wstring FileNameOf(const std::wstring& path)
{
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
    {
        trimmed.pop_back();
    }

    size_t lastSeparator = trimmed.find_last_of(L"\\/");
    if (lastSeparator == std::wstring::npos)
    {
        return trimmed;
    }
    return trimmed.substr(lastSeparator + 1);
}

// ----------------------------------------------------------------------------
// 生成副本名字：和资源管理器一致
//   文件夹 a      →  a - 副本
//   文件 fe.txt   →  fe - 副本.txt
// 如果已经存在同名副本，就继续加序号 "a - 副本 (2)"
// ----------------------------------------------------------------------------
std::wstring MakeCopyName(const std::wstring& directory,
                          const std::wstring& sourcePath,
                          bool isFolder)
{
    std::wstring fileName = FileNameOf(sourcePath);

    std::wstring baseName = fileName;
    std::wstring extension;

    if (!isFolder)
    {
        size_t lastDot = fileName.find_last_of(L'.');
        // 点在开头（如 ".gitignore"）或者没有点，就不拆扩展名
        if (lastDot != std::wstring::npos && lastDot > 0)
        {
            baseName  = fileName.substr(0, lastDot);
            extension = fileName.substr(lastDot);
        }
    }

    std::wstring directoryWithSlash = directory;
    if (!directoryWithSlash.empty() &&
        (directoryWithSlash.back() != L'\\' && directoryWithSlash.back() != L'/'))
    {
        directoryWithSlash += L'\\';
    }

    for (int index = 1; index <= 1000; ++index)
    {
        std::wstring candidate;
        if (index == 1)
        {
            candidate = baseName + L" - 副本" + extension;
        }
        else
        {
            candidate = baseName + L" - 副本 (" + std::to_wstring(index) + L")" + extension;
        }

        std::wstring fullPath = directoryWithSlash + candidate;
        if (::GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return candidate;
        }
    }

    return baseName + L" - 副本" + extension;
}

// ----------------------------------------------------------------------------
// 把一批完整路径转成 IShellItemArray
// （IFileOperation 的所有批量操作都要这个类型，才能一次弹一个进度框）
// ----------------------------------------------------------------------------
HRESULT CreateItemArray(const std::vector<std::wstring>& paths, IShellItemArray** result)
{
    *result = nullptr;

    if (paths.empty())
    {
        return E_INVALIDARG;
    }

    // 用「只读 PIDL」保存，正好匹配 SHCreateShellItemArrayFromIDLists 的参数类型
    std::vector<LPCITEMIDLIST> pidls;
    pidls.reserve(paths.size());

    HRESULT hr = S_OK;

    for (const std::wstring& path : paths)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = ::SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
        if (FAILED(hr))
        {
            break;
        }
        pidls.push_back(pidl);
    }

    if (SUCCEEDED(hr))
    {
        hr = ::SHCreateShellItemArrayFromIDLists(
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            result);
    }

    // 不管成功失败都要释放 PIDL
    for (LPCITEMIDLIST pidl : pidls)
    {
        ::CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(pidl));
    }

    return hr;
}

// ----------------------------------------------------------------------------
// 创建 IFileOperation 实例，设置标志和父窗口
//
// 【为什么要 SetOwnerWindow】
//   不设置的话，系统的进度框/确认框没有归属窗口，可能显示在奇怪的位置，
//   甚至"看不见但一直等用户点"，导致操作永远不结束。
// ----------------------------------------------------------------------------
HRESULT CreateFileOperation(wil::com_ptr<IFileOperation>& operation, DWORD flags, HWND owner)
{
    HRESULT hr = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(operation.put()));
    if (FAILED(hr))
    {
        return hr;
    }

    if (owner != nullptr)
    {
        operation->SetOwnerWindow(owner);
    }

    return operation->SetOperationFlags(flags);
}

} // namespace

namespace shellops
{

// ============================================================================
// 复制
// ============================================================================
HRESULT CopyItems(const std::vector<std::wstring>& sourcePaths,
                  const std::wstring& targetDirectory,
                  HWND owner)
{
    wil::com_ptr<IFileOperation> operation;
    HRESULT hr = CreateFileOperation(operation, FOF_ALLOWUNDO, owner);
    if (FAILED(hr))
    {
        return hr;
    }

    wil::com_ptr<IShellItem> target;
    hr = ::SHCreateItemFromParsingName(targetDirectory.c_str(), nullptr,
                                       IID_PPV_ARGS(target.put()));
    if (FAILED(hr))
    {
        return hr;
    }

    // ------------------------------------------------------------------
    // 情况一：在同一目录里复制（Ctrl+C 之后 Ctrl+V 没有换目录）
    //
    // 这种时候如果直接 CopyItems 到同目录，系统会弹
    // 「目标文件夹和源文件夹相同」，要用户先点一下才能继续。
    // 资源管理器不会弹——它是直接生成「xxx - 副本」。
    // 所以我们自己算好新名字，用 CopyItem(..., 新名字) 逐个复制。
    // ------------------------------------------------------------------
    if (AllSourcesInTargetDirectory(sourcePaths, targetDirectory))
    {
        for (const std::wstring& sourcePath : sourcePaths)
        {
            wil::com_ptr<IShellItem> item;
            hr = ::SHCreateItemFromParsingName(sourcePath.c_str(), nullptr,
                                               IID_PPV_ARGS(item.put()));
            if (FAILED(hr))
            {
                return hr;
            }

            DWORD attributes = ::GetFileAttributesW(sourcePath.c_str());
            bool isFolder = (attributes != INVALID_FILE_ATTRIBUTES) &&
                            ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);

            std::wstring newName = MakeCopyName(targetDirectory, sourcePath, isFolder);

            // 第 4 个参数是进度回调接口，不需要就传 nullptr
            hr = operation->CopyItem(item.get(), target.get(), newName.c_str(), nullptr);
            if (FAILED(hr))
            {
                return hr;
            }
        }

        return operation->PerformOperations();
    }

    // ------------------------------------------------------------------
    // 情况二：普通的复制到别的目录，批量做（一个进度框搞定）
    // ------------------------------------------------------------------
    wil::com_ptr<IShellItemArray> items;
    hr = CreateItemArray(sourcePaths, items.put());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = operation->CopyItems(items.get(), target.get());
    if (FAILED(hr))
    {
        return hr;
    }

    return operation->PerformOperations();
}

// ============================================================================
// 移动（剪切粘贴）
// ============================================================================
HRESULT MoveItems(const std::vector<std::wstring>& sourcePaths,
                  const std::wstring& targetDirectory,
                  HWND owner)
{
    wil::com_ptr<IFileOperation> operation;
    HRESULT hr = CreateFileOperation(operation, FOF_ALLOWUNDO, owner);
    if (FAILED(hr))
    {
        return hr;
    }

    wil::com_ptr<IShellItemArray> items;
    hr = CreateItemArray(sourcePaths, items.put());
    if (FAILED(hr))
    {
        return hr;
    }

    wil::com_ptr<IShellItem> target;
    hr = ::SHCreateItemFromParsingName(targetDirectory.c_str(), nullptr,
                                       IID_PPV_ARGS(target.put()));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = operation->MoveItems(items.get(), target.get());
    if (FAILED(hr))
    {
        return hr;
    }

    return operation->PerformOperations();
}

// ============================================================================
// 删除（进回收站）
// ============================================================================
HRESULT DeleteItems(const std::vector<std::wstring>& paths, HWND owner)
{
    wil::com_ptr<IFileOperation> operation;

    // FOF_ALLOWUNDO        ：能撤销（删除进回收站，而不是直接抹掉）
    // FOF_NOCONFIRMATION   ：不弹"确实要删除吗"，和资源管理器一致
    HRESULT hr = CreateFileOperation(operation, FOF_ALLOWUNDO | FOF_NOCONFIRMATION, owner);
    if (FAILED(hr))
    {
        return hr;
    }

    wil::com_ptr<IShellItemArray> items;
    hr = CreateItemArray(paths, items.put());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = operation->DeleteItems(items.get());
    if (FAILED(hr))
    {
        return hr;
    }

    return operation->PerformOperations();
}

// ============================================================================
// 新建文件夹（自动避开重名）
// ============================================================================
HRESULT CreateNewFolder(const std::wstring& directory, std::wstring& newFolderPath)
{
    newFolderPath.clear();

    if (directory.empty())
    {
        return E_INVALIDARG;
    }

    // 目录结尾没带反斜杠就补一个
    std::wstring base = directory;
    if (base.back() != L'\\' && base.back() != L'/')
    {
        base += L'\\';
    }

    // 「新建文件夹」「新建文件夹 (2)」「新建文件夹 (3)」…… 一直试到成功
    for (int index = 1; index <= 1000; ++index)
    {
        std::wstring name = L"新建文件夹";
        if (index > 1)
        {
            name += L" (" + std::to_wstring(index) + L")";
        }

        std::wstring fullPath = base + name;

        if (::CreateDirectoryW(fullPath.c_str(), nullptr))
        {
            newFolderPath = fullPath;
            return S_OK;
        }

        if (::GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return HRESULT_FROM_WIN32(::GetLastError());
        }
    }

    return E_FAIL;
}

// ============================================================================
// 重命名
// ============================================================================
HRESULT RenameItem(const std::wstring& oldPath, const std::wstring& newName, HWND owner)
{
    if (oldPath.empty() || newName.empty())
    {
        return E_INVALIDARG;
    }

    // 文件名里不能有这些字符，自己先挡掉，免得系统再弹一个看不懂的报错
    if (newName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    }

    wil::com_ptr<IShellItem> item;
    HRESULT hr = ::SHCreateItemFromParsingName(oldPath.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr))
    {
        return hr;
    }

    wil::com_ptr<IFileOperation> operation;
    hr = CreateFileOperation(operation, FOF_ALLOWUNDO, owner);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = operation->RenameItem(item.get(), newName.c_str(), nullptr);
    if (FAILED(hr))
    {
        return hr;
    }

    return operation->PerformOperations();
}

HRESULT CreateShortcut(const std::wstring& targetPath, const std::wstring& linkPath)
{
    if (targetPath.empty() || linkPath.empty())
    {
        return E_INVALIDARG;
    }

    wil::com_ptr<IShellLinkW> shellLink;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&shellLink));
    if (FAILED(hr))
    {
        return hr;
    }

    // 指向的目标
    hr = shellLink->SetPath(targetPath.c_str());
    if (FAILED(hr))
    {
        return hr;
    }

    // 工作目录设成目标所在的目录（双击运行时相对路径才正常）
    std::wstring parentDirectory = ParentDirectoryOf(targetPath);
    if (!parentDirectory.empty())
    {
        shellLink->SetWorkingDirectory(parentDirectory.c_str());
    }

    // 图标：直接用目标自己的图标
    shellLink->SetIconLocation(targetPath.c_str(), 0);

    // 让资源管理器把"这是个文件夹快捷方式"显示正确
    if (::GetFileAttributesW(targetPath.c_str()) & FILE_ATTRIBUTE_DIRECTORY)
    {
        shellLink->SetDescription(L"文件夹快捷方式");
    }

    wil::com_ptr<IPersistFile> persistFile;
    hr = shellLink->QueryInterface(IID_PPV_ARGS(&persistFile));
    if (FAILED(hr))
    {
        return hr;
    }

    // TRUE = 记住，立即写盘
    return persistFile->Save(linkPath.c_str(), TRUE);
}

} // namespace shellops
