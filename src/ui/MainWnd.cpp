// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.cpp
//
// 主窗口实现（阶段 3 版本）。
//
// 界面：导航栏（后退/前进/向上 + 地址栏） / 文件列表 / 状态栏
// 交互：地址栏回车跳转、按钮导航、双击进目录、双击文件打开
 // ============================================================================

#include "MainWnd.h"
#include "ui/WindowRelay.h"
#include "ui/BatchRenameDialog.h"
#include "core/Format.h"
#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/ShellClipboard.h"
#include "shell/ShellDragDrop.h"
#include "shell/ShellOperations.h"

#include <shellapi.h>   // ShellExecuteW
#include <shlobj.h>     // SHCreateItemFromParsingName / SHGetKnownFolderPath
#include <shlwapi.h>    // StrCmpLogicalW（资源管理器同款的本地化自然排序）
#include <shobjidl.h>   // IFileOperation / IShellItem2（撤销删除时要还原回收站里的项）
#include <propsys.h>    // PSGetPropertyKeyFromName（按属性名取回收站的 DeletedFrom 键）
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <thread>     // 拖出压缩包里的项：解压丢后台线程，主线程刷进度
#include <atomic>     // 后台线程解压完成标志
#include <memory>     // shared_ptr（把解压结果从后台线程递回主线程）

#pragma comment(lib, "propsys.lib")

#pragma comment(lib, "shlwapi.lib")

#include "../res/resource.h"   // IDI_APPICON 等资源 ID

// ============================================================================
// 常量
// ============================================================================

// 窗口类名的定义挪到了 WindowRelay（跨进程找"已经开着这个目录的窗口"要用它，
// 两处必须是同一个字符串，所以只留一个出处）
static const wchar_t* kWindowClassName = ui::kMainWindowClassName;
static const int      kInitialWidth    = 1180;
static const int      kInitialHeight   = 740;
static const wchar_t* kWindowTitleBase = L"文件管理器";

// 自定义消息：后台枚举完成
static const UINT WM_APP_ENUM_DONE = WM_APP + 1;

// 自定义消息：后台文件操作完成（复制 / 删除 / 移动）
static const UINT WM_APP_OPERATION_DONE = WM_APP + 2;

// 自定义消息：当前目录的内容变了（别的程序删了/搬走了/新建了文件）
static const UINT WM_APP_FOLDER_CHANGED = WM_APP + 3;

// 自定义消息：后台「新建」项枚举完成（主线程收到后去预取图标）
static const UINT WM_APP_SHELLNEW_READY = WM_APP + 4;

// 目录变化后的延迟刷新：短时间内的连续变化合并成一次刷新
// （批量复制一万个文件会连发几千条通知，不合并的话界面会刷成幻灯片）
static const UINT_PTR kFolderChangeTimerId  = 1;
static const UINT     kFolderChangeDelayMs  = 250;

// 起始路径
static const wchar_t* kInitialPath = L"C:\\";

// 日志里最多记多少条
static const size_t kMaxLogItems = 100;

// 拼接 / 拆路径的小工具（实现放在下面，这里先声明：上面的操作完成处理要用到）
static std::wstring JoinDirectoryFile(const std::wstring& directory, const std::wstring& fileName);
static std::wstring FileNameOf(const std::wstring& path);

// ============================================================================
// 内部辅助函数
// ============================================================================

// 上次记下来的窗口矩形还看得见吗？
//
// 为什么要检查：换了显示器、拔了副屏、改过分辨率，上次存的坐标可能整块落在屏幕外，
// 直接用会开出一个"看不见的窗口"，用户以为程序没启动。
// MONITOR_DEFAULTTONULL 在跟所有显示器都没交集时返回 NULL，正好用来判断。
static bool IsWindowRectVisible(const RECT& rect)
{
    return ::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr;
}

// ----------------------------------------------------------------------------
// 新窗口级联：再开一个窗口时往右下错开，别叠在完全同一个位置
// ----------------------------------------------------------------------------

// 每开一个新窗口，相对"已有的那个窗口"往右下错开多少像素。
// 取 32 的理由：既能把下层窗口的标题栏露出来，又不会像 22 那样容易点错到下层窗口。
static const int kCascadeOffset = 32;

// EnumWindows 的回调是 C 函数指针，带不了捕获，用这个结构把结果传回来
struct CascadeSearch
{
    RECT rect;      // 找到的那个窗口的"还原态"矩形
    bool found;
};

// EnumWindows 回调：找第一个（Z 序最上层）本程序主窗口，记下它的还原态矩形。
//
// 为什么用 GetWindowPlacement 的 rcNormalPosition，而不是 GetWindowRect：
// 基准窗口要是最大化状态，GetWindowRect 拿到的是整屏矩形，再往右下偏移必然出界，
// 级联就没意义了；rcNormalPosition 是它"还原"之后的位置，尺寸也更合理。
// 返回 FALSE 让 EnumWindows 立刻停下——第一个命中的就是最上层的那个，够用了。
static BOOL CALLBACK CascadeEnumProc(HWND hwnd, LPARAM param)
{
    CascadeSearch* search = reinterpret_cast<CascadeSearch*>(param);

    // 只要看得见的窗口：隐藏的不参与级联
    if (!::IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    // 只认本程序的窗口类名，别的程序的窗口一概不参与
    wchar_t className[64] = {};
    if (::GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0)
    {
        return TRUE;
    }
    if (::_wcsicmp(className, kWindowClassName) != 0)
    {
        return TRUE;
    }

    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (::GetWindowPlacement(hwnd, &placement))
    {
        search->rect = placement.rcNormalPosition;
    }
    else
    {
        // 取不到放置信息时退回普通矩形，总比没有基准强
        ::GetWindowRect(hwnd, &search->rect);
    }

    search->found = true;
    return FALSE;
}

// 有没有别的本程序窗口开着？有就把它的还原态矩形带回去（outRect）。
static bool FindExistingWindowRect(RECT& outRect)
{
    CascadeSearch search = {};
    ::EnumWindows(&CascadeEnumProc, reinterpret_cast<LPARAM>(&search));

    if (!search.found)
    {
        return false;
    }

    outRect = search.rect;
    return true;
}

// 从基准窗口算出新窗口的位置：往右下错开 kCascadeOffset。
//
// 关键细节——逐轴回退：
//   如果工作区右边 / 下边已经放不下错开后的窗口，就**只在那一轴不错开**，
//   另一轴照常错开。这样至少能保证"新窗口永远在基准的右下方（或同高 / 同左）"。
//   如果不做这个回退，交给后面的 ClampRectToWorkArea 硬推回来，
//   新窗口会被推到基准的**上方**，看起来像是"往上跳了一层"，很别扭。
static RECT CascadeRectFromBase(const RECT& base)
{
    // 基准窗口所在的那块屏的工作区（= 屏幕去掉任务栏）
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT work = base;
    if (::GetMonitorInfoW(::MonitorFromRect(&base, MONITOR_DEFAULTTONEAREST), &info))
    {
        work = info.rcWork;
    }

    const int width  = base.right - base.left;
    const int height = base.bottom - base.top;

    int left = base.left + kCascadeOffset;
    int top  = base.top  + kCascadeOffset;

    if (left + width > work.right)
    {
        left = base.left;      // 右边放不下：横向不错开
    }
    if (top + height > work.bottom)
    {
        top = base.top;        // 下边放不下：纵向不错开
    }

    RECT result = { left, top, left + width, top + height };
    return result;
}

// 把矩形推进目标显示器的工作区里（工作区 = 屏幕去掉任务栏的那一条）。
//
// 两种情况要处理：
//   · 级联之后右下角跑出屏幕 → 整体往左上推回来；
//   · 窗口本身比工作区还大 → 先把宽高压到工作区大小，否则怎么推都出界。
static void ClampRectToWorkArea(RECT& rect)
{
    MONITORINFO info = {};
    info.cbSize = sizeof(info);

    HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    if (::GetMonitorInfoW(monitor, &info) == FALSE)
    {
        return;
    }

    const RECT& work = info.rcWork;
    const int workWidth  = work.right - work.left;
    const int workHeight = work.bottom - work.top;

    int width  = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width > workWidth)
    {
        width = workWidth;
    }
    if (height > workHeight)
    {
        height = workHeight;
    }

    int left = rect.left;
    int top  = rect.top;

    if (left + width > work.right)
    {
        left = work.right - width;
    }
    if (top + height > work.bottom)
    {
        top = work.bottom - height;
    }
    if (left < work.left)
    {
        left = work.left;
    }
    if (top < work.top)
    {
        top = work.top;
    }

    rect.left   = left;
    rect.top    = top;
    rect.right  = left + width;
    rect.bottom = top + height;
}

// 把路径换成"给人看的名字"。
//   普通磁盘路径（C:\Windows）原样返回，零开销；
//   虚拟文件夹（此电脑这类）的路径是 ::{GUID}，直接显示很难看，
//   这里问系统要它的显示名（「此电脑」「回收站」）。
static std::wstring DisplayNameForPath(const std::wstring& path)
{
    // 命名空间最顶层那个容器不是真实文件夹，没有解析名，名字由我们自己定。
    // 用户要求叫「主文件夹」（系统里它叫「桌面」，容易和物理桌面目录混起来）。
    if (shell::IsTopDesktopPath(path))
    {
        return L"主文件夹";
    }

    const bool isShellNamespace = (path.compare(0, 2, L"::") == 0) ||
                                  (path.compare(0, 6, L"shell:") == 0);
    if (!isShellNamespace)
    {
        return path;
    }

    IShellItem* item = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)) ||
               item == nullptr))
    {
        return path;
    }

    std::wstring result = path;

    PWSTR displayName = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName)) && displayName != nullptr)
    {
        result.assign(displayName);
        ::CoTaskMemFree(displayName);
    }

    item->Release();
    return result;
}

// 上次记下的启动目录现在还能不能用？
//
// 普通磁盘目录：看目录还在不在（可能被用户删了、U 盘拔了），不在了就回默认目录；
// 虚拟项（「::」开头、「shell:」开头）：问 Shell 能不能解析出来。
static bool IsStartPathUsable(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    // 「主文件夹」是本程序自己的伪路径，不需要问 Shell
    if (shell::IsTopDesktopPath(path))
    {
        return true;
    }

    const bool isShellNamespace = (path.compare(0, 2, L"::") == 0) ||
                                  (path.compare(0, 6, L"shell:") == 0);

    if (isShellNamespace)
    {
        IShellItem* item = nullptr;
        const bool parsed = SUCCEEDED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                                   IID_PPV_ARGS(&item)));
        if (item != nullptr)
        {
            item->Release();
        }
        return parsed;
    }

    const DWORD attributes = ::GetFileAttributesW(path.c_str());

    // 压缩包（.zip/.rar/.7z…）磁盘上是文件，但能当文件夹枚举，
    // 启动参数给它是"要进去浏览"，所以按可用处理（否则会被回退到上次目录）
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        shell::IsContainerFileExtension(path))
    {
        return true;
    }

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// FILETIME 转成 64 位整数，方便比大小
static uint64_t FileTimeValue(const FILETIME& time){
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | static_cast<uint64_t>(time.dwLowDateTime);
}

// 排序规则
//   系统项永远排前面（「主文件夹」里先排此电脑/桌面/文档这些系统入口，
//   再排桌面上的文件，跟资源管理器一致）
//   文件夹永远排前面（和资源管理器一致，不受点哪一列影响）
//   column：0 名称 / 1 修改日期 / 2 类型 / 3 大小
//   ascending：true 从小到大
static bool CompareEntries(const shell::ShellEntry& left,
                           const shell::ShellEntry& right,
                           int  column,
                           bool ascending)
{
    // 系统项优先：这一条不受"点哪一列"影响，也不管是文件夹还是文件
    if (left.isSystemItem != right.isSystemItem)
    {
        return left.isSystemItem;
    }

    if (left.isFolder != right.isFolder)
    {
        return left.isFolder;
    }

    int result = 0;

    switch (column)
    {
    case 1:   // 修改日期
    {
        uint64_t a = FileTimeValue(left.modified);
        uint64_t b = FileTimeValue(right.modified);
        if (a != b)
        {
            result = (a < b) ? -1 : 1;
        }
        break;
    }

    case 2:   // 类型
        result = ::StrCmpLogicalW(format::TypeText(left.name, left.isFolder).c_str(),
                                  format::TypeText(right.name, right.isFolder).c_str());
        break;

    case 3:   // 大小
        if (left.size != right.size)
        {
            result = (left.size < right.size) ? -1 : 1;
        }
        break;

    default:  // 名称
        result = ::StrCmpLogicalW(left.name.c_str(), right.name.c_str());
        break;
    }

    // 比出来一样大时再按名字排，保证每次排出来顺序都稳定
    if (result == 0)
    {
        result = ::StrCmpLogicalW(left.name.c_str(), right.name.c_str());
    }

    return ascending ? (result < 0) : (result > 0);
}

// ============================================================================
// 构造 / 析构
// ============================================================================

MainWindow::MainWindow()
    : m_operationRunning(std::make_shared<std::atomic<bool>>(false))
{
    // 把排序规则交给列表控件：新建文件夹后要按这个规则插到正确位置
    m_fileListView.SetComparator([this](const shell::ShellEntry& left, const shell::ShellEntry& right)
    {
        return CompareEntries(left, right, m_sortColumn, m_sortAscending);
    });
}

MainWindow::~MainWindow()
{
    m_enumToken.Cancel();
    if (m_enumThread.joinable())
    {
        m_enumThread.join();
    }

    // 等后台文件操作收尾（大文件正在复制时会等它结束，或者用户在进度框点了取消）
    if (m_operationThread.joinable())
    {
        m_operationThread.join();
    }

    // 等后台「新建」项枚举收尾，再把菜单图标的取图线程收掉
    if (m_shellNewThread.joinable())
    {
        m_shellNewThread.join();
    }
    m_menuIcons.Shutdown();

    if (m_hAccelerator != nullptr)
    {
        ::DestroyAcceleratorTable(m_hAccelerator);
        m_hAccelerator = nullptr;
    }

    if (m_hUiFont != nullptr)
    {
        ::DeleteObject(m_hUiFont);
        m_hUiFont = nullptr;
    }
}

// ============================================================================
// 创建与消息循环
// ============================================================================

bool MainWindow::Create(HINSTANCE hInstance,
                        const std::wstring& initialPath,
                        const std::wstring& initialSelectFile)
{
    m_hInstance        = hInstance;
    m_startupPath      = initialPath;
    m_startupSelectFile = initialSelectFile;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &MainWindow::WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;

    // 主窗口图标（标题栏左上角 + 任务栏）：用 exe 自己带的图标，
    // 取不到（比如资源没编进去）就退回系统默认的空白程序图标
    wc.hIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (wc.hIcon == nullptr)
    {
        wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    // 小图标（16x16，标题栏和 Alt+Tab 用）
    int smallIconSize = ::GetSystemMetrics(SM_CXSMICON);
    wc.hIconSm = static_cast<HICON>(::LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                                IMAGE_ICON, smallIconSize, smallIconSize,
                                                LR_DEFAULTCOLOR));
    if (wc.hIconSm == nullptr)
    {
        wc.hIconSm = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    if (!::RegisterClassExW(&wc))
    {
        return false;
    }

    // 配置必须在建窗口之前读：窗口开在哪儿、多大，要按上次关掉时的样子来。
    // （Load 是幂等的，后面 CreateChildControls 里再调一次不会重读磁盘）
    core::Settings::Instance().Load();

    int posX = 0;
    int posY = 0;
    int windowWidth  = kInitialWidth;
    int windowHeight = kInitialHeight;

    const core::WindowState& saved = core::Settings::Instance().Window();

    if (saved.valid)
    {
        RECT savedRect = { saved.x, saved.y, saved.x + saved.width, saved.y + saved.height };

        if (IsWindowRectVisible(savedRect))
        {
            posX         = saved.x;
            posY         = saved.y;
            windowWidth  = saved.width;
            windowHeight = saved.height;
            m_startMaximized = saved.maximized;
        }
        else
        {
            Logger::Warn(L"上次的窗口位置已经不在任何显示器上了，改回居中开窗");
        }
    }

    // 已经有别的本程序窗口开着（典型场景：右键「在新窗口中打开」、Ctrl+双击文件夹）：
    // 新窗口相对那个窗口往右下错开，别叠在完全同一个位置上。
    //
    // 这段必须放在"首次运行居中"之前：级联算出来的位置也要算数，
    // 不能被居中逻辑再覆盖一次。
    bool cascaded = false;
    RECT cascadeBase = {};
    if (FindExistingWindowRect(cascadeBase))
    {
        RECT target = CascadeRectFromBase(cascadeBase);   // 往右下错开（放不下的那一轴不错开）
        ClampRectToWorkArea(target);                      // 兜底：还出界就推回来

        posX         = target.left;
        posY         = target.top;
        windowWidth  = target.right - target.left;
        windowHeight = target.bottom - target.top;

        // 新窗口一律按普通（非最大化）状态开：基准窗口最大化时再开一个最大化窗口，
        // 等于把基准整个盖住，级联就白做了。
        m_startMaximized = false;
        cascaded         = true;

        Logger::Info(L"新窗口级联错开：" + std::to_wstring(posX) + L"," + std::to_wstring(posY));
    }

    if (!cascaded && posX == 0 && posY == 0 && !saved.valid)
    {
        // 首次运行：按屏幕居中
        int screenWidth  = ::GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);
        posX = (screenWidth  - windowWidth)  / 2;
        posY = (screenHeight - windowHeight) / 2;
    }

    m_hWnd = ::CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitleBase,
        WS_OVERLAPPEDWINDOW,
        posX, posY, windowWidth, windowHeight,
        nullptr, nullptr, hInstance,
        this);

    return (m_hWnd != nullptr);
}

void MainWindow::Show(int nCmdShow)
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    // 上次关窗口时是最大化的，就还按最大化打开
    if (m_startMaximized)
    {
        nCmdShow = SW_SHOWMAXIMIZED;
    }

    ::ShowWindow(m_hWnd, nCmdShow);
    ::UpdateWindow(m_hWnd);
}

int MainWindow::RunMessageLoop()
{
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        // 地址栏正在手工打字时，Ctrl+C / Ctrl+V 应该作用于输入框，
        // 不能被我下面的快捷键表抢走
        bool editingInAddressBar = false;
        HWND focus = ::GetFocus();
        if (focus != nullptr)
        {
            wchar_t className[64] = {};
            ::GetClassNameW(focus, className, 64);
            if (::wcscmp(className, L"Edit") == 0)
            {
                editingInAddressBar = true;
            }
        }

        if (!editingInAddressBar && m_hAccelerator != nullptr &&
            ::TranslateAcceleratorW(m_hWnd, m_hAccelerator, &msg))
        {
            continue;
        }

        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// ============================================================================
// DPI 缩放
// ============================================================================

int MainWindow::Scale(int value) const
{
    UINT dpi = 96;
    if (m_hWnd != nullptr)
    {
        dpi = ::GetDpiForWindow(m_hWnd);
        if (dpi == 0)
        {
            dpi = 96;
        }
    }
    return ::MulDiv(value, static_cast<int>(dpi), 96);
}

// ============================================================================
// 子控件创建
// ============================================================================

void MainWindow::CreateChildControls()
{
    // 配置文件先读进来（固定区要靠它显示，窗口大小那些以后也用它）
    core::Settings::Instance().Load();

    // ---------------- 1. 字体 ----------------
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_hUiFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (m_hUiFont == nullptr)
    {
        m_hUiFont = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    // ---------------- 2. 文件列表 ----------------
    if (!m_fileListView.Create(m_hWnd, m_hInstance))
    {
        Logger::Error(L"文件列表控件创建失败");
    }
    m_fileListView.SetActivateCallback([this](size_t index, bool openInNewWindow)
    {
        // 按住 Ctrl 双击文件夹 = 在新窗口打开（跟右键那个菜单项一个效果）
        if (openInNewWindow && ActivateItemInNewWindow(index))
        {
            return;
        }

        OnItemActivated(index);
    });
    m_fileListView.SetContextMenuCallback([this](POINT screenPoint)
    {
        ShowContextMenu(screenPoint);
    });
    m_fileListView.SetColumnClickCallback([this](int column)
    {
        OnColumnClick(column);
    });
    m_fileListView.SetRenameCallback([this](size_t index, const std::wstring& newName)
    {
        DoRename(index, newName);
    });

    // ---------------- 2b. 左侧导航栏 ----------------
    if (!m_navigationPane.Create(m_hWnd, m_hInstance, &m_iconCache))
    {
        Logger::Warn(L"左侧导航栏创建失败，界面仍可用");
    }
    else
    {
        // 点导航栏里的「桌面」「下载」「C:」→ 进目录；点固定区里的程序 → 启动它
        m_navigationPane.SetNavigateCallback([this](const std::wstring& path)
        {
            OpenPinnedOrFolder(path);
        });

        // 固定区：先把配置文件里记着的那些显示出来（第一次运行时是空的，显示提示行）
        m_navigationPane.SetPinnedItems(core::Settings::Instance().PinnedItems());

        // 在固定项上点右键 →「取消固定」：从配置里删掉，存盘，再把左栏刷新一遍
        m_navigationPane.SetUnpinCallback([this](const std::wstring& path)
        {
            if (!core::Settings::Instance().RemovePinned(path))
            {
                return;
            }

            core::Settings::Instance().Save();

            m_navigationPane.SetPinnedItems(core::Settings::Instance().PinnedItems());
            m_navigationPane.SelectFolderPath(m_history.Current());   // 重建树后选中状态要补回来

            Logger::Info(L"取消固定：" + path);
        });
    }

    // ---------------- 3. 导航按钮（自绘扁平按钮，无边框、无白底） ----------------
    // 底色统一用顶部工具条的 248 灰，否则按钮区域会出现一块白底
    const COLORREF toolbarColor = RGB(248, 248, 248);

    m_homeButton.Create(m_hWnd, kControlIdHome, FlatButton::ArrowKind::Home, m_hInstance);
    m_homeButton.SetBackgroundColor(toolbarColor);

    m_backButton.Create(m_hWnd, kControlIdBack, FlatButton::ArrowKind::Back, m_hInstance);
    m_backButton.SetBackgroundColor(toolbarColor);

    m_forwardButton.Create(m_hWnd, kControlIdForward, FlatButton::ArrowKind::Forward, m_hInstance);
    m_forwardButton.SetBackgroundColor(toolbarColor);

    m_upButton.Create(m_hWnd, kControlIdUp, FlatButton::ArrowKind::Up, m_hInstance);
    m_upButton.SetBackgroundColor(toolbarColor);

    m_refreshButton.Create(m_hWnd, kControlIdRefresh, FlatButton::ArrowKind::Refresh, m_hInstance);
    m_refreshButton.SetBackgroundColor(toolbarColor);

    // 计算机图标：贴在地址栏左端，白色底，和路径的白底连成一片
    m_thisPcButton.Create(m_hWnd, kControlIdThisPc, FlatButton::ArrowKind::Computer, m_hInstance);
    m_thisPcButton.SetBackgroundColor(RGB(255, 255, 255));

    // 设置：贴在地址栏右端（齿轮），白色底
    m_settingsButton.Create(m_hWnd, kControlIdSettings, FlatButton::ArrowKind::Gear, m_hInstance);
    m_settingsButton.SetBackgroundColor(RGB(255, 255, 255));

    // ---------------- 4. 地址栏（分段可点击） ----------------
    if (m_addressBar.Create(m_hWnd, kControlIdAddress, m_hInstance))
    {
        if (m_hUiFont != nullptr)
        {
            m_addressBar.SetFont(m_hUiFont);
        }

        // 点了某一段（比如 "C:" 或 "Windows"）就跳到那个目录
        m_addressBar.SetNavigateCallback([this](const std::wstring& path)
        {
            NavigateToPath(path);
        });
    }
    else
    {
        Logger::Warn(L"地址栏控件创建失败");
    }

    // ---------------- 5. 状态栏 ----------------
    m_hStatusBar = ::CreateWindowExW(
        0,
        STATUSCLASSNAMEW,
        nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, m_hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1005)),
        m_hInstance, nullptr);

    if (m_hUiFont != nullptr)
    {
        ::SendMessageW(m_hStatusBar, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    }

    // ---------------- 6. 键盘快捷键 ----------------
    // 用系统的加速键表，好处是不用去拦截每个控件的按键消息
    //
    // 【重要·条目顺序】
    // TranslateAccelerator 是按表序逐个匹配、第一个命中就用的。实测发现：
    //   带不带修饰键的两条如果键码相同，"不带修饰键"的那条会把"带 Shift 的"一起吃掉
    //   （比如 Shift+Delete 落到了普通 Delete，Ctrl+Shift+C 落到了普通 Ctrl+C）。
    // 因此凡是存在"修饰键子集"关系的组合，一律把**修饰键更多的排在前面**。
    ACCEL accelerators[] =
    {
        // 复制文件路径（Ctrl+Shift+C）——必须排在 Ctrl+C 之前，否则会被 Ctrl+C 抢走
        { static_cast<BYTE>(FCONTROL | FSHIFT | FVIRTKEY), 'C', kCommandCopyPath },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'C',       kCommandCopy },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'X',       kCommandCut },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'V',       kCommandPaste },
        // 永久删除（Shift+Delete）——必须排在普通 Delete 之前，否则会被普通 Delete 抢走
        { static_cast<BYTE>(FSHIFT | FVIRTKEY),   VK_DELETE, kCommandDeletePermanent },
        { static_cast<BYTE>(FVIRTKEY),            VK_DELETE, kCommandDelete },
        { static_cast<BYTE>(FVIRTKEY),            VK_F5,     kCommandRefresh },
        { static_cast<BYTE>(FCONTROL | FSHIFT | FVIRTKEY), 'N', kCommandNewFolder },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'A',       kCommandSelectAll },
        { static_cast<BYTE>(FVIRTKEY),            VK_BACK,   kCommandGoUp },
        { static_cast<BYTE>(FVIRTKEY),            VK_F2,     kCommandRename },
        // 撤销 / 恢复：键盘发的是"键盘专用命令"，右键菜单发的是普通命令。
        // 拆开是为了让批量重命名只能从菜单撤销，Ctrl+Z 不响应它（见 PerformUndo 里的判断）
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'Z',       kCommandUndoKeyboard },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'Y',       kCommandRedoKeyboard },

        // ---- 2026-11-18 新增：对齐 Windows 11 资源管理器的常用快捷键 ----
        // 说明：这几个都是"窗口级"快捷键，只在焦点不在输入框时生效
        //       （消息循环里已经判断过，焦点在 Edit 时整张表都会跳过）。
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'N', kCommandNewWindow },    // 新窗口
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'T', kCommandHomeWindow },   // 主文件夹窗口
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'W', kCommandCloseWindow },  // 关闭当前窗口
        { static_cast<BYTE>(FALT | FVIRTKEY), VK_LEFT,  kCommandGoBackKey },   // Alt+← 后退
        { static_cast<BYTE>(FALT | FVIRTKEY), VK_RIGHT, kCommandGoForwardKey },// Alt+→ 前进
        { static_cast<BYTE>(FALT | FVIRTKEY), VK_UP,    kCommandGoUp },        // Alt+↑ 上一级
        { static_cast<BYTE>(FALT | FVIRTKEY), 'D', kCommandFocusAddressBar },  // Alt+D 焦点到地址栏
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'L', kCommandFocusAddressBar }, // Ctrl+L 同上
        { static_cast<BYTE>(FALT | FVIRTKEY), VK_RETURN, kCommandProperties }, // Alt+Enter 属性
        // 注意：Shift+Delete 的条目放在表头上方、普通 Delete 之前（顺序见本表开头的说明）
        // Ctrl+加号：列宽自适应。数字键盘的 + 是 VK_ADD，主键盘的 +/= 是 VK_OEM_PLUS，两个都认
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), VK_ADD, kCommandAutoFitColumns },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), VK_OEM_PLUS, kCommandAutoFitColumns },
    };

    m_hAccelerator = ::CreateAcceleratorTableW(accelerators, ARRAYSIZE(accelerators));
    if (m_hAccelerator == nullptr)
    {
        Logger::Warn(L"快捷键表创建失败，键盘操作不可用");
    }
}

