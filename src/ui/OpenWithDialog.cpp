// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/OpenWithDialog.cpp
//
// 「打开方式」设置窗口的实现。界面说明见 OpenWithDialog.h。
//
// 几个关键决定：
//   1. 按钮自己画（项目里所有按钮都是扁平的，混一个系统立体按钮会很突兀）；
//   2. 列表用系统 ListView 带复选框：勾选状态由系统画，稳定、好点；
//   3. 「默认」列用 ●/○ 字符表达（用颜色区分），点击这一列就设成默认；
//   4. 打开窗口时才读系统候选（用户明确要求），平时右键菜单绝不读。
// ============================================================================

#include "ui/OpenWithDialog.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/ShellOpenWith.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>

namespace ui
{

namespace
{

const wchar_t* const kClassName = L"FileExplorerOpenWithDialog";

const int kControlIdList = 2001;

// 窗口客户区尺寸（逻辑像素）
const int kDialogWidth  = 620;
const int kDialogHeight = 440;

const int kTitleHeight = 26;   // 标题一行
const int kHintHeight  = 22;   // 说明一行
const int kPadding     = 14;
const int kButtonHeight = 28;

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

OpenWithDialog::OpenWithDialog(HWND owner, shell::IconCache* icons)
    : m_hOwner(owner), m_icons(icons)
{
}

OpenWithDialog::~OpenWithDialog()
{
    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
        m_font = nullptr;
    }
}

// ============================================================================
// 窗口类
// ============================================================================

bool OpenWithDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = &OpenWithDialog::WindowProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // 自己画，避免闪
    wc.lpszClassName = kClassName;

    if (::RegisterClassExW(&wc) == 0)
    {
        return false;
    }

    registered = true;
    return true;
}

int OpenWithDialog::Scale(int value) const
{
    int dpi = 96;
    if (m_hOwner != nullptr)
    {
        dpi = ::GetDpiForWindow(m_hOwner);
    }
    if (dpi <= 0)
    {
        dpi = 96;
    }
    return ::MulDiv(value, dpi, 96);
}

// ============================================================================
// 显示（模态）
// ============================================================================

bool OpenWithDialog::ShowModal(const std::wstring& extension, bool firstTime)
{
    m_extension = extension;
    m_firstTime = firstTime;
    m_skipped   = false;

    // 从配置里拷一份工作副本（改完关窗口才写回，改一半放弃也不影响配置）
    const core::OpenWithEntry* saved = core::Settings::Instance().FindOpenWith(m_extension);
    if (saved != nullptr)
    {
        m_entry = *saved;
    }
    else
    {
        m_entry = core::OpenWithEntry();
    }
    m_entry.ext = m_extension;

    if (!CreateDialogWindow())
    {
        return false;
    }

    // ---- 第一次打开窗口：读系统候选 ----
    RefreshCandidatesFromSystem(false);

    // ---- 首次询问：把系统当前默认程序预选上 ----
    // 目的是让用户"直接关掉"就等于确认了系统默认，少点一次。
    if (m_firstTime && m_entry.defaultKey.empty())
    {
        core::OpenWithItem systemDefault;
        if (shell::QuerySystemDefault(m_extension, systemDefault))
        {
            const std::wstring key = core::OpenWithKey(systemDefault);

            // 已经在列表里就直接选中它
            bool found = false;
            for (const core::OpenWithItem& item : m_entry.items)
            {
                if (core::OpenWithKey(item) == key)
                {
                    found = true;
                    break;
                }
            }

            // 不在列表里就补一条（系统的默认程序，通常也是候选之一）
            if (!found)
            {
                systemDefault.enabled = true;
                m_entry.items.push_back(systemDefault);
            }

            m_entry.defaultKey = key;
        }
    }

    ProbeAll();
    ReloadList();

    // ---- 模态循环 ----
    ::EnableWindow(m_hOwner, FALSE);
    ::ShowWindow(m_hWnd, SW_SHOW);
    ::UpdateWindow(m_hWnd);
    ::SetFocus(m_hList);

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

    // ---- 写回配置 ----
    if (m_skipped)
    {
        // 点了「跳过」：这次不记录，把预选撤掉（下次双击还会问）
        m_entry.defaultKey.clear();
    }

    core::Settings::Instance().EnsureOpenWith(m_extension) = m_entry;
    core::Settings::Instance().Save();

    Logger::Info(L"打开方式已保存：扩展名=" + m_extension + L"，程序 " +
                 std::to_wstring(m_entry.items.size()) + L" 个，默认=" +
                 (m_entry.defaultKey.empty() ? std::wstring(L"（未设置）")
                                             : m_entry.defaultKey));

    return !m_entry.defaultKey.empty();
}

