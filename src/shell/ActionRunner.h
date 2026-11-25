// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ActionRunner.h
//
// 用户自定义「快捷动作」的展开与执行。
//
// 设计原则（和旧方案彻底分开）：
//   · 只执行用户显式配置的 program + arguments，不解析整条命令行；
//   · 占位符只认三个，路径自动加引号，用户不用懂 Windows 引号规则；
//   · 不采集系统右键菜单、不调第三方 Shell 扩展、不写注册表。
//
// 占位符：
//   %1     第一个选中项；空白区 = 当前目录        （自动加引号）
//   %sel%  全部选中项；空白区 = 当前目录          （每个自动加引号）
//   %dir%  当前文件夹                              （自动加引号）
// 占位符外面再包一层双引号也兼容（"%1"），不会变成 ""路径""。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ui/MenuLayout.h"

namespace shell
{

// 一次右键的上下文
struct ActionContext
{
    std::vector<std::wstring> paths;   // 选中项；空白区为空
    std::wstring              currentDir;
};

// 纯函数：按占位符展开参数模板。
// 返回 1 组（含 %sel% 或多选时只启动一次）或多组（没有 %sel% 时每个路径一组）。
std::vector<std::wstring> ExpandActionArguments(const ui::CustomAction& action,
                                                const ActionContext& context);

// 纯函数：给一个参数加 Windows 命令行引号（CommandLineToArgvW 兼容）。
std::wstring QuoteArgument(const std::wstring& argument);

// 纯函数：场景 + 扩展名是否匹配。
bool ActionMatchesPath(const ui::CustomAction& action,
                       ui::MenuScene scene,
                       const std::wstring& primaryPath,
                       const std::vector<std::wstring>& paths);

// 执行。失败时 error 是可读原因；成功返回 true。
// owner 用于 ShellExecuteEx / 错误提示的父窗口。
bool RunCustomAction(const ui::CustomAction& action,
                     const ActionContext& context,
                     HWND owner,
                     std::wstring* error);

} // namespace shell
