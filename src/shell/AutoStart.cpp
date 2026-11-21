// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/AutoStart.cpp
// ============================================================================

#include "shell/AutoStart.h"

#include "core/Logger.h"

#include <windows.h>

#include <string>

namespace
{

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kValueName = L"Yexplorer";

std::wstring ExpectedCommand()
{
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (length == 0 || length >= ARRAYSIZE(exe))
    {
        return std::wstring();
    }
    return L"\"" + std::wstring(exe, length) + L"\" --background";
}

std::wstring ReadRunValue()
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    DWORD type = 0;
    DWORD size = 0;
    LONG result = ::RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS || size == 0 || type != REG_SZ)
    {
        ::RegCloseKey(key);
        return std::wstring();
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    result = ::RegQueryValueExW(key, kValueName, nullptr, &type,
                                reinterpret_cast<LPBYTE>(value.data()), &size);
    ::RegCloseKey(key);

    if (result != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return value;
}

} // namespace

namespace shell
{

bool IsAutoStartEnabled()
{
    const std::wstring expected = ExpectedCommand();
    return !expected.empty() && ::_wcsicmp(ReadRunValue().c_str(), expected.c_str()) == 0;
}

bool SetAutoStart(bool enabled)
{
    if (!enabled)
    {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            return true;   // 没有这个键 = 本来就没启动项
        }
        const LONG result = ::RegDeleteValueW(key, kValueName);
        ::RegCloseKey(key);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }

    const std::wstring command = ExpectedCommand();
    if (command.empty())
    {
        return false;
    }

    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    const LONG result = ::RegSetValueExW(
        key, kValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));

    ::RegCloseKey(key);

    if (result == ERROR_SUCCESS)
    {
        Logger::Info(L"开机启动项已写入：" + command);
    }
    return result == ERROR_SUCCESS;
}

} // namespace shell
