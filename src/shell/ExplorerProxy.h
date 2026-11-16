// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ExplorerProxy.h
//
// 「把 explorer.exe 薄代理部署到 System32」这一套动作。
//
// 背景：
//   微信、批量打印软件这类程序的「打开所在文件夹」是
//   Process.Start("explorer.exe", "/select,\"路径\"")，直接按名字启动
//   explorer.exe，绕过 shell 的文件夹默认动词，注册表管不到。
//   因此在 System32 放一个同名的薄代理，让这类调用先命中它。
//   （代理源码见 src/launcher/ExplorerLauncher.cpp）
//
// 这里的职责只有四件事：
//   1. 判断 System32\explorer.exe 当前是什么状态（没有 / 是我们的 / 是我们的但旧了 / 不是我们的）
//   2. 点「注册为默认文件管理器」成功后，把代理放进去（需要管理员，会弹一次 UAC）
//   3. 点「恢复系统默认文件管理器」后，把代理删掉（同样需要管理员）
//   4. 提供提权子进程里跑的正式动作（--deploy-proxy / --remove-proxy）
//
// 安全原则：
//   System32\explorer.exe 只有在“确认是我们自己放的”时才覆盖或删除；
//   一旦发现是别人的文件，只弹提醒、绝不动手。
//   认亲依据是代理 exe 的版本资源 ProductName = "Yexplorer Launcher" + OriginalFilename = "explorer.exe"。
// ============================================================================

#pragma once

#include <windows.h>

namespace shell
{

// System32\explorer.exe 的当前状态
enum class ProxyState
{
    Missing,        // 这个路径上没有文件（还没部署过）
    OursUpToDate,   // 有，是我们的，且和当前自带的代理内容一致（不用动）
    OursOutdated,   // 有，是我们的，但内容不同（旧版本，应该替换）
    Foreign,        // 有，但不是我们放的（只提醒，不覆盖）
};

// 查 System32\explorer.exe 的状态。纯读操作，不需要管理员。
ProxyState QuerySystemProxyState();

// 点「注册为默认文件管理器」之后调用：
//   没有 → 提权部署；是我们的但旧了 → 提权替换；已经是最新 → 什么都不做；不是我们的 → 弹窗提醒。
// 返回值：true 表示“现在 System32 里的代理就是最新的”（或者本来就不需要动）。
bool DeployProxyIfNeeded(HWND owner);

// 点「恢复系统默认文件管理器」之后调用：
//   如果是我们放的 → 提权删除；不是我们放的或不存在 → 什么都不做。
// 返回值：true 表示“System32 里已经没有我们放的代理了”。
bool RemoveProxyIfOurs(HWND owner);

// 提权子进程里执行的正式动作（由主程序用 runas 重新启动自身触发）。
// remove = false：部署/替换；remove = true：删除。
// 返回进程退出码：0 成功，1 因为不是我们的而放弃，2 失败（找不到源文件/复制删除出错）。
int RunProxyCommandLine(bool remove);

} // namespace shell
