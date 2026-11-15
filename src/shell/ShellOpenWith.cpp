// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellOpenWith.cpp
//
// 「打开方式」的底层实现。设计说明见 ShellOpenWith.h 顶部。
//
// 这里干四件事：
//   1. 读注册表，把"能打开某个扩展名"的程序列出来（Win32 + 商店应用两种）；
//   2. 探测某条程序还在不在（用 shell 解析路径，避开 WindowsApps 的权限问题）；
//   3. 按命令行模板拼参数（模板里的 %1 / "%1" / --single-argument 三种形态都要照顾）；
//   4. 用指定程序打开文件（Win32 走 CreateProcess，商店应用走 AUMID 激活）。
// ============================================================================

#include "shell/ShellOpenWith.h"

#include "core/Logger.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <wil/com.h>

#include <algorithm>
#include <unordered_set>

namespace shell
{

namespace
{

// 不去接管的扩展名：这些类型双击就是"执行/进入"，不该弹"选程序"
const wchar_t* const kIgnoredExtensions[] = {
    L".exe", L".com", L".bat", L".cmd", L".msi", L".msp", L".scr", L".ps1",
    L".vbs", L".js",  L".jse", L".wsf", L".lnk", L".url", L".pif",
};

std::wstring ToLower(std::wstring text)
{
    for (wchar_t& ch : text)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return text;
}

// ---------------------------------------------------------------------------
// 读注册表的一个字符串值
//
// valueName 传 L"" 读的是键的默认值。
// 不存在的键 / 值统一返回空串，调用方按"没有"处理即可，不报错。
// ---------------------------------------------------------------------------
std::wstring ReadRegValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                          bool* typeIsExpand = nullptr)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;

