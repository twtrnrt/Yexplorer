// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/main.cpp
//
// 程序入口。职责很单一，只做四件事：
//   1. 开启 ComCtl32 v6（现代控件外观 / 现代控件行为）
//   2. 初始化 COM（Shell 相关接口全部依赖 COM）
//   3. 创建并显示主窗口，进入消息循环
//   4. 退出时清理 COM
//
// 为什么必须先初始化 COM：
//   我们后面要用 IShellFolder、IShellItem、IContextMenu、IFileOperation
//   这些接口，它们全部是 COM 对象。没有 CoInitializeEx 直接调用会失败。
//
// 为什么用 STA（单线程套间）而不是 MTA：
//   Shell 接口、OLE 拖放、剪贴板都要求 STA 环境；我们要做拖拽文件进/出，
//   所以必须 COINIT_APARTMENTTHREADED。
// ============================================================================

#include <windows.h>
#include <objbase.h>        // CoInitializeEx / CoUninitialize
#include <commctrl.h>       // InitCommonControlsEx

#include "ui/MainWnd.h"

// ============================================================================
// 链接所需的系统库（用 #pragma comment 写在这里，工程里同时保留，
// 保证直接用 cl.exe 单独编译时也不会缺库）
// ============================================================================
#pragma comment(lib, "comctl32.lib")   // 公共控件（ListView / TreeView / 工具栏）
#pragma comment(lib, "ole32.lib")      // COM 核心
#pragma comment(lib, "shell32.lib")    // Shell 接口（IShellFolder / SHGetFileInfo 等）
#pragma comment(lib, "shlwapi.lib")    // Shell 轻量工具（路径处理等）
#pragma comment(lib, "user32.lib")     // 窗口与消息
#pragma comment(lib, "gdi32.lib")      // 绘图
#pragma comment(lib, "uuid.lib")       // 接口 GUID 定义

// ============================================================================
// 内部辅助函数
// ============================================================================

// ----------------------------------------------------------------------------
// 开启 ComCtl32 v6 公共控件
//
// 说明：清单文件（res/app.manifest）里已经声明了 Common-Controls v6 依赖，
//       正常情况下系统会自动加载 v6。这里再手工调用一次 InitCommonControlsEx
//       作为双保险（如果清单因某些原因没生效，这里能兜住大部分控件类）。
// ----------------------------------------------------------------------------
static void EnableCommonControls()
{
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES          // 基础控件
               | ICC_STANDARD_CLASSES       // 标准控件
               | ICC_BAR_CLASSES            // 工具栏 / 状态栏
               | ICC_LISTVIEW_CLASSES       // 列表视图（文件列表核心）
               | ICC_TREEVIEW_CLASSES;      // 树视图（目录树核心）
    InitCommonControlsEx(&icc);
}

// ----------------------------------------------------------------------------
// 设置 DPI 感知
//
// 说明：清单里已经声明 PerMonitorV2，正常情况下系统会按清单设置；
//       这里再调一次 API 作为兜底。如果清单已生效，此调用会返回失败
//       （E_ACCESSDENIED），属于预期行为，直接忽略即可。
// ----------------------------------------------------------------------------
static void EnableDpiAwareness()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

// ============================================================================
// 程序入口
//
// 用 wWinMain 而不是 WinMain：全程使用宽字符（Unicode），
// 因为 Win32 的 Shell 接口、文件路径都优先使用宽字符，能避免大量转换。
// ============================================================================

int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE /*hPrevInstance*/,
                    LPWSTR    /*lpCmdLine*/,
                    int       /*nCmdShow*/)
{
    // ---------- 步骤 1：控件与 DPI ----------
    EnableCommonControls();
    EnableDpiAwareness();

    // ---------- 步骤 2：初始化 COM ----------
    // COINIT_DISABLE_OLE1DDE：禁用已过时的 OLE1 DDE，减少无谓开销
    HRESULT hr = CoInitializeEx(nullptr,
                                COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        MessageBoxW(nullptr,
                    L"COM 初始化失败，程序无法启动。",
                    L"文件管理器",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    int exitCode = 0;

    // ---------- 步骤 3：创建主窗口并进入消息循环 ----------
    MainWindow mainWindow;
    if (mainWindow.Create(hInstance)) {
        mainWindow.Show(SW_SHOW);
        exitCode = mainWindow.RunMessageLoop();
    } else {
        MessageBoxW(nullptr,
                    L"主窗口创建失败。",
                    L"文件管理器",
                    MB_ICONERROR | MB_OK);
        exitCode = 2;
    }

    // ---------- 步骤 4：清理 COM ----------
    // 必须与 CoInitializeEx 成对调用，否则进程退出时可能残留 COM 资源
    CoUninitialize();

    return exitCode;
}
