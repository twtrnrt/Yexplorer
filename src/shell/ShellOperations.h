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

// 在指定目录里新建一个文件夹，自动避开重名
// 比如目录里已有「新建文件夹」，就建「新建文件夹 (2)」
// 成功时 newFolderPath 返回新建出来的完整路径
HRESULT CreateNewFolder(const std::wstring& directory, std::wstring& newFolderPath);

// 重命名一项（F2）
// oldPath：现在的完整路径；newName：新名字（只给名字，不带路径）
// 名字里含非法字符会直接返回失败，不弹系统框
HRESULT RenameItem(const std::wstring& oldPath, const std::wstring& newName, HWND owner);

} // namespace shellops
