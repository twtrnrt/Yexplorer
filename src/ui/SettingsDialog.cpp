// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/SettingsDialog.cpp
//
// 「设置」窗口的实现。说明见 SettingsDialog.h。
//
// 【为什么白底】
//   里面唯一的系统控件是复选框，系统复选框会自己刷一块白底；
//   窗口底色也用白色，两者就融在一起了，不会出现"白方块"。
// ============================================================================

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>

#include "ui/SettingsDialog.h"

#include "core/Logger.h"
#include "core/Settings.h"

namespace
{

const wchar_t* kClassName = L"FileExplorerSettingsDialog";

// 客户区大小（不含标题栏与边框）
const int kClientWidth  = 420;
const int kClientHeight = 150;
const int kPadding      = 24;

// 控件 ID
const int kControlIdMenuIcons = 4001;
const int kControlIdClose     = 4002;

// 颜色
const COLORREF kBackgroundColor = RGB(255, 255, 255);
const COLORREF kTitleColor      = RGB(32, 32, 32);
const COLORREF kHintColor       = RGB(96, 103, 111);

// 关闭按钮（和「打开方式」窗口里那套标准按钮外观一致）
const COLORREF kButtonFace       = RGB(252, 252, 252);
const COLORREF kButtonBorder     = RGB(173, 173, 173);
const COLORREF kButtonHotFace    = RGB(240, 242, 245);
const COLORREF kButtonHotBorder  = RGB(120, 150, 200);
const COLORREF kButtonDownFace   = RGB(212, 214, 219);
const COLORREF kButtonDownBorder = RGB(140, 140, 140);
const COLORREF kButtonText       = RGB(32, 32, 32);

const wchar_t* kTitleText = L"设置";

const wchar_t* kHintText1 = L"勾选后，「打开方式」和「新建」菜单里的每一项前面会显示小图标。";
const wchar_t* kHintText2 = L"不勾选就是纯文字菜单（默认，弹出更快）。改动立即保存。";

} // 匿名命名空间

SettingsDialog::~SettingsDialog()
{
    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
    }
    if (m_fontTitle != nullptr)
    {
        ::DeleteObject(m_fontTitle);
    }
}

int SettingsDialog::Scale(int value) const
{
    int dpi = (m_hWnd != nullptr) ? ::GetDpiForWindow(m_hWnd) : ::GetDpiForWindow(m_hOwner);
    if (dpi <= 0)
    {
        dpi = 96;
    }
    return ::MulDiv(value, dpi, 96);
}

bool SettingsDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &SettingsDialog::WindowProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // 自己画，不要系统刷底色
    wc.lpszClassName = kClassName;

    if (::RegisterClassExW(&wc) == 0)
    {
        return false;
    }

    registered = true;
    return true;
}

LRESULT CALLBACK SettingsDialog::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SettingsDialog* self = reinterpret_cast<SettingsDialog*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<SettingsDialog*>(createStruct->lpCreateParams);

        if (self != nullptr)
        {
            self->m_hWnd = hWnd;
            ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }

    if (self == nullptr)
    {
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return self->HandleMessage(hWnd, msg, wParam, lParam);
}

bool SettingsDialog::ShowModal(HWND owner)
{
    m_hOwner = owner;

    if (!CreateDialogWindow())
    {
        return false;
    }

    // ---- 模态循环（和「打开方式」窗口同一套写法）----
    ::EnableWindow(m_hOwner, FALSE);
    ::ShowWindow(m_hWnd, SW_SHOW);
    ::UpdateWindow(m_hWnd);

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

    ::EnableWindow(m_hOwner, TRUE);
    ::SetForegroundWindow(m_hOwner);

    return true;
}

