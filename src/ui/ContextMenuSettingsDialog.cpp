// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuSettingsDialog.cpp
//
// 「配置右键菜单」窗口实现。界面说明见 ContextMenuSettingsDialog.h。
//
// 数据链路：
//   · 打开窗口：先读 menu-cache.json 缓存（含 menu-icons 里的渲染图）；
//   · 点「刷新」：CollectScenes 重新采集 → SaveCache → 重建列表。
//
// 列表用自绘 ListView：每一行贴采集时外壳扩展画好的位图，
// 所以图标和文字都跟系统右键菜单一模一样。
// ============================================================================

#include "ui/ContextMenuSettingsDialog.h"

#include "core/Logger.h"
#include "shell/ContextMenuCollector.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>

#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ui
{

namespace
{

const wchar_t* const kClassName = L"FileExplorerContextMenuSettingsDialog";

const int kControlIdStatus  = 2201;
const int kControlIdRefresh = 2202;
const int kControlIdList    = 2203;
const int kControlIdTab     = 2204;
const int kControlIdCombo   = 2205;

// 窗口客户区尺寸（逻辑像素）
const int kDialogWidth  = 760;
const int kDialogHeight = 620;
const int kPadding      = 12;
const int kButtonHeight = 28;
const int kButtonWidth  = 88;
const int kTopRowHeight = 34;
const int kTabHeight    = 26;
const int kComboHeight  = 26;

// 列表行高（逻辑像素）——和渲染图高度（26）对齐
const int kRowHeight = 26;

const COLORREF kRowTextColor   = RGB(31, 35, 40);
const COLORREF kSeparatorColor = RGB(222, 226, 230);
const COLORREF kFailTextColor  = RGB(178, 34, 34);

// 去掉 "文件 · " 之类的前缀，样本下拉里只显示样本名
std::wstring ShortSampleLabel(const std::wstring& label)
{
    const size_t pos = label.find(L"·");
    if (pos == std::wstring::npos)
    {
        return label;
    }
    std::wstring tail = label.substr(pos + 1);
    const size_t begin = tail.find_first_not_of(L" ");
    return begin == std::wstring::npos ? tail : tail.substr(begin);
}

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

ContextMenuSettingsDialog::ContextMenuSettingsDialog(HWND owner)
{
    m_hOwner = owner;
    SetDpiSource(owner);
}

ContextMenuSettingsDialog::~ContextMenuSettingsDialog()
{
    ReleaseBitmaps();
    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
        m_font = nullptr;
    }
}

// ============================================================================
// 窗口类
// ============================================================================

bool ContextMenuSettingsDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }
    if (!RegisterWindowClass(kClassName, ::GetModuleHandleW(nullptr), CS_DBLCLKS))
    {
        return false;
    }
    registered = true;
    return true;
}

// ============================================================================
// 显示（模态）
// ============================================================================

void ContextMenuSettingsDialog::ShowModal()
{
    // 先读缓存（缓存里有上次采集的图和文字，秒开）；
    // 没有缓存时先把窗口显示出来，再在消息循环里采集，避免"点了按钮半天没反应"。
    const bool hasCache = shell::LoadCache(m_report) && !m_report.scenes.empty();

    if (!CreateDialogWindow())
    {
        return;
    }

    if (!hasCache)
    {
        ::SetWindowTextW(m_hStatus, L"正在从系统采集右键菜单，请稍候…");
        ::UpdateWindow(m_hWnd);
        ::PostMessageW(m_hWnd, WM_APP + 1, 0, 0);   // 交给消息循环去采集
    }

    RunModalLoop(m_hOwner, m_hList);
}

// ============================================================================
// 建窗
// ============================================================================

