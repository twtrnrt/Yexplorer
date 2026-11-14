// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/NavigationPane.h
//
// 左侧导航栏（简化版）。
//
// 【为什么是"简化版"】
//   资源管理器的左栏是「可一层层展开的树」：点开 C: 能看到子文件夹，再点开子文件夹…
//   那是这块最费工的地方（每展开一个节点都要重新枚举一次目录、还要记住展开状态）。
//
//   这里做成固定两项：
//     · 快速访问：桌面 / 下载 / 文档 / 图片 / 音乐
//     · 此电脑：各个硬盘
//   点一下直接跳过去，不展开子文件夹 —— 效果等同于在资源管理器里双击那个硬盘。
//
// 【外观】
//   用系统树形控件，关掉连线（不设 TVS_HASLINES）、整行高亮（TVS_FULLROWSELECT），
//   和资源管理器左栏观感一致。图标直接复用 ShellIcon 的系统图像列表。
// ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>

#include "shell/ShellIcon.h"

#include <functional>
#include <string>
#include <vector>

class NavigationPane
{
public:
    using NavigateCallback = std::function<void(const std::wstring& path)>;

    NavigationPane() = default;
    ~NavigationPane();

    NavigationPane(const NavigationPane&) = delete;
    NavigationPane& operator=(const NavigationPane&) = delete;

    bool Create(HWND parent, HINSTANCE hInstance, shell::IconCache* iconCache);
    void Resize(int x, int y, int width, int height);

    void SetNavigateCallback(NavigateCallback callback) { m_onNavigate = std::move(callback); }

    HWND Handle() const { return m_hWnd; }

    LRESULT HandleNotify(LPARAM lParam);

private:
    void BuildTree();
    void ApplyFont();

    // 插入一个分组标题（例如「快速访问」），不可点击跳转
    void InsertGroup(HTREEITEM parentItem, const std::wstring& text, int iconIndex);

    // 插入一个可跳转的条目
    void InsertLeaf(HTREEITEM parentItem, const std::wstring& text,
                    const std::wstring& path, int iconIndex);

    // 枚举所有硬盘
    void AddDrives(HTREEITEM parentItem);

    // 取系统已知文件夹路径（桌面/下载/文档/图片/音乐）
    static bool GetKnownFolderPath(const GUID& folderId, std::wstring& outPath);

private:
    HWND              m_hWnd      = nullptr;
    HFONT             m_hFont     = nullptr;
    HIMAGELIST        m_hImageList = nullptr;
    shell::IconCache* m_iconCache  = nullptr;

    NavigateCallback  m_onNavigate;

    // 每个树节点对应的数据，lParam 里存的是这个数组的下标
    struct NodeData
    {
        std::wstring path;
        bool         isGroup = false;
    };
    std::vector<NodeData> m_nodes;
};
