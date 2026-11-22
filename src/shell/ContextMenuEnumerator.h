// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuEnumerator.h
//
// 注册表侧的右键菜单清单枚举（纯注册表读取，不碰 COM、不实例化 DLL）。
//
// 两类来源：
//   1. 静态动词：HKCR\<场景>\shell\<动词>\command 里写死命令行的项，
//      菜单文字 / 命令行 / 图标索引都能直接从注册表读出来；
//   2. shellex 处理器：HKCR\<场景>\shellex\ContextMenuHandlers\<名> 下登记
//      的 CLSID。这类处理器的菜单文字藏在 DLL 里，注册表只能查到 CLSID、
//      DLL 路径、归属软件名；具体菜单项交给 ContextMenuCollector 运行时采集。
//
// 一期只采集、不执行：本模块不写注册表、不调用任何菜单命令。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "shell/ContextMenuData.h"

namespace shell
{

// 注册表扫描出的原始 shellex 处理器（Collector 后续按 CLSID 实例化采集）
struct EnumeratedHandler
{
    CLSID        clsid = {};      // 处理器 CLSID
    std::wstring clsidStr;         // CLSID 字符串（带花括号，日志 / 归并用）
    std::wstring regName;          // 注册表子键名
    std::wstring dllPath;          // InprocServer32 DLL 路径
    std::wstring displayName;      // 综合多来源拼出的软件显示名
    unsigned int scenes = SceneNone; // 注册时声明出现的场景位掩码
};

// ---------------------------------------------------------------------------
// 扫注册表，产出一份初始报告：
//   · 静态动词直接作为菜单项放进对应软件分组；
//   · shellex 处理器先建分组占位，组内菜单项由 Collector 补全。
//
// 只在打开「配置右键菜单」窗口时调用，不进右键弹出链路。
// ---------------------------------------------------------------------------
ContextMenuReport EnumerateRegistry();

// 单独取 shellex 处理器清单（Collector 采集时用）。
std::vector<EnumeratedHandler> EnumerateShellexHandlers();

} // namespace shell
