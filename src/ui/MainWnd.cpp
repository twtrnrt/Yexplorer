// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.cpp
//
// 主窗口实现（阶段 1 版本）。
//
// 本阶段目标：验证「后台线程枚举 IShellFolder + 结果回传 UI」这条链路。
//   - 启动时自动枚举 C:\
//   - 枚举在后台线程做，UI 不阻塞
//   - 结果显示在客户区，同时写入日志文件
//
// 阶段 2 会把这段「文本展示」换成真正的虚拟 ListView 文件列表控件。
// ============================================================================

#include "MainWnd.h"
#include "core/Logger.h"

#include <chrono>
#include <string>
#include <vector>

// ============================================================================
// 常量
// ============================================================================

static const wchar_t* kWindowClassName = L"FileExplorerMainWindow";
static const int      kInitialWidth    = 1000;
static const int      kInitialHeight   = 650;
static const wchar_t* kWindowTitle     = L"文件管理器（阶段1：Shell 目录枚举）";

// 自定义消息：后台枚举完成，通知 UI 线程重绘
// WM_APP 之后的数值是留给应用程序自定义的，不会和其它系统消息冲突
static const UINT WM_APP_ENUM_DONE = WM_APP + 1;

// 阶段 1 先固定枚举根目录；阶段 2 会由地址栏/导航决定
static const wchar_t* kInitialPath = L"C:\\";

// 界面上最多展示多少条（日志里记更多）
static const size_t kMaxDisplayItems = 12;

// 日志里最多记多少条（防止根目录项太多把日志刷爆）
static const size_t kMaxLogItems = 200;

// ============================================================================
// 内部辅助函数
// ============================================================================

// ----------------------------------------------------------------------------
// FILETIME → 本地时间文本 "YYYY-MM-DD HH:MM"
// ----------------------------------------------------------------------------
static std::wstring FormatFileTime(const FILETIME& utcFileTime)
{
    if (utcFileTime.dwLowDateTime == 0 && utcFileTime.dwHighDateTime == 0)
    {
        return L"-";
    }

    FILETIME localFileTime = {};
    if (!::FileTimeToLocalFileTime(&utcFileTime, &localFileTime))
    {
        return L"-";
    }

    SYSTEMTIME st = {};
    if (!::FileTimeToSystemTime(&localFileTime, &st))
    {
        return L"-";
    }

    wchar_t buffer[64] = {};
    ::swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return std::wstring(buffer);
}

// ----------------------------------------------------------------------------
// 字节数 → 易读大小
// ----------------------------------------------------------------------------
static std::wstring FormatSize(uint64_t size, bool isFolder)
{
    if (isFolder)
    {
        return L"文件夹";
    }
    if (size < 1024)
    {
        wchar_t buffer[64] = {};
        ::swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(size));
        return std::wstring(buffer);
    }
    if (size < 1024ULL * 1024ULL)
    {
        wchar_t buffer[64] = {};
        ::swprintf_s(buffer, L"%.1f KB", static_cast<double>(size) / 1024.0);
        return std::wstring(buffer);
    }
    if (size < 1024ULL * 1024ULL * 1024ULL)
    {
        wchar_t buffer[64] = {};
        ::swprintf_s(buffer, L"%.1f MB", static_cast<double>(size) / (1024.0 * 1024.0));
        return std::wstring(buffer);
    }
    wchar_t buffer[64] = {};
    ::swprintf_s(buffer, L"%.2f GB", static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0));
    return std::wstring(buffer);
}

// ============================================================================
// 构造 / 析构
// ============================================================================

MainWindow::MainWindow() = default;

MainWindow::~MainWindow()
{
    // 关键：窗口对象销毁前必须确保后台线程已退出。
    // 否则后台线程可能访问已经销毁的 this，造成崩溃。
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
        kWindowTitle,
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
    // 窗口创建完成：启动第一次目录枚举
    // --------------------------------------------------------------
    case WM_CREATE:
        StartEnumeration(kInitialPath);
        return 0;

    // --------------------------------------------------------------
    // 绘制：把枚举结果显示出来
    // --------------------------------------------------------------
    case WM_PAINT:
    {
        PAINTSTRUCT ps = {};
        HDC hdc = ::BeginPaint(m_hWnd, &ps);

        RECT clientRect = {};
        ::GetClientRect(m_hWnd, &clientRect);
        PaintResult(hdc, clientRect);

        ::EndPaint(m_hWnd, &ps);
        return 0;
    }

    // --------------------------------------------------------------
    // 后台枚举完成：写日志并请求重绘
    // --------------------------------------------------------------
    case WM_APP_ENUM_DONE:
        OnEnumerationDone();
        ::InvalidateRect(m_hWnd, nullptr, TRUE);
        return 0;

    // --------------------------------------------------------------
    // 尺寸变化：重绘即可（阶段 2 在这里布局子控件）
    // --------------------------------------------------------------
    case WM_SIZE:
        ::InvalidateRect(m_hWnd, nullptr, TRUE);
        return 0;

    // --------------------------------------------------------------
    // 窗口销毁：先停掉后台任务，再退出消息循环
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
    // 先把上一个任务取消并等它结束，保证同一时刻只有一个枚举在跑
    m_enumToken.Cancel();
    if (m_enumThread.joinable())
    {
        m_enumThread.join();
    }
    m_enumToken.Reset();

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result      = shell::EnumerateResult{};
        m_resultReady = false;
        m_enumRunning = true;
        m_enumPath    = path;
    }

    HWND hwnd = m_hWnd;

    // 后台线程：只做枚举，把结果写成纯数据，然后通知 UI
    m_enumThread = std::thread([this, path, hwnd]()
    {
        auto startTime = std::chrono::steady_clock::now();

        auto enumResult = shell::EnumerateFolder(path, &m_enumToken);

        auto endTime = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_result      = std::move(enumResult);
            m_elapsedMs   = elapsedMs;
            m_resultReady = true;
            m_enumRunning = false;
        }

        // 通知 UI 线程重绘。窗口若已销毁，PostMessage 会失败，无害。
        if (hwnd != nullptr)
        {
            ::PostMessageW(hwnd, WM_APP_ENUM_DONE, 0, 0);
        }
    });
}

