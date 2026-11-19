// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/WindowPlacement.cpp
//
// 窗口位置计算。说明见 WindowPlacement.h。
// ============================================================================

#include "WindowPlacement.h"

#include "WindowRelay.h"   // ui::kMainWindowClassName

namespace ui
{

namespace
{

// 每开一个新窗口，相对"已有的那个窗口"往右下错开多少像素。
// 取 32 的理由：既能把下层窗口的标题栏露出来，又不会像 22 那样容易点错到下层窗口。
const int kCascadeOffset = 32;

// EnumWindows 的回调是 C 函数指针，带不了捕获，用这个结构把结果传回来
struct CascadeSearch
{
    RECT rect;      // 找到的那个窗口的"还原态"矩形
    bool found;
};

// EnumWindows 回调：找第一个（Z 序最上层）本程序主窗口，记下它的还原态矩形。
//
// 为什么用 GetWindowPlacement 的 rcNormalPosition，而不是 GetWindowRect：
// 基准窗口要是最大化状态，GetWindowRect 拿到的是整屏矩形，再往右下偏移必然出界，
// 级联就没意义了；rcNormalPosition 是它"还原"之后的位置，尺寸也更合理。
// 返回 FALSE 让 EnumWindows 立刻停下——第一个命中的就是最上层的那个，够用了。
BOOL CALLBACK CascadeEnumProc(HWND hwnd, LPARAM param)
{
    CascadeSearch* search = reinterpret_cast<CascadeSearch*>(param);

    // 只要看得见的窗口：隐藏的不参与级联
    if (!::IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    // 只认本程序的窗口类名，别的程序的窗口一概不参与
    wchar_t className[64] = {};
    if (::GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0)
    {
        return TRUE;
    }
    if (::_wcsicmp(className, kMainWindowClassName) != 0)
    {
        return TRUE;
    }

    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (::GetWindowPlacement(hwnd, &placement))
    {
        search->rect = placement.rcNormalPosition;
    }
    else
    {
        // 取不到放置信息时退回普通矩形，总比没有基准强
        ::GetWindowRect(hwnd, &search->rect);
    }

    search->found = true;
    return FALSE;
}

} // namespace

bool IsWindowRectVisible(const RECT& rect)
{
    return ::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr;
}

bool FindExistingWindowRect(RECT& outRect)
{
    CascadeSearch search = {};
    ::EnumWindows(&CascadeEnumProc, reinterpret_cast<LPARAM>(&search));

    if (!search.found)
    {
        return false;
    }

    outRect = search.rect;
    return true;
}

RECT CascadeRectFromBase(const RECT& base)
{
    // 基准窗口所在的那块屏的工作区（= 屏幕去掉任务栏）
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT work = base;
    if (::GetMonitorInfoW(::MonitorFromRect(&base, MONITOR_DEFAULTTONEAREST), &info))
    {
        work = info.rcWork;
    }

    const int width  = base.right - base.left;
    const int height = base.bottom - base.top;

    int left = base.left + kCascadeOffset;
    int top  = base.top  + kCascadeOffset;

    if (left + width > work.right)
    {
        left = base.left;      // 右边放不下：横向不错开
    }
    if (top + height > work.bottom)
    {
        top = base.top;        // 下边放不下：纵向不错开
    }

    RECT result = { left, top, left + width, top + height };
    return result;
}

void ClampRectToWorkArea(RECT& rect)
{
    MONITORINFO info = {};
    info.cbSize = sizeof(info);

    HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    if (::GetMonitorInfoW(monitor, &info) == FALSE)
    {
        return;
    }

    const RECT& work = info.rcWork;
    const int workWidth  = work.right - work.left;
    const int workHeight = work.bottom - work.top;

    int width  = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width > workWidth)
    {
        width = workWidth;
    }
    if (height > workHeight)
    {
        height = workHeight;
    }

    int left = rect.left;
    int top  = rect.top;

    if (left + width > work.right)
    {
        left = work.right - width;
    }
    if (top + height > work.bottom)
    {
        top = work.bottom - height;
    }
    if (left < work.left)
    {
        left = work.left;
    }
    if (top < work.top)
    {
        top = work.top;
    }

    rect.left   = left;
    rect.top    = top;
    rect.right  = left + width;
    rect.bottom = top + height;
}

} // namespace ui
