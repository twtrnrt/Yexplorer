// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/DefaultManager.cpp
//
// 「注册为默认文件管理器」的实现。说明见 DefaultManager.h。
//
// 【注册表动的是哪两个键】
//   HKCU\Software\Classes\Directory\shell\open\command   ← 双击文件夹
//   HKCU\Software\Classes\Drive\shell\open\command       ← 双击磁盘
//   两个都写成：  "本程序全路径" "%1"
//   注意必须带引号：路径里有空格（C:\Program Files\...）时，不带引号
//   命令行会被拆成好几段，程序收到的路径就是错的。
//
// 【原值备份在哪】
//   HKCU\Software\FileExplorer\DefaultFileManager
//     directoryExisted  DWORD  1=原来有值 0=原来没有
//     directoryCommand  SZ     原来的命令串（existed=1 时才有）
//     driveExisted / driveCommand  同上，对应磁盘
//
//   备份只写一次：如果备份已经存在，再注册一次也不会覆盖它。
//   这样即使先注册 Debug 版、再注册 Release 版，恢复时也还能回到
//   "第一次注册之前"那个原值，而不是回到 Debug 版的路径。
// ============================================================================

#include "shell/DefaultManager.h"

#include "core/Logger.h"

#include <shlobj.h>    // SHChangeNotify / SHCNE_ASSOCCHANGED
#include <string>

namespace
{

// 我们要接管的两个类。每个类都要动两个地方：
//   1) shell 键的「默认动词」——必须设成 "open"
//   2) shell\open\command 的默认值 —— 必须写成我们的 exe
//
// 【第 1 步才是关键，缺了它怎么改都没用】
//   Win10 里 HKLM 的 Directory\shell、Drive\shell 默认动词都是 "none"，
//   意思是"这类东西没有默认动作，交给 Folder 类（explorer.exe 的
//   DelegateExecute）去打开"。所以只改 command 完全不起作用 ——
//   实测双击文件夹照样开系统资源管理器。
//   把默认动词设成 "open" 之后，shell 才会真的来读 open\command。
const wchar_t* kDirectoryShellKey   = L"Software\\Classes\\Directory\\shell";
const wchar_t* kDirectoryOpenKey    = L"Software\\Classes\\Directory\\shell\\open";
const wchar_t* kDirectoryCommandKey = L"Software\\Classes\\Directory\\shell\\open\\command";

const wchar_t* kDriveShellKey   = L"Software\\Classes\\Drive\\shell";
const wchar_t* kDriveOpenKey    = L"Software\\Classes\\Drive\\shell\\open";
const wchar_t* kDriveCommandKey = L"Software\\Classes\\Drive\\shell\\open\\command";

// 备份放这里（自己的地盘，不跟系统键混在一起）
const wchar_t* kBackupKey = L"Software\\FileExplorer\\DefaultFileManager";

// 备份项：每个值都记"原来有没有 + 原来是什么"
const wchar_t* kValueDirectoryShellExisted   = L"directoryShellExisted";
const wchar_t* kValueDirectoryShellDefault   = L"directoryShellDefault";
const wchar_t* kValueDirectoryCommandExisted = L"directoryCommandExisted";
const wchar_t* kValueDirectoryCommand        = L"directoryCommand";

const wchar_t* kValueDriveShellExisted   = L"driveShellExisted";
const wchar_t* kValueDriveShellDefault   = L"driveShellDefault";
const wchar_t* kValueDriveCommandExisted = L"driveCommandExisted";
const wchar_t* kValueDriveCommand        = L"driveCommand";

// 本程序 exe 的全路径
std::wstring CurrentExePath()
{
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer,
                                              static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));

    if (length == 0)
    {
        return std::wstring();
    }

    return std::wstring(buffer, length);
}

// 我们期望写进注册表的那条命令："exe全路径" "%1"
std::wstring ExpectedCommand()
{
    const std::wstring exePath = CurrentExePath();
    if (exePath.empty())
    {
        return std::wstring();
    }

    return L"\"" + exePath + L"\" \"%1\"";
}

