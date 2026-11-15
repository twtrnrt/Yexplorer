// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/Settings.h
//
// 本软件唯一的配置文件（公用）。
//
// 【为什么只做一个配置文件】
//   以后要记的东西会越来越多：窗口大小、各列宽度、左栏宽度、上次打开的目录、
//   左边的固定区……如果每样东西单独存一个文件，目录里很快就一堆零碎文件，
//   备份、排查、手工改都麻烦。
//   所以统一存到 %APPDATA%\FileExplorer\settings.json 一个文件里，
//   结构按功能分组（现在只有 "pinned"，以后加 "window" / "columns" / "lastFolder"）。
//
// 【为什么用 JSON】
//   纯文本、带缩进，出问题时用记事本就能打开看清楚哪里不对；
//   比注册表好备份（拷走文件就行），比二进制好排查。
// ============================================================================

#pragma once

#include <string>
#include <vector>

namespace core
{

// 左栏固定区里的一条：拖进来的文件夹或程序
struct PinnedItem
{
    std::wstring name;      // 显示名（一般就是文件名本身）
    std::wstring path;      // 完整路径（文件夹 / .exe / .lnk）
    bool         isFolder;  // true = 文件夹（点了进目录）；false = 程序（点了启动）
};

// 本软件公用配置。全程序只有一个实例（Settings::Instance()）。
class Settings
{
public:
    static Settings& Instance();

    // 从磁盘读。文件不存在算正常情况（首次运行），返回 true。
    // 文件坏了会把坏文件改名成 settings.json.bad 再返回 false，不会悄悄丢内容。
    bool Load();

    // 写回磁盘。先写 .tmp 再替换，写到一半出错也不会把原文件弄坏。
    bool Save();

    const std::vector<PinnedItem>& PinnedItems() const { return m_pinnedItems; }

    // 加一条固定项。同路径（忽略大小写）已经有了就不重复加，返回 false。
    bool AddPinned(const PinnedItem& item);

    // 删掉一条固定项。路径不存在返回 false。
    bool RemovePinned(const std::wstring& path);

    bool IsPinned(const std::wstring& path) const;

private:
    Settings() = default;

    // 宽字符 ↔ UTF-8。JSON 里存的是 UTF-8 文字，程序内部用 wchar_t
    static std::string  ToUtf8(const std::wstring& text);
    static std::wstring FromUtf8(const std::string& text);

    // %APPDATA%\FileExplorer\settings.json
    static std::wstring ConfigFilePath();

    // 把配置文件所在目录建出来（首次运行时还没有）
    static bool EnsureDirectory(const std::wstring& filePath);

    std::vector<PinnedItem> m_pinnedItems;
};

} // namespace core
