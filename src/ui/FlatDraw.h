// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/FlatDraw.h
//
// 「设置」「打开方式」「批量重命名」三个自绘窗口共用的画法。
//
// 为什么单独一份：
//   这三个窗口的按钮长得一模一样（白底 / 1px 灰边 / 悬停浅灰 / 按下深灰，
//   主按钮蓝底白字，不可点时更浅），以前是各抄一遍 DrawButton：
//   颜色常量 3 份、边框/文字/状态的代码 3 份。改一处配色要改三个文件，
//   改漏一个就会出现"设置里的按钮和批量重命名里的不一样"。
//   现在只在 FlatDraw.cpp 里画一次，三个窗口传状态进来。
//
// 这里只放与窗口无关的绘制，不包含任何 HWND / 成员状态。
// ============================================================================
#pragma once

#include <windows.h>

namespace ui
{

// ----------------------------------------------------------------------------
// 1px 边框按钮的配色（三处窗口统一用这套）
// ----------------------------------------------------------------------------
inline constexpr COLORREF kButtonFace        = RGB(252, 252, 252);
inline constexpr COLORREF kButtonBorder      = RGB(173, 173, 173);
inline constexpr COLORREF kButtonHotFace     = RGB(240, 242, 245);
inline constexpr COLORREF kButtonHotBorder   = RGB(120, 150, 200);
inline constexpr COLORREF kButtonDownFace    = RGB(212, 214, 219);
inline constexpr COLORREF kButtonDownBorder  = RGB(140, 140, 140);
inline constexpr COLORREF kButtonText        = RGB(32, 32, 32);

// 不可点：浅灰底 + 灰字，一眼能看出"现在不能点"
inline constexpr COLORREF kButtonDisabledFace   = RGB(246, 247, 249);
inline constexpr COLORREF kButtonDisabledBorder = RGB(219, 221, 225);
inline constexpr COLORREF kButtonDisabledText   = RGB(160, 163, 168);

// 蓝色主按钮（批量重命名的「应用」）
inline constexpr COLORREF kPrimaryFace      = RGB(0, 103, 192);
inline constexpr COLORREF kPrimaryHotFace   = RGB(16, 120, 215);
inline constexpr COLORREF kPrimaryDownFace  = RGB(0, 84, 160);
inline constexpr COLORREF kPrimaryBorder    = RGB(0, 90, 170);

// ----------------------------------------------------------------------------
// 按钮当前的样子（和窗口里存的状态无关，只描述"怎么画"）
// ----------------------------------------------------------------------------
struct FlatButtonVisual
{
    bool hot      = false;   // 鼠标悬停
    bool pressed  = false;   // 左键按下中
    bool primary  = false;   // 蓝色主按钮
    bool enabled  = true;    // false = 灰底灰字
    bool ellipsis = false;   // 文字超宽时右侧显示省略号
};

// ----------------------------------------------------------------------------
// 画一个 1px 边框的矩形：先填 face，再用 border 描边。
// ----------------------------------------------------------------------------
void DrawBorderedRect(HDC dc, const RECT& rect, COLORREF face, COLORREF border);

// ----------------------------------------------------------------------------
// 单行居中文字：选字体 → 透明背景 → 设颜色 → DrawText → 还原字体。
// insetX：文字左右各缩进多少像素；ellipsis：超宽时右侧省略。
// ----------------------------------------------------------------------------
void DrawCenteredText(HDC dc, const RECT& rect, const wchar_t* text, HFONT font,
                      COLORREF color, int insetX = 0, bool ellipsis = false);

// ----------------------------------------------------------------------------
// 画一个标准按钮（三个窗口共用）。
// ----------------------------------------------------------------------------
void DrawFlatButton(HDC dc, const RECT& rect, const wchar_t* text, HFONT font,
                    const FlatButtonVisual& visual);

} // namespace ui
