// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/AddressBar.cpp
//
// 面包屑式地址栏实现。
// ============================================================================

#include "AddressBar.h"

#include <windows.h>
#include <commctrl.h>    // SetWindowSubclass / DefSubclassProc
#include <windowsx.h>    // GET_X_LPARAM

#include <algorithm>

namespace
{

const wchar_t* kAddressBarClassName = L"FileExplorerAddressBar";

// 视觉参数（100% DPI 下的像素，实际用 Scale 放大）
const int kPaddingX       = 8;    // 左侧起始留白
const int kSegmentPadX    = 7;    // 每段文字左右内边距
const int kSeparatorWidth = 16;   // 段与段之间留给 ">" 的宽度
const int kCornerRadius   = 6;

const COLORREF kHotColor      = RGB(240, 240, 240);
const COLORREF kSeparatorColor = RGB(120, 120, 120);

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

AddressBar::~AddressBar()
{
    if (m_hOwnFont != nullptr)
    {
        ::DeleteObject(m_hOwnFont);
        m_hOwnFont = nullptr;
    }
}

bool AddressBar::EnsureClassRegistered(HINSTANCE hInstance)
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &AddressBar::WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kAddressBarClassName;

    if (!::RegisterClassExW(&wc))
    {
        if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
    }

    registered = true;
    return true;
}

bool AddressBar::Create(HWND parent, UINT id, HINSTANCE hInstance)
{
    if (!EnsureClassRegistered(hInstance))
    {
        return false;
    }

    m_controlId = id;

    m_hWnd = ::CreateWindowExW(
        0,
        kAddressBarClassName,
        L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        hInstance,
        this);

    if (m_hWnd == nullptr)
    {
        return false;
    }

    // 外部没给字体时，自己拿系统的消息字体
    if (m_hFont == nullptr)
    {
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            m_hOwnFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
            m_hFont    = m_hOwnFont;
        }
    }

    return true;
}

