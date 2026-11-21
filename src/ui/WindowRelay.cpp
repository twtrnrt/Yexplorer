#include "ui/WindowRelay.h"

#include <vector>

#include "core/Logger.h"

namespace ui
{

const wchar_t kMainWindowClassName[] = L"FileExplorerMainWindow";

namespace
{

// 找到的候选窗口：句柄 + 进程 ID（置前授权要用 PID）
struct CandidateWindow
{
    HWND  hwnd;
    DWORD processId;
};

BOOL CALLBACK CollectCandidateWindows(HWND hwnd, LPARAM lParam)
{
    std::vector<CandidateWindow>* out = reinterpret_cast<std::vector<CandidateWindow>*>(lParam);
    if (out == nullptr)
    {
        return TRUE;
    }

    // 只认本程序的顶层窗口：按注册的类名比对，不碰别的程序
    wchar_t className[128] = {};
    if (::GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0)
    {
        return TRUE;
    }

    if (::_wcsicmp(className, kMainWindowClassName) != 0)
    {
        return TRUE;
    }

    // 不可见的窗口（比如隐藏的句柄）不算
    if (!::IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    DWORD processId = 0;
    ::GetWindowThreadProcessId(hwnd, &processId);

    CandidateWindow candidate = {};
    candidate.hwnd      = hwnd;
    candidate.processId = processId;
    out->push_back(candidate);
    return TRUE;
}

// 把定长数组成员清零后拷字符串，超长就截断（不越界）
void CopyToFixedBuffer(const std::wstring& text, wchar_t* buffer, size_t bufferCount)
{
    if (buffer == nullptr || bufferCount == 0)
    {
        return;
    }

    ::wcsncpy_s(buffer, bufferCount, text.c_str(), _TRUNCATE);
}

}   // namespace

std::wstring NormalizeFolderForCompare(const std::wstring& path)
{
    std::wstring normalized = path;

    // 虚拟位置（此电脑、控制面板、shell:xxx）原样比，不做路径规范化
    const bool isVirtual = (normalized.compare(0, 2, L"::") == 0) ||
                           (normalized.compare(0, 6, L"shell:") == 0);

    if (!isVirtual)
    {
        // 先修掉拖尾的斜杠；但 "C:\" 这种盘符根只有 3 个字符，去掉斜杠就变成 "C:"，
        // 那是"当前驱动器"的写法，不是同一个路径，所以保留。
        while (normalized.size() > 3 &&
               (normalized.back() == L'\\' || normalized.back() == L'/'))
        {
            normalized.pop_back();
        }

        if (normalized.size() == 3 && normalized[1] == L':' &&
            normalized.back() == L'\\')
        {
            // "C:\" 原样保留
        }
        else if (normalized.size() == 2 && normalized[1] == L':')
        {
            // "C:" 也算"当前驱动器"，不额外补反斜杠（比较时两边同样处理就行）
        }
    }

    return normalized;
}

bool IsSameFolder(const std::wstring& left, const std::wstring& right)
{
    if (left.empty() || right.empty())
    {
        return false;
    }

    const std::wstring normalizedLeft  = NormalizeFolderForCompare(left);
    const std::wstring normalizedRight = NormalizeFolderForCompare(right);

    return ::_wcsicmp(normalizedLeft.c_str(), normalizedRight.c_str()) == 0;
}

bool ForceForegroundWindow(HWND window)
{
    if (window == nullptr)
    {
        return false;
    }

    if (::IsIconic(window))
    {
        ::ShowWindow(window, SW_RESTORE);
    }
    ::ShowWindow(window, SW_SHOW);

    // 先把窗口临时置顶、马上取消置顶：这一步只改 Z 序，不需要前台权限，
    // 能保证窗口一定盖在其它普通窗口上面（SetForegroundWindow 失败时也能看见）。
    ::SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ::SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    ::BringWindowToTop(window);

    // 再抢焦点：临时挂到当前前台线程上，否则会被前台锁静默忽略。
    const HWND foreground = ::GetForegroundWindow();
    const DWORD foregroundThread =
        (foreground != nullptr) ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = ::GetCurrentThreadId();

    bool attached = false;
    if (foregroundThread != 0 && foregroundThread != currentThread)
    {
        attached = ::AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    }

    ::SetForegroundWindow(window);
    ::BringWindowToTop(window);
    ::SetActiveWindow(window);

    if (attached)
    {
        ::AttachThreadInput(currentThread, foregroundThread, FALSE);
    }

    Logger::Info(std::wstring(L"置前检查：visible=") + (::IsWindowVisible(window) ? L"是" : L"否") +
                 L"，iconic=" + (::IsIconic(window) ? L"是" : L"否") +
                 L"，foreground=" + ((::GetForegroundWindow() == window) ? L"是" : L"否"));

    return ::IsWindowVisible(window) != FALSE;
}

namespace
{

struct AllMainWindow
{
    HWND hwnd = nullptr;
    DWORD processId = 0;
};

BOOL CALLBACK CollectAllMainWindows(HWND hwnd, LPARAM lParam)
{
    auto* out = reinterpret_cast<std::vector<AllMainWindow>*>(lParam);
    wchar_t className[64] = {};
    ::GetClassNameW(hwnd, className, ARRAYSIZE(className));
    if (::_wcsicmp(className, kMainWindowClassName) != 0)
    {
        return TRUE;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    out->push_back(AllMainWindow{ hwnd, pid });
    return TRUE;
}

std::vector<AllMainWindow> AllMainWindows()
{
    std::vector<AllMainWindow> windows;
    ::EnumWindows(&CollectAllMainWindows, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

}   // namespace

bool RelayOpenInExistingWindow(const std::wstring& folder,
                               const std::wstring& selectFile,
                               HWND selfWindow)
{
    if (folder.empty() || folder.size() >= 520 || selectFile.size() >= 520)
    {
        return false;
    }

    RelayRequest request = {};
    CopyToFixedBuffer(NormalizeFolderForCompare(folder), request.folder, ARRAYSIZE(request.folder));
    CopyToFixedBuffer(selectFile, request.selectFile, ARRAYSIZE(request.selectFile));

    COPYDATASTRUCT data = {};
    data.dwData = kRelayOpenTabMagic;
    data.cbData = sizeof(request);
    data.lpData = &request;

    DWORD selfProcessId = 0;
    if (selfWindow != nullptr)
    {
        ::GetWindowThreadProcessId(selfWindow, &selfProcessId);
    }

    for (const AllMainWindow& candidate : AllMainWindows())
    {
        BOOL granted = FALSE;
        if (candidate.processId != selfProcessId)
        {
            granted = ::AllowSetForegroundWindow(candidate.processId);
        }
        (void)granted;

        DWORD_PTR reply = 0;
        const LRESULT sent = ::SendMessageTimeoutW(candidate.hwnd, WM_COPYDATA,
                                                   reinterpret_cast<WPARAM>(selfWindow),
                                                   reinterpret_cast<LPARAM>(&data),
                                                   SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                                   2000, &reply);
        if (sent != 0 && reply != 0)
        {
            ::ShowWindow(candidate.hwnd, SW_SHOW);
            ForceForegroundWindow(candidate.hwnd);
            return true;
        }
    }
    return false;
}

bool ActivateExistingWindow(HWND selfWindow)
{
    const std::vector<AllMainWindow> windows = AllMainWindows();
    if (windows.empty())
    {
        return false;
    }

    HWND fallback = nullptr;
    for (const AllMainWindow& candidate : windows)
    {
        if (candidate.hwnd == selfWindow)
        {
            continue;
        }
        if (::IsWindowVisible(candidate.hwnd) && ::GetForegroundWindow() == candidate.hwnd)
        {
            return ForceForegroundWindow(candidate.hwnd);
        }
        if (::IsWindowVisible(candidate.hwnd) && fallback == nullptr)
        {
            fallback = candidate.hwnd;
        }
    }

    HWND target = fallback;
    if (target == nullptr)
    {
        for (const AllMainWindow& candidate : windows)
        {
            if (candidate.hwnd != selfWindow)
            {
                target = candidate.hwnd;
                break;
            }
        }
    }

    if (target == nullptr)
    {
        return false;
    }

    ::ShowWindow(target, SW_SHOW);
    return ForceForegroundWindow(target);
}

bool RelayOpenFolderToExistingWindow(const std::wstring& folder,
                                     const std::wstring& selectFile,
                                     HWND selfWindow)
{
    if (folder.empty() || folder.size() >= 520 || selectFile.size() >= 520)
    {
        return false;
    }

    std::vector<CandidateWindow> candidates;
    ::EnumWindows(&CollectCandidateWindows, reinterpret_cast<LPARAM>(&candidates));

    if (candidates.empty())
    {
        return false;
    }

    RelayRequest request = {};
    CopyToFixedBuffer(NormalizeFolderForCompare(folder), request.folder, ARRAYSIZE(request.folder));
    CopyToFixedBuffer(selectFile, request.selectFile, ARRAYSIZE(request.selectFile));

    COPYDATASTRUCT data = {};
    data.dwData = kRelayRequestMagic;
    data.cbData = sizeof(request);
    data.lpData = &request;

    DWORD selfProcessId = 0;
    if (selfWindow != nullptr)
    {
        ::GetWindowThreadProcessId(selfWindow, &selfProcessId);
    }

    for (const CandidateWindow& candidate : candidates)
    {
        // 对方要自己置前，先给它"允许抢前台"的授权（否则 Windows 会把
        // 后台进程的 SetForegroundWindow 直接忽略掉，表现为任务栏闪一下没反应）。
        BOOL granted = FALSE;
        if (candidate.processId != selfProcessId)
        {
            granted = ::AllowSetForegroundWindow(candidate.processId);
        }

        DWORD_PTR reply = 0;
        const LRESULT sent = ::SendMessageTimeoutW(candidate.hwnd, WM_COPYDATA,
                                                   reinterpret_cast<WPARAM>(selfWindow),
                                                   reinterpret_cast<LPARAM>(&data),
                                                   SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                                   2000,   // 最多等 2 秒，对方卡住也不会拖住调用方
                                                   &reply);

        if (sent != 0 && reply != 0)
        {
            // 接收方在 WM_COPYDATA 里也会置前；这里再补一次，并且由"被前台
            // 程序启动的转发进程"来调用，成功率比接收方自己调用更高。
            const bool foreground = ForceForegroundWindow(candidate.hwnd);

            Logger::Info(L"目录已有窗口，已交给它：" + folder +
                         L"（抢前台授权=" + (granted ? L"有" : L"无") +
                         L"，最终前台=" + (foreground ? L"是" : L"否") + L"）");
            return true;
        }
    }

    return false;
}

}   // namespace ui
