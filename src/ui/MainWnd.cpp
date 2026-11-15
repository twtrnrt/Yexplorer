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
#include "core/Format.h"
#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/ShellClipboard.h"
#include "shell/ShellDragDrop.h"
#include "shell/ShellOperations.h"

#include <shellapi.h>   // ShellExecuteW
#include <shlobj.h>     // SHCreateItemFromParsingName / SHGetKnownFolderPath
#include <shlwapi.h>    // StrCmpLogicalW（资源管理器同款的本地化自然排序）
#include <algorithm>
#include <chrono>
#include <unordered_set>

#pragma comment(lib, "shlwapi.lib")

#include "../res/resource.h"   // IDI_APPICON 等资源 ID

// ============================================================================
// 常量
// ============================================================================

static const wchar_t* kWindowClassName = L"FileExplorerMainWindow";
static const int      kInitialWidth    = 1180;
static const int      kInitialHeight   = 740;
static const wchar_t* kWindowTitleBase = L"文件管理器";

// 自定义消息：后台枚举完成
static const UINT WM_APP_ENUM_DONE = WM_APP + 1;

// 自定义消息：后台文件操作完成（复制 / 删除 / 移动）
static const UINT WM_APP_OPERATION_DONE = WM_APP + 2;

// 自定义消息：当前目录的内容变了（别的程序删了/搬走了/新建了文件）
static const UINT WM_APP_FOLDER_CHANGED = WM_APP + 3;

// 目录变化后的延迟刷新：短时间内的连续变化合并成一次刷新
// （批量复制一万个文件会连发几千条通知，不合并的话界面会刷成幻灯片）
static const UINT_PTR kFolderChangeTimerId  = 1;
static const UINT     kFolderChangeDelayMs  = 250;

// 起始路径
static const wchar_t* kInitialPath = L"C:\\";

// 日志里最多记多少条
static const size_t kMaxLogItems = 100;

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