    std::wstring result;
    if (::RegQueryValueExW(key, valueName, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
    {
        result.assign(buffer);
    }

    ::RegCloseKey(key);

    // 注册表里写 %SystemRoot%\system32\notepad.exe 这种，要先展开环境变量
    if (!result.empty() && type == REG_EXPAND_SZ)
    {
        wchar_t expanded[2048] = {};
        if (::ExpandEnvironmentStringsW(result.c_str(), expanded, ARRAYSIZE(expanded)) > 0)
        {
            result = expanded;
        }
    }

    if (typeIsExpand != nullptr)
    {
        *typeIsExpand = (type == REG_EXPAND_SZ);
    }

    return result;
}

// 读一个键下所有子键名（用于遍历 HKCR\Applications）
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
        if (::RegEnumKeyExW(key, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
        {
            break;
        }
        names.push_back(name);
    }

    ::RegCloseKey(key);
    return names;
}

// 读一个键下所有值名（用于 OpenWithList / OpenWithProgids）
std::vector<std::wstring> ReadValueNames(HKEY root, const std::wstring& subKey)
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
        if (::RegEnumValueW(key, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
        {
            break;
        }
        names.push_back(name);
    }

    ::RegCloseKey(key);
    return names;
}

// ---------------------------------------------------------------------------
// 解析 `@{包标识?ms-resource://…}` 这种间接字符串
//
// 商店应用显示名、图标路径都是这个形态，直接用会显示成一长串乱码，
// 必须交给系统解析。解析不出来就原样返回（至少不是空的）。
// ---------------------------------------------------------------------------
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
// 从一条命令行里拆出「exe 路径」和「参数模板」
//
// 注册表里的命令行就两种写法：
//      "C:\Program Files\WPS Office\wps.exe" /prometheus /pdf "%1"
//      C:\Windows\system32\NOTEPAD.EXE %1
// 引号没闭合这种坏数据也要能不崩地处理掉。
// ---------------------------------------------------------------------------
bool SplitCommand(const std::wstring& command, std::wstring& exePath, std::wstring& args)
{
    exePath.clear();
    args.clear();

    std::wstring text = command;
    // 去掉首尾空白
    size_t begin = text.find_first_not_of(L" \t");
    if (begin == std::wstring::npos)
    {
        return false;
    }
    size_t end = text.find_last_not_of(L" \t");
    text = text.substr(begin, end - begin + 1);

    if (text[0] == L'"')
    {
        size_t closeQuote = text.find(L'"', 1);
        if (closeQuote == std::wstring::npos)
        {
            exePath = text.substr(1);
        }
        else
        {
            exePath = text.substr(1, closeQuote - 1);
            args    = text.substr(closeQuote + 1);
        }
    }
    else
    {
        // 没加引号：优先按 ".exe" 的位置切（路径里可能带空格，按空格切会切错）
        std::wstring lowered = ToLower(text);
        size_t exePos = lowered.find(L".exe");
        if (exePos != std::wstring::npos)
        {
            exePos += 4;   // ".exe" 四个字符
            exePath = text.substr(0, exePos);
            args    = text.substr(exePos);
        }
        else
        {
            size_t spacePos = text.find(L' ');
            if (spacePos == std::wstring::npos)
            {
                exePath = text;
            }
            else
            {
                exePath = text.substr(0, spacePos);
                args    = text.substr(spacePos);
            }
        }
    }

    // 参数去掉首尾空白
    size_t argsBegin = args.find_first_not_of(L" \t");
    args = (argsBegin == std::wstring::npos) ? std::wstring() : args.substr(argsBegin);

    // 展开环境变量（有些 command 写的是 %SystemRoot%\...）
    if (!exePath.empty() && exePath.find(L'%') != std::wstring::npos)
    {
        wchar_t expanded[2048] = {};
        if (::ExpandEnvironmentStringsW(exePath.c_str(), expanded, ARRAYSIZE(expanded)) > 0)
        {
            exePath = expanded;
        }
    }

    // exe 路径首尾可能残留空白
    size_t exeBegin = exePath.find_first_not_of(L" \t");
    size_t exeEnd   = exePath.find_last_not_of(L" \t");
    if (exeBegin == std::wstring::npos)
    {
        exePath.clear();
        return false;
    }
    exePath = exePath.substr(exeBegin, exeEnd - exeBegin + 1);

    return !exePath.empty();
}

// ---------------------------------------------------------------------------
// 一个 exe "文件描述"当显示名（"记事本"、"Microsoft Edge"）
//
// 注册表里另有一处友好名，但很多程序没写；系统问一下最省事。
// ---------------------------------------------------------------------------
std::wstring DisplayNameOfExe(const std::wstring& exePath)
{
    SHFILEINFOW info = {};
    if (::SHGetFileInfoW(exePath.c_str(), 0, &info, sizeof(info),
                         SHGFI_DISPLAYNAME | SHGFI_USEFILEATTRIBUTES))
    {
        if (info.szDisplayName[0] != L'\0')
        {
            return info.szDisplayName;
        }
    }

    // 退一步用文件名
    const wchar_t* fileName = ::PathFindFileNameW(exePath.c_str());
    return fileName != nullptr ? std::wstring(fileName) : std::wstring();
}

// 只有 exe 名（没有目录）时，试着找出真实路径
std::wstring ResolveExePath(const std::wstring& exeNameOrPath)
{
    if (exeNameOrPath.empty())
    {
        return std::wstring();
    }

    // 已经是完整路径
    if (exeNameOrPath.size() > 2 && exeNameOrPath[1] == L':' &&
        ::GetFileAttributesW(exeNameOrPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return exeNameOrPath;
    }

    // App Paths（系统登记过的程序路径，记事本、浏览器都在这）
    const wchar_t* appPathKeys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\",
    };

    for (const wchar_t* prefix : appPathKeys)
    {
        std::wstring subKey = std::wstring(prefix) + exeNameOrPath;
        std::wstring value  = ReadRegValue(HKEY_LOCAL_MACHINE, subKey, L"");
        if (!value.empty())
        {
            return value;
        }
    }

    // PATH 里搜一下
    wchar_t found[2048] = {};
    if (::SearchPathW(nullptr, exeNameOrPath.c_str(), nullptr, ARRAYSIZE(found), found, nullptr) > 0)
    {
        return found;
    }

    return std::wstring();
}

// ---------------------------------------------------------------------------
// 从一个 ProgID 构造候选程序
//
// 两条路：
//   有 AppUserModelID → 商店应用（哪怕它同时登记了 WindowsApps 里的 exe，
//                       也一律按 AUMID 激活，直接 CreateProcess 会缺包身份）
//   有 shell\open\command 默认值 → 普通 Win32 程序
// ---------------------------------------------------------------------------
bool BuildFromProgId(const std::wstring& progId, core::OpenWithItem& out)
{
    out = core::OpenWithItem();

    if (progId.empty())
    {
        return false;
    }

    // ---- 商店应用：拿 AppUserModelID ----
    std::wstring aumid = ReadRegValue(HKEY_CLASSES_ROOT, progId + L"\\Application",
                                      L"AppUserModelID");
    if (aumid.empty())
    {
        aumid = ReadRegValue(HKEY_CLASSES_ROOT, progId + L"\\shell\\open", L"AppUserModelID");
    }

    // ---- 显示名 ----
    std::wstring appName = ResolveIndirectString(
        ReadRegValue(HKEY_CLASSES_ROOT, progId + L"\\Application", L"ApplicationName"));

    if (!aumid.empty())
    {
        out.kind   = L"uwp";
        out.appId  = aumid;
        out.progId = progId;      // 打开商店应用时系统只认这个标识，必须存下来
        out.name   = appName.empty() ? progId : appName;
        return true;
    }

    // ---- 普通 Win32 程序 ----
    std::wstring command = ReadRegValue(HKEY_CLASSES_ROOT, progId + L"\\shell\\open\\command",
                                        L"");
    if (command.empty())
    {
        // 既没有 AUMID 也没有可执行命令：这种 ProgID 我们打不开，直接不要
        return false;
    }

    std::wstring exePath;
    std::wstring args;
    if (!SplitCommand(command, exePath, args))
    {
        return false;
    }

    // 极少数 ProgID 的 command 是 "%1"（意思是"就用这类文件自己的关联"），
    // 拆出来的 exe 不是路径，跳过（否则会拿着 "%1" 去 CreateProcess）
    if (exePath.find(L'%') != std::wstring::npos)
    {
        exePath = ResolveExePath(exePath);
        if (exePath.empty())
        {
            return false;
        }
    }

    out.kind = L"exe";
    out.path = exePath;
    out.args = args;

    if (!appName.empty())
    {
        out.name = appName;
    }
    else
    {
        // 类型名有时是"文本文档"这种，程序名更贴近用户预期
        out.name = DisplayNameOfExe(exePath);
    }

    return !out.name.empty();
}

// ---------------------------------------------------------------------------
// 从 exe 文件名构造候选（OpenWithList 里的那种）
// ---------------------------------------------------------------------------
bool BuildFromExeName(const std::wstring& exeName, core::OpenWithItem& out)
{
    out = core::OpenWithItem();

    const std::wstring exePath = ResolveExePath(exeName);
    if (exePath.empty())
    {
        return false;
    }

    out.kind = L"exe";
    out.path = exePath;

    // 系统给 Applications\<exe> 记的友好名（"记事本"）
    std::wstring friendly = ReadRegValue(HKEY_CLASSES_ROOT, L"Applications\\" + exeName, L"");
    out.name = friendly.empty() ? DisplayNameOfExe(exePath) : friendly;

    return !out.name.empty();
}

// 按身份串去重（同一个程序可能从好几个来源被读到）
void AddUnique(std::vector<core::OpenWithItem>& list,
               std::unordered_set<std::wstring>& seen,
               core::OpenWithItem candidate)
{
    if (candidate.path.empty() && candidate.appId.empty())
    {
        return;
    }

    const std::wstring key = core::OpenWithKey(candidate);
    if (key.empty() || seen.count(key) > 0)
    {
        return;
    }

    seen.insert(key);
    list.push_back(std::move(candidate));
}

// 浏览器家族：路径要正斜杠、不带 file:///、不做百分号编码
bool LooksLikeBrowser(const std::wstring& exePath)
{
    const wchar_t* fileName = ::PathFindFileNameW(exePath.c_str());
    if (fileName == nullptr)
    {
        return false;
    }

    const std::wstring name = ToLower(fileName);

    return name == L"msedge.exe" || name == L"chrome.exe" || name == L"firefox.exe" ||
           name == L"iexplore.exe" || name == L"brave.exe" || name == L"opera.exe" ||
           name == L"vivaldi.exe" || name == L"360se.exe" || name == L"qqbrowser.exe";
}

std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to)
{
    if (from.empty())
    {
        return text;
    }

    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos)
    {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

// ---------------------------------------------------------------------------
// 拼命令行参数
//
// 三种模板形态（都是本机实测存在的）：
//   "C:\…\Doubao.exe" --image-viewer-file="%1"      ← 引号在模板里
//   "C:\…\photolaunch.exe" /photo /view "%1"        ← 引号在模板里
//   "C:\…\msedge.exe" --single-argument %1          ← 不能给路径加引号
//
// 坑（照搬 BatchPrint 的教训）：
//   1. 模板里自带引号的 "%1"，要连引号一起替换，否则会变成 ""路径""
//   2. --single-argument 系列的路径不能加引号，加了 Edge/Electron 会把引号
//      当成路径的一部分，结果打不开
//   3. 浏览器家族要正斜杠，且不能带 file:/// 前缀
// ---------------------------------------------------------------------------
std::wstring BuildArguments(const core::OpenWithItem& program, const std::wstring& filePath)
{
    const bool singleArgument = program.args.find(L"--single-argument") != std::wstring::npos;

    std::wstring target = filePath;
    if (LooksLikeBrowser(program.path) || singleArgument)
    {
        std::replace(target.begin(), target.end(), L'\\', L'/');
    }

    const std::wstring bareTarget   = singleArgument ? target : (L"\"" + target + L"\"");
    const std::wstring quotedTarget = L"\"" + target + L"\"";

    std::wstring args = program.args;

    if (args.empty())
    {
        // 自己添加的程序没有模板：直接把路径接上
        return bareTarget;
    }

    bool replaced = false;

    // 先替换"带引号的占位符"，再替换裸占位符
    for (const wchar_t* token : {L"\"%1\"", L"\"%L\"", L"\"%l\""})
    {
        if (args.find(token) != std::wstring::npos)
        {
            args = ReplaceAll(args, token, quotedTarget);
            replaced = true;
        }
    }

    for (const wchar_t* token : {L"%1", L"%L", L"%l"})
    {
        if (args.find(token) != std::wstring::npos)
        {
            args = ReplaceAll(args, token, bareTarget);
            replaced = true;
        }
    }

    if (!replaced)
    {
        args += L" ";
        args += bareTarget;
    }

    return args;
}

} // namespace

// ============================================================================
// 扩展名
// ============================================================================

std::wstring ExtensionKeyForPath(const std::wstring& filePath)
{
    const wchar_t* extension = ::PathFindExtensionW(filePath.c_str());

    if (extension == nullptr || extension[0] != L'.')
    {
        return L"(无扩展名)";
    }

    return ToLower(extension);
}

bool IsExtensionHandled(const std::wstring& extension)
{
    const std::wstring lowered = ToLower(extension);

    for (const wchar_t* ignored : kIgnoredExtensions)
    {
        if (lowered == ignored)
        {
            return false;
        }
    }

    return true;
}

// ============================================================================
// 读系统候选
// ============================================================================

std::vector<core::OpenWithItem> EnumerateCandidates(const std::wstring& extension)
{
    std::vector<core::OpenWithItem> list;
    std::unordered_set<std::wstring> seen;

    if (extension.empty() || extension[0] != L'.')
    {
        return list;
    }

    const std::wstring fileExtsKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + extension;

    // ---- 1. 这个扩展名的默认 ProgID ----
    {
        std::wstring progId = ReadRegValue(HKEY_CLASSES_ROOT, extension, L"");
        core::OpenWithItem item;
        if (BuildFromProgId(progId, item))
        {
            AddUnique(list, seen, std::move(item));
        }
    }

    // ---- 2. HKCR\<扩展名>\OpenWithProgids ----
    {
        std::vector<std::wstring> progIds =
            ReadValueNames(HKEY_CLASSES_ROOT, extension + L"\\OpenWithProgids");

        for (const std::wstring& progId : progIds)
        {
            core::OpenWithItem item;
            if (BuildFromProgId(progId, item))
            {
                AddUnique(list, seen, std::move(item));
            }
        }
    }

    // ---- 3. HKCU 下的 OpenWithProgids ----
    {
        std::vector<std::wstring> progIds =
            ReadValueNames(HKEY_CURRENT_USER, fileExtsKey + L"\\OpenWithProgids");

        for (const std::wstring& progId : progIds)
        {
            core::OpenWithItem item;
            if (BuildFromProgId(progId, item))
            {
                AddUnique(list, seen, std::move(item));
            }
        }
    }

    // ---- 4. HKCU 下的 OpenWithList ----
    // 结构是「值名 = a / b / c，值内容 = notepad.exe」这种老式写法，
    // 也有的机器上直接「值名 = notepad.exe、值内容为空」，两种都试。
    {
        std::vector<std::wstring> valueNames =
            ReadValueNames(HKEY_CURRENT_USER, fileExtsKey + L"\\OpenWithList");

        for (const std::wstring& valueName : valueNames)
        {
            if (::_wcsicmp(valueName.c_str(), L"MRUList") == 0)
            {
                continue;
            }

            // 值内容优先（正常情况就是 exe 文件名）
            std::wstring exeName =
                ReadRegValue(HKEY_CURRENT_USER, fileExtsKey + L"\\OpenWithList",
                             valueName.c_str());
            if (exeName.empty())
            {
                exeName = valueName;   // 老写法：值名本身就是 exe 名
            }

            core::OpenWithItem item;
            if (BuildFromExeName(exeName, item))
            {
                AddUnique(list, seen, std::move(item));
            }
        }
    }

    // ---- 5. HKCR\Applications 里声明支持这个扩展名的程序 ----
    // 这一路最慢（要遍历上百个子键），所以放在最后；
    // 只有在打开设置窗口时才走，右键菜单绝不碰。
    {
        for (const std::wstring& appName : ReadSubKeyNames(HKEY_CLASSES_ROOT, L"Applications"))
        {
            const std::wstring supportedTypesKey =
                L"Applications\\" + appName + L"\\SupportedTypes";

            // 值名就是扩展名，值内容一般是空的
            std::vector<std::wstring> types = ReadValueNames(HKEY_CLASSES_ROOT, supportedTypesKey);

            bool supports = false;
            for (const std::wstring& type : types)
            {
                if (::_wcsicmp(type.c_str(), extension.c_str()) == 0)
                {
                    supports = true;
                    break;
                }
            }

            if (!supports)
            {
                continue;
            }

            core::OpenWithItem item;
            if (BuildFromExeName(appName, item))
            {
                AddUnique(list, seen, std::move(item));
            }
        }
    }

    Logger::Info(L"打开方式候选：扩展名=" + extension + L"，共 " +
                 std::to_wstring(list.size()) + L" 个程序");

    return list;
}

// ============================================================================
// 系统当前默认程序
// ============================================================================

bool QuerySystemDefault(const std::wstring& extension, core::OpenWithItem& out)
{
    out = core::OpenWithItem();

    if (extension.empty() || extension[0] != L'.')
    {
        return false;
    }

    const std::wstring userChoiceKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
        extension + L"\\UserChoice";

    std::wstring progId = ReadRegValue(HKEY_CURRENT_USER, userChoiceKey, L"ProgId");

    // 没有 UserChoice 就退一步用 HKCR\<扩展名> 的默认 ProgID
    if (progId.empty())
    {
        progId = ReadRegValue(HKEY_CLASSES_ROOT, extension, L"");
    }

    return BuildFromProgId(progId, out);
}

// ============================================================================
// 程序还在不在
// ============================================================================

bool IsAvailable(const core::OpenWithItem& program, bool& diskMissing)
{
    diskMissing = false;

    std::wstring probePath;

    if (program.kind == L"uwp")
    {
        if (program.appId.empty())
        {
            return false;
        }

        // 商店应用统一从「所有应用」这个视图里找它
        probePath = L"shell:AppsFolder\\" + program.appId;
    }
    else
    {
        if (program.path.empty())
        {
            return false;
        }

        probePath = program.path;

        // 网络路径一律不探测：断网时这条调用会卡十几秒，界面上看起来像死机。
        // 当作"未知但可用"，真的打开失败时再报错。
        if (probePath.size() >= 2 && probePath[0] == L'\\' && probePath[1] == L'\\')
        {
            return true;
        }

        // 盘符本身都不在了 → 是磁盘没连（U 盘拔了、移动硬盘没插），不是卸载
        if (probePath.size() >= 2 && probePath[1] == L':')
        {
            const std::wstring root = probePath.substr(0, 2) + L"\\";
            const UINT driveType = ::GetDriveTypeW(root.c_str());
            if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN)
            {
                diskMissing = true;
                return false;
            }
        }
    }

