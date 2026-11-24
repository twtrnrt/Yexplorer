// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellIcon.h
//
// 系统图标缓存。
//
// 【为什么这样做才快】
//   资源管理器显示图标并不慢，慢的是缩略图。图标走的是系统图标缓存。
//   这里做两件事保证速度：
//     1. 直接用系统图像列表（SHGetImageList），不自己构造图标
//     2. 文件图标按「扩展名」缓存，同一个扩展名只查一次；
//        查询时用 SHGFI_USEFILEATTRIBUTES，不实际访问文件，
//        只按扩展名从系统图标表里取，耗时在微秒级
//
//   结果：一万个文件也不会因为取图标而卡。代价是「同一扩展名的文件共用一个图标」，
//   和资源管理器默认行为一致（资源管理器也是按扩展名给图标）。
// ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>       // HIMAGELIST 定义
#include <shellapi.h>       // SHGetFileInfoW / SHFILEINFOW / SHGFI_* 标志
#include <commoncontrols.h> // IImageList / SHGetImageList
#include <wil/com.h>

#include <memory>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "IconDiskCache.h"  // 磁盘成品缓存（exe/lnk 这类图标的落盘层）

namespace shell
{

// 图标缓存
class IconCache
{
public:
    IconCache();
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    // ------------------------------------------------------------------
    // 初始化：拿系统小图标图像列表
    // 返回 false 表示初始化失败（此时界面仍可用，只是没有图标）
    // ------------------------------------------------------------------
    bool Initialize();

    // ------------------------------------------------------------------
    // 图标尺寸
    //
    //   Small  —— 16px，详细信息/列表/小图标视图、左栏导航都用这个
    //   Medium —— 32px，「打开方式」设置窗口、添加程序窗口的列表用（图标清楚又不占地方）
    //   Large  —— 48px，只有「大图标」视图用
    //
    // 三个尺寸都是系统现成的图像列表（SHIL_SMALL / SHIL_LARGE / SHIL_EXTRALARGE），
    // 我们只是引用它们、并不自己画图标，所以三条列表的内存都不额外占。
    //
    // 【索引为什么能通用】SHGetFileInfo 带 SHGFI_LARGEICON 拿回来的索引，
    // 在 SHIL_LARGE(32) 和 SHIL_EXTRALARGE(48) 两套列表里指的是同一个图标，
    // 所以 Medium 和 Large 共用同一份查询结果与缓存，不用各查一遍。
    // ------------------------------------------------------------------
    enum class IconSize
    {
        Small,
        Medium,
        Large
    };

    // 系统图标图像列表句柄（可直接塞给 ListView_SetImageList）
    HIMAGELIST ImageList(IconSize size = IconSize::Small) const
    {
        if (size == IconSize::Large)
        {
            return m_hLargeImageList;
        }
        return (size == IconSize::Medium) ? m_hMediumImageList : m_hImageList;
    }

    // 统一的文件夹图标索引（导航栏「快速访问」这类分组标题用）
    int FolderIconIndex(IconSize size = IconSize::Small) const
    {
        if (size == IconSize::Large)
        {
            return m_largeFolderIconIndex;
        }
        return (size == IconSize::Medium) ? m_largeFolderIconIndex : m_folderIconIndex;
    }

    // ------------------------------------------------------------------
    // 取某个条目的图标索引
    //   name     —— 文件/文件夹名称（不是完整路径，只用来取扩展名）
    //   isFolder —— 是否文件夹
    //   size     —— 要小图标还是大图标
    // ------------------------------------------------------------------
    int GetIconIndex(const std::wstring& name, bool isFolder,
                     IconSize size = IconSize::Small);

    // ------------------------------------------------------------------
    // 按「完整路径」取图标（左侧导航栏用）
    //
    // 和上面按扩展名的区别：这里会真的去问系统这个路径是什么图标，
    // 所以桌面、下载、各个硬盘都能拿到各自的正确图标。
    // 只用于导航栏那十几个固定项，一次性查询，不影响列表性能。
    // ------------------------------------------------------------------
    int GetIconIndexByPath(const std::wstring& path, IconSize size = IconSize::Small);

    // ------------------------------------------------------------------
    // 按「完整路径」取图标并缓存（exe / lnk 这类"每个文件有自己的图标"的条目用）
    //
    // 为什么需要它：按扩展名取图标用的是假文件名 + SHGFI_USEFILEATTRIBUTES，
    // 系统不会去读真实文件，所以所有 .exe 只能拿到同一个"通用程序"图标。
    // exe 的图标是打包在 exe 自己文件里的，必须拿真实路径去问系统。
    //
    // 代价：每个条目一次 shell 查询（系统有图标缓存，通常零点几毫秒），
    //       所以只在"可见行需要画图标"时才会调用，并且结果按路径缓存。
    //
    // fileSize / modified —— 文件大小和修改时间（列表枚举时已经有，直接传进来，
    //                       省掉一次属性查询）。带上这两个值才会启用磁盘缓存：
    //                       文件被替换（大小或时间变了）时 key 自动变，图标跟着更新。
    // ------------------------------------------------------------------
    int GetIconIndexByFullPath(const std::wstring& path, IconSize size = IconSize::Small,
                               unsigned long long fileSize = 0,
                               FILETIME modified = {});

