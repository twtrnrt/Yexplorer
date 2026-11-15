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
#include <vector>
#include <atomic>
#include <functional>

#include "core/AsyncTask.h"
#include "core/FolderWatcher.h"
#include "core/NavigationHistory.h"
#include "shell/ShellFolder.h"
#include "shell/ShellIcon.h"
#include "ui/AddressBar.h"
#include "ui/FileListView.h"
#include "ui/FlatButton.h"
#include "ui/NavigationPane.h"

// 子控件 ID
static const int kControlIdBack    = 1001;
static const int kControlIdForward = 1002;
static const int kControlIdUp      = 1003;
static const int kControlIdAddress = 1004;
static const int kControlIdHome    = 1010;   // 主页（去「我的文档」）
static const int kControlIdRefresh = 1011;   // 刷新
static const int kControlIdThisPc  = 1012;   // 计算机图标（去「此电脑」）

// 快捷键命令 ID（和控件 ID 分开编号，避免撞号）
static const int kCommandCopy      = 3101;   // Ctrl+C
static const int kCommandCut       = 3102;   // Ctrl+X
static const int kCommandPaste     = 3103;   // Ctrl+V
static const int kCommandDelete    = 3104;   // Delete
static const int kCommandRefresh   = 3105;   // F5
static const int kCommandNewFolder = 3106;   // Ctrl+Shift+N
static const int kCommandSelectAll = 3107;   // Ctrl+A
static const int kCommandGoUp      = 3108;   // Backspace

// 右键菜单里的命令
static const int kCommandOpen       = 3201;  // 打开
static const int kCommandProperties = 3202;  // 属性

// 重命名快捷键
static const int kCommandRename     = 3109;  // F2

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

    // 创建工具栏按钮、地址栏、状态栏、字体
    void CreateChildControls();

    // 用户主动导航（会记入历史）
    void NavigateToPath(const std::wstring& path);

    // 后退 / 前进 / 向上
    void GoBack();
    void GoForward();
    void GoUp();

    // 启动后台枚举（只负责枚举与界面刷新，不改历史）
    void StartEnumeration(const std::wstring& path);

    // 重新枚举当前目录（复制/删除/新建之后调用）
    void RefreshCurrentFolder();

    // ---- 文件操作（阶段 4）----
    void DoCopy();          // Ctrl+C：记住选中的路径
    void DoCut();           // Ctrl+X：记住选中的路径，粘贴时移动
    void DoPaste();         // Ctrl+V：复制/移动到家目录
    void DoDelete();        // Delete：删到回收站
    void DoNewFolder();     // Ctrl+Shift+N：新建文件夹

    // ---- 后台执行文件操作（避免复制大文件时界面卡住）----
    void RunFileOperationAsync(std::function<HRESULT()> operation);

    // ---- 拖放：接收别的程序拖进来的文件 ----
    void RegisterDropTargets();      // 列表 + 左栏都注册
    void UnregisterDropTargets();

    void PerformDropFromOutside(const std::vector<std::wstring>& sources,
                                const std::wstring&              targetDirectory,
                                DWORD                            effect,
                                bool                             createShortcut);

    // ---- 右键菜单 ----
    void ShowContextMenu(POINT screenPoint);
    void ShowPropertiesForSelection();

    // ---- 打开选中项 ----
    void OpenSelection();

// 左栏里点了一项：是目录就进去，是程序/快捷方式就启动它
void OpenPinnedOrFolder(const std::wstring& path);