bool ContextMenuSettingsDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    INITCOMMONCONTROLSEX cc = {};
    cc.dwSize = sizeof(cc);
    cc.dwICC  = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&cc);

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (m_font == nullptr)
    {
        m_font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    const int width  = Scale(kDialogWidth);
    const int height = Scale(kDialogHeight);

    // 把客户区尺寸换算成窗口尺寸（含标题栏边框）
    RECT windowRect = { 0, 0, width, height };
    ::AdjustWindowRectEx(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                         WS_EX_DLGMODALFRAME);
    const int windowW = windowRect.right - windowRect.left;
    const int windowH = windowRect.bottom - windowRect.top;

    RECT ownerRect = {};
    if (m_hOwner != nullptr)
    {
        ::GetWindowRect(m_hOwner, &ownerRect);
    }
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - windowW) / 2;
    const int y = ownerRect.top  + ((ownerRect.bottom - ownerRect.top) - windowH) / 2;

    m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                               kClassName,
                               L"配置右键菜单",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, windowW, windowH,
                               m_hOwner,
                               nullptr,
                               ::GetModuleHandleW(nullptr),
                               this);

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"配置右键菜单窗口创建失败");
        return false;
    }

    CreateControls();
    Layout();
    ReloadList();
    return true;
}

void ContextMenuSettingsDialog::CreateControls()
{
    // 顶部状态标签
    m_hStatus = ::CreateWindowExW(0, WC_STATICW, L"",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                  0, 0, 0, 0, m_hWnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdStatus)),
                                  ::GetModuleHandleW(nullptr), nullptr);

    // 刷新按钮
    m_hRefresh = ::CreateWindowExW(0, WC_BUTTONW, L"刷新",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 0, 0, m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdRefresh)),
                                   ::GetModuleHandleW(nullptr), nullptr);

    // 四个选项卡
    m_hTab = ::CreateWindowExW(0, WC_TABCONTROLW, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               0, 0, 0, 0, m_hWnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdTab)),
                               ::GetModuleHandleW(nullptr), nullptr);

    if (m_hTab != nullptr)
    {
        const wchar_t* tabNames[] = { L"文件", L"文件夹", L"文件夹空白区", L"多选文件" };
        for (int i = 0; i < ARRAYSIZE(tabNames); ++i)
        {
            TCITEMW tabItem = {};
            tabItem.mask = TCIF_TEXT;
            tabItem.pszText = const_cast<LPWSTR>(tabNames[i]);
            ::SendMessageW(m_hTab, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&tabItem));
        }
    }

    // 文件选项卡的样本下拉（txt / docx / rar / zip）
    m_hCombo = ::CreateWindowExW(0, WC_COMBOBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 0, 0, m_hWnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdCombo)),
                                 ::GetModuleHandleW(nullptr), nullptr);

    // 列表（自绘行）
    m_hList = ::CreateWindowExW(0, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER |
                                    LVS_NOSORTHEADER | LVS_OWNERDRAWFIXED,
                                0, 0, 0, 0, m_hWnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdList)),
                                ::GetModuleHandleW(nullptr), nullptr);

    if (m_hList != nullptr)
    {
        ::SendMessageW(m_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"");
        column.cx = Scale(1200);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    }

    for (HWND ctrl : { m_hStatus, m_hRefresh, m_hTab, m_hCombo, m_hList })
    {
        if (ctrl != nullptr)
        {
            ::SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        }
    }

    // 填充文件样本下拉
    if (m_hCombo != nullptr)
    {
        for (const shell::SceneMenu& scene : m_report.scenes)
        {
            if (scene.scene == shell::SceneFile)
            {
                const std::wstring shortLabel = ShortSampleLabel(scene.sampleLabel);
                ::SendMessageW(m_hCombo, CB_ADDSTRING, 0,
                               reinterpret_cast<LPARAM>(shortLabel.c_str()));
            }
        }
        ::SendMessageW(m_hCombo, CB_SETCURSEL, 0, 0);
    }
}