bool SettingsDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    // ---- 字体：正文用系统消息字体；标题同款加粗加大一号 ----
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);

    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);

        LOGFONTW titleFont = metrics.lfMessageFont;
        titleFont.lfWeight = FW_SEMIBOLD;
        titleFont.lfHeight = static_cast<LONG>(titleFont.lfHeight * 1.25);
        m_fontTitle = ::CreateFontIndirectW(&titleFont);
    }

    if (m_font == nullptr)
    {
        m_font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
    if (m_fontTitle == nullptr)
    {
        m_fontTitle = m_font;
    }

    // ---- 按客户区大小反推窗口大小（要把标题栏和边框算进去）----
    int dpi = ::GetDpiForWindow(m_hOwner != nullptr ? m_hOwner : ::GetDesktopWindow());
    if (dpi <= 0)
    {
        dpi = 96;
    }

    RECT windowRect = { 0, 0, ::MulDiv(kClientWidth, dpi, 96), ::MulDiv(kClientHeight, dpi, 96) };
    ::AdjustWindowRectEx(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                         WS_EX_DLGMODALFRAME);

    const int width  = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;

    // 居中于主窗口
    RECT ownerRect = {};
    if (m_hOwner != nullptr)
    {
        ::GetWindowRect(m_hOwner, &ownerRect);
    }

    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top  + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                               kClassName,
                               L"设置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, width, height,
                               m_hOwner,
                               nullptr,
                               ::GetModuleHandleW(nullptr),
                               this);

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"设置窗口创建失败");
        return false;
    }

    // ---- 复选框（系统原生的，白底上看着自然）----
    m_hCheck = ::CreateWindowExW(0,
                                 L"BUTTON",
                                 L"右键菜单显示图标",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, 0, 0,
                                 m_hWnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdMenuIcons)),
                                 ::GetModuleHandleW(nullptr),
                                 nullptr);

    if (m_hCheck != nullptr)
    {
        ::SendMessageW(m_hCheck, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

        // 按当前配置勾上/不勾
        ::SendMessageW(m_hCheck, BM_SETCHECK,
                       core::Settings::Instance().MenuIcons() ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    Layout();
    return true;
}

void SettingsDialog::Layout()
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    const int padding = Scale(kPadding);

    // 复选框：标题下面
    if (m_hCheck != nullptr)
    {
        ::MoveWindow(m_hCheck,
                     padding, padding + Scale(44),
                     clientRect.right - padding * 2, Scale(24),
                     TRUE);
    }

    // 关闭按钮：右下角
    const int buttonWidth  = Scale(88);
    const int buttonHeight = Scale(30);

    m_closeRect.left   = clientRect.right - padding - buttonWidth;
    m_closeRect.top    = clientRect.bottom - padding - buttonHeight;
    m_closeRect.right  = m_closeRect.left + buttonWidth;
    m_closeRect.bottom = m_closeRect.top + buttonHeight;
}

void SettingsDialog::ApplyCheckState()
{
    if (m_hCheck == nullptr)
    {
        return;
    }

    const bool showIcons =
        (::SendMessageW(m_hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

    core::Settings::Instance().SetMenuIcons(showIcons);
    core::Settings::Instance().Save();

    Logger::Info(std::wstring(L"设置：右键菜单显示图标 = ") + (showIcons ? L"开" : L"关"));
}

void SettingsDialog::Draw(HDC dc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    // ---- 背景 ----
    HBRUSH background = ::CreateSolidBrush(kBackgroundColor);
    ::FillRect(dc, &clientRect, background);
    ::DeleteObject(background);

    ::SetBkMode(dc, TRANSPARENT);

    // ---- 标题 ----
    RECT titleRect = { Scale(kPadding), Scale(kPadding), clientRect.right - Scale(kPadding),
                       Scale(kPadding) + Scale(30) };

    HGDIOBJ oldFont = ::SelectObject(dc, m_fontTitle);
    ::SetTextColor(dc, kTitleColor);
    ::DrawTextW(dc, kTitleText, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ---- 说明两行 ----
    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kHintColor);

    RECT hintRect = { Scale(kPadding), Scale(kPadding) + Scale(80),
                      clientRect.right - Scale(kPadding), m_closeRect.top - Scale(6) };

    ::DrawTextW(dc, kHintText1, -1, &hintRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

    hintRect.top += Scale(20);
    ::DrawTextW(dc, kHintText2, -1, &hintRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

    ::SelectObject(dc, oldFont);

    // ---- 关闭按钮 ----
    DrawCloseButton(dc);
}

void SettingsDialog::DrawCloseButton(HDC dc)
{
    COLORREF face   = kButtonFace;
    COLORREF border = kButtonBorder;

    if (m_pressedClose)
    {
        face   = kButtonDownFace;
        border = kButtonDownBorder;
    }
    else if (m_hotClose)
    {
        face   = kButtonHotFace;
        border = kButtonHotBorder;
    }

    // 底
    HBRUSH faceBrush = ::CreateSolidBrush(face);
    ::FillRect(dc, &m_closeRect, faceBrush);
    ::DeleteObject(faceBrush);

    // 1 像素边
    HPEN borderPen = ::CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen   = ::SelectObject(dc, borderPen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, m_closeRect.left, m_closeRect.top, m_closeRect.right, m_closeRect.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(borderPen);

    // 文字
    HGDIOBJ oldFont = ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kButtonText);
    ::SetBkMode(dc, TRANSPARENT);

    RECT textRect = m_closeRect;
    ::DrawTextW(dc, L"关闭", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    ::SelectObject(dc, oldFont);
}

LRESULT SettingsDialog::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);
        const int notifyCode = HIWORD(wParam);

        if (controlId == kControlIdMenuIcons && notifyCode == BN_CLICKED)
        {
            ApplyCheckState();
            return 0;
        }

        if (controlId == kControlIdClose && notifyCode == BN_CLICKED)
        {
            ::DestroyWindow(hWnd);
            return 0;
        }
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = ::BeginPaint(hWnd, &paint);
        Draw(dc);
        ::EndPaint(hWnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        // 背景在 WM_PAINT 里整块刷，这里不刷，避免闪一下
        return 1;

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const bool hot = (::PtInRect(&m_closeRect, pt) != FALSE);

        if (hot != m_hotClose)
        {
            m_hotClose = hot;
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
        }

        if (!m_trackingLeave)
        {
            TRACKMOUSEEVENT track = {};
            track.cbSize    = sizeof(track);
            track.dwFlags   = TME_LEAVE;
            track.hwndTrack = hWnd;
            ::TrackMouseEvent(&track);
            m_trackingLeave = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_trackingLeave = false;
        if (m_hotClose || m_pressedClose)
        {
            m_hotClose    = false;
            m_pressedClose = false;
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (::PtInRect(&m_closeRect, pt))
        {
            m_pressedClose = true;
            ::SetCapture(hWnd);
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        if (m_pressedClose)
        {
            m_pressedClose = false;
            ::ReleaseCapture();

            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const bool inside = (::PtInRect(&m_closeRect, pt) != FALSE);

            ::InvalidateRect(hWnd, &m_closeRect, FALSE);

            if (inside)
            {
                ::DestroyWindow(hWnd);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ::DestroyWindow(hWnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        ::DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        m_hWnd   = nullptr;
        m_hCheck = nullptr;
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
