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
#include <objbase.h>        // OleInitialize / OleUninitialize
#include <commctrl.h>       // InitCommonControlsEx
#include <shlobj.h>         // SetCurrentProcessExplicitAppUserModelID

#include <string>

#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/ExplorerProxy.h"   // 管理员身份下部署/移除 System32 里的 explorer.exe 代理
#include "shell/ListaryIntegration.h"   // 顺手把内嵌的 Listary 插件装进 Listary 目录
#include "shell/ShellFolder.h"     // IsContainerFileExtension：压缩包要当目录打开
#include "ui/BackgroundHost.h"
#include "ui/MainWnd.h"
#include "ui/WindowRelay.h"   // "这个目录已经开着了"就交给已有窗口

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

// ----------------------------------------------------------------------------
// 从命令行里取出"启动目录"
//
// 用途：右键「在新窗口中打开」时，本程序会带着一个目录参数重新启动自己。
// lpCmdLine 不含程序名，可能长这样：
//     "C:\Users\user\Desktop\owtest"
//      C:\Windows
// 这里把首尾空白和成对的引号去掉，剩下的当一个目录用。
// ----------------------------------------------------------------------------
static std::wstring ExtractStartupPath(LPWSTR cmdLine)
{
    if (cmdLine == nullptr)
    {
        return std::wstring();
    }

    std::wstring text(cmdLine);

    // 去首尾空白
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t'))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
    {
        text.pop_back();
    }

    if (text.empty())
    {
        return std::wstring();
    }

    // 被引号包着 → 取引号里面的内容
    if (text.front() == L'"')
    {
        text.erase(text.begin());

        size_t closing = text.find(L'"');
        if (closing != std::wstring::npos)
        {
            text = text.substr(0, closing);
        }
    }

    return text;
}

