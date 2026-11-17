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
    //   filePath：本次是从哪个文件触发打开这个窗口的（点「打开」时用它启动）；
    //             从右键「添加或管理程序」进来时传空串，此时「打开」按钮不显示。
    //   preferSelf：候选列表里要不要加一条「Yexplorer（本程序）」，并把它设为默认。
    //               压缩包（zip/rar…）双击时用 true —— 第一次打开也要弹这个窗口，
    //               默认就用我们自己的文件管理器进去浏览。
    // 返回 true＝关闭时这个扩展名已经有了默认程序。
    bool ShowModal(const std::wstring& extension,
                   bool firstTime,
                   const std::wstring& filePath = L"",
                   bool preferSelf = false);

    // 用户是不是点了「打开」（要立刻用默认程序打开 filePath）
    bool OpenRequested() const { return m_openRequested; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void CreateControls();
    void Layout();
    void Draw(HDC dc);
    void DrawRoundFrame(HDC dc);
    void DrawFlatButton(HDC dc, int index);    void DrawRow(HDC dc, const RECT& rowRect, int index, bool selected);
    void DrawCheckBox(HDC dc, const RECT& box, bool checked, bool dimmed);
    void DrawDefaultDot(HDC dc, const RECT& cell, bool isDefault, bool selected);

    // 工作副本 → 配置文件（改动即存）
    void SaveNow();

    // 读系统候选并合并进工作副本
    //   keepEnabled：库里已有的条目保留原勾选状态（这是默认行为）
    void RefreshCandidatesFromSystem(bool alsoProbe);

    // 工作副本 → ListView
    void ReloadList();

    // 探测所有条目的存在性，更新 alive
    void ProbeAll();

    void SetDefaultIndex(int index);
    int  DefaultIndex() const;
    void OnAddProgram();
    void OnRemoveSelected();
    void OnButtonClick(int buttonIndex);

    int  Scale(int value) const;

    // 按钮序号（自绘按钮，按下标分派）
    //   左边一组管"列表内容"：添加程序 / 删除 / 刷新
    //   右边一组管"这次打开"：打开 / 关闭
    enum Button
    {
        ButtonAdd = 0,
        ButtonRemove,
        ButtonRefresh,
        ButtonOpen,
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
    std::wstring        m_filePath;        // 本次要打开的文件（空 = 管理入口）
    bool                m_firstTime = false;
    bool                m_openRequested = false;   // 用户点了「打开」

    // 候选列表里是否要加「Yexplorer（本程序）」并默认选中它
    // （压缩包双击时传 true：没这选项的话用户没地方"用本程序打开压缩包"）
    bool                m_preferSelf = false;

    core::OpenWithEntry m_entry;              // 工作副本（关闭时写回配置）

    // 这一条是不是这次从系统候选里新读到的（只是显示用，不入配置文件）
    std::vector<bool>   m_inSystemCandidates;

    // 每一行的 24px 图标（ReloadList 时从 32px 那一档缩出来，自绘行时直接用）
    std::vector<HICON>  m_rowIcons;

    // 32px 图标列表句柄（自绘行时用它取原始图标）
    HIMAGELIST          m_iconList = nullptr;

    // 把 m_rowIcons 里的图标句柄全部销毁（重载列表 / 窗口销毁时用）
    void ReleaseRowIcons();

    // 按钮区域（Layout 里算好，WM_PAINT / 鼠标消息都照着用）
    RECT m_buttonRects[ButtonCount] = {};
    int  m_buttonCount = 0;
    int  m_hotButton   = -1;     // 鼠标悬停在哪个按钮
    int  m_pressedButton = -1;   // 正被按下的按钮

    HFONT m_font = nullptr;

    // 我们自己程序性设置勾选时置真，避免 LVN_ITEMCHANGED 当成用户操作回写
    bool m_syncingChecks = false;

    RECT m_listRect = {};
    RECT m_frameRect = {};        // 列表外面那圈圆角框（Layout 里算好）
    RECT m_defaultLabelRect = {}; // 列表上方「默认程序」四个字（对齐最右那列）
};

} // namespace ui
