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
#include <mutex>
#include <string>
#include <thread>
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

    // 角标配置 / 总开关变了之后重新判定一遍（丢掉上一轮结果，重新问处理器）
    void RefreshOverlay();

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

    // 右键任意一个"能用"的节点（桌面 / 下载 / 硬盘 / 常用区里的文件夹）
    // 时，把这一项交给外面（MainWindow）去弹菜单、执行命令。
    //
    // 菜单不在这里拼：菜单内容要和文件列表里"文件夹右键"保持一致
    // （打开 / 在新窗口中打开 / 复制文件路径 / 在终端中打开 / 属性），
    // 那套逻辑和命令执行都在 MainWindow 里，放在这里会变成两份维护。
    //
    // isPinned —— 这一项是不是常用区里的固定项（菜单里会多一条「取消固定」）
    // isFolder —— 这一项是不是文件夹（程序类固定项没有"目录"可用，菜单里只剩「取消固定」）
    using ContextMenuCallback = std::function<void(const std::wstring& path,
                                                   bool isPinned,
                                                   bool isFolder,
                                                   POINT screenPoint)>;
    void SetContextMenuCallback(ContextMenuCallback callback) { m_onContextMenu = std::move(callback); }

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

    // 右键：取到压在哪个节点上，然后交给 MainWindow 弹菜单并执行
    LRESULT ShowContextMenu(int screenX, int screenY);

    // 枚举所有硬盘
    void AddDrives(HTREEITEM parentItem);

    // 取系统已知文件夹路径（桌面/下载/文档/图片/音乐）
    static bool GetKnownFolderPath(const GUID& folderId, std::wstring& outPath);

    // ==================================================================
    // 同步状态角标（导航栏也打角标）
    //
    // 【为什么和主列表做法不同】
    //   主列表是自绘，拿 HICON 直接画；导航栏是系统 TreeView + ImageList，
    //   只能把"主图标 + 角标"合成成一张新图、换掉节点的图像列表索引。
    //
    // 【为什么要有后台线程】
    //   判定走 COM（首次还要实例化处理器），几十到上百毫秒，
    //   压在 UI 线程会让窗口启动发顿。所以丢到一次性后台线程，
    //   结果回来再 PostMessage 让 UI 线程去换索引。
    // ==================================================================
    void RequestOverlayResolve();   // 收集节点路径 → 起线程判定
    void ApplyOverlayResults();     // UI 线程：把判定结果换成合成图标索引
    void StopOverlayThread();       // 收掉上一轮线程（重建 / 析构前调用）

    // TreeView 子类过程：只为了接"判定结果已就绪"这条自定义消息
    static LRESULT CALLBACK OverlaySubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                LPARAM lParam, UINT_PTR uIdSubclass,
                                                DWORD_PTR dwRefData);

    HWND              m_hWnd      = nullptr;
    HFONT             m_hFont     = nullptr;
    HIMAGELIST        m_hImageList = nullptr;
    shell::IconCache* m_iconCache  = nullptr;

    NavigateCallback  m_onNavigate;

    // 每个树节点对应的数据，lParam 里存的是这个数组的下标
    struct NodeData
    {
        std::wstring path;
        bool         isSeparator  = false;  // 这一行是分隔线占位，不是目录
        bool         isPinned     = false;  // 这一行是固定区里的项（右键可取消固定）
        bool         isHint       = false;  // 这一行是"把文件夹拖到这里"的提示文字
        bool         isFolderLike = true;   // 这一行能不能当目录用（程序类固定项为 false）
        HTREEITEM    item         = nullptr; // 这一行对应的树节点句柄（用来反选中）
        int          baseImageIndex = I_IMAGENONE; // 原始图标索引（合成角标时以它为底，避免叠加）
    };
    std::vector<NodeData> m_nodes;

    // ---- 角标判定的线程与结果 ----
    std::thread         m_overlayThread;         // 一次性判定线程（每轮重建前先收掉）
    std::mutex          m_overlayMutex;          // 保护下面几个字段
    std::vector<int>    m_overlayHits;           // 每个路径命中的处理器下标（-1 未命中）
    std::vector<size_t> m_overlayNodes;          // 与 m_overlayHits 一一对应的 m_nodes 下标
    int                 m_overlayResultGen = 0;  // 结果对应的轮次

    // 上一轮的路径与结果：树重建时若路径没变，直接复用、不再问一次处理器
    std::vector<std::wstring> m_overlayLastPaths;
    std::vector<int>          m_overlayLastHits;

    // UI 线程递增：节点重建后旧结果要作废（防止旧线程的 PostMessage 换了新树）
    int                 m_overlayGeneration = 0;

    // 固定区现在的显示内容（数据真身在 core::Settings 里）
    std::vector<core::PinnedItem> m_pinnedItems;

    ContextMenuCallback m_onContextMenu;

    // 固定区起点（客户区 y 坐标，就是固定区上面那条分隔线的行顶），-1 表示没测到
    int m_pinnedTop = -1;

    // 当前被"拖放高亮"的节点（鼠标正压着的那一行），nullptr 表示没有
    HTREEITEM         m_dropHighlightItem = nullptr;
};
