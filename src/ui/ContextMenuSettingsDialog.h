// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuSettingsDialog.h
//
// 「配置右键菜单」窗口（二期：左侧系统菜单树 + 右侧我们的菜单 + 拖拽配置）。
//
// 界面：
//   ┌──────────────────────────────────────────────────────────────────────┐
//   │ 最近普查：…                                    [刷新系统菜单][一键补图标][保存] │
//   ├───────────────────────────┬──────────────────────────────────────────┤
//   │ 系统右键菜单（只读）      │ 我们的右键菜单（可编辑）                 │
//   │  WPS                      │  [新建组][删除][上移][下移]              │
//   │   单选文件                │  ▾ WPS                                   │
//   │     WPS · .docx .xlsx     │     ▾ WPS AI                              │
//   │       WPS AI              │         打开…                              │
//   │         打开…             │                                          │
//   │   多选文件                │  选中项：名称[____] 图标[改][清]          │
//   │  …                        │  生效范围：[文件][文件夹][空白区][多选]   │
//   │                           │  扩展名：[.docx .xlsx]                    │
//   │                           │  ─── 预览 ───                            │
//   │                           │  （我们菜单弹出来长什么样）               │
//   └───────────────────────────┴──────────────────────────────────────────┘
//
// 要点：
//   · 左侧树：一级=软件；二级=场景；三级（仅单选文件）=按扩展名分簇；叶子=菜单项；
//   · 拖拽：叶子 / 簇 / 场景 / 软件 四种粒度都能拖；拖到组或条目上即成为它的子项；
//   · 生效范围默认继承拖拽节点代表的范围，用户可在属性区改；
//   · 只写 menu-config.json，不碰注册表、不接菜单弹出（那属于下一阶段）。
// ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "shell/ContextMenuData.h"
#include "shell/MenuCensus.h"
#include "shell/MenuConfig.h"
#include "ui/CustomWindow.h"

namespace ui
{

class ContextMenuSettingsDialog : public CustomWindow
{
public:
    explicit ContextMenuSettingsDialog(HWND owner);
    ~ContextMenuSettingsDialog();

    ContextMenuSettingsDialog(const ContextMenuSettingsDialog&) = delete;
    ContextMenuSettingsDialog& operator=(const ContextMenuSettingsDialog&) = delete;

    void ShowModal();

private:
    // ------------------------------------------------------------------
    // 左侧系统树节点
    // ------------------------------------------------------------------
    struct SysNode
    {
        enum Kind { Software, Scene, Cluster, Group, Item };

        Kind         kind = Software;
        std::wstring brand;                    // 软件品牌（四种节点都带）
        unsigned int scene = 0;                // Scene 节点：场景位
        int          clusterIndex = -1;        // Cluster 节点：m_report.clusters 下标
        int          clusterItemIndex = -1;    // Item 节点：簇内条目下标
        int          groupIndex = -1;          // Group 节点：m_report.fileGroups 下标
        int          sceneItemIndex = -1;      // Item 节点（非文件场景）：场景条目下标
        unsigned int itemScene = 0;            // Item 节点：所属场景位
        // 「系统」品牌的合并菜单节点（不分簇）；非空时用这个而不是簇下标
        const shell::CensusSystemNode* systemNode = nullptr;
        std::wstring systemPath;              // 系统菜单节点的祖先文字路径（造身份键用）
    };

    // 右侧配置树节点
    struct CfgNode
    {
        enum Kind { Group, Entry };

        Kind         kind = Group;
        std::wstring id;    // 组 ID 或条目 ID
    };

    // 预览行
    struct PreviewRow
    {
        std::wstring text;
        int          level = 0;
        bool         isGroup = false;
        std::wstring iconFile;
    };

    // 拖拽展开出来的一条（扁平化后的菜单项）
    struct DropItem
    {
        std::wstring text;
        std::wstring pathKey;
        std::wstring brand;
        std::wstring verb;
        std::wstring iconFile;
        int          level = 0;              // 相对层级（0=拖拽节点的直接子项）
        unsigned int scenes = 0;             // 继承的生效场景
        std::vector<std::wstring> exts;      // 继承的扩展名
    };

    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void CreateControls();
    void Layout();

