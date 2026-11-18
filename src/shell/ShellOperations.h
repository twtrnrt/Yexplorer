// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellOperations.h
//
// 文件操作（复制 / 剪切 / 删除 / 新建）。
//
// 【为什么用 IFileOperation 而不是自己 CopyFile / DeleteFile】
//   自己调 API 的话，这些事都得自己实现：
//     · 进度条窗口
//     · 重名冲突时"覆盖 / 跳过 / 改名"的询问框
//     · 目标空间不足的提示
//     · 长路径、只读文件、权限不足的处理
//     · 删除时进回收站、以及"撤销"
//     · 复制大文件时不要卡死界面
//   而 IFileOperation 是 Windows 给资源管理器自己用的同一套接口，
//   所有这些都由系统完成，界面和资源管理器一模一样。
// ============================================================================

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace shellops
{

// 把若干项复制到目标目录
// owner：主窗口句柄，系统的进度框/确认框会挂在它上面
// 返回 S_OK 表示成功；用户在冲突框里点了"取消"会返回一个失败码
HRESULT CopyItems(const std::vector<std::wstring>& sourcePaths,
                  const std::wstring& targetDirectory,
                  HWND owner);

// 把若干项移动到目标目录（剪切粘贴用这个）
HRESULT MoveItems(const std::vector<std::wstring>& sourcePaths,
                  const std::wstring& targetDirectory,
                  HWND owner);

// 删除若干项（进回收站，可撤销）
HRESULT DeleteItems(const std::vector<std::wstring>& paths, HWND owner);

// 永久删除若干项（不进回收站，没法撤销）。
//
// 和 DeleteItems 只差标志位：不带 FOF_ALLOWUNDO（不进回收站），
// 也不带 FOF_NOCONFIRMATION（让 Shell 自己弹"确实要永久删除这些项吗？"）。
// 确认框就是系统那个，和资源管理器 Shift+Delete 的体验一致。
HRESULT DeleteItemsPermanently(const std::vector<std::wstring>& paths, HWND owner);

// ----------------------------------------------------------------------------
// 按「绝对 PIDL」删除（压缩包容器内部的项专用）
//
// 为什么必须单独做一条通道：容器里的项（C:\a.zip\inner.txt）磁盘上不存在，
// 用路径字符串交给 Shell 是解析不出来的（实测：删除会失败，而且有过把
// 整个压缩包误删的先例）。只有 PIDL 才能定位到容器内部的那一项。
//
// pidlBytes：ShellEntry::pidl（枚举时已经拷好的绝对 PIDL 字节）
// ----------------------------------------------------------------------------
HRESULT DeleteShellItemPidls(const std::vector<std::vector<unsigned char>>& pidlBytes,
                             HWND owner);

// 在指定目录里新建一个文件夹，自动避开重名
// 比如目录里已有「新建文件夹」，就建「新建文件夹 (2)」
// 成功时 newFolderPath 返回新建出来的完整路径
HRESULT CreateNewFolder(const std::wstring& directory, std::wstring& newFolderPath);

// ----------------------------------------------------------------------------
// 把压缩包容器内部的一项「提取」到普通目录（也就是解出来一个真实文件）
//
// 用途：容器里双击文件要先解到临时目录再打开（和资源管理器一个做法），
// 以后"容器内复制到外部"也走这一条。
//
// 为什么不能用 IFileOperation：容器里的项（C:\a.rar\inner.txt）磁盘上不存在，
// SHCreateItemFromParsingName 对 rar/7z/tar 直接返回 0x80070002，
// 只有 SHParseDisplayName 能拿到 PIDL；拿到 PIDL 后用
// IShellFolder::BindToObject(IID_IStream) 把内容读出来，自己写文件。
//
// virtualPath    ：容器内部项的虚拟路径（C:\a.rar\sub\inner.txt）
// containerRoot  ：它所属的压缩包完整路径（C:\a.rar）——走 Shell 自动化通道要用
// relativePath   ：这一项在包内的相对路径（sub\inner.txt），决定落到目标目录的哪个子目录
// targetDirectory：解到哪个普通目录（不存在会自动建）
// error          ：失败时的原因（中文，可直接给用户看）
// 返回：解出来的真实文件完整路径；失败返回空串
// ----------------------------------------------------------------------------
std::wstring ExtractContainerEntry(const std::wstring& virtualPath,
                                   const std::wstring& containerRoot,
                                   const std::wstring& relativePath,
                                   const std::wstring& targetDirectory,
                                   std::wstring& error);

// 重命名一项（F2）
// oldPath：现在的完整路径；newName：新名字（只给名字，不带路径）
// 名字里含非法字符会直接返回失败，不弹系统框
HRESULT RenameItem(const std::wstring& oldPath, const std::wstring& newName, HWND owner);

// 给某个文件/文件夹建一个快捷方式（拖动时按住 Alt 用这个）
// targetPath：快捷方式指向的目标；linkPath：快捷方式文件本身的完整路径（以 .lnk 结尾）
HRESULT CreateShortcut(const std::wstring& targetPath, const std::wstring& linkPath);

} // namespace shellops