bool MainWindow::Create(HINSTANCE hInstance)
{
    m_hInstance = hInstance;

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

    if (posX == 0 && posY == 0 && !saved.valid)
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
    m_fileListView.SetActivateCallback([this](size_t index)
    {
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
    ACCEL accelerators[] =
    {
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'C',       kCommandCopy },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'X',       kCommandCut },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'V',       kCommandPaste },
        { static_cast<BYTE>(FVIRTKEY),            VK_DELETE, kCommandDelete },
        { static_cast<BYTE>(FVIRTKEY),            VK_F5,     kCommandRefresh },
        { static_cast<BYTE>(FCONTROL | FSHIFT | FVIRTKEY), 'N', kCommandNewFolder },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'A',       kCommandSelectAll },
        { static_cast<BYTE>(FVIRTKEY),            VK_BACK,   kCommandGoUp },
        { static_cast<BYTE>(FVIRTKEY),            VK_F2,     kCommandRename },
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

        // 注册拖放接收：资源管理器/桌面/别的程序把文件拖进列表或左栏都能落地
        RegisterDropTargets();

        LayoutChildren();

        // 上次停在哪个目录就直接打开哪个（目录已经不在了就回 C:\）
        std::wstring startPath = core::Settings::Instance().LastFolder();

        if (startPath.empty() || !IsStartPathUsable(startPath))
        {
            if (!startPath.empty())
            {
                Logger::Warn(L"上次的目录已经不在了，改回默认目录：" + startPath);
            }
            startPath = kInitialPath;
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

        // ---- 快捷键命令（由加速键表发来，通知码为 0/1）----
        switch (controlId)
        {
        case kCommandCopy:
            DoCopy();
            return 0;
        case kCommandCut:
            DoCut();
            return 0;
        case kCommandPaste:
            DoPaste();
            return 0;
        case kCommandDelete:
            DoDelete();
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
            default:
                break;
            }
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
// 这样"从主文件夹点进文档"和"从 C:\Users\kf168 点进 Documents"互不干扰。
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
    //   而不是它的真实父目录 C:\Users\kf168（系统资源管理器就是这么走的）。
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
    //   下载 → kf168（用户主文件夹）→ 用户 → 本地磁盘 (C:) → 此电脑
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
    // 名字优先用"导航来源"里记的：从主文件夹点进「文档」，地址栏就写「文档」，
    // 而不是 C:\Users\kf168\Documents。
    std::wstring displayPath = DisplayNameFor(path);

    // 名字和路径不一样时，地址栏整条只显示这个名字（不切 C:\ > Users > … 那样一段段）
    const bool wholeLabel = (_wcsicmp(displayPath.c_str(), path.c_str()) != 0);

    m_addressBar.SetPathWithLabel(path, displayPath, wholeLabel);

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

void MainWindow::DoCut()
{
    std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
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

    Logger::Info(L"开始粘贴 " + std::to_wstring(paths.size()) + L" 项到 " + targetDirectory +
                 (isCut ? L"（移动）" : L"（复制）"));

    RunFileOperationAsync([paths, targetDirectory, owner, isCut]() -> HRESULT
    {
        return isCut
               ? shellops::MoveItems(paths, targetDirectory, owner)
               : shellops::CopyItems(paths, targetDirectory, owner);
    });
}

void MainWindow::DoDelete()
{
    std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
    if (selected.empty())
    {
        return;
    }

    HWND owner = m_hWnd;

    // 记下删的是哪些，成功之后直接从列表里摘掉这几行，不用重扫目录
    m_lastOperation  = FileOperationKind::Delete;
    m_operationPaths = selected;

    Logger::Info(L"开始删除 " + std::to_wstring(selected.size()) + L" 项");

    RunFileOperationAsync([selected, owner]() -> HRESULT
    {
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
// ============================================================================

void MainWindow::ShowContextMenu(POINT screenPoint)
{
    std::vector<std::wstring> selected     = m_fileListView.GetSelectedPaths();
    bool                      hasSelection = !selected.empty();
    bool                      hasClipboard = shellclip::HasFiles();   // 系统剪贴板里有没有文件

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    // 没选中东西时，"打开/复制/剪切/删除"变灰；剪贴板空时"粘贴"变灰
    ::AppendMenuW(menu, (hasSelection ? MF_STRING : (MF_STRING | MF_GRAYED)), kCommandOpen, L"打开(&O)");

    // ---- 打开方式（子菜单）----
    // 只给"普通文件"才出这项：文件夹、控制面板那类命名空间项都跳过。
    // 注意：这里绝对不能去读注册表 / 探测程序在不在——右键菜单必须零成本，
    // 菜单内容全部来自已经加载在内存里的配置。
    {
        std::wstring openWithTarget;

        std::shared_ptr<shell::EnumerateResult> result = CurrentResult();
        if (result != nullptr)
        {
            const int index = ListView_GetNextItem(m_fileListView.Handle(), -1, LVNI_SELECTED);
            if (index >= 0 && index < static_cast<int>(result->entries.size()))
            {
                const shell::ShellEntry& entry = result->entries[index];

                const bool isShellNamespace = (entry.fullPath.compare(0, 2, L"::") == 0) ||
                                              (entry.fullPath.compare(0, 6, L"shell:") == 0);

                if (!entry.isFolder && !entry.isGroupHeader && !entry.isSpacer &&
                    !entry.fullPath.empty() && !isShellNamespace)
                {
                    openWithTarget = entry.fullPath;
                }
            }
        }

        if (!openWithTarget.empty())
        {
            const std::wstring extension = shell::ExtensionKeyForPath(openWithTarget);
            if (shell::IsExtensionHandled(extension))
            {
                FillOpenWithMenu(menu, extension);
            }
        }
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, (hasSelection ? MF_STRING : (MF_STRING | MF_GRAYED)), kCommandCopy, L"复制(&C)");
    ::AppendMenuW(menu, (hasSelection ? MF_STRING : (MF_STRING | MF_GRAYED)), kCommandCut, L"剪切(&T)");
    ::AppendMenuW(menu, (hasClipboard ? MF_STRING : (MF_STRING | MF_GRAYED)), kCommandPaste, L"粘贴(&P)");
    ::AppendMenuW(menu, (hasSelection ? MF_STRING : (MF_STRING | MF_GRAYED)), kCommandDelete, L"删除(&D)");
    ::AppendMenuW(menu, (hasSelection ? MF_STRING : (MF_STRING | MF_GRAYED)), kCommandRename, L"重命名(&M)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandNewFolder, L"新建文件夹(&N)");
    ::AppendMenuW(menu, MF_STRING, kCommandRefresh,   L"刷新(&R)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- 查看（子菜单，和资源管理器右键的「查看」一样，当前视图打勾）----
    {
        HMENU viewMenu = ::CreatePopupMenu();

        if (viewMenu != nullptr)
        {
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

            ::AppendMenuW(menu, MF_STRING | MF_POPUP,
                          reinterpret_cast<UINT_PTR>(viewMenu), L"查看(&V)");
        }
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");

    // TPM_RETURNCMD：菜单自己跑完并返回点了哪一项，省去额外的一层消息往返
    int command = ::TrackPopupMenu(menu,
                                   TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                                   screenPoint.x, screenPoint.y,
                                   0, m_hWnd, nullptr);

    ::DestroyMenu(menu);

    if (command != 0)
    {
        ::PostMessageW(m_hWnd, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(command), 0), 0);
    }
}

void MainWindow::ShowPropertiesForSelection()
{
    std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

    // 选中多个时只显示第一个的属性（系统的属性窗口一次只能显示一个对象）
    std::wstring target = selected.empty() ? m_history.Current() : selected.front();
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

    // ---- 这里零 IO：只读配置里已经加载好的账本 ----
    const core::OpenWithEntry* entry = core::Settings::Instance().FindOpenWith(extension);

    int visibleCount = 0;

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

            ::AppendMenuW(subMenu, MF_STRING,
                          static_cast<UINT>(kCommandOpenWithBase + static_cast<int>(i)),
                          text.c_str());

            ++visibleCount;
        }
    }

    if (visibleCount > 0)
    {
        ::AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);
    }

    // 一个程序都没有时，这一项就是"直接进窗口去设置"
    ::AppendMenuW(subMenu, MF_STRING, kCommandOpenWithManage, L"添加或管理程序…");

    ::AppendMenuW(parentMenu, MF_STRING | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(subMenu), L"打开方式");
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
// 三件事：
//   1. 普通磁盘目录 → 详细信息（和资源管理器打开文件夹时一样）
//   2. 此电脑 / 控制面板 / 网络 / 回收站 / 主文件夹这些「系统文件夹」→ 小图标
//   3. 用户从右键「查看」里选过的，就不再自动改，一直按他选的来
// ============================================================================

FileListView::ViewMode MainWindow::DefaultViewModeForCurrentPath() const
{
    const std::wstring path = m_history.Current();

    // 「此电脑」用平铺：显示磁盘名、容量文字和占用进度条（和系统自带的此电脑一致）
    if (_wcsicmp(path.c_str(), shell::kThisPcPath) == 0)
    {
        return FileListView::ViewMode::Tiles;
    }

    // 其它 Shell 命名空间里的东西（"::" 开头的解析名、shell:xxx、以及那个顶层桌面）
    // 都算「系统文件夹」——这些地方系统默认是小图标视图。
    const bool isShellNamespace = shell::IsTopDesktopPath(path) ||
                                  path.compare(0, 2, L"::") == 0 ||
                                  path.compare(0, 6, L"shell:") == 0;

    return isShellNamespace ? FileListView::ViewMode::SmallIcon
                            : FileListView::ViewMode::Details;
}

void MainWindow::ApplyViewMode(FileListView::ViewMode mode, bool modeIsUserChoice)
{
    m_viewMode = mode;

    if (modeIsUserChoice)
    {
        // 用户亲自选的：以后进出目录都保持这个视图
        m_viewModeManual = true;
    }

    m_fileListView.SetViewMode(mode);

    // 详细信息视图带列头，其它视图没有列头——状态栏和布局要跟着变
    if (m_hWnd != nullptr)
    {
        LayoutChildren();
    }
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
    settings.SetViewMode(static_cast<int>(m_fileListView.CurrentViewMode()), m_viewModeManual);

    // ---- 上次停留的目录 ----
    const std::wstring current = m_history.Current();
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
        // 落在固定区（硬盘区下面那一段）：接受，但含义是"固定"，不是搬文件
        if (m_navigationPane.IsInPinnedArea(ptClient))
        {
            return kPinnedMarker;
        }

        // 落在某个文件夹节点上：那个节点的目录（和以前一样）
        return m_navigationPane.HitTestPath(ptClient);
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

// 固定区认的"应用"：可执行程序和快捷方式。
// 其它文件（txt、图片、压缩包…）拖进来一律忽略，免得固定区变成第二个桌面。
static bool LooksLikeApplication(const std::wstring& path)
{
    const wchar_t* extensions[] = { L".exe", L".lnk", L".bat", L".cmd", L".appref-ms" };

    for (const wchar_t* extension : extensions)
    {
        size_t extLength = static_cast<size_t>(::lstrlenW(extension));

        if (path.size() > extLength &&
            ::_wcsicmp(path.c_str() + (path.size() - extLength), extension) == 0)
        {
            return true;
        }
    }

    return false;
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

        if (!item.isFolder && !LooksLikeApplication(path))
        {
            ++ignored;
            continue;
        }

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

    // 把「源就在目标目录里」的项剔掉：自己往自己里放没有意义，还会弹一堆错
    std::vector<std::wstring> usableSources;
    usableSources.reserve(sources.size());

    for (const auto& source : sources)
    {
        if (::_wcsicmp(NavigationHistory::ParentPath(source).c_str(),
                       targetDirectory.c_str()) == 0)
        {
            continue;
        }
        usableSources.push_back(source);
    }

    if (usableSources.empty())
    {
        Logger::Warn(L"拖放的源和目标是同一个目录，忽略");
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
        entry.size     = 0;
        ::GetSystemTimeAsFileTime(&entry.modified);

        m_fileListView.InsertEntry(entry);
        UpdateTitle();
        UpdateStatusBar();

        Logger::Info(L"新建后局部插入 1 行（未重扫目录）：" + created);
    }
    else
    {
        Logger::Error(L"新建文件夹失败：" + Logger::FormatHresult(hr));
    }
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

    // 按当前排序状态排一遍（默认名称升序）
    std::sort(result->entries.begin(), result->entries.end(),
              [this](const shell::ShellEntry& left, const shell::ShellEntry& right)
              {
                  return CompareEntries(left, right, m_sortColumn, m_sortAscending);
              });
    m_fileListView.SetSortState(m_sortColumn, m_sortAscending);

    // ------------------------------------------------------------
    // 「此电脑」加分组标题（两组：设备和驱动器 / 网络位置）
    //
    // 系统里此电脑是这样分组的。我们用不了列表控件的原生分组（虚拟模式
    // 不支持），所以改成在数据里插"假标题行"，由列表控件把这种行画成标题。
    //
    // 分组依据是驱动器类型：DRIVE_REMOTE(4) 是网络盘，其余本地盘/光驱/U 盘
    // 都归「设备和驱动器」。网络位置为空时就不插第二个标题，免得出现空组。
    //
    // 插在这里（排序之后、SetResult 之前）最合适：
    //   · 排序不会把标题行挪到中间去
    //   · 换目录、刷新都会重新走一遍这里，标题不会丢也不会重复
    //
    // 前面先判一下第 0 行是不是已经有标题行了：万一同一份结果被处理两次
    // （比如刷新和"操作完成"挨着来），也不会插出两行标题。
    // ------------------------------------------------------------
    if (_wcsicmp(result->path.c_str(), shell::kThisPcPath) == 0 &&
        (result->entries.empty() || !result->entries.front().isGroupHeader))
    {
        std::vector<shell::ShellEntry> localDrives;    // 设备和驱动器
        std::vector<shell::ShellEntry> networkDrives;  // 网络位置

        for (auto& entry : result->entries)
        {
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
        grouped.reserve(result->entries.size() + 8);

        // 平铺视图里一行能并排几块卡片（卡片固定 300 宽）：
        // 组标题后面要补这么多"空占位格"，把这一行剩下的位置填掉，
        // 标题才能像系统那样独占一行。别的视图下一行放几个无所谓（不需要占位）。
        int perRow = 1;

        // 注意：视图是在这段之后才 ApplyViewMode 的，所以这里不能只看 m_viewMode，
        // 还要按"当前目录默认该用哪种视图"算一遍，否则首次进此电脑会按详细信息算，
        // 占位格就补错了。
        bool tilesView = (m_viewMode == FileListView::ViewMode::Tiles);
        if (!m_viewModeManual && DefaultViewModeForCurrentPath() == FileListView::ViewMode::Tiles)
        {
            tilesView = true;
        }

        if (tilesView)
        {
            RECT clientRect = {};
            if (::GetClientRect(m_fileListView.Handle(), &clientRect))
            {
                perRow = (clientRect.right - clientRect.left) / 300;
                if (perRow < 1)
                {
                    perRow = 1;
                }
            }
        }

        // 插一个组标题；平铺视图下再补上占位格
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

        if (!localDrives.empty())
        {
            pushHeader(L"设备和驱动器");

            for (auto& entry : localDrives)
            {
                grouped.push_back(std::move(entry));
            }

            // 这一组如果没把最后一行填满，后面的组标题就会挤到这一行的空位上。
            // 补几个占位格把行尾填满，下一组的标题才能从新的一行开始。
            const int remainder = static_cast<int>(localDrives.size()) % perRow;
            if (remainder != 0)
            {
                for (int i = remainder; i < perRow; ++i)
                {
                    shell::ShellEntry spacer;
                    spacer.isSpacer = true;
                    grouped.push_back(std::move(spacer));
                }
            }
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

        result->entries = std::move(grouped);
    }

    // 进入/刷新目录时定一次视图：本机磁盘目录用详细信息，
    // 此电脑和各种系统文件夹用小图标（和系统默认一致）。
    // 用户从「查看」菜单手动选过之后就不再自动改，一直按他选的来。
    if (!m_viewModeManual)
    {
        ApplyViewMode(DefaultViewModeForCurrentPath(), false);
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

    if (entry.isFolder && !entry.fullPath.empty())
    {
        // 记一条"双击的是哪一项"：以后排查"双击没反应"时，日志里这条后面
        // 紧跟的「导航到 / 按 PIDL 打开成功 / 打开失败」就是它的结果，
        // 一眼能对上是哪个项出的问题。
        Logger::Info(L"双击：[" + entry.name + L"] " + entry.fullPath);

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

    // ---- 导航按钮 ----
    int buttonY = (navHeight - buttonHeight) / 2;
    int cursorX = margin;

    m_homeButton.Resize(cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    m_backButton.Resize(cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    m_forwardButton.Resize(cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    m_upButton.Resize(cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + gap;

    m_refreshButton.Resize(cursorX, buttonY, buttonWidth, buttonHeight);
    cursorX += buttonWidth + Scale(8);   // 按钮组和地址栏之间留宽一点

    // ---- 计算机图标（地址栏左端，和路径共用白色底）----
    int addressY = (navHeight - addressHeight) / 2;
    int computerWidth = Scale(28);
    m_thisPcButton.Resize(cursorX, addressY, computerWidth, addressHeight);
    cursorX += computerWidth;

    // ---- 地址栏 ----
    int addressWidth = clientWidth - cursorX - margin;
    if (addressWidth < Scale(150))
    {
        addressWidth = Scale(150);
    }
    m_addressBar.Resize(cursorX, addressY, addressWidth, addressHeight);

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

    // 窗口太窄时收起左栏，保证列表还有足够宽度可看
    bool showPane = (m_navigationPane.Handle() != nullptr)
                    && (clientWidth - paneWidth) >= Scale(360);

    m_paneVisible = showPane;

    if (showPane)
    {
        ::ShowWindow(m_navigationPane.Handle(), SW_SHOW);

        // 左栏铺满整条（0 ~ 竖线）：选中那一行的高亮要占满整条，
        // 图标和文字的 10 像素留白在 NavigationPane 内部处理
        m_navigationPane.Resize(0, contentY, paneWidth, contentHeight);

        // 左栏右边那条竖线，宽 1 像素
        m_paneDividerRect.left   = paneWidth;
        m_paneDividerRect.top    = contentY;
        m_paneDividerRect.right  = paneWidth + 1;
        m_paneDividerRect.bottom = contentY + contentHeight;

        // 文件列表从竖线右边开始
        m_fileListView.Resize(paneWidth + 1, contentY,
                              clientWidth - paneWidth - 1, contentHeight);
    }
    else
    {
        if (m_navigationPane.Handle() != nullptr)
        {
            ::ShowWindow(m_navigationPane.Handle(), SW_HIDE);
        }

        ::SetRectEmpty(&m_paneDividerRect);
        m_fileListView.Resize(0, contentY, clientWidth, contentHeight);
    }

    // 布局变了，窗口自己那两条线要重画
    ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

// 鼠标是不是压在那条竖线上
// 线本身只有 1 像素宽，太细不好抓，所以左右各放宽 3 像素当热区
bool MainWindow::IsOnPaneDivider(int clientX) const
{
    if (!m_paneVisible)
    {
        return false;
    }

    int splitX    = CurrentPaneWidth();
    int tolerance = Scale(3);

    return clientX >= splitX - tolerance && clientX <= splitX + 1 + tolerance;
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