// 把拖进左栏固定区的东西记进配置文件（只记路径，不动文件本身）
void AddPinnedItems(const std::vector<std::wstring>& sources);

    // ---- 排序（点表头）----
    void OnColumnClick(int column);
    void SortEntries();          // 按当前排序状态排一遍数据

    // ---- 重命名 ----
    void DoRename(size_t index, const std::wstring& newName);

    // 取当前结果（加锁拿一份引用，避免和枚举线程抢）
    std::shared_ptr<shell::EnumerateResult> CurrentResult();

    // 更新状态栏和标题里的项目数（局部增删后要刷新这两个地方）
    void UpdateTitle();

    // 后台枚举完成
    void OnEnumerationDone();

    // 双击条目
    void OnItemActivated(size_t index);

    // 重新布局所有子控件
    void LayoutChildren();

    // 当前左栏宽度（还没拖过时用默认值 240）
    int CurrentPaneWidth() const { return m_navWidth > 0 ? m_navWidth : Scale(240); }

    // 鼠标是不是压在「左栏和列表中间那条竖线」上（热区比线本身宽几像素，方便抓住）
    bool IsOnPaneDivider(int clientX) const;

    // 拖动分隔线：把左栏宽度改成鼠标所在的位置
    void UpdatePaneWidth(int clientX);

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
    FlatButton   m_homeButton;         // 主页（点一下去「我的文档」）
    FlatButton   m_refreshButton;      // 刷新
    FlatButton   m_thisPcButton;       // 计算机图标（地址栏左端，点了去「此电脑」）
    AddressBar   m_addressBar;         // 地址栏（分段可点击，比如 C: > Windows）

    HWND         m_hStatusBar     = nullptr;
    HFONT        m_hUiFont        = nullptr;   // 普通控件字体
    HACCEL       m_hAccelerator   = nullptr;   // 键盘快捷键表

    // ---- 内部剪贴板（复制/剪切用）----
    // 说明：这里不写系统剪贴板，只在程序内部记住路径。
    // 好处是实现简单可靠；代价是不能和资源管理器互相复制粘贴（二期可扩展）
    std::vector<std::wstring> m_clipboardPaths;
    bool                      m_clipboardIsCut = false;

    // 粘贴是"剪切"时，操作成功后要清空剪贴板；成功前不动，失败还能重试
    bool                      m_clearClipboardWhenDone = false;

    // ---- 数据 ----
    shell::IconCache   m_iconCache;
    NavigationHistory  m_history;

    // ---- 后台枚举状态 ----
    std::thread                                  m_enumThread;
    core::CancellationToken                      m_enumToken;
    std::mutex                                   m_resultMutex;
    std::shared_ptr<shell::EnumerateResult>      m_result;

    // ---- 当前目录的变化监视（别的程序改了文件时自动刷新，不用按 F5）----
    core::FolderWatcher                          m_watcher;

    // ---- 后台文件操作状态（复制大文件时界面不卡住）----
    std::thread                                  m_operationThread;
    std::shared_ptr<std::atomic<bool>>           m_operationRunning;

    // 这次后台操作是干什么的（决定完成后是整表刷新还是局部增删）
    enum class FileOperationKind { None, Copy, Move, Delete };
    FileOperationKind                            m_lastOperation = FileOperationKind::None;

    // 复制/移动完成后的那次刷新，是否要把"新出现的项"选中（和资源管理器一致）
    bool                                         m_selectNewAfterEnum = false;
    std::vector<std::wstring>                    m_operationPaths;   // 操作涉及的路径

    // ---- 排序状态 ----
    int                                          m_sortColumn    = 0;      // 0 名称 1 日期 2 类型 3 大小
    bool                                         m_sortAscending = true;
    bool                                         m_resultReady = false;
    double                                       m_elapsedMs   = 0.0;
    std::wstring                                 m_enumPath;

    // ---- 当前窗口标题 ----
    // 记一份，只在标题真的变了才去设置：
    // 每次刷新都重设标题会让任务栏按钮重新生成，视觉上窗口会"闪一下"
    std::wstring                                 m_lastTitle;

    // ---- 状态栏最后写进去的文字 ----
    // 同样只在文字真的变了才重设，否则「14 个项目」和「正在读取…」来回切，
    // 状态栏看着一直在闪
    std::wstring                                 m_lastStatusText;

    // ---- 左栏宽度（拖中间那条分隔线时改变）----
    // 本轮只在这次运行里记住；关机重开又回默认 240，持久化留到阶段 10 统一做
    int  m_navWidth         = 0;       // 0 表示还没拖过，用默认宽度
    bool m_draggingSplitter = false;   // 是不是正在拖那条线
    bool m_paneVisible      = false;   // 左栏当前是否显示（窗口太窄时会收起来）

    // 两条分隔线的位置：LayoutChildren 里算好，WM_PAINT 直接照着画
    RECT m_addressDividerRect = {};    // 地址栏下面那条横线
    RECT m_paneDividerRect    = {};    // 左栏右边那条竖线
};
