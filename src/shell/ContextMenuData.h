// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuData.h
//
// 右键菜单采集结果的共享数据模型。
//
// 一期只做"拿数据 + 展示"：
//   · 注册表静态动词（shell 下写死命令行的项）由 Enumerator 直接读出；
//   · DLL 外壳扩展处理器（shellex）的具体菜单项由 Collector 运行时调
//     IContextMenu 读出；
//   · 本头文件只放结构和场景标签辅助函数，不依赖 UI。
//
// 二期接入真实右键菜单时，直接复用这里的 id / verb / commandOffset /
// commandLine 等字段做执行和过滤。
// ============================================================================

#pragma once

#include <string>
#include <vector>

namespace shell
{

// ---------------------------------------------------------------------------
// 菜单项出现的场景（位掩码，可叠加）
//
//   File       单个文件（注册表 *）
//   Folder     文件夹（Directory / Folder）
//   Multiple   多选
//   Background 目录背景空白处（Directory\Background）
// ---------------------------------------------------------------------------
enum MenuScene : unsigned int
{
    SceneNone       = 0,
    SceneFile       = 1 << 0,
    SceneFolder     = 1 << 1,
    SceneMultiple   = 1 << 2,
    SceneBackground = 1 << 3,
};

// 一条采集到的右键菜单项
struct ContextMenuItem
{
    std::wstring id;            // 稳定标识：静态=动词名；扩展=CLSID:命令偏移
    std::wstring text;          // 真实菜单文字（资源引用已解析成可读文字）
    std::wstring verb;          // 动词名（GetCommandString GCS_VERBW），二期执行用
    std::wstring parentPath;    // 子菜单层级路径，如 "7-Zip"；一级项为空
    std::wstring commandLine;    // 静态动词命令行模板；扩展项为空
    std::wstring dllPath;        // 扩展处理器 DLL 路径；静态项为空
    std::wstring clsid;          // 扩展处理器 CLSID 字符串；静态项为空
    unsigned int scenes = SceneNone; // MenuScene 位掩码
    int  commandOffset = -1;     // 扩展项在处理器菜单里的命令偏移（二期 InvokeCommand 用）
    bool isSeparator = false;    // 是否分隔线（一期已不再采集，字段保留兼容）
    bool disabled    = false;    // 是否灰态
    bool failed      = false;    // 该项 / 所属处理器读取失败
    bool fromRegistry = false;   // 来源：注册表静态动词
    bool fromExtension = false;  // 来源：DLL 外壳扩展（IContextMenu 采集）
    bool isSystemBuiltin = false; // 判定为系统自带项（默认在界面上隐藏）
    std::wstring failReason;     // 失败原因（failed=true 时填写）
};

// 一个归属分组（按软件归并）
struct ContextMenuGroup
{
    std::wstring name;          // 分组显示名（软件名）
    std::wstring sourceKey;     // 归并键（DLL / exe 路径小写）
    bool         isShellex = false; // true=DLL 扩展；false=注册表静态动词
    std::vector<ContextMenuItem> items;
};

// 一次采集的完整结果
struct ContextMenuReport
{
    std::vector<ContextMenuGroup> groups;
    std::wstring collectedAt;   // 采集时间（本地时间字符串，缓存展示用）
};

// 场景位掩码 → 界面文字，如 "文件 / 文件夹"；空掩码返回 "—"
std::wstring ScenesToText(unsigned int scenes);

// 来源标注文字：注册表 / 扩展 / 注册表+扩展；都为空时返回空串
std::wstring SourceLabelToText(bool fromRegistry, bool fromExtension);

// 判定是不是系统自带项（按常见系统动词名 / 菜单文字白名单）
// 只用于界面默认隐藏，判定不准不会丢数据，勾选「显示系统项」就能看到
bool IsSystemBuiltinItem(const std::wstring& text, const std::wstring& verb);

// 判定是不是系统自带分组（如「新建」「发送到」）
bool IsSystemBuiltinGroup(const std::wstring& groupName);

} // namespace shell
