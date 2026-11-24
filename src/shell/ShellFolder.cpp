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
//         → GetAttributesOf   判断"能不能进去"（压缩包也算容器）
//         → SHCreateItemWithParent + IShellItem2  取大小、修改时间、绝对路径
//         → GetFileAttributesW 校正"显示上是不是文件夹"（zip/rar 这类按文件显示）
//
// COM 释放说明：
//   PIDL 必须用 CoTaskMemFree 释放（不是 delete）；
//   接口指针全部用 wil::com_ptr 自动管理，避免漏掉 Release 导致泄漏。
// ============================================================================

#include "ShellFolder.h"
#include "../core/Logger.h"
#include "../core/Settings.h"   // 显示隐藏文件 / 系统文件两个开关

// WIL：COM 智能指针与 HRESULT 工具
#include <wil/com.h>
#include <wil/result.h>

#include <shlobj.h>       // SHCreateItemFromParsingName 等
#include <shobjidl.h>     // IShellItem2 / IEnumIDList / IShellFolder
#include <shellapi.h>     // ShellExecuteExW / SHELLEXECUTEINFOW（按 PIDL 打开要用）
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
//   "C:\Users\user\Downloads" → "C:\Users\user"
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
// 长路径前缀
//
// GetFileAttributesW 遇到超过 260 字符的路径会直接失败，得加 \\?\（UNC 用 \\?\UNC\）。
// 只在真的超长时才加，短路径保持原样（加前缀反而会让某些相对语义失效）。
// ----------------------------------------------------------------------------
static std::wstring WithLongPathPrefix(const std::wstring& path)
{
    if (path.size() < 260 || path.compare(0, 4, L"\\\\?\\") == 0)
    {
        return path;
    }

    if (path[0] == L'\\' && path[1] == L'\\')
    {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }

    return L"\\\\?\\" + path;
}

// ----------------------------------------------------------------------------
// 判断「主文件夹」（顶层桌面）里的某一项，是"系统项"还是"桌面上的东西"
//
// 顶层桌面会把两类东西混在一起枚举出来：
//   系统项：此电脑、回收站、控制面板、库、图库、网络、主文件夹，
//           还有音乐、下载、图片、视频、文档、桌面这些"系统给的目录入口"，
//           以及各个驱动器、用户目录、OneDrive；
//   桌面项：桌面目录（含公用桌面）里真实的文件、文件夹、快捷方式。
//
// 用户在「主文件夹」里要的是「系统项排前面、桌面上的东西排后面」，
// 所以这里按"是不是落在桌面目录下"来分：
//   落在桌面目录之下（但不等于目录本身）→ 桌面项；
//   其余（虚拟项、系统目录入口、驱动器…）→ 系统项。
// 桌面目录用 KnownFolder 现查，用户把桌面搬到别处也能正确判定。
// ----------------------------------------------------------------------------
static bool StartsWithNoCase(const std::wstring& text, const std::wstring& prefix)
{
    if (text.size() < prefix.size())
    {
        return false;
    }

    return ::_wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// 取桌面目录的全路径（只查一次，之后缓存；C++11 的静态局部变量初始化是线程安全的）
static const std::wstring& DesktopDirectory()
{
    static const std::wstring cached = []
    {
        std::wstring result;

        LPWSTR path = nullptr;

        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)) && path != nullptr)
        {
            result = path;
            ::CoTaskMemFree(path);
        }

        return result;
    }();

    return cached;
}

static const std::wstring& PublicDesktopDirectory()
{
    static const std::wstring cached = []
    {
        std::wstring result;

        LPWSTR path = nullptr;

        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_PublicDesktop, 0, nullptr, &path)) && path != nullptr)
        {
            result = path;
            ::CoTaskMemFree(path);
        }

        return result;
    }();

    return cached;
}

bool IsDesktopSystemItem(const std::wstring& fullPath)
{
    // 没有路径的纯虚拟项，一定是系统项
    if (fullPath.empty())
    {
        return true;
    }

    // "::{20D04FE0-…}" 这种桌面绝对解析名，是命名空间项
    if (fullPath.rfind(L"::", 0) == 0)
    {
        return true;
    }

    // 落在桌面目录里的才算"桌面上的东西"；
    // 注意"桌面"这个系统目录入口本身路径就等于桌面目录，要算系统项。
    const std::wstring& desktop  = DesktopDirectory();
    const std::wstring& pubDesk  = PublicDesktopDirectory();

    if (!desktop.empty() && (fullPath == desktop || StartsWithNoCase(fullPath, desktop + L"\\")))
    {
        // 等于桌面目录本身 → 是"桌面"这个系统入口，算系统项；
        // 桌面目录底下的东西 → 是桌面上的文件，不算系统项。
        return (fullPath == desktop);
    }

    if (!pubDesk.empty() && (fullPath == pubDesk || StartsWithNoCase(fullPath, pubDesk + L"\\")))
    {
        return (fullPath == pubDesk);
    }

    // 其余真实磁盘路径里，只有系统已知目录入口本身（下载 / 文档 / 图片 / 音乐 / 视频，
    // 桌面上面已单独判过）才算系统项；它们是 Shell 重定向的已知文件夹，位置记在用户
    // 配置里，需要在主文件夹里单独提到前面、且不允许剪切/删除。
    //
    // 注意这里是「路径精确等于已知目录本身」，不是「落在目录下面」：已知目录里的
    // 子文件夹/子文件属于普通项，不该跟着被提到前面。
    std::wstring knownFolderName;
    if (shell::KnownFolderDisplayName(fullPath, knownFolderName))
    {
        return true;
    }

    // 其它真实磁盘路径（开发目录如 C:\git\…、dist 安装包、网络盘 Z:\、AppData 工作区、
    // 盘根目录等）一律按普通项处理，不再兜底算系统项。
    return false;
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

    // 大小
    //
    // 这里不再先用 isFolder 过滤：调用时 isFolder 还是「Shell 认得容器吗」的
    // 中间值（zip 这类压缩包当时算容器），而它们恰恰是需要显示真实大小的。
    // 文件夹取 PKEY_Size 一般拿不到（返回失败，size 保持 0），不影响列表显示。
    {
        UINT64 size = 0;
        if (SUCCEEDED(item2->GetUInt64(PKEY_Size, &size)))
        {
            entry.size = size;
        }
    }
}

// ----------------------------------------------------------------------------
// 从「解析名」里抠出盘符
//
// 此电脑里的盘，fullPath 拿到的是解析名（如 "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\C:"），
// 不是 "C:\"。这里把末尾那个 "C:" 找出来，好去查它的类型和容量。
// 不是盘的项返回 0（'\0'）。
// ----------------------------------------------------------------------------
wchar_t DriveLetterFromParsingName(const std::wstring& parsingName)
{
    const size_t length = parsingName.size();
    if (length < 2)
    {
        return 0;
    }

    // 形如 "…\C:" 或 "…\C:\" —— 从末尾往前看，允许结尾带一个反斜杠
    size_t colonPos = length - 1;
    if (parsingName[colonPos] == L'\\' && length >= 3)
    {
        --colonPos;
    }

    if (parsingName[colonPos] != L':')
    {
        return 0;
    }

    const size_t letterPos = colonPos - 1;
    if (letterPos == 0 && length >= 3 && false)
    {
        return 0;
    }

    if (!::iswalpha(parsingName[letterPos]))
    {
        return 0;
    }

    // 盘符前必须是开头或者反斜杠，避免把文件名里冒号误判成盘
    if (letterPos > 0 && parsingName[letterPos - 1] != L'\\')
    {
        return 0;
    }

    return static_cast<wchar_t>(::towupper(parsingName[letterPos]));
}

