// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/NavigationHistory.h
//
// 导航历史（用两个栈实现，和浏览器 / 资源管理器行为一致）。
//
// 行为约定：
//   前进/后退“回到”某路径时，不产生新的历史记录（否则会死循环式膨胀）；
//   只有用户主动跳转（地址栏回车、双击进目录、点向上）才压入后退栈。
// ============================================================================

#pragma once

#include <string>
#include <vector>

class NavigationHistory
{
public:
    // 重置到某个初始路径（清空两侧历史）
    void Reset(const std::wstring& path)
    {
        m_current = path;
        m_backStack.clear();
        m_forwardStack.clear();
    }

    // 用户主动跳转到新路径：当前路径压入后退栈，前进栈清空
    void NavigateTo(const std::wstring& path)
    {
        if (path == m_current)
        {
            return;   // 同路径不重复记录
        }
        if (!m_current.empty())
        {
            m_backStack.push_back(m_current);
        }
        m_forwardStack.clear();
        m_current = path;
    }

    bool CanGoBack() const { return !m_backStack.empty(); }
    bool CanGoForward() const { return !m_forwardStack.empty(); }

    // 后退：当前路径进前进栈，从后退栈取一条
    std::wstring GoBack()
    {
        if (m_backStack.empty())
        {
            return m_current;
        }
        m_forwardStack.push_back(m_current);
        m_current = m_backStack.back();
        m_backStack.pop_back();
        return m_current;
    }

    std::wstring GoForward()
    {
        if (m_forwardStack.empty())
        {
            return m_current;
        }
        m_backStack.push_back(m_current);
        m_current = m_forwardStack.back();
        m_forwardStack.pop_back();
        return m_current;
    }

    const std::wstring& Current() const { return m_current; }

    // 只更新当前路径（例如枚举失败后同步）
    void SetCurrent(const std::wstring& path) { m_current = path; }

    // ------------------------------------------------------------------
    // 取父目录路径
    //   "C:\\Windows\\Boot" → "C:\\Windows"
    //   "C:\\Windows"       → "C:\\"
    //   "C:\\"              → "C:\\"（根目录没有上级）
    // ------------------------------------------------------------------
    static std::wstring ParentPath(const std::wstring& path)
    {
        std::wstring trimmed = path;

        // 去掉末尾多余的反斜杠（但保留 "C:\" 这种根路径的形态）
        while (trimmed.size() > 3 && trimmed.back() == L'\\')
        {
            trimmed.pop_back();
        }

        // 已经是根目录（C:\ 或 \\server）
        if (trimmed.size() <= 3 && trimmed.size() >= 2 && trimmed[1] == L':')
        {
            return trimmed;
        }

        size_t lastSlash = trimmed.find_last_of(L'\\');
        if (lastSlash == std::wstring::npos)
        {
            return trimmed;
        }

        // "C:\Windows" → 上级是 "C:\"，长度保留 3
        if (lastSlash == 2 && trimmed.size() > 2 && trimmed[1] == L':')
        {
            return trimmed.substr(0, 3);
        }

        if (lastSlash == 0)
        {
            return trimmed;
        }

        return trimmed.substr(0, lastSlash);
    }

private:
    std::wstring              m_current;
    std::vector<std::wstring> m_backStack;
    std::vector<std::wstring> m_forwardStack;
};
