// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellNew.cpp
//
// 「新建」菜单的实现。说明见 ShellNew.h。
// ============================================================================

#include "shell/ShellNew.h"

#include "core/Logger.h"

#include <algorithm>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace shell
{
namespace
{

// ---------------------------------------------------------------------------
// 读注册表字符串值（REG_SZ / REG_EXPAND_SZ）
// ---------------------------------------------------------------------------
std::wstring ReadStringValue(HKEY key, const wchar_t* name)
{
    if (key == nullptr || name == nullptr)
    {
        return std::wstring();
    }

    wchar_t buffer[2048] = {};
    DWORD   size = sizeof(buffer);
    DWORD   type = 0;

    if (::RegQueryValueExW(key, name, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    if (type != REG_SZ && type != REG_EXPAND_SZ)
    {
        return std::wstring();
    }

    return std::wstring(buffer);
}

// 某个值存不存在（不管内容是什么，NullFile / Directory 这种就是"有键无值"）
bool HasValue(HKEY key, const wchar_t* name)
{
    if (key == nullptr)
    {
        return false;
    }

    return ::RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// 解析 "@shell32.dll,-30318" 这种间接字符串（系统菜单里就是这种写法）
// ---------------------------------------------------------------------------
std::wstring ResolveIndirectString(const std::wstring& source)
{
    if (source.empty())
    {
        return std::wstring();
    }

    // 没有 @ 前缀的就是普通字符串
    if (source.front() != L'@')
    {
        return source;
    }

    wchar_t buffer[512] = {};
    if (SUCCEEDED(::SHLoadIndirectString(source.c_str(), buffer,
                                         static_cast<UINT>(std::size(buffer)), nullptr)) &&
        buffer[0] != L'\0')
    {
        return std::wstring(buffer);
    }

    return std::wstring();
}

// ---------------------------------------------------------------------------
// 打开某一行（扩展名或 ProgID）的 ShellNew 键。
//
// 依次找三个地方：HKCR（64 位）→ HKCU\Software\Classes → HKCR\Wow6432Node（32 位）。
// 最后那个不能少：WinRAR、WPS 这类 32 位程序把模板登记在 32 位视图里。
// ---------------------------------------------------------------------------
HKEY OpenShellNewKey(const std::wstring& menuKey)
{
    const std::wstring subPath = menuKey + L"\\ShellNew";

    HKEY key = nullptr;

    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, subPath.c_str(), 0, KEY_READ | KEY_WOW64_64KEY,
                        &key) == ERROR_SUCCESS)
    {
        return key;
    }

    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        (L"Software\\Classes\\" + subPath).c_str(), 0, KEY_READ,
                        &key) == ERROR_SUCCESS)
    {
        return key;
    }

    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT,
                        (L"Wow6432Node\\" + subPath).c_str(), 0, KEY_READ | KEY_WOW64_32KEY,
                        &key) == ERROR_SUCCESS)
    {
        return key;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// 读 HKCR 下某个键的默认值（用 RegGetValue，路径一次到位，比 RegQueryValueEx 稳）
// ---------------------------------------------------------------------------
std::wstring ReadHkcrDefault(const std::wstring& subKey)
{
    if (subKey.empty())
    {
        return std::wstring();
    }

    wchar_t buffer[512] = {};
    DWORD   size = static_cast<DWORD>(sizeof(buffer));
    DWORD   type = 0;

    const LONG result = ::RegGetValueW(HKEY_CLASSES_ROOT, subKey.c_str(), nullptr,
                                       RRF_RT_REG_SZ, &type, buffer, &size);

    if (result != ERROR_SUCCESS || buffer[0] == L'\0')
    {
        return std::wstring();
    }

    return ResolveIndirectString(std::wstring(buffer));
}

// ---------------------------------------------------------------------------
// 菜单显示名
//
//   1. ShellNew\MenuText（"@shell32.dll,-30318" → "快捷方式"）
//   2. ShellNew 的默认值
//   3. 扩展名登记的 ProgID → 这个 ProgID 的"友好类型名"
//      （和资源管理器一样：.zip 登记成 WinRAR.ZIP，显示"WinRAR ZIP 压缩文件"）
//   4. 按扩展名问系统
//   5. 问系统"这种文件叫什么类型"
//   6. 实在没有就用键名
// ---------------------------------------------------------------------------
std::wstring ResolveDisplayName(HKEY shellNewKey, const std::wstring& menuKey,
                                const std::wstring& extension)
{
    // 1. MenuText
    if (shellNewKey != nullptr)
    {
        const std::wstring menuText =
            ResolveIndirectString(ReadStringValue(shellNewKey, L"MenuText"));

        if (!menuText.empty())
        {
            return menuText;
        }
    }

    // 2. ShellNew 键的默认值
    wchar_t nameBuffer[512] = {};
    DWORD   nameSize = sizeof(nameBuffer);
    DWORD   nameType = 0;

    if (shellNewKey != nullptr &&
        ::RegQueryValueExW(shellNewKey, nullptr, nullptr, &nameType,
                           reinterpret_cast<LPBYTE>(nameBuffer), &nameSize) == ERROR_SUCCESS &&
        nameType == REG_SZ && nameBuffer[0] != L'\0')
    {
        return ResolveIndirectString(std::wstring(nameBuffer));
    }

    // 3. 扩展名 → 它登记的 ProgID → 问这个 ProgID 的"友好类型名"
    const std::wstring progId = ReadHkcrDefault(menuKey);

    if (!progId.empty())
    {
        wchar_t friendlyName[512] = {};
        DWORD   length = static_cast<DWORD>(std::size(friendlyName));

        if (SUCCEEDED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYDOCNAME,
                                          progId.c_str(), nullptr,
                                          friendlyName, &length)) &&
            friendlyName[0] != L'\0')
        {
            return ResolveIndirectString(std::wstring(friendlyName));
        }
    }

    // 3.5 按扩展名问系统（没登记 ProgID 的扩展名走这里）
    {
        wchar_t friendlyName[512] = {};
        DWORD   length = static_cast<DWORD>(std::size(friendlyName));

        if (SUCCEEDED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYDOCNAME,
                                          menuKey.c_str(), nullptr,
                                          friendlyName, &length)) &&
            friendlyName[0] != L'\0')
        {
            return ResolveIndirectString(std::wstring(friendlyName));
        }
    }

    // 4. 系统类型名（假装有个文件，不去碰真实文件）
    if (!extension.empty())
    {
        const std::wstring fakeName = L"x" + extension;

        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(fakeName.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                             SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) &&
            info.szTypeName[0] != L'\0')
        {
            return std::wstring(info.szTypeName);
        }
    }

    // 5. 兜底
    return menuKey;
}