// ----------------------------------------------------------------------------
// 读写注册表的小工具
// valueName 传 nullptr 表示那个键的"默认值"
// ----------------------------------------------------------------------------

bool ReadString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                std::wstring& value, DWORD& type)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD size      = 0;
    DWORD valueType = 0;
    LONG  result    = ::RegQueryValueExW(key, valueName, nullptr, &valueType, nullptr, &size);

    if (result != ERROR_SUCCESS || size == 0 ||
        (valueType != REG_SZ && valueType != REG_EXPAND_SZ))
    {
        ::RegCloseKey(key);
        return false;
    }

    std::wstring buffer(size / sizeof(wchar_t), L'\0');

    result = ::RegQueryValueExW(key, valueName, nullptr, &valueType,
                                reinterpret_cast<LPBYTE>(buffer.data()), &size);

    ::RegCloseKey(key);

    if (result != ERROR_SUCCESS)
    {
        return false;
    }

    buffer.resize(size / sizeof(wchar_t));

    // 值结尾带一个 '\0'，去掉再比较/写回
    while (!buffer.empty() && buffer.back() == L'\0')
    {
        buffer.pop_back();
    }

    value = buffer;
    type  = valueType;
    return true;
}

bool WriteString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                 const std::wstring& value, DWORD type)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                          &key, nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    const LONG result = ::RegSetValueExW(key, valueName, 0, type,
                                         reinterpret_cast<const BYTE*>(value.c_str()),
                                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));

    ::RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool ReadDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD& value)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD size      = sizeof(DWORD);
    DWORD type      = 0;
    DWORD data      = 0;

    const LONG result = ::RegQueryValueExW(key, valueName, nullptr, &type,
                                           reinterpret_cast<LPBYTE>(&data), &size);

    ::RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_DWORD)
    {
        return false;
    }

    value = data;
    return true;
}

bool WriteDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                          &key, nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    const LONG result = ::RegSetValueExW(key, valueName, 0, REG_DWORD,
                                         reinterpret_cast<const BYTE*>(&value), sizeof(value));

    ::RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// 整个子键删掉（键不存在也算成功）
bool DeleteKey(HKEY root, const wchar_t* subKey)
{
    HKEY key = nullptr;

    if (::RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return true;   // 本来就没有
    }

    ::RegCloseKey(key);
    return (::RegDeleteTreeW(root, subKey) == ERROR_SUCCESS);
}

