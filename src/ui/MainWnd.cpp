// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.cpp
//
// 主窗口实现（阶段 3 版本）。
//
// 界面：导航栏（后退/前进/向上 + 地址栏） / 文件列表 / 状态栏
// 交互：地址栏回车跳转、按钮导航、双击进目录、双击文件打开
 // ============================================================================

#include <windows.h>
#include <windowsx.h>
#include "MainWnd.h"
#include "ui/ShellWindowHost.h"
#include "ui/WindowPlacement.h"
#include "ui/WindowRelay.h"
#include "ui/BatchRenameDialog.h"
#include "core/Format.h"
#include "core/Logger.h"
#include "core/PathUtil.h"
#include "core/Settings.h"
#include "shell/ContainerExtract.h"
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
#include <cstring>
#include <unordered_set>
#include <thread>     // 拖出压缩包里的项：解压丢后台线程，主线程刷进度
#include <atomic>     // 后台线程解压完成标志
#include <mutex>      // std::once_flag：临时缓存清理整个进程只跑一次
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

// 自定义消息：粘贴「虚拟文件」（远程桌面复制过来的那种）时的进度
//   wParam = 百分比 + 1（0 表示"来源没告诉大小"，只能显示第几项）
//   lParam = MAKELONG(第几项, 一共几项)
static const UINT WM_APP_PASTE_PROGRESS = WM_APP + 5;

// 自定义消息：粘贴「虚拟文件」的"清单大小表"准备好了
// 后台线程读完清单、把每一项的字节数算好后发这条，界面线程收到才开始按字节
// 加权画进度条（发这条之前，进度先按"项数"估）
//   wParam / lParam 都不用，数据在 MainWindow::m_pasteItemSizes 里
static const UINT WM_APP_PASTE_PLAN = WM_APP + 6;

// 目录变化后的延迟刷新：短时间内的连续变化合并成一次刷新
// （批量复制一万个文件会连发几千条通知，不合并的话界面会刷成幻灯片）
static const UINT_PTR kFolderChangeTimerId  = 1;
static const UINT     kFolderChangeDelayMs  = 250;

// 自定义消息：选中项变了，延迟刷状态栏
//
// 为什么要延迟：虚拟列表（LVS_OWNERDATA）在 Shift 多选这种"一次性改一大段"的情况下，
// 先发 LVN_ITEMCHANGED、后更新它自己的选中状态。在回调里立刻去查控件状态会读到旧值
// —— 实测 Shift 多选查出来是 0 项（状态栏于是只显示"N 个项目"，没有"已选择 N 项 + 大小"）。
// 丢一条消息到队尾，等控件把状态提交完再算一次就准了。
static const UINT WM_APP_SELECTION_CHANGED = WM_APP + 7;

// 起始路径
static const wchar_t* kInitialPath = L"C:\\";

// 日志里最多记多少条
static const size_t kMaxLogItems = 100;

// 拼接 / 拆路径的小工具（实现放在下面，这里先声明：上面的操作完成处理要用到）
static std::wstring JoinDirectoryFile(const std::wstring& directory, const std::wstring& fileName);
static std::wstring FileNameOf(const std::wstring& path);

// 粘贴「虚拟文件」（远程桌面那类）要用到的两个小工具（实现也在下面）
//   MakeUniqueTargetPath     给清单里的相对路径算一个不重名的目标全路径
//   CreateDirectoriesRecursive 把 "子目录\文件" 这种中间目录建出来
static std::wstring MakeUniqueTargetPath(const std::wstring& directory,
                                         const std::wstring& relativeName,
                                         bool isFolder);
static bool CreateDirectoriesRecursive(const std::wstring& directory);

// ============================================================================
// 内部辅助函数
// ============================================================================

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
// 标签标题：优先用来源名/系统显示名；普通目录只用最后一段（explorer，而不是整条路径）
static std::wstring TabTitleForPath(const std::wstring& path)
{
    std::wstring name = DisplayNameForPath(path);
    if (name != path)
    {
        return name;
    }

    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
    {
        trimmed.pop_back();
    }

    const size_t slash = trimmed.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 >= trimmed.size())
    {
        return trimmed;
    }
    return trimmed.substr(slash + 1);
}

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

    // 驱动器按盘符字母排（C、D、E…），不按显示名：
    // 卷标各不相同的时候（"系统 (C:)"、"DATA (D:)"）按名字排会乱序，
    // 系统「此电脑」也是按盘符排的。
    // 只在按名称排（第 0 列）时算，两条都必须是驱动器（driveLetter 非 0）。
    if (column == 0 && left.driveLetter != 0 && right.driveLetter != 0 &&
        left.driveLetter != right.driveLetter)
    {
        const bool leftFirst = (left.driveLetter < right.driveLetter);
        return ascending ? leftFirst : !leftFirst;
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
    : m_operationRunning(std::make_shared<std::atomic<bool>>(false)),
      m_pasteCancelRequested(std::make_shared<std::atomic<bool>>(false))
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

    // 进度条：摘掉状态栏子类化 + 还回任务栏接口
    m_pasteBar.Detach();
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

        if (ui::IsWindowRectVisible(savedRect))
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
    if (ui::FindExistingWindowRect(cascadeBase))
    {
        RECT target = ui::CascadeRectFromBase(cascadeBase);   // 往右下错开（放不下的那一轴不错开）
        ui::ClampRectToWorkArea(target);                      // 兜底：还出界就推回来

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
        // 正在粘贴「虚拟文件」（远程桌面那类）时，ESC 就是取消粘贴。
        // 这种文件内容是从剪贴板现传的，取消后后台会把写了一半的文件删掉。
        if (m_pasteInProgress.load() && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            if (m_pasteCancelRequested != nullptr)
            {
                m_pasteCancelRequested->store(true);
            }

            m_pasteBar.SetText(L"正在取消粘贴…");

            continue;   // 这条 ESC 就吃掉，不再往下面分发
        }

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

        // 左栏右键：导航栏只负责"右键了哪个节点"，菜单和命令都在 ShowNavigationContextMenu 里
        m_navigationPane.SetContextMenuCallback(
            [this](const std::wstring& path, bool isPinned, bool isFolder, POINT screenPoint)
        {
            ShowNavigationContextMenu(path, isPinned, isFolder, screenPoint);
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

    // 状态栏要能"自己补画"粘贴进度条：子类化它的窗口过程 + 挂任务栏进度
    m_pasteBar.Attach(m_hWnd, m_hStatusBar);

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
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'T', kCommandNewTab },       // 新标签（主文件夹）
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), 'W', kCommandCloseWindow },  // 关闭当前标签
        { static_cast<BYTE>(FCONTROL | FSHIFT | FVIRTKEY), VK_TAB, kCommandPrevTab },
        { static_cast<BYTE>(FCONTROL | FVIRTKEY), VK_TAB, kCommandNextTab },
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

        // SHOpenFolderAndSelectItems 的私有协议：0xE32EE32E
        // 内容依次是「子项数量 + flags + 一串子项 PIDL」。
        // 只解析 + 选中，立刻返回，别让调用方的 UI 线程等我们。
        if (m_shellWindowHost && m_shellWindowHost->IsRegistered() &&
            data != nullptr && data->dwData == 0xE32EE32E &&
            data->lpData != nullptr && data->cbData >= 2 * sizeof(ULONG))
        {
            HandleShellSelectRequest(data->lpData, data->cbData);
            return TRUE;
        }

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
        OnCreate();
        return 0;

    // --------------------------------------------------------------
    // 尺寸变化
    // --------------------------------------------------------------
    case WM_SIZE:
        LayoutChildren();

        // 正在粘贴虚拟文件时，状态栏宽度变了，进度条要重新摆位（不然会错位/被截断）
        if (m_pasteInProgress.load())
        {
            m_pasteBar.Layout();
        }

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

        // 顶部标签栏
        DrawTabBar(hdc, clientRect);

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
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (pt.y < TabBarHeight())
        {
            bool closeButton = false;
            const int tabIndex = HitTestTab(pt, closeButton);
            if (tabIndex >= 0)
            {
                if (closeButton)
                {
                    CloseTab(static_cast<size_t>(tabIndex));
                }
                else
                {
                    ActivateTab(static_cast<size_t>(tabIndex));
                }
                return 0;
            }

            if (::PtInRect(&m_tabPlusRect, pt))
            {
                NewTab(shell::kHomeFolderPath);
            }
            return 0;
        }

        int x = static_cast<short>(LOWORD(lParam));
        if (IsOnPaneDivider(x))
        {
            m_draggingSplitter = true;
            ::SetCapture(m_hWnd);   // 抓住鼠标，拖出窗口范围也能继续跟手
            return 0;
        }
        break;
    }

    case WM_MBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (pt.y < TabBarHeight())
        {
            bool closeButton = false;
            const int tabIndex = HitTestTab(pt, closeButton);
            if (tabIndex >= 0)
            {
                CloseTab(static_cast<size_t>(tabIndex));
                return 0;
            }
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
        if (HandleCommandMessage(wParam))
        {
            return 0;
        }
        break;

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
        OnOperationDone(static_cast<HRESULT>(static_cast<LONG_PTR>(wParam)));
        return 0;

    // --------------------------------------------------------------
    // 选中项变了（延迟到消息队尾再算）
    //
    // 虚拟列表在 Shift 多选时先发通知、后提交选中状态，在通知里立刻查会读到旧值。
    // 到这里时控件状态已经提交完，再查就是准的。
    // --------------------------------------------------------------
    case WM_APP_SELECTION_CHANGED:
        m_selectionRefreshPending = false;
        UpdateStatusBar();
        return 0;

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
    // Shell 窗口自动化对象调过来的导航 / 刷新请求
    //
    // 注意：自动化对象那边只 PostMessage，真正的导航/枚举在这里做，
    // 避免挡住调用方（Chrome 的 UI 线程）。
    // --------------------------------------------------------------
    case ui::kShellWindowNavigateMessage:
    {
        std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lParam));
        if (path && !path->empty())
        {
            NavigateToPath(*path);
        }
        return 0;
    }

    case ui::kShellWindowRefreshMessage:
        RefreshCurrentFolder();
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
    // 粘贴「虚拟文件」（远程桌面那类）的进度
    //
    //   wParam = 百分比 + 1（0 表示来源没告诉大小，只能显示第几项）
    //   lParam = MAKELONG(第几项, 一共几项)
    // --------------------------------------------------------------
    case WM_APP_PASTE_PROGRESS:
    {
        const int    percent = (wParam == 0) ? -1 : static_cast<int>(wParam) - 1;
        const size_t index   = static_cast<size_t>(LOWORD(lParam));
        const size_t total   = static_cast<size_t>(HIWORD(lParam));

        m_pasteBar.Update(index, total, percent);
        return 0;
    }

    // --------------------------------------------------------------
    // 粘贴「虚拟文件」：清单大小表到位了，先把总字节算出来
    // （之后每收到一条进度消息，进度条就按"已完成字节 / 总字节"来画）
    // --------------------------------------------------------------
    case WM_APP_PASTE_PLAN:
    {
        LONGLONG totalBytes = 0;

        if (m_pasteItemSizes != nullptr)
        {
            for (LONGLONG size : *m_pasteItemSizes)
            {
                totalBytes += size;
            }
        }

        // 清单到了（项数已知）：按"第 N/N 项"这种最长文字重排一次，
        // 之后进度条位置就固定不动了（进度更新只改文字和填色）。
        m_pasteBar.SetPlan(m_pasteItemSizes, m_pasteDoneBytes, totalBytes);

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

        // 后台宿主的窗口：关闭只隐藏，进程和托盘继续跑
        if (m_hideOnClose)
        {
            ::ShowWindow(m_hWnd, SW_HIDE);
            return 0;
        }
        break;   // 交给系统继续走默认流程（DestroyWindow）

    // --------------------------------------------------------------
    // 销毁
    // --------------------------------------------------------------
    case WM_DESTROY:
        if (m_shellWindowHost)
        {
            m_shellWindowHost->Revoke();   // 注销 Shell 窗口，别留失效条目
        }
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

// ----------------------------------------------------------------------------
// 后台文件操作完成（复制 / 移动 / 删除）
//
// 原来整段内联在 HandleMessage 的 WM_APP_OPERATION_DONE 分支里（约 140 行），
// 抽出来以后消息分发只留一行，这段逻辑也能单独读、单独改。
// ----------------------------------------------------------------------------
void MainWindow::OnOperationDone(HRESULT hr)
{

    // ------------------------------------------------------------
    // 如果是"粘贴虚拟文件"（远程桌面那类），先把进度显示收掉：
    // 状态栏要退回"N 个项目"，进度文案和 ESC 取消都要复位
    // ------------------------------------------------------------
    const bool wasVirtualPaste = m_pasteInProgress.load();

    if (wasVirtualPaste)
    {
        m_pasteInProgress.store(false);
        m_lastStatusText.clear();   // 让下面的 UpdateStatusBar 真的把计数写回去

        // 进度条数据一并收掉，状态栏恢复成一整格（下次粘贴会重新建）
        m_pasteItemSizes.reset();
        m_pasteDoneBytes.reset();
        m_pasteBar.End();
    }

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

            if (m_pendingVirtualResults != nullptr)
            {
                // 虚拟文件粘贴：源在远程电脑上，本机只有"粘到哪了"，
                // 所以撤销就是按这些目标路径删掉（UndoKind::Copy 的逻辑正好是这个）
                record.afterPaths = *m_pendingVirtualResults;
            }
            else
            {
                for (const std::wstring& source : m_operationPaths)
                {
                    // 操作完成后这一项在目标目录里；用户中途改名会导致找不到，
                    // 撤销时那种项就静默跳过，不影响其它项
                    record.afterPaths.push_back(
                        JoinDirectoryFile(m_operationTargetDirectory, FileNameOf(source)));
                }
            }

            // 一项都没记下来就不压栈，免得出现"撤销 复制"点了却什么都不做
            if (!record.afterPaths.empty())
            {
                PushUndoRecord(record);
            }
        }
    }
    else
    {
        // 失败（含用户在进度框点取消）：保留剪贴板，用户可以直接重试
        Logger::Error(L"文件操作失败：" + Logger::FormatHresult(hr));
    }

    // 虚拟文件粘贴：失败/取消/部分失败时给用户一句明确提示（日志里有细节）
    if (wasVirtualPaste && m_pendingVirtualMessage != nullptr && !m_pendingVirtualMessage->empty())
    {
        ::MessageBoxW(m_hWnd, m_pendingVirtualMessage->c_str(), L"粘贴", MB_OK | MB_ICONWARNING);
    }

    m_pendingVirtualResults.reset();
    m_pendingVirtualMessage.reset();

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
}