void MainWindow::OnEnumerationDone()
{
    // 先把结果在锁内拷出来，避免持锁做 IO
    shell::EnumerateResult snapshot;
    double elapsedMs = 0.0;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (!m_resultReady)
        {
            return;
        }
        snapshot  = m_result;
        elapsedMs = m_elapsedMs;
    }

    // ---------------- 写日志 ----------------
    Logger::Info(L"枚举完成：路径=" + snapshot.path +
                 L"，HRESULT=" + Logger::FormatHresult(snapshot.hr) +
                 L"，条目数=" + std::to_wstring(snapshot.entries.size()) +
                 L"（文件夹 " + std::to_wstring(snapshot.folderCount) +
                 L"，文件 " + std::to_wstring(snapshot.fileCount) +
                 L"），耗时 " + std::to_wstring(static_cast<int>(elapsedMs)) + L" ms" +
                 (snapshot.cancelled ? L"（已取消）" : L""));

    if (FAILED(static_cast<HRESULT>(snapshot.hr)))
    {
        Logger::Error(L"枚举失败，路径=" + snapshot.path);
        return;
    }

    // 明细最多记 kMaxLogItems 条
    size_t logCount = (snapshot.entries.size() < kMaxLogItems) ? snapshot.entries.size() : kMaxLogItems;
    for (size_t i = 0; i < logCount; ++i)
    {
        const auto& entry = snapshot.entries[i];
        Logger::Info(std::wstring(entry.isFolder ? L"[目录] " : L"[文件] ") +
                     entry.name +
                     L"  大小=" + FormatSize(entry.size, entry.isFolder) +
                     L"  修改=" + FormatFileTime(entry.modified));
    }
    if (snapshot.entries.size() > logCount)
    {
        Logger::Info(L"... 其余 " + std::to_wstring(snapshot.entries.size() - logCount) + L" 条未记录");
    }
}

void MainWindow::PaintResult(HDC hdc, const RECT& clientRect)
{
    // 在锁内只拷贝展示需要的少量数据，随后立即释放锁
    long         hr          = E_FAIL;
    bool         ready       = false;
    bool         running     = false;
    bool         cancelled   = false;
    size_t       total       = 0;
    size_t       folderCount = 0;
    size_t       fileCount   = 0;
    double       elapsedMs   = 0.0;
    std::wstring path;
    std::vector<shell::ShellEntry> preview;

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        ready       = m_resultReady;
        running     = m_enumRunning;
        hr          = m_result.hr;
        cancelled   = m_result.cancelled;
        total       = m_result.entries.size();
        folderCount = m_result.folderCount;
        fileCount   = m_result.fileCount;
        elapsedMs   = m_elapsedMs;
        path        = m_enumPath;

        size_t previewCount = (total < kMaxDisplayItems) ? total : kMaxDisplayItems;
        preview.assign(m_result.entries.begin(), m_result.entries.begin() + previewCount);
    }

    // ---------------- 组装显示文本 ----------------
    std::wstring text;
    text += L"枚举路径：" + path + L"\r\n";

    if (!ready)
    {
        if (running)
        {
            text += L"状态：正在后台枚举……（界面未阻塞，可以拖动窗口）\r\n";
        }
        else
        {
            text += L"状态：等待中\r\n";
        }
    }
    else
    {
        text += L"状态：" + std::wstring(FAILED(static_cast<HRESULT>(hr)) ? L"失败" : L"成功") +
                L"  HRESULT=" + Logger::FormatHresult(hr) + L"\r\n";
        if (cancelled)
        {
            text += L"（枚举被取消，结果不完整）\r\n";
        }
        text += L"条目总数：" + std::to_wstring(total) +
                L"（文件夹 " + std::to_wstring(folderCount) +
                L"，文件 " + std::to_wstring(fileCount) + L"）\r\n";
        text += L"枚举耗时：" + std::to_wstring(static_cast<int>(elapsedMs)) + L" ms\r\n";
        text += L"详细清单见 exe 同目录的 FileExplorer.log\r\n";
        text += L"\r\n";

        for (const auto& entry : preview)
        {
            text += std::wstring(entry.isFolder ? L"[目录] " : L"[文件] ") + entry.name +
                    L"    " + FormatSize(entry.size, entry.isFolder) +
                    L"    " + FormatFileTime(entry.modified) + L"\r\n";
        }
        if (total > preview.size())
        {
            text += L"…… 还有 " + std::to_wstring(total - preview.size()) + L" 条\r\n";
        }
    }

    // ---------------- 绘制 ----------------
    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, RGB(32, 32, 32));

    RECT drawRect = clientRect;
    drawRect.left   += 24;
    drawRect.top    += 20;
    drawRect.right  -= 24;
    drawRect.bottom -= 20;

    ::DrawTextW(hdc, text.c_str(), -1, &drawRect,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
}
