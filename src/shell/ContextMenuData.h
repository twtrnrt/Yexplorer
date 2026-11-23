// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuData.h
//
// 右键菜单采集结果的数据模型（第三版：按场景照原样快照）。
//
// 设计原则：**不改写系统菜单**。每个场景采一份菜单快照，条目保留
//   · 系统里的原始顺序；
//   · 层级（一级 / 二级 / 三级…）；
//   · 分隔线位置；
//   · 灰态；
//   · 每条自己渲染出来的位图（图标 + 文字都由原扩展画，最忠实）。
//
// 二期接执行/过滤时，用 verb / commandOffset / commandLine 就能回放。
// ============================================================================

#pragma once

#include <string>
#include <vector>

namespace shell
{

// 菜单项出现的场景（位掩码）
enum MenuScene : unsigned int
{
    SceneNone       = 0,
    SceneFile       = 1 << 0,   // 文件
    SceneFolder     = 1 << 1,   // 文件夹
    SceneMultiple   = 1 << 2,   // 多选文件
    SceneBackground = 1 << 3,   // 文件夹空白区
};

// 一条菜单项（顺序、层级都按系统原样保留）
struct ContextMenuItem
{
    int          level = 0;      // 层级：0=一级，1=二级，2=三级…
    std::wstring text;           // 菜单文字（已去掉助记符 &）
    std::wstring verb;           // 动词名（GCS_VERBW），二期执行用
    std::wstring commandLine;    // 静态动词命令行；扩展项为空
    std::wstring dllPath;        // 扩展处理器 DLL；静态项为空
    std::wstring clsid;          // 扩展处理器 CLSID；静态项为空
    unsigned int scenes = SceneNone; // 场景位
    int  commandOffset = -1;     // 命令偏移（ID - idFirst），二期 InvokeCommand 用
    bool isSeparator  = false;   // 分隔线（展示成一条浅色横线）
    bool disabled     = false;   // 灰态
    bool failed       = false;   // 该项渲染 / 读取失败
    bool fromRegistry = false;   // 来源：注册表静态动词
    bool fromExtension = false;  // 来源：DLL 外壳扩展
    std::wstring imageFile;      // 渲染图文件名（menu-icons 目录内，空=没渲染出来）
    std::wstring failReason;     // 失败原因
};

// 一个场景（一个样本）的菜单快照
struct SceneMenu
{
    unsigned int scene = SceneNone;   // 场景位
    std::wstring sampleLabel;         // 样本说明，如 "sample.txt"、"文件夹"、"空白区"、"两个 txt 多选"
    std::wstring samplePath;          // 样本路径（文件夹 / 空白区用）
    std::vector<ContextMenuItem> items; // 扁平列表，level 表示层级
    bool ok = false;                  // 这个场景是否采集成功
    std::wstring failReason;          // 失败原因
};

// 一次采集的完整结果
struct ContextMenuReport
{
    std::vector<SceneMenu> scenes;   // 一个样本一份
    std::wstring collectedAt;        // 采集时间
};

// 场景位 → 界面文字
std::wstring ScenesToText(unsigned int scenes);

} // namespace shell
