// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/WindowPlacement.h
//
// 主窗口的"开在哪儿"：级联错开、可见性判断、推进工作区。
//
// 【为什么单独一个文件】
//   这几段原来在 MainWnd.cpp 开头（约 140 行），纯几何计算，和窗口里干什么无关。
//   抽出来以后 MainWnd::Create 只留"读配置 → 算位置 → 建窗"三步。
// ============================================================================

#pragma once

#include <windows.h>

namespace ui
{

// 上次记下来的窗口矩形还看得见吗？
//
// 为什么要检查：换了显示器、拔了副屏、改过分辨率，上次存的坐标可能整块落在屏幕外，
// 直接用会开出一个"看不见的窗口"，用户以为程序没启动。
// MONITOR_DEFAULTTONULL 在跟所有显示器都没交集时返回 NULL，正好用来判断。
bool IsWindowRectVisible(const RECT& rect);

// 有没有别的本程序窗口开着？有就把它的"还原态"矩形带回来（outRect）。
bool FindExistingWindowRect(RECT& outRect);

// 从基准窗口算出新窗口的位置：往右下错开一点，别叠在完全同一个位置。
// 工作区右边/下边放不下错开后的窗口时，只在那一轴不错开（不让新窗口跑到基准上方）。
RECT CascadeRectFromBase(const RECT& base);

// 把矩形推进目标显示器的工作区里（工作区 = 屏幕去掉任务栏的那一条）。
// 级联后跑出屏幕就推回来；窗口比工作区还大就先把宽高压到工作区大小。
void ClampRectToWorkArea(RECT& rect);

} // namespace ui
