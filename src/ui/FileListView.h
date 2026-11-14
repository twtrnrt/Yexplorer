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
    using ActivateCallback    = std::function<void(size_t index)>;
    using ContextMenuCallback = std::function<void(POINT screenPoint)>;
    using ColumnClickCallback = std::function<void(int column)>;
    using RenameCallback      = std::function<void(size_t index, const std::wstring& newName)>;

    // 拖完之后通知：参数是最终效果（DROPEFFECT_MOVE 表示文件被搬走了）
    using DragCompletedCallback = std::function<void(DWORD effect)>;

    // 排序比较器：返回 true 表示 left 应该排在 right 前面
    using Comparator = std::function<bool(const shell::ShellEntry& left, const shell::ShellEntry& right)>;

    FileListView() = default;
    ~FileListView();

    FileListView(const FileListView&) = delete;
    FileListView& operator=(const FileListView&) = delete;

    bool Create(HWND parent, HINSTANCE hInstance);

    // 设置图标缓存（必须在 SetResult 之前设置，否则列表没有图标）
    void SetIconCache(shell::IconCache* iconCache);

    void Resize(int x, int y, int width, int height);

    // 换一整个结果（换目录 / 重新枚举后）
    // 同一个目录刷新时会自动保留滚动位置和选中项
    void SetResult(std::shared_ptr<shell::EnumerateResult> result);

    // 最小范围更新：对比新旧结果，只重绘「从第一个变化处往下」的部分。
    // 同一个目录里复制/移动完再枚举时用这个，前面没变的行不重画，看着不闪。
    void MergeResult(std::shared_ptr<shell::EnumerateResult> result);

    // 设置「被剪切的项」：这些行显示成半透明灰（和资源管理器一致）
    void SetCutPaths(const std::vector<std::wstring>& paths);

    // 当前列表显示的是哪个目录（还没数据时返回空串）
    std::wstring CurrentPath() const;

    // 当前列表里所有项的完整路径（用来对比"刷新后多了哪些"）
    std::vector<std::wstring> AllPaths() const;

    // 只选中这些路径（先把已有选中全清掉）。粘贴完成后用来高亮新出现的东西
    void SelectPaths(const std::vector<std::wstring>& paths);

    void SetActivateCallback(ActivateCallback callback)       { m_onActivate    = std::move(callback); }
    void SetContextMenuCallback(ContextMenuCallback callback) { m_onContextMenu = std::move(callback); }
    void SetColumnClickCallback(ColumnClickCallback callback) { m_onColumnClick = std::move(callback); }
    void SetRenameCallback(RenameCallback callback)           { m_onRename      = std::move(callback); }

    // 拖完之后回调：效果是"移动"说明文件已经被搬走，需要主窗口刷新列表
    void SetDragCompletedCallback(DragCompletedCallback callback) { m_onDragCompleted = std::move(callback); }

    // 排序规则（用于新建项时判断插到哪个位置）
    void SetComparator(Comparator comparator) { m_comparator = std::move(comparator); }

    // 在表头上画排序箭头：column 这一列显示 ▲（升序）或 ▼（降序）
    void SetSortState(int column, bool ascending);

    HWND Handle() const { return m_hWnd; }

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

    LRESULT HandleNotify(LPARAM lParam);

private:
    void ApplyColumns();
    void LayoutColumns();
    void ApplyFont();

    // 在表头上去掉所有排序箭头
    void ClearSortArrows();

    // 某一项是不是"正在被剪切"的（显示成半透明）
    bool IsCutPath(const std::wstring& fullPath) const;

    // 刷新后把用户之前选中的、滚动的位置还原回去
    void RestoreSelectionAndScroll(const std::vector<std::wstring>& selectedNames,
                                   const std::wstring&              topName);

private:
    HWND                                          m_hWnd      = nullptr;
    std::shared_ptr<shell::EnumerateResult>        m_result;
    ActivateCallback                              m_onActivate;
    ContextMenuCallback                           m_onContextMenu;
    ColumnClickCallback                           m_onColumnClick;
    RenameCallback                                m_onRename;
    DragCompletedCallback                         m_onDragCompleted;
    Comparator                                    m_comparator;
    HFONT                                         m_hFont     = nullptr;

    // 图标缓存不归本控件所有（由主窗口持有并保证生命周期），这里只借指针
    shell::IconCache*                             m_iconCache = nullptr;

    // LVN_GETDISPINFO 回调里生成的文本缓冲（必须比回调活得久，所以放成员）
    std::wstring m_cacheTime;
    std::wstring m_cacheType;
    std::wstring m_cacheSize;

    // 被剪切（Ctrl+X）的完整路径列表：这些行画成灰色/半透明
    std::vector<std::wstring> m_cutPaths;
};
