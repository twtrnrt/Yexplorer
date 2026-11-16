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

#include "shell/ShellDragDrop.h"
#include "core/Format.h"
#include "core/Logger.h"
#include "core/PinyinInitials.h"   // 汉字 → 拼音首字母（按字母跳转时匹配中文名）

#include <uxtheme.h>   // SetWindowTheme / OpenThemeData / DrawThemeBackground
#include <vssym32.h>  // 主题部件、状态、属性常量（LVP_LISTITEM、LISS_SELECTED、TMT_TEXTCOLOR）
#include <algorithm>
#include <cwctype>   // towlower

#pragma comment(lib, "uxtheme.lib")

// ============================================================================
// 列定义
// ============================================================================

static const int kTimeColumnWidth  = 160;   // 修改日期（默认值，用户拖过之后用拖出来的）
static const int kTypeColumnWidth  = 120;   // 类型
static const int kSizeColumnWidth  = 110;   // 大小
static const int kNameColumnMin    = 260;   // 名称列保底宽度

// ============================================================================
// 按字母跳转：超时与匹配
// ============================================================================

// 两次按键间隔超过这个时间，累积的字母串就清零重新算（500ms，按需求定的）。
// 这个值决定了"输入快的两个字母能不能连成一个词"：慢于半秒就当成两次独立的单字母跳转。
static const DWORD kTypeAheadTimeoutMs = 500;

// ============================================================================
// 平铺视图的改名编辑框
//
// 平铺不用列表控件自带的原地改名，自己建一个"多行编辑框"：
//   · 自带编辑框是单行的，框比一行高时文字必然垂直居中，做不出"三行框 + 字贴左上"；
//   · 自建的多行编辑框文字天然靠左上，框也能做三行高，正好盖住名字位置。
// 这是失焦时的收尾消息：不能在 WM_KILLFOCUS 里同步销毁自己，排到队列后面再做。
// ============================================================================

static const UINT kMsgFinishTileRename = WM_APP + 101;

// 单行改名框（小图标 / 列表）的宽度 = 文字实际宽 + 这个值。
// 里面包含：左右内边距各 3px、边框与光标位置约 5px，再按用户要求向右多留 9px。
static const int kRenameEditExtraWidth = 17;

// 单行改名框的高度：列表视图的行距也是 24（和小图标一致），框正好贴满一行
static const int kRenameEditHeight = 24;

// 是不是"盘符根"路径（形如 D:\ 、V:\ ）。
// 用来看某个项是不是驱动器卷：卷根的系统属性经常带 SYSTEM/HIDDEN，
// 但资源管理器从不会把驱动器画成灰色/半透明，所以这类项一律不淡化。
static bool IsDriveRootPath(const std::wstring& path)
{
    return path.size() == 3 &&
           ::iswalpha(static_cast<wint_t>(path[0])) != 0 &&
           path[1] == L':' &&
           path[2] == L'\\';
}

// 是不是真实文件系统里的路径（C:\... 或 \\server\share\...）。
// 控制面板、"此电脑"里的那些虚拟项不是这种路径（它们是 CLSID 形式或空路径）。
static bool IsFileSystemPath(const std::wstring& path)
{
    if (path.size() < 2)
    {
        return false;
    }

    // 盘符路径 X:\...
    if (::iswalpha(static_cast<wint_t>(path[0])) != 0 && path[1] == L':')
    {
        return true;
    }

    // UNC 路径 \\server\share
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        return true;
    }

    return false;
}

// 这个项要不要参与"淡化"（变灰 / 半透明）。
//
// 驱动器和控制面板这类特殊项一律不淡化：
//   · 驱动器卷根常带 SYSTEM/HIDDEN 属性，但不是资源管理器里那个灰样子；
//   · 控制面板项是虚拟命名空间里的对象（路径是 CLSID 或空），既不能真剪切、
//     属性也带 SYSTEM，跟着变灰就完全不是资源管理器的样子了。
static bool ShouldSkipDimming(const shell::ShellEntry& entry)
{
    if (IsDriveRootPath(entry.fullPath) || entry.diskTotal > 0)
    {
        return true;   // 驱动器 / 磁盘卡片
    }

    return !IsFileSystemPath(entry.fullPath);   // 控制面板、虚拟项等
}

// ============================================================================
// 小图标视图的列宽参数：
//   名字能显示多宽 = 300，列与列之间空 20，图标 16 + 图标与文字之间 4
// 控件里每列总宽 = 名字宽 + 图标那一段 + 列间距
// ============================================================================

static const int kSmallIconTextWidth = 300;
static const int kSmallIconColumnGap = 20;
static const int kSmallIconIconSpan  = 20;   // 16px 图标 + 图标与文字之间的 4px
static const int kSmallIconColumnWidth =
    kSmallIconTextWidth + kSmallIconIconSpan + kSmallIconColumnGap;   // = 340



// 判断一个名字是不是以 typed 开头（支持中文名按拼音首字母匹配）
//
// 逐字符比较，不分配任何内存：
//   · 名字这一格是汉字 → 它的拼音首字母集合里要有 typed 对应的那个字母
//     （「重」的集合是 {c, t, z}，所以输入 c 或 z 都算命中）
//   · 名字这一格是普通字符（字母/数字/符号/空格/点）→ 直接比，不区分大小写
// typed 的字符都应该是小写（调用方已经归一化）。
static bool NameMatchesTypeAheadPrefix(const std::wstring& name, const std::wstring& typed)
{
    if (typed.empty() || name.size() < typed.size())
    {
        return false;
    }

    size_t index = 0;
    for (const wchar_t ch : name)
    {
        if (index >= typed.size())
        {
            return true;   // typed 比完了，前面全都对上，就是一个前缀
        }

        const wchar_t typedChar = typed[index];
        const unsigned int mask = core::PinyinInitialMask(ch);

        if (mask != 0)
        {
            // 名字这一格是汉字：输入必须是小写字母，且是这个字的某个读音首字母
            if (typedChar < L'a' || typedChar > L'z')
            {
                return false;
            }
            if ((mask & (1u << (typedChar - L'a'))) == 0)
            {
                return false;
            }
        }
        else
        {
            // 普通字符：统一按小写比较（数字、符号、空格、点本身就是它们自己）
            if (static_cast<wchar_t>(::towlower(ch)) != typedChar)
            {
                return false;
            }
        }

        ++index;
    }

    // 名字走完了还没把 typed 比完 → 名字太短，算不匹配
    return index >= typed.size();
}

// ============================================================================
// 哪些文件必须按「完整路径」取图标
//
// 背景：日常取图标走的是"按扩展名"的高速通道（不碰硬盘，几微秒一次），
//       但 exe、快捷方式这类文件的图标是打包在文件自己身上的，
//       按扩展名只能拿到系统那个"通用程序"的空白图标。
//       所以这些类型必须拿真实路径去问系统。
// ============================================================================

static bool NeedsOwnIcon(const std::wstring& name)
{
    static const wchar_t* const kOwnIconExtensions[] = {
        L".exe",       // 可执行程序：图标内嵌在 exe 里
        L".lnk",       // 快捷方式：图标来自指向的目标
        L".ico",       // 图标文件：本身就是一个图标
        L".scr",       // 屏幕保护程序
        L".cpl",       // 控制面板小程序
        L".url",       // 网址快捷方式
        L".msi",       // 安装包
        L".appref-ms", // ClickOnce 快捷方式
    };

    const size_t dotPos = name.find_last_of(L'.');
    if (dotPos == std::wstring::npos)
    {
        return false;
    }

    std::wstring extension = name.substr(dotPos);
    for (auto& ch : extension)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }

    for (auto candidate : kOwnIconExtensions)
    {
        if (extension == candidate)
        {
            return true;
        }
    }
    return false;
}


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

    if (m_hHeaderFont != nullptr)
    {
        ::DeleteObject(m_hHeaderFont);
        m_hHeaderFont = nullptr;
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

    // 用资源管理器自己的主题：选中行的样式（浅蓝底 + 细边）就和左栏导航一致了，
    // 这也是资源管理器列表本来的样子
    ::SetWindowTheme(m_hWnd, L"Explorer", nullptr);

    // 子类化：为了拦 WM_CHAR 做"按字母跳转"（见 FileListSubclassProc 的注释）
    ::SetWindowSubclass(m_hWnd, &FileListView::ListViewSubclassProc, 1,
                        reinterpret_cast<DWORD_PTR>(this));

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

    // 大图标列表挂到 LVSIL_NORMAL 上（「大图标」视图用这张）
    if (iconCache->ImageList(shell::IconCache::IconSize::Large) != nullptr)
    {
        ListView_SetImageList(m_hWnd,
                              iconCache->ImageList(shell::IconCache::IconSize::Large),
                              LVSIL_NORMAL);
    }
}

// ============================================================================
// 切换视图模式
//
// ListView 的四种视图只是样式位不同（LVS_TYPEMASK 那两位），
// 换样式用 SetWindowLongPtr 改 GWL_STYLE —— 但控件只在创建时读一次，
// 改完必须让它重新应用一次样式，所以这里配合 SetWindowPos 触发布局。
// ============================================================================

