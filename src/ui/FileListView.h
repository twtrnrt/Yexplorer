// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/FileListView.h
//
// 文件列表控件（ListView 虚拟模式封装）。
//
// 【为什么用虚拟模式 LVS_OWNERDATA】
//   普通模式要把每一行的数据都塞进控件，一万个文件就意味着控件内部维护
//   一万条记录，打开大目录会明显卡顿。
//   虚拟模式只告诉控件「一共有多少行」，具体每行显示什么，由控件在需要
//   绘制时回调我们（LVN_GETDISPINFO）临时取。这样十万级条目也不卡。
//
// 【列顺序】按 Windows 资源管理器的默认顺序：
//   名称 / 修改日期 / 类型 / 大小
//
// 【刷新时不闪的做法】
//   同一个目录刷新（删了文件、加了文件）时，不要清空列表再重填，
//   而是记住滚动位置和选中项，换完数据后按名字恢复，
//   这样视觉上是「少了一行/多了一行」，不是「整屏闪一下」。
// ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>

#include "core/Format.h"
#include "shell/ShellFolder.h"
#include "shell/ShellIcon.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class FileListView
{
public:
    // ------------------------------------------------------------------
    // 视图模式（对应资源管理器「查看」菜单里的四项）
    //
    //   LargeIcon —— 大图标（48px 图标 + 名字在下面，横向铺开）
    //   SmallIcon —— 小图标（16px 图标 + 名字在右边，纵向多列）
    //   List     —— 列表（16px 图标 + 名字，单列从上往下）
    //   Details  —— 详细信息（带列头，名称/日期/类型/大小）
    //   Tiles    —— 平铺（48px 图标 + 右侧名称和容量文字 + 容量进度条）
    //
    // 前四个都是同一个 ListView 控件换样式（LVS_ICON / LVS_SMALLICON / LVS_LIST / LVS_REPORT），
    // 虚拟模式 LVS_OWNERDATA 在任何视图下都成立，所以十万级条目照样不卡。
    //
    // Tiles（平铺）是「此电脑」专用的：系统在列表控件的平铺视图里能画容量进度条，
    // 但那种平铺（LVIF_TILEINFO）在虚拟模式（LVS_OWNERDATA）下不生效，
    // 所以这里用「大图标布局 + 自己把每个格子整块画出来」来实现，效果一样。
    // ------------------------------------------------------------------
    enum class ViewMode
    {
        LargeIcon,
        SmallIcon,
        List,
        Details,
        Tiles
    };

    // 双击回调：index = 被双击的项，openInNewWindow = 当时按着 Ctrl（要求在新窗口打开）
    using ActivateCallback    = std::function<void(size_t index, bool openInNewWindow)>;
    using ContextMenuCallback = std::function<void(POINT screenPoint)>;
    using ColumnClickCallback = std::function<void(int column)>;
    using RenameCallback      = std::function<void(size_t index, const std::wstring& newName)>;

    // 拖完之后通知：参数是最终效果（DROPEFFECT_MOVE 表示文件被搬走了）
    using DragCompletedCallback = std::function<void(DWORD effect)>;

    // 排序比较器：返回 true 表示 left 应该排在 right 前面
    using Comparator = std::function<bool(const shell::ShellEntry& left, const shell::ShellEntry& right)>;

    // 用户拖完表头、改了三列固定列的宽度之后通知主窗口（主窗口负责存进配置）
    using ColumnWidthChangedCallback = std::function<void()>;

    FileListView() = default;
    ~FileListView();

    FileListView(const FileListView&) = delete;
    FileListView& operator=(const FileListView&) = delete;

    bool Create(HWND parent, HINSTANCE hInstance);

    // 设置图标缓存（必须在 SetResult 之前设置，否则列表没有图标）
    void SetIconCache(shell::IconCache* iconCache);

    // ------------------------------------------------------------------
    // 切换视图模式（对应「查看」菜单）
    //
    // 换了视图要重挂图像列表：大图标视图用 48px 那张，其余用 16px 那张，
    // 否则图标会被控件拉伸出马赛克。
    // ------------------------------------------------------------------
    void SetViewMode(ViewMode mode);
    ViewMode CurrentViewMode() const { return m_viewMode; }

    // ------------------------------------------------------------------
    // 平铺视图一行能并排几块卡片
    //
    // 「此电脑」的分组标题要靠"占位格"把这行剩下的位置填掉才能独占一行，
    // 占位格的数量必须和控件真实排列的列数一致，否则窗口一改宽度
    // （比如从 3 列变 2 列）就整片错位。
    // 所以这里不写死 300，而是问控件自己要的间距（LVM_GETITEMSPACING）。
    // ------------------------------------------------------------------
    int TilesPerRow() const;

    void Resize(int x, int y, int width, int height);

    // 换一整个结果（换目录 / 重新枚举后）
    // 同一个目录刷新时会自动保留滚动位置和选中项
    void SetResult(std::shared_ptr<shell::EnumerateResult> result);

    // 最小范围更新：对比新旧结果，只重绘「从第一个变化处往下」的部分。
    // 同一个目录里复制/移动完再枚举时用这个，前面没变的行不重画，看着不闪。
    void MergeResult(std::shared_ptr<shell::EnumerateResult> result);

    // 后台角标判定完成后补角标：按完整路径找到对应项，改 overlayIndex 再重画。
    // path 是判定针对的目录，和当前显示目录不一致（用户已切走）就整批忽略。
    void ApplyOverlayIndices(const std::wstring& path,
                             const std::vector<std::pair<std::wstring, int>>& updates);

    // 设置「被剪切的项」：这些行显示成半透明灰（和资源管理器一致）
    void SetCutPaths(const std::vector<std::wstring>& paths);

    // 当前列表显示的是哪个目录（还没数据时返回空串）
    std::wstring CurrentPath() const;

    // 当前列表顶部那一项的完整路径（切标签时恢复滚动位置）
    std::wstring GetTopItemPath() const;

    // 当前列表里所有项的完整路径（用来对比"刷新后多了哪些"）
    std::vector<std::wstring> AllPaths() const;

    // 只选中这些路径（先把已有选中全清掉）。粘贴完成后用来高亮新出现的东西
    void SelectPaths(const std::vector<std::wstring>& paths);

    // 滚动到某个路径那一项，让它出现在可见区域里（列表里没有这一项就什么都不做）。
    // 启动时命令行传进来一个文件、要在父目录里高亮它，就靠这个把视图滚过去。
    void EnsureVisiblePath(const std::wstring& path);

    void SetActivateCallback(ActivateCallback callback)       { m_onActivate    = std::move(callback); }
    void SetContextMenuCallback(ContextMenuCallback callback) { m_onContextMenu = std::move(callback); }
    void SetColumnClickCallback(ColumnClickCallback callback) { m_onColumnClick = std::move(callback); }
    void SetRenameCallback(RenameCallback callback)           { m_onRename      = std::move(callback); }
    void SetColumnWidthChangedCallback(ColumnWidthChangedCallback callback) { m_onColumnWidthChanged = std::move(callback); }

    // 三列固定列的宽度（修改日期 / 类型 / 大小）。启动时用配置里存的值，
    // 没存过就按默认 160 / 120 / 110。名称列不受这个影响，它永远自适应。
    void SetFixedColumnWidths(int time, int type, int size);
    void GetFixedColumnWidths(int& time, int& type, int& size) const
    {
        time = m_timeColumnWidth;
        type = m_typeColumnWidth;
        size = m_sizeColumnWidth;
    }

    // 拖完之后回调：效果是"移动"说明文件已经被搬走，需要主窗口刷新列表
    void SetDragCompletedCallback(DragCompletedCallback callback) { m_onDragCompleted = std::move(callback); }

    // Ctrl+加号：三个固定列（修改日期/类型/大小）按"表头文字和内容里较宽的那个"收窄，
    // 名称列继续吃剩余宽度（这样不会冒出横向滚动条）。只在详细信息视图下有作用。
    void AutoFitColumns();

    // 拖出去之前的"换路径"回调。
    //
    // 压缩包内部的项是虚拟路径（C:\a\t.rar\sub\inner.txt），外部程序按这个路径
    // 什么都读不到，所以由主窗口先把它们解到临时目录，返回真实路径。
    // 返回空 = 这一批拖不了（调用方就别开始拖动）。
    // 没注册这个回调时，原样使用列表里的路径（普通磁盘文件就是这种情况）。
    using PrepareDragPathsCallback =
        std::function<std::vector<std::wstring>(const std::vector<std::wstring>& paths)>;
    void SetPrepareDragPathsCallback(PrepareDragPathsCallback callback)
    {
        m_onPrepareDragPaths = std::move(callback);
    }

    // 当前选中项的总字节数（判断"要不要显示解压进度窗"用）。
    // 只看已经枚举好的大小，不额外查磁盘。
    unsigned long long SelectedTotalSize() const;

    // 选中信息汇总（状态栏用，一次遍历拿三项）：
    //   count     选中了几项（组标题行、占位格不可选，天然不会算进来）
    //   hasFolder 选中的里面有没有文件夹
    //   totalSize 选中项的总字节数（文件夹按 0 算）
    // 只读内存里的条目，不碰磁盘，选中上千项也只是遍历一次数组。
    void SelectedSummary(size_t& count, bool& hasFolder, unsigned long long& totalSize) const;

    // 选中状态变化回调：主窗口拿它刷新状态栏里的"选中 N 项 / 总大小"
    using SelectionChangedCallback = std::function<void()>;
    void SetSelectionChangedCallback(SelectionChangedCallback callback)
    {
        m_onSelectionChanged = std::move(callback);
    }

    // 某一个路径的字节数（按完整路径查，找不到返回 0；目录按 0 算）。
    // 拖出压缩包里的项时用它算"这一项解完该有多大"，进度条靠这个算百分比。
    unsigned long long SizeOfPath(const std::wstring& path) const;

    // 某一个路径的修改时间（按完整路径查；找不到或容器没给时间时返回全 0）。
    // 拖出/打开压缩包里的项时用它当"缓存对不对得上"的基准。
    FILETIME ModifiedOfPath(const std::wstring& path) const;

    // 这个路径在列表里是不是文件夹（找不到就当不是）。
    // 拖出压缩包内的项时，文件夹要单独展开成文件清单算进度，靠它区分。
    bool IsFolderPath(const std::wstring& path) const;

    // 排序规则（用于新建项时判断插到哪个位置）
    void SetComparator(Comparator comparator) { m_comparator = std::move(comparator); }

    // 在表头上画排序箭头：column 这一列显示 ▲（升序）或 ▼（降序）
    void SetSortState(int column, bool ascending);

    HWND Handle() const { return m_hWnd; }

    // 列表的表头控件（SysHeader32）。
    // 拖表头改列宽的通知是从"表头"发出来的，不是从列表发的，
    // 所以主窗口转发通知时两边都要认。
    HWND HeaderHandle() const
    {
        return (m_hWnd != nullptr) ? ListView_GetHeader(m_hWnd) : nullptr;
    }

    // 取当前所有选中项的完整路径（复制/剪切/删除要用）
    std::vector<std::wstring> GetSelectedPaths() const;

    // 当前列表里一共有多少项
    size_t ItemCount() const;

    // 取某一项的名字 / 完整路径（重命名、局部更新用）
    std::wstring ItemName(size_t index) const;
    std::wstring ItemPath(size_t index) const;

    // 全选（Ctrl+A）
    void SelectAll();

    // 局部删掉一项（删文件之后用，不用重新扫整个目录）
    bool RemoveItemByPath(const std::wstring& fullPath);

    // 局部插入一项（新建文件夹之后用），按当前排序规则放好位置并选中它
    void InsertEntry(const shell::ShellEntry& entry);

    // 开始原地改名（F2）
    void BeginRenameSelected();

    // 平铺视图的原地改名：自己建一个多行编辑框（自带的是单行框，做不到三行高 + 字贴左上）
    void BeginTileRename(int index);

    // 平铺改名收尾：commit=true 提交改名，false 取消
    void FinishTileRename(bool commit);

    // 自建编辑框的子类过程：回车提交、Esc 取消、失焦提交
    static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

    // 单行改名框（小图标 / 列表）：输入内容变长时把框跟着往右加宽
    void GrowRenameEditWidth();

    // 详细信息 / 大图标视图由控件自己进入改名时，补设"只选主文件名、不选扩展名"
    void SelectMainNameInEdit();

    // 是否正在原地改名（列表上有个编辑框挂着）
    // 目录监视触发的自动刷新要避开这个时刻，否则编辑框会被冲掉
    bool IsEditingLabel() const { return m_editingLabel; }

    // ---- 拖放相关 ----

    // 落点命中测试：如果这个客户区坐标压在某个"文件夹行"上，返回那个文件夹的完整路径。
    // 返回空串表示没压在文件夹上（空白处、或者压在文件行上）。
    // 拖进列表时用它判断"是要放进这个子文件夹，还是放进当前目录"。
    std::wstring HitTestFolderPath(POINT ptClient) const;

    // 把某个文件夹行画成"拖放高亮"（系统自带的 LVIS_DROPHILITED 效果）。
    // 传空串表示清除所有高亮。路径没变时直接返回，不做任何事（拖动过程中会被高频调用）。
    void SetDropHighlight(const std::wstring& folderPath);

    LRESULT HandleNotify(LPARAM lParam);

private:
    void ApplyColumns();
    void LayoutColumns();

    // 列表视图（单列那种）：控件自己按"最长名字"算列宽，窗口变宽后它不会重算，
    // 结果右边留一大片空白、名字还在老位置被截断。这里把每项宽度按客户区重设一遍。
    void LayoutListWidth();
    void ApplyFont();

    // 按当前视图决定这项该用小图标还是大图标索引
    shell::IconCache::IconSize CurrentIconSize() const
    {
        return (m_viewMode == ViewMode::LargeIcon || m_viewMode == ViewMode::Tiles)
                   ? shell::IconCache::IconSize::Large
                   : shell::IconCache::IconSize::Small;
    }

    // 算某一项该用哪个图标索引（绘图和回调共用一套逻辑）
    int IconIndexForEntry(const shell::ShellEntry& entry) const;

    // 后台图标取好了（收到 shell::kMessageIconReady）：把结果塞进图像列表并重画
    void OnIconsReady();

    // 平铺视图：把一个格子（图标 + 名称 + 容量 + 进度条）整块画出来
    void DrawTile(HDC dc, const RECT& tileRect, size_t index, bool selected);

    // 平铺视图：算出格子里"名字"文字块的位置。
    // 画名字和弹编辑框都用它，编辑框才能正好压在名字上。小图标视图也走这里算名字区。
    RECT ComputeTileNameRect(HDC dc, const RECT& tileRect, const shell::ShellEntry& entry) const;

    // 在表头上去掉所有排序箭头
    void ClearSortArrows();

    // 某一项是不是"正在被剪切"的（显示成半透明）
    bool IsCutPath(const std::wstring& fullPath) const;

    // 刷新后把用户之前选中的、滚动的位置还原回去
    void RestoreSelectionAndScroll(const std::vector<std::wstring>& selectedNames,
                                   const std::wstring&              topName);

    // ------------------------------------------------------------------
    // 按字母跳转（和资源管理器「在视图中选择键入项」那一套一致）
    //
    // 规则：连续按下的字符累积成串（间隔超过 500ms 就清零重来），
    //       从当前焦点项的下一个开始找"名字以这个串开头"的项，
    //       找到末尾绕回开头；累积串没有匹配时退回用最后一个字符再找一次
    //       （这就是"连按同一个字母能一直往下循环"的原理）。
    //       中文名按拼音首字母参与匹配（文档 → wd，「重」的 z/c/t 都算命中）。
    // 实现：子类化列表控件，拦 WM_CHAR，吃掉字符，避免控件自己再做一次单字母跳转。
    // ------------------------------------------------------------------
    static LRESULT CALLBACK ListViewSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

    // 处理一个输入字符：返回 true 表示已经跳转、这个字符被消费掉了
    bool HandleTypeAheadCharacter(wchar_t ch);

    // 从 startIndex 之后开始按 needle 找匹配项（扫到末尾再绕回开头），找不到返回 -1
    int FindTypeAheadMatch(const std::wstring& needle, int startIndex) const;

    std::wstring m_typeAhead;              // 累积的输入串
    DWORD        m_typeAheadTick = 0;      // 上一次按键的 GetTickCount，用来判断超时

private:
    HWND                                          m_hWnd      = nullptr;
    std::shared_ptr<shell::EnumerateResult>        m_result;
    ActivateCallback                              m_onActivate;
    ContextMenuCallback                           m_onContextMenu;
    ColumnClickCallback                           m_onColumnClick;
    RenameCallback                                m_onRename;
    DragCompletedCallback                         m_onDragCompleted;
    PrepareDragPathsCallback                      m_onPrepareDragPaths;
    Comparator                                    m_comparator;
    ColumnWidthChangedCallback                    m_onColumnWidthChanged;
    SelectionChangedCallback                      m_onSelectionChanged;

    // 三列固定列的宽度（用户可以拖表头改，改完会存进配置）
    int                                           m_timeColumnWidth = 160;
    int                                           m_typeColumnWidth = 120;
    int                                           m_sizeColumnWidth = 110;

    // 上一次做列宽排版时的控件宽度。
    // 用途：只有宽度真的变了才需要重排列宽；拖上下边框、或只改高度时宽度不变，
    // 每次都 SetColumnWidth 会让整个列表白白重排一遍。
    int                                           m_lastLayoutWidth = -1;
    HFONT                                         m_hFont     = nullptr;
    HFONT                                         m_hHeaderFont = nullptr;   // 分组标题用的粗体字

    // 图标缓存不归本控件所有（由主窗口持有并保证生命周期），这里只借指针
    shell::IconCache*                             m_iconCache = nullptr;

    // LVN_GETDISPINFO 回调里生成的文本缓冲（必须比回调活得久，所以放成员）
    std::wstring m_cacheTime;
    std::wstring m_cacheType;
    std::wstring m_cacheSize;

    // 被剪切（Ctrl+X）的完整路径列表：这些行画成灰色/半透明
    std::vector<std::wstring> m_cutPaths;

    // 当前被"拖放高亮"的文件夹（拖进来的东西会落进它），空串表示没有
    std::wstring              m_dropHighlightPath;

    // 是否正在原地改名（LVN_BEGINLABELEDIT 到 LVN_ENDLABELEDIT 之间）
    bool                      m_editingLabel = false;

    // 正在改名的那一行（-1 表示没有）。
    // 平铺是自绘的，控件不知道名字画在哪儿，所以改名时由我们跳过这一行的名字，
    // 把位置让给控件的编辑框；改名结束后再画回来。
    int                       m_editingIndex = -1;

    // 平铺视图自建的改名编辑框（多行框）；其它视图用控件自带的那个，这里保持 nullptr
    HWND                      m_hEdit = nullptr;

    // 平铺视图"再点一下已选中项就进改名"用：上一次点中的是哪一项、什么时候点的。
    // 没有文字可比（owner-data 列表控件的 item 文本在平铺下是空的），只能靠时间判断
    // 这两下点击是"两下单点"还是"一次双击"。
    int                       m_lastClickItem = -1;
    ULONGLONG                 m_lastClickTick = 0;

    // 当前视图模式（默认详细信息，和资源管理器打开普通文件夹时一致）
    ViewMode                  m_viewMode = ViewMode::Details;
};
