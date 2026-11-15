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

#include "core/Settings.h"
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

    // 某个客户区坐标落在哪个节点上：返回该节点对应的路径；
    // 空白处、分隔条、或者不是文件夹的节点，返回空串
    // （拖放时用来判断"放到了哪个目录"）
    std::wstring HitTestPath(POINT ptClient) const;

    // 拖放：这个坐标落在哪个「可以放东西」的节点上。
    // 返回 nullptr 表示这里不能放（空白处、分隔条、或者压在展开箭头上）。
    // 和 HitTestPath 的判断规则一致，只是返回的是节点本身，方便给它画高亮。
    HTREEITEM HitTestDroppableItem(POINT ptClient) const;

    // 把某个节点画成"拖放高亮"（系统自带的 TVIS_DROPHILITED，和资源管理器左栏一样）。
    // 传 nullptr 表示清除。节点没变时直接返回——拖动过程中这个函数会被高频调用。
    void SetDropHighlight(HTREEITEM item);

    // 让左栏的选中状态跟着当前目录走：
    //   · 当前目录正好是左栏某个节点 → 选中那一项
    //   · 不是（比如进到了某个硬盘里的子文件夹）→ 清空选中
    //
    // 为什么要清空：左栏只在"选中项发生变化"时才通知导航。
    // 如果一直亮着"桌面"，用户进了子文件夹后再点"桌面"，
    // 选中项没变化 → 控件不发通知 → 点了没反应（这就是"回不去桌面"的原因）。
    // 清空之后"桌面"回到未选中状态，再点它就是"从无到有"，通知正常发出。
    void SelectFolderPath(const std::wstring& path);

    // ---- 固定区（磁盘区下面那一段，用户自己拖进来的文件夹/程序）----
    //
    // 传进来的这一批就是固定区现在的全部内容，左栏会整树重建一遍。
    // 数据本身存在 core::Settings（配置文件）里，这里只是显示副本。
    void SetPinnedItems(const std::vector<core::PinnedItem>& items);

    // 右键固定项 →「取消固定」时回调，参数是那一项的完整路径。
    // 真正从配置文件删、再刷新左栏，由外面（MainWnd）做。
    using UnpinCallback = std::function<void(const std::wstring& path)>;
    void SetUnpinCallback(UnpinCallback callback) { m_onUnpin = std::move(callback); }

    // 这个客户区坐标是不是落在固定区里（拖放时用来判断"拖进来就固定"）
    bool IsInPinnedArea(POINT ptClient) const;

    LRESULT HandleNotify(LPARAM lParam);

private:
    void BuildTree();
    void ApplyFont();

    // 取某个节点对应的目录路径（分隔条、空白返回空串）
    std::wstring ItemPath(HTREEITEM item) const;

    // 插入一条"分隔线"用的占位节点（不是目录，点了不跳转、也不能放东西）
    void InsertSeparator(HTREEITEM parentItem);

    // 插入一个可跳转的条目
    void InsertLeaf(HTREEITEM parentItem, const std::wstring& text,
                    const std::wstring& path, int iconIndex);

    // 插入一条固定项（文件夹或程序）
    void InsertPinned(HTREEITEM parentItem, const core::PinnedItem& item);

    // 插入"固定区空着"时那行灰色提示
    void InsertHint(HTREEITEM parentItem, const std::wstring& text);

    // 右键菜单（只有压着固定项时才弹）
    LRESULT ShowContextMenu(int screenX, int screenY);

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
        bool         isSeparator = false;   // 这一行是分隔线占位，不是目录
        bool         isPinned    = false;   // 这一行是固定区里的项（右键可取消固定）
        bool         isHint      = false;   // 这一行是"把文件夹拖到这里"的提示文字
        HTREEITEM    item        = nullptr; // 这一行对应的树节点句柄（用来反选中）
    };
    std::vector<NodeData> m_nodes;

    // 固定区现在的显示内容（数据真身在 core::Settings 里）
    std::vector<core::PinnedItem> m_pinnedItems;

    UnpinCallback m_onUnpin;

    // 固定区起点（客户区 y 坐标，就是固定区上面那条分隔线的行顶），-1 表示没测到
    int m_pinnedTop = -1;

    // 当前被"拖放高亮"的节点（鼠标正压着的那一行），nullptr 表示没有
    HTREEITEM         m_dropHighlightItem = nullptr;
};