void ContextMenuSettingsDialog::Layout()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    RECT client = {};
    ::GetClientRect(m_hWnd, &client);

    const int padding = Scale(kPadding);
    const int buttonW = Scale(kButtonWidth);
    const int buttonH = Scale(kButtonHeight);
    const int topH    = Scale(kTopRowHeight);
    const int tabH    = Scale(kTabHeight);
    const int comboH  = Scale(kComboHeight);

    // 状态 + 刷新
    ::SetWindowPos(m_hRefresh, nullptr,
                   client.right - padding - buttonW, padding,
                   buttonW, buttonH, SWP_NOZORDER);

    ::SetWindowPos(m_hStatus, nullptr,
                   padding, padding + (buttonH - Scale(16)) / 2,
                   client.right - padding * 2 - buttonW - Scale(10), Scale(16),
                   SWP_NOZORDER);

    // 选项卡
    const int tabTop = padding + topH;
    ::SetWindowPos(m_hTab, nullptr,
                   padding, tabTop,
                   client.right - padding * 2, tabH, SWP_NOZORDER);

    // 样本下拉：只在文件选项卡显示，放在选项卡下方右侧
    const int comboTop = tabTop + tabH + Scale(4);
    int listTop = comboTop + comboH + Scale(4);
    if (m_hCombo != nullptr)
    {
        ::SetWindowPos(m_hCombo, nullptr,
                       padding, comboTop, Scale(220), comboH + Scale(160),
                       SWP_NOZORDER);
    }

    // 列表
    ::SetWindowPos(m_hList, nullptr,
                   padding, listTop,
                   client.right - padding * 2,
                   client.bottom - padding - listTop,
                   SWP_NOZORDER);
}

// ============================================================================
// 场景选择
// ============================================================================

int ContextMenuSettingsDialog::SceneIndexForTab(int tab) const
{
    // 选项卡 → 场景位
    unsigned int wanted = shell::SceneNone;
    switch (tab)
    {
    case 0: wanted = shell::SceneFile;       break;
    case 1: wanted = shell::SceneFolder;     break;
    case 2: wanted = shell::SceneBackground; break;
    case 3: wanted = shell::SceneMultiple;   break;
    default: return -1;
    }

    // 文件选项卡：按样本下拉选第几个
    if (wanted == shell::SceneFile && m_hCombo != nullptr)
    {
        const int sel = static_cast<int>(::SendMessageW(m_hCombo, CB_GETCURSEL, 0, 0));
        int seen = 0;
        for (int i = 0; i < static_cast<int>(m_report.scenes.size()); ++i)
        {
            if (m_report.scenes[i].scene != shell::SceneFile)
            {
                continue;
            }
            if (seen == sel)
            {
                return i;
            }
            ++seen;
        }
        return -1;
    }

    for (int i = 0; i < static_cast<int>(m_report.scenes.size()); ++i)
    {
        if (m_report.scenes[i].scene == wanted)
        {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// 列表内容
// ============================================================================

void ContextMenuSettingsDialog::ReleaseBitmaps()
{
    for (HBITMAP bitmap : m_rowBitmaps)
    {
        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
        }
    }
    m_rowBitmaps.clear();
}

void ContextMenuSettingsDialog::ReloadList()
{
    if (m_hList == nullptr)
    {
        return;
    }

    ReleaseBitmaps();
    m_rows.clear();
    m_rowLevels.clear();

    ::SendMessageW(m_hList, LVM_DELETEALLITEMS, 0, 0);

    const int tab = m_hTab != nullptr
                        ? static_cast<int>(::SendMessageW(m_hTab, TCM_GETCURSEL, 0, 0))
                        : 0;

    // 样本下拉只在文件选项卡可见
    if (m_hCombo != nullptr)
    {
        ::ShowWindow(m_hCombo, tab == 0 ? SW_SHOW : SW_HIDE);
        Layout();
    }

    m_sceneIndex = SceneIndexForTab(tab);

    const std::wstring iconDir = shell::IconDirPath();

    std::wstring status;
    if (m_report.collectedAt.empty())
    {
        status = L"暂无缓存，点「刷新」从系统采集";
    }
    else
    {
        status = L"最近采集：" + m_report.collectedAt;
    }

    if (m_sceneIndex < 0 || m_sceneIndex >= static_cast<int>(m_report.scenes.size()))
    {
        ::SetWindowTextW(m_hStatus, (status + L"　（该场景没有数据）").c_str());
        return;
    }

    const shell::SceneMenu& scene = m_report.scenes[m_sceneIndex];

    int shown = 0;
    int noImage = 0;

    for (const shell::ContextMenuItem& item : scene.items)
    {
        m_rows.push_back(&item);
        m_rowLevels.push_back(item.level);

        HBITMAP bitmap = nullptr;
        if (!item.imageFile.empty())
        {
            const std::wstring path = iconDir + L"\\" + item.imageFile;
            bitmap = static_cast<HBITMAP>(
                ::LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0,
                             LR_LOADFROMFILE | LR_CREATEDIBSECTION));
        }
        if (bitmap == nullptr)
        {
            ++noImage;
        }
        m_rowBitmaps.push_back(bitmap);

        if (!item.isSeparator)
        {
            ++shown;
        }
    }

    // 建行：报告模式的自绘列表必须逐条插入（LVM_SETITEMCOUNT 只对虚拟列表有效）
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
    {
        LVITEMW lvItem = {};
        lvItem.mask  = LVIF_TEXT;
        lvItem.iItem = i;
        lvItem.pszText = const_cast<LPWSTR>(L"");
        ::SendMessageW(m_hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvItem));
    }

    // 状态栏
    status += L"　" + scene.sampleLabel + L"：" +
              std::to_wstring(shown) + L" 项";
    if (noImage > 0)
    {
        status += L"，其中 " + std::to_wstring(noImage) + L" 条没取到图（只显示文字）";
    }
    if (!scene.ok && !scene.failReason.empty())
    {
        status += L"　采集失败：" + scene.failReason;
    }
    ::SetWindowTextW(m_hStatus, status.c_str());

    ::InvalidateRect(m_hList, nullptr, TRUE);
}

