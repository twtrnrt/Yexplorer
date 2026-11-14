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

    void SetActivateCallback(ActivateCallback callback)       { m_onActivate    = std::move(callback); }
    void SetContextMenuCallback(ContextMenuCallback callback) { m_onContextMenu = std::move(callback); }
    void SetColumnClickCallback(ColumnClickCallback callback) { m_onColumnClick = std::move(callback); }
    void SetRenameCallback(RenameCallback callback)           { m_onRename      = std::move(callback); }

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

private:
    HWND                                          m_hWnd      = nullptr;
    std::shared_ptr<shell::EnumerateResult>        m_result;
    ActivateCallback                              m_onActivate;
    ContextMenuCallback                           m_onContextMenu;
    ColumnClickCallback                           m_onColumnClick;
    RenameCallback                                m_onRename;
    Comparator                                    m_comparator;
    HFONT                                         m_hFont     = nullptr;

    // 图标缓存不归本控件所有（由主窗口持有并保证生命周期），这里只借指针
    shell::IconCache*                             m_iconCache = nullptr;

    // LVN_GETDISPINFO 回调里生成的文本缓冲（必须比回调活得久，所以放成员）
    std::wstring m_cacheTime;
    std::wstring m_cacheType;
    std::wstring m_cacheSize;
};
