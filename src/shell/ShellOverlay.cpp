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

#pragma comment(lib, "shlwapi.lib")

namespace shell
{

namespace
{

// 注册表下角标处理器的父键
constexpr const wchar_t* kOverlayRegPath =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";

// 系统只取前 15 个处理器（与原生资源管理器一致）
constexpr int kMaxOverlayHandlers = 15;

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

// 读 CLSID 对应的 InprocServer32 DLL 路径（归并同族用）；取不到返回空串
std::wstring ReadClsidDllPath(const CLSID& clsid)
{
    LPOLESTR clsidStr = nullptr;
    if (::StringFromCLSID(clsid, &clsidStr) != S_OK || clsidStr == nullptr)
    {
        return std::wstring();
    }

    std::wstring subKey = L"CLSID\\";
    subKey += clsidStr;
    subKey += L"\\InprocServer32";
    ::CoTaskMemFree(clsidStr);

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

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

OverlayManager::~OverlayManager()
{
    Shutdown();
}

OverlayManager& OverlayManager::Instance()
{
    static OverlayManager instance;
    return instance;
}

void OverlayManager::Shutdown()
{
    for (auto& [index, icon] : m_iconSmall)
    {
        if (icon != nullptr)
        {
            ::DestroyIcon(icon);
        }
    }
    for (auto& [index, icon] : m_iconLarge)
    {
        if (icon != nullptr)
        {
            ::DestroyIcon(icon);
        }
    }
    m_iconSmall.clear();
    m_iconLarge.clear();

    m_handlers.clear();
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
    struct RawEntry
    {
        std::wstring regName;
        CLSID        clsid = {};
    };

    std::vector<RawEntry> raw;

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

            // 默认值 = 处理器 CLSID 字符串
            wchar_t clsidStr[128] = {};
            DWORD   size = sizeof(clsidStr);
            DWORD   type = 0;
            if (::RegQueryValueExW(key, nullptr, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(clsidStr), &size) != ERROR_SUCCESS ||
                type != REG_SZ)
            {
                continue;
            }

            CLSID clsid = {};
            if (::CLSIDFromString(clsidStr, &clsid) != S_OK)
            {
                continue;
            }

            raw.push_back(RawEntry{ name, clsid });
        }

        ::RegCloseKey(key);
    }

    // 按注册名字典序排序：前导空格越多 ASCII 越小、排越前（原生调顺序惯例）
    std::sort(raw.begin(), raw.end(),
              [](const RawEntry& a, const RawEntry& b) { return a.regName < b.regName; });

    // 系统只取前 15 个
    if (raw.size() > kMaxOverlayHandlers)
    {
        raw.resize(kMaxOverlayHandlers);
    }

    for (const RawEntry& entry : raw)
    {
        std::wstring dll = ReadClsidDllPath(entry.clsid);
        const std::wstring setId = ToLower(dll);

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
        info.regName   = entry.regName;
        info.setId     = setId;
        info.handler   = handler;

        // 套显示名：优先用 DLL 文件名去扩展名；取不到退回注册名
        if (!dll.empty())
        {
            info.displayName = StripExtension(BaseName(dll));
        }
        if (info.displayName.empty())
        {
            info.displayName = entry.regName;
        }

        // 取 overlay 图标信息（图标文件 + 资源索引）
        wchar_t iconFile[MAX_PATH] = {};
        int     iconIndex = 0;
        DWORD   flags = 0;
        hr = handler->GetOverlayInfo(iconFile, ARRAYSIZE(iconFile), &iconIndex, &flags);
        if (SUCCEEDED(hr))
        {
            info.iconFile  = iconFile;
            info.iconIndex = iconIndex;
            info.flags     = flags;
        }

        m_handlers.push_back(std::move(info));
    }

    // 与配置对齐：按配置里的顺序 / 范围套到同 id 的处理器上；未配置的追加末尾、默认全部路径
    const auto& sets = core::Settings::Instance().OverlaySets();

    // 先按配置出现顺序排好已配置的，未配置的保持原顺序追加
    std::vector<OverlayHandler> ordered;
    for (const core::OverlaySet& set : sets)
    {
        const std::wstring key = ToLower(set.id);
        for (auto& handler : m_handlers)
        {
            if (handler.setId == key)
            {
                handler.allPaths = set.allPaths;
                handler.paths    = set.paths;
                ordered.push_back(std::move(handler));
            }
        }
    }
    for (auto& handler : m_handlers)
    {
        if (!handler.handler)
        {
            continue; // 已挪进 ordered
        }
        ordered.push_back(std::move(handler));
    }
    m_handlers.swap(ordered);

    Logger::Info(L"角标处理器枚举完成，共 " + std::to_wstring(m_handlers.size()) + L" 个");
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

    const PCIDLIST_ABSOLUTE pidl = entry.pidl.empty()
        ? nullptr
        : reinterpret_cast<PCIDLIST_ABSOLUTE>(entry.pidl.data());

    for (size_t i = 0; i < m_handlers.size(); ++i)
    {
        const OverlayHandler& set = m_handlers[i];
        if (set.handler == nullptr)
        {
            continue;
        }

        // 本地路径范围过滤：范围外直接跳过，不调用处理器
        if (!PathInScope(set, entry.fullPath))
        {
            continue;
        }

        HRESULT hr = set.handler->IsMemberOf(entry.fullPath.c_str(), pidl);

        // S_OK = 命中；其它成功码（S_FALSE）或失败都按未命中处理
        if (hr == S_OK)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void OverlayManager::Annotate(std::vector<ShellEntry>& entries)
{
    if (!core::Settings::Instance().OverlayEnabled())
    {
        return;
    }

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
        SHFILEINFOW shfi = {};
        const DWORD flags = SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON);
        if (::SHGetFileInfoW(set.iconFile.c_str(), 0, &shfi, sizeof(shfi), flags) != 0)
        {
            icon = shfi.hIcon;
        }
    }

    cache[handlerIndex] = icon;
    return icon;
}

} // namespace shell