// ============================================================================
// 自绘行
// ============================================================================

void ContextMenuSettingsDialog::DrawSeparatorRow(HDC dc, const RECT& rowRect)
{
    const int y = (rowRect.top + rowRect.bottom) / 2;
    HPEN pen = ::CreatePen(PS_SOLID, 1, kSeparatorColor);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::MoveToEx(dc, rowRect.left + Scale(8), y, nullptr);
    ::LineTo(dc, rowRect.right - Scale(8), y);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void ContextMenuSettingsDialog::DrawRow(HDC dc, const RECT& rowRect, int index)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size()))
    {
        return;
    }

    const shell::ContextMenuItem& item = *m_rows[index];
    const int level = m_rowLevels[index];

    if (item.isSeparator)
    {
        DrawSeparatorRow(dc, rowRect);
        return;
    }

    const int indent = Scale(8) + Scale(18) * level;
    const int rowHeight = rowRect.bottom - rowRect.top;

    HBITMAP bitmap = (index < static_cast<int>(m_rowBitmaps.size()))
                         ? m_rowBitmaps[index] : nullptr;

    if (bitmap != nullptr)
    {
        // 渲染图是 96 DPI 逻辑尺寸，按当前 DPI 等比缩放
        BITMAP info = {};
        if (::GetObjectW(bitmap, sizeof(info), &info) == sizeof(info))
        {
            const int targetHeight = rowHeight - Scale(2);
            const int targetWidth = info.bmWidth * targetHeight / (info.bmHeight > 0 ? info.bmHeight : 1);

            HDC memDc = ::CreateCompatibleDC(dc);
            HGDIOBJ oldBitmap = ::SelectObject(memDc, bitmap);
            ::SetStretchBltMode(dc, HALFTONE);
            ::StretchBlt(dc, rowRect.left + indent, rowRect.top + Scale(1),
                         targetWidth, targetHeight,
                         memDc, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
            ::SelectObject(memDc, oldBitmap);
            ::DeleteDC(memDc);
        }
        return;
    }

    // 没有渲染图：只画文字（左缩进 + 可选图标区留白）
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, item.failed ? kFailTextColor : kRowTextColor);
    HGDIOBJ oldFont = ::SelectObject(dc, m_font);

    std::wstring text = item.text.empty() ? L"（无文字）" : item.text;
    if (item.failed && !item.failReason.empty())
    {
        text += L"　（" + item.failReason + L"）";
    }

    RECT textRect = { rowRect.left + indent + Scale(2), rowRect.top,
                      rowRect.right - Scale(6), rowRect.bottom };
    ::DrawTextW(dc, text.c_str(), -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    ::SelectObject(dc, oldFont);
}

