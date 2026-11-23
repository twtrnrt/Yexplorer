// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuCensus.h
//
// 右键菜单"普查"：把本机能找到的扩展名逐个真实采集一遍，得到
//   · 每种扩展名的菜单项（文字模板 / 层级 / 灰态 / 动词 / 图标）
//   · 按「软件 → 场景 → 簇（菜单完全相同的扩展名打包）」的树状数据
//
// 为什么要普查：
//   配置界面左侧要按"软件 → 场景 → 簇"展示，而簇只能在比较过全部扩展名的
//   菜单之后才算得出来。只采 4 个样本是不够的（896 个扩展名里大部分没采过）。
//
// 为什么放子进程：
//   第三方 shellex 质量参差，个别会把宿主进程搞崩。普查在独立进程里跑，
//   崩了只丢那一批，主程序不受影响。
// ============================================================================

#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace shell
{

// 一条菜单项（普查用，比展示用的 ContextMenuItem 更精简）
struct CensusItem
{
    int          level = 0;         // 层级：0=一级
    bool         separator = false; // 分隔线（只占位，不显示图标/文字）
    bool         disabled = false;  // 灰态
    std::wstring text;              // 模板文字（文件名 → <文件名>，去助记符）
    std::wstring verb;              // 动词（GCS_VERBW），静态项/扩展项都可能有
    std::wstring brand;             // 品牌 key：WPS / 7-Zip / WinRAR / 系统 …
    std::wstring pathKey;           // 身份键：品牌 + 动词 + 各级模板文字（稳定标识一条项）
    std::wstring iconFile;          // 图标小图文件名（menu-icons 目录内，空=没有）
};

// 一个扩展名的采集结果
struct CensusExtension
{
    std::wstring ext;               // 带点，如 ".docx"
    bool         ok = false;
    std::wstring failReason;
    std::vector<CensusItem> items;
};

// 非文件场景（多选 / 文件夹 / 空白区）的采集结果
struct CensusScene
{
    unsigned int scene = 0;         // MenuScene 位
    bool         ok = false;
    std::wstring failReason;
    std::vector<CensusItem> items;
};

// 一个簇：同一软件在"文件"场景下菜单完全相同的一组扩展名
struct CensusCluster
{
    std::wstring brand;                       // 软件品牌
    std::wstring label;                       // 显示名（自动生成，界面里可改）
    std::vector<std::wstring> exts;           // 覆盖的扩展名
    std::vector<CensusItem> items;            // 代表样本的条目（带层级）
    std::wstring sampleExt;                   // 代表扩展名
};

// 一次普查的完整结果
struct CensusReport
{
    std::wstring scannedAt;                    // 采集时间
    std::vector<CensusExtension> extensions;   // 各扩展名
    std::vector<CensusScene> scenes;           // 多选 / 文件夹 / 空白区
    std::vector<CensusCluster> clusters;       // 文件场景下按软件分簇

    // 便捷查询
    int BrokenCount() const;                   // 采集失败的扩展名数
};

// 缓存文件：%APPDATA%\FileExplorer\menu-census.json
std::wstring CensusFilePath();
bool LoadCensus(CensusReport& report);
bool SaveCensus(const CensusReport& report);

// 根据 extensions 重新计算 clusters（每次普查后调用）
void BuildClusters(CensusReport& report);

// 寄存器"印记"：用来判断是否需要重新普查（注册表键的修改时间 + 扩展名清单）
std::wstring RegistryStamp();

// 列出本机所有扩展名（带点的，如 ".docx"），按字母排序
std::vector<std::wstring> EnumerateExtensions();

// ---------------------------------------------------------------------------
// 普查子进程
// ---------------------------------------------------------------------------

// 子进程入口。命令行参数：
//   FileExplorer.exe --census-worker <批次号> <扩展名清单文件> <结果文件>
// 返回进程退出码（0=成功）
int RunCensusWorker(int argc, wchar_t** argv);

// 主进程用：跑一次完整普查（内部按批启动子进程），progress 回调报告进度
//   progress(已完成的批数, 总批数)
// 返回 true 表示至少有一批成功
bool RunCensusInChildProcesses(const std::wstring& exePath,
                               const std::vector<std::wstring>& extensions,
                               std::vector<CensusExtension>& outExtensions,
                               const std::function<void(int, int)>& progress);

// 三个非文件场景的采集（父进程直接调，量很小）
void CollectNonFileScenes(std::vector<CensusScene>& outScenes);

// ---------------------------------------------------------------------------
// 图标
// ---------------------------------------------------------------------------

// 品牌 → 可能的程序/DLL 路径（从注册表 shellex 处理器反查）
std::vector<std::wstring> BrandExecutablePaths(const std::wstring& brand);

// 用系统 API 从程序/DLL 提取 32×32 图标，存成 BMP 返回文件名（空=失败）
std::wstring ExtractBrandIcon(const std::wstring& brand);

// 只查不改：这个品牌之前提取过图标就返回文件名，没有返回空
// （界面重建树时用，避免每次重建都去碰图标提取）
std::wstring BrandIconFile(const std::wstring& brand);

// 图标目录：%APPDATA%\FileExplorer\menu-icons
std::wstring IconDir();

} // namespace shell
