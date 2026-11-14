// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/FileListView.cpp
//
// 文件列表控件实现。
//
// 列顺序对齐资源管理器：名称 / 修改日期 / 类型 / 大小
// 列宽策略：名称列自适应吃剩余宽度（保底 260），其余固定
 // ============================================================================

#include "FileListView.h"

// ============================================================================
// 列定义
// ============================================================================

static const int kTimeColumnWidth  = 160;   // 修改日期
static const int kTypeColumnWidth  = 120;   // 类型
static const int kSizeColumnWidth  = 110;   // 大小
static const int kNameColumnMin    = 260;   // 名称列保底宽度

// ============================================================================
// 构造 / 析构
// ============================================================================

FileListView::~FileListView()
{
    if (m_hFont != nullptr)
    {
        ::DeleteObject(m_hFont);
        m_hFont = nullptr;
    }
}

// ============================================================================
// 创建
// ============================================================================

bool FileListView::Create(HWND parent, HINSTANCE hInstance)
{
    // LVS_OWNERDATA 必须在创建时指定，创建后无法改；且必须是报告模式
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP
                | LVS_REPORT
                | LVS_OWNERDATA
                | LVS_SHOWSELALWAYS;

    m_hWnd = ::CreateWindowExW(
        0,
        WC_LISTVIEWW,
        L"",
        style,
        0, 0, 0, 0,
        parent,
        nullptr,
        hInstance,
        nullptr);

    if (m_hWnd == nullptr)
    {
        return false;
    }

    ListView_SetExtendedListViewStyle(
        m_hWnd,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    ApplyFont();
    ApplyColumns();

    return true;
}

void FileListView::SetIconCache(shell::IconCache* iconCache)
{
    m_iconCache = iconCache;

    if (m_hWnd == nullptr || iconCache == nullptr || iconCache->ImageList() == nullptr)
    {
        return;
    }

    // 把小图标图像列表挂到列表控件上
    ListView_SetImageList(m_hWnd, iconCache->ImageList(), LVSIL_SMALL);
}

// ============================================================================
// 外观
// ============================================================================

void FileListView::ApplyFont()
{
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);

    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_hFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }

    if (m_hFont == nullptr)
    {
        m_hFont = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    if (m_hFont != nullptr)
    {
        ::SendMessageW(m_hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    }
}

void FileListView::ApplyColumns()
{
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;

    // 第 0 列：名称（左对齐，宽度由 LayoutColumns 计算）
    column.iSubItem = 0;
    column.fmt      = LVCFMT_LEFT;
    column.pszText  = const_cast<wchar_t*>(L"名称");
    column.cx       = 300;
    ListView_InsertColumn(m_hWnd, 0, &column);

    // 第 1 列：修改日期
    column.iSubItem = 1;
    column.fmt      = LVCFMT_LEFT;
    column.pszText  = const_cast<wchar_t*>(L"修改日期");
    column.cx       = kTimeColumnWidth;
    ListView_InsertColumn(m_hWnd, 1, &column);

    // 第 2 列：类型
    column.iSubItem = 2;
    column.fmt      = LVCFMT_LEFT;
    column.pszText  = const_cast<wchar_t*>(L"类型");
    column.cx       = kTypeColumnWidth;
    ListView_InsertColumn(m_hWnd, 2, &column);

    // 第 3 列：大小（右对齐，和资源管理器一致）
    column.iSubItem = 3;
    column.fmt      = LVCFMT_RIGHT;
    column.pszText  = const_cast<wchar_t*>(L"大小");
    column.cx       = kSizeColumnWidth;
    ListView_InsertColumn(m_hWnd, 3, &column);
}

void FileListView::LayoutColumns()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    int fixedWidth     = kTimeColumnWidth + kTypeColumnWidth + kSizeColumnWidth;
    int scrollBarWidth = ::GetSystemMetrics(SM_CXVSCROLL);
    int nameWidth      = (clientRect.right - clientRect.left) - fixedWidth - scrollBarWidth;

    if (nameWidth < kNameColumnMin)
    {
        nameWidth = kNameColumnMin;
    }

    ListView_SetColumnWidth(m_hWnd, 0, static_cast<UINT>(nameWidth));
}

// ============================================================================
// 布局与数据
// ============================================================================

void FileListView::Resize(int x, int y, int width, int height)
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    ::SetWindowPos(m_hWnd, nullptr, x, y, width, height,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    LayoutColumns();
}

