// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ExplorerProxy.cpp
//
// 「explorer.exe 薄代理」的部署 / 升级 / 删除。
// 细节说明见 ExplorerProxy.h 的头部注释，这里只讲实现要点：
//
//   1. 认亲（判断某个 explorer.exe 是不是我们放的）
//        读它的版本资源 ProductName，等于 "Yexplorer Launcher" 才算我们的。
//        这个值写死在代理 exe 里（src/launcher/res/launcher.rc），
//        不依赖注册表，重装、换路径都认得出来。
//
//   2. 部署要管理员
//        System32 是受保护目录，普通权限复制会失败。所以主程序用
//        ShellExecuteEx(runas) 把自己以管理员身份再启动一次，
//        带上 --deploy-proxy 参数，由那个提权实例完成复制，然后弹窗报告结果。
//        用户会看到一次 UAC，同意即可。
//
//   3. 只动自己放的
//        目标已存在且不是我们的 → 只弹提醒，绝不覆盖。
// ============================================================================

#include "shell/ExplorerProxy.h"

#include "core/Logger.h"

#include "../../res/resource.h"   // IDR_EXPLORER_PROXY：内嵌在主程序里的代理二进制

#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")

namespace
{

// 代理 exe 的版本资源里，我们用来认亲的产品名（必须和 launcher.rc 里一致）
const wchar_t* kProxyProductName = L"Yexplorer Launcher";

// 提权子进程的命令行开关
const wchar_t* kArgDeployProxy = L"--deploy-proxy";
const wchar_t* kArgRemoveProxy = L"--remove-proxy";

// 取当前进程的 exe 全路径
std::wstring OwnExePath()
{
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    if (length == 0)
    {
        return std::wstring();
    }
    return std::wstring(buffer, length);
}

// 取当前进程 exe 所在目录（不带末尾反斜杠）
std::wstring OwnExeDirectory()
{
    const std::wstring full = OwnExePath();
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : full.substr(0, slash);
}

// System32\explorer.exe —— 就是我们要部署的位置
std::wstring SystemExplorerPath()
{
    wchar_t dir[MAX_PATH] = {};
    if (::GetSystemDirectoryW(dir, ARRAYSIZE(dir)) == 0)
    {
        return L"C:\\Windows\\System32\\explorer.exe";
    }
    return std::wstring(dir) + L"\\explorer.exe";
}

bool FileExists(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 读某个 exe 版本资源里的一个字符串字段（读不到返回空串）
std::wstring FileVersionString(const std::wstring& path, const wchar_t* fieldName)
{
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
    {
        return std::wstring();
    }

    std::vector<BYTE> buffer(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, buffer.data()))
    {
        return std::wstring();
    }

    struct LangAndCodePage
    {
        WORD language;
        WORD codePage;
    };

    LangAndCodePage* translation = nullptr;
    UINT translationLength = 0;
    if (!::VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<void**>(&translation), &translationLength) ||
        translationLength < sizeof(LangAndCodePage))
    {
        return std::wstring();
    }

    wchar_t subBlock[128] = {};
    ::swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s",
                 translation[0].language, translation[0].codePage, fieldName);

    wchar_t* value = nullptr;
    UINT valueLength = 0;
    if (!::VerQueryValueW(buffer.data(), subBlock, reinterpret_cast<void**>(&value), &valueLength) ||
        value == nullptr)
    {
        return std::wstring();
    }
    return std::wstring(value);
}

// 认亲：这个文件是不是我们放的 explorer.exe 代理
bool IsOursProxy(const std::wstring& path)
{
    if (!FileExists(path))
    {
        return false;
    }

    const std::wstring product = FileVersionString(path, L"ProductName");
    const std::wstring original = FileVersionString(path, L"OriginalFilename");

    return _wcsicmp(product.c_str(), kProxyProductName) == 0 &&
           _wcsicmp(original.c_str(), L"explorer.exe") == 0;
}

