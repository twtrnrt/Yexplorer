// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuInventory.h
//
// 右键菜单「可靠归属」模型（新方案，替代按菜单文字猜品牌的旧普查）。
//
// 设计目标：
//   · owner（归属）由「我们实际调用了哪个 handler」决定，不看文字、不看 logo；
//   · 每项同时保存执行描述，点击时可以调回原 handler / 原静态 verb；
//   · 右键弹出只读缓存，不查注册表、不调 COM；
//   · 找不到 owner 时明确标记 unknown，绝不按文字硬猜成 WPS。
//
// 一期只做「采集 + 缓存 + 可查询」，不接界面和弹出链路。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "shell/ContextMenuData.h"

namespace shell
{

// 菜单项由什么执行
enum class MenuInvokeKind
{
    StaticVerb = 0,       // 注册表静态 verb，ShellExecuteEx 执行
    ShellHandler = 1,     // ContextMenuHandlers 的 IContextMenu，InvokeCommand 执行
    PackagedCommand = 2, // Win11 IExplorerCommand
    WindowsBuiltin = 3,  // 系统内置项，走 shell 默认执行
};

// 归属：一个应用 / 一个 shell 扩展 / 一条静态 verb 来源
struct MenuOwner
{
    MenuInvokeKind kind = MenuInvokeKind::ShellHandler;
    std::wstring   key;          // 稳定身份：shellex:{CLSID} / static:{注册表路径}
    std::wstring   clsid;        // shell 扩展 CLSID；静态项为空
    std::wstring   server;       // DLL / EXE 路径；静态项尽量填命令 EXE
    std::wstring   appName;      // 从版本资源/包信息解析出的应用名
    std::wstring   displayName;  // 用户可改的显示名（采集时先等于 appName）
};

// 点击时怎么执行
struct MenuInvoke
{
    MenuInvokeKind kind = MenuInvokeKind::ShellHandler;
    std::wstring   clsid;            // ShellHandler
    std::wstring   verb;             // 静态 verb / PackagedCommand 的子命令索引路径（如 "0/2"）
    int            commandOffset = -1; // ShellHandler 的命令偏移
    std::wstring   commandLine;      // 静态 verb 的命令行
    std::wstring   delegateExecute;  // 静态 verb 的 DelegateExecute GUID
    std::wstring   packageId;        // PackagedCommand 的包标识
};

// 一条菜单项。children 保留原始子菜单结构，方便以后按原样显示。
struct MenuInventoryItem
{
    std::wstring id;              // 稳定 ID（owner + 文字路径 + 命令偏移）
    std::wstring text;            // 菜单文字（已去 &）
    std::wstring verb;            // 菜单项 verb（可能为空）
    int          level = 0;       // 0=一级
    bool         separator = false;
    bool         disabled = false;
    std::wstring iconFile;        // 采集到的图标文件；空=没采到
    std::wstring iconSource;      // icon / hbmp / handler / none
    MenuOwner    owner;
    MenuInvoke   invoke;
    unsigned int scenes = 0;                       // MenuScene 位
    std::vector<std::wstring> exts;               // 适用扩展名
    std::vector<MenuInventoryItem> children;      // 子菜单
};

// 动态组 owner 缓存：只登记「哪个应用/哪个 handler 在哪个场景出现」，
// 不预先采集它展开后有哪些菜单项，更不保存扩展名矩阵。
// 真正展开二级菜单时再用 CaptureOwnerMenu 现场采集。
struct DynamicOwner
{
    MenuInvokeKind kind = MenuInvokeKind::ShellHandler;
    std::wstring   key;          // shellex:{CLSID} / packaged:{CLSID}
    std::wstring   clsid;
    std::wstring   server;       // DLL / EXE
    std::wstring   appName;      // 从版本资源/包信息解析出的应用名
    unsigned int   scenes = 0;   // MenuScene 位
    std::wstring   sampleExt;    // 只用于展开/探测时造样本文件，不在界面显示

    // 下面三个字段只在“重建 owner 缓存”时更新，配置窗口和右键展开都不预查。
    // displayName：真正的一级菜单文字（如“打开方式”），不是厂商名。
    // nameSource：menu=现场抓到的标题；registry=注册表 handler 键名；
    //             clsid=CLSID 默认名；app=应用名。
    // probeStatus：ok=可正常初始化并产出菜单；unavailable=初始化/查询失败；
    //              invalid=CLSID 无效或没有 IContextMenu；unchecked=没有可探测场景。
    std::wstring   displayName;
    std::wstring   nameSource;
    std::wstring   probeStatus;
    std::wstring   probeError;   // 失败原因，只写日志/提示，不显示成长菜单

