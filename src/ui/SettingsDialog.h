// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/SettingsDialog.h
//
// 「设置」窗口（地址栏右边那个齿轮按钮弹出来的就是这个）。
//
// 【现在只有一个开关】
//   「右键菜单显示图标」：勾上以后，右键「打开方式」和「新建」里的每一行前面
//   显示程序/文件类型的小图标；不勾就是纯文字的菜单（默认，最快）。
//
// 【风格】
//   和「打开方式」设置窗口一套：无边框客户区、浅灰底、右下角一个自绘关闭按钮。
//   控件只有复选框是系统原生的，其余自己画。
//
// 【保存】
//   勾选/取消勾选立刻写进 settings.json，点关闭就行，没有"确定/取消"。
// ============================================================================

#pragma once

#include <windows.h>

class SettingsDialog
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
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 窗口类只需注册一次
    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void Layout();

    // 整窗重画：底色 + 标题 + 说明字 + 关闭按钮
    void Draw(HDC dc);
    void DrawCloseButton(HDC dc);

    int Scale(int value) const;

    // 复选框状态 → 写配置
    void ApplyCheckState();

private:
    HWND  m_hWnd   = nullptr;
    HWND  m_hOwner = nullptr;
    HWND  m_hCheck = nullptr;   // 系统复选框

    HFONT m_font      = nullptr;   // 正文
    HFONT m_fontTitle = nullptr;   // 标题（比正文大一号、略粗）

    RECT  m_closeRect = {};        // 「关闭」按钮
    bool  m_hotClose  = false;     // 鼠标在关闭按钮上
    bool  m_pressedClose = false;  // 关闭按钮按下中

    bool  m_trackingLeave = false; // 是否已注册"鼠标离开"通知
};
