// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.cpp
//
// 主窗口实现（阶段 2 版本）。
//
// 相比阶段 1 的变化：
//   - 客户区从「画文字」换成真正的虚拟 ListView 文件列表
//   - 双击文件夹进入该目录
//   - 双击文件调用系统默认程序打开
//   - 结果按「文件夹在前、名称升序」排序（资源管理器默认顺序）
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
static const int      kInitialWidth    = 1100;
static const int      kInitialHeight   = 700;
static const wchar_t* kWindowTitleBase = L"文件管理器";

// 自定义消息：后台枚举完成
static const UINT WM_APP_ENUM_DONE = WM_APP + 1;

// 起始路径
static const wchar_t* kInitialPath = L"C:\\";

// 日志里最多记多少条
static const size_t kMaxLogItems = 100;

// ============================================================================
// 内部辅助函数
// ============================================================================

// ----------------------------------------------------------------------------
// 条目排序规则：文件夹在前，同类按名称升序
// 说明：_wcsicmp 是按字符编码比较，对中文不是「拼音序」，
//       真正的本地化排序放到后续阶段用 CompareStringEx 优化。
// ----------------------------------------------------------------------------
static bool CompareEntries(const shell::ShellEntry& left, const shell::ShellEntry& right)
{
    if (left.isFolder != right.isFolder)
    {
        return left.isFolder;   // 文件夹排在前面
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
}

// ============================================================================
// 公开方法
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
// 私有方法
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
    // 创建文件列表控件，然后启动第一次枚举
    // --------------------------------------------------------------
    case WM_CREATE:
        if (!m_fileListView.Create(m_hWnd, m_hInstance))
        {
            Logger::Error(L"文件列表控件创建失败");
            return -1;  // 返回 -1 会让 CreateWindowEx 失败
        }
        m_fileListView.SetActivateCallback([this](size_t index)
        {
            OnItemActivated(index);
        });
        StartEnumeration(kInitialPath);
        return 0;

    // --------------------------------------------------------------
    // 尺寸变化：重排子控件
    // --------------------------------------------------------------
    case WM_SIZE:
        LayoutChildren();
        return 0;

    // --------------------------------------------------------------
    // 子控件通知：转发给文件列表
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
    // 窗口销毁：停后台任务，退出消息循环
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

void MainWindow::StartEnumeration(const std::wstring& path)
{
    // 取消上一个任务并等待结束，保证同一时刻只有一个枚举
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

    // 立刻清空列表并显示"加载中"，让用户知道正在切目录
    m_fileListView.SetResult(nullptr);
    ::SetWindowTextW(m_hWnd, (std::wstring(kWindowTitleBase) + L" - " + path + L"（加载中…）").c_str());

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
    std::wstring path;

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (!m_resultReady || m_result == nullptr)
        {
            return;
        }
        result    = m_result;
        elapsedMs = m_elapsedMs;
        path      = m_result->path;
    }

    // ---------------- 排序 ----------------
    // 排序在交给列表控件之前完成，保证控件拿到的数据不会再被改动
    std::sort(result->entries.begin(), result->entries.end(), CompareEntries);

    // ---------------- 交给列表显示 ----------------
    m_fileListView.SetResult(result);

    // ---------------- 更新窗口标题 ----------------
    ::SetWindowTextW(m_hWnd,
                     (std::wstring(kWindowTitleBase) + L" - " + path +
                      L"（" + std::to_wstring(result->entries.size()) + L" 项）").c_str());

    // ---------------- 写日志 ----------------
    Logger::Info(L"枚举完成：路径=" + result->path +
                 L"，HRESULT=" + Logger::FormatHresult(result->hr) +
                 L"，条目数=" + std::to_wstring(result->entries.size()) +
                 L"（文件夹 " + std::to_wstring(result->folderCount) +
                 L"，文件 " + std::to_wstring(result->fileCount) +
                 L"），耗时 " + std::to_wstring(static_cast<int>(elapsedMs)) + L" ms" +
                 (result->cancelled ? L"（已取消）" : L""));

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

    // 文件夹 → 进入该目录
    if (entry.isFolder && !entry.fullPath.empty())
    {
        Logger::Info(L"双击进入目录：" + entry.fullPath);
        StartEnumeration(entry.fullPath);
        return;
    }

    // 文件 → 用系统默认关联程序打开
    if (!entry.isFolder && !entry.fullPath.empty())
    {
        Logger::Info(L"双击打开文件：" + entry.fullPath);
        HINSTANCE openResult = ::ShellExecuteW(m_hWnd, L"open", entry.fullPath.c_str(),
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(openResult) <= 32)
        {
            Logger::Error(L"打开文件失败，路径=" + entry.fullPath);
        }
        return;
    }

    // 虚拟项（此电脑等）阶段 2 暂不处理
    Logger::Warn(L"双击的项暂不支持打开：" + entry.name);
}

void MainWindow::LayoutChildren()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    // 阶段 2：列表铺满整个客户区。
    // 阶段 3 会在顶部加上地址栏和工具栏，届时这里要留出顶部高度。
    int width  = clientRect.right  - clientRect.left;
    int height = clientRect.bottom - clientRect.top;

    m_fileListView.Resize(0, 0, width, height);
}