// ----------------------------------------------------------------------------
// 给盘符项补上「驱动器类型 + 容量」
//
// 只在后台枚举线程里调用（查容量会碰磁盘，不能在界面线程做）。
// 网络盘也查（系统自带的此电脑同样显示网络盘的容量和占用条），
// 只有「无效根目录」和「光驱里没盘」这两种查了也没意义的情况才跳过。
// ----------------------------------------------------------------------------
void FillDriveInfo(shell::ShellEntry& entry)
{
    const wchar_t letter = DriveLetterFromParsingName(entry.fullPath);
    if (letter == 0)
    {
        return;
    }

    const std::wstring rootPath = std::wstring(1, letter) + L":\\";

    // 先把盘符记下来：排序要用（驱动器按盘符字母排，不按显示名）。
    // 放在下面几个提前 return 之前，保证空光驱这类查不到容量的项也有盘符。
    entry.driveLetter = letter;

    entry.driveType = static_cast<int>(::GetDriveTypeW(rootPath.c_str()));

    // 无效的根目录：查容量没意义，跳过
    // （网络盘照查——系统自带的此电脑就显示网络盘的容量和占用条）
    if (entry.driveType == DRIVE_NO_ROOT_DIR)
    {
        return;
    }

    ULARGE_INTEGER freeBytes  = {};
    ULARGE_INTEGER totalBytes = {};
    ULARGE_INTEGER totalFree  = {};

    if (::GetDiskFreeSpaceExW(rootPath.c_str(), &freeBytes, &totalBytes, &totalFree))
    {
        entry.diskFree  = freeBytes.QuadPart;
        entry.diskTotal = totalBytes.QuadPart;
    }
}

} // namespace

namespace shell
{

// ----------------------------------------------------------------------------
// 快速通道：普通磁盘目录用一次 FindFirstFile 扫完
//
// 【为什么加这条路】
//   以前所有目录都走 Shell 的 IShellFolder 枚举，每个子项还要再问三次属性
//   （路径、大小、修改时间）。本地盘每项一两毫秒不明显，网络盘（\\server\share）
//   每项都是一次网络往返——实测 261 个 PDF 要 1025 ms，全花在逐项查属性上。
//
//   FindFirstFile / FindNextFile 是批量接口：一次请求拿回一整批目录项，
//   名字、大小、修改时间、属性都在同一份数据里，同样 261 项通常几十毫秒。
//
// 【什么时候不用这条路】（照样走原来的 Shell 枚举，行为一点不变）
//   · 虚拟命名空间：此电脑、控制面板、主文件夹、网络、回收站、库；
//   · 压缩包本身和压缩包内部（.zip 里那些项磁盘上并不存在）；
//   · 桌面目录（用户桌面 / 公共桌面）：Shell 那边是"两个目录合并显示"的视图，
//     自己扫目录会漏掉另一半；
//   · 目录不存在、没权限、网络断了：退回 Shell 老路，由它给出和以前一样的错误。
//
// 【和 Shell 枚举对齐的细节】
//   · .lnk / .url 的显示名去掉后缀（系统也是这么显示的，否则列表里会突然多出 ".lnk"）；
//     真实路径仍然带后缀，图标、打开、重命名都用真实路径；
//   · 隐藏 / 系统文件的过滤规则和两个开关的语义完全照旧；
//   · 压缩包扩展名（zip/rar/7z…）照旧标成"能双击进去的容器文件"；
//   · 大小、修改时间取自同一个磁盘数据源，和 Shell 给的一致。
// ----------------------------------------------------------------------------

// 显示名：系统对快捷方式类不显示后缀，这里照做，保持列表文字和以前一样
static std::wstring PlainEntryDisplayName(const std::wstring& fileName)
{
    static const wchar_t* const kHideSuffixes[] = { L".lnk", L".url" };

    for (const wchar_t* suffix : kHideSuffixes)
    {
        const size_t suffixLength = ::wcslen(suffix);
        if (fileName.size() > suffixLength &&
            ::_wcsicmp(fileName.c_str() + (fileName.size() - suffixLength), suffix) == 0)
        {
            return fileName.substr(0, fileName.size() - suffixLength);
        }
    }

    return fileName;
}

// 桌面目录（用户桌面 / 公共桌面）判断：这两个目录 Shell 会合并显示，不能自己扫
static bool IsDesktopDirectoryPath(const std::wstring& path)
{
    auto samePath = [](const std::wstring& left, const std::wstring& right)
    {
        std::wstring a = left;
        std::wstring b = right;
        while (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) { a.pop_back(); }
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) { b.pop_back(); }
        return !a.empty() && ::_wcsicmp(a.c_str(), b.c_str()) == 0;
    };

    return samePath(path, DesktopDirectory()) || samePath(path, PublicDesktopDirectory());
}