// 只删一个键的"默认值"（键本身留着）。
// 恢复 shell 默认动词时用：键可能还有别的值/子键，不能整棵砍。
bool RemoveDefaultValue(HKEY root, const wchar_t* subKey)
{
    HKEY key = nullptr;

    if (::RegOpenKeyExW(root, subKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
    {
        return true;   // 键都没有，当然没有值
    }

    const LONG result = ::RegDeleteValueW(key, nullptr);
    ::RegCloseKey(key);

    // ERROR_FILE_NOT_FOUND = 本来就没有这个值，也算成功
    return (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND);
}

// 键已经是空壳（没有子键、也没有值）才删掉，有内容就原样留着。
// 恢复时用：我们建的空键别留在用户注册表里。
bool DeleteKeyIfEmpty(HKEY root, const wchar_t* subKey)
{
    HKEY key = nullptr;

    if (::RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return true;   // 本来就没有
    }

    DWORD subKeyCount = 0;
    DWORD valueCount  = 0;

    ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr,
                       &subKeyCount, nullptr, nullptr,
                       &valueCount, nullptr, nullptr, nullptr, nullptr);

    ::RegCloseKey(key);

    if (subKeyCount != 0 || valueCount != 0)
    {
        return true;   // 还有东西，不是空壳，不能删
    }

    return (::RegDeleteTreeW(root, subKey) == ERROR_SUCCESS);
}

// 备份是不是已经写过了（写过就不再覆盖）
bool BackupExists()
{
    DWORD value = 0;
    return ReadDword(HKEY_CURRENT_USER, kBackupKey, kValueDirectoryShellExisted, value);
}

// ----------------------------------------------------------------------------
// 备份 / 还原"一个键的默认值"这一组动作
// 每个值都要记两件事：原来有没有（existed）、原来是什么（text）
// ----------------------------------------------------------------------------
void BackupValue(HKEY root, const wchar_t* subKey,
                 const wchar_t* existedName, const wchar_t* textName)
{
    std::wstring text;
    DWORD        type      = REG_SZ;
    const bool   existed   = ReadString(root, subKey, nullptr, text, type);

    WriteDword(HKEY_CURRENT_USER, kBackupKey, existedName, existed ? 1 : 0);

    if (existed)
    {
        WriteString(HKEY_CURRENT_USER, kBackupKey, textName, text, REG_SZ);
    }
}

bool RestoreValue(HKEY root, const wchar_t* subKey,
                  const wchar_t* existedName, const wchar_t* textName)
{
    DWORD existed = 0;

    if (!ReadDword(HKEY_CURRENT_USER, kBackupKey, existedName, existed) || existed == 0)
    {
        // 原来没有这个值 → 删掉我们写的，让系统回落 HKLM
        return RemoveDefaultValue(root, subKey);
    }

    std::wstring text;
    DWORD        type = REG_SZ;

    if (!ReadString(HKEY_CURRENT_USER, kBackupKey, textName, text, type))
    {
        return false;
    }

    // 系统原来那条一般是 REG_EXPAND_SZ（%SystemRoot%\Explorer.exe），
    // 按原类型写回，否则 %SystemRoot% 不会被展开
    const DWORD writeType = (type == REG_EXPAND_SZ) ? REG_EXPAND_SZ : REG_SZ;

    return WriteString(root, subKey, nullptr, text, writeType);
}

} // 匿名命名空间

