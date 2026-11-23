// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellOverlay.cpp
//
// 同步状态角标（Overlay）管理器实现。设计见 docs/图标覆盖角标设计.md。
// ============================================================================

#include "ShellOverlay.h"

#include "core/Settings.h"
#include "core/Logger.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

namespace shell
{

namespace
{

// 注册表下角标处理器的父键
constexpr const wchar_t* kOverlayRegPath =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";

// 系统只取前 15 个处理器（与原生资源管理器一致）
constexpr int kMaxOverlayHandlers = 15;

// 同一套处理器连续失败 / 崩溃多少次后熔断（本次会话不再询问）。
// 取 3 的原因：一次目录枚举会按文件逐个问同一套处理器，真坏的处理器几次就暴露；
// 偶发的单次失败由 NoteHandlerSuccess 清零，不会累积成误熔断。
constexpr int kHandlerFailureLimit = 3;

// ---------------------------------------------------------------------------
// 异常保护调用壳：IsMemberOf
//
// 为什么要单独成一个函数：
//   MSVC 不允许在「需要对象展开」（函数里有局部 C++ 对象需要析构）的函数里
//   直接用 __try/__except，会报 C2712。这个壳只转发裸接口指针、路径和 DWORD，
//   不涉及任何需要析构的 C++ 对象，所以可以安全地使用结构化异常。
//   noinline 是必须的：一旦被内联进调用者，C2712 的限制就会转移到调用者身上。
//
// 返回值：处理器自身返回的 HRESULT；其中发生崩溃时返回 E_FAIL，
//         并把 *crashed 置 true，由调用方按「这套坏了」处理。
// 栈溢出（EXCEPTION_STACK_OVERFLOW）不能就地恢复，继续向外抛，不做兜底。
// ---------------------------------------------------------------------------
__declspec(noinline)
HRESULT IsMemberOfGuarded(IOverlayIdentifier* handler, LPCWSTR path,
                          DWORD attrib, bool* crashed)
{
    *crashed = false;
    __try
    {
        return handler->IsMemberOf(path, attrib);
    }
    __except (::GetExceptionCode() == EXCEPTION_STACK_OVERFLOW
                  ? EXCEPTION_CONTINUE_SEARCH
                  : EXCEPTION_EXECUTE_HANDLER)
    {
        *crashed = true;
    }
    return E_FAIL;
}

// ---------------------------------------------------------------------------
// 异常保护调用壳：GetOverlayInfo（理由同上，单独成函数避开 C2712）
// ---------------------------------------------------------------------------
__declspec(noinline)
HRESULT GetOverlayInfoGuarded(IOverlayIdentifier* handler, LPWSTR iconFile,
                              int cchMax, int* index, DWORD* flags, bool* crashed)
{
    *crashed = false;
    __try
    {
        return handler->GetOverlayInfo(iconFile, cchMax, index, flags);
    }
    __except (::GetExceptionCode() == EXCEPTION_STACK_OVERFLOW
                  ? EXCEPTION_CONTINUE_SEARCH
                  : EXCEPTION_EXECUTE_HANDLER)
    {
        *crashed = true;
    }
    return E_FAIL;
}

// 小写化宽字符串（用于路径 / 归并键比较，忽略大小写）
std::wstring ToLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
    return text;
}

// 取路径的文件名部分（去目录）
std::wstring BaseName(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

// 去掉文件名扩展名
std::wstring StripExtension(std::wstring name)
{
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
    {
        name.resize(dot);
    }
    return name;
}

// ---------------------------------------------------------------------------
// 路径是否落在目录前缀范围内（含子目录）。忽略大小写。
//   dir 为空返回 false；path 等于 dir，或 path 在 dir 之下（dir\...）即命中。
// ---------------------------------------------------------------------------
bool IsPathUnderDirectory(const std::wstring& dir, const std::wstring& path)
{
    if (dir.empty() || path.empty())
    {
        return false;
    }

    std::wstring normDir = dir;
    while (!normDir.empty() && normDir.back() == L'\\')
    {
        normDir.pop_back();
    }

    if (_wcsicmp(normDir.c_str(), path.c_str()) == 0)
    {
        return true;
    }

    if (path.size() > normDir.size() &&
        path[normDir.size()] == L'\\' &&
        _wcsnicmp(normDir.c_str(), path.c_str(), normDir.size()) == 0)
    {
        return true;
    }

    return false;
}

// CLSID 转大写字符串（带花括号）；失败返回空串
std::wstring ClsidToString(const CLSID& clsid)
{
    LPOLESTR clsidStr = nullptr;
    if (::StringFromCLSID(clsid, &clsidStr) != S_OK || clsidStr == nullptr)
    {
        return std::wstring();
    }
    std::wstring result = clsidStr;
    ::CoTaskMemFree(clsidStr);
    return result;
}

// 读 CLSID 对应的 InprocServer32 DLL 路径（归并同族用）；取不到返回空串
std::wstring ReadClsidDllPath(const CLSID& clsid)
{
    const std::wstring clsidStr = ClsidToString(clsid);
    if (clsidStr.empty())
    {
        return std::wstring();
    }

    std::wstring subKey = L"CLSID\\";
    subKey += clsidStr;
    subKey += L"\\InprocServer32";

    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    wchar_t buffer[2048] = {};
    DWORD   size = sizeof(buffer) - sizeof(wchar_t);
    DWORD   type = 0;
    std::wstring dll;

    if (::RegQueryValueExW(key, nullptr, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
    {
        dll = buffer;
    }

    ::RegCloseKey(key);
    return dll;
}

// 读 CLSID 键的默认值（处理器友好描述，如 "TortoiseSVN"）；取不到返回空串
std::wstring ReadClsidDescription(const CLSID& clsid)
{
    const std::wstring clsidStr = ClsidToString(clsid);
    if (clsidStr.empty())
    {
        return std::wstring();
    }

    // 注意：HKCR 下的完整路径是 CLSID\{...}，不能只传花括号串，否则开键返回 2（找不到）
    const std::wstring subKey = L"CLSID\\" + clsidStr;

    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    wchar_t buffer[512] = {};
    DWORD   size = sizeof(buffer) - sizeof(wchar_t);
    DWORD   type = 0;
    std::wstring desc;

    if (::RegQueryValueExW(key, nullptr, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        type == REG_SZ)
    {
        desc = buffer;
    }

    ::RegCloseKey(key);
    return desc;
}

// 清理描述里冗余的尾巴（"Class" / "Handler Class" 等），让界面名更短
std::wstring CleanDescription(std::wstring desc)
{
    while (true)
    {
        const std::wstring before = desc;
        // 去首尾空白
        size_t start = desc.find_first_not_of(L" \t");
        size_t end   = desc.find_last_not_of(L" \t");
        if (start != std::wstring::npos)
        {
            desc = desc.substr(start, end - start + 1);
        }
        // 去掉结尾的 " Class" / " Handler Class" / " Class Handler"
        for (const wchar_t* tail : { L" Handler Class", L" Class Handler", L" Class", L" Handler" })
        {
            const size_t tlen = ::wcslen(tail);
            if (desc.size() > tlen && _wcsicmp(desc.c_str() + desc.size() - tlen, tail) == 0)
            {
                desc.resize(desc.size() - tlen);
                break;
            }
        }
        if (desc == before)
        {
            break;
        }
    }
    return desc;
}

// 读 DLL 版本资源里的指定字段（ProductName / FileDescription）；取不到返回空串
std::wstring ReadDllVersionString(const std::wstring& dllPath, const wchar_t* field)
{
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(dllPath.c_str(), &ignored);
    if (size == 0)
    {
        return std::wstring();
    }

    std::vector<BYTE> buffer(size);
    if (!::GetFileVersionInfoW(dllPath.c_str(), 0, size, buffer.data()))
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
                 translation[0].language, translation[0].codePage, field);

    wchar_t* value = nullptr;
    UINT valueLength = 0;
    if (!::VerQueryValueW(buffer.data(), subBlock, reinterpret_cast<void**>(&value), &valueLength) ||
        value == nullptr)
    {
        return std::wstring();
    }
    return std::wstring(value);
}

// 名字是否含内部实现词（Handler/Class/Overlay/Shim/Shell），含则视为不够友好、需降级
bool LooksInternal(const std::wstring& s)
{
    static const wchar_t* words[] = { L"handler", L"class", L"overlay", L"shim", L"shell" };
    for (const wchar_t* w : words)
    {
        const size_t wlen = ::wcslen(w);
        if (s.size() >= wlen &&
            std::search(s.begin(), s.end(), w, w + wlen,
                         [](wchar_t a, wchar_t b) { return ::towlower(a) == ::towlower(b); }) != s.end())
        {
            return true;
        }
    }
    return false;
}

// 是否是 Windows 操作系统这类没有信息量的产品名，需要排除
bool IsOsProductName(const std::wstring& s)
{
    return s.find(L"Operating System") != std::wstring::npos ||
           s.find(L"操作系统") != std::wstring::npos;
}

// 综合多个来源拼出界面显示名；优先级：干净的 CLSID 描述 → DLL 产品名 → 文件描述 →
// 含内部词的 CLSID 描述 → DLL 文件名 → 注册表子键名
std::wstring BuildDisplayName(const std::wstring& regName, const CLSID& clsid, const std::wstring& dll)
{
    const std::wstring clsidDesc = CleanDescription(ReadClsidDescription(clsid));

    std::wstring product;
    std::wstring fileDesc;
    if (!dll.empty())
    {
        product  = CleanDescription(ReadDllVersionString(dll, L"ProductName"));
        fileDesc = CleanDescription(ReadDllVersionString(dll, L"FileDescription"));
    }
    const std::wstring dllName = dll.empty() ? std::wstring() : StripExtension(BaseName(dll));

    // 1) CLSID 描述本身干净（如 TortoiseSVN）
    if (!clsidDesc.empty() && !LooksInternal(clsidDesc))
    {
        return clsidDesc;
    }
    // 2) DLL 产品名（排除操作系统名），如 Microsoft OneDrive / 豆包操作电脑
    if (!product.empty() && !IsOsProductName(product))
    {
        return product;
    }
    // 3) DLL 文件描述
    if (!fileDesc.empty())
    {
        return fileDesc;
    }
    // 4) 含内部词的 CLSID 描述也兜底
    if (!clsidDesc.empty())
    {
        return clsidDesc;
    }
    // 5) DLL 文件名（去扩展名）
    if (!dllName.empty())
    {
        return dllName;
    }
    // 6) 注册表子键名
    return regName;
}

// 注册表扫描出的原始处理器项（尚未实例化 COM）
struct RawHandler
{
    std::wstring regName;      // 注册表子键名
    CLSID        clsid = {};
    std::wstring dll;          // InprocServer32 DLL 路径（归并同族用）
    std::wstring setId;       // 归并套标识（DLL 路径小写）
    std::wstring displayName; // 套显示名
};

// ---------------------------------------------------------------------------
// 处理器用途黑名单（二期）
//
// 有些软件注册角标处理器并不是为了「同步 / 版本状态」。最典型的是豆包「操作电脑」：
// 它注册了 aPipIconOverlay（pip_remote.dll），实际是远程协助 / 影子会话的外壳扩展，
// 判定结果跟文件同步状态毫无关系，出现在配置列表里只会让人困惑「这为什么有同步角标」。
// 同类还有系统的便携设备加密（EnhancedStorageShell / EhStorShell.dll）与
// 脱机文件（Offline Files / cscui.dll）。
//
// 这里按 CLSID 与 DLL 文件名排除：既不进配置列表，也不参与判定。
//   · 只比 DLL 文件名、不写死绝对路径——豆包装在用户目录下，会随版本换目录；
//   · 不做白名单（只放行已知同步类）：靠猜类别容易误杀以后新装的合法工具。
// ---------------------------------------------------------------------------

// 按 CLSID 排除的处理器（大写带花括号，与 ClsidToString 输出格式一致，比较时忽略大小写）
const wchar_t* const kBlockedClsids[] = {
    L"{61135E38-1A85-4B66-A170-7E764DE2842A}",  // 豆包操作电脑（aPipIconOverlay）
};

// 按 DLL 文件名排除的处理器（一律小写，与 ToLower 后的文件名比较）
const wchar_t* const kBlockedDllNames[] = {
    L"pip_remote.dll",    // 豆包操作电脑：远程协助 / 影子会话外壳扩展
    L"ehstorshell.dll",   // 便携设备加密（EnhancedStorageShell）
    L"cscui.dll",         // 脱机文件（Offline Files）
};

// 该处理器是否命中用途黑名单（命中即跳过：不列配置、不参与判定）
bool IsBlockedHandler(const CLSID& clsid, const std::wstring& dllPath)
{
    // 先按 CLSID 判：即使 DLL 路径读不到也能拦住
    const std::wstring clsidStr = ClsidToString(clsid);
    for (const wchar_t* blocked : kBlockedClsids)
    {
        if (_wcsicmp(clsidStr.c_str(), blocked) == 0)
        {
            return true;
        }
    }

    // 再按 DLL 文件名判（去目录、转小写，不依赖安装路径）
    const std::wstring fileName = ToLower(BaseName(dllPath));
    for (const wchar_t* blocked : kBlockedDllNames)
    {
        if (fileName == blocked)
        {
            return true;
        }
    }
    return false;
}

// 读 HKLM/HKCU 注册表，按名排序、前 15 上限，查 DLL 归并键；纯注册表操作，不碰 COM
std::vector<RawHandler> ScanRegistry()
{
    struct RegEntry
    {
        std::wstring regName;
        CLSID        clsid = {};
    };

    std::vector<RegEntry> regEntries;

    const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (HKEY root : roots)
    {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, kOverlayRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            continue;
        }

        for (DWORD index = 0;; ++index)
        {
            wchar_t name[512] = {};
            DWORD   nameLength = ARRAYSIZE(name);
            if (::RegEnumKeyExW(key, index, name, &nameLength, nullptr, nullptr,
                                nullptr, nullptr) != ERROR_SUCCESS)
            {
                break;
            }

            // 打开子键，读它的默认值（处理器 CLSID 字符串）；父键本身默认值为空
            HKEY subKey = nullptr;
            if (::RegOpenKeyExW(key, name, 0, KEY_READ, &subKey) != ERROR_SUCCESS)
            {
                continue;
            }

            wchar_t clsidStr[128] = {};
            DWORD   size = sizeof(clsidStr);
            DWORD   type = 0;
            const bool hasClsid = ::RegQueryValueExW(subKey, nullptr, nullptr, &type,
                                       reinterpret_cast<LPBYTE>(clsidStr), &size) == ERROR_SUCCESS &&
                                  (type == REG_SZ);
            ::RegCloseKey(subKey);

            if (!hasClsid)
            {
                continue;
            }

            CLSID clsid = {};
            if (::CLSIDFromString(clsidStr, &clsid) != S_OK)
            {
                continue;
            }

            regEntries.push_back(RegEntry{ name, clsid });
        }

        ::RegCloseKey(key);
    }

    // 按注册名字典序排序：前导空格越多 ASCII 越小、排越前（原生调顺序惯例）
    std::sort(regEntries.begin(), regEntries.end(),
              [](const RegEntry& a, const RegEntry& b) { return a.regName < b.regName; });

    // 系统只取前 15 个
    if (regEntries.size() > kMaxOverlayHandlers)
    {
        regEntries.resize(kMaxOverlayHandlers);
    }

    std::vector<RawHandler> raw;
    for (const RegEntry& entry : regEntries)
    {
        RawHandler rawItem;
        rawItem.regName = entry.regName;
        rawItem.clsid   = entry.clsid;
        rawItem.dll     = ReadClsidDllPath(entry.clsid);

        // 套标识优先用 DLL 路径小写；DLL 读不到时用 CLSID 字符串兜底，保证唯一、不产生空 id
        const std::wstring clsidStr = ClsidToString(entry.clsid);
        rawItem.setId = !rawItem.dll.empty() ? ToLower(rawItem.dll) : ToLower(clsidStr);

        // 显示名综合 CLSID 描述 / DLL 版本资源 / 文件名多来源择优
        rawItem.displayName = BuildDisplayName(entry.regName, entry.clsid, rawItem.dll);

        // 用途黑名单：非同步 / 非版本控制类直接跳过，不进配置列表也不参与判定。
        // 注意：这一步排在「前 15 槽位截断」之后是有意的——保持与原生资源管理器
        // 一致（系统也只认前 15 个槽位），不让第 16 位以后的处理器因为我们过滤了
        // 黑名单项而"补位"显示出来。
        if (IsBlockedHandler(entry.clsid, rawItem.dll))
        {
            Logger::Info(L"角标处理器按用途黑名单跳过：" + rawItem.regName + L"（" +
                         rawItem.displayName + L"）");
            continue;
        }

        raw.push_back(std::move(rawItem));
    }

    return raw;
}

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

OverlayManager::~OverlayManager()
{
    // 先让常驻线程把处理器释放干净（COM 对象只能在其创建线程上释放），
    // 再把这根线程收掉
    Shutdown();
    StopWorkerThread();
}

OverlayManager& OverlayManager::Instance()
{
    static OverlayManager instance;
    return instance;
}

// ============================================================================
// 常驻专用 STA 线程（二期 2A 跨线程加固）
//
// 为什么要有它：处理器是 STA 对象，只在创建它的线程上调用才安全。一期是
// 「第一次枚举时在那根临时枚举线程里创建，下次枚举换线程却复用旧对象」，
// 跨线程调用会返回 RPC_E_WRONG_THREAD，被当成未命中，表现为角标偶尔不显示。
// 现在所有 COM 操作（实例化 / GetOverlayInfo / IsMemberOf / 释放）都固定在
// 这一根线程上，调用方只负责投递任务和等结果。
// ============================================================================

bool OverlayManager::StartWorkerThread()
{
    if (m_workerThread.joinable())
    {
        return true; // 已经起来了
    }

    // 三个同步事件：任务到达（自动重置）、请求退出（手动重置）、任务完成（手动重置）
    if (m_taskEvent == nullptr)
    {
        m_taskEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (m_stopEvent == nullptr)
    {
        m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (m_doneEvent == nullptr)
    {
        m_doneEvent = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    }
    if (m_taskEvent == nullptr || m_stopEvent == nullptr || m_doneEvent == nullptr)
    {
        Logger::Warn(L"角标常驻线程：创建同步事件失败，本次退回调用方线程直接处理");
        return false;
    }

    ::ResetEvent(m_stopEvent);

    try
    {
        m_workerThread = std::thread([this]() { WorkerLoop(); });
    }
    catch (...)
    {
        Logger::Warn(L"角标常驻线程：线程创建失败，本次退回调用方线程直接处理");
        return false;
    }
    return true;
}

void OverlayManager::StopWorkerThread()
{
    if (!m_workerThread.joinable())
    {
        return;
    }

    // 置退出事件；常驻线程从等待中醒来后走完循环、CoUninitialize，再 join 收尾
    ::SetEvent(m_stopEvent);
    m_workerThread.join();

    if (m_taskEvent != nullptr)
    {
        ::CloseHandle(m_taskEvent);
        m_taskEvent = nullptr;
    }
    if (m_stopEvent != nullptr)
    {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    if (m_doneEvent != nullptr)
    {
        ::CloseHandle(m_doneEvent);
        m_doneEvent = nullptr;
    }
}

void OverlayManager::WorkerLoop()
{
    m_workerThreadId = ::GetCurrentThreadId();

    // 这根线程就是处理器的「家」：必须自己初始化成 STA
    const HRESULT hrInit =
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comReady = SUCCEEDED(hrInit);
    if (!comReady)
    {
        Logger::Warn(L"角标常驻线程 CoInitializeEx 失败：" + Logger::FormatHresult(hrInit));
    }

    HANDLE waitHandles[2] = { m_stopEvent, m_taskEvent };

    bool running = true;
    while (running)
    {
        // 带消息泵的等待：既等任务 / 退出事件，也把消息泵掉。
        // 这一步不能省——处理器内部若需要 COM 跨 apartment 调度或窗口消息，
        // 没有消息泵会被我们自己卡死。
        const DWORD waitResult = ::MsgWaitForMultipleObjectsEx(
            2, waitHandles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        // 不论被什么唤醒，先把已排队的消息处理干净
        MSG msg = {};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (waitResult == WAIT_OBJECT_0)
        {
            running = false; // 退出事件
            break;
        }

        if (waitResult == WAIT_OBJECT_0 + 1)
        {
            TaskKind kind = TaskKind::Annotate;
            std::vector<ShellEntry>* entries = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_taskMutex);
                kind    = m_taskKind;
                entries = m_taskEntries;
            }

            if (kind == TaskKind::Shutdown)
            {
                RunShutdownOnWorker();
            }
            else if (entries != nullptr)
            {
                RunAnnotateOnWorker(*entries);
            }

            // 手动重置事件：保持置位，等下一次投递前由投递方 ResetEvent
            ::SetEvent(m_doneEvent);
        }
        // 其余返回值（有消息）已经在上面泵过，回到循环继续等
    }

    if (comReady)
    {
        ::CoUninitialize();
    }
    m_workerThreadId = 0;
}

void OverlayManager::PostTaskAndWait(TaskKind kind, std::vector<ShellEntry>* entries)
{
    // 同一时刻只允许一个任务在飞：枚举线程的 Annotate 与 UI 线程的 Shutdown
    // 可能并发，撞车会互相覆盖任务字段
    std::lock_guard<std::mutex> postLock(m_postMutex);

    if (!StartWorkerThread())
    {
        // 常驻线程起不来（事件 / 线程创建失败，极罕见）：退回在调用方线程直接做，
        // 行为退化成加固前，但功能不丢
        if (kind == TaskKind::Annotate && entries != nullptr)
        {
            RunAnnotateOnWorker(*entries);
        }
        else
        {
            RunShutdownOnWorker();
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_taskKind    = kind;
        m_taskEntries = entries;
    }

    ::ResetEvent(m_doneEvent);
    ::SetEvent(m_taskEvent);

    // 同步等结果。调用方是后台枚举线程或 UI 线程，等的是同一批条目的判定结果；
    // 线程间靠完成事件建立先后关系，等待期间调用方不会碰 entries。
    ::WaitForSingleObject(m_doneEvent, INFINITE);

    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_taskEntries = nullptr;
    }
}

void OverlayManager::Shutdown()
{
    // 从未启用过角标、也没起过常驻线程：没有任何东西要释放，直接返回，
    // 避免为了一个空 Shutdown 白起一根线程
    bool hasAnything = m_workerThread.joinable();
    if (!hasAnything)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        hasAnything = !m_handlers.empty() || !m_iconSmall.empty() ||
                      !m_iconLarge.empty() || !m_deadIcons.empty();
    }
    if (!hasAnything)
    {
        return;
    }

    // 处理器 COM 对象必须在其创建线程（常驻 STA）上释放，所以交给常驻线程做
    PostTaskAndWait(TaskKind::Shutdown, nullptr);

    // 图标 HICON 与 COM 无关，统一放在调用方（UI）线程销毁：
    // 常驻线程若直接销毁，UI 可能正在用这个 HICON 绘制，会画到已释放的对象
    std::vector<HICON> icons;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        icons.swap(m_deadIcons);
    }
    for (HICON icon : icons)
    {
        if (icon != nullptr)
        {
            ::DestroyIcon(icon);
        }
    }
}

void OverlayManager::RunShutdownOnWorker()
{
    // 只允许在常驻 STA 线程执行
    std::lock_guard<std::mutex> lock(m_stateMutex);

    for (auto& [index, icon] : m_iconSmall)
    {
        if (icon != nullptr)
        {
            m_deadIcons.push_back(icon); // 搬出去，交给调用方线程 DestroyIcon
        }
    }
    for (auto& [index, icon] : m_iconLarge)
    {
        if (icon != nullptr)
        {
            m_deadIcons.push_back(icon);
        }
    }
    m_iconSmall.clear();
    m_iconLarge.clear();

    m_handlers.clear();     // 到这里，所有处理器都在创建它们的线程上释放
    m_resultCache.clear();
    m_loaded = false;
}

void OverlayManager::ClearResultCache()
{
    m_resultCache.clear();
}

// ============================================================================
// 枚举与归并
// ============================================================================

void OverlayManager::EnsureLoaded()
{
    if (m_loaded)
    {
        return;
    }
    m_loaded = true; // 无论成败都标记已加载，避免每次枚举目录都重试注册表

    // 总开关关闭：整套不初始化，行为等价未做本功能
    if (!core::Settings::Instance().OverlayEnabled())
    {
        return;
    }

    EnumerateHandlers();
}

void OverlayManager::EnumerateHandlers()
{
    // 只允许在常驻 STA 线程执行（处理器要在这根线程上实例化）。
    // 先在局部容器里建好，最后一次性换进 m_handlers，
    // 这样 UI 线程读 m_handlers 时不会看到半成品或 vector 重分配。
    const std::vector<RawHandler> raw = ScanRegistry();

    std::vector<OverlayHandler> found;
    found.reserve(raw.size());

    for (const RawHandler& entry : raw)
    {
        wil::com_ptr<IOverlayIdentifier> handler;
        HRESULT hr = ::CoCreateInstance(entry.clsid, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_IShellIconOverlayIdentifier,
                                        reinterpret_cast<void**>(handler.put()));
        if (FAILED(hr) || handler == nullptr)
        {
            Logger::Warn(L"角标处理器实例化失败：" + entry.regName + L"，" +
                         Logger::FormatHresult(hr));
            continue;
        }

        OverlayHandler info;
        info.regName     = entry.regName;
        info.setId       = entry.setId;
        info.displayName = entry.displayName;
        info.handler     = handler;

        // 取 overlay 图标信息（图标文件 + 资源索引）
        // 外层包异常保护：读取图标信息属于第三方代码，崩了就跳过这套，
        // 不把主进程带走（这套即使能判定也取不到图标，留着没意义）。
        wchar_t iconFile[MAX_PATH] = {};
        int     iconIndex = 0;
        DWORD   flags = 0;
        bool    iconCrashed = false;
        hr = GetOverlayInfoGuarded(handler.get(), iconFile, ARRAYSIZE(iconFile),
                                   &iconIndex, &flags, &iconCrashed);
        if (iconCrashed)
        {
            Logger::Warn(L"角标处理器读取图标信息时崩溃，已跳过这套：" + entry.regName);
            continue;
        }
        if (SUCCEEDED(hr))
        {
            info.iconFile  = iconFile;
            info.iconIndex = iconIndex;
            info.flags     = flags;
        }

        found.push_back(std::move(info));
    }

    // 与配置对齐：按配置里的顺序 / 范围套到同 id 的处理器上；未配置的追加末尾、默认全部路径
    const auto& sets = core::Settings::Instance().OverlaySets();

    // 先按配置出现顺序排好已配置的，未配置的保持原顺序追加
    std::vector<OverlayHandler> ordered;
    ordered.reserve(found.size());
    for (const core::OverlaySet& set : sets)
    {
        const std::wstring key = ToLower(set.id);
        if (key.empty())
        {
            continue; // 丢弃历史遗留的空 id 配置项
        }
        for (auto& handler : found)
        {
            if (handler.setId == key)
            {
                handler.allPaths = set.allPaths;
                handler.paths    = set.paths;
                ordered.push_back(std::move(handler));
            }
        }
    }
    for (auto& handler : found)
    {
        if (!handler.handler)
        {
            continue; // 已挪进 ordered
        }
        ordered.push_back(std::move(handler));
    }

    size_t handlerCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // swap 出来的旧 handlers 在锁外由 ordered 析构，且析构发生在本线程，
        // 满足「COM 对象在其创建线程释放」的约束
        m_handlers.swap(ordered);
        handlerCount = m_handlers.size();
    }

    Logger::Info(L"角标处理器枚举完成，共 " + std::to_wstring(handlerCount) + L" 个");
}

std::vector<core::OverlaySet> OverlayManager::ListSets()
{
    const std::vector<RawHandler> raw = ScanRegistry();

    // 按 setId 归并去重，保持注册表优先级顺序
    std::vector<core::OverlaySet> sets;
    std::unordered_map<std::wstring, bool> seen;
    for (const RawHandler& entry : raw)
    {
        // 空 id 不进入列表（兜底已用 CLSID，理论上不会空；双保险）
        if (entry.setId.empty() || seen.count(entry.setId))
        {
            continue;
        }
        seen[entry.setId] = true;

        core::OverlaySet set;
        set.id        = entry.setId;
        set.name      = entry.displayName;
        set.allPaths  = true;
        sets.push_back(std::move(set));
    }

    // 套用已存配置的范围（全部 / 白名单），按配置出现顺序排前；注册表新发现的追加末尾。
    // 显示名始终以注册表 / 版本资源实时生成，不沿用历史存下的旧名（旧名多为 DLL 文件名）。
    const auto& saved = core::Settings::Instance().OverlaySets();
    std::vector<core::OverlaySet> ordered;
    std::unordered_map<std::wstring, bool> used;

    for (const core::OverlaySet& savedSet : saved)
    {
        const std::wstring key = ToLower(savedSet.id);
        if (key.empty())
        {
            continue; // 丢弃历史遗留的空 id 配置项
        }
        for (auto& set : sets)
        {
            if (set.id == key)
            {
                set.allPaths = savedSet.allPaths;
                set.paths    = savedSet.paths;
                ordered.push_back(std::move(set));
                used[key] = true;
                break; // 一套只匹配一个配置项
            }
        }
    }
    for (auto& set : sets)
    {
        // 跳过空 id（含已被 move 掏空的壳），避免重复追加出空行
        if (!set.id.empty() && !used.count(set.id))
        {
            ordered.push_back(std::move(set));
        }
    }

    return ordered;
}

// ============================================================================
// 判定
// ============================================================================

bool OverlayManager::PathInScope(const OverlayHandler& set, const std::wstring& path) const
{
    if (set.allPaths)
    {
        return true;
    }
    for (const std::wstring& dir : set.paths)
    {
        if (IsPathUnderDirectory(dir, path))
        {
            return true;
        }
    }
    return false;
}

int OverlayManager::ResolveEntry(const ShellEntry& entry)
{
    // 分组标题 / 占位格不是真实项，不参与判定
    if (entry.isGroupHeader || entry.isSpacer)
    {
        return -1;
    }

    // IsMemberOf 第二参是 DWORD 文件属性（目录 / 隐藏 / 系统等位），不是 PIDL。
    // 按完整路径取真实属性；取不到（虚拟项、权限不足等）传 0 兜底。
    DWORD dwAttrib = 0;
    if (!entry.fullPath.empty())
    {
        const DWORD attrs = ::GetFileAttributesW(entry.fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES)
        {
            dwAttrib = attrs;
        }
    }

    int hit = -1;
    for (size_t i = 0; i < m_handlers.size(); ++i)
    {
        // 需要写回失败计数 / 熔断标记，这里不能是 const 引用
        OverlayHandler& set = m_handlers[i];
        if (set.handler == nullptr || set.disabled)
        {
            continue;
        }

        // 本地路径范围过滤：范围外直接跳过，不调用处理器
        if (!PathInScope(set, entry.fullPath))
        {
            continue;
        }

        // 过保护壳调用：处理器代码里的访问违例等崩溃会被就地拦下，
        // 只把这一套记为失败并按未命中处理，主进程继续跑。
        bool    crashed = false;
        HRESULT hr = IsMemberOfGuarded(set.handler.get(), entry.fullPath.c_str(),
                                      dwAttrib, &crashed);
        if (crashed)
        {
            NoteHandlerFailure(i, L"IsMemberOf 访问违例等异常");
            continue;
        }

        // S_OK = 命中
        if (hr == S_OK)
        {
            NoteHandlerSuccess(i);
            hit = static_cast<int>(i);
            break;
        }

        // S_FALSE = 处理器正常应答「不是我」，不是故障：清零失败计数
        if (hr == S_FALSE)
        {
            NoteHandlerSuccess(i);
            continue;
        }

        // 其它（失败码或非标准成功码）：按未命中处理，同时记一次故障，
        // 连续多次即熔断，避免一套坏处理器被每个文件都问一遍
        NoteHandlerFailure(i, L"IsMemberOf 返回 " + Logger::FormatHresult(hr));
    }

    return hit;
}

// ============================================================================
// 失败记账与熔断（二期 3C）
// ============================================================================

void OverlayManager::NoteHandlerFailure(size_t index, const std::wstring& reason)
{
    if (index >= m_handlers.size())
    {
        return;
    }

    OverlayHandler& set = m_handlers[index];
    ++set.failureCount; // 熔断后该套被跳过、不再进本函数，计数不会无限增长

    if (set.failureCount >= kHandlerFailureLimit)
    {
        // 达到阈值：熔断。本次会话不再向这套发判定请求。
        if (!set.disabled)
        {
            set.disabled = true;
            Logger::Warn(L"角标处理器连续失败 " + std::to_wstring(set.failureCount) +
                         L" 次，已熔断本次会话不再询问：" + set.displayName + L"（" +
                         set.regName + L"），最近一次原因：" + reason);
        }
        return;
    }

    Logger::Warn(L"角标处理器判定失败（第 " + std::to_wstring(set.failureCount) +
                 L" 次）：" + set.displayName + L"（" + set.regName +
                 L"），原因：" + reason);
}

void OverlayManager::NoteHandlerSuccess(size_t index)
{
    if (index < m_handlers.size())
    {
        // 只要有一次正常应答就清零，偶发失败不会累积成误熔断
        m_handlers[index].failureCount = 0;
    }
}

void OverlayManager::Annotate(std::vector<ShellEntry>& entries)
{
    if (!core::Settings::Instance().OverlayEnabled())
    {
        return;
    }

    // 真正的判定与处理器懒加载 / 实例化全部投递到常驻 STA 线程执行，
    // 这里同步等结果。调用方（后台枚举线程）在等待期间不会碰 entries，
    // 两边靠完成事件建立先后关系，不会出现数据竞争。
    PostTaskAndWait(TaskKind::Annotate, &entries);
}

void OverlayManager::RunAnnotateOnWorker(std::vector<ShellEntry>& entries)
{
    // 只允许在常驻 STA 线程执行：处理器 COM 对象全部由这根线程持有
    EnsureLoaded();
    ClearResultCache();

    for (ShellEntry& entry : entries)
    {
        if (entry.isGroupHeader || entry.isSpacer)
        {
            entry.overlayIndex = -1;
            continue;
        }

        // 有真实路径才走缓存键；纯虚拟项每次直接判定
        if (!entry.fullPath.empty())
        {
            const std::wstring key = ToLower(entry.fullPath);
            auto cached = m_resultCache.find(key);
            if (cached != m_resultCache.end())
            {
                entry.overlayIndex = cached->second;
                continue;
            }

            const int hit = ResolveEntry(entry);
            m_resultCache[key] = hit;
            entry.overlayIndex = hit;
        }
        else
        {
            entry.overlayIndex = ResolveEntry(entry);
        }
    }
}

// ============================================================================
// overlay 图标
// ============================================================================

HICON OverlayManager::GetOverlayIcon(size_t handlerIndex, int size)
{
    // 本函数在 UI 线程被调用；m_handlers 与图标缓存会被常驻线程整体替换
    // （懒加载 / Shutdown），必须加锁，否则可能读到悬垂引用或半成品。
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (handlerIndex >= m_handlers.size())
    {
        return nullptr;
    }

    const bool large = size >= 32;
    auto& cache = large ? m_iconLarge : m_iconSmall;

    auto cached = cache.find(handlerIndex);
    if (cached != cache.end())
    {
        return cached->second;
    }

    const OverlayHandler& set = m_handlers[handlerIndex];
    HICON icon = nullptr;

    if (!set.iconFile.empty())
    {
        // GetOverlayInfo 给的 iconIndex 是图标在文件（DLL/EXE/ICO）内的资源索引，
        // 用 ExtractIconEx 按索引取对应图标；取不到再退回文件默认图标兜底。
        HICON hLarge = nullptr;
        HICON hSmall = nullptr;
        UINT  extracted = ::ExtractIconExW(set.iconFile.c_str(), set.iconIndex,
                                          &hLarge, &hSmall, 1);
        if (extracted > 0)
        {
            icon = large ? hLarge : hSmall;
            // 释放没用到的另一尺寸
            if (large && hSmall)
            {
                ::DestroyIcon(hSmall);
            }
            else if (!large && hLarge)
            {
                ::DestroyIcon(hLarge);
            }
        }

        if (icon == nullptr)
        {
            SHFILEINFOW shfi = {};
            const DWORD flags = SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON);
            if (::SHGetFileInfoW(set.iconFile.c_str(), 0, &shfi, sizeof(shfi), flags) != 0)
            {
                icon = shfi.hIcon;
            }
        }
    }

    cache[handlerIndex] = icon;
    return icon;
}

} // namespace shell
