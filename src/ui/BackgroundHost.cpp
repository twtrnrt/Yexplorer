// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/BackgroundHost.cpp
// ============================================================================

#include "ui/BackgroundHost.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "ui/MainWnd.h"
#include "ui/WinEHook.h"
#include "../res/resource.h"

#include <memory>
#include <string>

namespace
{

const wchar_t* kHostClassName = L"FileExplorerBackgroundHost";
const wchar_t* kHostMutexName = L"Local\\Yexplorer.BackgroundHost";

constexpr UINT kMsgTray   = WM_APP + 41;
constexpr UINT kMsgReload = WM_APP + 42;
constexpr UINT kMsgStop   = WM_APP + 43;

constexpr UINT kTrayId = 1;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuExit = 2;

std::wstring OwnExePath()
{
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return (length == 0 || length >= ARRAYSIZE(buffer)) ? std::wstring()
                                                        : std::wstring(buffer, length);
}

class BackgroundHost
{
public:
    int Run(HINSTANCE instance)
    {
        m_instance = instance;

        // 单实例：已经有一个宿主在跑就直接退出
        m_mutex = ::CreateMutexW(nullptr, FALSE, kHostMutexName);
        if (m_mutex == nullptr || ::GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (m_mutex != nullptr) { ::CloseHandle(m_mutex); }
            return 0;
        }

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &BackgroundHost::WndProc;
        wc.hInstance     = instance;
        wc.lpszClassName = kHostClassName;

        if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            Logger::Warn(L"后台宿主：窗口类注册失败");
            return 1;
        }

        m_hWnd = ::CreateWindowExW(0, kHostClassName, L"Yexplorer 后台", WS_POPUP,
                                   0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (m_hWnd == nullptr)
        {
            Logger::Warn(L"后台宿主：窗口创建失败");
            return 1;
        }

        AddTray();
        ReloadSettings();

        Logger::Info(L"后台宿主已启动");

        // 用热窗口的消息循环：这样地址栏快捷键（Ctrl+L/T/N…）、粘贴取消等
        // MainWindow 自己的消息预处理都还在；宿主窗口的消息也会一起分发。
        if (m_warm && m_warm->Handle() != nullptr)
        {
            m_warm->RunMessageLoop();
        }
        else
        {
            MSG msg = {};
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }

        Logger::Info(L"后台宿主已退出");

        if (m_mutex != nullptr)
        {
            ::CloseHandle(m_mutex);
            m_mutex = nullptr;
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        BackgroundHost* self = nullptr;

        if (msg == WM_NCCREATE)
        {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<BackgroundHost*>(cs->lpCreateParams);
            if (self != nullptr)
            {
                self->m_hWnd = hWnd;
                ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
        }
        else
        {
            self = reinterpret_cast<BackgroundHost*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        }

        if (self != nullptr)
        {
            return self->HandleMessage(msg, wParam, lParam);
        }
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case ui::kWinEMessage:
            ShowWarmWindow();
            return 0;

        case kMsgReload:
            ReloadSettings();
            return 0;

        case kMsgStop:
            StopBackground();
            return 0;

        case kMsgTray:
            OnTray(lParam);
            return 0;

        case WM_CLOSE:
            ExitHost();
            return 0;

        case WM_DESTROY:
            RemoveTray();
            ui::StopWinEHook();
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
        }
        return ::DefWindowProcW(m_hWnd, msg, wParam, lParam);
    }

    void ReloadSettings()
    {
        core::Settings& settings = core::Settings::Instance();
        settings.ReloadFromDisk();

        // 取消后台时托盘被移除了；再次启用要加回来
        if (m_tray.cbSize == 0)
        {
            AddTray();
        }

        if (settings.TakeOverWinE())
        {
            if (!ui::StartWinEHook(m_hWnd))
            {
                Logger::Warn(L"后台宿主：Win+E 钩子启动失败");
            }
        }
        else
        {
            ui::StopWinEHook();
        }

        EnsureWarmWindow();
    }

    void EnsureWarmWindow()
    {
        if (m_warm && m_warm->Handle() != nullptr)
        {
            return;
        }

        m_warm = std::make_unique<MainWindow>();
        m_warm->SetHideOnClose(true);

        const std::wstring path = core::Settings::Instance().ResolveStartupPath();
        if (!m_warm->Create(m_instance, path, std::wstring()))
        {
            Logger::Warn(L"后台宿主：热窗口创建失败");
            m_warm.reset();
        }
    }

    void ShowWarmWindow()
    {
        EnsureWarmWindow();
        if (!m_warm || m_warm->Handle() == nullptr)
        {
            Logger::Warn(L"后台宿主：没有可用的窗口");
            return;
        }

        // 已经有一个可见窗口：Win+E 按"新开一个"处理
        if (::IsWindowVisible(m_warm->Handle()))
        {
            LaunchNewWindow();
            return;
        }

        // 隐藏的热窗口：导航到设置的位置再显示
        const std::wstring path = core::Settings::Instance().ResolveStartupPath();
        if (!path.empty())
        {
            m_warm->OpenPath(path);
        }

        m_warm->Show(SW_SHOW);
        ::SetForegroundWindow(m_warm->Handle());
    }

    void LaunchNewWindow()
    {
        const std::wstring exe = OwnExePath();
        const std::wstring path = core::Settings::Instance().ResolveStartupPath();
        if (exe.empty() || path.empty())
        {
            return;
        }

        std::wstring command = L"\"" + exe + L"\" \"" + path + L"\" --new-window";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info = {};

        if (::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                             nullptr, nullptr, &startup, &info))
        {
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
            Logger::Info(L"Win+E：新开窗口，位置=" + path);
        }
        else
        {
            Logger::Warn(L"Win+E：新开窗口失败（错误码 " +
                         std::to_wstring(::GetLastError()) + L"）");
        }
    }

