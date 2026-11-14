// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/FlatButton.cpp
//
// 扁平图标按钮实现。
// ============================================================================

#include "FlatButton.h"

namespace
{

const wchar_t* kButtonClassName = L"FileExplorerFlatButton";

// 悬停 / 按下的背景色（接近 Windows 11 的浅灰）
const COLORREF kHotColor     = RGB(240, 240, 240);
const COLORREF kPressedColor = RGB(219, 219, 219);

// 圆角半径
const int kCornerRadius = 6;

// 图标字体里的箭头字符（和 Windows 自带的资源管理器用的是同一套字形）
const wchar_t* kGlyphBack    = L"\xE72B";   // ←
const wchar_t* kGlyphForward = L"\xE72A";   // →
const wchar_t* kGlyphUp      = L"\xE74A";   // ↑

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

FlatButton::~FlatButton()
{
    if (m_hArrowFont != nullptr)
    {
        ::DeleteObject(m_hArrowFont);
        m_hArrowFont = nullptr;
    }
}

// ============================================================================
// 窗口类注册
// ============================================================================

bool FlatButton::EnsureClassRegistered(HINSTANCE hInstance)
{
    static bool registered = false;

    if (registered)
    {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = &FlatButton::WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // 自己画背景，不要让系统刷
    wc.lpszClassName = kButtonClassName;

    if (!::RegisterClassExW(&wc))
    {
        // 类已经存在（多实例）也算成功
        if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
    }

    registered = true;
    return true;
}

// ============================================================================
// 创建
// ============================================================================

bool FlatButton::Create(HWND parent, UINT id, ArrowKind kind, HINSTANCE hInstance)
{
    if (!EnsureClassRegistered(hInstance))
    {
        return false;
    }

    m_controlId = id;
    m_kind      = kind;

    m_hWnd = ::CreateWindowExW(
        0,
        kButtonClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        hInstance,
        this);

    if (m_hWnd == nullptr)
    {
        return false;
    }

    // 箭头字体：Win11 先用 Segoe Fluent Icons，没有就退回 Segoe MDL2 Assets
    {
        int dpi        = ::GetDpiForWindow(m_hWnd);
        if (dpi <= 0)
        {
            dpi = 96;
        }
        int fontHeight = -::MulDiv(16, dpi, 96);

        m_hArrowFont = ::CreateFontW(
            fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe Fluent Icons");

        // 验证是否真的用上了这套字体（字体不存在时系统会悄悄换成别的）
        if (m_hArrowFont != nullptr)
        {
            HDC dc = ::GetDC(m_hWnd);
            HGDIOBJ oldFont = ::SelectObject(dc, m_hArrowFont);

            wchar_t actualFace[LF_FACESIZE] = {};
            ::GetTextFaceW(dc, LF_FACESIZE, actualFace);

            ::SelectObject(dc, oldFont);
            ::ReleaseDC(m_hWnd, dc);

            if (::wcscmp(actualFace, L"Segoe Fluent Icons") != 0)
            {
                ::DeleteObject(m_hArrowFont);

                m_hArrowFont = ::CreateFontW(
                    fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    L"Segoe MDL2 Assets");
            }
        }
    }

    return true;
}

void FlatButton::Resize(int x, int y, int width, int height)
{
    if (m_hWnd == nullptr)
    {
        return;
    }
    ::SetWindowPos(m_hWnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ============================================================================
// 窗口过程
// ============================================================================

LRESULT CALLBACK FlatButton::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    FlatButton* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<FlatButton*>(cs->lpCreateParams);
        if (self != nullptr)
        {
            self->m_hWnd = hWnd;
            ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }
    else
    {
        self = reinterpret_cast<FlatButton*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT FlatButton::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // --------------------------------------------------------------
    // 绘制
    // --------------------------------------------------------------
    case WM_PAINT:
    {
        PAINTSTRUCT paintStruct = {};
        HDC hdc = ::BeginPaint(m_hWnd, &paintStruct);
        Draw(hdc);
        ::EndPaint(m_hWnd, &paintStruct);
        return 0;
    }

    // 背景由 WM_PAINT 全权负责，告诉系统别刷
    case WM_ERASEBKGND:
        return 1;

    // --------------------------------------------------------------
    // 鼠标
    // --------------------------------------------------------------
    case WM_MOUSEMOVE:
    {
        if (!m_hot)
        {
            m_hot = true;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }

        // 注册"鼠标离开"通知（只需注册一次）
        if (!m_trackingLeave)
        {
            TRACKMOUSEEVENT track = {};
            track.cbSize      = sizeof(track);
            track.dwFlags     = TME_LEAVE;
            track.hwndTrack   = m_hWnd;
            track.dwHoverTime = HOVER_DEFAULT;
            ::TrackMouseEvent(&track);
            m_trackingLeave = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
    {
        m_hot           = false;
        m_pressed       = false;
        m_trackingLeave = false;
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        if (!m_enabled)
        {
            return 0;
        }
        m_pressed = true;
        ::SetCapture(m_hWnd);
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        bool wasPressed = m_pressed;
        m_pressed = false;

        if (::GetCapture() == m_hWnd)
        {
            ::ReleaseCapture();
        }

        ::InvalidateRect(m_hWnd, nullptr, FALSE);

        // 松开时鼠标还在按钮上，才算一次点击
        if (wasPressed && m_enabled)
        {
            RECT clientRect = {};
            ::GetClientRect(m_hWnd, &clientRect);

            POINT cursor = {};
            ::GetCursorPos(&cursor);
            ::ScreenToClient(m_hWnd, &cursor);

            if (::PtInRect(&clientRect, cursor))
            {
                HWND parent = ::GetParent(m_hWnd);
                if (parent != nullptr)
                {
                    ::PostMessageW(parent, WM_COMMAND,
                                   MAKEWPARAM(m_controlId, BN_CLICKED),
                                   reinterpret_cast<LPARAM>(m_hWnd));
                }
            }
        }
        return 0;
    }

    // 鼠标按下后拖出按钮，取消按下状态
    case WM_CAPTURECHANGED:
        if (m_pressed)
        {
            m_pressed = false;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return 0;

    // --------------------------------------------------------------
    // 启用 / 禁用
    // --------------------------------------------------------------
    case WM_ENABLE:
    {
        m_enabled = (wParam != 0);
        m_pressed = false;
        m_hot     = false;
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;
    }

    // 光标保持普通箭头（资源管理器的导航按钮也是箭头光标）
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    default:
        break;
    }

    return ::DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

// ============================================================================
// 绘制
// ============================================================================

void FlatButton::Draw(HDC hdc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    // ---- 背景：平时就是窗口底色（白色），不画任何边框 ----
    ::FillRect(hdc, &clientRect, ::GetSysColorBrush(COLOR_WINDOW));

    // ---- 悬停 / 按下：浅灰圆角背景 ----
    if (m_hot || m_pressed)
    {
        COLORREF fillColor = m_pressed ? kPressedColor : kHotColor;

        HBRUSH fillBrush = ::CreateSolidBrush(fillColor);
        HPEN   noPen     = ::CreatePen(PS_NULL, 0, 0);

        HGDIOBJ oldBrush = ::SelectObject(hdc, fillBrush);
        HGDIOBJ oldPen   = ::SelectObject(hdc, noPen);

        ::RoundRect(hdc, clientRect.left, clientRect.top,
                    clientRect.right, clientRect.bottom,
                    kCornerRadius, kCornerRadius);

        ::SelectObject(hdc, oldPen);
        ::SelectObject(hdc, oldBrush);
        ::DeleteObject(noPen);
        ::DeleteObject(fillBrush);
    }

    // ---- 箭头 ----
    DrawArrow(hdc, clientRect);
}

void FlatButton::DrawArrow(HDC hdc, const RECT& clientRect)
{
    // 箭头用图标字体画（和系统资源管理器是同一套字形，观感一致）
    const wchar_t* glyph = kGlyphBack;
    switch (m_kind)
    {
    case ArrowKind::Back:    glyph = kGlyphBack;    break;
    case ArrowKind::Forward: glyph = kGlyphForward; break;
    case ArrowKind::Up:      glyph = kGlyphUp;      break;
    }

    COLORREF arrowColor = m_enabled
                          ? ::GetSysColor(COLOR_WINDOWTEXT)
                          : ::GetSysColor(COLOR_GRAYTEXT);

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, arrowColor);

    HGDIOBJ oldFont = ::SelectObject(hdc, m_hArrowFont);

    RECT textRect = clientRect;
    ::DrawTextW(hdc, glyph, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    ::SelectObject(hdc, oldFont);
}
