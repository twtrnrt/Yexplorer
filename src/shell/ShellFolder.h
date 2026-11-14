// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellFolder.h
//
// Shell 层：目录枚举接口。
//
// 关键设计决定（很重要，接手时先看懂这条）：
//   1. 用 IShellFolder 而不是 FindFirstFile。
//      FindFirstFile 只能读真实文件系统目录；IShellFolder 是资源管理器用的同一套
//      命名空间接口，能枚举「此电脑」「回收站」「网络」这些虚拟文件夹，
//      也能枚举压缩包内部（如果系统装了压缩外壳）。
//
//   2. 返回的是「纯数据快照」（ShellEntry），不返回任何 COM 指针。
//      原因：COM 对象有套间（apartment）归属，跨线程传递很容易崩。
//      后台线程内部自己创建、自己释放 COM 对象，只把数据交出去，最安全。
//
//   3. 单个文件夹枚举不设条数上限，但支持取消：用户快速切目录时，
//      上一个枚举立即中断，不会堆积后台任务。
// ============================================================================

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#include "../core/AsyncTask.h"

namespace shell
{

// ----------------------------------------------------------------------------
// 一个 Shell 条目的数据快照（无 COM 指针，可自由跨线程复制传递）
// ----------------------------------------------------------------------------
struct ShellEntry
{
    std::wstring name;              // 显示名（文件名或虚拟项名称）
    std::wstring fullPath;          // 绝对路径；虚拟项（如「此电脑」）可能为空
    bool         isFolder = false;  // 是否文件夹
    uint64_t     size     = 0;      // 文件大小（字节）；文件夹固定为 0
    FILETIME     modified = {};     // 修改时间（UTC）；取不到时全为 0
};

// ----------------------------------------------------------------------------
// 一次枚举的结果
// ----------------------------------------------------------------------------
struct EnumerateResult
{
    std::wstring            path;          // 被枚举的路径
    long                    hr = E_FAIL;   // 枚举是否成功（S_OK 表示成功）
    std::vector<ShellEntry> entries;       // 条目列表
    size_t                  folderCount = 0;
    size_t                  fileCount   = 0;
    bool                    cancelled   = false;  // 是否因取消而提前中断
};

// ----------------------------------------------------------------------------
// 枚举指定路径下的所有条目
//
// 参数：
//   path  —— 目录绝对路径，例如 L"C:\\"、L"C:\\Windows"
//   token —— 取消令牌，可为 nullptr（表示不可取消）
//
// 返回：EnumerateResult（含 hr 状态码）
//
// 线程说明：可在任意线程调用。函数内部会自行 CoInitializeEx 并 CoUninitialize，
//           不依赖调用线程是否已初始化 COM。
// ----------------------------------------------------------------------------
EnumerateResult EnumerateFolder(const std::wstring& path,
                                const core::CancellationToken* token);

} // namespace shell
