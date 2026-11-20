// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ListaryIntegration.h
//
// 把内嵌的 Listary 文件管理器插件安装到 Listary 的插件目录。
//
// 为什么需要管理员：Listary 只扫描自己的安装目录
// （<Listary>\FileAppPlugins\），实测用户目录里的插件不会被加载。
// 所以安装必须写 Program Files，借用"注册为默认文件管理器"那一次 UAC。
//
// 用户视角：勾选注册后，Listary 的快速切换就能用；不需要单独安装包。
// ============================================================================

#pragma once

#include <string>

namespace shell
{

// 是否检测到本机安装了 Listary
bool IsListaryInstalled();

// 安装 / 更新内嵌的 Listary 插件。
//   restartListary = true：装完把正在运行的 Listary 重启，让插件立刻生效。
// 返回：0 = 不需要（没装 Listary / 已经是最新）；1 = 已安装或更新；-1 = 失败。
int InstallListaryPlugin(bool restartListary);

} // namespace shell
