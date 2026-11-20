// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/CustomWindow.cpp
//
// 自绘窗口 / 控件的公共底座，说明见 CustomWindow.h。
// ============================================================================

#include "ui/CustomWindow.h"

LRESULT CALLBACK CustomWindow::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CustomWindow* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        // CreateWindowEx 的最后那个参数就是派生类对象的 this
        CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<CustomWindow*>(createStruct->lpCreateParams);

        if (self != nullptr)
        {
            self->m_hWnd = hWnd;
            ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }
    else
    {
        self = reinterpret_cast<CustomWindow*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self == nullptr)
    {
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return self->HandleMessage(hWnd, msg, wParam, lParam);
}

bool CustomWindow::RegisterWindowClass(const wchar_t* className, HINSTANCE instance, UINT style)
{
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize        = sizeof(windowClass);
    windowClass.style         = style;
    windowClass.lpfnWndProc   = &CustomWindow::WindowProc;
    windowClass.hInstance     = instance;
    windowClass.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;   // 自绘，不让系统刷底色
    windowClass.lpszClassName = className;

    if (::RegisterClassExW(&windowClass) == 0)
    {
        // 类已经存在（多实例 / 重复注册）也算成功
        if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
    }

    return true;
}

int CustomWindow::Scale(int value) const
{
    const HWND reference = (m_hWnd != nullptr) ? m_hWnd : m_dpiSource;

    int dpi = (reference != nullptr) ? ::GetDpiForWindow(reference) : 96;
    if (dpi <= 0)
    {
        dpi = 96;
    }

    return ::MulDiv(value, dpi, 96);
}

void CustomWindow::RunModalLoop(HWND owner, HWND focusAfterShow)
{
    ::EnableWindow(owner, FALSE);
    ::ShowWindow(m_hWnd, SW_SHOW);
    ::UpdateWindow(m_hWnd);

    if (focusAfterShow != nullptr)
    {
        ::SetFocus(focusAfterShow);
    }

    MSG msg;

    while (::IsWindow(m_hWnd))
    {
        if (::GetMessageW(&msg, nullptr, 0, 0) <= 0)
        {
            if (msg.message == WM_QUIT)
            {
                ::PostQuitMessage(static_cast<int>(msg.wParam));
            }
            break;
        }

        if (!::IsDialogMessageW(m_hWnd, &msg))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    ::EnableWindow(owner, TRUE);
    ::SetForegroundWindow(owner);
}