// 两个文件内容是否完全一致（分块比较，避免把整个文件一次读进内存）
bool SameFileContents(const std::wstring& a, const std::wstring& b)
{
    WIN32_FILE_ATTRIBUTE_DATA infoA = {};
    WIN32_FILE_ATTRIBUTE_DATA infoB = {};
    if (!::GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &infoA) ||
        !::GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &infoB))
    {
        return false;
    }
    if (infoA.nFileSizeHigh != infoB.nFileSizeHigh || infoA.nFileSizeLow != infoB.nFileSizeLow)
    {
        return false;
    }

    HANDLE fileA = ::CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE fileB = ::CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (fileA == INVALID_HANDLE_VALUE || fileB == INVALID_HANDLE_VALUE)
    {
        if (fileA != INVALID_HANDLE_VALUE) { ::CloseHandle(fileA); }
        if (fileB != INVALID_HANDLE_VALUE) { ::CloseHandle(fileB); }
        return false;
    }

    const DWORD kChunk = 8192;
    std::vector<BYTE> bufferA(kChunk);
    std::vector<BYTE> bufferB(kChunk);
    bool same = true;

    for (;;)
    {
        DWORD readA = 0;
        DWORD readB = 0;
        const bool okA = ::ReadFile(fileA, bufferA.data(), kChunk, &readA, nullptr) != FALSE;
        const bool okB = ::ReadFile(fileB, bufferB.data(), kChunk, &readB, nullptr) != FALSE;

        if (!okA || !okB || readA != readB)
        {
            same = false;
            break;
        }
        if (readA == 0)
        {
            break;   // 两边同时读完
        }
        if (::memcmp(bufferA.data(), bufferB.data(), readA) != 0)
        {
            same = false;
            break;
        }
    }

    ::CloseHandle(fileA);
    ::CloseHandle(fileB);
    return same;
}

// ----------------------------------------------------------------------------
// 内嵌在主程序里的代理二进制（RCDATA 资源 IDR_EXPLORER_PROXY）
//
// 为什么要内嵌：发布包只想要一个 FileExplorer.exe，不想再附带 explorer-proxy.exe。
// 部署时把资源取出来写到临时文件，再复制进 System32，用完删掉临时文件。
// 资源内容来自 res\explorer-proxy-x64.exe（Launcher.vcxproj 的产物副本），
// 代理代码改过之后要跑 tools\update-proxy-payload.ps1 刷新。
// ----------------------------------------------------------------------------

// 从自身资源里取出代理二进制；没有这个资源（比如用老的构建）就返回 false
bool LoadEmbeddedProxy(std::vector<BYTE>& outData)
{
    HINSTANCE module = ::GetModuleHandleW(nullptr);
    if (module == nullptr)
    {
        return false;
    }

    const HRSRC resource =
        ::FindResourceW(module, MAKEINTRESOURCEW(IDR_EXPLORER_PROXY), RT_RCDATA);
    if (resource == nullptr)
    {
        return false;
    }

    const DWORD size = ::SizeofResource(module, resource);
    if (size == 0)
    {
        return false;
    }

    const HGLOBAL handle = ::LoadResource(module, resource);
    if (handle == nullptr)
    {
        return false;
    }

    const void* data = ::LockResource(handle);
    if (data == nullptr)
    {
        return false;
    }

    // 资源在进程生命周期内一直有效，这里拷一份，调用方随便用
    const BYTE* begin = static_cast<const BYTE*>(data);
    outData.assign(begin, begin + size);
    return true;
}