static bool TryEnumeratePlainDirectory(const std::wstring& path,
                                       const core::CancellationToken* token,
                                       EnumerateResult& result)
{
    // 目录本身得是磁盘上真实存在的目录；否则交给 Shell 老路
    const DWORD folderAttributes = ::GetFileAttributesW(WithLongPathPrefix(path).c_str());
    if (folderAttributes == INVALID_FILE_ATTRIBUTES ||
        (folderAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return false;
    }
    if (!IsFileSystemPath(path) || IsDesktopDirectoryPath(path))
    {
        return false;
    }

    // 拼搜索串：C:\ → C:\*；\\server\share → \\server\share\*
    std::wstring pattern = path;
    if (!pattern.empty() && (pattern.back() == L'\\' || pattern.back() == L'/'))
    {
        pattern.pop_back();
    }
    pattern += L"\\*";

    WIN32_FIND_DATAW findData = {};
    HANDLE findHandle = ::FindFirstFileW(WithLongPathPrefix(pattern).c_str(), &findData);
    if (findHandle == INVALID_HANDLE_VALUE)
    {
        // 空目录是正常情况：FindFirstFile 报"找不到文件"，这时算成功、零条目
        if (::GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            result.hr = S_OK;
            return true;
        }

        // 没权限、网络断开等：退回 Shell 老路，让它给出和以前一样的提示
        return false;
    }

    const core::Settings& settings   = core::Settings::Instance();
    const bool            needHidden = settings.NeedHiddenItems();

    // 拼 fullPath 的前缀：保证"目录 + 一个反斜杠 + 文件名"
    std::wstring folderPrefix = path;
    if (folderPrefix.empty() || (folderPrefix.back() != L'\\' && folderPrefix.back() != L'/'))
    {
        folderPrefix += L'\\';
    }

    bool cancelled = false;

    do
    {
        if (token != nullptr && token->IsCancelled())
        {
            cancelled = true;
            break;
        }

        const std::wstring fileName(findData.cFileName);

        // "." 和 ".." 正常不会出现，保险起见跳过
        if (fileName == L"." || fileName == L"..")
        {
            continue;
        }

        const bool isFolder = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isHidden = (findData.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        const bool isSystem = (findData.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;

        // 隐藏 / 系统文件的显示规则，和 Shell 那条老路完全一致：
        //   两个开关都关 → 隐藏项不显示（系统文件通常也带隐藏位，跟着一起不显示）
        //   开了开关   → 系统文件只看"显示系统文件"，普通隐藏项只看"显示隐藏文件"
        if (!needHidden)
        {
            if (isHidden)
            {
                continue;
            }
        }
        else
        {
            const bool shouldShow = isSystem ? settings.ShowSystemFiles()
                                             : (!isHidden || settings.ShowHiddenFiles());
            if (!shouldShow)
            {
                continue;
            }
        }

        ShellEntry entry;
        entry.name     = PlainEntryDisplayName(fileName);
        entry.fullPath = folderPrefix + fileName;

        // 文件夹：能进去；压缩包（zip/rar/7z…）：显示按文件、双击能进去看内容
        entry.isFolder        = isFolder;
        entry.isContainerFile = (!isFolder) && IsContainerFileExtension(entry.fullPath);
        entry.canEnter        = isFolder || entry.isContainerFile;

        // 这两个标记只在开了"显示隐藏/系统"时才填（和以前一样：没开就不淡化）
        if (needHidden)
        {
            entry.isHidden     = isHidden;
            entry.isSystemFile = isSystem;
        }

        if (!isFolder)
        {
            entry.size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) |
                         static_cast<uint64_t>(findData.nFileSizeLow);
        }

        entry.modified = findData.ftLastWriteTime;

        // 「主文件夹」里要区分系统项和桌面上的东西，别的目录里恒为 false
        entry.isSystemItem = IsDesktopSystemItem(entry.fullPath);

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
    while (::FindNextFileW(findHandle, &findData));

    ::FindClose(findHandle);

    result.cancelled = cancelled;
    result.hr        = S_OK;
    return true;
}

EnumerateResult EnumerateFolder(const std::wstring& path,
                                const core::CancellationToken* token)
{
    EnumerateResult result;
    result.path = path;

    // ------------------------------------------------------------------
    // 这个目录本身是不是压缩包这类「容器」
    //
    // 判定只看真实磁盘属性：路径存在、且不是目录 —— 那就是压缩包/容器文件
    // （C:\a.zip）。Shell 能把它当文件夹枚举，但里面的项都是虚拟项，
    // 菜单能力也和普通目录不同（压缩包内部一律只读），所以先记下来。
    // 虚拟命名空间（"::{GUID}"）没有磁盘属性，按普通目录处理。
    // ------------------------------------------------------------------
    if (IsFileSystemPath(path))
    {
        const DWORD ownAttributes = ::GetFileAttributesW(path.c_str());

        if (ownAttributes != INVALID_FILE_ATTRIBUTES &&
            (ownAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            // 路径自己就是磁盘上的文件 → 压缩包本身
            result.isContainer = true;
        }
        else if (ownAttributes == INVALID_FILE_ATTRIBUTES)
        {
            // 磁盘上没有这个路径 → 可能是在压缩包内部（C:\a.rar\sub）
            //
            // 容器里的子目录也算"容器内部"：里面的项同样是虚拟项，
            // 菜单能力、能不能粘贴、能不能剪切都和压缩包内部一致。
            // 之前只按"路径自己是文件"判定，进了包内子目录这些判断就全部失效。
            const std::wstring containerRoot = ContainerRootForPath(path);

            if (!containerRoot.empty())
            {
                result.isContainer = true;
            }
        }
    }

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

    // 父目录的绝对 PIDL（子项拼绝对 PIDL 用）。
    // 放在 try 外面是为了函数末尾统一释放——包括中途抛异常的情况。
    PIDLIST_ABSOLUTE folderPidl = nullptr;

    // ------------------------------------------------------------------
    // 快速通道：普通磁盘目录（不是压缩包、不是桌面这种合并视图）
    //
    // 放在 COM 初始化之后：这条路虽然不建 IShellFolder，但判断"系统项"时会问
    // 系统已知文件夹（SHGetKnownFolderItem），没初始化 COM 会直接崩。
    // 能用就直接返回，不建 IShellFolder、不逐项查属性，网络盘大目录省得最多。
    // ------------------------------------------------------------------
    if (!result.isContainer && IsFileSystemPath(path) && !IsTopDesktopPath(path))
    {
        if (TryEnumeratePlainDirectory(path, token, result))
        {
            if (needUninitialize)
            {
                ::CoUninitialize();
            }
            return result;
        }
    }

    try
    {
        // ---------------- 第 1-2 步：路径 → IShellFolder ----------------
        wil::com_ptr<IShellFolder> folder;

        // 父文件夹自身的绝对 PIDL。命名空间目录、以及压缩包这类容器里的子项
        // 都要拿它拼出「绝对 PIDL」——虚拟项没有可解析的磁盘路径，
        // 打开、拖出、删除都得靠 PIDL 才管用（详见 ShellEntry::pidl 注释）。
        // 所有权：函数返回前由下面的 ILFree 归还。
        // 物理目录（C:\…）不必拼，保持为 nullptr。
        bool pathIsVirtual = false;
        if (IsTopDesktopPath(path))
        {
            // 命名空间最顶层那个「桌面」：没有解析名，只能直接拿桌面根文件夹
            THROW_IF_FAILED(::SHGetDesktopFolder(folder.put()));

            // 桌面根文件夹的 PIDL 就是「空 PIDL」，枚举出的子项本身就是绝对 PIDL，
            // 所以这里不需要往外取一个父 PIDL 来拼。
            pathIsVirtual = true;
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

            // 什么情况下要给子项存 PIDL：
            //   1. 当前目录是 Shell 命名空间（"::{GUID}"、"shell:…"）——解析名拼不回来；
            //   2. 当前目录是压缩包这类容器 —— 里面的项是虚拟子项，磁盘上根本没有它们，
            //      只能用 PIDL 打开/拖出/删除。
            // 普通磁盘目录里的条目用 fullPath 打开就够了，省掉每项一次 PIDL 拼接。
            const bool pathIsNamespace = (path.compare(0, 2, L"::") == 0) ||
                                         (path.compare(0, 6, L"shell:") == 0);

            pathIsVirtual = pathIsNamespace || result.isContainer;

            if (pathIsVirtual)
            {
                // 取这个目录自身的绝对 PIDL；取不到就退回「不拼」——
                // 那说明这个目录路径本身能被字符串解析，子项也能用字符串打开。
                ::SHGetIDListFromObject(static_cast<IUnknown*>(shellItem.get()), &folderPidl);
            }
        }

        // ---------------- 第 3 步：拿枚举器 ----------------
        // SHCONTF_FOLDERS   —— 要文件夹
        // SHCONTF_NONFOLDERS—— 要文件
        // SHCONTF_INCLUDEHIDDEN —— 只有配置里开了"显示隐藏文件"或"显示系统文件"才带。
        //   不带的话隐藏项根本枚举不出来（和资源管理器默认一致，也更快）；
        //   带上了以后还要在第 4 步按两个开关逐项过滤：
        //     · 系统文件（带 FILE_ATTRIBUTE_SYSTEM，通常同时也带隐藏位）只看 showSystemFiles；
        //     · 普通隐藏项只看 showHiddenFiles。
        //   这就是资源管理器那两个选项的语义。
        wil::com_ptr<IEnumIDList> enumList;

        const core::Settings& settings = core::Settings::Instance();
        const bool            needHidden = settings.NeedHiddenItems();

        DWORD enumFlags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
        if (needHidden)
        {
            enumFlags |= SHCONTF_INCLUDEHIDDEN;
        }

        THROW_IF_FAILED(folder->EnumObjects(nullptr, enumFlags, enumList.put()));

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

            // ----------------------------------------------------------
            // 先按「显示隐藏文件 / 显示系统文件」两个开关过滤
            //
            // 只有开了其中至少一个才会走到这里（needHidden），每项多一次
            // GetAttributesOf（本地项很快，几万项的目录也只是多几十毫秒）。
            // 放在取名字、取大小之前判断：被过滤掉的项不做后续工作，省开销。
            // ----------------------------------------------------------
            bool isHidden     = false;
            bool isSystemFile = false;

            if (needHidden)
            {
                // 第二个参数要求 PCUITEMID_CHILD_ARRAY（每项是 const ITEMIDLIST*，
                // 注意它带的 __unaligned 限定符，所以这里用 PCUITEMID_CHILD 中转，
                // 直接写 const ITEMIDLIST* 会触发 C4090 警告）。
                PCUITEMID_CHILD attributeTarget = childPidl;
                SFGAOF          attrs           = SFGAO_HIDDEN | SFGAO_SYSTEM;

                if (SUCCEEDED(folder->GetAttributesOf(1, &attributeTarget, &attrs)))
                {
                    isHidden     = (attrs & SFGAO_HIDDEN) != 0;
                    isSystemFile = (attrs & SFGAO_SYSTEM) != 0;

                    // 系统文件只受 showSystemFiles 管；普通隐藏项只受 showHiddenFiles 管
                    const bool shouldShow = isSystemFile ? settings.ShowSystemFiles()
                                                        : (!isHidden || settings.ShowHiddenFiles());
                    if (!shouldShow)
                    {
                        ::CoTaskMemFree(childPidl);
                        childPidl = nullptr;
                        fetched   = 0;
                        continue;
                    }
                }
            }

            ShellEntry entry;
            entry.isHidden     = isHidden;
            entry.isSystemFile = isSystemFile;

            // Shell 那边的「能不能进去」：普通文件夹为真，zip / rar / 7z / cab
            // 这些压缩包也为真（它带「压缩存档」处理器，双击能进去看内容）
            entry.canEnter = IsFolder(folder.get(), childPidl);
            entry.name     = GetDisplayName(folder.get(), childPidl);
            FillExtraInfo(folder.get(), childPidl, entry);

            // ----------------------------------------------------------
            // 校正「显示语义」：Shell 说是容器，但磁盘上其实是文件
            //
            // zip / rar / 7z / cab 这类压缩包，Shell 因为它们带「压缩存档」
            // 文件夹处理器而报 SFGAO_FOLDER。不校正的话，一个 700KB 的 zip 会：
            //   · 类型列写「文件夹」、大小列空白
            //   · 排进排序里的「文件夹优先」那一段
            //   · F2 重命名时整名选中（按文件夹处理）
            // 所以这里按真实文件属性纠回来：不是目录的，按文件显示；
            // canEnter 保持为真，双击仍然能进去浏览（和资源管理器一致）。
            //
            // 只有 canEnter 为真的项才多查这一次磁盘（普通文件压根不查），
            // 一个目录里文件夹通常只占少数，开销可以忽略。
            // ----------------------------------------------------------
            entry.isFolder = entry.canEnter;

            DWORD realAttributes = INVALID_FILE_ATTRIBUTES;

            // 需要查磁盘的只有两种：Shell 说能进去的（可能是文件夹，也可能是 zip），
            // 以及扩展名属于压缩包的（rar / 7z 这类没装"压缩包当文件夹"处理器时，
            // Shell 只当它是普通文件，但它确实是个压缩包）。
            const bool extensionIsContainer = IsContainerFileExtension(entry.fullPath);

            if ((entry.canEnter || extensionIsContainer) &&
                !entry.fullPath.empty() && IsFileSystemPath(entry.fullPath))
            {
                realAttributes = ::GetFileAttributesW(WithLongPathPrefix(entry.fullPath).c_str());

                if (realAttributes != INVALID_FILE_ATTRIBUTES &&
                    (realAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    if (entry.canEnter)
                    {
                        entry.isFolder        = false;
                        entry.isContainerFile = true;
                    }
                    else if (extensionIsContainer)
                    {
                        // Shell 没把它报成文件夹，但扩展名是压缩包：
                        // 跟 zip / 7z 一样统一按「容器文件」处理，
                        // 双击走打开方式账本（默认本程序），就能进去看内容了。
                        entry.canEnter        = true;
                        entry.isFolder        = false;
                        entry.isContainerFile = true;
                    }
                }
            }

            // ----------------------------------------------------------
            // 补查「真正的」隐藏 / 系统文件属性
            //
            // 上面那次 GetAttributesOf 里 SFGAO_SYSTEM 在 Shell 的含义是"系统对象"，
            // 不是 FILE_ATTRIBUTE_SYSTEM —— pagefile.sys 这类受保护的系统文件用它
            // 永远判不出来，结果"显示受保护的操作系统文件"这个开关形同虚设。
            // 所以拿到绝对路径后再问一次文件属性（只对本地文件系统有意义，
            // Shell 命名空间里的虚拟项路径为空，直接跳过）。
            // ----------------------------------------------------------
            // 驱动器项（盘符根，如 C:\）豁免这一段：
            // 它的路径就是盘根目录，GetFileAttributesW 取到的是「根目录」的属性，
            // 而系统盘 / 某些保留盘、同步映射盘的根目录天然带 SYSTEM（常同时带
            // HIDDEN）位。拿根目录属性去判定盘本身，会把驱动器误标成系统文件，
            // 只开「显示隐藏文件」时就被过滤掉——资源管理器的此电脑从不按盘根
            // 目录属性过滤驱动器。这里直接跳过，驱动器始终保留。
            const bool entryIsDrive = DriveLetterFromParsingName(entry.fullPath) != 0;

            if (needHidden && !entry.fullPath.empty() && !entryIsDrive)
            {
                // 上面校正 isFolder 时可能已经问过一次了，能复用就复用，
                // 别对同一个路径重复查两次磁盘
                const DWORD fileAttributes = (realAttributes != INVALID_FILE_ATTRIBUTES)
                                                 ? realAttributes
                                                 : ::GetFileAttributesW(
                                                       WithLongPathPrefix(entry.fullPath).c_str());

                if (fileAttributes != INVALID_FILE_ATTRIBUTES)
                {
                    entry.isHidden     = entry.isHidden     || (fileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
                    entry.isSystemFile = entry.isSystemFile || (fileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;

                    // 按真实属性再判一次：前面那次判定拿不到系统位，
                    // 会把 pagefile.sys 这种项当成普通文件放过去。
                    const bool shouldShow = entry.isSystemFile
                                                ? settings.ShowSystemFiles()
                                                : (!entry.isHidden || settings.ShowHiddenFiles());
                    if (!shouldShow)
                    {
                        ::CoTaskMemFree(childPidl);
                        childPidl = nullptr;
                        fetched   = 0;
                        continue;
                    }
                }
            }

            // 「主文件夹」里要把"系统项"和"桌面上的东西"分开排，先把标记算出来。
            // 其他目录里判断出来也都是 false，不影响原有排序。
            entry.isSystemItem = IsDesktopSystemItem(entry.fullPath);

            // ----------------------------------------------------------
            // 丢掉那个"幽灵控制面板"
            //
            // 顶层桌面会枚举出一个也叫「控制面板」的项，解析名是
            // ::{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}。
            // 这个 GUID 连系统自己都打不开（Shell.Application.NameSpace 直接报
            // "找不到元素 0x80070490"），它只是个没有图标、双击没反应的残项。
            // 真正能用的「控制面板」是 ::{26EE0668-…}（9 项首页），我们保留那一个。
            // 在枚举阶段就把它扔掉，列表里就不会出现两个「控制面板」。
            // ----------------------------------------------------------
            if (!entry.fullPath.empty() &&
                ::_wcsicmp(entry.fullPath.c_str(),
                           L"::{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}") == 0)
            {
                ::CoTaskMemFree(childPidl);
                childPidl = nullptr;
                fetched   = 0;
                continue;
            }

            // ----------------------------------------------------------
            // 把这一项的「绝对 PIDL」拷成一段纯字节存进快照
            //
            // 在 Shell 命名空间目录、以及压缩包这类容器里做（控制面板、此电脑、
            // C:\a.zip 内部）。原因：这些目录里的项解析名是"…\0\显示名"或者根本
            // 不存在磁盘路径，Shell 用字符串解析不回来，打开/拖出/删除都必须用 PIDL。
            //
            // 注意必须在 CoTaskMemFree(childPidl) 之前拷，childPidl 是 COM 内存。
            // ----------------------------------------------------------
            if (pathIsVirtual)
            {
                // 顶层「桌面」的父就是根，子项 PIDL 已经绝对；其他命名空间要拼
                PIDLIST_ABSOLUTE absolutePidl = childPidl;
                PIDLIST_ABSOLUTE combinedPidl = nullptr;

                if (folderPidl != nullptr)
                {
                    combinedPidl = ::ILCombine(folderPidl, childPidl);
                    absolutePidl = combinedPidl;
                }

                if (absolutePidl != nullptr)
                {
                    const UINT cbSize = ::ILGetSize(absolutePidl);

                    if (cbSize > 0)
                    {
                        const unsigned char* beginPtr =
                            reinterpret_cast<const unsigned char*>(absolutePidl);
                        entry.pidl.assign(beginPtr, beginPtr + cbSize);
                    }
                }

                // 只释放刚拼出来的那个，childPidl 由下面统一释放
                if (combinedPidl != nullptr)
                {
                    ::ILFree(combinedPidl);
                }
            }

            // PIDL 是 COM 分配的内存，必须用 CoTaskMemFree 释放
            ::CoTaskMemFree(childPidl);
            childPidl = nullptr;
            fetched = 0;

            // 名称为空的项（罕见）跳过，避免列表里出现空白行
            if (entry.name.empty())
            {
                continue;
            }

            // 是盘符的话，顺手把「驱动器类型 + 容量」查出来（平铺视图要用）
            if (entry.isFolder)
            {
                FillDriveInfo(entry);
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

    // 释放父目录 PIDL（SHGetIDListFromObject 分配，与 ILFree 配对）
    if (folderPidl != nullptr)
    {
        ::ILFree(folderPidl);
        folderPidl = nullptr;
    }

    // 与 CoInitializeEx 配对
    if (needUninitialize)
    {
        ::CoUninitialize();
    }

    return result;
}

// ============================================================================
// 判断是不是"常用位置"那几个系统目录，并给出它的名字
// ============================================================================

// 左侧导航条最上面那一组（视频也带上，别的地方用到同样认）
static const KNOWNFOLDERID* const kKnownFolderIds[] =
{
    &FOLDERID_Desktop,
    &FOLDERID_Downloads,
    &FOLDERID_Documents,
    &FOLDERID_Pictures,
    &FOLDERID_Music,
    &FOLDERID_Videos,
};

// 取一个已知文件夹的"真实路径"和"系统显示名"
// 返回 false 表示这个文件夹在这台机器上取不到（被重定向到不存在的盘之类）
static bool GetKnownFolderInfo(const KNOWNFOLDERID& id,
                               std::wstring& folderPath,
                               std::wstring& displayName)
{
    folderPath.clear();
    displayName.clear();

    LPWSTR rawPath = nullptr;

    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &rawPath)) || rawPath == nullptr)
    {
        return false;
    }

    folderPath.assign(rawPath);
    ::CoTaskMemFree(rawPath);

    // 去掉可能的尾斜杠，后面拼接/比较都按"不带尾斜杠"统一
    while (folderPath.size() > 3 &&
           (folderPath.back() == L'\\' || folderPath.back() == L'/'))
    {
        folderPath.pop_back();
    }

    // 名字向系统要（SIGDN_NORMALDISPLAY），这样和资源管理器里显示的一致
    wil::com_ptr<IShellItem> item;

    if (SUCCEEDED(::SHGetKnownFolderItem(id, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(item.put())) &&
                  item != nullptr))
    {
        LPWSTR name = nullptr;

        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name != nullptr)
        {
            displayName.assign(name);
            ::CoTaskMemFree(name);
        }
    }

    return !folderPath.empty() && !displayName.empty();
}

bool KnownFolderDisplayName(const std::wstring& path, std::wstring& displayName)
{
    displayName.clear();

    if (path.empty())
    {
        return false;
    }

    struct CachedFolder
    {
        std::wstring path;
        std::wstring name;
    };

    // 已知文件夹的位置在运行期间不会变（用户改了位置要重启程序才生效），查一次缓存住。
    // 列表里每个条目都要问一次"是不是桌面上的东西"，261 个文件就是 1800 多次，
    // 不缓存的话这里比自己扫目录还贵。
    static const std::vector<CachedFolder> cachedFolders = []
    {
        std::vector<CachedFolder> list;

        for (const KNOWNFOLDERID* id : kKnownFolderIds)
        {
            std::wstring folderPath;
            std::wstring name;

            if (GetKnownFolderInfo(*id, folderPath, name))
            {
                list.push_back(CachedFolder{ folderPath, name });
            }
        }

        return list;
    }();

    for (const auto& folder : cachedFolders)
    {
        if (::_wcsicmp(folder.path.c_str(), path.c_str()) == 0)
        {
            displayName = folder.name;
            return true;
        }
    }

    return false;
}

bool KnownFolderAncestor(const std::wstring& path,
                         std::wstring& folderPathOut,
                         std::wstring& displayNameOut)
{
    folderPathOut.clear();
    displayNameOut.clear();

    if (path.empty())
    {
        return false;
    }

    std::wstring bestPath;   // 命中里最长的那一个 = 最深的祖先
    std::wstring bestName;

    for (const KNOWNFOLDERID* id : kKnownFolderIds)
    {
        std::wstring folderPath;
        std::wstring name;

        if (!GetKnownFolderInfo(*id, folderPath, name))
        {
            continue;
        }

        // 命中条件：就是它自己，或者 path 在它下面（必须整段对齐，避免
        // "Downloads2" 这种前缀相同的兄弟目录被误认成子目录）
        bool matched = (::_wcsicmp(folderPath.c_str(), path.c_str()) == 0);

        if (!matched &&
            folderPath.size() < path.size() &&
            ::_wcsnicmp(folderPath.c_str(), path.c_str(), folderPath.size()) == 0 &&
            (path[folderPath.size()] == L'\\' || path[folderPath.size()] == L'/'))
        {
            matched = true;
        }

        if (!matched)
        {
            continue;
        }

        if (!bestPath.empty() && folderPath.size() <= bestPath.size())
        {
            continue;   // 已经有更深的了
        }

        bestPath = folderPath;
        bestName = name;
    }

    if (bestPath.empty())
    {
        return false;
    }

    folderPathOut  = bestPath;
    displayNameOut = bestName;
    return true;
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
// ----------------------------------------------------------------------------
// 盘符根判断："D:\" 这种三个字符的路径
// ----------------------------------------------------------------------------
bool IsDriveRootPath(const std::wstring& path)
{
    if (path.size() != 3)
    {
        return false;
    }

    if (path[1] != L':' || path[2] != L'\\')
    {
        return false;
    }

    return (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
}

// ----------------------------------------------------------------------------
// 真实文件系统路径判断：盘符路径（X:\...）或 UNC 路径（\\server\share）
// 控制面板那类虚拟项走的是 "::{GUID}..."，这里会返回 false
// ----------------------------------------------------------------------------
bool IsFileSystemPath(const std::wstring& path)
{
    if (path.size() < 2)
    {
        return false;
    }

    // 盘符路径 X:\...
    if (path[1] == L':')
    {
        return (path[0] >= L'A' && path[0] <= L'Z') ||
               (path[0] >= L'a' && path[0] <= L'z');
    }

    // UNC 路径 \\server\share
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        return true;
    }

    return false;
}

bool IsContainerExtension(const std::wstring& extension)
{
    // 常见压缩包/容器扩展名（系统把它们的属性报成"文件夹"，其实磁盘上是文件）
    static const wchar_t* const kContainerExtensions[] = {
        L".zip",  L".zipx", L".rar", L".7z",  L".cab", L".tar",  L".gz",
        L".tgz",  L".bz2",  L".tbz", L".tbz2", L".xz", L".txz",  L".lz",
        L".lzma", L".zst",  L".arj", L".lzh", L".lha", L".iso",
    };

    for (const wchar_t* one : kContainerExtensions)
    {
        if (extension == one)
        {
            return true;
        }
    }

    return false;
}

bool IsContainerFileExtension(const std::wstring& path)
{
    // 直接按 PathFindExtensionW 取扩展名并转小写：
    // 这里不引用 ShellOpenWith 的 ExtensionKeyForPath，避免模块之间多一条依赖
    const wchar_t* dot = ::PathFindExtensionW(path.c_str());
    std::wstring extension = (dot == nullptr) ? L"" : dot;

    for (wchar_t& ch : extension)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }

    return IsContainerExtension(extension);
}

// ----------------------------------------------------------------------------
// 这个路径属于哪个压缩包（容器根）
//
// 容器内部的路径（C:\a.rar\sub\inner.txt）磁盘上并不存在，但从路径往上追，
// 第一段"磁盘上真的是文件"的路径就是压缩包本身（C:\a.rar）。
//
// 注意：只看 path 的**上级**，不看 path 自己——
//   · 传进来的是压缩包文件本身（C:\a.rar）→ 返回它的上级目录，不是它自己；
//   · 只是想判断"这一项在不在容器里"，用 IsPathInsideContainer。
//
// 只读文件属性，不碰 COM，很快（判断菜单能力时可以放心调）。
// ----------------------------------------------------------------------------
std::wstring ContainerRootForPath(const std::wstring& path)
{
    if (path.empty() || !IsFileSystemPath(path))
    {
        return L"";
    }

    std::wstring current = path;

    while (true)
    {
        const size_t lastSeparator = current.find_last_of(L"\\/");

        // 追到盘符根还什么都没有 → 不在任何容器里
        if (lastSeparator == std::wstring::npos || lastSeparator <= 2)
        {
            return L"";
        }

        current = current.substr(0, lastSeparator);

        const DWORD attributes = ::GetFileAttributesW(current.c_str());

        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            continue;   // 这一层也是虚拟的（容器内部的子目录），继续往上
        }

        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return current;   // 是文件 → 这就是压缩包
        }

        // 撞上真实目录 → 说明这一串只是"真实目录下面的名字"，不在容器里
        return L"";
    }
}

// ----------------------------------------------------------------------------
// 这一项是不是压缩包内部的虚拟项
//
// 判定：磁盘上不存在（既不是文件也不是目录），但往上能追到一个压缩包。
//   · C:\a.rar\inner.txt      → true （磁盘上没有，归 a.rar）
//   · C:\a.rar\sub            → true （容器内部的子目录也算）
//   · C:\a.rar                → false（它自己就是磁盘上的文件）
//   · D:\普通目录\普通文件     → false
// ----------------------------------------------------------------------------
bool IsPathInsideContainer(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    // 磁盘上存在（不管文件还是目录）→ 不是容器内部的虚拟项
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    return !ContainerRootForPath(path).empty();
}

// ----------------------------------------------------------------------------
// 这个路径是不是 .zip（只有 zip 有系统自带的可写处理器）
// ----------------------------------------------------------------------------
// 这个目录能不能接收"写入"（粘贴、拖动放入）
//
//   · 磁盘上的普通目录          → 能
//   · 压缩包内部（不分格式）    → 不能（一律只读，zip 也一样）
//   · 虚拟命名空间（::{GUID}）  → 按"能"处理，维持原来的行为
//
// 只查文件属性，不碰 COM。粘贴 / 拖放前调一次，成本可忽略；
// 右键菜单里不用它（菜单只用内存标记，保持零成本）。
// ----------------------------------------------------------------------------
bool IsWritableTargetDirectory(const std::wstring& directory)
{
    if (directory.empty())
    {
        return false;
    }

    // 虚拟命名空间不是真实目录，这里不拦（拦也拦不准，交给下层去失败）
    if (!IsFileSystemPath(directory))
    {
        return true;
    }

    const DWORD attributes = ::GetFileAttributesW(directory.c_str());

    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        // 磁盘上的目录能写；磁盘上的文件 = 压缩包本身，它内部只读
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // 磁盘上没有这个路径：压缩包内部（含包内子目录）一律只读；真不存在的路径也不能写
    return false;
}

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
    //    C:\Users\user\Downloads → C:\Users\user     （资源管理器就是这样）
    //    C:\Users\user\Desktop   → C:\Users\user
    //    C:\Users\user           → C:\Users
    //    C:\Users                 → C:\
    //    C:\                      → 此电脑
    //
    //    这里必须用路径层级，不能调 IShellItem::GetParent：
    //    系统把它算成「下载/桌面 → 桌面容器」，和资源管理器给出的目标不一致
    //    （资源管理器状态下，下载的向上按钮明明白白写着「上移到 user」）。
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

// 控制面板里那些"applet"（双击应该弹出面板窗口，而不是进到列表里看）
//
// 它们的解析名长这样：
//   ::{26EE0668-A00A-44D7-9371-BEB064C98683}\0\::{7B81BE6A-…}   （程序和功能）
//   ::{26EE0668-A00A-44D7-9371-BEB064C98683}\0\字体               （字体）
//
// 【为什么要固定成"不导航"】
//   以前是靠"我们自己的枚举器能不能列出东西"来判断的，结果同一个项两次测试
//   会给出不同答案（冷/热 COM 状态下 EnumerateFolder 结果不一致），表现就是
//   "这一项有时候进目录、有时候弹面板"，很怪。
//   控制面板里的 applet 在系统里双击就是弹面板，所以这里钉死，不再探测。
//
// 唯一例外是「Windows 工具」（::{D20EA4E1-…}），它真的是个容器，
// 系统双击也是进去看工具列表，所以照旧按容器处理。
static bool IsControlPanelAppletPath(const std::wstring& path)
{
    static const wchar_t* const kControlPanelChildPrefix =
        L"::{26EE0668-A00A-44D7-9371-BEB064C98683}\\0\\";

    static const wchar_t* const kWindowsToolsPath =
        L"::{26EE0668-A00A-44D7-9371-BEB064C98683}\\0\\::{D20EA4E1-3957-11D2-A40B-0C5020524153}";

    if (path.empty())
    {
        return false;
    }

    // 「Windows 工具」是个真容器，不算 applet
    if (::_wcsicmp(path.c_str(), kWindowsToolsPath) == 0)
    {
        return false;
    }

    const size_t prefixLength = ::wcslen(kControlPanelChildPrefix);
    if (path.size() < prefixLength)
    {
        return false;
    }

    return ::_wcsnicmp(path.c_str(), kControlPanelChildPrefix, prefixLength) == 0;
}

// 这些顶层命名空间项"此刻可能是空的"，但双击必须能进去看到空列表，
// 不能被当成 applet 弹一个系统窗口。这里精确匹配（不加前缀匹配），
// 免得把它们下面那些真正的 applet 也一起放过。
static bool IsKnownEmptyContainerRoot(const std::wstring& path)
{
    static const wchar_t* const kRoots[] = {
        L"::TopDesktop",                                // 本程序自己的顶层桌面伪路径
        L"::{645FF040-5081-101B-9F08-00AA002F954E}",    // 回收站
        L"::{26EE0668-A00A-44D7-9371-BEB064C98683}",    // 控制面板
        L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}",    // 网络
        L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",    // 此电脑
        L"::{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}",    // 桌面
        L"::{031E4825-7B94-4DC3-B131-E946B44C8DD5}",    // 库
        L"::{F874310E-B6B7-47DC-BC84-B9E6B38F5903}",    // 主文件夹
        L"::{6D809377-6AF0-444B-8957-A3773F02200E}",    // 用户文件夹（另一个 CLSID）
    };

    // 比较时去掉末尾反斜杠，忽略大小写
    std::wstring normalized = path;
    while (!normalized.empty() && (normalized.back() == L'\\' || normalized.back() == L'/'))
    {
        normalized.pop_back();
    }

    for (const wchar_t* root : kRoots)
    {
        if (::_wcsicmp(normalized.c_str(), root) == 0)
        {
            return true;
        }
    }

    return false;
}

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

    // 回收站等"可能为空"的顶层容器，直接算容器，不必探测
    if (IsKnownEmptyContainerRoot(path))
    {
        return true;
    }

    // 控制面板里的 applet：一律不导航，交给双击那条路按 PIDL 弹面板（和系统一致）
    if (IsControlPanelAppletPath(path))
    {
        return false;
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

            // 能绑定出 IShellFolder 的才算候选容器；纯 applet 这一步就会失败
            if (SUCCEEDED(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder))) &&
                folder != nullptr)
            {
                wil::com_ptr<IEnumIDList> enumList;
                const DWORD              flags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;

                // 判据升级版：光"能拿到枚举接口"不够——控制面板里那些 applet
                // （恢复、安全和维护、凭据管理器…）同样能拿到接口，只是枚举出来 0 项。
                // 所以必须真的枚举出至少一项，才算"能进去看到东西的容器"。
                //
                // 例外：空回收站、空的库这种"确实是容器、只是现在空的"项，
                // 枚举出来也是 0 项；它们由下面的精确白名单兜住，
                // 否则双击空回收站会弹出系统窗口。
                if (SUCCEEDED(folder->EnumObjects(nullptr, flags, enumList.put())) &&
                    enumList != nullptr)
                {
                    LPITEMIDLIST child = nullptr;
                    ULONG        fetched = 0;

                    if (SUCCEEDED(enumList->Next(1, &child, &fetched)) && fetched > 0 && child != nullptr)
                    {
                        // 光"这个命名空间里有一项"还不够 —— 我们自己的枚举器未必能解析
                        // 这种复合解析名（`::{26EE0668-…}\0\::{7B81BE6A-…}` 这种，
                        // 比如「存储空间」），真导航过去只会得到空列表，看起来就是"打不开"。
                        // 所以再用我们自己的枚举器试一次：它能不能列出东西，才是"能不能进去"。
                        EnumerateResult probe = EnumerateFolder(path, nullptr);
                        isContainer = (probe.entries.size() >= 1);

                        ::CoTaskMemFree(child);
                    }
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

// ============================================================================
// 用系统默认动词"打开"一个 Shell 项（等价于资源管理器里双击）
// ============================================================================

bool OpenShellItem(HWND owner, const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool    needUninitialize = SUCCEEDED(initHr);

    bool opened = false;

    try
    {
        wil::com_ptr<IShellItem> item;

        // 先按解析名建 IShellItem；建不出来就用 PIDL 兜底
        HRESULT hr = ::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()));

        if (FAILED(hr) || item == nullptr)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            SFGAOF           attrs = 0;

            if (SUCCEEDED(::SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, &attrs)) &&
                pidl != nullptr)
            {
                hr = ::SHCreateItemFromIDList(pidl, IID_PPV_ARGS(item.put()));
                ::CoTaskMemFree(pidl);
            }
        }

        if (SUCCEEDED(hr) && item != nullptr)
        {
            // BHID_SFUIObject 拿到的就是"这一项自己的右键菜单对象"，
            // 它的默认动词和资源管理器双击时执行的是同一个
            wil::com_ptr<IContextMenu> contextMenu;

            if (SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(contextMenu.put()))) &&
                contextMenu != nullptr)
            {
                CMINVOKECOMMANDINFOEX info = {};
                info.cbSize  = sizeof(info);
                // fMask 里两个位：
                //   CMIC_MASK_UNICODE      = 0x00004000  按 lpVerbW 走宽字符
                //   CMIC_MASK_FLAG_NO_UI   = 0x00000400  不让外壳扩展自己弹错误框
                // 这里直接写字面值：项目定义了 WIN32_LEAN_AND_MEAN，
                // 这两个宏（等价于 SEE_MASK_*）在头文件里被裁掉了，用名字编不过。
                info.fMask   = 0x00004000 | 0x00000400;
                info.hwnd    = owner;
                info.lpVerb  = "open";
                info.lpVerbW = L"open";
                info.nShow   = SW_SHOWNORMAL;

                HRESULT invokeHr = contextMenu->InvokeCommand(
                    reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
                opened = SUCCEEDED(invokeHr);

                if (!opened)
                {
                    // 退一步：用上下文菜单里的第一项（资源管理器里"打开"就是第一项）。
                    // 有些 applet 的默认动词不叫 "open"（比如 cplopen），只认菜单项序号。
                    HMENU menu = ::CreatePopupMenu();

                    if (menu != nullptr)
                    {
                        int idMax = contextMenu->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);

                        if (idMax > 0)
                        {
                            info.lpVerb  = MAKEINTRESOURCEA(0);
                            info.lpVerbW = MAKEINTRESOURCEW(0);

                            invokeHr = contextMenu->InvokeCommand(
                                reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
                            opened = SUCCEEDED(invokeHr);
                        }

                        ::DestroyMenu(menu);
                    }
                }
            }
        }
    }
    catch (const wil::ResultException& e)
    {
        LogIfFailed(e.GetErrorCode(), L"打开 Shell 项", path);
        opened = false;
    }
    catch (...)
    {
        Logger::Error(L"打开 Shell 项出现未知异常，路径=" + path);
        opened = false;
    }

    if (needUninitialize)
    {
        ::CoUninitialize();
    }

    return opened;
}

