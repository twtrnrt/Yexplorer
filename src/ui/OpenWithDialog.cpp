// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/OpenWithDialog.cpp
//
// 「打开方式」设置窗口的实现。界面说明见 OpenWithDialog.h。
//
// 几个关键决定：
//   1. 按钮自己画（跟主界面那种扁平的按钮保持一致）；
//   2. 列表整行自绘（LVS_OWNERDRAWFIXED）：系统自带复选框会把"选中行"涂成蓝底白字，
//      改不成参考窗口那种浅蓝底，所以复选框、图标、名字、圆点全部自己画；
//   3. 勾选框直接用系统主题画（uxtheme 的 BP_CHECKBOX），跟资源管理器里一模一样；
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
#include <uxtheme.h>
#include <vsstyle.h>   // 主题部件/状态常量（BP_CHECKBOX、CBS_CHECKEDNORMAL…）

#pragma comment(lib, "uxtheme.lib")

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

const int kHintHeight   = 22;   // 列表上方那一行说明字
const int kPadding      = 14;
const int kButtonHeight = 28;

// 列表外面那圈圆角浅灰框（照参考窗口的样子）
const int kFrameRadius  = 6;
const int kFrameInset   = 1;    // 列表控件往里缩 1 像素，别把框线盖住

// 列宽（逻辑像素）：程序名吃满剩余宽度，另外两列固定
const int kStatusColumnWidth = 120;
const int kDefaultColumnWidth = 56;

// 列表行高（逻辑像素）：32px 图标 + 上下各留 2
const int kRowHeight = 36;

// 列表配色（对照参考窗口）
const COLORREF kRowLineColor      = RGB(232, 235, 239);   // 行与行之间的浅灰线
const COLORREF kRowSelectedColor  = RGB(232, 240, 254);   // 选中行的浅蓝底
const COLORREF kRowSelectedLine   = RGB(196, 214, 244);   // 选中行底下的线，稍微深一点
const COLORREF kRowTextColor      = RGB(31, 35, 40);
const COLORREF kRowDimmedText     = RGB(150, 150, 150);   // 已卸载的行标灰
const COLORREF kDotOnColor        = RGB(47, 111, 208);    // 默认程序：实心蓝点
const COLORREF kDotOffColor       = RGB(154, 163, 173);   // 非默认：空心灰圈
const COLORREF kDotOnFill         = RGB(47, 111, 208);

    // 圆角框的边框色：和行分隔线同一个浅灰（用户指定 232,235,239）
const COLORREF kFrameColor    = RGB(232, 235, 239);
const COLORREF kHintTextColor = RGB(96, 103, 111);

// 行分隔线两端离表格左右边框的距离（用户要求 3 像素，别顶着边框）
const int kRowLineInset = 3;

// 列表上方那行说明字：左边一段管勾选，右边一段是「默认程序」列的表头
// （表格自己没有表头，这两个字由窗口画，并且要跟最右那列对齐）
const wchar_t* const kHintLabel    = L"勾选后在右键「打开方式」菜单显示";
const wchar_t* const kDefaultLabel = L"默认程序";

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
    ReleaseRowIcons();

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

