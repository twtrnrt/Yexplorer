// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/FlatButton.h
//
// 扁平图标按钮（替代系统标准按钮）。
//
// 【为什么要自己画】
//   系统标准按钮（PushButton）带灰色立体边框和渐变底，一看就是"老式控件"，
//   而 Windows 11 资源管理器顶部的按钮是「无边框、平时透明、鼠标悬停才出现
//   一层浅灰圆角背景」。
//   系统按钮没法改成那样，所以这里自绘：
//     · 平时：透明（露出窗口底色），只有箭头
//     · 悬停：浅灰圆角背景
//     · 按下：深一点的圆角背景
//     · 禁用：箭头变灰
//
// 【箭头为什么不用字体画】
//   用图标字体（Segoe MDL2 Assets）画箭头，在 100% 缩放下边缘会发毛，
//   不同机器字体版本也可能不一样。这里直接用两三条线段画，粗细、角度、
//   位置都能精确控制，和资源管理器观感一致。
// ============================================================================

#pragma once

#include <windows.h>

class FlatButton
{
public:
    enum class ArrowKind
    {
        Back,      // ←
        Forward,   // →
        Up,        // ↑
    };

    FlatButton() = default;
    ~FlatButton();

    FlatButton(const FlatButton&) = delete;
    FlatButton& operator=(const FlatButton&) = delete;

    // id：控件 ID，点击时会像普通按钮一样给父窗口发 WM_COMMAND + BN_CLICKED
    bool Create(HWND parent, UINT id, ArrowKind kind, HINSTANCE hInstance);

    void Resize(int x, int y, int width, int height);

    HWND Handle() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void Draw(HDC hdc);
    void DrawArrow(HDC hdc, const RECT& clientRect);

    // 窗口类只需注册一次
    static bool EnsureClassRegistered(HINSTANCE hInstance);

private:
    HWND      m_hWnd       = nullptr;
    UINT      m_controlId  = 0;
    ArrowKind m_kind       = ArrowKind::Back;

    bool m_hot           = false;   // 鼠标在按钮上
    bool m_pressed       = false;   // 左键按下中
    bool m_enabled       = true;    // 是否可用
    bool m_trackingLeave = false;   // 是否已注册"鼠标离开"通知
};