void FileListView::SetViewMode(ViewMode mode)
{
    if (m_hWnd == nullptr || mode == m_viewMode)
    {
        return;
    }

    m_viewMode = mode;

    // 先算出新的样式位
    DWORD viewStyle = LVS_REPORT;

    switch (mode)
    {
    case ViewMode::LargeIcon: viewStyle = LVS_ICON;      break;
    case ViewMode::Tiles:     viewStyle = LVS_ICON;      break;   // 平铺也用图标布局，只是格子画法不同
    // 小图标也用图标布局（不用 LVS_SMALLICON）：
    // 虚拟列表（LVS_OWNERDATA）配 LVS_SMALLICON 时，控件的列宽算不准 ——
    // 刚切过去会挤成一团、拉宽窗口反而只剩一列。用 LVS_ICON + 固定格子宽就稳定。
    case ViewMode::SmallIcon: viewStyle = LVS_ICON;      break;
    // 列表也用图标布局：控件在 LVS_LIST 下按"最长名字"算列宽、窗口拉宽不重算，
    // 右边一大片空白、名字还被截断。用 LVS_ICON + 格宽=客户区宽，一行一个，竖排照旧。
    case ViewMode::List:      viewStyle = LVS_ICON;      break;
    case ViewMode::Details:
    default:                  viewStyle = LVS_REPORT;    break;
    }

    const LONG_PTR oldStyle = ::GetWindowLongPtrW(m_hWnd, GWL_STYLE);
    const LONG_PTR newStyle = (oldStyle & ~static_cast<LONG_PTR>(LVS_TYPEMASK)) |
                              static_cast<LONG_PTR>(viewStyle);
    ::SetWindowLongPtrW(m_hWnd, GWL_STYLE, newStyle);

    // 图标按视图换尺寸：大图标视图用 48px，其余用 16px。
    // 控件在「大图标」视图里画的是 LVSIL_NORMAL 这张列表，
    // 所以切到大图标时把大图列表挂到 NORMAL，切回来时把小图列表挂回 NORMAL，
    // 避免控件拿 16px 图标去撑 48px 的格子（会糊）。
    if (m_iconCache != nullptr)
    {
        const auto smallList = m_iconCache->ImageList(shell::IconCache::IconSize::Small);
        const auto largeList = m_iconCache->ImageList(shell::IconCache::IconSize::Large);

        if ((mode == ViewMode::LargeIcon || mode == ViewMode::Tiles) && largeList != nullptr)
        {
            ListView_SetImageList(m_hWnd, largeList, LVSIL_NORMAL);
        }
        else if (smallList != nullptr)
        {
            ListView_SetImageList(m_hWnd, smallList, LVSIL_NORMAL);
        }

        if (smallList != nullptr)
        {
            ListView_SetImageList(m_hWnd, smallList, LVSIL_SMALL);
        }
    }

    // 大图标/小图标视图要指定格子大小，否则控件按默认 75x75 摆，
    // 48px 图标会挤在一起、16px 图标会散得太开。
    if (mode == ViewMode::LargeIcon)
    {
        // 宽 130：48px 图标下面能放下两行名字（系统大图标视图也是这样换行的）
        // 高 100：图标 48 + 上下留白 + 两行文字
        ListView_SetIconSpacing(m_hWnd, 130, 100);
    }
    else if (mode == ViewMode::Tiles)
    {
        // 平铺：每个磁盘是一块固定宽度的小卡片（和系统此电脑一样，一行并排几个），
        // 卡片高 68。组标题那行由主窗口插入"占位格"把它顶成独占一行。
        ListView_SetIconSpacing(m_hWnd, 300, 68);
    }
    else if (mode == ViewMode::SmallIcon)
    {
        // 小图标：每格总宽 340 = 名字 300 + 图标 16/间距 4 + 列间距 20；行距 24。
        // 用 LVS_ICON 的固定格子，拉宽窗口时控件会自动多排几列（行优先）。
        ListView_SetIconSpacing(m_hWnd, kSmallIconColumnWidth, 24);
    }

    // 触发控件按新样式重排（不改尺寸，只让它重算布局）
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);
    ::SetWindowPos(m_hWnd, nullptr, 0, 0,
                   clientRect.right - clientRect.left,
                   clientRect.bottom - clientRect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // 虚拟列表换视图以后，控件内部还留着旧布局算好的项位置，
    // 只 Invalidate 的话它按旧位置重画，看起来就是"没变、要鼠标划过去才一个个刷"。
    // 重新报一次条目数，逼它丢弃缓存、按新样式重排，然后整窗立即重画一遍。
    if (m_result != nullptr)
    {
        ListView_SetItemCount(m_hWnd, static_cast<int>(m_result->entries.size()));
    }

    // 列表视图：每项宽度按客户区重设（控件自己算的列宽在窗口变宽后不会更新）
    if (mode == ViewMode::List)
    {
        LayoutListWidth();
    }

    ::RedrawWindow(m_hWnd, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

int FileListView::TilesPerRow() const
{
    if (m_hWnd == nullptr)
    {
        return 1;
    }

    // 控件自己的列宽：SetIconSpacing 设的是 300，但主题/DPI 可能把它再放大一点，
    // 所以问控件要比自己按常数除更准。LVM_GETITEMSPACING 返回 LOWORD=cx、HIWORD=cy。
    const DWORD spacing  = static_cast<DWORD>(::SendMessageW(m_hWnd, LVM_GETITEMSPACING, FALSE, 0));
    const int   spacingX = static_cast<int>(LOWORD(spacing));

    int columnWidth = (spacingX > 0) ? spacingX : 300;

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    const int width  = clientRect.right - clientRect.left;
    const int perRow = width / columnWidth;

    return (perRow < 1) ? 1 : perRow;
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

    // 分组标题行用的字体：和系统一样是"小一号的灰字"（不是粗体）
    // 单独造一份，是为了不让正文的字号跟着变
    if (m_hHeaderFont != nullptr)
    {
        ::DeleteObject(m_hHeaderFont);
        m_hHeaderFont = nullptr;
    }

    NONCLIENTMETRICSW headerMetrics = {};
    headerMetrics.cbSize = sizeof(headerMetrics);

    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(headerMetrics), &headerMetrics, 0))
    {
        LOGFONTW logFont = headerMetrics.lfMessageFont;
        logFont.lfHeight = logFont.lfHeight - 1;   // 比正文小一号
        logFont.lfWeight = FW_NORMAL;
        m_hHeaderFont    = ::CreateFontIndirectW(&logFont);
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
    column.cx       = m_timeColumnWidth;
    ListView_InsertColumn(m_hWnd, 1, &column);

    // 第 2 列：类型
    column.iSubItem = 2;
    column.fmt      = LVCFMT_LEFT;
    column.pszText  = const_cast<wchar_t*>(L"类型");
    column.cx       = m_typeColumnWidth;
    ListView_InsertColumn(m_hWnd, 2, &column);

    // 第 3 列：大小（右对齐，和资源管理器一致）
    column.iSubItem = 3;
    column.fmt      = LVCFMT_RIGHT;
    column.pszText  = const_cast<wchar_t*>(L"大小");
    column.cx       = m_sizeColumnWidth;
    ListView_InsertColumn(m_hWnd, 3, &column);
}

// 启动时套用配置里存的三列宽度（没有配置就用默认值，调用方不必判断）
void FileListView::SetFixedColumnWidths(int time, int type, int size)
{
    m_timeColumnWidth = (time > 20) ? time : kTimeColumnWidth;
    m_typeColumnWidth = (type > 20) ? type : kTypeColumnWidth;
    m_sizeColumnWidth = (size > 20) ? size : kSizeColumnWidth;

    if (m_hWnd == nullptr)
    {
        return;
    }

    ListView_SetColumnWidth(m_hWnd, 1, static_cast<UINT>(m_timeColumnWidth));
    ListView_SetColumnWidth(m_hWnd, 2, static_cast<UINT>(m_typeColumnWidth));
    ListView_SetColumnWidth(m_hWnd, 3, static_cast<UINT>(m_sizeColumnWidth));

    LayoutColumns();   // 名称列跟着重新算（它吃剩下的宽度）
}

void FileListView::LayoutColumns()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    int fixedWidth     = m_timeColumnWidth + m_typeColumnWidth + m_sizeColumnWidth;
    int scrollBarWidth = ::GetSystemMetrics(SM_CXVSCROLL);
    int nameWidth      = (clientRect.right - clientRect.left) - fixedWidth - scrollBarWidth;

    if (nameWidth < kNameColumnMin)
    {
        nameWidth = kNameColumnMin;
    }

    ListView_SetColumnWidth(m_hWnd, 0, static_cast<UINT>(nameWidth));
}

// ============================================================================
// 平铺视图（此电脑专用）
//
// 系统在「此电脑」里显示的是平铺：左边 48px 图标，右边磁盘名，
// 磁盘名下面一行「97.7 GB 可用，共 234 GB」，再下面一条容量进度条。
//
// Windows 列表控件自己也有平铺视图（LVIF_TILEINFO），但它在虚拟模式
// （LVS_OWNERDATA）下不生效，所以这里改成「大图标布局 + 自己把整格画出来」。
// 控件只负责排队和命中，画什么由 DrawTile 全权决定。
// ============================================================================

namespace
{
    // 「97.7 GB 可用，共 234 GB」——和资源管理器一样的说法。
    // 数字交给 format::CapacityText：按 1024 进位自动换单位（GB/TB 都认）
    std::wstring DiskSpaceText(uint64_t freeBytes, uint64_t totalBytes)
    {
        return format::CapacityText(freeBytes) + L" 可用，共 " + format::CapacityText(totalBytes);
    }
}

