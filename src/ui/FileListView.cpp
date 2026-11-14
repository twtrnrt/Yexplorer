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

#include <algorithm>

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
    // LVS_EDITLABELS：允许按 F2 在原地改名字（和资源管理器一样）
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP
                | LVS_REPORT
                | LVS_OWNERDATA
                | LVS_SHOWSELALWAYS
                | LVS_EDITLABELS;

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

void FileListView::SetResult(std::shared_ptr<shell::EnumerateResult> result)
{
    if (m_hWnd == nullptr)
    {
        m_result = std::move(result);
        return;
    }

    // 刷新前先记住"用户看到的状态"：选中了哪些、滚动到哪一行
    std::vector<std::wstring> selectedNames;
    std::wstring              topName;

    if (m_result != nullptr)
    {
        int topIndex = ListView_GetTopIndex(m_hWnd);
        if (topIndex >= 0 && topIndex < static_cast<int>(m_result->entries.size()))
        {
            topName = m_result->entries[static_cast<size_t>(topIndex)].name;
        }

        for (size_t i = 0; i < m_result->entries.size(); ++i)
        {
            if ((ListView_GetItemState(m_hWnd, static_cast<int>(i), LVIS_SELECTED) & LVIS_SELECTED) != 0)
            {
                selectedNames.push_back(m_result->entries[i].name);
            }
        }
    }

    m_result = std::move(result);

    int itemCount = (m_result != nullptr) ? static_cast<int>(m_result->entries.size()) : 0;

    // LVSICF_NOINVALIDATEALL：不整屏重画，控件只重画需要变的区域
    // LVSICF_NOSCROLL：行数变少时不强制把滚动条拉回顶部
    ListView_SetItemCountEx(m_hWnd, itemCount, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    RestoreSelectionAndScroll(selectedNames, topName);

    // erase = FALSE：不擦背景再重画，避免整片闪白
    ::InvalidateRect(m_hWnd, nullptr, FALSE);

    LayoutColumns();
}

// ============================================================================
// 最小范围更新
//
// 场景：在同一个目录里复制/移动完，需要知道目录现在长什么样，
//       但不想让整个列表重画一遍。
// 做法：新旧列表从头开始逐行比对，找到第一个不一样的位置，
//       从这里往后才重画；前面一模一样的行原样不动。
// ============================================================================

void FileListView::MergeResult(std::shared_ptr<shell::EnumerateResult> result)
{
    if (m_hWnd == nullptr)
    {
        m_result = std::move(result);
        return;
    }

    // 换了目录就退化成整表更新
    if (m_result == nullptr || result == nullptr ||
        _wcsicmp(m_result->path.c_str(), result->path.c_str()) != 0)
    {
        SetResult(std::move(result));
        return;
    }

    // 记住用户状态
    std::vector<std::wstring> selectedNames;
    std::wstring              topName;

    int topIndex = ListView_GetTopIndex(m_hWnd);
    if (topIndex >= 0 && topIndex < static_cast<int>(m_result->entries.size()))
    {
        topName = m_result->entries[static_cast<size_t>(topIndex)].name;
    }

    for (size_t i = 0; i < m_result->entries.size(); ++i)
    {
        if ((ListView_GetItemState(m_hWnd, static_cast<int>(i), LVIS_SELECTED) & LVIS_SELECTED) != 0)
        {
            selectedNames.push_back(m_result->entries[i].name);
        }
    }

    // 从头开始逐行比对，找第一个不一样的位置
    const std::vector<shell::ShellEntry>& oldEntries = m_result->entries;
    const std::vector<shell::ShellEntry>& newEntries = result->entries;

    size_t common  = 0;
    size_t smaller = (oldEntries.size() < newEntries.size()) ? oldEntries.size() : newEntries.size();

    while (common < smaller &&
           _wcsicmp(oldEntries[common].fullPath.c_str(), newEntries[common].fullPath.c_str()) == 0)
    {
        ++common;
    }

    // 一行都没变：连重绘都不需要
    if (common == oldEntries.size() && common == newEntries.size())
    {
        return;
    }

    m_result = std::move(result);

    int newCount = static_cast<int>(m_result->entries.size());
    ListView_SetItemCountEx(m_hWnd, newCount, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    RestoreSelectionAndScroll(selectedNames, topName);

    // 只重画从第一个变化处往下的行：上面的内容没变，不碰
    if (common < static_cast<size_t>(newCount))
    {
        ListView_RedrawItems(m_hWnd, static_cast<int>(common), newCount - 1);
        ::UpdateWindow(m_hWnd);
    }
    else if (newCount > 0)
    {
        // 是"删掉了尾巴"，重画最后几行让空白区补上
        int from = (std::max)(0, newCount - 1);
        ListView_RedrawItems(m_hWnd, from, newCount - 1);
        ::UpdateWindow(m_hWnd);
    }
}

// ============================================================================
// 还原选中 / 滚动位置
// ============================================================================

void FileListView::RestoreSelectionAndScroll(const std::vector<std::wstring>& selectedNames,
                                             const std::wstring&              topName)
{
    if (m_result == nullptr)
    {
        return;
    }

    // 先把控件里原有的选中全清掉：
    // 刷新后索引会整体挪位，如果不清，选中的"行号"会粘在新内容上（选错东西）
    LVITEMW clearItem = {};
    clearItem.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    clearItem.state     = 0;
    ::SendMessageW(m_hWnd, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                   reinterpret_cast<LPARAM>(&clearItem));

    // 恢复选中（按名字找，因为增减之后行号会变）
    for (const auto& name : selectedNames)
    {
        for (size_t i = 0; i < m_result->entries.size(); ++i)
        {
            if (m_result->entries[i].name == name)
            {
                LVITEMW item = {};
                item.stateMask = LVIS_SELECTED;
                item.state     = LVIS_SELECTED;
                ::SendMessageW(m_hWnd, LVM_SETITEMSTATE, static_cast<WPARAM>(i),
                               reinterpret_cast<LPARAM>(&item));
                break;
            }
        }
    }

    // 恢复滚动位置（尽量让原来顶部那一行还在顶部附近）
    if (!topName.empty())
    {
        for (size_t i = 0; i < m_result->entries.size(); ++i)
        {
            if (m_result->entries[i].name == topName)
            {
                ListView_EnsureVisible(m_hWnd, static_cast<int>(i), FALSE);
                break;
            }
        }
    }
}

// ============================================================================
// 被剪切的项：显示成半透明（和资源管理器一致）
// ============================================================================

void FileListView::SetCutPaths(const std::vector<std::wstring>& paths)
{
    m_cutPaths = paths;

    if (m_hWnd != nullptr)
    {
        // 整体重画一次：淡化效果可能出现在任意行，没法只画局部
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

std::wstring FileListView::CurrentPath() const
{
    return (m_result != nullptr) ? m_result->path : std::wstring();
}

std::vector<std::wstring> FileListView::AllPaths() const
{
    std::vector<std::wstring> paths;

    if (m_result != nullptr)
    {
        paths.reserve(m_result->entries.size());
        for (const auto& entry : m_result->entries)
        {
            paths.push_back(entry.fullPath);
        }
    }

    return paths;
}

void FileListView::SelectPaths(const std::vector<std::wstring>& paths)
{
    if (m_hWnd == nullptr || m_result == nullptr)
    {
        return;
    }

    // 先把已有选中全部取消（wParam = -1 表示"所有项"）
    LVITEMW clearItem = {};
    clearItem.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    clearItem.state     = 0;
    ::SendMessageW(m_hWnd, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                   reinterpret_cast<LPARAM>(&clearItem));

    for (const auto& path : paths)
    {
        for (size_t i = 0; i < m_result->entries.size(); ++i)
        {
            if (_wcsicmp(m_result->entries[i].fullPath.c_str(), path.c_str()) == 0)
            {
                LVITEMW item = {};
                item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                item.state     = LVIS_SELECTED | LVIS_FOCUSED;
                ::SendMessageW(m_hWnd, LVM_SETITEMSTATE, static_cast<WPARAM>(i),
                               reinterpret_cast<LPARAM>(&item));
                break;
            }
        }
    }
}

bool FileListView::IsCutPath(const std::wstring& fullPath) const
{    if (m_cutPaths.empty() || fullPath.empty())
    {
        return false;
    }

    for (const auto& cutPath : m_cutPaths)
    {
        if (_wcsicmp(cutPath.c_str(), fullPath.c_str()) == 0)
        {
            return true;
        }
    }

    return false;
}

// ============================================================================
// 排序箭头
// ============================================================================

void FileListView::ClearSortArrows()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    HWND header = ListView_GetHeader(m_hWnd);
    if (header == nullptr)
    {
        return;
    }

    int count = Header_GetItemCount(header);
    for (int i = 0; i < count; ++i)
    {
        HDITEMW item = {};
        item.mask = HDI_FORMAT;
        if (Header_GetItem(header, i, &item))
        {
            item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            Header_SetItem(header, i, &item);
        }
    }
}

void FileListView::SetSortState(int column, bool ascending)
{
    ClearSortArrows();

    if (m_hWnd == nullptr)
    {
        return;
    }

    HWND header = ListView_GetHeader(m_hWnd);
    if (header == nullptr || column < 0)
    {
        return;
    }

    HDITEMW item = {};
    item.mask = HDI_FORMAT;
    if (Header_GetItem(header, column, &item))
    {
        // HDF_SORTUP / HDF_SORTDOWN 是系统列头自带的排序小箭头，不用自己画
        item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, column, &item);
    }
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

std::wstring FileListView::ItemName(size_t index) const
{
    if (m_result == nullptr || index >= m_result->entries.size())
    {
        return std::wstring();
    }
    return m_result->entries[index].name;
}

std::wstring FileListView::ItemPath(size_t index) const
{
    if (m_result == nullptr || index >= m_result->entries.size())
    {
        return std::wstring();
    }
    return m_result->entries[index].fullPath;
}

// ============================================================================
// 局部增删（删文件 / 新建文件夹之后用，不重扫整个目录）
// ============================================================================

bool FileListView::RemoveItemByPath(const std::wstring& fullPath)
{
    if (m_result == nullptr || m_hWnd == nullptr)
    {
        return false;
    }

    auto& entries = m_result->entries;

    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (_wcsicmp(entries[i].fullPath.c_str(), fullPath.c_str()) == 0)
        {
            entries.erase(entries.begin() + static_cast<ptrdiff_t>(i));

            ListView_SetItemCountEx(m_hWnd, static_cast<int>(entries.size()),
                                    LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
    }

    return false;
}

void FileListView::InsertEntry(const shell::ShellEntry& entry)
{
    if (m_result == nullptr || m_hWnd == nullptr)
    {
        return;
    }

    auto& entries = m_result->entries;

    // 按当前排序规则找插入位置
    size_t position = entries.size();
    if (m_comparator)
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (m_comparator(entry, entries[i]))
            {
                position = i;
                break;
            }
        }
    }

    entries.insert(entries.begin() + static_cast<ptrdiff_t>(position), entry);

    ListView_SetItemCountEx(m_hWnd, static_cast<int>(entries.size()),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    // 选中新建出来的这一项，并滚到能看见的位置（和资源管理器新建文件夹后的行为一致）
    ListView_SetItemState(m_hWnd, static_cast<int>(position), LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(m_hWnd, static_cast<int>(position), FALSE);
    ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

// ============================================================================
// 原地改名（F2）
// ============================================================================

void FileListView::BeginRenameSelected()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    int selectedIndex = ListView_GetNextItem(m_hWnd, -1, LVNI_SELECTED);
    if (selectedIndex < 0)
    {
        return;
    }

    ::SetFocus(m_hWnd);
    ListView_EditLabel(m_hWnd, selectedIndex);
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
                int iconIndex = m_iconCache->GetIconIndex(entry.name, entry.isFolder);

                // 被剪切（Ctrl+X）的项目：图标换成半透明版本，和资源管理器一样"变淡"
                if (IsCutPath(entry.fullPath))
                {
                    iconIndex = m_iconCache->GetDimmedIconIndex(iconIndex);
                }

                displayInfo->item.iImage = iconIndex;
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

    // --------------------------------------------------------------
    // 自绘：把被剪切的行的文字改成浅灰
    // 图标变淡靠上面的"半透明图标"，文字变灰靠这里
    // --------------------------------------------------------------
    case NM_CUSTOMDRAW:
    {
        NMLVCUSTOMDRAW* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);

        switch (customDraw->nmcd.dwDrawStage)
        {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;   // 要求逐行通知

        case CDDS_ITEMPREPAINT:
        {
            size_t index = static_cast<size_t>(customDraw->nmcd.dwItemSpec);

            if (m_result != nullptr && index < m_result->entries.size() &&
                IsCutPath(m_result->entries[index].fullPath))
            {
                customDraw->clrText = RGB(150, 150, 150);   // 浅灰
                return CDRF_NEWFONT;                         // 告诉控件按我改的颜色画
            }

            return CDRF_DODEFAULT;
        }

        default:
            break;
        }
        return CDRF_DODEFAULT;
    }

    // --------------------------------------------------------------
    // 点表头：按这一列排序
    // --------------------------------------------------------------
    case LVN_COLUMNCLICK:
    {
        const NMLISTVIEW* clickInfo = reinterpret_cast<const NMLISTVIEW*>(lParam);
        if (m_onColumnClick)
        {
            m_onColumnClick(clickInfo->iSubItem);
        }
        return 0;
    }

    // --------------------------------------------------------------
    // 原地改名：开始编辑时要告诉控件"现在这行叫什么"
    // 虚拟模式下控件自己不知道内容，必须由我们提供
    // --------------------------------------------------------------
    case LVN_BEGINLABELEDIT:
    {
        NMLVDISPINFOW* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);

        int index = info->item.iItem;
        if (m_result == nullptr || index < 0 || index >= static_cast<int>(m_result->entries.size()))
        {
            return TRUE;   // 返回真 = 不允许编辑
        }

        info->item.mask |= LVIF_TEXT;
        info->item.pszText = const_cast<wchar_t*>(m_result->entries[static_cast<size_t>(index)].name.c_str());

        return FALSE;      // 返回假 = 允许编辑
    }

    // --------------------------------------------------------------
    // 改名结束：拿到新名字就去做真正的重命名
    // --------------------------------------------------------------
    case LVN_ENDLABELEDIT:
    {
        NMLVDISPINFOW* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);

        if (info->item.pszText == nullptr)
        {
            return 0;      // 用户按 Esc 取消，什么都不做
        }

        int index = info->item.iItem;
        if (index >= 0 && m_onRename)
        {
            m_onRename(static_cast<size_t>(index), std::wstring(info->item.pszText));
        }
        return 0;
    }

    default:
        break;
    }

    return 0;
}
