// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContainerExtract.h
//
// 压缩包内部项的「解压到临时目录」模块。
//
// 【为什么单独一个模块】
//   这套逻辑原来是塞在 MainWnd.cpp 里的（约 980 行），但它跟窗口没有关系：
//   给一个包内路径，算出该落到 %TEMP%\Yexp\ 的哪个目录、在不在缓存里、
//   要不要真解、解的时候进度是多少。窗口只该负责"把进度文本画到状态栏"。
//   所以这里把执行侧全部收进来，窗口通过回调拿进度、通过返回值拿真实路径。
//
// 【入口只有两个】
//   双击包内文件打开 / 把包内项往外拖。两者共用 RunExtraction，
//   区别只是 showMouseTip（拖拽时在鼠标右下角多一个提示条）。
//
// 【缓存】
//   同一个包（路径、大小、修改时间都没变）永远落到同一个临时目录，
//   解出来的文件盖成"包内那一项的修改时间"，下次按"大小 + 时间"判断命中，
//   不重复解。目录名里带包指纹，包被替换后旧缓存自然失效。
// ============================================================================

#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace shell
{

// ----------------------------------------------------------------------------
// 一项"要解的项"：开解之前就把落点、中间路径、该有多大全部算好
// ----------------------------------------------------------------------------
struct ContainerExtractItem
{
    std::wstring       virtualPath;         // 容器内路径（C:\a.rar\sub\inner.txt）
    std::wstring       containerRoot;       // 压缩包本身（C:\a.rar）
    std::wstring       innerPath;           // 包内相对路径（sub\inner.txt）
    std::wstring       tempDirectory;       // 解到哪个临时目录
    std::wstring       outputPath;          // 解完的正式文件路径
    std::wstring       partialPath;         // zip 通道的中间文件（写完改名成正式文件）
    std::wstring       stagingPath;         // rar 通道的临时子目录
    std::wstring       landingPath;         // rar 落地后的那个文件（进度也盯它）
    unsigned long long expectedSize = 0;    // 包内声明的大小（0 = 包没给）
    FILETIME           expectedTime = {};   // 包内那一项的修改时间（缓存判据）

    // ---- 下面三项只对"文件夹项"有值 ----
    //
    // 整目录复制（CopyHere）中途拿不到"当前复制到哪个文件"，所以解压前先把这个
    // 目录在包里的文件清单数出来（文件名、大小、时间），复制过程中每秒对着清单
    // 问一次"这个文件出现了没有、长了多少"，就能算出真实进度。
    std::vector<std::wstring>       childRelativePaths;  // 相对该目录的子路径
    std::vector<unsigned long long> childSizes;          // 每个子文件在包内声明的大小
    std::vector<FILETIME>           childTimes;          // 每个子文件在包内的修改时间
};

// 顶层条目的元数据。由调用方（主窗口）从当前列表快照里给——
// 解压模块不持有列表，也不自己去枚举磁盘。
struct ContainerEntryMeta
{
    unsigned long long size     = 0;
    FILETIME           modified = {};
    bool               isFolder = false;
};

// 取某条路径的元数据；返回 false 表示取不到（这一条会被跳过并记日志）
using ContainerMetaProvider =
    std::function<bool(const std::wstring& path, ContainerEntryMeta& meta)>;

// %TEMP%\Yexp\ ...（不存在的目录由解压过程自己创建）
std::wstring TempDirectoryFor(const std::wstring& containerRoot);

// 启动时在后台线程跑一次：删过期的缓存目录和上次崩溃留下的半成品
void CleanupTempCache();

// 缓存命中判断：文件按"大小 + 修改时间"，文件夹按"目录时间 + 清单里每个文件"
bool CacheIsUsable(const std::wstring& outputPath,
                   unsigned long long declaredSize,
                   const FILETIME&   expectedTime,
                   const std::vector<std::wstring>& childRelativePaths,
                   const std::vector<unsigned long long>& childSizes);

// 解压成功后盖上"包内修改时间"（缓存判据靠它）
void StampFileTime(const std::wstring& path, const FILETIME& time);

// 把一批"包内路径"算成"要解的项"。
// 只做本地查询（拼路径、查调用方给的元数据、数包内清单），不碰 COM。
// 一项都没算出来时返回 false（原因已记日志）。
bool BuildItems(const std::vector<std::wstring>& paths,
                const ContainerMetaProvider&      meta,
                std::vector<ContainerExtractItem>& items,
                unsigned long long&                totalBytes);

// 解压执行器（双击和拖拽共用）
//
//   showMouseTip  true = 拖拽：显示鼠标右下提示条，并支持"松开左键取消"
//   onBegin/onEnd 只在真的要解的时候各调一次（全部命中缓存时一次都不调）
//   onProgress    主线程轮询时的高频回调，参数是状态栏要显示的整段文字
//   realPaths     解好的真实文件（按 items 顺序，失败/取消的项不在里面）
//   lastError     最后一次失败原因（双击时拿来弹窗）
//
// 返回 false 表示被取消（ESC / 松开左键）。
bool RunExtraction(const std::vector<ContainerExtractItem>& items,
                   bool                                   showMouseTip,
                   const std::function<void()>&                  onBegin,
                   const std::function<void(const std::wstring&)>& onProgress,
                   const std::function<void()>&                  onEnd,
                   std::vector<std::wstring>&             realPaths,
                   std::wstring&                          lastError);

} // namespace shell
