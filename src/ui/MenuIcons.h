// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MenuIcons.h
//
// 右键菜单里用的小图标（16px）缓存。
//
// 【为什么要有这个东西】
//   菜单项前面要画图标，但取图标本身是个"重活"：要问 Shell、要访问磁盘，
//   每个程序几十微秒到几毫秒。右键菜单的要求是"弹出零成本"，所以
//   **绝不能**在弹菜单那一刻去取图标。
//
// 【做法】
//   后台线程一次性把所有需要的图标取好，存进内存表（HICON）。
//   菜单弹出时只查表：有就画，没有就留空，绝不触发任何 IO。
//
//   程序启动时预取一次；「打开方式」设置窗口关掉后再增量补一次
//   （用户可能刚添加了新程序）。
//
// 【图标从哪来】
//   Win32 程序 → SHGetFileInfoW(真实 exe 路径)
//   商店应用   → SHParseDisplayName("shell:AppsFolder\<AUMID>") 再问图标
//   新建项     → SHGetFileInfoW(假文件名 + SHGFI_USEFILEATTRIBUTES)
// ============================================================================

#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Settings.h"
#include "shell/ShellNew.h"

class MenuIconCache
{
public:
    MenuIconCache() = default;
    ~MenuIconCache();

    MenuIconCache(const MenuIconCache&) = delete;
    MenuIconCache& operator=(const MenuIconCache&) = delete;

    // ------------------------------------------------------------------
    // 把清单交给后台线程去取图标（已经有的不会重复取）
    // 传进来的是快照，后台线程不再回头读配置（避免和主线程抢数据）
    // ------------------------------------------------------------------
    void PrefetchPrograms(const std::vector<core::OpenWithItem>& programs);
    void PrefetchNewItems(const std::vector<shell::ShellNewItem>& items);

    // 任意程序/文件路径的图标（手动添加的程序用）
    void PrefetchPaths(const std::vector<std::wstring>& paths);

    // 等后台线程收尾（程序退出前调，避免它还在跑就析构了）
    void Shutdown();

    // 查一个图标。没有返回 nullptr（菜单画空位，不阻塞）
    HICON Get(const std::wstring& key);

    // 查动态组二级项采集下来的 BMP 图标（相对 menu-icons 目录或绝对路径）
    HICON GetFile(const std::wstring& iconFile);

    // 查某个应用/来源自己的图标（动态组二级项没采到真实图标时兜底）
    HICON GetOwnerIcon(const std::wstring& server, const std::wstring& appName);

    // ---- 缓存键（键必须稳定，换一次就等于缓存失效）----
    static std::wstring KeyForProgram(const core::OpenWithItem& item);
    static std::wstring KeyForNewItem(const shell::ShellNewItem& item);
    static std::wstring KeyForFile(const std::wstring& iconFile);
    static std::wstring KeyForOwner(const std::wstring& server, const std::wstring& appName);
    static std::wstring KeyForPath(const std::wstring& path);

private:
    void StartWorker();

    // 取一个程序的 16px 图标（失败返回 nullptr）
    static HICON LoadProgramIcon(const core::OpenWithItem& item);

    // 取一个「新建」项的 16px 图标
    static HICON LoadNewItemIcon(const shell::ShellNewItem& item);

    // 取动态组二级项落盘的 BMP 图标
    static HICON LoadFileIcon(const std::wstring& iconFile);

    // 取应用/来源自己的图标
    static HICON LoadOwnerIcon(const std::wstring& server, const std::wstring& appName);

    // 取任意路径的图标（手动添加的程序）
    static HICON LoadPathIcon(const std::wstring& path);

    void Store(const std::wstring& key, HICON icon);

private:
    std::mutex                          m_mutex;
    std::unordered_map<std::wstring, HICON> m_icons;

    // 后台工作线程（一次一个，取完就退）
    std::thread                         m_worker;

    // 待取清单
    std::vector<core::OpenWithItem>     m_pendingPrograms;
    std::vector<shell::ShellNewItem>    m_pendingNewItems;
    std::vector<std::wstring>           m_pendingPaths;
    std::atomic<bool>                   m_workerRunning { false };
};