// ============================================================================
// 按 PIDL 打开 Shell 项
//
// 这是打开"控制面板 applet / 打印机 / 特殊命名空间项"唯一可靠的方式：
// 那些项的解析名形如 "::{26EE0668-…}\0\字体"，Shell 自己解析不回来
// （SHCreateItemFromParsingName 失败、Shell.Application.NameSpace 返回 null），
// 所以拿字符串永远打不开。资源管理器不靠字符串——它手里一直握着枚举出来的
// PIDL，我们这里就是把那份 PIDL 原样用上。
// ============================================================================

bool OpenShellItemByPidl(HWND owner, const std::vector<unsigned char>& pidlBytes)
{
    // 字节为空（或短到不成一个 PIDL）= 这条是普通文件/文件夹，
    // 调用方应该改走路径那套逻辑，这里直接拒绝，别白开一次 COM
    if (pidlBytes.size() < sizeof(USHORT) * 2)
    {
        return false;
    }

    HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool    needUninitialize = SUCCEEDED(initHr);

    bool opened = false;

    try
    {
        // 这段字节就是当初枚举出来的绝对 PIDL 原样拷贝，这里还原成指针。
        // PIDL 内部只有偏移量、没有指针，所以跨线程搬运字节是安全的。
        PCIDLIST_ABSOLUTE pidl =
            reinterpret_cast<PCIDLIST_ABSOLUTE>(const_cast<unsigned char*>(pidlBytes.data()));

        // --------------------------------------------------------------
        // 首选：ShellExecuteEx + 绝对 PIDL
        //
        // 这是系统官方认可的"按 IDList 执行默认动词"，就是资源管理器双击走的路。
        // lpVerb/lpIDList 在 SHELLEXECUTEINFO 里是同一个 union 成员，
        // 给了 lpIDList 就等于"用这一项的默认动词"，也就是双击。
        //
        // 为什么优先用它而不是 IContextMenu：
        //   IContextMenu 那条路在控制面板 applet 上会回 E_INVALIDARG(0x80070057)，
        //   实测「字体」的菜单能查出 45 项，但不论用 "open" 还是用第一项序号调用
        //   InvokeCommand 都被拒（部分 cpl 外壳扩展对 CMINVOKECOMMANDINFO 很挑）。
        //   ShellExecuteEx 没有这些参数讲究，兼容性反而更好。
        // --------------------------------------------------------------
        SHELLEXECUTEINFOW execInfo = {};
        execInfo.cbSize   = sizeof(execInfo);
        // fMask 三个位（WIN32_LEAN_AND_MEAN 下宏名编不过，直接写字面值）：
        //   SEE_MASK_IDLIST         = 0x00000004  这次执行用下面的 lpIDList 定位目标
        //   SEE_MASK_INVOKEIDLIST   = 0x0000000C  用 IDList 的默认动词（双击行为）
        //   SEE_MASK_FLAG_NO_UI     = 0x00000400  不让外壳扩展自己弹错误框
        execInfo.fMask    = 0x00000004 | 0x0000000C | 0x00000400;
        execInfo.hwnd     = owner;
        execInfo.lpIDList = const_cast<ITEMIDLIST*>(pidl);
        execInfo.nShow    = SW_SHOWNORMAL;

        if (::ShellExecuteExW(&execInfo) != FALSE)
        {
            opened = true;
            Logger::Info(L"按 PIDL 打开成功（ShellExecuteEx）");
        }
        else
        {
            Logger::Warn(L"ShellExecuteEx 按 PIDL 打开失败，HRESULT=" +
                         Logger::FormatHresult(static_cast<HRESULT>(::GetLastError())));
        }

        // --------------------------------------------------------------
        // 备用：IShellItem → IContextMenu → InvokeCommand
        // ------------------------------------------------------------------
        if (!opened)
        {
            wil::com_ptr<IShellItem> item;
            THROW_IF_FAILED(::SHCreateItemFromIDList(pidl, IID_PPV_ARGS(item.put())));


        // BHID_SFUIObject = "这一项自己的交互对象"，拿到的 IContextMenu
        // 和资源管理器双击时用的是同一个
        wil::com_ptr<IContextMenu> contextMenu;
        THROW_IF_FAILED(item->BindToHandler(nullptr, BHID_SFUIObject,
                                            IID_PPV_ARGS(contextMenu.put())));

        CMINVOKECOMMANDINFOEX info = {};
        info.cbSize = sizeof(info);
        // fMask 只留 CMIC_MASK_FLAG_NO_UI(0x0400)：不让外壳扩展自己弹错误框。
        // 之前这里还带了 CMIC_MASK_UNICODE(0x4000)，结果 InvokeCommand 直接回
        // E_INVALIDARG(0x80070057)—— 部分控制面板的外壳扩展不认 EX 结构里的
        // 宽字符标志，所以这里退回纯 ANSI（动词 "open" 本身就是 ASCII，够用）。
        // 写字面值是因为 WIN32_LEAN_AND_MEAN 下这两个宏名编不过。
        info.fMask  = 0x00000400;
        info.hwnd   = owner;
        info.nShow  = SW_SHOWNORMAL;

        // 先按 "open" 试：绝大多数项（含"设备和打印机"里的打印机）认这个动词
        info.lpVerb  = "open";
        info.lpVerbW = nullptr;

        HRESULT invokeHr = contextMenu->InvokeCommand(
            reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
        opened = SUCCEEDED(invokeHr);

        if (!opened)
        {
            // 退一步：用上下文菜单里的第一项。
            // 控制面板 applet 的默认动词常常不叫 "open"（例如 cplopen），
            // 只认菜单项序号。QueryContextMenu 的 idCmdFirst 传 1，
            // 则 MAKEINTRESOURCEA(0) 表示"第一项"，也就是双击时的默认动作。
            HMENU menu = ::CreatePopupMenu();

            if (menu != nullptr)
            {
                const int idMax = contextMenu->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);

                // 菜单项数记进日志：0 或负值说明这个 PIDL 拿到的菜单是空的，
                // 那就是 PIDL 本身不对，得往前查枚举时的拼接。
                Logger::Info(L"按 PIDL 打开：字节数=" + std::to_wstring(pidlBytes.size()) +
                             L"，菜单项数=" + std::to_wstring(idMax));

                if (idMax > 0)
                {
                    info.lpVerb  = MAKEINTRESOURCEA(0);
                    info.lpVerbW = nullptr;

                    invokeHr = contextMenu->InvokeCommand(
                        reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
                    opened = SUCCEEDED(invokeHr);
                }

                ::DestroyMenu(menu);
            }
        }

        // 两种动词都没成功：把真实 HRESULT 记下来，方便对着日志查是哪一步不认
            if (!opened)
            {
                Logger::Error(L"按 PIDL 打开失败，HRESULT=" + Logger::FormatHresult(invokeHr));
            }
        }
    }
    catch (const wil::ResultException& e)
    {
        LogIfFailed(e.GetErrorCode(), L"按 PIDL 打开 Shell 项", L"");
        opened = false;
    }
    catch (...)
    {
        Logger::Error(L"按 PIDL 打开 Shell 项出现未知异常");
        opened = false;
    }

    if (needUninitialize)
    {
        ::CoUninitialize();
    }

    return opened;
}

} // namespace shell
