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

#include <shellapi.h>   // ShellExecuteW
#include <algorithm>
#include <chrono>

// ============================================================================
// 常量
// ============================================================================

static const wchar_t* kWindowClassName = L"FileExplorerMainWindow";
static const int      kInitialWidth    = 1180;
static const int      kInitialHeight   = 740;
static const wchar_t* kWindowTitleBase = L"文件管理器";

// 自定义消息：后台枚举完成
static const UINT WM_APP_ENUM_DONE = WM_APP + 1;

// 起始路径
static const wchar_t* kInitialPath = L"C:\\";

// 日志里最多记多少条
static const size_t kMaxLogItems = 100;

// Segoe MDL2 Assets 字体里的箭头图标字符（Windows 10/11 自带这套图标字体）
static const wchar_t* kGlyphBack    = L"\xE72B";   // 后退
static const wchar_t* kGlyphForward = L"\xE72A";   // 前进
static const wchar_t* kGlyphUp      = L"\xE74A";   // 向上

// ============================================================================
// 内部辅助函数
// ============================================================================

// 去掉首尾空白
static std::wstring Trim(const std::wstring& text)
{
    size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos)
    {
        return L"";
    }
    size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// 排序：文件夹在前，同类按名称升序
static bool CompareEntries(const shell::ShellEntry& left, const shell::ShellEntry& right)
{
    if (left.isFolder != right.isFolder)
    {
        return left.isFolder;
    }
    return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
}

// ============================================================================
// 构造 / 析构
// ============================================================================

MainWindow::MainWindow() = default;

MainWindow::~MainWindow()
{
    m_enumToken.Cancel();
    if (m_enumThread.joinable())
    {
        m_enumThread.join();
    }

    if (m_hUiFont != nullptr)
    {
        ::DeleteObject(m_hUiFont);
        m_hUiFont = nullptr;
    }
    if (m_hIconFont != nullptr)
    {
        ::DeleteObject(m_hIconFont);
        m_hIconFont = nullptr;
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
    wc.hIcon         = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = ::LoadIconW(nullptr, IDI_APPLICATION);

    if (!::RegisterClassExW(&wc))
    {
        return false;
    }

    int screenWidth  = ::GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth  - kInitialWidth)  / 2;
    int posY = (screenHeight - kInitialHeight) / 2;

    m_hWnd = ::CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitleBase,
        WS_OVERLAPPEDWINDOW,
        posX, posY, kInitialWidth, kInitialHeight,
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
    ::ShowWindow(m_hWnd, nCmdShow);
    ::UpdateWindow(m_hWnd);
}

