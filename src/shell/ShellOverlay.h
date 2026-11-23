// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellOverlay.h
//
// 同步状态角标（Overlay）管理器。
//
// 职责（设计见 docs/图标覆盖角标设计.md）：
//   1. 懒加载枚举注册表 ShellIconOverlayIdentifiers 下的第三方角标处理器，
//      同族（同一 DLL / 同 CLSID 段）归并成一套；
//   2. 按配置（总开关 + 各套显示范围 + 优先级顺序）对条目逐个判定 IsMemberOf，
//      第一个命中的处理器记入 ShellEntry::overlayIndex；
//   3. 按路径缓存判定结果，切目录即清空；
//   4. 提供各处理器的 overlay 小图标，供图标合成时贴到主图标左下角。
//
// 线程约定：枚举实例化、IsMemberOf 判定都在后台 STA 线程进行；UI 线程只按
// overlayIndex 取图标做合成，不直接碰 COM 处理器。
// ============================================================================

#pragma once

#include <windows.h>
#include <shobjidl.h>          // IID_IShellIconOverlayIdentifier
#include <shlobj.h>            // PCIDLIST_ABSOLUTE
#include <wil/com.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>

#include "ShellFolder.h"       // ShellEntry
#include "../core/Settings.h"  // core::OverlaySet

namespace shell
{

// ---------------------------------------------------------------------------
// 角标处理器接口
//
// 与系统 IShellIconOverlayIdentifier 同 vtable 顺序（IsMemberOf → GetOverlayInfo
// → GetPriority），IID 一致。IsMemberOf 第二参是 DWORD 文件属性
// （FILE_ATTRIBUTE_DIRECTORY / HIDDEN / SYSTEM 等位），不是 PIDL。
// ---------------------------------------------------------------------------
MIDL_INTERFACE("0c6c4200-c589-11d0-999a-00c04fd655e1")
IOverlayIdentifier : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE IsMemberOf(LPCWSTR pwszPath,
                                                 DWORD dwAttrib) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOverlayInfo(LPWSTR pwszIconFile, int cchMax,
                                                      int* pIndex, DWORD* pdwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPriority(int* pPriority) = 0;
};

// 单个角标处理器实例（注册表一个槽位对应一个；同族多个槽位归并到同一套配置）
struct OverlayHandler
{
    std::wstring regName;                                  // 注册表子键名（OneDrive1 / Tortoise2Modified…）
    std::wstring setId;                                   // 归并套标识（同族共用，取 DLL 路径小写）
    std::wstring displayName;                              // 套显示名（取不到产品名时退回注册名）

    wil::com_ptr<IOverlayIdentifier> handler;             // 处理器 COM 对象

    std::wstring iconFile;                                 // GetOverlayInfo 返回的图标文件路径
    int          iconIndex = 0;                           // 图标在文件内的索引
    DWORD        flags     = 0;                            // GetOverlayInfo  返回的标志

    // 显示范围（从配置对齐；allPaths=true 时 paths 忽略）
    bool                      allPaths = true;
    std::vector<std::wstring> paths;

    // ---- 稳定性保护（二期 3C 失败熔断）----
    // 第三方处理器质量参差，个别在 IsMemberOf 里会访问违例甚至拖死。
    // failureCount 记「连续失败/崩溃次数」，只要有一次正常应答（命中或 S_FALSE）
    // 就清零；达到阈值即 disabled=true，本次会话不再向这套发判定请求，
    // 避免一套坏处理器反复拖慢枚举、反复触发崩溃。
    int  failureCount = 0;      // 连续失败计数（failure count，成功一次即清零）
    bool disabled     = false;  // 已熔断（disabled，本次会话不再询问这套）
};

class OverlayManager
{
public:
    static OverlayManager& Instance();

    OverlayManager(const OverlayManager&) = delete;
    OverlayManager& operator=(const OverlayManager&) = delete;

    // ------------------------------------------------------------------
    // 释放已实例化的处理器（程序退出 / 配置变更后调用）。
    // 可在任意线程调用：内部把「释放处理器」投递到常驻 STA 线程执行
    // （COM 对象只能在创建它的线程释放），图标 HICON 留在调用方线程销毁。
    // ------------------------------------------------------------------
    void Shutdown();

    // ------------------------------------------------------------------
    // 对条目列表做角标判定，把命中处理器下标写进 ShellEntry::overlayIndex。
    // 可在任意线程调用：内部把任务投递到常驻 STA 线程执行并同步等结果；
    // 等待期间调用方不要再碰 entries。
    // ------------------------------------------------------------------
    void Annotate(std::vector<ShellEntry>& entries);

    // 启用处理器列表（按优先级 + 配置顺序）。当前仅供调试查看；
    // 合成阶段请走 GetOverlayIcon（内部加锁），直接持有这个引用会在
    // 常驻线程整体替换处理器时失效。
    const std::vector<OverlayHandler>& Handlers() const { return m_handlers; }

    // ------------------------------------------------------------------
    // 归并后的套列表（供配置窗口展示）：只读注册表归并同族，不在此实例化 COM。
    // 已存配置的套沿用其名称 / 范围；新发现的套默认处理全部路径、追加末尾。
    // ------------------------------------------------------------------
    std::vector<core::OverlaySet> ListSets();

