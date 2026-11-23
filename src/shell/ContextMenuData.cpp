// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuData.cpp
//
// 共享数据模型的辅助函数实现（场景标签文字等）。
// ============================================================================

#include "shell/ContextMenuData.h"

#include <cwctype>

namespace shell
{

std::wstring ScenesToText(unsigned int scenes)
{
    if (scenes == SceneNone)
    {
        return L"—";
    }

    std::wstring result;
    const struct
    {
        unsigned int bit;
        const wchar_t* label;
    } labels[] =
    {
        { SceneFile,       L"文件" },
        { SceneFolder,     L"文件夹" },
        { SceneMultiple,   L"多选" },
        { SceneBackground, L"背景" },
    };

    for (const auto& one : labels)
    {
        if ((scenes & one.bit) != 0)
        {
            if (!result.empty())
            {
                result += L" / ";
            }
            result += one.label;
        }
    }

    return result.empty() ? L"—" : result;
}

std::wstring SourceLabelToText(bool fromRegistry, bool fromExtension)
{
    if (fromRegistry && fromExtension)
    {
        return L"注册表+扩展";
    }
    if (fromRegistry)
    {
        return L"注册表";
    }
    if (fromExtension)
    {
        return L"扩展";
    }
    return std::wstring();
}

namespace
{

// 小写比较用（Windows 文字不分大小写）
std::wstring LowerText(const std::wstring& text)
{
    std::wstring lowered;
    lowered.reserve(text.size());
    for (wchar_t ch : text)
    {
        lowered.push_back(static_cast<wchar_t>(::towlower(ch)));
    }
    return lowered;
}

// 去掉首尾空白和助记符 '&'
std::wstring NormalizeMenuText(const std::wstring& text)
{
    std::wstring out;
    for (wchar_t ch : text)
    {
        if (ch == L'&')
        {
            continue;
        }
        out.push_back(ch);
    }
    size_t begin = out.find_first_not_of(L" \t");
    size_t end   = out.find_last_not_of(L" \t");
    if (begin == std::wstring::npos)
    {
        return std::wstring();
    }
    return out.substr(begin, end - begin + 1);
}

} // namespace

bool IsSystemBuiltinItem(const std::wstring& text, const std::wstring& verb)
{
    // 1) 动词名白名单：这些是资源管理器自己注册的动词
    static const wchar_t* systemVerbs[] =
    {
        L"open", L"opennew", L"print", L"printto", L"copy", L"cut", L"paste",
        L"delet", L"delete", L"rename", L"properties", L"sendto", L"new",
        L"pintostart", L"pintostartscreen", L"unpinfromstart",
        L"copyaspath", L"pin", L"unpin", L"previousversions",
        L"runas", L"runasuser", L"edit", L"share", L"giveaccess",
    };

    if (!verb.empty())
    {
        const std::wstring loweredVerb = LowerText(verb);
        for (const wchar_t* one : systemVerbs)
        {
            if (loweredVerb == one)
            {
                return true;
            }
        }
    }

    // 2) 菜单文字白名单（中英混排，简繁两种都列）
    static const wchar_t* systemTexts[] =
    {
        L"打开", L"打开方式", L"在新窗口中打开", L"打印", L"编辑",
        L"复制", L"剪切", L"粘贴", L"粘贴快捷方式", L"删除", L"重命名",
        L"属性", L"发送到", L"新建", L"创建快捷方式",
        L"复制路径", L"共享", L"授予访问权限", L"还原以前的版本",
        L"固定到\"快速访问\"", L"固定到快速访问", L"从\"快速访问\"中取消固定",
        L"固定到\"开始\"屏幕", L"固定到开始屏幕", L"从开始屏幕取消固定",
        L"固定到任务栏", L"从任务栏取消固定", L"以管理员身份运行",
        L"Open", L"Open with", L"Print", L"Edit", L"Copy", L"Cut", L"Paste",
        L"Delete", L"Rename", L"Properties", L"Send to", L"New", L"Share",
        L"Copy as path", L"Restore previous versions", L"Create shortcut",
    };

    const std::wstring normalized = NormalizeMenuText(text);
    if (normalized.empty())
    {
        return false;
    }
    const std::wstring lowered = LowerText(normalized);
    for (const wchar_t* one : systemTexts)
    {
        if (lowered == LowerText(one))
        {
            return true;
        }
    }
    return false;
}

bool IsSystemBuiltinGroup(const std::wstring& groupName)
{
    static const wchar_t* systemGroups[] =
    {
        L"新建", L"发送到", L"共享", L"New", L"Send to", L"Share",
    };

    const std::wstring lowered = LowerText(NormalizeMenuText(groupName));
    for (const wchar_t* one : systemGroups)
    {
        if (lowered == LowerText(one))
        {
            return true;
        }
    }
    return false;
}

} // namespace shell