// ============================================================================
// 窗口过程
// ============================================================================

LRESULT CALLBACK MainWindow::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        if (self != nullptr)
        {
            self->m_hWnd = hWnd;
            ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }
    else
    {
        self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // --------------------------------------------------------------
    // 别的窗口把"打开这个目录"的请求转过来（跨进程 WM_COPYDATA）
    //
    // 对方问"你是不是正停在这个目录"，是就拉前自己并回 1，对方收到 1 就不再
    // 开新窗口；不是就回 0，对方继续问下一个窗口。
    // --------------------------------------------------------------
    case WM_COPYDATA:
    {
        const COPYDATASTRUCT* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);

        if (data != nullptr && data->dwData == ui::kRelayRequestMagic &&
            data->lpData != nullptr && data->cbData >= sizeof(ui::RelayRequest))
        {
            const ui::RelayRequest* request = static_cast<const ui::RelayRequest*>(data->lpData);

            if (ui::IsSameFolder(m_history.Current(), request->folder))
            {
                const std::wstring selectFile =
                    (request->selectFile[0] != 0) ? std::wstring(request->selectFile) : std::wstring();

                ActivateFromRelay(selectFile);
                return 1;   // 告诉发送方：这个请求我接了
            }
        }

        return 0;
    }

    // --------------------------------------------------------------
    // 创建
    // --------------------------------------------------------------
    case WM_CREATE:
    {
        // 父窗口绘制时跳过子控件占的区域，避免拖动分隔线时整片重画闪烁
        LONG_PTR windowStyle = ::GetWindowLongPtrW(m_hWnd, GWL_STYLE);
        ::SetWindowLongPtrW(m_hWnd, GWL_STYLE, windowStyle | WS_CLIPCHILDREN);

        // 图标缓存必须先初始化：左侧导航栏建树时就要用它取图标
        if (m_iconCache.Initialize())
        {
            Logger::Info(L"系统图标列表初始化成功");
        }
        else
        {
            Logger::Warn(L"系统图标列表初始化失败，界面将不显示图标");
        }

        CreateChildControls();
        m_fileListView.SetIconCache(&m_iconCache);

        // 拖完表头列宽，把三列宽度记进配置（下次打开还是这个宽度）
        m_fileListView.SetColumnWidthChangedCallback([this]()
        {
            SaveWindowState();
        });

        // 阶段 10：把上次记下来的样子恢复出来（左栏宽度、三列宽度、查看模式）
        RestoreSavedLayout();

        // 从本程序把文件拖到别处并且是"移动"时，源目录内容已经变了，要刷新列表
        m_fileListView.SetDragCompletedCallback([this](DWORD)
        {
            RefreshCurrentFolder();
        });

        // 拖出前把路径准备好：普通文件原样返回；压缩包内部的项先解到临时目录
        m_fileListView.SetPrepareDragPathsCallback([this](const std::vector<std::wstring>& paths)
        {
            return PrepareDragPaths(paths);
        });

        // 注册拖放接收：资源管理器/桌面/别的程序把文件拖进列表或左栏都能落地
        RegisterDropTargets();

        // 右键菜单要用的数据：「新建」项清单 + 菜单小图标，全部在后台准备，
        // 不占启动时间；菜单弹出时只读内存
        StartBackgroundMenuData();

        LayoutChildren();

        // 上次停在哪个目录就直接打开哪个（目录已经不在了就回 C:\）
        std::wstring startPath;

        // 「在新窗口中打开」等带命令行参数启动时，优先用参数里的目录
        if (!m_startupPath.empty() && IsStartPathUsable(m_startupPath))
        {
            startPath = m_startupPath;
            Logger::Info(L"按命令行参数启动：" + startPath);
        }
        else
        {
            if (!m_startupPath.empty())
            {
                Logger::Warn(L"命令行给的目录不可用：" + m_startupPath);
            }

            startPath = core::Settings::Instance().LastFolder();

            if (startPath.empty() || !IsStartPathUsable(startPath))
            {
                if (!startPath.empty())
                {
                    Logger::Warn(L"上次的目录已经不在了，改回默认目录：" + startPath);
                }
                startPath = kInitialPath;
            }
        }

        Logger::Info(L"启动目录：" + startPath);

        m_history.Reset(startPath);
        StartEnumeration(startPath);
        return 0;
    }

    // --------------------------------------------------------------
    // 尺寸变化
    // --------------------------------------------------------------
    case WM_SIZE:
        LayoutChildren();

        // 窗口宽度变了，「此电脑」平铺一行能放几块卡片就变了，
        // 分组标题后面的占位格数要跟着重算，否则标题行填不满、卡片会乱跑
        RefreshThisPcGrouping();
        return 0;

    // --------------------------------------------------------------
    // 绘制：地址栏下面那条横线、左栏右边那条竖线
    // --------------------------------------------------------------
    case WM_ERASEBKGND:
        // 背景我们已经在 WM_PAINT 里整块铺过了，这里直接说"擦好了"，
        // 少一次闪烁（拖动分隔线时尤其明显）
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC hdc = ::BeginPaint(m_hWnd, &paint);

        RECT clientRect = {};
        ::GetClientRect(m_hWnd, &clientRect);

        // 控件之间的缝隙是父窗口自己的地盘，先铺一层底色
        ::FillRect(hdc, &clientRect, ::GetSysColorBrush(COLOR_WINDOW));

        // 顶部工具条（按钮 + 地址栏那一行）用浅灰底（248,248,248），
        // 只有地址栏那块本身仍然是白底（它自己画），这样"路径区"才显得是独立一块
        if (!::IsRectEmpty(&m_addressDividerRect))
        {
            RECT topRect = clientRect;
            topRect.bottom = m_addressDividerRect.top;

            HBRUSH topBrush = ::CreateSolidBrush(RGB(248, 248, 248));
            ::FillRect(hdc, &topRect, topBrush);
            ::DeleteObject(topBrush);
        }

        // 地址栏下面那条横线（把上面的路径和下面的文件区分开）
        if (!::IsRectEmpty(&m_addressDividerRect))
        {
            ::FillRect(hdc, &m_addressDividerRect, ::GetSysColorBrush(COLOR_3DLIGHT));
        }

        // 左栏和文件列表中间那条竖线
        if (m_paneVisible && !::IsRectEmpty(&m_paneDividerRect))
        {
            ::FillRect(hdc, &m_paneDividerRect, ::GetSysColorBrush(COLOR_3DLIGHT));
        }

        ::EndPaint(m_hWnd, &paint);
        return 0;
    }

    // --------------------------------------------------------------
    // 中间那条竖线：鼠标移上去变成左右箭头，按住能拖着改左栏宽度
    // --------------------------------------------------------------
    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt = {};
            ::GetCursorPos(&pt);
            ::ScreenToClient(m_hWnd, &pt);

            if (IsOnPaneDivider(pt.x))
            {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        int x = static_cast<short>(LOWORD(lParam));
        if (IsOnPaneDivider(x))
        {
            m_draggingSplitter = true;
            ::SetCapture(m_hWnd);   // 抓住鼠标，拖出窗口范围也能继续跟手
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        if (m_draggingSplitter)
        {
            UpdatePaneWidth(static_cast<short>(LOWORD(lParam)));
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP:
    {
        if (m_draggingSplitter)
        {
            m_draggingSplitter = false;
            ::ReleaseCapture();

            // 拖完这一下就把左栏宽度记进配置：下次打开还是这个宽度
            SaveWindowState();
            return 0;
        }
        break;
    }

    case WM_CAPTURECHANGED:
        // 鼠标被别的东西抢走了（比如弹出菜单），就当拖动结束
        m_draggingSplitter = false;
        break;

    // --------------------------------------------------------------
    // 按钮点击
    // --------------------------------------------------------------
    case WM_COMMAND:
    {
        int controlId = LOWORD(wParam);

        // ---- 「打开方式」子菜单里点了某个程序：命令 ID = 3400 + 账本下标 ----
        if (controlId >= kCommandOpenWithBase &&
            controlId <= kCommandOpenWithBase + 99)
        {
            OpenSelectionWithProgram(static_cast<size_t>(controlId - kCommandOpenWithBase));
            return 0;
        }

        // ---- 「新建」子菜单里点了某一项：命令 ID = 3500 + 快照下标 ----
        if (controlId >= kCommandNewBase && controlId <= kCommandNewBase + 99)
        {
            DoNewItem(static_cast<size_t>(controlId - kCommandNewBase));
            return 0;
        }

        // ---- 快捷键命令（由加速键表发来，通知码为 0/1）----
        switch (controlId)
        {
        case kCommandCopy:
            DoCopy();
            return 0;
        case kCommandCopyPath:
            DoCopyPath();
            return 0;
        case kCommandCut:
            DoCut();
            return 0;
        case kCommandPaste:
            DoPaste();
            return 0;
        case kCommandDelete:
            // Delete：进回收站；按住 Shift 则永久删除（Shell 会弹"确实要永久删除"确认框）
            //
            // 为什么这里还要读一次 Shift 状态：
            //   正常情况下 Shift+Delete 会先命中表里那条 FSHIFT 条目（走 kCommandDeletePermanent），
            //   但保险起见再兜一层——万一哪天条目顺序被调整、或系统把带修饰键的匹配让给了
            //   普通条目，这里读真实键盘状态仍能走对分支。GetAsyncKeyState 读的是全局键状态，
            //   真人按键时可靠。
            DoDelete((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        case kCommandRefresh:
            RefreshCurrentFolder();
            return 0;
        case kCommandNewFolder:
            DoNewFolder();
            return 0;
        case kCommandSelectAll:
            m_fileListView.SelectAll();
            return 0;
        case kCommandGoUp:
            GoUp();
            return 0;
        case kCommandOpen:
            OpenSelection();
            return 0;
        case kCommandUndo:
            PerformUndo(false);   // 右键菜单「撤销」：批量重命名也能撤
            return 0;
        case kCommandRedo:
            PerformRedo(false);
            return 0;
        case kCommandUndoKeyboard:
            PerformUndo(true);    // Ctrl+Z：批量重命名这条会被拦掉
            return 0;
        case kCommandRedoKeyboard:
            PerformRedo(true);    // Ctrl+Y：同上
            return 0;
        case kCommandOpenInNewWindow:
        {
            // 「在新窗口中打开」：文件夹 / 盘符 用自身进程新开一个窗口
            const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

            if (!selected.empty())
            {
                OpenFolderInNewWindow(selected.front());
            }
            else
            {
                OpenFolderInNewWindow(m_history.Current());
            }
            return 0;
        }
        case kCommandOpenWithSelf:
        {
            // 「打开方式 → Yexplorer（本程序）」：这一次用本程序打开选中的压缩包，
            // 进容器里浏览。改不改默认由用户自己在设置窗口里决定，这里不动账本。
            const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

            if (!selected.empty() && shell::IsContainerFileExtension(selected.front()))
            {
                NavigateToPath(selected.front());
            }
            return 0;
        }
        case kCommandOpenWithManage:
        {
            // 「添加或管理程序…」：管的是当前选中文件那个扩展名
            std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
            if (!selected.empty())
            {
                ShowOpenWithSettings(shell::ExtensionKeyForPath(selected.front()), false);
            }
            return 0;
        }
        case kCommandProperties:
            ShowPropertiesForSelection();
            return 0;
        case kCommandRename:
            m_fileListView.BeginRenameSelected();
            return 0;
        case kCommandBatchRename:
            DoBatchRename();
            return 0;
        case kCommandViewLarge:
            ApplyViewMode(FileListView::ViewMode::LargeIcon, true);
            return 0;
        case kCommandViewSmall:
            ApplyViewMode(FileListView::ViewMode::SmallIcon, true);
            return 0;
        case kCommandViewList:
            ApplyViewMode(FileListView::ViewMode::List, true);
            return 0;
        case kCommandViewDetails:
            ApplyViewMode(FileListView::ViewMode::Details, true);
            return 0;
        case kCommandViewTiles:
            ApplyViewMode(FileListView::ViewMode::Tiles, true);
            return 0;

        // ---- 2026-11-18 新增的快捷键命令 ----
        case kCommandNewWindow:
        {
            // Ctrl+N：选中的是一个文件夹就用它；否则把当前目录再开一遍。
            // 用户要求"不排除当前目录"，所以跳过"该目录已有窗口就拉前台"的去重
            // （forceNewWindow = true），一定开出一个新窗口。
            std::wstring target = m_history.Current();

            const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
            if (selected.size() == 1)
            {
                // 只有"单选 且 确实是文件夹"才改目标；选的是文件、或选了多个，都按当前目录
                std::shared_ptr<shell::EnumerateResult> result = CurrentResult();
                const int index = ListView_GetNextItem(m_fileListView.Handle(), -1, LVNI_SELECTED);

                if (result != nullptr && index >= 0 &&
                    index < static_cast<int>(result->entries.size()))
                {
                    const shell::ShellEntry& entry = result->entries[index];
                    if (entry.isFolder && !entry.isGroupHeader && !entry.isSpacer)
                    {
                        target = selected.front();
                    }
                }
            }

            OpenFolderInNewWindow(target, true);
            return 0;
        }
        case kCommandHomeWindow:
            // Ctrl+T：开一个停在「主文件夹」的新窗口（同样跳过"已有窗口就拉前台"）
            OpenFolderInNewWindow(shell::kTopDesktopPath, true);
            return 0;
        case kCommandCloseWindow:
            // Ctrl+W：关掉当前窗口。用 PostMessage 而不是直接销毁，
            // 让关闭走正常流程（会把窗口大小/位置存进配置）
            ::PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
            return 0;
        case kCommandGoBackKey:
            GoBack();
            return 0;
        case kCommandGoForwardKey:
            GoForward();
            return 0;
        case kCommandFocusAddressBar:
            // Alt+D / Ctrl+L：地址栏进入输入状态
            // （内部会放到编辑框、设置焦点并全选，效果和点地址栏空白处一样）
            m_addressBar.BeginEditing();
            return 0;
        case kCommandDeletePermanent:
            // Shift+Delete、以及 Shift+右键点「删除」：永久删除（Shell 会弹确认框）
            DoDelete(true);
            return 0;
        case kCommandAutoFitColumns:
            // Ctrl+加号：列宽按内容自适应
            m_fileListView.AutoFitColumns();
            return 0;
        default:
            break;
        }

        int notification = HIWORD(wParam);

        if (notification == BN_CLICKED)
        {
            switch (controlId)
            {
            case kControlIdBack:
                GoBack();
                return 0;
            case kControlIdForward:
                GoForward();
                return 0;
            case kControlIdUp:
                GoUp();
                return 0;
            case kControlIdHome:
            {
                // 主页：去「我的文档」
                PWSTR documentsPath = nullptr;
                if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documentsPath)) &&
                    documentsPath != nullptr)
                {
                    std::wstring target(documentsPath);
                    ::CoTaskMemFree(documentsPath);
                    NavigateToPath(target);
                }
                return 0;
            }
            case kControlIdRefresh:
                RefreshCurrentFolder();
                return 0;
            case kControlIdThisPc:
                // 计算机图标：去「此电脑」（虚拟文件夹，路径是它的 CLSID）
                NavigateToPath(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");
                return 0;
            case kControlIdSettings:
            {
                // 设置窗口（右键菜单图标、显示隐藏文件、默认文件管理器）
                //
                // 显示隐藏文件 / 显示系统文件这两个开关一改，当前目录的内容就会变，
                // 所以记下改之前的值，关窗后发现有变化就按新规则重新枚举一次。
                const core::Settings& settings = core::Settings::Instance();
                const bool wasHidden = settings.ShowHiddenFiles();
                const bool wasSystem = settings.ShowSystemFiles();

                SettingsDialog dialog;
                dialog.ShowModal(m_hWnd);

                if (wasHidden != settings.ShowHiddenFiles() ||
                    wasSystem != settings.ShowSystemFiles())
                {
                    RefreshCurrentFolder();
                }
                return 0;
            }
            default:
                break;
            }
        }
        break;
    }

    // --------------------------------------------------------------
    // 自绘菜单项：量尺寸 + 绘制
    //
    // 只有「打开方式」「新建」两个子菜单在开关打开时才用 MF_OWNERDRAW，
    // 其余菜单项还是系统自己画。这两条消息是发给"菜单所有者"的（就是本窗口）。
    // --------------------------------------------------------------
    case WM_MEASUREITEM:
    {
        MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);

        if (measure != nullptr && measure->CtlType == ODT_MENU && measure->itemData != 0)
        {
            const MenuDrawItem* data =
                reinterpret_cast<const MenuDrawItem*>(measure->itemData);

            // 高度取系统菜单项高度，保证和同一菜单里的普通项一样高
            measure->itemHeight = static_cast<UINT>(::GetSystemMetrics(SM_CYMENU));

            // 宽度：图标位 + 文字 + 右侧留白
            int textWidth = Scale(120);

            HDC dc = ::GetDC(m_hWnd);
            if (dc != nullptr)
            {
                HGDIOBJ oldFont = ::SelectObject(dc, m_hUiFont);

                SIZE textSize = {};
                if (::GetTextExtentPoint32W(dc, data->text.c_str(),
                                            static_cast<int>(data->text.size()), &textSize))
                {
                    textWidth = textSize.cx;
                }

                ::SelectObject(dc, oldFont);
                ::ReleaseDC(m_hWnd, dc);
            }

            measure->itemWidth = static_cast<UINT>(Scale(8) + Scale(16) + Scale(8) +
                                                  textWidth + Scale(24));

            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);

        if (draw != nullptr && draw->CtlType == ODT_MENU)
        {
            DrawMenuItem(draw);
            return TRUE;
        }
        break;
    }

    // --------------------------------------------------------------
    // 文件列表通知
    // --------------------------------------------------------------
    case WM_NOTIFY:
    {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (header == nullptr)
        {
            break;
        }
        if (header->hwndFrom == m_fileListView.Handle() ||
            header->hwndFrom == m_fileListView.HeaderHandle())
        {
            // 表头的通知（拖完列宽 HDN_ENDTRACK）是从表头控件发出来的，
            // 列表只是帮忙转发，所以两个句柄都要认，否则拖完列宽记不住。
            return m_fileListView.HandleNotify(lParam);
        }
        if (header->hwndFrom == m_navigationPane.Handle())
        {
            return m_navigationPane.HandleNotify(lParam);
        }
        break;
    }

    // --------------------------------------------------------------
    // 后台枚举完成
    // --------------------------------------------------------------
    case WM_APP_ENUM_DONE:
        OnEnumerationDone();
        return 0;

    // --------------------------------------------------------------
    // 后台文件操作完成（复制 / 移动 / 删除）
    // --------------------------------------------------------------
    case WM_APP_OPERATION_DONE:
    {
        HRESULT hr = static_cast<HRESULT>(static_cast<LONG_PTR>(wParam));

        if (SUCCEEDED(hr))
        {
            // 剪切成功：剪贴板清空，避免再粘一次；其他情况剪贴板保持不变
            if (m_clearClipboardWhenDone)
            {
                m_clipboardPaths.clear();
                m_clipboardIsCut = false;
                shellclip::Clear(m_hWnd);   // 和资源管理器一样：剪切粘贴完成后剪贴板就空了
            }

            Logger::Info(L"文件操作完成");

            // 记进撤销栈（操作成功才记，失败/取消不留记录）
            // 永久删除例外：文件已经绕过回收站，没法还原，记了也是假的撤销
            if (m_lastOperation == FileOperationKind::Delete && !m_lastOperationPermanent)
            {
                UndoRecord record;
                record.kind        = UndoKind::Delete;
                record.beforePaths = m_operationPaths;   // 原路径：撤销时按它从回收站还原
                PushUndoRecord(record);
            }
            else if (m_lastOperation == FileOperationKind::Move ||
                     m_lastOperation == FileOperationKind::Copy)
            {
                UndoRecord record;
                record.kind        = (m_lastOperation == FileOperationKind::Move) ? UndoKind::Move
                                                                                  : UndoKind::Copy;
                record.beforePaths = m_operationPaths;

                for (const std::wstring& source : m_operationPaths)
                {
                    // 操作完成后这一项在目标目录里；用户中途改名会导致找不到，
                    // 撤销时那种项就静默跳过，不影响其它项
                    record.afterPaths.push_back(
                        JoinDirectoryFile(m_operationTargetDirectory, FileNameOf(source)));
                }

                PushUndoRecord(record);
            }
        }
        else
        {
            // 失败（含用户在进度框点取消）：保留剪贴板，用户可以直接重试
            Logger::Error(L"文件操作失败：" + Logger::FormatHresult(hr));
        }

        m_clearClipboardWhenDone = false;

        // 粘贴完成后取消"被剪切"的淡化显示
        m_fileListView.SetCutPaths({});

        // ------------------------------------------------------------
        // 完成后怎么更新界面（原则：能少动就少动，不整表重画）
        //   删除 → 只把删掉的那几行摘掉
        //   移动 → 如果当前目录正好是源目录，把搬走的行摘掉
        //   其余（复制/移动完成）→ 重新枚举一次，但只重绘变化的部分
        // ------------------------------------------------------------
        if (SUCCEEDED(hr) && m_lastOperation == FileOperationKind::Delete)
        {
            for (const auto& path : m_operationPaths)
            {
                m_fileListView.RemoveItemByPath(path);
            }
            Logger::Info(L"删除后局部移除 " + std::to_wstring(m_operationPaths.size()) + L" 行（未重扫目录）");
            UpdateTitle();
            UpdateStatusBar();
        }
        else
        {
            // 移动：源目录里已经搬走的项，从列表上摘掉
            if (SUCCEEDED(hr) && m_lastOperation == FileOperationKind::Move)
            {
                std::wstring currentDirectory = m_history.Current();

                for (const auto& path : m_operationPaths)
                {
                    std::wstring parent = NavigationHistory::ParentPath(path);
                    if (_wcsicmp(parent.c_str(), currentDirectory.c_str()) == 0)
                    {
                        m_fileListView.RemoveItemByPath(path);
                    }
                }

                UpdateTitle();
                UpdateStatusBar();
            }

            // 复制/移动完成后重新枚举一次：
            // 请求"把新出现的东西选中"（资源管理器粘贴完也是新文件高亮）
            if (SUCCEEDED(hr))
            {
                m_selectNewAfterEnum = true;
            }

            RefreshCurrentFolder();
        }
        m_lastOperation = FileOperationKind::None;
        m_lastOperationPermanent = false;
        m_operationPaths.clear();
        return 0;
    }

    // --------------------------------------------------------------
    // 当前目录的内容变了（资源管理器里删了/搬走了文件，或者别的程序写了新文件）
    //
    // 这里不立刻刷新，而是排一个 250ms 的定时器：
    // 大批文件同时变化时通知会连成串，等它们安静下来再刷一次，只刷一遍。
    // --------------------------------------------------------------
    case WM_APP_FOLDER_CHANGED:
        ::SetTimer(m_hWnd, kFolderChangeTimerId, kFolderChangeDelayMs, nullptr);
        return 0;

    // --------------------------------------------------------------
    // 后台「新建」项枚举完成：结果已经进 m_shellNewItems，这里去预取图标
    // --------------------------------------------------------------
    case WM_APP_SHELLNEW_READY:
    {
        std::vector<shell::ShellNewItem> items;

        {
            std::lock_guard<std::mutex> lock(m_shellNewMutex);
            items = m_shellNewItems;
        }

        m_menuIcons.PrefetchNewItems(items);
        return 0;
    }

    // --------------------------------------------------------------
    // 定时器：目录变化防抖之后真正刷新
    // --------------------------------------------------------------
    case WM_TIMER:
        if (wParam == kFolderChangeTimerId)
        {
            ::KillTimer(m_hWnd, kFolderChangeTimerId);

            // 有文件操作正在进行：先不刷（操作结束那一刻会自己刷一次）
            if (m_operationRunning->load())
            {
                Logger::Info(L"目录有变化，但文件操作正在进行，等操作结束后再刷");
                return 0;
            }

            // 正在原地改名：不能刷，否则编辑框会被冲掉
            if (m_fileListView.IsEditingLabel())
            {
                Logger::Info(L"目录有变化，但正在改名，跳过本次自动刷新");
                return 0;
            }

            Logger::Info(L"检测到当前目录有变化，自动刷新（走差异更新，不整表重绘）");
            RefreshCurrentFolder();
            return 0;
        }
        break;

    // --------------------------------------------------------------
    // 关窗口
    // --------------------------------------------------------------
    case WM_CLOSE:
        // 这是最后一次机会把"关掉时的样子"记下来（窗口还没销毁，位置大小都取得到）
        SaveWindowState();
        break;   // 交给系统继续走默认流程（DestroyWindow）

    // --------------------------------------------------------------
    // 销毁
    // --------------------------------------------------------------
    case WM_DESTROY:
        UnregisterDropTargets();
        m_watcher.Stop();          // 停掉目录监视，否则线程会一直等在那里
        ::KillTimer(m_hWnd, kFolderChangeTimerId);
        m_enumToken.Cancel();
        if (m_enumThread.joinable())
        {
            m_enumThread.join();
        }
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

// ============================================================================
// 导航
// ============================================================================

void MainWindow::NavigateToPath(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }

    Logger::Info(L"导航到：" + path);
    m_history.NavigateTo(path);
    StartEnumeration(path);
}

// ----------------------------------------------------------------------------
// 记下"这个目录是从哪儿点进来的、叫什么名字"
// 只由列表里的双击调用。地址栏、后退、前进、左栏都不记，
// 这样"从主文件夹点进文档"和"从 C:\Users\user 点进 Documents"互不干扰。
// ----------------------------------------------------------------------------
void MainWindow::RecordNavigationOrigin(const std::wstring& path,
                                        const std::wstring& displayName,
                                        const std::wstring& parentPath)
{
    if (path.empty())
    {
        return;
    }

    // 表别无限长大：超过 300 条就清空重记（都是导航缓存，丢了也不影响功能）
    if (m_navOrigins.size() > 300)
    {
        m_navOrigins.clear();
    }

    // key 用统一小写，避免同一个目录因大小写不同记成两条
    std::wstring key = path;
    for (wchar_t& ch : key)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }

    NavigationOrigin origin;
    origin.displayName = displayName;

    // 没有明确指定"上一级"时（从列表里双击进来的那种），
    // 就用进来之前待着的那个目录。
    origin.parentPath = parentPath.empty() ? m_history.Current() : parentPath;

    m_navOrigins[key] = origin;
}

// ----------------------------------------------------------------------------
// 当前目录给人看的名字
//   有来源记录（从列表里双击进来的）→ 用记录的名字，」文档」就显示「文档」
//   没有记录（地址栏输入、左栏点击、后退到更早的目录…）→ 按路径推导
// ----------------------------------------------------------------------------
std::wstring MainWindow::DisplayNameFor(const std::wstring& path)
{
    std::wstring key = path;
    for (wchar_t& ch : key)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }

    auto it = m_navOrigins.find(key);
    if (it != m_navOrigins.end() && !it->second.displayName.empty())
    {
        return it->second.displayName;
    }

    return DisplayNameForPath(path);
}

void MainWindow::GoBack()
{
    if (!m_history.CanGoBack())
    {
        return;
    }
    StartEnumeration(m_history.GoBack());
}

void MainWindow::GoForward()
{
    if (!m_history.CanGoForward())
    {
        return;
    }
    StartEnumeration(m_history.GoForward());
}

