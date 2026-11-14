// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.cpp
//
// 主窗口实现。
//
// 【当前阶段：骨架自检】
//   本版本只做三件事，目的是验证「工具链 + ComCtl32 v6 + COM 初始化 + 窗口」
//   这条链路在本机是通的：
//     1. 注册并创建主窗口
//     2. 在客户区绘制一段提示文字（能绘制说明消息循环和 GDI 正常）
//     3. 关闭窗口时正常退出进程
//
//   后续阶段才会往里加地址栏、文件列表（虚拟 ListView）、目录树等子控件。
// ============================================================================

#include "MainWnd.h"

// ============================================================================
// 常量定义
// ============================================================================

// 窗口类名（仅本文件内部使用，用 const 避免符号冲突）
static const wchar_t* kWindowClassName = L"FileExplorerMainWindow";

// 窗口初始宽高（自检阶段写死；后续会从配置文件读取并记住用户上次的窗口大小）
static const int kInitialWidth  = 1000;
static const int kInitialHeight = 650;

// 窗口标题
static const wchar_t* kWindowTitle = L"文件管理器（骨架自检）";

// ============================================================================
// 公开方法
// ============================================================================

bool MainWindow::Create(HINSTANCE hInstance)
{
    m_hInstance = hInstance;

    // ------------------------------------------------------------------
    // 第 1 步：注册窗口类
    // ------------------------------------------------------------------
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;     // 宽或高变化时整体重绘（自检阶段够用）
    wc.lpfnWndProc   = &MainWindow::WindowProc;     // 窗口过程
    wc.hInstance     = hInstance;                   // 实例句柄
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);  // 使用系统窗口背景色
    wc.lpszClassName = kWindowClassName;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        // 注册失败通常是窗口类已存在或参数非法，直接返回失败由上层弹提示
        return false;
    }

    // ------------------------------------------------------------------
    // 第 2 步：计算居中位置并创建窗口
    // 说明：GetSystemMetrics(SM_CXSCREEN) 在 PerMonitorV2 高 DPI 下返回的是
    //       主显示器缩放后的逻辑尺寸，自检阶段够用；后续会改成按工作区居中。
    // ------------------------------------------------------------------
    int screenWidth  = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth  - kInitialWidth)  / 2;
    int posY = (screenHeight - kInitialHeight) / 2;

    m_hWnd = CreateWindowExW(
        0,                              // 扩展样式（后续加 WS_EX_ACCEPTFILES 支持拖放）
        kWindowClassName,               // 窗口类名
        kWindowTitle,                   // 窗口标题
        WS_OVERLAPPEDWINDOW,            // 标准可缩放主窗口样式
        posX, posY, kInitialWidth, kInitialHeight,
        nullptr,                        // 父窗口
        nullptr,                        // 菜单
        hInstance,
        this);                          // 关键：把 this 作为创建参数传进去（WM_NCCREATE 里取用）

    return (m_hWnd != nullptr);
}

void MainWindow::Show(int nCmdShow)
{
    if (m_hWnd == nullptr) {
        return;
    }
    ShowWindow(m_hWnd, nCmdShow);
    UpdateWindow(m_hWnd);   // 立即触发一次 WM_PAINT，避免窗口先闪白
}

int MainWindow::RunMessageLoop()
{
    MSG msg = {};
    // GetMessage 返回 0 表示收到 WM_QUIT（正常退出），返回 -1 表示出错
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);     // 把按键消息翻译成字符消息（后续快捷键会用到）
        DispatchMessageW(&msg);     // 分发给对应窗口过程
    }
    return static_cast<int>(msg.wParam);
}

// ============================================================================
// 私有方法
// ============================================================================

LRESULT CALLBACK MainWindow::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        // 窗口创建的第一步：此时 lParam 指向 CREATESTRUCT，
        // 其中的 lpCreateParams 就是 CreateWindowEx 时传进来的 this 指针。
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        if (self != nullptr) {
            self->m_hWnd = hWnd;
            // 把 this 挂到窗口的用户数据区，后续消息从这里取回
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    } else {
        // 后续消息：从窗口用户数据区取回 this
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wParam, lParam);
    }

    // 还没挂上 this（极早期消息）时交给默认处理
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    // --------------------------------------------------------------
    // 绘制：自检阶段在客户区居中画一行提示文字
    // 能正常画出东西 = 消息循环、GDI、窗口过程都工作正常
    // --------------------------------------------------------------
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(m_hWnd, &ps);

        RECT rcClient = {};
        GetClientRect(m_hWnd, &rcClient);

        // 透明背景 + 深蓝色文字，方便一眼看出绘制成功
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 90, 160));

        DrawTextW(hdc,
                  L"骨架自检通过：窗口已创建，COM 已初始化，ComCtl32 v6 已加载。\n"
                  L"下一步：接入 IShellFolder 目录枚举 + 虚拟 ListView 文件列表。",
                  -1,
                  &rcClient,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);

        EndPaint(m_hWnd, &ps);
        return 0;
    }

    // --------------------------------------------------------------
    // 尺寸变化：后续在这里重新布局子控件（地址栏 / 文件列表 / 目录树）
    // --------------------------------------------------------------
    case WM_SIZE:
        return 0;

    // --------------------------------------------------------------
    // 窗口销毁：向消息队列投递退出消息，让 RunMessageLoop 结束
    // --------------------------------------------------------------
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(m_hWnd, msg, wParam, lParam);
}