    // 用 shell 解析：Win32 传 exe 路径，商店应用传 shell:AppsFolder\<AUMID>。
    // 在 → S_OK；不在 → 0x80070002（实测过的，见文档）。
    // 好处是绕开了 WindowsApps 目录的读权限限制（那里面普通用户读不了）。
    IShellItem* item = nullptr;
    const HRESULT result = ::SHCreateItemFromParsingName(probePath.c_str(), nullptr,
                                                         IID_PPV_ARGS(&item));
    if (item != nullptr)
    {
        item->Release();
    }

    return SUCCEEDED(result);
}

// ============================================================================
// 启动
// ============================================================================

bool Launch(HWND owner,
            const core::OpenWithItem& program,
            const std::wstring& filePath,
            std::wstring& error)
{
    error.clear();

    if (filePath.empty())
    {
        error = L"文件路径是空的";
        return false;
    }

    // ---------------------------------------------------------------
    // 商店应用：交给系统按"我们指定的 ProgID"启动
    //
    // 为什么不直接 CreateProcess WindowsApps 里的 exe：
    //   那些是打包应用，离开包身份直接跑会缺权限、缺运行环境。
    //
    // 为什么不用 IApplicationActivationManager::ActivateForFile：
    //   实测在本机（Win11）对这类"打包桌面应用"（商店版记事本、照片）
    //   一律返回 0x80270254，文件根本打不开（换成 MTA 线程也一样）。
    //   系统自己双击这类文件并不走它，而是走 ProgID 上登记的
    //   DelegateExecute 处理器。
    //
    // 所以这里照系统的路子办：ShellExecuteEx 指明 lpClass = ProgID，
    // 系统会去读 HKCR\<ProgID>\shell\open\command 里的 DelegateExecute，
    // 再按该 ProgID 自带的 AppUserModelID 把文件交给商店应用。
    //
    // 关键点：lpClass 是我们自己记账本里的 ProgID，不是留空，
    // 所以系统"当前默认关联"改成什么都影响不到这里——依然不被劫持。
    // ---------------------------------------------------------------
    if (program.kind == L"uwp")
    {
        if (program.progId.empty())
        {
            error = L"这个商店应用缺少类型标识，请重新设置打开方式";
            return false;
        }

        SHELLEXECUTEINFOW info = {};
        info.cbSize   = sizeof(info);
        info.fMask    = SEE_MASK_CLASSNAME;   // 告诉系统"按我给的类名找动词"
        info.hwnd     = owner;
        info.lpVerb   = L"open";
        info.lpFile   = filePath.c_str();
        info.lpClass  = program.progId.c_str();
        info.nShow    = SW_SHOWNORMAL;

        if (!::ShellExecuteExW(&info))
        {
            const DWORD code = ::GetLastError();

            wchar_t codeText[32] = {};
            ::swprintf_s(codeText, L"0x%08X", static_cast<unsigned int>(code));

            error = L"商店应用启动失败（可能已被卸载或没登记这类文件），错误码 " +
                    std::wstring(codeText);

            Logger::Warn(L"商店应用启动失败：" + program.name + L"，ProgID=" + program.progId +
                         L"，错误码 " + std::wstring(codeText));

            return false;
        }

        Logger::Info(L"打开文件（商店应用）：" + program.name + L"，文件=" + filePath);
        return true;
    }

    // ---------------------------------------------------------------
    // 普通 Win32 程序：CreateProcess
    //
    // 不走 ShellExecute，是为了彻底绕开系统关联（ShellExecute 会按注册表
    // 里当前登记的默认程序开，那就等于又被系统劫持回去了）。
    // ---------------------------------------------------------------
    if (program.path.empty())
    {
        error = L"这个程序的路径丢了，请重新设置";
        return false;
    }

    // 工作目录设成 exe 所在目录：不少程序（尤其 Electron 系）从工作目录
    // 读自己的资源文件，用文件管理器自己的目录会出怪问题
    std::wstring workingDirectory = program.path;
    const size_t lastSlash = workingDirectory.find_last_of(L'\\');
    if (lastSlash != std::wstring::npos)
    {
        workingDirectory = workingDirectory.substr(0, lastSlash);
    }
    else
    {
        workingDirectory.clear();
    }

    const std::wstring arguments = BuildArguments(program, filePath);

    std::wstring commandLine = L"\"" + program.path + L"\"";
    if (!arguments.empty())
    {
        commandLine += L" ";
        commandLine += arguments;
    }

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo = {};

    const BOOL started = ::CreateProcessW(nullptr,
                                          const_cast<LPWSTR>(commandLine.c_str()),
                                          nullptr, nullptr, FALSE,
                                          0,
                                          nullptr,
                                          workingDirectory.empty() ? nullptr
                                                                   : workingDirectory.c_str(),
                                          &startupInfo, &processInfo);

    if (!started)
    {
        error = L"程序启动失败（可能已被卸载，或者没有启动权限）";
        Logger::Warn(L"启动程序失败：" + program.name + L"，命令行=" + commandLine);
        return false;
    }

    ::CloseHandle(processInfo.hThread);
    ::CloseHandle(processInfo.hProcess);

    Logger::Info(L"打开文件：" + program.name + L"，文件=" + filePath);
    return true;
}