bool OpenWithDialog::ShowModal(const std::wstring& extension,
                               bool firstTime,
                               const std::wstring& filePath,
                               bool preferSelf)
{
    m_extension = extension;
    m_filePath  = filePath;
    m_firstTime = firstTime;
    m_preferSelf = preferSelf;
    m_openRequested = false;

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

    // ---- 压缩包（容器）专用：候选里加一条「Yexplorer（本程序）」并默认选它 ----
    //
    // 用户要求：第一次双击压缩包也要弹这个窗口，默认项是"我们自己的文件管理器"，
    // 设过就按设过的。所以这里在系统候选之前插一条 kind="self" 的项，
    // 让它成为默认；用户改选别的程序后，账本里存的就是那个程序。
    if (m_preferSelf)
    {
        core::OpenWithItem selfItem;
        selfItem.kind = L"self";

        // path 只用来取图标（本程序的 exe），身份串由 OpenWithKey 固定成 "self:yexplorer"
        wchar_t modulePath[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        selfItem.path    = modulePath;
        selfItem.name    = L"Yexplorer（本程序）";
        selfItem.enabled = true;
        selfItem.alive   = true;

        // 账本里已经存过本程序这一项就用存过的那份（用户可能改过名字）
        bool selfExists = false;
        for (const core::OpenWithItem& item : m_entry.items)
        {
            if (item.kind == L"self")
            {
                selfExists = true;
                break;
            }
        }

        if (!selfExists)
        {
            m_entry.items.insert(m_entry.items.begin(), selfItem);
        }

        if (m_firstTime && m_entry.defaultKey.empty())
        {
            m_entry.defaultKey = core::OpenWithKey(selfItem);
        }
    }

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

    // 打开窗口这一刻从系统拉到的候选就是账本快照，立刻落盘（用户要求"设置自动保存"）
    SaveNow();

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

    // ---- 写回配置（自动保存：窗口里任何改动都会即时落盘，这里再存一次兜底）----
    core::Settings::Instance().EnsureOpenWith(m_extension) = m_entry;
    core::Settings::Instance().Save();

    Logger::Info(L"打开方式已保存：扩展名=" + m_extension + L"，程序 " +
                 std::to_wstring(m_entry.items.size()) + L" 个，默认=" +
                 (m_entry.defaultKey.empty() ? std::wstring(L"（未设置）")
                                             : m_entry.defaultKey));

    return !m_entry.defaultKey.empty();
}

// ============================================================================
// 把当前工作副本立刻写进配置文件
//
// 用户要求"设置自动保存"：勾选、设默认、删除、加程序之后马上就存，
// 不用等关窗口；这样即使窗口被强制关掉／程序崩了，设置也不会丢。
// ============================================================================
void OpenWithDialog::SaveNow()
{
    if (m_extension.empty())
    {
        return;
    }

    core::Settings::Instance().EnsureOpenWith(m_extension) = m_entry;
    core::Settings::Instance().Save();
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

    // 扩展名：标题里用大写、不带点（"TXT"）
    std::wstring titleExtension = m_extension;

    if (!titleExtension.empty() && titleExtension.front() == L'.')
    {
        titleExtension.erase(titleExtension.begin());
    }

    std::transform(titleExtension.begin(), titleExtension.end(), titleExtension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(::towupper(ch)); });

    // 第一次打开这类文件 → "设置默认打开 TXT 的程序"；平时管理 → "打开方式（TXT）"
    const std::wstring title = m_firstTime
                                   ? (L"设置默认打开" + titleExtension + L"的程序")
                                   : (L"打开方式（" + titleExtension + L"）");

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
    // LVS_OWNERDRAWFIXED：整行由我们自己画。
    // 为什么不用系统自带的复选框 + NM_CUSTOMDRAW：那样"选中行"永远被系统涂成蓝色，
    // 改不成参考窗口那种浅蓝底，圆点也没法自己画。全自绘才完全可控。
    m_hList = ::CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER | LVS_OWNERDRAWFIXED,
        0, 0, 0, 0,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdList)),
        ::GetModuleHandleW(nullptr), nullptr);

    if (m_hList != nullptr)
    {
        ::SendMessageW(m_hList, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

        ::SendMessageW(m_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        // 图标用系统 32px 那一档（程序图标比 16px 的小图标看得清，也不像 48px 那么占地方）
        if (m_icons != nullptr)
        {
            m_iconList = m_icons->ImageList(shell::IconCache::IconSize::Medium);

            ::SendMessageW(m_hList, LVM_SETIMAGELIST, LVSIL_SMALL,
                           reinterpret_cast<LPARAM>(m_iconList));
        }

        LVCOLUMNW column = {};
        column.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.fmt     = LVCFMT_LEFT;
        column.pszText = const_cast<LPWSTR>(L"程序");
        column.cx      = Scale(320);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));

        column.pszText = const_cast<LPWSTR>(L"状态");
        column.cx      = Scale(kStatusColumnWidth);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));

        column.pszText = const_cast<LPWSTR>(L"默认");
        column.cx      = Scale(kDefaultColumnWidth);
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

    // ---- 框（列表住在框里）----
    // 上面只留一行说明字，不再像以前那样"一个大标题 + 一行小字"两行
    const int frameTop    = padding + Scale(kHintHeight) + Scale(8);
    const int frameBottom = clientHeight - padding - Scale(kButtonHeight) - Scale(8);

    m_frameRect.left   = padding;
    m_frameRect.top    = frameTop;
    m_frameRect.right  = clientWidth - padding;
    m_frameRect.bottom = frameBottom;

    // ---- 列表：往里缩 1 像素，让框线露出来 ----
    const int inset = Scale(kFrameInset);

    m_listRect.left   = m_frameRect.left + inset;
    m_listRect.top    = m_frameRect.top + inset;
    m_listRect.right  = m_frameRect.right - inset;
    m_listRect.bottom = m_frameRect.bottom - inset;

    // 框是直角，列表不用裁形状，直接摆进框里（比框小 1px，别盖住框线）
    ::MoveWindow(m_hList,
                 m_listRect.left, m_listRect.top,
                 m_listRect.right - m_listRect.left,
                 m_listRect.bottom - m_listRect.top,
                 TRUE);

    // 程序名列吃满剩余宽度，另外两列固定。
    // 只有内容超过可视高度（真会出现竖滚动条）时才给它留宽度，
    // 否则右边会空出一条——用户看到的"表格右侧 51px 空白"就是这个。
    const int listWidth  = m_listRect.right - m_listRect.left;
    const int listHeight = m_listRect.bottom - m_listRect.top;

    const int rowCount = static_cast<int>(m_entry.items.size());
    const bool needScrollBar = (rowCount * Scale(kRowHeight)) > (listHeight - Scale(2));
    const int scrollWidth = needScrollBar ? ::GetSystemMetrics(SM_CXVSCROLL) : 0;

    const int nameWidth = listWidth - scrollWidth -
                          Scale(kStatusColumnWidth) - Scale(kDefaultColumnWidth);

    ::SendMessageW(m_hList, LVM_SETCOLUMNWIDTH, 0, std::max(Scale(160), nameWidth));

    // ---- 「默认程序」四个字：正对最右那列居中 ----
    // 位置按列表里"真实的列宽"算，别自己猜（猜错了就会偏）
    const int actualNameWidth =
        static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 0, 0));
    const int actualStatusWidth =
        static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 1, 0));

    const int defaultColumnLeft = m_listRect.left + actualNameWidth + actualStatusWidth;
    const int defaultColumnWidth =
        static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 2, 0));

    const int defaultColumnCenter = defaultColumnLeft + defaultColumnWidth / 2;

    // 文字宽度要用真实字体量（拿空 DC + DrawText 量出来是 0，
    // 之前就出现过"右边只剩一个竖条"的现象）
    int labelWidth = Scale(50);

    HDC measureDc = ::GetDC(m_hWnd);
    if (measureDc != nullptr)
    {
        HGDIOBJ oldFont = (m_font != nullptr) ? ::SelectObject(measureDc, m_font) : nullptr;

        SIZE textSize = {};
        if (::GetTextExtentPoint32W(measureDc, kDefaultLabel,
                                    static_cast<int>(::wcslen(kDefaultLabel)), &textSize))
        {
            labelWidth = textSize.cx + Scale(2);
        }

        if (oldFont != nullptr)
        {
            ::SelectObject(measureDc, oldFont);
        }
        ::ReleaseDC(m_hWnd, measureDc);
    }

    m_defaultLabelRect.left   = defaultColumnCenter - labelWidth / 2;
    m_defaultLabelRect.right  = m_defaultLabelRect.left + labelWidth;
    m_defaultLabelRect.top    = padding;
    m_defaultLabelRect.bottom = padding + Scale(kHintHeight);

    // ---- 按钮：左边一组（添加程序/删除/刷新）靠左，右边一组（打开/关闭）靠右 ----
    m_buttonCount = 0;

    const int widthAdd    = Scale(104);
    const int widthSmall  = Scale(64);
    const int widthOpen   = Scale(76);
    const int gap         = Scale(8);
    const int buttonY     = clientHeight - padding - Scale(kButtonHeight);
    const int buttonHeight = Scale(kButtonHeight);

    // 左边：从左往右添加程序、删除、刷新
    int leftEdge = padding;

    m_buttonRects[ButtonAdd].left  = leftEdge;
    m_buttonRects[ButtonAdd].right = leftEdge + widthAdd;
    leftEdge = m_buttonRects[ButtonAdd].right + gap;

    m_buttonRects[ButtonRemove].left  = leftEdge;
    m_buttonRects[ButtonRemove].right = leftEdge + widthSmall;
    leftEdge = m_buttonRects[ButtonRemove].right + gap;

    m_buttonRects[ButtonRefresh].left  = leftEdge;
    m_buttonRects[ButtonRefresh].right = leftEdge + widthSmall;

    // 右边：从右往左关闭、打开
    int rightEdge = clientWidth - padding;

    m_buttonRects[ButtonClose].right = rightEdge;
    m_buttonRects[ButtonClose].left  = rightEdge - widthSmall;
    rightEdge = m_buttonRects[ButtonClose].left - gap;

    m_buttonRects[ButtonOpen].right = rightEdge;
    m_buttonRects[ButtonOpen].left  = rightEdge - widthOpen;

    for (int index = 0; index < ButtonCount; ++index)
    {
        m_buttonRects[index].top    = buttonY;
        m_buttonRects[index].bottom = buttonY + buttonHeight;
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

    // ---- 列表上方那一行说明字（照参考窗口：左一段、右一段，右段正对默认列）----
    ::SetTextColor(dc, kHintTextColor);

    RECT hintRect = { padding, padding, m_defaultLabelRect.left - Scale(8),
                      padding + Scale(kHintHeight) };
    ::DrawTextW(dc, kHintLabel, -1, &hintRect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    ::DrawTextW(dc, kDefaultLabel, -1, const_cast<LPRECT>(&m_defaultLabelRect),
                DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // ---- 列表外那圈圆角浅灰框 ----
    DrawRoundFrame(dc);

    // ---- 按钮 ----
    for (int index = 0; index < ButtonCount; ++index)
    {
        DrawFlatButton(dc, index);
    }
}

// ============================================================================
// 自绘一整行（LVS_OWNERDRAWFIXED 下 WM_DRAWITEM 调到这里）
//
// 一行从左到右是：复选框 | 32px 程序图标 | 程序名 | 状态 | 默认圆点
// 参考窗口里"选中行"是浅蓝底 + 深色字，所以不能用系统那套蓝底白字，必须自己铺底色。
// ============================================================================
void OpenWithDialog::DrawRow(HDC dc, const RECT& rowRect, int index, bool selected)
{
    if (index < 0 || index >= static_cast<int>(m_entry.items.size()))
    {
        return;
    }

    const core::OpenWithItem& item = m_entry.items[index];
    const bool dimmed = (item.alive == 0);   // 已卸载的行整体标灰

    // ---- 底色 ----
    HBRUSH background = ::CreateSolidBrush(selected ? kRowSelectedColor : RGB(255, 255, 255));
    ::FillRect(dc, &rowRect, background);
    ::DeleteObject(background);

    // ---- 行底下那条线（两端各离表格边框 3 像素，别顶着边框）----
    RECT listClient = {};
    ::GetClientRect(m_hList, &listClient);

    const int lineInset = Scale(kRowLineInset);

    HPEN linePen = ::CreatePen(PS_SOLID, 1, selected ? kRowSelectedLine : kRowLineColor);
    HGDIOBJ oldPen = ::SelectObject(dc, linePen);
    ::MoveToEx(dc, listClient.left + lineInset, rowRect.bottom - 1, nullptr);
    ::LineTo(dc, listClient.right - lineInset, rowRect.bottom - 1);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(linePen);

    const int rowHeight = rowRect.bottom - rowRect.top;

    // ---- 复选框 ----
    const int checkLeft = Scale(8);
    const int checkSize = Scale(14);
    RECT checkBox = { checkLeft, rowRect.top + (rowHeight - checkSize) / 2,
                      checkLeft + checkSize, rowRect.top + (rowHeight - checkSize) / 2 + checkSize };
    DrawCheckBox(dc, checkBox, item.enabled, dimmed);

    // ---- 图标（24px，垂直居中；行高不变，所以上下留白多了一点）----
    const int iconLeft = Scale(30);
    const int iconSize = Scale(24);
    const HICON icon = (index < static_cast<int>(m_rowIcons.size())) ? m_rowIcons[index] : nullptr;

    if (icon != nullptr)
    {
        ::DrawIconEx(dc, iconLeft, rowRect.top + (rowHeight - iconSize) / 2, icon,
                     iconSize, iconSize, 0, nullptr, DI_NORMAL);
    }

    ::SetBkMode(dc, TRANSPARENT);

    // ---- 程序名 ----
    const int nameLeft = iconLeft + iconSize + Scale(8);
    const int nameColumnWidth = static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 0, 0));
    const int statusLeft = nameColumnWidth;

    RECT nameRect = { nameLeft, rowRect.top, statusLeft - Scale(4), rowRect.bottom - 1 };
    ::SetTextColor(dc, dimmed ? kRowDimmedText : kRowTextColor);
    ::DrawTextW(dc, item.name.c_str(), -1, &nameRect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // ---- 状态 ---- 只标"已卸载或找不到"；"系统未列出"这种提示用户说不用显示
    std::wstring status;
    if (item.alive == 0)
    {
        status = L"已卸载或找不到";
    }

    const int statusColumnWidth = static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 1, 0));
    const int dotLeft = statusLeft + statusColumnWidth;
    const int dotWidth =
        static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 2, 0));

    RECT statusRect = { statusLeft, rowRect.top, dotLeft - Scale(4), rowRect.bottom - 1 };
    ::SetTextColor(dc, kRowDimmedText);
    ::DrawTextW(dc, status.c_str(), -1, &statusRect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // ---- 默认圆点 ----
    RECT dotRect = { dotLeft, rowRect.top, dotLeft + dotWidth, rowRect.bottom - 1 };
    DrawDefaultDot(dc, dotRect, (index == DefaultIndex()), selected);
}