void MainWindow::GoUp()
{
    std::wstring current = m_history.Current();
    if (current.empty())
    {
        return;
    }

    // 先看"这个目录是从哪儿进来的"：
    //   从「主文件夹」双击进「文档」，向上就回「主文件夹」，
    //   而不是它的真实父目录 C:\Users\user（系统资源管理器就是这么走的）。
    {
        std::wstring key = current;
        for (wchar_t& ch : key)
        {
            ch = static_cast<wchar_t>(::towlower(ch));
        }

        auto it = m_navOrigins.find(key);
        if (it != m_navOrigins.end() && !it->second.parentPath.empty())
        {
            Logger::Info(L"向上（按来源）：" + current + L" → " + it->second.parentPath);
            NavigateToPath(it->second.parentPath);
            return;
        }
    }

    // 没有来源记录时，上一级交给 Shell 命名空间自己算（和资源管理器那个「向上」按钮同一套逻辑）：
    //   下载 → user（用户主文件夹）→ 用户 → 本地磁盘 (C:) → 此电脑
    //   此电脑 → 顶层「桌面」（里面含此电脑、主文件夹、音乐、图片等）→ 再向上就没有了
    // 这里不能用 NavigationHistory::ParentPath 那种字符串截断：
    // 它对 "C:\" 只会原地返回，永远走不到「此电脑」和「桌面」这两层。
    std::wstring parent = shell::GetParentPath(current);
    if (parent.empty())
    {
        Logger::Info(L"向上：已经在命名空间顶层，路径=" + current);
        return;   // 已经在顶层（例如顶层「桌面」），什么都不做
    }

    Logger::Info(L"向上：" + current + L" → " + parent);
    NavigateToPath(parent);
}

// ============================================================================
// 枚举
// ============================================================================

void MainWindow::StartEnumeration(const std::wstring& path)
{
    m_enumToken.Cancel();
    if (m_enumThread.joinable())
    {
        m_enumThread.join();
    }

    // 等后台文件操作收尾（大文件正在复制时会等它结束，或者用户在进度框点了取消）
    if (m_operationThread.joinable())
    {
        m_operationThread.join();
    }
    m_enumToken.Reset();

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result      = nullptr;
        m_resultReady = false;
        m_enumPath    = path;
    }

    // 只有"换了目录"才把旧列表清掉；
    // 同一个目录刷新（F5、粘贴完成、增删之后）保留现有内容，
    // 等新结果到了再做局部更新。否则列表会先空一下再填满，看着就是整表刷新。
    std::wstring shownPath = m_fileListView.CurrentPath();
    bool         isSameDir = !shownPath.empty() && _wcsicmp(shownPath.c_str(), path.c_str()) == 0;

    if (!isSameDir)
    {
        m_fileListView.SetResult(nullptr);
    }

    // 地址栏立即反映新路径
    //
    // 三种写法：
    //   1) 虚拟命名空间（「此电脑」这种，路径是 ::{GUID}）：整条显示系统给的名字；
    //   2) 在系统文件夹（桌面 / 下载 / 文档 / 图片 / 音乐 / 视频）之下的目录：
    //      折叠成「下载 > 2025 > …」，第一段点着能回那个系统文件夹；
    //   3) 其它普通目录：完整的 C: > Users > … > 当前目录，每段都能点。
    //
    // 之前是"只要显示名和真实路径不一样就整条只写名字"，导致从列表里
    // 双击任何一个普通文件夹进去，地址栏都只剩一个名字、没有可点的上级路径。
    std::wstring displayPath = DisplayNameFor(path);

    const bool isShellNamespace = (path.compare(0, 2, L"::") == 0 ||
                                   path.compare(0, 6, L"shell:") == 0);

    if (isShellNamespace)
    {
        m_addressBar.SetPathWithLabel(path, displayPath, true);
    }
    else
    {
        std::wstring prefixPath;
        std::wstring prefixLabel;

        if (shell::KnownFolderAncestor(path, prefixPath, prefixLabel))
        {
            m_addressBar.SetPathWithPrefix(path, prefixPath, prefixLabel);
        }
        else
        {
            m_addressBar.SetPathWithLabel(path, displayPath, false);
        }
    }

    // 标题只在"真的换了目录"时改一次，而且这一次先不带项数：
    //   换目录 → 「文件管理器 - C:\」，等结果回来再补成「（14 项）」
    //   同目录刷新（F5 / 自动感知变化 / 粘贴完成 / 新建之后重扫）→ 标题一个字都不动
    // 之前每次刷新都会先写「（加载中…）」再写「（14 项）」，这两段文字来回切，
    // 看上去就是标题栏括号里一直在闪。
    if (!isSameDir)
    {
        m_lastTitle = std::wstring(kWindowTitleBase) + L" - " + displayPath;
        ::SetWindowTextW(m_hWnd, m_lastTitle.c_str());
    }

    // 状态栏同理：只有换目录时才切成「正在读取…」，
    // 同一目录刷新期间保持原来的「14 个项目」不动
    if (!isSameDir)
    {
        UpdateStatusBar();
    }

    UpdateNavigationState();

    HWND hwnd = m_hWnd;

    m_enumThread = std::thread([this, path, hwnd]()
    {
        auto startTime = std::chrono::steady_clock::now();

        auto enumResult = shell::EnumerateFolder(path, &m_enumToken);

        auto endTime = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_result      = std::make_shared<shell::EnumerateResult>(std::move(enumResult));
            m_elapsedMs   = elapsedMs;
            m_resultReady = true;
        }

        if (hwnd != nullptr)
        {
            ::PostMessageW(hwnd, WM_APP_ENUM_DONE, 0, 0);
        }
    });
}

// ============================================================================
// 刷新当前目录
// ============================================================================

void MainWindow::RefreshCurrentFolder()
{
    std::wstring current = m_history.Current();
    if (current.empty())
    {
        return;
    }
    StartEnumeration(current);
}

// ============================================================================
// 文件操作（阶段 4）
// ============================================================================

void MainWindow::DoCopy()
{
    std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

    // 控制面板、"此电脑"里的控制项这类虚拟对象（没有真实文件系统路径）
    // 不能复制：复制它们没有意义（粘不出东西），资源管理器里也是禁用的。
    selected.erase(std::remove_if(selected.begin(), selected.end(),
                                  [](const std::wstring& path)
                                  {
                                      return !shell::IsFileSystemPath(path);
                                  }),
                   selected.end());

    if (selected.empty())
    {
        return;
    }

    m_clipboardPaths = selected;
    m_clipboardIsCut = false;

    // 复制：列表里的行保持正常显示（只有剪切才变淡）
    m_fileListView.SetCutPaths({});

    // 写进系统剪贴板：这样在资源管理器、桌面里也能直接粘贴
    if (!shellclip::WriteFiles(selected, false, m_hWnd))
    {
        Logger::Warn(L"写入系统剪贴板失败（可能被别的程序占用）");
        return;
    }

    Logger::Info(L"已复制 " + std::to_wstring(selected.size()) + L" 项（已写入系统剪贴板）");
}

// ---------------------------------------------------------------------------
// 复制文件路径（右键菜单 + Ctrl+Shift+C）
//
// 和资源管理器一致：多条的话一行一条，整段文字进系统剪贴板，
// 粘到记事本、聊天窗口、命令行里都能直接用。
// ---------------------------------------------------------------------------
void MainWindow::DoCopyPath()
{
    const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
    if (selected.empty())
    {
        return;
    }

    std::wstring text;

    for (const auto& path : selected)
    {
        if (!text.empty())
        {
            text += L"\r\n";
        }
        text += path;
    }

    if (!shellclip::WriteText(text, m_hWnd))
    {
        Logger::Warn(L"复制文件路径失败（剪贴板可能被别的程序占用）");
        return;
    }

    Logger::Info(L"已复制 " + std::to_wstring(selected.size()) + L" 条文件路径到剪贴板");
}

void MainWindow::DoCut()
{
    // 压缩包内部：暂不支持剪切
    //
    // 容器里的项是虚拟项，剪贴板里放 CF_HDROP（真实路径清单）对被剪的那一项
    // 无效，粘贴端只会失败。要支持得把剪贴板改成 IDList 通道，这一步还在做，
    // 所以这里先挡住快捷键，避免"剪了粘不动"的假成功。
    if (m_currentIsContainer)
    {
        Logger::Info(L"剪切：当前在压缩包内部，暂不支持");
        return;
    }

    std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

    // 这里要踢掉两类项：
    //   1. 驱动器（D:\ 这种盘符根）—— 资源管理器里驱动器不允许剪切（搬不走整盘）；
    //   2. 控制面板等虚拟对象（没有真实文件系统路径）—— 既不能复制也不能剪切。
    // 压缩包文件本身不在此列：和系统一致，zip/rar 文件都能剪。
    const size_t beforeCount = selected.size();
    selected.erase(std::remove_if(selected.begin(), selected.end(),
                                  [](const std::wstring& path)
                                  {
                                      return !shell::IsFileSystemPath(path) ||
                                             shell::IsDriveRootPath(path);
                                  }),
                   selected.end());

    if (selected.size() != beforeCount)
    {
        Logger::Info(L"剪切：跳过 " + std::to_wstring(beforeCount - selected.size()) +
                     L" 项（驱动器 / 虚拟对象）");
    }

    if (selected.empty())
    {
        return;
    }

    m_clipboardPaths = selected;
    m_clipboardIsCut = true;

    // 剪切：被剪切的这些行显示成半透明灰（和资源管理器一致），
    // 提示用户"这些已经被拿走了，还没落地"
    m_fileListView.SetCutPaths(selected);

    // 剪切要多写一个"首选效果 = 移动"，接收方才知道粘完要删源
    if (!shellclip::WriteFiles(selected, true, m_hWnd))
    {
        Logger::Warn(L"写入系统剪贴板失败（可能被别的程序占用）");
        return;
    }

    Logger::Info(L"已剪切 " + std::to_wstring(selected.size()) + L" 项（已写入系统剪贴板）");
}

void MainWindow::DoPaste()
{
    std::wstring targetDirectory = m_history.Current();
    if (targetDirectory.empty())
    {
        return;
    }

    // ------------------------------------------------------------------
    // 只读压缩包内部（rar / 7z / tar）：粘贴一定写不进去
    //
    // WinRAR 提供的这类处理器只读（实测四种"外部→内部"只有 zip 成功），
    // 按定好的做法：Ctrl+V 按了什么都不做，不弹窗、不写状态栏，
    // 只留一条日志方便排查。菜单里也不会出现「粘贴」（见 BuildBackgroundMenu）。
    // ------------------------------------------------------------------
    if (!shell::IsWritableTargetDirectory(targetDirectory))
    {
        Logger::Info(L"粘贴：目标是只读压缩包内部，忽略本次粘贴（" + targetDirectory + L"）");
        return;
    }

    // 每次都从系统剪贴板读，而不是用程序自己记的：
    // 这样从资源管理器、桌面复制过来的东西也能粘进来
    std::vector<std::wstring> paths;
    bool                      isCut = false;

    if (!shellclip::ReadFiles(paths, isCut, m_hWnd))
    {
        Logger::Warn(L"剪贴板里没有可粘贴的文件");
        return;
    }

    HWND owner = m_hWnd;

    // 剪切的话，等操作真的成功了再清空剪贴板（失败还能重试）
    m_clearClipboardWhenDone = isCut;

    m_lastOperation  = isCut ? FileOperationKind::Move : FileOperationKind::Copy;
    m_operationPaths = paths;
    m_operationTargetDirectory = targetDirectory;   // 撤销时要按这个目录算"粘贴到哪了"

    Logger::Info(L"开始粘贴 " + std::to_wstring(paths.size()) + L" 项到 " + targetDirectory +
                 (isCut ? L"（移动）" : L"（复制）"));

    RunFileOperationAsync([paths, targetDirectory, owner, isCut]() -> HRESULT
    {
        return isCut
               ? shellops::MoveItems(paths, targetDirectory, owner)
               : shellops::CopyItems(paths, targetDirectory, owner);
    });
}

// ============================================================================
// 批量重命名（右键「批量重命名」）
//
// 改名的"落盘"和"失败回滚"都在 BatchRenameDialog 里做完了，这里只负责两件事：
//   1. 全部成功之后把列表局部更新（不重扫目录），并选中改完名的那几项；
//   2. 把整批改名压成一条撤销记录 —— 一次 Ctrl+Z 就把整批改回原名。
// ============================================================================
void MainWindow::DoBatchRename()
{
    const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

    if (selected.size() < 2)
    {
        return;
    }

    // 只认文件系统里的真实文件/文件夹：控制面板、此电脑里的项没法改名
    std::vector<std::wstring> paths;
    paths.reserve(selected.size());

    for (const auto& path : selected)
    {
        if (shell::IsFileSystemPath(path))
        {
            paths.push_back(path);
        }
    }

    if (paths.size() < 2)
    {
        return;
    }

    BatchRenameDialog dialog;
    std::vector<BatchRenameResult> results;

    // 返回 true 只代表"整批都改成功了"；中途失败时窗口会自己留着提示，
    // 走的是 false 这条路（文件一个都没变，这里也不用做什么）
    if (!dialog.ShowModal(m_hWnd, paths, results))
    {
        return;
    }

    if (results.empty())
    {
        return;
    }

    Logger::Info(L"批量重命名：已改名 " + std::to_wstring(results.size()) + L" 项");

    // ---- 整批压一条撤销记录 ----
    // 用户拍板：批量重命名进撤销栈，右键菜单「撤销 批量重命名」能撤整批，
    // 但 Ctrl+Z / Ctrl+Y 不响应它（靠 batchRename 这个标记拦，见 PerformUndo）。
    // results 里只有**真正改过名**的项（名字没变的、被跳过异常的都不在里面），
    // 所以撤销时不会去动没改过的文件，也不会出现"原名和目标名相同"。
    {
        UndoRecord record;
        record.kind        = UndoKind::Rename;
        record.batchRename = true;

        record.beforePaths.reserve(results.size());
        record.afterPaths.reserve(results.size());

        for (const auto& rename : results)
        {
            record.beforePaths.push_back(rename.oldPath);
            record.afterPaths.push_back(
                JoinDirectoryFile(NavigationHistory::ParentPath(rename.oldPath), rename.newName));
        }

        PushUndoRecord(record);
    }

    // ---- 局部更新：只换名字和路径，再按当前的排序规则重排，不重扫目录 ----
    std::shared_ptr<shell::EnumerateResult> result = CurrentResult();

    if (result != nullptr)
    {
        std::vector<std::wstring> renamedPaths;
        renamedPaths.reserve(results.size());

        for (const auto& rename : results)
        {
            const std::wstring newPath =
                JoinDirectoryFile(NavigationHistory::ParentPath(rename.oldPath), rename.newName);

            for (auto& entry : result->entries)
            {
                if (_wcsicmp(entry.fullPath.c_str(), rename.oldPath.c_str()) == 0)
                {
                    entry.name     = rename.newName;
                    entry.fullPath = newPath;
                    renamedPaths.push_back(newPath);
                    break;
                }
            }
        }

        SortEntries();
        m_fileListView.SetSortState(m_sortColumn, m_sortAscending);
        m_fileListView.SetResult(result);

        // 改完名还把这批项选着，方便接着操作（也能一眼看出是它们变了）
        if (!renamedPaths.empty())
        {
            m_fileListView.SelectPaths(renamedPaths);
            m_fileListView.EnsureVisiblePath(renamedPaths.front());
        }

        UpdateTitle();
        UpdateStatusBar();
    }
}

// 按路径把当前枚举结果里对应条目的绝对 PIDL 取出来（压缩包内部操作用）
// 返回 false = 有哪一项找不到 PIDL（这时宁可不做，也不要退化成按路径操作）
bool MainWindow::SelectedEntryPidls(const std::vector<std::wstring>& paths,
                                    std::vector<std::vector<unsigned char>>& pidls)
{
    std::shared_ptr<shell::EnumerateResult> result = CurrentResult();

    if (result == nullptr)
    {
        return false;
    }

    pidls.clear();
    pidls.reserve(paths.size());

    for (const std::wstring& path : paths)
    {
        bool found = false;

        for (const shell::ShellEntry& entry : result->entries)
        {
            if (::_wcsicmp(entry.fullPath.c_str(), path.c_str()) == 0)
            {
                if (entry.pidl.empty())
                {
                    return false;
                }

                pidls.push_back(entry.pidl);
                found = true;
                break;
            }
        }

        if (!found)
        {
            return false;
        }
    }

    return !pidls.empty();
}

void MainWindow::DoDelete(bool permanent)
{
    // 除 zip 以外的压缩包内部是只读的：系统里删不动，我们菜单也不给这项，
    // 这里兜一层，防止 Delete 快捷键绕过菜单。
    if (m_currentIsContainer && !m_currentIsZipContainer)
    {
        Logger::Info(L"删除：当前是只读压缩包内部，忽略");
        return;
    }

    std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
    if (selected.empty())
    {
        return;
    }

    HWND owner = m_hWnd;

    // ------------------------------------------------------------
    // 压缩包内部：暂不执行删除
    //
    // 容器里的项磁盘上不存在，路径字符串交给 Shell 解析不出来（实测报
    // "参数错误"）。真正可行的 PIDL 删除通道还在做，这里先挡住，
    // 避免出现"点了没反应/报错"，更重要的是避免误删整个压缩包。
    // ------------------------------------------------------------
    if (m_currentIsContainer)
    {
        Logger::Info(L"删除：压缩包内部项暂不支持，已忽略");
        return;
    }

    // 记下删的是哪些，成功之后直接从列表里摘掉这几行，不用重扫目录
    m_lastOperation  = FileOperationKind::Delete;
    m_operationPaths = selected;
    // 永久删除不进撤销栈（文件已经不在回收站里，撤销也还原不了）
    m_lastOperationPermanent = permanent;

    Logger::Info(std::wstring(permanent ? L"开始永久删除 " : L"开始删除 ") +
                 std::to_wstring(selected.size()) + L" 项");

    RunFileOperationAsync([selected, owner, permanent]() -> HRESULT
    {
        // permanent = true：不进回收站，Shell 会弹"确实要永久删除"确认框
        if (permanent)
        {
            return shellops::DeleteItemsPermanently(selected, owner);
        }

        return shellops::DeleteItems(selected, owner);
    });
}

// ============================================================================
// 后台执行文件操作
//
// 为什么要放后台：
//   复制 1G 文件要几秒钟，如果直接在界面线程里等，整个窗口就卡住不能点。
//   交给一条单独线程去等，界面线程照常处理消息，用户能继续浏览别的目录。
//   操作结束（成功或用户点取消）后，线程往主窗口发一条消息，主窗口刷新列表。
// ============================================================================

void MainWindow::RunFileOperationAsync(std::function<HRESULT()> operation)
{
    // 同一时间只允许一个文件操作：两个操作同时弹进度框会互相打架
    if (m_operationRunning->load())
    {
        Logger::Warn(L"上一个文件操作还没结束，本次请求忽略");
        return;
    }

    // 上一次的线程已经结束了，这里只是把它回收掉
    if (m_operationThread.joinable())
    {
        m_operationThread.join();
    }

    m_operationRunning->store(true);

    HWND                               notifyWindow = m_hWnd;
    std::shared_ptr<std::atomic<bool>> running      = m_operationRunning;

    m_operationThread = std::thread([operation, notifyWindow, running]()
    {
        // 后台线程要自己初始化 COM（每个线程独立，不能借用主线程的）
        HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        HRESULT result = E_FAIL;

        if (SUCCEEDED(initHr) || initHr == RPC_E_CHANGED_MODE)
        {
            result = operation();
        }
        else
        {
            Logger::Error(L"后台线程 COM 初始化失败：" + Logger::FormatHresult(initHr));
        }

        if (SUCCEEDED(initHr))
        {
            ::CoUninitialize();
        }

        running->store(false);

        // 把结果带回界面线程；窗口若已关闭，这条消息会被系统丢弃，不会出问题
        ::PostMessageW(notifyWindow, WM_APP_OPERATION_DONE, static_cast<WPARAM>(result), 0);
    });
}

// ============================================================================
// 右键菜单
//
// 资源管理器的右键菜单是按"点在什么上"重新组合的，不是一套菜单加灰。
// 这里同样分场景：
//
//   Background    文件夹空白处：查看 / 粘贴 / 新建文件夹 / 新建 / 刷新 / 属性
//   SingleFile    单选一个文件：打开 / 打开方式 / 剪切 / 复制 / 删除 / 重命名 / 属性
//   SingleFolder  单选一个文件夹：打开 / 在新窗口中打开 / 剪切 / 复制 / 删除 / 重命名 / 属性
//   Multiple      选中多个：打开 / 剪切 / 复制 / 删除 / 属性
//   ShellItem     盘符 / 此电脑 / 控制面板：打开 / 在新窗口中打开 / 属性
//   None          组标题 / 分隔行：不弹菜单
//
// 场景判定只用已经加载在内存里的条目数据（isFolder / 路径前缀 / 选中数量），
// 不读注册表、不调 COM、不探测文件 —— 右键菜单依然是零成本。
// ============================================================================

void MainWindow::ShowContextMenu(POINT screenPoint)
{
    // 每次弹菜单前把上一次的自绘项数据清掉（下面的 owner-draw 项要用到它们）
    m_menuDrawItems.clear();
    m_menuPrimaryIsContainerFile = false;

    // 这次右键是"按住 Shift 点的"吗？
    // 是的话「删除」执行永久删除（不进回收站），菜单文字保持和资源管理器一样（仍是「删除」），
    // 只是点下去之后系统会弹"确实要永久删除"的确认框。
    m_menuPermanentDelete = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0);

    const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

    // ---- 1. 判定场景 ----
    ContextScene scene         = ContextScene::Background;
    std::wstring primaryPath;       // 单选时的那一项
    bool         primaryIsFolder = false;

    if (!selected.empty())
    {
        primaryPath = selected.front();

        const bool isShellNamespace = (primaryPath.compare(0, 2, L"::") == 0) ||
                                      (primaryPath.compare(0, 6, L"shell:") == 0);

        // 条目的元数据（是不是文件夹、是不是组标题）都从当前枚举结果里取
        std::shared_ptr<shell::EnumerateResult> result = CurrentResult();

        bool isGroupHeader = false;
        bool isFolder      = false;
        bool isContainerFile = false;

        if (result != nullptr)
        {
            const int index = ListView_GetNextItem(m_fileListView.Handle(), -1, LVNI_SELECTED);
            if (index >= 0 && index < static_cast<int>(result->entries.size()))
            {
                const shell::ShellEntry& entry = result->entries[index];
                isGroupHeader = entry.isGroupHeader || entry.isSpacer;
                isFolder      = entry.isFolder;
                isContainerFile = entry.isContainerFile;
            }
        }

        primaryIsFolder = isFolder;

        // 记住"选中的是不是压缩包文件"，拼单选文件菜单时要靠它决定
        // 要不要多给一条「在新窗口中打开」（和文件夹一致）
        m_menuPrimaryIsContainerFile = isContainerFile;

        if (isGroupHeader)
        {
            scene = ContextScene::None;            // 组标题、分隔行：不弹菜单
        }
        else if (selected.size() >= 2)
        {
            scene = ContextScene::Multiple;
        }
        else if (isShellNamespace || (isFolder && ::PathIsRootW(primaryPath.c_str())))
        {
            // 盘符、此电脑、控制面板这些：不给删除/重命名（删盘符是灾难）
            scene = ContextScene::ShellItem;
        }
        else if (isFolder)
        {
            scene = ContextScene::SingleFolder;
        }
        else
        {
            scene = ContextScene::SingleFile;
        }
    }

    if (scene == ContextScene::None)
    {
        return;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    // ---- 2. 按场景拼菜单 ----
    switch (scene)
    {
    case ContextScene::Background:
        BuildBackgroundMenu(menu);
        break;

    case ContextScene::SingleFile:
        BuildSingleFileMenu(menu, primaryPath);
        break;

    case ContextScene::SingleFolder:
        BuildSingleFolderMenu(menu, primaryPath);
        break;

    case ContextScene::Multiple:
        BuildMultipleMenu(menu);
        break;

    case ContextScene::ShellItem:
        BuildShellItemMenu(menu, primaryPath);
        break;

    default:
        break;
    }

    // TPM_RETURNCMD：菜单自己跑完并返回点了哪一项，省去额外的一层消息往返
    int command = ::TrackPopupMenu(menu,
                                   TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                                   screenPoint.x, screenPoint.y,
                                   0, m_hWnd, nullptr);

    ::DestroyMenu(menu);

    // 菜单已经关了，自绘项的数据可以放掉（下次弹菜单前还会再清一次，双保险）
    m_menuDrawItems.clear();

    if (command != 0)
    {
        ::PostMessageW(m_hWnd, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(command), 0), 0);
    }
}

// ---------------------------------------------------------------------------
// 文件夹空白处
//
// 分组按用户要求：
//   查看 >
//   刷新            （和「查看」同一组，紧在查看下面）
//   ─────
//   粘贴            （剪贴板里有文件才出现）
//   撤销 / 恢复      （紧跟在「粘贴」那一格的下一格，有才出现）
//   ─────
//   新建文件夹
//   新建 >
//   ─────
//   属性
// ---------------------------------------------------------------------------
void MainWindow::BuildBackgroundMenu(HMENU menu)
{
    FillViewMenu(menu);
    ::AppendMenuW(menu, MF_STRING, kCommandRefresh, L"刷新(&R)");

    // 只读压缩包内部（rar / 7z / tar）不显示「粘贴」：
    // 那里根本写不进去，出现了只会让用户点一次、白等一次。
    const bool pasteAllowed = !(m_currentIsContainer && !m_currentIsZipContainer);

    if (pasteAllowed && shellclip::HasFiles())
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kCommandPaste, L"粘贴(&P)");
    }

    // 撤销 / 恢复 紧跟在「粘贴」的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    FillNewFolderAndNewMenu(menu);

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 单选一个文件
//
//   打开
//   打开方式 >      （这个扩展名在账本里有程序时才出现）
//   ─────
//   剪切
//   复制
//   复制文件路径        （只把路径文字放进剪贴板，Ctrl+Shift+C 也能触发）
//   ─────
//   撤销 / 恢复      （紧跟「复制」的下一格，有才出现）
//   ─────
//   删除
//   ─────
//   重命名
//   ─────
//   属性
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 「复制 / 剪切」能不能用
//
//   · 驱动器（D:\ 这种盘符根）不能剪切 —— 整盘搬不走，资源管理器里也是灰的；
//   · 控制面板、"此电脑"里的控制项这类虚拟对象（没有真实文件系统路径）
//     既不能复制也不能剪切。
// 压缩包文件本身和普通文件一样：zip/rar 都能剪（用户实测系统就是这样，之前
// 按"除 zip 外禁剪"是记错了，这里已改回）。
// 这些判断只查字符串，零成本（符合"右键菜单弹出不查注册表、不碰 COM"的要求）。
// ---------------------------------------------------------------------------
static bool CanCopyPath(const std::wstring& path)
{
    return shell::IsFileSystemPath(path);
}

static bool CanCutPath(const std::wstring& path)
{
    // 两条"不能剪"：驱动器、虚拟对象
    return shell::IsFileSystemPath(path) &&
           !shell::IsDriveRootPath(path);
}

// ---------------------------------------------------------------------------
// 压缩包内部的右键菜单（在 zip / rar / 7z 里选中文件时走这里）
//
// 系统实际给的项（用户实测资源管理器）：
//   · zip 内部      : 打开 / 剪切 / 复制 / 删除 / 属性
//   · 其它压缩包内部 : 打开 / 复制
//
// 但"剪切 / 删除 / 属性"要真能干活，必须走容器内项的 PIDL 通道：
// 容器里的项没有真实磁盘路径，路径字符串交给 Shell 解析不出来（实测会报
// "参数错误"，还有过把整个压缩包误删的先例）。这条通道还在做，
// 所以这里先只给「打开 / 复制」——宁可少给，也不给一个点了会出错/出事的项。
// 通道接好后再把 zip 的 剪切/删除/属性 补回来。
// ---------------------------------------------------------------------------
void MainWindow::BuildContainerItemMenu(HMENU menu)
{
    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandCopy, L"复制(&C)");
}

