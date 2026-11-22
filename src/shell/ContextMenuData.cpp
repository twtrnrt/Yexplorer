// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuData.cpp
//
// 共享数据模型的辅助函数实现（场景标签文字等）。
// ============================================================================

#include "shell/ContextMenuData.h"

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

} // namespace shell