namespace shell
{

bool IsDefaultFileManager()
{
    const std::wstring expected = ExpectedCommand();
    if (expected.empty())
    {
        return false;
    }

    // 两个条件都满足才算"已注册"：
    //   1) Directory\shell 的默认动词是 open（否则我写的命令根本没被使用）
    //   2) Directory\shell\open\command 是本程序
    std::wstring shellVerb;
    DWORD        shellType = REG_SZ;

    if (!ReadString(HKEY_CURRENT_USER, kDirectoryShellKey, nullptr, shellVerb, shellType))
    {
        return false;
    }

    if (_wcsicmp(shellVerb.c_str(), L"open") != 0)
    {
        return false;
    }

    std::wstring command;
    DWORD        type = 0;

    if (!ReadString(HKEY_CURRENT_USER, kDirectoryCommandKey, nullptr, command, type))
    {
        return false;
    }

    // 路径大小写不敏感
    return (_wcsicmp(command.c_str(), expected.c_str()) == 0);
}

bool RegisterDefaultFileManager()
{
    const std::wstring expected = ExpectedCommand();

    if (expected.empty())
    {
        Logger::Error(L"注册默认文件管理器失败：取不到本程序路径");
        return false;
    }

    // 已经是默认（当前 exe 就是）就不用重复写
    if (IsDefaultFileManager())
    {
        Logger::Info(L"注册默认文件管理器：本来就是，不用重写");
        return true;
    }

    // ---- 第一次注册时把"原状"备份下来 ----
    // 备份只写一次：先注册 Debug 版、再注册 Release 版，
    // 也还能回到"第一次注册之前"那个原值
    if (!BackupExists())
    {
        BackupValue(HKEY_CURRENT_USER, kDirectoryShellKey,
                    kValueDirectoryShellExisted, kValueDirectoryShellDefault);

        BackupValue(HKEY_CURRENT_USER, kDirectoryCommandKey,
                    kValueDirectoryCommandExisted, kValueDirectoryCommand);

        BackupValue(HKEY_CURRENT_USER, kDriveShellKey,
                    kValueDriveShellExisted, kValueDriveShellDefault);

        BackupValue(HKEY_CURRENT_USER, kDriveCommandKey,
                    kValueDriveCommandExisted, kValueDriveCommand);
    }

    // ---- 写四项：两个类的默认动词 + 两个类的 open 命令 ----
    bool ok = true;

    ok = WriteString(HKEY_CURRENT_USER, kDirectoryShellKey, nullptr, L"open", REG_SZ) && ok;
    ok = WriteString(HKEY_CURRENT_USER, kDirectoryCommandKey, nullptr, expected, REG_SZ) && ok;

    ok = WriteString(HKEY_CURRENT_USER, kDriveShellKey, nullptr, L"open", REG_SZ) && ok;
    ok = WriteString(HKEY_CURRENT_USER, kDriveCommandKey, nullptr, expected, REG_SZ) && ok;

    if (!ok)
    {
        Logger::Error(L"注册默认文件管理器失败：写注册表失败（可能是权限或键被占用）");
        return false;
    }

    // 让 shell 立刻丢掉旧的关联缓存
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    Logger::Info(L"已注册为默认文件管理器：" + expected);
    return true;
}

bool RestoreSystemFileManager()
{
    bool ok = true;

    // ---- 文件夹 ----
    ok = RestoreValue(HKEY_CURRENT_USER, kDirectoryCommandKey,
                      kValueDirectoryCommandExisted, kValueDirectoryCommand) && ok;

    ok = RestoreValue(HKEY_CURRENT_USER, kDirectoryShellKey,
                      kValueDirectoryShellExisted, kValueDirectoryShellDefault) && ok;

    // ---- 磁盘 ----
    ok = RestoreValue(HKEY_CURRENT_USER, kDriveCommandKey,
                      kValueDriveCommandExisted, kValueDriveCommand) && ok;

    ok = RestoreValue(HKEY_CURRENT_USER, kDriveShellKey,
                      kValueDriveShellExisted, kValueDriveShellDefault) && ok;

    // ---- 原来是"没有"的，把删完值之后剩下的空壳一层层收掉 ----
    // （不然用户的 HKCU\Software\Classes 里会永远躺着几个空键）
    DWORD directoryCommandExisted = 0;
    DWORD directoryShellExisted   = 0;
    DWORD driveCommandExisted     = 0;
    DWORD driveShellExisted       = 0;

    ReadDword(HKEY_CURRENT_USER, kBackupKey, kValueDirectoryCommandExisted, directoryCommandExisted);
    ReadDword(HKEY_CURRENT_USER, kBackupKey, kValueDirectoryShellExisted, directoryShellExisted);
    ReadDword(HKEY_CURRENT_USER, kBackupKey, kValueDriveCommandExisted, driveCommandExisted);
    ReadDword(HKEY_CURRENT_USER, kBackupKey, kValueDriveShellExisted, driveShellExisted);

    if (directoryCommandExisted == 0)
    {
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, kDirectoryCommandKey);
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, kDirectoryOpenKey);
    }
    if (directoryShellExisted == 0)
    {
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, kDirectoryShellKey);
    }

    if (driveCommandExisted == 0)
    {
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, kDriveCommandKey);
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, kDriveOpenKey);
    }
    if (driveShellExisted == 0)
    {
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, kDriveShellKey);
    }

    // 备份用完就扔掉：下次再注册时重新按当时的原状备份
    ::RegDeleteTreeW(HKEY_CURRENT_USER, kBackupKey);

    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (ok)
    {
        Logger::Info(L"已恢复系统默认文件管理器");
    }
    else
    {
        Logger::Error(L"恢复系统默认文件管理器：有键写回失败，详见上方日志");
    }

    return ok;
}

} // namespace shell
