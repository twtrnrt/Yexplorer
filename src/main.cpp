// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/main.cpp
//
// 程序入口。职责：
//   1. 初始化日志（写 exe 同目录的 FileExplorer.log）
//   2. 开启 ComCtl32 v6
//   3. 初始化 COM（STA）
//   4. 创建主窗口并进入消息循环
//   5. 退出时清理 COM 与日志
// ============================================================================

#include <windows.h>
#include <objbase.h>        // CoInitializeEx / CoUninitialize
#include <commctrl.h>       // InitCommonControlsEx

#include <string>

#include "core/Logger.h"
#include "ui/MainWnd.h"

// ============================================================================
// 链接所需的系统库
// ============================================================================
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

// ============================================================================
// 内部辅助函数
// ============================================================================

// ----------------------------------------------------------------------------
// 取当前 exe 所在目录（不带末尾反斜杠）
// 用途：日志文件写在 exe 旁边，方便用户直接打开查看
// ----------------------------------------------------------------------------
static std::wstring GetExecutableDirectory()
{
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD length = ::GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    if (length == 0)
    {
        return L".";
    }

    std::wstring fullPath(buffer, length);
    size_t lastSlash = fullPath.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos)
    {
        return L".";
    }
    return fullPath.substr(0, lastSlash);
}

// ----------------------------------------------------------------------------
// 开启 ComCtl32 v6 公共控件
// （清单里已声明，这里再调一次作为双保险）
// ----------------------------------------------------------------------------
static void EnableCommonControls()
{
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES
               | ICC_STANDARD_CLASSES
               | ICC_BAR_CLASSES
               | ICC_LISTVIEW_CLASSES
               | ICC_TREEVIEW_CLASSES;
    ::InitCommonControlsEx(&icc);
}

// ----------------------------------------------------------------------------
// 设置 DPI 感知
// （清单里已声明 PerMonitorV2，这里兜底；若清单已生效该调用会失败，忽略即可）
// ----------------------------------------------------------------------------
static void EnableDpiAwareness()
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

// ============================================================================
// 程序入口
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE /*hPrevInstance*/,
                    LPWSTR    /*lpCmdLine*/,
                    int       /*nCmdShow*/)
{
    // ---------- 步骤 1：日志 ----------
    // 尽早初始化，后面任何一步失败都能留下记录
    std::wstring logPath = GetExecutableDirectory() + L"\\FileExplorer.log";
    Logger::Init(logPath);
    Logger::Info(L"程序启动，日志文件：" + logPath);

    // ---------- 步骤 2：控件与 DPI ----------
    EnableCommonControls();
    EnableDpiAwareness();

    // ---------- 步骤 3：初始化 COM / OLE ----------
    // 用 OleInitialize 而不是 CoInitializeEx：
    //   OLE 拖放（RegisterDragDrop / DoDragDrop）要求线程做过 OLE 初始化，
    //   OleInitialize 内部会顺带做 STA 的 CoInitializeEx，一举两得。
    HRESULT hr = ::OleInitialize(nullptr);
    if (FAILED(hr))
    {
        Logger::Error(L"OLE 初始化失败，HRESULT=" + Logger::FormatHresult(hr));
        ::MessageBoxW(nullptr,
                      L"COM 初始化失败，程序无法启动。",
                      L"文件管理器",
                      MB_ICONERROR | MB_OK);
        Logger::Shutdown();
        return 1;
    }

    int exitCode = 0;

    // ---------- 步骤 4：主窗口与消息循环 ----------
    {
        MainWindow mainWindow;
        if (mainWindow.Create(hInstance))
        {
            mainWindow.Show(SW_SHOW);
            exitCode = mainWindow.RunMessageLoop();
        }
        else
        {
            Logger::Error(L"主窗口创建失败");
            ::MessageBoxW(nullptr,
                          L"主窗口创建失败。",
                          L"文件管理器",
                          MB_ICONERROR | MB_OK);
            exitCode = 2;
        }
    }   // 主窗口对象在此析构，析构时会 join 后台线程

    // ---------- 步骤 5：清理 ----------
    ::OleUninitialize();
    Logger::Info(L"程序退出，退出码=" + std::to_wstring(exitCode));
    Logger::Shutdown();

    return exitCode;
}