    // 现代包做过一次经典 IContextMenu 能力探测后记下结果，避免每次开设置都重试：
    // ""=没探测；"ok"=补了经典；"failed"=不支持；"skip"=已有经典来源。
    std::wstring   classicProbe;
};

// 缓存文件：%APPDATA%\FileExplorer\menu-owners.json
std::wstring DynamicOwnerCachePath();
bool LoadDynamicOwnerCache(std::vector<DynamicOwner>& owners);
bool SaveDynamicOwnerCache(const std::vector<DynamicOwner>& owners);

// 只扫注册表和 AppxManifest，不初始化任何 shell 扩展。
bool EnumerateDynamicOwners(std::vector<DynamicOwner>& owners, std::wstring* error);

// 重新生成 owner 缓存：先扫注册表，再对每个 owner 做一次“标题+健康”探测。
// 探测只把一级菜单文字和可用状态写进缓存，不保存菜单项树、不保存扩展名矩阵。
bool BuildDynamicOwnerCache();

// 给「只注册了现代菜单、但同一个 CLSID 仍实现经典 IContextMenu」的应用补一条
// ShellHandler owner。WinRAR 7.x 就是这种情况：PackagedCom 里有它，经典
// ContextMenuHandlers 里没有，但 RarExt.dll 仍能按经典接口实例化。
// 只做一次、结果写回 owner 缓存，不写注册表；失败就跳过。
bool EnsureClassicFallbacks();

// 一个扩展名的采集结果
struct MenuInventoryExtension
{
    std::wstring ext;              // 带点，如 ".bmp"
    bool         ok = false;
    std::wstring failReason;
    std::wstring samplePath;
    std::vector<MenuInventoryItem> items; // 顶层项，children 表示子菜单
};

// 一次完整普查
struct MenuInventoryReport
{
    std::wstring scannedAt;
    std::vector<MenuInventoryExtension> extensions;
};

// 采集一个扩展名。sampleDir 由调用方提供（会创建 sample.ext 空文件）。
// 只采集 owner 明确的 handler / 静态 verb；采不到的项不会凭空生成。
bool CollectExtensionInventory(const std::wstring& sampleDir,
                               const std::wstring& ext,
                               MenuInventoryExtension& out);

// 只采一个 CLSID（调试/验证用；owner 已知时使用）
bool CollectHandlerInventory(const std::wstring& sampleDir,
                             const std::wstring& ext,
                             const std::wstring& clsid,
                             MenuInventoryExtension& out);

// 采集一个「场景」的菜单（文件夹 / 空白区 / 多选）。
// 和文件扩展名采集走同一条路：注册表只给 handler 名单，真实初始化时才产出菜单。
//   · SceneFolder     → 选中一个文件夹，父 PIDL = 文件夹上一级
//   · SceneBackground → 文件夹空白区，父 PIDL = 文件夹本身，无选中项
//   · SceneMultiple   → 选中多个文件，父 PIDL = 样本目录
// sampleDir 由调用方提供；函数内部会创建临时样本，用完自己删。
bool CollectSceneInventory(unsigned int scene,
                           const std::wstring& sampleDir,
                           std::vector<MenuInventoryItem>& out,
                           std::wstring* error);

// 二级菜单现场采集：给一个已经知道身份的 handler，按当前真实路径重新问一遍。
// 一级菜单不调用这个函数；只有用户展开某个应用组时才调用。
//   · ShellHandler    → 用父 PIDL + SHCreateDataObject 初始化，QueryContextMenu
//   · PackagedCommand → 用当前路径的 IShellItemArray 实例化 IExplorerCommand
// paths 为空表示「文件夹空白区」；parentFolder 是空白区所在目录。
bool CaptureOwnerMenu(MenuInvokeKind kind,
                      const std::wstring& clsid,
                      const std::vector<std::wstring>& paths,
                      const std::wstring& parentFolder,
                      unsigned int scene,
                      std::vector<MenuInventoryItem>& out,
                      std::wstring* error);

// [perf] 临时对照实验（定位完删除）：
//   ProbeClassicNativePidl：父文件夹 EnumObjects 枚举原生 child PIDL 喂经典扩展；
//   ProbeModernNativeArray：从标准 IDataObject 用 SHCreateShellItemArrayFromDataObject 造数组。
// 仅在性能探针里调用，结果写日志，不产出菜单项。
void ProbeClassicNativePidl(const std::wstring& clsid,
                            const std::vector<std::wstring>& paths,
                            const std::wstring& parentFolder);
void ProbeModernNativeArray(const std::wstring& clsid,
                            const std::vector<std::wstring>& paths,
                            const std::wstring& parentFolder);

// 缓存：%APPDATA%\FileExplorer\menu-inventory.json
std::wstring MenuInventoryPath();
bool LoadMenuInventory(MenuInventoryReport& report);
bool SaveMenuInventory(const MenuInventoryReport& report);

// 给界面/运行时用的简单查询
const MenuInventoryItem* FindInventoryItem(const MenuInventoryReport& report,
                                           const std::wstring& itemId);

// 点击执行：按 item.invoke 的原始描述回放，不重写第三方功能。
//   · StaticVerb   → 优先按 commandLine 启动；没有命令行则 ShellExecuteEx(verb)
//   · ShellHandler → 重新初始化原 CLSID，QueryContextMenu 后 InvokeCommand
//   · WindowsBuiltin → ShellExecuteEx 默认打开
// 返回 false 时 error 里是可读原因（用于日志，不弹给最终用户）。
bool InvokeInventoryItem(const MenuInventoryItem& item,
                         const std::wstring& targetPath,
                         const std::wstring& workingDir,
                         HWND owner,
                         std::wstring* error);

// 多选版本：同一个菜单项对多选路径执行。静态 verb 逐路径执行，
// shell handler / 现代命令把整批路径放进一个 IShellItemArray。
bool InvokeInventoryItem(const MenuInventoryItem& item,
                         const std::vector<std::wstring>& targetPaths,
                         const std::wstring& workingDir,
                         HWND owner,
                         std::wstring* error);

} // namespace shell