// ============================================================================
// 自绘复选框
//
// 优先用系统主题（uxtheme）画，跟资源管理器里那个勾选框一模一样：
// 白底、细灰边、蓝/黑勾。只有在主题不可用（经典外观）时才退回手画。
// ============================================================================
void OpenWithDialog::DrawCheckBox(HDC dc, const RECT& box, bool checked, bool dimmed)
{
    // 主题里的勾选框比我们自己量的框小一圈，整体往里收一点才不至于撑满
    RECT themed = box;
    ::InflateRect(&themed, -Scale(1), -Scale(1));

    HTHEME theme = ::OpenThemeData(m_hWnd, L"Button");

    if (theme != nullptr)
    {
        int state = 0;

        if (dimmed)
        {
            state = checked ? CBS_CHECKEDDISABLED : CBS_UNCHECKEDDISABLED;
        }
        else
        {
            state = checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
        }

        ::DrawThemeBackground(theme, dc, BP_CHECKBOX, state, &themed, nullptr);
        ::CloseThemeData(theme);
        return;
    }

    // ---- 没有主题时的手画版本 ----
    HPEN pen = ::CreatePen(PS_SOLID, 1, dimmed ? RGB(190, 190, 190) : RGB(120, 120, 120));
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(WHITE_BRUSH));
    ::Rectangle(dc, box.left, box.top, box.right, box.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    if (!checked)
    {
        return;
    }

    // 对勾：两段 1 像素线，位置按方框比例算
    const int width  = box.right - box.left;
    const int height = box.bottom - box.top;

    HPEN markPen = ::CreatePen(PS_SOLID, 1, dimmed ? RGB(150, 150, 150) : RGB(60, 60, 60));
    oldPen = ::SelectObject(dc, markPen);

    ::MoveToEx(dc, box.left + width / 4, box.top + height / 2, nullptr);
    ::LineTo(dc, box.left + width * 2 / 5, box.top + height * 3 / 4);
    ::LineTo(dc, box.right - width / 5, box.top + height / 4);

    ::SelectObject(dc, oldPen);
    ::DeleteObject(markPen);
}