// ============================================================================
// 刷新
// ============================================================================

void ContextMenuSettingsDialog::Refresh()
{
    // 采集要几秒，先把提示画上去，并临时禁用刷新按钮，避免连点
    if (m_hStatus != nullptr)
    {
        ::SetWindowTextW(m_hStatus, L"正在从系统采集右键菜单，请稍候…");
    }
    if (m_hRefresh != nullptr)
    {
        ::EnableWindow(m_hRefresh, FALSE);
    }
    ::UpdateWindow(m_hWnd);

    shell::CollectScenes(m_report);
    shell::SaveCache(m_report);

    if (m_hRefresh != nullptr)
    {
        ::EnableWindow(m_hRefresh, TRUE);
    }

    // 样本下拉重新填（采集后文件场景数量固定，但保持同步）
    if (m_hCombo != nullptr)
    {
        const int oldSel = static_cast<int>(::SendMessageW(m_hCombo, CB_GETCURSEL, 0, 0));
        ::SendMessageW(m_hCombo, CB_RESETCONTENT, 0, 0);
        for (const shell::SceneMenu& scene : m_report.scenes)
        {
            if (scene.scene == shell::SceneFile)
            {
                const std::wstring shortLabel = ShortSampleLabel(scene.sampleLabel);
                ::SendMessageW(m_hCombo, CB_ADDSTRING, 0,
                               reinterpret_cast<LPARAM>(shortLabel.c_str()));
            }
        }
        ::SendMessageW(m_hCombo, CB_SETCURSEL, oldSel >= 0 ? oldSel : 0, 0);
    }

    ReloadList();
}

// ============================================================================
// 消息处理
// ============================================================================

LRESULT ContextMenuSettingsDialog::HandleMessage(HWND, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        Layout();
        return 0;

    case WM_MEASUREITEM:
    {
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
            // 选中行先铺底色
            if (draw->itemState & ODS_SELECTED)
            {
                ::FillRect(draw->hDC, &draw->rcItem,
                           static_cast<HBRUSH>(::GetSysColorBrush(COLOR_HIGHLIGHT)));
            }
            else
            {
                ::FillRect(draw->hDC, &draw->rcItem,
                           static_cast<HBRUSH>(::GetSysColorBrush(COLOR_WINDOW)));
            }

            DrawRow(draw->hDC, draw->rcItem, static_cast<int>(draw->itemID));
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header == nullptr)
        {
            break;
        }
        if (header->hwndFrom == m_hTab && header->code == TCN_SELCHANGE)
        {
            ReloadList();
            return 0;
        }
        break;
    }

    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);
        if (controlId == kControlIdRefresh)
        {
            Refresh();
            return 0;
        }
        if (controlId == kControlIdCombo && HIWORD(wParam) == CBN_SELCHANGE)
        {
            ReloadList();
            return 0;
        }
        if (controlId == IDCANCEL || controlId == IDOK)
        {
            ::DestroyWindow(m_hWnd);
            return 0;
        }
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ::DestroyWindow(m_hWnd);
            return 0;
        }
        break;

    case WM_APP + 1:
        // 窗口已经显示出来了，这里再真正去采集
        Refresh();
        return 0;

    case WM_CLOSE:
        ::DestroyWindow(m_hWnd);
        return 0;

    case WM_DESTROY:
        ReleaseBitmaps();
        m_hWnd = nullptr;
        return 0;
    }

    return ::DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

} // namespace ui