// ----------------------------------------------------------------------------
// 菜单 / 快捷键命令分发（WM_COMMAND）
//
// 原来整段在 HandleMessage 的 WM_COMMAND 分支里（约 270 行），抽出来以后
// 消息分发只留几行。返回 true = 这条命令已经处理；false = 不认识，交回默认流程。
// ----------------------------------------------------------------------------
bool MainWindow::HandleCommandMessage(WPARAM wParam)
{
    int controlId = LOWORD(wParam);

    // ---- 「打开方式」子菜单里点了某个程序：命令 ID = 3400 + 账本下标 ----
    if (controlId >= kCommandOpenWithBase &&
        controlId <= kCommandOpenWithBase + 99)
    {
        OpenSelectionWithProgram(static_cast<size_t>(controlId - kCommandOpenWithBase));
        return true;
    }

    // ---- 「新建」子菜单里点了某一项：命令 ID = 3500 + 快照下标 ----
    if (controlId >= kCommandNewBase && controlId <= kCommandNewBase + 99)
    {
        DoNewItem(static_cast<size_t>(controlId - kCommandNewBase));
        return true;
    }

    // ---- 快捷键命令（由加速键表发来，通知码为 0/1）----
    switch (controlId)
    {
    case kCommandCopy:
        DoCopy();
        return true;
    case kCommandCopyPath:
        DoCopyPath();
        return true;
    case kCommandCut:
        DoCut();
        return true;
    case kCommandPaste:
        DoPaste();
        return true;
    case kCommandDelete:
        // Delete：进回收站；按住 Shift 则永久删除（Shell 会弹"确实要永久删除"确认框）
        //
        // 为什么这里还要读一次 Shift 状态：
        //   正常情况下 Shift+Delete 会先命中表里那条 FSHIFT 条目（走 kCommandDeletePermanent），
        //   但保险起见再兜一层——万一哪天条目顺序被调整、或系统把带修饰键的匹配让给了
        //   普通条目，这里读真实键盘状态仍能走对分支。GetAsyncKeyState 读的是全局键状态，
        //   真人按键时可靠。
        DoDelete((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
        return true;
    case kCommandRefresh:
        RefreshCurrentFolder();
        return true;
    case kCommandNewFolder:
        DoNewFolder();
        return true;
    case kCommandSelectAll:
        m_fileListView.SelectAll();
        return true;
    case kCommandGoUp:
        GoUp();
        return true;
    case kCommandOpen:
        OpenSelection();
        return true;
    case kCommandUndo:
        PerformUndo(false);   // 右键菜单「撤销」：批量重命名也能撤
        return true;
    case kCommandRedo:
        PerformRedo(false);
        return true;
    case kCommandUndoKeyboard:
        PerformUndo(true);    // Ctrl+Z：批量重命名这条会被拦掉
        return true;
    case kCommandRedoKeyboard:
        PerformRedo(true);    // Ctrl+Y：同上
        return true;
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
        return true;
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
        return true;
    }
    case kCommandOpenWithManage:
    {
        // 「添加或管理程序…」：管的是当前选中文件那个扩展名
        std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();
        if (!selected.empty())
        {
            ShowOpenWithSettings(shell::ExtensionKeyForPath(selected.front()), false);
        }
        return true;
    }
    case kCommandOpenTerminal:
    {
        // 「在终端中打开」：选中一个文件夹/盘符就用它，否则用当前目录
        // （只在空白处、单个文件夹、盘符这三种场景出现，所以这里不会拿到文件路径）
        const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

        const std::wstring target = (selected.size() == 1 && shell::IsFileSystemPath(selected.front()))
                                    ? selected.front()
                                    : m_history.Current();

        if (shell::IsFileSystemPath(target))
        {
            DoOpenTerminalIn(target);
        }
        return true;
    }
    case kCommandProperties:
        ShowPropertiesForSelection();
        return true;
    case kCommandRename:
        m_fileListView.BeginRenameSelected();
        return true;
    case kCommandBatchRename:
        DoBatchRename();
        return true;
    case kCommandViewLarge:
        ApplyViewMode(FileListView::ViewMode::LargeIcon, true);
        return true;
    case kCommandViewSmall:
        ApplyViewMode(FileListView::ViewMode::SmallIcon, true);
        return true;
    case kCommandViewList:
        ApplyViewMode(FileListView::ViewMode::List, true);
        return true;
    case kCommandViewDetails:
        ApplyViewMode(FileListView::ViewMode::Details, true);
        return true;
    case kCommandViewTiles:
        ApplyViewMode(FileListView::ViewMode::Tiles, true);
        return true;

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
        return true;
    }
    case kCommandHomeWindow:
        // Ctrl+T 已改为新标签；这个命令保留给菜单里的「新窗口打开主文件夹」
        OpenFolderInNewWindow(shell::kTopDesktopPath, true);
        return true;
    case kCommandNewTab:
        NewTab(shell::kHomeFolderPath);
        return true;
    case kCommandNextTab:
        if (m_tabs.size() > 1)
        {
            ActivateTab((m_activeTab + 1) % m_tabs.size());
        }
        return true;
    case kCommandPrevTab:
        if (m_tabs.size() > 1)
        {
            ActivateTab(m_activeTab == 0 ? m_tabs.size() - 1 : m_activeTab - 1);
        }
        return true;
    case kCommandCloseWindow:
        // Ctrl+W：关当前标签；最后一个按后台/退出规则处理
        CloseTab(m_activeTab);
        return true;
    case kCommandGoBackKey:
        GoBack();
        return true;
    case kCommandGoForwardKey:
        GoForward();
        return true;
    case kCommandFocusAddressBar:
        // Alt+D / Ctrl+L：地址栏进入输入状态
        // （内部会放到编辑框、设置焦点并全选，效果和点地址栏空白处一样）
        m_addressBar.BeginEditing();
        return true;
    case kCommandDeletePermanent:
        // Shift+Delete、以及 Shift+右键点「删除」：永久删除（Shell 会弹确认框）
        DoDelete(true);
        return true;
    case kCommandAutoFitColumns:
        // Ctrl+加号：列宽按内容自适应
        m_fileListView.AutoFitColumns();
        return true;
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
            return true;
        case kControlIdForward:
            GoForward();
            return true;
        case kControlIdUp:
            GoUp();
            return true;
        case kControlIdHome:
            // 主页：去「主文件夹」（和 Ctrl+T 开的主文件夹窗口、导航栏里那一项一致）
            NavigateToPath(shell::kTopDesktopPath);
            return true;
        case kControlIdRefresh:
            RefreshCurrentFolder();
            return true;
        case kControlIdThisPc:
            // 计算机图标：去「此电脑」（虚拟文件夹，路径是它的 CLSID）
            NavigateToPath(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");
            return true;
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

            // Shell 窗口集成开关可能变了：按新设置注册/注销
            SyncShellWindowRegistration();

            if (wasHidden != settings.ShowHiddenFiles() ||
                wasSystem != settings.ShowSystemFiles())
            {
                RefreshCurrentFolder();
            }
            return true;
        }
        default:
            break;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// 窗口创建：初始化图标缓存、建子控件、恢复上次的样子、打开启动目录
//
// 原来整段在 HandleMessage 的 WM_CREATE 分支里，抽出来以后消息分发更薄。
// ----------------------------------------------------------------------------
void MainWindow::OnCreate()
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

    // 选中项变了：状态栏里的"选中 N 项 / 总大小"要跟着变
    //
    // 不能在这里立刻算：虚拟列表在 Shift 多选时是先发通知、后提交选中状态，
    // 立刻查会读到旧值（0 项）。丢一条消息到队尾，合并成一次刷新。
    m_fileListView.SetSelectionChangedCallback([this]()
    {
        if (!m_selectionRefreshPending)
        {
            m_selectionRefreshPending = true;
            ::PostMessageW(m_hWnd, WM_APP_SELECTION_CHANGED, 0, 0);
        }
    });

    // 注册拖放接收：资源管理器/桌面/别的程序把文件拖进列表或左栏都能落地
    RegisterDropTargets();

    // 右键菜单要用的数据：「新建」项清单 + 菜单小图标，全部在后台准备，
    // 不占启动时间；菜单弹出时只读内存
    StartBackgroundMenuData();

    // 临时缓存清理也丢后台线程：枚举、判时间、删除都不挡启动界面；
    // 一次性任务，detach 即可，退出时没删完的留到下次启动再清。
    //
    // 整个进程只跑一次：这个函数是"每个窗口建出来"都会走的，多开几个窗口就会
    // 起好几条清理线程同时扫同一批目录（重复 I/O + 互相抢着删 + 日志刷屏），
    // 所以用 call_once 兜住。首次调用者起线程，后面的窗口直接跳过。
    static std::once_flag s_tempCleanupOnce;
    std::call_once(s_tempCleanupOnce, []
    {
        std::thread([]{ shell::CleanupTempCache(); }).detach();
    });

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

    // 第一个标签
    m_tabs.clear();
    {
        TabState first;
        first.path = startPath;
        first.title = TabTitleForPath(startPath);
        first.history.Reset(startPath);
        first.viewMode = m_viewMode;
        first.viewModeManual = m_viewModeManual;
        first.sortColumn = m_sortColumn;
        first.sortAscending = m_sortAscending;
        m_tabs.push_back(std::move(first));
        m_activeTab = 0;
    }

    m_history.Reset(startPath);
    LayoutChildren();
    StartEnumeration(startPath);

    // Shell 窗口集成：开关打开时把主窗口注册进 IShellWindows（默认关，零影响）
    SyncShellWindowRegistration();
}

// ----------------------------------------------------------------------------
// Shell 窗口集成：按设置注册 / 注销
//
// 注册本身是纯运行时的（不写注册表）：开关一关就 Revoke，行为回到以前。
// ----------------------------------------------------------------------------
void MainWindow::SyncShellWindowRegistration()
{
    const bool enabled = core::Settings::Instance().ShellWindowIntegration();

    if (!enabled)
    {
        if (m_shellWindowHost)
        {
            m_shellWindowHost->Revoke();
            m_shellWindowHost.reset();
            Logger::Info(L"Shell 窗口集成：已关闭");
        }
        return;
    }

    if (!m_shellWindowHost)
    {
        m_shellWindowHost = std::make_unique<ui::ShellWindowHost>();
    }

    if (!m_shellWindowHost->IsRegistered())
    {
        m_shellWindowHost->Register(m_hWnd, m_history.Current());
    }
    else
    {
        m_shellWindowHost->UpdateFolder(m_history.Current());
    }
}

// ----------------------------------------------------------------------------
// 其它程序发来的"打开文件夹并选中"（WM_COPYDATA 0xE32EE32E）
//
// 数据格式：ULONG count; ULONG flags; 然后 count 个子项 PIDL（相对当前目录）。
// 只做解析 + 选中，不做导航/枚举——调用方在同步等我们返回。
// ----------------------------------------------------------------------------
void MainWindow::HandleShellSelectRequest(const void* data, DWORD size)
{
    if (data == nullptr || size < 2 * sizeof(ULONG))
    {
        return;
    }

    const BYTE* p = static_cast<const BYTE*>(data);
    const BYTE* const end = p + size;

    ULONG count = 0;
    ULONG flags = 0;
    std::memcpy(&count, p, sizeof(count));
    p += sizeof(count);
    std::memcpy(&flags, p, sizeof(flags));
    p += sizeof(flags);
    (void)flags;

    if (count == 0 || count > 64)
    {
        return;
    }

    const std::wstring folder = m_history.Current();

    PIDLIST_ABSOLUTE folderPidl = nullptr;
    if (FAILED(::SHParseDisplayName(folder.c_str(), nullptr, &folderPidl, 0, nullptr)) ||
        folderPidl == nullptr)
    {
        Logger::Info(L"Shell 窗口：定位请求的目录解析失败：" + folder);
        return;
    }

    std::vector<std::wstring> files;

    for (ULONG i = 0; i < count && p + sizeof(USHORT) <= end; ++i)
    {
        const ITEMID_CHILD* child = reinterpret_cast<const ITEMID_CHILD*>(p);
        const UINT childSize = ::ILGetSize(child);

        if (childSize < sizeof(USHORT) || p + childSize > end)
        {
            break;
        }

        PIDLIST_ABSOLUTE full = ::ILCombine(folderPidl, child);
        wchar_t path[MAX_PATH * 2] = {};

        if (full != nullptr && ::SHGetPathFromIDListW(full, path) && path[0] != L'\0')
        {
            files.emplace_back(path);
        }
        if (full != nullptr)
        {
            ::ILFree(full);
        }

        p += childSize;
    }

    ::ILFree(folderPidl);

    if (files.empty())
    {
        Logger::Info(L"Shell 窗口：收到定位请求，但没有解析出文件路径");
        return;
    }

    m_fileListView.SelectPaths(files);
    m_fileListView.EnsureVisiblePath(files.front());

    Logger::Info(L"Shell 窗口：已定位并选中 " + std::to_wstring(files.size()) + L" 项");
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

// ============================================================================
// 标签：快照 / 恢复 / 新建 / 关闭
// ============================================================================

void MainWindow::SaveActiveTab()
{
    if (m_tabs.empty() || m_activeTab >= m_tabs.size())
    {
        return;
    }

    TabState& tab = m_tabs[m_activeTab];
    tab.path            = m_history.Current();
    tab.title           = TabTitleForPath(tab.path);
    tab.history         = m_history;
    tab.viewMode        = m_viewMode;
    tab.viewModeManual  = m_viewModeManual;
    tab.sortColumn      = m_sortColumn;
    tab.sortAscending   = m_sortAscending;
    tab.selectedPaths   = m_fileListView.GetSelectedPaths();
    tab.topItemPath     = m_fileListView.GetTopItemPath();
    tab.origins         = m_navOrigins;
}

void MainWindow::RestoreTab(size_t index)
{
    if (index >= m_tabs.size())
    {
        return;
    }

    m_activeTab = index;

    const TabState& tab = m_tabs[index];
    m_history              = tab.history;
    m_viewMode             = tab.viewMode;
    m_viewModeManual       = tab.viewModeManual;
    m_sortColumn           = tab.sortColumn;
    m_sortAscending        = tab.sortAscending;
    m_navOrigins           = tab.origins;
    m_pendingSelection     = tab.selectedPaths;
    m_pendingTopItem       = tab.topItemPath;

    const int tabHeight = TabBarHeight();
    RECT strip = { 0, 0, 0, tabHeight };
    ::GetClientRect(m_hWnd, &strip);
    strip.bottom = tabHeight;
    ::InvalidateRect(m_hWnd, &strip, FALSE);

    StartEnumeration(tab.path);
}

void MainWindow::ActivateTab(size_t index)
{
    if (index >= m_tabs.size() || index == m_activeTab)
    {
        return;
    }

    SaveActiveTab();
    RestoreTab(index);
}

void MainWindow::NewTab(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }

    SaveActiveTab();

    TabState tab;
    tab.path            = path;
    tab.title           = TabTitleForPath(path);
    tab.history.Reset(path);
    tab.viewMode        = m_viewMode;
    tab.viewModeManual  = false;
    tab.sortColumn      = m_sortColumn;
    tab.sortAscending   = m_sortAscending;

    const size_t insertAt = (m_tabs.empty() ? 0 : m_activeTab + 1);
    m_tabs.insert(m_tabs.begin() + static_cast<ptrdiff_t>(insertAt), std::move(tab));

    RestoreTab(insertAt);
}

void MainWindow::CloseTab(size_t index)
{
    if (m_tabs.empty() || index >= m_tabs.size())
    {
        return;
    }

    // 最后一个：后台模式隐藏到托盘，否则正常关窗口
    if (m_tabs.size() == 1)
    {
        if (m_hideOnClose)
        {
            ::ShowWindow(m_hWnd, SW_HIDE);
        }
        else
        {
            ::PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
        }
        return;
    }

    SaveActiveTab();
    m_tabs.erase(m_tabs.begin() + static_cast<ptrdiff_t>(index));

    size_t next = m_activeTab;
    if (index < m_activeTab)
    {
        next = m_activeTab - 1;
    }
    else if (index == m_activeTab)
    {
        next = (index < m_tabs.size()) ? index : m_tabs.size() - 1;
    }

    RestoreTab(next);
}

void MainWindow::UpdateActiveTabFromCurrent()
{
    if (m_tabs.empty() || m_activeTab >= m_tabs.size())
    {
        return;
    }

    TabState& tab = m_tabs[m_activeTab];
    tab.path           = m_history.Current();
    tab.title          = TabTitleForPath(tab.path);
    tab.history        = m_history;
    tab.viewMode       = m_viewMode;
    tab.viewModeManual = m_viewModeManual;
    tab.sortColumn     = m_sortColumn;
    tab.sortAscending  = m_sortAscending;
    tab.selectedPaths  = m_fileListView.GetSelectedPaths();
    tab.topItemPath    = m_fileListView.GetTopItemPath();
    tab.origins        = m_navOrigins;

    const int tabHeight = TabBarHeight();
    RECT strip = { 0, 0, 0, tabHeight };
    ::GetClientRect(m_hWnd, &strip);
    strip.bottom = tabHeight;
    ::InvalidateRect(m_hWnd, &strip, FALSE);
}

void MainWindow::DrawTabBar(HDC dc, const RECT& clientRect)
{
    const int tabHeight = TabBarHeight();
    if (tabHeight <= 0 || m_tabs.empty())
    {
        return;
    }

    RECT strip = { 0, 0, clientRect.right, tabHeight };
    HBRUSH stripBrush = ::CreateSolidBrush(RGB(236, 238, 242));
    ::FillRect(dc, &strip, stripBrush);
    ::DeleteObject(stripBrush);

    m_tabRects.assign(m_tabs.size(), RECT{});
    m_tabCloseRects.assign(m_tabs.size(), RECT{});

    HGDIOBJ oldFont = ::SelectObject(dc, m_hUiFont != nullptr ? m_hUiFont
                                                             : ::GetStockObject(DEFAULT_GUI_FONT));
    ::SetBkMode(dc, TRANSPARENT);

    const int top = Scale(3);
    const int bottom = tabHeight - Scale(1);
    int x = Scale(8);

    for (size_t i = 0; i < m_tabs.size(); ++i)
    {
        const std::wstring& title = m_tabs[i].title.empty()
                                  ? m_tabs[i].path
                                  : m_tabs[i].title;

        SIZE textSize = {};
        ::GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &textSize);

        int tabWidth = textSize.cx + Scale(48);
        if (tabWidth < Scale(110)) { tabWidth = Scale(110); }
        if (tabWidth > Scale(220)) { tabWidth = Scale(220); }

        RECT tabRect = { x, top, x + tabWidth, bottom };
        const bool active = (i == m_activeTab);

        HBRUSH tabBrush = ::CreateSolidBrush(active ? RGB(255, 255, 255)
                                                    : RGB(246, 247, 249));
        ::FillRect(dc, &tabRect, tabBrush);
        ::DeleteObject(tabBrush);

        RECT textRect = { tabRect.left + Scale(10), tabRect.top,
                          tabRect.right - Scale(26), tabRect.bottom };
        ::SetTextColor(dc, active ? RGB(32, 32, 32) : RGB(96, 103, 111));
        ::DrawTextW(dc, title.c_str(), -1, &textRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT closeRect = { tabRect.right - Scale(22), tabRect.top + Scale(2),
                           tabRect.right - Scale(4), tabRect.bottom - Scale(2) };
        ::SetTextColor(dc, RGB(96, 103, 111));
        ::DrawTextW(dc, L"×", -1, &closeRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        m_tabRects[i] = tabRect;
        m_tabCloseRects[i] = closeRect;

        x = tabRect.right + Scale(2);
    }

    // 末尾的「+」：新标签（主文件夹）
    m_tabPlusRect = { x, top, x + Scale(30), bottom };
    ::SetTextColor(dc, RGB(80, 88, 96));
    ::DrawTextW(dc, L"+", -1, &m_tabPlusRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    ::SelectObject(dc, oldFont);
}

int MainWindow::HitTestTab(POINT pt, bool& closeButton) const
{
    closeButton = false;

    for (size_t i = 0; i < m_tabRects.size(); ++i)
    {
        if (::PtInRect(&m_tabCloseRects[i], pt))
        {
            closeButton = true;
            return static_cast<int>(i);
        }
        if (::PtInRect(&m_tabRects[i], pt))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
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
    //
    // 例外：粘贴「虚拟文件」（远程桌面那类）不等。
    // 那种粘贴大文件可能要几分钟，等它等于用户一点"切到别的目录"就整个卡住。
    // 它写的是"开始粘贴时定下的那个目录"，跟用户现在看哪个目录无关；
    // 写完自己会发 WM_APP_OPERATION_DONE 刷新，不会留下不一致。
    if (m_operationThread.joinable() && !m_pasteInProgress.load())
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
        // 真实路径一条都没读到：可能是远程桌面（3389）里复制过来的那种"虚拟文件"
        // —— 文件还在远程电脑上，剪贴板里只有一份清单（名字/大小/属性）
        // 和一条内容流，本来就没有路径。
        //
        // 先把剪贴板里到底有什么记下来：用户测试反馈日志时，
        // 一眼就能分清是"剪贴板里根本没有文件清单"，还是"有清单但没处理成功"
        Logger::Info(std::wstring(L"粘贴：剪贴板里没有真实路径（CF_HDROP），虚拟文件清单") +
                     (shellclip::HasVirtualFiles() ? L"：有" : L"：没有"));

        if (DoPasteVirtualFiles(targetDirectory))
        {
            return;
        }

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
// 粘贴「虚拟文件」：远程桌面（3389）里复制过来的那种
//
// 这类文件在本机没有路径。剪贴板里给的是：
//    FileGroupDescriptorW —— 一张清单（名字、大小、属性、修改时间）
//    FileContents         —— 内容流，要第几个就现传第几个
// 资源管理器认这一套，所以能粘；我们原来只读 CF_HDROP（真实路径），所以粘不了。
//
// 为什么不能像普通粘贴那样交给 IFileOperation：
//     IFileOperation 只接受"路径"，这里根本没有路径，内容是边传边写的，
//     只能自己跑线程 + 自己写盘 + 自己做进度。
//
// 流程：
//    界面线程：确认是这类格式 → 占上"文件操作中"标志 → 状态栏切成"正在粘贴…"
//    后台线程：逐项算不重名的目标名 → 建中间目录 → 从剪贴板取内容写盘 → 补属性/时间
//    进度：每写一块发 WM_APP_PASTE_PROGRESS，界面线程刷新状态栏
//    取消：界面线程收到 ESC 置 m_pasteCancelRequested，后台看到就删掉写了一半的文件
//    完成：WM_APP_OPERATION_DONE（原来的通用收尾：刷新列表 + 记撤销）
// ============================================================================
bool MainWindow::DoPasteVirtualFiles(const std::wstring& targetDirectory)
{
    // 不是这类格式（比如剪贴板里就是一段文字）就什么都不做，让调用方走原来的日志
    if (!shellclip::HasVirtualFiles())
    {
        return false;
    }

    // 同一时间只允许一个文件操作。通用那条路（RunFileOperationAsync）也有同样的检查，
    // 这里提前拦一下，免得把"剪贴板里没有文件"这种误导日志写进去
    if (m_operationRunning != nullptr && m_operationRunning->load())
    {
        Logger::Warn(L"上一个文件操作还没结束，本次粘贴忽略");
        return true;
    }

    // 上一次的取消标志复位
    if (m_pasteCancelRequested != nullptr)
    {
        m_pasteCancelRequested->store(false);
    }

    // 撤销记录要用的"操作后在哪"，由后台线程填；先挂一个空的
    m_pendingVirtualResults     = std::make_shared<std::vector<std::wstring>>();
    m_pendingVirtualMessage     = std::make_shared<std::wstring>();

    // 进度条要用的数据：清单每项字节数 + 已写字节。都由后台线程填
    m_pasteItemSizes = std::make_shared<std::vector<LONGLONG>>();
    m_pasteDoneBytes = std::make_shared<std::atomic<LONGLONG>>(0);

    // 远程剪贴板不传"剪切"语义（源文件还在远程电脑上），一律按复制处理，
    // 也不清空剪贴板（用户可能还想粘第二次）
    m_lastOperation            = FileOperationKind::Copy;
    m_lastOperationPermanent   = false;
    m_clearClipboardWhenDone   = false;
    m_operationPaths.clear();                       // 没有源路径可记
    m_operationTargetDirectory = targetDirectory;

    // 状态栏切成进度显示（左文字 / 条 / 右文字），把 ESC 取消打开，
    // 任务栏按钮上也挂一条进度（切到别的程序也能看到剩多少）
    m_pasteInProgress.store(true);
    m_lastStatusText.clear();   // 让收尾时能立刻把"N 个项目"写回去

    m_pasteBar.Begin(m_pasteItemSizes, m_pasteDoneBytes, 0, 0);

    HWND                                      notifyWindow = m_hWnd;
    const std::wstring                        target       = targetDirectory;
    std::shared_ptr<std::atomic<bool>>        cancelFlag   = m_pasteCancelRequested;
    std::shared_ptr<std::vector<std::wstring>> results     = m_pendingVirtualResults;
    std::shared_ptr<std::wstring>             messageOut   = m_pendingVirtualMessage;
    std::shared_ptr<std::vector<LONGLONG>>    itemSizes     = m_pasteItemSizes;
    std::shared_ptr<std::atomic<LONGLONG>>    doneBytes     = m_pasteDoneBytes;

    Logger::Info(L"开始粘贴剪贴板里的虚拟文件（远程桌面那类）到 " + target);

    RunFileOperationAsync([target, notifyWindow, cancelFlag, results, messageOut, itemSizes, doneBytes]() -> HRESULT
    {
        // ------------------------------------------------------------
        // 以下都在后台线程上跑。
        // RunFileOperationAsync 已经给线程做过 CoInitializeEx(STA)，
        // OleGetClipboard / IDataObject 正需要这个。
        // ------------------------------------------------------------
        shellclip::VirtualFileSession session;

        if (!session.Open())
        {
            Logger::Error(L"粘贴虚拟文件：打开剪贴板失败：" + session.LastError());

            if (messageOut != nullptr)
            {
                *messageOut = L"读不到剪贴板里的文件：" + session.LastError();
            }

            return E_FAIL;
        }

        const size_t total = session.Count();
        Logger::Info(L"剪贴板虚拟文件清单：" + std::to_wstring(total) + L" 项");

        // ----------------------------------------------------------------
        // 给界面准备"清单大小表"。
        //
        // 进度条按"一格 = 相同字节量"来画：大文件占的格多、走得慢，小文件占的
        // 格少、走得快 —— 视觉效果就是一个文件一格、文件越大格子越宽。
        //
        // 一次写完再通知界面，界面之后只读不写，不用加锁。
        // ----------------------------------------------------------------
        if (itemSizes != nullptr)
        {
            std::vector<LONGLONG> sizes(total, 0);

            for (size_t i = 0; i < total; ++i)
            {
                const shellclip::VirtualFileInfo& info = session.Item(i);
                const bool isFolder = info.hasAttributes &&
                                      (info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                // 文件夹没有内容、来源没给大小的项，都记 0（不占进度条的格子）
                if (!isFolder && info.hasSize && info.size > 0)
                {
                    sizes[i] = info.size;
                }
            }

            *itemSizes = std::move(sizes);
        }

        if (doneBytes != nullptr)
        {
            doneBytes->store(0);
        }

        // 通知界面"清单表好了"。这条消息排在后面的进度消息前面，
        // 所以界面收到它之后再读 m_pasteItemSizes 一定读到的是完整内容。
        ::PostMessageW(notifyWindow, WM_APP_PASTE_PLAN, 0, 0);

        size_t done      = 0;   // 成功写出来的项数
        size_t failures  = 0;   // 失败的项数
        bool   cancelled = false;

        // ---- 中途被打断的保险 ----
        // 实测：从远程桌面粘上千个小文件时，任务可能在某一项之后整体断掉
        // （最常见的原因是传输期间有人在接收端复制/剪切，剪贴板被换掉），
        // 之后每一项都会报"读剪贴板数据失败"。
        // 连续失败到这个数就提前停下并说清还剩多少，别白跑几百项。
        const size_t kMaxConsecutiveFailures = 5;

        size_t consecutiveFailures = 0;
        bool   sourceLost          = false;

        for (size_t i = 0; i < total; ++i)
        {
            if (cancelFlag != nullptr && cancelFlag->load())
            {
                cancelled = true;
                break;
            }

            const shellclip::VirtualFileInfo& info = session.Item(i);
            const bool isFolder = info.hasAttributes &&
                                  (info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            // 进度消息的两个字段。
            // 单独取出来是因为 MAKELONG 是宏，参数里直接写带逗号的表达式会被当成两个参数。
            const WORD progressIndex = static_cast<WORD>(std::min<size_t>(i + 1, 0xFFFF));
            const WORD progressTotal = static_cast<WORD>(std::min<size_t>(total, 0xFFFF));

            // 目标路径：中间目录照清单里的相对路径建，只有最后一段要防重名
            const std::wstring destination = MakeUniqueTargetPath(target, info.name, isFolder);

            // 逐项记一条：用户从日志就能看出清单里是什么、写到哪、多大
            Logger::Info(L"粘贴清单第 " + std::to_wstring(i + 1) + L"/" + std::to_wstring(total) +
                         L" 项：「" + info.name + L"」" +
                         (isFolder ? L"（文件夹）"
                                   : (info.hasSize ? L"（" + std::to_wstring(info.size) + L" 字节）"
                                                   : L"（大小未知）")));

            if (destination.empty())
            {
                ++failures;
                Logger::Warn(L"粘贴「" + info.name + L"」：生成目标路径失败");
                continue;
            }

            // "子目录\文件" 这种清单项：先把中间目录建出来
            const std::wstring parent = NavigationHistory::ParentPath(destination);
            if (!parent.empty() && !CreateDirectoriesRecursive(parent))
            {
                ++failures;
                Logger::Warn(L"粘贴「" + info.name + L"」：建目录失败 " + parent);
                continue;
            }

            // 这一项开始：报 0%，界面显示"第 i+1/N 项"
            ::PostMessageW(notifyWindow, WM_APP_PASTE_PROGRESS,
                           static_cast<WPARAM>(1),
                           static_cast<LPARAM>(MAKELONG(progressIndex, progressTotal)));

            std::wstring error;

            const bool ok = session.Extract(i, destination,
                [&](int percent) -> bool
                {
                    // 用户按了 ESC：让 Extract 把半截文件删掉
                    if (cancelFlag != nullptr && cancelFlag->load())
                    {
                        return false;
                    }

                    ::PostMessageW(notifyWindow, WM_APP_PASTE_PROGRESS,
                                   static_cast<WPARAM>(percent < 0 ? 0 : percent + 1),
                                   static_cast<LPARAM>(MAKELONG(progressIndex, progressTotal)));
                    return true;
                },
                error);

            if (ok)
            {
                consecutiveFailures = 0;   // 成功了就清零连续失败计数
                results->push_back(destination);
                ++done;

                // 进度条按字节推进：这一项写完了就把它的字节数记进"已完成"
                if (doneBytes != nullptr && !isFolder && info.hasSize && info.size > 0)
                {
                    doneBytes->fetch_add(info.size);
                }

                Logger::Info(L"  已写入：" + destination);
                continue;
            }

            if (error == L"已取消")
            {
                cancelled = true;
                break;
            }

            ++failures;
            Logger::Warn(L"粘贴「" + info.name + L"」失败：" + error);

            // 连续失败太多了：不再一项一项硬跑，提前停
            ++consecutiveFailures;

            if (consecutiveFailures >= kMaxConsecutiveFailures)
            {
                sourceLost = true;

                Logger::Error(L"连续 " + std::to_wstring(consecutiveFailures) +
                              L" 项都读不出剪贴板数据，判定传输已中断，提前停止");
                break;
            }
        }

        Logger::Info(L"粘贴虚拟文件结束：成功 " + std::to_wstring(done) + L" 项，失败 " +
                     std::to_wstring(failures) + L" 项" + (cancelled ? L"（已取消）" : L""));

        // 给界面留一句提示（一项都没成功 / 有失败项 / 被取消 才需要弹窗）
        if (messageOut != nullptr)
        {
            // "没粘出来"的项数：失败项 + 取消后没处理的项，统一按"总数 - 成功数"算
            const size_t notDone = (total > done) ? (total - done) : 0;

            if (cancelled)
            {
                *messageOut = L"已粘贴成功 " + std::to_wstring(done) + L" 项（" +
                              std::to_wstring(notDone) + L" 项被取消）";
            }
            else if (sourceLost)
            {
                *messageOut = L"粘贴过程中断：" +
                              std::to_wstring(notDone) + L" 项没能粘贴（已成功 " +
                              std::to_wstring(done) + L" 项）。\n"
                              L"注意：传输过程使用复制/剪切会中断传输。";
            }
            else if (failures > 0)
            {
                *messageOut = L"有 " + std::to_wstring(failures) +
                              L" 项没能粘贴成功（成功的 "
                              + std::to_wstring(done) + L" 项已经在目标目录里）。";
            }
            else if (done == 0)
            {
                *messageOut = L"剪贴板里没有可以粘出来的文件。";
            }
        }

        // 一项都没成功：算失败，让通用收尾别刷新、别记撤销
        if (done == 0)
        {
            return E_FAIL;
        }

        // 有失败或取消：按"部分成功"处理（SUCCEEDED 为真，界面照样刷新）
        return (failures > 0 || cancelled) ? S_FALSE : S_OK;
    });

    return true;
}

// ----------------------------------------------------------------------------
// 按 Windows 命令行规则把一个路径包成"能安全塞进命令行"的参数
//
// 关键坑：路径以反斜杠结尾时（盘符根 "C:\" 就是），如果直接写 "\"" 包起来，
// 末尾那个反斜杠会把结束引号转义成字面的引号（命令行里 \" 表示一个真引号），
// 对方拿到的路径就是坏的。标准做法是把末尾的反斜杠翻倍：
//     "C:\"   → "C:\\"        （对方解析出来仍然是 C:\）
//     "D:\abc\" → "D:\abc\\"
// 实测：Windows Terminal 收到坏的盘符根路径会报 0x8007010b「无法访问启动目录」。
// ----------------------------------------------------------------------------
static std::wstring QuoteCommandLineArg(const std::wstring& value)
{
    std::wstring result = L"\"";

    // 数一下末尾连着有几个反斜杠
    size_t trailingBackslashes = 0;
    while (trailingBackslashes < value.size() &&
           value[value.size() - 1 - trailingBackslashes] == L'\\')
    {
        ++trailingBackslashes;
    }

    result.append(value);
    result.append(trailingBackslashes, L'\\');   // 末尾反斜杠翻倍，避免转义掉结束引号
    result.append(L"\"");
    return result;
}

// ============================================================================
// 在指定文件夹里开一个终端窗口（右键「在终端中打开」）
//
// 优先用 Windows 11 的 Windows Terminal（wt.exe -d <目录>），没装就退回
// 系统自带的 cmd.exe。两者都是新开一个独立窗口，工作目录就是目标文件夹。
//
// 只在"点了这一项"的时候才去查 wt.exe 在不在（一次 GetFileAttributes），
// 右键菜单弹出时不做任何探测，依旧零成本。
// ============================================================================
void MainWindow::DoOpenTerminalIn(const std::wstring& folderPath)
{
    // 目录可能在弹出菜单之后被删掉/改名了：先确认一次，免得开出一个错误的工作目录
    const DWORD attributes = ::GetFileAttributesW(folderPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        Logger::Warn(L"在终端中打开：目录不存在或不是文件夹 " + folderPath);
        ::MessageBoxW(m_hWnd, L"这个文件夹已经不存在了，无法在其中打开终端。",
                      L"在终端中打开", MB_OK | MB_ICONWARNING);
        return;
    }

    // UNC 路径（\\server\share）不能直接当进程的当前目录，cmd 得靠 pushd 切进去
    const bool isUncPath = (folderPath.size() >= 2 && folderPath[0] == L'\\' && folderPath[1] == L'\\');

    // ---- 1. 先试 Windows Terminal ----
    // Store 版终端的入口是 WindowsApps 下的别名文件，按固定路径拼出来看一眼就够了，
    // 不查注册表、不碰 COM（符合"右键菜单相关操作都轻量"的约定）。
    std::wstring wtPath;
    {
        wchar_t localAppData[MAX_PATH] = {};
        if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0)
        {
            wtPath = std::wstring(localAppData) + L"\\Microsoft\\WindowsApps\\wt.exe";
        }
    }

    if (!wtPath.empty() && ::GetFileAttributesW(wtPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        // wt -d "目录"：直接在该目录打开一个标签页
        // 盘符根 "C:\" 这种以反斜杠结尾的路径必须按命令行规则加引号（见 QuoteCommandLineArg）
        const std::wstring arguments = L"-d " + QuoteCommandLineArg(folderPath);

        // lpDirectory 也指向目标目录：万一 -d 没生效，wt 的启动目录也落在正确的位置
        const HINSTANCE result = ::ShellExecuteW(m_hWnd, L"open", wtPath.c_str(), arguments.c_str(),
                                                 isUncPath ? nullptr : folderPath.c_str(), SW_SHOWNORMAL);

        // ShellExecute 返回值大于 32 才算成功（32 及以下都是错误码）
        if (reinterpret_cast<INT_PTR>(result) > 32)
        {
            Logger::Info(L"在终端中打开（Windows Terminal）：" + folderPath);
            return;
        }

        // 终端启动失败就把球踢给 cmd，别让用户白点一次
        Logger::Warn(L"在终端中打开：Windows Terminal 启动失败，改用 cmd");
    }

    // ---- 2. 退回 cmd.exe ----
    std::wstring comspec = L"C:\\Windows\\System32\\cmd.exe";
    {
        wchar_t buffer[MAX_PATH] = {};
        if (::GetEnvironmentVariableW(L"COMSPEC", buffer, MAX_PATH) > 0)
        {
            comspec = buffer;
        }
    }

    const std::wstring commandLine = isUncPath
        ? (QuoteCommandLineArg(comspec) + L" /k pushd " + QuoteCommandLineArg(folderPath))
        : QuoteCommandLineArg(comspec);

    // CreateProcessW 会改动命令行缓冲区，必须传一份可写的拷贝（不能直接传 const 串）
    std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back(L'\0');

    STARTUPINFOW startupInfo = {};
    startupInfo.cb          = sizeof(startupInfo);
    startupInfo.dwFlags     = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION processInfo = {};

    const BOOL created = ::CreateProcessW(comspec.c_str(),
                                          commandLineBuffer.data(),
                                          nullptr, nullptr, FALSE,
                                          CREATE_NEW_CONSOLE,
                                          nullptr,
                                          isUncPath ? nullptr : folderPath.c_str(),
                                          &startupInfo, &processInfo);

    if (!created)
    {
        const DWORD error = ::GetLastError();
        Logger::Warn(L"在终端中打开：cmd 启动失败，错误码 " + std::to_wstring(error) + L"，目录 " + folderPath);
        ::MessageBoxW(m_hWnd, L"打开终端失败，请检查该文件夹是否存在或是否有访问权限。",
                      L"在终端中打开", MB_OK | MB_ICONERROR);
        return;
    }

    // 进程/线程句柄用完就关，不然每开一次终端就漏两个句柄
    ::CloseHandle(processInfo.hThread);
    ::CloseHandle(processInfo.hProcess);

    Logger::Info(L"在终端中打开（cmd）：" + folderPath);
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

// ---------------------------------------------------------------------------
// 逐级创建目录（"子目录\文件" 这种剪贴板清单项要用）
//
// 用 SHCreateDirectoryEx 一步到位：它会把中间缺的层级都建出来，
// 已经存在也返回成功（ERROR_ALREADY_EXISTS），不用自己切段循环。
// ---------------------------------------------------------------------------
static bool CreateDirectoriesRecursive(const std::wstring& directory)
{
    if (directory.empty())
    {
        return false;
    }

    const DWORD result = static_cast<DWORD>(
        ::SHCreateDirectoryExW(nullptr, pathutil::MakeLongPath(directory).c_str(), nullptr));

    return result == ERROR_SUCCESS || result == ERROR_FILE_EXISTS || result == ERROR_ALREADY_EXISTS;
}

// ---------------------------------------------------------------------------
// 给"清单里的相对路径"算一个不重名的目标全路径
//
//   directory    目标目录
//   relativeName 清单里的相对路径（可能带 "子目录\文件"，已清过非法字符）
//   isFolder     是不是目录项（决定" - 副本"加在扩展名前还是末尾）
//
// 规则和资源管理器一致：重名就 "名字 - 副本"、"名字 - 副本 (2)"……
// 只有最后一段参与防重名，中间目录照原样复用（同一个压缩/远程文件夹里的
// 多个文件不该各自生成一套目录）。
// ---------------------------------------------------------------------------
static std::wstring MakeUniqueTargetPath(const std::wstring& directory,
                                         const std::wstring& relativeName,
                                         bool isFolder)
{
    if (directory.empty() || relativeName.empty())
    {
        return std::wstring();
    }

    // 拆出"中间目录"和"最后一段"
    std::wstring folderPart;    // 以反斜杠结尾；没有中间目录就是空
    std::wstring leafName = relativeName;

    const size_t lastSlash = relativeName.find_last_of(L'\\');
    if (lastSlash != std::wstring::npos)
    {
        folderPart = relativeName.substr(0, lastSlash + 1);
        leafName   = relativeName.substr(lastSlash + 1);
    }

    if (leafName.empty())
    {
        return std::wstring();
    }

    // 拆扩展名（"." 在开头的不算扩展名，和系统规则一致）
    std::wstring baseName = leafName;
    std::wstring extension;

    if (!isFolder)
    {
        const size_t lastDot = leafName.find_last_of(L'.');
        if (lastDot != std::wstring::npos && lastDot > 0)
        {
            baseName  = leafName.substr(0, lastDot);
            extension = leafName.substr(lastDot);
        }
    }

    // 先试原名：不存在就直接用
    const std::wstring firstChoice = JoinDirectoryFile(directory, relativeName);

    if (::GetFileAttributesW(pathutil::MakeLongPath(firstChoice).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return firstChoice;
    }

    // 同名存在：按 " - 副本" 往后排（最多试 1000 次，和 shellops::MakeCopyName 一致）
    for (int index = 1; index <= 1000; ++index)
    {
        std::wstring candidateName;

        if (index == 1)
        {
            candidateName = baseName + L" - 副本" + extension;
        }
        else
        {
            candidateName = baseName + L" - 副本 (" + std::to_wstring(index) + L")" + extension;
        }

        const std::wstring candidate =
            JoinDirectoryFile(directory, folderPart + candidateName);

        if (::GetFileAttributesW(pathutil::MakeLongPath(candidate).c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return candidate;
        }
    }

    // 一千个副本都撞上了（基本不可能）：返回空，让调用方记为失败，别乱覆盖
    return std::wstring();
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
// 撤销栈的维护（栈本身在 core::UndoStack，这里只做转发）
// ---------------------------------------------------------------------------
void MainWindow::PushUndoRecord(const UndoRecord& record)
{
    m_undo.Push(record);
}

void MainWindow::ClearUndoHistory()
{
    m_undo.Clear();
}


void MainWindow::PerformUndo(bool fromKeyboard)
{
    if (m_operationRunning != nullptr && m_operationRunning->load())
    {
        Logger::Warn(L"有文件操作正在进行，撤销先不执行");
        return;
    }

    if (!m_undo.CanUndo())
    {
        Logger::Info(L"撤销：没有可撤销的步骤");
        return;
    }

    // 批量重命名：只能在右键菜单里撤，Ctrl+Z 不响应（用户拍板）。
    // 这里是"整条不执行"，不是"跳过这条去撤上一条"——跳过去会让撤销和用户看到的
    // 操作顺序对不上，太容易误删。
    if (fromKeyboard && m_undo.TopUndo()->batchRename)
    {
        Logger::Info(L"撤销：批量重命名不支持 Ctrl+Z，请用右键菜单里的「撤销 批量重命名」");
        return;
    }

    // 从栈顶那一 步开始反着做
    const UndoRecord record = m_undo.TakeUndo();

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
            m_undo.ClearRedo();
        }
        else
        {
            m_undo.PushRedo(record);
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
        m_undo.PushRedo(record);
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
            m_undo.PushRedo(record);
            break;
        }

        // 改回原来的名字
        const size_t count = std::min(record.afterPaths.size(), record.beforePaths.size());
        for (size_t i = 0; i < count; ++i)
        {
            shellops::RenameItem(record.afterPaths[i], FileNameOf(record.beforePaths[i]), m_hWnd);
        }
        Logger::Info(L"撤销：已改回 " + std::to_wstring(count) + L" 项的名字");
        m_undo.PushRedo(record);
        break;
    }

    case UndoKind::Delete:
        RestoreDeletedFromRecycleBin(record.beforePaths, m_hWnd);
        m_undo.PushRedo(record);
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

    if (!m_undo.CanRedo())
    {
        Logger::Info(L"恢复：没有可恢复的步骤");
        return;
    }

    // 和撤销对称：批量重命名的重做也只能走右键菜单，Ctrl+Y 不碰它
    if (fromKeyboard && m_undo.TopRedo()->batchRename)
    {
        Logger::Info(L"恢复：批量重命名不支持 Ctrl+Y，请用右键菜单里的「恢复 批量重命名」");
        return;
    }

    const UndoRecord record = m_undo.TakeRedo();

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
            m_undo.PushUndoOnly(record);
            break;
        }

        const size_t count = std::min(record.afterPaths.size(), record.beforePaths.size());
        for (size_t i = 0; i < count; ++i)
        {
            shellops::RenameItem(record.beforePaths[i], FileNameOf(record.afterPaths[i]), m_hWnd);
        }
        Logger::Info(L"恢复：已改回新名字 " + std::to_wstring(count) + L" 项");
        m_undo.PushUndoOnly(record);
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
        m_undo.PushUndoOnly(record);
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
        m_undo.PushUndoOnly(record);
        break;
    }

    case UndoKind::Delete:
        for (const std::wstring& path : record.beforePaths)
        {
            shellops::DeleteItems({ path }, m_hWnd);
        }
        Logger::Info(L"恢复：已再删除 " + std::to_wstring(record.beforePaths.size()) + L" 项");
        m_undo.PushUndoOnly(record);
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

        if (::MoveFileExW(pathutil::MakeLongPath(fromPaths[i]).c_str(),
                          pathutil::MakeLongPath(temp).c_str(), 0))
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

        if (::MoveFileExW(pathutil::MakeLongPath(tempPaths[i]).c_str(), pathutil::MakeLongPath(target).c_str(), 0))
        {
            continue;
        }

        ++failures;

        if (!::MoveFileExW(pathutil::MakeLongPath(tempPaths[i]).c_str(),
                           pathutil::MakeLongPath(fromPaths[i]).c_str(), 0))
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
    ShowPropertiesForPath(target);
}

// ---------------------------------------------------------------------------
// 按指定路径弹系统的「属性」窗口
// （列表里选中一项、左栏右键某个节点，都走这里，保证和资源管理器一样是原生属性窗）
// ---------------------------------------------------------------------------
void MainWindow::ShowPropertiesForPath(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_INVOKEIDLIST;   // 走 shell 的"属性"动词，弹系统原生属性窗口
    info.lpVerb = L"properties";
    info.lpFile = path.c_str();
    info.nShow  = SW_SHOW;
    info.hwnd   = m_hWnd;

    if (!::ShellExecuteExW(&info))
    {
        Logger::Warn(L"打开属性窗口失败：" + path);
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
    // 前台锁会让普通的 SetForegroundWindow 静默失效，统一走这个更稳的版本
    const bool foreground = ui::ForceForegroundWindow(m_hWnd);

    if (selectFile.empty())
    {
        Logger::Info(std::wstring(L"收到其它窗口的请求，已尝试置前（成功=") +
                     (foreground ? L"是" : L"否") + L"）");
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
    HWND listView = m_fileListView.Handle();

    // 只选了一项：保持原来的行为（文件夹是"进入"，文件是"打开"）
    if (ListView_GetSelectedCount(listView) <= 1)
    {
        const int singleIndex = ListView_GetNextItem(listView, -1, LVNI_SELECTED);

        if (singleIndex >= 0)
        {
            OnItemActivated(static_cast<size_t>(singleIndex));
        }
        return;
    }

    // 多选：和资源管理器一致，把所有选中的文件都打开
    //
    // 压缩包内部的项也一项一项来 —— 每一项都会先解到临时目录再打开，
    // 状态栏会依次显示各自的解压进度（以前这里只处理了第一个，所以"多选只开一个"）。
    //
    // 文件夹跳过：多选打开多个文件夹只会让当前窗口来回跳，没有意义。
    //
    // 失败先攒着，整个循环结束只弹一条汇总：压缩包在打开前被删/读不了时，
    // 每项都会解压失败，逐项弹框会连弹好几个。
    m_batchOpenFailures.clear();
    m_collectBatchOpenFailures = true;

    int index = ListView_GetNextItem(listView, -1, LVNI_SELECTED);

    while (index >= 0)
    {
        bool isFolder = false;

        {
            std::lock_guard<std::mutex> lock(m_resultMutex);

            if (m_result != nullptr && static_cast<size_t>(index) < m_result->entries.size())
            {
                isFolder = m_result->entries[index].isFolder;
            }
        }

        if (!isFolder)
        {
            OnItemActivated(static_cast<size_t>(index));
        }

        index = ListView_GetNextItem(listView, index, LVNI_SELECTED);
    }

    m_collectBatchOpenFailures = false;
    ShowBatchOpenFailures();
}

// ----------------------------------------------------------------------------
// 多选打开失败：合并成一条提示
// 逐项弹框实测很折磨人（8 项失败 = 8 个框，8 秒才点得完），
// 这里最多列 8 条，剩下的折成"等共 N 项"。
// ----------------------------------------------------------------------------
void MainWindow::ShowBatchOpenFailures()
{
    if (m_batchOpenFailures.empty())
    {
        return;
    }

    const size_t total = m_batchOpenFailures.size();
    const size_t shown = (total < 8) ? total : 8;

    std::wstring message = L"有 " + std::to_wstring(total) + L" 项打开失败：\r\n\r\n";

    for (size_t i = 0; i < shown; ++i)
    {
        message += L"· " + m_batchOpenFailures[i].name;

        if (!m_batchOpenFailures[i].reason.empty())
        {
            message += L" —— " + m_batchOpenFailures[i].reason;
        }

        message += L"\r\n";
    }

    if (total > shown)
    {
        message += L"……等共 " + std::to_wstring(total) + L" 项\r\n";
    }

    Logger::Warn(L"多选打开：共 " + std::to_wstring(total) + L" 项失败，已合并提示（不再逐项弹框）");

    ::MessageBoxW(m_hWnd, message.c_str(), L"打开文件", MB_OK | MB_ICONWARNING);

    m_batchOpenFailures.clear();
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


// ============================================================================
// 「新建」二级菜单
//
// 内容和系统资源管理器一样：文件夹 + 这台机器上注册过「新建」模板的文件类型。
// 清单是启动时后台枚举好的快照（m_shellNewItems），这里只读内存，零成本。
// 开关打开时每一项都按自绘菜单画（前面带 16px 图标）。
// ============================================================================

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
// 压缩包内部项的解压：入口 + 状态栏
//
// 执行侧（解压、缓存判断、临时目录、进度轮询）全部在 shell::ContainerExtract；
// 这里只做界面这一半：给解压模块提供列表快照里的元数据，并把进度接到状态栏。
// ============================================================================

// ----------------------------------------------------------------------------
// 解压时状态栏：整条一格，文字里自带「(按 ESC 取消)」
//
// 曾经切成"左文字 / 右侧固定(按 ESC 取消)"两格，用户要求取消提示就跟在百分比
// 后面，所以又回到一格。这里的整段文字由 shell::RunExtraction 拼好传进来。
// ----------------------------------------------------------------------------
void MainWindow::ShowExtractStatus(const std::wstring& text)
{
    if (m_hStatusBar == nullptr)
    {
        return;
    }

    if (!m_extractStatusShown)
    {
        // 第一次进来：把状态栏复位成一整格（粘贴进度可能刚把它切成了几段）
        int parts[1] = { -1 };
        ::SendMessageW(m_hStatusBar, SB_SETPARTS, 1, reinterpret_cast<LPARAM>(parts));
        ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, 0);

        m_extractStatusShown = true;
        m_extractStatusText.clear();
    }

    // 文字没变就不重写（每写一次状态栏都要重绘一次）
    if (text == m_extractStatusText)
    {
        return;
    }

    m_extractStatusText = text;
    ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 0,
                   reinterpret_cast<LPARAM>(m_extractStatusText.c_str()));

    // 解压期间主线程在轮询，状态栏自己收不到重绘消息，得推它一把才看得见
    ::InvalidateRect(m_hStatusBar, nullptr, FALSE);
    ::UpdateWindow(m_hStatusBar);
}

// ----------------------------------------------------------------------------
// 解压结束：状态栏恢复成一整格，并把"N 个项目…"写回去
// ----------------------------------------------------------------------------
void MainWindow::HideExtractStatus()
{
    m_extractInProgress = false;
    m_extractStatusText.clear();

    if (m_hStatusBar != nullptr && m_extractStatusShown)
    {
        int parts[1] = { -1 };
        ::SendMessageW(m_hStatusBar, SB_SETPARTS, 1, reinterpret_cast<LPARAM>(parts));
        ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, 0);
    }

    m_extractStatusShown = false;

    // 缓存里还是刚才那串解压文字，清掉才能真的把"N 个项目"写回去
    m_lastStatusText.clear();
    UpdateStatusBar();
}

// ============================================================================
// 把容器内路径解成真实文件（双击打开 / 往外拖 共用）
//
// 解压本身在 shell::ContainerExtract 里；这里只负责：
//   · 从当前列表快照给解压模块提供"大小 / 修改时间 / 是不是文件夹"
//   · 把进度回调接到状态栏，并在解压期间立起 m_extractInProgress
// 返回 false = 用户中途取消（ESC / 松开鼠标）；
// 返回 true 但 realPaths 为空 = 一项都没解出来，原因在 lastError。
// ============================================================================
bool MainWindow::ExtractContainerPaths(const std::vector<std::wstring>& paths,
                                       bool showMouseTip,
                                       std::vector<std::wstring>& realPaths,
                                       std::wstring& lastError)
{
    std::vector<shell::ContainerExtractItem> items;
    unsigned long long                       totalBytes = 0;

    shell::ContainerMetaProvider meta = [this](const std::wstring& path,
                                               shell::ContainerEntryMeta& entryMeta)
    {
        entryMeta.size     = m_fileListView.SizeOfPath(path);
        entryMeta.modified = m_fileListView.ModifiedOfPath(path);
        entryMeta.isFolder = m_fileListView.IsFolderPath(path);
        return true;
    };

    if (!shell::BuildItems(paths, meta, items, totalBytes))
    {
        lastError = L"算不出这一项的解压落点（包可能已经不在了）";
        return true;
    }

    (void)totalBytes;   // 进度由执行器内部按"需要解的项"自己算

    const auto onBegin = [this]()
    {
        m_extractInProgress  = true;
        m_extractStatusShown = false;
        m_extractStatusText.clear();
    };

    const auto onProgress = [this](const std::wstring& text)
    {
        ShowExtractStatus(text);
    };

    const auto onEnd = [this]()
    {
        HideExtractStatus();
    };

    return shell::RunExtraction(items, showMouseTip, onBegin, onProgress, onEnd,
                                realPaths, lastError);
}

// ============================================================================
// 拖出压缩包内部的项
// ============================================================================

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

    // ---- 2) 交给解压执行器（和双击共用同一套），拖拽要显示鼠标提示条 ----
    std::vector<std::wstring> realPaths;
    std::wstring              lastError;

    if (!ExtractContainerPaths(paths, true, realPaths, lastError))
    {
        // 松开左键 / 按 ESC 取消：不进拖动循环（已解完的项留在临时目录当缓存）
        return std::vector<std::wstring>();
    }

    // 一项都没成功：返回空，调用方据此放弃这次拖动
    return realPaths;
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
        // 压缩包内部（zip / rar / 7z / tar，不分格式）：整个列表都不接受放置
        //
        // 返回空串就是对拖拽那边说"这里不能放"：系统给禁止光标、提示条也显示
        // 「⊘ 不能放到 此处」，松手不落地（和资源管理器表现一致）。
        //
        // 以前 zip 被单独当成"内部可写"放行了，结果拖到 zip 内部空白区或文件夹上，
        // 光标显示能复制、松手才弹错 —— 压缩包其实是只读的，不分格式一律不接受。
        // ------------------------------------------------------------------
        if (m_currentIsContainer)
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
        //
        // 但当前目录本身不是真目录时（「此电脑」「控制面板」「网络」「库」「回收站」
        // 这些虚拟文件夹，以及「主文件夹」这种容器视图）往空白处放什么都不合适：
        // 返回空串，拖拽那边给禁止光标、提示条显示「⊘ 不能放到 此处」。
        // 压在里面的文件夹图标上照旧可以放（那才是真目录）。
        const std::wstring currentPath = m_history.Current();
        if (!shell::IsFileSystemPath(currentPath))
        {
            return std::wstring();
        }

        return currentPath;
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
        const std::wstring path = m_addressBar.HitTestPath(ptClient);

        // 面包屑的段必须是真目录：在「此电脑」这类虚拟视图里整条只有一个虚拟段，
        // 拖上去没有落脚点，一律不接受
        if (!shell::IsFileSystemPath(path))
        {
            return std::wstring();
        }

        return path;
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

    // 落点必须是真目录（zip 内部的 C:\a.zip\sub 也算 —— 那是可写容器路径）。
    // 虚拟文件夹（此电脑、控制面板、网络、库、回收站、主文件夹视图）放不进东西，
    // 上面三个入口都判过一遍了，这里再兜一道，防止以后新增入口绕过拖动阶段。
    if (!shell::IsFileSystemPath(targetDirectory))
    {
        Logger::Warn(L"拖放目标不是真实目录，忽略：" + targetDirectory);
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

                // 盘符根（C:\、D:\）没有"最后一段"，按上面的算法会得到空名字，
                // 最后生成一个叫 ".lnk" 的怪文件。这里单独起名：卷标 + 盘符，
                // 没卷标就叫「本地磁盘 (C)」，和资源管理器里给盘符起的名字一致。
                // （GetVolumeInformation 在后台线程里跑，网络盘慢也不卡界面）
                if (fileName.empty() && shell::IsDriveRootPath(source))
                {
                    wchar_t volumeName[MAX_PATH] = {};

                    if (::GetVolumeInformationW(source.c_str(), volumeName, MAX_PATH,
                                                nullptr, nullptr, nullptr, nullptr, 0) &&
                        volumeName[0] != L'\0')
                    {
                        fileName = std::wstring(volumeName) + L" (" + source[0] + L")";
                    }
                    else
                    {
                        fileName = std::wstring(L"本地磁盘 (") + source[0] + L")";
                    }
                }

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

// ----------------------------------------------------------------------------
// 枚举失败：路径在 %TEMP%\Yexp\ 下 = 临时解压缓存被清理了
//
// 场景：双击压缩包里的文件，程序把它解到 %TEMP%\Yexp\...\ 再打开；
// 之后磁盘清理/本程序的缓存清理把目录删了，窗口却还停在那条已经不存在的路径上。
// 不处理的话，每次刷新/目录监视都会再枚举一次、再失败一次，日志里刷一串
// 0x80070002（用户看到的就是日志暴涨、窗口卡在一个空目录）。
//
// 处理：退回最近一个还存在的上级目录；提示只弹一次（同一个窗口里）。
// ----------------------------------------------------------------------------
void MainWindow::HandleTempRootEnumerationFailure(const std::wstring& missingPath)
{
    std::wstring fallback = NavigationHistory::ParentPath(missingPath);

    // 清理是整棵删的，上级目录也可能已经没了，继续往上找
    while (!fallback.empty())
    {
        const DWORD attributes = ::GetFileAttributesW(fallback.c_str());

        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            break;
        }

        fallback = NavigationHistory::ParentPath(fallback);
    }

    if (fallback.empty())
    {
        fallback = L"C:\\";
    }

    Logger::Warn(L"临时解压目录已经不存在，返回上级目录：" + fallback +
                 L"（原路径：" + missingPath + L"）");

    if (!m_tempRootGoneNotified)
    {
        m_tempRootGoneNotified = true;

        ::MessageBoxW(m_hWnd,
                      L"这个临时解压目录已经被清理了，窗口已经返回上级目录。\r\n\r\n"
                      L"压缩包里的文件会先解到临时目录再打开；系统磁盘清理可能会把"
                      L"临时目录删掉，重新打开那个文件就会再解一次。",
                      L"临时目录已清理", MB_OK | MB_ICONINFORMATION);
    }

    NavigateToPath(fallback);
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

    // 切标签带过来的选中/滚动位置：等结果到了再恢复
    if (!m_pendingSelection.empty())
    {
        m_fileListView.SelectPaths(m_pendingSelection);
        m_pendingSelection.clear();
    }
    if (!m_pendingTopItem.empty())
    {
        m_fileListView.EnsureVisiblePath(m_pendingTopItem);
        m_pendingTopItem.clear();
    }

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

    // 当前标签的路径/标题/历史/视图/选中同步回快照
    UpdateActiveTabFromCurrent();

    // Shell 窗口集成：把当前目录报给 IShellWindows（Listary 等工具读这个）
    if (m_shellWindowHost)
    {
        m_shellWindowHost->UpdateFolder(result->path);
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

        // 临时解压目录被清理：退回还在的上级目录，别停在一个已经没了的地方反复枚举
        if (shell::IsUnderTempRoot(result->path))
        {
            HandleTempRootEnumerationFailure(result->path);
        }

        return;
    }

    // 枚举成功：开始盯着这个目录，别的程序改动它时自动刷新
    // （已经在盯同一个目录的话内部会直接返回，不会反复拆建线程）
    // 第 4 个参数是日志路径：日志就写在本程序旁边，用户停在 exe 目录时
    // 每写一行日志都会让监视器以为"目录变了"，必须跳过，否则会自我刷新死循环。
    //
    // 压缩包内部（C:\a.zip\inner）是虚拟路径，ReadDirectoryChangesW 打不开，
    // 每进一次只会刷一条 warning；那里也不会被别的程序原地改，所以直接跳过。
    if (shell::IsFileSystemPath(result->path) && !shell::IsPathInsideContainer(result->path))
    {
        m_watcher.Start(result->path, m_hWnd, WM_APP_FOLDER_CHANGED, Logger::FilePath());
    }
    else
    {
        m_watcher.Stop();
    }

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
            // 和拖拽共用同一套解压：状态栏显示"正在解压… 45%"，按 ESC 可取消。
            // 这里不显示鼠标右下角提示条（showMouseTip = false）。
            std::vector<std::wstring> realPaths;
            std::wstring              lastError;

            if (!ExtractContainerPaths({ entry.fullPath }, false, realPaths, lastError))
            {
                // 按 ESC 取消：不打开文件（已解完的项留在临时目录当缓存）
                Logger::Info(L"打开：解压途中按 ESC，取消打开：" + entry.fullPath);
                return;
            }

            if (realPaths.empty())
            {
                Logger::Error(L"提取容器内部文件失败：" + entry.fullPath +
                              L"，原因：" + (lastError.empty() ? L"未知" : lastError));

                const std::wstring reason = lastError.empty() ? L"解压没有成功" : lastError;

                // 多选打开时先攒着，由 ShowBatchOpenFailures 统一提示
                if (m_collectBatchOpenFailures)
                {
                    m_batchOpenFailures.push_back(OpenFailure{ entry.name, reason });
                    return;
                }

                std::wstring message = L"打开失败：\r\n" + entry.name + L" —— " + reason;
                ::MessageBoxW(m_hWnd, message.c_str(), L"打开文件", MB_OK | MB_ICONWARNING);
                return;
            }

            const std::wstring extractedPath = realPaths.front();

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
    const int tabBarHeight = TabBarHeight();

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
    int buttonY = tabBarHeight + (navHeight - buttonHeight) / 2;
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
    int addressY = tabBarHeight + (navHeight - addressHeight) / 2;
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
    int bodyHeight = clientHeight - tabBarHeight - navHeight - statusHeight;
    if (bodyHeight < 0)
    {
        bodyHeight = 0;
    }

    m_addressDividerRect.left   = 0;
    m_addressDividerRect.top    = tabBarHeight + navHeight;
    m_addressDividerRect.right  = clientWidth;
    m_addressDividerRect.bottom = tabBarHeight + navHeight + 1;

    // ---- 左侧导航栏 + 文件列表 ----
    // 内容区从横线下面 1 像素开始
    int contentY      = tabBarHeight + navHeight + 1;
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
    // 表现就是"有时候能拖、有时候拖不动"。现在两个子窗口各后退 2 像素，
    // 中间 5 像素都归主窗口；因为左栏、列表、父窗口背景都是白底，
    // 视觉上完全看不出差别，只是竖线两侧多了一点空白。
    // 注意：这个数值必须和 IsOnPaneDivider() 里的容差一致，否则会出现
    // "光标变成左右箭头了但按下去拖不动"。
    const int splitterHit = Scale(2);

    // 窗口太窄时收起左栏，保证列表还有足够宽度可看
    bool showPane = (m_navigationPane.Handle() != nullptr)
                    && (clientWidth - paneWidth) >= Scale(360);

    m_paneVisible = showPane;

    if (showPane)
    {
        ::ShowWindow(m_navigationPane.Handle(), SW_SHOW);

        // 左栏铺满整条（0 ~ 竖线）：选中那一行的高亮要占满整条，
        // 图标和文字的 10 像素留白在 NavigationPane 内部处理。
        //
        // 左边留 2px 空白（和右侧观感对称），右边只让出分隔线热区 2px。
        // 只动这两个布局数字，没有额外计算和绘制，不影响性能。
        const int navLeftGap = Scale(2);
        placeWindow(m_navigationPane.Handle(), navLeftGap, contentY,
                    paneWidth - navLeftGap - splitterHit, contentHeight);

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

    // 热区范围必须和 LayoutChildren() 里的实际让位一致，
    // 否则会出现"光标变成左右箭头了但按下去拖不动"：
    //   左侧：导航栏在 splitX - 2 处收边；
    //   右侧：列表在 splitX + 3 处开始。
    int toleranceLeft  = Scale(2);
    int toleranceRight = Scale(2);

    return clientX >= splitX - toleranceLeft && clientX <= splitX + toleranceRight;
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

// ----------------------------------------------------------------------------
// 字节数 → 人能看的大小文字（状态栏选中总大小）
//
// 用 core::format::CapacityText：和「此电脑」容量卡、列表大小列同一套口径
// （1024 进位 + 3 位有效数字）。以前这里另写了一份 "%.1f" 的实现，
// 于是同一次选中的大小在状态栏和列表里可能显示成 "1.2 GB" / "1.23 GB"。
// ----------------------------------------------------------------------------
static std::wstring SelectedSizeText(unsigned long long bytes)
{
    return format::CapacityText(static_cast<uint64_t>(bytes));
}

void MainWindow::UpdateStatusBar()
{
    if (m_hStatusBar == nullptr)
    {
        return;
    }

    // 正在解压压缩包内部的项（双击 / 拖拽）：状态栏让位给"正在解压…"，
    // 文字由 shell::RunExtraction 经回调直接写，这里只负责别把它覆盖掉。
    if (m_extractInProgress.load())
    {
        // 解压结束后要能立刻把"N 个项目"写回去，所以这里把缓存清掉
        m_lastStatusText.clear();
        return;
    }

    // 正在粘贴虚拟文件（远程桌面那类）：状态栏让位给进度（第 i/N 项 + 百分比）。
    // 文案和绘制由 PasteProgressBar 负责，这里只负责别把它覆盖掉。
    if (m_pasteInProgress.load())
    {
        // 进度结束后要能立刻把"N 个项目"写回去，所以这里把缓存清掉
        m_lastStatusText.clear();

        m_pasteBar.RefreshText();
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

            // 选中信息：列表数据已就绪，直接从控件里汇总（只遍历内存里的条目，
            // 不查磁盘）。规则：
            //   选中 1 项   → "选中1个项目"（是文件还要带大小，文件夹没有大小）
            //   选中多项    → "已选择N个项目"；全是文件才带总大小，混了文件夹就不带
            size_t             selectedCount = 0;
            bool               hasFolder     = false;
            unsigned long long selectedSize  = 0;

            m_fileListView.SelectedSummary(selectedCount, hasFolder, selectedSize);

            if (selectedCount == 1)
            {
                text += L"　选中1个项目";

                if (!hasFolder)
                {
                    text += L"　" + SelectedSizeText(selectedSize);
                }
            }
            else if (selectedCount > 1)
            {
                text += L"　已选择" + std::to_wstring(selectedCount) + L"个项目";

                if (!hasFolder)
                {
                    text += L"　" + SelectedSizeText(selectedSize);
                }
            }
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
