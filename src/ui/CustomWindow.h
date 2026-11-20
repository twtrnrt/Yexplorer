// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/CustomWindow.h
//
// 自绘窗口 / 控件的公共底座。
//
// 为什么要有它：
//   下面这几个窗口处理的消息各不相同，但外面那层样板一模一样：
//   注册窗口类、WM_NCCREATE 时把 this 挂到 GWLP_USERDATA、把消息转给成员函数、
//   DPI 缩放、模态循环。以前每份抄一遍（5 份 × 几十行），改一处要改五处，
//   而且抄漏一行（比如某个窗口忘了挂 GWLP_USERDATA）就是空指针崩溃。
//   现在只留这一份。
//
// 派生类要做的：
//   · 实现 HandleMessage（消息分发）；
//   · 要绘制的自己写 Draw（基类不拦 WM_PAINT / WM_ERASEBKGND）；
//   · 注册窗口类时调 RegisterWindowClass。
// ============================================================================
#pragma once

#include <windows.h>

class CustomWindow
{
public:
    virtual ~CustomWindow() = default;

    HWND Handle() const { return m_hWnd; }

protected:
    CustomWindow() = default;

    CustomWindow(const CustomWindow&) = delete;
    CustomWindow& operator=(const CustomWindow&) = delete;

    // 派生类的消息处理。hWnd 就是 m_hWnd（WM_NCCREATE 时已经挂好）；
    // 用不到窗口句柄的派生类可以不写参数名。
    virtual LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;

    // 注册窗口类。一律不刷背景（自绘窗口自己负责 WM_ERASEBKGND / WM_PAINT）。
    // 类已经注册过（ERROR_CLASS_ALREADY_EXISTS）也算成功——多窗口/重复调用都安全。
    static bool RegisterWindowClass(const wchar_t* className, HINSTANCE instance,
                                    UINT style = CS_HREDRAW | CS_VREDRAW);

    // DPI 缩放。窗口建好之前用 m_dpiSource（一般是 owner 窗口；空 = 按 96 DPI）。
    int Scale(int value) const;

    // 设置 Scale 的参考窗口（在 CreateWindow 之前调，比如 owner）
    void SetDpiSource(HWND source) { m_dpiSource = source; }

    // 模态循环：禁用 owner → 显示本窗口 → GetMessage/IsDialogMessage 循环 →
    // 窗口销毁后恢复 owner。focusAfterShow 非空时显示后把焦点给它
    // （「打开方式」窗口要直接用键盘操作列表）。
    void RunModalLoop(HWND owner, HWND focusAfterShow = nullptr);

    HWND m_hWnd      = nullptr;   // 派生类直接用它（各窗口原来就有这个成员）
    HWND m_dpiSource = nullptr;   // Scale 在窗口创建前的参考窗口

private:
    // 所有派生类共用的窗口过程：WM_NCCREATE 挂 this，其余转 HandleMessage
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