void AddressBar::Resize(int x, int y, int width, int height)
{
    if (m_hWnd == nullptr)
    {
        return;
    }
    ::SetWindowPos(m_hWnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void AddressBar::SetFont(HFONT font)
{
    m_hFont = font;
    if (m_hWnd != nullptr)
    {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void AddressBar::SetNavigateCallback(std::function<void(const std::wstring&)> callback)
{
    m_navigateCallback = std::move(callback);
}

int AddressBar::Scale(int value) const
{
    int dpi = (m_hWnd != nullptr) ? ::GetDpiForWindow(m_hWnd) : 96;
    if (dpi <= 0)
    {
        dpi = 96;
    }
    return ::MulDiv(value, dpi, 96);
}

// ============================================================================
// 路径分段
// ============================================================================

void AddressBar::SetPath(const std::wstring& path)
{
    m_path = path;

    // 普通磁盘路径：清掉虚拟文件夹的显示名，避免串味。
    // 例外是"整条显示 label"模式（从主文件夹点进文档这种）：
    // 那种情况本来就是「真实路径 + 一个给人看的名字」，名字不能清掉。
    if (!m_wholeLabel &&
        m_path.compare(0, 2, L"::") != 0 && m_path.compare(0, 6, L"shell:") != 0)
    {
        m_label.clear();
    }

    RebuildSegments();

    if (m_editing && m_hEdit != nullptr)
    {
        ::SetWindowTextW(m_hEdit, m_path.c_str());
    }

    if (m_hWnd != nullptr)
    {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void AddressBar::SetPathWithLabel(const std::wstring& path,
                                  const std::wstring& label,
                                  bool              wholeLabel)
{
    // 清掉上一次可能留下的折叠前缀，避免串味
    m_prefixPath.clear();
    m_prefixLabel.clear();

    m_label      = label;
    m_wholeLabel = wholeLabel;
    SetPath(path);
}

void AddressBar::SetPathWithPrefix(const std::wstring& path,
                                   const std::wstring& prefixPath,
                                   const std::wstring& prefixLabel)
{
    m_prefixPath  = prefixPath;
    m_prefixLabel = prefixLabel;

    // 折叠模式自己不整条显示，靠 RebuildSegments 里的前缀分支处理
    m_wholeLabel = false;
    m_label.clear();

    SetPath(path);
}

void AddressBar::RebuildSegments()
{
    m_segments.clear();
    m_hotIndex = -1;

    std::wstring p = m_path;
    if (p.empty())
    {
        return;
    }

    // 虚拟文件夹（「此电脑」这类）：路径是 ::{GUID}，没法切段，
    // 整条显示系统给的名字，点它就是回它自己
    //
    // 另一种是"整条显示 label"模式（m_wholeLabel）：从「主文件夹」双击进去的
    // 系统目录（文档、图片、下载…），地址栏就显示「文档」，不切路径。
    if (!m_label.empty() &&
        (m_wholeLabel || p.compare(0, 2, L"::") == 0 || p.compare(0, 6, L"shell:") == 0))
    {
        Segment segment;
        segment.text     = m_label;
        segment.fullPath = p;
        m_segments.push_back(segment);
        return;
    }

    // 折叠前缀模式：当前路径在某个系统文件夹（下载/图片/文档…）之下。
    // 第一段显示那个文件夹的名字（点它回文件夹本身），后面照常一段段切。
    //   例：...\Downloads\2025\abc → 「下载 > 2025 > abc」
    if (!m_prefixPath.empty() && !m_prefixLabel.empty())
    {
        std::wstring prefix = m_prefixPath;

        while (prefix.size() > 3 && (prefix.back() == L'\\' || prefix.back() == L'/'))
        {
            prefix.pop_back();
        }

        const bool isSelf = (::_wcsicmp(prefix.c_str(), p.c_str()) == 0);

        const bool underIt = (prefix.size() < p.size() &&
                              ::_wcsnicmp(prefix.c_str(), p.c_str(), prefix.size()) == 0 &&
                              (p[prefix.size()] == L'\\' || p[prefix.size()] == L'/'));

        if (isSelf || underIt)
        {
            Segment first;
            first.text     = m_prefixLabel;
            first.fullPath = prefix;
            m_segments.push_back(first);

            // 前缀之后剩下来的部分
            std::wstring rest = isSelf ? std::wstring() : p.substr(prefix.size());

            while (!rest.empty() && (rest.front() == L'\\' || rest.front() == L'/'))
            {
                rest.erase(rest.begin());
            }

            // 按反斜杠再切一次
            std::vector<std::wstring> parts;
            {
                std::wstring current;

                for (wchar_t ch : rest)
                {
                    if (ch == L'\\' || ch == L'/')
                    {
                        if (!current.empty())
                        {
                            parts.push_back(current);
                            current.clear();
                        }
                    }
                    else
                    {
                        current.push_back(ch);
                    }
                }

                if (!current.empty())
                {
                    parts.push_back(current);
                }
            }

            std::wstring accumulate = prefix;

            for (const std::wstring& part : parts)
            {
                accumulate += L"\\" + part;

                Segment segment;
                segment.text     = part;
                segment.fullPath = accumulate;
                m_segments.push_back(segment);
            }

            return;
        }
    }

    // 去掉末尾多余的斜杠，但保留 "C:\" 这种盘符根
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
    {
        p.pop_back();
    }

    // 先按反斜杠切成若干名字
    std::vector<std::wstring> parts;
    {
        std::wstring current;
        for (wchar_t ch : p)
        {
            if (ch == L'\\' || ch == L'/')
            {
                if (!current.empty())
                {
                    parts.push_back(current);
                    current.clear();
                }
            }
            else
            {
                current.push_back(ch);
            }
        }
        if (!current.empty())
        {
            parts.push_back(current);
        }
    }

    if (parts.empty())
    {
        return;
    }

    // 拼回每一段对应的完整路径
    // 例：C:\Windows\System32  →
    //   { "C:",      "C:\" }
    //   { "Windows", "C:\Windows" }
    //   { "System32","C:\Windows\System32" }
    std::wstring accumulate = parts[0] + L"\\";

    Segment first;
    first.text     = parts[0];
    first.fullPath = accumulate;

    // 只有一个名字（就是盘符根目录）时，显示成 "C:\" 更直观
    if (parts.size() == 1 && !parts[0].empty() && parts[0].back() == L':')
    {
        first.text = parts[0] + L"\\";
    }

    m_segments.push_back(first);

    for (size_t i = 1; i < parts.size(); ++i)
    {
        accumulate += parts[i];

        Segment segment;
        segment.text     = parts[i];
        segment.fullPath = accumulate;
        m_segments.push_back(segment);

        if (i + 1 < parts.size())
        {
            accumulate += L'\\';
        }
    }
}

// ============================================================================
// 布局：算出每段的矩形（含"放不下时从中间省略"的处理）
// ============================================================================

void AddressBar::LayoutSegments(HDC hdc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    int count = static_cast<int>(m_segments.size());
    m_segmentRects.assign(count, RECT{ 0, 0, 0, 0 });
    m_ellipsisRect = RECT{ 0, 0, 0, 0 };

    if (count == 0)
    {
        return;
    }

    HGDIOBJ oldFont = ::SelectObject(hdc, m_hFont);

    int padX     = Scale(kSegmentPadX);
    int sepWidth = Scale(kSeparatorWidth);
    int availWidth = clientRect.right - Scale(kPaddingX) * 2;

    // 每段文字宽度 + 内边距
    std::vector<int> widths(count, 0);
    for (int i = 0; i < count; ++i)
    {
        SIZE size = {};
        ::GetTextExtentPoint32W(hdc, m_segments[i].text.c_str(),
                                static_cast<int>(m_segments[i].text.size()), &size);
        widths[i] = size.cx + padX * 2;
    }

    // 总宽度
    int totalWidth = 0;
    for (int i = 0; i < count; ++i)
    {
        totalWidth += widths[i];
        if (i > 0)
        {
            totalWidth += sepWidth;
        }
    }

    // 放不下时：保留第一段（盘符）和最后几段，中间用 "…" 代过
    std::vector<bool> visible(count, true);

    if (totalWidth > availWidth && count > 2)
    {
        SIZE ellipsisSize = {};
        ::GetTextExtentPoint32W(hdc, L"\x2026", 1, &ellipsisSize);
        int ellipsisWidth = ellipsisSize.cx + padX * 2;

        for (int i = 1; i < count; ++i)
        {
            visible[i] = false;
        }

        int used = widths[0] + sepWidth + ellipsisWidth;
        for (int i = count - 1; i >= 1; --i)
        {
            if (used + widths[i] + sepWidth <= availWidth)
            {
                visible[i] = true;
                used += widths[i] + sepWidth;
            }
            else
            {
                break;
            }
        }
    }

    // 记录可见段，并生成矩形
    std::vector<int> visibleIndexes;
    for (int i = 0; i < count; ++i)
    {
        if (visible[i])
        {
            visibleIndexes.push_back(i);
        }
    }

    int x = Scale(kPaddingX);
    int previousIndex = -1;
    SIZE ellipsisSize = {};
    ::GetTextExtentPoint32W(hdc, L"\x2026", 1, &ellipsisSize);
    int ellipsisWidth = ellipsisSize.cx + padX * 2;

    for (size_t k = 0; k < visibleIndexes.size(); ++k)
    {
        int index = visibleIndexes[k];

        // 与前一段不连续 → 中间有被省略的段，先画一个 "…"
        if (previousIndex >= 0 && index != previousIndex + 1)
        {
            m_ellipsisRect.left   = x;
            m_ellipsisRect.right  = x + ellipsisWidth;
            m_ellipsisRect.top    = 0;
            m_ellipsisRect.bottom = clientRect.bottom;
            x += ellipsisWidth + sepWidth;
        }

        RECT rect = { x, 0, x + widths[index], clientRect.bottom };
        m_segmentRects[index] = rect;
        x += widths[index];

        if (k + 1 < visibleIndexes.size())
        {
            x += sepWidth;
        }

        previousIndex = index;
    }

    ::SelectObject(hdc, oldFont);
}

// ============================================================================
// 绘制
// ============================================================================

void AddressBar::Draw(HDC hdc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    // 背景
    ::FillRect(hdc, &clientRect, ::GetSysColorBrush(COLOR_WINDOW));

    // 编辑模式：输入框自己填白，这里给它描一圈边框
    // （输入框内缩了 1 像素，这圈线正好露在外面，不会被输入框盖住；
    //   之前靠输入框自己的 STATICEDGE，右边和下边会被父窗口边缘裁掉）
    if (m_editing)
    {
        RECT frame = clientRect;
        ::FrameRect(hdc, &frame, ::GetSysColorBrush(COLOR_3DLIGHT));
        return;
    }

    LayoutSegments(hdc);

    HGDIOBJ oldFont = ::SelectObject(hdc, m_hFont);
    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));

    int count = static_cast<int>(m_segments.size());
    int sepWidth = Scale(kSeparatorWidth);

    // 省略号
    if (m_ellipsisRect.right > m_ellipsisRect.left)
    {
        ::SetTextColor(hdc, kSeparatorColor);
        ::DrawTextW(hdc, L"\x2026", -1, &m_ellipsisRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));
    }

    for (int i = 0; i < count; ++i)
    {
        RECT rect = m_segmentRects[i];
        if (rect.right <= rect.left)
        {
            continue;   // 该段被省略
        }

        // 悬停背景
        if (i == m_hotIndex)
        {
            HBRUSH hotBrush = ::CreateSolidBrush(kHotColor);
            HPEN   noPen    = ::CreatePen(PS_NULL, 0, 0);
            HGDIOBJ oldBrush = ::SelectObject(hdc, hotBrush);
            HGDIOBJ oldPen   = ::SelectObject(hdc, noPen);

            ::RoundRect(hdc, rect.left, rect.top + Scale(2),
                        rect.right, rect.bottom - Scale(2),
                        kCornerRadius, kCornerRadius);

            ::SelectObject(hdc, oldPen);
            ::SelectObject(hdc, oldBrush);
            ::DeleteObject(noPen);
            ::DeleteObject(hotBrush);
        }

        // 段文字
        RECT textRect = rect;
        ::DrawTextW(hdc, m_segments[i].text.c_str(), -1, &textRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 段与段之间的 ">"
        // 下一段的左边就是本段右边 + sepWidth
        bool hasNext = false;
        for (int j = i + 1; j < count; ++j)
        {
            if (m_segmentRects[j].right > m_segmentRects[j].left)
            {
                hasNext = true;
                break;
            }
        }

        if (hasNext)
        {
            RECT sepRect = { rect.right, rect.top, rect.right + sepWidth, rect.bottom };
            ::SetTextColor(hdc, kSeparatorColor);
            ::DrawTextW(hdc, L">", -1, &sepRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));
        }
    }

    ::SelectObject(hdc, oldFont);
}

// ============================================================================
// 编辑模式
// ============================================================================

void AddressBar::BeginEditing()
{
    BeginEditingImpl();
}

void AddressBar::BeginEditingImpl()
{
    if (m_editing)
    {
        return;
    }

    if (m_hEdit == nullptr)
    {
        m_hEdit = ::CreateWindowExW(
            0,
            L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 0, 0,
            m_hWnd,
            nullptr,
            reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(m_hWnd, GWLP_HINSTANCE)),
            nullptr);

        if (m_hEdit == nullptr)
        {
            return;
        }

        if (m_hFont != nullptr)
        {
            ::SendMessageW(m_hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
        }

        ::SetWindowSubclass(m_hEdit, &AddressBar::EditSubclassProc, 1,
                            reinterpret_cast<DWORD_PTR>(this));
    }

    // 比客户区小一圈（内缩 1 像素）：
    // 编辑框自己的边框画在窗口内侧，如果铺满客户区，右边和下边的边框
    // 正好压在父窗口边界上会被裁掉，看着像"没有右边线、没有下边线"
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);
    int inset     = 1;
    int editWidth  = clientRect.right  - inset * 2;
    int editHeight = clientRect.bottom - inset * 2;
    if (editWidth  < 1) { editWidth  = 1; }
    if (editHeight < 1) { editHeight = 1; }

    ::SetWindowPos(m_hEdit, HWND_TOP, inset, inset, editWidth, editHeight, SWP_SHOWWINDOW);

    m_editing = true;
    ::SetWindowTextW(m_hEdit, m_path.c_str());
    ::SetFocus(m_hEdit);
    ::SendMessageW(m_hEdit, EM_SETSEL, 0, -1);
}

void AddressBar::EndEditing()
{
    if (!m_editing)
    {
        return;
    }

    m_editing = false;

    if (m_hEdit != nullptr)
    {
        ::ShowWindow(m_hEdit, SW_HIDE);
    }

    ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

void AddressBar::CommitEditing()
{
    if (m_hEdit == nullptr)
    {
        return;
    }

    wchar_t buffer[2048] = {};
    ::GetWindowTextW(m_hEdit, buffer, ARRAYSIZE(buffer));

    std::wstring path = buffer;

    // 去掉首尾空白（这里手写，避免引入额外依赖）
    while (!path.empty() && (path.front() == L' ' || path.front() == L'\t'))
    {
        path.erase(path.begin());
    }
    while (!path.empty() && (path.back() == L' ' || path.back() == L'\t'))
    {
        path.pop_back();
    }

    if (path.empty())
    {
        EndEditing();
        return;
    }

    // 只输入了 "C:" 这种盘符，补一个反斜杠
    if (path.size() == 2 && path[1] == L':')
    {
        path += L'\\';
    }

    EndEditing();

    if (m_navigateCallback)
    {
        m_navigateCallback(path);
    }
}

// ============================================================================
// 窗口过程
// ============================================================================

LRESULT CALLBACK AddressBar::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AddressBar* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<AddressBar*>(cs->lpCreateParams);
        if (self != nullptr)
        {
            self->m_hWnd = hWnd;
            ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }
    else
    {
        self = reinterpret_cast<AddressBar*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT AddressBar::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paintStruct = {};
        HDC hdc = ::BeginPaint(m_hWnd, &paintStruct);
        Draw(hdc);
        ::EndPaint(m_hWnd, &paintStruct);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (m_hEdit != nullptr && m_editing)
        {
            RECT clientRect = {};
            ::GetClientRect(m_hWnd, &clientRect);
            int inset      = 1;
            int editWidth  = clientRect.right  - inset * 2;
            int editHeight = clientRect.bottom - inset * 2;
            if (editWidth  < 1) { editWidth  = 1; }
            if (editHeight < 1) { editHeight = 1; }

            ::SetWindowPos(m_hEdit, HWND_TOP, inset, inset, editWidth, editHeight,
                           SWP_NOACTIVATE);
        }
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        int hot = -1;
        for (size_t i = 0; i < m_segmentRects.size(); ++i)
        {
            const RECT& rect = m_segmentRects[i];
            if (rect.right > rect.left && ::PtInRect(&rect, pt))
            {
                hot = static_cast<int>(i);
                break;
            }
        }

        if (hot != m_hotIndex)
        {
            m_hotIndex = hot;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }

        // 注册"鼠标离开"通知（只需一次）
        if (!m_trackingLeave)
        {
            TRACKMOUSEEVENT track = {};
            track.cbSize    = sizeof(track);
            track.dwFlags   = TME_LEAVE;
            track.hwndTrack = m_hWnd;
            ::TrackMouseEvent(&track);
            m_trackingLeave = true;
        }

        return 0;
    }

    case WM_MOUSELEAVE:
        m_trackingLeave = false;
        if (m_hotIndex != -1)
        {
            m_hotIndex = -1;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // 点到某一段 → 跳过去
        for (size_t i = 0; i < m_segmentRects.size(); ++i)
        {
            const RECT& rect = m_segmentRects[i];
            if (rect.right > rect.left && ::PtInRect(&rect, pt))
            {
                if (m_navigateCallback)
                {
                    std::wstring target = m_segments[i].fullPath;
                    m_navigateCallback(target);
                }
                return 0;
            }
        }

        // 点到空白 → 进入手动输入
        BeginEditingImpl();
        return 0;
    }

    // 鼠标移进时注册"离开"通知
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
// 输入框的子类过程
// ============================================================================

LRESULT CALLBACK AddressBar::EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR idSubclass, DWORD_PTR refData)
{
    AddressBar* self = reinterpret_cast<AddressBar*>(refData);

    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN && self != nullptr)
        {
            self->CommitEditing();
            return 0;
        }
        if (wParam == VK_ESCAPE && self != nullptr)
        {
            self->EndEditing();
            return 0;
        }
    }

    // 屏蔽回车字符，避免系统"叮"一声
    if (msg == WM_CHAR && wParam == L'\r')
    {
        return 0;
    }

    // 失去焦点就退出编辑（点别处时自动回到分段显示）
    if (msg == WM_KILLFOCUS && self != nullptr)
    {
        self->EndEditing();
        return 0;
    }

    if (msg == WM_NCDESTROY)
    {
        ::RemoveWindowSubclass(hWnd, &AddressBar::EditSubclassProc, idSubclass);
    }

    return ::DefSubclassProc(hWnd, msg, wParam, lParam);
}