// ============================================================================
// 程序入口
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE /*hPrevInstance*/,
                    LPWSTR    lpCmdLine,
                    int       /*nCmdShow*/)
{
    // ---------- 步骤 1：日志 ----------
    // 尽早初始化，后面任何一步失败都能留下记录
    std::wstring logPath = GetExecutableDirectory() + L"\\FileExplorer.log";
    Logger::Init(logPath);
    Logger::Info(L"程序启动，日志文件：" + logPath);

    // ---------- 步骤 1.5：特殊命令行（提权后执行的代理部署/移除） ----------
    // 「注册为默认文件管理器」和「恢复系统默认文件管理器」需要往 System32 里
    // 放/删一个 explorer.exe 代理，那需要管理员权限。
    // 做法是主程序用 ShellExecuteEx(runas) 把自己再启动一次，带下面这两个开关，
    // 由这个提权实例干完活、弹窗报告、然后退出——不会进入正常的主窗口流程。
    {
        std::wstring command = lpCmdLine != nullptr ? lpCmdLine : L"";
        while (!command.empty() && (command.front() == L' ' || command.front() == L'\t'))
        {
            command.erase(command.begin());
        }
        while (!command.empty() && (command.back() == L' ' || command.back() == L'\t'))
        {
            command.pop_back();
        }

        if (command == L"--deploy-proxy" || command == L"--remove-proxy")
        {
            Logger::Info(L"以管理员身份执行代理操作：" + command);

            const int result = shell::RunProxyCommandLine(command == L"--remove-proxy");

            // 注册为默认文件管理器时顺手装 Listary 插件（同一把 UAC，无额外步骤）。
            // 恢复系统默认时不动它：插件只影响 Listary，留着不影响系统还原。
            if (command == L"--deploy-proxy")
            {
                shell::InstallListaryPlugin(true);
            }

            Logger::Info(L"代理操作结束，退出码=" + std::to_wstring(result));
            Logger::Shutdown();
            return result;
        }

        if (command == L"--install-listary-plugin")
        {
            const int installed = shell::InstallListaryPlugin(true);
            Logger::Info(L"Listary 插件安装结果=" + std::to_wstring(installed));
            Logger::Shutdown();
            return installed < 0 ? 1 : 0;
        }

        if (command == L"--remove-listary-plugin")
        {
            const bool removed = shell::RemoveListaryPlugin(true);
            Logger::Info(L"Listary 插件删除结果=" + std::wstring(removed ? L"成功" : L"失败"));
            Logger::Shutdown();
            return removed ? 0 : 1;
        }
    }

    // ---------- 步骤 2：控件与 DPI ----------
    EnableCommonControls();
    EnableDpiAwareness();

    // 给进程一个固定的应用标识（AppUserModelID）。
    // 不设的话，每次改窗口标题（比如刷新后更新"（N 项）"）系统都会
    // 重新生成任务栏按钮，表现为任务栏图标闪一下、窗口像失去了焦点。
    ::SetCurrentProcessExplicitAppUserModelID(L"Kf168.FileExplorer");

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

    // ---------- 步骤 3.5：后台宿主（--background） ----------
    // 开机启动项/设置里启动的就是这个：托盘常驻 + 可选 Win+E 钩子 + 隐藏热窗口。
    {
        std::wstring command = (lpCmdLine != nullptr) ? std::wstring(lpCmdLine) : std::wstring();
        while (!command.empty() && (command.front() == L' ' || command.front() == L'\t'))
        {
            command.erase(command.begin());
        }
        while (!command.empty() && (command.back() == L' ' || command.back() == L'\t'))
        {
            command.pop_back();
        }

        if (command == L"--background")
        {
            Logger::Info(L"以后台宿主身份启动");
            const int backgroundExit = ui::RunBackgroundHost(hInstance);
            ::OleUninitialize();
            Logger::Info(L"后台宿主退出码=" + std::to_wstring(backgroundExit));
            Logger::Shutdown();
            return backgroundExit;
        }
    }

    // ---------- 步骤 4：主窗口与消息循环 ----------
    {
        // 命令行参数有两种可能：
        //   1) 一个目录（「在新窗口中打开」就是这么传的）→ 直接打开它
        //   2) 一个文件（别的程序「打开所在文件夹」、或者被当成某扩展的打开方式）
        //      → 打开它所在的目录，并把这个文件选中高亮
        std::wstring startupPath = ExtractStartupPath(lpCmdLine);
        std::wstring startupSelectFile;

        // 没有显式路径（直接启动 / 任务栏 / 开始菜单）时，按设置里的「启动位置」决定。
        // 显式路径（双击文件夹、/select、右键在新窗口打开）永远优先。
        if (startupPath.empty() || startupPath.front() == L'-')
        {
            core::Settings::Instance().Load();
            startupPath = core::Settings::Instance().ResolveStartupPath();
        }

        if (!startupPath.empty())
        {
            // 判断是不是"存在的文件"。目录、取不到属性的（不存在/无权限）都按原样走：
            // 目录照旧打开；不存在的路径照旧让主窗口回退到默认目录。
            //
            // 压缩包（.zip/.rar/.7z…）例外：磁盘上是文件，但要当"目录"打开进去浏览，
            // 所以不按"文件 → 打开它所在目录并选中"处理，路径原样传下去。
            const DWORD attributes = ::GetFileAttributesW(startupPath.c_str());
            const bool  isContainerFile = shell::IsContainerFileExtension(startupPath);

            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                !isContainerFile)
            {
                const size_t slash = startupPath.find_last_of(L"\\/");

                if (slash != std::wstring::npos)
                {
                    startupSelectFile = startupPath;

                    // 父目录。注意 "C:\a.txt" 会切出 "C:"，
                    // 那不是"根目录"的写法，得补一个反斜杠
                    startupPath = startupPath.substr(0, slash);

                    if (startupPath.size() == 2 && startupPath[1] == L':')
                    {
                        startupPath.push_back(L'\\');
                    }
                }
            }

            Logger::Info(L"命令行参数：" + startupPath +
                         (startupSelectFile.empty() ? L"" : L"（启动后选中：" + startupSelectFile + L"）"));
        }

        // 「强制新开一个窗口」开关：Ctrl+N / Ctrl+T 启动新进程时会带上它。
        // 有它就跳过下面"该目录已有窗口就拉前台"的去重，一定真的开一个新窗口。
        const std::wstring startupCommandLine =
            (lpCmdLine != nullptr) ? std::wstring(lpCmdLine) : std::wstring();
        const bool forceNewWindow =
            (startupCommandLine.find(L"--new-window") != std::wstring::npos);

        // 已经有一个窗口停在这个目录（或这个文件所在的目录）：
        // 把那个窗口拉到前台，自己不再开新窗口。
        // 只在"带路径启动"时问一次，不带参数的正常启动（比如点任务栏图标）不受影响。
        const bool relayedToExistingWindow =
            !forceNewWindow &&
            !startupPath.empty() &&
            ui::RelayOpenFolderToExistingWindow(startupPath, startupSelectFile, nullptr);

        if (relayedToExistingWindow)
        {
            Logger::Info(L"该目录已有窗口，本次启动交给它，不再开新窗口：" + startupPath);
        }
        else
        {
            MainWindow mainWindow;
            if (mainWindow.Create(hInstance, startupPath, startupSelectFile))
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
        }
    }   // 主窗口对象在此析构，析构时会 join 后台线程

    // ---------- 步骤 5：清理 ----------
    ::OleUninitialize();
    Logger::Info(L"程序退出，退出码=" + std::to_wstring(exitCode));
    Logger::Shutdown();

    return exitCode;
}