int FileListView::IconIndexForEntry(const shell::ShellEntry& entry) const
{
    if (m_iconCache == nullptr)
    {
        return 0;
    }

    // 图标尺寸跟着视图走：大图标/平铺要 48px 那套索引，其余用 16px 那套
    const auto iconSize = CurrentIconSize();
    int        iconIndex = 0;

    // 系统命名空间里的项（枚举时手里有它的绝对 PIDL）：直接按 PIDL 问图标。
    // 这类项很多解析成字符串之后 Shell 就认不回来了（控制面板那种
    // `::{GUID}\0\显示名`），按字符串取只会得到 0 —— 也就是"图标不显示"。
    if (!entry.pidl.empty())
    {
        iconIndex = m_iconCache->GetIconIndexByPidl(entry.pidl, iconSize);
    }

    // 按 PIDL 没拿到（或本来就没有 PIDL）才走下面的兜底逻辑
    if (iconIndex <= 0)
    {
        if (entry.isFolder)
        {
            // 驱动器 / 虚拟文件夹（此电脑里的各个盘、网络位置等）：
            // 按路径问系统，才能显示硬盘、光驱、网络盘各自的图标；普通文件夹走统一图标
            const bool isDriveLetter = entry.fullPath.size() >= 2 &&
                                       entry.fullPath.size() <= 3 &&
                                       entry.fullPath[1] == L':';
            const bool isShellItem  = entry.fullPath.compare(0, 2, L"::") == 0;

            if (isDriveLetter || isShellItem)
            {
                iconIndex = m_iconCache->GetIconIndexByFullPath(entry.fullPath, iconSize);
                if (iconIndex <= 0)
                {
                    iconIndex = m_iconCache->GetIconIndex(entry.name, true, iconSize);
                }
            }
            else
            {
                iconIndex = m_iconCache->GetIconIndex(entry.name, true, iconSize);
            }
        }
        else if (NeedsOwnIcon(entry.name))
        {
            iconIndex = m_iconCache->GetIconIndexByFullPath(entry.fullPath, iconSize);
            if (iconIndex <= 0)
            {
                // 读不出图标（被占用、权限不足等）时退回按扩展名
                iconIndex = m_iconCache->GetIconIndex(entry.name, false, iconSize);
            }
        }
        else
        {
            iconIndex = m_iconCache->GetIconIndex(entry.name, false, iconSize);
        }
    }

    // 被剪切（Ctrl+X）的项目、隐藏项、系统文件：图标换成半透明版本
    //
    // 半透明图标不是"另存一份完整图标列表"，而是用到哪个图标才给哪个做一份
    // 50% alpha 的副本，塞进同一个 image list 并按"原索引 → 淡索引"缓存
    // （实现见 IconCache::GetDimmedIconIndex）。所以隐藏项接进来几乎不增加负担。
    // 驱动器、控制面板这类特殊项不参与淡化（见 ShouldSkipDimming）。
    const bool dimThisEntry = !ShouldSkipDimming(entry);

    if (dimThisEntry &&
        (IsCutPath(entry.fullPath) || entry.isHidden || entry.isSystemFile))
    {
        iconIndex = m_iconCache->GetDimmedIconIndex(iconIndex, iconSize);
    }

    return iconIndex;
}

// ============================================================================
// 平铺格子里"名字"文字块的位置
//
// 画名字和弹编辑框都用这一个函数算，编辑框才能正好压在名字上、把它盖住。
//   · 驱动器卡片（有容量信息）：名字固定一行，在格子上沿往下 11px
//   · 普通项：名字最多 3 行，按字体行高算，整体在格子里垂直居中
// 传入的 dc 必须已经选好列表字体，否则测出来的行高不对。
// ============================================================================
RECT FileListView::ComputeTileNameRect(HDC dc, const RECT& tileRect,
                                       const shell::ShellEntry& entry) const
{
    // 小图标 / 列表：16px 图标在左，名字从图标右边开始，单行、整格垂直居中。
    // 右边留白和小图标一致（小图标让出列间距留 8，列表一行占满只留 2）
    if (m_viewMode == ViewMode::SmallIcon || m_viewMode == ViewMode::List)
    {
        // 注意不能把变量叫 small：Windows 头文件里 small 是个宏（展开成 char）
        const int rightPad = (m_viewMode == ViewMode::SmallIcon) ? 8 : 2;
        RECT nameBox = { tileRect.left + 22, tileRect.top + 1,
                         tileRect.right - rightPad, tileRect.bottom - 1 };
        return nameBox;
    }

    const int textLeft = tileRect.left + 62;

    if (entry.diskTotal > 0)
    {
        // 驱动器卡片：名字一行 + 容量 + 进度条，名字位置是固定的
        RECT single = { textLeft, tileRect.top + 11, tileRect.right - 6, tileRect.top + 31 };
        return single;
    }

    // 量一下当前字体一行多高，用它算"3 行"的上限
    TEXTMETRICW metrics = {};
    int lineHeight = 19;
    if (::GetTextMetricsW(dc, &metrics))
    {
        lineHeight = static_cast<int>(metrics.tmHeight) + 2;
    }

    const int nameWidth = tileRect.right - 6 - textLeft;

    // 先按实际换行算出这段名字要占多高，再封顶到 3 行
    RECT measure = { 0, 0, nameWidth, 0 };
    ::DrawTextW(dc, entry.name.c_str(), -1, &measure,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);

    int nameHeight = static_cast<int>(measure.bottom - measure.top);
    const int maxNameHeight = lineHeight * 3;

    if (nameHeight > maxNameHeight)
    {
        nameHeight = maxNameHeight;
    }
    if (nameHeight < lineHeight)
    {
        nameHeight = lineHeight;
    }

    // 文字块在格子里垂直居中
    const int blockTop = tileRect.top + ((tileRect.bottom - tileRect.top) - nameHeight) / 2;

    RECT nameRect = { textLeft, blockTop, textLeft + nameWidth, blockTop + nameHeight };
    return nameRect;
}

