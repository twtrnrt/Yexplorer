// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ListaryIntegration.cpp
//
// 实现见头文件。
// ============================================================================

#include "ListaryIntegration.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <vector>

#include "core/Logger.h"
#include "../res/resource.h"

namespace shell
{

namespace
{

struct ByteBuffer
{
    const BYTE* data = nullptr;
    DWORD       size = 0;
};

// 从 exe 的内嵌资源里取一段二进制
bool LoadResourceBytes(int resourceId, ByteBuffer& out)
{
    HRSRC resource = ::FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resource == nullptr)
    {
        return false;
    }

    const DWORD size = ::SizeofResource(nullptr, resource);
    if (size == 0)
    {
        return false;
    }

    HGLOBAL loaded = ::LoadResource(nullptr, resource);
    if (loaded == nullptr)
    {
        return false;
    }

    const void* data = ::LockResource(loaded);
    if (data == nullptr)
    {
        return false;
    }

    out.data = static_cast<const BYTE*>(data);
    out.size = size;
    return true;
}

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& out)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024)
    {
        ::CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = out.empty() ||
                    (::ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                     read == out.size());
    ::CloseHandle(file);
    return ok;
}

bool WriteFileBytes(const std::wstring& path, const ByteBuffer& buffer)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    const bool ok = buffer.size == 0 ||
                    (::WriteFile(file, buffer.data, buffer.size, &written, nullptr) &&
                     written == buffer.size);
    ::CloseHandle(file);
    return ok;
}

bool SameContent(const std::wstring& path, const ByteBuffer& buffer)
{
    std::vector<BYTE> existing;
    if (!ReadFileBytes(path, existing))
    {
        return false;
    }
    return existing.size() == buffer.size &&
           (buffer.size == 0 || std::memcmp(existing.data(), buffer.data, buffer.size) == 0);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty())
    {
        return right;
    }
    std::wstring result = left;
    if (result.back() != L'\\' && result.back() != L'/')
    {
        result.push_back(L'\\');
    }
    result += right;
    return result;
}

std::wstring FindListaryDirectory()
{
    const wchar_t* bases[] = { L"ProgramFiles", L"ProgramFiles(x86)" };
    for (const wchar_t* base : bases)
    {
        wchar_t buffer[MAX_PATH * 2] = {};
        const DWORD length = ::GetEnvironmentVariableW(base, buffer, ARRAYSIZE(buffer));
        if (length == 0 || length >= ARRAYSIZE(buffer))
        {
            continue;
        }

        const std::wstring candidate = JoinPath(buffer, L"Listary");
        if (::GetFileAttributesW(JoinPath(candidate, L"Listary.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            return candidate;
        }
    }
    return std::wstring();
}

// 让正在运行的 Listary 重启，插件才会被重新扫描
void RestartListary(const std::wstring& listaryDirectory)
{
    std::vector<DWORD> pids;

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (::_wcsicmp(entry.szExeFile, L"Listary.exe") == 0)
                {
                    pids.push_back(entry.th32ProcessID);
                }
            } while (::Process32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
    }

    for (DWORD pid : pids)
    {
        HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (process == nullptr)
        {
            continue;
        }
        ::TerminateProcess(process, 0);
        ::WaitForSingleObject(process, 3000);
        ::CloseHandle(process);
    }

    if (!pids.empty())
    {
        ::Sleep(500);
    }

    const std::wstring exe = JoinPath(listaryDirectory, L"Listary.exe");
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    std::wstring command = L"\"" + exe + L"\"";

    if (::CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, 0,
                         nullptr, listaryDirectory.c_str(), &startup, &info))
    {
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        Logger::Info(L"Listary 已重启，插件生效");
    }
    else
    {
        Logger::Warn(L"Listary 重启失败（错误码 " + std::to_wstring(::GetLastError()) + L"）");
    }
}

} // namespace

bool IsListaryInstalled()
{
    return !FindListaryDirectory().empty();
}

int InstallListaryPlugin(bool restartListary)
{
    const std::wstring listary = FindListaryDirectory();
    if (listary.empty())
    {
        return 0;   // 没装 Listary，静默跳过
    }

    ByteBuffer dll = {};
    ByteBuffer manifest = {};
    ByteBuffer icon = {};
    if (!LoadResourceBytes(IDR_LISTARY_PLUGIN_DLL, dll) ||
        !LoadResourceBytes(IDR_LISTARY_PLUGIN_MANIFEST, manifest) ||
        !LoadResourceBytes(IDR_LISTARY_PLUGIN_ICON, icon))
    {
        Logger::Warn(L"Listary 插件：程序里没有内嵌资源，跳过");
        return -1;
    }

    const std::wstring pluginDirectory = JoinPath(JoinPath(listary, L"FileAppPlugins"), L"Yexplorer");
    const std::wstring dllPath      = JoinPath(pluginDirectory, L"Listary.FileAppPlugin.Yexplorer.dll");
    const std::wstring manifestPath = JoinPath(pluginDirectory, L"ListaryPluginManifest.json");
    const std::wstring iconPath     = JoinPath(pluginDirectory, L"Icon.png");

    // 已经是最新就不动，避免每次注册都重启 Listary
    if (SameContent(dllPath, dll) && SameContent(manifestPath, manifest) &&
        SameContent(iconPath, icon))
    {
        Logger::Info(L"Listary 插件已是最新，无需更新");
        return 0;
    }

    ::CreateDirectoryW(pluginDirectory.c_str(), nullptr);

    bool ok = WriteFileBytes(dllPath, dll) &&
              WriteFileBytes(manifestPath, manifest) &&
              WriteFileBytes(iconPath, icon);

    if (!ok)
    {
        Logger::Warn(L"Listary 插件：写入失败（错误码 " + std::to_wstring(::GetLastError()) + L"）");
        return -1;
    }

    // 从 Listary 自带的官方插件目录复制接口 DLL，避免重新分发 Listary 的文件
    const std::wstring reference = JoinPath(JoinPath(listary, L"FileAppPlugins"), L"Files");
    const wchar_t* dependencies[] = {
        L"Listary.FileAppPlugin.dll",
        L"Microsoft.Extensions.Logging.Abstractions.dll",
        L"System.Buffers.dll",
        L"System.Memory.dll",
        L"System.Numerics.Vectors.dll",
        L"System.Runtime.CompilerServices.Unsafe.dll",
    };

    for (const wchar_t* name : dependencies)
    {
        const std::wstring source = JoinPath(reference, name);
        if (::GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            ::CopyFileW(source.c_str(), JoinPath(pluginDirectory, name).c_str(), FALSE);
        }
    }

    Logger::Info(L"Listary 插件已安装：" + pluginDirectory);

    if (restartListary)
    {
        RestartListary(listary);
    }

    return 1;
}

} // namespace shell
