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

    // 单位按 1024 逐级升：B → KB → MB → GB → TB → PB → EB。
    // 用户要求"超过 1024M 就显示 1G、超过 1024G 就显示 1T"，所以这里统一用
    // 乘除 1024 的方式一级级往上走，绝不在某个单位上一直堆大数字。
    static const wchar_t* const kUnits[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB", L"EB" };

    double value   = static_cast<double>(size);
    int    unitIdx = 0;

    while (value >= 1024.0 && unitIdx < 6)
    {
        value /= 1024.0;
        ++unitIdx;
    }

    wchar_t buffer[64] = {};

    if (unitIdx == 0)
    {
        ::swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(size));
    }
    else
    {
        // 大单位保留两位小数，小单位一位就够了（和原来列表里的观感一致）
        const int decimals = (unitIdx >= 3) ? 2 : 1;
        ::swprintf_s(buffer, L"%.*f %s", decimals, value, kUnits[unitIdx]);
    }

    return std::wstring(buffer);
}

// ----------------------------------------------------------------------------
// 磁盘容量文本：同一个 1024 进位规则，按 3 位有效数字写
//   512 字节 → "512 字节"
//   97.7 GB  → "97.7 GB"
//   234 GB   → "234 GB"
//   3.63 TB  → "3.63 TB"
// ----------------------------------------------------------------------------
std::wstring CapacityText(uint64_t bytes)
{
    static const wchar_t* const kUnits[] = { L"KB", L"MB", L"GB", L"TB", L"PB", L"EB" };

    // 不足 1 KB 的容量（U 盘一般不会这样，但别写错）直接按字节说
    if (bytes < 1024ULL)
    {
        wchar_t small[64] = {};
        ::swprintf_s(small, L"%llu 字节", static_cast<unsigned long long>(bytes));
        return std::wstring(small);
    }

    double value   = static_cast<double>(bytes) / 1024.0;
    int    unitIdx = 0;

    while (value >= 1024.0 && unitIdx < 5)
    {
        value /= 1024.0;
        ++unitIdx;
    }

    wchar_t buffer[64] = {};

    // 3 位有效数字：>=100 不带小数；>=10 一位；其余两位
    if (value >= 100.0)
    {
        ::swprintf_s(buffer, L"%.0f %s", value, kUnits[unitIdx]);
    }
    else if (value >= 10.0)
    {
        ::swprintf_s(buffer, L"%.1f %s", value, kUnits[unitIdx]);
    }
    else
    {
        ::swprintf_s(buffer, L"%.2f %s", value, kUnits[unitIdx]);
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
