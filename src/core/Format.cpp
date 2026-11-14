// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/Format.cpp
//
// 显示格式化实现。
// ============================================================================

#include "Format.h"

#include <cwctype>

namespace format
{

std::wstring FileTimeText(const FILETIME& utcFileTime)
{
    if (utcFileTime.dwLowDateTime == 0 && utcFileTime.dwHighDateTime == 0)
    {
        return L"-";
    }

    // UTC 转本地时间，再转系统时间结构
    FILETIME localFileTime = {};
    if (!::FileTimeToLocalFileTime(&utcFileTime, &localFileTime))
    {
        return L"-";
    }

    SYSTEMTIME st = {};
    if (!::FileTimeToSystemTime(&localFileTime, &st))
    {
        return L"-";
    }

    wchar_t buffer[64] = {};
    ::swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return std::wstring(buffer);
}

std::wstring SizeText(uint64_t size, bool isFolder)
{
    if (isFolder)
    {
        return L"文件夹";
    }

    wchar_t buffer[64] = {};

    if (size < 1024ULL)
    {
        ::swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(size));
    }
    else if (size < 1024ULL * 1024ULL)
    {
        ::swprintf_s(buffer, L"%.1f KB", static_cast<double>(size) / 1024.0);
    }
    else if (size < 1024ULL * 1024ULL * 1024ULL)
    {
        ::swprintf_s(buffer, L"%.1f MB", static_cast<double>(size) / (1024.0 * 1024.0));
    }
    else
    {
        ::swprintf_s(buffer, L"%.2f GB", static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0));
    }

    return std::wstring(buffer);
}

std::wstring TypeText(const std::wstring& name, bool isFolder)
{
    if (isFolder)
    {
        return L"文件夹";
    }

    // 取最后一个点号后的扩展名
    size_t dotPos = name.find_last_of(L'.');
    if (dotPos == std::wstring::npos || dotPos == 0 || dotPos + 1 >= name.size())
    {
        return L"文件";
    }

    std::wstring extension = name.substr(dotPos + 1);
    for (auto& ch : extension)
    {
        ch = static_cast<wchar_t>(::towupper(ch));
    }
    return extension + L" 文件";
}

} // namespace format
