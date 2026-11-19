// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/PathUtil.h
//
// 路径相关的公共小工具。
//
// 【为什么单独一个文件】
//   `MakeLongPath` 原来在 MainWnd.cpp 和 ShellClipboard.cpp 里各有一份，
//   逐字相同，改一处忘一处就会让长路径在不同功能里表现不一致
//   （批量重命名能改、粘贴却失败这种）。这里收成一份。
// ============================================================================

#pragma once

#include <string>

namespace pathutil
{

// 超过 260 字符的路径，普通 Win32 调用会失败，得换成 \\?\ 写法。
//   本地路径：C:\a\b            → \\?\C:\a\b
//   UNC 路径：\\server\share\x  → \\?\UNC\server\share\x
// 已经在 \\?\ 下的原样返回；小于 260 字符或超长的（>=32767）也原样返回。
inline std::wstring MakeLongPath(const std::wstring& path)
{
    if (path.size() < 260 || path.size() >= 32767)
    {
        return path;
    }

    if (path.compare(0, 4, L"\\\\?\\") == 0)
    {
        return path;
    }

    if (path.size() > 1 && path[1] == L':')
    {
        return L"\\\\?\\" + path;                       // C:\...
    }

    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        return L"\\\\?\\UNC\\" + path.substr(2);        // \\server\share
    }

    return path;   // 相对路径之类，原样交给系统
}

} // namespace pathutil
