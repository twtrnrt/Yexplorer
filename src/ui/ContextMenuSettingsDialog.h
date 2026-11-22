// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuSettingsDialog.h
//
// 「配置右键菜单」窗口（一期：纯查看，只读展示采集结果）。
//
// 界面：
//   ┌──────────────────────────────────────────────────────────┐
//   │ 最近采集：2026-11-22 10:00:00            [刷新]          │
//   ├──────────────────────────────────────────────────────────┤
//   │  ▼ 7-Zip                                                  │
//   │      ├ 添加到压缩包…            [文件/文件夹]             │
//   │      ├ 解压到当前文件夹…        [文件/文件夹]             │
//   │  ▼ WPS                                                    │
//   │      ├ 上传到 WPS              [文件/背景]                │
//   └──────────────────────────────────────────────────────────┘
//
// 一期边界（已拍板）：
//   · 只展示：不勾选、不隐藏、不排序、不改名字、不改位置；
//   · 不执行任何菜单命令、不写注册表；
//   · 打开窗口先读本地缓存；点「刷新」才重新跑采集。
//
// 二期再接：勾选/隐藏/排序/位置配置、命令执行、图标、多选场景。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>

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

    // 显示模态窗口（一期无「确定/取消」写回概念，关闭即结束）。
    void ShowModal();

private:
    LRESULT HandleMessage(HWND, UINT msg, WPARAM wParam, LPARAM lParam) override;

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void CreateControls();
    void Layout();

    // 用 m_report 重建 TreeView
    void ReloadTree();

    // 重新枚举 + 采集 + 存缓存，然后刷新界面
    void Refresh();

    HWND  m_hOwner   = nullptr;
    HWND  m_hTree    = nullptr;
    HWND  m_hRefresh = nullptr;
    HWND  m_hStatus  = nullptr;
    HFONT m_font     = nullptr;

    shell::ContextMenuReport m_report;
};

} // namespace ui