void FileListView::SetResult(std::shared_ptr<const shell::EnumerateResult> result)
{
    m_result = std::move(result);

    if (m_hWnd == nullptr)
    {
        return;
    }

    int itemCount = (m_result != nullptr) ? static_cast<int>(m_result->entries.size()) : 0;

    ListView_SetItemCountEx(m_hWnd, itemCount, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    ::SendMessageW(m_hWnd, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), 0);
    ::InvalidateRect(m_hWnd, nullptr, TRUE);

    LayoutColumns();
}

// ============================================================================
// 选中项相关
// ============================================================================

std::vector<std::wstring> FileListView::GetSelectedPaths() const
{
    std::vector<std::wstring> paths;

    if (m_hWnd == nullptr || m_result == nullptr)
    {
        return paths;
    }

    int itemCount = static_cast<int>(m_result->entries.size());

    for (int index = 0; index < itemCount; ++index)
    {
        if ((ListView_GetItemState(m_hWnd, index, LVIS_SELECTED) & LVIS_SELECTED) != 0)
        {
            paths.push_back(m_result->entries[index].fullPath);
        }
    }

    return paths;
}

size_t FileListView::ItemCount() const
{
    if (m_result == nullptr)
    {
        return 0;
    }
    return m_result->entries.size();
}

void FileListView::SelectAll()
{
    if (m_hWnd == nullptr)
    {
        return;
    }
    ListView_SetItemState(m_hWnd, -1, LVIS_SELECTED, LVIS_SELECTED);
}

// ============================================================================
// 通知处理
// ============================================================================

LRESULT FileListView::HandleNotify(LPARAM lParam)
{
    if (lParam == 0)
    {
        return 0;
    }

    NMHDR* header = reinterpret_cast<NMHDR*>(lParam);

    switch (header->code)
    {
    // --------------------------------------------------------------
    // 虚拟模式核心回调：控件要哪一行哪一列的内容，在这里提供
    // --------------------------------------------------------------
    case LVN_GETDISPINFO:
    {
        NMLVDISPINFOW* displayInfo = reinterpret_cast<NMLVDISPINFOW*>(lParam);

        if (m_result == nullptr)
        {
            return 0;
        }

        size_t index = static_cast<size_t>(displayInfo->item.iItem);
        if (index >= m_result->entries.size())
        {
            return 0;
        }

        const shell::ShellEntry& entry = m_result->entries[index];

        // ---- 图标（只对整行生效，与具体列无关）----
        if ((displayInfo->item.mask & LVIF_IMAGE) != 0)
        {
            if (m_iconCache != nullptr)
            {
                displayInfo->item.iImage = m_iconCache->GetIconIndex(entry.name, entry.isFolder);
            }
            else
            {
                displayInfo->item.iImage = 0;
            }
        }

        // ---- 文本 ----
        if ((displayInfo->item.mask & LVIF_TEXT) == 0)
        {
            return 0;
        }

        switch (displayInfo->item.iSubItem)
        {
        case 0:   // 名称
            displayInfo->item.pszText = const_cast<wchar_t*>(entry.name.c_str());
            break;

        case 1:   // 修改日期
            m_cacheTime = format::FileTimeText(entry.modified);
            displayInfo->item.pszText = const_cast<wchar_t*>(m_cacheTime.c_str());
            break;

        case 2:   // 类型
            m_cacheType = format::TypeText(entry.name, entry.isFolder);
            displayInfo->item.pszText = const_cast<wchar_t*>(m_cacheType.c_str());
            break;

        case 3:   // 大小
            m_cacheSize = format::SizeText(entry.size, entry.isFolder);
            displayInfo->item.pszText = const_cast<wchar_t*>(m_cacheSize.c_str());
            break;

        default:
            break;
        }
        return 0;
    }

    // --------------------------------------------------------------
    // 双击：交给主窗口处理（文件夹进目录，文件用默认程序打开）
    // --------------------------------------------------------------
    case NM_DBLCLK:
    {
        int selectedIndex = ListView_GetNextItem(m_hWnd, -1, LVNI_SELECTED);
        if (selectedIndex >= 0 && m_onActivate)
        {
            m_onActivate(static_cast<size_t>(selectedIndex));
        }
        return 0;
    }

    // --------------------------------------------------------------
    // 右键：交给主窗口弹菜单
    // --------------------------------------------------------------
    case NM_RCLICK:
    {
        POINT screenPoint = {};
        ::GetCursorPos(&screenPoint);

        if (m_onContextMenu)
        {
            m_onContextMenu(screenPoint);
        }
        return 1;   // 返回非 0 表示已处理，不再弹默认菜单
    }

    default:
        break;
    }

    return 0;
}