void MainWindow::BuildSingleFileMenu(HMENU menu, const std::wstring& filePath)
{
    // 压缩包内部：按容器能力单独给菜单，早退
    if (m_currentIsContainer)
    {
        BuildContainerItemMenu(menu);
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");

    // 压缩包文件本身和文件夹一样，也能"在新窗口中打开"（Ctrl+双击同理）
    if (m_menuPrimaryIsContainerFile)
    {
        ::AppendMenuW(menu, MF_STRING, kCommandOpenInNewWindow, L"在新窗口中打开(&N)");
    }

    // 打开方式：只给"普通文件"；菜单内容全部来自内存里的账本，零成本
    {
        const std::wstring extension = shell::ExtensionKeyForPath(filePath);

        if (!extension.empty() && shell::IsExtensionHandled(extension))
        {
            FillOpenWithMenu(menu, extension);
        }
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (CanCutPath(filePath) ? 0u : MF_GRAYED), kCommandCut, L"剪切(&T)");
    ::AppendMenuW(menu, MF_STRING | (CanCopyPath(filePath) ? 0u : MF_GRAYED), kCommandCopy, L"复制(&C)");
    ::AppendMenuW(menu, MF_STRING, kCommandCopyPath, L"复制文件路径(&A)");

    // 撤销 / 恢复 紧跟在「复制」那一格的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    // 按住 Shift 弹的菜单：「删除」改为永久删除（文字不变，行为变，和资源管理器一致）
    ::AppendMenuW(menu, MF_STRING,
                  m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete,
                  L"删除(&D)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandRename, L"重命名(&M)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 单选一个文件夹
//
//   打开
//   在新窗口中打开
//   ─────
//   剪切
//   复制
//   复制文件路径        （只把路径文字放进剪贴板，Ctrl+Shift+C 也能触发）
//   ─────
//   撤销 / 恢复      （紧跟「复制」的下一格，有才出现）
//   ─────
//   删除
//   ─────
//   重命名
//   ─────
//   属性
// ---------------------------------------------------------------------------
void MainWindow::BuildSingleFolderMenu(HMENU menu, const std::wstring& folderPath)
{
    // 压缩包内部的子目录：按容器能力给菜单
    // （zip 里能进去、能剪能删；rar/7z 里只能「打开 / 复制」，和系统一致）
    if (m_currentIsContainer)
    {
        BuildContainerItemMenu(menu);
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING, kCommandOpenInNewWindow, L"在新窗口中打开(&N)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (CanCutPath(folderPath) ? 0u : MF_GRAYED), kCommandCut, L"剪切(&T)");
    ::AppendMenuW(menu, MF_STRING | (CanCopyPath(folderPath) ? 0u : MF_GRAYED), kCommandCopy, L"复制(&C)");
    ::AppendMenuW(menu, MF_STRING, kCommandCopyPath, L"复制文件路径(&A)");

    // 撤销 / 恢复 紧跟在「复制」那一格的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    // 按住 Shift 弹的菜单：「删除」改为永久删除（文字不变，行为变，和资源管理器一致）
    ::AppendMenuW(menu, MF_STRING,
                  m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete,
                  L"删除(&D)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandRename, L"重命名(&M)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 选中多个
//
//   打开
//   ─────
//   剪切
//   复制
//   复制文件路径        （多选时一行一条路径，Ctrl+Shift+C 也能触发）
//   ─────
//   撤销 / 恢复      （紧跟「复制」的下一格，有才出现）
//   ─────
//   删除
//   ─────
//   批量重命名          （多个一起改名，系统的「重命名」对多选只改一个，这里给它更全的）
//   ─────
//   属性
//
// 没有单个「重命名」（系统也不给多选逐个改名），没有新建，没有打开方式。
// ---------------------------------------------------------------------------
void MainWindow::BuildMultipleMenu(HMENU menu)
{
    // 压缩包内部多选：和单选文件一样按容器能力给项，早退
    if (m_currentIsContainer)
    {
        BuildContainerItemMenu(menu);
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // 多选：只要里面有一项不能复制/剪切（驱动器、控制面板等），整条就灰掉
    const std::vector<std::wstring> selectedPaths = m_fileListView.GetSelectedPaths();
    bool canCut  = !selectedPaths.empty();
    bool canCopy = canCut;

    for (const auto& path : selectedPaths)
    {
        canCut  = canCut  && CanCutPath(path);
        canCopy = canCopy && CanCopyPath(path);
    }

    ::AppendMenuW(menu, MF_STRING | (canCut ? 0u : MF_GRAYED), kCommandCut, L"剪切(&T)");
    ::AppendMenuW(menu, MF_STRING | (canCopy ? 0u : MF_GRAYED), kCommandCopy, L"复制(&C)");
    ::AppendMenuW(menu, MF_STRING, kCommandCopyPath, L"复制文件路径(&A)");

    // 撤销 / 恢复 紧跟在「复制」那一格的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    // 按住 Shift 弹的菜单：「删除」改为永久删除（文字不变，行为变，和资源管理器一致）
    ::AppendMenuW(menu, MF_STRING,
                  m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete,
                  L"删除(&D)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandBatchRename, L"批量重命名(&R)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 盘符 / 此电脑 / 控制面板 这类命名空间项
//
//   打开
//   在新窗口中打开
//   ─────
//   属性
// ---------------------------------------------------------------------------
void MainWindow::BuildShellItemMenu(HMENU menu, const std::wstring& /*itemPath*/)
{
    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING, kCommandOpenInNewWindow, L"在新窗口中打开(&N)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 「查看」子菜单（和资源管理器一样，当前视图打勾）
// ---------------------------------------------------------------------------
void MainWindow::FillViewMenu(HMENU parentMenu)
{
    HMENU viewMenu = ::CreatePopupMenu();
    if (viewMenu == nullptr)
    {
        return;
    }

    const FileListView::ViewMode current = m_fileListView.CurrentViewMode();

    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::LargeIcon) ? MF_CHECKED : 0),
                  kCommandViewLarge, L"大图标(&L)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::SmallIcon) ? MF_CHECKED : 0),
                  kCommandViewSmall, L"小图标(&S)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::List) ? MF_CHECKED : 0),
                  kCommandViewList, L"列表(&T)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::Details) ? MF_CHECKED : 0),
                  kCommandViewDetails, L"详细信息(&D)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::Tiles) ? MF_CHECKED : 0),
                  kCommandViewTiles, L"平铺(&I)");

    ::AppendMenuW(parentMenu, MF_STRING | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(viewMenu), L"查看(&V)");
}

// ---------------------------------------------------------------------------
// 「新建文件夹」+「新建」子菜单（两者相邻，新建文件夹在上面）
// ---------------------------------------------------------------------------
void MainWindow::FillNewFolderAndNewMenu(HMENU menu)
{
    const std::wstring currentPath = m_history.Current();
    const bool canCreate = !currentPath.empty() &&
                           currentPath.compare(0, 2, L"::") != 0 &&
                           currentPath.compare(0, 6, L"shell:") != 0;

    ::AppendMenuW(menu, MF_STRING | (canCreate ? 0 : MF_GRAYED),
                  kCommandNewFolder, L"新建文件夹(&N)");

    FillNewMenu(menu);
}

// ============================================================================
// 撤销（和资源管理器一样：单步，只记最近一次操作）
//
// 文案跟系统同步，显示成「撤销 删除」「撤销 重命名」「撤销 移动」
// 「撤销 复制」「撤销 新建」；没有可撤销的操作时整项不出现。
//
// 各种操作怎么反做：
//   新建 / 复制 → 把多出来的那几项删掉（进回收站，安全，删错还能再捡回来）
//   移动       → 从目标目录搬回原目录
//   重命名     → 改回原来的名字
//   删除       → 从回收站按原路径还原
// ============================================================================

// 把目录和文件名拼成完整路径（目录末尾补反斜杠）
static std::wstring JoinDirectoryFile(const std::wstring& directory, const std::wstring& fileName)
{
    if (directory.empty())
    {
        return fileName;
    }

    std::wstring result = directory;
    if (result.back() != L'\\' && result.back() != L'/')
    {
        result += L'\\';
    }
    result += fileName;
    return result;
}

// 从完整路径里取文件名
static std::wstring FileNameOf(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

// 超过 260 字符的路径，普通 Win32 调用会失败，得换成 \\?\ 写法。
// 批量改名那边也是这么处理的，撤销/重做要跟它一致，否则长路径的项改不回来。
static std::wstring MakeLongPath(const std::wstring& path)
{
    if (path.size() < 260 || path.size() >= 32767)
    {
        return path;
    }

    if (path.compare(0, 4, L"\\\\?\\") == 0)
    {
        return path;
    }

    if (path.size() > 1 && path[1] == L':')
    {
        return L"\\\\?\\" + path;                       // C:\...
    }

    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        return L"\\\\?\\UNC\\" + path.substr(2);        // \\server\share
    }

    return path;   // 相对路径之类，原样交给系统
}

// ---------------------------------------------------------------------------
// 从回收站还原若干项（按原路径匹配）
//
// 回收站里的每一项都有 System.Recycle.DeletedFrom 属性（它原来在哪个目录），
// 配合项的名字就能还原出完整原路径。匹配上就 MoveItem 搬回原目录。
//
// 这个属性键在 SDK 的 propkey.h 里没有定义（只在系统属性系统里登记），
// 所以运行时用 PSGetPropertyKeyFromName 按名字取，取不到就当成"不支持还原"。
//
// 找不到（用户清空过回收站、或者在同一目录里又删过同名文件）就什么都不做，
// 调用方刷新一下列表即可。
// ---------------------------------------------------------------------------
static PROPERTYKEY RecycleDeletedFromKey()
{
    static PROPERTYKEY key;
    static bool        resolved = false;

    if (!resolved)
    {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // 已初始化过也无所谓
        ::PSGetPropertyKeyFromName(L"System.Recycle.DeletedFrom", &key);
        resolved = true;
    }

    return key;
}

static HRESULT RestoreDeletedFromRecycleBin(const std::vector<std::wstring>& originalPaths, HWND owner)
{
    if (originalPaths.empty())
    {
        return E_INVALIDARG;
    }

    // ---- 1. 拿回收站这个虚拟文件夹 ----
    IShellItem* binItem = nullptr;
    HRESULT     hr      = ::SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr,
                                                 IID_IShellItem, reinterpret_cast<void**>(&binItem));
    if (FAILED(hr) || binItem == nullptr)
    {
        Logger::Warn(L"打开回收站失败：" + Logger::FormatHresult(hr));
        return hr;
    }

    IShellFolder* binFolder = nullptr;
    hr = binItem->BindToHandler(nullptr, BHID_SFObject, IID_IShellFolder,
                                reinterpret_cast<void**>(&binFolder));
    binItem->Release();

    if (FAILED(hr) || binFolder == nullptr)
    {
        Logger::Warn(L"取回收站文件夹失败：" + Logger::FormatHresult(hr));
        return hr;
    }

    IEnumIDList* idList  = nullptr;
    BOOL         matched = FALSE;
    hr = binFolder->EnumObjects(owner, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &idList);

    IFileOperation* operation = nullptr;
    if (SUCCEEDED(hr))
    {
        hr = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_IFileOperation, reinterpret_cast<void**>(&operation));
    }

    if (SUCCEEDED(hr) && operation != nullptr)
    {
        operation->SetOwnerWindow(owner);
        operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI);

        LPITEMIDLIST child = nullptr;
        while (idList->Next(1, &child, nullptr) == S_OK)
        {
            IShellItem2* item = nullptr;
            if (SUCCEEDED(::SHCreateItemWithParent(nullptr, binFolder, child,
                                                   IID_IShellItem2, reinterpret_cast<void**>(&item))))
            {
                LPWSTR deletedFrom = nullptr;

                if (SUCCEEDED(item->GetString(RecycleDeletedFromKey(), &deletedFrom)) && deletedFrom != nullptr)
                {
                    LPWSTR displayName = nullptr;

                    if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVE, &displayName)) &&
                        displayName != nullptr)
                    {
                        const std::wstring origin = JoinDirectoryFile(deletedFrom, displayName);

                        auto found = std::find_if(originalPaths.begin(), originalPaths.end(),
                                                  [&origin](const std::wstring& path)
                                                  {
                                                      return _wcsicmp(path.c_str(), origin.c_str()) == 0;
                                                  });

                        if (found != originalPaths.end())
                        {
                            // 目标目录要包成 IShellItem，IFileOperation 只认这个
                            IShellItem* destination = nullptr;

                            if (SUCCEEDED(::SHCreateItemFromParsingName(deletedFrom, nullptr,
                                                                        IID_IShellItem,
                                                                        reinterpret_cast<void**>(&destination))) &&
                                destination != nullptr)
                            {
                                // 搬回原目录 = 还原
                                operation->MoveItem(item, destination, nullptr, nullptr);
                                destination->Release();
                                matched = TRUE;
                            }
                        }

                        ::CoTaskMemFree(displayName);
                    }

                    ::CoTaskMemFree(deletedFrom);
                }

                item->Release();
            }

            ::CoTaskMemFree(child);
        }

        if (matched)
        {
            hr = operation->PerformOperations();
            Logger::Info(L"撤销删除：已从回收站还原，HRESULT=" + Logger::FormatHresult(hr));
        }
        else
        {
            Logger::Warn(L"撤销删除：回收站里找不到这些项（可能已被清空）");
        }

        operation->Release();
    }

    if (idList != nullptr)
    {
        idList->Release();
    }
    binFolder->Release();

    return hr;
}

// ---------------------------------------------------------------------------
// 撤销栈的维护
// ---------------------------------------------------------------------------
void MainWindow::PushUndoRecord(const UndoRecord& record)
{
    if (record.kind == UndoKind::None)
    {
        return;
    }

    m_undoStack.push_back(record);

    // 超出上限就把最老的一步挤掉
    if (m_undoStack.size() > kMaxUndoSteps)
    {
        m_undoStack.erase(m_undoStack.begin());
    }

    // 出现新操作后，原来"恢复"过的那些步骤就作废了（和所有编辑器一样）
    m_redoStack.clear();
}

void MainWindow::ClearUndoHistory()
{
    m_undoStack.clear();
    m_redoStack.clear();
}

// 把一步操作的动词转成菜单文案里的字（新建 / 重命名 / 移动 / 复制 / 删除）
const wchar_t* MainWindow::UndoVerbText(UndoKind kind)
{
    switch (kind)
    {
    case UndoKind::Create: return L"新建";
    case UndoKind::Rename: return L"重命名";
    case UndoKind::Move:   return L"移动";
    case UndoKind::Copy:   return L"复制";
    case UndoKind::Delete: return L"删除";
    default:               return nullptr;
    }
}

void MainWindow::FillUndoItem(HMENU menu)
{
    bool anyItem = false;

    // 「撤销 删除」：撤销栈顶那一步
    if (!m_undoStack.empty())
    {
        const UndoRecord& record = m_undoStack.back();

        // 批量重命名单独说一句「撤销 批量重命名」，让用户一眼看出这一条
        // 和单文件改名不是同一档（也就不会奇怪 Ctrl+Z 为什么不动它）
        const wchar_t* verb = record.batchRename ? L"批量重命名" : UndoVerbText(record.kind);

        if (verb != nullptr)
        {
            const std::wstring text = std::wstring(L"撤销 ") + verb + L"(&U)";
            ::AppendMenuW(menu, MF_STRING, kCommandUndo, text.c_str());
            anyItem = true;
        }
    }

    // 「恢复 删除」：重做栈顶那一步（撤销过、还没被新操作冲掉的那些）
    if (!m_redoStack.empty())
    {
        const UndoRecord& record = m_redoStack.back();

        const wchar_t* verb = record.batchRename ? L"批量重命名" : UndoVerbText(record.kind);

        if (verb != nullptr)
        {
            const std::wstring text = std::wstring(L"恢复 ") + verb + L"(&E)";
            ::AppendMenuW(menu, MF_STRING, kCommandRedo, text.c_str());
            anyItem = true;
        }
    }

    if (anyItem)
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
}

void MainWindow::PerformUndo(bool fromKeyboard)
{
    if (m_operationRunning != nullptr && m_operationRunning->load())
    {
        Logger::Warn(L"有文件操作正在进行，撤销先不执行");
        return;
    }

    if (m_undoStack.empty())
    {
        Logger::Info(L"撤销：没有可撤销的步骤");
        return;
    }

    // 批量重命名：只能在右键菜单里撤，Ctrl+Z 不响应（用户拍板）。
    // 这里是"整条不执行"，不是"跳过这条去撤上一条"——跳过去会让撤销和用户看到的
    // 操作顺序对不上，太容易误删。
    if (fromKeyboard && m_undoStack.back().batchRename)
    {
        Logger::Info(L"撤销：批量重命名不支持 Ctrl+Z，请用右键菜单里的「撤销 批量重命名」");
        return;
    }

    // 从栈顶那一 步开始反着做
    const UndoRecord record = m_undoStack.back();
    m_undoStack.pop_back();

    switch (record.kind)
    {
    case UndoKind::Create:
    case UndoKind::Copy:
    {
        // 把多出来的项删掉（进回收站，删错还能再捡回来）
        for (const std::wstring& path : record.afterPaths)
        {
            shellops::DeleteItems({ path }, m_hWnd);
        }
        Logger::Info(L"撤销：已删除 " + std::to_wstring(record.afterPaths.size()) + L" 项");

        // 「新建」撤销后没法重做（原来的文件内容已经没了），
        // 把重做栈清掉，免得 Ctrl+Y 恢复到一半
        if (record.kind == UndoKind::Create)
        {
            m_redoStack.clear();
        }
        else
        {
            m_redoStack.push_back(record);
        }
        break;
    }

    case UndoKind::Move:
    {
        // 从目标目录搬回原目录
        const size_t count = std::min(record.afterPaths.size(), record.beforePaths.size());
        for (size_t i = 0; i < count; ++i)
        {
            const std::wstring parent = NavigationHistory::ParentPath(record.beforePaths[i]);
            shellops::MoveItems({ record.afterPaths[i] }, parent, m_hWnd);
        }
        Logger::Info(L"撤销：已移回 " + std::to_wstring(count) + L" 项");
        m_redoStack.push_back(record);
        break;
    }

    case UndoKind::Rename:
    {
        if (record.batchRename)
        {
            // 批量改名：走两段式（一批文件同时互换名字时才不会自己撞自己）。
            // 不走 shellops::RenameItem：那个每项起一次 IFileOperation，
            // 几十上百项会很慢，而且这里不需要进回收站。
            std::vector<std::wstring> oldNames(record.beforePaths.size());

            for (size_t i = 0; i < record.beforePaths.size(); ++i)
            {
                oldNames[i] = FileNameOf(record.beforePaths[i]);
            }

            const int failures = RenameManyTwoStage(record.afterPaths, oldNames);

            Logger::Info(L"撤销：批量重命名已改回 " +
                         std::to_wstring(record.afterPaths.size() - static_cast<size_t>(failures)) +
                         L" 项" + (failures > 0 ? L"，" + std::to_wstring(failures) + L" 项失败" : L""));
            m_redoStack.push_back(record);
            break;
        }

        // 改回原来的名字
        const size_t count = std::min(record.afterPaths.size(), record.beforePaths.size());
        for (size_t i = 0; i < count; ++i)
        {
            shellops::RenameItem(record.afterPaths[i], FileNameOf(record.beforePaths[i]), m_hWnd);
        }
        Logger::Info(L"撤销：已改回 " + std::to_wstring(count) + L" 项的名字");
        m_redoStack.push_back(record);
        break;
    }

    case UndoKind::Delete:
        RestoreDeletedFromRecycleBin(record.beforePaths, m_hWnd);
        m_redoStack.push_back(record);
        break;

    default:
        return;
    }

    RefreshCurrentFolder();
}

// ---------------------------------------------------------------------------
// 恢复（重做）：把撤销过的那一步再正向做一遍
//
// 每类操作"正向"怎么做，和它撤销正好相反：
//   重命名 → 改成 afterPaths 里的新名字
//   移动   → 搬到 afterPaths 所在的目录
//   复制   → 复制到 afterPaths 所在的目录
//   删除   → 再删一次
//   （新建没有重做：撤销时原有的文件内容已经进回收站，重做不出同一个文件）
// ---------------------------------------------------------------------------
void MainWindow::PerformRedo(bool fromKeyboard)
{
    if (m_operationRunning != nullptr && m_operationRunning->load())
    {
        Logger::Warn(L"有文件操作正在进行，恢复先不执行");
        return;
    }

    if (m_redoStack.empty())
    {
        Logger::Info(L"恢复：没有可恢复的步骤");
        return;
    }

    // 和撤销对称：批量重命名的重做也只能走右键菜单，Ctrl+Y 不碰它
    if (fromKeyboard && m_redoStack.back().batchRename)
    {
        Logger::Info(L"恢复：批量重命名不支持 Ctrl+Y，请用右键菜单里的「恢复 批量重命名」");
        return;
    }

    const UndoRecord record = m_redoStack.back();
    m_redoStack.pop_back();

    switch (record.kind)
    {
    case UndoKind::Rename:
    {
        if (record.batchRename)
        {
            // 正向再改一遍：原来的名字 → 批量改出来的名字（同样两段式）
            std::vector<std::wstring> newNames(record.afterPaths.size());

            for (size_t i = 0; i < record.afterPaths.size(); ++i)
            {
                newNames[i] = FileNameOf(record.afterPaths[i]);
            }

            const int failures = RenameManyTwoStage(record.beforePaths, newNames);

            Logger::Info(L"恢复：批量重命名已重做 " +
                         std::to_wstring(record.afterPaths.size() - static_cast<size_t>(failures)) +
                         L" 项" + (failures > 0 ? L"，" + std::to_wstring(failures) + L" 项失败" : L""));
            m_undoStack.push_back(record);
            break;
        }

        const size_t count = std::min(record.afterPaths.size(), record.beforePaths.size());
        for (size_t i = 0; i < count; ++i)
        {
            shellops::RenameItem(record.beforePaths[i], FileNameOf(record.afterPaths[i]), m_hWnd);
        }
        Logger::Info(L"恢复：已改回新名字 " + std::to_wstring(count) + L" 项");
        m_undoStack.push_back(record);
        break;
    }

    case UndoKind::Move:
    {
        const size_t count = std::min(record.afterPaths.size(), record.beforePaths.size());
        for (size_t i = 0; i < count; ++i)
        {
            shellops::MoveItems({ record.beforePaths[i] },
                                NavigationHistory::ParentPath(record.afterPaths[i]), m_hWnd);
        }
        Logger::Info(L"恢复：已再移动 " + std::to_wstring(count) + L" 项");
        m_undoStack.push_back(record);
        break;
    }

    case UndoKind::Copy:
    {
        const size_t count = std::min(record.afterPaths.size(), record.beforePaths.size());
        for (size_t i = 0; i < count; ++i)
        {
            shellops::CopyItems({ record.beforePaths[i] },
                                NavigationHistory::ParentPath(record.afterPaths[i]), m_hWnd);
        }
        Logger::Info(L"恢复：已再复制 " + std::to_wstring(count) + L" 项");
        m_undoStack.push_back(record);
        break;
    }

    case UndoKind::Delete:
        for (const std::wstring& path : record.beforePaths)
        {
            shellops::DeleteItems({ path }, m_hWnd);
        }
        Logger::Info(L"恢复：已再删除 " + std::to_wstring(record.beforePaths.size()) + L" 项");
        m_undoStack.push_back(record);
        break;

    default:
        return;
    }

    RefreshCurrentFolder();
}

// ---------------------------------------------------------------------------
// 批量改名的撤销 / 重做：两段式批量改名
//
//   fromPaths 里的项 → 各自先挪到同目录下的临时名 → 再改成 toNames 里的名字
//
// 为什么要两段：批量改名经常出现 a→b、b→a 这种互换，直接改会撞上"目标名已存在"。
// 为什么不走 shellops::RenameItem：那个每项起一次 IFileOperation，几十上百项太慢，
// 而且撤销批量改名不需要进回收站。
// 失败项尽量回退到"改之前的名字"，实在回不去就留在临时名上（日志里有记录，文件不会丢）。
// ---------------------------------------------------------------------------
int MainWindow::RenameManyTwoStage(const std::vector<std::wstring>& fromPaths,
                                   const std::vector<std::wstring>& toNames)
{
    const size_t count = std::min(fromPaths.size(), toNames.size());

    if (count == 0)
    {
        return 0;
    }

    std::vector<std::wstring> tempPaths(count, std::wstring());
    const ULONGLONG tick = ::GetTickCount64();
    int failures = 0;

    // 第一段：原名 → 临时名（temp 带下标 + tick，同一个目录里不会撞；只保留扩展名）
    for (size_t i = 0; i < count; ++i)
    {
        const std::wstring parent = NavigationHistory::ParentPath(fromPaths[i]);
        const std::wstring name   = FileNameOf(fromPaths[i]);

        std::wstring extension;
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < name.size())
        {
            extension = name.substr(dot);   // 含那个点
        }

        const std::wstring temp = JoinDirectoryFile(
            parent,
            L"~urex_" + std::to_wstring(i) + L"_" + std::to_wstring(tick % 100000) + extension);

        if (::MoveFileExW(MakeLongPath(fromPaths[i]).c_str(),
                          MakeLongPath(temp).c_str(), 0))
        {
            tempPaths[i] = temp;
        }
        else
        {
            ++failures;   // 这一项没挪走，第二段跳过它
        }
    }

    // 第二段：临时名 → 目标名；失败就尽量挪回原名，别留一个 ~urex_ 名字
    for (size_t i = 0; i < count; ++i)
    {
        if (tempPaths[i].empty())
        {
            continue;
        }

        const std::wstring target =
            JoinDirectoryFile(NavigationHistory::ParentPath(fromPaths[i]), toNames[i]);

        if (::MoveFileExW(MakeLongPath(tempPaths[i]).c_str(), MakeLongPath(target).c_str(), 0))
        {
            continue;
        }

        ++failures;

        if (!::MoveFileExW(MakeLongPath(tempPaths[i]).c_str(),
                           MakeLongPath(fromPaths[i]).c_str(), 0))
        {
            Logger::Warn(L"批量改名撤销/重做失败，文件停在临时名：" + tempPaths[i]);
        }
    }

    return failures;
}

