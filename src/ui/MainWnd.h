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
#include <map>

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

// 右键菜单「查看」子菜单（和资源管理器一样，几种视图里挑一个）
static const int kCommandViewLarge   = 3301;  // 大图标
static const int kCommandViewSmall   = 3302;  // 小图标
static const int kCommandViewList    = 3303;  // 列表
static const int kCommandViewDetails = 3304;  // 详细信息
static const int kCommandViewTiles   = 3305;  // 平铺（此电脑那种带容量进度条的）

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

    // ----------------------------------------------------------------
    // 导航来源（决定"地址栏上写什么名字"和"向上按钮去哪儿"）
    //
    // 从「主文件夹」双击「文档」进去以后：
    //   地址栏显示「文档」，而不是 C:\Users\kf168\Documents；
    //   向上按钮回「主文件夹」，而不是它的真实父目录 C:\Users\kf168。
    // 这就是系统资源管理器的表现——它记住的是"从哪儿点进来的"。
    // 所以我们给每个"从列表里双击进去的目录"记一条来源。
    // ----------------------------------------------------------------
    struct NavigationOrigin
    {
        std::wstring displayName;   // 地址栏/标题上显示的名字（如「文档」）
        std::wstring parentPath;    // 向上按钮要去的地方（如「主文件夹」）
    };

    // parentPath 留空表示"上一级就是进来之前待着的那个目录"（列表双击走这条）；
    // 传了具体路径就用它（左栏"常用位置"那几项固定回「主文件夹」）。
    void RecordNavigationOrigin(const std::wstring& path,
                                const std::wstring& displayName,
                                const std::wstring& parentPath = L"");

    // 当前目录给人看的名字：有来源记录就用记录的名字，否则按路径推导
    std::wstring DisplayNameFor(const std::wstring& path);

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

    // 当前目录里「真正有多少项」——分组标题行不算（标题、状态栏都按它显示）
    size_t RealItemCount();

    // 切换视图模式（modeIsUserChoice 为真表示是用户从菜单选的，之后不再自动改）
    void ApplyViewMode(FileListView::ViewMode mode, bool modeIsUserChoice);

    // 按"当前目录是什么类型"给出该用的默认视图
    FileListView::ViewMode DefaultViewModeForCurrentPath() const;

private:
    HWND      m_hWnd      = nullptr;
    HINSTANCE m_hInstance = nullptr;

    // ---- 视图模式 ----
    // 当前列表视图（大图标/小图标/列表/详细信息）。
    // 进入新目录时按"这个目录该用哪种视图"重新定一次（系统文件夹默认小图标，
    // 普通磁盘目录默认详细信息），用户从「查看」菜单手动选过之后就一直用他选的。
    FileListView::ViewMode m_viewMode        = FileListView::ViewMode::Details;
    bool                   m_viewModeManual  = false;   // 用户是否手动指定过视图

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

    // 导航来源表：key 是小写路径，value 是"从哪儿进来的、叫什么名字"。
    // 见上面 NavigationOrigin 的说明。只在列表里双击进入时写入，
    // 后退/前进/地址栏跳转都沿用它，所以来回切也还是显示「文档」。
    std::map<std::wstring, NavigationOrigin> m_navOrigins;

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
