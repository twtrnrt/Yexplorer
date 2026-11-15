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

// 窗口位置与大小（重启后要回到原样）
// valid = false 表示配置文件里还没记过（首次运行），此时按默认大小居中开。
struct WindowState
{
    int  x         = 0;
    int  y         = 0;
    int  width     = 0;
    int  height    = 0;
    bool maximized = false;
    bool valid     = false;
};

// 详细信息视图里三列固定列的宽度。
// 名称列不存 —— 它永远"吃掉剩下的宽度"（和资源管理器一样），
// 只有「修改日期 / 类型 / 大小」是用户可能去拖的固定列。
struct ColumnWidths
{
    int  time  = 0;
    int  type  = 0;
    int  size  = 0;
    bool valid = false;
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

    // ---------------- 阶段 10：记住窗口状态 ----------------
    // 说明：这一组不做"每改一次就自动存盘"，由调用方决定什么时候写。
    // 现在只在两处写：关窗口时、拖完分隔线 / 拖完表头列宽时。

    // 配置是否已经从磁盘读过了（读是幂等的，重复调用不会重读）
    bool Loaded() const { return m_loaded; }

    const WindowState&  Window() const { return m_window; }
    void                SetWindow(const WindowState& state) { m_window = state; }

    // 左栏宽度。0 表示没记过（用默认值）
    int  PaneWidth() const { return m_paneWidth; }
    void SetPaneWidth(int width) { m_paneWidth = width; }

    const ColumnWidths& Columns() const { return m_columns; }
    void                SetColumns(const ColumnWidths& widths) { m_columns = widths; }

    // 上次用的查看模式（FileListView::ViewMode 的整数值）。-1 表示没记过。
    // manual = 用户是不是从「查看」菜单亲手选的（是的话以后不再按目录自动切）
    int  ViewMode() const { return m_viewMode; }
    bool ViewModeManual() const { return m_viewModeManual; }
    void SetViewMode(int mode, bool manual)
    {
        m_viewMode       = mode;
        m_viewModeManual = manual;
    }

    // 上次停留的目录（下次启动直接打开这里）
    const std::wstring& LastFolder() const { return m_lastFolder; }
    void                SetLastFolder(const std::wstring& path) { m_lastFolder = path; }

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

    // 阶段 10 的窗口状态
    bool          m_loaded         = false;   // 已经读过磁盘了
    WindowState   m_window;
    int           m_paneWidth      = 0;       // 0 = 没记过
    ColumnWidths  m_columns;
    int           m_viewMode       = -1;      // -1 = 没记过
    bool          m_viewModeManual = false;
    std::wstring  m_lastFolder;
};

} // namespace core