// ============================================================================
// 自绘菜单项的绘制（开关打开后「打开方式」「新建」子菜单里的每一行都走这里）
//
// 和系统菜单保持一致的观感：
//   选中行 → 系统高亮色底 + 高亮字色
//   普通行 → 菜单底色 + 菜单字色
//   左侧固定 16px 图标位，没有图标就留空（图标都是提前缓存好的，这里不取）
// ============================================================================
void MainWindow::DrawMenuItem(const DRAWITEMSTRUCT* drawItem)
{
    if (drawItem == nullptr || drawItem->itemData == 0)
    {
        return;
    }

    const MenuDrawItem* data = reinterpret_cast<const MenuDrawItem*>(drawItem->itemData);
    const RECT& rc = drawItem->rcItem;

    const bool selected = ((drawItem->itemState & ODS_SELECTED) != 0);
    const bool disabled = ((drawItem->itemState & ODS_DISABLED) != 0);

    // ---- 底色 ----
    HBRUSH background = ::CreateSolidBrush(
        selected ? ::GetSysColor(COLOR_HIGHLIGHT) : ::GetSysColor(COLOR_MENU));
    ::FillRect(drawItem->hDC, &rc, background);
    ::DeleteObject(background);

    // ---- 图标（左端）----
    const int iconSize = Scale(16);
    const int iconLeft = rc.left + Scale(8);
    const int iconTop  = rc.top + ((rc.bottom - rc.top) - iconSize) / 2;

    if (data->icon != nullptr)
    {
        ::DrawIconEx(drawItem->hDC, iconLeft, iconTop, data->icon, iconSize, iconSize,
                     0, nullptr, DI_NORMAL);
    }

    // ---- 文字 ----
    RECT textRect = rc;
    textRect.left = iconLeft + iconSize + Scale(8);
    textRect.right -= Scale(12);

    ::SetBkMode(drawItem->hDC, TRANSPARENT);
    ::SetTextColor(drawItem->hDC,
                   disabled ? ::GetSysColor(COLOR_GRAYTEXT)
                            : (selected ? ::GetSysColor(COLOR_HIGHLIGHTTEXT)
                                        : ::GetSysColor(COLOR_MENUTEXT)));

    HGDIOBJ oldFont = ::SelectObject(drawItem->hDC, m_hUiFont);
    ::DrawTextW(drawItem->hDC, data->text.c_str(), -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ::SelectObject(drawItem->hDC, oldFont);
}

// ---------------------------------------------------------------------------
// 弹一个"多项目属性"窗口（系统的「多个项目属性」对话框）
//
// 系统的做法是：把选中的多个对象装进一个 IDataObject，再交给
// SHMultiFileProperties() 弹出合并后的属性窗口（顶部会显示"N 个文件"、
// 有"计算大小"按钮、第二页能改只读/隐藏这些公共属性）。
//
// 这里照做：先把每个路径解析成 PIDL，再用同一父目录的 PIDL 组一个
// IDataObject。解析失败（比如虚拟项）就直接返回 false，交给调用方兜底。
// ---------------------------------------------------------------------------
static bool ShowMultiPropertiesForPaths(const std::vector<std::wstring>& paths)
{
    if (paths.size() < 2)
    {
        return false;
    }

    // ---- 1. 取第一个文件的父目录 PIDL（多选都在同一个目录里）----
    std::wstring parentPath = paths.front();
    {
        size_t slash = parentPath.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return false;
        }
        parentPath = parentPath.substr(0, slash);
        if (parentPath.size() == 2 && parentPath[1] == L':')
        {
            parentPath += L"\\";        // "C:" 要补成 "C:\"
        }
    }

    PIDLIST_ABSOLUTE folderPidl = nullptr;
    if (FAILED(::SHParseDisplayName(parentPath.c_str(), nullptr, &folderPidl, 0, nullptr)) ||
        folderPidl == nullptr)
    {
        return false;
    }

    // ---- 2. 每个文件解析成完整 PIDL，只取最后一段（相对父目录的子项 ID）----
    std::vector<PIDLIST_ABSOLUTE> fullPidls;        // 负责释放
    std::vector<PCUITEMID_CHILD>  children;         // 传给 SHCreateDataObject

    for (const std::wstring& path : paths)
    {
        PIDLIST_ABSOLUTE fullPidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(path.c_str(), nullptr, &fullPidl, 0, nullptr)) &&
            fullPidl != nullptr)
        {
            fullPidls.push_back(fullPidl);
            children.push_back(::ILFindLastID(fullPidl));
        }
    }

    IDataObject* dataObject = nullptr;
    HRESULT      hr         = E_FAIL;

    if (children.size() >= 2)
    {
        hr = ::SHCreateDataObject(folderPidl,
                                  static_cast<UINT>(children.size()),
                                  children.data(),
                                  nullptr,
                                  IID_IDataObject,
                                  reinterpret_cast<void**>(&dataObject));
    }

    if (SUCCEEDED(hr) && dataObject != nullptr)
    {
        ::SHMultiFileProperties(dataObject, 0);      // 模态，用户关掉才返回
        dataObject->Release();
    }

    for (PIDLIST_ABSOLUTE pidl : fullPidls)
    {
        ::CoTaskMemFree(pidl);
    }
    ::CoTaskMemFree(folderPidl);

    return SUCCEEDED(hr);
}

void MainWindow::ShowPropertiesForSelection()
{
    const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

    // ---- 多选：弹系统的「多个项目属性」窗口（和资源管理器一样）----
    if (selected.size() >= 2 && ShowMultiPropertiesForPaths(selected))
    {
        return;
    }

    // ---- 单选 / 没选中：显示一个对象的属性（没选中就是当前目录）----
    const std::wstring target = selected.empty() ? m_history.Current() : selected.front();
    if (target.empty())
    {
        return;
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_INVOKEIDLIST;   // 走 shell 的"属性"动词，弹系统原生属性窗口
    info.lpVerb = L"properties";
    info.lpFile = target.c_str();
    info.nShow  = SW_SHOW;
    info.hwnd   = m_hWnd;

    if (!::ShellExecuteExW(&info))
    {
        Logger::Warn(L"打开属性窗口失败：" + target);
    }
}

// ---------------------------------------------------------------------------
// 在新窗口中打开
//
// 先问一圈"是不是已经有一个窗口停在这个目录了"：
//   · 有（包括自己就在这个目录）→ 把那个窗口拉到前台，不再开新窗口；
//   · 没有 → 再启动一个自己（带目录参数），两个窗口各看各的。
//
// 这个"问一圈"只发生在真的要开新窗口的那一下，日常导航不碰。
// ---------------------------------------------------------------------------
void MainWindow::OpenFolderInNewWindow(const std::wstring& folderPath, bool forceNewWindow)
{
    if (folderPath.empty())
    {
        return;
    }

    // 已经有窗口停在这个目录：拉起来就够了，别重复开。
    // forceNewWindow = true（Ctrl+N / Ctrl+T）时跳过这一步：用户明确要求再开一个。
    if (!forceNewWindow &&
        ui::RelayOpenFolderToExistingWindow(folderPath, std::wstring(), m_hWnd))
    {
        Logger::Info(L"该目录已有窗口，已拉起到前台：" + folderPath);
        return;
    }

    wchar_t exePath[MAX_PATH * 2] = {};
    if (::GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) == 0)
    {
        Logger::Warn(L"取自身路径失败，无法在新窗口中打开");
        return;
    }

    std::wstring parameters = L"\"" + folderPath + L"\"";

    if (forceNewWindow)
    {
        // 开头那个 --new-window 是给新进程看的：告诉它"别因为该目录已有窗口就拉前台"，
        // 一定真的开一个（main.cpp 里读这个开关）。
        parameters += L" --new-window";
    }

    HINSTANCE result = ::ShellExecuteW(m_hWnd, L"open", exePath, parameters.c_str(),
                                       nullptr, SW_SHOWNORMAL);

    // ShellExecuteW 返回值 > 32 才算成功
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        Logger::Warn(L"在新窗口中打开失败：" + folderPath);
    }
    else
    {
        Logger::Info(L"已在新窗口中打开：" + folderPath);
    }
}

// ---------------------------------------------------------------------------
// 按住 Ctrl 双击：只有文件夹才在新窗口打开；文件仍按默认程序打开（跟系统一致）。
// 返回 true = 这个双击已经被"开新窗口"这条处理掉了，别再走默认的导航/打开。
// ---------------------------------------------------------------------------
bool MainWindow::ActivateItemInNewWindow(size_t index)
{
    std::shared_ptr<shell::EnumerateResult> result;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        result = m_result;
    }

    if (result == nullptr || index >= result->entries.size())
    {
        return false;
    }

    const shell::ShellEntry& entry = result->entries[index];

    if (entry.isGroupHeader || entry.isSpacer)
    {
        return true;    // 标题行、占位格：吞掉，什么都不做
    }

    // 判断要不要"进去"，看 canEnter 而不是 isFolder：压缩包（zip / rar …）
    // 显示上算文件，但 Shell 能当容器打开，Ctrl+双击同样应该在新窗口里进去看。
    // 文件不在这儿处理：交给默认程序打开，和系统一致
    if (!entry.canEnter || entry.fullPath.empty())
    {
        return false;
    }

    Logger::Info(L"Ctrl+双击：在新窗口中打开 [" + entry.name + L"] " + entry.fullPath);
    OpenFolderInNewWindow(entry.fullPath);
    return true;
}

// ---------------------------------------------------------------------------
// 别的窗口把"打开这个目录"的请求转过来，而且当前目录正好就是它：
//   1) 把自己拉到前台（最小化就还原）；
//   2) 请求里带了要选中的文件，就选中它并滚到可见。
// ---------------------------------------------------------------------------
void MainWindow::ActivateFromRelay(const std::wstring& selectFile)
{
    if (::IsIconic(m_hWnd))
    {
        ::ShowWindow(m_hWnd, SW_RESTORE);
    }

    ::SetForegroundWindow(m_hWnd);
    ::BringWindowToTop(m_hWnd);

    if (selectFile.empty())
    {
        Logger::Info(L"收到其它窗口的请求，已拉到前台");
        return;
    }

    // 列表还没枚举完（比如刚启动的窗口）就先记下来，OnEnumerationDone 里会选中
    if (RealItemCount() == 0)
    {
        m_startupSelectFile = selectFile;
    }
    else
    {
        m_fileListView.SelectPaths({ selectFile });
        m_fileListView.EnsureVisiblePath(selectFile);
    }

    Logger::Info(L"收到其它窗口的请求，已选中文件：" + selectFile);
}

void MainWindow::OpenSelection()
{
    int selectedIndex = ListView_GetNextItem(m_fileListView.Handle(), -1, LVNI_SELECTED);

    if (selectedIndex < 0)
    {
        return;
    }

    OnItemActivated(static_cast<size_t>(selectedIndex));
}

// ============================================================================
// 「打开方式」（自研账本）
//
// 规则回顾（和用户对齐过的）：
//   · 双击 / 右键「打开」永远用账本里记的默认程序，不受系统关联影响；
//   · 某个扩展名第一次用时弹一次设置窗口，设好就固定；
//   · 设置窗口里点「跳过」= 这次按系统默认打开、不记录、下次还会问；
//   · 默认程序被卸载 → 提示并让用户重选；
//   · 右键子菜单只读内存里的账本，绝不在弹出时去查注册表。
// ============================================================================

void MainWindow::FillOpenWithMenu(HMENU parentMenu, const std::wstring& extension)
{
    if (parentMenu == nullptr)
    {
        return;
    }

    HMENU subMenu = ::CreatePopupMenu();
    if (subMenu == nullptr)
    {
        return;
    }

    // 菜单里要不要画图标（设置窗口里那个开关）
    const bool showIcons = core::Settings::Instance().MenuIcons();

    // ---- 这里零 IO：只读配置里已经加载好的账本 ----
    const core::OpenWithEntry* entry = core::Settings::Instance().FindOpenWith(extension);

    int visibleCount = 0;
    bool ledgerHasSelf = false;   // 账本里已经有「本程序」这一项了（下面别重复加）

    if (entry != nullptr)
    {
        for (size_t i = 0; i < entry->items.size(); ++i)
        {
            // 命令 ID 只有 3400~3499 这一百个坑，超了就截断
            if (static_cast<int>(i) > 99)
            {
                break;
            }

            const core::OpenWithItem& item = entry->items[i];

            // 没勾选的（用户在设置窗口里取消过）不进菜单
            if (!item.enabled)
            {
                continue;
            }

            // 已知已经卸载/找不到的，直接消失（不标灰）。
            // alive = -1 表示"还没查过"——这种照常显示，
            // 否则右键菜单就得去探测，那正是用户不允许的。
            if (item.alive == 0)
            {
                continue;
            }

            std::wstring text = item.name;
            if (core::OpenWithKey(item) == entry->defaultKey)
            {
                text += L"（默认）";
            }

            if (item.kind == L"self")
            {
                ledgerHasSelf = true;
            }

            const UINT commandId =
                static_cast<UINT>(kCommandOpenWithBase + static_cast<int>(i));

            if (showIcons)
            {
                // 自绘菜单项：图标从内存缓存里拿（没有就留空，绝不在这里探测）
                auto drawItem = std::make_unique<MenuDrawItem>();
                drawItem->text = text;
                drawItem->icon = m_menuIcons.Get(MenuIconCache::KeyForProgram(item));

                MenuDrawItem* raw = drawItem.get();
                m_menuDrawItems.push_back(std::move(drawItem));

                ::AppendMenuW(subMenu, MF_OWNERDRAW, commandId,
                              reinterpret_cast<LPCTSTR>(raw));
            }
            else
            {
                ::AppendMenuW(subMenu, MF_STRING, commandId, text.c_str());
            }

            ++visibleCount;
        }
    }

    if (visibleCount > 0)
    {
        ::AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);
    }

    // 压缩包（zip/rar/7z…）：固定给一条「Yexplorer（本程序）」入口
    //
    // 为什么要固定加：账本里记默认程序时，压缩包的默认很可能是 WinRAR/7-Zip
    // （系统关联就是这样）。用户想用我们自己的文件管理器进去浏览内部，
    // 就得有个随时可点的入口，而不是只能改默认程序。
    // 点它不改账本，只是这一次用本程序打开（进容器浏览）。
    if (shell::IsContainerExtension(extension) && !ledgerHasSelf)
    {
        ::AppendMenuW(subMenu, MF_STRING, kCommandOpenWithSelf,
                      L"Yexplorer");
        ::AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);
    }

    // 一个程序都没有时，这一项就是"直接进窗口去设置"
    ::AppendMenuW(subMenu, MF_STRING, kCommandOpenWithManage, L"添加或管理程序…");

    ::AppendMenuW(parentMenu, MF_STRING | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(subMenu), L"打开方式");
}

// ============================================================================
// 「新建」二级菜单
//
// 内容和系统资源管理器一样：文件夹 + 这台机器上注册过「新建」模板的文件类型。
// 清单是启动时后台枚举好的快照（m_shellNewItems），这里只读内存，零成本。
// 开关打开时每一项都按自绘菜单画（前面带 16px 图标）。
// ============================================================================
void MainWindow::FillNewMenu(HMENU parentMenu)
{
    if (parentMenu == nullptr)
    {
        return;
    }

    HMENU subMenu = ::CreatePopupMenu();
    if (subMenu == nullptr)
    {
        return;
    }

    std::vector<shell::ShellNewItem> items;

    {
        std::lock_guard<std::mutex> lock(m_shellNewMutex);
        items = m_shellNewItems;
    }

    const bool showIcons = core::Settings::Instance().MenuIcons();

    // 虚拟文件夹（此电脑、控制面板这类）里不能建文件，全部变灰（和资源管理器一致）
    const std::wstring currentPath = m_history.Current();
    const bool canCreate = !currentPath.empty() &&
                           currentPath.compare(0, 2, L"::") != 0 &&
                           currentPath.compare(0, 6, L"shell:") != 0;

    for (size_t i = 0; i < items.size() && i < 100; ++i)
    {
        const shell::ShellNewItem& item = items[i];
        const UINT commandId = static_cast<UINT>(kCommandNewBase + static_cast<int>(i));

        if (showIcons)
        {
            auto drawItem = std::make_unique<MenuDrawItem>();
            drawItem->text = item.displayName;
            drawItem->icon = m_menuIcons.Get(MenuIconCache::KeyForNewItem(item));

            MenuDrawItem* raw = drawItem.get();
            m_menuDrawItems.push_back(std::move(drawItem));

            ::AppendMenuW(subMenu, MF_OWNERDRAW | (canCreate ? 0 : MF_GRAYED), commandId,
                          reinterpret_cast<LPCTSTR>(raw));
        }
        else
        {
            ::AppendMenuW(subMenu, MF_STRING | (canCreate ? 0 : MF_GRAYED), commandId,
                          item.displayName.c_str());
        }
    }

    // 快照还没准备好（启动那一刻就点了右键）：至少给个「新建文件夹」占位，
    // 别让菜单是空的
    if (items.empty())
    {
        ::AppendMenuW(subMenu, MF_STRING, kCommandNewFolder, L"文件夹");
    }

    ::AppendMenuW(parentMenu, MF_STRING | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(subMenu), L"新建(&W)");
}

// ============================================================================
// 按「新建」快照里第 index 项真的建一个出来
//
// 和资源管理器一致：建完立刻选中它、直接进入改名状态，
// 用户不用再点一下 F2。
// ============================================================================
void MainWindow::DoNewItem(size_t index)
{
    std::vector<shell::ShellNewItem> items;

    {
        std::lock_guard<std::mutex> lock(m_shellNewMutex);
        items = m_shellNewItems;
    }

    if (index >= items.size())
    {
        return;
    }

    const std::wstring current = m_history.Current();
    if (current.empty())
    {
        return;
    }

    // 虚拟文件夹（此电脑、控制面板）里不能建文件
    if (current.compare(0, 2, L"::") == 0 || current.compare(0, 6, L"shell:") == 0)
    {
        return;
    }

    std::wstring created;
    std::wstring error;

    if (!shell::CreateShellNewItem(items[index], current, created, error))
    {
        if (!error.empty())
        {
            ::MessageBoxW(m_hWnd, error.c_str(), L"新建", MB_OK | MB_ICONWARNING);
        }
        return;
    }

    Logger::Info(L"新建项：" + items[index].displayName +
                 L"，结果=" + (created.empty() ? std::wstring(L"（向导）") : created));

    if (created.empty())
    {
        // 快捷方式向导：它自己会建，建完目录监视会触发刷新；
        // 这里仍然主动刷一次，免得监视消息来得慢
        RefreshCurrentFolder();
        return;
    }

    // 记进撤销栈：撤销「新建」= 把刚建出来的这个文件删掉
    {
        UndoRecord record;
        record.kind = UndoKind::Create;
        record.afterPaths.assign(1, created);
        PushUndoRecord(record);
    }

    // ---- 局部插入一行（不重扫整个目录）----
    std::wstring name = created;
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        name = name.substr(slash + 1);
    }

    shell::ShellEntry entry;
    entry.name     = name;
    entry.fullPath = created;
    entry.isFolder = (items[index].kind == shell::ShellNewKind::Folder);
    entry.canEnter = entry.isFolder;    // 文件夹才能双击进去；新建的文件/快捷方式不行

    WIN32_FILE_ATTRIBUTE_DATA attributeData = {};
    if (::GetFileAttributesExW(created.c_str(), GetFileExInfoStandard, &attributeData))
    {
        entry.modified = attributeData.ftLastWriteTime;
        entry.size     = attributeData.nFileSizeLow;
    }
    else
    {
        ::GetSystemTimeAsFileTime(&entry.modified);
        entry.size = 0;
    }

    m_fileListView.InsertEntry(entry);
    UpdateTitle();
    UpdateStatusBar();

    // 选中并进入改名（名字是「新建 文本文档」，用户多半要改）
    m_fileListView.SelectPaths({ created });
    m_fileListView.BeginRenameSelected();
}

// ============================================================================
// 启动后准备菜单数据（全部在后台，不占启动时间，弹菜单时零成本）
//   · 「新建」项清单：读一次注册表（ShellNew）
//   · 菜单小图标：程序图标 + 新建项图标
// ============================================================================
void MainWindow::StartBackgroundMenuData()
{
    // ---- 1. 已有账本里的程序图标先取一遍（主线程拿快照，后台去取）----
    {
        std::vector<core::OpenWithItem> programs;

        for (const core::OpenWithEntry& entry : core::Settings::Instance().OpenWithEntries())
        {
            for (const core::OpenWithItem& item : entry.items)
            {
                programs.push_back(item);
            }
        }

        m_menuIcons.PrefetchPrograms(programs);
    }

    // ---- 2. 「新建」项清单（后台线程读注册表）----
    if (m_shellNewThread.joinable())
    {
        return;
    }

    HWND hwnd = m_hWnd;

    m_shellNewThread = std::thread([this, hwnd]()
    {
        // 后台线程里 SHGetFileInfo 读类型名要用 COM
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        std::vector<shell::ShellNewItem> items = shell::EnumerateShellNew();

        {
            std::lock_guard<std::mutex> lock(m_shellNewMutex);
            m_shellNewItems = items;
        }

        if (::IsWindow(hwnd))
        {
            ::PostMessageW(hwnd, WM_APP_SHELLNEW_READY, 0, 0);
        }

        ::CoUninitialize();
    });
}

void MainWindow::SetOpenWithProgramAlive(const std::wstring& extension,
                                         const std::wstring& programKey,
                                         int alive)
{
    core::OpenWithEntry& entry = core::Settings::Instance().EnsureOpenWith(extension);

    bool changed = false;
    for (core::OpenWithItem& item : entry.items)
    {
        if (core::OpenWithKey(item) == programKey && item.alive != alive)
        {
            item.alive = alive;
            changed = true;
        }
    }

    // 只在真的变了才落盘：打开文件很频繁，每次都写一遍配置没必要
    if (changed)
    {
        core::Settings::Instance().Save();
    }
}

void MainWindow::ShowOpenWithSettings(const std::wstring& extension, bool firstTime,
                                      const std::wstring& filePath)
{
    ui::OpenWithDialog dialog(m_hWnd, &m_iconCache);
    dialog.ShowModal(extension, firstTime, filePath);

    // 用户在窗口里可能加了新程序：后台补一次图标，下次右键菜单就有它的图标了
    {
        std::vector<core::OpenWithItem> programs;

        for (const core::OpenWithEntry& entry : core::Settings::Instance().OpenWithEntries())
        {
            for (const core::OpenWithItem& item : entry.items)
            {
                programs.push_back(item);
            }
        }

        m_menuIcons.PrefetchPrograms(programs);
    }

    // 用户点了「打开」→ 用刚设好的默认程序把这个文件打开
    if (!dialog.OpenRequested() || filePath.empty())
    {
        return;
    }

    const core::OpenWithEntry* entry = core::Settings::Instance().FindOpenWith(extension);

    if (entry == nullptr || entry->defaultKey.empty())
    {
        return;
    }

    for (const core::OpenWithItem& item : entry->items)
    {
        if (core::OpenWithKey(item) == entry->defaultKey)
        {
            std::wstring error;
            if (!shell::Launch(m_hWnd, item, filePath, error))
            {
                ::MessageBoxW(m_hWnd, error.c_str(), L"打开方式", MB_OK | MB_ICONERROR);
            }
            return;
        }
    }
}

void MainWindow::OpenSelectionWithProgram(size_t programIndex)
{
    std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
    if (selected.empty())
    {
        return;
    }

    const std::wstring extension = shell::ExtensionKeyForPath(selected.front());

    const core::OpenWithEntry* entry = core::Settings::Instance().FindOpenWith(extension);
    if (entry == nullptr || programIndex >= entry->items.size())
    {
        return;
    }

    // 拷贝一份：下面写配置可能让 vector 重新分配，指针会失效
    const core::OpenWithItem program = entry->items[programIndex];
    const std::wstring programKey = core::OpenWithKey(program);

    // 打开前先确认程序还在（时机 D）
    // 不在的话不弹消息框，直接打开设置窗口重选（用户要求）
    bool diskMissing = false;
    if (!shell::IsAvailable(program, diskMissing))
    {
        SetOpenWithProgramAlive(extension, programKey, 0);

        ShowOpenWithSettings(extension, false, selected.front());
        return;
    }

    for (const std::wstring& filePath : selected)
    {
        if (filePath.empty())
        {
            continue;
        }

        std::wstring error;
        if (!shell::Launch(m_hWnd, program, filePath, error))
        {
            // 注意：这里不把 alive 标成 0。
            // 「启动失败」不等于「程序被卸载」——文件被占用、参数不对都可能失败；
            // 真正"不在"由上面的 IsAvailable 判定，那个结论才写进配置。
            std::wstring message = error + L"\n\n要重新选择打开程序吗？";
            if (::MessageBoxW(m_hWnd, message.c_str(), L"打开方式",
                              MB_OKCANCEL | MB_ICONWARNING) == IDOK)
            {
                ShowOpenWithSettings(extension, false);
            }
            return;
        }
    }

    SetOpenWithProgramAlive(extension, programKey, 1);
}

bool MainWindow::TryOpenFileWithOurChoice(const std::wstring& filePath,
                                          const std::wstring& extension)
{
    if (filePath.empty() || !shell::IsExtensionHandled(extension))
    {
        return false;
    }

    // ---- 1. 查账本里这个扩展名的默认程序 ----
    core::OpenWithItem program;          // 拷贝一份，后面配置变动不会影响它
    bool haveProgram = false;

    const core::OpenWithEntry* entry = core::Settings::Instance().FindOpenWith(extension);
    const bool neverConfigured = (entry == nullptr || entry->defaultKey.empty());

    if (!neverConfigured)
    {
        for (const core::OpenWithItem& item : entry->items)
        {
            if (core::OpenWithKey(item) == entry->defaultKey)
            {
                program     = item;
                haveProgram = true;
                break;
            }
        }
    }

    // ---- 2. 没设过（或设的那个已经从账本里被删掉了）→ 让用户设一次 ----
    // 用户要求：不再弹"原来的默认程序已经不在列表里了"这种消息框，直接打开设置窗口
    if (!haveProgram)
    {
        ui::OpenWithDialog dialog(m_hWnd, &m_iconCache);
        dialog.ShowModal(extension, neverConfigured, filePath);

        // 没设默认就关掉了（点「关闭」）→ 这次按系统默认打开，不记录，下次还会问
        if (!dialog.OpenRequested())
        {
            if (!shell::LaunchWithSystemDefault(m_hWnd, filePath))
            {
                ::MessageBoxW(m_hWnd, L"按系统默认打开失败。",
                              L"打开文件", MB_OK | MB_ICONERROR);
            }
            return true;
        }

        // 用户点了「打开」：用刚设好的默认程序开这个文件
        const core::OpenWithEntry* refreshed =
            core::Settings::Instance().FindOpenWith(extension);

        if (refreshed == nullptr || refreshed->defaultKey.empty())
        {
            return true;
        }

        for (const core::OpenWithItem& item : refreshed->items)
        {
            if (core::OpenWithKey(item) == refreshed->defaultKey)
            {
                program     = item;
                haveProgram = true;
                break;
            }
        }

        if (!haveProgram)
        {
            return true;
        }
    }

    // ---- 3. 打开前最后确认一次程序还在（时机 D）----
    // 程序不在了（卸载/移动/磁盘没插）：不再弹消息框，直接把设置窗口弹出来让用户重选
    bool diskMissing = false;
    if (!shell::IsAvailable(program, diskMissing))
    {
        SetOpenWithProgramAlive(extension, core::OpenWithKey(program), 0);

        ShowOpenWithSettings(extension, false, filePath);
        return true;
    }

    // ---- 4. 打开 ----
    std::wstring error;
    if (!shell::Launch(m_hWnd, program, filePath, error))
    {
        // 同上：启动失败不当作"程序被卸载"，不写 alive=0
        std::wstring message = error + L"\n\n要重新选择打开程序吗？";
        if (::MessageBoxW(m_hWnd, message.c_str(), L"打开方式",
                          MB_OKCANCEL | MB_ICONWARNING) == IDOK)
        {
            ShowOpenWithSettings(extension, false);
        }
        return true;
    }

    SetOpenWithProgramAlive(extension, core::OpenWithKey(program), 1);
    return true;
}

