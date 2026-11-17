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
// 按「绝对 PIDL」删除（压缩包容器里的项）
//
// 容器里的项没有真实磁盘路径，路径字符串交给 Shell 解析不出来。
// 这里走外壳自己的右键动词（IContextMenu 的 "delete"）：
//   1) SHBindToParent 拿到父 IShellFolder 和子 PIDL；
//   2) GetUIObjectOf 要一个 IContextMenu；
//   3) InvokeCommand 执行 "delete"。
// 这也正是资源管理器删除 zip 内文件走的路，行为（弹不弹确认、要不要回收站）
// 全部交给 shell。
// ============================================================================
namespace
{
// 对单个绝对 PIDL 执行一个外壳右键动词
//
// 不能直接把动词名字符串丢给 InvokeCommand：容器（zip）里的项，其右键菜单
// 只认"菜单项 ID"，给字符串会返回 E_INVALIDARG（实测）。
// 所以先 QueryContextMenu 建一份菜单，再用 GetCommandString(GCS_VERBW)
// 把每个项的动词名读出来，找到名字匹配的那一项，最后按它的 ID 执行。
HRESULT InvokeShellVerbByPidl(const ITEMIDLIST* absolutePidl, const wchar_t* verb, HWND owner)
{
    wil::com_ptr<IShellFolder> parent;
    PCITEMID_CHILD child = nullptr;

    HRESULT hr = ::SHBindToParent(absolutePidl, IID_IShellFolder, parent.put_void(), &child);
    if (FAILED(hr))
    {
        return hr;
    }

    if (child == nullptr)
    {
        return E_FAIL;
    }

    wil::com_ptr<IContextMenu> contextMenu;
    hr = parent->GetUIObjectOf(owner, 1, &child,
                               IID_IContextMenu, nullptr, contextMenu.put_void());
    if (FAILED(hr))
    {
        return hr;
    }

    // shell 的菜单命令 ID 从 1 开始
    const UINT idFirst = 1;
    const UINT idLast  = 0x7FFF;

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    hr = contextMenu->QueryContextMenu(menu, 0, idFirst, idLast, CMF_NORMAL);

    HRESULT result = E_FAIL;
    bool    found  = false;

    if (SUCCEEDED(hr))
    {
        const int menuCount = ::GetMenuItemCount(menu);

        for (int i = 0; i < menuCount; ++i)
        {
            const UINT commandId = ::GetMenuItemID(menu, i);
            if (commandId == 0 || commandId == static_cast<UINT>(-1))
            {
                continue;   // 分隔线 / 子菜单
            }

            const UINT offset = commandId - idFirst;

            wchar_t verbName[128] = {};
            if (FAILED(contextMenu->GetCommandString(offset, GCS_VERBW, nullptr,
                                                     reinterpret_cast<CHAR*>(verbName),
                                                     ARRAYSIZE(verbName)))
                )
            {
                continue;
            }

            if (::_wcsicmp(verbName, verb) != 0)
            {
                continue;
            }

            CMINVOKECOMMANDINFOEX info = {};
            info.cbSize  = sizeof(info);
            info.fMask   = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI;
            info.hwnd    = owner;
            info.lpVerb  = MAKEINTRESOURCEA(offset);
            info.lpVerbW = MAKEINTRESOURCEW(offset);
            info.nShow   = SW_SHOWNORMAL;

            result = contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
            found  = true;
            break;
        }
    }

    ::DestroyMenu(menu);

    return found ? result : E_FAIL;
}
} // namespace

