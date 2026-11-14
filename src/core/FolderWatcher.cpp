// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/FolderWatcher.cpp
//
// 目录变化监视实现（ReadDirectoryChangesW）。
// ============================================================================

#include "FolderWatcher.h"

#include "core/Logger.h"

#include <vector>

namespace core
{

FolderWatcher::~FolderWatcher()
{
    Stop();
}

void FolderWatcher::Start(const std::wstring& path, HWND notifyWindow, UINT message)
{
    if (path.empty() || notifyWindow == nullptr || message == 0)
    {
        return;
    }

    // 已经在盯着同一个目录了，不用重来（每次刷新都重启线程是浪费）
    if (IsWatching(path) && m_notifyWindow == notifyWindow && m_message == message)
    {
        return;
    }

    Stop();

    // 打开目录句柄。
    // FILE_FLAG_BACKUP_SEMANTICS 是打开"目录"（而不是文件）时必须带的标志。
    m_dirHandle = ::CreateFileW(path.c_str(),
                                FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);

    if (m_dirHandle == INVALID_HANDLE_VALUE)
    {
        m_dirHandle = nullptr;
        Logger::Warn(L"目录监视启动失败（打不开目录）：" + path);
        return;
    }

    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr)
    {
        ::CloseHandle(m_dirHandle);
        m_dirHandle = nullptr;
        return;
    }

    m_path         = path;
    m_notifyWindow = notifyWindow;
    m_message      = message;

    m_thread = std::thread(&FolderWatcher::ThreadMain, this);
}

void FolderWatcher::Stop()
{
    if (m_thread.joinable())
    {
        ::SetEvent(m_stopEvent);

        // ReadDirectoryChangesW 正阻塞在 m_dirHandle 上，取消掉它才会返回
        if (m_dirHandle != nullptr)
        {
            ::CancelIoEx(m_dirHandle, nullptr);
        }

        m_thread.join();
    }

    if (m_dirHandle != nullptr)
    {
        ::CloseHandle(m_dirHandle);
        m_dirHandle = nullptr;
    }

    if (m_stopEvent != nullptr)
    {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }

    m_path.clear();
    m_notifyWindow = nullptr;
    m_message      = 0;
}

bool FolderWatcher::IsWatching(const std::wstring& path) const
{
    return m_thread.joinable() && !m_path.empty() &&
           ::_wcsicmp(m_path.c_str(), path.c_str()) == 0;
}

void FolderWatcher::ThreadMain()
{
    // 64KB 缓冲：一大批文件同时变化时，一次就能读回来，不用读好几轮
    const DWORD bufferSize = 64 * 1024;
    std::vector<BYTE> buffer(bufferSize);

    while (::WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT)
    {
        DWORD bytesReturned = 0;

        BOOL ok = ::ReadDirectoryChangesW(
            m_dirHandle,
            buffer.data(),
            bufferSize,
            TRUE,                                                       // 连子目录一起盯
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_ATTRIBUTES,
            &bytesReturned,
            nullptr,                                                    // 同步模式，不用 OVERLAPPED
            nullptr);

        // 失败要么是目录没了，要么是我们调 CancelIoEx 主动打断（停止监视）
        if (ok == FALSE)
        {
            break;
        }

        // 变化太多导致缓冲区溢出时 bytesReturned 为 0，这时同样要通知一次（让上层重扫）
        if (m_notifyWindow != nullptr)
        {
            ::PostMessageW(m_notifyWindow, m_message, 0, 0);
        }
    }
}

} // namespace core