// ============================================================================
// 压缩包（zip / rar / 7z…）双击的打开判定
//
// 走和普通文件同一本「打开方式」账本，但默认项不一样：
//   1. 账本里设过默认程序
//        · 设的是「本程序」→ 就在这个窗口里进去浏览（等同资源管理器双击 zip）
//        · 设的是别的程序  → 用那个程序打开（.rar 用 WinRAR 这类场景）
//   2. 没设过 → 弹「打开方式」窗口，候选里多一条「Yexplorer（本程序）」并默认选中；
//      用户点「打开」就按选中的走；直接关窗口＝按默认（本程序）进去浏览。
// ============================================================================
void MainWindow::OpenContainerEntry(const shell::ShellEntry& entry)
{
    Logger::Info(L"双击压缩包：[" + entry.name + L"] " + entry.fullPath);

    const std::wstring extension = shell::ExtensionKeyForPath(entry.fullPath);

    // 账本里设过的那一项（拷贝一份，后面配置变动不影响它）
    auto findConfiguredProgram = [&extension](core::OpenWithItem& program) -> bool
    {
        const core::OpenWithEntry* ledger = core::Settings::Instance().FindOpenWith(extension);

        if (ledger == nullptr || ledger->defaultKey.empty())
        {
            return false;
        }

        for (const core::OpenWithItem& item : ledger->items)
        {
            if (core::OpenWithKey(item) == ledger->defaultKey)
            {
                program = item;
                return true;
            }
        }

        return false;
    };

    core::OpenWithItem program;
    bool haveProgram = findConfiguredProgram(program);

    if (!haveProgram)
    {
        // 没设过（或设的那个已经被删）：弹设置窗口，默认替用户选好「本程序」
        const bool neverConfigured =
            core::Settings::Instance().FindOpenWith(extension) == nullptr;

        ui::OpenWithDialog dialog(m_hWnd, &m_iconCache);
        dialog.ShowModal(extension, neverConfigured, entry.fullPath, /*preferSelf=*/true);

        if (!dialog.OpenRequested())
        {
            // 直接关窗口：按默认（本程序）进去浏览，和资源管理器双击 zip 一样
            NavigateContainerEntry(entry);
            return;
        }

        haveProgram = findConfiguredProgram(program);

        if (!haveProgram)
        {
            // 极端情况：窗口里没留下默认项（比如账本被清空），退回本程序浏览
            NavigateContainerEntry(entry);
            return;
        }
    }

    // ---- 本程序：进容器里浏览 ----
    if (program.kind == L"self")
    {
        NavigateContainerEntry(entry);
        return;
    }

    // ---- 别的程序：先确认还在，再启动 ----
    bool diskMissing = false;
    if (!shell::IsAvailable(program, diskMissing))
    {
        SetOpenWithProgramAlive(extension, core::OpenWithKey(program), 0);
        ShowOpenWithSettings(extension, false, entry.fullPath);
        return;
    }

    std::wstring error;
    if (!shell::Launch(m_hWnd, program, entry.fullPath, error))
    {
        std::wstring message = error + L"\n\n要重新选择打开程序吗？";
        if (::MessageBoxW(m_hWnd, message.c_str(), L"打开压缩包",
                          MB_OKCANCEL | MB_ICONWARNING) == IDOK)
        {
            ShowOpenWithSettings(extension, false, entry.fullPath);
        }
        return;
    }

    SetOpenWithProgramAlive(extension, core::OpenWithKey(program), 1);
}

// 进压缩包容器里浏览（地址栏写它的名字，向上按钮能回到进来之前的目录）
void MainWindow::NavigateContainerEntry(const shell::ShellEntry& entry)
{
    RecordNavigationOrigin(entry.fullPath, entry.name);
    NavigateToPath(entry.fullPath);
}

// ============================================================================
// 打开容器内部的文件（先解成临时文件再打开）
// ============================================================================

// 某个压缩包里解出来的东西落在哪个临时目录（%TEMP%\{本进程固定 guid}_{压缩包文件名}）
//
// 单独抽出来是因为"打开压缩包里的文件"和"拖出压缩包里的项"两处都要用，
// 而且拖出那边还要靠它算出"这一项会解到哪个文件"来显示真实进度。
std::wstring MainWindow::ContainerTempDirectoryFor(const std::wstring& containerRoot)
{
    if (containerRoot.empty())
    {
        return L"";
    }

    // ---- 临时目录名里那一段固定 guid ----
    //
    // 整个进程只生成一次，好处是同一个压缩包反复打开/拖出里面的文件，
    // 都解到同一个目录，不会每点一次就在临时目录里堆一个新的文件夹。
    if (m_containerTempTag.empty())
    {
        GUID guid = {};
        if (SUCCEEDED(::CoCreateGuid(&guid)))
        {
            wchar_t guidText[64] = {};
            if (::StringFromGUID2(guid, guidText, _countof(guidText)) > 0)
            {
                std::wstring text(guidText);

                // StringFromGUID2 给的是 {769C474B-...} 这种带花括号的形式，去掉两边
                if (text.size() >= 2 && text.front() == L'{' && text.back() == L'}')
                {
                    text = text.substr(1, text.size() - 2);
                }

                m_containerTempTag = text + L"_";
            }
        }

        if (m_containerTempTag.empty())
        {
            // 极端情况兜底：拿不到 GUID 就用运行时长和进程号拼一个
            m_containerTempTag = L"yexplorer_" +
                                 std::to_wstring(::GetTickCount64()) + L"_" +
                                 std::to_wstring(::GetCurrentProcessId()) + L"_";
        }
    }

    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(_countof(tempPath), tempPath) == 0)
    {
        return L"";
    }

    std::wstring containerName = containerRoot;
    size_t nameSeparator = containerName.find_last_of(L"\\/");
    if (nameSeparator != std::wstring::npos)
    {
        containerName = containerName.substr(nameSeparator + 1);
    }

    // GetTempPathW 返回的路径自带末尾反斜杠，直接拼
    return std::wstring(tempPath) + m_containerTempTag + containerName;
}

std::wstring MainWindow::ExtractContainerEntryToTemp(const std::wstring& virtualPath,
                                                     std::wstring& error)
{
    error.clear();

    if (virtualPath.empty())
    {
        error = L"路径为空";
        return L"";
    }

    // ---- 1) 往上找容器根（压缩包本身）----
    //
    // 例：C:\a\t.rar\sub\inner.txt → C:\a\t.rar
    // 判断逻辑和右键菜单用的是同一套（shell::ContainerRootForPath），
    // 只查文件属性、不碰 COM，很快。
    const std::wstring containerRoot = shell::ContainerRootForPath(virtualPath);

    if (containerRoot.empty())
    {
        error = L"找不到这个文件所属的压缩包";
        return L"";
    }

    // ---- 2) 这一项在包内的相对路径（保留包里的子目录结构）----
    std::wstring innerPath = virtualPath.substr(containerRoot.size());

    while (!innerPath.empty() && (innerPath.front() == L'\\' || innerPath.front() == L'/'))
    {
        innerPath.erase(innerPath.begin());
    }

    // ---- 3) 临时目录 ----
    const std::wstring tempDirectory = ContainerTempDirectoryFor(containerRoot);

    if (tempDirectory.empty())
    {
        error = L"取不到系统临时目录";
        return L"";
    }

    // ---- 4) 真正去解（先读流，读不出来走 Shell 复制通道；失败原因填在 error 里）----
    return shellops::ExtractContainerEntry(virtualPath, containerRoot, innerPath,
                                           tempDirectory, error);
}

// ============================================================================
// 拖出压缩包内部的项
// ============================================================================

namespace
{
// 解压超过这个字节数才显示进度小窗（小的几百 KB 是一瞬间的事，弹窗反而闪眼睛）
const unsigned long long kDragProgressThresholdBytes = 8ull * 1024ull * 1024ull;
}

std::vector<std::wstring> MainWindow::PrepareDragPaths(const std::vector<std::wstring>& paths)
{
    // ---- 1) 先看这一批里有没有压缩包内部的项 ----
    //
    // 普通磁盘文件走的还是原来的路（路径原样交给拖动循环），
    // 这里只查文件属性、不碰 COM，所以来回拖动不会变慢。
    bool hasContainerEntry = false;
    for (const auto& path : paths)
    {
        if (shell::IsPathInsideContainer(path))
        {
            hasContainerEntry = true;
            break;
        }
    }

    if (!hasContainerEntry)
    {
        return paths;
    }

    // ---- 2) 先把每一项的"落点"和"该有多大"算出来 ----
    //
    // 这一步全是本地查询（拼路径、查列表里已经枚举好的大小），不碰 COM，很快。
    // 有了落点文件路径，进度就能按"文件长到多大了"来算，而不是按项数糊弄。
    struct DragItem
    {
        std::wstring       virtualPath;      // 容器内路径
        std::wstring       containerRoot;    // 压缩包本身
        std::wstring       innerPath;        // 包内相对路径
        std::wstring       tempDirectory;    // 解压落点的目录
        std::wstring       outputPath;       // 会解到哪个文件（进度靠盯它的大小）
        std::wstring       name;             // 进度窗里显示的名字
        unsigned long long expectedSize = 0; // 解完应该多大（0 = 包没给出大小）
    };

    std::vector<DragItem> items;
    items.reserve(paths.size());

    unsigned long long totalBytes = 0;   // 这一批解完一共多少字节

    for (const auto& path : paths)
    {
        DragItem item;
        item.virtualPath = path;

        item.containerRoot = shell::ContainerRootForPath(path);

        if (item.containerRoot.empty())
        {
            Logger::Warn(L"拖出：找不到这项所属的压缩包，已跳过：" + path);
            continue;
        }

        // 包内相对路径（去掉前导的分隔符，保留里面的子目录结构）
        item.innerPath = path.substr(item.containerRoot.size());

        while (!item.innerPath.empty() &&
               (item.innerPath.front() == L'\\' || item.innerPath.front() == L'/'))
        {
            item.innerPath.erase(item.innerPath.begin());
        }

        item.tempDirectory = ContainerTempDirectoryFor(item.containerRoot);

        if (item.tempDirectory.empty() || item.innerPath.empty())
        {
            Logger::Warn(L"拖出：算不出这项的解压落点，已跳过：" + path);
            continue;
        }

        // 落点 = 临时目录 + 包内相对路径（规则和 shellops::ExtractContainerEntry 一致）
        item.outputPath = item.tempDirectory;

        if (item.outputPath.back() != L'\\' && item.outputPath.back() != L'/')
        {
            item.outputPath += L'\\';
        }
        item.outputPath += item.innerPath;

        for (wchar_t& ch : item.outputPath)
        {
            if (ch == L'/')
            {
                ch = L'\\';   // 统一成反斜杠，方便后面查文件大小
            }
        }

        const size_t slash = path.find_last_of(L"\\/");
        item.name = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;

        item.expectedSize = m_fileListView.SizeOfPath(path);
        totalBytes += item.expectedSize;

        items.push_back(std::move(item));
    }

    if (items.empty())
    {
        return std::vector<std::wstring>();
    }

    // ---- 3) 大的才弹进度窗 ----
    //
    // 解压发生在进入拖动循环之前：这期间用户一直按着鼠标、屏幕上什么都不动，
    // 很容易以为"卡住了"。小的几百毫秒就完事，弹窗反而闪眼睛。
    const bool showProgress = totalBytes >= kDragProgressThresholdBytes;

    std::vector<std::wstring> realPaths;
    realPaths.reserve(items.size());

    unsigned long long finishedBytes = 0;   // 已经解完的项累计多少字节

    for (size_t index = 0; index < items.size(); ++index)
    {
        const DragItem& item = items[index];

        // ---- 解压放后台线程，主线程留着刷进度条 ----
        //
        // 这里必须先起线程再轮询：CopyHere 是同步阻塞的（实测 652MB 要十二三秒），
        // 在主线程里直接调，进度窗在这十几秒里一次都刷不出来。
        struct ExtractJob
        {
            std::atomic<bool> finished{ false };
            std::wstring      realPath;
            std::wstring      error;
        };

        auto job = std::make_shared<ExtractJob>();

        std::thread worker([item, job]()
        {
            // Shell.Application / IStream 都是 COM 对象，这条线程得先初始化成 STA
            const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

            job->realPath = shellops::ExtractContainerEntry(item.virtualPath, item.containerRoot,
                                                            item.innerPath, item.tempDirectory,
                                                            job->error);

            if (SUCCEEDED(comResult))
            {
                ::CoUninitialize();
            }

            job->finished = true;
        });
        worker.detach();

        // ---- 主线程：每 100 毫秒看一眼落点文件长到多大了 ----
        while (!job->finished)
        {
            unsigned long long doneBytes = 0;

            WIN32_FILE_ATTRIBUTE_DATA fileData = {};
            if (::GetFileAttributesExW(item.outputPath.c_str(),
                                       ::GetFileExInfoStandard, &fileData))
            {
                doneBytes = (static_cast<unsigned long long>(fileData.nFileSizeHigh) << 32) |
                            static_cast<unsigned long long>(fileData.nFileSizeLow);
            }

            if (showProgress)
            {
                // 这一项的解压进度：别超过它自己的预期大小（有的包给不出大小，就用总字节兜底）
                const unsigned long long expected =
                    (item.expectedSize > 0) ? item.expectedSize : totalBytes;
                const unsigned long long cappedDone =
                    (doneBytes < expected) ? doneBytes : expected;

                const unsigned long long overallDone = finishedBytes + cappedDone;

                int percent = 0;
                if (totalBytes > 0)
                {
                    percent = static_cast<int>((overallDone * 100ull) / totalBytes);
                }

                std::wstring text = L"正在解出：" + item.name +
                                    L"（第 " + std::to_wstring(index + 1) +
                                    L" / 共 " + std::to_wstring(items.size()) + L" 项）" +
                                    L"  " + std::to_wstring(percent) + L"%";

                if (totalBytes > 0)
                {
                    text += L"（" + std::to_wstring(overallDone / (1024ull * 1024ull)) + L" / " +
                            std::to_wstring(totalBytes / (1024ull * 1024ull)) + L" MB）";
                }

                ShowDragProgress(text, percent);
            }

            ::Sleep(100);
        }

        if (job->realPath.empty())
        {
            // 解不出来就跳过这一项，其余照拖（日志里留个原因）
            Logger::Warn(L"拖出：压缩包内项解不出来，已跳过：" + item.virtualPath +
                         L"，原因：" + (job->error.empty() ? L"未知" : job->error));
            continue;
        }

        realPaths.push_back(std::move(job->realPath));
        finishedBytes += item.expectedSize;
    }

    if (showProgress)
    {
        HideDragProgress();
    }

    Logger::Info(L"拖出：从压缩包解出 " + std::to_wstring(realPaths.size()) + L" / " +
                 std::to_wstring(items.size()) + L" 项到临时目录");

    // 一项都没成功：返回空，调用方据此放弃这次拖动
    return realPaths;
}

void MainWindow::ShowDragProgress(const std::wstring& text, int percent)
{
    RECT client = {};
    ::GetClientRect(m_hWnd, &client);

    const int panelWidth  = 420;   // 要放得下"名字 + 项数 + 百分比 + 已解/总量"
    const int panelHeight = 96;
    const int left = ((client.right - client.left) - panelWidth) / 2;
    const int top  = ((client.bottom - client.top) - panelHeight) / 2;

    // ---- 第一次用的时候创建三个控件 ----
    //
    // 面板用 STATIC + SS_WHITERECT（一个白底方块），文字和进度条做成它的子窗口。
    //
    // 【为什么文字/进度条必须是面板的子窗口，而不是主窗口的子窗口】
    //   解压这段是同步循环，主窗口在解压期间不处理任何消息（没有消息泵），
    //   所以主窗口的子窗口永远等不到 WM_PAINT，画不出来。
    //   只有把它们挂在面板下面，才能靠一次 RedrawWindow(RDW_ALLCHILDREN)
    //   把"白底 + 文字 + 进度条"三块一起强制画出来。
    //   （之前的写法三块都挂在主窗口上，结果屏幕上只有一块空白的白底。）
    if (m_dragProgressPanel == nullptr)
    {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);

        m_dragProgressPanel = ::CreateWindowExW(0, L"STATIC", L"",
                                                WS_CHILD | WS_VISIBLE | WS_BORDER | SS_WHITERECT,
                                                left, top, panelWidth, panelHeight,
                                                m_hWnd, nullptr, instance, nullptr);

        // 坐标相对面板（也就是面板内部左上角）
        // SS_ENDELLIPSIS：名字特别长的时候尾巴上打省略号，不至于被硬生生切掉
        m_dragProgressText = ::CreateWindowExW(0, L"STATIC", text.c_str(),
                                               WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX |
                                                   SS_ENDELLIPSIS,
                                               12, 20, panelWidth - 24, 22,
                                               m_dragProgressPanel, nullptr, instance, nullptr);

        m_dragProgressBar = ::CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE,
                                              12, 54, panelWidth - 24, 18,
                                              m_dragProgressPanel, nullptr, instance, nullptr);

        if (m_dragProgressBar != nullptr)
        {
            ::SendMessageW(m_dragProgressBar, PBM_SETRANGE32, 0, 100);
        }

        // 字体跟列表一致，看起来不像"野控件"
        if (m_dragProgressText != nullptr && m_fileListView.Handle() != nullptr)
        {
            HFONT font = reinterpret_cast<HFONT>(
                ::SendMessageW(m_fileListView.Handle(), WM_GETFONT, 0, 0));
            if (font != nullptr)
            {
                ::SendMessageW(m_dragProgressText, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
        }
    }

    if (m_dragProgressText != nullptr)
    {
        ::SetWindowTextW(m_dragProgressText, text.c_str());
    }
    if (m_dragProgressBar != nullptr)
    {
        ::SendMessageW(m_dragProgressBar, PBM_SETPOS, static_cast<WPARAM>(percent), 0);
    }

    // 同步循环里没有消息泵：这三块必须自己强制画出来，
    // 否则要等到解压结束（控件销毁）才会真的出现在屏幕上。
    if (m_dragProgressPanel != nullptr)
    {
        ::ShowWindow(m_dragProgressPanel, SW_SHOW);

        for (HWND control : { m_dragProgressText, m_dragProgressBar })
        {
            if (control != nullptr)
            {
                ::ShowWindow(control, SW_SHOW);
            }
        }

        // RDW_ALLCHILDREN 会把面板下面的文字和进度条一起重画
        ::RedrawWindow(m_dragProgressPanel, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

void MainWindow::HideDragProgress()
{
    // 三个控件一起销毁，下次需要时重新建（用它的时间是"拖大文件"这种低频场景，
    // 常驻没必要，留着还容易被别的重绘逻辑误伤）
    for (HWND* handle : { &m_dragProgressBar, &m_dragProgressText, &m_dragProgressPanel })
    {
        if (*handle != nullptr)
        {
            ::DestroyWindow(*handle);
            *handle = nullptr;
        }
    }
}

// ============================================================================
// 排序
// ============================================================================

std::shared_ptr<shell::EnumerateResult> MainWindow::CurrentResult()
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_result;
}

// 当前目录里「真正有多少项」：分组标题行只是画出来的一行字，不算一个项目
size_t MainWindow::RealItemCount()
{
    std::lock_guard<std::mutex> lock(m_resultMutex);

    if (!m_resultReady || m_result == nullptr)
    {
        return 0;
    }

    size_t count = 0;
    for (const auto& entry : m_result->entries)
    {
        // 分组标题行和占位格都不是真项目，不计数
        if (!entry.isGroupHeader && !entry.isSpacer)
        {
            ++count;
        }
    }

    return count;
}

void MainWindow::UpdateTitle()
{
    std::wstring path = DisplayNameFor(m_history.Current());

    std::wstring title = std::wstring(kWindowTitleBase) + L" - " + path +
                         L"（" + std::to_wstring(RealItemCount()) + L" 项）";

    // 标题没变就不要重设：频繁改窗口标题会让任务栏按钮重新生成，
    // 视觉上就是窗口"闪一下"，像失去了焦点
    if (title != m_lastTitle)
    {
        m_lastTitle = title;
        ::SetWindowTextW(m_hWnd, title.c_str());
    }
}

void MainWindow::SortEntries()
{
    std::shared_ptr<shell::EnumerateResult> result = CurrentResult();
    if (result == nullptr)
    {
        return;
    }

    // 分组标题行要留在最前面，不参加排序：
    // 把开头那段标题行先切出来，只排后面真正的条目，排完再接回去。
    size_t headerCount = 0;
    while (headerCount < result->entries.size() && result->entries[headerCount].isGroupHeader)
    {
        ++headerCount;
    }

    std::sort(result->entries.begin() + static_cast<std::ptrdiff_t>(headerCount),
              result->entries.end(),
              [this](const shell::ShellEntry& left, const shell::ShellEntry& right)
              {
                  return CompareEntries(left, right, m_sortColumn, m_sortAscending);
              });
}

void MainWindow::OnColumnClick(int column)
{
    // 「此电脑」是分组显示的（上面有一行「设备和驱动器」标题）。
    // 一排序，那一行标题就会被当成普通条目排到中间去，分组就散了。
    // 系统的此电脑本来也不让按列排序，所以这里直接不响应。
    if (_wcsicmp(m_history.Current().c_str(), shell::kThisPcPath) == 0)
    {
        Logger::Info(L"「此电脑」是分组视图，忽略按列排序");
        return;
    }

    // 点同一列就换个方向，点别的列从升序开始（资源管理器就是这么做的）
    if (column == m_sortColumn)
    {
        m_sortAscending = !m_sortAscending;
    }
    else
    {
        m_sortColumn    = column;
        m_sortAscending = true;
    }

    SortEntries();

    std::shared_ptr<shell::EnumerateResult> result = CurrentResult();
    m_fileListView.SetSortState(m_sortColumn, m_sortAscending);

    // 排序只改了「谁排第几」，条目数量一个没变，增量更新（MergeResult）看不出任何变化，
    // 结果就是列表还显示旧顺序，非得鼠标划过才重画。这里必须整表重设一遍。
    // SetResult 内部会自己记住选中项和滚动位置，再按名字恢复，所以排序后选中不会乱跳。
    m_fileListView.SetResult(result);

    // 记一笔，方便验证点了表头到底有没有生效
    if (result != nullptr)
    {
        const wchar_t* columnNames[] = { L"名称", L"修改日期", L"类型", L"大小" };
        std::wstring columnName = (column >= 0 && column <= 3) ? columnNames[column] : L"未知";

        std::wstring sample;
        for (size_t i = 0; i < result->entries.size() && i < 5; ++i)
        {
            sample += result->entries[i].name + L" | ";
        }

        Logger::Info(L"按「" + columnName + L"」排序（" +
                     std::wstring(m_sortAscending ? L"升序" : L"降序") +
                     L"），前 5 项：" + sample);
    }
}

// ============================================================================
// 视图模式
//
// 四件事：
//   1. 「此电脑」「控制面板」这两处**单独记忆**，默认都是平铺，
//      互不影响，也不跟普通文件夹的全局设置走（见下面 ViewLocationKeyForPath）
//   2. 普通磁盘目录 → 详细信息（和资源管理器打开文件夹时一样）
//   3. 其它系统文件夹（网络 / 回收站 / 主文件夹 / 顶层桌面）→ 小图标
//   4. 普通目录里用户从右键「查看」里选过的，就不再自动改，一直按他选的来
// ============================================================================

namespace
{
// 控制面板根（分类首页）。它下面的子项（含「所有控制面板项」）解析名都以它开头，
// 所以前缀匹配就能把整片控制面板当成同一处。
constexpr const wchar_t* kControlPanelRootPath = L"::{26EE0668-A00A-44D7-9371-BEB064C98683}";

// 写进配置文件的位置标识。用固定英文而不是路径：
// 以后路径写法（GUID、shell:: 前缀）改了，配置里的记录也不会失效。
constexpr const wchar_t* kViewLocThisPc       = L"thispc";
constexpr const wchar_t* kViewLocControlPanel = L"controlpanel";
}

// ----------------------------------------------------------------------------
// 当前路径属于哪个"独立记忆视图"的位置
//
// 只比字符串，不查注册表、不调 COM、不探测文件——切目录时也是零成本。
// ----------------------------------------------------------------------------
std::wstring MainWindow::ViewLocationKeyForPath(const std::wstring& path) const
{
    if (_wcsicmp(path.c_str(), shell::kThisPcPath) == 0)
    {
        return kViewLocThisPc;
    }

    const std::wstring root(kControlPanelRootPath);
    if (path.size() >= root.size() &&
        _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0)
    {
        return kViewLocControlPanel;
    }

    return std::wstring();
}

// ----------------------------------------------------------------------------
// 某个位置该用的视图
//
// 配置里记过（用户在里面换过）就用记的；没记过就用它自己的默认 —— 平铺。
// ----------------------------------------------------------------------------
FileListView::ViewMode MainWindow::DefaultViewModeForLocation(const std::wstring& key) const
{
    const int saved = core::Settings::Instance().LocationViewMode(key);
    if (saved >= 0)
    {
        return static_cast<FileListView::ViewMode>(saved);
    }

    // 第一次运行（配置里没有这条）：此电脑、控制面板都是平铺。
    // 此电脑用平铺是为了显示磁盘名、容量和占用条，和系统自带的此电脑一致。
    return FileListView::ViewMode::Tiles;
}

FileListView::ViewMode MainWindow::DefaultViewModeForCurrentPath() const
{
    const std::wstring path = m_history.Current();

    // 1) 此电脑 / 控制面板：用它们自己的记忆，完全不看全局设置
    const std::wstring locKey = ViewLocationKeyForPath(path);
    if (!locKey.empty())
    {
        return DefaultViewModeForLocation(locKey);
    }

    // 2) 其它 Shell 命名空间里的东西（"::" 开头的解析名、shell:xxx、以及那个顶层桌面）
    //    都算「系统文件夹」——这些地方系统默认是小图标视图。
    const bool isShellNamespace = shell::IsTopDesktopPath(path) ||
                                  path.compare(0, 2, L"::") == 0 ||
                                  path.compare(0, 6, L"shell:") == 0;

    // 3) 普通磁盘目录 → 详细信息
    return isShellNamespace ? FileListView::ViewMode::SmallIcon
                            : FileListView::ViewMode::Details;
}

void MainWindow::ApplyViewMode(FileListView::ViewMode mode, bool modeIsUserChoice)
{
    m_viewMode = mode;

    // 当前位置是不是「此电脑」「控制面板」这类独立记忆的位置。
    // 只比路径字符串，零成本。
    const std::wstring locKey = ViewLocationKeyForPath(m_history.Current());

    if (locKey.empty())
    {
        // 普通目录：这就是"全局"那份视图，记下来，
        // 从位置页回到普通目录时按它恢复。
        m_globalViewMode = static_cast<int>(mode);
    }

    if (modeIsUserChoice)
    {
        // 用户从「查看」菜单亲手选的。记到哪儿取决于当前位置：
        //   此电脑 / 控制面板 → 只记这一处，不动全局，也不影响另一处；
        //   普通目录         → 锁住全局（原来那套行为完全不变）。
        if (!locKey.empty())
        {
            core::Settings::Instance().SetLocationViewMode(locKey, static_cast<int>(mode));
        }
        else
        {
            m_viewModeManual = true;
        }
    }

    m_fileListView.SetViewMode(mode);

    // 详细信息视图带列头，其它视图没有列头——状态栏和布局要跟着变
    if (m_hWnd != nullptr)
    {
        LayoutChildren();
    }

    // 切到/切出平铺视图时，一行几格变了，「此电脑」的占位格要重补
    RefreshThisPcGrouping();
}

// ============================================================================
// 阶段 10：窗口状态记住 / 恢复
// ============================================================================

// ----------------------------------------------------------------------------
// 启动时恢复
//
// 只恢复"不影响正确性"的东西：左栏宽度、三列宽度、上次的查看模式。
// 窗口位置大小在 Create() 里就用了（那时候还没有 HWND，没法在这里做）。
//
// 查看模式分两种情况：
//   用户以前从「查看」菜单亲手选过 → 一直用他选的那个；
//   没选过 → 保持"按目录类型自动定"的老逻辑（这里不插手）。
// ----------------------------------------------------------------------------
void MainWindow::RestoreSavedLayout()
{
    core::Settings& settings = core::Settings::Instance();

    // ---- 左栏宽度 ----
    const int paneWidth = settings.PaneWidth();
    if (paneWidth > 0)
    {
        m_navWidth = paneWidth;
        Logger::Info(L"恢复左栏宽度：" + std::to_wstring(paneWidth));
    }

    // ---- 三列固定列宽度 ----
    const core::ColumnWidths& columns = settings.Columns();
    if (columns.valid)
    {
        m_fileListView.SetFixedColumnWidths(columns.time, columns.type, columns.size);
        Logger::Info(L"恢复列宽：修改日期=" + std::to_wstring(columns.time) +
                     L"，类型=" + std::to_wstring(columns.type) +
                     L"，大小=" + std::to_wstring(columns.size));
    }

    // ---- 查看模式 ----
    const int savedView = settings.ViewMode();
    if (savedView >= 0 && savedView <= static_cast<int>(FileListView::ViewMode::Tiles))
    {
        m_viewModeManual = settings.ViewModeManual();
        ApplyViewMode(static_cast<FileListView::ViewMode>(savedView), false);
        Logger::Info(L"恢复查看模式：" + std::to_wstring(savedView) +
                     (m_viewModeManual ? L"（用户选定）" : L"（自动）"));
    }
}

// ----------------------------------------------------------------------------
// 写进配置
//
// 什么时候调：
//   · 关窗口时（最后一次，记窗口位置）
//   · 拖完左栏分隔线松手时（记左栏宽度）
//   · 拖完表头列宽松手时（记三列宽度）
// ----------------------------------------------------------------------------
void MainWindow::SaveWindowState()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    core::Settings& settings = core::Settings::Instance();

    // ---- 窗口位置与大小 ----
    // 用 GetWindowPlacement 而不是 GetWindowRect：
    // 最大化时 GetWindowRect 拿到的是"铺满屏幕"的矩形，直接存下来的话
    // 下次先还原会变成一个占满屏幕的怪尺寸。GetWindowPlacement 的
    // rcNormalPosition 是"还原之后"的那个矩形，正是我们要记的。
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);

    core::WindowState window;

    if (::GetWindowPlacement(m_hWnd, &placement))
    {
        window.x         = placement.rcNormalPosition.left;
        window.y         = placement.rcNormalPosition.top;
        window.width     = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
        window.height    = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
        window.maximized = (placement.showCmd == SW_SHOWMAXIMIZED);
    }
    else
    {
        RECT windowRect = {};
        ::GetWindowRect(m_hWnd, &windowRect);
        window.x      = windowRect.left;
        window.y      = windowRect.top;
        window.width  = windowRect.right - windowRect.left;
        window.height = windowRect.bottom - windowRect.top;
    }

    // 太小或者明显的异常值不存（免得配置文件写坏之后开出一个看不见的窗口）
    window.valid = (window.width >= 320 && window.height >= 240);

    if (window.valid)
    {
        settings.SetWindow(window);
    }

    // ---- 左栏宽度 ----
    if (m_navWidth > 0)
    {
        settings.SetPaneWidth(m_navWidth);
    }

    // ---- 三列固定列宽度 ----
    core::ColumnWidths columns;
    m_fileListView.GetFixedColumnWidths(columns.time, columns.type, columns.size);
    columns.valid = true;
    settings.SetColumns(columns);

    // ---- 查看模式 ----
    // 先看当前站在哪儿：此电脑 / 控制面板只更新它们自己那份，绝不写全局，
    // 否则下次开普通文件夹也会跟着变成平铺或列表。
    const std::wstring current = m_history.Current();
    const std::wstring viewLocKey = ViewLocationKeyForPath(current);
    if (!viewLocKey.empty())
    {
        settings.SetLocationViewMode(viewLocKey, static_cast<int>(m_fileListView.CurrentViewMode()));

        // 站在位置页的时候 m_viewMode 已经是位置页那份了，不能拿它当全局写。
        // 全局那份用 m_globalViewMode（普通目录时记下的）；没记过就不写，
        // 免得把位置页的视图顺手写进全局。
        settings.SetViewMode(m_globalViewMode, m_viewModeManual);
    }
    else
    {
        settings.SetViewMode(static_cast<int>(m_fileListView.CurrentViewMode()), m_viewModeManual);
    }

    // ---- 上次停留的目录 ----
    if (!current.empty())
    {
        settings.SetLastFolder(current);
    }

    if (!settings.Save())
    {
        Logger::Warn(L"窗口状态保存失败（下次打开会是默认的样子）");
        return;
    }

    Logger::Info(L"窗口状态已保存：位置=" + std::to_wstring(window.x) + L"," +
                 std::to_wstring(window.y) + L"，大小=" + std::to_wstring(window.width) + L"x" +
                 std::to_wstring(window.height) +
                 (window.maximized ? L"（最大化）" : L"") +
                 L"，左栏=" + std::to_wstring(m_navWidth) +
                 L"，目录=" + current);
}

