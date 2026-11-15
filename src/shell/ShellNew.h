// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellNew.h
//
// 「新建」二级菜单：跟系统资源管理器一样，列出这台机器上能"新建"的各类文件。
//
// 【数据来源 —— 和系统一模一样】
//   资源管理器不是自己枚举注册表的，它读一份算好的清单：
//     HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\
//          Discardable\PostSetup\ShellNew  →  Classes（REG_MULTI_SZ）
//   这个多字符串就是菜单项的顺序表，每行要么是扩展名（".txt"），
//   要么是 ProgID（"Folder"、"CompressedFolder"）。
//   对每一行再去 HKCR\<行>\ShellNew 里读模板怎么建：
//     NullFile  = 建空文件
//     FileName  = 复制模板文件（如 WinRAR 的 zipnew.dat）
//     Command   = 交给程序自己建（WPS 那两个"新建 PDF / AI 演示"就是这样）
//     Directory = 文件夹
//   ShellNew 里还有两个"外观"值：
//     MenuText = 菜单上显示的名字（可以是 "@shell32.dll,-30318" 这种资源引用）
//     IconPath = 菜单图标（"C:\...\shell32.dll,-16769"）
//
// 【什么时候读 —— 零成本原则】
//   绝不能在右键菜单弹出时读注册表。做法是：程序启动后在后台线程读一次，
//   结果放内存里，菜单只读这份快照。新装软件要看到新的"新建"项，重启程序即可。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace shell
{

// 新建项的类型
enum class ShellNewKind
{
    Folder,     // 新建文件夹
    NullFile,   // 建空文件（NullFile）
    FileName,   // 复制模板文件（FileName）
    Command,    // 交给外部程序自己建（Command，%1 = 目标目录）
};

struct ShellNewItem
{
    ShellNewKind kind = ShellNewKind::NullFile;

    // 注册表里的键名：扩展名（".zip"）或 ProgID（"Folder"）。
    // 同一份清单里可能同时出现两者，菜单图标的缓存键也用它。
    std::wstring menuKey;

    std::wstring extension;     // 扩展名（".zip"）；Folder / ProgID 可能为空
    std::wstring displayName;   // 菜单上显示的名字（"压缩(zipped)文件夹"）
    std::wstring iconPath;      // IconPath 原值（"C:\...\shell32.dll,-16769"）
    std::wstring templatePath;  // kind=FileName 时的模板文件全路径
    std::wstring command;       // kind=Command 时的命令行（可能含 %1）
    std::wstring handlerClsid;  // ShellNew\Handler 的 CLSID（.lnk 的向导就靠它）
};

// ---------------------------------------------------------------------------
// 读一次系统里支持的所有「新建」项（后台线程里调）
//
// 顺序和资源管理器的菜单一致（按 Discardable\Classes 那份清单）；
// 清单里没有、但注册表里确实登记了 ShellNew 的项，会补在最后，免得漏。
// ---------------------------------------------------------------------------
std::vector<ShellNewItem> EnumerateShellNew();

// ---------------------------------------------------------------------------
// 在某目录里创建一个新建项
//
//   directory   —— 目标目录（当前浏览的文件夹）
//   createdPath —— 成功后新项的完整路径
//                  （kind=Command / 快捷方式向导这类交给外部程序的，返回空串）
//   error       —— 失败原因（给用户看的一句话）
// ---------------------------------------------------------------------------
bool CreateShellNewItem(const ShellNewItem& item,
                        const std::wstring& directory,
                        std::wstring& createdPath,
                        std::wstring& error);

// 新建文件时用的基础名（"新建 文本文档"），也用来判断重名
std::wstring NewItemBaseName(const ShellNewItem& item);

} // namespace shell
