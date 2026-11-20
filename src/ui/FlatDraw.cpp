// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/FlatDraw.cpp
//
// 三个自绘窗口共用的按钮画法，说明见 FlatDraw.h。
// ============================================================================

#include "ui/FlatDraw.h"

namespace ui
{

void DrawBorderedRect(HDC dc, const RECT& rect, COLORREF face, COLORREF border)
{
    // 画笔和画刷一起选：Rectangle() 会同时用当前画刷刷内部，
    // 只选画笔的话里面会被默认白刷盖掉（蓝色主按钮的底就是这么丢的）。
    HBRUSH  brush = ::CreateSolidBrush(face);
    HPEN    pen   = ::CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen   = ::SelectObject(dc, pen);

    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(pen);
    ::DeleteObject(brush);
}

void DrawCenteredText(HDC dc, const RECT& rect, const wchar_t* text, HFONT font,
                      COLORREF color, int insetX, bool ellipsis)
{
    HGDIOBJ oldFont = ::SelectObject(dc, font);

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, color);

    RECT textRect = rect;
    textRect.left  += insetX;
    textRect.right -= insetX;

    UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
    if (ellipsis)
    {
        flags |= DT_END_ELLIPSIS;
    }

    ::DrawTextW(dc, text, -1, &textRect, flags);

    ::SelectObject(dc, oldFont);
}

void DrawFlatButton(HDC dc, const RECT& rect, const wchar_t* text, HFONT font,
                    const FlatButtonVisual& visual)
{
    COLORREF face   = kButtonFace;
    COLORREF border = kButtonBorder;
    COLORREF color  = kButtonText;

    if (!visual.enabled)
    {
        face   = kButtonDisabledFace;
        border = kButtonDisabledBorder;
        color  = kButtonDisabledText;
    }
    else if (visual.primary)
    {
        face   = visual.pressed ? kPrimaryDownFace : (visual.hot ? kPrimaryHotFace : kPrimaryFace);
        border = kPrimaryBorder;
        color  = RGB(255, 255, 255);
    }
    else if (visual.pressed)
    {
        face   = kButtonDownFace;
        border = kButtonDownBorder;
    }
    else if (visual.hot)
    {
        face   = kButtonHotFace;
        border = kButtonHotBorder;
    }

    DrawBorderedRect(dc, rect, face, border);
    DrawCenteredText(dc, rect, text, font, color, 0, visual.ellipsis);
}

} // namespace ui
