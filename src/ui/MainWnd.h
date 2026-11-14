// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.h
//
// 主窗口（阶段 3 版本）。
//
// 界面结构（对齐资源管理器）：
//
//   ┌────────────────────────────────────────────────────┐
//   │ [←] [→] [↑]  [ 地址栏：当前路径              ]      │  导航栏
//   ├────────────────────────────────────────────────────┤
//   │                                                    │
//   │  文件列表（图标 + 名称 / 修改日期 / 类型 / 大小）    │
//   │                                                    │
//   ├────────────────────────────────────────────────────┤
//   │ 14 个项目                                          │  状态栏
//   └────────────────────────────────────────────────────┘
//
 // ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/AsyncTask.h"
#include "core/NavigationHistory.h"
#include "shell/ShellFolder.h"
#include "shell/ShellIcon.h"
#include "ui/FileListView.h"
#include "ui/FlatButton.h"
#include "ui/NavigationPane.h"

// 子控件 ID
static const int kControlIdBack    = 1001;
static const int kControlIdForward = 1002;
static const int kControlIdUp      = 1003;
static const int kControlIdAddress = 1004;

class MainWindow
{
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE hInstance);
    void Show(int nCmdShow);
    int  RunMessageLoop();

    HWND Handle() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // 地址栏子类化过程：捕获回车键触发跳转
    static LRESULT CALLBACK AddressEditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                    LPARAM lParam, UINT_PTR idSubclass,
                                                    DWORD_PTR refData);

    // 创建工具栏按钮、地址栏、状态栏、字体
    void CreateChildControls();

    // 用户主动导航（会记入历史）
    void NavigateToPath(const std::wstring& path);

    // 地址栏回车
    void NavigateFromAddressBar();

    // 后退 / 前进 / 向上
    void GoBack();
    void GoForward();
    void GoUp();

    // 启动后台枚举（只负责枚举与界面刷新，不改历史）
    void StartEnumeration(const std::wstring& path);

    // 后台枚举完成
    void OnEnumerationDone();

    // 双击条目
    void OnItemActivated(size_t index);

    // 重新布局所有子控件
    void LayoutChildren();

    // 按 DPI 缩放
    int Scale(int value) const;

    // 刷新导航按钮可用状态、状态栏文本
    void UpdateNavigationState();
    void UpdateStatusBar();

private:
    HWND      m_hWnd      = nullptr;
    HINSTANCE m_hInstance = nullptr;

    // ---- 子控件 ----
    FileListView m_fileListView;
    NavigationPane m_navigationPane;   // 左侧导航栏（快速访问 / 各硬盘）
    FlatButton   m_backButton;         // 后退（自绘扁平按钮）
    FlatButton   m_forwardButton;      // 前进
    FlatButton   m_upButton;           // 向上

    HWND         m_hAddressEdit   = nullptr;
    HWND         m_hStatusBar     = nullptr;
    HFONT        m_hUiFont        = nullptr;   // 普通控件字体

    // ---- 数据 ----
    shell::IconCache   m_iconCache;
    NavigationHistory  m_history;

    // ---- 后台枚举状态 ----
    std::thread                                  m_enumThread;
    core::CancellationToken                      m_enumToken;
    std::mutex                                   m_resultMutex;
    std::shared_ptr<shell::EnumerateResult>      m_result;
    bool                                         m_resultReady = false;
    double                                       m_elapsedMs   = 0.0;
    std::wstring                                 m_enumPath;
};