// 把代理字节写到临时文件，成功后 outPath 是文件全路径（调用方负责删除）
bool WriteProxyToTempFile(const std::vector<BYTE>& data, std::wstring& outPath)
{
    wchar_t tempDir[MAX_PATH] = {};
    if (::GetTempPathW(ARRAYSIZE(tempDir), tempDir) == 0)
    {
        return false;
    }

    // 统一收进 %TEMP%\Yexp\proxy\（目录已存在时 CreateDirectory 返回失败，忽略即可）
    const std::wstring root = std::wstring(tempDir) + L"Yexp";
    ::CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring dir = root + L"\\proxy";
    ::CreateDirectoryW(dir.c_str(), nullptr);

    outPath = dir + L"\\explorer-proxy-x64.exe";

    HANDLE file = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        outPath.clear();
        return false;
    }

    const DWORD size    = static_cast<DWORD>(data.size());
    DWORD       written = 0;
    const bool  ok      = ::WriteFile(file, data.data(), size, &written, nullptr) != FALSE &&
                          written == size;

    ::CloseHandle(file);

    if (!ok)
    {
        ::DeleteFileW(outPath.c_str());
        outPath.clear();
    }
    return ok;
}

// 目标文件的内容是否和内存里的代理完全一致（大小先比，再分块比字节）
bool SameFileAsMemory(const std::wstring& path, const std::vector<BYTE>& data)
{
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
    {
        return false;
    }
    if (info.nFileSizeHigh != 0 ||
        info.nFileSizeLow != static_cast<DWORD>(data.size()))
    {
        return false;
    }

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const DWORD      kChunk = 8192;
    std::vector<BYTE> buffer(kChunk);
    size_t           offset = 0;
    bool             same   = true;

    for (;;)
    {
        DWORD read = 0;
        if (::ReadFile(file, buffer.data(), kChunk, &read, nullptr) == FALSE)
        {
            same = false;
            break;
        }
        if (read == 0)
        {
            break;
        }
        if (offset + read > data.size() ||
            ::memcmp(buffer.data(), data.data() + offset, read) != 0)
        {
            same = false;
            break;
        }
        offset += read;
    }

    if (offset != data.size())
    {
        same = false;
    }

    ::CloseHandle(file);
    return same;
}

// 找本程序自带的代理文件（编译产物 / 旧版发布包里才会有）
// 现在正常路径是「内嵌资源」，这个函数只作为兜底：
//   1) exe 同目录\explorer-proxy.exe             ← 老发布包 / PostBuild 复制
//   2) exe 同目录\launcher\explorer.exe           ← 如果按目录分开放
//   3) exe 同目录\..\..\launcher\x64\explorer.exe ← 开发树：bin\x64\Release → bin\launcher\x64
std::wstring FindLocalProxyPath()
{
    const std::wstring dir = OwnExeDirectory();

    std::wstring candidates[3];
    candidates[0] = dir + L"\\explorer-proxy.exe";
    candidates[1] = dir + L"\\launcher\\explorer.exe";
    candidates[2] = dir + L"\\..\\..\\launcher\\x64\\explorer.exe";

    for (const std::wstring& candidate : candidates)
    {
        if (IsOursProxy(candidate))
        {
            wchar_t full[MAX_PATH * 2] = {};
            if (::GetFullPathNameW(candidate.c_str(), ARRAYSIZE(full), full, nullptr) > 0)
            {
                return full;
            }
            return candidate;
        }
    }

    return std::wstring();
}

// 以管理员身份重新启动自己，执行参数里指定的动作
bool RunElevated(const std::wstring& parameters, HWND owner)
{
    const std::wstring exe = OwnExePath();
    if (exe.empty())
    {
        return false;
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";           // 触发 UAC
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info))
    {
        const DWORD error = ::GetLastError();
        if (error == ERROR_CANCELLED)
        {
            ::MessageBoxW(owner,
                          L"没有获得管理员权限，System32 里的 explorer.exe 代理没有变。\n\n"
                          L"这是可选的：没有它，微信等程序点「打开所在文件夹」仍会开系统资源管理器。",
                          L"已取消", MB_ICONINFORMATION | MB_OK);
        }
        else
        {
            ::MessageBoxW(owner,
                          (L"启动管理员进程失败（错误码 " + std::to_wstring(error) + L"）。").c_str(),
                          L"文件管理器", MB_ICONWARNING | MB_OK);
        }
        return false;
    }

    // 等提权实例干完活（它会自己弹结果框，用户点了确定才退出）
    ::WaitForSingleObject(info.hProcess, 5 * 60 * 1000);

    DWORD exitCode = 2;
    ::GetExitCodeProcess(info.hProcess, &exitCode);
    ::CloseHandle(info.hProcess);

    return exitCode == 0;
}

}  // namespace

