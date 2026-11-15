// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/OpenWithDialog.h
//
// 「打开方式」设置窗口（模态）。
//
// 界面长这样：
//
//   ┌──────────────────────────────────────────────────────────┐
//   │ 选择用哪个程序打开 .txt 文件                              │
//   │ 勾选左侧 = 出现在右键「打开方式」菜单；点右侧圆点 = 设为默认 │
//   ├──────────────────────────────────────────────────────────┤
//   │ ☑ [图标] 记事本                      系统未列出      ●     │
//   │ ☑ [图标] 豆包                                        ○     │
//   │ ☐ [图标] WPS                                         ○     │
//   ├──────────────────────────────────────────────────────────┤
//   │        [添加程序…] [删除] [刷新]        [跳过] [关闭]      │
//   └──────────────────────────────────────────────────────────┘
//
// 【列表什么时候更新】
//   只在你打开这个窗口的那一刻（和点「刷新」时）去系统读一次候选。
//   系统里新冒出来的程序默认是勾上的，不满意就取消（取消状态会永久保存）。
//   平时右键菜单不会自己去读系统，所以装什么软件都改不了你的菜单。
//
// 【即改即存】
//   勾选、默认程序改动都记在工作副本里，窗口关闭时一次性写进配置文件。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/Settings.h"
#include "shell/ShellIcon.h"

namespace ui
{

class OpenWithDialog
{
public:
    // icons 用来取程序图标（传主窗口那份缓存即可）
    OpenWithDialog(HWND owner, shell::IconCache* icons);
    ~OpenWithDialog();

    OpenWithDialog(const OpenWithDialog&) = delete;
    OpenWithDialog& operator=(const OpenWithDialog&) = delete;

    // 显示窗口。
    //   extension：".txt" 这种（无扩展名的传 "(无扩展名)"）
    //   firstTime：是否"第一次用这个扩展名"。是的话多一个「跳过」按钮，
    //              并且会把系统当前默认程序预先标成默认，方便一键确认。
    // 返回 true＝关闭时这个扩展名已经有了默认程序；false＝还没有（调用方按系统默认打开）。
    bool ShowModal(const std::wstring& extension, bool firstTime);

    // 用户这次是不是点了「跳过」（首次询问里"这次先用系统默认打开"）
    bool Skipped() const { return m_skipped; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void CreateControls();
    void Layout();
    void Draw(HDC dc);
    void DrawFlatButton(HDC dc, int index);

    // 读系统候选并合并进工作副本
    //   keepEnabled：库里已有的条目保留原勾选状态（这是默认行为）
    void RefreshCandidatesFromSystem(bool alsoProbe);

    // 工作副本 → ListView
    void ReloadList();

    // 更新某行的勾选状态（不回写，避免和用户点击互相触发）
    void SyncCheckState(int index, bool checked);

    // 探测所有条目的存在性，更新 alive
    void ProbeAll();

    void OnCheckChanged(int index, bool checked);
    void SetDefaultIndex(int index);
    int  DefaultIndex() const;
    void OnAddProgram();
    void OnRemoveSelected();
    void OnButtonClick(int buttonIndex);

    int  Scale(int value) const;

    // 按钮序号（自绘按钮，按下标分派）
    enum Button
    {
        ButtonAdd = 0,
        ButtonRemove,
        ButtonRefresh,
        ButtonSkip,
        ButtonClose,
        ButtonCount
    };

private:
    HWND m_hWnd    = nullptr;
    HWND m_hOwner  = nullptr;

    HWND m_hList   = nullptr;   // 程序列表
    HWND m_hTitle  = nullptr;   // 标题文字（静态控件只用来占位，实际自绘）

    shell::IconCache* m_icons = nullptr;

    std::wstring        m_extension;
    bool                m_firstTime = false;
    bool                m_skipped   = false;

    core::OpenWithEntry m_entry;              // 工作副本（关闭时写回配置）

    // 这一条是不是这次从系统候选里新读到的（只是显示用，不入配置文件）
    std::vector<bool>   m_inSystemCandidates;

    // 按钮区域（Layout 里算好，WM_PAINT / 鼠标消息都照着用）
    RECT m_buttonRects[ButtonCount] = {};
    int  m_buttonCount = 0;
    int  m_hotButton   = -1;     // 鼠标悬停在哪个按钮
    int  m_pressedButton = -1;   // 正被按下的按钮

    HFONT m_font = nullptr;

    // 我们自己程序性设置勾选时置真，避免 LVN_ITEMCHANGED 当成用户操作回写
    bool m_syncingChecks = false;

    RECT m_listRect = {};
};

} // namespace ui