bool LaunchWithSystemDefault(HWND owner, const std::wstring& filePath)
{
    if (filePath.empty())
    {
        return false;
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize    = sizeof(info);
    info.fMask     = SEE_MASK_NOASYNC;
    info.lpVerb    = L"open";
    info.lpFile    = filePath.c_str();
    info.nShow     = SW_SHOWNORMAL;
    info.hwnd      = owner;

    if (!::ShellExecuteExW(&info))
    {
        Logger::Warn(L"按系统默认打开失败：" + filePath);
        return false;
    }

    return true;
}

// ============================================================================
// 图标
// ============================================================================

int IconIndexFor(const core::OpenWithItem& program,
                 IconCache& icons,
                 IconCache::IconSize size)
{
    if (program.kind == L"uwp")
    {
        if (program.appId.empty())
        {
            return -1;
        }

        const std::wstring probePath = L"shell:AppsFolder\\" + program.appId;

        wil::com_ptr<IShellItem> item;
        if (FAILED(::SHCreateItemFromParsingName(probePath.c_str(), nullptr,
                                                IID_PPV_ARGS(&item))) ||
            item == nullptr)
        {
            return -1;
        }

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(::SHGetIDListFromObject(item.get(), &pidl)) || pidl == nullptr)
        {
            return -1;
        }

        // 把 PIDL 拷成字节数组（IconCache 的接口要的是这个形态）
        const int byteCount = ::ILGetSize(pidl);
        std::vector<unsigned char> bytes(static_cast<size_t>(byteCount));
        ::memcpy(bytes.data(), pidl, static_cast<size_t>(byteCount));
        ::CoTaskMemFree(pidl);

        return icons.GetIconIndexByPidl(bytes, size);
    }

    if (!program.path.empty())
    {
        return icons.GetIconIndexByFullPath(program.path, size);
    }

    return -1;
}

} // namespace shell