    // ------------------------------------------------------------------
    // 取某处理器在指定尺寸下的 overlay 小图标 HICON（管理器内按尺寸缓存）。
    // 失败返回 nullptr，调用方按"不叠角标"处理。
    // 【注意】返回的 HICON 归管理器所有，调用方不要 DestroyIcon。
    // ------------------------------------------------------------------
    HICON GetOverlayIcon(size_t handlerIndex, int size);

private:
    OverlayManager() = default;
    ~OverlayManager();

    // 读 HKLM/HKCU 注册表，归并同族、实例化处理器、对齐配置范围与顺序。
    // 【只允许在常驻 STA 线程执行】处理器要在这根线程上实例化
    void EnumerateHandlers();

    // 懒加载：总开关打开时才枚举注册表、实例化处理器；重复调用安全。
    // 【只允许在常驻 STA 线程执行】
    void EnsureLoaded();

    // 切目录清空判定结果缓存（不释放处理器对象本身）。
    // 【只允许在常驻 STA 线程执行】
    void ClearResultCache();

    // 路径是否落在这套的显示范围内（allPaths 直接 true；否则按目录前缀含子目录匹配）
    bool PathInScope(const OverlayHandler& set, const std::wstring& path) const;

    // 单个条目按顺序询问处理器，返回命中下标；未命中返回 -1
    int  ResolveEntry(const ShellEntry& entry);

    // ------------------------------------------------------------------
    // 失败记账（二期 3C 失败熔断）：
    //   NoteHandlerFailure 累计一次失败，达到阈值把该套置为 disabled 并记日志；
    //   NoteHandlerSuccess 一次正常应答即清零，避免偶发失败累积成误熔断。
    //   reason 只用于日志，不参与判定。
    // ------------------------------------------------------------------
    void NoteHandlerFailure(size_t index, const std::wstring& reason);
    void NoteHandlerSuccess(size_t index);

    // ------------------------------------------------------------------
    // 常驻专用 STA 线程（二期 2A 跨线程加固）
    //
    // 背景：这些处理器是 STA 对象，只在创建它的那根线程上才能安全调用。
    // 一期是「第一次枚举时在那根临时枚举线程上创建，下次枚举换了线程
    // 却复用旧对象」，跨线程可能返回 RPC_E_WRONG_THREAD，被判成未命中。
    //
    // 做法：管理器自己养一根不随枚举结束而退出的 STA 线程，处理器的创建、
    // GetOverlayInfo、IsMemberOf、以及释放全部固定在这根线程上；调用方
    // （枚举线程 / UI 线程）只把任务投递过去、等结果回来。
    // 线程带消息泵（MsgWaitForMultipleObjectsEx + PeekMessage），
    // 保证处理器内部若需要 COM 调度或窗口消息时不会被我们自己卡死。
    // ------------------------------------------------------------------
    enum class TaskKind
    {
        Annotate,   // 对一批条目做角标判定（含懒加载处理器）
        Shutdown    // 释放处理器与判定缓存
    };

    bool StartWorkerThread();                 // 懒启动常驻线程，重复调用安全；失败返回 false
    void StopWorkerThread();                  // 请求退出并 join
    void WorkerLoop();                        // 常驻线程主循环（内部自己 CoInitializeEx）
    void PostTaskAndWait(TaskKind kind, std::vector<ShellEntry>* entries);
    void RunAnnotateOnWorker(std::vector<ShellEntry>& entries);  // 只允许在常驻线程执行
    void RunShutdownOnWorker();                                  // 只允许在常驻线程执行

    bool m_loaded = false;

    std::vector<OverlayHandler> m_handlers;

    // 判定结果缓存：小写完整路径 → 处理器下标（-1 表示未命中）；切目录清空
    std::unordered_map<std::wstring, int> m_resultCache;

    // ---- 常驻 STA 线程的线程间设施 ----
    std::thread m_workerThread;              // 常驻 STA 工作线程
    DWORD       m_workerThreadId = 0;        // 该线程的线程 ID（排查日志用）

    HANDLE m_taskEvent = nullptr;            // 自动重置事件：有新任务待执行
    HANDLE m_stopEvent = nullptr;            // 手动重置事件：请求常驻线程退出
    HANDLE m_doneEvent = nullptr;            // 手动重置事件：当前任务已执行完毕

    std::mutex m_taskMutex;                  // 保护下面两个任务字段
    TaskKind   m_taskKind = TaskKind::Annotate;
    std::vector<ShellEntry>* m_taskEntries = nullptr;   // Annotate 任务的目标列表

    // 串行化「投递 + 等待」整个过程：枚举线程投递 Annotate、UI 线程投递 Shutdown
    // 有可能撞在一起，同一时刻只允许一个任务在飞。worker 线程不取这把锁，
    // 所以不会出现「投递方持锁等 worker、worker 又要锁」的死锁。
    std::mutex m_postMutex;

    // ---- worker 线程与 UI 线程共享的状态锁 ----
    // m_handlers 会被 worker 线程整体替换（懒加载 / Shutdown），UI 线程
    // GetOverlayIcon 要读它，必须加锁，否则 vector 重分配会让 UI 读到悬垂引用。
    // 注意：Annotate 判定期间不持这把锁（它和 Shutdown 同在 worker 线程串行），
    // 否则会让 UI 线程等锁卡住。
    std::mutex         m_stateMutex;
    std::vector<HICON> m_deadIcons;          // 待销毁图标：由调用方线程统一 DestroyIcon

    // overlay 图标缓存：处理器下标 → HICON（小 / 大两套）
    std::unordered_map<size_t, HICON> m_iconSmall;
    std::unordered_map<size_t, HICON> m_iconLarge;
};

} // namespace shell