// ============================================================================
// 重命名
// ============================================================================

void MainWindow::DoRename(size_t index, const std::wstring& newName)
{
    std::wstring oldPath = m_fileListView.ItemPath(index);
    std::wstring oldName = m_fileListView.ItemName(index);

    if (oldPath.empty() || newName.empty())
    {
        return;
    }

    // 名字压根没变，什么都不用做
    if (_wcsicmp(oldName.c_str(), newName.c_str()) == 0)
    {
        return;
    }

    HRESULT hr = shellops::RenameItem(oldPath, newName, m_hWnd);

    if (FAILED(hr))
    {
        Logger::Error(L"重命名失败：" + oldName + L" → " + newName +
                      L"，" + Logger::FormatHresult(hr));
        // 失败了列表里还显示着编辑器里的新名字，重扫一遍回到真实状态
        RefreshCurrentFolder();
        return;
    }

    Logger::Info(L"重命名成功：" + oldName + L" → " + newName);

    // 记进撤销栈：撤销「重命名」就是把这个新名字改回 oldName
    {
        UndoRecord record;
        record.kind        = UndoKind::Rename;
        record.beforePaths.assign(1, oldPath);
        record.afterPaths.assign(1, NavigationHistory::ParentPath(oldPath) + L"\\" + newName);
        PushUndoRecord(record);
    }

    // 局部更新：只改这一行的名字和路径，再按新的名字重新排一下，不重扫目录
    std::shared_ptr<shell::EnumerateResult> result = CurrentResult();

    if (result != nullptr)
    {
        size_t slash = oldPath.find_last_of(L"\\/");
        std::wstring parent = (slash == std::wstring::npos)
                              ? std::wstring()
                              : oldPath.substr(0, slash + 1);

        for (auto& entry : result->entries)
        {
            if (_wcsicmp(entry.fullPath.c_str(), oldPath.c_str()) == 0)
            {
                entry.name     = newName;
                entry.fullPath = parent + newName;
                break;
            }
        }

        SortEntries();
        m_fileListView.SetSortState(m_sortColumn, m_sortAscending);
        m_fileListView.SetResult(result);
        UpdateTitle();
        UpdateStatusBar();
    }
}

// ============================================================================
// 拖放：注册 / 注销接收目标
//
// 两个窗口各自注册：
//   · 文件列表 —— 拖进来的东西落在「当前目录」
//   · 左栏树   —— 拖到哪个硬盘/文件夹节点上，就落在那个目录
// ============================================================================

void MainWindow::RegisterDropTargets()
{
    // ---- 文件列表 ----
    shell::DropTargetCallbacks listCallbacks;

    listCallbacks.ResolveTargetDirectory = [this](POINT ptClient) -> std::wstring
    {
        // ------------------------------------------------------------------
        // 只读压缩包内部（rar / 7z / tar）：整个列表都不接受放置
        //
        // 返回空串就是对拖拽那边说"这里不能放"：系统给禁止光标、提示条也显示
        // 「不能放到 此处」，松手不落地（和资源管理器表现一致）。
        //
        // 注：曾经改成"接受但不落地"，好处是在压缩包里按住文件往外拖、
        // 光标还压在列表上时不会立刻变成禁止；但那样提示条会显示成「复制到 xxx」，
        // 和实际"放不进去"的语义打架，用户选择回到禁止这一种。
        // zip 内部是可写的，照常接受。
        // ------------------------------------------------------------------
        if (m_currentIsContainer && !m_currentIsZipContainer)
        {
            return std::wstring();
        }

        RECT clientRect = {};
        if (!::GetClientRect(m_fileListView.Handle(), &clientRect))
        {
            return std::wstring();
        }

        // 落在列表客户区外就不接受
        if (ptClient.x < clientRect.left || ptClient.x >= clientRect.right ||
            ptClient.y < clientRect.top || ptClient.y >= clientRect.bottom)
        {
            return std::wstring();
        }

        // 先看是不是压在某一行文件夹上：是就把那个文件夹当落点，
        // 这样"拖到列表里的某个文件夹图标上"就会放进那个文件夹（和资源管理器一致）
        std::wstring folderPath = m_fileListView.HitTestFolderPath(ptClient);
        if (!folderPath.empty())
        {
            return folderPath;
        }

        // 压在文件行上或者压空白处：放进当前目录
        return m_history.Current();
    };

    // 拖动过程中把"鼠标正压着的那个文件夹"高亮出来（系统自带的拖放高亮效果）
    listCallbacks.DragOverFeedback = [this](POINT ptClient, bool acceptable)
    {
        std::wstring folderPath;
        if (acceptable)
        {
            folderPath = m_fileListView.HitTestFolderPath(ptClient);
        }
        m_fileListView.SetDropHighlight(folderPath);
    };

    listCallbacks.PerformDrop = [this](const std::vector<std::wstring>& sources,
                                       const std::wstring& targetDirectory,
                                       DWORD effect, bool createShortcut)
    {
        PerformDropFromOutside(sources, targetDirectory, effect, createShortcut);
    };

    shell::RegisterWindowDropTarget(m_fileListView.Handle(), listCallbacks);

    // ---- 左栏 ----
    shell::DropTargetCallbacks treeCallbacks;

    // 拖到左栏固定区时的"落点标记"：用一个不会和真实路径撞车的假名称，
    // 放到 PerformDrop 里就能分清"这是要固定"还是"这是要放进某个文件夹"
    static const std::wstring kPinnedMarker = L"::PINNED";

    treeCallbacks.ResolveTargetDirectory = [this](POINT ptClient) -> std::wstring
    {
        // 先看是否压在具体节点上（固定区里的每一项也是一个节点）
        //
        // 这里的顺序很关键：固定区不是"整块都算添加到常用"。
        // 压在固定区里的文件夹上，应该跟普通文件夹一样把东西放进去；
        // 只有压在固定区的空白处，才是"添加一个常用项"。
        std::wstring path = m_navigationPane.HitTestPath(ptClient);

        if (!path.empty())
        {
            DWORD attributes = ::GetFileAttributesW(path.c_str());

            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return path;   // 目录：落点就是这个目录
            }

            // 压在了一个「文件」节点上（比如固定进来的程序、文档）：往里放没有意义
            return std::wstring();
        }

        // 没压在任何节点上：落在固定区空白处 → 添加一个常用项
        if (m_navigationPane.IsInPinnedArea(ptClient))
        {
            return kPinnedMarker;
        }

        return std::wstring();
    };

    // 固定区空白处的提示语不是"复制到 xxx"，而是"添加到常用"
    treeCallbacks.ResolveDropHint = [this](POINT ptScreen, std::wstring& actionText,
                                           std::wstring& targetName) -> bool
    {
        POINT clientPoint = ptScreen;
        ::ScreenToClient(m_navigationPane.Handle(), &clientPoint);

        // 压在某个节点上：交给默认的「复制到/移动到 目录名」
        if (!m_navigationPane.HitTestPath(clientPoint).empty())
        {
            return false;
        }

        if (!m_navigationPane.IsInPinnedArea(clientPoint))
        {
            return false;
        }

        actionText = L"添加到";
        targetName = L"常用";
        return true;
    };

    treeCallbacks.PerformDrop = [this](const std::vector<std::wstring>& sources,
                                       const std::wstring& targetDirectory,
                                       DWORD effect, bool createShortcut)
    {
        if (targetDirectory == kPinnedMarker)
        {
            // 拖进固定区：只记一个快捷入口，文件本身原地不动
            AddPinnedItems(sources);
            return;
        }

        PerformDropFromOutside(sources, targetDirectory, effect, createShortcut);
    };

    // 拖动过程中的反馈
    treeCallbacks.DragOverFeedback = [this](POINT ptClient, bool acceptable)
    {
        // 固定区没有"放进哪个目录"的概念，这里不点亮任何行
        if (acceptable && m_navigationPane.IsInPinnedArea(ptClient))
        {
            m_navigationPane.SetDropHighlight(nullptr);
            return;
        }

        HTREEITEM item = acceptable ? m_navigationPane.HitTestDroppableItem(ptClient) : nullptr;
        m_navigationPane.SetDropHighlight(item);
    };

    shell::RegisterWindowDropTarget(m_navigationPane.Handle(), treeCallbacks);

    // ---- 地址栏（面包屑）----
    //
    // 地址栏的每一段都是一个真目录（C: > Users > xxx），所以拖文件压在某一段上，
    // 语义就是"放进那一段对应的目录"，跟资源管理器把文件拖到面包屑上一样。
    // 段与段之间的箭头、末尾空白不接受。
    shell::DropTargetCallbacks addressCallbacks;

    addressCallbacks.ResolveTargetDirectory = [this](POINT ptClient) -> std::wstring
    {
        return m_addressBar.HitTestPath(ptClient);
    };

    addressCallbacks.PerformDrop = [this](const std::vector<std::wstring>& sources,
                                          const std::wstring& targetDirectory,
                                          DWORD effect, bool createShortcut)
    {
        PerformDropFromOutside(sources, targetDirectory, effect, createShortcut);
    };

    addressCallbacks.DragOverFeedback = [this](POINT ptClient, bool acceptable)
    {
        // 拖动时把落点那一段用浅蓝点亮，松手前就能看清会落到哪
        m_addressBar.UpdateDropHighlight(ptClient, acceptable);
    };

    shell::RegisterWindowDropTarget(m_addressBar.Handle(), addressCallbacks);
}

// ============================================================================
// 左栏固定区
// ============================================================================

// 左栏点了一项：是目录就进去，是程序 / 快捷方式就启动它
void MainWindow::OpenPinnedOrFolder(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }

    DWORD attributes = ::GetFileAttributesW(path.c_str());

    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        // 左栏最上面那几个「常用位置」（桌面/下载/文档/图片/音乐）：
        // 点它们进去以后，地址栏显示名字（「文档」）、向上回「主文件夹」，
        // 和从主文件夹里双击进去的表现一致。
        // 其它目录（硬盘、固定区里的文件夹）不受影响，向上还是走真实父目录。
        std::wstring displayName;

        if (shell::KnownFolderDisplayName(path, displayName))
        {
            RecordNavigationOrigin(path, displayName, shell::kTopDesktopPath);
        }

        NavigateToPath(path);
        return;
    }

    // 不是目录 → 当程序启动（.exe / .lnk 交给系统，效果和双击一样）
    HINSTANCE result = ::ShellExecuteW(m_hWnd, L"open", path.c_str(),
                                      nullptr, nullptr, SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        Logger::Warn(L"左栏启动失败：" + path);
    }
    else
    {
        Logger::Info(L"左栏启动：" + path);
    }
}

void MainWindow::AddPinnedItems(const std::vector<std::wstring>& sources)
{
    bool changed = false;
    int  ignored = 0;

    for (const std::wstring& path : sources)
    {
        DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            ++ignored;
            continue;
        }

        core::PinnedItem item;
        item.path     = path;
        item.isFolder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        // 不再限制类型：文件夹、程序、普通文档都能固定到常用（用户要求放开）

        // 显示名取路径末段；"C:\" 这种把尾部反斜杠去掉
        size_t lastSlash = path.find_last_of(L'\\');
        item.name = (lastSlash == std::wstring::npos) ? path : path.substr(lastSlash + 1);

        while (!item.name.empty() && item.name.back() == L'\\')
        {
            item.name.pop_back();
        }

        if (item.name.empty())
        {
            ++ignored;
            continue;
        }

        if (core::Settings::Instance().AddPinned(item))
        {
            changed = true;
            Logger::Info(L"固定：" + item.name + L"（" + item.path + L"）");
        }
    }

    if (!changed)
    {
        Logger::Info(L"拖进固定区的内容里没有可固定的项（忽略 " +
                     std::to_wstring(ignored) + L" 项）");
        return;
    }

    core::Settings::Instance().Save();

    // 整栏重建后把选中状态补回来（重建会把选中清掉）
    m_navigationPane.SetPinnedItems(core::Settings::Instance().PinnedItems());
    m_navigationPane.SelectFolderPath(m_history.Current());
}

void MainWindow::UnregisterDropTargets()
{
    shell::UnregisterWindowDropTarget(m_fileListView.Handle());
    shell::UnregisterWindowDropTarget(m_navigationPane.Handle());
    shell::UnregisterWindowDropTarget(m_addressBar.Handle());
}

// ============================================================================
// 拖放：真正把东西放进去
//
// 三种情况：
//   · 按住 Alt 拖     → 在目标目录里建快捷方式（.lnk）
//   · 同盘拖 / 按 Shift → 移动
//   · 跨盘拖 / 按 Ctrl  → 复制
// 具体用哪个效果，放置目标那边已经算好了，这里照着执行。
// ============================================================================

void MainWindow::PerformDropFromOutside(const std::vector<std::wstring>& sources,
                                        const std::wstring&              targetDirectory,
                                        DWORD                            effect,
                                        bool                             createShortcut)
{
    if (sources.empty() || targetDirectory.empty())
    {
        return;
    }

    // 把「源就在目标目录里」的项剔掉：自己往自己里放没有意义，还会弹一堆错。
    //
    // 但要注意「复制」这种效果是有意义的：资源管理器在同目录里 Ctrl 拖，
    // 会当场生成一份「xxx - 副本」，我们也要一样，所以只有「移动」才剔。
    const bool copying = (effect & DROPEFFECT_COPY) != 0;

    std::vector<std::wstring> usableSources;
    usableSources.reserve(sources.size());

    for (const auto& source : sources)
    {
        const bool sameDirectory =
            (::_wcsicmp(NavigationHistory::ParentPath(source).c_str(),
                        targetDirectory.c_str()) == 0);

        if (sameDirectory && !copying)
        {
            continue;
        }

        // 目标就是源自己、或者目标在源的里面（拖文件夹到自己身上 / 自己肚子里）：
        // 复制和移动在系统层面都做不了（IFileOperation 会报"不能把文件夹移到自身或子文件夹"
        // 并弹它自己的错误框），所以这里再剔一遍。
        // 建快捷方式（Alt）例外——在文件夹自己里面建一个指向自己的 .lnk 是合法的。
        if (!createShortcut && shell::IsSelfOrDescendantPath(targetDirectory, source))
        {
            continue;
        }

        usableSources.push_back(source);
    }

    if (usableSources.empty())
    {
        Logger::Warn(L"拖放的源和目标是同一个目录（移动对自己没意义），忽略");
        return;
    }

    // 只读压缩包内部不接受放置。
    // 界面上（列表的落点判定）已经给了禁止光标，这里是第二道保险：
    // 万一从别的入口（左栏树、以后的新入口）进来，也不要真去白跑一次文件操作。
    if (!shell::IsWritableTargetDirectory(targetDirectory))
    {
        Logger::Warn(L"拖放目标是只读压缩包内部，忽略：" + targetDirectory);
        return;
    }

    HWND owner = m_hWnd;

    Logger::Info(L"接收拖放：" + std::to_wstring(usableSources.size()) + L" 项 → " +
                 targetDirectory + (createShortcut ? L"（建快捷方式）" : L"") +
                 L"，效果=" + std::to_wstring(effect));

    // ------------------------------------------------------------
    // 建快捷方式（Alt 拖动）
    // ------------------------------------------------------------
    if (createShortcut)
    {
        m_lastOperation  = FileOperationKind::Copy;   // 完成后按"复制"的路径刷新
        m_operationPaths.clear();

        RunFileOperationAsync([usableSources, targetDirectory]() -> HRESULT
        {
            HRESULT lastResult = S_OK;

            for (const auto& source : usableSources)
            {
                // 取文件名（去掉目录部分）
                size_t slashPos = source.find_last_of(L"\\/");
                std::wstring fileName =
                    (slashPos == std::wstring::npos) ? source : source.substr(slashPos + 1);

                std::wstring linkPath = targetDirectory + L"\\" + fileName + L".lnk";

                // 重名就往后加 (2)(3)…，不覆盖已有的快捷方式
                int suffix = 2;
                while (::GetFileAttributesW(linkPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    linkPath = targetDirectory + L"\\" + fileName +
                               L" (" + std::to_wstring(suffix) + L").lnk";
                    ++suffix;
                }

                HRESULT hr = shellops::CreateShortcut(source, linkPath);
                if (FAILED(hr))
                {
                    lastResult = hr;
                    Logger::Warn(L"建快捷方式失败：" + linkPath +
                                 L"，" + Logger::FormatHresult(static_cast<long>(hr)));
                }
            }

            return lastResult;
        });

        m_selectNewAfterEnum = true;
        return;
    }

    // ------------------------------------------------------------
    // 移动 / 复制
    // ------------------------------------------------------------
    bool isMove = (effect == DROPEFFECT_MOVE);

    m_lastOperation  = isMove ? FileOperationKind::Move : FileOperationKind::Copy;
    m_operationPaths = usableSources;
    m_operationTargetDirectory = targetDirectory;   // 撤销时要按这个目录算"放到了哪"

    RunFileOperationAsync([usableSources, targetDirectory, owner, isMove]() -> HRESULT
    {
        return isMove
               ? shellops::MoveItems(usableSources, targetDirectory, owner)
               : shellops::CopyItems(usableSources, targetDirectory, owner);
    });

    // 目标目录正好是当前显示的目录，完成刷新后要选中新出现的项
    if (::_wcsicmp(targetDirectory.c_str(), m_history.Current().c_str()) == 0)
    {
        m_selectNewAfterEnum = true;
    }
}

void MainWindow::DoNewFolder()
{
    std::wstring current = m_history.Current();
    if (current.empty())
    {
        return;
    }

    std::wstring created;
    HRESULT hr = shellops::CreateNewFolder(current, created);

    if (SUCCEEDED(hr))
    {
        Logger::Info(L"新建文件夹成功：" + created);

        // 记进撤销栈：撤销「新建」= 把这个刚建的文件夹删掉
        {
            UndoRecord record;
            record.kind = UndoKind::Create;
            record.afterPaths.assign(1, created);
            PushUndoRecord(record);
        }

        // 不重扫整个目录：直接往列表里插一条，界面立刻更新
        // 目录名是最后一段
        std::wstring name = created;
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            name = name.substr(slash + 1);
        }

        shell::ShellEntry entry;
        entry.name     = name;
        entry.fullPath = created;
        entry.isFolder = true;
        entry.canEnter = true;    // 新建的是真文件夹，双击要能进去
        entry.size     = 0;
        ::GetSystemTimeAsFileTime(&entry.modified);

        m_fileListView.InsertEntry(entry);
        UpdateTitle();
        UpdateStatusBar();

        // 和「新建」子菜单一样：建完立刻选中并进入改名，用户不用再按 F2
        m_fileListView.SelectPaths({ created });
        m_fileListView.BeginRenameSelected();

        Logger::Info(L"新建后局部插入 1 行（未重扫目录）：" + created);
    }
    else
    {
        Logger::Error(L"新建文件夹失败：" + Logger::FormatHresult(hr));
    }
}

// ============================================================================
// 「此电脑」分组
//
// entries：排序之后的原始条目（进来时不含标题行/占位格）
// perRow ：平铺视图一行能并排几格；其它视图传 1（不需要占位格）
//
// 分组依据是驱动器类型：DRIVE_REMOTE(4) 是网络盘，其余本地盘/光驱/U 盘
// 都归「设备和驱动器」。某组为空就不插那行标题，免得出现空组。
//
// 占位格是给平铺视图用的：标题只占一格，后面补 perRow-1 个空格把它顶成
// 独占一行；每组末尾如果没把行填满，也要补齐，否则下一组标题会挤在半行上。
// ============================================================================
static void BuildThisPcGrouped(std::vector<shell::ShellEntry>& entries, int perRow)
{
    if (perRow < 1)
    {
        perRow = 1;
    }

    std::vector<shell::ShellEntry> localDrives;    // 设备和驱动器
    std::vector<shell::ShellEntry> networkDrives;  // 网络位置

    for (auto& entry : entries)
    {
        // 幂等：传进来的可能是"上次已经分好组"的列表（含标题行和占位格）。
        // 这里先剔掉它们再重新分，否则标题会被当成真实条目再插一遍——
        // 列表视图下就会看到两行同名的「设备和驱动器」「网络位置」。
        if (entry.isGroupHeader || entry.isSpacer)
        {
            continue;
        }

        if (entry.driveType == DRIVE_REMOTE)
        {
            networkDrives.push_back(std::move(entry));
        }
        else
        {
            localDrives.push_back(std::move(entry));
        }
    }

    std::vector<shell::ShellEntry> grouped;
    grouped.reserve(entries.size() + 8);

    // 插一个组标题；平铺视图下再补上占位格，把这一行剩下的位置填掉
    auto pushHeader = [&grouped, perRow](const wchar_t* text)
    {
        shell::ShellEntry header;
        header.name          = text;
        header.isGroupHeader = true;
        grouped.push_back(std::move(header));

        for (int i = 1; i < perRow; ++i)
        {
            shell::ShellEntry spacer;
            spacer.isSpacer = true;
            grouped.push_back(std::move(spacer));
        }
    };

    // 这一组的最后一行如果没填满，补几个占位格；
    // 否则下一组的标题会挤到这一行的空位上
    auto padLastRow = [&grouped, perRow](size_t itemCount)
    {
        const int remainder = static_cast<int>(itemCount % static_cast<size_t>(perRow));

        if (remainder != 0)
        {
            for (int i = remainder; i < perRow; ++i)
            {
                shell::ShellEntry spacer;
                spacer.isSpacer = true;
                grouped.push_back(std::move(spacer));
            }
        }
    };

    if (!localDrives.empty())
    {
        pushHeader(L"设备和驱动器");

        for (auto& entry : localDrives)
        {
            grouped.push_back(std::move(entry));
        }

        padLastRow(localDrives.size());
    }

    if (!networkDrives.empty())
    {
        pushHeader(L"网络位置");

        for (auto& entry : networkDrives)
        {
            grouped.push_back(std::move(entry));
        }
    }

    // 顺手记一笔，方便核对"到底枚举出几项、分了几组"
    Logger::Info(L"此电脑分组完成：设备 " + std::to_wstring(localDrives.size()) +
                 L" 个、网络 " + std::to_wstring(networkDrives.size()) +
                 L" 个、一行 " + std::to_wstring(perRow) + L" 格、总行数 " +
                 std::to_wstring(grouped.size()));

    entries = std::move(grouped);
}

// ---------------------------------------------------------------------------
// 窗口尺寸 / 视图变化之后，按控件真实列数重排「此电脑」的分组标题行
//
// 只在"列数变了"的时候才重排（拖窗口跨过一格宽度边界时才触发），
// 平时什么都不做。
// ---------------------------------------------------------------------------
void MainWindow::RefreshThisPcGrouping()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    if (_wcsicmp(m_history.Current().c_str(), shell::kThisPcPath) != 0)
    {
        return;
    }

    std::shared_ptr<shell::EnumerateResult> result = CurrentResult();
    if (result == nullptr || result->entries.empty())
    {
        return;
    }

    const bool tilesView = (m_fileListView.CurrentViewMode() == FileListView::ViewMode::Tiles);
    const int  perRow    = tilesView ? m_fileListView.TilesPerRow() : 1;

    if (perRow == m_thisPcPerRow)
    {
        return;     // 列数没变，不用动
    }

    // 把上次插的标题/占位格剔掉，只留真实条目，再按新列数重建
    std::vector<shell::ShellEntry> raw;
    raw.reserve(result->entries.size());

    for (const auto& entry : result->entries)
    {
        if (!entry.isGroupHeader && !entry.isSpacer)
        {
            raw.push_back(entry);
        }
    }

    if (raw.empty())
    {
        return;
    }

    BuildThisPcGrouped(raw, perRow);

    result->entries = std::move(raw);
    m_thisPcPerRow  = perRow;

    // SetResult 内部会自己记住选中项和滚动位置，再按路径恢复
    m_fileListView.SetResult(result);

    Logger::Info(L"此电脑分组按新列数重排：一行 " + std::to_wstring(perRow) + L" 格");
}