// ---------------------------------------------------------------------------
// 读一份 ShellNew 子键，转成一条菜单项。解析不出来返回 false。
// ---------------------------------------------------------------------------
bool ParseShellNewKey(const std::wstring& menuKey, ShellNewItem& item)
{
    HKEY shellNewKey = OpenShellNewKey(menuKey);
    if (shellNewKey == nullptr)
    {
        return false;
    }

    item.menuKey = menuKey;

    // 扩展名（以 '.' 开头才算）
    if (!menuKey.empty() && menuKey.front() == L'.')
    {
        item.extension = menuKey;
    }

    item.iconPath   = ReadStringValue(shellNewKey, L"IconPath");
    item.handlerClsid = ReadStringValue(shellNewKey, L"Handler");

    bool recognized = false;

    // ---- 文件夹 ----
    if (HasValue(shellNewKey, L"Directory") || menuKey == L"Folder" ||
        _wcsicmp(menuKey.c_str(), L"Directory") == 0)
    {
        item.kind  = ShellNewKind::Folder;
        recognized = true;
    }

    // ---- 空文件 ----
    if (!recognized && (HasValue(shellNewKey, L"NullFile") || HasValue(shellNewKey, L"Data")))
    {
        item.kind  = ShellNewKind::NullFile;
        recognized = true;
    }

    // ---- 复制模板文件 ----
    if (!recognized)
    {
        const std::wstring templateValue = ReadStringValue(shellNewKey, L"FileName");

        if (!templateValue.empty())
        {
            wchar_t expanded[MAX_PATH * 2] = {};
            ::ExpandEnvironmentStringsW(templateValue.c_str(), expanded,
                                        static_cast<DWORD>(std::size(expanded)));

            if (expanded[0] != L'\0' && ::PathIsRelativeW(expanded) == FALSE)
            {
                item.kind         = ShellNewKind::FileName;
                item.templatePath = std::wstring(expanded);
                recognized        = true;
            }
        }
    }

    // ---- 交给外部程序自己建 ----
    if (!recognized)
    {
        const std::wstring commandValue = ReadStringValue(shellNewKey, L"Command");

        if (!commandValue.empty())
        {
            item.kind    = ShellNewKind::Command;
            item.command = commandValue;
            recognized   = true;
        }
    }

    item.displayName = ResolveDisplayName(shellNewKey, menuKey, item.extension);

    ::RegCloseKey(shellNewKey);
    return recognized;
}