HRESULT DeleteShellItemPidls(const std::vector<std::vector<unsigned char>>& pidlBytes,
                             HWND owner)
{
    if (pidlBytes.empty())
    {
        return E_INVALIDARG;
    }

    HRESULT lastError = S_OK;
    bool anyDone = false;

    for (const std::vector<unsigned char>& bytes : pidlBytes)
    {
        if (bytes.size() < sizeof(ITEMIDLIST))
        {
            continue;
        }

        const ITEMIDLIST* pidl = reinterpret_cast<const ITEMIDLIST*>(bytes.data());

        const HRESULT hr = InvokeShellVerbByPidl(pidl, L"delete", owner);
        if (SUCCEEDED(hr))
        {
            anyDone = true;
        }
        else
        {
            lastError = hr;
        }
    }

    if (!anyDone)
    {
        return FAILED(lastError) ? lastError : E_FAIL;
    }

    return S_OK;
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

namespace
{

// HRESULT 转成 "0x80070002" 这种文本，写日志和提示框用
std::wstring HResultText(HRESULT hr)
{
    wchar_t buffer[32] = {};
    ::_snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"0x%08X",
                  static_cast<unsigned int>(hr));
    return buffer;
}

// 一级一级建目录；目录已经存在也算成功
bool EnsureDirectoryExists(const std::wstring& directory)
{
    if (directory.empty())
    {
        return false;
    }

    DWORD attributes = ::GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // 先把上级建出来，再建自己
    std::wstring parent = ParentDirectoryOf(directory);
    if (!parent.empty() && parent != directory && !EnsureDirectoryExists(parent))
    {
        return false;
    }

    ::CreateDirectoryW(directory.c_str(), nullptr);

    attributes = ::GetFileAttributesW(directory.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ----------------------------------------------------------------------------
// 备用提取通道：借 Shell.Application（资源管理器自己的自动化对象）来复制
//
// 为什么需要这条路：
//   WinRAR 给 rar / 7z / tar 提供的"只读压缩包处理器"不支持 IStream
//   （BindToObject 要 IStream 会返回 0x80004002 E_NOINTERFACE），
//   但它自己的"复制"通道是可用的：
//     Shell.Application.Namespace(压缩包).ParseName(包内路径)   → 拿到那一项
//     Shell.Application.Namespace(目标目录).CopyHere(那一项)     → 就解出来了
//   实测 zip / rar / 7z / tar 四种都能解出来（含包里的多级子目录）。
//
// 注意 CopyHere 是异步的：调用返回时文件可能还没落地，调用方要轮询等。
// ----------------------------------------------------------------------------

// 按名字调一个自动化方法/属性（拿不到 ITypeInfo，只能走 GetIDsOfNames 这一套）
HRESULT InvokeAutomation(IDispatch* object, const wchar_t* name,
                         VARIANT* arguments, unsigned int argumentCount, VARIANT* result)
{
    if (object == nullptr)
    {
        return E_POINTER;
    }

    LPOLESTR nameList[1] = {const_cast<LPOLESTR>(name)};
    DISPID id = 0;

    HRESULT hr = object->GetIDsOfNames(IID_NULL, nameList, 1, LOCALE_SYSTEM_DEFAULT, &id);
    if (FAILED(hr))
    {
        return hr;
    }

    DISPPARAMS parameters = {};
    parameters.rgvarg = arguments;
    parameters.cArgs = argumentCount;

    // DISPPARAMS 的参数是"最后一个参数在前"，调用方自己按这个顺序摆好
    return object->Invoke(id, IID_NULL, LOCALE_SYSTEM_DEFAULT,
                          DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                          &parameters, result, nullptr, nullptr);
}

// 把 BSTR 参数和返回值都收拾干净的用法：这里手写，注意每一处 VariantClear
HRESULT GetAutomationFolder(IDispatch* shellApp, const std::wstring& path,
                            wil::com_ptr<IDispatch>& folder)
{
    VARIANT argument;
    ::VariantInit(&argument);
    argument.vt = VT_BSTR;
    argument.bstrVal = ::SysAllocString(path.c_str());

    VARIANT value;
    ::VariantInit(&value);

    HRESULT hr = InvokeAutomation(shellApp, L"Namespace", &argument, 1, &value);

    ::VariantClear(&argument);

    if (FAILED(hr) || value.vt != VT_DISPATCH || value.pdispVal == nullptr)
    {
        ::VariantClear(&value);
        return FAILED(hr) ? hr : E_FAIL;
    }

    folder.attach(value.pdispVal);   // 接管这个引用，不需要再 VariantClear
    return S_OK;
}

} // namespace

// 走 Shell 自动化把一项解出来（成功时文件已经落在 outputPath）
bool ExtractViaShellAutomation(const std::wstring& containerRoot,
                               const std::wstring& innerPath,
                               const std::wstring& outputPath,
                               std::wstring& error)
{
    wil::com_ptr<IDispatch> shellApp;

    // 用 ProgID 让 COM 自己去找 CLSID：写死 GUID 在个别机器上会撞到
    // "类没有注册"（REGDB_E_CLASSNOTREG），按名字找更稳
    CLSID shellApplicationClsid = {};
    HRESULT hr = ::CLSIDFromProgID(L"Shell.Application", &shellApplicationClsid);

    if (FAILED(hr))
    {
        error = L"系统里找不到 Shell.Application（" + HResultText(hr) + L"）";
        return false;
    }

    hr = ::CoCreateInstance(shellApplicationClsid, nullptr, CLSCTX_ALL,
                            IID_PPV_ARGS(shellApp.put()));
    if (FAILED(hr) || shellApp == nullptr)
    {
        error = L"启动系统 Shell 复制通道失败（" + HResultText(hr) + L"）";
        return false;
    }

    // ---- 1) 打开压缩包，拿到它内部的那一项 ----
    wil::com_ptr<IDispatch> containerFolder;
    if (FAILED(GetAutomationFolder(shellApp.get(), containerRoot, containerFolder)))
    {
        error = L"打不开这个压缩包（" + containerRoot + L"）";
        return false;
    }

    VARIANT nameArgument;
    ::VariantInit(&nameArgument);
    nameArgument.vt = VT_BSTR;
    nameArgument.bstrVal = ::SysAllocString(innerPath.c_str());

    VARIANT itemValue;
    ::VariantInit(&itemValue);

    hr = InvokeAutomation(containerFolder.get(), L"ParseName", &nameArgument, 1, &itemValue);
    ::VariantClear(&nameArgument);

    if (FAILED(hr) || itemValue.vt != VT_DISPATCH || itemValue.pdispVal == nullptr)
    {
        ::VariantClear(&itemValue);
        error = L"压缩包里找不到这一项：" + innerPath;
        return false;
    }

    wil::com_ptr<IDispatch> folderItem;
    folderItem.attach(itemValue.pdispVal);

    // ---- 2) 准备落地目录 ----
    //
    // CopyHere 只认"文件夹"，而且落地时用的是项自己的名字，
    // 所以这里定到 outputPath 的父目录（对应包内那一层子目录），
    // 落地后再挪到 outputPath 这个准确位置，避免包内不同目录的同名文件互相覆盖。
    const std::wstring landingDirectory = ParentDirectoryOf(outputPath);

    wil::com_ptr<IDispatch> destinationFolder;
    if (FAILED(GetAutomationFolder(shellApp.get(), landingDirectory, destinationFolder)))
    {
        error = L"打不开临时目录：" + landingDirectory;
        return false;
    }

    // 这一项在包里的名字（落地文件名）
    std::wstring itemName;

    {
        VARIANT itemNameValue;
        ::VariantInit(&itemNameValue);

        if (SUCCEEDED(InvokeAutomation(folderItem.get(), L"Name", nullptr, 0, &itemNameValue)))
        {
            if (itemNameValue.vt == VT_BSTR && itemNameValue.bstrVal != nullptr)
            {
                itemName = itemNameValue.bstrVal;
            }
            ::VariantClear(&itemNameValue);
        }
    }

    if (itemName.empty())
    {
        // 拿不到名字就用目标文件名兜底
        size_t lastSeparator = outputPath.find_last_of(L"\\/");
        itemName = (lastSeparator == std::wstring::npos)
                       ? outputPath
                       : outputPath.substr(lastSeparator + 1);
    }

    std::wstring landingPath = landingDirectory;

    if (landingPath.back() != L'\\' && landingPath.back() != L'/')
    {
        landingPath += L'\\';
    }
    landingPath += itemName;

    // ---- 3) 复制（CopyHere(项, 标志)）----
    // 标志 0x14 = 静默 + 不弹确认框，和资源管理器后台解压一个效果
    VARIANT copyArguments[2];
    ::VariantInit(&copyArguments[0]);
    ::VariantInit(&copyArguments[1]);

    copyArguments[0].vt = VT_I4;
    copyArguments[0].lVal = 0x14;

    copyArguments[1].vt = VT_DISPATCH;
    copyArguments[1].pdispVal = folderItem.get();
    folderItem.get()->AddRef();     // VariantClear 会 Release，这里先补一个引用

    hr = InvokeAutomation(destinationFolder.get(), L"CopyHere", copyArguments, 2, nullptr);

    ::VariantClear(&copyArguments[0]);
    ::VariantClear(&copyArguments[1]);

    if (FAILED(hr))
    {
        error = L"系统复制通道拒绝了这个操作（" + HResultText(hr) + L"）";
        return false;
    }

    // ---- 4) 等东西真的落盘（CopyHere 是异步的，最多等 5 秒）----
    bool landed = false;
    bool isDirectory = false;

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const DWORD attributes = ::GetFileAttributesW(landingPath.c_str());

        // 文件、目录都算落地：
        // 拖出压缩包里的整个文件夹时，CopyHere 复制出来的是一项目录，
        // 以前这里按"不是目录"判断，导致文件夹永远等不到、最后报超时。
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            landed      = true;
            isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            break;
        }

        ::Sleep(50);
    }

    if (!landed)
    {
        error = L"复制超时，文件没有出现：" + landingPath;
        return false;
    }

    // ---- 5) 挪到准确位置（名字不一样时改名），再设成只读 ----
    if (_wcsicmp(landingPath.c_str(), outputPath.c_str()) != 0)
    {
        if (::GetFileAttributesW(outputPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            ::SetFileAttributesW(outputPath.c_str(), FILE_ATTRIBUTE_NORMAL);

            // 目录要先清空才能删，这里只做文件那一套；
            // 目录撞名时直接让 MoveFileEx 失败，由调用方报错（临时目录下一般不会撞）
            if (!isDirectory)
            {
                ::DeleteFileW(outputPath.c_str());
            }
        }

        if (!::MoveFileExW(landingPath.c_str(), outputPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            error = L"解出来的内容挪不到目标位置：" + outputPath;
            return false;
        }
    }

    // 只读：和系统解出来的临时文件一样，防止把包里的原件改坏。
    // 目录不加只读标记（加了会让里面新建文件变麻烦）。
    if (!isDirectory)
    {
        DWORD outputAttributes = ::GetFileAttributesW(outputPath.c_str());

        if (outputAttributes != INVALID_FILE_ATTRIBUTES)
        {
            ::SetFileAttributesW(outputPath.c_str(), outputAttributes | FILE_ATTRIBUTE_READONLY);
        }
    }

    return true;
}

std::wstring ExtractContainerEntry(const std::wstring& virtualPath,
                                   const std::wstring& containerRoot,
                                   const std::wstring& relativePath,
                                   const std::wstring& targetDirectory,
                                   std::wstring& error)
{
    error.clear();

    if (virtualPath.empty() || targetDirectory.empty())
    {
        error = L"路径参数为空";
        return L"";
    }

    // ---- 这一项在包内的相对路径（决定落到目标目录里的哪个子目录）----
    std::wstring innerPath = relativePath;

    while (!innerPath.empty() && (innerPath.front() == L'\\' || innerPath.front() == L'/'))
    {
        innerPath.erase(innerPath.begin());
    }

    if (innerPath.empty())
    {
        // 没给相对路径就用虚拟路径自己的文件名
        size_t lastSeparator = virtualPath.find_last_of(L"\\/");
        innerPath = (lastSeparator == std::wstring::npos)
                        ? virtualPath
                        : virtualPath.substr(lastSeparator + 1);
    }

    // ---- 目标文件完整路径 ----
    std::wstring outputPath = targetDirectory;

    if (outputPath.back() != L'\\' && outputPath.back() != L'/')
    {
        outputPath += L'\\';
    }
    outputPath += innerPath;

    for (wchar_t& ch : outputPath)
    {
        if (ch == L'/')
        {
            ch = L'\\';
        }
    }

    if (!EnsureDirectoryExists(ParentDirectoryOf(outputPath)))
    {
        error = L"建不出临时目录：" + ParentDirectoryOf(outputPath);
        return L"";
    }

    // ---- 上一次留下的临时文件先清掉（它是只读的，得先去掉只读属性）----
    if (::GetFileAttributesW(outputPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        ::SetFileAttributesW(outputPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::DeleteFileW(outputPath.c_str());
    }

    // ---- 解析内部项的 PIDL（容器里的路径只有 PIDL 认得出来）----
    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    HRESULT hr = ::SHParseDisplayName(virtualPath.c_str(), nullptr, &absolutePidl, 0, nullptr);

    if (FAILED(hr) || absolutePidl == nullptr)
    {
        error = L"定位容器内部项失败（" + HResultText(hr) + L"）";
        return L"";
    }

    // ---- 绑定它所在的文件夹，拿到"最后一段"的 PIDL ----
    wil::com_ptr<IShellFolder> parentFolder;
    PCUITEMID_CHILD childPidl = nullptr;

    hr = ::SHBindToParent(absolutePidl, IID_PPV_ARGS(parentFolder.put()), &childPidl);

    if (FAILED(hr) || parentFolder == nullptr || childPidl == nullptr)
    {
        ::CoTaskMemFree(absolutePidl);
        error = L"定位容器内部项失败（" + HResultText(hr) + L"）";
        return L"";
    }

    // ---- 读流：压缩包处理器把这一项的内容当成文件流交给我们 ----
    wil::com_ptr<IStream> stream;
    hr = parentFolder->BindToObject(childPidl, nullptr, IID_PPV_ARGS(stream.put()));

    ::CoTaskMemFree(absolutePidl);   // childPidl 的内存属于它，用完再放

    if (FAILED(hr) || stream == nullptr)
    {
        // WinRAR 给 rar / 7z / tar 的只读处理器要 IStream 会直接拒绝
        // （实测 0x80004002 E_NOINTERFACE），这时改走 Shell 自动化那条复制通道
        // （见上面 ExtractViaShellAutomation 的注释）。zip 一般走不到这里。
        if (ExtractViaShellAutomation(containerRoot, innerPath, outputPath, error))
        {
            return outputPath;
        }
        return L"";
    }

    // ---- 写到目标文件：设成只读，跟系统解出来的临时文件一致（防止改坏原件）----
    HANDLE file = ::CreateFileW(outputPath.c_str(),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_READONLY,
                                nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        error = L"建不出临时文件：" + outputPath;
        return L"";
    }

    // ---- 一块一块读出来写进文件 ----
    std::vector<char> buffer(64 * 1024);
    bool succeeded = true;

    while (true)
    {
        ULONG readBytes = 0;
        hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &readBytes);

        if (FAILED(hr))
        {
            succeeded = false;
            error = L"读压缩包内容失败（" + HResultText(hr) + L"）";
            break;
        }

        if (readBytes == 0)
        {
            break;   // 读完了
        }

        DWORD writtenBytes = 0;
        if (!::WriteFile(file, buffer.data(), readBytes, &writtenBytes, nullptr) ||
            writtenBytes != readBytes)
        {
            succeeded = false;
            error = L"写临时文件失败：" + outputPath;
            break;
        }
    }

    ::CloseHandle(file);

    if (!succeeded)
    {
        // 写了一半的文件不要留下
        ::SetFileAttributesW(outputPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::DeleteFileW(outputPath.c_str());
        return L"";
    }

    return outputPath;
}

} // namespace shellops
