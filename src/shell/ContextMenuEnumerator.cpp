// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuEnumerator.cpp
//
// 注册表侧右键菜单清单枚举实现。套路沿用 ShellOpenWith / ShellOverlay：
//   · 注册表字符串读取、间接字符串解析、命令行拆分；
//   · CLSID → InprocServer32 DLL 路径 → 版本资源 / 描述拼显示名。
//
// 本文件不实例化任何 DLL、不调 IContextMenu，纯注册表 + 文件版本资源读取。
// ============================================================================

#include "shell/ContextMenuEnumerator.h"

#include "core/Logger.h"

#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")

namespace shell
{

namespace
{

// ---------------------------------------------------------------------------
// 小工具：字符串转小写（Windows 路径不区分大小写，归并键用）
// ---------------------------------------------------------------------------
std::wstring ToLower(std::wstring text)
{
    for (wchar_t& ch : text)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return text;
}

// ---------------------------------------------------------------------------
// 读注册表字符串值；不存在 / 类型不符返回空串
// ---------------------------------------------------------------------------
std::wstring ReadRegValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    wchar_t buffer[2048] = {};
    DWORD   size = sizeof(buffer) - sizeof(wchar_t);
    DWORD   type = 0;
    std::wstring result;

    if (::RegQueryValueExW(key, valueName, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
    {
        result.assign(buffer);
    }

    ::RegCloseKey(key);

    if (!result.empty() && type == REG_EXPAND_SZ)
    {
        wchar_t expanded[2048] = {};
        if (::ExpandEnvironmentStringsW(result.c_str(), expanded, ARRAYSIZE(expanded)) > 0)
        {
            result = expanded;
        }
    }
    return result;
}

// 读一个键下所有子键名
std::vector<std::wstring> ReadSubKeyNames(HKEY root, const std::wstring& subKey)
{
    std::vector<std::wstring> names;

    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return names;
    }

    wchar_t name[512] = {};
    for (DWORD index = 0;; ++index)
    {
        DWORD nameLength = ARRAYSIZE(name);
        if (::RegEnumKeyExW(key, index, name, &nameLength, nullptr, nullptr,
                            nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        names.push_back(name);
    }

    ::RegCloseKey(key);
    return names;
}

// 解析 `@{dll,-resid}` / ms-resource 间接字符串；解析不出来原样返回
std::wstring ResolveIndirectString(const std::wstring& raw)
{
    if (raw.empty() || raw[0] != L'@')
    {
        return raw;
    }

    wchar_t buffer[512] = {};
    if (SUCCEEDED(::SHLoadIndirectString(raw.c_str(), buffer, ARRAYSIZE(buffer), nullptr)))
    {
        std::wstring result(buffer);
        if (!result.empty())
        {
            return result;
        }
    }
    return raw;
}

// ---------------------------------------------------------------------------
// CLSID 相关（照搬 ShellOverlay 套路）
// ---------------------------------------------------------------------------
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

std::wstring ReadClsidDllPath(const CLSID& clsid)
{
    const std::wstring clsidStr = ClsidToString(clsid);
    if (clsidStr.empty())
    {
        return std::wstring();
    }

    const std::wstring subKey = L"CLSID\\" + clsidStr + L"\\InprocServer32";
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

    if (!dll.empty() && dll.find(L'%') != std::wstring::npos)
    {
        wchar_t expanded[2048] = {};
        if (::ExpandEnvironmentStringsW(dll.c_str(), expanded, ARRAYSIZE(expanded)) > 0)
        {
            dll = expanded;
        }
    }
    return dll;
}

std::wstring ReadClsidDescription(const CLSID& clsid)
{
    const std::wstring clsidStr = ClsidToString(clsid);
    if (clsidStr.empty())
    {
        return std::wstring();
    }

    const std::wstring subKey = L"CLSID\\" + clsidStr;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    wchar_t buffer[512] = {};
    DWORD   size = sizeof(buffer);
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

// 读 DLL 版本资源里的指定字段（ProductName / FileDescription）
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
    if (!::VerQueryValueW(buffer.data(), subBlock, reinterpret_cast<void**>(&value),
                          &valueLength) || value == nullptr)
    {
        return std::wstring();
    }
    return std::wstring(value);
}

// 去掉显示名首尾空白和尾部 " Class / Handler" 这类冗余词
std::wstring CleanDescription(std::wstring desc)
{
    size_t start = desc.find_first_not_of(L" \t");
    size_t end   = desc.find_last_not_of(L" \t");
    if (start != std::wstring::npos)
    {
        desc = desc.substr(start, end - start + 1);
    }

    for (const wchar_t* tail : { L" Handler Class", L" Class Handler", L" Class", L" Handler" })
    {
        const size_t tlen = ::wcslen(tail);
        if (desc.size() > tlen && _wcsicmp(desc.c_str() + desc.size() - tlen, tail) == 0)
        {
            desc.resize(desc.size() - tlen);
            break;
        }
    }
    return desc;
}

// 名字含 Handler / Class / Overlay 这类内部词，视为不够友好
bool LooksInternal(const std::wstring& s)
{
    static const wchar_t* words[] = { L"handler", L"class", L"overlay", L"shim" };
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

// 文件名去目录去扩展名
std::wstring StripExtension(const std::wstring& path)
{
    const wchar_t* fileName = ::PathFindFileNameW(path.c_str());
    std::wstring name = fileName != nullptr ? std::wstring(fileName) : path;
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
    {
        name.resize(dot);
    }
    return name;
}

// 综合多来源拼处理器显示名
std::wstring BuildHandlerDisplayName(const std::wstring& regName,
                                     const CLSID& clsid,
                                     const std::wstring& dll)
{
    const std::wstring clsidDesc = CleanDescription(ReadClsidDescription(clsid));

    std::wstring product;
    std::wstring fileDesc;
    if (!dll.empty())
    {
        product  = CleanDescription(ReadDllVersionString(dll, L"ProductName"));
        fileDesc = CleanDescription(ReadDllVersionString(dll, L"FileDescription"));
    }
    const std::wstring dllName = dll.empty() ? std::wstring() : StripExtension(dll);

    if (!clsidDesc.empty() && !LooksInternal(clsidDesc))
    {
        return clsidDesc;
    }
    if (!product.empty() && product.find(L"Operating System") == std::wstring::npos)
    {
        return product;
    }
    if (!fileDesc.empty())
    {
        return fileDesc;
    }
    if (!clsidDesc.empty())
    {
        return clsidDesc;
    }
    if (!dllName.empty())
    {
        return dllName;
    }
    return regName;
}

// ---------------------------------------------------------------------------
// 命令行拆分：拆出 exe 路径（参数模板本模块不需要，只取 exe 用于归并分组）
// ---------------------------------------------------------------------------
std::wstring ExePathFromCommand(const std::wstring& command)
{
    std::wstring text = command;
    size_t begin = text.find_first_not_of(L" \t");
    if (begin == std::wstring::npos)
    {
        return std::wstring();
    }
    size_t end = text.find_last_not_of(L" \t");
    text = text.substr(begin, end - begin + 1);

    std::wstring exe;
    if (text[0] == L'"')
    {
        const size_t closeQuote = text.find(L'"', 1);
        exe = closeQuote == std::wstring::npos ? text.substr(1) : text.substr(1, closeQuote - 1);
    }
    else
    {
        std::wstring lowered = ToLower(text);
        const size_t exePos = lowered.find(L".exe");
        if (exePos != std::wstring::npos)
        {
            exe = text.substr(0, exePos + 4);
        }
        else
        {
            const size_t spacePos = text.find(L' ');
            exe = spacePos == std::wstring::npos ? text : text.substr(0, spacePos);
        }
    }

    if (!exe.empty() && exe.find(L'%') != std::wstring::npos)
    {
        wchar_t expanded[2048] = {};
        if (::ExpandEnvironmentStringsW(exe.c_str(), expanded, ARRAYSIZE(expanded)) > 0)
        {
            exe = expanded;
        }
    }
    return exe;
}

// exe 的文件显示名（分组名）；取不到用文件名兜底
std::wstring DisplayNameOfExe(const std::wstring& exePath)
{
    if (exePath.empty())
    {
        return std::wstring();
    }
    SHFILEINFOW info = {};
    if (::SHGetFileInfoW(exePath.c_str(), 0, &info, sizeof(info),
                         SHGFI_DISPLAYNAME | SHGFI_USEFILEATTRIBUTES) &&
        info.szDisplayName[0] != L'\0')
    {
        return info.szDisplayName;
    }
    return StripExtension(exePath);
}

// ---------------------------------------------------------------------------
// 静态动词扫描：一个注册表根（如 *\shell）下的所有动词
// ---------------------------------------------------------------------------
struct StaticRoot
{
    const wchar_t* path;
    unsigned int    scenes;
};

const StaticRoot kStaticRoots[] =
{
    { L"*\\shell",                              SceneFile },
    { L"Directory\\shell",                      SceneFolder },
    { L"Directory\\Background\\shell",          SceneBackground },
    { L"Folder\\shell",                         SceneFolder },
};

// 在报告里按归并键找分组，找不到就新建
ContextMenuGroup& FindOrAddGroup(ContextMenuReport& report,
                                 const std::wstring& sourceKey,
                                 const std::wstring& displayName,
                                 bool isShellex)
{
    for (ContextMenuGroup& group : report.groups)
    {
        if (group.sourceKey == sourceKey)
        {
            return group;
        }
    }

    ContextMenuGroup group;
    group.sourceKey   = sourceKey;
    group.name        = displayName;
    group.isShellex   = isShellex;
    report.groups.push_back(std::move(group));
    return report.groups.back();
}

// 扫一个静态动词根，把读到的项写进报告
void ScanStaticRoot(const StaticRoot& root, ContextMenuReport& report)
{
    const std::vector<std::wstring> verbs = ReadSubKeyNames(HKEY_CLASSES_ROOT, root.path);

    for (const std::wstring& verb : verbs)
    {
        const std::wstring verbKey = std::wstring(root.path) + L"\\" + verb;

        // command 子键默认值 = 命令行；没有 command 的纯占位动词跳过
        const std::wstring command = ReadRegValue(HKEY_CLASSES_ROOT,
                                                  verbKey + L"\\command", nullptr);
        if (command.empty())
        {
            continue;
        }

        // 显示名：MUIVerb 值优先，其次键默认值，再兜底动词名；间接字符串要解析
        std::wstring text = ReadRegValue(HKEY_CLASSES_ROOT, verbKey, L"MUIVerb");
        if (text.empty())
        {
            text = ReadRegValue(HKEY_CLASSES_ROOT, verbKey, nullptr);
        }
        if (text.empty())
        {
            text = verb;
        }
        text = ResolveIndirectString(text);

        // 归并分组：按命令行 exe 路径小写；拆不出 exe 就用动词名兜底
        const std::wstring exe = ExePathFromCommand(command);
        const std::wstring sourceKey = !exe.empty() ? ToLower(exe) : L"static:" + ToLower(verb);
        const std::wstring groupName = DisplayNameOfExe(exe);

        ContextMenuGroup& group = FindOrAddGroup(report, sourceKey,
                                                 groupName.empty() ? verb : groupName, false);

        ContextMenuItem item;
        item.id          = verb;
        item.text        = text;
        item.verb        = verb;
        item.commandLine = command;
        item.scenes      = root.scenes;
        item.dllPath     = exe;
        group.items.push_back(std::move(item));
    }
}

// 扫一个 shellex 根，把处理器写进报告（组内菜单项留空，待 Collector 采集）
void ScanShellexRoot(const StaticRoot& root, ContextMenuReport& report,
                     std::vector<EnumeratedHandler>* handlersOut)
{
    const std::wstring handlerRoot =
        std::wstring(root.path) + L"\\shellex\\ContextMenuHandlers";
    const std::vector<std::wstring> names =
        ReadSubKeyNames(HKEY_CLASSES_ROOT, handlerRoot);

    for (const std::wstring& name : names)
    {
        const std::wstring handlerKey = handlerRoot + L"\\" + name;

        const std::wstring clsidStr = ReadRegValue(HKEY_CLASSES_ROOT, handlerKey, nullptr);
        if (clsidStr.empty())
        {
            continue;
        }

        CLSID clsid = {};
        if (::CLSIDFromString(clsidStr.c_str(), &clsid) != S_OK)
        {
            continue;
        }

        const std::wstring dll = ReadClsidDllPath(clsid);
        const std::wstring displayName = BuildHandlerDisplayName(name, clsid, dll);
        const std::wstring sourceKey = !dll.empty() ? ToLower(dll) : L"clsid:" + ToLower(clsidStr);

        // 同一处理器可能在多个根注册：场景位叠加，分组只建一次
        ContextMenuGroup* group = nullptr;
        for (ContextMenuGroup& g : report.groups)
        {
            if (g.sourceKey == sourceKey)
            {
                group = &g;
                break;
            }
        }

        EnumeratedHandler handler;
        handler.clsid    = clsid;
        handler.clsidStr = ClsidToString(clsid);
        handler.regName  = name;
        handler.dllPath  = dll;
        handler.displayName = displayName;
        handler.scenes   = root.scenes;

        if (group == nullptr)
        {
            ContextMenuGroup newGroup;
            newGroup.sourceKey = sourceKey;
            newGroup.name      = displayName;
            newGroup.isShellex = true;
            report.groups.push_back(std::move(newGroup));
            group = &report.groups.back();
        }
        else
        {
            // 已存在：把新场景位并进去
            handler.scenes |= root.scenes;
        }

        if (handlersOut != nullptr)
        {
            handlersOut->push_back(handler);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------

std::vector<EnumeratedHandler> EnumerateShellexHandlers()
{
    ContextMenuReport throwaway;
    std::vector<EnumeratedHandler> handlers;

    for (const StaticRoot& root : kStaticRoots)
    {
        ScanShellexRoot(root, throwaway, &handlers);
    }

    // AllFilesystemObjects：文件和文件夹都出现
    const std::vector<std::wstring> names =
        ReadSubKeyNames(HKEY_CLASSES_ROOT,
                        L"AllFilesystemObjects\\shellex\\ContextMenuHandlers");
    for (const std::wstring& name : names)
    {
        const std::wstring handlerKey =
            L"AllFilesystemObjects\\shellex\\ContextMenuHandlers\\" + name;
        const std::wstring clsidStr = ReadRegValue(HKEY_CLASSES_ROOT, handlerKey, nullptr);
        if (clsidStr.empty())
        {
            continue;
        }
        CLSID clsid = {};
        if (::CLSIDFromString(clsidStr.c_str(), &clsid) != S_OK)
        {
            continue;
        }

        EnumeratedHandler handler;
        handler.clsid    = clsid;
        handler.clsidStr = ClsidToString(clsid);
        handler.regName  = name;
        handler.dllPath  = ReadClsidDllPath(clsid);
        handler.displayName = BuildHandlerDisplayName(name, clsid, handler.dllPath);
        handler.scenes   = SceneFile | SceneFolder;
        handlers.push_back(handler);
    }

    return handlers;
}

ContextMenuReport EnumerateRegistry()
{
    ContextMenuReport report;

    // 1) 静态动词（命令行可读，直接成项）
    for (const StaticRoot& root : kStaticRoots)
    {
        ScanStaticRoot(root, report);
    }

    // 2) shellex 处理器（只建分组占位，菜单项由 Collector 补）
    for (const StaticRoot& root : kStaticRoots)
    {
        ScanShellexRoot(root, report, nullptr);
    }

    Logger::Info(L"右键菜单注册表枚举完成：分组 " + std::to_wstring(report.groups.size()));
    return report;
}

} // namespace shell
