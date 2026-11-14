// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/FolderWatcher.h
//
// 目录变化监视。
//
// 【为什么需要它】
//   以前列表只在"本程序自己做完文件操作"之后刷新。只要文件是被别的程序
//   （资源管理器、微信、命令行、正在运行的脚本）改动的，列表就一直显示旧内容，
//   用户得自己按 F5。这里用系统的 ReadDirectoryChangesW 盯着当前目录，
//   一有变化就通知主窗口刷新。
//
// 【为什么单独开一条线程】
//   ReadDirectoryChangesW 是阻塞式的：没有变化时它就卡在那里不返回。
//   放在界面线程里会把整个窗口冻住，所以单独开一条后台线程等。
//
// 【性能上怎么保证"不影响速度"】
//   1. 只在"真的有变化"时才通知，空闲时这条线程完全睡着，不占 CPU
//   2. 主窗口收到通知后合并处理（见 MainWnd 里的防抖定时器），
//      批量复制一万个文件也只会触发一次刷新
//   3. 刷新走的是"差异对比"路径（FileListView::MergeResult），只重绘真正变了的那几行
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <thread>

namespace core
{

class FolderWatcher
{
public:
    FolderWatcher() = default;
    ~FolderWatcher();

    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    // 开始监视某个目录。已经在监视同一个目录时什么都不做（避免反复拆建线程）。
    //   notifyWindow ：有变化时往哪个窗口发消息
    //   message      ：发什么消息（主窗口自己定义，比如 WM_APP + 3）
    void Start(const std::wstring& path, HWND notifyWindow, UINT message);

    // 停止监视（切换目录、窗口关闭时都要调）
    void Stop();

    // 现在是否正在监视这个目录
    bool IsWatching(const std::wstring& path) const;

private:
    // 后台线程主体：循环等待变化
    void ThreadMain();

    HANDLE       m_dirHandle    = nullptr;   // 被监视目录的句柄
    HANDLE       m_stopEvent    = nullptr;   // 停止信号
    HWND         m_notifyWindow = nullptr;   // 往谁发消息
    UINT         m_message      = 0;         // 发哪条消息
    std::wstring m_path;                     // 当前监视的目录
    std::thread  m_thread;
};

} // namespace core