    // ---- 左侧系统树 ----
    void RebuildSysTree();
    int  IconIndexFor(const std::wstring& iconFile);
    void ClearIconList();

    // ---- 右侧配置树 ----
    void ReloadConfig();
    void RebuildCfgTree();
    void RebuildPreview();
    void ReleasePreviewBitmaps();
    void LoadSelectionProperties();
    void ApplyNameFromEdit();
    void ApplyExtsFromEdit();
    void ApplyScopeFromChecks();

    // ---- 拖拽 ----
    void BeginDrag(HTREEITEM sysItem);
    void UpdateDragHighlight(POINT screenPt);
    void FinishDrag(POINT screenPt);
    void BuildDropItems(int sysNodeIndex, std::vector<DropItem>& out) const;
    void InsertDropItems(const std::vector<DropItem>& items,
                         const std::wstring& groupId,
                         const std::wstring& parentEntryId);

    // ---- 普查 ----
    void StartCensus();
    void OnCensusDone(shell::CensusReport* report);
    void SaveConfig();

    // ---- 工具 ----
    HTREEITEM AddTreeItem(HWND tree, HTREEITEM parent, const std::wstring& text,
                          const std::wstring& iconFile, LPARAM data);
    int  SelectedSysNodeIndex() const;
    bool SelectedCfgNode(CfgNode& out) const;
    void SelectCfgNodeById(const std::wstring& id);
    HWND HitTestCfgTree(POINT screenPt, int* cfgIndex) const;

    // ---- 控件 ----
    HWND m_hOwner     = nullptr;
    HWND m_hStatus    = nullptr;
    HWND m_hRefresh   = nullptr;
    HWND m_hSupplement = nullptr;
    HWND m_hSave      = nullptr;
    HWND m_hSysTree   = nullptr;
    HWND m_hCfgTree   = nullptr;
    HWND m_hPreview   = nullptr;
    HWND m_hNameEdit  = nullptr;
    HWND m_hIconBtn   = nullptr;
    HWND m_hIconClear = nullptr;
    HWND m_hNewGroup  = nullptr;
    HWND m_hDelete    = nullptr;
    HWND m_hMoveUp    = nullptr;
    HWND m_hMoveDown  = nullptr;
    HWND m_hCheckFile = nullptr;
    HWND m_hCheckDir  = nullptr;
    HWND m_hCheckBg   = nullptr;
    HWND m_hCheckMulti = nullptr;
    HWND m_hExtsEdit  = nullptr;
    HWND m_labelSys   = nullptr;   // 「系统右键菜单（只读）」
    HWND m_labelCfg   = nullptr;   // 「我们的右键菜单（拖左侧项目进来）」
    HWND m_labelProps = nullptr;   // 「选中项设置」
    HWND m_labelPreview = nullptr; // 「预览：我们的右键菜单弹出效果」
    HFONT m_font      = nullptr;

    // ---- 数据 ----
    shell::CensusReport m_report;
    bool                m_hasReport = false;
    shell::MenuConfig   m_config;

    std::vector<SysNode> m_sysNodes;
    std::vector<CfgNode> m_cfgNodes;

    HIMAGELIST               m_icons = nullptr;
    std::vector<std::wstring> m_iconFiles;   // 图标文件名（已加入 m_icons）

    std::vector<PreviewRow> m_previewRows;
    std::vector<HBITMAP>    m_previewBitmaps;

    // ---- 拖拽状态 ----
    bool m_dragging = false;
    POINT m_dragStart = { 0, 0 };

    // ---- 普查线程 ----
    std::thread       m_censusThread;
    std::atomic<bool> m_censusRunning{ false };
    std::atomic<bool> m_censusCancel{ false };

    // ---- 防止"属性区回填"时触发模型写回 ----
    bool m_fillingProperties = false;
    std::wstring m_extsSummary;   // 属性区里扩展名的显示摘要（原样未改就不重写模型）
};

} // namespace ui
