// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuSettingsDialog.h
//
// 「配置右键菜单」窗口（一期：纯查看，按系统右键菜单原样展示）。
//
// 界面：
//   ┌───────────────────────────────────────────────────────────┐
//   │ 最近采集：2026-11-23 09:00                      [刷新]     │
//   ├───────────────────────────────────────────────────────────┤
//   │ [文件] [文件夹] [文件夹空白区] [多选文件]                  │
//   │ 样本：[文件 · sample.txt ▾]                                │
//   ├───────────────────────────────────────────────────────────┤
//   │  🗜 添加到压缩文件...                                      │
//   │  🗜 解压到当前文件夹                                        │
//   │  ─────────────────                                        │
//   │  📦 7-Zip                                                  │
//   └───────────────────────────────────────────────────────────┘
//
// 要点：
//   · 每个选项卡对应一种场景，条目顺序 / 层级 / 分隔线 / 灰态都照系统原样；
//   · 每一行是采集时由外壳扩展自己画出来的位图（图标 + 文字都在图里）；
//   · 纯查看：不执行命令、不写注册表、不做启用/隐藏配置。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "shell/ContextMenuData.h"
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
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void CreateControls();
    void Layout();

    // 当前选项卡编号 → 报告里的场景下标；没有对应场景返回 -1
    int SceneIndexForTab(int tab) const;

    // 重建列表（含按需加载渲染图）
    void ReloadList();
    void ReleaseBitmaps();

    void DrawRow(HDC dc, const RECT& rowRect, int index);
    void DrawSeparatorRow(HDC dc, const RECT& rowRect);

    // 重新采集 + 落缓存 + 刷新
    void Refresh();

    HWND m_hOwner   = nullptr;
    HWND m_hTab     = nullptr;
    HWND m_hCombo   = nullptr;   // 文件选项卡的样本下拉
    HWND m_hList    = nullptr;
    HWND m_hRefresh = nullptr;
    HWND m_hStatus  = nullptr;
    HFONT m_font    = nullptr;

    shell::ContextMenuReport m_report;

    // 列表当前内容（与 ListView 行一一对应）
    std::vector<const shell::ContextMenuItem*> m_rows;
    std::vector<int>     m_rowLevels;
    std::vector<HBITMAP> m_rowBitmaps;   // 与 m_rows 平行；null = 没有渲染图

    int m_sceneIndex = -1;   // 当前展示的场景在 m_report.scenes 里的下标
};

} // namespace ui