    void AddTray()
    {
        m_tray = {};
        m_tray.cbSize           = sizeof(m_tray);
        m_tray.hWnd             = m_hWnd;
        m_tray.uID              = kTrayId;
        m_tray.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        m_tray.uCallbackMessage = kMsgTray;
        m_tray.hIcon            = ::LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APPICON));
        if (m_tray.hIcon == nullptr)
        {
            m_tray.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
        }
        wcscpy_s(m_tray.szTip, L"Yexplorer（后台）");

        if (!::Shell_NotifyIconW(NIM_ADD, &m_tray))
        {
            Logger::Warn(L"后台宿主：托盘图标添加失败");
        }
    }

    void RemoveTray()
    {
        if (m_tray.cbSize != 0)
        {
            ::Shell_NotifyIconW(NIM_DELETE, &m_tray);
            m_tray = {};
        }
    }

    void OnTray(LPARAM event)
    {
        if (event == WM_LBUTTONDBLCLK)
        {
            ShowWarmWindow();
            return;
        }

        if (event != WM_RBUTTONUP)
        {
            return;
        }

        HMENU menu = ::CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        ::AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

        POINT pt = {};
        ::GetCursorPos(&pt);
        ::SetForegroundWindow(m_hWnd);
        const UINT command = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                              pt.x, pt.y, 0, m_hWnd, nullptr);
        ::DestroyMenu(menu);
        ::PostMessageW(m_hWnd, WM_NULL, 0, 0);

        if (command == kMenuOpen)
        {
            ShowWarmWindow();
        }
        else if (command == kMenuExit)
        {
            ExitHost();
        }
    }

    void StopBackground()
    {
        Logger::Info(L"后台宿主：收到停止请求");
        ui::StopWinEHook();
        RemoveTray();

        if (!m_warm || m_warm->Handle() == nullptr)
        {
            ::PostQuitMessage(0);
            return;
        }

        m_warm->SetHideOnClose(false);

        if (!::IsWindowVisible(m_warm->Handle()))
        {
            ::DestroyWindow(m_warm->Handle());
            m_warm.reset();
            ::PostQuitMessage(0);
        }
        // 可见窗口：让它继续显示，用户关掉时走正常退出
    }

    void ExitHost()
    {
        ui::StopWinEHook();
        RemoveTray();

        if (m_warm)
        {
            m_warm->SetHideOnClose(false);
            if (m_warm->Handle() != nullptr)
            {
                ::DestroyWindow(m_warm->Handle());
            }
            m_warm.reset();
        }

        ::PostQuitMessage(0);
    }

private:
    HINSTANCE        m_instance = nullptr;
    HWND             m_hWnd = nullptr;
    HANDLE           m_mutex = nullptr;
    NOTIFYICONDATAW  m_tray = {};
    std::unique_ptr<MainWindow> m_warm;
};

} // namespace

namespace ui
{

int RunBackgroundHost(HINSTANCE instance)
{
    BackgroundHost host;
    return host.Run(instance);
}

bool StartBackgroundHost()
{
    if (IsBackgroundHostRunning())
    {
        return true;
    }

    const std::wstring exe = OwnExePath();
    if (exe.empty())
    {
        return false;
    }

    std::wstring command = L"\"" + exe + L"\" --background";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};

    if (!::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                          nullptr, nullptr, &startup, &info))
    {
        Logger::Warn(L"启动后台宿主失败（错误码 " +
                     std::to_wstring(::GetLastError()) + L"）");
        return false;
    }

    ::CloseHandle(info.hThread);
    ::CloseHandle(info.hProcess);

    // 等窗口出现（最多 2 秒）
    for (int i = 0; i < 20; ++i)
    {
        if (::FindWindowW(kHostClassName, nullptr) != nullptr)
        {
            return true;
        }
        ::Sleep(100);
    }
    return true;
}

void StopBackgroundHost()
{
    HWND host = ::FindWindowW(kHostClassName, nullptr);
    if (host != nullptr)
    {
        ::PostMessageW(host, kMsgStop, 0, 0);
    }
}

void ReloadBackgroundHost()
{
    HWND host = ::FindWindowW(kHostClassName, nullptr);
    if (host != nullptr)
    {
        ::PostMessageW(host, kMsgReload, 0, 0);
    }
}

bool IsBackgroundHostRunning()
{
    return ::FindWindowW(kHostClassName, nullptr) != nullptr;
}

} // namespace ui