// ============================================================================
// 建窗
// ============================================================================

bool OpenWithDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    if (m_font == nullptr)
    {
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);
        }
    }

    const int width  = Scale(kDialogWidth);
    const int height = Scale(kDialogHeight);

    // 居中于主窗口
    RECT ownerRect = {};
    if (m_hOwner != nullptr)
    {
        ::GetWindowRect(m_hOwner, &ownerRect);
    }
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top  + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    std::wstring title = L"打开方式 - " + m_extension;
    if (m_firstTime)
    {
        title += L"（第一次打开这类文件）";
    }

    m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                               kClassName,
                               title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, width, height,
                               m_hOwner,
                               nullptr,
                               ::GetModuleHandleW(nullptr),
                               this);

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"打开方式窗口创建失败");
        return false;
    }

    CreateControls();
    Layout();
    return true;
}

void OpenWithDialog::CreateControls()
{
    m_hList = ::CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER,
        0, 0, 0, 0,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdList)),
        ::GetModuleHandleW(nullptr), nullptr);

    if (m_hList != nullptr)
    {
        ::SendMessageW(m_hList, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

        ::SendMessageW(m_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                       LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        // 小图标来自系统图标列表（和主窗口同一份）
        if (m_icons != nullptr)
        {
            ::SendMessageW(m_hList, LVM_SETIMAGELIST, LVSIL_SMALL,
                           reinterpret_cast<LPARAM>(m_icons->ImageList(
                               shell::IconCache::IconSize::Small)));
        }

        LVCOLUMNW column = {};
        column.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.fmt     = LVCFMT_LEFT;
        column.pszText = const_cast<LPWSTR>(L"程序");
        column.cx      = Scale(320);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));

        column.pszText = const_cast<LPWSTR>(L"状态");
        column.cx      = Scale(120);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));

        column.pszText = const_cast<LPWSTR>(L"默认");
        column.cx      = Scale(56);
        column.fmt     = LVCFMT_CENTER;
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
    }
}

// ============================================================================
// 布局
// ============================================================================

