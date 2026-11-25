// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MenuSpec.h
//
// 一条「待渲染的右键菜单项」描述。
//
// 构建阶段只产出描述，不直接往 HMENU 里塞：
//   · 先由各场景的 Build*MenuSpecs() 报出「这次有哪些项可用」；
//   · 再由 MenuLayout 按用户配置决定顺序、显隐；
//   · 最后由 RenderMenu() 统一渲染。
//
// 这样拆开的好处：
//   · 默认顺序和改造前一模一样，配置只是过滤器；
//   · 子菜单内容延迟到真正要显示时才填（被隐藏的项不建子菜单）；
//   · 单元测试可以直接验证「配置 → 顺序」这个纯函数。
// ============================================================================

#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace ui
{

struct MenuSpec
{
    std::wstring id;          // 稳定 ID（见 MenuLayout 的目录表）
    std::wstring text;        // 菜单文字（带 & 助记符）
    UINT         command = 0; // 命令号；子菜单为 0
    UINT         flags = MF_STRING;

    // 当前场景下这一项是否真的可用。
    // false 的项不参与渲染；配置里即使有也当没有。
    bool available = true;

    // 图标：只有配了图标的项才走自绘（目前是用户自定义动作）。
    // 为空的项一律用系统原生绘制，保持现有外观和零开销。
    HICON icon = nullptr;

    // 子菜单填充函数。为空 = 普通菜单项。
    // 只有这一项真的会被显示时才调用，避免隐藏项白建子菜单。
    std::function<void(HMENU)> fillSubmenu;

    bool IsSubmenu() const { return fillSubmenu != nullptr; }
};

} // namespace ui