    // ------------------------------------------------------------------
    // 按「绝对 PIDL」取图标
    //
    // 为什么需要它：控制面板里那些项（「字体」「恢复」「程序和功能」…）的
    // 解析名是 `::{26EE0668-…}\0\显示名` 这种形式，Shell 根本解析不回 PIDL
    // （实测 SHParseDisplayName 一律失败），所以按字符串取图标必然拿到 0，
    // 结果就是这些项全都显示成空白。枚举时我们手里已经有每项的绝对 PIDL，
    // 直接拿它问系统就准了。
    // ------------------------------------------------------------------
    int GetIconIndexByPidl(const std::vector<unsigned char>& pidl, IconSize size = IconSize::Small);

    // ------------------------------------------------------------------
    // 取「半透明版」图标索引（被剪切的项目要看起来淡一点，和资源管理器一致）
    //
    // 做法：把原图标按 50% 透明度重画一份，追加进同一个图像列表，返回新索引。
    // 同一个原索引只生成一次，之后走缓存。
    // 生成失败时返回原始索引（宁可不变淡，也不能显示不出来）
    // ------------------------------------------------------------------
    int GetDimmedIconIndex(int originalIndex, IconSize size = IconSize::Small);

    // ------------------------------------------------------------------
    // 取「主图标 + overlay 小角标」合成后的图标索引。
    //
    // overlay 角标贴在主图标左下角，边长约为主图标 1/4（对齐原生经验值）。
    // overlay 传 nullptr 时原样返回 baseIndex。合成结果按「主索引 + overlay 图标」
    // 组合缓存，不重复合成；失败返回原始索引。
    // ------------------------------------------------------------------
    int GetCompositeIconIndex(int baseIndex, HICON overlay, IconSize size = IconSize::Small);

private:
    // ---- 磁盘缓存（只服务 exe/lnk 这类"必须读文件"的图标）----

    // 统一小写：缓存 key 里的路径必须大小写无关（同一个文件不应存两份）
    static std::wstring Lowercase(std::wstring text);

    // 组装缓存 key：路径 + 文件大小 + 修改时间 + 边长 + 版本
    std::wstring BuildDiskKey(const std::wstring& path, IconSize size,
                              unsigned long long fileSize, const FILETIME& modified) const;

    // 该尺寸的图标往哪张图像列表里追加
    HIMAGELIST TargetImageList(IconSize size) const;

    // 从磁盘读一条并塞进图像列表：
    //   返回 -1 表示没命中（调用方去问系统），-2 表示负缓存（确定取不到），>=0 是索引
    int LoadIconFromDisk(const std::wstring& key, IconSize size);

    // 把刚挖出来的图标转成像素存盘（失败只影响下次还得重取）
    void SaveIconToDisk(const std::wstring& key, HICON icon);

    // 从名称里提取扩展名（小写，含点号，如 ".dll"；没有扩展名返回空串）
    static std::wstring ExtractExtension(const std::wstring& name);

    // 把图标做成 50% 透明的版本；失败返回 nullptr
    HICON CreateDimmedIcon(HICON source, int size);

    // 把主图标与 overlay 小角标合成一张图；失败返回 nullptr
    HICON CreateCompositeIcon(HICON base, HICON overlay, int size);

private:
    wil::com_ptr<IImageList> m_systemImageList;       // 系统小图标图像列表（16px）
    wil::com_ptr<IImageList> m_mediumSystemImageList; // 系统中图标图像列表（32px）
    wil::com_ptr<IImageList> m_largeSystemImageList;  // 系统大图标图像列表（48px）
    HIMAGELIST               m_hImageList        = nullptr;
    HIMAGELIST               m_hMediumImageList  = nullptr;
    HIMAGELIST               m_hLargeImageList   = nullptr;

    // 缓存分两套：小图标一套、大图标一套（两种尺寸的索引彼此不通用）
    std::unordered_map<std::wstring, int> m_extensionIconCache;       // 扩展名 → 小图标索引
    std::unordered_map<std::wstring, int> m_largeExtensionIconCache;  // 扩展名 → 大图标索引
    std::unordered_map<std::wstring, int> m_pathIconCache;            // 完整路径 → 小图标索引
    std::unordered_map<std::wstring, int> m_mediumPathIconCache;      // 完整路径 → 中图标索引（和大小图标分开）
    std::unordered_map<std::wstring, int> m_largePathIconCache;       // 完整路径 → 大图标索引

    // 磁盘缓存：key（路径+大小+时间+边长）→ 图像列表索引
    // 这个索引是本进程追加进图像列表时拿到的，重启后失效，所以内存表只活在本次运行
    std::map<std::wstring, int>                m_diskIndexCache;
    std::unordered_set<std::wstring>           m_diskNegativeKeys;    // 确认取不到的 key，不再重试
    std::unique_ptr<IconDiskCache>             m_diskCache;
    std::unordered_map<int, int>          m_dimmedIconCache;          // 小图标原索引 → 半透明索引
    std::unordered_map<int, int>          m_largeDimmedIconCache;     // 大图标原索引 → 半透明索引
    std::map<std::pair<int, const void*>, int> m_overlayCompositeCache;      // 小：(主索引, overlay) → 合成索引
    std::map<std::pair<int, const void*>, int> m_largeOverlayCompositeCache; // 大：同上

    int  m_folderIconIndex      = 0;   // 文件夹统一小图标
    int  m_largeFolderIconIndex = 0;   // 文件夹统一大图标
    bool m_initialized          = false;
};

} // namespace shell
