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

#include <string>
#include <unordered_map>

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

    // 系统小图标图像列表句柄（可直接塞给 ListView_SetImageList）
    HIMAGELIST ImageList() const { return m_hImageList; }

    // 统一的文件夹图标索引（导航栏「快速访问」这类分组标题用）
    int FolderIconIndex() const { return m_folderIconIndex; }

    // ------------------------------------------------------------------
    // 取某个条目的图标索引
    //   name     —— 文件/文件夹名称（不是完整路径，只用来取扩展名）
    //   isFolder —— 是否文件夹
    // ------------------------------------------------------------------
    int GetIconIndex(const std::wstring& name, bool isFolder);

    // ------------------------------------------------------------------
    // 按「完整路径」取图标（左侧导航栏用）
    //
    // 和上面按扩展名的区别：这里会真的去问系统这个路径是什么图标，
    // 所以桌面、下载、各个硬盘都能拿到各自的正确图标。
    // 只用于导航栏那十几个固定项，一次性查询，不影响列表性能。
    // ------------------------------------------------------------------
    int GetIconIndexByPath(const std::wstring& path);

    // ------------------------------------------------------------------
    // 按「完整路径」取图标并缓存（exe / lnk 这类"每个文件有自己的图标"的条目用）
    //
    // 为什么需要它：按扩展名取图标用的是假文件名 + SHGFI_USEFILEATTRIBUTES，
    // 系统不会去读真实文件，所以所有 .exe 只能拿到同一个"通用程序"图标。
    // exe 的图标是打包在 exe 自己文件里的，必须拿真实路径去问系统。
    //
    // 代价：每个条目一次 shell 查询（系统有图标缓存，通常零点几毫秒），
    //       所以只在"可见行需要画图标"时才会调用，并且结果按路径缓存。
    // ------------------------------------------------------------------
    int GetIconIndexByFullPath(const std::wstring& path);

    // ------------------------------------------------------------------
    // 取「半透明版」图标索引（被剪切的项目要看起来淡一点，和资源管理器一致）
    //
    // 做法：把原图标按 50% 透明度重画一份，追加进同一个图像列表，返回新索引。
    // 同一个原索引只生成一次，之后走缓存。
    // 生成失败时返回原始索引（宁可不变淡，也不能显示不出来）
    // ------------------------------------------------------------------
    int GetDimmedIconIndex(int originalIndex);

private:
    // 从名称里提取扩展名（小写，含点号，如 ".dll"；没有扩展名返回空串）
    static std::wstring ExtractExtension(const std::wstring& name);

    // 把图标做成 50% 透明的版本；失败返回 nullptr
    HICON CreateDimmedIcon(HICON source, int size);

private:
    wil::com_ptr<IImageList> m_systemImageList;   // 系统图像列表（持有引用保证存活）
    HIMAGELIST               m_hImageList = nullptr;

    std::unordered_map<std::wstring, int> m_extensionIconCache;  // 扩展名 → 图标索引
    std::unordered_map<std::wstring, int> m_pathIconCache;       // 完整路径 → 图标索引
    std::unordered_map<int, int>          m_dimmedIconCache;     // 原索引 → 半透明索引

    int  m_folderIconIndex  = 0;   // 文件夹统一图标
    bool m_initialized      = false;
};

} // namespace shell