namespace shell
{

ProxyState QuerySystemProxyState()
{
    const std::wstring target = SystemExplorerPath();

    if (!FileExists(target))
    {
        return ProxyState::Missing;
    }

    if (!IsOursProxy(target))
    {
        return ProxyState::Foreign;
    }

    // 是我们的。再和当前自带的代理比一下，判断要不要替换
    // 正常的构建里代理是内嵌资源，直接拿内存里的字节和目标文件比；
    // 万一没有资源（老的构建），退回按外部文件比较。
    std::vector<BYTE> embedded;
    if (LoadEmbeddedProxy(embedded))
    {
        return SameFileAsMemory(target, embedded) ? ProxyState::OursUpToDate
                                                 : ProxyState::OursOutdated;
    }

    const std::wstring source = FindLocalProxyPath();
    if (source.empty())
    {
        // 连自带的代理都找不到（发布包不完整），当作“需要重新部署”
        return ProxyState::OursOutdated;
    }

    return SameFileContents(target, source) ? ProxyState::OursUpToDate
                                            : ProxyState::OursOutdated;
}

bool DeployProxyIfNeeded(HWND owner)
{
    const ProxyState state = QuerySystemProxyState();

    switch (state)
    {
    case ProxyState::Missing:
        Logger::Info(L"代理部署：System32 里还没有 explorer.exe，开始部署");
        return RunElevated(kArgDeployProxy, owner);

    case ProxyState::OursOutdated:
        Logger::Info(L"代理部署：System32 里的 explorer.exe 是我们放的旧版本，开始替换");
        return RunElevated(kArgDeployProxy, owner);

    case ProxyState::OursUpToDate:
        Logger::Info(L"代理部署：System32 里已经是最新的代理，无需变动");
        return true;

    case ProxyState::Foreign:
    default:
        Logger::Info(L"代理部署：System32 里已有一个不是本程序放的 explorer.exe，只提醒不覆盖");
        ::MessageBoxW(owner,
                      L"C:\\Windows\\System32 里已经存在一个 explorer.exe，但它不是本程序放的\n"
                      L"（可能是系统文件、别的软件，或者你以前手工放进去的、没有版本标记的旧版本）。\n\n"
                      L"为了安全，这次没有替换它。\n\n"
                      L"如果你确认它是旧版本、想换成现在的：请先手动删除它，再点一次\n"
                      L"「注册为默认文件管理器」。",
                      L"未替换 explorer.exe 代理", MB_ICONWARNING | MB_OK);
        return false;
    }
}

bool RemoveProxyIfOurs(HWND owner)
{
    const ProxyState state = QuerySystemProxyState();

    if (state == ProxyState::Missing)
    {
        Logger::Info(L"代理清理：System32 里没有 explorer.exe，无需删除");
        return true;
    }

    if (state == ProxyState::Foreign)
    {
        // 不是我们的，绝不删
        Logger::Info(L"代理清理：System32 里的 explorer.exe 不是本程序放的，不删除");
        return true;
    }

    Logger::Info(L"代理清理：删除 System32 里本程序放的 explorer.exe 代理");
    return RunElevated(kArgRemoveProxy, owner);
}

int RunProxyCommandLine(bool remove)
{
    const std::wstring target = SystemExplorerPath();

    if (remove)
    {
        if (!FileExists(target))
        {
            ::MessageBoxW(nullptr, L"System32 里没有 explorer.exe 代理，不需要删除。",
                          L"文件管理器", MB_ICONINFORMATION | MB_OK);
            return 0;
        }

        if (!IsOursProxy(target))
        {
            ::MessageBoxW(nullptr,
                          L"C:\\Windows\\System32\\explorer.exe 不是本程序放的，已跳过删除。",
                          L"文件管理器", MB_ICONWARNING | MB_OK);
            return 1;
        }

        if (!::DeleteFileW(target.c_str()))
        {
            ::MessageBoxW(nullptr,
                          (L"删除失败（错误码 " + std::to_wstring(::GetLastError()) +
                           L"）。可能被占用，或被安全软件拦截。").c_str(),
                          L"文件管理器", MB_ICONERROR | MB_OK);
            return 2;
        }

        ::MessageBoxW(nullptr,
                      L"已删除 C:\\Windows\\System32\\explorer.exe 代理。\n"
                      L"微信等程序的「打开所在文件夹」会恢复成系统资源管理器。",
                      L"文件管理器", MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    // 先把代理的字节准备好：
    //   正常情况从内嵌资源里取（发布包只有一个 exe，靠的就是这条）；
    //   万一没有资源，退回老做法：找同目录的 explorer-proxy.exe。
    std::vector<BYTE> embedded;
    std::wstring      sourcePath;
    std::wstring      tempPath;
    bool              fromTemp = false;

    if (LoadEmbeddedProxy(embedded))
    {
        if (!WriteProxyToTempFile(embedded, tempPath))
        {
            ::MessageBoxW(nullptr,
                          L"释放内嵌的代理失败（临时目录不可写？）。",
                          L"文件管理器", MB_ICONERROR | MB_OK);
            return 2;
        }
        sourcePath = tempPath;
        fromTemp   = true;
    }
    else
    {
        sourcePath = FindLocalProxyPath();
    }

    if (sourcePath.empty())
    {
        ::MessageBoxW(nullptr,
                      L"程序里没有内嵌代理资源，同目录也找不到 explorer-proxy.exe\n"
                      L"（安装文件可能不完整）。\n"
                      L"请重新下载完整压缩包。",
                      L"文件管理器", MB_ICONERROR | MB_OK);
        return 2;
    }

    if (FileExists(target) && !IsOursProxy(target))
    {
        if (fromTemp) { ::DeleteFileW(tempPath.c_str()); }
        ::MessageBoxW(nullptr,
                      L"C:\\Windows\\System32 里已有一个不是本程序放的 explorer.exe，"
                      L"为了安全没有替换。",
                      L"文件管理器", MB_ICONWARNING | MB_OK);
        return 1;
    }

    const bool copied = ::CopyFileW(sourcePath.c_str(), target.c_str(), FALSE) != FALSE;
    const DWORD error = copied ? 0 : ::GetLastError();

    // 临时文件用完就删，不在磁盘上留东西
    if (fromTemp)
    {
        ::DeleteFileW(tempPath.c_str());
    }

    if (!copied)
    {
        ::MessageBoxW(nullptr,
                      (L"复制失败（错误码 " + std::to_wstring(error) + L"）。\n"
                       L"常见原因：文件正被占用（重启后再试），或被安全软件拦截（请加白名单）。").c_str(),
                      L"文件管理器", MB_ICONERROR | MB_OK);
        return 2;
    }

    Logger::Info(L"代理已部署到 System32：" + target +
                 (fromTemp ? L"（来源：主程序内嵌资源）" : L"（来源：" + sourcePath + L"）"));
    ::MessageBoxW(nullptr,
                  L"已部署 explorer.exe 代理到 C:\\Windows\\System32。\n\n"
                  L"之后微信、批量打印软件等点「打开所在文件夹」就会用本程序打开。",
                  L"文件管理器", MB_ICONINFORMATION | MB_OK);
    return 0;
}

} // namespace shell
