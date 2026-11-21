// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/WinEHook.cpp
// ============================================================================

#include "ui/WinEHook.h"

#include "core/Logger.h"

#include <atomic>
#include <thread>

namespace
{

std::atomic<HHOOK> g_hook{ nullptr };
HWND              g_notify = nullptr;
DWORD             g_threadId = 0;
std::atomic<bool> g_running{ false };
std::thread       g_thread;
bool              g_eDown = false;

bool IsWinDown()
{
    return (::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
           (::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
}

bool OtherModifiersDown()
{
    return (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
           (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            if (key != nullptr && key->vkCode == 'E' && IsWinDown() && !OtherModifiersDown())
            {
                // 长按的重复事件：继续吞掉，但不再重复开窗口
                if (!g_eDown)
                {
                    g_eDown = true;
                    if (g_notify != nullptr)
                    {
                        ::PostMessageW(g_notify, ui::kWinEMessage, 0, 0);
                    }
                }
                return 1;
            }
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
        {
            if (key != nullptr && key->vkCode == 'E')
            {
                g_eDown = false;
            }
        }
    }

    return ::CallNextHookEx(g_hook.load(), code, wParam, lParam);
}

void HookThread()
{
    g_threadId = ::GetCurrentThreadId();

    HHOOK installed = ::SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                          ::GetModuleHandleW(nullptr), 0);
    g_hook.store(installed);

    if (installed == nullptr)
    {
        Logger::Warn(L"Win+E 钩子安装失败（错误码 " + std::to_wstring(::GetLastError()) + L"）");
        g_running = false;
        return;
    }

    Logger::Info(L"Win+E 钩子已安装");

    MSG msg = {};
    while (g_running && ::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    HHOOK current = g_hook.exchange(nullptr);
    if (current != nullptr)
    {
        ::UnhookWindowsHookEx(current);
    }
    Logger::Info(L"Win+E 钩子已卸载");
}

} // namespace

namespace ui
{

bool StartWinEHook(HWND notify)
{
    if (g_running)
    {
        g_notify = notify;
        return true;
    }

    g_notify = notify;
    g_eDown = false;
    g_running = true;
    g_thread = std::thread(HookThread);

    // 等线程把钩子装好（最多 500ms），失败时把 running 复位
    for (int i = 0; i < 50 && g_running && g_hook.load() == nullptr; ++i)
    {
        ::Sleep(10);
    }

    if (g_hook.load() == nullptr)
    {
        g_running = false;
        if (g_thread.joinable())
        {
            g_thread.join();
        }
        return false;
    }
    return true;
}

void StopWinEHook()
{
    if (!g_running.exchange(false))
    {
        if (g_thread.joinable())
        {
            g_thread.join();
        }
        return;
    }

    if (g_threadId != 0)
    {
        ::PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    }

    if (g_thread.joinable())
    {
        g_thread.join();
    }

    g_threadId = 0;
    g_notify = nullptr;
    g_eDown = false;
}

} // namespace ui