void FileListView::DrawTile(HDC dc, const RECT& tileRect, size_t index, bool selected)
{
    if (m_result == nullptr || index >= m_result->entries.size())
    {
        return;
    }

    const shell::ShellEntry& entry = m_result->entries[index];

    // ---- 整格背景 + 文字颜色 ----
    //
    // 平铺是自绘的：以前选中时直接填系统高亮色（深蓝 + 白字），
    // 和详情/列表/大图标视图（Explorer 主题的浅蓝 + 深色文字）完全两样。
    // 现在选中背景交给同一套主题去画，文字颜色也从主题里取，四种视图看起来一致。
    COLORREF nameColor = ::GetSysColor(COLOR_WINDOWTEXT);
    COLORREF infoColor = RGB(96, 96, 96);

    // 先看这一项要不要淡化。淡化和选中互不影响 —— 选中着也要变灰，
    // 这样"图标半透明"和"文字变灰"同时发生，不会出现图标淡了字还黑着：
    //   · 被剪切的项目                        → RGB(160,160,160)
    //   · 隐藏的文件和文件夹、受保护的系统文件 → RGB(100,100,100)
    // 剪切优先：一个隐藏项被剪切时，按"被剪切"的颜色显示。
    // 驱动器、控制面板这类特殊项不参与淡化（见 ShouldSkipDimming），
    // 哪怕是剪贴板里刚 Ctrl+X 过它们也不变灰
    const bool skipDim       = ShouldSkipDimming(entry);
    const bool dimByCut      = (!skipDim && IsCutPath(entry.fullPath));
    const bool dimByAttribute = (!skipDim && !dimByCut &&
                                 (entry.isHidden || entry.isSystemFile));

    if (dimByCut)
    {
        nameColor = RGB(160, 160, 160);
        infoColor = RGB(160, 160, 160);
    }
    else if (dimByAttribute)
    {
        nameColor = RGB(100, 100, 100);
        infoColor = RGB(100, 100, 100);
    }

    if (selected)
    {
        // 改名时焦点在编辑框上、列表本身失焦，这时不要把选中项画成"灰底"
        // （系统失焦选中是浅灰），保持和有焦点时一样的颜色
        const bool focused = (::GetFocus() == m_hWnd) || (m_hEdit != nullptr);

        bool painted = false;

        HTHEME theme = ::OpenThemeData(m_hWnd, L"Listview");
        if (theme != nullptr)
        {
            painted = SUCCEEDED(::DrawThemeBackground(
                theme, dc, LVP_LISTITEM,
                focused ? LISS_SELECTED : LISS_SELECTEDNOTFOCUS,
                &tileRect, nullptr));

            // 只有"没淡化"的项才用主题文字色，淡化项保持上面定好的灰字
            if (painted && !dimByAttribute && !dimByCut)
            {
                COLORREF themeText = 0;
                if (SUCCEEDED(::GetThemeColor(theme, LVP_LISTITEM, LISS_SELECTED,
                                              TMT_TEXTCOLOR, &themeText)))
                {
                    nameColor = themeText;
                    infoColor = themeText;
                }
            }

            ::CloseThemeData(theme);
        }

        if (!painted)
        {
            // 拿不到主题（理论上不会）就退回系统高亮色，至少选中是看得出来的
            HBRUSH backBrush = ::CreateSolidBrush(::GetSysColor(COLOR_HIGHLIGHT));
            if (backBrush != nullptr)
            {
                ::FillRect(dc, &tileRect, backBrush);
                ::DeleteObject(backBrush);
            }

            if (!dimByAttribute && !dimByCut)
            {
                nameColor = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
                infoColor = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
            }
        }
    }
    else
    {
        HBRUSH backBrush = ::CreateSolidBrush(::GetSysColor(COLOR_WINDOW));
        if (backBrush != nullptr)
        {
            ::FillRect(dc, &tileRect, backBrush);
            ::DeleteObject(backBrush);
        }
    }

    // ---- 小图标 / 列表：16px 图标在左、单行名字在右，整格垂直居中 ----
    if (m_viewMode == ViewMode::SmallIcon || m_viewMode == ViewMode::List)
    {
        HGDIOBJ oldFont = ::SelectObject(dc, m_hFont);
        ::SetBkMode(dc, TRANSPARENT);

        HIMAGELIST smallList = ListView_GetImageList(m_hWnd, LVSIL_SMALL);
        const int iconTop = tileRect.top + ((tileRect.bottom - tileRect.top) - 16) / 2;

        if (smallList != nullptr)
        {
            ::ImageList_Draw(smallList, IconIndexForEntry(entry), dc,
                             tileRect.left + 2, iconTop, ILD_TRANSPARENT);
        }

        // 名字在图标右边：左起 22（图标 16 + 间距 4 + 2）。
        // 右边留白：小图标格宽固定，留 8 让出列间距；列表一行占满宽度，留 2 就够
        const int rightPad = (m_viewMode == ViewMode::SmallIcon) ? 8 : 2;

        // 正在改名的那一行不画名字（位置让给编辑框）
        if (static_cast<int>(index) != m_editingIndex)
        {
            RECT nameRect = { tileRect.left + 22, tileRect.top + 1,
                              tileRect.right - rightPad, tileRect.bottom - 1 };

            ::SetTextColor(dc, nameColor);
            ::DrawTextW(dc, entry.name.c_str(), -1, &nameRect,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        ::SelectObject(dc, oldFont);
        return;
    }

    // ---- 左边 48px 图标 ----
    // 垂直位置：和右边"名称 + 容量"两行文字整体居中对齐（系统平铺就是这么排的）。
    // 格子高 68，图标 48 → 上下各留 10；文字块从 +11 起两行（各 19px），中心也在 34 附近。
    HIMAGELIST imageList = ListView_GetImageList(m_hWnd, LVSIL_NORMAL);
    if (imageList != nullptr)
    {
        ::ImageList_Draw(imageList, IconIndexForEntry(entry), dc,
                         tileRect.left + 4, tileRect.top + 10, ILD_TRANSPARENT);
    }

    const int textLeft = tileRect.left + 62;

    ::SetBkMode(dc, TRANSPARENT);

    HGDIOBJ oldFont = ::SelectObject(dc, m_hFont);

    // ---- 名字：最多 3 行自动换行，放不下就在第三行末尾用省略号收住 ----
    //
    // 有容量信息的项（磁盘、网络盘）名字只占一行，把下面留给容量文字和进度条；
    // 普通文件和文件夹没有那两行，整格高度都能用来放名字
    // （格高 68，3 行大约 57，装得下）。
    //
    // 正在改名的这一行不画名字：控件在那个位置挂了一个编辑框，
    // 我们再画一遍会和框里的文字叠在一起（平铺是自绘的，控件不知道那儿有字）。
    const bool hasDiskInfo = (entry.diskTotal > 0);

    if (static_cast<int>(index) != m_editingIndex)
    {
        const RECT nameRect = ComputeTileNameRect(dc, tileRect, entry);

        ::SetTextColor(dc, nameColor);

        const DWORD nameFlags = hasDiskInfo
            ? (DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX)
            : (DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);

        // 单行时 DrawText 从矩形顶部开始排版，这里这些矩形本身已经是算好的位置
        RECT drawRect = nameRect;
        ::DrawTextW(dc, entry.name.c_str(), -1, &drawRect, nameFlags);
    }

    // ---- 第二行 + 进度条：只有查得到容量时才画（网络盘、空光驱没有）----
    if (entry.diskTotal > 0)
    {
        RECT infoRect = { textLeft, tileRect.top + 30, tileRect.right - 6, tileRect.top + 50 };
        ::SetTextColor(dc, infoColor);

        const std::wstring info = DiskSpaceText(entry.diskFree, entry.diskTotal);
        ::DrawTextW(dc, info.c_str(), -1, &infoRect,
                    DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // 进度条：轨道浅灰，已用部分蓝（快满了变红，跟系统一样）
        const int barLeft   = textLeft;
        const int barTop    = tileRect.top + 53;
        const int barHeight = 9;

        int barWidth = tileRect.right - 10 - barLeft;
        if (barWidth > 220)
        {
            barWidth = 220;   // 太宽的格子里进度条也别拉太长，跟系统比例接近
        }

        if (barWidth > 24)
        {
            RECT trackRect = { barLeft, barTop, barLeft + barWidth, barTop + barHeight };

            HBRUSH trackBrush = ::CreateSolidBrush(RGB(229, 229, 229));
            if (trackBrush != nullptr)
            {
                ::FillRect(dc, &trackRect, trackBrush);
                ::DeleteObject(trackBrush);
            }

            // 已用比例 = 1 - 可用/总量
            double usedRatio = 1.0 - (static_cast<double>(entry.diskFree) /
                                      static_cast<double>(entry.diskTotal));
            if (usedRatio < 0.0) { usedRatio = 0.0; }
            if (usedRatio > 1.0) { usedRatio = 1.0; }

            const int usedWidth = static_cast<int>(barWidth * usedRatio);

            if (usedWidth > 0)
            {
                RECT usedRect = { barLeft, barTop, barLeft + usedWidth, barTop + barHeight };

                const COLORREF usedColor = (usedRatio >= 0.9) ? RGB(209, 52, 56)
                                                              : RGB(38, 120, 214);

                HBRUSH usedBrush = ::CreateSolidBrush(usedColor);
                if (usedBrush != nullptr)
                {
                    ::FillRect(dc, &usedRect, usedBrush);
                    ::DeleteObject(usedBrush);
                }
            }

            // 1px 灰边框，让它看起来像系统的那条
            HPEN borderPen = ::CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
            if (borderPen != nullptr)
            {
                HGDIOBJ oldPen   = ::SelectObject(dc, borderPen);
                HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
                ::Rectangle(dc, trackRect.left, trackRect.top, trackRect.right, trackRect.bottom);
                ::SelectObject(dc, oldBrush);
                ::SelectObject(dc, oldPen);
                ::DeleteObject(borderPen);
            }
        }
    }

    ::SelectObject(dc, oldFont);
}

// ============================================================================
// 布局与数据
// ============================================================================



// 列表视图（单列那种）的每项宽度
//
// 控件自己在 LVS_LIST 下是按"这批里最长的名字"算列宽的，窗口拉宽以后它不会重算，
// 于是右边留一大片空白、名字还在老宽度里被截断。虚拟列表（LVS_OWNERDATA）下更不准。
//
// 所以列表视图也改用 LVS_ICON：格子宽直接设成"客户区宽"，一行只有一个格子，
// 看起来还是列表那种竖排，但每行占满宽度，名字能吃满整行。
void FileListView::LayoutListWidth()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    // 每行占满客户区，只扣掉竖直滚动条那一条（不然文字会顶到滚动条上）
    int itemWidth = (clientRect.right - clientRect.left) - ::GetSystemMetrics(SM_CXVSCROLL);
    if (itemWidth < 100)
    {
        itemWidth = 100;
    }

    // 行高 24：和小图标视图一致（两边改名框都是 24 高，框正好贴满一行、不压邻居）。
    // 宽度没变就别重复设：拖窗口时 Resize 会连续来，重复 SetIconSpacing 会让控件反复重排
    const DWORD currentSpacing = static_cast<DWORD>(
        ::SendMessageW(m_hWnd, LVM_GETITEMSPACING, FALSE, 0));
    if (static_cast<int>(LOWORD(currentSpacing)) == itemWidth &&
        static_cast<int>(HIWORD(currentSpacing)) == 24)
    {
        return;
    }

    ListView_SetIconSpacing(m_hWnd, itemWidth, 24);
}

void FileListView::Resize(int x, int y, int width, int height)
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    ::SetWindowPos(m_hWnd, nullptr, x, y, width, height,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    LayoutColumns();

    // 列表视图的名字占满宽度要跟着窗口走，否则拉宽窗口后右边一片空白、名字还是被截着
    if (m_viewMode == ViewMode::List)
    {
        LayoutListWidth();
    }

    // 平铺视图下格子宽度是固定的 300，这里不用跟着窗口改
}

void FileListView::SetResult(std::shared_ptr<shell::EnumerateResult> result)
{
    if (m_hWnd == nullptr)
    {
        m_result = std::move(result);
        return;
    }

    // 换目录 / 重扫时如果还有平铺的改名框挂着，先取消掉（旧行号马上就对不上了）
    if (m_hEdit != nullptr)
    {
        FinishTileRename(false);
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

    // 换目录 / 重新枚举了，之前攒的"按字母跳转"输入串就没意义了，清掉
    m_typeAhead.clear();
    m_typeAheadTick = 0;

    // 内容换了，之前记下的"拖放高亮"行号就对不上了，清掉（拖动中 DragOver 会重新设）
    m_dropHighlightPath.clear();

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

    // 逐行比对，找第一个不一样的位置。
    //
    // 光比路径不够：同一个路径上"隐藏 / 系统文件标记、大小、修改时间"都可能在
    // 目录之外被改掉（比如在资源管理器里把某个文件设成隐藏）。只比路径就会把
    // 这种变化当成"这一行没变"，于是界面不重绘，隐藏项要等切目录再回来才变淡。
    auto sameRow = [](const shell::ShellEntry& before, const shell::ShellEntry& after)
    {
        if (_wcsicmp(before.fullPath.c_str(), after.fullPath.c_str()) != 0)
        {
            return false;
        }

        if (before.isHidden != after.isHidden || before.isSystemFile != after.isSystemFile)
        {
            return false;
        }

        if (before.size != after.size)
        {
            return false;
        }

        return ::CompareFileTime(&before.modified, &after.modified) == 0;
    };

    while (common < smaller && sameRow(oldEntries[common], newEntries[common]))
    {
        ++common;
    }

    // 一行都没变：连重绘都不需要
    if (common == oldEntries.size() && common == newEntries.size())
    {
        return;
    }

    m_result = std::move(result);

    // 内容变了，行号可能挪位：之前的高亮作废（继续拖动时 DragOver 会重新设）
    m_dropHighlightPath.clear();

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

void FileListView::EnsureVisiblePath(const std::wstring& path)
{
    if (m_hWnd == nullptr || m_result == nullptr || path.empty())
    {
        return;
    }

    for (size_t i = 0; i < m_result->entries.size(); ++i)
    {
        if (_wcsicmp(m_result->entries[i].fullPath.c_str(), path.c_str()) == 0)
        {
            // 第二个参数 FALSE = 只要露出来就行，不要硬顶到最上面
            ListView_EnsureVisible(m_hWnd, static_cast<int>(i), FALSE);
            return;
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
// 拖放：落点命中测试 + 文件夹高亮
//
// 场景：从资源管理器拖一个文件到本程序列表里，鼠标如果正好压在某个文件夹
// 那一行上，文件应该放进这个文件夹（和资源管理器一样），而不是放进当前目录。
// 为了让用户看得出"要放进哪一个"，那一行会用系统自带的拖放高亮显示。
// ============================================================================

std::wstring FileListView::HitTestFolderPath(POINT ptClient) const
{
    if (m_hWnd == nullptr || m_result == nullptr)
    {
        return std::wstring();
    }

    if (ptClient.x < 0 || ptClient.y < 0)
    {
        return std::wstring();
    }

    LVHITTESTINFO hit = {};
    hit.pt = ptClient;

    // 返回命中的行号，-1 表示没压在行上（空白区、表头、下方留白）
    int row = ListView_HitTest(m_hWnd, &hit);

    if (row < 0 || static_cast<size_t>(row) >= m_result->entries.size())
    {
        return std::wstring();
    }

    // 整行都算命中（列表开了"整行选中"，压在其他列上也应该认这个文件夹）
    const shell::ShellEntry& entry = m_result->entries[static_cast<size_t>(row)];

    // 只有文件夹才能作为落点；压在文件行上按"没命中"处理，退回当前目录
    if (!entry.isFolder)
    {
        return std::wstring();
    }

    return entry.fullPath;
}

void FileListView::SetDropHighlight(const std::wstring& folderPath)
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    // 拖动过程中 DragOver 会一直调过来，路径没变就立刻返回，不做全表扫描
    if (m_dropHighlightPath == folderPath)
    {
        return;
    }

    if (!m_dropHighlightPath.empty() && !folderPath.empty() &&
        _wcsicmp(m_dropHighlightPath.c_str(), folderPath.c_str()) == 0)
    {
        return;
    }

    m_dropHighlightPath = folderPath;

    // 先把所有行的拖放高亮清掉（-1 表示"全部行"）
    ListView_SetItemState(m_hWnd, -1, 0, LVIS_DROPHILITED);

    if (folderPath.empty() || m_result == nullptr)
    {
        return;
    }

    for (size_t i = 0; i < m_result->entries.size(); ++i)
    {
        if (_wcsicmp(m_result->entries[i].fullPath.c_str(), folderPath.c_str()) == 0)
        {
            // 只设 DROPHILITED，不动选中状态（选中状态是用户自己的东西）
            ListView_SetItemState(m_hWnd, static_cast<int>(i), LVIS_DROPHILITED, LVIS_DROPHILITED);
            break;
        }
    }
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
    if (m_hWnd == nullptr || m_result == nullptr)
    {
        return;
    }

    int selectedIndex = ListView_GetNextItem(m_hWnd, -1, LVNI_SELECTED);
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_result->entries.size()))
    {
        return;
    }

    // 这三个视图都是"图标在左、名字在右"，控件自带编辑框按"图标正下方"摆，
    // 位置和大小都对不上，所以统一用自己建的编辑框
    if (m_viewMode == ViewMode::Tiles || m_viewMode == ViewMode::SmallIcon ||
        m_viewMode == ViewMode::List)
    {
        BeginTileRename(selectedIndex);
        return;
    }

    ::SetFocus(m_hWnd);
    HWND edit = ListView_EditLabel(m_hWnd, static_cast<UINT>(selectedIndex));

    // 控件没能开始编辑（比如这一行是分组标题，BEGINLABELEDIT 里已经拒掉了）
    if (edit == nullptr)
    {
        return;
    }

    const shell::ShellEntry& entry = m_result->entries[static_cast<size_t>(selectedIndex)];

    // 编辑框里的文字也主动灌一遍：小图标视图交给控件的名字是"截断 + 省略号"的显示文本，
    // 不能拿它当改名的初始内容（否则一提交就把名字改成截断版了）。
    ::SetWindowTextW(edit, entry.name.c_str());

    // ---- 只选中主文件名、不选扩展名（和资源管理器一致）----
    // 控件的编辑框默认是全选的（连扩展名一起），这里改回来
    int selectEnd = static_cast<int>(entry.name.size());
    if (!entry.isFolder)
    {
        const size_t dot = entry.name.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0)
        {
            selectEnd = static_cast<int>(dot);
        }
    }

    ::SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(selectEnd));
}

// ============================================================================
// 平铺视图的原地改名：自己建一个多行编辑框
//
// 为什么不用列表控件自带的原地改名：
//   · 自带的是单行编辑框，框比一行高时文字必定垂直居中，做不出"三行框 + 字贴左上"；
//   · 它的位置按"图标视图的 label 区"（图标正下方）算，和平铺"图标在左、名字在右"对不上；
//   · 虚拟列表里它从 LVN_GETDISPINFO 取初始文字，而平铺那边为了格子高度稳定交的是空串。
// 自己建框就能三件事全解决：位置照着名字矩形摆、高度按名字行数给、文字天然靠左上。
// ============================================================================

void FileListView::BeginTileRename(int index)
{
    if (m_hWnd == nullptr || m_result == nullptr)
    {
        return;
    }

    if (index < 0 || index >= static_cast<int>(m_result->entries.size()))
    {
        return;
    }

    const shell::ShellEntry& entry = m_result->entries[static_cast<size_t>(index)];

    // 分组标题行、占位格不是真文件，不能改名
    if (entry.isGroupHeader || entry.isSpacer)
    {
        return;
    }

    // 已经有一个编辑框挂着（比如连按两次 F2）：先把上一个按"提交"收尾
    if (m_hEdit != nullptr)
    {
        FinishTileRename(true);
    }

    RECT itemRect = {};
    if (!ListView_GetItemRect(m_hWnd, index, &itemRect, LVIR_BOUNDS))
    {
        return;
    }

    // 格子高度按行距夹一下（和自绘时的口径一致）
    const DWORD spacing = static_cast<DWORD>(
        ::SendMessageW(m_hWnd, LVM_GETITEMSPACING, FALSE, 0));
    const int rowHeight = static_cast<int>(HIWORD(spacing));

    if (rowHeight > 0 && (itemRect.bottom - itemRect.top) > rowHeight)
    {
        itemRect.bottom = itemRect.top + rowHeight;
    }

    // 用"画名字的同一个函数"算出名字文字块的位置，编辑框照着它摆，位置天然一致
    RECT nameRect = {};
    HDC measureDc = ::GetDC(m_hWnd);
    if (measureDc == nullptr)
    {
        return;
    }

    HGDIOBJ oldFont = ::SelectObject(measureDc, m_hFont);
    nameRect = ComputeTileNameRect(measureDc, itemRect, entry);
    ::SelectObject(measureDc, oldFont);
    ::ReleaseDC(m_hWnd, measureDc);

    // 平铺：多行框，高度按名字文字块给（最多三行），再多给 4px 余量，少给就会把字挤扁。
    // 小图标 / 列表：单行框，高度就一行字，框内可以左右滚（输入长了以后再自动加宽）。
    const bool singleLine = (m_viewMode == ViewMode::SmallIcon || m_viewMode == ViewMode::List);

    int editWidth = nameRect.right - nameRect.left;

    // 单行框（小图标 / 列表）：宽度按内容给 —— 文字多宽，框就多宽（+左右边距，再向右多 9px），
    // 不占用整格。短名字就是个小框，长名字往后拉，到控件右边界为止。
    if (singleLine)
    {
        int textWidth = 0;

        HDC textDc = ::GetDC(m_hWnd);
        if (textDc != nullptr)
        {
            HGDIOBJ oldTextFont = (m_hFont != nullptr) ? ::SelectObject(textDc, m_hFont) : nullptr;
            SIZE textSize = {};
            if (::GetTextExtentPoint32W(textDc, entry.name.c_str(),
                                        static_cast<int>(entry.name.size()), &textSize))
            {
                textWidth = static_cast<int>(textSize.cx);
            }
            if (oldTextFont != nullptr)
            {
                ::SelectObject(textDc, oldTextFont);
            }
            ::ReleaseDC(m_hWnd, textDc);
        }

        editWidth = textWidth + kRenameEditExtraWidth;

        // 上限：不能超过控件自己的客户区右边界（超出去的部分会被控件裁掉、看不见）
        RECT clientRect = {};
        ::GetClientRect(m_hWnd, &clientRect);
        const int limitWidth = static_cast<int>(clientRect.right) - static_cast<int>(nameRect.left);
        if (limitWidth > 0 && editWidth > limitWidth)
        {
            editWidth = limitWidth;
        }

        if (editWidth < 40)
        {
            editWidth = 40;   // 空名字这种极端情况给个最小宽度
        }
    }

    if (editWidth <= 40)
    {
        return;
    }

    int editHeight = static_cast<int>(nameRect.bottom - nameRect.top) + 4;
    int editTop    = nameRect.top;

    if (singleLine)
    {
        // 单行框高度统一 25（列表视图的行距也是 25，正好贴满一行、不会压到上下邻居；
        // 小图标的行距 24，多出的 1px 看不出来）
        editHeight = kRenameEditHeight;

        // 在名字那块里垂直居中摆，再整体上抬 1px（按用户要求）
        const int available = static_cast<int>(nameRect.bottom - nameRect.top);
        editTop = nameRect.top + (available > editHeight ? (available - editHeight) / 2 : 0);
        editTop -= 1;
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        ::GetWindowLongPtrW(m_hWnd, GWLP_HINSTANCE));

    // 这两种框用同一套消息处理（回车提交、Esc 取消、失焦提交）：
    //   平铺 = 多行框（ES_MULTILINE + ES_AUTOVSCROLL），文字靠左上、能装三行；
    //   小图标 / 列表 = 单行框（ES_AUTOHSCROLL），宽度随输入内容自动加长
    const DWORD editStyle = (singleLine)
        ? (WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL)
        : (WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL);

    m_hEdit = ::CreateWindowExW(
        0,
        L"EDIT",
        entry.name.c_str(),
        editStyle,
        nameRect.left, editTop, editWidth, editHeight,
        m_hWnd,
        nullptr,
        instance,
        nullptr);

    if (m_hEdit == nullptr)
    {
        return;
    }

    if (m_hFont != nullptr)
    {
        ::SendMessageW(m_hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    }

    // 左右各留 3px，字不要顶死在边框上
    ::SendMessageW(m_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(3, 3));

    ::SetWindowSubclass(m_hEdit, &FileListView::EditSubclassProc, 1,
                        reinterpret_cast<DWORD_PTR>(this));

    // 记住"正在改名"和"改的是哪一行"：DrawTile 见到这一行就不画名字，
    // 目录监视的自动刷新也要绕开（否则编辑框会被刷掉）
    m_editingLabel = true;
    m_editingIndex = index;

    // 只选中主文件名、不选扩展名（文件夹没有扩展名，整段选中）
    int selectEnd = static_cast<int>(entry.name.size());
    if (!entry.isFolder)
    {
        const size_t dot = entry.name.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0)
        {
            selectEnd = static_cast<int>(dot);
        }
    }

    ::SetFocus(m_hEdit);
    ::SendMessageW(m_hEdit, EM_SETSEL, 0, static_cast<LPARAM>(selectEnd));

    // 先把这一格重画一遍（名字已经被跳过，不再画），再把新框露出来
    ::InvalidateRect(m_hWnd, &itemRect, FALSE);
    ::UpdateWindow(m_hWnd);
}

// ============================================================================
// 单行改名框跟着输入内容伸缩
//
// 小图标 / 列表的改名框宽度是"文字宽 + 边距"算出来的：
// 打字变长就往后拉、删字就收回来。上限是列表控件自己的客户区右边界
// （再往外就被控件裁掉，看不见）。
// ============================================================================

void FileListView::GrowRenameEditWidth()
{
    if (m_hEdit == nullptr || m_hWnd == nullptr || m_editingIndex < 0 ||
        m_result == nullptr || m_editingIndex >= static_cast<int>(m_result->entries.size()))
    {
        return;
    }

    // 多行框（平铺）宽度固定，换行就行，不用伸缩
    if (m_viewMode == ViewMode::Tiles)
    {
        return;
    }

    // 当前输入的文字有多宽
    wchar_t buffer[512] = {};
    ::GetWindowTextW(m_hEdit, buffer, static_cast<int>(std::size(buffer)));

    RECT editRect = {};
    ::GetWindowRect(m_hEdit, &editRect);
    ::MapWindowPoints(nullptr, m_hWnd, reinterpret_cast<POINT*>(&editRect), 2);

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    int currentWidth = static_cast<int>(editRect.right - editRect.left);
    int textWidth    = 0;

    HDC dc = ::GetDC(m_hWnd);
    if (dc != nullptr)
    {
        HGDIOBJ oldFont  = (m_hFont != nullptr) ? ::SelectObject(dc, m_hFont) : nullptr;
        SIZE    textSize = {};
        if (::GetTextExtentPoint32W(dc, buffer, static_cast<int>(::wcslen(buffer)), &textSize))
        {
            textWidth = static_cast<int>(textSize.cx);
        }
        if (oldFont != nullptr)
        {
            ::SelectObject(dc, oldFont);
        }
        ::ReleaseDC(m_hWnd, dc);
    }

    // 文字宽 + 边距；不超过控件客户区右边界；也不缩到 40 以下
    int wantWidth = textWidth + kRenameEditExtraWidth;
    const int limitWidth = static_cast<int>(clientRect.right) - static_cast<int>(editRect.left);
    if (limitWidth > 0 && wantWidth > limitWidth)
    {
        wantWidth = limitWidth;
    }
    if (wantWidth < 40)
    {
        wantWidth = 40;
    }

    // 变长变短都要跟（删字时框收回来，才和"按内容"一致）
    if (wantWidth != currentWidth)
    {
        ::SetWindowPos(m_hEdit, nullptr, 0, 0, wantWidth, static_cast<int>(editRect.bottom - editRect.top),
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ============================================================================
// 平铺改名收尾：提交（回车 / 失焦）或取消（Esc）
// ============================================================================

void FileListView::FinishTileRename(bool commit)
{
    HWND edit = m_hEdit;
    const int index = m_editingIndex;

    m_hEdit = nullptr;
    m_editingIndex = -1;
    m_editingLabel = false;

    if (edit == nullptr || m_hWnd == nullptr)
    {
        return;
    }

    std::wstring newName;

    if (commit)
    {
        const int length = ::GetWindowTextLengthW(edit);
        if (length > 0)
        {
            std::wstring raw(static_cast<size_t>(length) + 1, L'\0');
            ::GetWindowTextW(edit, raw.data(), length + 1);
            raw.resize(static_cast<size_t>(length));

            // 多行编辑框里可能混进换行（粘贴多行文本时），文件名里不能有，统一换成空格
            std::wstring cleaned;
            cleaned.reserve(raw.size());

            for (wchar_t ch : raw)
            {
                if (ch == L'\r' || ch == L'\n')
                {
                    if (!cleaned.empty() && cleaned.back() != L' ')
                    {
                        cleaned.push_back(L' ');
                    }
                    continue;
                }
                cleaned.push_back(ch);
            }

            // 去掉首尾空白
            while (!cleaned.empty() && (cleaned.front() == L' ' || cleaned.front() == L'\t'))
            {
                cleaned.erase(cleaned.begin());
            }
            while (!cleaned.empty() && (cleaned.back() == L' ' || cleaned.back() == L'\t'))
            {
                cleaned.pop_back();
            }

            newName = cleaned;
        }
    }

    // 焦点还给列表（这一步之后失焦消息还会再来一次，但 m_hEdit 已经清空了，会被忽略）
    ::SetFocus(m_hWnd);

    ::RemoveWindowSubclass(edit, &FileListView::EditSubclassProc, 1);
    ::DestroyWindow(edit);

    // 把那一格重画回去，名字重新出现
    if (index >= 0 && m_result != nullptr && index < static_cast<int>(m_result->entries.size()))
    {
        RECT itemRect = {};
        if (ListView_GetItemRect(m_hWnd, index, &itemRect, LVIR_BOUNDS))
        {
            const DWORD spacing = static_cast<DWORD>(
                ::SendMessageW(m_hWnd, LVM_GETITEMSPACING, FALSE, 0));
            const int rowHeight = static_cast<int>(HIWORD(spacing));

            if (rowHeight > 0 && (itemRect.bottom - itemRect.top) > rowHeight)
            {
                itemRect.bottom = itemRect.top + rowHeight;
            }

            ::InvalidateRect(m_hWnd, &itemRect, FALSE);
        }
    }

    // 真正去改名：只有名字确实变了才叫主窗口干活
    if (commit && !newName.empty() && index >= 0 && m_onRename &&
        m_result != nullptr && index < static_cast<int>(m_result->entries.size()) &&
        newName != m_result->entries[static_cast<size_t>(index)].name)
    {
        m_onRename(static_cast<size_t>(index), newName);
    }
}

// ============================================================================
// 自建编辑框的子类过程：回车提交、Esc 取消、失焦提交
// ============================================================================

LRESULT CALLBACK FileListView::EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                LPARAM lParam, UINT_PTR id, DWORD_PTR refData)
{
    FileListView* self = reinterpret_cast<FileListView*>(refData);

    if (self == nullptr)
    {
        return ::DefSubclassProc(hWnd, msg, wParam, lParam);
    }

    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN)
        {
            self->FinishTileRename(true);
            return 0;
        }
        if (wParam == VK_ESCAPE)
        {
            self->FinishTileRename(false);
            return 0;
        }
        break;

    case WM_CHAR:
        // 多行编辑框默认会把回车/退格插进文本，这里吃掉，避免文件名里出现换行
        if (wParam == 13 || wParam == 27)
        {
            return 0;
        }
        break;

    case WM_KILLFOCUS:
        // 点到别处（列表里别的行、地址栏、菜单）就当改名结束，和资源管理器一致。
        // 不能在失焦过程中同步销毁自己，排到消息队列后面再收尾。
        if (self->m_hEdit == hWnd)
        {
            ::PostMessageW(self->m_hWnd, kMsgFinishTileRename, 0, 0);
        }
        break;

    case WM_NCDESTROY:
        ::RemoveWindowSubclass(hWnd, &FileListView::EditSubclassProc, id);
        break;

    default:
        break;
    }

    return ::DefSubclassProc(hWnd, msg, wParam, lParam);
}

// ============================================================================
// 按字母跳转
//
// 和资源管理器「在视图中选择键入项」一套行为：
//   1. 连续输入的字符累积成串（间隔超过 500ms 清零重来）；
//   2. 从当前焦点项的下一个开始，找名字以这个串开头的项；
//   3. 找到末尾绕回开头继续找（最多绕一圈）；
//   4. 累积串一个都没匹配上，就退回用最后一个字符再找一次
//      —— 这就是"连按同一个字母能一直往下循环"的原理；
//   5. 命中后只选中它（清掉原来的多选）、滚到可见位置，不打开。
//
// 中文名按拼音首字母匹配，多音字的任一读音首字母都算命中（见 PinyinInitialMask）。
 // ============================================================================

LRESULT CALLBACK FileListView::ListViewSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                    LPARAM lParam, UINT_PTR id, DWORD_PTR refData)
{
    FileListView* self = reinterpret_cast<FileListView*>(refData);

    if (msg == WM_CHAR && self != nullptr)
    {
        // 注意：控件自己也有"单字母跳转"，返回 0 把它吃掉就不会跳第二次
        if (self->HandleTypeAheadCharacter(static_cast<wchar_t>(wParam)))
        {
            return 0;
        }
    }
    else if (msg == WM_NCDESTROY)
    {
        // 窗口要销毁了，把子类化摘掉（否则回调里还拿着已经释放的 this）
        ::RemoveWindowSubclass(hWnd, &FileListView::ListViewSubclassProc, id);
    }
    else if (msg == kMsgFinishTileRename && self != nullptr)
    {
        // 平铺改名框失焦后排队回来的收尾（提交）
        self->FinishTileRename(true);
        return 0;
    }
    else if (msg == WM_COMMAND && self != nullptr && self->m_hEdit != nullptr &&
             HIWORD(wParam) == EN_CHANGE &&
             reinterpret_cast<HWND>(lParam) == self->m_hEdit)
    {
        // 单行改名框：输入内容变长就把框跟着加长（多行框不动）
        self->GrowRenameEditWidth();
        return 0;
    }

    return ::DefSubclassProc(hWnd, msg, wParam, lParam);
}

int FileListView::FindTypeAheadMatch(const std::wstring& needle, int startIndex) const
{
    if (m_result == nullptr || m_result->entries.empty())
    {
        return -1;
    }

    const int count = static_cast<int>(m_result->entries.size());

    // 从焦点项的下一项开始；没有焦点项（startIndex < 0）就从第一项开始
    const int begin = (startIndex >= 0) ? (startIndex + 1) % count : 0;

    for (int step = 0; step < count; ++step)
    {
        const int index = (begin + step) % count;
        const shell::ShellEntry& entry = m_result->entries[static_cast<size_t>(index)];

        // 分组标题行和占位格不是真实文件，跳过
        if (entry.isGroupHeader || entry.isSpacer)
        {
            continue;
        }

        if (NameMatchesTypeAheadPrefix(entry.name, needle))
        {
            return index;
        }
    }

    return -1;
}

bool FileListView::HandleTypeAheadCharacter(wchar_t ch)
{
    if (m_hWnd == nullptr || m_result == nullptr || m_result->entries.empty())
    {
        return false;
    }

    // 只处理可打印字符。控制字符（回车、退格、Esc 等）原样交给控件和主窗口，
    // 免得把"回车打开、Esc 取消"这类操作抢掉。
    if (ch < 0x20 || ch == 0x7F)
    {
        return false;
    }

    // 大写统一成小写，比较时就不用再折一次
    if (ch >= L'A' && ch <= L'Z')
    {
        ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }

    // 超时（500ms）就当成新的一轮输入
    const DWORD now = ::GetTickCount();
    if (m_typeAhead.empty() || now - m_typeAheadTick > kTypeAheadTimeoutMs)
    {
        m_typeAhead.clear();
    }

    m_typeAheadTick = now;
    m_typeAhead.push_back(ch);

    const int startIndex = ListView_GetNextItem(m_hWnd, -1, LVNI_FOCUSED);

    // 第一轮：用完整的累积串找
    int foundIndex = FindTypeAheadMatch(m_typeAhead, startIndex);

    if (foundIndex < 0 && m_typeAhead.size() > 1)
    {
        // 第二轮：累积串没命中（比如连按了两次 a，串变成 "aa"），
        // 退回只用一个字符重新找 —— 这样连按同一个字母能一直循环到下一个
        m_typeAhead.assign(1, m_typeAhead.back());
        foundIndex = FindTypeAheadMatch(m_typeAhead, startIndex);
    }

    if (foundIndex < 0)
    {
        // 一个都没匹配上：不动作，也不响，保持安静（系统在没匹配时也不给提示）
        return true;   // 字符仍然吃掉，别让它跑进控件触发默认的单字母跳转
    }

    // 清掉原来的多选，只选中命中的这一项（和系统一致：输入字母等于"选这一项"）
    ListView_SetItemState(m_hWnd, -1, 0, LVIS_SELECTED);

    ListView_SetItemState(m_hWnd, foundIndex,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);

    ListView_EnsureVisible(m_hWnd, foundIndex, FALSE);

    return true;
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

        // ---- 占位格（平铺视图里用来把组标题顶成独占一行的空档）----
        // 什么都不给：没有名字、没有图标，就是一块空白
        if (entry.isSpacer)
        {
            if ((displayInfo->item.mask & LVIF_IMAGE) != 0)
            {
                displayInfo->item.iImage = I_IMAGENONE;
            }
            if ((displayInfo->item.mask & LVIF_TEXT) != 0)
            {
                displayInfo->item.pszText = const_cast<wchar_t*>(L"");
            }
            return 0;
        }

        // ---- 分组标题行（如「此电脑」里的「设备和驱动器」）----
        // 这种行只有名字，不要图标、不要日期/类型/大小
        if (entry.isGroupHeader)
        {
            if ((displayInfo->item.mask & LVIF_IMAGE) != 0)
            {
                displayInfo->item.iImage = I_IMAGENONE;   // 不画图标，名字从最左边开始
            }

            if ((displayInfo->item.mask & LVIF_TEXT) != 0 && displayInfo->item.iSubItem == 0)
            {
                displayInfo->item.pszText = const_cast<wchar_t*>(entry.name.c_str());
            }
            return 0;
        }

        // ---- 图标（只对整行生效，与具体列无关）----
        if ((displayInfo->item.mask & LVIF_IMAGE) != 0)
        {
            displayInfo->item.iImage = IconIndexForEntry(entry);
        }

        // ---- 文本 ----
        if ((displayInfo->item.mask & LVIF_TEXT) == 0)
        {
            return 0;
        }

        switch (displayInfo->item.iSubItem)
        {
        case 0:   // 名称
            // 平铺、小图标、列表都是整格自己画的，控件这里的文字只被它用来算格子大小：
            // 名字越长它把格子算得越高/越宽（平铺能算到 89 高，行距只有 68），
            // 自绘的背景就会溢出压到下一行、或者列宽被顶大。
            // 既然用不到，交空串，让格子尺寸只按图标算，稳定下来。
            if (m_viewMode == ViewMode::Tiles || m_viewMode == ViewMode::SmallIcon ||
                m_viewMode == ViewMode::List)
            {
                displayInfo->item.pszText = const_cast<wchar_t*>(L"");
            }
            else
            {
                displayInfo->item.pszText = const_cast<wchar_t*>(entry.name.c_str());
            }
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
    // 开始拖动：把选中的这一批交给系统拖动循环
    // （DoDragDrop 是阻塞的：这个分支返回时拖动已经结束）
    // --------------------------------------------------------------
    case LVN_BEGINDRAG:
    {
        NMLISTVIEW* dragInfo = reinterpret_cast<NMLISTVIEW*>(lParam);
        std::vector<std::wstring> paths = GetSelectedPaths();

        if (!paths.empty())
        {
            // 对外只声明复制和移动（和资源管理器自己一样）；
            // 在本程序内部按 Alt 建快捷方式，由我们自己的放置目标处理，不依赖这个声明
            DWORD effect = shell::DoDragFiles(paths, m_hWnd, dragInfo->ptAction,
                                              DROPEFFECT_COPY | DROPEFFECT_MOVE);

            // 移动走了：源目录里的东西已经没了，需要主窗口更新列表
            if (effect == DROPEFFECT_MOVE && m_onDragCompleted)
            {
                m_onDragCompleted(effect);
            }
        }
        return 0;
    }

    // --------------------------------------------------------------
    // 双击：交给主窗口处理（文件夹进目录，文件用默认程序打开）
    // --------------------------------------------------------------
    case NM_DBLCLK:
    {
        // 用"鼠标命中的那一项"而不是"当前选中项"：
        // 按住 Ctrl 点第一下会把这个项取消选中，等第二下触发双击时已经没有
        // 选中项了，靠选中项取会什么都打不开。
        const NMITEMACTIVATE* item = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
        int hitIndex = (item != nullptr) ? item->iItem : -1;

        if (hitIndex < 0)
        {
            // 兜底：没有命中项（比如键盘回车触发的）就还按选中项走
            hitIndex = ListView_GetNextItem(m_hWnd, -1, LVNI_SELECTED);
        }

        if (hitIndex >= 0 && m_onActivate)
        {
            // 用 GetAsyncKeyState 而不是 GetKeyState：前者读的是"此刻真实的物理按键状态"，
            // 后者跟着本线程消息队列走，遇到注入的按键（自动化测试、远程桌面）会读到旧值。
            const bool ctrlHeld = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            m_onActivate(static_cast<size_t>(hitIndex), ctrlHeld);
        }
        return 0;
    }

    // --------------------------------------------------------------
    // 单击：平铺视图补"再点一下已经选中的那一项就进改名"
    //
    // 详情 / 列表 / 大图标这些视图是控件原生支持的（点中已选中项的名字就进编辑），
    // 平铺是整格自绘的，控件不知道名字画在哪儿，所以自己判，判法很简单：
    //   · 点中的这一项就是当前唯一选中的那一项；
    //   · 和上一次点同一项之间隔了超过系统双击时间 —— 那就是两下单点，不是双击。
    // 快速双击仍由 NM_DBLCLK 走"打开"，不会被这里抢走。
    // --------------------------------------------------------------
    case NM_CLICK:
    {
        // 平铺 / 小图标 / 列表：单击"已经选中的那一项"就是进改名（和资源管理器一致）。
        // 双击仍然走 NM_DBLCLK 打开文件，这里用"距上次点击是否超过双击时间"分开两种情况。
        if ((m_viewMode == ViewMode::Tiles || m_viewMode == ViewMode::SmallIcon ||
             m_viewMode == ViewMode::List) && m_result != nullptr)
        {
            const NMITEMACTIVATE* click = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
            const int hitIndex = (click != nullptr) ? click->iItem : -1;

            if (hitIndex >= 0 && hitIndex < static_cast<int>(m_result->entries.size()))
            {
                const shell::ShellEntry& hit = m_result->entries[static_cast<size_t>(hitIndex)];
                const bool isRealItem = (!hit.isGroupHeader && !hit.isSpacer);

                const bool singleSelectedHere =
                    isRealItem &&
                    ListView_GetSelectedCount(m_hWnd) == 1 &&
                    (ListView_GetItemState(m_hWnd, hitIndex, LVIS_SELECTED) & LVIS_SELECTED) != 0;

                const ULONGLONG now = ::GetTickCount64();

                if (singleSelectedHere && m_lastClickItem == hitIndex &&
                    (now - m_lastClickTick) > static_cast<ULONGLONG>(::GetDoubleClickTime()))
                {
                    // 隔了比双击时间更长：这是"又点了一下已经选中的项"，进改名
                    m_lastClickItem = -1;
                    m_lastClickTick = 0;
                    BeginRenameSelected();
                }
                else
                {
                    // 记下来，等下一次点击时拿来算间隔
                    m_lastClickItem = hitIndex;
                    m_lastClickTick = now;
                }
            }
            else
            {
                m_lastClickItem = -1;
                m_lastClickTick = 0;
            }
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
            // 去掉选中行/当前项外面那圈点状虚线。
            // 那圈虚线是 Windows 列表控件的"键盘焦点提示框"（告诉你按键会作用在这一行），
            // 资源管理器在主题风格下不画它，所以这里把焦点位清掉，让主题只画选中背景。
            customDraw->nmcd.uItemState &= ~CDIS_FOCUS;

            size_t index = static_cast<size_t>(customDraw->nmcd.dwItemSpec);

            // ---- 占位格：就是一块空白，什么都不画（挡住控件默认的背景）----
            if (m_result != nullptr && index < m_result->entries.size() &&
                m_result->entries[index].isSpacer)
            {
                RECT spacerRect = {};
                if (ListView_GetItemRect(m_hWnd, static_cast<int>(index), &spacerRect, LVIR_BOUNDS))
                {
                    ::FillRect(customDraw->nmcd.hdc, &spacerRect,
                               ::GetSysColorBrush(COLOR_WINDOW));
                }
                return CDRF_SKIPDEFAULT;
            }

            // ---- 分组标题行：整行自己画 ----
            // 样子对齐系统：白底、深灰粗体字、左边缩进一点点、没有选中效果、没有图标
            if (m_result != nullptr && index < m_result->entries.size() &&
                m_result->entries[index].isGroupHeader)
            {
                RECT rowRect = {};
                if (ListView_GetItemRect(m_hWnd, static_cast<int>(index), &rowRect, LVIR_BOUNDS))
                {
                    // 平铺视图里一格只有 300 宽，但标题要像系统那样铺满一整行，
                    // 所以右边一直画到列表右边缘（后面的占位格也是白的，看不出接缝）
                    RECT clientRect = {};
                    ::GetClientRect(m_hWnd, &clientRect);
                    rowRect.right = clientRect.right;

                    ::FillRect(customDraw->nmcd.hdc, &rowRect, ::GetSysColorBrush(COLOR_WINDOW));

                    HGDIOBJ oldFont  = ::SelectObject(customDraw->nmcd.hdc,
                                                      (m_hHeaderFont != nullptr) ? m_hHeaderFont : m_hFont);
                    int     oldMode  = ::SetBkMode(customDraw->nmcd.hdc, TRANSPARENT);
                    COLORREF oldColor = ::SetTextColor(customDraw->nmcd.hdc, RGB(96, 96, 96));

                    rowRect.left += 6;   // 左边留一点缩进，看起来像小标题

                    ::DrawTextW(customDraw->nmcd.hdc,
                                m_result->entries[index].name.c_str(), -1,
                                &rowRect,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                    ::SetTextColor(customDraw->nmcd.hdc, oldColor);
                    ::SetBkMode(customDraw->nmcd.hdc, oldMode);
                    ::SelectObject(customDraw->nmcd.hdc, oldFont);
                }

                return CDRF_SKIPDEFAULT;   // 这一行已经画完了，别再让控件画
            }

            // ---- 平铺 / 小图标 / 列表：每个格子整块自己画 ----
            // 平铺画 48px 图标 + 三行名字 + 容量进度条；小图标画 16px 图标 + 单行名字；
            // 列表一行占满宽度、也是 16px 图标 + 单行名字。具体分支在 DrawTile 里按 m_viewMode 走
            if (m_viewMode == ViewMode::Tiles || m_viewMode == ViewMode::SmallIcon ||
                m_viewMode == ViewMode::List)
            {
                RECT tileRect = {};
                if (ListView_GetItemRect(m_hWnd, static_cast<int>(index), &tileRect, LVIR_BOUNDS))
                {
                    // 格子的高度取控件的行距（SetIconSpacing 设的 68），不用 LVIR_BOUNDS 给的高度。
                    // 控件那个高度是按名字文本换行算出来的，长名字会比行距大，
                    // 照它画背景就会把下面一行压住。这里夹一下，兜住各种 DPI / 主题差异。
                    const DWORD spacing = static_cast<DWORD>(
                        ::SendMessageW(m_hWnd, LVM_GETITEMSPACING, FALSE, 0));
                    const int rowHeight = static_cast<int>(HIWORD(spacing));

                    if (rowHeight > 0 && (tileRect.bottom - tileRect.top) > rowHeight)
                    {
                        tileRect.bottom = tileRect.top + rowHeight;
                    }

                    // 选中状态必须问控件（owner-data 下 uItemState 里的位不可靠，
                    // 之前直接读 CDIS_SELECTED 导致每格都被当成"选中"的画成蓝底）
                    const bool selected =
                        (ListView_GetItemState(m_hWnd, static_cast<int>(index), LVIS_SELECTED) &
                         LVIS_SELECTED) != 0;

                    DrawTile(customDraw->nmcd.hdc, tileRect, index, selected);
                }
                return CDRF_SKIPDEFAULT;
            }

            // ---- 隐藏项 / 系统文件 / 剪切项：文字画淡 ----
            //
            // 图标已经走半透明（IconIndexForEntry 里统一处理），这里只管文字颜色：
            //   · 被剪切的项目                        → 160 灰
            //   · 隐藏的文件和文件夹、受保护的系统文件 → 100 灰（两者观感一样）
            // 剪切优先：隐藏项 / 系统文件被剪切时，显示"被剪切"的颜色。
            //
            // 和"选中"无关：选中着也要变灰。剪切、改成隐藏这些动作发生时，
            // 用户往往正把那一项选中着，如果非要等它被取消选中才变灰，
            // 就会出现"图标立刻淡了、文字还黑着"的割裂感。
            if (m_result != nullptr && index < m_result->entries.size())
            {
                const shell::ShellEntry& entry = m_result->entries[index];

                // 驱动器、控制面板这类特殊项一律不淡化，被剪切也不变灰
                if (ShouldSkipDimming(entry))
                {
                    return CDRF_DODEFAULT;
                }

                if (IsCutPath(entry.fullPath))
                {
                    customDraw->clrText = RGB(160, 160, 160);
                    return CDRF_NEWFONT;   // 告诉控件按我改的颜色画
                }

                if (entry.isHidden || entry.isSystemFile)
                {
                    customDraw->clrText = RGB(100, 100, 100);
                    return CDRF_NEWFONT;
                }
            }

            return CDRF_DODEFAULT;
        }

        default:
            break;
        }
        return CDRF_DODEFAULT;
    }

    // --------------------------------------------------------------
    // 选中状态即将改变：分组标题行不许选中
    //
    // 返回 TRUE 就是"拒绝这次改变"，控件会把这一行保持原样。
    // 这样单击它、Ctrl+A 全选都选不上它，和系统的组标题行为一致。
    // --------------------------------------------------------------
    case LVN_ITEMCHANGING:
    {
        const NMLISTVIEW* changeInfo = reinterpret_cast<const NMLISTVIEW*>(lParam);

        if (m_result != nullptr &&
            changeInfo->iItem >= 0 &&
            static_cast<size_t>(changeInfo->iItem) < m_result->entries.size() &&
            m_result->entries[static_cast<size_t>(changeInfo->iItem)].isGroupHeader &&
            (changeInfo->uNewState & LVIS_SELECTED) != 0)
        {
            return TRUE;
        }

        // 占位格同样不许选中（否则 Ctrl+A 会选中一片空白，看着像 bug）
        if (m_result != nullptr &&
            changeInfo->iItem >= 0 &&
            static_cast<size_t>(changeInfo->iItem) < m_result->entries.size() &&
            m_result->entries[static_cast<size_t>(changeInfo->iItem)].isSpacer &&
            (changeInfo->uNewState & LVIS_SELECTED) != 0)
        {
            return TRUE;
        }

        return FALSE;
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
    // 用户拖表头改完某一列的宽度（松手那一刻通知）
    //
    // 只记「修改日期 / 类型 / 大小」这三列；名称列永远自适应吃剩余宽度，
    // 拖它没有意义，所以不记（和资源管理器的行为一致）。
    // --------------------------------------------------------------
    case HDN_ENDTRACK:
    {
        const NMHEADERW* headerInfo = reinterpret_cast<const NMHEADERW*>(lParam);
        if (headerInfo == nullptr || headerInfo->iItem < 1)
        {
            return 0;
        }

        const int newWidth = ListView_GetColumnWidth(m_hWnd, headerInfo->iItem);

        switch (headerInfo->iItem)
        {
        case 1: m_timeColumnWidth = newWidth; break;
        case 2: m_typeColumnWidth = newWidth; break;
        case 3: m_sizeColumnWidth = newWidth; break;
        default: return 0;
        }

        LayoutColumns();   // 名称列跟着重算

        if (m_onColumnWidthChanged)
        {
            m_onColumnWidthChanged();
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
        if (m_result == nullptr || index < 0 || index >= static_cast<int>(m_result->entries.size()) ||
            m_result->entries[static_cast<size_t>(index)].isGroupHeader ||
            m_result->entries[static_cast<size_t>(index)].isSpacer)
        {
            return TRUE;   // 返回真 = 不允许编辑（分组标题、占位格都不能改名）
        }

        info->item.mask |= LVIF_TEXT;
        info->item.pszText = const_cast<wchar_t*>(m_result->entries[static_cast<size_t>(index)].name.c_str());

        // 记住"现在正在改名"：目录监视的自动刷新要绕开这一小段时间
        m_editingLabel = true;

        // 平铺是自绘的，控件不知道名字画在哪儿，没法自己把字擦掉。
        // 记下正在编辑的行号，DrawTile 见到这一行就不画名字，把位置让给编辑框。
        m_editingIndex = index;

        return FALSE;      // 返回假 = 允许编辑
    }

    // --------------------------------------------------------------
    // 改名结束：拿到新名字就去做真正的重命名
    // --------------------------------------------------------------
    case LVN_ENDLABELEDIT:
    {
        NMLVDISPINFOW* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);

        m_editingLabel = false;   // 改名结束了（不管有没有改成功）

        // 把刚才让给编辑框的那一行重新画回来（名字要重新出现）
        if (m_editingIndex >= 0)
        {
            const int editedIndex = m_editingIndex;
            m_editingIndex = -1;

            if (m_viewMode == ViewMode::Tiles)
            {
                RECT itemRect = {};
                if (ListView_GetItemRect(m_hWnd, editedIndex, &itemRect, LVIR_BOUNDS))
                {
                    const DWORD spacing = static_cast<DWORD>(
                        ::SendMessageW(m_hWnd, LVM_GETITEMSPACING, FALSE, 0));
                    const int rowHeight = static_cast<int>(HIWORD(spacing));

                    if (rowHeight > 0 && (itemRect.bottom - itemRect.top) > rowHeight)
                    {
                        itemRect.bottom = itemRect.top + rowHeight;
                    }

                    ::InvalidateRect(m_hWnd, &itemRect, FALSE);
                }
            }
        }

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