// ---------------------------------------------------------------------------
// 读系统那份「新建」清单（Discardable\PostSetup\ShellNew\Classes）
// ---------------------------------------------------------------------------
std::vector<std::wstring> ReadSystemClassList()
{
    std::vector<std::wstring> list;

    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
                        L"Discardable\\PostSetup\\ShellNew",
                        0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return list;
    }

    DWORD type = 0;
    DWORD size = 0;

    // 先问一下有多长（REG_MULTI_SZ 是一串以 \0 分隔、双 \0 结尾的字符串）
    if (::RegQueryValueExW(key, L"Classes", nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_MULTI_SZ && type != REG_SZ) || size == 0)
    {
        ::RegCloseKey(key);
        return list;
    }

    std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 2, L'\0');

    if (::RegQueryValueExW(key, L"Classes", nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer.data()), &size) == ERROR_SUCCESS)
    {
        // 逐个取到空串为止
        const wchar_t* cursor = buffer.data();
        while (*cursor != L'\0')
        {
            list.push_back(std::wstring(cursor));
            cursor += ::wcslen(cursor) + 1;
        }
    }

    ::RegCloseKey(key);
    return list;
}

// ---------------------------------------------------------------------------
// 把 HKCR 里所有带 ShellNew 的顶层键都列出来（给系统清单做补充）
// ---------------------------------------------------------------------------
void CollectAllShellNewKeys(std::vector<std::wstring>& keys)
{
    HKEY root = nullptr;

    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, L"", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
    {
        return;
    }

    DWORD subKeyCount = 0;
    ::RegQueryInfoKeyW(root, nullptr, nullptr, nullptr, &subKeyCount, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr, nullptr);

    const int maxCount = static_cast<int>(subKeyCount);

    for (int index = 0; index < maxCount; ++index)
    {
        wchar_t subKeyName[256] = {};
        DWORD   nameLength = 255;

        if (::RegEnumKeyExW(root, index, subKeyName, &nameLength, nullptr, nullptr, nullptr,
                            nullptr) != ERROR_SUCCESS)
        {
            continue;
        }

        HKEY shellNewKey = nullptr;
        if (::RegOpenKeyExW(root, (std::wstring(subKeyName) + L"\\ShellNew").c_str(), 0,
                            KEY_READ, &shellNewKey) == ERROR_SUCCESS)
        {
            keys.push_back(std::wstring(subKeyName));
            ::RegCloseKey(shellNewKey);
        }
    }

    ::RegCloseKey(root);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 从命令行里取出第一个程序路径（"C:\a\b.exe" /c 这种）
// ---------------------------------------------------------------------------
std::wstring ExtractCommandProgram(const std::wstring& command)
{
    std::wstring text = command;

    // 去掉前导空格
    while (!text.empty() && text.front() == L' ')
    {
        text.erase(text.begin());
    }

    if (text.empty())
    {
        return std::wstring();
    }

    if (text.front() == L'"')
    {
        const size_t closing = text.find(L'"', 1);
        if (closing != std::wstring::npos)
        {
            return text.substr(1, closing - 1);
        }
        return std::wstring();
    }

    const size_t space = text.find(L' ');
    return (space == std::wstring::npos) ? text : text.substr(0, space);
}

// 这条命令还能不能用：程序还在就认为能用（不带路径的系统命令当作可用）
bool CommandProgramExists(const std::wstring& command)
{
    const std::wstring program = ExtractCommandProgram(command);

    if (program.empty())
    {
        return false;
    }

    if (program.find(L'\\') == std::wstring::npos &&
        program.find(L'/') == std::wstring::npos)
    {
        return true;
    }

    return ::GetFileAttributesW(program.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---------------------------------------------------------------------------
// 在菜单（含各级子菜单）里按文字找「快捷方式」那一项，返回它的命令 ID
//
// 系统背景菜单里的「新建」子菜单是"延迟填充"的：QueryContextMenu 之后它是空的，
// 必须让 IContextMenu2 处理一次 WM_INITMENUPOPUP 才会把文件夹/快捷方式/各种
// 文件类型填进去。所以这里每遇到一个子菜单都先喂一次 HandleMenuMsg。
// ---------------------------------------------------------------------------
UINT FindShortcutCommandId(HMENU menu, IContextMenu2* contextMenu2)
{
    const int count = ::GetMenuItemCount(menu);

    for (int index = 0; index < count; ++index)
    {
        MENUITEMINFOW itemInfo = {};

        wchar_t text[256] = {};
        itemInfo.cbSize     = sizeof(itemInfo);
        itemInfo.fMask      = MIIM_SUBMENU | MIIM_ID | MIIM_STRING;
        itemInfo.dwTypeData = text;
        itemInfo.cch        = static_cast<UINT>(std::size(text));

        if (!::GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &itemInfo))
        {
            continue;
        }

        if (itemInfo.hSubMenu != nullptr)
        {
            // 先让系统把这一级子菜单填满
            if (contextMenu2 != nullptr)
            {
                contextMenu2->HandleMenuMsg(WM_INITMENUPOPUP,
                                            reinterpret_cast<WPARAM>(itemInfo.hSubMenu),
                                            MAKELPARAM(index, 0));
            }

            const UINT nested = FindShortcutCommandId(itemInfo.hSubMenu, contextMenu2);
            if (nested != 0)
            {
                return nested;
            }
            continue;
        }

        // 去掉 & 快捷键标记再比较（系统菜单里写的是「快捷方式(&S)」）
        std::wstring clean;
        for (const wchar_t* ch = text; *ch != L'\0'; ++ch)
        {
            if (*ch != L'&')
            {
                clean.push_back(*ch);
            }
        }

        if (clean.find(L"快捷方式") != std::wstring::npos ||
            clean.find(L"Shortcut") != std::wstring::npos)
        {
            return itemInfo.wID;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// 弹系统的「创建快捷方式」向导
//
// Win10/11 上老的 rundll32.exe appwiz.cpl,NewLinkHere 已经不弹任何东西了
// （实测进程启动后立刻退出）。可靠做法是执行资源管理器自己的
// 「新建 → 快捷方式」那一项：拿目录的背景右键菜单，找到那一项并 Invoke。
// 这样向导、默认名字、保存位置都是系统行为，和资源管理器完全一致。
// ---------------------------------------------------------------------------
bool LaunchShortcutWizard(const std::wstring& directory, std::wstring& error)
{
    constexpr UINT kCommandIdFirst = 1;
    constexpr UINT kCommandIdLast  = 0x7FFF;

    LPITEMIDLIST folderPidl = nullptr;
    if (FAILED(::SHParseDisplayName(directory.c_str(), nullptr, &folderPidl, 0, nullptr)) ||
        folderPidl == nullptr)
    {
        error = L"打开「创建快捷方式」向导失败（路径解析不出来）。";
        return false;
    }

    IShellFolder*      desktopFolder = nullptr;
    IShellFolder*      targetFolder  = nullptr;
    IContextMenu*      contextMenu   = nullptr;
    IContextMenu2*     contextMenu2  = nullptr;
    HMENU              menu          = nullptr;
    bool               started       = false;

    if (SUCCEEDED(::SHGetDesktopFolder(&desktopFolder)) && desktopFolder != nullptr &&
        SUCCEEDED(desktopFolder->BindToObject(folderPidl, nullptr, IID_IShellFolder,
                                              reinterpret_cast<void**>(&targetFolder))) &&
        targetFolder != nullptr &&
        SUCCEEDED(targetFolder->CreateViewObject(nullptr, IID_IContextMenu,
                                                 reinterpret_cast<void**>(&contextMenu))) &&
        contextMenu != nullptr)
    {
        menu = ::CreatePopupMenu();

        if (menu != nullptr &&
            SUCCEEDED(contextMenu->QueryContextMenu(menu, 0, kCommandIdFirst, kCommandIdLast,
                                                    CMF_NORMAL)))
        {
            // 「新建」子菜单要 IContextMenu2 处理 WM_INITMENUPOPUP 才有内容
            contextMenu->QueryInterface(IID_IContextMenu2,
                                        reinterpret_cast<void**>(&contextMenu2));

            const UINT commandId = FindShortcutCommandId(menu, contextMenu2);

            if (commandId != 0)
            {
                CMINVOKECOMMANDINFOEX invoke = {};
                invoke.cbSize = sizeof(invoke);
                invoke.fMask  = CMIC_MASK_UNICODE;
                invoke.hwnd   = ::GetForegroundWindow();
                invoke.lpVerb  = MAKEINTRESOURCEA(commandId - kCommandIdFirst);
                invoke.lpVerbW = MAKEINTRESOURCEW(commandId - kCommandIdFirst);
                invoke.nShow   = SW_SHOWNORMAL;

                started = SUCCEEDED(contextMenu->InvokeCommand(
                    reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke)));
            }
        }
    }

    if (menu != nullptr)
    {
        ::DestroyMenu(menu);
    }
    if (contextMenu2 != nullptr)
    {
        contextMenu2->Release();
    }
    if (contextMenu != nullptr)
    {
        contextMenu->Release();
    }
    if (targetFolder != nullptr)
    {
        targetFolder->Release();
    }
    if (desktopFolder != nullptr)
    {
        desktopFolder->Release();
    }
    ::CoTaskMemFree(folderPidl);

    if (!started)
    {
        error = L"没能启动系统的「创建快捷方式」向导。";
        Logger::Error(L"新建快捷方式失败：系统菜单里没找到「快捷方式」项");
        return false;
    }

    return true;
}

// 生成唯一名字：目录里已经有同名时加 (2)、(3)…
// ---------------------------------------------------------------------------
std::wstring MakeUniquePath(const std::wstring& directory,
                            const std::wstring& baseName,
                            const std::wstring& extension)
{
    for (int attempt = 1; attempt <= 999; ++attempt)
    {
        std::wstring candidate = directory + L"\\" + baseName;

        if (attempt > 1)
        {
            candidate += L" (" + std::to_wstring(attempt) + L")";
        }

        candidate += extension;

        if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return candidate;
        }
    }

    return directory + L"\\" + baseName + L" (999)" + extension;
}

} // 匿名命名空间

// ============================================================================
// 枚举
// ============================================================================

std::vector<ShellNewItem> EnumerateShellNew()
{
    std::vector<ShellNewItem> items;
    std::vector<std::wstring> orderedKeys;

    // ---- 1. 系统自己的顺序表 ----
    orderedKeys = ReadSystemClassList();

    // ---- 2. 表里没有的（或者压根没这张表）都用全量枚举补上 ----
    {
        std::vector<std::wstring> allKeys;
        CollectAllShellNewKeys(allKeys);

        for (const std::wstring& key : allKeys)
        {
            bool exists = false;
            for (const std::wstring& known : orderedKeys)
            {
                if (_wcsicmp(known.c_str(), key.c_str()) == 0)
                {
                    exists = true;
                    break;
                }
            }

            if (!exists)
            {
                orderedKeys.push_back(key);
            }
        }
    }

    // ---- 3. 按顺序解析每一项 ----
    for (const std::wstring& key : orderedKeys)
    {
        ShellNewItem item;
        bool recognized = ParseShellNewKey(key, item);

        if (!recognized)
        {
            // 系统清单里有这一行、但注册表里没读出任何模板（常见于软件卸载后
            // 留下的 .doc/.docx 这类"曾经注册过"的项）。
            // 这种情况下按"建一个空文件"处理——资源管理器显示的也就是这一项，
            // 少了它用户会觉得菜单比系统的少。
            if (!key.empty() && key.front() == L'.')
            {
                item.menuKey     = key;
                item.extension   = key;
                item.kind        = ShellNewKind::NullFile;
                item.displayName = ResolveDisplayName(nullptr, key, key);
                recognized       = true;
            }
            else
            {
                continue;
            }
        }

        // 去掉完全重复的（.wps 之类有时会登记两遍）
        bool duplicated = false;
        for (const ShellNewItem& existing : items)
        {
            if (_wcsicmp(existing.menuKey.c_str(), item.menuKey.c_str()) == 0)
            {
                duplicated = true;
                break;
            }
        }

        if (duplicated)
        {
            continue;
        }

        // ---- 过滤掉"显示了也点不动"的项（系统菜单里同样不显示它们）----
        //
        //   · Handler 项：模板由一个 COM 处理器负责（.library-ms 的"库"就是），
        //     我们没实现这个处理器，显示出来只会点了没反应 —— 跳过。
        //     .lnk 虽然有 Handler，但它走的是系统的「创建快捷方式」向导，单独保留。
        //   · Command 项：命令里的程序已经不在了（比如卸载 Windows Mail 后的
        //     .contact 指着 Wab.exe）—— 跳过，免得点了报错。
        if (!item.handlerClsid.empty() && _wcsicmp(item.menuKey.c_str(), L".lnk") != 0)
        {
            Logger::Info(L"「新建」跳过 Handler 项：" + item.menuKey);
            continue;
        }

        if (item.kind == ShellNewKind::Command && !CommandProgramExists(item.command))
        {
            Logger::Info(L"「新建」跳过失效的 Command 项：" + item.menuKey);
            continue;
        }

        items.push_back(std::move(item));
    }

    // ---- 3.5 Windows 内置的两项「新建」----
    //
    // 资源管理器的「新建」里一直有「BMP 图像」和「文本文档」，但它们并不依赖
    // 注册表里的 ShellNew 键（这台机器上 .txt/.bmp 都没有 ShellNew 却照样显示）。
    // 注册表里没有登记时，我们自己补上，文字用系统菜单里的写法。
    struct BuiltinNewItem
    {
        const wchar_t* extension;
        const wchar_t* displayName;
    };

    static const BuiltinNewItem kBuiltinNewItems[] = {
        {L".bmp", L"BMP 图像"},
        {L".txt", L"文本文档"},
    };

    for (const BuiltinNewItem& builtin : kBuiltinNewItems)
    {
        bool exists = false;
        for (const ShellNewItem& existing : items)
        {
            if (_wcsicmp(existing.extension.c_str(), builtin.extension) == 0)
            {
                exists = true;
                break;
            }
        }

        if (!exists)
        {
            ShellNewItem item;
            item.kind        = ShellNewKind::NullFile;
            item.menuKey     = builtin.extension;
            item.extension   = builtin.extension;
            item.displayName = builtin.displayName;
            items.push_back(std::move(item));
        }
    }

    // ---- 4. 排序：文件夹、快捷方式固定最前，其余按扩展名字母序 ----
    //        资源管理器就是这么排的（.bmp → .doc → .docx → .pdf… → .txt）
    std::vector<ShellNewItem> ordered;
    ordered.reserve(items.size());

    for (const ShellNewItem& item : items)
    {
        if (item.kind == ShellNewKind::Folder)
        {
            ordered.push_back(item);
        }
    }

    // 注册表里没有 Folder 项（极少见）时自己补一条，保证「文件夹」永远在
    if (ordered.empty())
    {
        ShellNewItem folderItem;
        folderItem.kind        = ShellNewKind::Folder;
        folderItem.menuKey     = L"Folder";
        folderItem.displayName = L"文件夹";
        ordered.push_back(std::move(folderItem));
    }

    for (const ShellNewItem& item : items)
    {
        if (_wcsicmp(item.menuKey.c_str(), L".lnk") == 0)
        {
            ordered.push_back(item);
        }
    }

    for (const ShellNewItem& item : items)
    {
        if (item.kind == ShellNewKind::Folder ||
            _wcsicmp(item.menuKey.c_str(), L".lnk") == 0)
        {
            continue;
        }

        ordered.push_back(item);
    }

    // 剩下这些按扩展名字母序（Folder / .lnk 已经在前面，不受影响）
    if (ordered.size() > 2)
    {
        std::stable_sort(ordered.begin() + 2, ordered.end(),
                         [](const ShellNewItem& left, const ShellNewItem& right)
                         {
                             return ::CompareStringOrdinal(left.menuKey.c_str(), -1,
                                                           right.menuKey.c_str(), -1,
                                                           TRUE) == CSTR_LESS_THAN;
                         });
    }

    items.swap(ordered);

    Logger::Info(L"「新建」项枚举完成，共 " + std::to_wstring(items.size()) + L" 项");

    for (const ShellNewItem& item : items)
    {
        Logger::Info(L"  · " + item.displayName + L"（键=" + item.menuKey +
                     L"，类型=" + std::to_wstring(static_cast<int>(item.kind)) + L"）");
    }

    return items;
}

// ============================================================================
// 新建文件时用的基础名
//
// 和资源管理器一样的观感：「新建 文本文档.txt」。
// 显示名本身就和扩展名一样时（比如 "txt"），不再重复加。
// ============================================================================

std::wstring NewItemBaseName(const ShellNewItem& item)
{
    if (item.kind == ShellNewKind::Folder)
    {
        return L"新建文件夹";
    }

    if (item.displayName.empty())
    {
        return L"新建";
    }

    std::wstring lowerExtension = item.extension;
    if (!lowerExtension.empty() && lowerExtension.front() == L'.')
    {
        lowerExtension.erase(lowerExtension.begin());
    }

    std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });

    std::wstring lowerName = item.displayName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });

    if (!lowerExtension.empty() && lowerName == lowerExtension)
    {
        return L"新建";
    }

    return L"新建 " + item.displayName;
}