void OpenWithDialog::Layout()
{
    if (m_hWnd == nullptr || m_hList == nullptr)
    {
        return;
    }

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    const int padding = Scale(kPadding);
    const int clientWidth  = clientRect.right;
    const int clientHeight = clientRect.bottom;

    // ---- 列表 ----
    const int listTop = padding + Scale(kTitleHeight) + Scale(kHintHeight) + Scale(6);
    const int listBottomMargin = Scale(kButtonHeight) + padding * 2 + Scale(6);

    m_listRect.left   = padding;
    m_listRect.top    = listTop;
    m_listRect.right  = clientWidth - padding;
    m_listRect.bottom = clientHeight - listBottomMargin;

    ::MoveWindow(m_hList,
                 m_listRect.left, m_listRect.top,
                 m_listRect.right - m_listRect.left,
                 m_listRect.bottom - m_listRect.top,
                 TRUE);

    // 程序名列吃满剩余宽度，另外两列固定
    const int nameWidth = (m_listRect.right - m_listRect.left) - Scale(120) - Scale(56) -
                          ::GetSystemMetrics(SM_CXVSCROLL) - Scale(4);
    ::SendMessageW(m_hList, LVM_SETCOLUMNWIDTH, 0, std::max(Scale(160), nameWidth));

    // ---- 按钮（从右往左排）----
    m_buttonCount = 0;
    const int buttonWidths[ButtonCount] = {
        Scale(104),   // 添加程序…
        Scale(64),    // 删除
        Scale(64),    // 刷新
        Scale(76),    // 跳过（只有首次显示）
        Scale(64),    // 关闭
    };

    const int gap = Scale(8);
    const int buttonY = clientHeight - padding - Scale(kButtonHeight);
    int rightEdge = clientWidth - padding;

    // 从右往左：关闭在最右
    for (int index = ButtonClose; index >= 0; --index)
    {
        if (index == ButtonSkip && !m_firstTime)
        {
            m_buttonRects[index] = {};
            continue;
        }

        const int width = buttonWidths[index];
        m_buttonRects[index].left   = rightEdge - width;
        m_buttonRects[index].top    = buttonY;
        m_buttonRects[index].right  = rightEdge;
        m_buttonRects[index].bottom = buttonY + Scale(kButtonHeight);

        rightEdge -= width + gap;
        ++m_buttonCount;
    }
}

