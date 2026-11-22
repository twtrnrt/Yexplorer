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

#include "ShellFolder.h"       // ShellEntry

namespace shell
{

// ---------------------------------------------------------------------------
// 角标处理器接口（自定义签名）
//
// SDK 头文件把 IShellIconOverlayIdentifier::IsMemberOf 的第二参数声明成了
// DWORD dwAttrib，但真实运行时传的是绝对 PIDL 指针；64 位下按 DWORD 传会截断
// 指针。这里按真实 vtable 顺序（IsMemberOf → GetOverlayInfo → GetPriority）
// 自定义一份签名正确的接口，IID 与系统接口一致。
// ---------------------------------------------------------------------------
MIDL_INTERFACE("0c6c4200-c589-11d0-999a-00c04fd655e1")
IOverlayIdentifier : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE IsMemberOf(LPCWSTR pwszPath,
                                                 PCIDLIST_ABSOLUTE pidl) = 0;
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
};

class OverlayManager
{
public:
    static OverlayManager& Instance();

    OverlayManager(const OverlayManager&) = delete;
    OverlayManager& operator=(const OverlayManager&) = delete;

    // ------------------------------------------------------------------
    // 懒加载：总开关打开时才枚举注册表、实例化处理器；重复调用安全。
    // 总开关关闭时不初始化，等价未做本功能。
    // ------------------------------------------------------------------
    void EnsureLoaded();

    // 释放已实例化的处理器（程序退出 / 总开关关闭时调用）
    void Shutdown();

    // 切目录清空判定结果缓存（不释放处理器对象本身）
    void ClearResultCache();

    // ------------------------------------------------------------------
    // 对条目列表做角标判定，把命中处理器下标写进 ShellEntry::overlayIndex。
    // 必须在已 CoInitializeEx(STA) 的后台线程调用。
    // ------------------------------------------------------------------
    void Annotate(std::vector<ShellEntry>& entries);

    // 启用处理器列表（按优先级 + 配置顺序），供合成阶段取 overlay 图标
    const std::vector<OverlayHandler>& Handlers() const { return m_handlers; }

    // ------------------------------------------------------------------
    // 取某处理器在指定尺寸下的 overlay 小图标 HICON（管理器内按尺寸缓存）。
    // 失败返回 nullptr，调用方按"不叠角标"处理。
    // 【注意】返回的 HICON 归管理器所有，调用方不要 DestroyIcon。
    // ------------------------------------------------------------------
    HICON GetOverlayIcon(size_t handlerIndex, int size);

private:
    OverlayManager() = default;
    ~OverlayManager();

    // 读 HKLM/HKCU 注册表，归并同族、实例化处理器、对齐配置范围与顺序
    void EnumerateHandlers();

    // 路径是否落在这套的显示范围内（allPaths 直接 true；否则按目录前缀含子目录匹配）
    bool PathInScope(const OverlayHandler& set, const std::wstring& path) const;

    // 单个条目按顺序询问处理器，返回命中下标；未命中返回 -1
    int  ResolveEntry(const ShellEntry& entry);

private:
    bool m_loaded = false;

    std::vector<OverlayHandler> m_handlers;

    // 判定结果缓存：小写完整路径 → 处理器下标（-1 表示未命中）；切目录清空
    std::unordered_map<std::wstring, int> m_resultCache;

    // overlay 图标缓存：处理器下标 → HICON（小 / 大两套）
    std::unordered_map<size_t, HICON> m_iconSmall;
    std::unordered_map<size_t, HICON> m_iconLarge;
};

} // namespace shell