int MainWindow::RunMessageLoop()
{
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
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

    // 箭头图标字体（Windows 10/11 自带的图标字体，用来画后退/前进/向上）
    m_hIconFont = ::CreateFontW(
        -Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe MDL2 Assets");

    // ---------------- 2. 文件列表 ----------------
    if (!m_fileListView.Create(m_hWnd, m_hInstance))
    {
        Logger::Error(L"文件列表控件创建失败");
        return;
    }
    m_fileListView.SetActivateCallback([this](size_t index)
    {
        OnItemActivated(index);
    });

    // ---------------- 3. 三个导航按钮 ----------------
    DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;

    m_hBackButton = ::CreateWindowExW(0, L"BUTTON", kGlyphBack, buttonStyle,
                                      0, 0, 0, 0, m_hWnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdBack)),
                                      m_hInstance, nullptr);

    m_hForwardButton = ::CreateWindowExW(0, L"BUTTON", kGlyphForward, buttonStyle,
                                         0, 0, 0, 0, m_hWnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdForward)),
                                         m_hInstance, nullptr);

    m_hUpButton = ::CreateWindowExW(0, L"BUTTON", kGlyphUp, buttonStyle,
                                    0, 0, 0, 0, m_hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdUp)),
                                    m_hInstance, nullptr);

    if (m_hIconFont != nullptr)
    {
        ::SendMessageW(m_hBackButton,    WM_SETFONT, reinterpret_cast<WPARAM>(m_hIconFont), TRUE);
        ::SendMessageW(m_hForwardButton, WM_SETFONT, reinterpret_cast<WPARAM>(m_hIconFont), TRUE);
        ::SendMessageW(m_hUpButton,      WM_SETFONT, reinterpret_cast<WPARAM>(m_hIconFont), TRUE);
    }

    // ---------------- 4. 地址栏 ----------------
    m_hAddressEdit = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 0, 0, m_hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdAddress)),
        m_hInstance, nullptr);

    if (m_hUiFont != nullptr)
    {
        ::SendMessageW(m_hAddressEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    }

    // 子类化地址栏，捕获回车
    ::SetWindowSubclass(m_hAddressEdit, &MainWindow::AddressEditSubclassProc, 1,
                        reinterpret_cast<DWORD_PTR>(this));

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

LRESULT CALLBACK MainWindow::AddressEditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                      LPARAM lParam, UINT_PTR idSubclass,
                                                      DWORD_PTR refData)
{
    MainWindow* self = reinterpret_cast<MainWindow*>(refData);

    if (msg == WM_KEYDOWN && wParam == VK_RETURN)
    {
        if (self != nullptr)
        {
            self->NavigateFromAddressBar();
        }
        return 0;
    }

    // 屏蔽回车字符消息，避免系统"叮"一声
    if (msg == WM_CHAR && wParam == L'\r')
    {
        return 0;
    }

    if (msg == WM_NCDESTROY)
    {
        ::RemoveWindowSubclass(hWnd, &MainWindow::AddressEditSubclassProc, idSubclass);
    }

    return ::DefSubclassProc(hWnd, msg, wParam, lParam);
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
        CreateChildControls();

        // 图标缓存：一次初始化，全局复用
        if (m_iconCache.Initialize())
        {
            m_fileListView.SetIconCache(&m_iconCache);
        }
        else
        {
            Logger::Warn(L"系统图标列表初始化失败，列表将不显示图标");
        }

        LayoutChildren();

        m_history.Reset(kInitialPath);
        StartEnumeration(kInitialPath);
        return 0;
    }

    // --------------------------------------------------------------
    // 尺寸变化
    // --------------------------------------------------------------
    case WM_SIZE:
        LayoutChildren();
        return 0;

    // --------------------------------------------------------------
    // 按钮点击
    // --------------------------------------------------------------
    case WM_COMMAND:
    {
        int controlId   = LOWORD(wParam);
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
        if (header != nullptr && header->hwndFrom == m_fileListView.Handle())
        {
            return m_fileListView.HandleNotify(lParam);
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
    // 销毁
    // --------------------------------------------------------------
    case WM_DESTROY:
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

void MainWindow::NavigateFromAddressBar()
{
    wchar_t buffer[2048] = {};
    ::GetWindowTextW(m_hAddressEdit, buffer, ARRAYSIZE(buffer));

    std::wstring path = Trim(std::wstring(buffer));
    if (path.empty())
    {
        // 空路径：恢复显示当前路径
        ::SetWindowTextW(m_hAddressEdit, m_history.Current().c_str());
        return;
    }

    // 用户只输入了 "C:" 这种盘符，补一个反斜杠
    if (path.size() == 2 && path[1] == L':')
    {
        path += L'\\';
    }

    NavigateToPath(path);
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
    std::wstring parent  = NavigationHistory::ParentPath(current);
    if (parent == current)
    {
        return;   // 已经在根目录
    }
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
    m_enumToken.Reset();

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result      = nullptr;
        m_resultReady = false;
        m_enumPath    = path;
    }

    m_fileListView.SetResult(nullptr);

    // 地址栏、标题、状态栏立即反映新路径
    ::SetWindowTextW(m_hAddressEdit, path.c_str());
    ::SetWindowTextW(m_hWnd, (std::wstring(kWindowTitleBase) + L" - " + path + L"（加载中…）").c_str());
    UpdateStatusBar();
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

    std::sort(result->entries.begin(), result->entries.end(), CompareEntries);
    m_fileListView.SetResult(result);

    ::SetWindowTextW(m_hWnd,
                     (std::wstring(kWindowTitleBase) + L" - " + result->path +
                      L"（" + std::to_wstring(result->entries.size()) + L" 项）").c_str());

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

    if (entry.isFolder && !entry.fullPath.empty())
    {
        NavigateToPath(entry.fullPath);
        return;
    }

    if (!entry.isFolder && !entry.fullPath.empty())
    {
        HINSTANCE openResult = ::ShellExecuteW(m_hWnd, L"open", entry.fullPath.c_str(),
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(openResult) <= 32)
        {
            Logger::Error(L"打开文件失败，路径=" + entry.fullPath);
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
    int buttonWidth  = Scale(38);
    int buttonHeight = Scale(30);
    int gap        = Scale(4);
    int navHeight  = Scale(46);
    int addressHeight = Scale(26);

    // 状态栏自己会贴到底部，先让它调整，再取高度
    ::SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);
    RECT statusRect = {};
    ::GetWindowRect(m_hStatusBar, &statusRect);
    int statusHeight = statusRect.bottom - statusRect.top;

    // ---- 导航按钮 ----
    int buttonY = (navHeight - buttonHeight) / 2;
    int cursorX = margin;

    ::MoveWindow(m_hBackButton, cursorX, buttonY, buttonWidth, buttonHeight, TRUE);
    cursorX += buttonWidth + gap;

    ::MoveWindow(m_hForwardButton, cursorX, buttonY, buttonWidth, buttonHeight, TRUE);
    cursorX += buttonWidth + gap;

    ::MoveWindow(m_hUpButton, cursorX, buttonY, buttonWidth, buttonHeight, TRUE);
    cursorX += buttonWidth + Scale(10);

    // ---- 地址栏 ----
    int addressY = (navHeight - addressHeight) / 2;
    int addressWidth = clientWidth - cursorX - margin;
    if (addressWidth < Scale(150))
    {
        addressWidth = Scale(150);
    }
    ::MoveWindow(m_hAddressEdit, cursorX, addressY, addressWidth, addressHeight, TRUE);

    // ---- 文件列表 ----
    int listHeight = clientHeight - navHeight - statusHeight;
    if (listHeight < 0)
    {
        listHeight = 0;
    }
    m_fileListView.Resize(0, navHeight, clientWidth, listHeight);
}

void MainWindow::UpdateNavigationState()
{
    if (m_hBackButton == nullptr)
    {
        return;
    }

    ::EnableWindow(m_hBackButton,    m_history.CanGoBack()    ? TRUE : FALSE);
    ::EnableWindow(m_hForwardButton, m_history.CanGoForward() ? TRUE : FALSE);

    std::wstring current = m_history.Current();
    std::wstring parent  = NavigationHistory::ParentPath(current);
    ::EnableWindow(m_hUpButton, (parent != current) ? TRUE : FALSE);
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
            text = std::to_wstring(m_result->entries.size()) + L" 个项目";
        }
        else
        {
            text = L"正在读取…";
        }
    }

    ::SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}
