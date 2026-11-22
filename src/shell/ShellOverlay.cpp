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
    const std::vector<RawHandler> raw = ScanRegistry();

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
        if (key.empty())
        {
            continue; // 丢弃历史遗留的空 id 配置项
        }
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

        HRESULT hr = set.handler->IsMemberOf(entry.fullPath.c_str(), dwAttrib);

        // S_OK = 命中；其它成功码（S_FALSE）或失败都按未命中处理
        if (hr == S_OK)
        {
            hit = static_cast<int>(i);
            break;
        }
    }

    return hit;
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