// ============================================================================
// 画「默认」列那个圆点
//
// 不用 ●/○ 字符：字符大小跟着字体走，跟最右那列对不齐也看不出"是个单选按钮"。
// 自己画直径十几像素的圆：默认 = 实心蓝点，非默认 = 空心灰圈。
// 因为这一列整列跳过了系统绘制，底色得自己先铺一遍。
// ============================================================================
void OpenWithDialog::DrawDefaultDot(HDC dc, const RECT& cell, bool isDefault, bool isSelected)
{
    HBRUSH background = ::CreateSolidBrush(isSelected ? kRowSelectedColor : RGB(255, 255, 255));
    ::FillRect(dc, &cell, background);
    ::DeleteObject(background);

    const int diameter = Scale(14);
    const int centerX  = (cell.left + cell.right) / 2;
    const int centerY  = (cell.top + cell.bottom) / 2;

    RECT circle = { centerX - diameter / 2, centerY - diameter / 2,
                    centerX + diameter / 2, centerY + diameter / 2 };

    HPEN ringPen = ::CreatePen(PS_SOLID, 1, isDefault ? kDotOnColor : kDotOffColor);
    HGDIOBJ oldPen  = ::SelectObject(dc, ringPen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::Ellipse(dc, circle.left, circle.top, circle.right, circle.bottom);

    if (isDefault)
    {
        // 实心内点
        const int inner = (diameter / 2) - 1;

        HBRUSH fillBrush = ::CreateSolidBrush(kDotOnFill);
        ::SelectObject(dc, fillBrush);
        ::SelectObject(dc, ringPen);

        ::Ellipse(dc, centerX - inner, centerY - inner,
                  centerX + inner, centerY + inner);

        ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::DeleteObject(fillBrush);
    }

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(ringPen);
}

// ============================================================================
// 画列表外面那圈框
//
// 原来是圆角，反复调还是不对劲（圆角在列表白底上接缝明显），用户拍板"直接直角好了"。
// 直角 1 像素线最稳，颜色跟上面那行说明字一样（灰 RGB(96,103,111)）。
// ============================================================================
void OpenWithDialog::DrawRoundFrame(HDC dc)
{
    if (m_frameRect.right <= m_frameRect.left || m_frameRect.bottom <= m_frameRect.top)
    {
        return;
    }

    ::SetBkMode(dc, TRANSPARENT);

    HPEN pen = ::CreatePen(PS_SOLID, 1, kFrameColor);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::Rectangle(dc, m_frameRect.left, m_frameRect.top, m_frameRect.right, m_frameRect.bottom);

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void OpenWithDialog::DrawFlatButton(HDC dc, int index)
{
    const RECT& rect = m_buttonRects[index];
    if (rect.right <= rect.left)
    {
        return;
    }

    // 照参考窗口的样子画成"系统标准按钮"：白底 + 1px 灰边 + 悬停/按下变色。
    // （项目里其它地方是纯扁平按钮，但这是个弹窗，跟参考窗口一致更顺眼）
    COLORREF fill   = RGB(252, 252, 252);
    COLORREF border = RGB(173, 173, 173);

    if (m_pressedButton == index)
    {
        fill   = RGB(212, 214, 219);
        border = RGB(140, 140, 140);
    }
    else if (m_hotButton == index)
    {
        fill   = RGB(240, 242, 245);
        border = RGB(120, 150, 200);
    }

    HBRUSH brush = ::CreateSolidBrush(fill);
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);

    // 边框：矩形向下取整到 rect 内侧（gdi 的 right/bottom 是开区间）
    HPEN pen = ::CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    const wchar_t* texts[ButtonCount] = {
        L"添加程序…", L"删除", L"刷新", L"打开", L"关闭",
    };

    ::SetTextColor(dc, RGB(32, 32, 32));

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

    case WM_MEASUREITEM:
    {
        // 自绘列表的行高由我们说了算（ListView 自己不量）
        MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);

        if (measure != nullptr && measure->CtlID == static_cast<UINT>(kControlIdList))
        {
            measure->itemHeight = static_cast<UINT>(Scale(kRowHeight));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);

        if (draw != nullptr && draw->CtlID == static_cast<UINT>(kControlIdList) &&
            draw->itemID != static_cast<UINT>(-1))
        {
            DrawRow(draw->hDC, draw->rcItem, static_cast<int>(draw->itemID),
                    (draw->itemState & ODS_SELECTED) != 0);
            return TRUE;
        }
        break;
    }

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

        // 空格：切换当前选中那一行的勾选（跟参考窗口里空格切复选框一个意思）
        if (wParam == VK_SPACE && m_hList != nullptr)
        {
            const int selected = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
            if (selected >= 0 && selected < static_cast<int>(m_entry.items.size()))
            {
                m_entry.items[selected].enabled = !m_entry.items[selected].enabled;
                SaveNow();
                ListView_RedrawItems(m_hList, selected, selected);
                ::UpdateWindow(m_hList);
            }
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
        case NM_CLICK:
        {
            // NM_CLICK 带的是 NMITEMACTIVATE，里面直接有"点了哪一行、点的坐标"。
            // 不要自己用 GetMessagePos 去算：那是全局鼠标位置，跟这个通知不是一回事
            const NMITEMACTIVATE* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
            const int index = activate->iItem;
            const int clickX = activate->ptAction.x;

            if (index < 0)
            {
                return 0;
            }

            const int nameColumnWidth =
                static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 0, 0));
            const int statusColumnWidth =
                static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 1, 0));
            const int dotColumnLeft = nameColumnWidth + statusColumnWidth;

            // 整行都是自绘的，所以按点击的横坐标分区判断：
            //   最左边一小块 = 复选框；最右一列 = 默认圆点；其余地方只选中不改状态
            if (clickX < Scale(28))
            {
                if (index < static_cast<int>(m_entry.items.size()))
                {
                    m_entry.items[index].enabled = !m_entry.items[index].enabled;
                    SaveNow();       // 勾选即存（自动保存）
                    ListView_RedrawItems(m_hList, index, index);
                    ::UpdateWindow(m_hList);
                }
            }
            else if (clickX >= dotColumnLeft)
            {
                SetDefaultIndex(index);
            }

            return 0;
        }

        case NM_DBLCLK:
        {
            const NMITEMACTIVATE* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);

            // 双击任意一行也是"就用这个程序"
            if (activate->iItem >= 0)
            {
                SetDefaultIndex(activate->iItem);
            }
            return 0;
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

    ReleaseRowIcons();
    m_rowIcons.reserve(m_entry.items.size());

    for (size_t i = 0; i < m_entry.items.size(); ++i)
    {
        const core::OpenWithItem& item = m_entry.items[i];

        // 图标先拿系统 32px 那一档，再缩成 24px（参考窗口里的图标就是 24 的观感，
        // 但 SHGetFileInfo 没有 24 这一档，只能从 32 缩下来，效果跟系统一致）
        const int iconIndex = (m_icons != nullptr)
                                  ? shell::IconIndexFor(item, *m_icons,
                                                        shell::IconCache::IconSize::Medium)
                                  : -1;

        HICON icon = nullptr;

        if (m_iconList != nullptr && iconIndex >= 0)
        {
            HICON source = ::ImageList_GetIcon(m_iconList, iconIndex, ILD_TRANSPARENT);

            if (source != nullptr)
            {
                icon = reinterpret_cast<HICON>(
                    ::CopyImage(source, IMAGE_ICON, Scale(24), Scale(24), 0));
                ::DestroyIcon(source);
            }
        }

        m_rowIcons.push_back(icon);

        // 整行都是自绘的：ListView 里只挂一个"行存在"的记号（lParam 就是下标），
        // 文字、图标都不交给它，避免两边各画一份
        LVITEMW row = {};
        row.mask     = LVIF_PARAM;
        row.iItem    = static_cast<int>(i);
        row.lParam   = static_cast<LPARAM>(i);

        const int inserted = static_cast<int>(
            ::SendMessageW(m_hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&row)));

        if (inserted < 0)
        {
            continue;
        }

        // 状态列 / 默认列的文字不用塞进 ListView：整行都是我们自己画的，
        // 内容直接从 m_entry.items 里取，塞一份反而会出现"两边不一致"的问题
    }

    m_syncingChecks = false;

    ::SendMessageW(m_hList, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(m_hList, nullptr, TRUE);
}

// ============================================================================
// 释放每行的 24px 图标
// ============================================================================
void OpenWithDialog::ReleaseRowIcons()
{
    for (HICON icon : m_rowIcons)
    {
        if (icon != nullptr)
        {
            ::DestroyIcon(icon);
        }
    }

    m_rowIcons.clear();
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

    SaveNow();       // 唯一改动就即时落盘
    ReloadList();
}

void OpenWithDialog::OnAddProgram()
{
    // 直接弹系统的 exe 选择框（用户明确要求：不必为"挑一个程序"单独做一个窗口）
    wchar_t fileName[MAX_PATH] = L"";

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
    SaveNow();
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

    SaveNow();
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

    case ButtonOpen:
        // 「打开」：保存设置，然后用默认程序打开这个文件
        m_openRequested = true;
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