void OpenWithDialog::Draw(HDC dc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    // 背景：和主界面一致的浅灰
    HBRUSH background = ::CreateSolidBrush(RGB(248, 248, 248));
    ::FillRect(dc, &clientRect, background);
    ::DeleteObject(background);

    if (m_font != nullptr)
    {
        ::SelectObject(dc, m_font);
    }
    ::SetBkMode(dc, TRANSPARENT);

    const int padding = Scale(kPadding);

    // ---- 标题 ----
    std::wstring title = L"选择用哪个程序打开「" + m_extension + L"」文件";
    ::SetTextColor(dc, RGB(32, 32, 32));

    RECT titleRect = { padding, padding, clientRect.right - padding,
                       padding + Scale(kTitleHeight) };
    ::DrawTextW(dc, title.c_str(), -1, &titleRect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // ---- 说明 ----
    std::wstring hint = m_firstTime
        ? L"这一步只需做一次：勾选留在右键菜单里的程序，右侧 ● 表示默认用它打开"
        : L"勾选 = 出现在右键「打开方式」菜单；点右侧圆点 = 设为默认程序";

    RECT hintRect = { padding, padding + Scale(kTitleHeight),
                      clientRect.right - padding,
                      padding + Scale(kTitleHeight) + Scale(kHintHeight) };
    ::SetTextColor(dc, RGB(110, 110, 110));
    ::DrawTextW(dc, hint.c_str(), -1, &hintRect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // ---- 按钮 ----
    for (int index = 0; index < ButtonCount; ++index)
    {
        DrawFlatButton(dc, index);
    }
}

void OpenWithDialog::DrawFlatButton(HDC dc, int index)
{
    if (index == ButtonSkip && !m_firstTime)
    {
        return;
    }

    const RECT& rect = m_buttonRects[index];
    if (rect.right <= rect.left)
    {
        return;
    }

    // 悬停/按下的是浅灰/深灰底，其余透明（露出窗口底色）
    COLORREF fill = RGB(248, 248, 248);
    if (m_pressedButton == index)
    {
        fill = RGB(210, 210, 210);
    }
    else if (m_hotButton == index)
    {
        fill = RGB(232, 232, 232);
    }

    HBRUSH brush = ::CreateSolidBrush(fill);
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);

    const wchar_t* texts[ButtonCount] = {
        L"添加程序…", L"删除", L"刷新", L"跳过", L"关闭",
    };

    ::SetTextColor(dc, (index == ButtonClose) ? RGB(32, 32, 32) : RGB(64, 64, 64));

    RECT textRect = rect;
    ::DrawTextW(dc, texts[index], -1, &textRect,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

// ============================================================================
// 消息
// ============================================================================

LRESULT CALLBACK OpenWithDialog::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    OpenWithDialog* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OpenWithDialog*>(create->lpCreateParams);
        self->m_hWnd = hWnd;
        ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<OpenWithDialog*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT OpenWithDialog::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = ::BeginPaint(m_hWnd, &paint);
        Draw(dc);
        ::EndPaint(m_hWnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // 背景在 WM_PAINT 里画，这里返回真避免闪烁

    case WM_MOUSEMOVE:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        int hot = -1;
        for (int index = 0; index < ButtonCount; ++index)
        {
            if (index == ButtonSkip && !m_firstTime)
            {
                continue;
            }

            const RECT& rect = m_buttonRects[index];
            if (rect.right > rect.left && ::PtInRect(&rect, POINT{x, y}))
            {
                hot = index;
                break;
            }
        }

        if (hot != m_hotButton)
        {
            m_hotButton = hot;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }

        // 鼠标离开窗口时清掉悬停状态
        TRACKMOUSEEVENT track = {};
        track.cbSize    = sizeof(track);
        track.dwFlags   = TME_LEAVE;
        track.hwndTrack = m_hWnd;
        ::TrackMouseEvent(&track);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (m_hotButton != -1)
        {
            m_hotButton = -1;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        for (int index = 0; index < ButtonCount; ++index)
        {
            if (index == ButtonSkip && !m_firstTime)
            {
                continue;
            }

            const RECT& rect = m_buttonRects[index];
            if (rect.right > rect.left && ::PtInRect(&rect, POINT{x, y}))
            {
                m_pressedButton = index;
                ::InvalidateRect(m_hWnd, nullptr, FALSE);
                ::SetCapture(m_hWnd);
                return 0;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        if (::GetCapture() == m_hWnd)
        {
            ::ReleaseCapture();
        }

        const int pressed = m_pressedButton;
        m_pressedButton = -1;
        ::InvalidateRect(m_hWnd, nullptr, FALSE);

        if (pressed < 0)
        {
            return 0;
        }

        const RECT& rect = m_buttonRects[pressed];
        if (rect.right > rect.left && ::PtInRect(&rect, POINT{x, y}))
        {
            OnButtonClick(pressed);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ::SendMessageW(m_hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_CLOSE:
        ::ShowWindow(m_hWnd, SW_HIDE);
        ::DestroyWindow(m_hWnd);
        return 0;

    case WM_DESTROY:
        m_hWnd = nullptr;
        return 0;

    case WM_NOTIFY:
    {
        const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);

        if (header->hwndFrom != m_hList)
        {
            break;
        }

        switch (header->code)
        {
        case LVN_ITEMCHANGED:
        {
            const NMLISTVIEW* change = reinterpret_cast<const NMLISTVIEW*>(lParam);

            if ((change->uChanged & LVIF_STATE) != 0 &&
                ((change->uNewState & LVIS_STATEIMAGEMASK) !=
                 (change->uOldState & LVIS_STATEIMAGEMASK)) &&
                !m_syncingChecks && change->iItem >= 0)
            {
                const bool checked = ListView_GetCheckState(m_hList, change->iItem) != FALSE;
                OnCheckChanged(change->iItem, checked);
            }
            return 0;
        }

        case NM_CLICK:
        {
            LVHITTESTINFO hit = {};
            hit.pt = {GET_X_LPARAM(::GetMessagePos()), GET_Y_LPARAM(::GetMessagePos())};
            ::ScreenToClient(m_hList, &hit.pt);
            ::SendMessageW(m_hList, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));

            // 点「默认」列 → 设为默认程序
            if (hit.iItem >= 0 && hit.iSubItem == 2)
            {
                SetDefaultIndex(hit.iItem);
            }
            return 0;
        }

        case NM_DBLCLK:
        {
            LVHITTESTINFO hit = {};
            hit.pt = {GET_X_LPARAM(::GetMessagePos()), GET_Y_LPARAM(::GetMessagePos())};
            ::ScreenToClient(m_hList, &hit.pt);
            ::SendMessageW(m_hList, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));

            // 双击任意一行也是"就用这个程序"
            if (hit.iItem >= 0)
            {
                SetDefaultIndex(hit.iItem);
            }
            return 0;
        }

        case NM_CUSTOMDRAW:
        {
            const LPNMLVCUSTOMDRAW custom = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);

            if (custom->nmcd.dwDrawStage == CDDS_PREPAINT)
            {
                return CDRF_NOTIFYITEMDRAW;
            }

            if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                const int index = static_cast<int>(custom->nmcd.dwItemSpec);

                if (index >= 0 && index < static_cast<int>(m_entry.items.size()))
                {
                    // 已卸载的条目在设置窗口里标灰（菜单里才直接消失）
                    if (m_entry.items[index].alive == 0)
                    {
                        custom->clrText = RGB(150, 150, 150);
                        return CDRF_NEWFONT;
                    }

                    // 「默认」列用圆点表示，选中的画成蓝色
                    if (custom->iSubItem == 2)
                    {
                        const bool isDefault = (index == DefaultIndex());
                        custom->clrText = isDefault ? RGB(0, 95, 184) : RGB(140, 140, 140);
                        return CDRF_NEWFONT;
                    }
                }
            }
            return CDRF_DODEFAULT;
        }

        default:
            break;
        }
        return 0;
    }

    default:
        break;
    }

    return ::DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

// ============================================================================
// 读系统候选 / 列表刷新
// ============================================================================

void OpenWithDialog::RefreshCandidatesFromSystem(bool alsoProbe)
{
    std::vector<core::OpenWithItem> candidates = shell::EnumerateCandidates(m_extension);

    // 先把"这一条是不是这次系统列出来的"全部清掉
    m_inSystemCandidates.assign(m_entry.items.size(), false);

    for (const core::OpenWithItem& candidate : candidates)
    {
        const std::wstring key = core::OpenWithKey(candidate);

        bool found = false;

        // --- 先按身份串匹配（exe 全路径 / AUMID）---
        for (size_t i = 0; i < m_entry.items.size(); ++i)
        {
            if (core::OpenWithKey(m_entry.items[i]) == key)
            {
                // 路径、参数模板、名字都跟着系统更新一次：
                // 这样程序升级换目录时（WPS 版本号就在目录里）能自动跟上，
                // 而勾选状态保留用户原来的选择。
                m_entry.items[i].path = candidate.path;
                m_entry.items[i].args = candidate.args;
                m_entry.items[i].name = candidate.name;

                // ProgID 也补上：老条目可能没记这个（早期版本没存），
                // 而商店应用打开文件时必须靠它
                if (!candidate.progId.empty())
                {
                    m_entry.items[i].progId = candidate.progId;
                }

                m_inSystemCandidates[i] = true;
                found = true;
                break;
            }
        }

        if (found)
        {
            continue;
        }

        // --- 身份串没匹配上，但名字一样、而且旧的那条已经找不到：当作同一条（路径变了）---
        for (size_t i = 0; i < m_entry.items.size(); ++i)
        {
            if (m_entry.items[i].alive == 0 &&
                ::_wcsicmp(m_entry.items[i].name.c_str(), candidate.name.c_str()) == 0)
            {
                m_entry.items[i].path = candidate.path;
                m_entry.items[i].args = candidate.args;
                m_entry.items[i].alive = -1;   // 换了路径，待会儿重新探测

                if (!candidate.progId.empty())
                {
                    m_entry.items[i].progId = candidate.progId;
                }

                m_inSystemCandidates[i] = true;
                found = true;
                break;
            }
        }

        if (found)
        {
            continue;
        }

        // --- 真是新冒出来的：默认勾上 ---
        core::OpenWithItem fresh = candidate;
        fresh.enabled = true;

        m_entry.items.push_back(fresh);
        m_inSystemCandidates.push_back(true);
    }

    if (alsoProbe)
    {
        ProbeAll();
    }

    ReloadList();

    Logger::Info(L"打开方式窗口刷新：扩展名=" + m_extension + L"，系统候选 " +
                 std::to_wstring(candidates.size()) + L" 个，登记 " +
                 std::to_wstring(m_entry.items.size()) + L" 个");
}

void OpenWithDialog::ProbeAll()
{
    // 这里就是"时机 C"：打开设置窗口时把每条都在不在查一遍。
    // 结果会写进配置文件里的 alive 字段，以后右键菜单直接读它，不再探测。
    for (core::OpenWithItem& item : m_entry.items)
    {
        bool diskMissing = false;
        const bool available = shell::IsAvailable(item, diskMissing);
        item.alive = available ? 1 : 0;
    }
}

void OpenWithDialog::ReloadList()
{
    if (m_hList == nullptr)
    {
        return;
    }

    ::SendMessageW(m_hList, WM_SETREDRAW, FALSE, 0);

    // 先把勾选状态存下来：删表会让 LVN_ITEMCHANGED 乱发
    m_syncingChecks = true;

    ListView_DeleteAllItems(m_hList);

    const int defaultIndex = DefaultIndex();

    for (size_t i = 0; i < m_entry.items.size(); ++i)
    {
        const core::OpenWithItem& item = m_entry.items[i];

        const int iconIndex = (m_icons != nullptr)
                                  ? shell::IconIndexFor(item, *m_icons)
                                  : -1;

        LVITEMW row = {};
        row.mask     = LVIF_TEXT | LVIF_PARAM;
        if (iconIndex >= 0)
        {
            row.mask  |= LVIF_IMAGE;
            row.iImage = iconIndex;
        }
        row.iItem    = static_cast<int>(i);
        row.pszText  = const_cast<LPWSTR>(item.name.c_str());
        row.lParam   = static_cast<LPARAM>(i);

        const int inserted = static_cast<int>(
            ::SendMessageW(m_hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&row)));

        if (inserted < 0)
        {
            continue;
        }

        // 状态列
        std::wstring status;
        if (item.alive == 0)
        {
            status = L"已卸载或找不到";
        }
        else if (!m_inSystemCandidates[i])
        {
            status = L"（系统未列出）";
        }

        LVITEMW text = {};
        text.mask    = LVIF_TEXT;
        text.iItem   = inserted;
        text.iSubItem = 1;
        text.pszText = const_cast<LPWSTR>(status.c_str());
        ::SendMessageW(m_hList, LVM_SETITEMTEXTW, inserted, reinterpret_cast<LPARAM>(&text));

        // 默认列
        text.iSubItem = 2;
        text.pszText = const_cast<LPWSTR>(
            (inserted == defaultIndex) ? L"●" : L"○");
        ::SendMessageW(m_hList, LVM_SETITEMTEXTW, inserted, reinterpret_cast<LPARAM>(&text));

        ListView_SetCheckState(m_hList, inserted, item.enabled ? TRUE : FALSE);
    }

    m_syncingChecks = false;

    ::SendMessageW(m_hList, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(m_hList, nullptr, TRUE);
}

void OpenWithDialog::SyncCheckState(int index, bool checked)
{
    if (m_hList == nullptr || index < 0)
    {
        return;
    }

    m_syncingChecks = true;
    ListView_SetCheckState(m_hList, index, checked ? TRUE : FALSE);
    m_syncingChecks = false;
}

// ============================================================================
// 交互
// ============================================================================

void OpenWithDialog::OnCheckChanged(int index, bool checked)
{
    if (index < 0 || index >= static_cast<int>(m_entry.items.size()))
    {
        return;
    }

    m_entry.items[index].enabled = checked;
}

int OpenWithDialog::DefaultIndex() const
{
    if (m_entry.defaultKey.empty())
    {
        return -1;
    }

    for (size_t i = 0; i < m_entry.items.size(); ++i)
    {
        if (core::OpenWithKey(m_entry.items[i]) == m_entry.defaultKey)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void OpenWithDialog::SetDefaultIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entry.items.size()))
    {
        return;
    }

    m_entry.defaultKey = core::OpenWithKey(m_entry.items[index]);

    // 设为默认的程序一定要在菜单里能点到，顺手勾上
    if (!m_entry.items[index].enabled)
    {
        m_entry.items[index].enabled = true;
    }

    ReloadList();
}

void OpenWithDialog::OnAddProgram()
{
    wchar_t fileName[MAX_PATH] = {};

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = m_hWnd;
    dialog.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile   = fileName;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrTitle  = L"选择要用哪个程序打开";
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                         OFN_HIDEREADONLY;

    if (::GetOpenFileNameW(&dialog) == FALSE)
    {
        return;
    }

    core::OpenWithItem item;
    item.kind    = L"exe";
    item.path    = fileName;
    item.enabled = true;

    // 名字用 exe 的文件描述（"记事本"），没有就退文件名
    {
        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(fileName, 0, &info, sizeof(info),
                             SHGFI_DISPLAYNAME | SHGFI_USEFILEATTRIBUTES) &&
            info.szDisplayName[0] != L'\0')
        {
            item.name = info.szDisplayName;
        }
        else
        {
            const wchar_t* shortName = ::PathFindFileNameW(fileName);
            item.name = (shortName != nullptr) ? shortName : std::wstring(L"程序");
        }
    }

    const std::wstring key = core::OpenWithKey(item);

    // 已经有了就不重复加
    for (size_t i = 0; i < m_entry.items.size(); ++i)
    {
        if (core::OpenWithKey(m_entry.items[i]) == key)
        {
            if (m_hList != nullptr)
            {
                ListView_SetItemState(m_hList, static_cast<int>(i),
                                      LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(m_hList, static_cast<int>(i), FALSE);
            }

            ::MessageBoxW(m_hWnd, L"这个程序已经在列表里了。", L"打开方式",
                          MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    m_entry.items.push_back(item);
    m_inSystemCandidates.push_back(false);

    ProbeAll();
    ReloadList();
}

void OpenWithDialog::OnRemoveSelected()
{
    if (m_hList == nullptr)
    {
        return;
    }

    const int selected = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(m_entry.items.size()))
    {
        return;
    }

    const std::wstring removedKey = core::OpenWithKey(m_entry.items[selected]);

    m_entry.items.erase(m_entry.items.begin() + selected);
    if (selected < static_cast<int>(m_inSystemCandidates.size()))
    {
        m_inSystemCandidates.erase(m_inSystemCandidates.begin() + selected);
    }

    // 删掉的正好是默认程序 → 默认也一起清掉（变成"还没设过"）
    if (m_entry.defaultKey == removedKey)
    {
        m_entry.defaultKey.clear();
    }

    ReloadList();
}

void OpenWithDialog::OnButtonClick(int buttonIndex)
{
    switch (buttonIndex)
    {
    case ButtonAdd:
        OnAddProgram();
        return;

    case ButtonRemove:
        OnRemoveSelected();
        return;

    case ButtonRefresh:
        // 手动刷新：重新从系统读一次（新程序默认勾上）
        RefreshCandidatesFromSystem(true);
        return;

    case ButtonSkip:
        // 首次询问里的「跳过」：这次先用系统默认打开，不记录，下次还会问
        m_skipped = true;
        ::SendMessageW(m_hWnd, WM_CLOSE, 0, 0);
        return;

    case ButtonClose:
        ::SendMessageW(m_hWnd, WM_CLOSE, 0, 0);
        return;

    default:
        return;
    }
}

} // namespace ui
