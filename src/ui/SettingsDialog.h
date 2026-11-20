// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/SettingsDialog.h
//
// 「设置」窗口（地址栏右边那个齿轮按钮弹出来的就是这个）。
//
// 【现在有两块】
//   1. 查看选项：右键菜单图标、显示隐藏文件、显示系统文件。
//   2. 注册与集成：4 个独立复选框，勾哪个就只做哪一项——
//        · 注册为默认文件管理器（文件夹/磁盘关联）
//        · 部署 System32\explorer.exe 代理
//        · 安装 Listary 快速切换插件
//        · 允许其它程序定位到我打开的文件（Shell 窗口）
//      方便逐项测试，互不牵连。
//
// 【风格】
//   和「打开方式」设置窗口一套：无边框客户区、浅灰底、右下角一个自绘关闭按钮。
//   控件只有复选框是系统原生的，其余自己画。
//
// 【保存】
//   勾选/取消勾选立刻写进 settings.json；注册类操作立刻执行，点关闭就行，
//   没有"确定/取消"。
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

    // 以模态方式打开（owner 会被临时禁用，关掉窗口后恢复）。
    // 返回 true 表示窗口正常显示过。
    bool ShowModal(HWND owner);

private:
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    // 窗口类只需注册一次
    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void Layout();

    // 整窗重画：底色 + 标题 + 说明字 + 复选框旁的小字 + 关闭按钮
    void Draw(HDC dc);

    // 自绘按钮（只有关闭按钮）
    void DrawButton(HDC dc, const RECT& rect, const wchar_t* text,
                    bool hot, bool pressed);

    // 查看类复选框状态 → 写配置
    void ApplyCheckState();

    // 注册类复选框：勾/取消时执行对应动作
    void OnRegistrationToggle(int controlId);

    // 按实际状态刷新 4 个注册类复选框
    void RefreshRegistrationChecks();

    static bool IsChecked(HWND check);
    static void SetChecked(HWND check, bool checked);

private:
    HWND  m_hOwner = nullptr;
    HWND  m_hCheck = nullptr;        // 右键菜单显示图标
    HWND  m_hCheckHidden = nullptr;  // 显示隐藏的文件和文件夹
    HWND  m_hCheckSystem = nullptr;  // 显示受保护的操作系统文件

    HWND  m_hCheckDefaultManager = nullptr;  // 注册为默认文件管理器
    HWND  m_hCheckProxy = nullptr;           // 部署 System32\explorer.exe 代理
    HWND  m_hCheckListary = nullptr;         // 安装 Listary 插件
    HWND  m_hCheckShellWindow = nullptr;     // Shell 窗口集成

    HFONT m_font      = nullptr;   // 正文
    HFONT m_fontTitle = nullptr;   // 标题（比正文大一号、略粗）

    RECT  m_closeRect  = {};       // 「关闭」按钮
    bool  m_hotClose   = false;    // 鼠标在关闭按钮上
    bool  m_pressedClose = false;  // 关闭按钮按下中

    bool  m_lastActionFailed = false;   // 上一次注册类操作失败了

    bool  m_trackingLeave = false; // 是否已注册"鼠标离开"通知
};
