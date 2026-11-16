// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/SettingsDialog.h
//
// 「设置」窗口（地址栏右边那个齿轮按钮弹出来的就是这个）。
//
// 【现在有两块】
//   1. 「右键菜单显示图标」：勾上以后，右键「打开方式」和「新建」里的每一行前面
//      显示程序/文件类型的小图标；不勾就是纯文字的菜单（默认，最快）。
//   2. 「注册为默认文件管理器」按钮：管的是"双击文件夹/磁盘时用谁打开"。
//      注册后就变成「恢复系统默认文件管理器」，一键切回去。
//      （具体改了哪两个注册表键、为什么能一键恢复，见 shell/DefaultManager.h）
//
// 【风格】
//   和「打开方式」设置窗口一套：无边框客户区、浅灰底、右下角一个自绘关闭按钮。
//   控件只有复选框是系统原生的，其余自己画。
//
// 【保存】
//   勾选/取消勾选立刻写进 settings.json；按按钮立刻执行，点关闭就行，
//   没有"确定/取消"。
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

    // 整窗重画：底色 + 标题 + 说明字 + 默认文件管理器按钮 + 关闭按钮
    void Draw(HDC dc);

    // 自绘按钮（关闭和"注册/恢复"共用这一套外观）
    void DrawButton(HDC dc, const RECT& rect, const wchar_t* text,
                    bool hot, bool pressed);

    int Scale(int value) const;

    // 复选框状态 → 写配置
    void ApplyCheckState();

    // 按「注册为默认文件管理器 / 恢复系统默认文件管理器」
    void ToggleDefaultManager();

private:
    HWND  m_hWnd   = nullptr;
    HWND  m_hOwner = nullptr;
    HWND  m_hCheck = nullptr;        // 系统复选框：右键菜单显示图标
    HWND  m_hCheckHidden = nullptr;  // 系统复选框：显示隐藏的文件和文件夹
    HWND  m_hCheckSystem = nullptr;  // 系统复选框：显示受保护的操作系统文件

    HFONT m_font      = nullptr;   // 正文
    HFONT m_fontTitle = nullptr;   // 标题（比正文大一号、略粗）

    RECT  m_closeRect  = {};       // 「关闭」按钮
    bool  m_hotClose   = false;    // 鼠标在关闭按钮上
    bool  m_pressedClose = false;  // 关闭按钮按下中

    RECT  m_actionRect = {};       // 「注册 / 恢复默认文件管理器」按钮
    bool  m_hotAction  = false;
    bool  m_pressedAction = false;

    // 当前是不是已经注册成默认文件管理器（窗口打开时问一次，点按钮后再更新）
    bool  m_defaultManagerRegistered = false;
    bool  m_lastActionFailed         = false;   // 上一次点按钮失败了

    bool  m_trackingLeave = false; // 是否已注册"鼠标离开"通知
};