void MainWindow::OnEnumerationDone()
{
    std::shared_ptr<shell::EnumerateResult> result;
    double elapsedMs = 0.0;

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (!m_resultReady || m_result == nullptr)
        {
            return;
        }
        result    = m_result;
        elapsedMs = m_elapsedMs;
    }

    // 记下"当前显示的是不是压缩包内部"：右键菜单、剪切、删除都按它决定能力
    // （zip 内部能剪能删，rar/7z 这类只读，菜单只给「打开 / 复制」）
    m_currentIsContainer    = result->isContainer;
    m_currentIsZipContainer = result->isZipContainer;

    // 按当前排序状态排一遍（默认名称升序）
    std::sort(result->entries.begin(), result->entries.end(),
              [this](const shell::ShellEntry& left, const shell::ShellEntry& right)
              {
                  return CompareEntries(left, right, m_sortColumn, m_sortAscending);
              });
    m_fileListView.SetSortState(m_sortColumn, m_sortAscending);

    // ------------------------------------------------------------
    // 「此电脑」加分组标题（设备和驱动器 / 网络位置）
    //
    // 列表控件在虚拟模式（LVS_OWNERDATA）下不支持原生分组，所以改成在数据里
    // 插「假标题行」，由列表控件把这种行画成标题（见 FileListView 的
    // CDDS_ITEMPREPAINT 分支）。
    //
    // 平铺视图下标题只占一格，必须在后面补「占位格」把这一行剩下的格子填掉，
    // 标题才能像系统那样独占一行、后面的卡片才不会跑到标题右边去。
    // 补几个格 = 控件这一行能并排几个（TilesPerRow，按真实间距算，不写死 300）。
    //
    // 顺便记下这次用的列数：窗口一改宽度列数就变，RefreshThisPcGrouping
    // 会拿这个值比较，变了就重排一次。
    // ------------------------------------------------------------
    int thisPcPerRow = 1;

    // 每次都重建（BuildThisPcGrouped 自己会先剔掉上次插的标题和占位格，幂等）。
    // 不再靠"第一项是不是标题"来判断：排序会把标题排到中间，判断会失效，
    // 于是标题被当真实项再分一次，列表视图下就出现两行同名标题。
    if (_wcsicmp(result->path.c_str(), shell::kThisPcPath) == 0)
    {
        // 视图是在这段之后才 ApplyViewMode 的，所以这里不能只看 m_viewMode，
        // 还要按"当前目录默认该用哪种视图"算一遍，否则首次进此电脑会按
        // 详细信息算，占位格就补错了。
        bool tilesView = (m_viewMode == FileListView::ViewMode::Tiles);
        if (DefaultViewModeForCurrentPath() == FileListView::ViewMode::Tiles)
        {
            tilesView = true;
        }

        thisPcPerRow = tilesView ? m_fileListView.TilesPerRow() : 1;

        BuildThisPcGrouped(result->entries, thisPcPerRow);
    }

    m_thisPcPerRow = thisPcPerRow;

    // 进入/刷新目录时定一次视图：
    //   此电脑 / 控制面板 → 永远用它们自己那份记忆（不受全局"用户选过"影响）
    //   普通目录         → 用户手动选过就按他那套（从位置页回来时恢复到全局那份），
    //                      没选过就按目录类型给默认
    const std::wstring viewLocKey = ViewLocationKeyForPath(m_history.Current());
    if (!viewLocKey.empty())
    {
        ApplyViewMode(DefaultViewModeForCurrentPath(), false);
    }
    else if (!m_viewModeManual)
    {
        ApplyViewMode(DefaultViewModeForCurrentPath(), false);
    }
    else if (m_globalViewMode >= 0 &&
             static_cast<int>(m_viewMode) != m_globalViewMode)
    {
        // 刚从「此电脑」/「控制面板」出来：m_viewMode 还是那两处的模式，
        // 要按普通目录自己的那份恢复，否则会被带跑。
        ApplyViewMode(static_cast<FileListView::ViewMode>(m_globalViewMode), false);
    }

    // ------------------------------------------------------------
    // 如果是"粘贴完成后的那次刷新"，把新出现的项选中（和资源管理器一致）
    // 必须在 MergeResult 之前抓旧路径表，之后就没得比了
    // ------------------------------------------------------------
    std::vector<std::wstring>        pathsBefore;
    std::unordered_set<std::wstring> pathSetBefore;
    bool                             selectNew = m_selectNewAfterEnum;
    m_selectNewAfterEnum = false;

    if (selectNew)
    {
        pathsBefore = m_fileListView.AllPaths();
        pathSetBefore.reserve(pathsBefore.size());

        for (const auto& oldPath : pathsBefore)
        {
            std::wstring lowered = oldPath;
            ::_wcslwr_s(lowered.data(), lowered.size() + 1);
            pathSetBefore.insert(std::move(lowered));
        }
    }

    m_fileListView.MergeResult(result);   // 最小范围更新，只重绘变化的部分

    if (selectNew && !pathsBefore.empty())
    {
        std::vector<std::wstring> addedPaths;

        for (const auto& entry : result->entries)
        {
            std::wstring lowered = entry.fullPath;
            ::_wcslwr_s(lowered.data(), lowered.size() + 1);

            if (pathSetBefore.find(lowered) == pathSetBefore.end())
            {
                addedPaths.push_back(entry.fullPath);
            }
        }

        if (!addedPaths.empty())
        {
            m_fileListView.SelectPaths(addedPaths);
        }
    }

    // ------------------------------------------------------------
    // 启动时命令行传进来的是「文件」：
    // 目录已经打开了，现在把这个文件选中并滚到可见。
    // 只做一次 —— 选完立刻清空，之后 F5 刷新、文件变化都不会再抢用户的选中。
    // ------------------------------------------------------------
    if (!m_startupSelectFile.empty())
    {
        const std::wstring selectPath = m_startupSelectFile;
        m_startupSelectFile.clear();

        m_fileListView.SelectPaths({ selectPath });
        m_fileListView.EnsureVisiblePath(selectPath);

        Logger::Info(L"启动时选中传入的文件：" + selectPath);
    }

    // 标题也只在内容真的变了才重设（道理同 UpdateTitle：避免任务栏闪）
    {
        // 项目数一律走 RealItemCount：分组标题行和占位格都不算（口径只有一处，免得对不上）
        std::wstring title = std::wstring(kWindowTitleBase) + L" - " + DisplayNameFor(result->path) +
                             L"（" + std::to_wstring(RealItemCount()) + L" 项）";

        if (title != m_lastTitle)
        {
            m_lastTitle = title;
            ::SetWindowTextW(m_hWnd, title.c_str());
        }
    }

    // 左栏选中状态跟着当前目录走：
    // 当前目录在左栏里就选中它；不在（进了硬盘里的子文件夹）就清空选中。
    // 清空是为了让"再点桌面"能生效——见 NavigationPane::SelectFolderPath 的说明
    m_navigationPane.SelectFolderPath(result->path);

    UpdateNavigationState();
    UpdateStatusBar();

    Logger::Info(L"枚举完成：路径=" + result->path +
                 L"，HRESULT=" + Logger::FormatHresult(result->hr) +
                 L"，条目数=" + std::to_wstring(result->entries.size()) +
                 L"（文件夹 " + std::to_wstring(result->folderCount) +
                 L"，文件 " + std::to_wstring(result->fileCount) +
                 L"），耗时 " + std::to_wstring(static_cast<int>(elapsedMs)) + L" ms");

    if (FAILED(static_cast<HRESULT>(result->hr)))
    {
        Logger::Error(L"枚举失败，路径=" + result->path);
        return;
    }

    // 枚举成功：开始盯着这个目录，别的程序改动它时自动刷新
    // （已经在盯同一个目录的话内部会直接返回，不会反复拆建线程）
    m_watcher.Start(result->path, m_hWnd, WM_APP_FOLDER_CHANGED);

    size_t logCount = (result->entries.size() < kMaxLogItems) ? result->entries.size() : kMaxLogItems;
    for (size_t i = 0; i < logCount; ++i)
    {
        const auto& entry = result->entries[i];
        Logger::Info(std::wstring(entry.isFolder ? L"[目录] " : L"[文件] ") +
                     entry.name +
                     L"  大小=" + format::SizeText(entry.size, entry.isFolder) +
                     L"  修改=" + format::FileTimeText(entry.modified));
    }
    if (result->entries.size() > logCount)
    {
        Logger::Info(L"... 其余 " + std::to_wstring(result->entries.size() - logCount) + L" 条未记录");
    }
}

// ============================================================================
// 交互
// ============================================================================

void MainWindow::OnItemActivated(size_t index)
{
    std::shared_ptr<shell::EnumerateResult> result;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        result = m_result;
    }

    if (result == nullptr || index >= result->entries.size())
    {
        return;
    }

    const shell::ShellEntry& entry = result->entries[index];

    // 分组标题行（如「此电脑」的「设备和驱动器」）不是真文件，双击它什么都不做
    if (entry.isGroupHeader)
    {
        return;
    }

    // 平铺视图里的占位格同理，双击没有任何反应
    if (entry.isSpacer)
    {
        return;
    }

    // 判断要不要"进去"，看 canEnter 而不是 isFolder：
    // zip / rar / 7z / cab 这类压缩包显示上算文件（isFolder=false），
    // 但 Shell 能当容器打开，双击应该进去浏览，和资源管理器一致。
    if (entry.canEnter && !entry.fullPath.empty())
    {
        // 记一条"双击的是哪一项"：以后排查"双击没反应"时，日志里这条后面
        // 紧跟的「导航到 / 按 PIDL 打开成功 / 打开失败」就是它的结果，
        // 一眼能对上是哪个项出的问题。
        Logger::Info(L"双击：[" + entry.name + L"] " + entry.fullPath);

        // ------------------------------------------------------------
        // 压缩包（zip / rar / 7z…）：双击走「打开方式」账本
        //
        // 和普通文件一样：
        //   · 设过默认程序 → 按设的那个打开（本程序 = 进去浏览，别的程序 = 启动它）；
        //   · 没设过       → 弹打开方式窗口，默认选中「本程序」这一项，
        //                    用户直接点「打开」就进去浏览。
        // ------------------------------------------------------------
        if (entry.isContainerFile)
        {
            OpenContainerEntry(entry);
            return;
        }

        // Shell 命名空间里的虚拟项（「::{GUID}…」「shell:…」）不一定真能"进去"：
        // 控制面板里「恢复」「凭据管理器」「Windows Defender 防火墙」这些，
        // 属性上是文件夹，其实是 applet（双击应该弹设置界面），往里进只会得到一个空列表，
        // 用户看到的就是"双击打不开、只有个空文件夹"。
        // 所以先探一下它是"容器"还是"applet"：容器就导航进去（空的也进去，显示空列表），
        // applet 就交给系统按"打开"处理——和资源管理器双击的行为一致。
        const bool isShellNamespace = (entry.fullPath.compare(0, 2, L"::") == 0) ||
                                      (entry.fullPath.compare(0, 6, L"shell:") == 0);

        if (isShellNamespace && !shell::ShellItemIsContainer(entry.fullPath))
        {
            // 交给系统按"默认动词"打开：和资源管理器双击走同一套路径，
            // 控制面板 applet、设备和打印机里的打印机都能正确打开。
            //
            // 顺序很重要：
            //   1) 先按 PIDL 打开 —— 控制面板里「字体」「恢复」「设备管理器」这些项
            //      的解析名是 "::{26EE0668-…}\0\字体"（末尾是显示名，不是 GUID），
            //      Shell 拿这个字符串根本解析不回来（实测 NameSpace 返回 null），
            //      所以字符串那条路对它们必然失败，只有 PIDL 管用。
            //   2) 再按路径字符串打开 —— 兼容"路径本身可解析"的项。
            //   3) 最后退 ShellExecuteW。
            bool opened = shell::OpenShellItemByPidl(m_hWnd, entry.pidl);

            if (!opened)
            {
                opened = shell::OpenShellItem(m_hWnd, entry.fullPath);
            }

            if (!opened)
            {
                HINSTANCE openResult = ::ShellExecuteW(m_hWnd, L"open", entry.fullPath.c_str(),
                                                       nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(openResult) <= 32)
                {
                    Logger::Error(L"打开虚拟项失败，路径=" + entry.fullPath);
                }
            }
            return;
        }

        // 记下"这个目录是从哪儿点进来的、叫什么名字"：
        // 地址栏会写这一项的名字（「文档」），向上按钮回我们进来之前的那个目录
        // （「主文件夹」），跟系统资源管理器一致。
        RecordNavigationOrigin(entry.fullPath, entry.name);

        NavigateToPath(entry.fullPath);
        return;
    }

    if (!entry.isFolder && !entry.fullPath.empty())
    {
        // 同上：日志里留下"双击了哪一项"，后面跟的就是它的打开结果
        Logger::Info(L"双击：[" + entry.name + L"] " + entry.fullPath);

        // ----------------------------------------------------------
        // 压缩包容器内部的文件：先解成只读临时文件，再打开那个文件
        //
        // 为什么不能直接打开：容器里的路径（C:\a.rar\inner.txt）磁盘上并不存在，
        // 交给 Shell 会失败（rar/7z/tar 实测打不开，zip 也不稳）。
        // 资源管理器也是这么干的——先解到 %TEMP%\ 下的临时目录，再打开解出来的那个文件。
        //
        // 判定不用 m_currentIsContainer：那个标记只覆盖"当前目录本身在容器里"的情况，
        // 这里按路径自己往上找容器根（包内任意层级都认）。
        // ----------------------------------------------------------
        if (shell::IsPathInsideContainer(entry.fullPath))
        {
            std::wstring extractError;
            const std::wstring extractedPath = ExtractContainerEntryToTemp(entry.fullPath,
                                                                          extractError);

            if (extractedPath.empty())
            {
                Logger::Error(L"提取容器内部文件失败：" + entry.fullPath + L"，原因：" + extractError);

                std::wstring message = L"打开失败：\r\n" + extractError;
                ::MessageBoxW(m_hWnd, message.c_str(), L"打开文件", MB_OK | MB_ICONWARNING);
                return;
            }

            Logger::Info(L"已提取到临时文件：" + extractedPath);

            // 解出来就是普通文件了，之后和普通文件走同一套：
            // 先查「打开方式」账本（比如 .pptx 会按账本用 WPS 打开），没有就交给系统。
            const std::wstring extractedExtension = shell::ExtensionKeyForPath(extractedPath);

            if (shell::IsExtensionHandled(extractedExtension))
            {
                if (TryOpenFileWithOurChoice(extractedPath, extractedExtension))
                {
                    return;
                }
            }

            HINSTANCE extractedOpenResult = ::ShellExecuteW(m_hWnd, L"open", extractedPath.c_str(),
                                                            nullptr, nullptr, SW_SHOWNORMAL);

            if (reinterpret_cast<INT_PTR>(extractedOpenResult) <= 32)
            {
                Logger::Error(L"打开提取出来的临时文件失败：" + extractedPath);
                ::MessageBoxW(m_hWnd,
                              L"打开失败：系统里没有能打开这个文件的程序。",
                              L"打开文件", MB_OK | MB_ICONWARNING);
            }
            return;
        }

        // ----------------------------------------------------------
        // 先走我们自己记的「打开方式」账本
        //
        // 这一步是"不被系统劫持"的核心：只要这个扩展名在我们账户本里设过默认程序，
        // 双击就永远用账本里那个——系统关联怎么改都影响不到。
        // 第一次用这个扩展名时会弹一次设置窗口，设好就固定。
        // ----------------------------------------------------------
        const bool isShellFileNamespace = (entry.fullPath.compare(0, 2, L"::") == 0) ||
                                          (entry.fullPath.compare(0, 6, L"shell:") == 0);

        if (!isShellFileNamespace)
        {
            const std::wstring extension = shell::ExtensionKeyForPath(entry.fullPath);

            // 特殊类型（exe/lnk/bat…）不接管，照旧直接执行
            if (shell::IsExtensionHandled(extension))
            {
                if (TryOpenFileWithOurChoice(entry.fullPath, extension))
                {
                    return;
                }
            }
        }

        // 普通文件也走系统默认动词：这样"设备和打印机"里的打印机、
        // 带特殊外壳扩展的文档等非常规项也能打开（ShellExecute 对它们会失败）。
        // 同样先试 PIDL（命名空间目录里的项才带 PIDL），再退路径字符串。
        bool opened = shell::OpenShellItemByPidl(m_hWnd, entry.pidl);

        if (!opened)
        {
            opened = shell::OpenShellItem(m_hWnd, entry.fullPath);
        }

        if (!opened)
        {
            HINSTANCE openResult = ::ShellExecuteW(m_hWnd, L"open", entry.fullPath.c_str(),
                                                   nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(openResult) <= 32)
            {
                Logger::Error(L"打开文件失败，路径=" + entry.fullPath);
            }
        }
        return;
    }

    Logger::Warn(L"双击的项暂不支持打开：" + entry.name);
}

// ============================================================================
// 布局与状态刷新
// ============================================================================

void MainWindow::LayoutChildren()
{
    if (m_hWnd == nullptr || m_hStatusBar == nullptr)
    {
        return;
    }

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);
    int clientWidth  = clientRect.right  - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;

     int margin     = Scale(8);
    int buttonWidth  = Scale(34);
    int buttonHeight = Scale(32);
    int gap        = Scale(4);
    int navHeight  = Scale(40);
    int addressHeight = Scale(30);

    // 状态栏自己会贴到底部，先让它调整，再取高度
    ::SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);
    RECT statusRect = {};
    ::GetWindowRect(m_hStatusBar, &statusRect);
    int statusHeight = statusRect.bottom - statusRect.top;

    // ------------------------------------------------------------------
    // 子窗口批量布局
    //
    // 以前是一个控件一次 SetWindowPos：拖动窗口时每动一个控件都各自重绘一轮，
    // 父窗口再刷一次背景，一帧要刷十来遍，手感就很"重"。
    // 改成 Begin/Defer/End 一次性提交：中间不重绘，最后统一刷。
    // ------------------------------------------------------------------
    HDWP deferredLayout = ::BeginDeferWindowPos(12);

    // 把控件塞进批量布局；万一中途失败，就先把已排队的提交、再单独摆这一个，
    // 保证不会漏掉任何控件（宁慢勿错）。
    auto placeWindow = [&deferredLayout](HWND hwnd, int x, int y, int w, int h)
    {
        if (hwnd == nullptr)
        {
            return;
        }

        if (deferredLayout == nullptr)
        {
            ::SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }

        HDWP next = ::DeferWindowPos(deferredLayout, hwnd, nullptr,
                                     x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

        if (next == nullptr)
        {
            ::EndDeferWindowPos(deferredLayout);
            deferredLayout = nullptr;
            ::SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }

        deferredLayout = next;
    };

    // ---- 导航按钮 ----
    int buttonY = (navHeight - buttonHeight) / 2;
    int cursorX = margin;

    placeWindow(m_homeButton.Handle(), cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    placeWindow(m_backButton.Handle(), cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    placeWindow(m_forwardButton.Handle(), cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    placeWindow(m_upButton.Handle(), cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    placeWindow(m_refreshButton.Handle(), cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + Scale(8);   // 按钮组和地址栏之间留宽一点

    // ---- 计算机图标（地址栏左端，和路径共用白色底）----
    int addressY = (navHeight - addressHeight) / 2;
    int computerWidth = Scale(28);
    placeWindow(m_thisPcButton.Handle(), cursorX, addressY, computerWidth, addressHeight);
    cursorX += computerWidth;

    // ---- 地址栏 ----
    // 地址栏右端留出齿轮按钮的位置；齿轮贴在地址栏右边、白色底连成一片
    int settingsWidth = Scale(28);
    int addressWidth = clientWidth - cursorX - margin - settingsWidth;
    if (addressWidth < Scale(150))
    {
        addressWidth = Scale(150);
    }
    placeWindow(m_addressBar.Handle(), cursorX, addressY, addressWidth, addressHeight);

    placeWindow(m_settingsButton.Handle(), cursorX + addressWidth, addressY, settingsWidth, addressHeight);

    // ---- 地址栏下面那条横线 ----
    // 整条窗口宽，把上方"路径 + 按钮"的工具条和下面的内容区隔开
    int bodyHeight = clientHeight - navHeight - statusHeight;
    if (bodyHeight < 0)
    {
        bodyHeight = 0;
    }

    m_addressDividerRect.left   = 0;
    m_addressDividerRect.top    = navHeight;
    m_addressDividerRect.right  = clientWidth;
    m_addressDividerRect.bottom = navHeight + 1;

    // ---- 左侧导航栏 + 文件列表 ----
    // 内容区从横线下面 1 像素开始
    int contentY      = navHeight + 1;
    int contentHeight = bodyHeight - 1;
    if (contentHeight < 0)
    {
        contentHeight = 0;
    }

    int paneWidth = CurrentPaneWidth();

    // 分隔线两侧各让出这么多像素，交给主窗口当"抓取热区"。
    //
    // 为什么必须让出来：左栏和文件列表是两个独立子窗口，鼠标落在它们身上时
    // WM_LBUTTONDOWN / WM_SETCURSOR 只会发给子窗口，主窗口收不到。
    // 以前竖线只有 1 像素归主窗口，实际上要精确点到那 1 个像素才能拖，
    // 表现就是"有时候能拖、有时候拖不动"。现在两个子窗口各后退 4 像素，
    // 中间 9 像素都归主窗口；因为左栏、列表、父窗口背景都是白底，
    // 视觉上完全看不出差别，只是竖线两侧多了一点空白。
    // 注意：这个数值必须和 IsOnPaneDivider() 里的容差一致，否则会出现
    // "光标变成左右箭头了但按下去拖不动"。
    const int splitterHit = Scale(4);

    // 窗口太窄时收起左栏，保证列表还有足够宽度可看
    bool showPane = (m_navigationPane.Handle() != nullptr)
                    && (clientWidth - paneWidth) >= Scale(360);

    m_paneVisible = showPane;

    if (showPane)
    {
        ::ShowWindow(m_navigationPane.Handle(), SW_SHOW);

        // 左栏铺满整条（0 ~ 竖线）：选中那一行的高亮要占满整条，
        // 图标和文字的 10 像素留白在 NavigationPane 内部处理。
        // 右边让出 splitterHit 像素，让给分隔线的热区。
        placeWindow(m_navigationPane.Handle(), 0, contentY,
                    paneWidth - splitterHit, contentHeight);

        // 左栏右边那条竖线，宽 1 像素（线仍画在原来的位置，不跟着让）
        m_paneDividerRect.left   = paneWidth;
        m_paneDividerRect.top    = contentY;
        m_paneDividerRect.right  = paneWidth + 1;
        m_paneDividerRect.bottom = contentY + contentHeight;
    }
    else
    {
        if (m_navigationPane.Handle() != nullptr)
        {
            ::ShowWindow(m_navigationPane.Handle(), SW_HIDE);
        }

        ::SetRectEmpty(&m_paneDividerRect);
    }

    // 上面所有子窗口的位置一次性提交（中间过程不重绘，只统一刷一次）
    if (deferredLayout != nullptr)
    {
        ::EndDeferWindowPos(deferredLayout);
        deferredLayout = nullptr;
    }

    // 文件列表放在批量提交之后单独摆：
    // 它内部要按"自己的最终宽度"重排列宽（LayoutColumns），
    // 所以必须等它的尺寸真正生效之后再算，否则算出来的是旧宽度。
    // 左边同样让出 splitterHit 像素给分隔线热区。
    if (showPane)
    {
        m_fileListView.Resize(paneWidth + splitterHit + 1, contentY,
                              clientWidth - paneWidth - splitterHit - 1, contentHeight);
    }
    else
    {
        m_fileListView.Resize(0, contentY, clientWidth, contentHeight);
    }

    // ------------------------------------------------------------------
    // 只作废两条分隔线（新旧位置取并集），不再整块客户区作废。
    //
    // 为什么要并集：窗口变窄/变矮以后线会跟着变短，旧线多出来的那一段
    // 也在客户区里，光作废新矩形会擦不掉它，所以把上一次画的矩形也算进来。
    // 背景不用整块重刷：子控件区域被 WS_CLIPCHILDREN 挡住了，
    // 真正需要重画的只有这两条 1 像素的线所在的细长区域。
    // ------------------------------------------------------------------
    RECT dirtyAddress = {};
    if (::UnionRect(&dirtyAddress, &m_addressDividerRect, &m_prevAddressDividerRect))
    {
        ::InvalidateRect(m_hWnd, &dirtyAddress, FALSE);
    }

    RECT dirtyPane = {};
    if (::UnionRect(&dirtyPane, &m_paneDividerRect, &m_prevPaneDividerRect))
    {
        ::InvalidateRect(m_hWnd, &dirtyPane, FALSE);
    }

    m_prevAddressDividerRect = m_addressDividerRect;
    m_prevPaneDividerRect    = m_paneDividerRect;
}

// 鼠标是不是压在那条竖线上
// 线本身只有 1 像素宽，太细不好抓，所以左右各放宽 3 像素当热区
bool MainWindow::IsOnPaneDivider(int clientX) const
{
    if (!m_paneVisible)
    {
        return false;
    }

    int splitX = CurrentPaneWidth();

    // 热区半径：必须和 LayoutChildren() 里的 splitterHit 一致。
    // 左栏在 splitX - 4 处收边、列表在 splitX + 5 处开始，
    // 所以 splitX 左右各 4 像素都是主窗口自己的地盘，鼠标消息能收到。
    int tolerance = Scale(4);

    return clientX >= splitX - tolerance && clientX <= splitX + tolerance;
}

// 拖动分隔线：把左栏宽度改成鼠标所在的位置
//
// 宽度限制的考虑：
//   最窄 150 —— 再窄的话「本地磁盘 (C:)」这种长名字会被截得只剩「本地…」
//   最宽 400 —— 再宽就没必要了，列表那边该留的地方得留
//   右边至少留 360 给列表，窗口本身很小的时候上限自动往下收
void MainWindow::UpdatePaneWidth(int clientX)
{
    int minWidth = Scale(150);
    int maxWidth = Scale(400);

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);
    int clientWidth = clientRect.right - clientRect.left;

    int roomForList = clientWidth - Scale(360);
    if (maxWidth > roomForList)
    {
        maxWidth = roomForList;
    }

    if (maxWidth < minWidth)
    {
        maxWidth = minWidth;
    }

    int width = clientX;

    if (width < minWidth)
    {
        width = minWidth;
    }
    if (width > maxWidth)
    {
        width = maxWidth;
    }

    if (width != CurrentPaneWidth())
    {
        m_navWidth = width;
        LayoutChildren();
    }
}

void MainWindow::UpdateNavigationState()
{
    if (m_backButton.Handle() == nullptr)
    {
        return;
    }

    ::EnableWindow(m_backButton.Handle(),    m_history.CanGoBack()    ? TRUE : FALSE);
    ::EnableWindow(m_forwardButton.Handle(), m_history.CanGoForward() ? TRUE : FALSE);

    std::wstring current = m_history.Current();

    // 「向上」能不能点也用同一套 Shell 逻辑判断：
    // 这样到了顶层「桌面」时按钮会变灰（和资源管理器一致），
    // 而不是像以前那样在 "C:\" 上永远亮着却点不动
    std::wstring parent = current.empty() ? std::wstring() : shell::GetParentPath(current);
    ::EnableWindow(m_upButton.Handle(), parent.empty() ? FALSE : TRUE);
}

void MainWindow::UpdateStatusBar()
{
    if (m_hStatusBar == nullptr)
    {
        return;
    }

    std::wstring text;

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (m_resultReady && m_result != nullptr)
        {
            // 分组标题行、占位格都不是真项目，计数时要去掉
            // （否则「此电脑」会显示成 11 项）
            size_t realCount = 0;
            for (const auto& entry : m_result->entries)
            {
                if (!entry.isGroupHeader && !entry.isSpacer)
                {
                    ++realCount;
                }
            }

            text = std::to_wstring(realCount) + L" 个项目";
        }
        else
        {
            text = L"正在读取…";
        }
    }

    // 文字没变就不重设：反复写同一句话也会让状态栏重绘一次，看着像闪
    if (text == m_lastStatusText)
    {
        return;
    }

    m_lastStatusText = text;
    ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}