// ============================================================================
// 创建
// ============================================================================

bool CreateShellNewItem(const ShellNewItem& item,
                        const std::wstring& directory,
                        std::wstring& createdPath,
                        std::wstring& error)
{
    createdPath.clear();
    error.clear();

    if (directory.empty())
    {
        error = L"当前没有打开的文件夹。";
        return false;
    }

    switch (item.kind)
    {
    case ShellNewKind::Folder:
    {
        const std::wstring target = MakeUniquePath(directory, L"新建文件夹", L"");

        if (!::CreateDirectoryW(target.c_str(), nullptr))
        {
            error = L"新建文件夹失败（可能没有写入权限）。";
            Logger::Error(L"新建文件夹失败：" + target);
            return false;
        }

        createdPath = target;
        return true;
    }

    case ShellNewKind::NullFile:
    {
        // 「快捷方式」在注册表里登记的是 NullFile + Handler（由 COM 处理器生成），
        // 直接建个空 .lnk 会得到一个打不开的坏文件。
        // 这里按资源管理器的实际表现：弹系统的「创建快捷方式」向导。
        if (_wcsicmp(item.menuKey.c_str(), L".lnk") == 0)
        {
            return LaunchShortcutWizard(directory, error);
        }

        const std::wstring target = MakeUniquePath(directory, NewItemBaseName(item), item.extension);

        HANDLE file = ::CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            error = L"新建文件失败（可能没有写入权限）。";
            Logger::Error(L"新建文件失败：" + target);
            return false;
        }

        ::CloseHandle(file);
        createdPath = target;
        return true;
    }

    case ShellNewKind::FileName:
    {
        const std::wstring target = MakeUniquePath(directory, NewItemBaseName(item), item.extension);

        if (!::CopyFileW(item.templatePath.c_str(), target.c_str(), FALSE))
        {
            error = L"复制模板文件失败（可能没有写入权限）。";
            Logger::Error(L"复制模板失败：" + item.templatePath + L" → " + target);
            return false;
        }

        createdPath = target;
        return true;
    }

    case ShellNewKind::Command:
    {
        // 系统的做法：把命令里的 %1 换成"目标目录"，然后直接执行。
        // 命令本身来自本机注册表（可信来源），用户点的那一项就是它。
        //
        // 快捷方式（.lnk）比较特殊：它的 ShellNew 是 Handler（一个 COM 组件）+
        // NullFile，直接建空文件只会得到一个坏掉的 .lnk。
        // 这里按系统的实际表现，改成弹「创建快捷方式」向导。
        if (_wcsicmp(item.menuKey.c_str(), L".lnk") == 0)
        {
            return LaunchShortcutWizard(directory, error);
        }

        // 其它 Command 项（WPS 的"新建 PDF / AI 演示"之类）：原样执行，
        // 有 %1 就换成目标目录
        std::wstring commandLine = item.command;

        if (commandLine.find(L"%1") != std::wstring::npos)
        {
            std::wstring quoted = L"\"" + directory + L"\"";
            const size_t position = commandLine.find(L"%1");
            commandLine.replace(position, 2, quoted);
        }

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);

        PROCESS_INFORMATION process = {};

        if (!::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                              0, nullptr, directory.c_str(), &startup, &process))
        {
            error = L"启动程序失败（可能是它已经升级或卸载了）。";
            Logger::Error(L"执行新建命令失败：" + commandLine);
            return false;
        }

        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);

        // 交给外部程序建的，名字我们不知道，createdPath 留空
        return true;
    }

    default:
        error = L"不支持这种新建类型。";
        return false;
    }
}

} // namespace shell
