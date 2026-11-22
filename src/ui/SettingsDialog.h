// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/SettingsDialog.h
//
// 「设置」窗口（地址栏右边那个齿轮按钮弹出来的就是这个）。
//
// 【四块】
//   1. 查看选项：右键菜单图标、显示隐藏文件、显示系统文件。
//   2. 启动位置：此电脑 / 主文件夹 / 自定义文件夹 / 最后一次打开的文件夹。
//   3. 快速启动：开机后台常驻、接管 Win+E（勾 Win+E 自动勾后台）。
//   4. 注册与集成：默认文件管理器、System32 代理、Listary 插件、Shell 窗口。
//
// 【风格】
//   和「打开方式」设置窗口一套：无边框客户区、浅灰底、右下角一个自绘关闭按钮。
//   控件只有复选框/单选是系统原生的，其余自己画。
//
// 【保存】
//   改动立即写 settings.json；注册类/快速启动类操作立刻执行，点关闭就行。
// ============================================================================

#pragma once

#include <windows.h>

#include "ui/CustomWindow.h"

class SettingsDialog : public CustomWindow
{
public:
    SettingsDialog() = default;
    ~SettingsDialog();

    SettingsDialog(const SettingsDialog&) = delete;
    SettingsDialog& operator=(const SettingsDialog&) = delete;

    bool ShowModal(HWND owner);

    // 角标配置弹窗点过确定（套范围 / 顺序有改动），主窗口关闭设置后据此刷新当前目录
    bool OverlayConfigDirty() const { return m_overlayConfigDirty; }

private:
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void Layout();

    void Draw(HDC dc);
    void DrawButton(HDC dc, const RECT& rect, const wchar_t* text,
                    bool hot, bool pressed, bool enabled = true);

    // 查看类复选框 → 写配置
    void ApplyCheckState();

    // 启动位置单选
    void ApplyStartupLocation();
    void PickCustomStartupFolder();
    void RefreshStartupControls();

    // 快速启动两个复选框
    void OnQuickStartToggle(int controlId);
    void RefreshQuickStart();

    // 窗口模式
    void RefreshWindowModeChecks();

    // 注册类复选框
    void OnRegistrationToggle(int controlId);
    void RefreshRegistrationChecks();

    // 同步状态角标：总开关联动配置按钮置灰；点「配置…」打开角标配置窗口
    void RefreshOverlayControls();
    void OnOverlayConfigButton();

    // 右键菜单：点「配置右键菜单…」打开菜单采集结果查看窗口（一期纯展示）
    void OnCtxMenuConfigButton();

    // 「恢复/重置」按钮：弹选择框（仅恢复设置 / 清除所有痕迹），确认后执行
    void OnRestoreButton();

    // 模式一：撤销对系统的全部集成改动（自启、默认关联、代理、Listary、
    // 后台宿主、Win+E、Shell 窗口），保留个人数据。返回提权部分是否成功。
    bool RestoreSystemIntegration();

    // 模式二：在模式一基础上删除全部个人痕迹，然后退出程序。
    void PurgeAllTracesAndExit();

    static bool IsChecked(HWND check);
    static void SetChecked(HWND check, bool checked);

private:
    HWND  m_hOwner = nullptr;
    HWND  m_hCheck = nullptr;
    HWND  m_hCheckHidden = nullptr;
    HWND  m_hCheckSystem = nullptr;

    // 启动位置（下拉框）
    HWND  m_hStartupCombo = nullptr;

    // 快速启动
    HWND  m_hCheckBackground = nullptr;
    HWND  m_hCheckWinE = nullptr;

    // 窗口模式
    HWND  m_hCheckTabs = nullptr;
    HWND  m_hCheckSingleInstance = nullptr;

    // 注册与集成
    HWND  m_hCheckDefaultManager = nullptr;
    HWND  m_hCheckProxy = nullptr;
    HWND  m_hCheckListary = nullptr;
    HWND  m_hCheckShellWindow = nullptr;

    // 同步状态角标（Overlay）：总开关复选框 + 自绘「配置…」按钮
    HWND  m_hCheckOverlay = nullptr;
    RECT  m_overlayConfigRect = {};
    bool  m_hotOverlayConfig = false;
    bool  m_pressedOverlayConfig = false;
    bool  m_overlayConfigDirty = false;

    // 右键菜单配置：自绘「配置右键菜单…」按钮（一期纯查看，无总开关联动）
    RECT  m_ctxMenuRect = {};
    bool  m_hotCtxMenu = false;
    bool  m_pressedCtxMenu = false;

    HFONT m_font        = nullptr;
    HFONT m_fontTitle   = nullptr;
    HFONT m_fontSection = nullptr;

    RECT  m_closeRect  = {};
    bool  m_hotClose   = false;
    bool  m_pressedClose = false;

    // 「恢复/重置」按钮（自绘，位于关闭按钮左侧）
    RECT  m_restoreRect = {};
    bool  m_hotRestore  = false;
    bool  m_pressedRestore = false;

    bool  m_lastActionFailed = false;

    bool  m_trackingLeave = false;
};
